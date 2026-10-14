# Copyright 2026 Kristóf Szerencsi
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

# Windows build: the platform-neutral core + Bergamot engine as translatekit.dll,
# exposing only the C ABI (core/include/translate_kit/translate_kit.h), plus a
# smoke test that consumes the DLL through its import library.
#
#   scripts/build-windows.ps1     (or configure this directory with MSVC + Ninja)

cmake_minimum_required(VERSION 3.22)

# Statically link the C/C++ runtime (/MT) so the DLL has no Visual C++
# Redistributable dependency. This is also what marian asks for in its own MSVC
# release flags; engine patch 0012 forwards it to the external pcre2 build so
# every object agrees on the runtime.
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "")

project(translatekit_windows LANGUAGES C CXX)

if(NOT MSVC)
    message(FATAL_ERROR "windows/ is the MSVC build; use core/ directly for other toolchains")
endif()

set(TRANSLATEKIT_WITH_ENGINE ON CACHE BOOL "" FORCE)

# A distributed DLL must not bake in the build machine's CPU. marian maps its
# default BUILD_ARCH=native to /arch:AVX2 on MSVC, which faults on AVX2-less x64
# CPUs. Pin the same portable x86-64-v2 baseline the Android/Apple x86_64 slices
# use (core/cmake/BergamotEngine.cmake); intgemm still dispatches its AVX2 int8
# kernels at runtime via CPUID.
set(BUILD_ARCH "x86-64-v2" CACHE STRING "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../core ${CMAKE_CURRENT_BINARY_DIR}/translatekit_core)

# translatekit_core is a static library whose C ABI functions are marked
# __declspec(dllexport). A DLL only pulls the archive members it references, so
# link it whole to keep every export.
add_library(translatekit SHARED dll.c)
target_link_libraries(translatekit PRIVATE translatekit_core)
target_link_options(translatekit PRIVATE "/WHOLEARCHIVE:$<TARGET_FILE:translatekit_core>")
target_include_directories(translatekit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../core/include)

# Same C smoke test the Apple slices run, here linked against the DLL's import
# library only, so it proves the exported surface is complete.
add_executable(tk_smoke ${CMAKE_CURRENT_SOURCE_DIR}/../apple/smoke/smoke.c)
target_link_libraries(tk_smoke PRIVATE translatekit)
target_compile_definitions(tk_smoke PRIVATE TK_USING_DLL)
