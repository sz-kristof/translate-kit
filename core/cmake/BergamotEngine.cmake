# Copyright 2026 Marcos Holgado
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Builds the Bergamot translation engine and wires it into translate-kit.
#
# This module is where ALL of the engine's per-architecture / per-compiler quirks
# live, so the rest of core/ stays platform-neutral. It keys off the target
# ARCHITECTURE (not any particular OS), so the same logic serves Android arm64,
# Apple Silicon, Windows, and x86_64:
#   - arm64  -> marian's ruy + simde path (needs the ARM/FMA/SSE defines + <fenv.h>)
#   - x86_64 -> marian's intgemm/AVX2 path
#
# Public API:
#   translatekit_add_engine(<engine_dir>)          configure + build engine targets
#   translatekit_link_engine(<target> <engine_dir>) link the engine into <target>

# Resolve the TARGET architecture, not the build host. On Apple we cross-compile
# (x86_64 slices on an arm64 host, and vice versa) and CMAKE_SYSTEM_PROCESSOR
# stays the HOST arch — only CMAKE_OSX_ARCHITECTURES carries the real target. We
# must agree with marian's own arm/x86 decision, which it makes from
# CMAKE_OSX_ARCHITECTURES via its (patched) TargetArch.cmake; keying off the host
# here would apply marian's ARM SIMD flags to an x86_64 slice (and skip the x86
# GEMM config below). Outside Apple, CMAKE_OSX_ARCHITECTURES is empty and the host
# processor is the target (Android pins its own arch; Linux/Windows build native).
if(CMAKE_OSX_ARCHITECTURES)
    set(_translatekit_target_arch "${CMAKE_OSX_ARCHITECTURES}")
else()
    set(_translatekit_target_arch "${CMAKE_SYSTEM_PROCESSOR}")
endif()
if(_translatekit_target_arch MATCHES "aarch64|arm64|ARM64")
    set(TRANSLATEKIT_ARCH_ARM TRUE)
else()
    set(TRANSLATEKIT_ARCH_ARM FALSE)
endif()

# marian enables its ARM SIMD path (simde) via the ARM/FMA/SSE defines, and the
# simde headers also need <fenv.h>. marian scopes these to its OWN targets; any
# other target that includes marian headers needs them too. We apply them
# per-target (never globally) so unrelated C dependencies — zlib, pcre2 — are not
# compiled with these defines, which would break their use of system headers
# (e.g. against the macOS SDK).
function(_translatekit_apply_marian_arch_flags target)
    if(TRANSLATEKIT_ARCH_ARM AND TARGET ${target})
        target_compile_definitions(${target} PRIVATE ARM FMA SSE)
        target_compile_options(${target} PRIVATE -include fenv.h)
    endif()
endfunction()

function(translatekit_add_engine engine_dir)
    if(NOT EXISTS ${engine_dir}/CMakeLists.txt)
        message(FATAL_ERROR
            "TRANSLATEKIT_WITH_ENGINE=ON but the engine is missing at ${engine_dir}.\n"
            "Run: git submodule update --init --checkout --recursive third_party/translations\n"
            "Then: scripts/apply-engine-patches.sh")
    endif()

    # CMake 4 removed compatibility with cmake_minimum_required(VERSION < 3.5), which
    # some vendored engine dependencies still declare (e.g. ruy's cpuinfo/deps/clog),
    # so configuring fails outright on CMake 4 (e.g. the windows-latest runner). Let
    # them run under 3.5 policies; patch 0012 forwards this to the external pcre2 build.
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.0 AND NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
        set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    endif()

    # Engine build knobs (shared across platforms).
    set(SSPLIT_USE_INTERNAL_PCRE2 ON CACHE BOOL "" FORCE)  # build PCRE2 for the target
    set(COMPILE_TESTS OFF CACHE BOOL "" FORCE)
    set(COMPILE_UNIT_TESTS OFF CACHE BOOL "" FORCE)
    # marian defaults USE_DOXYGEN=ON and, when doxygen is installed (it is on the
    # windows-latest runner), configures Doxyfile.in, which the vendored engine does
    # not ship: configure fails. We never build the engine's API docs.
    set(USE_DOXYGEN OFF CACHE BOOL "" FORCE)

    # Pin the target CPU so marian never probes the build host. marian defaults
    # BUILD_ARCH=native, which makes it (a) run FindSSE try-compiles against the
    # HOST and (b) pass -march=native. Both are wrong for a distributed,
    # cross-compiled binary: native bakes in whatever CPU happened to build the
    # slice (an x86_64 slice on an arm64 Mac can't even accept -march=native, and
    # host SSE detection is meaningless), and host FindSSE injects bogus x86 -msse*
    # flags even onto an ARM target. So we pin an explicit portable baseline for
    # every cross/distributed target. Only a genuinely native build elsewhere
    # (Linux/Windows host == target) is left on marian's own detection.
    if(ANDROID OR APPLE)
        if(TRANSLATEKIT_ARCH_ARM)
            # arm64: Android phones, macOS arm64, iOS device + arm64 simulator.
            # Pin armv8-a (the 64-bit ARM baseline) — marian's ruy/NEON + simde
            # path. simde's NEON mapping keys off the ARM/FMA/SSE *defines* applied
            # per-target below, NOT the -msse* compiler flags, so dropping FindSSE
            # is safe; this just avoids host-native over-targeting (e.g. baking the
            # build Mac's M-series features into an older-iPhone binary).
            set(BUILD_ARCH "armv8-a" CACHE STRING "" FORCE)
        else()
            # x86_64: Android emulator/CI, macOS x86_64, iOS x86_64 simulator —
            # marian's intgemm path, all cross-compiled on an arm64 host here. Pin
            # x86-64-v2 (SSE3/SSSE3/SSE4.2, no AVX) so marian compiles with a valid
            # target -march (never the host's "native") AND never emits AVX that
            # SIGILLs on an AVX2-less x86 emulator. We deliberately do NOT pin AVX2:
            # intgemm runtime-dispatches its AVX2/SSSE3 int8 kernels via CPUID
            # independent of this -march, so the int8 hot path still uses AVX2 where
            # the CPU has it — pinning v2 costs nothing there and buys a portable
            # floor for a distributed slice + emulator/CI robustness.
            set(BUILD_ARCH "x86-64-v2" CACHE STRING "" FORCE)
        endif()
    endif()

    # marian needs a float32 GEMM (sgemm) for batched products (attention). On
    # ARM it routes sgemm through ruy (marian sets USE_RUY_SGEMM itself). On x86
    # it expects MKL/OpenBLAS, and with neither present marian's fallback sgemm is
    # a stub that aborts at the first translate (BeamSearch -> ProdBatched ->
    # sgemm -> abort). Apple ships a BLAS: marian auto-selects Accelerate on APPLE
    # (its BLAS block takes the `APPLE AND USE_APPLE_ACCELERATE` branch BEFORE the
    # ruy-sgemm one), so the float path is already covered on every Apple slice —
    # the same Accelerate backend the arm64 slice uses — and ruy's sgemm is neither
    # needed nor reachable there (forcing it would only build dead ruy code). Only
    # Android's x86_64 has no BLAS, so route ITS float sgemm through ruy (portable;
    # ruy has x86 AVX2/SSE kernels). int8 stays on intgemm. Requires engine patch
    # 0006 (marian builds ruy under USE_RUY_SGEMM, not only USE_RUY).
    if(NOT TRANSLATEKIT_ARCH_ARM AND NOT APPLE)
        set(USE_RUY_SGEMM ON CACHE BOOL "" FORCE)
    endif()

    add_subdirectory(${engine_dir} ${CMAKE_BINARY_DIR}/bergamot EXCLUDE_FROM_ALL)

    # The bergamot wrapper and marian itself include marian's SIMD headers.
    _translatekit_apply_marian_arch_flags(marian)
    _translatekit_apply_marian_arch_flags(bergamot-translator-source)
endfunction()

function(translatekit_link_engine target engine_dir)
    target_link_libraries(${target} PRIVATE bergamot-translator-source)
    target_include_directories(${target} PRIVATE
            ${engine_dir}/src
            ${engine_dir}/marian-fork/src
            ${engine_dir}/marian-fork/src/3rd_party
            ${engine_dir}/3rd_party/ssplit-cpp/src)
    target_compile_definitions(${target} PRIVATE TRANSLATEKIT_WITH_ENGINE=1)
    # Our translator.cpp includes marian headers, so it needs the same arch flags.
    _translatekit_apply_marian_arch_flags(${target})
endfunction()
