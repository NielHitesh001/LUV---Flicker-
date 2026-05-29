#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "luv_arena.hpp"
#include "luv_consumer.hpp"
#include "luv_lob.hpp"
#include "luv_decode_itch.hpp"

namespace {

constexpr uint64_t kDefaultMessages = 10'000'000ULL;
constexpr size_t kMiB = 1024ULL * 1024ULL;

uint64_t now_ns() {
    timespec ts {};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL
         + static_cast<uint64_t>(ts.tv_nsec);
}

uint64_t live_target_for(uint64_t messages) {
    const uint64_t quarter = messages / 4u;
    const uint64_t cap = luv::LOBEngine::order_ref_map_capacity() / 8u;
    return std::max<uint64_t>(1, std::min<uint64_t>(quarter, cap));
}

luv::TickMsg make_add(uint64_t idx) {
    luv::TickMsg tick {};
    const uint64_t order_ref = idx + 1u;
    const uint16_t sym = static_cast<uint16_t>(idx % luv::Config::kSymbols);
    const uint64_t book_idx = idx / luv::Config::kSymbols;
    const uint8_t side = static_cast<uint8_t>(book_idx & 1u);
    const uint64_t level_idx = (book_idx / 2u) % luv::Config::kLevelsPerSide;
    const uint64_t slot_wave = book_idx / (2u * luv::Config::kLevelsPerSide);

    tick.msg_type = luv::itch::kAddOrder;
    tick.symbol_idx = sym;
    tick.timestamp = idx;
    tick.order_ref = order_ref;
    tick.flags = (side == 0) ? luv::tick_flags::kBuy : 0;
    tick.qty = 100 + static_cast<int64_t>(slot_wave & 31u);
    const int64_t side_sorted_level = (side == 0)
        ? static_cast<int64_t>(luv::Config::kLevelsPerSide - 1u - level_idx)
        : static_cast<int64_t>(level_idx);
    tick.price = 1'000'000
               + side_sorted_level * 100
               + static_cast<int64_t>(sym) * 1'000'000;
    return tick;
}

luv::TickMsg make_delete(uint64_t idx) {
    luv::TickMsg tick {};
    tick.msg_type = luv::itch::kOrderDelete;
    tick.symbol_idx = static_cast<uint16_t>(idx % luv::Config::kSymbols);
    tick.timestamp = idx;
    tick.order_ref = idx + 1u;
    return tick;
}

luv::TickMsg make_trade(uint64_t seq) {
    luv::TickMsg tick {};
    tick.msg_type = luv::itch::kTrade;
    tick.symbol_idx = static_cast<uint16_t>(seq % luv::Config::kSymbols);
    tick.timestamp = seq;
    tick.price = 1'000'000 + static_cast<int64_t>(seq & 1023u) * 100;
    tick.qty = 1 + static_cast<int64_t>(seq & 255u);
    tick.flags = luv::tick_flags::kPrintable
               | ((seq & 1u) ? luv::tick_flags::kBuy : 0);
    tick.match_num = seq + 1u;
    return tick;
}

luv::TickMsg make_tick(uint64_t seq, uint64_t messages) {
    const uint64_t live_target = live_target_for(messages);
    if (seq < live_target) return make_add(seq);
    if (seq < live_target * 2u)
        return make_delete(live_target - 1u - (seq - live_target));
    return make_trade(seq);
}

void assert_ring_full_boundary(luv::Arena& arena) {
    std::printf("\n== Ring boundary ==\n");

    for (uint32_t i = 0; i < luv::Config::kTickCapacity; ++i) {
        luv::TickMsg* slot = arena.tick_ring.try_claim();
        assert(slot != nullptr);
        slot->msg_type = luv::itch::kTrade;
        slot->symbol_idx = 0;
        arena.tick_ring.commit();
    }

    assert(arena.tick_ring.size() == luv::Config::kTickCapacity);
    assert(arena.tick_ring.try_claim() == nullptr);

    while (arena.tick_ring.try_peek()) {
        arena.tick_ring.consume();
    }

    assert(arena.tick_ring.size() == 0);
    std::printf("  [OK] capacity=%u backpressure=null no overwrite\n",
                luv::Config::kTickCapacity);
}

void run_engine_burst(uint64_t messages) {
    std::printf("\n== Engine burst ==\n");

    static_assert(luv::OrderRefMap::kBytes == 192ULL * kMiB,
                  "OrderRefMap budget must remain 192 MiB");
    assert(luv::LOBEngine::order_ref_map_bytes() == 192ULL * kMiB);

    luv::Arena arena;
    assert(arena.init());

    assert_ring_full_boundary(arena);

    luv::Consumer consumer;
    assert(consumer.init(arena));

    uint64_t produced = 0;
    uint64_t claim_full = 0;
    uint64_t high_water = 0;
    uint64_t max_active_orders = 0;

    const uint64_t start = now_ns();
    while (produced < messages) {
        luv::TickMsg* slot = arena.tick_ring.try_claim();
        if (!slot) {
            ++claim_full;
            assert(consumer.process_one());
            continue;
        }

        *slot = make_tick(produced, messages);
        arena.tick_ring.commit();
        ++produced;

        const uint64_t depth = arena.tick_ring.size();
        high_water = std::max(high_water, depth);

        // Let producer lead in short bursts, then drain one. This keeps the
        // SPSC path hot without requiring a second thread for deterministic CI.
        if ((produced & 255u) == 0) {
            for (uint32_t i = 0; i < 128; ++i) {
                if (!consumer.process_one()) break;
            }
        }

        const uint32_t active = consumer.lob().active_order_count();
        max_active_orders = std::max<uint64_t>(max_active_orders, active);
        assert(active < luv::LOBEngine::order_ref_map_capacity());
    }

    while (consumer.process_one()) {
        const uint32_t active = consumer.lob().active_order_count();
        max_active_orders = std::max<uint64_t>(max_active_orders, active);
        assert(active < luv::LOBEngine::order_ref_map_capacity());
    }

    const uint64_t elapsed_ns = now_ns() - start;
    const double seconds = static_cast<double>(elapsed_ns) / 1e9;
    const double throughput = static_cast<double>(messages) / seconds;

    assert(consumer.ticks_processed() == messages);
    assert(consumer.lob().active_order_count() == 0);
    assert(luv::LOBEngine::order_ref_map_bytes() == 192ULL * kMiB);

    std::printf("  messages:          %llu\n",
                static_cast<unsigned long long>(messages));
    std::printf("  processed:         %llu\n",
                static_cast<unsigned long long>(consumer.ticks_processed()));
    std::printf("  elapsed:           %.3f s\n", seconds);
    std::printf("  throughput:        %.0f msg/s\n", throughput);
    std::printf("  ring high-water:   %llu / %u\n",
                static_cast<unsigned long long>(high_water),
                luv::Config::kTickCapacity);
    std::printf("  claim-full events: %llu\n",
                static_cast<unsigned long long>(claim_full));
    std::printf("  max active orders: %llu / %u\n",
                static_cast<unsigned long long>(max_active_orders),
                luv::LOBEngine::order_ref_map_capacity());
    std::printf("  order map budget:  %.1f MiB\n",
                static_cast<double>(luv::LOBEngine::order_ref_map_bytes()) / kMiB);
    std::printf("  [OK] no overflow, no budget breach\n");
}

}  // namespace

int main(int argc, char** argv) {
    uint64_t messages = kDefaultMessages;
    if (argc > 1) {
        messages = std::strtoull(argv[1], nullptr, 10);
        if (messages == 0) messages = kDefaultMessages;
    }

    std::printf("LUV Engine Stress Test\n");
    run_engine_burst(messages);
    std::printf("\nStress test passed.\n");
    return 0;
}
