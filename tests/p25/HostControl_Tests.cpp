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
#include "common/lookups/ChannelLookup.h"
#include "common/lookups/IdenTableLookup.h"
#include "common/lookups/RSSIInterpolator.h"
#include "common/lookups/RadioIdLookup.h"
#include "common/lookups/TalkgroupRulesLookup.h"
#include "common/network/NetRPC.h"
#include "common/p25/P25Defines.h"
#include "common/p25/data/LowSpeedData.h"
#include "common/p25/lc/LC.h"
#include "host/modem/Modem.h"
#include "modem/port/IModemPort.h"
#include "host/HostTestHooks.h"

#include <catch2/catch_test_macros.hpp>
#include <cstring>

extern network::NetRPC* g_RPC;
#include "host/p25/Control.h"

// ---------------------------------------------------------------------------
//  Constants
// ---------------------------------------------------------------------------

// LDU1_1K payload bytes from firmware calibration constants (without leading control byte).
static const uint8_t CAL_P25_LDU1_1K[p25::defines::P25_LDU_FRAME_LENGTH_BYTES] = {
    0x55U, 0x75U, 0xF5U, 0xFFU, 0x77U, 0xFFU, 0x29U, 0x35U, 0x54U, 0x7BU, 0xCBU, 0x19U, 0x4DU, 0x0DU, 0xCEU, 0x24U, 0xA1U, 0x24U,
    0x0DU, 0x43U, 0x3CU, 0x0BU, 0xE1U, 0xB9U, 0x18U, 0x44U, 0xFCU, 0xC1U, 0x62U, 0x96U, 0x27U, 0x60U, 0xE4U, 0xE2U, 0x4AU, 0x10U,
    0x90U, 0xD4U, 0x33U, 0xC0U, 0xBEU, 0x1BU, 0x91U, 0x84U, 0x4CU, 0xFCU, 0x16U, 0x29U, 0x62U, 0x76U, 0x0EU, 0xC0U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x03U, 0x89U, 0x28U, 0x49U, 0x0DU, 0x43U, 0x3CU, 0x02U, 0xF8U, 0x6EU, 0x46U, 0x11U, 0x3FU, 0xC1U, 0x62U, 0x94U,
    0x89U, 0xD8U, 0x39U, 0x00U, 0x00U, 0x00U, 0x00U, 0x1CU, 0x38U, 0x24U, 0xA1U, 0x24U, 0x35U, 0x0CU, 0xF0U, 0x2FU, 0x86U, 0xE4U,
    0x18U, 0x44U, 0xFFU, 0x05U, 0x8AU, 0x58U, 0x9DU, 0x83U, 0xB0U, 0x00U, 0x00U, 0x00U, 0x00U, 0x70U, 0xE2U, 0x4AU, 0x12U, 0x40U,
    0xD4U, 0x33U, 0xC0U, 0xBEU, 0x1BU, 0x91U, 0x84U, 0x4FU, 0xF0U, 0x16U, 0x29U, 0x62U, 0x76U, 0x0EU, 0x6DU, 0xE5U, 0xD5U, 0x48U,
    0xADU, 0xE3U, 0x89U, 0x28U, 0x49U, 0x0DU, 0x43U, 0x3CU, 0x08U, 0xF8U, 0x6EU, 0x46U, 0x11U, 0x3FU, 0xC1U, 0x62U, 0x96U, 0x24U,
    0xD8U, 0x3BU, 0xA1U, 0x41U, 0xC2U, 0xD2U, 0xBAU, 0x38U, 0x90U, 0xA1U, 0x24U, 0x35U, 0x0CU, 0xF0U, 0x2FU, 0x86U, 0xE4U, 0x60U,
    0x44U, 0xFFU, 0x05U, 0x8AU, 0x58U, 0x9DU, 0x83U, 0x94U, 0xC8U, 0xFBU, 0x02U, 0x35U, 0xA4U, 0xE2U, 0x4AU, 0x12U, 0x43U, 0x50U,
    0x33U, 0xC0U, 0xBEU, 0x1BU, 0x91U, 0x84U, 0x4FU, 0xF0U, 0x58U, 0x29U, 0x62U, 0x76U, 0x0EU, 0xC0U, 0x00U, 0x00U, 0x00U, 0x0CU,
    0x89U, 0x28U, 0x49U, 0x0DU, 0x43U, 0x3CU, 0x0BU, 0xE1U, 0xB8U, 0x46U, 0x11U, 0x3FU, 0xC1U, 0x62U, 0x96U, 0x27U, 0x60U, 0xE4U
};

// LDU2_1K payload bytes from firmware calibration constants (without leading control byte).
static const uint8_t CAL_P25_LDU2_1K[p25::defines::P25_LDU_FRAME_LENGTH_BYTES] = {
    0x55U, 0x75U, 0xF5U, 0xFFU, 0x77U, 0xFFU, 0x29U, 0x3AU, 0xB8U, 0xA4U, 0xEFU, 0xB0U, 0x9AU, 0x8AU, 0xCEU, 0x24U, 0xA1U, 0x24U,
    0x0DU, 0x43U, 0x3CU, 0x0BU, 0xE1U, 0xB9U, 0x18U, 0x44U, 0xFCU, 0xC1U, 0x62U, 0x96U, 0x27U, 0x60U, 0xECU, 0xE2U, 0x4AU, 0x10U,
    0x90U, 0xD4U, 0x33U, 0xC0U, 0xBEU, 0x1BU, 0x91U, 0x84U, 0x4CU, 0xFCU, 0x16U, 0x29U, 0x62U, 0x76U, 0x0EU, 0x40U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x03U, 0x89U, 0x28U, 0x49U, 0x0DU, 0x43U, 0x3CU, 0x02U, 0xF8U, 0x6EU, 0x46U, 0x11U, 0x3FU, 0xC1U, 0x62U, 0x94U,
    0x89U, 0xD8U, 0x3BU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x38U, 0x24U, 0xA1U, 0x24U, 0x35U, 0x0CU, 0xF0U, 0x2FU, 0x86U, 0xE4U,
    0x18U, 0x44U, 0xFFU, 0x05U, 0x8AU, 0x58U, 0x9DU, 0x83U, 0x90U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0xE2U, 0x4AU, 0x12U, 0x40U,
    0xD4U, 0x33U, 0xC0U, 0xBEU, 0x1BU, 0x91U, 0x84U, 0x4FU, 0xF0U, 0x16U, 0x29U, 0x62U, 0x76U, 0x0EU, 0xE0U, 0xE0U, 0x00U, 0x00U,
    0x00U, 0x03U, 0x89U, 0x28U, 0x49U, 0x0DU, 0x43U, 0x3CU, 0x08U, 0xF8U, 0x6EU, 0x46U, 0x11U, 0x3FU, 0xC1U, 0x62U, 0x96U, 0x24U,
    0xD8U, 0x39U, 0xAEU, 0x8BU, 0x48U, 0xB6U, 0x49U, 0x38U, 0x90U, 0xA1U, 0x24U, 0x35U, 0x0CU, 0xF0U, 0x2FU, 0x86U, 0xE4U, 0x60U,
    0x44U, 0xFFU, 0x05U, 0x8AU, 0x58U, 0x9DU, 0x83U, 0xB9U, 0xA8U, 0xF4U, 0xF1U, 0xFDU, 0x60U, 0xE2U, 0x4AU, 0x12U, 0x43U, 0x50U,
    0x33U, 0xC0U, 0xBEU, 0x1BU, 0x91U, 0x84U, 0x4FU, 0xF0U, 0x58U, 0x29U, 0x62U, 0x76U, 0x0EU, 0x40U, 0x00U, 0x00U, 0x00U, 0x0CU,
    0x89U, 0x28U, 0x49U, 0x0DU, 0x43U, 0x3CU, 0x0BU, 0xE1U, 0xB8U, 0x46U, 0x11U, 0x3FU, 0xC1U, 0x62U, 0x96U, 0x27U, 0x60U, 0xECU
};

// ---------------------------------------------------------------------------
//  Global Functions
// ---------------------------------------------------------------------------
namespace {

/**
 * @brief Helper function to calculate the number of timer ticks needed to expire a given timer.
 * @param timer The timer for which to calculate the expiration ticks.
 * @returns uint32_t The number of timer ticks needed to expire the timer.
 */
uint32_t expireTimerTicks(const Timer& timer)
{
    return (timer.getTimeout() + 1U) * 1000U;
}

}

/**
 * @brief Builds a P25 RF frame.
 * @param payload The payload data to include in the frame.
 * @param frame The buffer to store the constructed frame.
 */
void buildP25RFFrame(const uint8_t* payload, uint8_t* frame)
{
    frame[0U] = modem::TAG_DATA;
    frame[1U] = 0x01U;
    ::memcpy(frame + 2U, payload, p25::defines::P25_LDU_FRAME_LENGTH_BYTES);
}

// ---------------------------------------------------------------------------
//  Class Declaration
// ---------------------------------------------------------------------------

/**
 * @brief Dummy implementation of a modem port for testing purposes.
 */
class TestModemPort final : public modem::port::IModemPort {
public:
    /**
     * @brief Finalizes the instance of the TestModemPort class.
     */
    ~TestModemPort() override = default;

    /**
     * @brief Opens the modem port.
     * @returns bool True if the modem port was successfully opened, false otherwise.
     */
    bool open() override { return true; }

    /**
     * @brief Reads data from the modem port.
     * @param buffer The buffer to store the read data.
     * @param length The number of bytes to read.
     * @returns int The number of bytes actually read.
     */
    int read(uint8_t* buffer, uint32_t length) override 
    { 
        (void)buffer; 
        (void)length; 
        return 0; 
    }

    /**
     * @brief Writes data to the modem port.
     * @param buffer The buffer containing the data to write.
     * @param length The number of bytes to write.
     * @returns int The number of bytes actually written.
     */
    int write(const uint8_t* buffer, uint32_t length) override 
    { 
        (void)buffer; 
        return static_cast<int>(length); 
    }
    
    /**
     * @brief Closes the modem port.
     */
    void close() override {}
};

// ---------------------------------------------------------------------------
//  Class Declaration
// ---------------------------------------------------------------------------

/**
 * @brief Harness class for testing P25 host control functionality.
 */
class P25HostHarness {
public:
    /**
     * @brief Initializes a new instance of the P25HostHarness class.
     * @param authoritative Indicates whether the host is authoritative.
     */
    explicit P25HostHarness(bool authoritative = true) :
        m_rpc("127.0.0.1", 1U, 0U, "test", false),
        m_modem(new TestModemPort(), false, false, false, false, false, false,
            0U, 0U, 0U, 1024U, 4096U, 1024U, true, true, false, false, false, false),
        m_chLookup(),
        m_ridLookup("", 0U, false, false),
        m_tidLookup("", 0U, false, false),
        m_idenLookup("", 0U),
        m_rssiMapper(),
        m_control(nullptr)
    {
        g_RPC = &m_rpc;
        m_modem.setModeParams(false, true, false);

        m_control = new p25::Control(authoritative, 0x293U, 1U, 4096U, &m_modem, nullptr,
            5U, 2U, false, &m_chLookup, &m_ridLookup, &m_tidLookup, &m_idenLookup,
            &m_rssiMapper, false, false, false, false, false);
    }
    /**
     * @brief Finalizes an instance of the P25HostHarness class.
     */
    ~P25HostHarness()
    {
        delete m_control;
        g_RPC = nullptr;
    }

    /**
     * @brief Starts a network voice call with the specified source and destination IDs.
     * @param srcId The source ID for the network voice call.
     * @param dstId The destination ID for the network voice call.
     */
    void startNetworkVoiceCall(uint32_t srcId = 1001U, uint32_t dstId = 2001U)
    {
        p25::lc::LC control;
        control.setLCO(p25::defines::LCO::GROUP);
        control.setMFId(p25::defines::MFG_STANDARD);
        control.setSrcId(srcId);
        control.setDstId(dstId);

        p25::data::LowSpeedData lsd;

        HostTestHooks::p25StartNetCall(*m_control, control, lsd);
    }

public:
    network::NetRPC m_rpc;
    modem::Modem m_modem;
    lookups::ChannelLookup m_chLookup;
    lookups::RadioIdLookup m_ridLookup;
    lookups::TalkgroupRulesLookup m_tidLookup;
    lookups::IdenTableLookup m_idenLookup;
    lookups::RSSIInterpolator m_rssiMapper;
    p25::Control* m_control;
};

TEST_CASE("P25 host arms the network watchdog when network voice starts", "[p25][host][control]")
{
    P25HostHarness harness;

    harness.startNetworkVoiceCall();

    REQUIRE(HostTestHooks::p25NetState(*harness.m_control) == RS_NET_AUDIO);
    REQUIRE(HostTestHooks::p25NetworkWatchdog(*harness.m_control).isRunning());
    REQUIRE_FALSE(HostTestHooks::p25NetworkWatchdog(*harness.m_control).hasExpired());
}

TEST_CASE("P25 host watchdog expiry returns network voice to idle", "[p25][host][control]")
{
    P25HostHarness harness;
    harness.startNetworkVoiceCall();

    HostTestHooks::p25NetworkWatchdog(*harness.m_control).clock(expireTimerTicks(HostTestHooks::p25NetworkWatchdog(*harness.m_control)));
    harness.m_control->clock();

    REQUIRE(HostTestHooks::p25NetState(*harness.m_control) == RS_NET_IDLE);
    REQUIRE(HostTestHooks::p25TailOnIdle(*harness.m_control));
    REQUIRE_FALSE(HostTestHooks::p25NetworkWatchdog(*harness.m_control).isRunning());
}

TEST_CASE("P25 host net hang expiry clears active network voice state", "[p25][host][control]")
{
    P25HostHarness harness;
    harness.startNetworkVoiceCall();

    HostTestHooks::p25NetTGHang(*harness.m_control).clock(expireTimerTicks(HostTestHooks::p25NetTGHang(*harness.m_control)));
    harness.m_control->clock();

    REQUIRE(HostTestHooks::p25NetState(*harness.m_control) == RS_NET_IDLE);
    REQUIRE(HostTestHooks::p25TailOnIdle(*harness.m_control));
    REQUIRE(HostTestHooks::p25NetLastDstId(*harness.m_control) == 0U);
    REQUIRE(HostTestHooks::p25NetLastSrcId(*harness.m_control) == 0U);
    REQUIRE_FALSE(HostTestHooks::p25NetworkWatchdog(*harness.m_control).isRunning());
}

TEST_CASE("P25 host network terminator clears active network voice state", "[p25][host][control]")
{
    P25HostHarness harness;
    harness.startNetworkVoiceCall();

    p25::lc::LC control;
    control.setLCO(p25::defines::LCO::GROUP);
    control.setSrcId(1001U);
    control.setDstId(2001U);

    REQUIRE(HostTestHooks::p25TerminateNetCall(*harness.m_control, control));
    REQUIRE(HostTestHooks::p25NetState(*harness.m_control) == RS_NET_IDLE);
    REQUIRE(HostTestHooks::p25TailOnIdle(*harness.m_control));
    REQUIRE_FALSE(HostTestHooks::p25NetworkWatchdog(*harness.m_control).isRunning());
}

TEST_CASE("P25 non-authoritative unpermit clears active network state", "[p25][host][control]")
{
    P25HostHarness harness(false);
    harness.m_control->permittedTG(2001U);
    harness.startNetworkVoiceCall();

    harness.m_control->permittedTG(0U);

    REQUIRE(HostTestHooks::p25PermittedDstId(*harness.m_control) == 0U);
    REQUIRE(HostTestHooks::p25NetState(*harness.m_control) == RS_NET_IDLE);
    REQUIRE_FALSE(HostTestHooks::p25NetworkWatchdog(*harness.m_control).isRunning());
}

TEST_CASE("P25 clearRFReject returns rejected RF state to listening", "[p25][host][control][rf]")
{
    P25HostHarness harness;
    HostTestHooks::p25SetRFRejected(*harness.m_control);

    harness.m_control->clearRFReject();

    REQUIRE(HostTestHooks::p25RFState(*harness.m_control) == RS_RF_LISTENING);
}

TEST_CASE("P25 processFrame accepts calibration-based RF LDU1 frame", "[p25][host][rf]")
{
    P25HostHarness harness;
    harness.m_control->reset();

    uint8_t frame[p25::defines::P25_LDU_FRAME_LENGTH_BYTES + 2U];
    buildP25RFFrame(CAL_P25_LDU1_1K, frame);
    HostTestHooks::p25StampRFFrameNID(*harness.m_control, frame, p25::defines::DUID::LDU1);

    REQUIRE(harness.m_control->processFrame(frame, sizeof(frame)));
    REQUIRE(HostTestHooks::p25RFState(*harness.m_control) == RS_RF_AUDIO);
    REQUIRE(harness.m_control->getLastSrcId() == 1U);
    REQUIRE(harness.m_control->getLastDstId() == 1U);
}

TEST_CASE("P25 processFrame accepts calibration-based RF LDU2 frame after LDU1", "[p25][host][rf]")
{
    P25HostHarness harness;
    harness.m_control->reset();

    uint8_t ldu1[p25::defines::P25_LDU_FRAME_LENGTH_BYTES + 2U];
    uint8_t ldu2[p25::defines::P25_LDU_FRAME_LENGTH_BYTES + 2U];
    buildP25RFFrame(CAL_P25_LDU1_1K, ldu1);
    buildP25RFFrame(CAL_P25_LDU2_1K, ldu2);
    HostTestHooks::p25StampRFFrameNID(*harness.m_control, ldu1, p25::defines::DUID::LDU1);
    HostTestHooks::p25StampRFFrameNID(*harness.m_control, ldu2, p25::defines::DUID::LDU2);

    REQUIRE(harness.m_control->processFrame(ldu1, sizeof(ldu1)));
    REQUIRE(harness.m_control->processFrame(ldu2, sizeof(ldu2)));
    REQUIRE(HostTestHooks::p25RFState(*harness.m_control) == RS_RF_AUDIO);
    REQUIRE(harness.m_control->getLastSrcId() == 1U);
    REQUIRE(harness.m_control->getLastDstId() == 1U);
}
