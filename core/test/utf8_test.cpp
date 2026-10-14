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

#include <gtest/gtest.h>

#include <string>

#include "utf8.h"

using translatekit::SanitizeUtf8;

namespace {
const std::string R = "\xEF\xBF\xBD";  // U+FFFD
}

TEST(Utf8, WellFormedIsUnchanged) {
    const std::string s =
        "ascii, "
        "\xC3\xA9 "              // é (2 bytes)
        "\xC5\x91 "              // ő (2 bytes)
        "\xE2\x82\xAC "          // € (3 bytes)
        "\xED\x9F\xBF "          // U+D7FF, last code point before surrogates
        "\xEE\x80\x80 "          // U+E000, first after surrogates
        "\xF0\x9F\x98\x80 "      // 😀 (4 bytes)
        "\xF4\x8F\xBF\xBF";      // U+10FFFF, the maximum
    EXPECT_EQ(SanitizeUtf8(s), s);
    EXPECT_EQ(SanitizeUtf8(""), "");
}

TEST(Utf8, InvalidLeadBytesBecomeReplacement) {
    EXPECT_EQ(SanitizeUtf8("a\xF6z"), "a" + R + "z");   // Latin-2/1 'ö'
    EXPECT_EQ(SanitizeUtf8("\xC0\x80"), R + R);         // Java "modified UTF-8" NUL
    EXPECT_EQ(SanitizeUtf8("\xC1\xBF"), R + R);         // overlong
    EXPECT_EQ(SanitizeUtf8("\xF5\x80\x80\x80"), R + R + R + R);
    EXPECT_EQ(SanitizeUtf8("\xFF"), R);
    EXPECT_EQ(SanitizeUtf8("\x80"), R);                 // stray continuation
}

TEST(Utf8, MaximalSubpartSubstitution) {
    // Unicode ch. 3 Table 3-8 example: one U+FFFD per maximal subpart.
    EXPECT_EQ(SanitizeUtf8("\x61\xF1\x80\x80\xE1\x80\xC2\x62\x80\x63\x80\xBF\x64"),
              "a" + R + R + R + "b" + R + "c" + R + R + "d");
}

TEST(Utf8, TruncatedSequenceAtEnd) {
    EXPECT_EQ(SanitizeUtf8("ab\xC3"), "ab" + R);
    EXPECT_EQ(SanitizeUtf8("ab\xE2\x82"), "ab" + R);
    EXPECT_EQ(SanitizeUtf8("ab\xF0\x9F\x98"), "ab" + R);
}

TEST(Utf8, SurrogatesAndOverlongsRejected) {
    EXPECT_EQ(SanitizeUtf8("\xED\xA0\x80"), R + R + R);       // U+D800 (lone surrogate)
    EXPECT_EQ(SanitizeUtf8("\xED\xA0\xBD\xED\xB8\x80"),       // CESU-8 😀 (Java JNI)
              R + R + R + R + R + R);
    EXPECT_EQ(SanitizeUtf8("\xE0\x80\xAF"), R + R + R);       // overlong '/'
    EXPECT_EQ(SanitizeUtf8("\xF0\x80\x80\xAF"), R + R + R + R);
    EXPECT_EQ(SanitizeUtf8("\xF4\x90\x80\x80"), R + R + R + R);  // > U+10FFFF
}
