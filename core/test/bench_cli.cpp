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
// tk_translate_batch, at several worker counts, after an untimed warm-up. Run one
// worker count per process for a meaningful peak-memory figure.
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

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#else
#include <sys/resource.h>
#endif

// Peak resident memory of this process so far. Run one worker count per process
// to attribute it to that configuration.
static double PeakMemoryMB() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc = {};
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    return pmc.PeakWorkingSetSize / (1024.0 * 1024.0);
#else
    struct rusage ru = {};
    getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
    return ru.ru_maxrss / (1024.0 * 1024.0);  // bytes
#else
    return ru.ru_maxrss / 1024.0;  // kilobytes
#endif
#endif
}

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

        std::vector<const char*> inputs;
        for (const auto& p : paragraphs) inputs.push_back(p.c_str());
        auto translate_all = [&] {
            std::vector<tk_translation_result> rs(inputs.size());
            tk_translate_batch(m, inputs.data(), inputs.size(), html ? 1 : 0, rs.data());
            for (auto& r : rs) tk_translation_result_free(&r);
        };

        // Warm-up (untimed): every worker builds its model replica on its first
        // batch; a whole-text batch reaches all of them.
        translate_all();

        // one call per paragraph (what a caller without the batch API does)
        auto t0 = clock::now();
        for (const auto& p : paragraphs) {
            tk_translation_result r = {};
            tk_translate(m, p.c_str(), html ? 1 : 0, &r);
            tk_translation_result_free(&r);
        }
        const double single = secs(t0);

        // everything in one batch
        t0 = clock::now();
        translate_all();
        const double batch = secs(t0);

        std::printf("workers %2d | per-paragraph %6.2f s (%5.0f words/s) | batch %6.2f s (%5.0f words/s) | peak RAM %4.0f MB\n",
                    workers, single, words / single, batch, words / batch, PeakMemoryMB());
        tk_model_close(m);
    }
    tk_shutdown(ctx);
    return 0;
}
