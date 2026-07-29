// Copyright (C) 2026 TiltedPhoques SRL.
// For licensing information see LICENSE at the root of this distribution.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>

// Server-side traffic and timing metrics, rendered in Prometheus exposition
// format by MetricsServer.
//
// Deliberately depends on nothing but the standard library -- no GameServer, no
// World, no entt, no server PCH -- so it is compiled directly into TPTests and
// unit tested without standing up a server. Do not add server includes here.
//
// Threading: RecordX() runs on the game loop, RenderPrometheus() on the HTTP
// thread. Counters are atomic. The maps take a shared lock on the hot path and
// escalate to a unique lock only when a player or opcode is seen for the first
// time.
class NetworkMetrics
{
public:
    // Upper bounds in milliseconds. A 30Hz tick is 33ms and 60Hz is 16ms, so the
    // interesting detail sits below 50 and anything above it is a stall.
    static constexpr std::array<double, 7> kTickBucketsMs{0.5, 1.0, 5.0, 10.0, 25.0, 50.0, 100.0};

    NetworkMetrics() = default;

    NetworkMetrics(const NetworkMetrics&) = delete;
    NetworkMetrics& operator=(const NetworkMetrics&) = delete;

    static NetworkMetrics& Get() noexcept;

    // aUncompressedBytes is the serialized payload; aWireBytes is what went on
    // the wire after compression. They are equal wherever the seam cannot yet
    // observe the compressed size -- see the callers in GameServer.
    void RecordSent(uint32_t aPlayerId, uint16_t aOpcode, uint32_t aUncompressedBytes, uint32_t aWireBytes) noexcept;
    void RecordReceived(uint32_t aPlayerId, uint16_t aOpcode, uint32_t aUncompressedBytes, uint32_t aWireBytes) noexcept;

    void RecordTick(double aMilliseconds) noexcept;

    // Drops a player's counters. Opcode totals are server-wide and survive.
    void RemovePlayer(uint32_t aPlayerId) noexcept;

    [[nodiscard]] std::string RenderPrometheus() const;

private:
    struct PlayerCounters
    {
        std::atomic<uint64_t> SentWire{};
        std::atomic<uint64_t> SentUncompressed{};
        std::atomic<uint64_t> RecvWire{};
        std::atomic<uint64_t> RecvUncompressed{};
    };

    struct OpcodeCounters
    {
        std::atomic<uint64_t> SentCount{};
        std::atomic<uint64_t> SentBytes{};
        std::atomic<uint64_t> RecvCount{};
        std::atomic<uint64_t> RecvBytes{};
    };

    PlayerCounters& PlayerSlot(uint32_t aPlayerId) noexcept;
    OpcodeCounters& OpcodeSlot(uint16_t aOpcode) noexcept;

    mutable std::shared_mutex m_mutex;

    // unique_ptr so that inserting a new key never relocates a counter another
    // thread is holding a reference to.
    std::map<uint32_t, std::unique_ptr<PlayerCounters>> m_players;
    std::map<uint16_t, std::unique_ptr<OpcodeCounters>> m_opcodes;

    // One extra slot for the +Inf overflow bucket.
    std::array<std::atomic<uint64_t>, kTickBucketsMs.size() + 1> m_tickBuckets{};
    std::atomic<uint64_t> m_tickCount{};
    std::atomic<uint64_t> m_tickSumMicroseconds{};
};
