#pragma once

// ╔══════════════════════════════════════════════════════════════════════════╗
// ║  LUV — Zero-Copy AI Inference Engine                                    ║
// ║  luv_feed.hpp — Abstract feed source interface                          ║
// ║                                                                          ║
// ║  Every market-data source (simulated replay, DPDK NIC, kernel socket)   ║
// ║  implements IFeedSource.  The ingestion thread owns exactly one          ║
// ║  IFeedSource and calls poll() in a tight busy loop.                     ║
// ║                                                                          ║
// ║  Contract:                                                               ║
// ║    1. init() is called ONCE, before any poll().                          ║
// ║    2. poll() is called from a SINGLE pinned thread.                      ║
// ║    3. The feed writes decoded TickMsg into arena.tick_ring               ║
// ║       via try_claim() / commit().                                        ║
// ║    4. No dynamic allocation, no exceptions, no RTTI.                    ║
// ╚══════════════════════════════════════════════════════════════════════════╝

#include "luv_arena.hpp"

namespace luv {

// ─────────────────────────────────────────────────────────────────────────────
//  IFeedSource — polymorphic base for all feed backends
//
//  Vtable dispatch cost is negligible: poll() is called once per
//  busy-loop iteration, not once per message.  Inside poll(), the
//  implementation uses direct (non-virtual) calls for per-message work.
// ─────────────────────────────────────────────────────────────────────────────
class IFeedSource {
public:
    virtual ~IFeedSource() = default;

    // ── Initialisation ───────────────────────────────────────────────────
    //
    // Called once at startup.  The Arena must already be initialised
    // (arena.is_initialised() == true).  The feed stores a reference to
    // arena internally and writes decoded ticks into arena.tick_ring.
    //
    // Returns true on success.  On failure the feed is unusable.
    [[nodiscard]] virtual bool init(Arena& arena) noexcept = 0;

    // ── Hot-path polling ─────────────────────────────────────────────────
    //
    // Called from the ingestion thread's inner loop:
    //
    //     while (running) {
    //         feed->poll();
    //     }
    //
    // Returns the number of TickMsg written to the ring in this call.
    // Zero means no data was available (the caller should immediately
    // retry — no back-off is needed on a pinned core).
    [[nodiscard]] virtual uint32_t poll() noexcept = 0;

    // ── Diagnostics ──────────────────────────────────────────────────────
    //
    // Monotonically increasing counters.  Safe to read from any thread
    // (relaxed ordering is acceptable for telemetry).

    // Total number of messages successfully decoded and committed.
    [[nodiscard]] virtual uint64_t total_messages() const noexcept = 0;

    // Total raw bytes consumed from the wire / file / generator.
    [[nodiscard]] virtual uint64_t total_bytes() const noexcept = 0;
};

}  // namespace luv
