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

// Implements the translate_kit.h C ABI on top of the C++ LanguageDetector and
// Translator. This file is platform-neutral; the Android JNI shim and any
// future iOS/Windows bridge call only these functions.

#include "translate_kit/translate_kit.h"

#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

#include "language_detector.h"
#include "translator.h"
#include "utf8.h"

namespace {

// Per-thread last-error message, exposed via tk_last_error.
thread_local std::string g_last_error;

void set_error(const char* msg) { g_last_error = msg ? msg : ""; }
void clear_error() { g_last_error.clear(); }

char* dup_cstr(const std::string& s) {
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (out == nullptr) return nullptr;
    std::memcpy(out, s.c_str(), s.size() + 1);
    return out;
}

// Copies a translation into a caller-owned result. False on allocation failure
// (the text could not be copied); a missing quality buffer is not an error.
bool fill_result(const translatekit::TranslationResult& r, tk_translation_result* out) {
    out->text = dup_cstr(r.text);
    if (out->text == nullptr) return false;
    if (!r.quality_scores.empty()) {
        const size_t n = r.quality_scores.size();
        out->quality_scores = static_cast<float*>(std::malloc(n * sizeof(float)));
        if (out->quality_scores != nullptr) {
            std::memcpy(out->quality_scores, r.quality_scores.data(), n * sizeof(float));
            out->quality_len = n;
        }
    }
    return true;
}

}  // namespace

// Opaque handle definitions.
struct tk_context {
    translatekit::LanguageDetector detector;
};

struct tk_model {
    translatekit::Translator translator;
    explicit tk_model(const translatekit::ModelSpec& spec) : translator(spec) {}
};

extern "C" {

#ifndef TK_VERSION
#error "TK_VERSION not defined - see the Version block in core/CMakeLists.txt"
#endif
const char* tk_version(void) { return TK_VERSION; }

const char* tk_last_error(void) { return g_last_error.c_str(); }

tk_status tk_init(tk_context** out_ctx) {
    clear_error();
    if (out_ctx == nullptr) {
        set_error("tk_init: out_ctx is null");
        return TK_ERR_INVALID_ARG;
    }
    *out_ctx = nullptr;
    try {
        *out_ctx = new tk_context();
        return TK_OK;
    } catch (const std::exception& e) {
        set_error(e.what());
        return TK_ERR_MODEL_LOAD;
    } catch (...) {
        set_error("tk_init: unknown error");
        return TK_ERR_INTERNAL;
    }
}

tk_status tk_detect_language(tk_context* ctx,
                             const char* text,
                             tk_language_result* out_result) {
    clear_error();
    if (out_result == nullptr) {
        set_error("tk_detect_language: out_result is null");
        return TK_ERR_INVALID_ARG;
    }
    out_result->language[0] = '\0';
    out_result->confidence = 0.0f;
    if (ctx == nullptr) {
        set_error("tk_detect_language: context not initialized");
        return TK_ERR_NOT_INITIALIZED;
    }
    if (text == nullptr) {
        set_error("tk_detect_language: text is null");
        return TK_ERR_INVALID_ARG;
    }
    try {
        translatekit::DetectedLanguage d = ctx->detector.detect(std::string(text));
        std::snprintf(out_result->language, sizeof(out_result->language), "%s",
                      d.language.c_str());
        out_result->confidence = d.confidence;
        return TK_OK;
    } catch (const std::exception& e) {
        set_error(e.what());
        return TK_ERR_INTERNAL;
    } catch (...) {
        set_error("tk_detect_language: unknown error");
        return TK_ERR_INTERNAL;
    }
}

tk_status tk_load_model(tk_context* ctx,
                        const tk_model_spec* spec,
                        tk_model** out_model) {
    clear_error();
    if (out_model == nullptr) {
        set_error("tk_load_model: out_model is null");
        return TK_ERR_INVALID_ARG;
    }
    *out_model = nullptr;
    if (ctx == nullptr) {
        set_error("tk_load_model: context not initialized");
        return TK_ERR_NOT_INITIALIZED;
    }
    if (spec == nullptr || spec->model_path == nullptr || spec->model_path[0] == '\0') {
        set_error("tk_load_model: spec/model_path is null/empty");
        return TK_ERR_INVALID_ARG;
    }
    try {
        translatekit::ModelSpec cpp_spec;
        cpp_spec.source_lang = spec->source_lang ? spec->source_lang : "";
        cpp_spec.target_lang = spec->target_lang ? spec->target_lang : "";
        cpp_spec.model_path = spec->model_path;
        cpp_spec.shortlist_path = spec->shortlist_path ? spec->shortlist_path : "";
        cpp_spec.config_path = spec->config_path ? spec->config_path : "";
        cpp_spec.num_workers = spec->num_workers > 0 ? spec->num_workers : 1;
        for (size_t i = 0; i < spec->vocab_count; ++i) {
            if (spec->vocab_paths != nullptr && spec->vocab_paths[i] != nullptr) {
                cpp_spec.vocab_paths.emplace_back(spec->vocab_paths[i]);
            }
        }
        *out_model = new tk_model(cpp_spec);
        return TK_OK;
    } catch (const std::runtime_error& e) {
        set_error(e.what());
        return TK_ERR_MODEL_LOAD;
    } catch (const std::exception& e) {
        set_error(e.what());
        return TK_ERR_INTERNAL;
    } catch (...) {
        set_error("tk_load_model: unknown error");
        return TK_ERR_INTERNAL;
    }
}

tk_status tk_translate(tk_model* model,
                       const char* input,
                       int is_html,
                       tk_translation_result* out_result) {
    clear_error();
    if (out_result == nullptr) {
        set_error("tk_translate: out_result is null");
        return TK_ERR_INVALID_ARG;
    }
    out_result->text = nullptr;
    out_result->quality_scores = nullptr;
    out_result->quality_len = 0;
    if (model == nullptr) {
        set_error("tk_translate: model is null");
        return TK_ERR_INVALID_ARG;
    }
    if (input == nullptr) {
        set_error("tk_translate: input is null");
        return TK_ERR_INVALID_ARG;
    }
    try {
        // A single ill-formed byte makes the engine drop the whole input and
        // return an empty translation, so repair it to U+FFFD first.
        translatekit::TranslationResult r =
            model->translator.translate(translatekit::SanitizeUtf8(input), is_html != 0);
        if (!fill_result(r, out_result)) {
            set_error("tk_translate: out of memory");
            return TK_ERR_INTERNAL;
        }
        return TK_OK;
    } catch (const std::exception& e) {
        set_error(e.what());
        return TK_ERR_TRANSLATE;
    } catch (...) {
        set_error("tk_translate: unknown error");
        return TK_ERR_TRANSLATE;
    }
}

tk_status tk_translate_batch(tk_model* model,
                             const char* const* inputs,
                             size_t count,
                             int is_html,
                             tk_translation_result* out_results) {
    clear_error();
    if (count > 0 && (inputs == nullptr || out_results == nullptr)) {
        set_error("tk_translate_batch: inputs/out_results is null");
        return TK_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < count; ++i) out_results[i] = tk_translation_result{};
    if (model == nullptr) {
        set_error("tk_translate_batch: model is null");
        return TK_ERR_INVALID_ARG;
    }
    std::vector<std::string> sources;
    sources.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        if (inputs[i] == nullptr) {
            set_error("tk_translate_batch: an input is null");
            return TK_ERR_INVALID_ARG;
        }
        sources.push_back(translatekit::SanitizeUtf8(inputs[i]));
    }
    try {
        std::vector<translatekit::TranslationResult> rs =
            model->translator.translateBatch(sources, is_html != 0);
        for (size_t i = 0; i < count; ++i) {
            if (!fill_result(rs[i], &out_results[i])) {
                for (size_t j = 0; j <= i; ++j) tk_translation_result_free(&out_results[j]);
                set_error("tk_translate_batch: out of memory");
                return TK_ERR_INTERNAL;
            }
        }
        return TK_OK;
    } catch (const std::exception& e) {
        set_error(e.what());
        return TK_ERR_TRANSLATE;
    } catch (...) {
        set_error("tk_translate_batch: unknown error");
        return TK_ERR_TRANSLATE;
    }
}

void tk_translation_result_free(tk_translation_result* result) {
    if (result == nullptr) return;
    std::free(result->text);
    std::free(result->quality_scores);
    result->text = nullptr;
    result->quality_scores = nullptr;
    result->quality_len = 0;
}

void tk_model_close(tk_model* model) { delete model; }

void tk_shutdown(tk_context* ctx) { delete ctx; }

}  // extern "C"
