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

#ifndef TRANSLATE_KIT_UTF8_H
#define TRANSLATE_KIT_UTF8_H

#include <string>
#include <string_view>

namespace translatekit {

// Returns `in` as well-formed UTF-8: every ill-formed sequence (stray
// continuation bytes, overlongs, surrogates, > U+10FFFF, truncated sequences)
// is replaced by U+FFFD, one per maximal subpart, as the Unicode Standard
// (ch. 3, "U+FFFD Substitution of Maximal Subparts") and the WHATWG Encoding
// spec recommend. Well-formed input is returned unchanged.
//
// The C ABI takes caller-supplied bytes, but CLD2's ExtDetectLanguageSummary
// requires interchange-valid UTF-8 and reads out of bounds on anything else
// (see compact_lang_det.h: "never trust raw user-supplied bytes").
std::string SanitizeUtf8(std::string_view in);

}  // namespace translatekit

#endif  // TRANSLATE_KIT_UTF8_H
