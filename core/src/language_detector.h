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

#ifndef TRANSLATE_KIT_LANGUAGE_DETECTOR_H
#define TRANSLATE_KIT_LANGUAGE_DETECTOR_H

#include <string>

namespace translatekit {

struct DetectedLanguage {
    std::string language;  // ISO-639-1 / BCP-47-ish, e.g. "en"; "und" if unknown
    float confidence = 0.0f;  // [0, 1]
};

// Offline language identification backed by the bundled CLD2 detector
// (Apache-2.0). CLD2 is self-contained: its language profiles are compiled into
// the library, so there is no model file to load and detection is stateless.
class LanguageDetector {
public:
    LanguageDetector() = default;

    // `text` is UTF-8; ill-formed sequences are treated as U+FFFD.
    DetectedLanguage detect(const std::string& text) const;
};

}  // namespace translatekit

#endif  // TRANSLATE_KIT_LANGUAGE_DETECTOR_H
