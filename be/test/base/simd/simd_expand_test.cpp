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

#include <cstring>
#include <random>
#include <vector>

#include "base/simd/expand.h"
#include "base/testutil/parallel_test.h"
#include "gtest/gtest.h"
#include "types/value_generator.h"

namespace starrocks {

template <class CppType>
void test_expand_load() {
    const size_t chunk_size = 4095;
    std::vector<CppType> srcs;
    std::vector<CppType> dsts;

    srcs.resize(chunk_size);
    dsts.resize(chunk_size);

    for (size_t i = 0; i < chunk_size; ++i) {
        srcs[i] = i;
    }

    {
        // test no nulls
        std::vector<uint8_t> nulls;
        nulls.resize(chunk_size);

        SIMD::Expand::expand_load(dsts.data(), srcs.data(), nulls.data(), chunk_size);
        for (size_t i = 0; i < chunk_size; ++i) {
            ASSERT_EQ(srcs[i], dsts[i]);
        }

        SIMD::Expand::expand_load_branchless(dsts.data(), srcs.data(), nulls.data(), chunk_size);
        for (size_t i = 0; i < chunk_size; ++i) {
            ASSERT_EQ(srcs[i], dsts[i]);
        }
    }
    {
        // test all nulls
        std::vector<uint8_t> nulls;
        nulls.resize(chunk_size, 1);
        // all nulls we don't have to check result
        SIMD::Expand::expand_load_branchless(dsts.data(), srcs.data(), nulls.data(), chunk_size);
        SIMD::Expand::expand_load(dsts.data(), srcs.data(), nulls.data(), chunk_size);
    }
    {
        // test interleave
        std::vector<uint8_t> nulls;
        nulls.resize(chunk_size);
        for (size_t i = 0; i < chunk_size; ++i) {
            nulls[i] = i % 2;
        }

        std::vector<CppType> dsts1(chunk_size);
        std::vector<CppType> dsts2(chunk_size);

        SIMD::Expand::expand_load_branchless(dsts1.data(), srcs.data(), nulls.data(), chunk_size);
        SIMD::Expand::expand_load(dsts2.data(), srcs.data(), nulls.data(), chunk_size);
        for (size_t i = 0; i < chunk_size; ++i) {
            if (nulls[i] == 0) {
                ASSERT_EQ(dsts1[i], dsts2[i]);
            }
        }
    }
    {
        // test enum

        std::vector<uint8_t> nulls;
        nulls.resize(chunk_size);
        // Enumerate every non-zero 8-bit null pattern. Iterating the masks directly instead of using
        // std::next_permutation avoids a GCC -Wstringop-overflow false positive on the vectorized std::reverse.
        size_t i = 0;
        for (uint32_t mask = 1; mask < 256 && i + 8 < chunk_size; ++mask) {
            for (int j = 0; j < 8; ++j) {
                nulls[i++] = (mask >> j) & 1;
            }
        }

        std::vector<CppType> dsts1(chunk_size);
        std::vector<CppType> dsts2(chunk_size);

        SIMD::Expand::expand_load_branchless(dsts1.data(), srcs.data(), nulls.data(), chunk_size);
        SIMD::Expand::expand_load(dsts2.data(), srcs.data(), nulls.data(), chunk_size);

        for (size_t i = 0; i < chunk_size; ++i) {
            if (nulls[i] == 0) {
                ASSERT_EQ(dsts1[i], dsts2[i]);
            }
        }
    }

    // test full rand
    {
        using Gen = RandomGenerator<uint8_t, 2>;
        std::vector<uint8_t> nulls;
        nulls.resize(chunk_size);
        for (size_t i = 0; i < chunk_size; ++i) {
            nulls[i] = Gen::next_value();
        }

        std::vector<CppType> dsts1(chunk_size);
        std::vector<CppType> dsts2(chunk_size);

        SIMD::Expand::expand_load_branchless(dsts1.data(), srcs.data(), nulls.data(), chunk_size);
        SIMD::Expand::expand_load(dsts2.data(), srcs.data(), nulls.data(), chunk_size);

        for (size_t i = 0; i < chunk_size; ++i) {
            if (nulls[i] == 0) {
                ASSERT_EQ(dsts1[i], dsts2[i]);
            }
        }
    }
}

PARALLEL_TEST(ExpandTest, All) {
    test_expand_load<int8_t>();
    test_expand_load<int16_t>();
    test_expand_load<int32_t>();
    test_expand_load<int64_t>();
    test_expand_load<float>();
    test_expand_load<double>();
}

// The source may be misaligned for the element type (a parquet page's values start wherever the definition
// levels end): every variant must read it correctly, including the scalar tails.
template <class CppType>
void test_expand_load_misaligned() {
    std::mt19937 rng(7);
    for (size_t count : {1, 3, 17, 64, 4095}) {
        std::vector<uint8_t> nulls(count);
        std::vector<CppType> values;
        for (size_t i = 0; i < count; ++i) {
            nulls[i] = rng() % 3 == 0;
            if (!nulls[i]) values.push_back(static_cast<CppType>(rng()));
        }
        // trailing nulls would read one element past the values; stop at the last non-null row as callers do
        size_t expand_count = count;
        while (expand_count > 0 && nulls[expand_count - 1]) --expand_count;
        std::vector<char> bytes(1 + values.size() * sizeof(CppType));
        if (!values.empty()) memcpy(bytes.data() + 1, values.data(), values.size() * sizeof(CppType));
        const auto* src = reinterpret_cast<const CppType*>(bytes.data() + 1);

        std::vector<CppType> dst(count), dst_branchless(count);
        SIMD::Expand::expand_load(dst.data(), src, nulls.data(), expand_count);
        SIMD::Expand::expand_load_branchless(dst_branchless.data(), src, nulls.data(), expand_count);
        size_t k = 0;
        for (size_t i = 0; i < count; ++i) {
            if (!nulls[i]) {
                ASSERT_EQ(values[k], dst[i]) << "count " << count << " row " << i;
                ASSERT_EQ(values[k], dst_branchless[i]) << "count " << count << " row " << i;
                k++;
            }
        }
    }
}

PARALLEL_TEST(ExpandTest, Misaligned) {
    test_expand_load_misaligned<int8_t>();
    test_expand_load_misaligned<int16_t>();
    test_expand_load_misaligned<int32_t>();
    test_expand_load_misaligned<int64_t>();
    test_expand_load_misaligned<float>();
    test_expand_load_misaligned<double>();
}

template <class CppType>
void verify_expand_load_pattern(const std::vector<uint8_t>& nulls) {
    const size_t count = nulls.size();
    size_t total_valid = 0;
    for (size_t i = 0; i < count; ++i) {
        total_valid += (nulls[i] == 0);
    }

    // Allocate src strictly to total_valid elements so ASan catches any out-of-bounds read.
    std::vector<CppType> src(total_valid);
    for (size_t i = 0; i < total_valid; ++i) {
        src[i] = static_cast<CppType>((i * 37) + 11);
    }
    const CppType* src_ptr = (total_valid == 0) ? nullptr : src.data();

    // 1. Test 4-argument overload
    {
        std::vector<CppType> dst(count, static_cast<CppType>(0));
        SIMD::Expand::expand_load(dst.data(), src_ptr, nulls.data(), count);
        size_t k = 0;
        for (size_t i = 0; i < count; ++i) {
            if (nulls[i] == 0) {
                ASSERT_EQ(src[k], dst[i])
                        << "4-arg mismatch at count=" << count << " total_valid=" << total_valid << " row=" << i;
                ++k;
            }
        }
        ASSERT_EQ(total_valid, k);
    }

    // 2. Test 5-argument overload (with precomputed total_valid)
    {
        std::vector<CppType> dst(count, static_cast<CppType>(0));
        SIMD::Expand::expand_load(dst.data(), src_ptr, nulls.data(), count, total_valid);
        size_t k = 0;
        for (size_t i = 0; i < count; ++i) {
            if (nulls[i] == 0) {
                ASSERT_EQ(src[k], dst[i])
                        << "5-arg mismatch at count=" << count << " total_valid=" << total_valid << " row=" << i;
                ++k;
            }
        }
        ASSERT_EQ(total_valid, k);
    }

    // 3. Test branchless reference overloads with strict src sizing
    {
        std::vector<CppType> dst4(count, static_cast<CppType>(0));
        std::vector<CppType> dst5(count, static_cast<CppType>(0));
        SIMD::Expand::expand_load_branchless(dst4.data(), src_ptr, nulls.data(), count);
        SIMD::Expand::expand_load_branchless(dst5.data(), src_ptr, nulls.data(), count, total_valid);
        size_t k = 0;
        for (size_t i = 0; i < count; ++i) {
            if (nulls[i] == 0) {
                ASSERT_EQ(src[k], dst4[i]) << "branchless 4-arg mismatch at count=" << count << " row=" << i;
                ASSERT_EQ(src[k], dst5[i]) << "branchless 5-arg mismatch at count=" << count << " row=" << i;
                ++k;
            }
        }
        ASSERT_EQ(total_valid, k);
    }
}

template <class CppType>
void test_expand_load_comprehensive() {
    const size_t sizes[] = {0, 1, 3, 4, 7, 8, 15, 16, 17, 31, 32, 33, 64, 128, 1024};
    std::mt19937 rng(42);

    for (size_t count : sizes) {
        // 1. All-null (total_valid == 0)
        {
            std::vector<uint8_t> nulls(count, 1);
            verify_expand_load_pattern<CppType>(nulls);
        }

        // 2. All-valid (total_valid == count)
        {
            std::vector<uint8_t> nulls(count, 0);
            verify_expand_load_pattern<CppType>(nulls);
        }

        // 3. Leading all-valid 16-row and 32-row batches followed by all-null or mixed
        for (size_t batch_width : {size_t(16), size_t(32)}) {
            // Leading all-valid batch, trailing all-null
            {
                std::vector<uint8_t> nulls(count, 1);
                for (size_t i = 0; i < std::min(count, batch_width); ++i) {
                    nulls[i] = 0;
                }
                verify_expand_load_pattern<CppType>(nulls);
            }
            // Leading all-null batch, trailing all-valid
            {
                std::vector<uint8_t> nulls(count, 0);
                for (size_t i = 0; i < std::min(count, batch_width); ++i) {
                    nulls[i] = 1;
                }
                verify_expand_load_pattern<CppType>(nulls);
            }
            // Trailing all-valid batch, leading all-null
            if (count >= batch_width) {
                std::vector<uint8_t> nulls(count, 1);
                for (size_t i = count - batch_width; i < count; ++i) {
                    nulls[i] = 0;
                }
                verify_expand_load_pattern<CppType>(nulls);
            }
            // Trailing all-null batch, leading all-valid
            if (count >= batch_width) {
                std::vector<uint8_t> nulls(count, 0);
                for (size_t i = count - batch_width; i < count; ++i) {
                    nulls[i] = 1;
                }
                verify_expand_load_pattern<CppType>(nulls);
            }
            // Alternating 16/32-row all-valid / all-null / mixed blocks
            {
                std::vector<uint8_t> nulls(count, 0);
                for (size_t i = 0; i < count; ++i) {
                    size_t block = (i / batch_width) % 3;
                    if (block == 0) {
                        nulls[i] = 0;
                    } else if (block == 1) {
                        nulls[i] = 1;
                    } else {
                        nulls[i] = static_cast<uint8_t>((i % 3 == 0) ? 1 : 0);
                    }
                }
                verify_expand_load_pattern<CppType>(nulls);
            }
        }

        // 4. Random mixed null masks across multiple densities (10%, 50%, 90% nulls)
        for (int null_pct : {10, 25, 50, 75, 90}) {
            std::vector<uint8_t> nulls(count);
            for (size_t i = 0; i < count; ++i) {
                nulls[i] = (static_cast<int>(rng() % 100) < null_pct) ? 1 : 0;
            }
            verify_expand_load_pattern<CppType>(nulls);
        }
    }
}

PARALLEL_TEST(ExpandTest, ComprehensiveOverloadsAndStrictBounds) {
    test_expand_load_comprehensive<int8_t>();
    test_expand_load_comprehensive<int16_t>();
    test_expand_load_comprehensive<int32_t>();
    test_expand_load_comprehensive<int64_t>();
    test_expand_load_comprehensive<float>();
    test_expand_load_comprehensive<double>();
}

} // namespace starrocks
