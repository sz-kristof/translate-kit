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

// Host dev tool for manual translation testing through the C ABI. Points the
// engine at an extracted Bergamot model directory (model + vocab + shortlist +
// config, e.g. from scripts/fetch-models.sh) and translates argv text.
//
//   translate_cli <model-dir> <src> <tgt> [--html] "text to translate"
//   translate_cli models/test-models/ende.student.tiny11 en de "Hello World!"
//   translate_cli models/test-models/ende.student.tiny11 en de --html "<b>Hello World!</b>"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "translate_kit/translate_kit.h"
#include "utf8_args.h"

// Find the first file in `dir` whose name starts with `prefix` and ends with
// `suffix` (model bundles vary the middle, e.g. vocab.deen.spm).
static std::string FindFile(const std::string& dir, const std::string& prefix,
                            const std::string& suffix) {
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        const std::string name = e.path().filename().string();
        if (name.rfind(prefix, 0) == 0 &&
            name.size() >= suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return e.path().string();
        }
    }
    return std::string();
}

int main(int argc, char** argv) {
    const std::vector<std::string> args = Utf8Args(argc, argv);
    if (args.size() < 5) {
        std::fprintf(stderr,
                     "usage: %s <model-dir> <src> <tgt> [--html] \"text\"\n", argv[0]);
        return 2;
    }
    const std::string dir = args[1];
    const std::string src = args[2];
    const std::string tgt = args[3];

    size_t argi = 4;
    bool isHtml = false;
    if (args[argi] == "--html") {
        isHtml = true;
        ++argi;
    }
    std::string text;
    for (; argi < args.size(); ++argi) {
        if (!text.empty()) text += ' ';
        text += args[argi];
    }

    const std::string model = FindFile(dir, "model.", ".bin");
    const std::string vocab = FindFile(dir, "vocab.", ".spm");
    const std::string lex = FindFile(dir, "lex.", ".bin");
    const std::string config = FindFile(dir, "config.", ".yml");
    const char* vocabs[] = {vocab.c_str()};

    tk_context* ctx = nullptr;
    if (tk_init(&ctx) != TK_OK) {
        std::fprintf(stderr, "init failed: %s\n", tk_last_error());
        return 1;
    }

    tk_model_spec spec = {};
    spec.source_lang = src.c_str();
    spec.target_lang = tgt.c_str();
    spec.model_path = model.c_str();
    spec.vocab_paths = vocabs;
    spec.vocab_count = 1;
    spec.shortlist_path = lex.c_str();
    spec.config_path = config.c_str();
    spec.num_workers = 1;

    tk_model* m = nullptr;
    if (tk_load_model(ctx, &spec, &m) != TK_OK) {
        std::fprintf(stderr, "loadModel failed: %s\n", tk_last_error());
        tk_shutdown(ctx);
        return 1;
    }

    tk_translation_result r = {};
    if (tk_translate(m, text.c_str(), isHtml ? 1 : 0, &r) != TK_OK) {
        std::fprintf(stderr, "translate failed: %s\n", tk_last_error());
        tk_model_close(m);
        tk_shutdown(ctx);
        return 1;
    }

    std::printf("%s\n", r.text != nullptr ? r.text : "");
    tk_translation_result_free(&r);
    tk_model_close(m);
    tk_shutdown(ctx);
    return 0;
}
