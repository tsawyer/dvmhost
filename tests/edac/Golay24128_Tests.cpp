// SPDX-License-Identifier: GPL-2.0-only
/*
 * Digital Voice Modem - Test Suite
 * GPLv2 Open Source. Use is subject to license terms.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 *  Copyright (C) 2026 Bryan Biedenkapp, N2PLL
 *
 */

#include "common/Log.h"
#include "common/Utils.h"

#include <catch2/catch_test_macros.hpp>
#include <cstring>

#include "common/edac/Golay24128.h"

using namespace edac;

/*
** NOTE: decode23127 operates on the packed 23-bit form used by the voice FEC
** code paths. The raw 24-bit table value returned by encode23127() includes an
** extra alignment bit, so direct round-trip tests should shift right by one.
*/

TEST_CASE("Golay24128 encode23127 preserves zero data", "[edac][golay24128]") {
    uint32_t data = 0x000U;
    uint32_t encoded = Golay24128::encode23127(data);
    
    REQUIRE(encoded == 0x000000U);  // All zeros should encode to all zeros
}

TEST_CASE("Golay24128 encode23127 produces valid encodings", "[edac][golay24128]") {
    // Test that encoding produces non-zero values for non-zero inputs (uses lookup table)
    const uint32_t testValues[] = {
        0x001U, 0x555U, 0xAAAU, 0x0FFU, 0xF00U
    };

    for (auto value : testValues) {
        uint32_t encoded = Golay24128::encode23127(value);
        
        // Just verify encoding produces a non-zero value for non-zero input
        REQUIRE(encoded != 0x000000U);
        // Encoded value should fit in 24 bits (despite name "23127", encodes to 24 bits)
        REQUIRE((encoded & 0xFF000000U) == 0);
    }
}

TEST_CASE("Golay24128 decode23127 round trips packed encoded values", "[edac][golay24128]") {
    for (uint32_t value = 0U; value < 0x1000U; value++) {
        uint32_t packed = Golay24128::encode23127(value) >> 1;
        REQUIRE(Golay24128::decode23127(packed) == value);
    }
}

TEST_CASE("Golay24128 encode24128 preserves zero data", "[edac][golay24128]") {
    uint32_t data = 0x000U;
    uint32_t encoded = Golay24128::encode24128(data);
    uint32_t decoded;
    bool result = Golay24128::decode24128(encoded, decoded);

    REQUIRE(result);
    REQUIRE(decoded == data);
}

TEST_CASE("Golay24128 encode24128 preserves all-ones data", "[edac][golay24128]") {
    uint32_t data = 0xFFFU;  // 12 bits of data
    uint32_t encoded = Golay24128::encode24128(data);
    uint32_t decoded;
    bool result = Golay24128::decode24128(encoded, decoded);

    REQUIRE(result);
    REQUIRE(decoded == data);
}

TEST_CASE("Golay24128 encode24128 handles various patterns", "[edac][golay24128]") {
    const uint32_t testValues[] = {
        0x000U, 0x555U, 0xAAAU, 0x0FFU, 0xF00U, 0x333U, 0xCCCU,
        0x5A5U, 0xA5AU, 0x123U, 0x456U, 0x789U, 0xABCU, 0xDEFU
    };

    for (auto value : testValues) {
        uint32_t encoded = Golay24128::encode24128(value);
        uint32_t decoded;
        bool result = Golay24128::decode24128(encoded, decoded);

        REQUIRE(result);
        REQUIRE(decoded == value);
    }
}

TEST_CASE("Golay24128 encode24128 corrects single-bit errors", "[edac][golay24128]") {
    uint32_t original = 0xA5AU;
    uint32_t encoded = Golay24128::encode24128(original);

    // Test single-bit errors in all 24 bit positions
    for (uint32_t bit = 0U; bit < 24U; bit++) {
        uint32_t corrupted = encoded ^ (1U << bit);
        uint32_t decoded;
        bool result = Golay24128::decode24128(corrupted, decoded);

        REQUIRE(result);
        REQUIRE(decoded == original);
    }
}

TEST_CASE("Golay24128 encode24128 corrects two-bit errors", "[edac][golay24128]") {
    uint32_t original = 0x3C3U;
    uint32_t encoded = Golay24128::encode24128(original);

    // Test two-bit error patterns
    const uint32_t errorPairs[][2] = {
        {0, 6}, {1, 11}, {4, 16}, {8, 19}, {13, 23}
    };

    for (auto& pair : errorPairs) {
        uint32_t corrupted = encoded ^ (1U << pair[0]) ^ (1U << pair[1]);
        uint32_t decoded;
        bool result = Golay24128::decode24128(corrupted, decoded);

        REQUIRE(result);
        REQUIRE(decoded == original);
    }
}

TEST_CASE("Golay24128 encode24128 detects uncorrectable errors", "[edac][golay24128]") {
    uint32_t original = 0x456U;
    uint32_t encoded = Golay24128::encode24128(original);

    // Introduce 4 bit errors (beyond correction capability of 3)
    uint32_t corrupted = encoded ^ (1U << 0) ^ (1U << 7) ^ (1U << 14) ^ (1U << 21);
    uint32_t decoded;
    bool result = Golay24128::decode24128(corrupted, decoded);

    // Should fail or return incorrect data
    if (result) {
        // If it doesn't fail, the decoded data should not match
        REQUIRE(decoded != original);
    } else {
        // Or it should return false
        REQUIRE_FALSE(result);
    }
}

TEST_CASE("Golay24128 encode24128 accepts all correctable <=3-bit errors", "[edac][golay24128]") {
    const uint32_t testValues[] = {0x000U, 0xA5AU};

    for (auto original : testValues) {
        uint32_t encoded = Golay24128::encode24128(original);

        for (uint32_t i = 0U; i < 24U; i++) {
            uint32_t corrupted = encoded ^ (1U << i);
            uint32_t decoded;
            bool result = Golay24128::decode24128(corrupted, decoded);

            REQUIRE(result);
            REQUIRE(decoded == original);

            for (uint32_t j = i + 1U; j < 24U; j++) {
                corrupted = encoded ^ (1U << i) ^ (1U << j);
                result = Golay24128::decode24128(corrupted, decoded);

                REQUIRE(result);
                REQUIRE(decoded == original);

                for (uint32_t k = j + 1U; k < 24U; k++) {
                    corrupted = encoded ^ (1U << i) ^ (1U << j) ^ (1U << k);
                    result = Golay24128::decode24128(corrupted, decoded);

                    REQUIRE(result);
                    REQUIRE(decoded == original);
                }
            }
        }
    }
}

TEST_CASE("Golay24128 encode24128 byte array interface works", "[edac][golay24128]") {
    // Test the byte array encode/decode interface
    // 3 input bytes → 6 encoded bytes (two 24-bit Golay codewords)
    // So 6 input bytes → 12 encoded bytes
    const uint8_t testData[] = {0x12U, 0x34U, 0x56U, 0x78U, 0x9AU, 0xBCU};
    uint8_t encoded[12U];  // 6 bytes data → 12 bytes encoded
    uint8_t decoded[6U];

    Golay24128::encode24128(encoded, testData, 6U);

    Golay24128::decode24128(decoded, encoded, 6U);

    REQUIRE(::memcmp(decoded, testData, 6U) == 0);
}
