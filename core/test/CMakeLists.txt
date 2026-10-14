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

# Host (developer-machine) gtest suite for the C ABI. Fetches GoogleTest.

include(FetchContent)
FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(translatekit_core_test c_api_test.cpp utf8_test.cpp)
target_link_libraries(translatekit_core_test PRIVATE translatekit_core GTest::gtest_main)
# utf8_test exercises an internal helper directly.
target_include_directories(translatekit_core_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
# Absolute path to the repo-root VERSION file, so VersionMatchesVersionFile can
# read it at run time regardless of the working directory the test runs from.
target_compile_definitions(translatekit_core_test
        PRIVATE TK_VERSION_FILE="${CMAKE_CURRENT_SOURCE_DIR}/../../VERSION")

include(GoogleTest)
gtest_discover_tests(translatekit_core_test)

# Manual dev tool for interactive language-detection testing (not shipped).
add_executable(detect_cli detect_cli.cpp)
target_link_libraries(detect_cli PRIVATE translatekit_core)

# Let the tests know whether the real engine is compiled in (vs the echo stub),
# so they assert the right behaviour.
if(TRANSLATEKIT_WITH_ENGINE)
    target_compile_definitions(translatekit_core_test PRIVATE TRANSLATEKIT_WITH_ENGINE=1)
    target_compile_definitions(detect_cli PRIVATE TRANSLATEKIT_WITH_ENGINE=1)

    # Manual dev tool for translation testing (only meaningful with the engine).
    add_executable(translate_cli translate_cli.cpp)
    target_link_libraries(translate_cli PRIVATE translatekit_core)
    target_compile_definitions(translate_cli PRIVATE TRANSLATEKIT_WITH_ENGINE=1)

    # Throughput benchmark: per-paragraph tk_translate vs tk_translate_batch.
    add_executable(bench_cli bench_cli.cpp)
    target_link_libraries(bench_cli PRIVATE translatekit_core)
endif()
