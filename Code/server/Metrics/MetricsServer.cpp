// Copyright (C) 2026 TiltedPhoques SRL.
// For licensing information see LICENSE at the root of this distribution.

#include <Metrics/MetricsServer.h>
#include <Metrics/NetworkMetrics.h>

#include <Setting.h>
#include <spdlog/spdlog.h>

#include <httplib.h>

// Loopback by default: metrics are unauthenticated and expose per-player traffic
// volumes, so they must not be reachable from the internet without a deliberate
// choice. Inside a container bind 0.0.0.0 and publish with an explicit
// "-p 127.0.0.1:8100:8100" on the host instead -- see docs/deploy/grain-silo.md.
Console::Setting bMetricsEnabled{"Metrics:bEnabled", "Serve Prometheus metrics over HTTP", true};
Console::Setting uMetricsPort{"Metrics:uPort", "Port for the metrics HTTP endpoint", 8100u};
Console::StringSetting sMetricsBindAddress{"Metrics:sBindAddress", "Bind address for the metrics endpoint. Use 0.0.0.0 inside a container and publish to host loopback.", "127.0.0.1"};

MetricsServer::MetricsServer() = default;

MetricsServer::~MetricsServer()
{
    Stop();
}

void MetricsServer::Start() noexcept
{
    if (!bMetricsEnabled)
    {
        spdlog::info("Metrics endpoint disabled (Metrics:bEnabled=false)");
        return;
    }

    if (m_pServer)
        return;

    const std::string cAddress = sMetricsBindAddress.empty() ? std::string("127.0.0.1") : std::string(sMetricsBindAddress.c_str());
    const int cPort = static_cast<int>(uMetricsPort.value_as<uint32_t>());

    m_pServer = std::make_unique<httplib::Server>();

    m_pServer->Get("/metrics", [](const httplib::Request&, httplib::Response& aResponse) {
        aResponse.set_content(NetworkMetrics::Get().RenderPrometheus(), "text/plain; version=0.0.4");
    });

    m_pServer->Get("/healthz", [](const httplib::Request&, httplib::Response& aResponse) { aResponse.set_content("ok", "text/plain"); });

    m_thread = std::thread([this, cAddress, cPort]() {
        // Blocks until stop() is called from Stop().
        if (!m_pServer->listen(cAddress.c_str(), cPort))
            spdlog::error("Metrics endpoint failed to bind {}:{} -- continuing without it", cAddress, cPort);
    });

    spdlog::info("Metrics endpoint listening on http://{}:{}/metrics", cAddress, cPort);
}

void MetricsServer::Stop() noexcept
{
    if (m_pServer)
        m_pServer->stop();

    if (m_thread.joinable())
        m_thread.join();

    m_pServer.reset();
}
