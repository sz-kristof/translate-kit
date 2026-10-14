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

// Phase A: exercises the C ABI against the stub engine (echo translate, neutral
// language detection). Phase B/C add golden detection/translation cases here.

#include "translate_kit/translate_kit.h"

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

tk_context* MakeContext() {
    tk_context* ctx = nullptr;
    EXPECT_EQ(tk_init(&ctx), TK_OK);
    EXPECT_NE(ctx, nullptr);
    return ctx;
}

tk_model* LoadStubModel(tk_context* ctx) {
    const char* vocab[] = {"/tmp/vocab.spm"};
    tk_model_spec spec = {};
    spec.source_lang = "es";
    spec.target_lang = "en";
    spec.model_path = "/tmp/model.bin";
    spec.vocab_paths = vocab;
    spec.vocab_count = 1;
    spec.shortlist_path = "/tmp/lex.bin";
    spec.config_path = nullptr;
    spec.num_workers = 1;
    tk_model* model = nullptr;
    EXPECT_EQ(tk_load_model(ctx, &spec, &model), TK_OK);
    EXPECT_NE(model, nullptr);
    return model;
}

TEST(CApi, VersionIsNonEmpty) {
    EXPECT_STRNE(tk_version(), "");
}

// The version has two consumers that must agree: the Android AAR reads the
// repo-root VERSION file at build time, and tk_version() gets it from CMake
// (see the Version block in core/CMakeLists.txt). Reading the file again HERE,
// at run time, is what makes that wiring load-bearing: hardcode a literal back
// into c_api.cpp and this fails.
TEST(CApi, VersionMatchesVersionFile) {
    std::ifstream file(TK_VERSION_FILE);
    ASSERT_TRUE(file.is_open()) << "cannot open " << TK_VERSION_FILE;

    std::string expected;
    ASSERT_TRUE(std::getline(file, expected)) << "VERSION file is empty";
    while (!expected.empty() && (expected.back() == '\r' || expected.back() == ' ')) {
        expected.pop_back();
    }

    EXPECT_EQ(std::string(tk_version()), expected);
}

TEST(CApi, InitRejectsNullOutArg) {
    EXPECT_EQ(tk_init(nullptr), TK_ERR_INVALID_ARG);
}

TEST(CApi, DetectLanguageReturnsResult) {
    tk_context* ctx = MakeContext();
    tk_language_result r = {};
    EXPECT_EQ(tk_detect_language(ctx, "hola mundo", &r), TK_OK);
    EXPECT_STRNE(r.language, "");
    EXPECT_GE(r.confidence, 0.0f);
    EXPECT_LE(r.confidence, 1.0f);
    tk_shutdown(ctx);
}

// Golden language-detection cases against the bundled CLD2 detector. Sentences
// are long enough for CLD2 to score reliably.
TEST(CApi, DetectLanguageGoldens) {
    tk_context* ctx = MakeContext();
    struct Case {
        const char* text;
        const char* expected;
    };
    const Case cases[] = {
        {"This is a simple sentence written in the English language for testing purposes.", "en"},
        {"Esta es una frase sencilla escrita en el idioma español para realizar algunas pruebas.", "es"},
        {"Ceci est une phrase simple écrite en langue française afin d'effectuer quelques tests.", "fr"},
        {"Dies ist ein einfacher Satz, der zu Testzwecken in deutscher Sprache geschrieben wurde.", "de"},
        {"Questa è una frase semplice scritta in lingua italiana per scopi di test e verifica.", "it"},
    };
    for (const Case& c : cases) {
        tk_language_result r = {};
        ASSERT_EQ(tk_detect_language(ctx, c.text, &r), TK_OK) << c.text;
        EXPECT_STREQ(r.language, c.expected) << "text: " << c.text;
        EXPECT_GT(r.confidence, 0.0f) << c.text;
    }
    tk_shutdown(ctx);
}

TEST(CApi, DetectLanguageEmptyIsUnknown) {
    tk_context* ctx = MakeContext();
    tk_language_result r = {};
    EXPECT_EQ(tk_detect_language(ctx, "", &r), TK_OK);
    EXPECT_STREQ(r.language, "und");
    EXPECT_FLOAT_EQ(r.confidence, 0.0f);
    tk_shutdown(ctx);
}

// Caller-supplied bytes are not guaranteed to be UTF-8 (e.g. Latin-1/2 text, or
// Java's "modified UTF-8" from JNI GetStringUTFChars, which encodes U+0000 as
// C0 80). CLD2 reads out of bounds on such input unless it is sanitized first;
// each of these crashed the process before the fix.
TEST(CApi, DetectLanguageIllFormedUtf8DoesNotCrash) {
    tk_context* ctx = MakeContext();
    const char* inputs[] = {
        "\xF6",
        "abc\xC0\x80" "def",
        "Hello world, this is text \xC0\x80 with a NUL from Java.",
        "Ez egy magyar mondat a b\xF6ng\xE9sz\xF5r\xF5l.",
        "abc \xC1 def",
        "abc \xEE def",
        "abc \xF7 def",
        "\xC3",
    };
    for (const char* in : inputs) {
        tk_language_result r = {};
        EXPECT_EQ(tk_detect_language(ctx, in, &r), TK_OK) << in;
        EXPECT_GE(r.confidence, 0.0f);
        EXPECT_LE(r.confidence, 1.0f);
    }
    tk_shutdown(ctx);
}

// Unsanitized, these either crash or return garbage (a lone truncated byte came
// back as "el" with confidence 1.0), depending on what memory CLD2 reads.
TEST(CApi, DetectLanguageIllFormedUtf8Goldens) {
    tk_context* ctx = MakeContext();
    struct Case { const char* text; const char* lang; };
    const Case cases[] = {
        {"\xC3", "und"},
        {"\xF6", "und"},
        {"abc\xC0\x80" "def", "und"},
        {"Ez egy magyar mondat a b\xF6ng\xE9sz\xF5r\xF5l.", "hu"},  // Latin-2 bytes
        {"Das ist ein deutscher Satz \xC3", "de"},
        {"The quick brown fox jumps over the lazy dog \xF6 while the cat "
         "sleeps quietly in the warm afternoon sun.", "en"},
    };
    for (const Case& c : cases) {
        tk_language_result r = {};
        ASSERT_EQ(tk_detect_language(ctx, c.text, &r), TK_OK) << c.text;
        EXPECT_STREQ(r.language, c.lang) << c.text;
    }
    tk_shutdown(ctx);
}

TEST(CApi, DetectLanguageWithoutContextFails) {
    tk_language_result r = {};
    EXPECT_EQ(tk_detect_language(nullptr, "x", &r), TK_ERR_NOT_INITIALIZED);
}

#ifndef TRANSLATEKIT_WITH_ENGINE
TEST(CApi, TranslateEchoesInputInStub) {
    tk_context* ctx = MakeContext();
    tk_model* model = LoadStubModel(ctx);

    const std::string input = "Hola <b>mundo</b>";
    tk_translation_result r = {};
    EXPECT_EQ(tk_translate(model, input.c_str(), /*is_html=*/1, &r), TK_OK);
    ASSERT_NE(r.text, nullptr);
    EXPECT_EQ(std::string(r.text), input);  // stub echoes
    tk_translation_result_free(&r);

    tk_model_close(model);
    tk_shutdown(ctx);
}
TEST(CApi, TranslateBatchEchoesInputsInStub) {
    tk_context* ctx = MakeContext();
    tk_model* model = LoadStubModel(ctx);

    const char* inputs[] = {"uno", "dos <b>tres</b>"};
    tk_translation_result r[2] = {};
    EXPECT_EQ(tk_translate_batch(model, inputs, 2, /*is_html=*/1, r), TK_OK);
    EXPECT_STREQ(r[0].text, "uno");
    EXPECT_STREQ(r[1].text, "dos <b>tres</b>");
    tk_translation_result_free(&r[0]);
    tk_translation_result_free(&r[1]);

    tk_model_close(model);
    tk_shutdown(ctx);
}
#endif  // !TRANSLATEKIT_WITH_ENGINE

TEST(CApi, TranslateBatchRejectsBadArgs) {
    const char* inputs[] = {"x", nullptr};
    tk_translation_result r[2] = {};
    EXPECT_EQ(tk_translate_batch(nullptr, inputs, 1, 0, r), TK_ERR_INVALID_ARG);
    EXPECT_EQ(tk_translate_batch(nullptr, nullptr, 1, 0, r), TK_ERR_INVALID_ARG);
    EXPECT_EQ(tk_translate_batch(nullptr, inputs, 1, 0, nullptr), TK_ERR_INVALID_ARG);
    EXPECT_EQ(r[0].text, nullptr);
}

#ifdef TRANSLATEKIT_WITH_ENGINE
// Golden translation against a real tiny en->de model. Provide the extracted
// ende.student.tiny11 directory via TK_TEST_MODEL_DIR (see scripts/fetch-models.sh);
// the test is skipped when it is not set.
TEST(CApi, TranslateRealModelEnDe) {
    const char* dir = std::getenv("TK_TEST_MODEL_DIR");
    if (dir == nullptr || dir[0] == '\0') {
        GTEST_SKIP() << "set TK_TEST_MODEL_DIR to the ende.student.tiny11 directory";
    }
    const std::string d(dir);
    const std::string model = d + "/model.intgemm.alphas.bin";
    const std::string vocab = d + "/vocab.deen.spm";
    const std::string lex = d + "/lex.s2t.bin";
    const std::string config = d + "/config.intgemm8bitalpha.yml";
    const char* vocabs[] = {vocab.c_str()};

    tk_context* ctx = MakeContext();
    tk_model_spec spec = {};
    spec.source_lang = "en";
    spec.target_lang = "de";
    spec.model_path = model.c_str();
    spec.vocab_paths = vocabs;
    spec.vocab_count = 1;
    spec.shortlist_path = lex.c_str();
    spec.config_path = config.c_str();
    spec.num_workers = 1;

    tk_model* m = nullptr;
    ASSERT_EQ(tk_load_model(ctx, &spec, &m), TK_OK) << tk_last_error();

    // (a) plain text
    tk_translation_result r = {};
    ASSERT_EQ(tk_translate(m, "Hello World!", /*is_html=*/0, &r), TK_OK) << tk_last_error();
    ASSERT_NE(r.text, nullptr);
    const std::string out(r.text);
    EXPECT_NE(out.find("Hallo"), std::string::npos) << "plain output: " << out;
    tk_translation_result_free(&r);

    // (b) HTML fragments: tags/attributes round-trip (detag-and-project). These
    // mirror TranslateKitInstrumentedTest.HTML_CASES so the host and on-device
    // goldens stay in lock-step. Tag/attribute substrings are structural
    // invariants (projected via alignment, not generated); the German tokens are
    // the stable greedy (beam-size 1) outputs.
    struct HtmlCase {
        const char* input;
        std::vector<const char*> must_contain;  // checked when `exact` is null
        const char* exact;                      // exact match when non-null
    };
    const std::vector<HtmlCase> html_cases = {
        // Canonical inline tag.
        {"<b>Hello World!</b>", {}, "<b>Hallo Welt!</b>"},
        // Attribute (href) preserved verbatim; content translated.
        {"<a href=\"https://example.com\">Hello World!</a>",
         {"<a href=\"https://example.com\">", "</a>", "Hallo Welt!"}, nullptr},
        // Tag repositioned across sentence reordering.
        {"Click <a href=\"https://example.com\">here</a> to continue.",
         {"<a href=\"https://example.com\">", "</a>", "hier", "Klicken"}, nullptr},
        // Nested inline tags.
        {"<b>Hello <i>beautiful</i> World!</b>",
         {"<b>", "<i>", "</i>", "</b>", "Welt"}, nullptr},
        // Sibling inline tags.
        {"<b>Hello</b> <i>World</i>!", {"<b>Hallo</b>", "<i>Welt</i>"}, nullptr},
        // Void element preserved and repositioned.
        {"Hello<br>World", {"<br>", "Hallo", "Welt"}, nullptr},
        // Block elements translate independently; structure intact.
        {"<p>Hello World!</p><p>Good morning!</p>",
         {"<p>Hallo Welt!</p>", "<p>Guten Morgen!</p>"}, nullptr},
        // Multiple attributes preserved verbatim.
        {"<span class=\"greeting\" data-id=\"42\">Hello World!</span>",
         {"<span class=\"greeting\" data-id=\"42\">", "</span>", "Hallo Welt!"}, nullptr},
        // HTML entity survives.
        {"Hello &amp; goodbye", {"&amp;", "Hallo"}, nullptr},
        // Full combo: block + repositioned link + complex href (query + entity).
        {"<p>Please <a href=\"https://example.com/path?q=1&amp;x=2\">click here</a> now.</p>",
         {"<p>", "<a href=\"https://example.com/path?q=1&amp;x=2\">", "</a>", "</p>"}, nullptr},
    };
    for (const HtmlCase& c : html_cases) {
        tk_translation_result h = {};
        ASSERT_EQ(tk_translate(m, c.input, /*is_html=*/1, &h), TK_OK)
            << "input: " << c.input << " err: " << tk_last_error();
        ASSERT_NE(h.text, nullptr) << "input: " << c.input;
        const std::string html(h.text);
        if (c.exact != nullptr) {
            EXPECT_EQ(html, c.exact) << "input: " << c.input;
        } else {
            for (const char* needle : c.must_contain) {
                EXPECT_NE(html.find(needle), std::string::npos)
                    << "input: " << c.input << "\n  output: " << html
                    << "\n  missing: " << needle;
            }
        }
        tk_translation_result_free(&h);
    }

    tk_model_close(m);
    tk_shutdown(ctx);
}

// Loading a second model in the same process must succeed. marian's
// createLoggers() registers spdlog loggers ("general", "valid") on every model
// load, and spdlog throws when a name is already registered; without the
// idempotent-logger engine patch the second load aborts with
// "logger with name 'general' already exists". The app loads several models per
// process (e.g. en->es then en->de) and pivoting needs two loaded at once.
// One ill-formed byte used to make the engine return an EMPTY translation with
// TK_OK, silently dropping the whole input.
TEST(CApi, TranslateIllFormedUtf8IsNotDropped) {
    const char* dir = std::getenv("TK_TEST_MODEL_DIR");
    if (dir == nullptr || dir[0] == '\0') {
        GTEST_SKIP() << "set TK_TEST_MODEL_DIR to the ende.student.tiny11 directory";
    }
    const std::string d(dir);
    const std::string model = d + "/model.intgemm.alphas.bin";
    const std::string vocab = d + "/vocab.deen.spm";
    const std::string lex = d + "/lex.s2t.bin";
    const std::string config = d + "/config.intgemm8bitalpha.yml";
    const char* vocabs[] = {vocab.c_str()};

    tk_context* ctx = MakeContext();
    tk_model_spec spec = {};
    spec.source_lang = "en";
    spec.target_lang = "de";
    spec.model_path = model.c_str();
    spec.vocab_paths = vocabs;
    spec.vocab_count = 1;
    spec.shortlist_path = lex.c_str();
    spec.config_path = config.c_str();
    spec.num_workers = 1;
    tk_model* m = nullptr;
    ASSERT_EQ(tk_load_model(ctx, &spec, &m), TK_OK) << tk_last_error();

    const char* inputs[] = {"Hello \xF6 world.", "Hello \xC0\x80" " world."};
    for (const char* in : inputs) {
        tk_translation_result r = {};
        ASSERT_EQ(tk_translate(m, in, /*is_html=*/0, &r), TK_OK) << tk_last_error();
        ASSERT_NE(r.text, nullptr);
        EXPECT_NE(std::string(r.text).find("Welt"), std::string::npos) << r.text;
        tk_translation_result_free(&r);
    }
    tk_model_close(m);
    tk_shutdown(ctx);
}

TEST(CApi, LoadMultipleModelsInOneProcess) {
    const char* dir = std::getenv("TK_TEST_MODEL_DIR");
    if (dir == nullptr || dir[0] == '\0') {
        GTEST_SKIP() << "set TK_TEST_MODEL_DIR to the ende.student.tiny11 directory";
    }
    const std::string d(dir);
    const std::string model = d + "/model.intgemm.alphas.bin";
    const std::string vocab = d + "/vocab.deen.spm";
    const std::string lex = d + "/lex.s2t.bin";
    const std::string config = d + "/config.intgemm8bitalpha.yml";
    const char* vocabs[] = {vocab.c_str()};

    auto make_spec = [&]() {
        tk_model_spec spec = {};
        spec.source_lang = "en";
        spec.target_lang = "de";
        spec.model_path = model.c_str();
        spec.vocab_paths = vocabs;
        spec.vocab_count = 1;
        spec.shortlist_path = lex.c_str();
        spec.config_path = config.c_str();
        spec.num_workers = 1;
        return spec;
    };

    tk_context* ctx = MakeContext();

    tk_model_spec spec1 = make_spec();
    tk_model* m1 = nullptr;
    ASSERT_EQ(tk_load_model(ctx, &spec1, &m1), TK_OK) << tk_last_error();

    // The second load is the regression: it used to abort inside spdlog.
    tk_model_spec spec2 = make_spec();
    tk_model* m2 = nullptr;
    ASSERT_EQ(tk_load_model(ctx, &spec2, &m2), TK_OK) << tk_last_error();

    // Both handles still translate.
    for (tk_model* m : {m1, m2}) {
        tk_translation_result r = {};
        ASSERT_EQ(tk_translate(m, "Hello World!", /*is_html=*/0, &r), TK_OK) << tk_last_error();
        ASSERT_NE(r.text, nullptr);
        EXPECT_NE(std::string(r.text).find("Hallo"), std::string::npos);
        tk_translation_result_free(&r);
    }

    tk_model_close(m1);
    tk_model_close(m2);
    tk_shutdown(ctx);
}

// A batch on a multi-worker model must give, in order, exactly what translating
// each input on its own gives (plain text and HTML).
TEST(CApi, TranslateBatchMatchesSingleTranslations) {
    const char* dir = std::getenv("TK_TEST_MODEL_DIR");
    if (dir == nullptr || dir[0] == '\0') {
        GTEST_SKIP() << "set TK_TEST_MODEL_DIR to the ende.student.tiny11 directory";
    }
    const std::string d(dir);
    const std::string model = d + "/model.intgemm.alphas.bin";
    const std::string vocab = d + "/vocab.deen.spm";
    const std::string lex = d + "/lex.s2t.bin";
    const std::string config = d + "/config.intgemm8bitalpha.yml";
    const char* vocabs[] = {vocab.c_str()};

    tk_context* ctx = MakeContext();
    tk_model_spec spec = {};
    spec.source_lang = "en";
    spec.target_lang = "de";
    spec.model_path = model.c_str();
    spec.vocab_paths = vocabs;
    spec.vocab_count = 1;
    spec.shortlist_path = lex.c_str();
    spec.config_path = config.c_str();
    spec.num_workers = 4;
    tk_model* m = nullptr;
    ASSERT_EQ(tk_load_model(ctx, &spec, &m), TK_OK) << tk_last_error();

    for (int is_html : {0, 1}) {
        const char* inputs[] = {
            "Hello World!",
            "The weather is nice today. We are going to the park.",
            "<p>Your <b>search history</b> stays on your device.</p>",
            "Privacy should be the default on the web.",
            "One more sentence, so that the batch is split across workers.",
            "Good morning.",
        };
        constexpr size_t n = sizeof(inputs) / sizeof(inputs[0]);
        tk_translation_result batch[n] = {};
        ASSERT_EQ(tk_translate_batch(m, inputs, n, is_html, batch), TK_OK) << tk_last_error();
        for (size_t i = 0; i < n; ++i) {
            tk_translation_result single = {};
            ASSERT_EQ(tk_translate(m, inputs[i], is_html, &single), TK_OK) << tk_last_error();
            ASSERT_NE(batch[i].text, nullptr);
            EXPECT_STREQ(batch[i].text, single.text) << "input " << i << " html=" << is_html;
            tk_translation_result_free(&single);
            tk_translation_result_free(&batch[i]);
        }
    }

    tk_translation_result none = {};
    EXPECT_EQ(tk_translate_batch(m, nullptr, 0, 0, &none), TK_OK);  // empty batch is fine

    tk_model_close(m);
    tk_shutdown(ctx);
}
#endif  // TRANSLATEKIT_WITH_ENGINE

TEST(CApi, LoadModelRejectsMissingModelPath) {
    tk_context* ctx = MakeContext();
    tk_model_spec spec = {};
    spec.source_lang = "es";
    spec.target_lang = "en";
    spec.model_path = nullptr;
    tk_model* model = nullptr;
    EXPECT_EQ(tk_load_model(ctx, &spec, &model), TK_ERR_INVALID_ARG);
    EXPECT_EQ(model, nullptr);
    tk_shutdown(ctx);
}

TEST(CApi, FreeResultIsIdempotentAndNullSafe) {
    tk_translation_result r = {};
    tk_translation_result_free(&r);  // zero-initialized
    tk_translation_result_free(nullptr);
    tk_model_close(nullptr);
    tk_shutdown(nullptr);
}

}  // namespace
