#include <catch2/catch.hpp>

#include <Metrics/NetworkMetrics.h>

#include <thread>
#include <vector>

// NetworkMetrics is constructed directly here rather than through Get(), so the
// cases are independent of process-wide state and of each other.

TEST_CASE("NetworkMetrics aggregates per player", "[metrics]")
{
    NetworkMetrics metrics;

    metrics.RecordSent(1, 5, 100, 60);
    metrics.RecordSent(1, 5, 100, 60);
    metrics.RecordSent(2, 7, 50, 30);
    metrics.RecordReceived(1, 3, 80, 40);

    const auto rendered = metrics.RenderPrometheus();

    REQUIRE(rendered.find("st_player_sent_wire_bytes_total{player=\"1\"} 120") != std::string::npos);
    REQUIRE(rendered.find("st_player_sent_uncompressed_bytes_total{player=\"1\"} 200") != std::string::npos);
    REQUIRE(rendered.find("st_player_sent_wire_bytes_total{player=\"2\"} 30") != std::string::npos);
    REQUIRE(rendered.find("st_player_recv_wire_bytes_total{player=\"1\"} 40") != std::string::npos);
    REQUIRE(rendered.find("st_player_recv_uncompressed_bytes_total{player=\"1\"} 80") != std::string::npos);
}

TEST_CASE("NetworkMetrics aggregates per opcode", "[metrics]")
{
    NetworkMetrics metrics;

    metrics.RecordSent(1, 5, 100, 60);
    metrics.RecordSent(2, 5, 100, 60);
    metrics.RecordReceived(1, 9, 20, 10);

    const auto rendered = metrics.RenderPrometheus();

    // Opcode counters aggregate across players.
    REQUIRE(rendered.find("st_message_sent_total{opcode=\"5\"} 2") != std::string::npos);
    REQUIRE(rendered.find("st_message_sent_bytes_total{opcode=\"5\"} 120") != std::string::npos);
    REQUIRE(rendered.find("st_message_recv_total{opcode=\"9\"} 1") != std::string::npos);
    REQUIRE(rendered.find("st_message_recv_bytes_total{opcode=\"9\"} 10") != std::string::npos);
}

TEST_CASE("NetworkMetrics buckets tick timings cumulatively", "[metrics]")
{
    NetworkMetrics metrics;

    metrics.RecordTick(0.4);  // <= 0.5
    metrics.RecordTick(5.0);  // <= 5
    metrics.RecordTick(50.0); // <= 50
    metrics.RecordTick(500.0);

    const auto rendered = metrics.RenderPrometheus();

    REQUIRE(rendered.find("st_tick_duration_ms_count 4") != std::string::npos);

    // Prometheus histogram buckets are cumulative: each le includes all below.
    REQUIRE(rendered.find("st_tick_duration_ms_bucket{le=\"0.5\"} 1") != std::string::npos);
    REQUIRE(rendered.find("st_tick_duration_ms_bucket{le=\"1\"} 1") != std::string::npos);
    REQUIRE(rendered.find("st_tick_duration_ms_bucket{le=\"5\"} 2") != std::string::npos);
    REQUIRE(rendered.find("st_tick_duration_ms_bucket{le=\"10\"} 2") != std::string::npos);
    REQUIRE(rendered.find("st_tick_duration_ms_bucket{le=\"50\"} 3") != std::string::npos);
    REQUIRE(rendered.find("st_tick_duration_ms_bucket{le=\"+Inf\"} 4") != std::string::npos);
}

TEST_CASE("NetworkMetrics drops a player on removal", "[metrics]")
{
    NetworkMetrics metrics;

    metrics.RecordSent(1, 5, 100, 60);
    metrics.RecordSent(2, 5, 100, 60);
    metrics.RemovePlayer(1);

    const auto rendered = metrics.RenderPrometheus();

    REQUIRE(rendered.find("player=\"1\"") == std::string::npos);
    REQUIRE(rendered.find("player=\"2\"") != std::string::npos);

    // Opcode totals are server-wide history and must survive a player leaving.
    REQUIRE(rendered.find("st_message_sent_total{opcode=\"5\"} 2") != std::string::npos);
}

TEST_CASE("NetworkMetrics emits TYPE metadata", "[metrics]")
{
    NetworkMetrics metrics;
    metrics.RecordSent(1, 5, 100, 60);

    const auto rendered = metrics.RenderPrometheus();

    REQUIRE(rendered.find("# TYPE st_player_sent_wire_bytes_total counter") != std::string::npos);
    REQUIRE(rendered.find("# TYPE st_tick_duration_ms histogram") != std::string::npos);
}

TEST_CASE("NetworkMetrics survives concurrent writers", "[metrics]")
{
    // The record path runs on the game loop while RenderPrometheus runs on the
    // HTTP thread, so this guards the locking rather than any single number.
    NetworkMetrics metrics;

    constexpr int cThreads = 4;
    constexpr int cPerThread = 500;

    std::vector<std::thread> threads;
    threads.reserve(cThreads);

    for (int t = 0; t < cThreads; ++t)
    {
        threads.emplace_back([&metrics, t]() {
            for (int i = 0; i < cPerThread; ++i)
            {
                metrics.RecordSent(static_cast<uint32_t>(t), static_cast<uint16_t>(i % 8), 10, 5);
                metrics.RecordTick(1.0);
            }
        });
    }

    std::thread reader([&metrics]() {
        for (int i = 0; i < 50; ++i)
            (void)metrics.RenderPrometheus();
    });

    for (auto& thread : threads)
        thread.join();
    reader.join();

    const auto rendered = metrics.RenderPrometheus();

    // Every write must be accounted for exactly once.
    REQUIRE(rendered.find("st_tick_duration_ms_count " + std::to_string(cThreads * cPerThread)) != std::string::npos);
    REQUIRE(rendered.find("st_player_sent_wire_bytes_total{player=\"0\"} " + std::to_string(cPerThread * 5)) != std::string::npos);
}
