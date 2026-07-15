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
#include "common/network/udp/Socket.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>

using namespace network::udp;

namespace
{
#if defined(_WIN32)
    using NativeSocket = SOCKET;
    static constexpr NativeSocket INVALID_NATIVE_SOCKET = INVALID_SOCKET;
#else
    using NativeSocket = int;
    static constexpr NativeSocket INVALID_NATIVE_SOCKET = -1;
#endif // defined(_WIN32)

    void closeNativeSocket(NativeSocket fd)
    {
#if defined(_WIN32)
        if (fd != INVALID_NATIVE_SOCKET)
            ::closesocket(fd);
#else
        if (fd != INVALID_NATIVE_SOCKET)
            ::close(fd);
#endif // defined(_WIN32)
    }

    class LoopbackReceiver {
    public:
        LoopbackReceiver() :
            m_fd(INVALID_NATIVE_SOCKET),
            m_address(),
            m_addrLen(0U),
            m_wsaStarted(false)
        {
#if defined(_WIN32)
            WSAData data;
            if (::WSAStartup(MAKEWORD(2, 2), &data) != 0)
                return;
            m_wsaStarted = true;
#endif // defined(_WIN32)

            m_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
            if (m_fd == INVALID_NATIVE_SOCKET)
                return;

            sockaddr_in address = {};
            address.sin_family = AF_INET;
            address.sin_port = htons(0U);
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

            if (::bind(m_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
                closeNativeSocket(m_fd);
                m_fd = INVALID_NATIVE_SOCKET;
                return;
            }

            socklen_t addrLen = sizeof(address);
            if (::getsockname(m_fd, reinterpret_cast<sockaddr*>(&address), &addrLen) < 0) {
                closeNativeSocket(m_fd);
                m_fd = INVALID_NATIVE_SOCKET;
                return;
            }

            ::memcpy(&m_address, &address, sizeof(address));
            m_addrLen = (uint32_t)addrLen;

#if defined(_WIN32)
            DWORD timeout = 250U;
            ::setsockopt(m_fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
            timeval timeout = {};
            timeout.tv_usec = 250000;
            ::setsockopt(m_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif // defined(_WIN32)
        }

        ~LoopbackReceiver()
        {
            closeNativeSocket(m_fd);
#if defined(_WIN32)
            if (m_wsaStarted)
                ::WSACleanup();
#endif // defined(_WIN32)
        }

        bool valid() const
        {
            return m_fd != INVALID_NATIVE_SOCKET;
        }

        const sockaddr_storage& address() const
        {
            return m_address;
        }

        uint32_t addrLen() const
        {
            return m_addrLen;
        }

        bool receive(const std::string& expected)
        {
            uint8_t buffer[256U] = { 0U };
            sockaddr_storage source = {};
            socklen_t sourceLen = sizeof(source);
            ssize_t length = ::recvfrom(m_fd, reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
                reinterpret_cast<sockaddr*>(&source), &sourceLen);
            if (length < 0)
                return false;

            if ((size_t)length != expected.size())
                return false;

            return ::memcmp(buffer, expected.data(), expected.size()) == 0;
        }

    private:
        NativeSocket m_fd;
        sockaddr_storage m_address;
        uint32_t m_addrLen;
        bool m_wsaStarted;
    };

    UDPDatagram* makeDatagram(const std::string& payload, const sockaddr_storage& address, uint32_t addrLen)
    {
        UDPDatagram* datagram = new UDPDatagram;
        datagram->buffer = new uint8_t[payload.size()];
        ::memcpy(datagram->buffer, payload.data(), payload.size());
        datagram->length = payload.size();
        datagram->address = address;
        datagram->addrLen = addrLen;
        return datagram;
    }
}

TEST_CASE("UDP socket sends every datagram in a batch", "[network][udp][batch]")
{
    LoopbackReceiver receivers[5U];
    for (const LoopbackReceiver& receiver : receivers)
        REQUIRE(receiver.valid());

    Socket sender;
    REQUIRE(sender.open(AF_INET));

    const std::string payloads[5U] = { "peer-1", "peer-2", "peer-3", "peer-4", "peer-5" };
    BufferQueue queue;
    ssize_t expectedBytes = 0;
    for (size_t i = 0U; i < 5U; i++) {
        queue.push(makeDatagram(payloads[i], receivers[i].address(), receivers[i].addrLen()));
        expectedBytes += (ssize_t)payloads[i].size();
    }

    ssize_t bytesWritten = -1;
    REQUIRE(sender.write(&queue, &bytesWritten));
    REQUIRE(bytesWritten == expectedBytes);
    REQUIRE(queue.empty());

    for (size_t i = 0U; i < 5U; i++)
        REQUIRE(receivers[i].receive(payloads[i]));

    sender.close();
}

// The old implementation may terminate under an address sanitizer because it
// reads uninitialized cleanup pointers. Keep this destructive regression case
// opt-in when the suite is run against an old binary.
TEST_CASE("UDP socket compacts a batch containing a skipped entry", "[network][udp][batch][.unsafe-old]")
{
    LoopbackReceiver first;
    LoopbackReceiver second;
    REQUIRE(first.valid());
    REQUIRE(second.valid());

    Socket sender;
    REQUIRE(sender.open(AF_INET));

    const std::string firstPayload = "before-skip";
    const std::string secondPayload = "after-skip";
    BufferQueue queue;
    queue.push(makeDatagram(firstPayload, first.address(), first.addrLen()));
    queue.push(nullptr);
    queue.push(makeDatagram(secondPayload, second.address(), second.addrLen()));

    ssize_t bytesWritten = -1;
    REQUIRE(sender.write(&queue, &bytesWritten));
    REQUIRE(bytesWritten == (ssize_t)(firstPayload.size() + secondPayload.size()));
    REQUIRE(queue.empty());
    REQUIRE(first.receive(firstPayload));
    REQUIRE(second.receive(secondPayload));

    sender.close();
}

TEST_CASE("UDP socket reports a complete batch send failure", "[network][udp][batch]")
{
    LoopbackReceiver receiver;
    REQUIRE(receiver.valid());

    Socket sender;
    REQUIRE(sender.open(AF_INET));

    BufferQueue queue;
    queue.push(makeDatagram("invalid-address-length", receiver.address(), 0U));

    ssize_t bytesWritten = 0;
    REQUIRE_FALSE(sender.write(&queue, &bytesWritten));
    REQUIRE(bytesWritten == -1);
    REQUIRE(queue.empty());
    REQUIRE_FALSE(receiver.receive("invalid-address-length"));

    sender.close();
}

TEST_CASE("UDP socket does not report a partial batch send as success", "[network][udp][batch]")
{
    LoopbackReceiver first;
    LoopbackReceiver third;
    REQUIRE(first.valid());
    REQUIRE(third.valid());

    Socket sender;
    REQUIRE(sender.open(AF_INET));

    const std::string firstPayload = "sent-before-error";
    const std::string thirdPayload = "not-sent-after-error";
    BufferQueue queue;
    queue.push(makeDatagram(firstPayload, first.address(), first.addrLen()));
    queue.push(makeDatagram("invalid-address-length", third.address(), 0U));
    queue.push(makeDatagram(thirdPayload, third.address(), third.addrLen()));

    ssize_t bytesWritten = 0;
    REQUIRE_FALSE(sender.write(&queue, &bytesWritten));
    REQUIRE(bytesWritten == -1);
    REQUIRE(queue.empty());
    REQUIRE(first.receive(firstPayload));
    REQUIRE_FALSE(third.receive(thirdPayload));

    sender.close();
}

#if (defined(HAVE_SENDMSG) && !defined(HAVE_SENDMMSG)) || defined(_WIN32)
TEST_CASE("UDP sendmmsg compatibility wrapper returns a datagram count", "[network][udp][batch][compatibility]")
{
    LoopbackReceiver first;
    LoopbackReceiver second;
    REQUIRE(first.valid());
    REQUIRE(second.valid());

    NativeSocket sender = ::socket(AF_INET, SOCK_DGRAM, 0);
    REQUIRE(sender != INVALID_NATIVE_SOCKET);

    const std::string firstPayload = "compatibility-one";
    const std::string secondPayload = "compatibility-two";
    struct iovec chunks[2U] = {};
    struct mmsghdr headers[2U] = {};

    chunks[0U].iov_base = (void*)firstPayload.data();
    chunks[0U].iov_len = firstPayload.size();
    chunks[1U].iov_base = (void*)secondPayload.data();
    chunks[1U].iov_len = secondPayload.size();

    headers[0U].msg_hdr.msg_name = (void*)&first.address();
    headers[0U].msg_hdr.msg_namelen = first.addrLen();
    headers[0U].msg_hdr.msg_iov = &chunks[0U];
    headers[0U].msg_hdr.msg_iovlen = 1U;
    headers[1U].msg_hdr.msg_name = (void*)&second.address();
    headers[1U].msg_hdr.msg_namelen = second.addrLen();
    headers[1U].msg_hdr.msg_iov = &chunks[1U];
    headers[1U].msg_hdr.msg_iovlen = 1U;

    int sent = sendmmsg(sender, headers, 2U, 0);
    REQUIRE(sent == 2);
    REQUIRE(first.receive(firstPayload));
    REQUIRE(second.receive(secondPayload));

    closeNativeSocket(sender);
}
#endif // (defined(HAVE_SENDMSG) && !defined(HAVE_SENDMMSG)) || defined(_WIN32)
