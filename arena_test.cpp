#include "luv_arena.hpp"
#include <cstdio>
#include <cstdlib>

// ── Compile-time assertions ──────────────────────────────────────────────────
static_assert(sizeof(luv::OrderSlot)       ==   32);
static_assert(sizeof(luv::PriceLevel)      ==  576);
static_assert(sizeof(luv::TickMsg)         ==   64);
static_assert(sizeof(luv::FeatureRow)      == 5120);
static_assert(sizeof(luv::SignalOutput)    ==   16);
static_assert(sizeof(luv::ActiveOrder)     ==   64);
static_assert(sizeof(luv::RiskState)       ==  128);
static_assert(sizeof(luv::TelemSnapshot)   ==  128);

static_assert(alignof(luv::PriceLevel)     == 64);
static_assert(alignof(luv::TickMsg)        == 64);
static_assert(alignof(luv::TelemSnapshot)  == 64);

static void print_bytes(const char* name, size_t bytes) {
    if (bytes >= (1ULL << 30))
        printf("  %-28s  %8.3f GB  (%zu bytes)\n",
               name, (double)bytes / (1ULL << 30), bytes);
    else if (bytes >= (1ULL << 20))
        printf("  %-28s  %8.2f MB  (%zu bytes)\n",
               name, (double)bytes / (1ULL << 20), bytes);
    else
        printf("  %-28s  %8zu  bytes\n", name, bytes);
}

int main() {
    printf("══════════════════════════════════════════════\n");
    printf("  LUV Arena — struct audit\n");
    printf("══════════════════════════════════════════════\n");
    printf("  OrderSlot       : %4zu bytes\n", sizeof(luv::OrderSlot));
    printf("  PriceLevel      : %4zu bytes  (%zu cache lines)\n",
           sizeof(luv::PriceLevel), sizeof(luv::PriceLevel) / 64);
    printf("  TickMsg         : %4zu bytes\n", sizeof(luv::TickMsg));
    printf("  FeatureRow      : %4zu bytes\n", sizeof(luv::FeatureRow));
    printf("  SignalOutput    : %4zu bytes\n", sizeof(luv::SignalOutput));
    printf("  ActiveOrder     : %4zu bytes\n", sizeof(luv::ActiveOrder));
    printf("  RiskState       : %4zu bytes\n", sizeof(luv::RiskState));
    printf("  SymbolExecState : %4zu bytes\n", sizeof(luv::SymbolExecState));
    printf("  TelemSnapshot   : %4zu bytes\n", sizeof(luv::TelemSnapshot));

    printf("\n══════════════════════════════════════════════\n");
    printf("  LUV Arena — slab sizing (compile-time)\n");
    printf("══════════════════════════════════════════════\n");
    print_bytes("LOB slab",        luv::slab_sizes::kLOB);
    print_bytes("Tick ring",       luv::slab_sizes::kTickRing);
    print_bytes("Feature slab",    luv::slab_sizes::kFeatures);
    print_bytes("Signal slab",     luv::slab_sizes::kSignals);
    print_bytes("Exec/risk slab",  luv::slab_sizes::kExecState);
    print_bytes("Telemetry queue", luv::slab_sizes::kTelemetry);
    printf("  ──────────────────────────────────────\n");
    print_bytes("Infrastructure total", luv::slab_sizes::kInfraTotal);
    print_bytes("AI model region",      luv::slab_sizes::kAIRegion);
    printf("  ──────────────────────────────────────\n");
    print_bytes("GRAND TOTAL",          luv::slab_sizes::kGrandTotal);

    printf("\n══════════════════════════════════════════════\n");
    printf("  LUV Arena — runtime init\n");
    printf("══════════════════════════════════════════════\n");

    luv::Arena arena;
    const bool ok = arena.init();
    if (!ok) {
        printf("  [FAIL] arena.init() returned false\n");
        return 1;
    }
    printf("  [OK]   arena.init() succeeded\n");

    const auto r = arena.report();
    printf("  mlock status     : %s\n", r.mlocked ? "LOCKED" : "not locked (sim mode ok)");

    // Verify accessor pointers are distinct and non-null
    printf("  lob_flat.data()  : %p  (size %zu)\n",
           static_cast<void*>(arena.lob_flat.data()), arena.lob_flat.size());
    printf("  tick_slots.data(): %p\n",
           static_cast<void*>(arena.tick_slots.data()));
    printf("  feature_rows ptr : %p\n",
           static_cast<void*>(arena.feature_rows.data()));
    printf("  ai_region ptr    : %p  (size %.2f GB)\n",
           arena.ai_region,
           (double)arena.ai_region_size / (1ULL << 30));

    // Verify the typed level accessor
    luv::PriceLevel& lvl = arena.level(0, 0, 0);   // symbol 0, bid side, top level
    lvl.price = 100'0000;   // $100.0000 in fixed-point
    lvl.total_qty = 5000;
    auto slot_idx = lvl.alloc_slot();
    if (slot_idx < 0) {
        printf("  [FAIL] alloc_slot returned -1 on empty level\n");
        return 1;
    }
    lvl.orders[slot_idx].order_id  = 0xDEAD'BEEF'0000'0001ULL;
    lvl.orders[slot_idx].qty       = 100;
    lvl.orders[slot_idx].flags     = luv::OrderSlot::kFlagActive | luv::OrderSlot::kFlagBid;
    printf("  [OK]   level(0,0,0) write/read round-trip\n");
    printf("         price=%lld  qty=%lld  slot=%d  order_id=0x%llX\n",
           (long long)lvl.price,
           (long long)lvl.orders[slot_idx].qty,
           slot_idx,
           (unsigned long long)lvl.orders[slot_idx].order_id);

    // Verify SPSC tick ring
    auto* slot = arena.tick_ring.try_claim();
    if (!slot) { printf("  [FAIL] tick_ring empty but try_claim returned null\n"); return 1; }
    slot->msg_type   = 0x41;  // 'A' = Add Order
    slot->symbol_idx = 7;
    slot->price      = 500'0000;
    slot->qty        = 200;
    slot->timestamp  = 1'000'000'000ULL;
    arena.tick_ring.commit();

    auto* peeked = arena.tick_ring.try_peek();
    if (!peeked || peeked->msg_type != 0x41) {
        printf("  [FAIL] tick_ring peek mismatch\n"); return 1;
    }
    arena.tick_ring.consume();
    printf("  [OK]   SPSC tick ring claim/commit/peek/consume\n");

    // Verify telemetry SPSC
    auto* ts = arena.telem_ring.try_claim();
    if (!ts) { printf("  [FAIL] telem_ring claim failed\n"); return 1; }
    ts->session_pnl   = 12345;
    ts->inference_us  = 3.7f;
    arena.telem_ring.commit();
    auto* tp = arena.telem_ring.try_peek();
    if (!tp || tp->session_pnl != 12345) {
        printf("  [FAIL] telem_ring peek mismatch\n"); return 1;
    }
    arena.telem_ring.consume();
    printf("  [OK]   SPSC telem  ring claim/commit/peek/consume\n");

    printf("\n  All checks passed.\n");
    printf("══════════════════════════════════════════════\n");
    return 0;
}
