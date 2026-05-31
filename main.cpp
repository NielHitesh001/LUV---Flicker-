#include <iostream>
#include <thread>
#include <pthread.h>
#include <sched.h>
#include <cassert>
#include <unistd.h>
#include <signal.h>

#include "luv_arena.hpp"
#include "luv_execution.hpp"
#include "luv_telemetry.hpp"

// Utility to pin a thread to a specific CPU core.
bool pin_thread_to_core(pthread_t thread, int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);

    int rc = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
    return rc == 0;
}

// Utility to set SCHED_FIFO scheduling policy with maximum real-time priority.
// This requires CAP_SYS_NICE privileges (typically root).
bool set_realtime_priority() {
    struct sched_param param;
    param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    if (sched_setscheduler(0, SCHED_FIFO, &param) != 0) {
        std::cerr << "Failed to set SCHED_FIFO (are you root?)" << std::endl;
        return false;
    }
    return true;
}

volatile sig_atomic_t g_running = 1;
void handle_sigint(int) {
    g_running = 0;
}

int main() {
    // 1. Arena Initialization
    // Instantiate the global luv_arena at startup.
    // This is the one and only time malloc/mmap is allowed to run.
    luv::Arena arena;
    if (!arena.init()) {
        std::cerr << "Fatal: Failed to initialize Arena (mmap failed or not enough memory)." << std::endl;
        return 1;
    }

    // 2. Initialize execution gateway & telemetry bridge
    luv::ExecutionGateway gateway;
    if (!gateway.init(arena)) {
        std::cerr << "Fatal: Failed to initialize ExecutionGateway." << std::endl;
        return 1;
    }

    luv::exec::RiskLimits limits {};
    limits.max_order_qty = 1'000;
    limits.max_abs_position = 10'000;
    limits.max_alpha_age_ns = 1'000'000;
    gateway.risk().set_limits(3, limits); // Apply to a sample symbol

    luv::TelemetryBridge telemetry;
    luv::TelemetryBridgeConfig telem_cfg;
    if (!telemetry.init(arena, telem_cfg)) {
        std::cerr << "Fatal: Failed to initialize TelemetryBridge." << std::endl;
        return 1;
    }

    // 3. Thread Affinity & Scheduling
    // Pin the main hot-path thread to Core 1 and set it to SCHED_FIFO.
    // SCHED_FIFO and core pinning guarantee that the Linux kernel will not interrupt
    // our hot path for other OS tasks. This is absolutely critical to achieve
    // consistent 8-nanosecond execution times. OS context switches cost microseconds.
    if (!pin_thread_to_core(pthread_self(), 1)) {
        std::cerr << "Warning: Failed to pin main thread to Core 1." << std::endl;
    }

    // In a real environment, you run this with sudo to allow SCHED_FIFO.
    set_realtime_priority();

    // Start background Telemetry thread.
    // We pin the telemetry consumer to a non-critical core (e.g., Core 2) to ensure
    // that its UDP packet serialization does not contend for CPU cache with the hot path.
    if (!telemetry.start()) {
        std::cerr << "Fatal: Failed to start Telemetry thread." << std::endl;
        return 1;
    }

    // In our actual code we just patched a comment to simulate the setup of pinning
    // the background thread within run_loop(), but conceptually here is where we'd
    // grab the thread native handle and pin it to Core 2.
    // e.g. pin_thread_to_core(telemetry.native_handle(), 2);

    signal(SIGINT, handle_sigint);

    std::cout << "LUV Engine started and running loop..." << std::endl;

    // 4. The Infinite Loop (Hot Path)
    // Busy-poll loop simulating a hot path reacting to signals.
    luv::exec::OrderIntent intent{};
    intent.symbol_idx = 3;
    intent.side = luv::exec::kBuy;
    intent.qty = 100;
    intent.price = 1'234'500;
    intent.client_order_id = 0xAABBCCDDu;

    luv::OutboundPacket packet{};

    uint64_t simulated_now = 10'000'000;
    uint32_t simulated_iters = 0;

    while (g_running) {
        // Simulate NIC poll / receiving an alpha signal
        simulated_now += 1000; // time steps forward
        intent.now_ns = simulated_now;
        intent.alpha_timestamp_ns = simulated_now - 500; // 500ns delay

        // 1. Branchless Risk Check & OUCH Packet Construction (Zero-Allocation)
        const auto decision = gateway.try_build(intent, packet);

        // 2. Dispatch Packet to Network (Mocked)
        if (decision.pass) {
            // e.g., send(packet.bytes, packet.len)
        }

        // 3. Fire-and-forget Telemetry update to the SPSC ring
        // We push to the ring buffer occasionally to track health without blocking
        if (++simulated_iters % 10000 == 0) {
            (void)luv::TelemetryPublisher::publish_heartbeat(arena, 100000, 1.2f, 2.5f);

            // Just for demonstration, yield briefly to avoid 100% CPU lock up in testing
            // Real hotpath would NEVER sleep or yield.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    telemetry.stop();
    std::cout << "\nLUV Engine shutdown cleanly." << std::endl;
    return 0;
}
