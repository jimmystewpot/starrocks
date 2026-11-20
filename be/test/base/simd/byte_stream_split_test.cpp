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

#include "base/simd/byte_stream_split.h"

#include <gtest/gtest.h>

namespace starrocks {

TEST(ByteStreamSplitTest, ByteStreamSplitFLBA) {
    auto f = [](int byte_width, int num_values) {
        std::cout << "running ByteStreamSplitFLBA test for byte_width: " << byte_width << ", N: " << num_values
                  << std::endl;
        const int SIZE = byte_width * num_values;
        std::vector<uint8_t> bytes_data(SIZE);

        for (int i = 0; i < SIZE; i++) {
            bytes_data[i] = (uint8_t)(i % 256);
        }

        std::vector<uint8_t> expected(SIZE);
        int idx = 0;
        for (int i = 0; i < byte_width; i++) {
            for (int j = 0; j < num_values; j++) {
                expected[idx++] = bytes_data[j * byte_width + i];
            }
        }

        std::vector<uint8_t> encoded(SIZE);
        ByteStreamSplitUtil::ByteStreamSplitEncode(bytes_data.data(), byte_width, num_values, encoded.data());
        for (int i = 0; i < SIZE; i++) {
            ASSERT_EQ(encoded[i], expected[i]);
        }
        std::vector<uint8_t> decoded(SIZE);
        ByteStreamSplitUtil::ByteStreamSplitDecode(encoded.data(), byte_width, num_values, num_values, decoded.data());
        for (int i = 0; i < SIZE; i++) {
            ASSERT_EQ(decoded[i], bytes_data[i]);
        }
    };

    std::vector<int> byte_widths = {1, 2, 4, 5, 8, 15, 16, 31, 32, 63, 64, 127, 128};
    std::vector<int> num_values = {3, 4, 5, 10, 31, 127, 255};
    for (int byte_width : byte_widths) {
        for (int num_value : num_values) {
            f(byte_width, num_value);
        }
    }
}

namespace {

template <int kWidth>
void VerifyByteStreamSplitAgainstScalar(int64_t num_values) {
    const int64_t total_bytes = num_values * kWidth;
    std::vector<uint8_t> raw_data(total_bytes);
    for (int64_t i = 0; i < total_bytes; ++i) {
        raw_data[i] = static_cast<uint8_t>((i * 131 + (i >> 3) * 17 + 29) & 0xFF);
    }

    std::vector<uint8_t> expected_encoded(total_bytes, 0);
    std::vector<uint8_t> actual_encoded(total_bytes, 0xAA);
    ByteStreamSplitUtil::ByteStreamSplitEncodeScalar<kWidth>(raw_data.data(), kWidth, num_values,
                                                             expected_encoded.data());
    ByteStreamSplitUtil::ByteStreamSplitEncode(raw_data.data(), kWidth, num_values, actual_encoded.data());
    ASSERT_EQ(actual_encoded, expected_encoded) << "Encode mismatch for width=" << kWidth << " N=" << num_values;

    std::vector<uint8_t> expected_decoded(total_bytes, 0);
    std::vector<uint8_t> actual_decoded(total_bytes, 0x55);
    ByteStreamSplitUtil::ByteStreamSplitDecodeScalar<kWidth>(expected_encoded.data(), kWidth, num_values, num_values,
                                                             expected_decoded.data());
    ByteStreamSplitUtil::ByteStreamSplitDecode(actual_encoded.data(), kWidth, num_values, num_values,
                                               actual_decoded.data());
    ASSERT_EQ(actual_decoded, raw_data) << "Decode round-trip mismatch for width=" << kWidth << " N=" << num_values;
    ASSERT_EQ(actual_decoded, expected_decoded) << "Decode scalar mismatch for width=" << kWidth << " N=" << num_values;

    // Also test non-tight stride (stride > num_values) for Decode
    const int64_t padded_stride = num_values + 19;
    std::vector<uint8_t> strided_encoded(padded_stride * kWidth, 0xCC);
    for (int s = 0; s < kWidth; ++s) {
        if (num_values > 0) {
            std::memcpy(&strided_encoded[s * padded_stride], &expected_encoded[s * num_values], num_values);
        }
    }
    std::fill(actual_decoded.begin(), actual_decoded.end(), 0x55);
    ByteStreamSplitUtil::ByteStreamSplitDecode(strided_encoded.data(), kWidth, num_values, padded_stride,
                                               actual_decoded.data());
    ASSERT_EQ(actual_decoded, raw_data) << "Strided decode mismatch for width=" << kWidth << " N=" << num_values;

#if defined(__ARM_NEON) || defined(__SSE4_2__)
    if constexpr (kWidth == 2 || kWidth == 4 || kWidth == 8) {
        std::vector<uint8_t> simd128_encoded(total_bytes, 0xBB);
        ByteStreamSplitUtil::ByteStreamSplitEncodeSimd128<kWidth>(raw_data.data(), kWidth, num_values,
                                                                  simd128_encoded.data());
        ASSERT_EQ(simd128_encoded, expected_encoded)
                << "Simd128 Encode mismatch for width=" << kWidth << " N=" << num_values;

        std::vector<uint8_t> simd128_decoded(total_bytes, 0x66);
        ByteStreamSplitUtil::ByteStreamSplitDecodeSimd128<kWidth>(expected_encoded.data(), kWidth, num_values,
                                                                  num_values, simd128_decoded.data());
        ASSERT_EQ(simd128_decoded, raw_data) << "Simd128 Decode mismatch for width=" << kWidth << " N=" << num_values;
    }
#endif

#if defined(__AVX2__)
    if constexpr (kWidth == 2 || kWidth == 4 || kWidth == 8) {
        std::vector<uint8_t> avx2_encoded(total_bytes, 0xDD);
        ByteStreamSplitUtil::ByteStreamSplitEncodeAvx2<kWidth>(raw_data.data(), kWidth, num_values,
                                                               avx2_encoded.data());
        ASSERT_EQ(avx2_encoded, expected_encoded) << "Avx2 Encode mismatch for width=" << kWidth << " N=" << num_values;

        std::vector<uint8_t> avx2_decoded(total_bytes, 0x77);
        ByteStreamSplitUtil::ByteStreamSplitDecodeAvx2<kWidth>(expected_encoded.data(), kWidth, num_values, num_values,
                                                               avx2_decoded.data());
        ASSERT_EQ(avx2_decoded, raw_data) << "Avx2 Decode mismatch for width=" << kWidth << " N=" << num_values;
    }
#endif
}

} // namespace

TEST(ByteStreamSplitTest, BoundarySizesParityWithScalar) {
    const std::vector<int64_t> kBoundaryCounts = {0, 1, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 65, 128, 1024};
    for (int64_t n : kBoundaryCounts) {
        VerifyByteStreamSplitAgainstScalar<2>(n);
        VerifyByteStreamSplitAgainstScalar<4>(n);
        VerifyByteStreamSplitAgainstScalar<8>(n);
        VerifyByteStreamSplitAgainstScalar<16>(n);
    }
}

} // namespace starrocks
