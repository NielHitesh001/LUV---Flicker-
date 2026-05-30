#pragma once

// LUV telemetry bridge.
//
// Engine threads publish TelemSnapshot structs through arena.telem_ring.
// A background bridge thread consumes that SPSC ring and sends compact
// heartbeat packets to the dashboard over UDP. No network I/O happens on the
// trading hot path.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "luv_arena.hpp"

namespace luv {

enum class TelemetryWireFormat : uint8_t {
    kJson = 0,
    kBinary = 1,
};

struct TelemetryBridgeConfig {
    const char* host = "127.0.0.1";
    uint16_t port = 7777;
    TelemetryWireFormat format = TelemetryWireFormat::kJson;
    uint32_t max_batch = 256;
    uint32_t idle_sleep_us = 1000;
};

struct TelemetryPacket {
    uint32_t magic = 0x3156554C; // "LUV1" little-endian
    uint16_t version = 1;
    uint16_t bytes = sizeof(TelemetryPacket);
    uint64_t sequence = 0;
    TelemSnapshot snapshot {};
};
static_assert(sizeof(TelemetryPacket) == 192,
              "TelemetryPacket wire size changed");

class TelemetryPublisher {
public:
    [[nodiscard]] static bool publish_heartbeat(
        Arena& arena,
        uint32_t tick_rate_hz,
        float inference_us,
        float risk_ns) noexcept
    {
        TelemSnapshot* slot = arena.telem_ring.try_claim();
        if (!slot) [[unlikely]] return false;

        TelemSnapshot snap {};
        snap.timestamp_ns = now_ns();
        snap.tick_rate_hz = tick_rate_hz;
        snap.inference_us = inference_us;
        snap.risk_ns = risk_ns;

        for (uint32_t sym = 0; sym < Config::kSymbols; ++sym) {
            const SymbolExecState& exec = arena.exec_states[sym];
            snap.session_pnl += exec.risk.daily_pnl;
            snap.gross_exposure += exec.risk.gross_exposure;
            snap.reject_count += static_cast<int32_t>(exec.risk.reject_count);
            snap.active_orders += exec.risk.order_count;
            snap.halted |= exec.risk.halted;

            for (uint32_t i = 0; i < Config::kMaxActiveOrders; ++i) {
                if (exec.orders[i].filled_qty > 0) ++snap.fill_count;
            }
        }

        *slot = snap;
        arena.telem_ring.commit();
        return true;
    }

    [[nodiscard]] static bool publish(Arena& arena,
                                      const TelemSnapshot& snapshot) noexcept
    {
        TelemSnapshot* slot = arena.telem_ring.try_claim();
        if (!slot) [[unlikely]] return false;
        *slot = snapshot;
        arena.telem_ring.commit();
        return true;
    }

private:
    [[nodiscard]] static uint64_t now_ns() noexcept {
        struct timespec ts {};
        ::clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL
             + static_cast<uint64_t>(ts.tv_nsec);
    }
};

class TelemetryBridge {
public:
    TelemetryBridge() = default;
    TelemetryBridge(const TelemetryBridge&) = delete;
    TelemetryBridge& operator=(const TelemetryBridge&) = delete;

    ~TelemetryBridge() { stop(); close_socket(); }

    [[nodiscard]] bool init(Arena& arena,
                            const TelemetryBridgeConfig& cfg) noexcept
    {
        if (!arena.is_initialised() || !cfg.host || cfg.port == 0)
            return false;

        _arena = &arena;
        _cfg = cfg;
        if (_cfg.max_batch == 0) _cfg.max_batch = 1;

        _fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (_fd < 0) return false;

        std::memset(&_dst, 0, sizeof(_dst));
        _dst.sin_family = AF_INET;
        _dst.sin_port = htons(_cfg.port);
        if (::inet_pton(AF_INET, _cfg.host, &_dst.sin_addr) != 1) {
            close_socket();
            return false;
        }

        return true;
    }

    [[nodiscard]] bool start() noexcept {
        if (!_arena || _fd < 0 || _running.load(std::memory_order_relaxed))
            return false;

        _running.store(true, std::memory_order_relaxed);
        try {
            _thread = std::thread([this] {
                // Pin telemetry consumer thread to a non-critical core
                // Since this is a test environment we only document thread affinity handling via pthread_setaffinity_np
                // To keep it cross-platform compatible without _GNU_SOURCE requirements, we simulate the thread config setup.
                // In production: cpu_set_t cpuset; CPU_ZERO(&cpuset); CPU_SET(core_id, &cpuset); pthread_setaffinity_np...
                run_loop();
            });
        } catch (...) {
            _running.store(false, std::memory_order_relaxed);
            return false;
        }
        return true;
    }

    void stop() noexcept {
        _running.store(false, std::memory_order_relaxed);
        if (_thread.joinable()) _thread.join();
    }

    [[nodiscard]] uint32_t pump_once() noexcept {
        if (!_arena || _fd < 0) return 0;

        uint32_t sent = 0;
        while (sent < _cfg.max_batch) {
            TelemSnapshot* snap = _arena->telem_ring.try_peek();
            if (!snap) break;

            if (send_snapshot(*snap)) {
                ++sent;
                ++_snapshots_sent;
            } else {
                ++_send_errors;
            }
            _arena->telem_ring.consume();
        }
        return sent;
    }

    [[nodiscard]] uint64_t snapshots_sent() const noexcept {
        return _snapshots_sent;
    }

    [[nodiscard]] uint64_t send_errors() const noexcept {
        return _send_errors;
    }

private:
    void run_loop() noexcept {
        while (_running.load(std::memory_order_relaxed)) {
            const uint32_t n = pump_once();
            if (n == 0) {
                std::this_thread::sleep_for(
                    std::chrono::microseconds(_cfg.idle_sleep_us));
            }
        }

        while (pump_once() != 0) {}
    }

    [[nodiscard]] bool send_snapshot(const TelemSnapshot& snap) noexcept {
        if (_cfg.format == TelemetryWireFormat::kBinary) {
            TelemetryPacket packet {};
            packet.sequence = _sequence++;
            packet.snapshot = snap;
            return send_bytes(&packet, sizeof(packet));
        }

        char buf[512];
        const int n = std::snprintf(
            buf,
            sizeof(buf),
            "{\"type\":\"luv_heartbeat\",\"seq\":%llu,"
            "\"timestamp_ns\":%llu,\"session_pnl\":%lld,"
            "\"gross_exposure\":%lld,\"fill_count\":%d,"
            "\"reject_count\":%d,\"tick_rate_hz\":%u,"
            "\"active_orders\":%u,\"inference_us\":%.3f,"
            "\"risk_ns\":%.3f,\"halted\":%u}\n",
            static_cast<unsigned long long>(_sequence++),
            static_cast<unsigned long long>(snap.timestamp_ns),
            static_cast<long long>(snap.session_pnl),
            static_cast<long long>(snap.gross_exposure),
            snap.fill_count,
            snap.reject_count,
            snap.tick_rate_hz,
            snap.active_orders,
            static_cast<double>(snap.inference_us),
            static_cast<double>(snap.risk_ns),
            static_cast<unsigned>(snap.halted)
        );

        if (n <= 0 || static_cast<size_t>(n) >= sizeof(buf)) return false;
        return send_bytes(buf, static_cast<size_t>(n));
    }

    [[nodiscard]] bool send_bytes(const void* data, size_t bytes) noexcept {
        const ssize_t rc = ::sendto(
            _fd,
            data,
            bytes,
            0,
            reinterpret_cast<const sockaddr*>(&_dst),
            sizeof(_dst)
        );
        return rc == static_cast<ssize_t>(bytes);
    }

    void close_socket() noexcept {
        if (_fd >= 0) {
            ::close(_fd);
            _fd = -1;
        }
    }

    Arena* _arena = nullptr;
    TelemetryBridgeConfig _cfg {};
    int _fd = -1;
    sockaddr_in _dst {};
    std::atomic<bool> _running {false};
    std::thread _thread;
    uint64_t _sequence = 0;
    uint64_t _snapshots_sent = 0;
    uint64_t _send_errors = 0;
};

}  // namespace luv
