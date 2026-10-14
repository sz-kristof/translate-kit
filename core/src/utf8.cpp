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

#include "utf8.h"

namespace translatekit {

namespace {

constexpr char kReplacement[] = "\xEF\xBF\xBD";  // U+FFFD

bool IsContinuation(unsigned char c) { return (c & 0xC0) == 0x80; }

}  // namespace

std::string SanitizeUtf8(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    const auto* s = reinterpret_cast<const unsigned char*>(in.data());
    const size_t n = in.size();
    size_t i = 0;
    while (i < n) {
        const unsigned char c = s[i];
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
            ++i;
            continue;
        }
        // Sequence length and the valid range of the SECOND byte (Unicode
        // Table 3-7). Narrowing the second byte rejects overlongs (E0/F0),
        // surrogates (ED) and code points above U+10FFFF (F4) up front.
        size_t len = 0;
        unsigned char lo = 0x80, hi = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) {
            len = 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            len = 3;
            if (c == 0xE0) lo = 0xA0;
            if (c == 0xED) hi = 0x9F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4;
            if (c == 0xF0) lo = 0x90;
            if (c == 0xF4) hi = 0x8F;
        } else {
            // C0, C1, F5..FF, or a stray continuation byte.
            out += kReplacement;
            ++i;
            continue;
        }
        // Consume the longest valid prefix (the "maximal subpart"); on the
        // first bad or missing byte, emit one U+FFFD and resume at that byte.
        size_t j = 1;
        while (j < len && i + j < n) {
            const unsigned char b = s[i + j];
            const bool ok = (j == 1) ? (b >= lo && b <= hi) : IsContinuation(b);
            if (!ok) break;
            ++j;
        }
        if (j == len) {
            out.append(in.data() + i, len);
        } else {
            out += kReplacement;
        }
        i += j;
    }
    return out;
}

}  // namespace translatekit
