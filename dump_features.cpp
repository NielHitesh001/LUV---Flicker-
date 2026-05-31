#include <iostream>
#include <vector>
#include <cassert>

#include "luv_arena.hpp"
#include "luv_lob.hpp"
#include "luv_consumer.hpp"
#include "luv_feed_sim.hpp"

using namespace luv;

int main(int argc, char** argv) {
    Arena arena;
    if (!arena.init()) {
        std::cerr << "Failed to init arena\n";
        return 1;
    }

    SimConfig cfg;
    cfg.mode = SimConfig::Mode::kSynthetic;
    cfg.synthetic_symbols = 64;
    cfg.target_rate_hz = 0;
    cfg.prebuf_count = 10000;
    cfg.seed = 42;

    SimFeedSource feed(cfg);
    if (!feed.init(arena)) {
        std::cerr << "Failed to init feed\n";
        return 1;
    }

    Consumer consumer;
    if (!consumer.init(arena)) {
        std::cerr << "Failed to init consumer\n";
        return 1;
    }

    uint64_t total_ticks = 0;
    const uint64_t target_ticks = 500'000;

    int64_t prev_mid[Config::kSymbols] = {0};
    std::vector<float> row_buf(21, 0.0f); // 20 features + 1 label

    // warm up for each symbol individually
    uint32_t sym_ticks[Config::kSymbols] = {0};

    while (total_ticks < target_ticks) {
        (void)feed.poll();

        TickMsg* slot = arena.tick_ring.try_peek();
        if (slot) {
            uint16_t sym = slot->symbol_idx;

            (void)consumer.process_one();
            total_ticks++;
            sym_ticks[sym]++;

            uint32_t cursor = consumer.features().current_cursor(sym);
            if (sym_ticks[sym] > 64) {
                int64_t mid_now = consumer.lob().mid_price(sym);
                int64_t mid_prev = prev_mid[sym];

                if (mid_prev > 0 && mid_now != mid_prev) {
                    float label = (mid_now > mid_prev) ? 1.0f : -1.0f;

                    uint32_t latest_idx = (cursor + feat::kLookback - 1) & feat::kLookbackMask;

                    for (int f = 0; f < 20; ++f) {
                        row_buf[f] = arena.feature_rows[sym].data[f * feat::kLookback + latest_idx];
                    }
                    row_buf[20] = label;

                    std::fwrite(row_buf.data(), sizeof(float), 21, stdout);
                }
                prev_mid[sym] = mid_now;
            } else {
                prev_mid[sym] = consumer.lob().mid_price(sym);
            }
        }
    }
    return 0;
}
