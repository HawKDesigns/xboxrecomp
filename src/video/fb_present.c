/**
 * Show the guest framebuffer in a window.
 *
 * The title renders into its own framebuffer in guest RAM and tells the kernel
 * where it is through AvSetDisplayMode; on hardware the CRTC scans that memory
 * out. Nothing here scans anything out, so however much of the GPU is
 * implemented, none of it is observable. This is the other half: a window that
 * reads that memory and puts it on screen.
 *
 * Deliberately plain GDI rather than the D3D8 layer. The point is to display
 * whatever the guest actually wrote, so the fewer stages between guest memory
 * and the screen the better -- and it must keep working while the D3D8 layer
 * is busy with something else, such as the FMV player's own window.
 * RECOMP_D3D_PRESENT=1 shows the same frames through a D3D11 swap chain of
 * the window's own instead (gpu_present.c): scaled on the GPU, vsync-paced.
 *
 * The window opens at RECOMP_WINDOW_SCALE times the frame (default 2, made
 * smaller until it fits the desktop) and can be resized; the frame is
 * letterboxed to its 4:3 shape. F11 or Alt+Enter toggles borderless
 * fullscreen, RECOMP_FULLSCREEN=1 starts in it. Closing the window ends the
 * process, as the SDL window does.
 *
 * Off unless RECOMP_FB_WINDOW is set.
 */
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Which keys are down, for the pad stand-in in src/input: this window
 * records them in the shared key table (src/input/keyboard.c).
 *
 * GetAsyncKeyState looked like the cheaper way to ask and does not work
 * here: it reads a state Wine keeps for the X server, and a guest process
 * drawing through GDI never sees it change. The window that has the focus
 * is the thing that receives the keys, so that is what has to remember
 * them. */
#include "keyboard.h"   /* src/input */
#include "gpu_present.h"
#include "present_layout.h"
#include "kernel/nv2a_render_scale.h"
#include "platform/frame_trace.h"
#include "platform/host_time.h"
#include "platform/thread_cpu.h"
#include "recomp_exit_hook.h"   /* src/platform */
#include <stdbool.h>
#include "pvideo_scanout.h"
#include "../nv2a/nv2a_mmio_hook.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);

static volatile LONG s_fb_running;
/* A user close is terminal for this diagnostic host. Clear/start paths run
 * throughout guest shutdown and must never recreate a dismissed window. */
static volatile LONG s_fb_closed;
static uint32_t      s_fb_va, s_fb_pitch, s_fb_width = 640, s_fb_height = 480;
static uint32_t     *s_rgb;           /* converted 32-bit copy for GDI */
static unsigned      s_rgb_scale=1;
static XboxPresentFilter s_filter;
static int s_widescreen;

/* A finished frame, taken at the flip and shown until the next one.
 *
 * The window used to convert straight out of guest memory every 16 ms. Even
 * pointed at the buffer the title had just finished, that races the executor
 * drawing the next frame into the other one and, whenever the two swap, puts
 * a half-drawn image on the screen -- which is the flicker. Copying the
 * finished frame once per flip means the window never reads memory the
 * rasteriser is writing, so what it shows cannot be half of anything.
 *
 * The lock also protects the window's copy: two flips can otherwise reuse
 * the buffer the window is still reading. */
static uint32_t     *s_present[2];
static size_t        s_present_capacity[2];
static unsigned      s_present_scale=1;
static uint32_t      s_present_frame_id;
static LONG          s_present_idx = -1;   /* -1 until the first flip */
static SRWLOCK       s_present_lock = SRWLOCK_INIT;

/* The last PVIDEO frame shown. A frame stays on screen until the next one or
 * STOP, but once its bank is consumed the title may reuse the memory, so the
 * pixels are kept here rather than read again. */
static uint8_t      *s_pvideo_source;
static size_t        s_pvideo_bytes;
static unsigned      s_pvideo_bank;
static uint32_t      s_pvideo_regs[0x1000 / 4];

/* Draw the video overlay over s_rgb: a newly submitted bank if there is one
 * (copied, drawn, then handed back to the title), else the last frame. */
static void fb_overlay(void)
{
    uint32_t regs[0x1000 / 4];
    unsigned bank;
    bool presented = false;

    if (!s_rgb || !nv2a_hook_pvideo_snapshot(regs)) return;
    if ((regs[NV_PVIDEO_STOP / 4] & 1u) ||
        (s_pvideo_source &&
         regs[(NV_PVIDEO_SIZE_IN + s_pvideo_bank * 4) / 4] == UINT32_MAX)) {
        free(s_pvideo_source);
        s_pvideo_source = NULL;
        if (regs[NV_PVIDEO_STOP / 4] & 1u) return;
    }
    for (bank = 0; bank < 2; ++bank) {
        uint64_t physical = (uint64_t)regs[(NV_PVIDEO_BASE + bank * 4) / 4] +
                            regs[(NV_PVIDEO_OFFSET + bank * 4) / 4];
        unsigned pitch = regs[(NV_PVIDEO_FORMAT + bank * 4) / 4] & NV_PVIDEO_FORMAT_PITCH;
        unsigned height = (regs[(NV_PVIDEO_SIZE_IN + bank * 4) / 4] >> 16) & 0x7ff;
        uint64_t bytes = (uint64_t)pitch * height;
        uintptr_t va;
        MEMORY_BASIC_INFORMATION mapping;
        uint8_t *snapshot;

        if (!(regs[NV_PVIDEO_BUFFER / 4] & (1u << (bank * 4u)))) continue;
        if (!bytes || physical + bytes > 0x04000000u) continue;
        va = (uintptr_t)(0x80000000u + (uint32_t)physical) +
             (uintptr_t)xbox_GetMemoryOffset();
        if (!VirtualQuery((const void *)va, &mapping, sizeof(mapping)) ||
            mapping.State != MEM_COMMIT ||
            (mapping.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            bytes > mapping.RegionSize - (size_t)(va - (uintptr_t)mapping.BaseAddress))
            continue;
        snapshot = (uint8_t *)malloc((size_t)bytes);
        if (!snapshot) continue;
        memcpy(snapshot, (const void *)va, (size_t)bytes);
        if (!pvideo_scanout_scaled(regs, snapshot, (size_t)bytes, s_rgb,
                                   s_fb_width, s_fb_height, bank, s_rgb_scale)) {
            free(snapshot);
            continue;
        }
        free(s_pvideo_source);
        s_pvideo_source = snapshot;
        s_pvideo_bytes = (size_t)bytes;
        s_pvideo_bank = bank;
        memcpy(s_pvideo_regs, regs, sizeof(s_pvideo_regs));
        presented = true;
        nv2a_hook_pvideo_consume(bank);
    }
    if (!presented && s_pvideo_source)
        pvideo_scanout_scaled(s_pvideo_regs, s_pvideo_source, s_pvideo_bytes, s_rgb,
                              s_fb_width, s_fb_height, s_pvideo_bank, s_rgb_scale);
    if (presented && xbox_frame_trace_enabled()) {
        static DWORD last_activity;
        DWORD now = GetTickCount();
        if (!last_activity || now - last_activity >= 250) {
            xbox_frame_trace_note(XBOX_TRACE_WINDOW, XBOX_TRACE_SCENE_ACTIVITY,
                                  0, XBOX_PHASE_VIDEO, 0, 0);
            last_activity = now;
        }
    }
}

/* Publish a resolved frame using the same lock as guest-memory flips. */
static int fb_reserve_present_slot(LONG slot,size_t bytes)
{
    if(s_present_capacity[slot]>=bytes)return 1;
    uint32_t *pixels=realloc(s_present[slot],bytes);
    if(!pixels)return 0;
    s_present[slot]=pixels;
    s_present_capacity[slot]=bytes;
    return 1;
}

void xbox_FramebufferWindowPresentPixels(const uint32_t *pixels,
                                        uint32_t width, uint32_t height, uint32_t frame_id)
{
    unsigned scale=width/s_fb_width;
    if (!s_fb_running || !pixels || scale<1 || scale>NV2A_MAX_RENDER_SCALE ||
        width!=s_fb_width*scale || height!=s_fb_height*scale || getenv("RECOMP_FB_VA"))
        return;

    AcquireSRWLockExclusive(&s_present_lock);
    LONG next = (s_present_idx == 0) ? 1 : 0;
    size_t bytes = (size_t)width * height * sizeof(*pixels);
    if (fb_reserve_present_slot(next,bytes)) {
        memcpy(s_present[next], pixels, bytes);
        s_present_idx = next;
        s_present_scale = scale;
        s_present_frame_id = frame_id;
        xbox_frame_trace_note(XBOX_TRACE_RENDERER, XBOX_TRACE_HANDOFF, frame_id, 0, 0, 0);
    }
    ReleaseSRWLockExclusive(&s_present_lock);
}

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    /* RECOMP_FB_VA pins the window to one guest address instead of following
     * whichever surface is being drawn into. A black window cannot distinguish
     * "the read path is broken" from "the title rendered black", and pointing
     * it at memory known to have content settles that. */
    const char *pin = getenv("RECOMP_FB_VA");

    s_fb_va = pin ? (uint32_t)strtoul(pin, NULL, 0) : fb_va;
    if (pitch)
        s_fb_pitch = pitch;
}

/* Vertical anti-aliasing factor of the surface being shown: 2 for 2x2, where
 * the surface has twice the rows as well as twice the columns, and 1 for
 * everything else (2x1 included). Only the pushbuffer executor knows it. */
static uint32_t s_fb_aa_sy = 1;

void xbox_FramebufferWindowSetAA(uint32_t sy)
{
    s_fb_aa_sy = sy ? sy : 1;
}

/* Called by the pushbuffer executor when the title flips. */
void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch)
{
    const uint8_t *src;
    LONG next;
    uint32_t bpp, x, y;

    if (!s_fb_running || !fb_va || !pitch)
        return;
    if (getenv("RECOMP_FB_VA"))
        return;                       /* pinned: leave the old path alone */
    AcquireSRWLockExclusive(&s_present_lock);
    next = (s_present_idx == 0) ? 1 : 0;
    if (!fb_reserve_present_slot(next,(size_t)s_fb_width*s_fb_height*4u)) {
        ReleaseSRWLockExclusive(&s_present_lock);
        fprintf(stderr, "  [FBWIN] cannot allocate a presentation frame\n");
        return;
    }
    bpp = pitch / s_fb_width;
    src = (const uint8_t *)((uintptr_t)fb_va + xbox_GetMemoryOffset());
    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * pitch * (bpp == 8 ? s_fb_aa_sy : 1);
        uint32_t *dst = s_present[next] + (size_t)y * s_fb_width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 8) {
            /* 2x-wide anti-aliased 32-bit surface: every other pixel, as in
             * fb_convert. */
            const uint32_t *p = (const uint32_t *)row;
            for (x = 0; x < s_fb_width; x++)
                dst[x] = p[x * 2];
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < s_fb_width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s_fb_width * 4);
        }
    }
    s_present_idx = next;
    s_present_scale = 1;
    s_present_frame_id = 0;
    ReleaseSRWLockExclusive(&s_present_lock);
}

static int fb_copy_presented_frame(uint32_t **dst,size_t *capacity,
                                    unsigned *scale,uint32_t *frame_id)
{
    int copied = 0;

    AcquireSRWLockShared(&s_present_lock);
    if (s_present_idx >= 0) {
        size_t bytes=(size_t)s_fb_width*s_fb_height*s_present_scale*s_present_scale*4u;
        if(*capacity<bytes) {
            uint32_t *next=realloc(*dst,bytes);
            if(next){*dst=next;*capacity=bytes;}
        }
        if(*capacity>=bytes) {
            memcpy(*dst,s_present[s_present_idx],bytes);
            *frame_id=s_present_frame_id;
            *scale=s_present_scale;
            copied=1;
        }
    }
    ReleaseSRWLockShared(&s_present_lock);
    return copied;
}

/* Title bar, the way ps3recomp's window shows it. Written by the flip,
 * read once a second by the window thread; a torn read shows one stale
 * number for a second, which nobody can tell apart from a real one. */
static wchar_t       s_title[48] = L"Xbox Recomp";
static volatile LONG s_flips;
static volatile LONG s_frame_draws;
#ifdef XBOX_FRAME_TELEMETRY
#define FB_STATS_HISTORY 120
static volatile LONG s_stats_open;
static SRWLOCK s_stats_lock = SRWLOCK_INIT;
static DWORD s_stats_frame_us[FB_STATS_HISTORY];
static unsigned s_stats_next, s_stats_count;
static HDC s_stats_canvas;
static HBITMAP s_stats_bitmap;
static HGDIOBJ s_stats_old_bitmap;
static uint32_t *s_stats_pixels;

static int compare_frame_us(const void *a, const void *b)
{
    DWORD left = *(const DWORD *)a, right = *(const DWORD *)b;
    return (left > right) - (left < right);
}

/* Draw into the host's private frame copy so the same overlay works with
 * both presenters, including borderless fullscreen. */
static void fb_draw_stats(HDC target)
{
    enum { width = 308, height = 75, graph_y = 67 };
    static DWORD updated;
    if (s_fb_width < width + 16 || s_fb_height < height + 16) return;
    if (!s_stats_canvas) {
        BITMAPINFO info = {0};
        void *bits = NULL;
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        s_stats_canvas = CreateCompatibleDC(target);
        if (!s_stats_canvas) return;
        s_stats_bitmap = CreateDIBSection(s_stats_canvas, &info, DIB_RGB_COLORS, &bits, NULL, 0);
        if (!s_stats_bitmap) { DeleteDC(s_stats_canvas); s_stats_canvas = NULL; return; }
        s_stats_pixels = bits;
        s_stats_old_bitmap = SelectObject(s_stats_canvas, s_stats_bitmap);
        SetBkMode(s_stats_canvas, TRANSPARENT);
        SetTextColor(s_stats_canvas, RGB(234, 240, 246));
        SelectObject(s_stats_canvas, GetStockObject(DEFAULT_GUI_FONT));
        updated = 0;
    }
    DWORD now = GetTickCount();
    if (!updated || now - updated >= 250) {
        DWORD samples[FB_STATS_HISTORY];
        unsigned count, first, slow = 0;
        uint64_t total = 0;
        AcquireSRWLockShared(&s_stats_lock);
        count = s_stats_count;
        first = (s_stats_next + FB_STATS_HISTORY - count) % FB_STATS_HISTORY;
        for (unsigned i = 0; i < count; ++i)
            samples[i] = s_stats_frame_us[(first + i) % FB_STATS_HISTORY];
        ReleaseSRWLockShared(&s_stats_lock);
        for (unsigned i = 0; i < width * height; ++i) s_stats_pixels[i] = 0x00101822;
        for (unsigned i = 0; i < count; ++i) {
            DWORD us = samples[i];
            unsigned bar = us < 40000 ? (unsigned)us / 1000 : 40;
            unsigned x = 12 + i * 2;
            uint32_t color = us > 33333 ? 0x00E3A251 : 0x0067D6A8;
            slow += us > 33333;
            total += us;
            for (unsigned y = graph_y - bar; y < graph_y; ++y)
                s_stats_pixels[y * width + x] = s_stats_pixels[y * width + x + 1] = color;
        }
        qsort(samples, count, sizeof(*samples), compare_frame_us);
        char label[120];
        if (total)
            snprintf(label, sizeof label, "%.1f FPS   P95 %.1f ms   %u/%u over 33 ms   %ld draws",
                     count * 1000000.0 / total, samples[(count * 95 - 1) / 100] / 1000.0,
                     slow, count, (long)s_frame_draws);
        else
            snprintf(label, sizeof label, "Waiting for completed flips");
        TextOutA(s_stats_canvas, 10, 5, label, (int)strlen(label));
        GdiFlush();
        updated = now;
    }
    for (unsigned y = 0; y < height; ++y) {
        unsigned stride=s_fb_width*s_rgb_scale;
        for(unsigned dy=0;dy<s_rgb_scale;dy++) {
            uint32_t *row=s_rgb+(size_t)((y+8)*s_rgb_scale+dy)*stride+8*s_rgb_scale;
            for(unsigned x=0;x<width;x++)
                for(unsigned dx=0;dx<s_rgb_scale;dx++)
                    row[x*s_rgb_scale+dx]=s_stats_pixels[y*width+x];
        }
    }
}

static void fb_close_stats(void)
{
    if (!s_stats_canvas) return;
    SelectObject(s_stats_canvas, s_stats_old_bitmap);
    DeleteObject(s_stats_bitmap);
    DeleteDC(s_stats_canvas);
    s_stats_canvas = NULL;
}
#endif

void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars)
{
    int i;
    for (i = 0; i < max_chars && i < 47 && name[i]; i++)
        s_title[i] = (wchar_t)name[i];
    if (i)
        s_title[i] = 0;
}

void xbox_FramebufferWindowFrameStats(uint32_t draws)
{
    InterlockedIncrement(&s_flips);
    InterlockedExchange(&s_frame_draws, (LONG)draws);
#ifdef XBOX_FRAME_TELEMETRY
    static uint64_t previous;
    if (InterlockedCompareExchange(&s_stats_open, 0, 0)) {
        uint64_t now = xbox_HostNowNs();
        if (previous && TryAcquireSRWLockExclusive(&s_stats_lock)) {
            uint64_t elapsed = (now - previous) / 1000;
            s_stats_frame_us[s_stats_next] = elapsed < UINT32_MAX ? (DWORD)elapsed : UINT32_MAX;
            s_stats_next = (s_stats_next + 1) % FB_STATS_HISTORY;
            if (s_stats_count < FB_STATS_HISTORY) ++s_stats_count;
            ReleaseSRWLockExclusive(&s_stats_lock);
        }
        previous = now;
    } else {
        previous = 0;
    }
#endif
}

/* Alt+Enter / F11: borderless fullscreen on the window's monitor, and back.
 * (DXGI's own Alt+Enter, exclusive mode at the frame's 640x480, is off --
 * gpu_present.c; both presenters letterbox to any size.) */
static void fb_toggle_fullscreen(HWND h)
{
    static int full;
    static RECT saved;
    static LONG_PTR saved_style;
    if (!full) {
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        if (!GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi))
            return;
        GetWindowRect(h, &saved);
        saved_style = GetWindowLongPtrA(h, GWL_STYLE);
        SetWindowLongPtrA(h, GWL_STYLE, (saved_style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        full = 1;
    } else {
        SetWindowLongPtrA(h, GWL_STYLE, saved_style);
        SetWindowPos(h, NULL, saved.left, saved.top, saved.right - saved.left,
                     saved.bottom - saved.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
        full = 0;
    }
}

extern void nv_gpu_capture_request(void);
static LRESULT CALLBACK fb_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    /* First press only (bit 30: the key was already down), and with Alt held
     * (bit 29) for Enter; neither reaches the key table. */
    if ((m == WM_SYSKEYDOWN && w == VK_RETURN && (l & (1 << 29)) && !(l & (1 << 30)))
        || (m == WM_KEYDOWN && w == VK_F11 && !(l & (1 << 30)))) {
        fb_toggle_fullscreen(h);
        return 0;
    }
    if (m == WM_SYSKEYDOWN && w == VK_RETURN)
        return 0;                       /* auto-repeat of Alt+Enter */
#ifdef XBOX_FRAME_TELEMETRY
    if ((m == WM_KEYDOWN || m == WM_KEYUP) && w == VK_F2) {
        if (m == WM_KEYDOWN && !(l & ((LPARAM)1 << 30))) {
            LONG open = InterlockedCompareExchange(&s_stats_open, 0, 0);
            if (!open) {
                AcquireSRWLockExclusive(&s_stats_lock);
                s_stats_count = s_stats_next = 0;
                ReleaseSRWLockExclusive(&s_stats_lock);
            }
            InterlockedExchange(&s_stats_open, !open);
            fb_close_stats();
            xbox_frame_trace_note(XBOX_TRACE_WINDOW, XBOX_TRACE_OVERLAY, 0, !open, 0, 0);
        }
        return 0;
    }
#endif
    if ((m == WM_KEYDOWN || m == WM_KEYUP) && w == VK_F3 && xbox_frame_trace_enabled()) {
        static int paused;
        if (m == WM_KEYDOWN && !(l & ((LPARAM)1 << 30))) {
            paused = !paused;
            xbox_frame_trace_note(XBOX_TRACE_WINDOW, XBOX_TRACE_PAUSE, 0, paused, 0, 0);
        }
        return 0;
    }
    if ((m == WM_KEYDOWN || m == WM_KEYUP) &&
        ((w >= VK_F4 && w <= VK_F7) || w == VK_F1 || w == VK_F10) &&
        xbox_frame_trace_enabled()) {
        if (m == WM_KEYDOWN && !(l & ((LPARAM)1 << 30))) {
            const enum XboxTracePhase phases[] = {XBOX_PHASE_UNMARKED, XBOX_PHASE_COUNTDOWN,
                                                  XBOX_PHASE_AHEAD, XBOX_PHASE_BEHIND};
            enum XboxTracePhase phase = w == VK_F1 ? XBOX_PHASE_RIDER_MENU :
                                        w == VK_F10 ? XBOX_PHASE_ATV_MENU : phases[w - VK_F4];
            xbox_frame_trace_note(XBOX_TRACE_WINDOW, XBOX_TRACE_PHASE, 0, phase, 1, 0);
        }
        return 0;
    }
    switch (m) {
    case WM_CLOSE:
        InterlockedExchange(&s_fb_closed, 1);
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        /* Closing the window ends the run. Only stopping this thread left
         * every other one running -- audio played on, and the process had
         * to be ended from Task Manager. The guest has no exit path of its
         * own to ask. TerminateProcess rather than ExitProcess: the guest
         * threads are still running, and ExitProcess's cleanup can wait on
         * a lock one of them held. The exit hook does the reporting that
         * would otherwise be skipped. RECOMP_CLOSE_KEEPS_RUNNING=1 only
         * closes the window, as before. */
        InterlockedExchange(&s_fb_running, 0);
        if (!getenv("RECOMP_CLOSE_KEEPS_RUNNING")) {
            fprintf(stderr, "  [FBWIN] window closed: exiting\n");
#ifdef XBOX_FRAME_TELEMETRY
            xbox_frame_trace_stop();
#endif
            recomp_exit_hook_run("window");
            fflush(stdout);
            fflush(stderr);
            TerminateProcess(GetCurrentProcess(), 0);
        }
        return 0;

    case WM_KEYDOWN:
        if(w==0x77 && !(l & ((LPARAM)1<<30))) {nv_gpu_capture_request();return 0;} /* F8 */
    case WM_SYSKEYDOWN:
        xbox_InputKeySet((int)w, 1);
        /* A Shift, Ctrl or Alt is also its side's key, for bindings to
         * "lshift" and the like; wParam stays the either-side one. */
        {
            int side = xbox_InputWin32SideVk((int)w, (uint32_t)l);
            if (side)
                xbox_InputKeySet(side, 1);
        }
        /* RECOMP_KEY_TRACE: each key as it arrives, edge-triggered.
         *
         * The obvious diagnostic -- sampling which keys are held, once a
         * second, from the input path -- cannot tell a key that was never
         * pressed from one that was tapped: a 100 ms press is caught about
         * one time in ten. That ambiguity is expensive when the only way
         * to test is to ask someone to press a key and describe what
         * happened. This answers "did it arrive" on its own. */
        if (getenv("RECOMP_KEY_TRACE")) {
            static unsigned n;
            if (n++ < 40) {
                fprintf(stderr, "  [KEY] down vk=0x%02X\n", (unsigned)w);
                fflush(stderr);
            }
        }
        /* System keys still go to Windows, or Alt+F4 stops closing us. */
        return m == WM_SYSKEYDOWN ? DefWindowProcA(h, m, w, l) : 0;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        xbox_InputKeySet((int)w, 0);
        {
            int side = xbox_InputWin32SideVk((int)w, (uint32_t)l);
            if (side)
                xbox_InputKeySet(side, 0);
            /* With both Shifts down Windows sends one key-up, for the
             * last released, so the other side would stay held. Autorepeat
             * sets a Shift still down again. */
            if (w == VK_SHIFT) {
                xbox_InputKeySet(VK_LSHIFT, 0);
                xbox_InputKeySet(VK_RSHIFT, 0);
            }
        }
        return m == WM_SYSKEYUP ? DefWindowProcA(h, m, w, l) : 0;

    /* Mouse buttons go in the same table, for bindings that name them.
     * Capturing the mouse while one is down delivers its release even
     * outside the window. */
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP:
        {
            int down = 0;
            int vk = xbox_InputWin32MouseVk(m, (uint32_t)w, &down);
            if (vk)
                xbox_InputKeySet(vk, down);
            if (down)
                SetCapture(h);
            else if (!(w & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2)))
                ReleaseCapture();
        }
        return m == WM_XBUTTONDOWN || m == WM_XBUTTONUP ? TRUE : 0;

    /* Alt-tabbing away with a key held would leave it held for ever. */
    case WM_KILLFOCUS:
        xbox_InputKeysClear();
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

/* The display formats an Xbox front buffer is actually set to. The pitch says
 * how wide a row is in bytes, so pitch/width gives the pixel size; the exact
 * component layout only matters for 16-bit, where 5:6:5 and 1:5:5:5 differ. */
static void fb_convert(const uint8_t *src, uint32_t bpp)
{
    uint32_t x, y;

    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * s_fb_pitch
                           * (bpp == 8 ? s_fb_aa_sy : 1);
        uint32_t *dst = s_rgb + (size_t)y * s_fb_width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 8) {
            /* A 2x-wide anti-aliased 32-bit surface (pitch = 2 * width * 4):
             * take every other pixel. X-Men Legends renders its 640-wide
             * frame into a 1280-wide surface, which read as "8 bytes per
             * pixel" and showed black. */
            const uint32_t *p = (const uint32_t *)row;
            for (x = 0; x < s_fb_width; x++)
                dst[x] = p[x * 2];
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < s_fb_width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s_fb_width * 4);
        }
    }
}

/* Write what the window is currently showing to a 24-bit BMP.
 *
 * A black window is ambiguous: it means either that the read path is wrong or
 * that the title really did render black. Dumping the same converted pixels
 * the window draws settles which, and does it without a screenshot. */
int xbox_FramebufferDumpBmp(const char *path)
{
    FILE *f;
    uint32_t width=s_fb_width*s_rgb_scale,height=s_fb_height*s_rgb_scale;
    uint32_t row = ((width * 3u) + 3u) & ~3u;
    uint32_t img = row * height, total = 54u + img, y, x;
    uint8_t hdr[54], *line;

    if (!s_rgb || !s_fb_va)
        return -1;
    f = fopen(path, "wb");
    if (!f)
        return -1;
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &total, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &width, 4);
    memcpy(hdr + 22, &height, 4);
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, sizeof(hdr), f);

    line = (uint8_t *)calloc(1, row);
    for (y = 0; y < height; y++) {
        const uint32_t *src = s_rgb + (size_t)(height - 1 - y) * width;
        for (x = 0; x < width; x++) {
            line[x * 3 + 0] = (uint8_t)(src[x] & 0xFF);
            line[x * 3 + 1] = (uint8_t)((src[x] >> 8) & 0xFF);
            line[x * 3 + 2] = (uint8_t)((src[x] >> 16) & 0xFF);
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    fprintf(stderr, "  [FBWIN] wrote %s (%ux%u from 0x%08X)\n",
            path, width, height, s_fb_va);
    return 0;
}

/* GDI: the frame letterboxed into the client area, like the D3D11
 * presenter; the borders are cleared when the size changes. */
static int gdi_blit(HWND hwnd, HDC hdc, BITMAPINFO *bi)
{
    static LONG lw, lh;
    static unsigned last_scale;
    RECT c;
    LONG cw, ch;
    GetClientRect(hwnd, &c);
    cw = c.right - c.left; ch = c.bottom - c.top;
    if (cw <= 0 || ch <= 0)
        return 0;
    if (cw != lw || ch != lh || last_scale!=s_rgb_scale) {
        FillRect(hdc, &c, (HBRUSH)GetStockObject(BLACK_BRUSH));
        lw = cw; lh = ch;
        last_scale=s_rgb_scale;
    }
    int width=(int)(s_fb_width*s_rgb_scale),height=(int)(s_fb_height*s_rgb_scale);
    bi->bmiHeader.biWidth=width;
    bi->bmiHeader.biHeight=-height;
    XboxPresentRect pic=xbox_present_rect(cw,ch,width,height,s_widescreen,s_filter);
    SetStretchBltMode(hdc, s_filter == XBOX_PRESENT_LINEAR ? HALFTONE : COLORONCOLOR);
    if (s_filter == XBOX_PRESENT_LINEAR) {
        SetBrushOrgEx(hdc, 0, 0, NULL);  /* HALFTONE wants it after the mode */
    }
    int lines = StretchDIBits(hdc, pic.x, pic.y, pic.width, pic.height,
                             0, 0, width, height,
                             s_rgb, bi, DIB_RGB_COLORS, SRCCOPY);
    return lines != 0 && lines != GDI_ERROR;
}

/* The converted frame in s_rgb, on screen: D3D11 when it is up (which waits
 * for vsync, so returns 1 when the loop is already paced), else GDI. */
static int fb_show(HWND hwnd, HDC hdc, BITMAPINFO *bi, int *shown)
{
#ifdef XBOX_FRAME_TELEMETRY
    if (InterlockedCompareExchange(&s_stats_open, 0, 0)) fb_draw_stats(hdc);
#endif
    if (gpu_present_active()) {
        if(gpu_present_resize_source(s_fb_width*s_rgb_scale,s_fb_height*s_rgb_scale))
            return gpu_present_frame(s_rgb, shown);
        fprintf(stderr,"  [D3D] source resize failed; switching to GDI\n");
        gpu_present_shutdown();
    }
    *shown = gdi_blit(hwnd, hdc, bi);
    return 0;
}

static DWORD WINAPI fb_thread(LPVOID unused)
{
    xbox_ThreadCpuRegister("presenter");
    HWND hwnd;
    HDC hdc;
    BITMAPINFO bi;
    RECT r;
    int fb_pinned;
    uint32_t last_drawn = 0;

    (void)unused;
    xbox_frame_trace_thread(XBOX_TRACE_PRESENTER);
    s_filter = xbox_present_filter(XBOX_PRESENT_LINEAR);
    s_widescreen = xbox_video_widescreen();

    {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc   = fb_wndproc;
        wc.hInstance     = GetModuleHandleA(NULL);
        wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
        wc.lpszClassName = "XboxRecompFramebuffer";
        RegisterClassA(&wc);
    }
    {
        /* RECOMP_WINDOW_SCALE (1-8, default 2, as in the SDL window): the
         * client area is the frame times this, made smaller until the window
         * fits the desktop work area, and centred on it. */
        const char *e = getenv("RECOMP_WINDOW_SCALE");
        int scale = e ? atoi(e) : 2;
        RECT work;
        if (scale < 1 || scale > 8)
            scale = 2;
        if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0)) {
            work.left = work.top = 0; work.right = 1920; work.bottom = 1080;
        }
        for (;; scale--) {
            r.left = 0; r.top = 0;
            r.right = xbox_present_display_width((int)s_fb_width, (int)s_fb_height,
                                                 s_widescreen) * scale;
            r.bottom = (LONG)s_fb_height * scale;
            AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
            if (scale <= 1 || (r.right - r.left <= work.right - work.left
                               && r.bottom - r.top <= work.bottom - work.top))
                break;
        }
        hwnd = CreateWindowExA(0, "XboxRecompFramebuffer", "Xbox Recomp - Framebuffer",
                               WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               work.left + ((work.right - work.left) - (r.right - r.left)) / 2,
                               work.top + ((work.bottom - work.top) - (r.bottom - r.top)) / 2,
                               r.right - r.left, r.bottom - r.top,
                               NULL, NULL, GetModuleHandleA(NULL), NULL);
    }
    if (!hwnd) {
        InterlockedExchange(&s_fb_running, 0);
        return 0;
    }
    hdc = GetDC(hwnd);
    {
        const char *fs = getenv("RECOMP_FULLSCREEN");
        if (fs && *fs && *fs != '0')
            fb_toggle_fullscreen(hwnd);
    }

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = (LONG)s_fb_width;
    bi.bmiHeader.biHeight      = -(LONG)s_fb_height;   /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    size_t rgb_capacity=(size_t)s_fb_width*s_fb_height*4u;
    s_rgb=calloc(1,rgb_capacity);
    if(!s_rgb)rgb_capacity=0;
    s_rgb_scale=1;
    /* A window pinned to one address shows that memory and nothing else. */
    fb_pinned = getenv("RECOMP_FB_VA") != NULL;

    fprintf(stderr, "  [FBWIN] framebuffer window open (%ux%u)\n",
            s_fb_width, s_fb_height);
    if (gpu_present_enabled())
        gpu_present_init(hwnd, s_fb_width, s_fb_height);

    while (InterlockedCompareExchange(&s_fb_running, 1, 1)) {
        MSG msg;
        int paced = 0, shown = 0;
        uint32_t frame_id = 0;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (fb_copy_presented_frame(&s_rgb,&rgb_capacity,&s_rgb_scale,&frame_id)) {
            /* A finished frame, published by the flip. Copied into s_rgb so
             * the dump path and GDI see one consistent image even if the
             * next flip lands mid-blit. */
            if (!fb_pinned)
                fb_overlay();
            paced = fb_show(hwnd, hdc, &bi, &shown);
        } else if (s_fb_va && s_fb_pitch && s_rgb) {
            /* No flip yet, or pinned with RECOMP_FB_VA: read guest memory as
             * before, which is also what a title that never flips needs. */
            const uint8_t *src =
                (const uint8_t *)((uintptr_t)s_fb_va + xbox_GetMemoryOffset());
            s_rgb_scale=1;
            fb_convert(src, s_fb_pitch / s_fb_width);
            if (!fb_pinned)
                fb_overlay();
            paced = fb_show(hwnd, hdc, &bi, &shown);
        }
        if (shown && frame_id && frame_id != last_drawn) {
            last_drawn = frame_id;
            xbox_frame_trace_note(XBOX_TRACE_WINDOW, XBOX_TRACE_DRAW, frame_id, 0, 0, 0);
        }
        {
            /* One dump a few seconds in, so the title has had time to render
             * something rather than catching the first blank frame.
             *
             * RECOMP_FB_WINDOW_DUMP_EVERY=<frames> dumps repeatedly instead.
             * This window follows the address AvSetDisplayMode gave, which is
             * what the CRTC scans and therefore what a person sees; the
             * pushbuffer executor's own dump follows its draw surface. With
             * double buffering those are different buffers, and measuring
             * progress from the executor's dump reports a blank screen while
             * the window is showing the title's logo. Ask the window. */
            const char *dump = getenv("RECOMP_FB_DUMP");
            const char *every = getenv("RECOMP_FB_WINDOW_DUMP_EVERY");
            static int frames;
            int period = every ? atoi(every) : 0;
            frames++;
            if (dump && period > 0) {
                if (frames % period == 0)
                    xbox_FramebufferDumpBmp(dump);
            } else if (dump && frames == 600) {
                xbox_FramebufferDumpBmp(dump);
            }
        }
        {
            static DWORD t0;
            static LONG f0;
            DWORD now = GetTickCount();
            if (now - t0 >= 1000) {
                LONG f = s_flips;
                wchar_t tb[128];
                _snwprintf(tb, 127, L"%ls | FPS: %.1f | draws: %ld", s_title,
                           t0 ? (f - f0) * 1000.0 / (now - t0) : 0.0,
                           (long)s_frame_draws);
                tb[127] = 0;
                SetWindowTextW(hwnd, tb);
                t0 = now; f0 = f;
            }
        }
        if (!paced)
            Sleep(16);                  /* D3D11's Present waited for vsync */
    }

#ifdef XBOX_FRAME_TELEMETRY
    InterlockedExchange(&s_stats_open, 0);
    fb_close_stats();
#endif
    gpu_present_shutdown();
    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
    free(s_rgb);
    s_rgb = NULL;
    free(s_pvideo_source);
    s_pvideo_source = NULL;
    return 0;
}

void xbox_FramebufferWindowStart(void)
{
    HANDLE th;

    if (!getenv("RECOMP_FB_WINDOW"))
        return;
    if (InterlockedCompareExchange(&s_fb_closed, 0, 0))
        return;
    if (InterlockedCompareExchange(&s_fb_running, 1, 0) != 0)
        return;
    th = CreateThread(NULL, 0, fb_thread, NULL, 0, NULL);
    if (th)
        CloseHandle(th);
    else
        InterlockedExchange(&s_fb_running, 0);
}

#endif /* _WIN32: POSIX hosts get the SDL2 window in fb_present_sdl.c */
