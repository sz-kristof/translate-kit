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

#include "translator.h"

#ifdef TRANSLATEKIT_WITH_ENGINE

// Engine-backed implementation: wraps the Bergamot inference engine. Each
// Translator owns a BlockingService and one loaded TranslationModel; the
// service is the recommended blocking path (the caller drives it from a single
// IO thread). HTML-aware translation uses ResponseOptions{ HTML = true }, which
// performs detag-and-project so inline tags ride along on the alignment.

#include <future>
#include <sstream>
#include <stdexcept>

#include "common/options.h"
#include "translator/parser.h"
#include "translator/response.h"
#include "translator/response_options.h"
#include "translator/service.h"
#include "translator/translation_model.h"

namespace translatekit {
namespace {

namespace mb = marian::bergamot;

// Minimal Bergamot student-model config used when the caller does not supply a
// pre-generated config. Architecture params (e.g. gemm-precision) are normally
// produced off-device from the model's metadata (SPEC-1 §10a); these are the
// common `tiny`/`base` student defaults. The model/vocab/shortlist paths are set
// explicitly from the ModelSpec below regardless of this base.
std::string BaseConfigYaml() {
    return
        "beam-size: 1\n"
        "normalize: 1.0\n"
        "word-penalty: 0\n"
        "max-length-break: 128\n"
        "mini-batch-words: 1024\n"
        "workspace: 128\n"
        "max-length-factor: 2.0\n"
        "skip-cost: true\n"
        "cpu-threads: 0\n"
        "quiet: true\n"
        "quiet-translation: true\n"
        "gemm-precision: int8shiftAlphaAll\n"
        "alignment: soft\n";
}

std::shared_ptr<marian::Options> BuildOptions(const ModelSpec& spec) {
    // Start from the supplied config if any (it carries the architecture
    // params), else the student defaults. Then force the model/vocab/shortlist
    // paths to the caller's absolute paths so resolution never depends on the
    // config file's location or relative entries.
    std::shared_ptr<marian::Options> options =
        spec.config_path.empty()
            ? mb::parseOptionsFromString(BaseConfigYaml(), /*validate=*/false)
            : mb::parseOptionsFromFilePath(spec.config_path, /*validate=*/false);

    // We pass absolute paths from the ModelSpec, so disable marian's
    // relative-path rooting (model configs ship with `relative-paths: true`).
    options->set("relative-paths", false);

    options->set("models", std::vector<std::string>{spec.model_path});

    std::vector<std::string> vocabs = spec.vocab_paths;
    if (vocabs.size() == 1) vocabs.push_back(vocabs[0]);  // shared src/tgt vocab
    options->set("vocabs", vocabs);

    // shortlist: [<lex file>, <check flag>]
    options->set("shortlist", std::vector<std::string>{spec.shortlist_path, "false"});

    // Bergamot runtime keys the engine requires but a raw marian config lacks
    // (normally injected off-device by patch-marian-for-bergamot.py; SPEC-1 §10a).
    // marian's parser DECLARES these with defaults (so `has()` is always true and
    // the default mini-batch-words=0 aborts BatchingPool); set them outright. These
    // are batching/splitting runtime tuning, not model-output parameters.
    options->set("mini-batch-words", 1024);
    options->set("max-length-break", 128);
    options->set("max-length-factor", 2.5f);
    options->set("ssplit-mode", std::string("paragraph"));
    // HTML-aware translation (detag-and-project) needs word alignments; the
    // student models are trained with guided alignment for exactly this.
    options->set("alignment", std::string("soft"));

    return options;
}

size_t Workers(const ModelSpec& spec) {
    return static_cast<size_t>(spec.num_workers > 0 ? spec.num_workers : 1);
}

mb::AsyncService::Config ServiceConfig(size_t workers) {
    mb::AsyncService::Config config;
    config.numWorkers = workers;
    config.cacheSize = 0;  // no cross-request cache; the caller owns lifecycle
    return config;
}

}  // namespace

struct Translator::Impl {
    // Declared before `service` so it outlives the worker threads, which the
    // service joins in its destructor.
    std::shared_ptr<mb::TranslationModel> model;
    // One worker thread per model replica; requests queued together are batched
    // across sentences by the service's batching pool.
    mb::AsyncService service;

    explicit Impl(const ModelSpec& spec)
        : model(std::make_shared<mb::TranslationModel>(BuildOptions(spec), Workers(spec))),
          service(ServiceConfig(Workers(spec))) {}
};

Translator::Translator(const ModelSpec& spec) : impl_(new Impl(spec)) {}

Translator::~Translator() {
    delete impl_;
}

TranslationResult Translator::translate(const std::string& input, bool is_html) const {
    return translateBatch({input}, is_html).front();
}

std::vector<TranslationResult> Translator::translateBatch(const std::vector<std::string>& inputs,
                                                          bool is_html) const {
    mb::ResponseOptions options;
    options.HTML = is_html;
    // HTML restore (detag-and-project) projects tags onto the target via word
    // alignments, so request them whenever translating HTML.
    options.alignment = is_html;

    // Queue every input before waiting on any, so the batching pool sees all of
    // their sentences at once and the workers can share them out. The callbacks
    // share ownership of the promises: a worker may still be inside set_value()
    // when this thread wakes up and returns.
    auto promises = std::make_shared<std::vector<std::promise<mb::Response>>>(inputs.size());
    std::vector<std::future<mb::Response>> futures;
    futures.reserve(inputs.size());
    for (auto& p : *promises) futures.push_back(p.get_future());
    for (size_t i = 0; i < inputs.size(); ++i) {
        std::string source = inputs[i];
        impl_->service.translate(
            impl_->model, std::move(source),
            [promises, i](mb::Response&& response) { (*promises)[i].set_value(std::move(response)); },
            options);
    }

    std::vector<TranslationResult> results(inputs.size());
    for (size_t i = 0; i < inputs.size(); ++i) {
        results[i].text = futures[i].get().target.text;
    }
    return results;
}

}  // namespace translatekit

#else  // TRANSLATEKIT_WITH_ENGINE

// Stub implementation (host builds / engine disabled): echoes the input so the
// JNI/AAR/distribution path is exercisable without the engine.

namespace translatekit {

struct Translator::Impl {
    ModelSpec spec;
};

Translator::Translator(const ModelSpec& spec) : impl_(new Impl{spec}) {}

Translator::~Translator() {
    delete impl_;
}

TranslationResult Translator::translate(const std::string& input, bool /*is_html*/) const {
    TranslationResult result;
    result.text = input;  // echo
    return result;
}

std::vector<TranslationResult> Translator::translateBatch(const std::vector<std::string>& inputs,
                                                          bool is_html) const {
    std::vector<TranslationResult> results;
    results.reserve(inputs.size());
    for (const std::string& input : inputs) results.push_back(translate(input, is_html));
    return results;
}

}  // namespace translatekit

#endif  // TRANSLATEKIT_WITH_ENGINE
