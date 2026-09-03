// Simulation-only end-to-end LUV topology.
//
// Ingest: SimFeedSource -> arena.tick_ring
// Strategy: Consumer + AI signal -> ExecutionGateway -> packet queue
// Egress: packet queue -> localhost UDP (optional)

#include <atomic>
#include <cerrno>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "luv_ai.hpp"
#include "luv_consumer.hpp"
#include "luv_execution.hpp"
#include "luv_feed_sim.hpp"

namespace {

constexpr uint32_t kPacketQueueCapacity = 1u << 12;

struct RunConfig {
    uint64_t message_limit = 100'000;
    uint16_t udp_port = 0;  // 0 keeps egress disabled.
    const char* model_path = nullptr;
};

[[nodiscard]] bool parse_args(int argc, char** argv, RunConfig& cfg) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--messages") == 0 && i + 1 < argc) {
            cfg.message_limit = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--udp-port") == 0 && i + 1 < argc) {
            cfg.udp_port = static_cast<uint16_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            cfg.model_path = argv[++i];
        } else {
            return false;
        }
    }
    return cfg.message_limit != 0;
}

void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
}

void egress_loop(luv::StaticSpscQueue<luv::OutboundPacket, kPacketQueueCapacity>& queue,
                 const std::atomic<bool>& strategy_done, uint16_t port,
                 std::atomic<uint64_t>& sent) noexcept {
    int fd = -1;
    sockaddr_in destination{};
    if (port != 0) {
        fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        destination.sin_family = AF_INET;
        destination.sin_port = htons(port);
        (void)::inet_pton(AF_INET, "127.0.0.1", &destination.sin_addr);
    }

    luv::OutboundPacket packet{};
    // Do not exit merely because production stopped: every accepted order is
    // drained before the queue owner tears down.
    while (!strategy_done.load(std::memory_order_acquire) || !queue.empty()) {
        if (!queue.try_pop(packet)) {
            cpu_relax();
            continue;
        }
        if (fd >= 0 && packet.len != 0) {
            const ssize_t result = ::sendto(fd, packet.bytes, packet.len,
                                            MSG_DONTWAIT,
                                            reinterpret_cast<sockaddr*>(&destination),
                                            sizeof(destination));
            if (result == static_cast<ssize_t>(packet.len)) ++sent;
        }
    }
    if (fd >= 0) ::close(fd);
}

}  // namespace

int main(int argc, char** argv) {
    RunConfig cfg{};
    if (!parse_args(argc, argv, cfg)) {
        std::fprintf(stderr, "Usage: %s [--messages N] [--model PATH] [--udp-port PORT]\n", argv[0]);
        return 2;
    }

    luv::Arena arena;
    if (!arena.init()) {
        std::fprintf(stderr, "Unable to initialise LUV arena.\n");
        return 1;
    }

    luv::SimConfig feed_cfg{};
    feed_cfg.target_rate_hz = 0;
    feed_cfg.prebuf_count = 1u << 12;
    luv::SimFeedSource feed(feed_cfg);
    if (!feed.init(arena)) return 1;

    luv::Consumer consumer;
    luv::ExecutionGateway execution;
    if (!consumer.init(arena) || !execution.init(arena)) return 1;
    execution.risk().set_limits(0, {1'000, 100'000, 1'000'000'000});

    luv::AIEngine ai;
    if (cfg.model_path) {
        if (!ai.init(arena) || !ai.load_model_file(cfg.model_path)) {
            std::fprintf(stderr, "Unable to load model: %s\n", cfg.model_path);
            return 1;
        }
        consumer.set_ai_engine(&ai);
    }

    std::atomic<bool> ingest_done{false};
    std::atomic<bool> strategy_done{false};
    std::atomic<uint64_t> sent{0};
    luv::StaticSpscQueue<luv::OutboundPacket, kPacketQueueCapacity> outbound;

    std::thread ingest([&] {
        while (feed.total_messages() < cfg.message_limit) (void)feed.poll();
        ingest_done.store(true, std::memory_order_release);
    });

    std::thread egress(egress_loop, std::ref(outbound), std::cref(strategy_done),
                       cfg.udp_port, std::ref(sent));

    uint64_t accepted = 0;
    uint32_t client_order_id = 1;
    while (!ingest_done.load(std::memory_order_acquire) ||
           arena.tick_ring.size() != 0) {
        luv::TickMsg* tick = arena.tick_ring.try_peek();
        if (!tick) {
            cpu_relax();
            continue;
        }
        const uint16_t symbol = tick->symbol_idx;
        const int64_t price = tick->price;
        const uint64_t timestamp = tick->timestamp;
        (void)consumer.process_one();

        if (!cfg.model_path) continue;
        const luv::SignalOutput& signal = arena.signal(symbol);
        if (signal.direction == 0 || price <= 0) continue;
        luv::exec::OrderIntent intent{};
        intent.symbol_idx = symbol;
        intent.side = signal.direction > 0 ? luv::exec::kBuy : luv::exec::kSell;
        intent.qty = 1;
        intent.price = price;
        intent.alpha_timestamp_ns = timestamp;
        intent.now_ns = timestamp;
        intent.client_order_id = client_order_id++;
        luv::OutboundPacket packet{};
        if (execution.try_build(intent, packet).pass) {
            while (!outbound.try_push(packet)) cpu_relax();
            ++accepted;
        }
    }
    strategy_done.store(true, std::memory_order_release);
    ingest.join();
    egress.join();

    std::printf("processed=%llu accepted=%llu udp_sent=%llu\n",
                static_cast<unsigned long long>(consumer.ticks_processed()),
                static_cast<unsigned long long>(accepted),
                static_cast<unsigned long long>(sent.load()));
    return 0;
}
