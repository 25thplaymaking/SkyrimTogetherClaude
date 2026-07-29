// Copyright (C) 2026 TiltedPhoques SRL.
// For licensing information see LICENSE at the root of this distribution.
#pragma once

#include <memory>
#include <thread>

namespace httplib
{
class Server;
}

// Serves NetworkMetrics over HTTP in Prometheus exposition format.
//
// httplib::Server is forward declared so httplib.h -- which is enormous and
// carries an OpenSSL macro that already collides in this project -- stays inside
// MetricsServer.cpp.
//
// Runs on its own thread. A failure to bind is logged and swallowed: a metrics
// port collision must never stop the game server from starting.
class MetricsServer
{
public:
    // Both defined out of line: a defaulted constructor here would instantiate
    // unique_ptr's deleter against the still-incomplete httplib::Server.
    MetricsServer();
    ~MetricsServer();

    MetricsServer(const MetricsServer&) = delete;
    MetricsServer& operator=(const MetricsServer&) = delete;

    // No-op when Metrics:bEnabled is false.
    void Start() noexcept;

    // Safe to call twice, and safe when Start() did nothing.
    void Stop() noexcept;

private:
    std::unique_ptr<httplib::Server> m_pServer;
    std::thread m_thread;
};
