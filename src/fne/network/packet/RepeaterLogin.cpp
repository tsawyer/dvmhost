// SPDX-License-Identifier: GPL-2.0-only
/*
 * Digital Voice Modem - Converged FNE Software
 * GPLv2 Open Source. Use is subject to license terms.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 *  Copyright (C) 2023-2026 Bryan Biedenkapp, N2PLL
 *
 */
#include "fne/Defines.h"
#include "common/Log.h"
#include "network/TrafficNetwork.h"
#include "fne/ActivityLog.h"
#include "HostFNE.h"

using namespace network;

// ---------------------------------------------------------------------------
//  Public Class Members
// ---------------------------------------------------------------------------

/* Handles NET_FUNC::RPTL packets. */

void TrafficNetwork::PacketHandler::repeaterLogin(TrafficNetwork* network, NetPacketRequest* req, uint32_t peerId, uint32_t ssrc, uint32_t streamId, uint64_t now)
{
    std::lock_guard<std::mutex> peerGuard(TrafficNetwork::getPeerStateLock(peerId));

    if (peerId > 0 && (network->m_peers.find(peerId) == network->m_peers.end())) {
        if (network->m_peers.size() >= MAX_HARD_CONN_CAP) {
            LogError(LOG_MASTER, "PEER %u attempted to connect with no more connections available, currConnections = %u", peerId, network->m_peers.size());
            network->writePeerNAK(peerId, TAG_REPEATER_LOGIN, NET_CONN_NAK_FNE_MAX_CONN, req->address, req->addrLen);
            return;
        }

        if (network->m_softConnLimit > 0U && network->m_peers.size() >= network->m_softConnLimit) {
            LogError(LOG_MASTER, "PEER %u attempted to connect with no more connections available, maxConnections = %u, currConnections = %u", peerId, network->m_softConnLimit, network->m_peers.size());
            network->writePeerNAK(peerId, TAG_REPEATER_LOGIN, NET_CONN_NAK_FNE_MAX_CONN, req->address, req->addrLen);
            return;
        }

        FNEPeerConnection* connection = new FNEPeerConnection(peerId, req->address, req->addrLen);
        connection->lastPing(now);

        network->applyJitterBufferConfig(peerId, connection);
        network->setupRepeaterLogin(peerId, streamId, connection);

        // check if the peer is in the peer ACL list
        if (network->m_peerListLookup->getACL()) {
            if (network->m_peerListLookup->isPeerListEmpty()) {
                LogWarning(LOG_MASTER, "Peer List ACL enabled, but we have an empty peer list? Passing all peers.");
            }

            if (!network->m_peerListLookup->isPeerAllowed(peerId) && !network->m_peerListLookup->isPeerListEmpty()) {
                LogWarning(LOG_MASTER, "PEER %u RPTL, failed peer ACL check", peerId);

                network->writePeerNAK(peerId, TAG_REPEATER_LOGIN, NET_CONN_NAK_PEER_ACL, req->address, req->addrLen);
                network->disconnectPeer(peerId, connection);
            }
        }
    }
    else {
        // check if the peer is in our peer list -- if he is, and he isn't in a running state, reset
        // the login sequence
        if (peerId > 0 && (network->m_peers.find(peerId) != network->m_peers.end())) {
            FNEPeerConnection* connection = network->m_peers[peerId];
            if (connection != nullptr) {
                if (connection->connectionState() == NET_STAT_RUNNING) {
                    LogInfoEx(LOG_MASTER, "PEER %u (%s) resetting peer connection, connectionState = %u", peerId, connection->identWithQualifier().c_str(),
                        connection->connectionState());
                    network->disconnectPeer(peerId, connection);

                    connection = new FNEPeerConnection(peerId, req->address, req->addrLen);
                    connection->lastPing(now);

                    network->applyJitterBufferConfig(peerId, connection);
                    network->erasePeerAffiliations(peerId);
                    network->setupRepeaterLogin(peerId, streamId, connection);

                    // check if the peer is in the peer ACL list
                    if (network->m_peerListLookup->getACL()) {
                        if (network->m_peerListLookup->isPeerListEmpty()) {
                            LogWarning(LOG_MASTER, "Peer List ACL enabled, but we have an empty peer list? Passing all peers.");
                        }

                        if (!network->m_peerListLookup->isPeerAllowed(peerId) && !network->m_peerListLookup->isPeerListEmpty()) {
                            LogWarning(LOG_MASTER, "PEER %u RPTL, failed peer ACL check", peerId);

                            network->writePeerNAK(peerId, TAG_REPEATER_LOGIN, NET_CONN_NAK_PEER_ACL, req->address, req->addrLen);
                            network->disconnectPeer(peerId, connection);
                        }
                    }
                } else {
                    // perform source address validation
                    if (connection->address() != udp::Socket::address(req->address)) {
                        LogError(LOG_MASTER, "PEER %u RPTL NAK, IP address mismatch on RPTL attempt while not running, old = %s:%u, new = %s:%u, connectionState = %u", peerId,
                            connection->address().c_str(), connection->port(), udp::Socket::address(req->address).c_str(), udp::Socket::port(req->address), connection->connectionState());

                        network->writePeerNAK(peerId, TAG_REPEATER_LOGIN, NET_CONN_NAK_FNE_UNAUTHORIZED, req->address, req->addrLen);
                        return;
                    }

                    network->writePeerNAK(peerId, TAG_REPEATER_LOGIN, NET_CONN_NAK_BAD_CONN_STATE, req->address, req->addrLen);

                    LogWarning(LOG_MASTER, "PEER %u (%s) RPTL NAK, bad connection state, connectionState = %u", peerId, connection->identWithQualifier().c_str(),
                        connection->connectionState());
                    network->disconnectPeer(peerId, connection);
                }
            } else {
                network->writePeerNAK(peerId, TAG_REPEATER_LOGIN, NET_CONN_NAK_BAD_CONN_STATE, req->address, req->addrLen);
                network->erasePeer(peerId);
                LogWarning(LOG_MASTER, "PEER %u RPTL NAK, having no connection", peerId);
            }
        }
    }
}
