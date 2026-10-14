/*
 * Copyright 2026 Kristóf Szerencsi
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef TRANSLATE_KIT_TEST_UTF8_ARGS_H
#define TRANSLATE_KIT_TEST_UTF8_ARGS_H

#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

// The C ABI takes UTF-8. On Windows, main()'s argv is in the ANSI code page
// (so "ő" arrives as a Latin-2 byte or "?"), and the console prints in the OEM
// code page. Re-read the command line as UTF-16, convert it to UTF-8, and switch
// console output to UTF-8. Elsewhere argv is already UTF-8 and this is a copy.
inline std::vector<std::string> Utf8Args(int argc, char** argv) {
    std::vector<std::string> args;
#ifdef _WIN32
    (void)argc;
    (void)argv;
    SetConsoleOutputCP(CP_UTF8);
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    for (int i = 0; w != nullptr && i < n; ++i) {
        const int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(len > 0 ? len - 1 : 0, '\0');
        if (len > 1) WideCharToMultiByte(CP_UTF8, 0, w[i], -1, s.data(), len, nullptr, nullptr);
        args.push_back(std::move(s));
    }
    LocalFree(w);
#else
    args.assign(argv, argv + argc);
#endif
    return args;
}

#endif  // TRANSLATE_KIT_TEST_UTF8_ARGS_H
