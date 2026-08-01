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
#include "host/modem/Modem.h"
#include "modem/port/IModemPort.h"
#include "host/HostTestHooks.h"

#include <catch2/catch_test_macros.hpp>

extern network::NetRPC* g_RPC;
#include "host/dmr/Control.h"

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
 * @brief Harness class for testing DMR host control functionality.
 */
class DMRHostHarness {
public:
    /**
     * @brief Initializes a new instance of the DMRHostHarness class.
     * @param authoritative Indicates whether the host is authoritative.
     */
    explicit DMRHostHarness(bool authoritative = true) :
        m_rpc("127.0.0.1", 1U, 0U, "test", false),
        m_modem(new TestModemPort(), false, false, false, false, false, false,
            0U, 0U, 0U, 4096U, 4096U, 1024U, true, true, false, false, false, false),
        m_chLookup(),
        m_ridLookup("", 0U, false, false),
        m_tidLookup("", 0U, false, false),
        m_idenLookup("", 0U),
        m_rssiMapper(),
        m_control(nullptr)
    {
        g_RPC = &m_rpc;
        m_modem.setModeParams(true, false, false);

        m_control = new dmr::Control(authoritative, 1U, 1U, 4096U, false, false, 5U, 2U,
            &m_modem, nullptr, false, &m_chLookup, &m_ridLookup, &m_tidLookup, &m_idenLookup,
            &m_rssiMapper, 60U, false, false, false, false, false);
    }
    /**
     * @brief Finalizes an instance of the DMRHostHarness class.
     */
    ~DMRHostHarness()
    {
        delete m_control;
        g_RPC = nullptr;
    }

public:
    network::NetRPC m_rpc;
    modem::Modem m_modem;
    lookups::ChannelLookup m_chLookup;
    lookups::RadioIdLookup m_ridLookup;
    lookups::TalkgroupRulesLookup m_tidLookup;
    lookups::IdenTableLookup m_idenLookup;
    lookups::RSSIInterpolator m_rssiMapper;
    dmr::Control* m_control;
};

TEST_CASE("DMR slot network voice start arms watchdog on the targeted slot", "[dmr][host][control]")
{
    DMRHostHarness harness;
    HostTestHooks::dmrStartNetVoiceCall(*HostTestHooks::dmrSlot1(*harness.m_control), 1001U, 2001U);

    REQUIRE(HostTestHooks::dmrNetState(*HostTestHooks::dmrSlot1(*harness.m_control)) == RS_NET_AUDIO);
    REQUIRE(HostTestHooks::dmrNetworkWatchdog(*HostTestHooks::dmrSlot1(*harness.m_control)).isRunning());
    REQUIRE(HostTestHooks::dmrNetState(*HostTestHooks::dmrSlot2(*harness.m_control)) == RS_NET_IDLE);
}

TEST_CASE("DMR slot watchdog expiry returns that slot to idle", "[dmr][host][control]")
{
    DMRHostHarness harness;
    HostTestHooks::dmrStartNetVoiceCall(*HostTestHooks::dmrSlot1(*harness.m_control), 1001U, 2001U);

    HostTestHooks::dmrNetworkWatchdog(*HostTestHooks::dmrSlot1(*harness.m_control)).clock(
        expireTimerTicks(HostTestHooks::dmrNetworkWatchdog(*HostTestHooks::dmrSlot1(*harness.m_control))));
    HostTestHooks::dmrSlot1(*harness.m_control)->clock();

    REQUIRE(HostTestHooks::dmrNetState(*HostTestHooks::dmrSlot1(*harness.m_control)) == RS_NET_IDLE);
}

TEST_CASE("DMR permittedTG only affects the targeted slot", "[dmr][host][control]")
{
    DMRHostHarness harness(false);

    harness.m_control->permittedTG(2001U, 1U);

    REQUIRE(HostTestHooks::dmrPermittedDstId(*HostTestHooks::dmrSlot1(*harness.m_control)) == 2001U);
    REQUIRE(HostTestHooks::dmrPermittedDstId(*HostTestHooks::dmrSlot2(*harness.m_control)) == 0U);
}

TEST_CASE("DMR clearRFReject only clears the targeted slot", "[dmr][host][control][rf]")
{
    DMRHostHarness harness;
    HostTestHooks::dmrSetRFRejected(*HostTestHooks::dmrSlot1(*harness.m_control));
    HostTestHooks::dmrSetRFRejected(*HostTestHooks::dmrSlot2(*harness.m_control));

    harness.m_control->clearRFReject(1U);

    REQUIRE(HostTestHooks::dmrRFState(*HostTestHooks::dmrSlot1(*harness.m_control)) == RS_RF_LISTENING);
    REQUIRE(HostTestHooks::dmrRFState(*HostTestHooks::dmrSlot2(*harness.m_control)) == RS_RF_REJECTED);
}

TEST_CASE("DMR slot rejects mismatched network traffic while RF hang is active", "[dmr][host][control][rf]")
{
    DMRHostHarness harness;
    HostTestHooks::dmrSetRFCall(*HostTestHooks::dmrSlot1(*harness.m_control), 3001U, 4001U);

    HostTestHooks::dmrStartNetVoiceCall(*HostTestHooks::dmrSlot1(*harness.m_control), 1001U, 2001U);

    REQUIRE(HostTestHooks::dmrNetState(*HostTestHooks::dmrSlot1(*harness.m_control)) == RS_NET_IDLE);
}

TEST_CASE("DMR processFrame accepts a synthetic RF voice call on the targeted slot", "[dmr][host][control][rf]")
{
    DMRHostHarness harness;

    REQUIRE(HostTestHooks::dmrStartRFVoiceCall(*HostTestHooks::dmrSlot1(*harness.m_control), 1001U, 2001U));
    REQUIRE(HostTestHooks::dmrRFState(*HostTestHooks::dmrSlot1(*harness.m_control)) == RS_RF_AUDIO);
    REQUIRE(harness.m_control->getLastDstId(1U) == 2001U);
}