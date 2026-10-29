cmake_minimum_required(VERSION 3.20)

# Xbox Kernel Replacement Layer
# Replaces 147 Xbox kernel imports with Win32 equivalents.
# Memory layout, file I/O, threading, synchronization, crypto, HAL.

set(KERNEL_SOURCES
    kernel_path.c
    kernel_pool.c
    kernel_rtl.c
    kernel_memory.c
    kernel_file.c
    kernel_missing.c
    kernel_thread.c
    kernel_sync.c
    kernel_hal.c
    kernel_guest_cpu.c
    kernel_xbox.c
    kernel_ob.c
    kernel_io.c
    kernel_crypto.c
    kernel_thunks.c
    kernel_bridge.c
    xbox_memory_layout.c
    guest_vmem.c
    icall_feedback.c
    recomp_trace.c
    nv2a_pb_scan.c
    nv2a_pb_exec.c
    nv2a_flip_hold.c
    nv2a_vsh_interp.c
    nv2a_combiner.c
    nv2a_gpu_raster.c
    nv2a_gpu_tiles.c
)

# Build the generic embedded shader locally from source. Optional QPR2 literal
# bodies are generated from a user-owned GPU capture, never committed.
if(WIN32)
    set(QPR2_SHADER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
    set(QPR2_SHADER_INPUTS)
    foreach(name nv2a_gpu_raster.hlsl nv2a_gpu_rov.hlsl nv2a_gpu_report_reset.hlsl
                 nv2a_gpu_coverage.hlsl nv2a_gpu_clear.hlsl nv2a_gpu_types.h
                 nv2a_gpu_pixel.inc nv2a_gpu_literal.inc nv2a_gpu_compact.inc
                 nv2a_gpu_shade.inc nv2a_gpu_source.inc nv2a_gpu_geometry.hlsl
                 nv2a_gpu_texcopy.hlsl nv2a_gpu_raw_types.h nv2a_gpu_vsh.inc
                 nv2a_gpu_prepare.inc)
        list(APPEND QPR2_SHADER_INPUTS "${QPR2_SHADER_ROOT}/${name}")
    endforeach()
    add_custom_command(OUTPUT "${QPR2_SHADER_ROOT}/nv2a_gpu_shader.h"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT_ROOT=${QPR2_SHADER_ROOT}"
                -P "${QPR2_SHADER_ROOT}/embed_qpr2_gpu_shader.cmake"
        DEPENDS ${QPR2_SHADER_INPUTS} "${QPR2_SHADER_ROOT}/embed_qpr2_gpu_shader.cmake"
        VERBATIM)
    add_custom_target(xbox_kernel_shader_sources DEPENDS "${QPR2_SHADER_ROOT}/nv2a_gpu_shader.h")
endif()

add_library(xbox_kernel STATIC ${KERNEL_SOURCES})
if(WIN32)
    add_dependencies(xbox_kernel xbox_kernel_shader_sources)
endif()

# See src/platform/CMakeLists.txt: resolve src/ relative to this file, not via
# CMAKE_SOURCE_DIR, which is the consuming project's root under add_subdirectory().
get_filename_component(XBOXRECOMP_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

target_include_directories(xbox_kernel PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${XBOXRECOMP_SRC_DIR}
)

if(WIN32)
    # platform is an INTERFACE target on Windows: it contributes the src/
    # include path that "platform/xbox_winnt.h" resolves against.
    target_link_libraries(xbox_kernel PUBLIC platform kernel32 bcrypt d3d11 dxgi dxguid d3dcompiler)
    target_compile_definitions(xbox_kernel PRIVATE NV2A_ENABLE_GPU_RASTER=1)
else()
    # Linux: platform layer (POSIX host primitives) + OpenSSL for crypto.
    find_package(OpenSSL REQUIRED)
    target_link_libraries(xbox_kernel PUBLIC platform OpenSSL::Crypto)
endif()

if(MSVC)
    # Shader specializations retain the interpreter's operation ordering.
    set_source_files_properties(nv2a_vsh_interp.c PROPERTIES COMPILE_OPTIONS "/fp:precise")
    target_compile_options(xbox_kernel PRIVATE /W4 /WX- /O2 /GS- /Gy)
    target_compile_definitions(xbox_kernel PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    set_source_files_properties(nv2a_vsh_interp.c PROPERTIES COMPILE_OPTIONS "-fno-fast-math;-ffp-contract=off")
    target_compile_options(xbox_kernel PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()

# The trapped-MMIO dispatch (xbox_VehMmioFault, xbox_PosixMmioFault) routes
# to the APU and the USB OHCI models. Link-only: both already call back into
# the kernel, and CMake repeats static libraries in such a cycle.
target_link_libraries(xbox_kernel PRIVATE xbox_apu xbox_usb)

target_compile_definitions(xbox_kernel PRIVATE $<$<CONFIG:Debug>:_DEBUG>)
