// SPDX-License-Identifier: GPL-2.0-only
/*
 * Digital Voice Modem - Converged FNE Software
 * GPLv2 Open Source. Use is subject to license terms.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 *  Copyright (C) 2025 Bryan Biedenkapp, N2PLL
 *
 */
#include "common/Log.h"
#include "network/FNEPeerConnection.h"

using namespace network;

// ---------------------------------------------------------------------------
//  Public Class Members
// ---------------------------------------------------------------------------

/* Finalizes a instance of the FNEPeerConnection class. */

FNEPeerConnection::~FNEPeerConnection()
{
    std::lock_guard<std::mutex> lock(m_jitterMutex);

    for (auto& pair : m_jitterBuffers) {
        delete pair.second;
    }
    m_jitterBuffers.clear();
}

/* Gets or creates a jitter buffer for the specified stream. */

AdaptiveJitterBuffer* FNEPeerConnection::getOrCreateJitterBuffer(uint64_t streamId)
{
    std::lock_guard<std::mutex> lock(m_jitterMutex);

    if (m_jitterBuffers.find(streamId) == m_jitterBuffers.end()) {
        m_jitterBuffers[streamId] = new AdaptiveJitterBuffer(m_jitterMaxSize, m_jitterMaxWait);
    }

    return m_jitterBuffers[streamId];
}

/* Processes a frame through the jitter buffer for the specified stream. */

bool FNEPeerConnection::processJitterFrame(uint64_t streamId, uint16_t seq, const uint8_t* data, uint32_t length,
    std::vector<BufferedFrame*>& readyFrames)
{
    std::lock_guard<std::mutex> lock(m_jitterMutex);

    if (m_jitterBuffers.find(streamId) == m_jitterBuffers.end()) {
        m_jitterBuffers[streamId] = new AdaptiveJitterBuffer(m_jitterMaxSize, m_jitterMaxWait);
    }

    AdaptiveJitterBuffer* buffer = m_jitterBuffers[streamId];
    if (buffer == nullptr) {
        return false;
    }

    buffer->checkTimeouts(readyFrames);
    return buffer->processFrame(seq, data, length, readyFrames);
}

/* Flushes all buffered frames for the specified stream. */

void FNEPeerConnection::flushJitterBuffer(uint64_t streamId, std::vector<BufferedFrame*>& readyFrames)
{
    std::lock_guard<std::mutex> lock(m_jitterMutex);

    auto it = m_jitterBuffers.find(streamId);
    if (it != m_jitterBuffers.end() && it->second != nullptr) {
        it->second->flush(readyFrames);
    }
}

/* Cleans up jitter buffer for the specified stream. */

void FNEPeerConnection::cleanupJitterBuffer(uint64_t streamId)
{
    std::lock_guard<std::mutex> lock(m_jitterMutex);

    auto it = m_jitterBuffers.find(streamId);
    if (it != m_jitterBuffers.end()) {
        delete it->second;
        m_jitterBuffers.erase(it);
    }
}
