/*
 * Copyright 2026 Marcos Holgado
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

// Tiny host dev tool for manual language-detection testing through the C ABI.
// Reads text from argv (joined) or, if none given, from stdin, and prints the
// detected language + confidence.
//
//   echo "Bonjour le monde, comment allez-vous" | ./build/test/detect_cli
//   ./build/test/detect_cli "Hola, esto es una prueba de deteccion"

#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>

#include "translate_kit/translate_kit.h"
#include "utf8_args.h"

int main(int argc, char** argv) {
    const std::vector<std::string> args = Utf8Args(argc, argv);
    std::string text;
    if (args.size() > 1) {
        for (size_t i = 1; i < args.size(); ++i) {
            if (i > 1) text += ' ';
            text += args[i];
        }
    } else {
        std::ostringstream ss;
        ss << std::cin.rdbuf();
        text = ss.str();
    }

    tk_context* ctx = nullptr;
    if (tk_init(&ctx) != TK_OK) {
        std::fprintf(stderr, "init failed: %s\n", tk_last_error());
        return 1;
    }

    tk_language_result result = {};
    const tk_status status = tk_detect_language(ctx, text.c_str(), &result);
    if (status != TK_OK) {
        std::fprintf(stderr, "detect failed: %s\n", tk_last_error());
        tk_shutdown(ctx);
        return 1;
    }

    std::printf("language=%s confidence=%.2f\n", result.language, result.confidence);
    tk_shutdown(ctx);
    return 0;
}
