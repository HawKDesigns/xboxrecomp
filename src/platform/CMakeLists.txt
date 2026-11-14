cmake_minimum_required(VERSION 3.20)
project(platform C)

# Platform layer
#   xbox_winnt.h   - Win32 / NT type vocabulary (header only)
#   win32_compat.* - POSIX implementation of generic Win32 host primitives
#
# On Windows the real <windows.h> is used, so this is an INTERFACE library
# that only contributes the src/ include path. On Linux it is a real static
# library carrying the POSIX primitive implementations.

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# src/, the directory this one lives in. Resolved relative to here rather than
# via CMAKE_SOURCE_DIR, which points at the *consuming* project's root when a
# game project pulls xboxrecomp in with add_subdirectory() -- the documented
# integration pattern -- and would send "platform/xbox_winnt.h" looking in the
# game's own src/ tree. PROJECT_SOURCE_DIR is no good either: this file
# declares its own project().
get_filename_component(XBOXRECOMP_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# recomp_exit_hook: work that must run when a window ends the process with
# ExitProcess, which skips atexit (recomp_exit_hook.h). A real library on
# every host, carried by platform, so the D3D windows can call it without
# linking the kernel.
add_library(recomp_exit_hook STATIC recomp_exit_hook.c)
target_include_directories(recomp_exit_hook PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# host_time: the host's monotonic clock and the timer thread's sleep
# (host_time.h). A real library on every host, carried by platform to every
# library of the runtime.
# thread_cpu.c rides along: per-thread CPU time for the periodic report
# (thread_cpu.h), built on the same clock.
add_library(host_time STATIC host_time.c thread_cpu.c)
target_include_directories(host_time PUBLIC
    ${XBOXRECOMP_SRC_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_definitions(host_time PUBLIC XBOX_THREAD_CPU=1)
if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(host_time PUBLIC Threads::Threads)
endif()

if(WIN32)
    add_library(xbox_frame_trace STATIC frame_trace.c)
    target_compile_definitions(xbox_frame_trace PUBLIC XBOX_FRAME_TELEMETRY=1)
    target_compile_definitions(xbox_frame_trace PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_include_directories(xbox_frame_trace PUBLIC ${XBOXRECOMP_SRC_DIR})
    target_link_libraries(xbox_frame_trace PRIVATE host_time)
    add_library(platform INTERFACE)
    target_link_libraries(platform INTERFACE xbox_frame_trace)
    target_link_libraries(platform INTERFACE host_time)
    target_include_directories(platform INTERFACE ${XBOXRECOMP_SRC_DIR})
    target_link_libraries(platform INTERFACE recomp_exit_hook)
else()
    # frame_trace.c rides in the platform library here: it is built on the
    # Win32-style primitives win32_compat.c implements.
    add_library(platform STATIC win32_compat.c frame_trace.c)
    target_compile_definitions(platform PUBLIC XBOX_FRAME_TELEMETRY=1)
    target_include_directories(platform PUBLIC
        ${XBOXRECOMP_SRC_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    find_package(Threads REQUIRED)
    target_link_libraries(platform PUBLIC Threads::Threads recomp_exit_hook host_time)
    if(NOT APPLE)
        target_link_libraries(platform PUBLIC m)   # feclearexcept etc.; libSystem has them on macOS
    endif()
    target_compile_options(platform PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()
