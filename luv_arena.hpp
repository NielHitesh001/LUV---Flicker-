#pragma once

// ╔══════════════════════════════════════════════════════════════════════════╗
// ║  LUV — Zero-Copy AI Inference Engine                                    ║
// ║  luv_arena.hpp — Pre-allocated memory arena                             ║
// ║                                                                          ║
// ║  ALL memory used by the hot path is carved from a single contiguous     ║
// ║  mmap region at startup.  After Arena::init() returns, the system       ║
// ║  never calls malloc / new / mmap again on the critical path.            ║
// ║                                                                          ║
// ║  Memory map (physical layout, low → high address):                      ║
// ║    [LOB slab]  [Tick ring]  [Feature slab]  [Signal slab]               ║
// ║    [Order/Risk slab]  [Telemetry SPSC]  [AI model region]               ║
// ╚══════════════════════════════════════════════════════════════════════════╝

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cassert>
#include <atomic>
#include <span>
#include <sys/mman.h>
#include <unistd.h>

namespace luv {

// ─────────────────────────────────────────────────────────────────────────────
//  Compile-time configuration
//  All capacity constants are powers of two so index masking replaces modulo.
// ─────────────────────────────────────────────────────────────────────────────
struct Config {
    // LOB dimensions
    static constexpr uint32_t kSymbols          = 512;   // power-of-two headroom over 128+
    static constexpr uint32_t kLevelsPerSide    = 1024;  // full depth-of-market
    static constexpr uint32_t kMaxOrdersPerLevel = 16;   // intrusive slots per price level

    // Tick ring buffer
    static constexpr uint32_t kTickCapacity     = 1u << 22;  // 4 194 304 slots
    static constexpr uint32_t kTickMask         = kTickCapacity - 1;

    // Feature extraction
    static constexpr uint32_t kFeaturesPerSymbol = 20;
    static constexpr uint32_t kLookbackSteps     = 64;

    // Signal output
    static constexpr uint32_t kSignalSlots       = kSymbols;  // one per symbol

    // Order / risk state
    static constexpr uint32_t kMaxActiveOrders   = 64;   // per symbol

    // Telemetry SPSC
    static constexpr uint32_t kTelemCapacity     = 1u << 16;  // 65 536 slots
    static constexpr uint32_t kTelemMask         = kTelemCapacity - 1;

    // AI model region (remainder after infrastructure)
    // Measured at boot; this is the compile-time ceiling used for mmap sizing.
    static constexpr size_t kAIBudgetBytes       = 13ULL * 1024 * 1024 * 1024;  // 13 GB
};

// ─────────────────────────────────────────────────────────────────────────────
//  Alignment helpers
// ─────────────────────────────────────────────────────────────────────────────
static constexpr size_t kCacheLine  = 64;
static constexpr size_t kPageSize   = 4096;
static constexpr size_t kHugePage   = 2 * 1024 * 1024;

[[nodiscard]] constexpr size_t align_up(size_t n, size_t align) noexcept {
    return (n + align - 1) & ~(align - 1);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Order slot — 32 bytes, intrusive doubly-linked list by index
//  Price level — one 576-byte cache-aligned slab per (symbol, side, level)
//
//  Header    :  24 bytes
//  Orders[]  :  16 × 32 = 512 bytes
//  Padding   :  40 bytes  → total = 576 bytes (9 cache lines)
// ─────────────────────────────────────────────────────────────────────────────
struct alignas(32) OrderSlot {
    uint64_t order_id   = 0;
    int64_t  qty        = 0;
    uint32_t flags      = 0;   // bit 0 = is_bid, bit 1 = is_active
    uint32_t pad0       = 0;
    int32_t  next_idx   = -1;  // index within the level's slot array, -1 = none
    int32_t  prev_idx   = -1;

    static constexpr uint32_t kFlagBid    = 1u << 0;
    static constexpr uint32_t kFlagActive = 1u << 1;

    [[nodiscard]] bool is_active() const noexcept { return (flags & kFlagActive) != 0; }
    [[nodiscard]] bool is_bid()    const noexcept { return (flags & kFlagBid)    != 0; }
};
static_assert(sizeof(OrderSlot) == 32, "OrderSlot must be 32 bytes");

// ─────────────────────────────────────────────────────────────────────────────
struct alignas(kCacheLine) PriceLevel {
    OrderSlot orders[Config::kMaxOrdersPerLevel];

    int64_t  price        = 0;
    int64_t  total_qty    = 0;
    int32_t  order_count  = 0;
    int32_t  head_idx     = -1;  // index of first active slot

    // Padding to reach 576 bytes (9 × 64)
    static constexpr size_t kHeaderBytes = 24; // size of fields after orders
    static constexpr size_t kRawBytes    = sizeof(orders) + kHeaderBytes; // 536
    static constexpr size_t kPadBytes    = align_up(kRawBytes, kCacheLine) - kRawBytes;
    uint8_t _pad[kPadBytes];

    void reset() noexcept {
        price = 0; total_qty = 0; order_count = 0; head_idx = -1;
        for (auto& o : orders) o = OrderSlot{};
    }

    // Returns index of a free slot, or -1 if level is full
    [[nodiscard]] int32_t alloc_slot() noexcept {
        for (int32_t i = 0; i < static_cast<int32_t>(Config::kMaxOrdersPerLevel); ++i)
            if (!orders[i].is_active()) return i;
        return -1;
    }
};
static_assert(sizeof(PriceLevel) == 576, "PriceLevel must be 576 bytes");

// ─────────────────────────────────────────────────────────────────────────────
//  Tick message — 64 bytes, one cache line, in-place ITCH decode target
// ─────────────────────────────────────────────────────────────────────────────
struct alignas(kCacheLine) TickMsg {
    uint8_t  msg_type    = 0;
    uint8_t  flags       = 0;
    uint16_t symbol_idx  = 0;  // resolved at decode time
    uint32_t pad0        = 0;
    uint64_t timestamp   = 0;  // nanoseconds since midnight
    int64_t  price       = 0;  // price × 10^4 (integer fixed-point)
    int64_t  qty         = 0;
    uint64_t order_ref   = 0;
    uint64_t match_num   = 0;
    uint8_t  _pad[16]{};       // reserved for future fields
};
static_assert(sizeof(TickMsg) == 64, "TickMsg must be one cache line");

// ─────────────────────────────────────────────────────────────────────────────
//  Feature vector — packed float32, SIMD-friendly layout
//
//  Layout per symbol (20 features × 64 steps = 1280 floats = 5120 bytes):
//    Features [0..4]  : mid-price delta (normalised), last 64 steps
//    Features [5..9]  : bid/ask imbalance ratio, last 64 steps
//    Features [10..14]: trade flow (signed volume), last 64 steps
//    Features [15..19]: spread in ticks, last 64 steps
//
//  The entire slab is 512 × 5120 = 2 621 440 bytes = 2.5 MB — fits in L3.
// ─────────────────────────────────────────────────────────────────────────────
struct FeatureRow {
    static constexpr uint32_t kFloats = Config::kFeaturesPerSymbol * Config::kLookbackSteps;
    float data[kFloats];  // 1280 floats = 5120 bytes
};
static_assert(sizeof(FeatureRow) == 5120);

// ─────────────────────────────────────────────────────────────────────────────
//  Signal output — 16 bytes per symbol, written by inference, read by risk
// ─────────────────────────────────────────────────────────────────────────────
struct alignas(16) SignalOutput {
    float    confidence    = 0.f;  // [0, 1]
    float    expected_move = 0.f;  // in ticks (signed)
    int8_t   direction     = 0;    // -1 short, 0 flat, +1 long
    uint8_t  model_id      = 0;    // which model produced this signal
    uint8_t  stale_count   = 0;    // ticks since last refresh
    uint8_t  _pad[5]{};
};
static_assert(sizeof(SignalOutput) == 16);

// ─────────────────────────────────────────────────────────────────────────────
//  Order state — per-symbol active order tracking (64 × 64 = 4096 bytes)
// ─────────────────────────────────────────────────────────────────────────────
struct alignas(kCacheLine) ActiveOrder {
    uint64_t order_id     = 0;
    int64_t  price        = 0;
    int64_t  qty          = 0;
    int64_t  filled_qty   = 0;
    uint32_t symbol_idx   = 0;
    uint8_t  side         = 0;    // 0 = bid, 1 = ask
    uint8_t  state        = 0;    // 0=pending, 1=live, 2=partial, 3=done
    uint8_t  _pad[18]{};
};
static_assert(sizeof(ActiveOrder) == 64);

struct RiskState {
    int64_t  net_position     = 0;
    int64_t  gross_exposure   = 0;
    int64_t  daily_pnl        = 0;    // fixed-point, ×10^4
    int64_t  max_drawdown     = 0;    // limit
    int64_t  current_drawdown = 0;
    uint32_t order_count      = 0;
    uint32_t reject_count     = 0;
    uint8_t  halted           = 0;    // non-zero = trading halted
    uint8_t  _pad[79]{};
};
static_assert(sizeof(RiskState) == 128);

struct SymbolExecState {
    ActiveOrder orders[Config::kMaxActiveOrders];  // 64 × 64 = 4096 bytes
    RiskState   risk;                              // 128 bytes
    uint8_t     _pad[128]{};                       // align next symbol to page
};
static_assert(sizeof(SymbolExecState) == (4096 + 128 + 128));

// ─────────────────────────────────────────────────────────────────────────────
//  Telemetry snapshot — 128 bytes, fire-and-forget from hot path
// ─────────────────────────────────────────────────────────────────────────────
struct alignas(kCacheLine) TelemSnapshot {
    uint64_t timestamp_ns    = 0;
    int64_t  session_pnl     = 0;   // ×10^4
    int64_t  gross_exposure  = 0;
    int32_t  fill_count      = 0;
    int32_t  reject_count    = 0;
    uint32_t tick_rate_hz    = 0;
    uint32_t active_orders   = 0;
    float    inference_us    = 0.f; // last inference latency
    float    risk_ns         = 0.f; // last risk check latency
    uint8_t  halted          = 0;
    uint8_t  _pad[55]{};
};
static_assert(sizeof(TelemSnapshot) == 128);

// ─────────────────────────────────────────────────────────────────────────────
//  SPSC ring — used for both tick ingestion and telemetry
//  head/tail are on separate cache lines to eliminate false sharing.
// ─────────────────────────────────────────────────────────────────────────────
template <typename T, uint32_t Capacity>
struct alignas(kCacheLine) SPSCRing {
    // Exactly one producer may call try_claim/commit and one consumer may
    // call try_peek/consume; violating this contract is a data race.
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of two");
    static constexpr uint32_t kMask = Capacity - 1;

    // Producer-side cache line
    alignas(kCacheLine) std::atomic<uint64_t> head{0};
    // Consumer-side cache line
    alignas(kCacheLine) std::atomic<uint64_t> tail{0};

    // Slots array — separate page-aligned allocation; set by Arena::init()
    T* slots = nullptr;

    // ── Producer (single writer) ──────────────────────────────────────────
    [[nodiscard]] T* try_claim() noexcept {
        const uint64_t h = head.load(std::memory_order_relaxed);
        if (h - tail.load(std::memory_order_acquire) >= Capacity) [[unlikely]]
            return nullptr;  // full
        return &slots[h & kMask];
    }

    void commit() noexcept {
        head.fetch_add(1, std::memory_order_release);
    }

    // ── Consumer (single reader) ──────────────────────────────────────────
    [[nodiscard]] T* try_peek() noexcept {
        const uint64_t t = tail.load(std::memory_order_relaxed);
        if (head.load(std::memory_order_acquire) == t) [[unlikely]]
            return nullptr;  // empty
        return &slots[t & kMask];
    }

    void consume() noexcept {
        tail.fetch_add(1, std::memory_order_release);
    }

    [[nodiscard]] uint64_t size() const noexcept {
        return head.load(std::memory_order_relaxed)
             - tail.load(std::memory_order_relaxed);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  Slab size constants (computed once, used by Arena::init)
// ─────────────────────────────────────────────────────────────────────────────
namespace slab_sizes {

constexpr size_t kLOB = static_cast<size_t>(Config::kSymbols)
                      * 2
                      * Config::kLevelsPerSide
                      * sizeof(PriceLevel);  // 603 979 776 bytes ≈ 576 MB

constexpr size_t kTickRing = static_cast<size_t>(Config::kTickCapacity)
                           * sizeof(TickMsg);  // 268 435 456 bytes ≈ 256 MB

constexpr size_t kFeatures = static_cast<size_t>(Config::kSymbols)
                           * sizeof(FeatureRow);  // 2 621 440 bytes ≈ 2.5 MB

constexpr size_t kSignals = static_cast<size_t>(Config::kSignalSlots)
                          * sizeof(SignalOutput);  // 8 192 bytes

constexpr size_t kExecState = static_cast<size_t>(Config::kSymbols)
                            * sizeof(SymbolExecState);  // ~2.2 MB

constexpr size_t kTelemetry = static_cast<size_t>(Config::kTelemCapacity)
                            * sizeof(TelemSnapshot);  // 8 388 608 bytes ≈ 8 MB

// Total infrastructure (rounded up to 2 MB huge-page boundary)
constexpr size_t kInfraRaw  = kLOB + kTickRing + kFeatures
                            + kSignals + kExecState + kTelemetry;
constexpr size_t kInfraTotal = align_up(kInfraRaw, kHugePage);

// AI model region — everything else up to 15.5 GB ceiling
// (leaves 0.5 GB for OS, stack, and dylibs)
constexpr size_t kAIRegion = Config::kAIBudgetBytes;

constexpr size_t kGrandTotal = kInfraTotal + kAIRegion;

}  // namespace slab_sizes

// ─────────────────────────────────────────────────────────────────────────────
//  Arena — the single ownership object for all LUV memory
//
//  Usage:
//      luv::Arena arena;
//      arena.init();          // one call at startup, never again
//      auto& lob = arena.lob; // zero-cost accessor
// ─────────────────────────────────────────────────────────────────────────────
class Arena {
public:
    // ── Primary views ────────────────────────────────────────────────────

    // lob[symbol][side][level]
    // side 0 = bid, side 1 = ask
    std::span<PriceLevel> lob_flat;   // full flat view

    std::span<TickMsg>       tick_slots;
    std::span<FeatureRow>    feature_rows;
    std::span<SignalOutput>  signal_slots;
    std::span<SymbolExecState> exec_states;
    std::span<TelemSnapshot> telem_slots;

    // SPSC ring heads (slots pointers wired to the spans above after init)
    SPSCRing<TickMsg,       Config::kTickCapacity>  tick_ring;
    SPSCRing<TelemSnapshot, Config::kTelemCapacity> telem_ring;

    // Raw pointer to the AI model region — passed to the inference layer
    void*  ai_region      = nullptr;
    size_t ai_region_size = 0;

    // ── Lifecycle ────────────────────────────────────────────────────────

    Arena() = default;
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    ~Arena() { teardown(); }

    // init() must be called exactly once before any hot-path code runs.
    // Returns true on success; false if mmap or mlock fails.
    [[nodiscard]] bool init() noexcept {
        if (_base) return false;  // already initialised

        const size_t total = slab_sizes::kGrandTotal;

        // Single mmap — huge-page backed where the kernel allows it
        void* mem = ::mmap(
            nullptr, total,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS,
            -1, 0
        );
        if (mem == MAP_FAILED) return false;

        // Request huge-page backing for the infrastructure slabs
        // (advisory; kernel may ignore on macOS without entitlements)
#ifdef MADV_HUGEPAGE
        ::madvise(mem, slab_sizes::kInfraTotal, MADV_HUGEPAGE);
#endif

        // Lock infrastructure region into physical RAM. Define
        // LUV_REQUIRE_MLOCK for deployments requiring deterministic residency.
        if (::mlock(mem, slab_sizes::kInfraTotal) != 0) {
    #ifdef LUV_REQUIRE_MLOCK
            ::munmap(mem, total);
            return false;
    #else
            _mlocked = false;  // log warning in production
    #endif
        } else {
            _mlocked = true;
        }

        _base       = static_cast<uint8_t*>(mem);
        _total_size = total;

        // ── Wire up typed spans ──────────────────────────────────────────
        uint8_t* cursor = _base;

        auto* lob_ptr = reinterpret_cast<PriceLevel*>(cursor);
        lob_flat      = {lob_ptr, slab_sizes::kLOB / sizeof(PriceLevel)};
        cursor       += slab_sizes::kLOB;

        auto* tick_ptr = reinterpret_cast<TickMsg*>(cursor);
        tick_slots     = {tick_ptr, Config::kTickCapacity};
        tick_ring.slots = tick_ptr;
        cursor        += slab_sizes::kTickRing;

        auto* feat_ptr = reinterpret_cast<FeatureRow*>(cursor);
        feature_rows   = {feat_ptr, Config::kSymbols};
        cursor        += slab_sizes::kFeatures;

        auto* sig_ptr = reinterpret_cast<SignalOutput*>(cursor);
        signal_slots  = {sig_ptr, Config::kSignalSlots};
        cursor       += slab_sizes::kSignals;

        auto* exec_ptr = reinterpret_cast<SymbolExecState*>(cursor);
        exec_states    = {exec_ptr, Config::kSymbols};
        cursor        += slab_sizes::kExecState;

        auto* telem_ptr = reinterpret_cast<TelemSnapshot*>(cursor);
        telem_slots     = {telem_ptr, Config::kTelemCapacity};
        telem_ring.slots = telem_ptr;
        cursor          += slab_sizes::kTelemetry;

        // Align cursor to huge-page boundary before AI region
        cursor = _base + slab_sizes::kInfraTotal;

        ai_region      = cursor;
        ai_region_size = slab_sizes::kAIRegion;

        // Zero infrastructure region (AI region left uninitialised until model load)
        std::memset(_base, 0, slab_sizes::kInfraTotal);

        _initialised = true;
        return true;
    }

    void teardown() noexcept {
        if (!_base) return;
        if (_mlocked) ::munlock(_base, slab_sizes::kInfraTotal);
        ::munmap(_base, _total_size);
        _base = nullptr;
        _initialised = false;
    }

    // ── Typed LOB accessors (zero-cost; arithmetic only) ─────────────────

    [[nodiscard]] PriceLevel& level(
        uint32_t symbol, uint8_t side, uint32_t depth) noexcept
    {
        // Layout: [symbol][side][depth]
        const size_t idx = (static_cast<size_t>(symbol) * 2 + side)
                         * Config::kLevelsPerSide + depth;
        return lob_flat[idx];
    }

    [[nodiscard]] const PriceLevel& level(
        uint32_t symbol, uint8_t side, uint32_t depth) const noexcept
    {
        const size_t idx = (static_cast<size_t>(symbol) * 2 + side)
                         * Config::kLevelsPerSide + depth;
        return lob_flat[idx];
    }

    [[nodiscard]] FeatureRow&    features(uint32_t sym)       noexcept { return feature_rows[sym]; }
    [[nodiscard]] SignalOutput&  signal(uint32_t sym)         noexcept { return signal_slots[sym]; }
    [[nodiscard]] SymbolExecState& exec(uint32_t sym)         noexcept { return exec_states[sym]; }

    // ── Diagnostics ──────────────────────────────────────────────────────

    struct MemoryReport {
        size_t lob_bytes;
        size_t tick_ring_bytes;
        size_t feature_bytes;
        size_t signal_bytes;
        size_t exec_bytes;
        size_t telem_bytes;
        size_t infra_total_bytes;
        size_t ai_region_bytes;
        size_t grand_total_bytes;
        bool   mlocked;
    };

    [[nodiscard]] MemoryReport report() const noexcept {
        return {
            slab_sizes::kLOB,
            slab_sizes::kTickRing,
            slab_sizes::kFeatures,
            slab_sizes::kSignals,
            slab_sizes::kExecState,
            slab_sizes::kTelemetry,
            slab_sizes::kInfraTotal,
            slab_sizes::kAIRegion,
            slab_sizes::kGrandTotal,
            _mlocked,
        };
    }

    [[nodiscard]] bool is_initialised() const noexcept { return _initialised; }
    [[nodiscard]] bool is_mlocked()     const noexcept { return _mlocked;      }

private:
    uint8_t* _base        = nullptr;
    size_t   _total_size  = 0;
    bool     _initialised = false;
    bool     _mlocked     = false;
};

}  // namespace luv
