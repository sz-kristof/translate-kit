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

#ifndef TRANSLATE_KIT_TRANSLATOR_H
#define TRANSLATE_KIT_TRANSLATOR_H

#include <string>
#include <vector>

namespace translatekit {

// What a caller passes to load one (source -> target) model. Mirrors the C ABI
// tk_model_spec / the Kotlin ModelSpec. Paths are to decompressed files.
struct ModelSpec {
    std::string source_lang;
    std::string target_lang;
    std::string model_path;
    std::vector<std::string> vocab_paths;  // 1 (shared) or 2 (src+tgt)
    std::string shortlist_path;
    std::string config_path;  // empty => engine defaults
    int num_workers = 1;
};

struct TranslationResult {
    std::string text;
    std::vector<float> quality_scores;  // empty unless requested/available
};

// Wraps the Bergamot engine for a single language pair. Drives an AsyncService
// with spec.num_workers worker threads (one model replica each); inputs submitted
// together are batched across sentences by the engine. Without the engine it is a
// stub that echoes input.
class Translator {
public:
    // Throws std::runtime_error on missing files or engine rejection.
    explicit Translator(const ModelSpec& spec);
    ~Translator();

    // Translate one input. When `is_html`, inline tags are preserved/re-aligned.
    TranslationResult translate(const std::string& input, bool is_html) const;

    // Translate several inputs in one go (same order as `inputs`). Much faster
    // than one translate() per input: sentences from all inputs share batches and
    // the batches are spread over the worker threads.
    std::vector<TranslationResult> translateBatch(const std::vector<std::string>& inputs,
                                                  bool is_html) const;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace translatekit

#endif  // TRANSLATE_KIT_TRANSLATOR_H
