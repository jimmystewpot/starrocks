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

#include "base/simd/delta_decode.h"

#include <gtest/gtest.h>

#include <climits>
#include <limits>
#include <vector>

namespace starrocks {
class DeltaDecodeTest : public testing::Test {
public:
    void SetUp() override {}
    void TearDown() override {}
};

// The ISA-specific kernels carry per-function target attributes and are always compiled, so the
// guards below are runtime __builtin_cpu_supports checks rather than compile-time #ifdefs: this
// exercises the AVX2 and AVX-512 kernels on capable CPUs even when the binary was built with
// USE_AVX512=OFF. The AVX-512 kernels are skipped under ASan (known zmm-spill redzone trap),
// matching the dispatcher.

namespace {

void verify_all_int32_kernels(const std::vector<int32_t>& values, int32_t min_delta, int32_t start_last) {
    const int n = static_cast<int>(values.size());
    std::vector<int32_t> expected(values);
    int32_t expected_last = start_last;
    delta_decode_chain_scalar_prefetch<int32_t>(expected.data(), n, min_delta, expected_last);
    if (n > 0) {
        ASSERT_EQ(expected.back(), expected_last);
    } else {
        ASSERT_EQ(expected_last, start_last);
    }

    // public dispatcher
    {
        std::vector<int32_t> v(values);
        int32_t last_value = start_last;
        delta_decode_chain_int32(v.data(), n, min_delta, last_value);
        ASSERT_EQ(v, expected) << "n=" << n << " min_delta=" << min_delta;
        ASSERT_EQ(last_value, expected_last) << "n=" << n << " min_delta=" << min_delta;
    }

    // scalar entry point
    {
        std::vector<int32_t> v(values);
        int32_t last_value = start_last;
        delta_decode_chain_int32_scalar(v.data(), n, min_delta, last_value);
        ASSERT_EQ(v, expected) << "n=" << n << " min_delta=" << min_delta;
        ASSERT_EQ(last_value, expected_last) << "n=" << n << " min_delta=" << min_delta;
    }

#if defined(__ARM_NEON) && defined(__aarch64__)
    {
        std::vector<int32_t> v(values);
        int32_t last_value = start_last;
        delta_decode_chain_int32_neon(v.data(), n, min_delta, last_value);
        ASSERT_EQ(v, expected) << "n=" << n << " min_delta=" << min_delta;
        ASSERT_EQ(last_value, expected_last) << "n=" << n << " min_delta=" << min_delta;
    }
#endif

#if defined(__x86_64__)
    if (__builtin_cpu_supports("avx2")) {
        std::vector<int32_t> v(values);
        int32_t last_value = start_last;
        delta_decode_chain_int32_avx2(v.data(), n, min_delta, last_value);
        ASSERT_EQ(v, expected) << "n=" << n << " min_delta=" << min_delta;
        ASSERT_EQ(last_value, expected_last) << "n=" << n << " min_delta=" << min_delta;
    }
#if !defined(ADDRESS_SANITIZER) && !defined(__SANITIZE_ADDRESS__)
    if (__builtin_cpu_supports("avx512f")) {
        std::vector<int32_t> v(values);
        int32_t last_value = start_last;
        delta_decode_chain_int32_avx512(v.data(), n, min_delta, last_value);
        ASSERT_EQ(v, expected) << "n=" << n << " min_delta=" << min_delta;
        ASSERT_EQ(last_value, expected_last) << "n=" << n << " min_delta=" << min_delta;
    }
#endif
#endif
}

void verify_all_int64_kernels(const std::vector<int64_t>& values, int64_t min_delta, int64_t start_last) {
    const int n = static_cast<int>(values.size());
    std::vector<int64_t> expected(values);
    int64_t expected_last = start_last;
    delta_decode_chain_scalar_prefetch<int64_t>(expected.data(), n, min_delta, expected_last);
    if (n > 0) {
        ASSERT_EQ(expected.back(), expected_last);
    } else {
        ASSERT_EQ(expected_last, start_last);
    }

    // public dispatcher
    {
        std::vector<int64_t> v(values);
        int64_t last_value = start_last;
        delta_decode_chain_int64(v.data(), n, min_delta, last_value);
        ASSERT_EQ(v, expected) << "n=" << n << " min_delta=" << min_delta;
        ASSERT_EQ(last_value, expected_last) << "n=" << n << " min_delta=" << min_delta;
    }

    // scalar entry point
    {
        std::vector<int64_t> v(values);
        int64_t last_value = start_last;
        delta_decode_chain_int64_scalar(v.data(), n, min_delta, last_value);
        ASSERT_EQ(v, expected) << "n=" << n << " min_delta=" << min_delta;
        ASSERT_EQ(last_value, expected_last) << "n=" << n << " min_delta=" << min_delta;
    }

#if defined(__ARM_NEON) && defined(__aarch64__)
    {
        std::vector<int64_t> v(values);
        int64_t last_value = start_last;
        delta_decode_chain_int64_neon(v.data(), n, min_delta, last_value);
        ASSERT_EQ(v, expected) << "n=" << n << " min_delta=" << min_delta;
        ASSERT_EQ(last_value, expected_last) << "n=" << n << " min_delta=" << min_delta;
    }
#endif

#if defined(__x86_64__)
    if (__builtin_cpu_supports("avx2")) {
        std::vector<int64_t> v(values);
        int64_t last_value = start_last;
        delta_decode_chain_int64_avx2(v.data(), n, min_delta, last_value);
        ASSERT_EQ(v, expected) << "n=" << n << " min_delta=" << min_delta;
        ASSERT_EQ(last_value, expected_last) << "n=" << n << " min_delta=" << min_delta;
    }
#if !defined(ADDRESS_SANITIZER) && !defined(__SANITIZE_ADDRESS__)
    if (__builtin_cpu_supports("avx512f")) {
        std::vector<int64_t> v(values);
        int64_t last_value = start_last;
        delta_decode_chain_int64_avx512(v.data(), n, min_delta, last_value);
        ASSERT_EQ(v, expected) << "n=" << n << " min_delta=" << min_delta;
        ASSERT_EQ(last_value, expected_last) << "n=" << n << " min_delta=" << min_delta;
    }
#endif
#endif
}

} // namespace

TEST_F(DeltaDecodeTest, test_int32) {
    std::vector<int32_t> values(233, 1);
    verify_all_int32_kernels(values, 10, 10);
}

TEST_F(DeltaDecodeTest, test_int64) {
    std::vector<int64_t> values(223, 1);
    verify_all_int64_kernels(values, 10, 10);
}

TEST_F(DeltaDecodeTest, test_boundary_sizes_and_negative_deltas) {
    std::vector<int> sizes;
    for (int n = 0; n <= 65; ++n) {
        sizes.push_back(n);
    }
    sizes.push_back(1024);

    for (int n : sizes) {
        std::vector<int32_t> base32(n);
        std::vector<int64_t> base64(n);
        for (int i = 0; i < n; ++i) {
            base32[i] = ((i * 7) % 13) - 6;
            base64[i] = ((i * 11) % 19) - 9;
        }

        for (int32_t min_delta : {0, 5, -10}) {
            verify_all_int32_kernels(base32, min_delta, -500);
        }

        for (int64_t min_delta : {int64_t{0}, int64_t{5}, int64_t{-10}}) {
            verify_all_int64_kernels(base64, min_delta, -500);
        }
    }
}

TEST_F(DeltaDecodeTest, test_int32_wrapping_overflow) {
    // Deliberately test values that wrap across INT32_MAX and INT32_MIN across sizes 0..65 and 1024.
    // In standard C++, signed overflow is UB unless unsigned or SIMD wrap-around arithmetic is used.
    // Under UBSan (-fsanitize=undefined), this verifies zero traps occur.
    std::vector<int> sizes;
    for (int n = 0; n <= 65; ++n) {
        sizes.push_back(n);
    }
    sizes.push_back(137);
    sizes.push_back(1024);

    const int32_t boundary_deltas[] = {INT32_MAX, INT32_MIN, 1, -1, 1000, -1000, INT32_MAX / 2, INT32_MIN / 2};
    for (int sz : sizes) {
        std::vector<int32_t> values(sz);
        for (int i = 0; i < sz; ++i) {
            values[i] = boundary_deltas[i % 8];
        }
        verify_all_int32_kernels(values, 100, INT32_MAX - 50);
        verify_all_int32_kernels(values, -100, INT32_MIN + 50);
        verify_all_int32_kernels(values, INT32_MAX, INT32_MAX);
        verify_all_int32_kernels(values, INT32_MIN, INT32_MIN);
    }
}

TEST_F(DeltaDecodeTest, test_int64_wrapping_overflow) {
    std::vector<int> sizes;
    for (int n = 0; n <= 65; ++n) {
        sizes.push_back(n);
    }
    sizes.push_back(137);
    sizes.push_back(1024);

    const int64_t boundary_deltas[] = {INT64_MAX, INT64_MIN, 1, -1, 1000, -1000, INT64_MAX / 2, INT64_MIN / 2};
    for (int sz : sizes) {
        std::vector<int64_t> values(sz);
        for (int i = 0; i < sz; ++i) {
            values[i] = boundary_deltas[i % 8];
        }
        verify_all_int64_kernels(values, 100, INT64_MAX - 50);
        verify_all_int64_kernels(values, -100, INT64_MIN + 50);
        verify_all_int64_kernels(values, INT64_MAX, INT64_MAX);
        verify_all_int64_kernels(values, INT64_MIN, INT64_MIN);
    }
}

TEST_F(DeltaDecodeTest, test_scalar_various_sizes) {
    const std::vector<size_t> sizes = {0, 1, 2, 7, 8, 9, 15, 16, 23, 24, 31, 32, 64, 127, 1024};
    for (size_t sz : sizes) {
        // int32
        {
            std::vector<int32_t> values(sz);
            for (size_t i = 0; i < sz; i++) {
                values[i] = static_cast<int32_t>(i + 1);
            }
            verify_all_int32_kernels(values, 3, 5);
        }
        // int64
        {
            std::vector<int64_t> values(sz);
            for (size_t i = 0; i < sz; i++) {
                values[i] = static_cast<int64_t>(i + 1);
            }
            verify_all_int64_kernels(values, 3, 5);
        }
    }
}

TEST_F(DeltaDecodeTest, test_contains_negative_value) {
    for (size_t sz = 0; sz <= 129; ++sz) {
        std::vector<int32_t> data(sz, 0);
        for (size_t i = 0; i < sz; ++i) {
            data[i] = (i % 2 == 0) ? 0 : ((i % 3 == 0) ? INT32_MAX : static_cast<int32_t>(i));
        }
        EXPECT_FALSE(contains_negative_value(data.data(), sz)) << "sz=" << sz;
        EXPECT_FALSE(contains_negative_value_scalar(data.data(), static_cast<int>(sz))) << "sz=" << sz;

        // Explicitly exercises index 0, index 31, index 32, last index (sz - 1), and all intermediate positions.
        for (size_t pos = 0; pos < sz; ++pos) {
            int32_t saved = data[pos];
            data[pos] = -1;
            EXPECT_TRUE(contains_negative_value(data.data(), sz)) << "sz=" << sz << " pos=" << pos;
            EXPECT_TRUE(contains_negative_value_scalar(data.data(), static_cast<int>(sz)))
                    << "sz=" << sz << " pos=" << pos;
            data[pos] = INT32_MIN;
            EXPECT_TRUE(contains_negative_value(data.data(), sz)) << "sz=" << sz << " pos=" << pos;
            EXPECT_TRUE(contains_negative_value_scalar(data.data(), static_cast<int>(sz)))
                    << "sz=" << sz << " pos=" << pos;
            data[pos] = saved;
        }
    }
}

TEST_F(DeltaDecodeTest, ContainsNegativeValueBenchmarkRegimes) {
    for (int n : {4096, 4099, 8192, 8195, 16384, 32768}) {
        std::vector<int32_t> buf(n, 42);
        EXPECT_FALSE(contains_negative_value_scalar(buf.data(), n));
        EXPECT_FALSE(contains_negative_value(buf.data(), n));

        for (int pos : {0, 31, 32, n - 1}) {
            buf[pos] = -1;
            EXPECT_TRUE(contains_negative_value_scalar(buf.data(), n)) << "n=" << n << " pos=" << pos;
            EXPECT_TRUE(contains_negative_value(buf.data(), n)) << "n=" << n << " pos=" << pos;
            buf[pos] = 42;
        }
    }
}

} // namespace starrocks
