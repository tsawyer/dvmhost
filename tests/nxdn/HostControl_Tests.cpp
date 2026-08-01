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
#include "common/nxdn/NXDNDefines.h"
#include "common/nxdn/lc/RTCH.h"
#include "host/modem/Modem.h"
#include "modem/port/IModemPort.h"
#include "host/HostTestHooks.h"

#include <catch2/catch_test_macros.hpp>

extern network::NetRPC* g_RPC;
#include "host/nxdn/Control.h"

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
 * @brief Harness class for testing NXDN host control functionality.
 */
class NXDNHostHarness {
public:
    /**
     * @brief Initializes a new instance of the NXDNHostHarness class.
     * @param authoritative Indicates whether the host is authoritative.
     */
    explicit NXDNHostHarness(bool authoritative = true) :
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
        m_modem.setModeParams(false, false, true);

        m_control = new nxdn::Control(authoritative, 1U, 1U, 4096U, 5U, 2U, &m_modem, nullptr,
            false, &m_chLookup, &m_ridLookup, &m_tidLookup, &m_idenLookup, &m_rssiMapper, false, false, false);
    }
    /**
     * @brief Finalizes an instance of the NXDNHostHarness class.
     */
    ~NXDNHostHarness()
    {
        delete m_control;
        g_RPC = nullptr;
    }

    /**
     * @brief Starts a network voice call with the specified source and destination IDs.
     * @param srcId The source ID for the network voice call.
     * @param dstId The destination ID for the network voice call.
     */
    void startNetworkVoiceCall(uint16_t srcId = 1001U, uint16_t dstId = 2001U)
    {
        nxdn::lc::RTCH control;
        control.setMessageType(nxdn::defines::MessageType::RTCH_VCALL);
        control.setSrcId(srcId);
        control.setDstId(dstId);
        control.setGroup(true);
        control.setTransmissionMode(nxdn::defines::TransmissionMode::MODE_4800);

        REQUIRE(HostTestHooks::nxdnStartNetCall(*m_control, control));
    }

public:
    network::NetRPC m_rpc;
    modem::Modem m_modem;
    lookups::ChannelLookup m_chLookup;
    lookups::RadioIdLookup m_ridLookup;
    lookups::TalkgroupRulesLookup m_tidLookup;
    lookups::IdenTableLookup m_idenLookup;
    lookups::RSSIInterpolator m_rssiMapper;
    nxdn::Control* m_control;
};

TEST_CASE("NXDN host arms the network watchdog when network voice starts", "[nxdn][host][control]")
{
    NXDNHostHarness harness;
    harness.startNetworkVoiceCall();

    REQUIRE(HostTestHooks::nxdnNetState(*harness.m_control) == RS_NET_AUDIO);
    REQUIRE(HostTestHooks::nxdnNetworkWatchdog(*harness.m_control).isRunning());
    REQUIRE_FALSE(HostTestHooks::nxdnNetworkWatchdog(*harness.m_control).hasExpired());
}

TEST_CASE("NXDN host watchdog expiry returns network voice to idle", "[nxdn][host][control]")
{
    NXDNHostHarness harness;
    harness.startNetworkVoiceCall();

    HostTestHooks::nxdnNetworkWatchdog(*harness.m_control).clock(expireTimerTicks(HostTestHooks::nxdnNetworkWatchdog(*harness.m_control)));
    harness.m_control->clock();

    REQUIRE(HostTestHooks::nxdnNetState(*harness.m_control) == RS_NET_IDLE);
    REQUIRE_FALSE(HostTestHooks::nxdnNetworkWatchdog(*harness.m_control).isRunning());
}

TEST_CASE("NXDN host net hang expiry clears active network voice state", "[nxdn][host][control]")
{
    NXDNHostHarness harness;
    harness.startNetworkVoiceCall();

    HostTestHooks::nxdnNetTGHang(*harness.m_control).clock(expireTimerTicks(HostTestHooks::nxdnNetTGHang(*harness.m_control)));
    harness.m_control->clock();

    REQUIRE(HostTestHooks::nxdnNetState(*harness.m_control) == RS_NET_AUDIO);
    REQUIRE(HostTestHooks::nxdnNetLastDstId(*harness.m_control) == 0U);
    REQUIRE(HostTestHooks::nxdnNetLastSrcId(*harness.m_control) == 0U);
}

TEST_CASE("NXDN clearRFReject returns rejected RF state to listening", "[nxdn][host][control][rf]")
{
    NXDNHostHarness harness;
    HostTestHooks::nxdnSetRFRejected(*harness.m_control);

    harness.m_control->clearRFReject();

    REQUIRE(HostTestHooks::nxdnRFState(*harness.m_control) == RS_RF_LISTENING);
}

TEST_CASE("NXDN rejects mismatched network traffic while RF hang is active", "[nxdn][host][control][rf]")
{
    NXDNHostHarness harness;
    HostTestHooks::nxdnSetRFCall(*harness.m_control, 3001U, 4001U);

    nxdn::lc::RTCH control;
    control.setMessageType(nxdn::defines::MessageType::RTCH_VCALL);
    control.setSrcId(1001U);
    control.setDstId(2001U);
    control.setGroup(true);
    control.setTransmissionMode(nxdn::defines::TransmissionMode::MODE_4800);

    REQUIRE_FALSE(HostTestHooks::nxdnStartNetCall(*harness.m_control, control));
    REQUIRE(HostTestHooks::nxdnNetState(*harness.m_control) == RS_NET_IDLE);
}

TEST_CASE("NXDN processFrame accepts a synthetic RF voice call", "[nxdn][host][control][rf]")
{
    NXDNHostHarness harness;

    REQUIRE(HostTestHooks::nxdnStartRFCall(*harness.m_control, 1001U, 2001U));
    REQUIRE(HostTestHooks::nxdnRFState(*harness.m_control) == RS_RF_AUDIO);
    REQUIRE(harness.m_control->getLastDstId() == 2001U);
}