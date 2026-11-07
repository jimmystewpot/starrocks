// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "base/string/string_searcher.h"

#include <gtest/gtest.h>
#include <sys/mman.h>
#include <unistd.h>

#include <chrono>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "base/string/tight_buffer.h"
#include "base/string/volnitsky.h"

namespace starrocks {

class StringSearcherTest : public ::testing::Test {};

// Runs every compiled kernel; all results must be identical.
static std::vector<const uint8_t*> run_all_kernels(const StringSearcher& s, const uint8_t* b, const uint8_t* e,
                                                   size_t needle_len) {
    std::vector<const uint8_t*> r{s.search(b, e), s.search_scalar(b, e)};
    if (needle_len >= 2) {
#if defined(__x86_64__)
        if (__builtin_cpu_supports("sse4.1")) r.push_back(s.search_sse41(b, e));
        if (__builtin_cpu_supports("avx2")) r.push_back(s.search_avx2(b, e));
#elif defined(__ARM_NEON)
        r.push_back(s.search_neon(b, e));
#endif
    }
    return r;
}

TEST_F(StringSearcherTest, FindAndNotFound) {
    const std::string haystack = "abchellodef";
    const uint8_t* begin = reinterpret_cast<const uint8_t*>(haystack.data());
    const uint8_t* end = begin + haystack.size();

    const char needle[] = "hello";
    StringSearcher searcher(needle, 5);
    EXPECT_EQ(begin + 3, searcher.search(begin, end));

    const char missing_needle[] = "xyz";
    StringSearcher missing(missing_needle, 3);
    EXPECT_EQ(end, missing.search(begin, end));
}

TEST_F(StringSearcherTest, EmptyNeedleReturnsHaystackBegin) {
    const std::string haystack = "abcdef";
    const uint8_t* begin = reinterpret_cast<const uint8_t*>(haystack.data());
    const uint8_t* end = begin + haystack.size();

    const char needle[] = "";
    StringSearcher searcher(needle, 0);
    EXPECT_EQ(begin, searcher.search(begin, end));
}

TEST_F(StringSearcherTest, EmbeddedNullNeedle) {
    const uint8_t haystack_raw[] = {'a', 'b', 'c', '\0', 'd', 'e'};
    const uint8_t needle_raw[] = {'c', '\0', 'd'};
    const uint8_t* begin = haystack_raw;
    const uint8_t* end = begin + sizeof(haystack_raw);

    StringSearcher searcher(needle_raw, sizeof(needle_raw));
    EXPECT_EQ(begin + 2, searcher.search(begin, end));
}

TEST_F(StringSearcherTest, LibcSearcherFindAndMiss) {
    const char haystack[] = "aaaaab";
    LibcASCIICaseSensitiveStringSearcher searcher("aab", 3);
    EXPECT_EQ(haystack + 3, searcher.search(haystack, 6));

    LibcASCIICaseSensitiveStringSearcher missing("AAB", 3);
    EXPECT_EQ(nullptr, missing.search(haystack, 6));
}

TEST_F(StringSearcherTest, CompareRespectsHaystackBounds) {
    const char buf[] = "Qxyzhello";
    const char* haystack = buf + 1;
    const char* short_end = buf + 6; // view is "xyzhe"
    StringSearcher searcher("hello", 5);

    EXPECT_FALSE(searcher.compare(haystack, short_end, buf + 4)); // only "he" remains
    EXPECT_FALSE(searcher.compare(haystack, short_end + 3, buf)); // pos before haystack
    EXPECT_TRUE(searcher.compare(haystack, buf + 9, buf + 4));    // full view contains "hello"
}

TEST_F(StringSearcherTest, FirstEqualsLastSaturation) {
    for (size_t len : {size_t(3), size_t(16), size_t(17), size_t(32), size_t(33)}) {
        const std::string needle = "a" + std::string(len - 2, 'X') + "a";
        const std::string hit = std::string(300, 'a') + needle;
        TightBuffer hay(hit);
        TightBuffer ndl(needle);
        StringSearcher searcher(ndl.begin(), ndl.size());
        for (const auto* r : run_all_kernels(searcher, hay.begin(), hay.end(), len)) {
            EXPECT_EQ(hay.begin() + 300, r) << "len=" << len;
        }
        const std::string miss(300, 'a');
        TightBuffer miss_hay(miss);
        for (const auto* r : run_all_kernels(searcher, miss_hay.begin(), miss_hay.end(), len)) {
            EXPECT_EQ(miss_hay.end(), r) << "len=" << len;
        }
    }

    // Space-filled haystack with 32B needle (" " + 30*'X' + " ") and single-interior-diff needle
    for (const std::string& space_needle :
         {std::string(" ") + std::string(30, 'X') + " ", std::string(15, ' ') + "X" + std::string(16, ' ')}) {
        const std::string space_miss(4096, ' ');
        TightBuffer miss_buf(space_miss);
        TightBuffer ndl_buf(space_needle);
        StringSearcher searcher(ndl_buf.begin(), ndl_buf.size());
        for (const auto* r : run_all_kernels(searcher, miss_buf.begin(), miss_buf.end(), space_needle.size())) {
            EXPECT_EQ(miss_buf.end(), r);
        }
        const std::string space_hit = space_miss + space_needle;
        TightBuffer hit_buf(space_hit);
        for (const auto* r : run_all_kernels(searcher, hit_buf.begin(), hit_buf.end(), space_needle.size())) {
            EXPECT_EQ(hit_buf.begin() + 4096, r);
        }
    }
}

TEST_F(StringSearcherTest, EmptyNullAndEmbeddedNul) {
    StringSearcher two("ab", 2);
    TightBuffer empty_hay("");
    EXPECT_EQ(empty_hay.end(), two.search(empty_hay.begin(), empty_hay.end()));
    EXPECT_EQ(nullptr, two.search(static_cast<const uint8_t*>(nullptr), static_cast<const uint8_t*>(nullptr)));
    StringSearcher empty("", 0);
    TightBuffer h("xyz");
    EXPECT_EQ(h.begin(), empty.search(h.begin(), h.end()));

    const std::string nul_needle("a\0b", 3);
    const std::string nul_hay("xx a\0b yy", 9);
    TightBuffer nh(nul_hay);
    TightBuffer nn(nul_needle);
    StringSearcher nul_searcher(nn.begin(), nn.size());
    for (const auto* r : run_all_kernels(nul_searcher, nh.begin(), nh.end(), 3)) {
        EXPECT_EQ(nh.begin() + 3, r);
    }
    const std::string zero_byte("\0", 1);
    TightBuffer zb(zero_byte);
    StringSearcher zero_searcher(zb.begin(), zb.size());
    EXPECT_EQ(nh.begin() + 4, zero_searcher.search(nh.begin(), nh.end()));
}

TEST_F(StringSearcherTest, NearMatchAmplification) {
    const std::string needle = "F" + std::string(30, 'M') + "L";
    const std::string decoy = "F" + std::string(14, 'M') + "X" + std::string(15, 'M') + "L"; // 32 B, differs mid-way
    std::string hay;
    for (int i = 0; i < 2000; ++i) hay += decoy;
    hay += needle;
    TightBuffer h(hay);
    TightBuffer n(needle);
    StringSearcher searcher(n.begin(), n.size());
    for (const auto* r : run_all_kernels(searcher, h.begin(), h.end(), needle.size())) {
        EXPECT_EQ(h.begin() + 2000 * 32, r);
    }
}

TEST_F(StringSearcherTest, RandomisedDifferentialFuzz) {
    constexpr uint32_t kSeed = 0x5EED;
    std::mt19937 rng(kSeed);
    for (int iter = 0; iter < 100000; ++iter) {
        const int alphabet = 2 + static_cast<int>(rng() % 2);
        auto gen = [&](size_t n) {
            std::string s(n, 'a');
            for (auto& c : s) c = static_cast<char>('a' + rng() % alphabet);
            return s;
        };
        const size_t needle_len = 1 + rng() % 40;
        const std::string needle = gen(needle_len);
        const std::string hay = gen(rng() % 301);
        TightBuffer h(hay);
        TightBuffer n(needle);
        StringSearcher searcher(n.begin(), n.size());
        const size_t pos = std::string_view(hay).find(needle);
        const uint8_t* expected = pos == std::string_view::npos ? h.end() : h.begin() + pos;
        for (const auto* r : run_all_kernels(searcher, h.begin(), h.end(), needle_len)) {
            ASSERT_EQ(expected, r) << "seed=" << kSeed << " iter=" << iter << " needle=" << needle << " hay=" << hay;
        }
    }
}

TEST_F(StringSearcherTest, OverlappingDecoysAndMaskSaturation) {
    // 1. Multiple "g....e" decoys within the same 16B/32B SIMD window before the real "google"
    TightBuffer hay("gxxxxeg1234egooXlegoglgoogle_tail");
    const auto* begin = hay.begin();
    const auto* end = hay.end();

    StringSearcher searcher("google", 6);
    for (const auto* r : run_all_kernels(searcher, begin, end, 6)) {
        EXPECT_EQ(begin + 22, r);
    }

    // 2. All-'a' prefix followed by "baaaa"
    const std::string sat_hay = std::string(150, 'a') + "baaaa";
    TightBuffer sat_buf(sat_hay);
    const auto* sat_begin = sat_buf.begin();
    const auto* sat_end = sat_buf.end();

    StringSearcher sat_miss("aaaaac", 6);
    EXPECT_EQ(sat_end, sat_miss.search(sat_begin, sat_end));
    EXPECT_EQ(sat_end, sat_miss.search_scalar(sat_begin, sat_end));

    StringSearcher sat_hit("aaaaab", 6);
    EXPECT_EQ(sat_begin + 145, sat_hit.search(sat_begin, sat_end));
    EXPECT_EQ(sat_begin + 145, sat_hit.search_scalar(sat_begin, sat_end));

    StringSearcher sat_same("aaaaaa", 6);
    EXPECT_EQ(sat_begin, sat_same.search(sat_begin, sat_end));
    EXPECT_EQ(sat_begin, sat_same.search_scalar(sat_begin, sat_end));
}

TEST_F(StringSearcherTest, AllLengthsOffsetsAndKernelEquivalence) {
    for (size_t needle_len : {size_t(1), size_t(2), size_t(3), size_t(6), size_t(15), size_t(16), size_t(17),
                              size_t(31), size_t(32), size_t(33)}) {
        std::string needle(needle_len, 'a');
        needle.front() = 'F';
        needle.back() = 'L';
        if (needle_len > 2) {
            needle[needle_len / 2] = 'M';
        }
        TightBuffer ndl(needle);
        StringSearcher searcher(ndl.begin(), ndl.size());

        for (size_t hay_len : {size_t(0), needle_len - 1, needle_len, needle_len + 1, size_t(48), size_t(96)}) {
            std::string hay(hay_len, 'x');
            TightBuffer miss_buf(hay);
            EXPECT_EQ(miss_buf.end(), searcher.search(miss_buf.begin(), miss_buf.end()));
            EXPECT_EQ(miss_buf.end(), searcher.search_scalar(miss_buf.begin(), miss_buf.end()));

            if (hay_len >= needle_len) {
                for (size_t pos = 0; pos <= hay_len - needle_len; ++pos) {
                    std::string h = hay;
                    h.replace(pos, needle_len, needle);
                    TightBuffer hit_buf(h);
                    const auto* expected = hit_buf.begin() + pos;
                    for (const auto* r : run_all_kernels(searcher, hit_buf.begin(), hit_buf.end(), needle_len)) {
                        EXPECT_EQ(expected, r)
                                << "needle_len=" << needle_len << " hay_len=" << hay_len << " pos=" << pos;
                    }
                }
            }
        }
    }
}

TEST_F(StringSearcherTest, LongHomogeneousNeedleCapsAnchorSelection) {
    const size_t page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    const size_t num_pages = 32;
    const size_t map_bytes = num_pages * page_size;
    const size_t needle_size = 100000;
    ASSERT_LT(needle_size, map_bytes);

    void* mapped = mmap(nullptr, map_bytes, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(MAP_FAILED, mapped);

    const auto* needle_ptr = static_cast<const char*>(mapped);
    VolnitskyUTF8 volnitsky(needle_ptr, needle_size, 278528);

    std::vector<unsigned char> residency(num_pages, 0);
    ASSERT_EQ(0, mincore(mapped, map_bytes, residency.data()));
    // Anchor selection must stay within the first page and never fault in middle pages (e.g. page 8 at 32 KiB).
    EXPECT_EQ(0, residency[8] & 1) << "StringSearcher constructor scanned past bounded interior anchor window";
    ASSERT_EQ(0, munmap(mapped, map_bytes));

    const std::string long_needle(needle_size, 'a');
    StringSearcher searcher(long_needle.data(), long_needle.size());

    const std::string miss_haystack(278528, 'b');
    const auto* miss_begin = reinterpret_cast<const uint8_t*>(miss_haystack.data());
    const auto* miss_end = miss_begin + miss_haystack.size();
    for (const auto* r : run_all_kernels(searcher, miss_begin, miss_end, long_needle.size())) {
        EXPECT_EQ(miss_end, r);
    }

    std::string hit_haystack(100100, 'b');
    hit_haystack.replace(37, long_needle.size(), long_needle);
    const auto* hit_begin = reinterpret_cast<const uint8_t*>(hit_haystack.data());
    const auto* hit_end = hit_begin + hit_haystack.size();
    for (const auto* r : run_all_kernels(searcher, hit_begin, hit_end, long_needle.size())) {
        EXPECT_EQ(hit_begin + 37, r);
    }
}

static std::vector<const uint8_t*> run_all_rkernels(const StringSearcher& s, const uint8_t* b, const uint8_t* e,
                                                    size_t needle_len) {
    std::vector<const uint8_t*> r{s.rsearch(b, e), s.rsearch_scalar(b, e)};
    if (needle_len >= 2) {
#if defined(__x86_64__)
        if (__builtin_cpu_supports("sse4.1")) r.push_back(s.rsearch_sse41(b, e));
        if (__builtin_cpu_supports("avx2")) r.push_back(s.rsearch_avx2(b, e));
#elif defined(__ARM_NEON)
        r.push_back(s.rsearch_neon(b, e));
#endif
    }
    return r;
}

TEST_F(StringSearcherTest, RSearchFindLastAndNotFound) {
    const std::string haystack = "abchellodefhelloghi";
    TightBuffer hay(haystack);
    StringSearcher searcher("hello", 5);
    for (const auto* r : run_all_rkernels(searcher, hay.begin(), hay.end(), 5)) {
        EXPECT_EQ(hay.begin() + 11, r);
    }
    EXPECT_EQ(reinterpret_cast<const char*>(hay.begin() + 11),
              searcher.rsearch(reinterpret_cast<const char*>(hay.begin()), hay.size()));

    StringSearcher missing("xyz", 3);
    for (const auto* r : run_all_rkernels(missing, hay.begin(), hay.end(), 3)) {
        EXPECT_EQ(hay.end(), r);
    }
}

TEST_F(StringSearcherTest, RSearchMultipleHitsSameWindow) {
    // Multiple hits within a single 16B and 32B window; rsearch must return the rightmost match.
    const std::string haystack = "xy_ab_ab_ab_ab_ab_ab_ab_ab_ab_zw";
    TightBuffer hay(haystack);
    StringSearcher s2("ab", 2);
    for (const auto* r : run_all_rkernels(s2, hay.begin(), hay.end(), 2)) {
        EXPECT_EQ(hay.begin() + 27, r);
    }
    StringSearcher s1("a", 1);
    for (const auto* r : run_all_rkernels(s1, hay.begin(), hay.end(), 1)) {
        EXPECT_EQ(hay.begin() + 27, r);
    }
}

TEST_F(StringSearcherTest, RSearchOverlappingNeedlesAndHeadWindow) {
    // "abc###123###234" with "##": last hit at 10, then in [0, 10) ("abc###123#") last hit at 4,
    // then in [0, 4) ("abc#") miss.
    TightBuffer hay("abc###123###234");
    StringSearcher s("##", 2);
    for (const auto* r1 : run_all_rkernels(s, hay.begin(), hay.end(), 2)) {
        ASSERT_EQ(hay.begin() + 10, r1);
    }
    for (const auto* r2 : run_all_rkernels(s, hay.begin(), hay.begin() + 10, 2)) {
        ASSERT_EQ(hay.begin() + 4, r2);
    }
    for (const auto* r3 : run_all_rkernels(s, hay.begin(), hay.begin() + 4, 2)) {
        ASSERT_EQ(hay.begin() + 4, r3);
    }

    // Exercise the overlapping head window (1 <= p - haystack < 16) with a false decoy at index 16
    // that was already excluded by shrinking cur_end, or a match only in the head prefix [0, rem).
    for (size_t rem = 1; rem < 16; ++rem) {
        std::string buf(16 + rem + 3, 'x');
        buf.replace(rem - 1, 4, "ABCD");
        TightBuffer tb(buf);
        StringSearcher s4("ABCD", 4);
        for (const auto* r : run_all_rkernels(s4, tb.begin(), tb.end(), 4)) {
            EXPECT_EQ(tb.begin() + (rem - 1), r) << "rem=" << rem;
        }
    }
}

TEST_F(StringSearcherTest, RSearchEmptyNullAndEmbeddedNul) {
    StringSearcher two("ab", 2);
    TightBuffer empty_hay("");
    EXPECT_EQ(empty_hay.end(), two.rsearch(empty_hay.begin(), empty_hay.end()));
    EXPECT_EQ(nullptr, two.rsearch(static_cast<const uint8_t*>(nullptr), static_cast<const uint8_t*>(nullptr)));
    StringSearcher empty("", 0);
    TightBuffer h("xyz");
    EXPECT_EQ(h.end(), empty.rsearch(h.begin(), h.end()));

    const std::string nul_needle("a\0b", 3);
    const std::string nul_hay("xx a\0b yy a\0b zz", 16);
    TightBuffer nh(nul_hay);
    TightBuffer nn(nul_needle);
    StringSearcher nul_searcher(nn.begin(), nn.size());
    for (const auto* r : run_all_rkernels(nul_searcher, nh.begin(), nh.end(), 3)) {
        EXPECT_EQ(nh.begin() + 10, r);
    }
    const std::string zero_byte("\0", 1);
    TightBuffer zb(zero_byte);
    StringSearcher zero_searcher(zb.begin(), zb.size());
    EXPECT_EQ(nh.begin() + 11, zero_searcher.rsearch(nh.begin(), nh.end()));
}

TEST_F(StringSearcherTest, RSearchAllLengthsOffsetsAndKernelEquivalence) {
    for (size_t needle_len : {size_t(1), size_t(2), size_t(3), size_t(6), size_t(15), size_t(16), size_t(17),
                              size_t(31), size_t(32), size_t(33)}) {
        std::string needle(needle_len, 'a');
        needle.front() = 'F';
        needle.back() = 'L';
        if (needle_len > 2) {
            needle[needle_len / 2] = 'M';
        }
        TightBuffer ndl(needle);
        StringSearcher searcher(ndl.begin(), ndl.size());

        for (size_t hay_len : {size_t(0), needle_len - 1, needle_len, needle_len + 1, size_t(48), size_t(96)}) {
            std::string hay(hay_len, 'x');
            TightBuffer miss_buf(hay);
            for (const auto* r : run_all_rkernels(searcher, miss_buf.begin(), miss_buf.end(), needle_len)) {
                EXPECT_EQ(miss_buf.end(), r);
            }

            if (hay_len >= needle_len) {
                for (size_t pos = 0; pos <= hay_len - needle_len; ++pos) {
                    std::string h = hay;
                    h.replace(pos, needle_len, needle);
                    TightBuffer hit_buf(h);
                    const auto* expected = hit_buf.begin() + pos;
                    for (const auto* r : run_all_rkernels(searcher, hit_buf.begin(), hit_buf.end(), needle_len)) {
                        EXPECT_EQ(expected, r)
                                << "needle_len=" << needle_len << " hay_len=" << hay_len << " pos=" << pos;
                    }
                }
            }
        }
    }
}

TEST_F(StringSearcherTest, RSearchRandomisedDifferentialFuzz) {
    constexpr uint32_t kSeed = 0x5EED1234;
    std::mt19937 rng(kSeed);
    for (int iter = 0; iter < 100000; ++iter) {
        const int alphabet = 2 + static_cast<int>(rng() % 2);
        auto gen = [&](size_t n) {
            std::string s(n, 'a');
            for (auto& c : s) c = static_cast<char>('a' + rng() % alphabet);
            return s;
        };
        const size_t needle_len = 1 + rng() % 40;
        const std::string needle = gen(needle_len);
        const std::string hay = gen(rng() % 301);
        TightBuffer h(hay);
        TightBuffer n(needle);
        StringSearcher searcher(n.begin(), n.size());
        const size_t pos = std::string_view(hay).rfind(needle);
        const uint8_t* expected = pos == std::string_view::npos ? h.end() : h.begin() + pos;
        for (const auto* r : run_all_rkernels(searcher, h.begin(), h.end(), needle_len)) {
            ASSERT_EQ(expected, r) << "seed=" << kSeed << " iter=" << iter << " needle=" << needle << " hay=" << hay;
        }
    }
}

TEST_F(StringSearcherTest, LongRepetitiveNeedleForwardAndReverseBoundedTime) {
    // Pathological cases where a long repetitive needle shares first/last/prefix/suffix bytes with an all-'a' haystack:
    // 1. 16 KiB needle of 'a' with 'b' at needle_size - 2 (tail anchor outside the first 64B window).
    // 2. 16 KiB needle of 'a' with 'b' at needle_size / 2 (outside both 64B head and tail anchor windows).
    // 3. 8 KiB periodic needle of all 'a' against a haystack of ("aaa...ab") blocks.
    constexpr size_t kNeedleLen = 16 * 1024;
    constexpr size_t kHayLen = 256 * 1024;

    std::string needle_tail_b(kNeedleLen, 'a');
    needle_tail_b[kNeedleLen - 2] = 'b';

    std::string needle_mid_b(kNeedleLen, 'a');
    needle_mid_b[kNeedleLen / 2] = 'b';

    const std::string all_a_haystack(kHayLen, 'a');
    const auto* miss_b = reinterpret_cast<const uint8_t*>(all_a_haystack.data());
    const auto* miss_e = miss_b + all_a_haystack.size();

    const auto start_tp = std::chrono::steady_clock::now();

    for (const std::string& needle : {needle_tail_b, needle_mid_b}) {
        StringSearcher searcher(needle.data(), needle.size());
        for (const auto* r : run_all_kernels(searcher, miss_b, miss_e, needle.size())) {
            EXPECT_EQ(miss_e, r);
        }
        for (const auto* r : run_all_rkernels(searcher, miss_b, miss_e, needle.size())) {
            EXPECT_EQ(miss_e, r);
        }

        std::string hit_haystack = all_a_haystack;
        const size_t hit1 = 12345;
        const size_t hit2 = kHayLen - kNeedleLen - 777;
        hit_haystack.replace(hit1, needle.size(), needle);
        hit_haystack.replace(hit2, needle.size(), needle);
        const auto* hit_b = reinterpret_cast<const uint8_t*>(hit_haystack.data());
        const auto* hit_e = hit_b + hit_haystack.size();

        for (const auto* r : run_all_kernels(searcher, hit_b, hit_e, needle.size())) {
            EXPECT_EQ(hit_b + hit1, r);
        }
        for (const auto* r : run_all_rkernels(searcher, hit_b, hit_e, needle.size())) {
            EXPECT_EQ(hit_b + hit2, r);
        }
    }

    // Homogeneous 8 KiB all-'a' needle searched against blocks of 4095 'a's + 'b' (no match)
    const std::string hom_needle(8 * 1024, 'a');
    StringSearcher hom_searcher(hom_needle.data(), hom_needle.size());
    std::string periodic_hay;
    periodic_hay.reserve(kHayLen);
    const std::string block = std::string(4095, 'a') + "b";
    while (periodic_hay.size() < kHayLen) {
        periodic_hay.append(block);
    }
    const auto* per_b = reinterpret_cast<const uint8_t*>(periodic_hay.data());
    const auto* per_e = per_b + periodic_hay.size();
    for (const auto* r : run_all_kernels(hom_searcher, per_b, per_e, hom_needle.size())) {
        EXPECT_EQ(per_e, r);
    }
    for (const auto* r : run_all_rkernels(hom_searcher, per_b, per_e, hom_needle.size())) {
        EXPECT_EQ(per_e, r);
    }

    const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_tp).count();
    EXPECT_LT(elapsed_ms, 500) << "Long repetitive needle search degraded to quadratic complexity: " << elapsed_ms
                               << " ms";
}

} // namespace starrocks
