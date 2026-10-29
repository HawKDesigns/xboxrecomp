/* Run with: python tests/audio_mixer/run.py */
#include <assert.h>
#include "audio/dsound_xbox.h"

static IDirectSoundBuffer8 *make_buffer(WORD channels, DWORD rate,
                                      const int16_t *pcm, DWORD bytes)
{
    IDirectSound8 *ds;
    IDirectSoundBuffer8 *buf;
    XBOX_WAVEFORMATEX format = {0};
    format.nChannels = channels;
    format.nSamplesPerSec = rate;
    format.wBitsPerSample = 16;
    DSBUFFERDESC desc = {0};
    desc.lpwfxFormat = &format;
    assert(xbox_DirectSoundCreate(NULL, &ds, NULL) == S_OK);
    assert(ds->lpVtbl->CreateSoundBuffer(ds, &desc, &buf, NULL) == S_OK);
    assert(buf->lpVtbl->SetBufferData(buf, pcm, bytes) == S_OK);
    return buf;
}

static void check_state(IDirectSoundBuffer8 *buf, DWORD cursor, DWORD status)
{
    DWORD play = ~0u, write = ~0u, actual_status = ~0u;
    assert(buf->lpVtbl->GetCurrentPosition(buf, &play, &write) == S_OK);
    assert(play == cursor && write == cursor);
    assert(buf->lpVtbl->GetStatus(buf, &actual_status) == S_OK);
    assert(actual_status == status);
}

static void check_sample(int16_t expected)
{
    float out[1][2] = {{0}};
    mixer_render(out, 1);
    assert(out[0][0] == expected && out[0][1] == expected);
}

static void check_playback(WORD channels)
{
    int16_t pcm[8];
    for (int i = 0; i < 4 * channels; i++) pcm[i] = (i / channels + 1) * 100;
    DWORD frame_bytes = channels * sizeof(int16_t);
    IDirectSoundBuffer8 *buf = make_buffer(channels, 48000, pcm, 4 * frame_bytes);
    check_state(buf, 0, 0);
    assert(buf->lpVtbl->SetCurrentPosition(buf, 2 * frame_bytes) == S_OK);
    check_state(buf, 2 * frame_bytes, 0);
    assert(buf->lpVtbl->Play(buf, 0, 0, 0) == S_OK);
    check_sample(300);
    check_state(buf, 3 * frame_bytes, DSBSTATUS_PLAYING);

    /* Play on a playing buffer must neither rewind nor double-count it. */
    assert(buf->lpVtbl->Play(buf, 0, 0, 0) == S_OK);
    assert(g_mixer_active_count == 1);
    assert(buf->lpVtbl->Stop(buf) == S_OK);
    assert(buf->lpVtbl->Stop(buf) == S_OK);
    check_sample(0);
    check_state(buf, 3 * frame_bytes, 0);
    assert(g_mixer_active_count == 0);
    assert(buf->lpVtbl->Play(buf, 0, 0, 0) == S_OK);
    check_sample(400);
    check_state(buf, 0, 0);
    assert(g_mixer_active_count == 0);
    check_sample(0);

    assert(buf->lpVtbl->Play(buf, 0, 0, DSBPLAY_LOOPING) == S_OK);
    check_sample(100);
    assert(buf->lpVtbl->SetCurrentPosition(buf, 3 * frame_bytes + 1) == S_OK);
    check_sample(400);
    check_state(buf, 0, DSBSTATUS_PLAYING | DSBSTATUS_LOOPING);
    check_sample(100);
    assert(buf->lpVtbl->SetCurrentPosition(buf, 4 * frame_bytes) == E_INVALIDARG);
    assert(buf->lpVtbl->SetCurrentPosition(buf, UINT32_MAX) == E_INVALIDARG);
    check_state(buf, frame_bytes, DSBSTATUS_PLAYING | DSBSTATUS_LOOPING);

    /* Updating Play flags takes effect without restarting. */
    assert(buf->lpVtbl->Play(buf, 0, 0, 0) == S_OK);
    float out[5][2] = {{0}};
    mixer_render(out, 5);
    assert(out[0][0] == 200 && out[1][0] == 300 && out[2][0] == 400);
    assert(out[3][0] == 0 && out[4][0] == 0);
    check_state(buf, 0, 0);
    assert(g_mixer_active_count == 0);
    buf->lpVtbl->GetCurrentPosition(buf, NULL, NULL);
    buf->lpVtbl->GetStatus(buf, NULL);
    buf->lpVtbl->Release(buf);
}

/* Overload from early slots must cancel before final clipping. */
static void check_cancellation(void)
{
    int16_t a=30000,b=30000,c=-30000;
    int slots[3];int16_t *pcm[3]={&a,&b,&c};
    for(int i=0;i<3;i++) {
        slots[i]=apu_mixer_alloc_voice();assert(slots[i]>=0);
        APUMixerVoice *v=apu_mixer_get_voice(slots[i]);
        v->pcm_data=pcm[i];v->pcm_bytes=2;v->num_channels=1;v->sample_rate=48000;
        apu_mixer_play(slots[i],0);
    }
    float out[1][2]={{0}};mixer_render(out,1);
    assert(out[0][0]==30000 && out[0][1]==30000);
    for(int i=0;i<3;i++)apu_mixer_free_voice(slots[i]);
}

int main(void)
{
    check_cancellation();
    check_playback(1);
    check_playback(2);

    /* Fractional source frames survive a loop and a Stop/Play boundary. */
    const int16_t pcm[] = {100, 200, 300, 400};
    IDirectSoundBuffer8 *buf = make_buffer(1, 72000, pcm, sizeof(pcm));
    assert(buf->lpVtbl->Play(buf, 0, 0, DSBPLAY_LOOPING) == S_OK);
    check_sample(100);
    check_sample(200);
    check_sample(400);
    check_state(buf, 0, DSBSTATUS_PLAYING | DSBSTATUS_LOOPING);
    assert(buf->lpVtbl->Stop(buf) == S_OK);
    assert(buf->lpVtbl->Play(buf, 0, 0, DSBPLAY_LOOPING) == S_OK);
    check_sample(100);
    check_state(buf, 4, DSBSTATUS_PLAYING | DSBSTATUS_LOOPING);
    /* Replacing storage starts a new buffer; clearing it cannot replay freed PCM. */
    assert(buf->lpVtbl->SetBufferData(buf, pcm, 2 * sizeof(int16_t)) == S_OK);
    check_state(buf, 0, 0);
    assert(buf->lpVtbl->Play(buf, 0, 0, 0) == S_OK);
    check_sample(100);
    assert(buf->lpVtbl->SetBufferData(buf, NULL, 0) == S_OK);
    assert(buf->lpVtbl->Play(buf, 0, 0, 0) == S_OK);
    check_state(buf, 0, 0);
    check_sample(0);
    buf->lpVtbl->Release(buf);

    /* Crossing the old 16.16 limit must not jump back to the first frame. */
    int16_t *long_pcm = calloc(65538, sizeof(int16_t));
    assert(long_pcm);
    long_pcm[65535] = 123;
    long_pcm[65536] = 456;
    long_pcm[65537] = 789;
    buf = make_buffer(1, 48000, long_pcm, 65538 * sizeof(int16_t));
    free(long_pcm);
    assert(buf->lpVtbl->SetCurrentPosition(buf, 65535 * sizeof(int16_t)) == S_OK);
    assert(buf->lpVtbl->Play(buf, 0, 0, 0) == S_OK);
    check_sample(123);
    check_state(buf, 65536 * sizeof(int16_t), DSBSTATUS_PLAYING);
    check_sample(456);
    check_sample(789);
    check_state(buf, 0, 0);
    buf->lpVtbl->Release(buf);

    buf = make_buffer(1, 48000, NULL, 0);
    assert(buf->lpVtbl->Play(buf, 0, 0, 0) == S_OK);
    check_state(buf, 0, 0);
    assert(buf->lpVtbl->SetCurrentPosition(buf, 0) == E_INVALIDARG);
    buf->lpVtbl->Release(buf);
    assert(g_mixer_active_count == 0);
    puts("audio mixer regression: passed");
    return 0;
}
