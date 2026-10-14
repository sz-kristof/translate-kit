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

#include "language_detector.h"

#include "compact_lang_det.h"  // CLD2::ExtDetectLanguageSummary, CLD2::Language
#include "lang_script.h"       // CLD2::LanguageCode
#include "utf8.h"

namespace translatekit {

DetectedLanguage LanguageDetector::detect(const std::string& raw) const {
    if (raw.empty()) return DetectedLanguage{"und", 0.0f};

    // CLD2's ExtDetectLanguageSummary trusts its input to be valid UTF-8 and
    // reads past its lookup tables otherwise, so never hand it raw bytes.
    const std::string text = SanitizeUtf8(raw);

    bool is_reliable = false;
    CLD2::Language top3[3] = {CLD2::UNKNOWN_LANGUAGE, CLD2::UNKNOWN_LANGUAGE,
                              CLD2::UNKNOWN_LANGUAGE};
    int percent3[3] = {0, 0, 0};
    int text_bytes = 0;

    const CLD2::Language language = CLD2::ExtDetectLanguageSummary(
        text.data(), static_cast<int>(text.size()),
        /*is_plain_text=*/true,
        top3, percent3, &text_bytes, &is_reliable);

    if (language == CLD2::UNKNOWN_LANGUAGE) {
        return DetectedLanguage{"und", 0.0f};
    }

    const char* code = CLD2::LanguageCode(language);

    // CLD2 reports the share of the text attributed to the top language plus a
    // boolean reliability flag (rather than a probability). Map that to a [0,1]
    // confidence: the byte share, halved when CLD2 deems the call unreliable.
    float confidence = static_cast<float>(percent3[0]) / 100.0f;
    if (!is_reliable) confidence *= 0.5f;
    if (confidence < 0.0f) confidence = 0.0f;
    if (confidence > 1.0f) confidence = 1.0f;

    return DetectedLanguage{code != nullptr ? code : "und", confidence};
}

}  // namespace translatekit
