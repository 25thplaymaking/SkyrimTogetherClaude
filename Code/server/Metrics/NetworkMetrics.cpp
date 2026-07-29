// Copyright (C) 2026 TiltedPhoques SRL.
// For licensing information see LICENSE at the root of this distribution.

#include <Metrics/NetworkMetrics.h>

#include <sstream>

namespace
{
// Prometheus wants bucket bounds without trailing zeros, so 1.0 renders as "1"
// while 0.5 stays "0.5".
std::string FormatBound(double aValue)
{
    std::ostringstream stream;
    stream << aValue;
    return stream.str();
}
} // namespace

NetworkMetrics& NetworkMetrics::Get() noexcept
{
    static NetworkMetrics s_instance;
    return s_instance;
}

NetworkMetrics::PlayerCounters& NetworkMetrics::PlayerSlot(uint32_t aPlayerId) noexcept
{
    {
        std::shared_lock lock(m_mutex);
        const auto itor = m_players.find(aPlayerId);
        if (itor != m_players.end())
            return *itor->second;
    }

    std::unique_lock lock(m_mutex);
    auto& slot = m_players[aPlayerId];
    if (!slot)
        slot = std::make_unique<PlayerCounters>();

    return *slot;
}

NetworkMetrics::OpcodeCounters& NetworkMetrics::OpcodeSlot(uint16_t aOpcode) noexcept
{
    {
        std::shared_lock lock(m_mutex);
        const auto itor = m_opcodes.find(aOpcode);
        if (itor != m_opcodes.end())
            return *itor->second;
    }

    std::unique_lock lock(m_mutex);
    auto& slot = m_opcodes[aOpcode];
    if (!slot)
        slot = std::make_unique<OpcodeCounters>();

    return *slot;
}

void NetworkMetrics::RecordSent(uint32_t aPlayerId, uint16_t aOpcode, uint32_t aUncompressedBytes, uint32_t aWireBytes) noexcept
{
    auto& player = PlayerSlot(aPlayerId);
    player.SentWire.fetch_add(aWireBytes, std::memory_order_relaxed);
    player.SentUncompressed.fetch_add(aUncompressedBytes, std::memory_order_relaxed);

    auto& opcode = OpcodeSlot(aOpcode);
    opcode.SentCount.fetch_add(1, std::memory_order_relaxed);
    opcode.SentBytes.fetch_add(aWireBytes, std::memory_order_relaxed);
}

void NetworkMetrics::RecordReceived(uint32_t aPlayerId, uint16_t aOpcode, uint32_t aUncompressedBytes, uint32_t aWireBytes) noexcept
{
    auto& player = PlayerSlot(aPlayerId);
    player.RecvWire.fetch_add(aWireBytes, std::memory_order_relaxed);
    player.RecvUncompressed.fetch_add(aUncompressedBytes, std::memory_order_relaxed);

    auto& opcode = OpcodeSlot(aOpcode);
    opcode.RecvCount.fetch_add(1, std::memory_order_relaxed);
    opcode.RecvBytes.fetch_add(aWireBytes, std::memory_order_relaxed);
}

void NetworkMetrics::RecordTick(double aMilliseconds) noexcept
{
    size_t index = kTickBucketsMs.size(); // +Inf unless a bound matches
    for (size_t i = 0; i < kTickBucketsMs.size(); ++i)
    {
        if (aMilliseconds <= kTickBucketsMs[i])
        {
            index = i;
            break;
        }
    }

    m_tickBuckets[index].fetch_add(1, std::memory_order_relaxed);
    m_tickCount.fetch_add(1, std::memory_order_relaxed);
    m_tickSumMicroseconds.fetch_add(static_cast<uint64_t>(aMilliseconds * 1000.0), std::memory_order_relaxed);
}

void NetworkMetrics::RemovePlayer(uint32_t aPlayerId) noexcept
{
    std::unique_lock lock(m_mutex);
    m_players.erase(aPlayerId);
}

std::string NetworkMetrics::RenderPrometheus() const
{
    std::ostringstream out;

    {
        std::shared_lock lock(m_mutex);

        const auto emitPlayer = [&](const char* acName, uint64_t (*acGet)(const PlayerCounters&)) {
            out << "# TYPE " << acName << " counter\n";
            for (const auto& [playerId, counters] : m_players)
                out << acName << "{player=\"" << playerId << "\"} " << acGet(*counters) << "\n";
        };

        emitPlayer("st_player_sent_wire_bytes_total", [](const PlayerCounters& c) { return c.SentWire.load(std::memory_order_relaxed); });
        emitPlayer("st_player_sent_uncompressed_bytes_total", [](const PlayerCounters& c) { return c.SentUncompressed.load(std::memory_order_relaxed); });
        emitPlayer("st_player_recv_wire_bytes_total", [](const PlayerCounters& c) { return c.RecvWire.load(std::memory_order_relaxed); });
        emitPlayer("st_player_recv_uncompressed_bytes_total", [](const PlayerCounters& c) { return c.RecvUncompressed.load(std::memory_order_relaxed); });

        const auto emitOpcode = [&](const char* acName, uint64_t (*acGet)(const OpcodeCounters&)) {
            out << "# TYPE " << acName << " counter\n";
            for (const auto& [opcode, counters] : m_opcodes)
                out << acName << "{opcode=\"" << opcode << "\"} " << acGet(*counters) << "\n";
        };

        emitOpcode("st_message_sent_total", [](const OpcodeCounters& c) { return c.SentCount.load(std::memory_order_relaxed); });
        emitOpcode("st_message_sent_bytes_total", [](const OpcodeCounters& c) { return c.SentBytes.load(std::memory_order_relaxed); });
        emitOpcode("st_message_recv_total", [](const OpcodeCounters& c) { return c.RecvCount.load(std::memory_order_relaxed); });
        emitOpcode("st_message_recv_bytes_total", [](const OpcodeCounters& c) { return c.RecvBytes.load(std::memory_order_relaxed); });

        out << "st_player_count " << m_players.size() << "\n";
    }

    // Histogram buckets are cumulative, so each bound reports everything at or
    // below it.
    out << "# TYPE st_tick_duration_ms histogram\n";

    uint64_t cumulative = 0;
    for (size_t i = 0; i < kTickBucketsMs.size(); ++i)
    {
        cumulative += m_tickBuckets[i].load(std::memory_order_relaxed);
        out << "st_tick_duration_ms_bucket{le=\"" << FormatBound(kTickBucketsMs[i]) << "\"} " << cumulative << "\n";
    }

    cumulative += m_tickBuckets[kTickBucketsMs.size()].load(std::memory_order_relaxed);
    out << "st_tick_duration_ms_bucket{le=\"+Inf\"} " << cumulative << "\n";

    const double cSumMs = static_cast<double>(m_tickSumMicroseconds.load(std::memory_order_relaxed)) / 1000.0;
    out << "st_tick_duration_ms_sum " << cSumMs << "\n";
    out << "st_tick_duration_ms_count " << m_tickCount.load(std::memory_order_relaxed) << "\n";

    return out.str();
}
