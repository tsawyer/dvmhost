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

namespace
{
    void logJitterEvent(FNEPeerConnection* connection, uint64_t streamId, const JitterBufferEvent& event)
    {
        if (connection == nullptr) {
            return;
        }

        switch (event.type) {
        case JitterBufferEvent::Type::TIMEOUT:
            LogWarning(LOG_MASTER, "FNE-RX-JB PEER %u (%s) stream %u timeout; delivered seq %u after %lluus, missing seq %u",
                connection->id(), connection->identWithQualifier().c_str(), (uint32_t)streamId, (uint32_t)event.seq,
                (unsigned long long)event.age, (uint32_t)event.expectedSeq);
            break;
        case JitterBufferEvent::Type::LATE_DROP:
            LogWarning(LOG_MASTER, "FNE-RX-JB PEER %u (%s) stream %u dropped late seq %u, expected seq %u",
                connection->id(), connection->identWithQualifier().c_str(), (uint32_t)streamId,
                (uint32_t)event.seq, (uint32_t)event.expectedSeq);
            break;
        case JitterBufferEvent::Type::DUPLICATE_DROP:
            LogWarning(LOG_MASTER, "FNE-RX-JB PEER %u (%s) stream %u dropped duplicate buffered seq %u, expected seq %u",
                connection->id(), connection->identWithQualifier().c_str(), (uint32_t)streamId,
                (uint32_t)event.seq, (uint32_t)event.expectedSeq);
            break;
        case JitterBufferEvent::Type::OVERFLOW_DROP:
            LogWarning(LOG_MASTER, "FNE-RX-JB PEER %u (%s) stream %u overflow; dropped seq %u, expected seq %u",
                connection->id(), connection->identWithQualifier().c_str(), (uint32_t)streamId,
                (uint32_t)event.seq, (uint32_t)event.expectedSeq);
            break;
        case JitterBufferEvent::Type::STREAM_RESET:
            LogWarning(LOG_MASTER, "FNE-RX-JB PEER %u (%s) stream %u reset; got seq %u, expected seq %u",
                connection->id(), connection->identWithQualifier().c_str(), (uint32_t)streamId,
                (uint32_t)event.seq, (uint32_t)event.expectedSeq);
            break;
        }
    }

    void logJitterStats(FNEPeerConnection* connection, uint64_t streamId, AdaptiveJitterBuffer* buffer)
    {
        if (connection == nullptr || buffer == nullptr) {
            return;
        }

        uint64_t totalFrames = 0ULL;
        uint64_t reorderedFrames = 0ULL;
        uint64_t recoveredFrames = 0ULL;
        uint64_t droppedFrames = 0ULL;
        uint64_t timedOutFrames = 0ULL;
        uint64_t flushedFrames = 0ULL;
        buffer->getStatistics(totalFrames, reorderedFrames, recoveredFrames, droppedFrames, timedOutFrames, flushedFrames);

        if (reorderedFrames == 0ULL && recoveredFrames == 0ULL && droppedFrames == 0ULL &&
            timedOutFrames == 0ULL && flushedFrames == 0ULL) {
            return;
        }

        LogInfoEx(LOG_MASTER, "FNE-RX-JB PEER %u (%s) stream %u stats: total=%llu, reordered=%llu, recovered=%llu, timedOut=%llu, dropped=%llu, flushed=%llu",
            connection->id(), connection->identWithQualifier().c_str(), (uint32_t)streamId,
            (unsigned long long)totalFrames, (unsigned long long)reorderedFrames, (unsigned long long)recoveredFrames,
            (unsigned long long)timedOutFrames, (unsigned long long)droppedFrames, (unsigned long long)flushedFrames);
    }
}

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

    std::vector<JitterBufferEvent> events;
    buffer->checkTimeouts(readyFrames, 0ULL, &events);
    bool ret = buffer->processFrame(seq, data, length, readyFrames, &events);
    for (const JitterBufferEvent& event : events) {
        logJitterEvent(this, streamId, event);
    }

    return ret;
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
        logJitterStats(this, streamId, it->second);
        delete it->second;
        m_jitterBuffers.erase(it);
    }
}
