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
#include "network/MetadataNetwork.h"
#include "fne/ActivityLog.h"
#include "HostFNE.h"

using namespace network;

// ---------------------------------------------------------------------------
//  Public Class Members
// ---------------------------------------------------------------------------

/* Handles NET_FUNC::TRANSFER packets. */

void MetadataNetwork::PacketHandler::transfer(TrafficNetwork* network, MetadataNetwork* mdNetwork, NetPacketRequest* req, uint32_t peerId, uint32_t ssrc, uint32_t streamId)
{
    (void)mdNetwork;
    (void)ssrc;
    (void)streamId;

    // resolve peer ID (used for Activity Log and Status Transfer)
    bool validPeerId = false;
    uint32_t pktPeerId = 0U;
    if (peerId > 0 && (network->m_peers.find(peerId) != network->m_peers.end())) {
        validPeerId = true;
        pktPeerId = peerId;
    } else {
        if (peerId > 0) {
            // this could be a replica transfer -- in which case, we need to check the SSRC of the packet not the peer ID
            if (network->m_peers.find(req->rtpHeader.getSSRC()) != network->m_peers.end()) {
                FNEPeerConnection* connection = network->m_peers[req->rtpHeader.getSSRC()];
                if (connection != nullptr) {
                    if (connection->peerClass() == PEER_CONN_CLASS_NEIGHBOR && connection->isReplica()) {
                        validPeerId = true;
                        pktPeerId = req->rtpHeader.getSSRC();
                    }
                }
            }
        }
    }

    // process incoming message subfunction opcodes
    switch (req->fneHeader.getSubFunction()) {
    case NET_SUBFUNC::TRANSFER_SUBFUNC_ACTIVITY:        // Peer Activity Log Transfer
        {
            if (network->m_allowActivityTransfer) {
                if (pktPeerId > 0 && validPeerId) {
                    FNEPeerConnection* connection = network->m_peers[pktPeerId];
                    if (connection != nullptr) {
                        std::string ip = udp::Socket::address(req->address);

                        // validate peer (simple validation really)
                        if (connection->connected() && connection->address() == ip) {
                            DECLARE_UINT8_ARRAY(rawPayload, req->length - 11U);
                            ::memcpy(rawPayload, req->buffer + 11U, req->length - 11U);
                            std::string payload(rawPayload, rawPayload + (req->length - 11U));

                            ::ActivityLog("%.9u (%8s) %s", pktPeerId, connection->identWithQualifier().c_str(), payload.c_str());

                            // report activity log to metrics
                            TrafficNetwork::MetricsLogging::logActivity(network, pktPeerId, connection->identity(), payload);

                            // repeat traffic to the connected SysView peers
                            if (network->m_peers.size() > 0U) {
                                for (auto peer : network->m_peers) {
                                    if (peer.second != nullptr) {
                                        if (peer.second->peerClass() == PEER_CONN_CLASS_SYSVIEW) {
                                            sockaddr_storage addr = peer.second->socketStorage();
                                            uint32_t addrLen = peer.second->sockStorageLen();

                                            network->m_frameQueue->write(req->buffer, req->length, network->createStreamId(), pktPeerId, network->m_peerId,
                                                { NET_FUNC::TRANSFER, NET_SUBFUNC::TRANSFER_SUBFUNC_ACTIVITY }, RTP_END_OF_CALL_SEQ, addr, addrLen);
                                        }
                                    } else {
                                        continue;
                                    }
                                }
                            }

                            // attempt to repeat traffic to replica masters
                            if (network->m_host->m_peerNetworks.size() > 0) {
                                for (auto peer : network->m_host->m_peerNetworks) {
                                    if (peer.second != nullptr) {
                                        if (peer.second->isEnabled() && peer.second->isReplica()) {
                                            peer.second->writeMaster({ NET_FUNC::TRANSFER, NET_SUBFUNC::TRANSFER_SUBFUNC_ACTIVITY },
                                                req->buffer, req->length, RTP_END_OF_CALL_SEQ, 0U, true, pktPeerId);
                                        }
                                    }
                                }
                            }
                        }
                        else {
                            network->writePeerNAK(pktPeerId, network->createStreamId(), TAG_TRANSFER_ACT_LOG, NET_CONN_NAK_FNE_UNAUTHORIZED);
                        }
                    }
                }
            }
        }
        break;

    case NET_SUBFUNC::TRANSFER_SUBFUNC_DIAG:            // Peer Diagnostic Log Transfer
        {
            if (network->m_allowDiagnosticTransfer) {
                if (peerId > 0 && (network->m_peers.find(peerId) != network->m_peers.end())) {
                    FNEPeerConnection* connection = network->m_peers[peerId];
                    if (connection != nullptr) {
                        std::string ip = udp::Socket::address(req->address);

                        // validate peer (simple validation really)
                        if (connection->connected() && connection->address() == ip) {
                            DECLARE_UINT8_ARRAY(rawPayload, req->length - 11U);
                            ::memcpy(rawPayload, req->buffer + 11U, req->length - 11U);
                            std::string payload(rawPayload, rawPayload + (req->length - 11U));

                            bool currState = g_disableTimeDisplay;
                            g_disableTimeDisplay = true;
                            ::Log(9999U, {nullptr, nullptr, 0U, nullptr}, "%.9u (%8s) %s", peerId, connection->identWithQualifier().c_str(), payload.c_str());
                            g_disableTimeDisplay = currState;

                            // report diagnostic log to metrics
                            TrafficNetwork::MetricsLogging::logDiag(network, peerId, connection->identity(), payload);
                        }
                        else {
                            network->writePeerNAK(peerId, network->createStreamId(), TAG_TRANSFER_DIAG_LOG, NET_CONN_NAK_FNE_UNAUTHORIZED);
                        }
                    }
                }
            }
        }
        break;

    case NET_SUBFUNC::TRANSFER_SUBFUNC_STATUS:          // Peer Status Transfer
        {
            if (pktPeerId > 0 && validPeerId) {
                FNEPeerConnection* connection = network->m_peers[pktPeerId];
                if (connection != nullptr) {
                    std::string ip = udp::Socket::address(req->address);

                    // validate peer (simple validation really)
                    if (connection->connected() && connection->address() == ip) {
                        if (network->m_peers.size() > 0U) {
                            // attempt to repeat status traffic to SysView clients
                            for (auto peer : network->m_peers) {
                                if (peer.second != nullptr) {
                                    if (peer.second->peerClass() == PEER_CONN_CLASS_SYSVIEW) {
                                        sockaddr_storage addr = peer.second->socketStorage();
                                        uint32_t addrLen = peer.second->sockStorageLen();

                                        if (network->m_debug) {
                                            LogDebug(LOG_DIAG, "SysView, srcPeer = %u, dstPeer = %u, peer status message, len = %u",
                                                pktPeerId, peer.first, req->length);
                                        }
                                        network->m_frameQueue->write(req->buffer, req->length, network->createStreamId(), pktPeerId, network->m_peerId,
                                            { NET_FUNC::TRANSFER, NET_SUBFUNC::TRANSFER_SUBFUNC_STATUS }, RTP_END_OF_CALL_SEQ, addr, addrLen);
                                    }
                                } else {
                                    continue;
                                }
                            }

                            // attempt to repeat status traffic to replica masters
                            if (network->m_host->m_peerNetworks.size() > 0) {
                                for (auto peer : network->m_host->m_peerNetworks) {
                                    if (peer.second != nullptr) {
                                        if (peer.second->isEnabled() && peer.second->isReplica()) {
                                            peer.second->writeMaster({ NET_FUNC::TRANSFER, NET_SUBFUNC::TRANSFER_SUBFUNC_STATUS },
                                                req->buffer, req->length, RTP_END_OF_CALL_SEQ, 0U, true, pktPeerId);
                                        }
                                    }
                                }
                            }
                        }
                    }
                    else {
                        network->writePeerNAK(pktPeerId, network->createStreamId(), TAG_TRANSFER_STATUS, NET_CONN_NAK_FNE_UNAUTHORIZED);
                    }
                }
            }
        }
        break;

    default:
        {
            LogWarning(LOG_MASTER, "PEER %u, unknown/unsupported transfer opcode %u", peerId, req->fneHeader.getSubFunction());
            if (network->m_debug)
                Utils::dump("Unknown/unsupported transfer opcode from the peer", req->buffer, req->length);
        }
        break;
    }
}
