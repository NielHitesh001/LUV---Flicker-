#pragma once

// ╔══════════════════════════════════════════════════════════════════════════╗
// ║  LUV — Zero-Copy AI Inference Engine                                    ║
// ║  luv_consumer.hpp — Tick-ring consumer orchestration                    ║
// ║                                                                          ║
// ║  Owns the consumer-side hot path: pull decoded TickMsg structs from    ║
// ║  arena.tick_ring, reconstruct the LOB, extract micro-structure         ║
// ║  features, and finally consume the ring slot.                           ║
// ╚══════════════════════════════════════════════════════════════════════════╝

#include <atomic>
#include <cstdint>

#include "luv_arena.hpp"
#include "luv_lob.hpp"
#include "luv_features.hpp"
#include "luv_ai.hpp"

namespace luv {

class Consumer {
public:
    Consumer() = default;
    Consumer(const Consumer&) = delete;
    Consumer& operator=(const Consumer&) = delete;

    [[nodiscard]] bool init(Arena& arena) noexcept {
        if (!arena.is_initialised()) return false;

        _arena = &arena;
        if (!_lob.init(arena)) return false;
        if (!_features.init(arena)) return false;

        _features.set_lob_view(make_lob_view(&_lob));
        _tick_count = 0;
        _running.store(false, std::memory_order_relaxed);
        return true;
    }

    void run() noexcept {
        _running.store(true, std::memory_order_relaxed);
        while (_running.load(std::memory_order_relaxed)) [[likely]] {
            (void)process_one();
        }
    }

    void stop() noexcept {
        _running.store(false, std::memory_order_relaxed);
    }

    [[nodiscard]] bool process_one() noexcept {
        if (!_arena) [[unlikely]] return false;

        TickMsg* tick = _arena->tick_ring.try_peek();
        if (!tick) [[unlikely]] return false;

        _lob.process(*tick);
        _features.update(tick->symbol_idx, *tick);
        if (_ai) (void)_ai->infer_symbol(tick->symbol_idx);
        _arena->tick_ring.consume();
        ++_tick_count;
        return true;
    }

    void set_ai_engine(AIEngine* ai) noexcept {
        _ai = ai;
    }

    [[nodiscard]] uint64_t ticks_processed() const noexcept {
        return _tick_count;
    }

    [[nodiscard]] LOBEngine& lob() noexcept { return _lob; }
    [[nodiscard]] const LOBEngine& lob() const noexcept { return _lob; }

    [[nodiscard]] FeatureExtractor& features() noexcept { return _features; }
    [[nodiscard]] const FeatureExtractor& features() const noexcept {
        return _features;
    }

private:
    [[nodiscard]] static LOBView make_lob_view(LOBEngine* lob) noexcept {
        return LOBView{
            lob,
            [](void* ctx, uint16_t sym) noexcept -> int64_t {
                return static_cast<LOBEngine*>(ctx)->best_bid_price(sym);
            },
            [](void* ctx, uint16_t sym) noexcept -> int64_t {
                return static_cast<LOBEngine*>(ctx)->best_ask_price(sym);
            },
            [](void* ctx, uint16_t sym) noexcept -> int64_t {
                return static_cast<LOBEngine*>(ctx)->mid_price(sym);
            },
            [](void* ctx, uint16_t sym) noexcept -> int64_t {
                return static_cast<LOBEngine*>(ctx)->spread(sym);
            },
            [](void* ctx, uint16_t sym, uint32_t levels) noexcept -> int64_t {
                return static_cast<LOBEngine*>(ctx)->bid_depth_qty(sym, levels);
            },
            [](void* ctx, uint16_t sym, uint32_t levels) noexcept -> int64_t {
                return static_cast<LOBEngine*>(ctx)->ask_depth_qty(sym, levels);
            },
        };
    }

    LOBEngine         _lob;
    FeatureExtractor  _features;
    AIEngine*         _ai = nullptr;
    Arena*            _arena = nullptr;
    std::atomic<bool> _running{false};
    uint64_t          _tick_count = 0;
};

}  // namespace luv
