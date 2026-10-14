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

// Throughput benchmark through the C ABI: one tk_translate per paragraph vs one
// tk_translate_batch, at several worker counts.
//
//   bench_cli <model-dir> <src> <tgt> <paragraphs.txt> [--html] [workers...]
//   bench_cli models/huen hu en budapest.txt 1 2 4 8
//
// paragraphs.txt is UTF-8, one paragraph per line (HTML fragments with --html).

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "translate_kit/translate_kit.h"

static std::string FindFile(const std::string& dir, const std::string& prefix, const std::string& suffix) {
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        const std::string name = e.path().filename().string();
        if (name.rfind(prefix, 0) == 0 && name.size() >= suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return e.path().string();
        }
    }
    return std::string();
}

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: %s <model-dir> <src> <tgt> <paragraphs.txt> [--html] [workers...]\n", argv[0]);
        return 2;
    }
    const std::string dir = argv[1];
    std::vector<std::string> paragraphs;
    {
        std::ifstream in(argv[4], std::ios::binary);
        for (std::string line; std::getline(in, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) paragraphs.push_back(line);
        }
    }
    size_t words = 0;
    for (const auto& p : paragraphs) {
        std::istringstream ss(p);
        for (std::string w; ss >> w;) ++words;
    }
    int argi = 5;
    const bool html = argi < argc && std::string(argv[argi]) == "--html";
    if (html) ++argi;
    std::vector<int> workerCounts;
    for (int i = argi; i < argc; ++i) workerCounts.push_back(std::atoi(argv[i]));
    if (workerCounts.empty()) workerCounts = {1};

    const std::string model = FindFile(dir, "model.", ".bin");
    const std::string vocab = FindFile(dir, "vocab.", ".spm");
    const std::string lex = FindFile(dir, "lex.", ".bin");
    const std::string config = FindFile(dir, "config.", ".yml");
    const char* vocabs[] = {vocab.c_str()};

    tk_context* ctx = nullptr;
    if (tk_init(&ctx) != TK_OK) return 1;
    std::printf("%zu paragraphs, %zu words\n", paragraphs.size(), words);

    using clock = std::chrono::steady_clock;
    auto secs = [](clock::time_point a) { return std::chrono::duration<double>(clock::now() - a).count(); };

    for (int workers : workerCounts) {
        tk_model_spec spec = {};
        spec.source_lang = argv[2];
        spec.target_lang = argv[3];
        spec.model_path = model.c_str();
        spec.vocab_paths = vocabs;
        spec.vocab_count = 1;
        spec.shortlist_path = lex.c_str();
        spec.config_path = config.empty() ? nullptr : config.c_str();
        spec.num_workers = workers;
        tk_model* m = nullptr;
        if (tk_load_model(ctx, &spec, &m) != TK_OK) {
            std::fprintf(stderr, "load failed: %s\n", tk_last_error());
            return 1;
        }

        // one call per paragraph (what a caller without the batch API does)
        auto t0 = clock::now();
        for (const auto& p : paragraphs) {
            tk_translation_result r = {};
            tk_translate(m, p.c_str(), html ? 1 : 0, &r);
            tk_translation_result_free(&r);
        }
        const double single = secs(t0);

        // everything in one batch
        std::vector<const char*> inputs;
        for (const auto& p : paragraphs) inputs.push_back(p.c_str());
        std::vector<tk_translation_result> rs(inputs.size());
        t0 = clock::now();
        tk_translate_batch(m, inputs.data(), inputs.size(), html ? 1 : 0, rs.data());
        const double batch = secs(t0);
        for (auto& r : rs) tk_translation_result_free(&r);

        std::printf("workers %2d | per-paragraph %6.2f s (%5.0f words/s) | batch %6.2f s (%5.0f words/s)\n",
                    workers, single, words / single, batch, words / batch);
        tk_model_close(m);
    }
    tk_shutdown(ctx);
    return 0;
}
