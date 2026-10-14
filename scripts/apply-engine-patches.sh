#!/usr/bin/env bash
#
# Copyright 2026 Marcos Holgado
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Applies translate-kit's local patches to the vendored Bergamot engine
# (third_party/translations). Run once after checking out the engine submodules:
#
#   git submodule update --init --checkout --recursive third_party/translations
#   scripts/apply-engine-patches.sh
#
# Idempotent: patches already present are skipped. The patches are NOT committed
# into the submodule (it is upstream); they live in third_party/patches and are
# re-applied to the working tree by this script. `patch` (not `git apply`) is
# used so patches that touch nested submodules (e.g. sentencepiece) apply the
# same way as ones touching regular files.
#
# Patches (all paths relative to third_party/translations):
#   0001-marian-git-revision-submodule-path
#     Fix marian's git_revision.h .git-dir resolution when marian-fork is nested
#     inside a submodule (else ninja fails: "logs/HEAD ... no known rule").
#   0002-marian-drop-werror
#     Drop -Werror from marian's warning set. Modern compilers (e.g. AppleClang)
#     flag warnings the engine's pinned toolchains did not; vendored third-party
#     warnings should not hard-fail our build. Needed for the Apple/native target.
#   0003-sentencepiece-constexpr-const
#     sentencepiece trainer code casts -1 to an enum in a constexpr; newer clang
#     rejects it as non-constant. Use const. (Trainer code is unused by inference.)
#   0004-zlib-classic-mac-fdopen
#     zlib's vendored zutil.h takes its CLASSIC Mac OS branch on any Apple target
#     (TARGET_OS_MAC) and #defines fdopen() to NULL, clobbering the modern macOS
#     SDK declaration. Skip that branch when __APPLE__ (modern macOS has fdopen).
#   0005-faiss-include-x86-intrinsics
#     faiss/VectorTransform.cpp uses __m128/_mm_* under #ifdef __SSE__ but never
#     includes <immintrin.h>; on a normal x86 build it arrives transitively via
#     MKL. The Android x86_64 NDK has no MKL, so include it explicitly. Needed
#     for the x86_64 (emulator/CI) ABI.
#   0006-marian-build-ruy-when-ruy-sgemm
#     marian links ruy under (USE_RUY OR USE_RUY_SGEMM) but only ADDS the ruy
#     subdirectory under USE_RUY, so enabling ruy's float sgemm alone (our x86
#     config: int8 via intgemm, float sgemm via ruy, no MKL/OpenBLAS) references
#     an unbuilt target. Gate the subdirectory on (USE_RUY OR USE_RUY_SGEMM) too.
#     Needed for the x86_64 ABI: without a BLAS, marian's fallback sgemm aborts.
#   0007-marian-idempotent-loggers
#     marian's createLoggers() runs on every model load and registers spdlog
#     loggers ("general", "valid") unconditionally; spdlog throws if a name is
#     already registered, so loading a SECOND model in the same process aborts
#     with "logger with name 'general' already exists". Drop any existing logger
#     before registering so it is idempotent. Runtime fix needed wherever more
#     than one model is loaded per process (switching language pairs, pivoting);
#     all ABIs.
#   0008-marian-targetarch-apple-arm64
#     marian's TargetArch.cmake reads CMAKE_OSX_ARCHITECTURES but only maps the
#     legacy ppc/i386/x86_64 names, so it FATAL_ERRORs ("Invalid OS X arch name:
#     arm64") the moment an Apple build pins arm64 — which every per-arch Apple
#     slice must do (a native build that leaves the arch unpinned dodges this, but
#     cross-compiling x86_64/iOS cannot). Map arm64/arm64e to an "arm" arch code
#     so marian takes its ARM (ruy/simde) path. Needed for the Apple target.
#   0009-ssplit-pcre2-apple-crosscompile
#     ssplit builds PCRE2 via an ExternalProject (SSPLIT_USE_INTERNAL_PCRE2),
#     which spawns a child CMake that does NOT inherit the parent's CMAKE_OSX_*
#     cache vars. So PCRE2 builds for the BUILD HOST: wrong arch/SDK when
#     cross-compiling (iOS, or x86_64 on an arm64 host) and the wrong min-OS stamp
#     on a native macOS slice. Forward CMAKE_SYSTEM_NAME / CMAKE_OSX_ARCHITECTURES
#     / CMAKE_OSX_SYSROOT / CMAKE_OSX_DEPLOYMENT_TARGET into the ExternalProject
#     (mirrors the existing ANDROID block), and force PCRE2_SUPPORT_JIT=OFF on iOS
#     (iOS forbids JIT). On iOS also forward CMAKE_MACOSX_BUNDLE=OFF (CMake makes
#     add_executable() a bundle on iOS, so pcre2's install(TARGETS pcre2grep/...)
#     fails configure with "no BUNDLE DESTINATION") and CMAKE_TRY_COMPILE_TARGET_TYPE
#     =STATIC_LIBRARY (keep the child's configure checks compile-only on the
#     unrunnable cross target). Needed for the Apple target (iOS + universal macOS).
#   0010-cpuinfo-arm-mach-ios
#     ruy's bundled cpuinfo selects its ARM Mach init source (src/arm/mach/init.c,
#     which defines cpuinfo_arm_mach_init + cpuinfo_isa) only when the bare IOS
#     variable is set — which is the case under a community iOS *toolchain file*
#     but NOT when iOS is configured via CMAKE_SYSTEM_NAME=iOS (as translate-kit
#     does). The source is then skipped and ruy fails to link (CpuInfo::NeonDotprod
#     references those symbols). Match CMAKE_SYSTEM_NAME=iOS too, as cpuinfo's own
#     x86-mach and topology branches already do. Needed for the iOS arm slices
#     (device + arm64 simulator). Pair with -DCMAKE_SYSTEM_PROCESSOR=<arch> at
#     configure (build-apple.sh), which is what makes cpuinfo enter the arm branch.
#   0011-marian-msvc-drop-wx
#     The MSVC counterpart of 0002: drop /WX from marian's MSVC warning set. MSVC
#     warns (C4068) on the `#pragma GCC` lines in tensors/cpu/prod_blas.h, which
#     /WX turns into a hard error. Needed for the Windows target.
#
# Patches 0002-0004 are needed to build the engine on modern compilers / macOS
# (the Apple target); 0005-0006 are needed for the x86_64 ABI; 0008-0010 are
# needed for the Apple target (0008 for any pinned-arch Apple slice, 0009 for
# cross-compiled / non-host-OS Apple slices, 0010 for the iOS arm slices). The
# Android arm64 NDK build needs only 0001 to build (the other build patches are
# harmless there); 0007 is a runtime correctness fix that matters on every target.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENGINE_DIR="$REPO_ROOT/third_party/translations"
PATCH_DIR="$REPO_ROOT/third_party/patches"

if [ ! -d "$ENGINE_DIR/inference" ]; then
    echo "error: engine submodule not initialized at $ENGINE_DIR" >&2
    echo "  run: git submodule update --init --checkout --recursive third_party/translations" >&2
    exit 1
fi

shopt -s nullglob
for patch in "$PATCH_DIR"/*.patch; do
    name="$(basename "$patch" .patch)"
    dry="$(patch -p1 -d "$ENGINE_DIR" --forward --dry-run < "$patch" 2>&1 || true)"
    if printf '%s' "$dry" | grep -qiE "previously applied|Reversed"; then
        echo "already applied: $name"
    elif printf '%s' "$dry" | grep -qiE "FAILED|can't find file|No file to patch"; then
        echo "error: cannot apply $name cleanly (engine version drift?)" >&2
        printf '%s\n' "$dry" >&2
        exit 1
    else
        patch -p1 -d "$ENGINE_DIR" --forward < "$patch" >/dev/null
        echo "applied: $name"
    fi
done

echo "engine patches up to date."
