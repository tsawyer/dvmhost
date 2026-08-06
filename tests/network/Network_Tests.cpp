// SPDX-License-Identifier: GPL-2.0-only
/*
 * Digital Voice Modem - Test Suite
 * GPLv2 Open Source. Use is subject to license terms.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES FROM THIS FILE HEADER.
 *
 *  Copyright (C) 2026 Bryan Biedenkapp, N2PLL
 *
 */
#include "Defines.h"
#include "common/network/FrameQueue.h"
#include "common/network/Network.h"
#include "common/network/RTPFNEHeader.h"
#include "common/network/RTPHeader.h"
#include "common/network/udp/Socket.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace network;
using namespace network::frame;
using namespace network::udp;

// ---------------------------------------------------------------------------
//  Global Functions
// ---------------------------------------------------------------------------
namespace {

/**
 * @brief Reserve a UDP port on the loopback interface for testing.
 * @returns uint16_t Reserved UDP port number, or 0 if reservation failed.
 */
static uint16_t reserveLoopbackPort()
{
#if defined(_WIN32)
    SOCKET fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == INVALID_SOCKET)
        return 0U;
#else
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return 0U;
#endif // defined(_WIN32)

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(0U);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
#if defined(_WIN32)
        ::closesocket(fd);
#else
        ::close(fd);
#endif // defined(_WIN32)
        return 0U;
    }

    socklen_t addrLen = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &addrLen) < 0) {
#if defined(_WIN32)
        ::closesocket(fd);
#else
        ::close(fd);
#endif // defined(_WIN32)
        return 0U;
    }

#if defined(_WIN32)
    ::closesocket(fd);
#else
    ::close(fd);
#endif // defined(_WIN32)

    return ntohs(address.sin_port);
}

/**
 * @brief Wait for a frame message to be received on the specified FrameQueue.
 * @param queue Reference to the FrameQueue to read from.
 * @param[out] out String to store the received message.
 * @param[out] rtpHeader RTPHeader to store the received RTP header.
 * @param[out] fneHeader RTPFNEHeader to store the received FNE header.
 * @returns bool True if a message was received, otherwise false.
 */
static bool waitForFrameMessage(FrameQueue& queue, std::string& out, RTPHeader& rtpHeader, RTPFNEHeader& fneHeader)
{
    for (uint32_t attempt = 0U; attempt < 100U; attempt++) {
        int messageLength = -1;
        sockaddr_storage source = {};
        uint32_t sourceLen = 0U;
        UInt8Array message = queue.read(messageLength, source, sourceLen, &rtpHeader, &fneHeader);
        if (message != nullptr && messageLength > 0) {
            out.assign(reinterpret_cast<char*>(message.get()), (size_t)messageLength);
            return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    return false;
}

/**
 * @brief Send a frame message to the specified destination using the FrameQueue.
 * @param senderQueue Reference to the FrameQueue to send from.
 * @param destination sockaddr_storage containing the destination address.
 * @param destinationLen Length of the destination address.
 * @param payload Vector containing the message payload to send.
 * @param opcode OpcodePair containing the function and sub-function.
 * @param streamId Stream ID for the message.
 * @param peerId Peer ID for the message.
 * @param ssrc SSRC for the RTP header.
 * @param sequence Sequence number for the RTP header.
 * @returns bool True if the message was sent successfully, otherwise false.
 */
static bool sendLoopbackFrame(FrameQueue& senderQueue, sockaddr_storage& destination, uint32_t destinationLen,
    const std::vector<uint8_t>& payload, FrameQueue::OpcodePair opcode, uint32_t streamId, uint32_t peerId,
    uint32_t ssrc, uint16_t sequence)
{
    return senderQueue.write(payload.data(), (uint32_t)payload.size(), streamId, peerId, ssrc, opcode, sequence,
        destination, destinationLen);
}

} // namespace

TEST_CASE("Network performs a full loopback login handshake", "[network][loopback]")
{
    const uint16_t masterPort = reserveLoopbackPort();
    REQUIRE(masterPort != 0U);

    const uint16_t localPort = reserveLoopbackPort();
    REQUIRE(localPort != 0U);

    Socket masterReceiver("127.0.0.1", masterPort);
    REQUIRE(masterReceiver.open(AF_INET));

    FrameQueue masterReceiverQueue(&masterReceiver, 0x1234U, false);
    FrameQueue masterSenderQueue(&masterReceiver, 0x5678U, false);

    sockaddr_storage networkDestination = {};
    uint32_t networkDestinationLen = 0U;
    REQUIRE(Socket::lookup("127.0.0.1", localPort, networkDestination, networkDestinationLen) == 0);

    Network network("127.0.0.1", masterPort, localPort, 4242U, "loopback-password", true, false, true, true, true, true, true, true, true, true, false, false);
    network.enable(true);
    REQUIRE(network.open());

    bool connected = false;
    network.setPeerConnectedCallback([&connected]() {
        connected = true;
    });

    network.clock(11000U);

    std::string loginPayload;
    RTPHeader loginRtp;
    RTPFNEHeader loginFne;
    REQUIRE(waitForFrameMessage(masterReceiverQueue, loginPayload, loginRtp, loginFne));
    REQUIRE(loginFne.getFunction() == NET_FUNC::RPTL);
    REQUIRE(loginPayload.size() == 8U);
    REQUIRE(std::string(loginPayload.begin(), loginPayload.begin() + 4U) == TAG_REPEATER_LOGIN);

    std::array<uint8_t, 10U> saltPayload = {};
    ::memcpy(saltPayload.data() + 6U, "ABCD", 4U);

    REQUIRE(sendLoopbackFrame(masterSenderQueue, networkDestination, networkDestinationLen, std::vector<uint8_t>(saltPayload.begin(), saltPayload.end()),
        { NET_FUNC::ACK, NET_SUBFUNC::NOP }, loginFne.getStreamId(), 4242U, 4242U, 0U));

    network.clock(1000U);

    std::string authPayload;
    RTPHeader authRtp;
    RTPFNEHeader authFne;
    REQUIRE(waitForFrameMessage(masterReceiverQueue, authPayload, authRtp, authFne));
    REQUIRE(authFne.getFunction() == NET_FUNC::RPTK);
    REQUIRE(authPayload.size() == 40U);
    REQUIRE(std::string(authPayload.begin(), authPayload.begin() + 4U) == TAG_REPEATER_AUTH);

    std::array<uint8_t, 8U> ackPayload = {};
    REQUIRE(sendLoopbackFrame(masterSenderQueue, networkDestination, networkDestinationLen, std::vector<uint8_t>(ackPayload.begin(), ackPayload.end()),
        { NET_FUNC::ACK, NET_SUBFUNC::NOP }, authFne.getStreamId(), 4242U, 4242U, 0U));

    network.clock(1000U);

    std::string configPayload;
    RTPHeader configRtp;
    RTPFNEHeader configFne;
    REQUIRE(waitForFrameMessage(masterReceiverQueue, configPayload, configRtp, configFne));
    REQUIRE(configFne.getFunction() == NET_FUNC::RPTC);
    REQUIRE(configPayload.size() >= 8U);
    REQUIRE(std::string(configPayload.begin(), configPayload.begin() + 4U) == TAG_REPEATER_CONFIG);

    std::array<uint8_t, 8U> configAckPayload = { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x80U, 0x00U };
    REQUIRE(sendLoopbackFrame(masterSenderQueue, networkDestination, networkDestinationLen, std::vector<uint8_t>(configAckPayload.begin(), configAckPayload.end()),
        { NET_FUNC::ACK, NET_SUBFUNC::NOP }, configFne.getStreamId(), 4242U, 4242U, 0U));

    network.clock(1000U);
    REQUIRE(connected);

    masterReceiver.close();
}

TEST_CASE("Network stores loopback DMR protocol frames into the receive ring buffer", "[network][loopback]")
{
    const uint16_t masterPort = reserveLoopbackPort();
    REQUIRE(masterPort != 0U);

    const uint16_t localPort = reserveLoopbackPort();
    REQUIRE(localPort != 0U);

    Socket masterReceiver("127.0.0.1", masterPort);
    REQUIRE(masterReceiver.open(AF_INET));

    FrameQueue masterReceiverQueue(&masterReceiver, 0x9ABCU, false);
    FrameQueue masterSenderQueue(&masterReceiver, 0xDEF0U, false);

    sockaddr_storage networkDestination = {};
    uint32_t networkDestinationLen = 0U;
    REQUIRE(Socket::lookup("127.0.0.1", localPort, networkDestination, networkDestinationLen) == 0);

    Network network("127.0.0.1", masterPort, localPort, 7777U, "loopback-password", true, false, true, false, false, false, true, true, true, true, false, false);
    network.enable(true);
    REQUIRE(network.open());

    network.resetDMR(1U);

    network.clock(11000U);

    std::string loginPayload;
    RTPHeader loginRtp;
    RTPFNEHeader loginFne;
    REQUIRE(waitForFrameMessage(masterReceiverQueue, loginPayload, loginRtp, loginFne));

    std::array<uint8_t, 10U> saltPayload = {};
    ::memcpy(saltPayload.data() + 6U, "ABCD", 4U);
    REQUIRE(sendLoopbackFrame(masterSenderQueue, networkDestination, networkDestinationLen, std::vector<uint8_t>(saltPayload.begin(), saltPayload.end()),
        { NET_FUNC::ACK, NET_SUBFUNC::NOP }, loginFne.getStreamId(), 7777U, 7777U, 0U));

    network.clock(1000U);

    std::string authPayload;
    RTPHeader authRtp;
    RTPFNEHeader authFne;
    REQUIRE(waitForFrameMessage(masterReceiverQueue, authPayload, authRtp, authFne));

    std::array<uint8_t, 8U> ackPayload = {};
    REQUIRE(sendLoopbackFrame(masterSenderQueue, networkDestination, networkDestinationLen, std::vector<uint8_t>(ackPayload.begin(), ackPayload.end()),
        { NET_FUNC::ACK, NET_SUBFUNC::NOP }, authFne.getStreamId(), 7777U, 7777U, 0U));

    network.clock(1000U);

    std::string configPayload;
    RTPHeader configRtp;
    RTPFNEHeader configFne;
    REQUIRE(waitForFrameMessage(masterReceiverQueue, configPayload, configRtp, configFne));

    std::array<uint8_t, 8U> configAckPayload = { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x80U, 0x00U };
    REQUIRE(sendLoopbackFrame(masterSenderQueue, networkDestination, networkDestinationLen, std::vector<uint8_t>(configAckPayload.begin(), configAckPayload.end()),
        { NET_FUNC::ACK, NET_SUBFUNC::NOP }, configFne.getStreamId(), 7777U, 7777U, 0U));

    network.clock(1000U);

    std::vector<uint8_t> payload = { 0x10U, 0x11U, 0x12U, 0x13U, 0x14U, 0x15U };
    REQUIRE(sendLoopbackFrame(masterSenderQueue, networkDestination, networkDestinationLen, payload,
        { NET_FUNC::PROTOCOL, NET_SUBFUNC::PROTOCOL_SUBFUNC_DMR }, 0x12345678U, 7777U, 7777U, 0x1234U));

    network.clock(0U);

    bool ret = false;
    uint32_t frameLength = 0U;
    UInt8Array decoded = network.readDMR(ret, frameLength);
    REQUIRE(ret);
    REQUIRE(frameLength == payload.size());
    REQUIRE(decoded != nullptr);
    REQUIRE(std::memcmp(decoded.get(), payload.data(), payload.size()) == 0);

    masterReceiver.close();
}

TEST_CASE("Network invokes loopback in-call callbacks for incoming control frames", "[network][loopback]")
{
    const uint16_t masterPort = reserveLoopbackPort();
    REQUIRE(masterPort != 0U);

    const uint16_t localPort = reserveLoopbackPort();
    REQUIRE(localPort != 0U);

    Socket masterReceiver("127.0.0.1", masterPort);
    REQUIRE(masterReceiver.open(AF_INET));

    FrameQueue masterReceiverQueue(&masterReceiver, 0xA1B2U, false);
    FrameQueue masterSenderQueue(&masterReceiver, 0xC3D4U, false);

    sockaddr_storage networkDestination = {};
    uint32_t networkDestinationLen = 0U;
    REQUIRE(Socket::lookup("127.0.0.1", localPort, networkDestination, networkDestinationLen) == 0);

    Network network("127.0.0.1", masterPort, localPort, 6000U, "loopback-password", true, false, true, false, false, false, true, true, true, true, false, false);
    network.enable(true);
    REQUIRE(network.open());

    network.clock(11000U);

    std::string loginPayload;
    RTPHeader loginRtp;
    RTPFNEHeader loginFne;
    REQUIRE(waitForFrameMessage(masterReceiverQueue, loginPayload, loginRtp, loginFne));

    std::array<uint8_t, 10U> saltPayload = {};
    ::memcpy(saltPayload.data() + 6U, "ABCD", 4U);
    REQUIRE(sendLoopbackFrame(masterSenderQueue, networkDestination, networkDestinationLen, std::vector<uint8_t>(saltPayload.begin(), saltPayload.end()),
        { NET_FUNC::ACK, NET_SUBFUNC::NOP }, loginFne.getStreamId(), 6000U, 6000U, 0U));

    network.clock(1000U);

    std::string authPayload;
    RTPHeader authRtp;
    RTPFNEHeader authFne;
    REQUIRE(waitForFrameMessage(masterReceiverQueue, authPayload, authRtp, authFne));

    std::array<uint8_t, 8U> ackPayload = {};
    REQUIRE(sendLoopbackFrame(masterSenderQueue, networkDestination, networkDestinationLen, std::vector<uint8_t>(ackPayload.begin(), ackPayload.end()),
        { NET_FUNC::ACK, NET_SUBFUNC::NOP }, authFne.getStreamId(), 6000U, 6000U, 0U));

    network.clock(1000U);

    std::string configPayload;
    RTPHeader configRtp;
    RTPFNEHeader configFne;
    REQUIRE(waitForFrameMessage(masterReceiverQueue, configPayload, configRtp, configFne));

    std::array<uint8_t, 8U> configAckPayload = { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x80U, 0x00U };
    REQUIRE(sendLoopbackFrame(masterSenderQueue, networkDestination, networkDestinationLen, std::vector<uint8_t>(configAckPayload.begin(), configAckPayload.end()),
        { NET_FUNC::ACK, NET_SUBFUNC::NOP }, configFne.getStreamId(), 6000U, 6000U, 0U));

    network.clock(1000U);

    bool callbackFired = false;
    NET_ICC::ENUM lastCommand = NET_ICC::NOP;
    uint32_t lastDstId = 0U;
    uint8_t lastSlot = 0U;
    uint32_t lastPeerId = 0U;
    uint32_t lastSsrc = 0U;
    uint32_t lastStreamId = 0U;

    network.setDMRICCCallback([&](NET_ICC::ENUM command, uint32_t dstId, uint8_t slotNo, uint32_t peerId, uint32_t ssrc, uint32_t streamId) {
        callbackFired = true;
        lastCommand = command;
        lastDstId = dstId;
        lastSlot = slotNo;
        lastPeerId = peerId;
        lastSsrc = ssrc;
        lastStreamId = streamId;
    });

    std::vector<uint8_t> payload(15U, 0x00U);
    payload[10U] = static_cast<uint8_t>(NET_ICC::REJECT_TRAFFIC);
    payload[11U] = 0x0AU;
    payload[12U] = 0x0BU;
    payload[13U] = 0x0CU;
    payload[14U] = 0x02U;
    REQUIRE(sendLoopbackFrame(masterSenderQueue, networkDestination, networkDestinationLen, payload,
        { NET_FUNC::INCALL_CTRL, NET_SUBFUNC::PROTOCOL_SUBFUNC_DMR }, 0x55AA55AAU, 6000U, 6000U, 0x0102U));

    network.clock(1000U);

    REQUIRE(callbackFired);
    REQUIRE(lastCommand == NET_ICC::REJECT_TRAFFIC);
    REQUIRE(lastDstId == 0x0A0B0CU);
    REQUIRE(lastSlot == 0x02U);
    REQUIRE(lastPeerId == 6000U);
    REQUIRE(lastSsrc == 6000U);
    REQUIRE(lastStreamId == 0x55AA55AAU);

    masterReceiver.close();
}
