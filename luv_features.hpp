#pragma once

// ╔══════════════════════════════════════════════════════════════════════════╗
// ║  LUV — Zero-Copy AI Inference Engine                                    ║
// ║  luv_features.hpp — Feature extraction layer                            ║
// ║                                                                          ║
// ║  Computes 20 features per symbol on every LOB update and writes them    ║
// ║  into the Arena's FeatureRow slab using a circular lookback buffer of   ║
// ║  64 steps.  All state is pre-allocated inline — zero dynamic allocation ║
// ║  on the hot path.                                                       ║
// ║                                                                          ║
// ║  Feature layout in FeatureRow::data[1280] (feature-major order):        ║
// ║    data[feature * 64 + step]                                            ║
// ║                                                                          ║
// ║  Feature groups:                                                         ║
// ║    [0..4]   Mid-price delta (1/5/10/30/60-tick return)                  ║
// ║    [5..9]   Bid/ask imbalance ratio (top 1/3/5/10/20 levels)            ║
// ║    [10..14] Trade flow — signed volume (1/5/10/30/60 ticks)             ║
// ║    [15..19] Spread metrics (raw, EMA fast/slow, max/min)                ║
// ║                                                                          ║
// ║  Design choices:                                                         ║
// ║    • Header-only, all functions inline — no TU coupling                 ║
// ║    • No dynamic allocation, no exceptions, no RTTI                      ║
// ║    • LOBView function-pointer struct avoids circular #include           ║
// ║    • Division-by-zero guarded everywhere                                ║
// ╚══════════════════════════════════════════════════════════════════════════╝

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>

#include "luv_arena.hpp"
// NOTE: We deliberately do NOT include luv_lob.hpp to avoid circular
//       header dependencies.  Instead, we use the LOBView struct below
//       to access LOB state through opaque function pointers.

namespace luv {

// ─────────────────────────────────────────────────────────────────────────────
//  LOBView — Decoupled interface to the LOB engine
//
//  The LOB engine's query functions are bound here via function pointers so
//  that luv_features.hpp never needs to #include luv_lob.hpp.  The caller
//  (typically main or the tick processing loop) wires these up once after
//  both the LOB engine and the feature extractor are initialised.
//
//  All price/spread values are in the same fixed-point representation used
//  throughout the system: integer × 10⁴.
//
//  All quantity values are in raw share counts (integer).
// ─────────────────────────────────────────────────────────────────────────────
struct LOBView {
    void* ctx;  // opaque pointer to LOBEngine (or whatever owns LOB state)

    // Best bid price for a symbol (fixed-point ×10⁴).  Returns 0 if no bids.
    int64_t (*best_bid_price)(void* ctx, uint16_t sym);

    // Best ask price for a symbol (fixed-point ×10⁴).  Returns 0 if no asks.
    int64_t (*best_ask_price)(void* ctx, uint16_t sym);

    // Mid-price = (best_bid + best_ask) / 2 (fixed-point ×10⁴).
    // Returns 0 if either side is empty.
    int64_t (*mid_price)(void* ctx, uint16_t sym);

    // Spread = best_ask - best_bid (fixed-point ×10⁴).
    // Returns 0 if either side is empty.
    int64_t (*spread)(void* ctx, uint16_t sym);

    // Total bid quantity across the top `levels` price levels.
    int64_t (*bid_depth_qty)(void* ctx, uint16_t sym, uint32_t levels);

    // Total ask quantity across the top `levels` price levels.
    int64_t (*ask_depth_qty)(void* ctx, uint16_t sym, uint32_t levels);
};

// ─────────────────────────────────────────────────────────────────────────────
//  Compile-time constants for feature extraction
// ─────────────────────────────────────────────────────────────────────────────
namespace feat {

    // Number of lookback steps — must match Config::kLookbackSteps
    inline constexpr uint32_t kLookback      = Config::kLookbackSteps;  // 64
    inline constexpr uint32_t kLookbackMask  = kLookback - 1;          // 63

    static_assert((kLookback & (kLookback - 1)) == 0,
                  "kLookback must be power of two for masking");

    // Feature count — must match Config::kFeaturesPerSymbol
    inline constexpr uint32_t kNumFeatures   = Config::kFeaturesPerSymbol;  // 20

    // Feature indices
    inline constexpr uint32_t kMidDelta1     = 0;
    inline constexpr uint32_t kMidDelta5     = 1;
    inline constexpr uint32_t kMidDelta10    = 2;
    inline constexpr uint32_t kMidDelta30    = 3;
    inline constexpr uint32_t kMidDelta60    = 4;

    inline constexpr uint32_t kImbal1        = 5;
    inline constexpr uint32_t kImbal3        = 6;
    inline constexpr uint32_t kImbal5        = 7;
    inline constexpr uint32_t kImbal10       = 8;
    inline constexpr uint32_t kImbal20       = 9;

    inline constexpr uint32_t kFlow1         = 10;
    inline constexpr uint32_t kFlow5         = 11;
    inline constexpr uint32_t kFlow10        = 12;
    inline constexpr uint32_t kFlow30        = 13;
    inline constexpr uint32_t kFlow60        = 14;

    inline constexpr uint32_t kSpreadRaw     = 15;
    inline constexpr uint32_t kSpreadEMAFast = 16;
    inline constexpr uint32_t kSpreadEMASlow = 17;
    inline constexpr uint32_t kSpreadMax     = 18;
    inline constexpr uint32_t kSpreadMin     = 19;

    // Spread EMA smoothing factors
    inline constexpr float kAlphaFast = 0.1f;
    inline constexpr float kAlphaSlow = 0.01f;

    // Mid-price delta lookback offsets
    inline constexpr uint32_t kDeltaOffsets[5] = { 1, 5, 10, 30, 60 };

    // Imbalance level depths
    inline constexpr uint32_t kImbalLevels[5] = { 1, 3, 5, 10, 20 };

    // Trade flow lookback windows
    inline constexpr uint32_t kFlowWindows[5] = { 1, 5, 10, 30, 60 };

    // Fixed-point → float conversion divisor (prices are ×10⁴)
    inline constexpr float kPriceScale = 10000.0f;

}  // namespace feat

// ─────────────────────────────────────────────────────────────────────────────
//  SymbolState — Per-symbol circular buffers and running statistics
//
//  Each symbol gets its own state block.  The entire array is stack-allocated
//  inside the FeatureExtractor (kSymbols × 1120 bytes ≈ 560 KB — fits in L2).
// ─────────────────────────────────────────────────────────────────────────────
struct SymbolState {
    int64_t  mid_history[feat::kLookback];    // circular buffer of mid-prices (fixed-point)
    int64_t  trade_flow[feat::kLookback];     // circular buffer of signed volumes per tick
    float    spread_ema_fast;                 // EMA with α = 0.1
    float    spread_ema_slow;                 // EMA with α = 0.01
    uint32_t cursor;                          // circular index, always in [0, 63]
    uint32_t tick_count;                      // total ticks observed for this symbol
    bool     warm;                            // true after ≥ 64 ticks (lookback fully populated)
    uint8_t  _pad[3];                         // padding for alignment
};

// ─────────────────────────────────────────────────────────────────────────────
//  FeatureExtractor — The main feature computation engine
//
//  Usage:
//      luv::Arena arena;
//      arena.init();
//
//      luv::FeatureExtractor features;
//      features.init(arena);
//      features.set_lob_view(view);
//
//      // On each LOB update:
//      features.update(sym, tick);
// ─────────────────────────────────────────────────────────────────────────────
class FeatureExtractor {
public:
    FeatureExtractor() = default;
    FeatureExtractor(const FeatureExtractor&) = delete;
    FeatureExtractor& operator=(const FeatureExtractor&) = delete;

    // ── Initialisation ──────────────────────────────────────────────────────
    //  Binds the extractor to the arena's feature slab and zeroes all
    //  per-symbol state.  Must be called once before any update() calls.
    //  Returns false if the arena is not initialised.
    [[nodiscard]] bool init(Arena& arena) noexcept {
        if (!arena.is_initialised()) return false;

        _arena = &arena;

        // Zero all per-symbol state
        std::memset(_state, 0, sizeof(_state));

        // Initialise spread EMA seeds to zero (will be overwritten on first tick)
        for (uint32_t s = 0; s < Config::kSymbols; ++s) {
            _state[s].spread_ema_fast = 0.0f;
            _state[s].spread_ema_slow = 0.0f;
            _state[s].cursor          = 0;
            _state[s].tick_count      = 0;
            _state[s].warm            = false;
        }

        _initialised = true;
        return true;
    }

    // ── Set the LOB view ────────────────────────────────────────────────────
    //  Called once after the LOB engine is initialised.  The LOBView struct
    //  is copied by value (it's just pointers).
    void set_lob_view(const LOBView& view) noexcept {
        _lob = view;
    }

    // ── Update — called after each LOB update ───────────────────────────────
    //
    //  Reads the current LOB state through the LOBView, computes all 20
    //  features, and writes them into arena.feature_rows[sym] at the current
    //  circular cursor position.
    //
    //  This function is on the critical path — it must be as fast as possible.
    //  All branches are organised for the common (warm) case.
    void update(uint16_t sym, const TickMsg& tick) noexcept {
        // ── Bounds check ────────────────────────────────────────────────
        if (sym >= Config::kSymbols) [[unlikely]] return;

        SymbolState& st   = _state[sym];
        FeatureRow&  row  = _arena->feature_rows[sym];
        const uint32_t c  = st.cursor;

        // ── 1. Sample current LOB state ─────────────────────────────────
        const int64_t mid_now    = _lob.mid_price(_lob.ctx, sym);
        const int64_t spread_now = _lob.spread(_lob.ctx, sym);

        // ── 2. Record mid-price into circular history ───────────────────
        st.mid_history[c] = mid_now;

        // ── 3. Record trade flow (signed volume) ────────────────────────
        //  Only meaningful for printable (trade) ticks.  For non-trade ticks
        //  we record zero signed volume so the rolling sums remain correct.
        if (tick.flags & tick_flags::kPrintable) {
            // Buy trades get positive volume, sell trades get negative
            const int64_t signed_vol = (tick.flags & tick_flags::kBuy)
                                         ? tick.qty
                                         : -tick.qty;
            st.trade_flow[c] = signed_vol;
        } else {
            st.trade_flow[c] = 0;
        }

        // ── 4. Compute features ─────────────────────────────────────────

        // ── Group 1: Mid-price delta (features 0–4) ─────────────────────
        //
        //  For each lookback offset N ∈ {1, 5, 10, 30, 60}:
        //    feature = (mid_now - mid_N_ago) / mid_N_ago
        //
        //  This produces a dimensionless return.  If mid_N_ago is 0 (no
        //  historical data yet, or book was empty), the feature is 0.0f.
        for (uint32_t i = 0; i < 5; ++i) {
            const uint32_t offset = feat::kDeltaOffsets[i];
            float val = 0.0f;

            if (st.tick_count >= offset && mid_now != 0) {
                // Walk backwards `offset` steps in the circular buffer
                const uint32_t past_idx = (c - offset) & feat::kLookbackMask;
                const int64_t mid_past  = st.mid_history[past_idx];

                if (mid_past != 0) {
                    // Fixed-point subtraction then float division
                    val = static_cast<float>(mid_now - mid_past)
                        / static_cast<float>(mid_past);
                }
            }

            write_feature(row, feat::kMidDelta1 + i, c, val);
        }

        // ── Group 2: Bid/ask imbalance ratio (features 5–9) ─────────────
        //
        //  For each depth D ∈ {1, 3, 5, 10, 20}:
        //    bid_qty = total bid quantity across top D levels
        //    ask_qty = total ask quantity across top D levels
        //    feature = bid_qty / (bid_qty + ask_qty)
        //
        //  Result is in [0, 1].  If both sides are empty, feature = 0.5f
        //  (neutral).
        for (uint32_t i = 0; i < 5; ++i) {
            const uint32_t depth = feat::kImbalLevels[i];
            const int64_t bid_q  = _lob.bid_depth_qty(_lob.ctx, sym, depth);
            const int64_t ask_q  = _lob.ask_depth_qty(_lob.ctx, sym, depth);
            const int64_t total  = bid_q + ask_q;

            const float val = (total > 0)
                ? static_cast<float>(bid_q) / static_cast<float>(total)
                : 0.5f;

            write_feature(row, feat::kImbal1 + i, c, val);
        }

        // ── Group 3: Trade flow — signed volume (features 10–14) ────────
        //
        //  For each window W ∈ {1, 5, 10, 30, 60}:
        //    feature = Σ signed_volume over the last W ticks
        //
        //  The sum is computed by iterating backwards through the circular
        //  buffer.  Raw integer sums are cast to float.
        for (uint32_t i = 0; i < 5; ++i) {
            const uint32_t window = feat::kFlowWindows[i];

            // Only sum over as many ticks as we actually have
            const uint32_t effective = (st.tick_count < window)
                                         ? (st.tick_count + 1)  // include current
                                         : window;
            int64_t sum = 0;
            for (uint32_t j = 0; j < effective; ++j) {
                const uint32_t idx = (c - j) & feat::kLookbackMask;
                sum += st.trade_flow[idx];
            }

            write_feature(row, feat::kFlow1 + i, c, static_cast<float>(sum));
        }

        // ── Group 4: Spread metrics (features 15–19) ────────────────────

        // Convert fixed-point spread to float (in price units, e.g. dollars)
        const float spread_f = static_cast<float>(spread_now) / feat::kPriceScale;

        // Feature 15: Current spread (float)
        write_feature(row, feat::kSpreadRaw, c, spread_f);

        // Feature 16: Spread EMA (α = 0.1, fast)
        if (st.tick_count == 0) {
            // Seed EMA with first observation
            st.spread_ema_fast = spread_f;
        } else {
            st.spread_ema_fast += feat::kAlphaFast * (spread_f - st.spread_ema_fast);
        }
        write_feature(row, feat::kSpreadEMAFast, c, st.spread_ema_fast);

        // Feature 17: Spread EMA (α = 0.01, slow)
        if (st.tick_count == 0) {
            st.spread_ema_slow = spread_f;
        } else {
            st.spread_ema_slow += feat::kAlphaSlow * (spread_f - st.spread_ema_slow);
        }
        write_feature(row, feat::kSpreadEMASlow, c, st.spread_ema_slow);

        // Features 18–19: Spread max/min over the last 64 ticks
        //
        //  We scan the entire FeatureRow column for the raw spread (feature 15)
        //  across all 64 steps.  Before the buffer is warm (< 64 ticks), we
        //  only scan the valid entries.
        {
            const uint32_t scan_count = st.warm
                ? feat::kLookback
                : (st.tick_count + 1);  // include current tick

            float spread_max = -1e30f;
            float spread_min =  1e30f;

            for (uint32_t j = 0; j < scan_count; ++j) {
                const uint32_t idx = (c - j) & feat::kLookbackMask;
                const float s = read_feature(row, feat::kSpreadRaw, idx);
                if (s > spread_max) spread_max = s;
                if (s < spread_min) spread_min = s;
            }

            // Edge case: no valid ticks yet (shouldn't happen since we just
            // wrote one, but guard anyway)
            if (scan_count == 0) {
                spread_max = spread_f;
                spread_min = spread_f;
            }

            write_feature(row, feat::kSpreadMax, c, spread_max);
            write_feature(row, feat::kSpreadMin, c, spread_min);
        }

        // ── 5. Advance cursor and tick count ────────────────────────────
        st.cursor = (c + 1) & feat::kLookbackMask;
        ++st.tick_count;

        // Mark warm once we've filled the entire lookback window
        if (!st.warm && st.tick_count >= feat::kLookback) {
            st.warm = true;
        }
    }

    // ── Accessors ───────────────────────────────────────────────────────────

    [[nodiscard]] bool is_initialised() const noexcept { return _initialised; }

    // Access per-symbol state (for diagnostics / testing)
    [[nodiscard]] const SymbolState& state(uint16_t sym) const noexcept {
        return _state[sym];
    }

    // Get the most recent cursor position for a symbol (points to the NEXT
    // write position, so the most recent write is at (cursor - 1) & mask).
    [[nodiscard]] uint32_t current_cursor(uint16_t sym) const noexcept {
        return _state[sym].cursor;
    }

    // Get the number of ticks processed for a symbol
    [[nodiscard]] uint32_t tick_count(uint16_t sym) const noexcept {
        return _state[sym].tick_count;
    }

    // Check if a symbol's lookback buffer is fully warm
    [[nodiscard]] bool is_warm(uint16_t sym) const noexcept {
        return _state[sym].warm;
    }

private:
    // ── Feature read/write helpers ──────────────────────────────────────────
    //
    //  Feature-major layout:  data[feature * 64 + step]
    //  These are the only two functions that touch FeatureRow::data directly,
    //  centralising the index arithmetic.

    static void write_feature(
        FeatureRow& row, uint32_t feature, uint32_t step, float val) noexcept
    {
        row.data[feature * feat::kLookback + step] = val;
    }

    [[nodiscard]] static float read_feature(
        const FeatureRow& row, uint32_t feature, uint32_t step) noexcept
    {
        return row.data[feature * feat::kLookback + step];
    }

    // ── Member data ─────────────────────────────────────────────────────────

    Arena*      _arena       = nullptr;
    LOBView     _lob         = {};          // zeroed — all function pointers null until set
    bool        _initialised = false;

    // Per-symbol state — inline allocated, no dynamic memory.
    // Config::kSymbols × sizeof(SymbolState) ≈ 512 × 1096 = ~548 KB
    SymbolState _state[Config::kSymbols];
};

}  // namespace luv
