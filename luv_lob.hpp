#pragma once

// ╔══════════════════════════════════════════════════════════════════════════╗
// ║  LUV — Zero-Copy AI Inference Engine                                     ║
// ║  luv_lob.hpp — Limit Order Book reconstruction engine                    ║
// ║                                                                          ║
// ║  Reconstructs a full depth-of-book from ITCH 5.0 TickMsg events.         ║
// ║  The book state lives in the Arena's LOB slab; this module maintains     ║
// ║  a separate mmap-allocated open-addressing hash map (OrderRefMap) to     ║
// ║  locate any order by its reference number in O(1) amortised time.        ║
// ║                                                                          ║
// ║  Design:                                                                 ║
// ║    • Header-only, all functions inline — no TU coupling                  ║
// ║    • No dynamic allocation after init, no exceptions, no RTTI            ║
// ║    • Price levels sorted: bids DESCENDING, asks ASCENDING (best=idx 0)   ║
// ║    • Level insert/remove uses memmove + OrderRefMap fixup                ║
// ║    • Replace ('U') uses order_ref=original, match_num=new ref            ║
// ║    • Trade ('P') is informational only — book modified by 'E' msgs       ║
// ╚══════════════════════════════════════════════════════════════════════════╝

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sys/mman.h>

#include "luv_arena.hpp"
#include "luv_decode_itch.hpp"

namespace luv {

// ─────────────────────────────────────────────────────────────────────────────
//  OrderLocation — compact descriptor for where an order lives in the LOB
//
//  Padded to 16 bytes so each hash entry accounts for 24 bytes total
//  (8-byte key + 16-byte value), matching the documented memory budget.
// ─────────────────────────────────────────────────────────────────────────────
struct OrderLocation {
    uint16_t symbol_idx;    // which symbol this order belongs to
    uint8_t  side;          // 0 = bid, 1 = ask
    uint8_t  _pad0 = 0;
    uint16_t level_idx;     // depth index within the side (0 = best)
    uint16_t _pad1 = 0;
    int32_t  slot_idx;      // OrderSlot index within the PriceLevel
    uint32_t _pad2 = 0;
};
static_assert(sizeof(OrderLocation) == 16, "OrderLocation must be 16 bytes");

// ─────────────────────────────────────────────────────────────────────────────
//  OrderRefMap — open-addressing hash map: uint64_t order_ref → OrderLocation
//
//  Uses Fibonacci hashing (same technique as SymbolTable in the decoder) with
//  linear probing.  Capacity covers every physical OrderSlot in the LOB slab.
//
//  The entire backing store is mmap-allocated at init time as a single
//  contiguous region (MAP_PRIVATE | MAP_ANONYMOUS) — completely separate from
//  the Arena.  No dynamic allocation occurs after init().
//
//  Capacity calculation:
//    kSymbols(512) × kLevelsPerSide(1024) × kMaxOrdersPerLevel(16)
//    = 8,388,608 possible active order slots across the full book.
// ─────────────────────────────────────────────────────────────────────────────
class OrderRefMap {
public:
    // Number of potential active orders across all symbols/levels/slots.
    static constexpr uint32_t kCapacity =
        Config::kSymbols * Config::kLevelsPerSide * Config::kMaxOrdersPerLevel;
    static constexpr size_t kBytes =
        static_cast<size_t>(kCapacity) * (sizeof(uint64_t) + sizeof(OrderLocation));
    static_assert((kCapacity & (kCapacity - 1)) == 0,
                  "OrderRefMap capacity must be power of two");
    static constexpr uint32_t kMask = kCapacity - 1;

    // Sentinel values
    static constexpr uint64_t kEmptyKey   = 0;         // order_ref 0 is never valid
    static constexpr uint64_t kTombstone  = ~uint64_t(0);  // deleted slot marker

    OrderRefMap() = default;
    ~OrderRefMap() { teardown(); }

    // Non-copyable, non-movable
    OrderRefMap(const OrderRefMap&) = delete;
    OrderRefMap& operator=(const OrderRefMap&) = delete;

    // ── Lifecycle ────────────────────────────────────────────────────────

    [[nodiscard]] bool init() noexcept {
        if (_keys) return false;  // already initialised

        const size_t keys_bytes   = static_cast<size_t>(kCapacity) * sizeof(uint64_t);
        const size_t values_bytes = static_cast<size_t>(kCapacity) * sizeof(OrderLocation);
        _mmap_size = keys_bytes + values_bytes;

        void* mem = ::mmap(
            nullptr, _mmap_size,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS,
            -1, 0
        );
        if (mem == MAP_FAILED) return false;

        _keys   = static_cast<uint64_t*>(mem);
        _values = reinterpret_cast<OrderLocation*>(
            static_cast<uint8_t*>(mem) + keys_bytes
        );

        // mmap with MAP_ANONYMOUS is zero-filled on Linux/macOS, which
        // conveniently sets all keys to kEmptyKey (0).  Explicit clear
        // for safety on exotic platforms:
        clear();

        return true;
    }

    void teardown() noexcept {
        if (!_keys) return;
        ::munmap(_keys, _mmap_size);
        _keys   = nullptr;
        _values = nullptr;
        _count  = 0;
    }

    // ── Clear all entries ─────────────────────────────────────────────────
    //  Resets every key to kEmptyKey (0).  O(N) but only called at init
    //  or full session reset.
    void clear() noexcept {
        std::memset(_keys, 0, static_cast<size_t>(kCapacity) * sizeof(uint64_t));
        // Values don't need clearing — they're only read when key matches.
        _count = 0;
    }

    // ── Insert / update ───────────────────────────────────────────────────
    //  Inserts a new mapping or updates an existing one.
    //  Returns true on success.  The table is sized to the physical maximum
    //  number of orders that can be represented in the LOB slab.
    bool insert(uint64_t order_ref, const OrderLocation& loc) noexcept {
        if (order_ref == kEmptyKey || order_ref == kTombstone ||
            loc.symbol_idx >= Config::kSymbols || loc.side > 1 ||
            loc.level_idx >= Config::kLevelsPerSide || loc.slot_idx < 0 ||
            loc.slot_idx >= static_cast<int32_t>(Config::kMaxOrdersPerLevel))
            return false;
        uint32_t slot = hash(order_ref);

        // Probe for an existing entry or an empty/tombstone slot.
        for (uint32_t probed = 0; probed < kCapacity; ++probed) {
            const uint64_t k = _keys[slot];
            if (k == order_ref) {
                // Update existing entry
                _values[slot] = loc;
                return true;
            }
            if (k == kEmptyKey || k == kTombstone) {
                // Insert into free slot
                _keys[slot]   = order_ref;
                _values[slot] = loc;
                ++_count;
                return true;
            }
            slot = (slot + 1) & kMask;
        }

        return false;
    }

    // ── Lookup ────────────────────────────────────────────────────────────
    //  Returns pointer to the OrderLocation if found, nullptr if not.
    //  The returned pointer is stable until the next insert/remove/clear.
    [[nodiscard]] OrderLocation* lookup(uint64_t order_ref) noexcept {
        uint32_t slot = hash(order_ref);

        for (uint32_t probed = 0; probed < kCapacity; ++probed) {
            const uint64_t k = _keys[slot];
            if (k == order_ref)  return &_values[slot];
            if (k == kEmptyKey)  return nullptr;  // not found
            // kTombstone: keep probing
            slot = (slot + 1) & kMask;
        }
        return nullptr;
    }

    [[nodiscard]] const OrderLocation* lookup(uint64_t order_ref) const noexcept {
        return const_cast<OrderRefMap*>(this)->lookup(order_ref);
    }

    // ── Remove ────────────────────────────────────────────────────────────
    //  Marks the slot as a tombstone so probe chains remain intact.
    void remove(uint64_t order_ref) noexcept {
        uint32_t slot = hash(order_ref);

        for (uint32_t probed = 0; probed < kCapacity; ++probed) {
            const uint64_t k = _keys[slot];
            if (k == order_ref) {
                _keys[slot] = kTombstone;
                --_count;
                return;
            }
            if (k == kEmptyKey) return;  // not found — no-op
            slot = (slot + 1) & kMask;
        }
    }

    [[nodiscard]] uint32_t size() const noexcept { return _count; }

    // ── Bulk update: fix level_idx for all orders on shifted levels ──────
    //  After inserting or removing a price level, the level_idx values for
    //  orders on levels that moved must be corrected.  This is O(N) over
    //  the map but level insertions/removals are relatively rare events
    //  (new price levels don't appear every tick).
    //
    //  direction = +1 means levels shifted DOWN (insert happened before them;
    //              their level_idx increased by 1)
    //  direction = -1 means levels shifted UP   (remove happened before them;
    //              their level_idx decreased by 1)
    void fixup_levels(uint16_t symbol_idx, uint8_t side,
                      uint16_t from_level, int direction) noexcept
    {
        for (uint32_t i = 0; i < kCapacity; ++i) {
            const uint64_t k = _keys[i];
            if (k == kEmptyKey || k == kTombstone) continue;

            OrderLocation& loc = _values[i];
            if (loc.symbol_idx == symbol_idx &&
                loc.side == side &&
                loc.level_idx >= from_level)
            {
                loc.level_idx = static_cast<uint16_t>(
                    static_cast<int>(loc.level_idx) + direction
                );
            }
        }
    }

private:
    // Fibonacci hash — same golden-ratio constant as SymbolTable.
    static constexpr uint64_t kFibMul = 0x9E3779B97F4A7C15ULL;
    static constexpr uint32_t kShift  = 64 - __builtin_ctz(kCapacity);

    [[nodiscard]] static uint32_t hash(uint64_t key) noexcept {
        key ^= key >> 30;
        key *= 0xBF58476D1CE4E5B9ULL;
        key ^= key >> 27;
        key *= 0x94D049BB133111EBULL;
        key ^= key >> 31;
        return static_cast<uint32_t>((key * kFibMul) >> kShift);
    }

    uint64_t*       _keys      = nullptr;
    OrderLocation*  _values    = nullptr;
    uint32_t        _count     = 0;
    size_t          _mmap_size = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
//  SymbolMeta — lightweight per-symbol bookkeeping
//
//  Tracks the number of active bid and ask price levels so that the LOB
//  engine can efficiently iterate only populated levels.
// ─────────────────────────────────────────────────────────────────────────────
struct SymbolMeta {
    uint16_t bid_levels = 0;   // number of active bid price levels
    uint16_t ask_levels = 0;   // number of active ask price levels
};

// ─────────────────────────────────────────────────────────────────────────────
//  LOBEngine — full depth-of-book reconstruction from ITCH TickMsg events
//
//  The engine operates on the Arena's pre-allocated LOB slab and maintains
//  a separately mmap-allocated OrderRefMap for O(1) order lookup.
//
//  Processing flow:
//    1. Decoder produces a TickMsg (in luv_decode_itch.hpp)
//    2. process(tick) dispatches to the appropriate handler
//    3. The handler modifies the Arena's PriceLevel/OrderSlot data in-place
//    4. Feature extraction reads the book state via the query accessors
// ─────────────────────────────────────────────────────────────────────────────
class LOBEngine {
public:
    LOBEngine() = default;
    ~LOBEngine() { teardown(); }

    // Non-copyable
    LOBEngine(const LOBEngine&) = delete;
    LOBEngine& operator=(const LOBEngine&) = delete;

    // ── Lifecycle ────────────────────────────────────────────────────────

    /// Initialise the LOB engine.  Must be called once after Arena::init().
    /// Allocates the OrderRefMap via a separate mmap.
    /// Returns false if the OrderRefMap allocation fails.
    [[nodiscard]] bool init(Arena& arena) noexcept {
        if (!arena.is_initialised()) return false;
        _arena = &arena;

        if (!_order_map.init()) return false;

        // Zero out per-symbol metadata
        std::memset(_meta, 0, sizeof(_meta));

        // Zero out statistics
        _stat_adds     = 0;
        _stat_executes = 0;
        _stat_cancels  = 0;
        _stat_deletes  = 0;
        _stat_replaces = 0;
        _stat_trades   = 0;

        return true;
    }

    /// Release the OrderRefMap memory.
    void teardown() noexcept {
        _order_map.teardown();
        _arena = nullptr;
    }

    // ── Main dispatch ────────────────────────────────────────────────────

    /// Process a single decoded TickMsg.  Dispatches to the appropriate
    /// handler based on msg_type.  Messages for unknown types are silently
    /// ignored.
    void process(const TickMsg& tick) noexcept {
        if (tick.symbol_idx >= Config::kSymbols) [[unlikely]] return;

        switch (tick.msg_type) {
        case itch::kAddOrder:       // 'A' — Add Order (no MPID)
        case itch::kAddOrderMPID:   // 'F' — Add Order (with MPID)
            if (tick.order_ref == 0 || tick.qty <= 0 || tick.price <= 0) return;
            on_add_order(tick);
            break;
        case itch::kOrderExecuted:  // 'E' — Order Executed
        case itch::kOrderExecPrice: // 'C' — Order Executed w/ Price
            if (tick.order_ref == 0 || tick.qty <= 0) return;
            on_order_executed(tick);
            break;
        case itch::kOrderCancel:    // 'X' — Order Cancel
            if (tick.order_ref == 0 || tick.qty <= 0) return;
            on_order_cancel(tick);
            break;
        case itch::kOrderDelete:    // 'D' — Order Delete
            if (tick.order_ref == 0) return;
            on_order_delete(tick);
            break;
        case itch::kOrderReplace:   // 'U' — Order Replace
            if (tick.order_ref == 0 || tick.match_num == 0 ||
                tick.order_ref == tick.match_num || tick.qty <= 0 ||
                tick.price <= 0) return;
            on_order_replace(tick);
            break;
        case itch::kTrade:          // 'P' — Trade (non-cross)
            if (tick.qty <= 0 || tick.price <= 0) return;
            on_trade(tick);
            break;
        default:
            break;  // silently ignore unhandled message types
        }
    }

    // ── Per-symbol book state queries ────────────────────────────────────
    //  These are called by the feature extraction layer and must be fast.
    //  They read directly from the Arena's LOB slab via the level() accessor.

    /// Best bid price (highest bid), or 0 if no bids.
    [[nodiscard]] int64_t best_bid_price(uint16_t sym) const noexcept {
        if (sym >= Config::kSymbols) return 0;
        if (_meta[sym].bid_levels == 0) return 0;
        return _arena->level(sym, 0, 0).price;  // bids sorted descending
    }

    /// Best ask price (lowest ask), or 0 if no asks.
    [[nodiscard]] int64_t best_ask_price(uint16_t sym) const noexcept {
        if (sym >= Config::kSymbols) return 0;
        if (_meta[sym].ask_levels == 0) return 0;
        return _arena->level(sym, 1, 0).price;  // asks sorted ascending
    }

    /// Mid-price = (best_bid + best_ask) / 2.  Returns 0 if either side empty.
    /// Note: returns integer division — caller may want ×2 for precision.
    [[nodiscard]] int64_t mid_price(uint16_t sym) const noexcept {
        const int64_t bid = best_bid_price(sym);
        const int64_t ask = best_ask_price(sym);
        if (bid == 0 || ask == 0) return 0;
        return (bid + ask) / 2;
    }

    /// Spread = best_ask - best_bid.  Returns 0 if either side empty.
    [[nodiscard]] int64_t spread(uint16_t sym) const noexcept {
        const int64_t bid = best_bid_price(sym);
        const int64_t ask = best_ask_price(sym);
        if (bid == 0 || ask == 0) return 0;
        return ask - bid;
    }

    /// Sum of total_qty across the top `levels` bid levels.
    [[nodiscard]] int64_t bid_depth_qty(uint16_t sym, uint32_t levels) const noexcept {
        if (sym >= Config::kSymbols) return 0;
        const uint32_t n = (levels < _meta[sym].bid_levels)
                         ? levels : _meta[sym].bid_levels;
        int64_t total = 0;
        for (uint32_t i = 0; i < n; ++i)
            total += _arena->level(sym, 0, i).total_qty;
        return total;
    }

    /// Sum of total_qty across the top `levels` ask levels.
    [[nodiscard]] int64_t ask_depth_qty(uint16_t sym, uint32_t levels) const noexcept {
        if (sym >= Config::kSymbols) return 0;
        const uint32_t n = (levels < _meta[sym].ask_levels)
                         ? levels : _meta[sym].ask_levels;
        int64_t total = 0;
        for (uint32_t i = 0; i < n; ++i)
            total += _arena->level(sym, 1, i).total_qty;
        return total;
    }

    /// Number of active bid price levels for a symbol.
    [[nodiscard]] uint16_t bid_level_count(uint16_t sym) const noexcept {
        if (sym >= Config::kSymbols) return 0;
        return _meta[sym].bid_levels;
    }

    /// Number of active ask price levels for a symbol.
    [[nodiscard]] uint16_t ask_level_count(uint16_t sym) const noexcept {
        if (sym >= Config::kSymbols) return 0;
        return _meta[sym].ask_levels;
    }

    // ── Statistics ───────────────────────────────────────────────────────

    [[nodiscard]] uint64_t total_adds()     const noexcept { return _stat_adds;     }
    [[nodiscard]] uint64_t total_executes() const noexcept { return _stat_executes; }
    [[nodiscard]] uint64_t total_cancels()  const noexcept { return _stat_cancels;  }
    [[nodiscard]] uint64_t total_deletes()  const noexcept { return _stat_deletes;  }
    [[nodiscard]] uint64_t total_replaces() const noexcept { return _stat_replaces; }
    [[nodiscard]] uint64_t total_trades()   const noexcept { return _stat_trades;   }

    [[nodiscard]] uint32_t active_order_count() const noexcept {
        return _order_map.size();
    }

    [[nodiscard]] static constexpr size_t order_ref_map_bytes() noexcept {
        return OrderRefMap::kBytes;
    }

    [[nodiscard]] static constexpr uint32_t order_ref_map_capacity() noexcept {
        return OrderRefMap::kCapacity;
    }

private:
    Arena*      _arena = nullptr;
    OrderRefMap _order_map;
    SymbolMeta  _meta[Config::kSymbols];

    // Counters
    uint64_t _stat_adds     = 0;
    uint64_t _stat_executes = 0;
    uint64_t _stat_cancels  = 0;
    uint64_t _stat_deletes  = 0;
    uint64_t _stat_replaces = 0;
    uint64_t _stat_trades   = 0;

    // ═════════════════════════════════════════════════════════════════════════
    //  Price Level Management
    // ═════════════════════════════════════════════════════════════════════════

    /// Get the active level count for a (symbol, side) pair.
    [[nodiscard]] uint16_t& level_count(uint16_t sym, uint8_t side) noexcept {
        return (side == 0) ? _meta[sym].bid_levels : _meta[sym].ask_levels;
    }
    [[nodiscard]] uint16_t level_count(uint16_t sym, uint8_t side) const noexcept {
        return (side == 0) ? _meta[sym].bid_levels : _meta[sym].ask_levels;
    }

    // ── find_level ───────────────────────────────────────────────────────
    //  Linear scan through active levels to find one with a matching price.
    //  Returns the level index, or -1 if not found.
    //
    //  Levels are sorted (bids descending, asks ascending), so we use
    //  binary search to find the correct level in O(log N) time, which
    //  improves performance significantly for deep books.
    [[nodiscard]] int32_t find_level(uint16_t sym, uint8_t side,
                                     int64_t price) const noexcept
    {
        const uint16_t count = level_count(sym, side);
        if (count == 0) return -1;

        int32_t low = 0;
        int32_t high = static_cast<int32_t>(count) - 1;

        if (side == 0) {
            // Bids: descending order
            while (low <= high) {
                const int32_t mid = low + (high - low) / 2;
                const int64_t mp = _arena->level(sym, side, static_cast<uint16_t>(mid)).price;
                if (mp == price) return mid;
                if (mp < price) {
                    high = mid - 1;
                } else {
                    low = mid + 1;
                }
            }
        } else {
            // Asks: ascending order
            while (low <= high) {
                const int32_t mid = low + (high - low) / 2;
                const int64_t mp = _arena->level(sym, side, static_cast<uint16_t>(mid)).price;
                if (mp == price) return mid;
                if (mp > price) {
                    high = mid - 1;
                } else {
                    low = mid + 1;
                }
            }
        }
        return -1;
    }

    // ── remove_level ─────────────────────────────────────────────────────
    //  Removes a level at the given index if its order_count == 0.
    //  Shifts all subsequent levels up by one and fixes OrderRefMap entries.
    void remove_level_if_empty(uint16_t sym, uint8_t side,
                               uint16_t lvl_idx) noexcept
    {
        PriceLevel& lvl = _arena->level(sym, side, lvl_idx);
        if (lvl.order_count > 0) return;  // still has orders

        uint16_t& count = level_count(sym, side);
        if (count == 0) return;  // shouldn't happen, but guard

        const uint16_t last = count - 1;

        if (lvl_idx < last) {
            // Shift levels [lvl_idx+1, last] up by one
            PriceLevel* base = &_arena->level(sym, side, 0);
            std::memmove(
                base + lvl_idx,
                base + lvl_idx + 1,
                static_cast<size_t>(last - lvl_idx) * sizeof(PriceLevel)
            );

            refresh_level_locations(sym, side, lvl_idx, last - 1);
        }

        // Clear the now-unused last level
        _arena->level(sym, side, last).reset();
        --count;
    }

    // ═════════════════════════════════════════════════════════════════════════
    //  Order Slot Management — intrusive doubly-linked list operations
    // ═════════════════════════════════════════════════════════════════════════

    /// Link a newly allocated slot into the level's order list at the tail.
    /// Updates head_idx and the slot's prev/next pointers.
    void link_slot(PriceLevel& lvl, int32_t slot_idx) noexcept {
        OrderSlot& slot = lvl.orders[slot_idx];
        slot.next_idx = -1;

        if (lvl.head_idx == -1) {
            // Empty list — this slot becomes the head
            slot.prev_idx = -1;
            lvl.head_idx  = slot_idx;
        } else {
            // Walk to the tail and append.
            // For kMaxOrdersPerLevel=16 this is at most 15 hops.
            int32_t cur = lvl.head_idx;
            while (lvl.orders[cur].next_idx != -1)
                cur = lvl.orders[cur].next_idx;

            lvl.orders[cur].next_idx = slot_idx;
            slot.prev_idx = cur;
        }
    }

    /// Unlink a slot from the level's order list.
    void unlink_slot(PriceLevel& lvl, int32_t slot_idx) noexcept {
        OrderSlot& slot = lvl.orders[slot_idx];

        if (slot.prev_idx != -1)
            lvl.orders[slot.prev_idx].next_idx = slot.next_idx;
        else
            lvl.head_idx = slot.next_idx;  // was the head

        if (slot.next_idx != -1)
            lvl.orders[slot.next_idx].prev_idx = slot.prev_idx;

        slot.prev_idx = -1;
        slot.next_idx = -1;
    }

    /// Deactivate a slot: clear the active flag and reset fields.
    void deactivate_slot(OrderSlot& slot) noexcept {
        slot.flags    = 0;
        slot.order_id = 0;
        slot.qty      = 0;
    }

    void refresh_level_locations(uint16_t sym, uint8_t side,
                                 uint16_t first, uint16_t last) noexcept
    {
        if (last < first) return;

        for (uint16_t level_idx = first; level_idx <= last; ++level_idx) {
            PriceLevel& lvl = _arena->level(sym, side, level_idx);
            for (int32_t slot_idx = 0;
                 slot_idx < static_cast<int32_t>(Config::kMaxOrdersPerLevel);
                 ++slot_idx)
            {
                OrderSlot& slot = lvl.orders[slot_idx];
                if (!slot.is_active()) continue;

                OrderLocation* loc = _order_map.lookup(slot.order_id);
                if (!loc) [[unlikely]] continue;

                loc->symbol_idx = sym;
                loc->side       = side;
                loc->level_idx  = level_idx;
                loc->slot_idx   = slot_idx;
            }
        }
    }

    [[nodiscard]] static int64_t clamped_qty(int64_t requested,
                                             int64_t available) noexcept
    {
        if (requested <= 0 || available <= 0) return 0;
        return (requested < available) ? requested : available;
    }

    [[nodiscard]] bool valid_location(uint64_t order_ref,
                                      const OrderLocation& loc) const noexcept
    {
        if (loc.symbol_idx >= Config::kSymbols || loc.side > 1 ||
            loc.level_idx >= level_count(loc.symbol_idx, loc.side) ||
            loc.slot_idx < 0 ||
            loc.slot_idx >= static_cast<int32_t>(Config::kMaxOrdersPerLevel))
            return false;

        const PriceLevel& lvl = _arena->level(
            loc.symbol_idx, loc.side, loc.level_idx);
        const OrderSlot& slot = lvl.orders[loc.slot_idx];
        return slot.is_active() && slot.order_id == order_ref &&
               slot.qty >= 0 && lvl.total_qty >= 0 && lvl.order_count > 0;
    }

    // ═════════════════════════════════════════════════════════════════════════
    //  Message Handlers
    // ═════════════════════════════════════════════════════════════════════════

    // ── Add Order (A/F) ──────────────────────────────────────────────────
    //
    //  1. Determine side from tick.flags & kBuy
    //  2. Find or create the price level at tick.price
    //  3. Allocate an OrderSlot, link it into the level's list
    //  4. Update level total_qty and order_count
    //  5. Insert into OrderRefMap
    void on_add_order(const TickMsg& tick) noexcept {
        ++_stat_adds;

        if (_order_map.lookup(tick.order_ref)) [[unlikely]] return;

        const uint16_t sym  = tick.symbol_idx;
        const uint8_t  side = (tick.flags & tick_flags::kBuy) ? 0 : 1;  // 0=bid, 1=ask
        const int64_t  price = tick.price;

        // Find existing level or insert a new one
        int32_t lvl_idx = find_level(sym, side, price);
        if (lvl_idx < 0) {
            lvl_idx = insert_level_impl(sym, side, price);
            if (lvl_idx < 0) [[unlikely]] return;  // max depth reached
        }

        PriceLevel& lvl = _arena->level(sym, side, static_cast<uint32_t>(lvl_idx));

        // Allocate a slot
        const int32_t slot_idx = lvl.alloc_slot();
        if (slot_idx < 0) [[unlikely]] return;  // level full

        // Initialise the slot
        OrderSlot& slot = lvl.orders[slot_idx];
        slot.order_id = tick.order_ref;
        slot.qty      = tick.qty;
        slot.flags    = OrderSlot::kFlagActive
                      | ((side == 0) ? OrderSlot::kFlagBid : 0u);

        // Link into the intrusive list
        link_slot(lvl, slot_idx);

        // Update level aggregates
        lvl.total_qty   += tick.qty;
        lvl.order_count += 1;

        // Register in the order-ref map
        OrderLocation loc;
        loc.symbol_idx = sym;
        loc.side       = side;
        loc.level_idx  = static_cast<uint16_t>(lvl_idx);
        loc.slot_idx   = slot_idx;
        if (!_order_map.insert(tick.order_ref, loc)) [[unlikely]] {
            unlink_slot(lvl, slot_idx);
            deactivate_slot(slot);
            lvl.total_qty   -= tick.qty;
            lvl.order_count -= 1;
            remove_level_if_empty(sym, side, static_cast<uint16_t>(lvl_idx));
        }
    }

    // ── Order Executed (E/C) ─────────────────────────────────────────────
    //
    //  Subtract executed qty from the order slot and level total.
    //  If the order is fully filled (qty == 0): deactivate, unlink, remove
    //  from map, and potentially remove the price level.
    void on_order_executed(const TickMsg& tick) noexcept {
        ++_stat_executes;

        OrderLocation* loc = _order_map.lookup(tick.order_ref);
        if (!loc || !valid_location(tick.order_ref, *loc)) [[unlikely]] return;

        PriceLevel& lvl = _arena->level(
            loc->symbol_idx, loc->side, loc->level_idx);
        OrderSlot& slot = lvl.orders[loc->slot_idx];

        // Subtract executed quantity.  Real ITCH should never overfill, but
        // clamping keeps simulated or corrupted data from poisoning totals.
        const int64_t delta = clamped_qty(tick.qty, slot.qty);
        slot.qty      -= delta;
        lvl.total_qty -= delta;

        if (slot.qty <= 0) {
            // Fully filled — deactivate and remove
            unlink_slot(lvl, loc->slot_idx);
            deactivate_slot(slot);
            lvl.order_count -= 1;

            const uint16_t sym  = loc->symbol_idx;
            const uint8_t  side = loc->side;
            const uint16_t li   = loc->level_idx;

            _order_map.remove(tick.order_ref);
            remove_level_if_empty(sym, side, li);
        }
    }

    // ── Order Cancel (X) ─────────────────────────────────────────────────
    //
    //  Subtract cancelled qty.  If qty reaches 0, same teardown as Execute.
    void on_order_cancel(const TickMsg& tick) noexcept {
        ++_stat_cancels;

        OrderLocation* loc = _order_map.lookup(tick.order_ref);
        if (!loc || !valid_location(tick.order_ref, *loc)) [[unlikely]] return;

        PriceLevel& lvl = _arena->level(
            loc->symbol_idx, loc->side, loc->level_idx);
        OrderSlot& slot = lvl.orders[loc->slot_idx];

        const int64_t delta = clamped_qty(tick.qty, slot.qty);
        slot.qty      -= delta;
        lvl.total_qty -= delta;

        if (slot.qty <= 0) {
            unlink_slot(lvl, loc->slot_idx);
            deactivate_slot(slot);
            lvl.order_count -= 1;

            const uint16_t sym  = loc->symbol_idx;
            const uint8_t  side = loc->side;
            const uint16_t li   = loc->level_idx;

            _order_map.remove(tick.order_ref);
            remove_level_if_empty(sym, side, li);
        }
    }

    // ── Order Delete (D) ─────────────────────────────────────────────────
    //
    //  Unconditionally remove the order: subtract its remaining qty,
    //  deactivate, unlink, remove from map, and clean up the level.
    void on_order_delete(const TickMsg& tick) noexcept {
        ++_stat_deletes;

        OrderLocation* loc = _order_map.lookup(tick.order_ref);
        if (!loc || !valid_location(tick.order_ref, *loc)) [[unlikely]] return;

        PriceLevel& lvl = _arena->level(
            loc->symbol_idx, loc->side, loc->level_idx);
        OrderSlot& slot = lvl.orders[loc->slot_idx];

        // Keep a corrupted aggregate from wrapping below zero.
        lvl.total_qty -= clamped_qty(slot.qty, lvl.total_qty);

        unlink_slot(lvl, loc->slot_idx);
        deactivate_slot(slot);
        lvl.order_count -= 1;

        const uint16_t sym  = loc->symbol_idx;
        const uint8_t  side = loc->side;
        const uint16_t li   = loc->level_idx;

        _order_map.remove(tick.order_ref);
        remove_level_if_empty(sym, side, li);
    }

    // ── Order Replace (U) ────────────────────────────────────────────────
    //
    //  In our TickMsg encoding (see luv_decode_itch.hpp):
    //    tick.order_ref = original order reference number
    //    tick.match_num = new order reference number (repurposed field)
    //    tick.qty       = new quantity
    //    tick.price     = new price
    //
    //  Semantics: delete the original order, then insert a new order with
    //  the new ref/qty/price on the same side as the original.
    void on_order_replace(const TickMsg& tick) noexcept {
        ++_stat_replaces;

        // Look up the original order to determine its side
        const OrderLocation* orig_loc = _order_map.lookup(tick.order_ref);
        if (!orig_loc || !valid_location(tick.order_ref, *orig_loc)) [[unlikely]] return;

        // Save the side before deleting (the pointer may be invalidated
        // after remove if it triggers a level removal)
        const uint8_t  saved_side = orig_loc->side;
        const uint16_t saved_sym  = orig_loc->symbol_idx;
        if (_order_map.lookup(tick.match_num) != nullptr) [[unlikely]] return;

        // ── Delete the original order ────────────────────────────────────
        {
            PriceLevel& lvl = _arena->level(
                orig_loc->symbol_idx, orig_loc->side, orig_loc->level_idx);
            OrderSlot& slot = lvl.orders[orig_loc->slot_idx];

            lvl.total_qty -= clamped_qty(slot.qty, lvl.total_qty);
            unlink_slot(lvl, orig_loc->slot_idx);
            deactivate_slot(slot);
            lvl.order_count -= 1;

            const uint16_t li = orig_loc->level_idx;
            _order_map.remove(tick.order_ref);
            remove_level_if_empty(saved_sym, saved_side, li);
        }

        // ── Insert the new order ─────────────────────────────────────────
        // Reuse the Add logic with the new ref, qty, and price.
        {
            const uint64_t new_ref = tick.match_num;
            const int64_t  price   = tick.price;
            const int64_t  qty     = tick.qty;

            int32_t lvl_idx = find_level(saved_sym, saved_side, price);
            if (lvl_idx < 0) {
                lvl_idx = insert_level_impl(saved_sym, saved_side, price);
                if (lvl_idx < 0) [[unlikely]] return;
            }

            PriceLevel& lvl = _arena->level(
                saved_sym, saved_side, static_cast<uint32_t>(lvl_idx));

            const int32_t slot_idx = lvl.alloc_slot();
            if (slot_idx < 0) [[unlikely]] return;

            OrderSlot& slot = lvl.orders[slot_idx];
            slot.order_id = new_ref;
            slot.qty      = qty;
            slot.flags    = OrderSlot::kFlagActive
                          | ((saved_side == 0) ? OrderSlot::kFlagBid : 0u);

            link_slot(lvl, slot_idx);
            lvl.total_qty   += qty;
            lvl.order_count += 1;

            OrderLocation loc;
            loc.symbol_idx = saved_sym;
            loc.side       = saved_side;
            loc.level_idx  = static_cast<uint16_t>(lvl_idx);
            loc.slot_idx   = slot_idx;
            if (!_order_map.insert(new_ref, loc)) [[unlikely]] {
                unlink_slot(lvl, slot_idx);
                deactivate_slot(slot);
                lvl.total_qty   -= qty;
                lvl.order_count -= 1;
                remove_level_if_empty(saved_sym, saved_side,
                                      static_cast<uint16_t>(lvl_idx));
            }
        }
    }

    // ── Trade (P) ────────────────────────────────────────────────────────
    //
    //  Informational only — does NOT modify the order book.  The LOB is
    //  updated by Execute ('E') messages, not Trade messages.  We just
    //  increment the trade counter.
    void on_trade(const TickMsg& tick) noexcept {
        (void)tick;  // unused — trade is purely informational
        ++_stat_trades;
    }

    /// Insert a new price level at the correct sorted position.
    /// Returns the level index, or -1 if max depth is reached.
    [[nodiscard]] int32_t insert_level_impl(uint16_t sym, uint8_t side,
                                             int64_t price) noexcept
    {
        uint16_t& count = level_count(sym, side);

        if (count >= Config::kLevelsPerSide) [[unlikely]]
            return -1;

        // ── Determine insertion point ────────────────────────────────────
        uint16_t insert_pos = count;  // default: append at end
        for (uint16_t i = 0; i < count; ++i) {
            const int64_t lp = _arena->level(sym, side, i).price;
            if (side == 0) {
                // Bids: descending — new price goes before first lower price
                if (price > lp) { insert_pos = i; break; }
            } else {
                // Asks: ascending — new price goes before first higher price
                if (price < lp) { insert_pos = i; break; }
            }
        }

        // ── Shift existing levels to make room ──────────────────────────
        if (insert_pos < count) {
            PriceLevel* base = &_arena->level(sym, side, 0);
            std::memmove(
                base + insert_pos + 1,
                base + insert_pos,
                static_cast<size_t>(count - insert_pos) * sizeof(PriceLevel)
            );

            refresh_level_locations(sym, side,
                                    static_cast<uint16_t>(insert_pos + 1),
                                    count);
        }

        // ── Initialise the new level ────────────────────────────────────
        PriceLevel& new_level = _arena->level(sym, side, insert_pos);
        new_level.reset();
        new_level.price = price;

        ++count;
        return static_cast<int32_t>(insert_pos);
    }
};

}  // namespace luv
