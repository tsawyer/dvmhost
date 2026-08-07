// SPDX-License-Identifier: GPL-2.0-only
/*
 * Digital Voice Modem - Converged FNE Software
 * GPLv2 Open Source. Use is subject to license terms.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 *  Copyright (C) 2026 Bryan Biedenkapp, N2PLL
 *
 */
#include "fne/Defines.h"
#include "common/Log.h"
#include "common/Thread.h"
#include "network/TrafficNetwork.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace network;

// ---------------------------------------------------------------------------
//  Constants
// ---------------------------------------------------------------------------

#define SQLITE_BATCH_SIZE 256U
#define SQLITE_MAX_QUEUE_DEPTH 32768U
#define SQLITE_BATCH_WAIT_MS 10U

// ---------------------------------------------------------------------------
//  Static Class Members
// ---------------------------------------------------------------------------

int32_t TrafficNetwork::MetricsLogging::s_totalActiveCalls = 0;
uint64_t TrafficNetwork::MetricsLogging::s_totalCallsProcessed = 0U;
uint64_t TrafficNetwork::MetricsLogging::s_totalCallCollisions = 0U;

namespace {
    static const char* SQLITE_COUNTER_CALLS_PROCESSED = "total_calls_processed";
    static const char* SQLITE_COUNTER_CALL_COLLISIONS = "total_call_collisions";

    static std::mutex g_totalsMutex;

    /**
     * @brief Get the current time in nanoseconds since the epoch.
     * @return The current time in nanoseconds since the epoch.
     */
    static inline uint64_t nowNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    /**
     * @brief Types of metric events.
     */
    enum class MetricEventType {
        ACTIVITY,
        DIAG,
        CALL_EVENT,
        CALL_ERROR_EVENT,
        TSBK_EVENT,
        CSBK_EVENT,
        COUNTER_EVENT
    };

    // ---------------------------------------------------------------------------
    //  Structure Declaration
    // ---------------------------------------------------------------------------

    /**
     * @brief Represents a metric event with various attributes.
     */
    struct MetricEvent {
        MetricEventType type;
        uint64_t tsNs;
        uint32_t peerId;
        uint32_t streamId;
        uint32_t srcId;
        uint32_t dstId;
        uint64_t durationMs;
        uint8_t slotNo;
        bool hasSlot;

        std::string mode;
        std::string identity;
        std::string message;
        std::string raw;
        std::string lco;
        std::string rawDescription;
        std::string counterKey;
        uint64_t counterValue;

        /**
         * @brief Default constructor for MetricEvent. Initializes all members to default values.
         */
        MetricEvent() :
            type(MetricEventType::ACTIVITY),
            tsNs(0U),
            peerId(0U),
            streamId(0U),
            srcId(0U),
            dstId(0U),
            durationMs(0U),
            slotNo(0U),
            hasSlot(false),
            counterKey(),
            counterValue(0U)
        {
            /* stub */
        }
    };

    // ---------------------------------------------------------------------------
    //  Structure Declaration
    // ---------------------------------------------------------------------------

    /**
     * @brief Holds prepared SQLite statements for different metric event types.
     */
    struct SQLiteStatements {
        sqlite3_stmt* activity;
        sqlite3_stmt* diag;
        sqlite3_stmt* callEvent;
        sqlite3_stmt* callErrorEvent;
        sqlite3_stmt* tsbkEvent;
        sqlite3_stmt* csbkEvent;
        sqlite3_stmt* counterUpsert;

        /**
         * @brief Default constructor for SQLiteStatements. Initializes all statement pointers to nullptr.
         */
        SQLiteStatements() :
            activity(nullptr),
            diag(nullptr),
            callEvent(nullptr),
            callErrorEvent(nullptr),
            tsbkEvent(nullptr),
            csbkEvent(nullptr),
            counterUpsert(nullptr)
        {
            /* stub */
        }
    };

    // ---------------------------------------------------------------------------
    //  Structure Declaration
    // ---------------------------------------------------------------------------

    /**
     * @brief Holds the state for the SQLite writer, including the queue, worker thread, and prepared statements.
     */
    struct SQLiteWriterState {
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<MetricEvent> queue;
        thread_t workerThread;
        bool workerStarted;
        sqlite3* db;
        SQLiteStatements statements;
        bool stopRequested;
        uint64_t droppedCount;

        /**
         * @brief Default constructor for SQLiteWriterState. Initializes all members to default values.
         */
        SQLiteWriterState() :
            mutex(),
            cv(),
            queue(),
            workerThread(),
            workerStarted(false),
            db(nullptr),
            statements(),
            stopRequested(false),
            droppedCount(0U)
        {
            /* stub */
        }
    };

    static std::mutex g_sqliteWriterStatesMutex;
    static std::unordered_map<TrafficNetwork*, std::unique_ptr<SQLiteWriterState>> g_sqliteWriterStates;

    // ---------------------------------------------------------------------------
    //  Global Functions
    // ---------------------------------------------------------------------------

    /**
     * @brief Binds a text value to a SQLite statement at the specified index.
     * @param stmt The SQLite statement.
     * @param idx The index of the parameter to bind.
     * @param value The text value to bind.
     * @return SQLITE_OK if the binding was successful, otherwise an error code.
     */
    static inline int bindText(sqlite3_stmt* stmt, int idx, const std::string& value)
    {
        return sqlite3_bind_text(stmt, idx, value.c_str(), -1, SQLITE_TRANSIENT);
    }

    /**
     * @brief Finalizes all prepared SQLite statements and sets their pointers to nullptr.
     * @param stmts The SQLiteStatements structure containing the prepared statements.
     */
    static void finalizeStatements(SQLiteStatements& stmts)
    {
        if (stmts.activity != nullptr) {
            sqlite3_finalize(stmts.activity);
            stmts.activity = nullptr;
        }
        if (stmts.diag != nullptr) {
            sqlite3_finalize(stmts.diag);
            stmts.diag = nullptr;
        }
        if (stmts.callEvent != nullptr) {
            sqlite3_finalize(stmts.callEvent);
            stmts.callEvent = nullptr;
        }
        if (stmts.callErrorEvent != nullptr) {
            sqlite3_finalize(stmts.callErrorEvent);
            stmts.callErrorEvent = nullptr;
        }
        if (stmts.tsbkEvent != nullptr) {
            sqlite3_finalize(stmts.tsbkEvent);
            stmts.tsbkEvent = nullptr;
        }
        if (stmts.csbkEvent != nullptr) {
            sqlite3_finalize(stmts.csbkEvent);
            stmts.csbkEvent = nullptr;
        }
        if (stmts.counterUpsert != nullptr) {
            sqlite3_finalize(stmts.counterUpsert);
            stmts.counterUpsert = nullptr;
        }
    }

    /**
     * @brief Prepares a single SQLite statement.
     * @param db The SQLite database connection.
     * @param sql The SQL query string.
     * @param stmt Pointer to the SQLite statement to be prepared.
     * @return true if the statement was prepared successfully, false otherwise.
     */
    static bool prepareStatement(sqlite3* db, const char* sql, sqlite3_stmt** stmt)
    {
        if (sqlite3_prepare_v2(db, sql, -1, stmt, nullptr) != SQLITE_OK) {
            return false;
        }

        return true;
    }

    /**
     * @brief Prepares all SQLite statements in the given SQLiteStatements structure.
     * @param db The SQLite database connection.
     * @param stmts The SQLiteStatements structure to be prepared.
     * @return true if all statements were prepared successfully, false otherwise.
     */
    static bool prepareStatements(sqlite3* db, SQLiteStatements& stmts)
    {
        finalizeStatements(stmts);

        if (!prepareStatement(db, "INSERT INTO activity(ts_ns, peer_id, identity, msg) VALUES(?, ?, ?, ?);", &stmts.activity))
            return false;
        if (!prepareStatement(db, "INSERT INTO diag(ts_ns, peer_id, identity, msg) VALUES(?, ?, ?, ?);", &stmts.diag))
            return false;
        if (!prepareStatement(db, "INSERT INTO call_event(ts_ns, peer_id, mode, stream_id, src_id, dst_id, duration_ms, slot) VALUES(?, ?, ?, ?, ?, ?, ?, ?);", &stmts.callEvent))
            return false;
        if (!prepareStatement(db, "INSERT INTO call_error_event(ts_ns, peer_id, stream_id, src_id, dst_id, message, slot) VALUES(?, ?, ?, ?, ?, ?, ?);", &stmts.callErrorEvent))
            return false;
        if (!prepareStatement(db, "INSERT INTO tsbk_event(ts_ns, peer_id, lco, tsbk, raw) VALUES(?, ?, ?, ?, ?);", &stmts.tsbkEvent))
            return false;
        if (!prepareStatement(db, "INSERT INTO csbk_event(ts_ns, peer_id, lco, csbk, raw) VALUES(?, ?, ?, ?, ?);", &stmts.csbkEvent))
            return false;
        if (!prepareStatement(db,
                "INSERT INTO metrics_counters(key, value) VALUES(?, ?) "
                "ON CONFLICT(key) DO UPDATE SET value = excluded.value;",
                &stmts.counterUpsert))
            return false;

        return true;
    }

    /**
     * @brief Executes a SQLite statement, resets it, and clears its bindings.
     * @param stmt The SQLite statement to execute.
     * @return true if the statement executed successfully, false otherwise.
     */
    static bool execStepAndReset(sqlite3_stmt* stmt)
    {
        int rc = sqlite3_step(stmt);
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        return rc == SQLITE_DONE;
    }

    /**
     * @brief Inserts a metric event into the appropriate SQLite statement.
     * @param stmts The SQLiteStatements structure containing the prepared statements.
     * @param e The metric event to insert.
     * @return true if the event was inserted successfully, false otherwise.
     */
    static bool insertEvent(SQLiteStatements& stmts, const MetricEvent& e)
    {
        sqlite3_stmt* stmt = nullptr;

        switch (e.type) {
        case MetricEventType::ACTIVITY:
            stmt = stmts.activity;
            if (stmt == nullptr) 
                return false;
            if (sqlite3_bind_int64(stmt, 1, (sqlite3_int64)e.tsNs) != SQLITE_OK) 
                return false;
            if (sqlite3_bind_int(stmt, 2, (int)e.peerId) != SQLITE_OK) 
                return false;
            if (bindText(stmt, 3, e.identity) != SQLITE_OK) 
                return false;
            if (bindText(stmt, 4, e.message) != SQLITE_OK) 
                return false;
            return execStepAndReset(stmt);

        case MetricEventType::DIAG:
            stmt = stmts.diag;
            if (stmt == nullptr) 
                return false;
            if (sqlite3_bind_int64(stmt, 1, (sqlite3_int64)e.tsNs) != SQLITE_OK) 
                return false;
            if (sqlite3_bind_int(stmt, 2, (int)e.peerId) != SQLITE_OK)
                return false;
            if (bindText(stmt, 3, e.identity) != SQLITE_OK) 
                return false;
            if (bindText(stmt, 4, e.message) != SQLITE_OK) 
                return false;
            return execStepAndReset(stmt);

        case MetricEventType::CALL_EVENT:
            stmt = stmts.callEvent;
            if (stmt == nullptr) return false;
            if (sqlite3_bind_int64(stmt, 1, (sqlite3_int64)e.tsNs) != SQLITE_OK) 
                return false;
            if (sqlite3_bind_int(stmt, 2, (int)e.peerId) != SQLITE_OK) 
                return false;
            if (bindText(stmt, 3, e.mode) != SQLITE_OK) 
                return false;
            if (sqlite3_bind_int(stmt, 4, (int)e.streamId) != SQLITE_OK) 
                return false;
            if (sqlite3_bind_int(stmt, 5, (int)e.srcId) != SQLITE_OK) 
                return false;
            if (sqlite3_bind_int(stmt, 6, (int)e.dstId) != SQLITE_OK) 
                return false;
            if (sqlite3_bind_int64(stmt, 7, (sqlite3_int64)e.durationMs) != SQLITE_OK) 
                return false;
            if (e.hasSlot) {
                if (sqlite3_bind_int(stmt, 8, (int)e.slotNo) != SQLITE_OK) 
                    return false;
            } else {
                if (sqlite3_bind_null(stmt, 8) != SQLITE_OK) 
                    return false;
            }
            return execStepAndReset(stmt);

        case MetricEventType::CALL_ERROR_EVENT:
            stmt = stmts.callErrorEvent;
            if (stmt == nullptr) return false;
            if (sqlite3_bind_int64(stmt, 1, (sqlite3_int64)e.tsNs) != SQLITE_OK) 
                return false;
            if (sqlite3_bind_int(stmt, 2, (int)e.peerId) != SQLITE_OK) 
                return false;
            if (sqlite3_bind_int(stmt, 3, (int)e.streamId) != SQLITE_OK) 
                return false;
            if (sqlite3_bind_int(stmt, 4, (int)e.srcId) != SQLITE_OK) 
                return false;
            if (sqlite3_bind_int(stmt, 5, (int)e.dstId) != SQLITE_OK) 
                return false;
            if (bindText(stmt, 6, e.message) != SQLITE_OK) 
                return false;
            if (e.hasSlot) {
                if (sqlite3_bind_int(stmt, 7, (int)e.slotNo) != SQLITE_OK) 
                    return false;
            } else {
                if (sqlite3_bind_null(stmt, 7) != SQLITE_OK) 
                    return false;
            }
            return execStepAndReset(stmt);

        case MetricEventType::TSBK_EVENT:
            stmt = stmts.tsbkEvent;
            if (stmt == nullptr) return false;
            if (sqlite3_bind_int64(stmt, 1, (sqlite3_int64)e.tsNs) != SQLITE_OK) 
                return false;
            if (sqlite3_bind_int(stmt, 2, (int)e.peerId) != SQLITE_OK) 
                return false;
            if (bindText(stmt, 3, e.lco) != SQLITE_OK) 
                return false;
            if (bindText(stmt, 4, e.rawDescription) != SQLITE_OK) 
                return false;
            if (bindText(stmt, 5, e.raw) != SQLITE_OK) 
                return false;
            return execStepAndReset(stmt);

        case MetricEventType::CSBK_EVENT:
            stmt = stmts.csbkEvent;
            if (stmt == nullptr) return false;
            if (sqlite3_bind_int64(stmt, 1, (sqlite3_int64)e.tsNs) != SQLITE_OK) 
                return false;
            if (sqlite3_bind_int(stmt, 2, (int)e.peerId) != SQLITE_OK) 
                return false;
            if (bindText(stmt, 3, e.lco) != SQLITE_OK) 
                return false;
            if (bindText(stmt, 4, e.rawDescription) != SQLITE_OK) 
                return false;
            if (bindText(stmt, 5, e.raw) != SQLITE_OK) 
                return false;
            return execStepAndReset(stmt);

        case MetricEventType::COUNTER_EVENT:
            stmt = stmts.counterUpsert;
            if (stmt == nullptr)
                return false;
            if (bindText(stmt, 1, e.counterKey) != SQLITE_OK)
                return false;
            if (sqlite3_bind_int64(stmt, 2, (sqlite3_int64)e.counterValue) != SQLITE_OK)
                return false;
            return execStepAndReset(stmt);
        }

        return false;
    }

    /**
     * @brief Enqueues a metric event to be written to the SQLite database by the writer thread.
     * @param network Pointer to the TrafficNetwork instance.
     * @param event The metric event to enqueue.
     */
    static void enqueueSQLiteMetric(TrafficNetwork* network, MetricEvent&& event)
    {
        if (network == nullptr)
            return;

        std::unique_lock<std::mutex> stateLock(g_sqliteWriterStatesMutex);
        auto it = g_sqliteWriterStates.find(network);
        if (it == g_sqliteWriterStates.end() || it->second == nullptr) {
            return;
        }

        SQLiteWriterState* state = it->second.get();
        stateLock.unlock();

        std::unique_lock<std::mutex> lock(state->mutex);
        if (state->queue.size() >= SQLITE_MAX_QUEUE_DEPTH) {
            state->droppedCount++;
            if ((state->droppedCount % 1000U) == 1U) {
                LogWarning(LOG_MASTER, "SQLite metrics queue full (%zu), dropped %llu events",
                    state->queue.size(), (unsigned long long)state->droppedCount);
            }
            return;
        }

        state->queue.push_back(std::move(event));
        lock.unlock();
        state->cv.notify_one();
    }

    /**
     * @brief Entry point for the SQLite writer thread.
     * @param arg Pointer to the thread object containing the SQLite writer state.
     * @return Always returns nullptr.
     */
    static void* sqliteWriterThreadMain(void* arg)
    {
        thread_t* th = (thread_t*)arg;
        if (th == nullptr) {
            return nullptr;
        }

        SQLiteWriterState* state = (SQLiteWriterState*)th->obj;

        if (state == nullptr || state->db == nullptr)
            return nullptr;

        sqlite3* db = state->db;

        std::vector<MetricEvent> batch;
        batch.reserve(SQLITE_BATCH_SIZE);

        while (true) {
            // scope is intentional
            {
                std::unique_lock<std::mutex> lock(state->mutex);
                state->cv.wait_for(lock, std::chrono::milliseconds(SQLITE_BATCH_WAIT_MS), [&]() {
                    return state->stopRequested || !state->queue.empty();
                });

                while (!state->queue.empty() && batch.size() < SQLITE_BATCH_SIZE) {
                    batch.push_back(std::move(state->queue.front()));
                    state->queue.pop_front();
                }

                if (state->stopRequested && batch.empty() && state->queue.empty()) {
                    break;
                }
            }

            if (batch.empty()) {
                continue;
            }

            char* errMsg = nullptr;
            if (sqlite3_exec(db, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, &errMsg) != SQLITE_OK) {
                LogError(LOG_MASTER, "SQLite metrics begin transaction failed: %s", (errMsg != nullptr) ? errMsg : "unknown error");
                if (errMsg != nullptr) {
                    sqlite3_free(errMsg);
                }
                batch.clear();
                continue;
            }

            bool ok = true;
            for (const auto& event : batch) {
                if (!insertEvent(state->statements, event)) {
                    ok = false;
                    LogError(LOG_MASTER, "SQLite metrics insert failed: %s", sqlite3_errmsg(db));
                    break;
                }
            }

            if (ok) {
                if (sqlite3_exec(db, "COMMIT;", nullptr, nullptr, &errMsg) != SQLITE_OK) {
                    LogError(LOG_MASTER, "SQLite metrics commit failed: %s", (errMsg != nullptr) ? errMsg : "unknown error");
                    if (errMsg != nullptr) {
                        sqlite3_free(errMsg);
                    }
                    sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
                }
            } else {
                sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
            }

            batch.clear();
        }

        return nullptr;
    }
}

// ---------------------------------------------------------------------------
//  Public Class Members
// ---------------------------------------------------------------------------

/* Initializes metric sinks for the specified TrafficNetwork instance. */

void TrafficNetwork::MetricsLogging::initialize(TrafficNetwork* network)
{
    if (network == nullptr)
        return;
    if (!network->m_enableMetrics)
        return;

    if (network->m_enableSQLite) {
        if (network->m_sqliteDB == nullptr) {
            if (sqlite3_open(network->m_sqliteDBFile.c_str(), &network->m_sqliteDB) != SQLITE_OK) {
                LogError(LOG_MASTER, "Failed to open SQLite metrics database file: %s", network->m_sqliteDBFile.c_str());
                network->m_enableSQLite = false;
                if (network->m_sqliteDB != nullptr) {
                    sqlite3_close(network->m_sqliteDB);
                    network->m_sqliteDB = nullptr;
                }
                return;
            }
        }

        if (isSQLiteBlank(network)) {
            initializeSQLite(network);
        }

        if (network->m_sqliteDB != nullptr) {
            if (!ensureSQLiteCounterTable(network)) {
                sqlite3_close(network->m_sqliteDB);
                network->m_sqliteDB = nullptr;
                network->m_enableSQLite = false;
                return;
            }

            loadSQLiteCounters(network);
        }

        if (network->m_sqliteDB != nullptr) {
            std::unique_lock<std::mutex> lock(g_sqliteWriterStatesMutex);
            auto it = g_sqliteWriterStates.find(network);
            if (it == g_sqliteWriterStates.end()) {
                auto state = std::unique_ptr<SQLiteWriterState>(new SQLiteWriterState());
                if (!prepareStatements(network->m_sqliteDB, state->statements)) {
                    LogError(LOG_MASTER, "Failed to prepare SQLite metrics statements: %s", sqlite3_errmsg(network->m_sqliteDB));
                    finalizeStatements(state->statements);
                    sqlite3_close(network->m_sqliteDB);
                    network->m_sqliteDB = nullptr;
                    network->m_enableSQLite = false;
                    return;
                }

                state->db = network->m_sqliteDB;
                if (!Thread::runAsThread(state.get(), sqliteWriterThreadMain, &state->workerThread)) {
                    LogError(LOG_MASTER, "Failed to start SQLite metrics writer thread");
                    finalizeStatements(state->statements);
                    sqlite3_close(network->m_sqliteDB);
                    network->m_sqliteDB = nullptr;
                    network->m_enableSQLite = false;
                    return;
                }
                state->workerStarted = true;
                g_sqliteWriterStates.emplace(network, std::move(state));
            }
        }
    }
}

/* Finalizes metric sinks for the specified TrafficNetwork instance. */

void TrafficNetwork::MetricsLogging::finalize(TrafficNetwork* network)
{
    if (network == nullptr)
        return;

    std::unique_ptr<SQLiteWriterState> state;
    {
        std::unique_lock<std::mutex> lock(g_sqliteWriterStatesMutex);
        auto it = g_sqliteWriterStates.find(network);
        if (it != g_sqliteWriterStates.end()) {
            state = std::move(it->second);
            g_sqliteWriterStates.erase(it);
        }
    }

    if (state != nullptr) {
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->stopRequested = true;
        }
        state->cv.notify_one();

        if (state->workerStarted) {
#if defined(_WIN32)
            ::WaitForSingleObject(state->workerThread.thread, INFINITE);
            ::CloseHandle(state->workerThread.thread);
#else
            ::pthread_join(state->workerThread.thread, nullptr);
#endif
            state->workerStarted = false;
        }

        finalizeStatements(state->statements);
    }

    if (network->m_sqliteDB != nullptr) {
        sqlite3_close(network->m_sqliteDB);
        network->m_sqliteDB = nullptr;
    }
}

/* Increments the active call counter. */

void TrafficNetwork::MetricsLogging::incrementActiveCalls(TrafficNetwork* network)
{
    (void)network;

    std::lock_guard<std::mutex> lock(g_totalsMutex);
    s_totalActiveCalls++;
}

/* Decrements the active call counter with floor at zero. */

void TrafficNetwork::MetricsLogging::decrementActiveCalls(TrafficNetwork* network)
{
    (void)network;

    std::lock_guard<std::mutex> lock(g_totalsMutex);
    s_totalActiveCalls--;
    if (s_totalActiveCalls < 0) {
        s_totalActiveCalls = 0;
    }
}

/* Resets the active call counter to zero. */

void TrafficNetwork::MetricsLogging::resetActiveCalls()
{
    std::lock_guard<std::mutex> lock(g_totalsMutex);
    s_totalActiveCalls = 0;
}

/* Increments the total processed calls counter. */

void TrafficNetwork::MetricsLogging::incrementCallsProcessed(TrafficNetwork* network)
{
    uint64_t value = 0U;
    {
        std::lock_guard<std::mutex> lock(g_totalsMutex);
        s_totalCallsProcessed++;
        value = s_totalCallsProcessed;
    }

    persistSQLiteCounter(network, SQLITE_COUNTER_CALLS_PROCESSED, value);
}

/* Increments the total call collisions counter. */

void TrafficNetwork::MetricsLogging::incrementCallCollisions(TrafficNetwork* network)
{
    uint64_t value = 0U;
    {
        std::lock_guard<std::mutex> lock(g_totalsMutex);
        s_totalCallCollisions++;
        value = s_totalCallCollisions;
    }

    persistSQLiteCounter(network, SQLITE_COUNTER_CALL_COLLISIONS, value);
}

/* Resets the total processed calls counter to zero. */

void TrafficNetwork::MetricsLogging::resetCallsProcessed(TrafficNetwork* network)
{
    {
        std::lock_guard<std::mutex> lock(g_totalsMutex);
        s_totalCallsProcessed = 0U;
    }

    persistSQLiteCounter(network, SQLITE_COUNTER_CALLS_PROCESSED, 0U);
}

/* Resets the total call collisions counter to zero. */

void TrafficNetwork::MetricsLogging::resetCallCollisions(TrafficNetwork* network)
{
    {
        std::lock_guard<std::mutex> lock(g_totalsMutex);
        s_totalCallCollisions = 0U;
    }

    persistSQLiteCounter(network, SQLITE_COUNTER_CALL_COLLISIONS, 0U);
}

/* Gets the active call counter. */

int32_t TrafficNetwork::MetricsLogging::getTotalActiveCalls()
{
    return s_totalActiveCalls;
}

/* Gets the total processed calls counter. */

uint64_t TrafficNetwork::MetricsLogging::getTotalCallsProcessed()
{
    return s_totalCallsProcessed;
}

/* Gets the total call collisions counter. */

uint64_t TrafficNetwork::MetricsLogging::getTotalCallCollisions()
{
    return s_totalCallCollisions;
}

/* Logs a activity transfer event. */

void TrafficNetwork::MetricsLogging::logActivity(TrafficNetwork* network, uint32_t peerId, const std::string& identity, const std::string& msg)
{
    if (network == nullptr || !network->m_enableMetrics)
        return;

    uint64_t ts = nowNs();

    if (network->m_enableInfluxDB) {
        influxdb::QueryBuilder()
            .meas("activity")
                .tag("peerId", std::to_string(peerId))
                    .field("identity", identity)
                    .field("msg", msg)
                .timestamp(ts)
            .requestAsync(network->m_influxServer);
    }

    if (network->m_enableSQLite && network->m_sqliteDB != nullptr) {
        MetricEvent e;
        e.type = MetricEventType::ACTIVITY;
        e.tsNs = ts;
        e.peerId = peerId;
        e.identity = identity;
        e.message = msg;
        enqueueSQLiteMetric(network, std::move(e));
    }
}

/* Logs a diagnostic transfer event. */

void TrafficNetwork::MetricsLogging::logDiag(TrafficNetwork* network, uint32_t peerId, const std::string& identity, const std::string& msg)
{
    if (network == nullptr || !network->m_enableMetrics)
        return;

    uint64_t ts = nowNs();

    if (network->m_enableInfluxDB) {
        influxdb::QueryBuilder()
            .meas("diag")
                .tag("peerId", std::to_string(peerId))
                    .field("identity", identity)
                    .field("msg", msg)
                .timestamp(ts)
            .requestAsync(network->m_influxServer);
    }

    if (network->m_enableSQLite && network->m_sqliteDB != nullptr) {
        MetricEvent e;
        e.type = MetricEventType::DIAG;
        e.tsNs = ts;
        e.peerId = peerId;
        e.identity = identity;
        e.message = msg;
        enqueueSQLiteMetric(network, std::move(e));
    }
}

/* Logs a call event. */

void TrafficNetwork::MetricsLogging::logCallEvent(TrafficNetwork* network, const char* mode, uint32_t peerId, uint32_t streamId,
    uint32_t srcId, uint32_t dstId, uint64_t durationMs)
{
    if (network == nullptr || !network->m_enableMetrics)
        return;

    uint64_t ts = nowNs();

    if (network->m_enableInfluxDB) {
        influxdb::QueryBuilder()
            .meas("call_event")
                .tag("peerId", std::to_string(peerId))
                .tag("mode", mode)
                .tag("streamId", std::to_string(streamId))
                .tag("srcId", std::to_string(srcId))
                .tag("dstId", std::to_string(dstId))
                    .field("duration", durationMs)
                .timestamp(ts)
            .requestAsync(network->m_influxServer);
    }

    if (network->m_enableSQLite && network->m_sqliteDB != nullptr) {
        MetricEvent e;
        e.type = MetricEventType::CALL_EVENT;
        e.tsNs = ts;
        e.peerId = peerId;
        e.streamId = streamId;
        e.srcId = srcId;
        e.dstId = dstId;
        e.durationMs = durationMs;
        e.mode = (mode != nullptr) ? std::string(mode) : std::string("unknown");
        enqueueSQLiteMetric(network, std::move(e));
    }
}

/* Logs a call event with slot number. */

void TrafficNetwork::MetricsLogging::logCallEvent(TrafficNetwork* network, const char* mode, uint32_t peerId, uint32_t streamId,
    uint32_t srcId, uint32_t dstId, uint64_t durationMs, uint8_t slotNo)
{
    if (network == nullptr || !network->m_enableMetrics)
        return;

    uint64_t ts = nowNs();

    if (network->m_enableInfluxDB) {
        influxdb::QueryBuilder()
            .meas("call_event")
                .tag("peerId", std::to_string(peerId))
                .tag("mode", mode)
                .tag("streamId", std::to_string(streamId))
                .tag("srcId", std::to_string(srcId))
                .tag("dstId", std::to_string(dstId))
                    .field("duration", durationMs)
                    .field("slot", slotNo)
                .timestamp(ts)
            .requestAsync(network->m_influxServer);
    }

    if (network->m_enableSQLite && network->m_sqliteDB != nullptr) {
        MetricEvent e;
        e.type = MetricEventType::CALL_EVENT;
        e.tsNs = ts;
        e.peerId = peerId;
        e.streamId = streamId;
        e.srcId = srcId;
        e.dstId = dstId;
        e.durationMs = durationMs;
        e.mode = (mode != nullptr) ? std::string(mode) : std::string("unknown");
        e.hasSlot = true;
        e.slotNo = slotNo;
        enqueueSQLiteMetric(network, std::move(e));
    }
}

/* Logs a call error event. */

void TrafficNetwork::MetricsLogging::logCallErrorEvent(TrafficNetwork* network, uint32_t peerId, uint32_t streamId, uint32_t srcId,
    uint32_t dstId, const std::string& message)
{
    if (network == nullptr || !network->m_enableMetrics)
        return;

    uint64_t ts = nowNs();

    if (network->m_enableInfluxDB) {
        influxdb::QueryBuilder()
            .meas("call_error_event")
                .tag("peerId", std::to_string(peerId))
                .tag("streamId", std::to_string(streamId))
                .tag("srcId", std::to_string(srcId))
                .tag("dstId", std::to_string(dstId))
                    .field("message", message)
                .timestamp(ts)
            .requestAsync(network->m_influxServer);
    }

    if (network->m_enableSQLite && network->m_sqliteDB != nullptr) {
        MetricEvent e;
        e.type = MetricEventType::CALL_ERROR_EVENT;
        e.tsNs = ts;
        e.peerId = peerId;
        e.streamId = streamId;
        e.srcId = srcId;
        e.dstId = dstId;
        e.message = message;
        enqueueSQLiteMetric(network, std::move(e));
    }
}

/* Logs a call error event with slot number. */

void TrafficNetwork::MetricsLogging::logCallErrorEvent(TrafficNetwork* network, uint32_t peerId, uint32_t streamId, uint32_t srcId,
    uint32_t dstId, const std::string& message, uint8_t slotNo)
{
    if (network == nullptr || !network->m_enableMetrics)
        return;

    uint64_t ts = nowNs();

    if (network->m_enableInfluxDB) {
        influxdb::QueryBuilder()
            .meas("call_error_event")
                .tag("peerId", std::to_string(peerId))
                .tag("streamId", std::to_string(streamId))
                .tag("srcId", std::to_string(srcId))
                .tag("dstId", std::to_string(dstId))
                    .field("message", message)
                    .field("slot", slotNo)
                .timestamp(ts)
            .requestAsync(network->m_influxServer);
    }

    if (network->m_enableSQLite && network->m_sqliteDB != nullptr) {
        MetricEvent e;
        e.type = MetricEventType::CALL_ERROR_EVENT;
        e.tsNs = ts;
        e.peerId = peerId;
        e.streamId = streamId;
        e.srcId = srcId;
        e.dstId = dstId;
        e.message = message;
        e.hasSlot = true;
        e.slotNo = slotNo;
        enqueueSQLiteMetric(network, std::move(e));
    }
}

/* Logs a P25 TSBK raw event. */

void TrafficNetwork::MetricsLogging::logTSBKEvent(TrafficNetwork* network, uint32_t peerId, const std::string& lco, const std::string& tsbk,
    const std::string& raw)
{
    if (network == nullptr || !network->m_enableMetrics)
        return;

    uint64_t ts = nowNs();

    if (network->m_enableInfluxDB && network->m_metricsLogRawData) {
        influxdb::QueryBuilder()
            .meas("tsbk_event")
                .tag("peerId", std::to_string(peerId))
                .tag("lco", lco)
                .tag("tsbk", tsbk)
                    .field("raw", raw)
                .timestamp(ts)
            .requestAsync(network->m_influxServer);
    }

    if (network->m_enableSQLite && network->m_sqliteDB != nullptr && network->m_metricsLogRawData) {
        MetricEvent e;
        e.type = MetricEventType::TSBK_EVENT;
        e.tsNs = ts;
        e.peerId = peerId;
        e.lco = lco;
        e.rawDescription = tsbk;
        e.raw = raw;
        enqueueSQLiteMetric(network, std::move(e));
    }
}

/* Logs a DMR CSBK raw event. */

void TrafficNetwork::MetricsLogging::logCSBKEvent(TrafficNetwork* network, uint32_t peerId, const std::string& lco, const std::string& csbk,
    const std::string& raw)
{
    if (network == nullptr || !network->m_enableMetrics)
        return;

    uint64_t ts = nowNs();

    if (network->m_enableInfluxDB && network->m_metricsLogRawData) {
        influxdb::QueryBuilder()
            .meas("csbk_event")
                .tag("peerId", std::to_string(peerId))
                .tag("lco", lco)
                .tag("csbk", csbk)
                    .field("raw", raw)
                .timestamp(ts)
            .requestAsync(network->m_influxServer);
    }

    if (network->m_enableSQLite && network->m_sqliteDB != nullptr && network->m_metricsLogRawData) {
        MetricEvent e;
        e.type = MetricEventType::CSBK_EVENT;
        e.tsNs = ts;
        e.peerId = peerId;
        e.lco = lco;
        e.rawDescription = csbk;
        e.raw = raw;
        enqueueSQLiteMetric(network, std::move(e));
    }
}

// ---------------------------------------------------------------------------
//  Private Class Members
// ---------------------------------------------------------------------------

/* Checks if the SQLite database for the specified TrafficNetwork instance is blank. */

bool TrafficNetwork::MetricsLogging::isSQLiteBlank(TrafficNetwork* network)
{
    if (!network->m_enableMetrics)
        return false;
    if (!network->m_enableSQLite)
        return false;

    std::string sql = "SELECT count(*) FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%';";

    sqlite3_stmt* stmt = nullptr;
    bool blank = true;

    if (sqlite3_prepare_v2(network->m_sqliteDB, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int cnt = sqlite3_column_int(stmt, 0);
            if (cnt > 0) {
                blank = false;
            }
        }
    }
    sqlite3_finalize(stmt);
    return blank;
}

/* Initializes the SQLite database for the specified TrafficNetwork instance. */

void TrafficNetwork::MetricsLogging::initializeSQLite(TrafficNetwork* network)
{
    if (!network->m_enableMetrics)
        return;
    if (!network->m_enableSQLite)
        return;

    std::string sql = R"(
PRAGMA journal_mode = WAL;
PRAGMA synchronous = NORMAL;
PRAGMA foreign_keys = ON;

CREATE TABLE activity (
  id INTEGER PRIMARY KEY,
  ts_ns INTEGER NOT NULL,
  ts_s REAL GENERATED ALWAYS AS (ts_ns / 1000000000.0) STORED,
  peer_id INTEGER NOT NULL,
  identity TEXT NOT NULL,
  msg TEXT NOT NULL
);

CREATE INDEX idx_activity_time ON activity(ts_ns);
CREATE INDEX idx_activity_peer_time ON activity(peer_id, ts_ns);

CREATE TABLE diag (
  id INTEGER PRIMARY KEY,
  ts_ns INTEGER NOT NULL,
  ts_s REAL GENERATED ALWAYS AS (ts_ns / 1000000000.0) STORED,
  peer_id INTEGER NOT NULL,
  identity TEXT NOT NULL,
  msg TEXT NOT NULL
);

CREATE INDEX idx_diag_time ON diag(ts_ns);
CREATE INDEX idx_diag_peer_time ON diag(peer_id, ts_ns);

CREATE TABLE call_event (
  id INTEGER PRIMARY KEY,
  ts_ns INTEGER NOT NULL,
  ts_s REAL GENERATED ALWAYS AS (ts_ns / 1000000000.0) STORED,
  peer_id INTEGER NOT NULL,
  mode TEXT NOT NULL CHECK (mode IN ('Analog','DMR','NXDN','P25')),
  stream_id INTEGER NOT NULL,
  src_id INTEGER NOT NULL,
  dst_id INTEGER NOT NULL,
  duration_ms INTEGER NOT NULL,
  slot INTEGER NULL
);

CREATE INDEX idx_call_event_time ON call_event(ts_ns);
CREATE INDEX idx_call_event_mode_time ON call_event(mode, ts_ns);
CREATE INDEX idx_call_event_peer_time ON call_event(peer_id, ts_ns);
CREATE INDEX idx_call_event_dst_time ON call_event(dst_id, ts_ns);
CREATE INDEX idx_call_event_stream ON call_event(stream_id);

CREATE TABLE call_error_event (
  id INTEGER PRIMARY KEY,
  ts_ns INTEGER NOT NULL,
  ts_s REAL GENERATED ALWAYS AS (ts_ns / 1000000000.0) STORED,
  peer_id INTEGER NOT NULL,
  stream_id INTEGER NOT NULL,
  src_id INTEGER NOT NULL,
  dst_id INTEGER NOT NULL,
  message TEXT NOT NULL,
  slot INTEGER NULL
);

CREATE INDEX idx_call_error_time ON call_error_event(ts_ns);
CREATE INDEX idx_call_error_peer_time ON call_error_event(peer_id, ts_ns);
CREATE INDEX idx_call_error_message_time ON call_error_event(message, ts_ns);
CREATE INDEX idx_call_error_stream ON call_error_event(stream_id);

CREATE TABLE tsbk_event (
  id INTEGER PRIMARY KEY,
  ts_ns INTEGER NOT NULL,
  ts_s REAL GENERATED ALWAYS AS (ts_ns / 1000000000.0) STORED,
  peer_id INTEGER NOT NULL,
  lco TEXT NOT NULL,
  tsbk TEXT NOT NULL,
  raw TEXT NOT NULL
);

CREATE INDEX idx_tsbk_time ON tsbk_event(ts_ns);
CREATE INDEX idx_tsbk_peer_time ON tsbk_event(peer_id, ts_ns);
CREATE INDEX idx_tsbk_lco_time ON tsbk_event(lco, ts_ns);

CREATE TABLE csbk_event (
  id INTEGER PRIMARY KEY,
  ts_ns INTEGER NOT NULL,
  ts_s REAL GENERATED ALWAYS AS (ts_ns / 1000000000.0) STORED,
  peer_id INTEGER NOT NULL,
  lco TEXT NOT NULL,
  csbk TEXT NOT NULL,
  raw TEXT NOT NULL
);

CREATE INDEX idx_csbk_time ON csbk_event(ts_ns);
CREATE INDEX idx_csbk_peer_time ON csbk_event(peer_id, ts_ns);
CREATE INDEX idx_csbk_lco_time ON csbk_event(lco, ts_ns);

CREATE TABLE IF NOT EXISTS metrics_counters (
    key TEXT PRIMARY KEY,
    value INTEGER NOT NULL
);
)";

    if (sqlite3_exec(network->m_sqliteDB, sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK) {
        LogError(LOG_MASTER, "Error creating SQLite tables, error: %s", sqlite3_errmsg(network->m_sqliteDB));
        sqlite3_close(network->m_sqliteDB);
        network->m_sqliteDB = nullptr;
        network->m_enableSQLite = false;
    }
}

/* Ensures SQLite tables required for persisted metrics counters exist. */

bool TrafficNetwork::MetricsLogging::ensureSQLiteCounterTable(TrafficNetwork* network)
{
    if (network == nullptr || network->m_sqliteDB == nullptr) {
        return false;
    }

    const char* sql =
        "CREATE TABLE IF NOT EXISTS metrics_counters ("
        "key TEXT PRIMARY KEY,"
        "value INTEGER NOT NULL"
        ");";

    if (sqlite3_exec(network->m_sqliteDB, sql, nullptr, nullptr, nullptr) != SQLITE_OK) {
        LogError(LOG_MASTER, "Error creating SQLite metrics_counters table, error: %s", sqlite3_errmsg(network->m_sqliteDB));
        return false;
    }

    return true;
}

/* Loads persisted metrics counters from SQLite. */

void TrafficNetwork::MetricsLogging::loadSQLiteCounters(TrafficNetwork* network)
{
    if (network == nullptr || network->m_sqliteDB == nullptr) {
        return;
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT key, value FROM metrics_counters WHERE key IN (?, ?);";
    if (sqlite3_prepare_v2(network->m_sqliteDB, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        LogWarning(LOG_MASTER, "Unable to prepare SQLite counter load statement: %s", sqlite3_errmsg(network->m_sqliteDB));
        return;
    }

    sqlite3_bind_text(stmt, 1, SQLITE_COUNTER_CALLS_PROCESSED, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, SQLITE_COUNTER_CALL_COLLISIONS, -1, SQLITE_STATIC);

    uint64_t loadedProcessed = 0U;
    uint64_t loadedCollisions = 0U;
    bool hasProcessed = false;
    bool hasCollisions = false;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* key = (const char*)sqlite3_column_text(stmt, 0);
        uint64_t value = (uint64_t)sqlite3_column_int64(stmt, 1);
        if (key == nullptr) {
            continue;
        }

        if (::strcmp(key, SQLITE_COUNTER_CALLS_PROCESSED) == 0) {
            loadedProcessed = value;
            hasProcessed = true;
        } else if (::strcmp(key, SQLITE_COUNTER_CALL_COLLISIONS) == 0) {
            loadedCollisions = value;
            hasCollisions = true;
        }
    }

    sqlite3_finalize(stmt);

    std::lock_guard<std::mutex> lock(g_totalsMutex);
    if (hasProcessed) {
        s_totalCallsProcessed = loadedProcessed;
    }
    if (hasCollisions) {
        s_totalCallCollisions = loadedCollisions;
    }
}

/* Persists a single metrics counter to SQLite. */

void TrafficNetwork::MetricsLogging::persistSQLiteCounter(TrafficNetwork* network, const char* key, uint64_t value)
{
    if (network == nullptr || key == nullptr) {
        return;
    }
    if (!network->m_enableSQLite || network->m_sqliteDB == nullptr) {
        return;
    }

    MetricEvent e;
    e.type = MetricEventType::COUNTER_EVENT;
    e.counterKey = key;
    e.counterValue = value;
    enqueueSQLiteMetric(network, std::move(e));
}
