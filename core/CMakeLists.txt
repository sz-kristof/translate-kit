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

# Platform-agnostic translate-kit engine core.
#
# Produces a static library `translatekit_core` (the C ABI + engine wrappers)
# that every platform wrapper links into its own binding:
#   - Android (android/translate-kit) -> JNI -> libtranslate-kit.so -> AAR
#   - Apple (future)                  -> Obj-C/Swift bridge -> framework
#   - Windows (future)                -> C exports -> DLL
#   - native build (this CMakeLists, directly) -> dev + tests
#
# This file stays platform-neutral. Anything architecture- or compiler-specific
# about the Bergamot engine lives in cmake/BergamotEngine.cmake.
#
# Optionally builds the gtest suite when -DTRANSLATEKIT_BUILD_TESTS=ON, and the
# Bergamot engine when -DTRANSLATEKIT_WITH_ENGINE=ON.

cmake_minimum_required(VERSION 3.18.1)

project(translatekit_core LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

option(TRANSLATEKIT_BUILD_TESTS "Build the translate-kit core unit tests" OFF)
option(TRANSLATEKIT_WITH_ENGINE "Build and link the Bergamot translation engine" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# ---- CLD2: bundled, self-contained offline language detector ---------------
# Apache-2.0. No external deps and no separate model file (language profiles are
# compiled into the library). Source list mirrors the smaller `libcld2` variant
# from third_party/cld2/internal/compile_libs.sh (~80 languages).
set(CLD2_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cld2)
set(CLD2_INTERNAL ${CLD2_DIR}/internal)
set(CLD2_SOURCES
        ${CLD2_INTERNAL}/cldutil.cc
        ${CLD2_INTERNAL}/cldutil_shared.cc
        ${CLD2_INTERNAL}/compact_lang_det.cc
        ${CLD2_INTERNAL}/compact_lang_det_hint_code.cc
        ${CLD2_INTERNAL}/compact_lang_det_impl.cc
        ${CLD2_INTERNAL}/debug.cc
        ${CLD2_INTERNAL}/fixunicodevalue.cc
        ${CLD2_INTERNAL}/generated_entities.cc
        ${CLD2_INTERNAL}/generated_language.cc
        ${CLD2_INTERNAL}/generated_ulscript.cc
        ${CLD2_INTERNAL}/getonescriptspan.cc
        ${CLD2_INTERNAL}/lang_script.cc
        ${CLD2_INTERNAL}/offsetmap.cc
        ${CLD2_INTERNAL}/scoreonescriptspan.cc
        ${CLD2_INTERNAL}/tote.cc
        ${CLD2_INTERNAL}/utf8statetable.cc
        ${CLD2_INTERNAL}/cld_generated_cjk_uni_prop_80.cc
        ${CLD2_INTERNAL}/cld2_generated_cjk_compatible.cc
        ${CLD2_INTERNAL}/generated_distinct_bi_0.cc
        ${CLD2_INTERNAL}/cld_generated_cjk_delta_bi_4.cc
        ${CLD2_INTERNAL}/cld2_generated_quadchrome_2.cc
        ${CLD2_INTERNAL}/cld2_generated_deltaoctachrome.cc
        ${CLD2_INTERNAL}/cld2_generated_distinctoctachrome.cc
        ${CLD2_INTERNAL}/cld_generated_score_quad_octa_2.cc)

add_library(cld2 STATIC ${CLD2_SOURCES})
target_include_directories(cld2 PUBLIC ${CLD2_DIR}/public ${CLD2_INTERNAL})
# CLD2 is decade-old third-party code; silence its warnings so they don't drown
# ours, and downgrade the braced-init narrowing that modern clang promotes to a
# hard error. Keep it position-independent for linking into the shared object.
target_compile_options(cld2 PRIVATE -w -Wno-c++11-narrowing -Wno-narrowing)
set_target_properties(cld2 PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---- translate-kit core ----------------------------------------------------
add_library(translatekit_core STATIC
        src/c_api.cpp
        src/language_detector.cpp
        src/translator.cpp
        src/utf8.cpp)

# Public: the C ABI header. Private: internal C++ headers under src/.
target_include_directories(translatekit_core
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(translatekit_core PRIVATE cld2)

# ---- Version ----------------------------------------------------------------
# Single source of truth is the repo-root VERSION file, the same one
# android/translate-kit/build.gradle reads, so tk_version() cannot drift from
# what a release publishes. -DTRANSLATEKIT_VERSION=x.y.z overrides it (the
# release workflow builds before the version is stamped into the tree).
set(TRANSLATEKIT_VERSION "" CACHE STRING "Version reported by tk_version(); defaults to the root VERSION file")
if(TRANSLATEKIT_VERSION STREQUAL "")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" TRANSLATEKIT_VERSION_RAW)
    string(STRIP "${TRANSLATEKIT_VERSION_RAW}" TRANSLATEKIT_VERSION)
endif()
if(NOT TRANSLATEKIT_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "TRANSLATEKIT_VERSION must be MAJOR.MINOR.PATCH, got '${TRANSLATEKIT_VERSION}'")
endif()
target_compile_definitions(translatekit_core PRIVATE TK_VERSION="${TRANSLATEKIT_VERSION}")

# ---- Bergamot translation engine -------------------------------------------
# Vendored at third_party/translations/inference (mozilla/translations v0.6.0).
# Off by default so the unit tests build without the (large, slow) engine.
# Requires the inference submodules + local engine patches:
#   git submodule update --init --checkout --recursive third_party/translations
#   scripts/apply-engine-patches.sh
if(TRANSLATEKIT_WITH_ENGINE)
    include(BergamotEngine)
    set(ENGINE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/translations/inference)
    translatekit_add_engine(${ENGINE_DIR})
    translatekit_link_engine(translatekit_core ${ENGINE_DIR})
endif()

# Position-independent so it can be linked into a shared object (the .so / JNI lib).
set_target_properties(translatekit_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(TRANSLATEKIT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
