// SPDX-License-Identifier: GPL-2.0-only
/*
 * Digital Voice Modem - Test Suite
 * GPLv2 Open Source. Use is subject to license terms.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 *  Copyright (C) 2026 Bryan Biedenkapp, N2PLL
 *
 */
#include "host/Defines.h"
#include "common/p25/P25Defines.h"
#include "common/p25/kmm/KMMFactory.h"
#include "common/p25/kmm/KMMHello.h"
#include "common/p25/kmm/KMMNoService.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>

using namespace p25;
using namespace p25::defines;
using namespace p25::kmm;

TEST_CASE("KMM factory decodes encoded HELLO and NO_SERVICE frames", "[p25][kmm][factory]")
{
    SECTION("HELLO frame round-trips with message number") {
        KMMHello tx;
        tx.setDstLLId(0x123456U);
        tx.setSrcLLId(0x654321U);
        tx.setMessageNumber(0x1122U);
        tx.setFlag(KMM_HelloFlag::REKEY_REQUEST_NO_UKEK);

        UInt8Array buffer = std::make_unique<uint8_t[]>(tx.fullLength());
        ::memset(buffer.get(), 0x00U, tx.fullLength());
        tx.encode(buffer.get());

        std::unique_ptr<KMMFrame> base = KMMFactory::create(buffer.get());
        REQUIRE(base != nullptr);

        KMMHello* rx = dynamic_cast<KMMHello*>(base.get());
        REQUIRE(rx != nullptr);
        REQUIRE(rx->getMessageId() == KMM_MessageType::HELLO);
        REQUIRE(rx->getFlag() == KMM_HelloFlag::REKEY_REQUEST_NO_UKEK);
        REQUIRE(rx->getDstLLId() == 0x123456U);
        REQUIRE(rx->getSrcLLId() == 0x654321U);
        REQUIRE(rx->getHasMessageNumber() == true);
        REQUIRE(rx->getMessageNumber() == 0x1122U);
    }

    SECTION("NO_SERVICE frame round-trips") {
        KMMNoService tx;
        tx.setDstLLId(0x010203U);
        tx.setSrcLLId(0xA0B0C0U);

        UInt8Array buffer = std::make_unique<uint8_t[]>(tx.fullLength());
        ::memset(buffer.get(), 0x00U, tx.fullLength());
        tx.encode(buffer.get());

        std::unique_ptr<KMMFrame> base = KMMFactory::create(buffer.get());
        REQUIRE(base != nullptr);

        KMMNoService* rx = dynamic_cast<KMMNoService*>(base.get());
        REQUIRE(rx != nullptr);
        REQUIRE(rx->getMessageId() == KMM_MessageType::NO_SERVICE);
        REQUIRE(rx->getDstLLId() == 0x010203U);
        REQUIRE(rx->getSrcLLId() == 0xA0B0C0U);
    }

    SECTION("Factory returns nullptr for unknown message ID") {
        uint8_t buffer[16U] = { 0U };
        buffer[0U] = 0xFFU;

        std::unique_ptr<KMMFrame> frame = KMMFactory::create(buffer);
        REQUIRE(frame == nullptr);
    }
}
