#pragma once

// ╔══════════════════════════════════════════════════════════════════════════╗
// ║  LUV — Zero-Copy AI Inference Engine                                    ║
// ║  luv_decode_itch.hpp — Stateless ITCH 5.0 binary decoder               ║
// ║                                                                          ║
// ║  Converts raw ITCH 5.0 wire bytes into TickMsg structs (defined in      ║
// ║  luv_arena.hpp).  The decoder is entirely stateless — order book state  ║
// ║  is maintained elsewhere.  This file provides:                          ║
// ║                                                                          ║
// ║    1. SymbolTable — open-addressing hash map (8-byte ticker → uint16)   ║
// ║    2. decode_itch() — the main decode entry point                       ║
// ║                                                                          ║
// ║  Design choices:                                                         ║
// ║    • Header-only, all functions inline — no TU coupling                 ║
// ║    • No dynamic allocation, no exceptions, no RTTI                      ║
// ║    • Fibonacci hashing for near-branchless symbol lookup                ║
// ║    • Correct on both big-endian and little-endian hosts                 ║
// ╚══════════════════════════════════════════════════════════════════════════╝

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "luv_arena.hpp"

namespace luv {

// ─────────────────────────────────────────────────────────────────────────────
//  Endian utilities
//
//  ITCH 5.0 is big-endian (network byte order).  On little-endian hosts we
//  must byte-swap multi-byte integers.  We detect endianness at compile time
//  and produce a no-op on big-endian machines.
// ─────────────────────────────────────────────────────────────────────────────
namespace detail {

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    inline constexpr bool kHostBigEndian = true;
#else
    inline constexpr bool kHostBigEndian = false;
#endif

[[nodiscard]] inline uint16_t be16(const void* p) noexcept {
    uint16_t v;
    std::memcpy(&v, p, 2);
    if constexpr (!kHostBigEndian) v = __builtin_bswap16(v);
    return v;
}

[[nodiscard]] inline uint32_t be32(const void* p) noexcept {
    uint32_t v;
    std::memcpy(&v, p, 4);
    if constexpr (!kHostBigEndian) v = __builtin_bswap32(v);
    return v;
}

[[nodiscard]] inline uint64_t be64(const void* p) noexcept {
    uint64_t v;
    std::memcpy(&v, p, 8);
    if constexpr (!kHostBigEndian) v = __builtin_bswap64(v);
    return v;
}

// ── 6-byte ITCH timestamp ──────────────────────────────────────────────────
//  Bytes 5–10 of every ITCH message encode nanoseconds since midnight as a
//  48-bit big-endian unsigned integer.  We load 8 bytes starting 2 bytes
//  before the timestamp, byte-swap the full 64 bits, then mask the low 48.
//
//  However, the timestamp starts at offset 5 — loading 8 bytes from offset 3
//  would be simpler but risks unaligned reads across the order_ref boundary.
//  Instead we load 6 bytes explicitly and reconstruct.
// ───────────────────────────────────────────────────────────────────────────
[[nodiscard]] inline uint64_t be_timestamp48(const uint8_t* p) noexcept {
    // p points to the first byte of the 6-byte timestamp field.
    // Layout:  p[0] p[1] p[2] p[3] p[4] p[5]  (MSB first)
    //
    // We reconstruct the 48-bit value by reading two aligned chunks:
    //   high16 = p[0..1]  →  bits [47:32]
    //   low32  = p[2..5]  →  bits [31:0]
    const uint64_t hi = be16(p);
    const uint64_t lo = be32(p + 2);
    return (hi << 32) | lo;
}

// ── Load 8-byte stock ticker as a uint64 for hash keying ──────────────────
//  ITCH stock fields are 8 ASCII bytes, right-padded with spaces (0x20).
//  We treat them as opaque 64-bit keys — no endian swap needed because
//  both insertion and lookup use the same raw representation.
// ───────────────────────────────────────────────────────────────────────────
[[nodiscard]] inline uint64_t load_ticker_key(const void* p) noexcept {
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}

}  // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
//  SymbolTable — open-addressing hash map: 8-byte ticker → uint16_t index
//
//  Capacity is the next power-of-two ≥ 2×Config::kSymbols to keep the load
//  factor ≤ 0.5 and guarantee short probe chains.
//
//  Hash function: Fibonacci hashing (multiply by golden-ratio constant,
//  shift right).  This distributes keys well for small alphabets and avoids
//  clustering that plagues simple modular hashing.
//
//  Lookup is "near-branchless": the while-loop body is a single comparison
//  with predictable branch direction (almost always taken on first probe for
//  load factors ≤ 0.5).
// ─────────────────────────────────────────────────────────────────────────────
class SymbolTable {
public:
    // Capacity must be a power of two.  2× kSymbols gives 1024, load ≤ 0.5.
    static constexpr uint32_t kCapacity = Config::kSymbols * 2;
    static_assert((kCapacity & (kCapacity - 1)) == 0,
                  "SymbolTable capacity must be power of two");
    static constexpr uint32_t kMask = kCapacity - 1;

    // Sentinel value: unused slot
    static constexpr uint16_t kNotFound = 0xFFFF;

    SymbolTable() noexcept { clear(); }

    // ── Clear all entries ─────────────────────────────────────────────────
    void clear() noexcept {
        for (uint32_t i = 0; i < kCapacity; ++i) {
            _keys[i]   = 0;
            _values[i] = kNotFound;
        }
        _count = 0;
    }

    // ── Insert a ticker → symbol_idx mapping ──────────────────────────────
    //  ticker: pointer to 8 ASCII bytes (right-padded with spaces)
    //  idx:    the symbol index to associate (must be < Config::kSymbols)
    //  Returns true on success, false if table is full.
    [[nodiscard]] bool insert(const void* ticker, uint16_t idx) noexcept {
        if (_count >= kCapacity / 2) return false;  // load factor > 0.5

        const uint64_t key = detail::load_ticker_key(ticker);
        uint32_t slot = hash(key);

        while (_values[slot] != kNotFound) {
            if (_keys[slot] == key) {
                // Duplicate — update in place
                _values[slot] = idx;
                return true;
            }
            slot = (slot + 1) & kMask;
        }

        _keys[slot]   = key;
        _values[slot] = idx;
        ++_count;
        return true;
    }

    // ── Lookup a ticker key (as uint64) ───────────────────────────────────
    //  Returns symbol_idx or kNotFound (0xFFFF) if not present.
    [[nodiscard]] inline uint16_t lookup(uint64_t key) const noexcept {
        uint32_t slot = hash(key);

        // Open-addressing linear probe.  At load ≤ 0.5 the expected number
        // of probes is < 1.5 — typically a single iteration.
        while (true) {
            const uint16_t val = _values[slot];
            if (val == kNotFound)   return kNotFound;  // empty slot → miss
            if (_keys[slot] == key) return val;         // hit
            slot = (slot + 1) & kMask;
        }
    }

    // ── Convenience: lookup from raw 8-byte ticker pointer ────────────────
    [[nodiscard]] inline uint16_t lookup(const void* ticker) const noexcept {
        return lookup(detail::load_ticker_key(ticker));
    }

    [[nodiscard]] uint32_t size() const noexcept { return _count; }

private:
    // Fibonacci hash: multiply by the golden-ratio constant for 64-bit,
    // then shift right to fold into [0, kCapacity).
    //
    //   φ⁻¹ × 2⁶⁴ ≈ 0x9E3779B97F4A7C15  (Knuth's multiplicative hash)
    //
    // The shift amount = 64 - log2(kCapacity).
    static constexpr uint64_t kFibMul = 0x9E3779B97F4A7C15ULL;
    static constexpr uint32_t kShift  = 64 - __builtin_ctz(kCapacity);

    [[nodiscard]] static uint32_t hash(uint64_t key) noexcept {
        return static_cast<uint32_t>((key * kFibMul) >> kShift);
    }

    uint64_t _keys  [kCapacity];
    uint16_t _values[kCapacity];
    uint32_t _count = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
//  TickMsg flag bits — encode buy/sell side
// ─────────────────────────────────────────────────────────────────────────────
namespace tick_flags {
    inline constexpr uint8_t kBuy        = 0x01;  // bit 0: 1=buy, 0=sell
    inline constexpr uint8_t kPrintable  = 0x02;  // bit 1: trade (has match_num)
}

// ─────────────────────────────────────────────────────────────────────────────
//  ITCH 5.0 message type constants
// ─────────────────────────────────────────────────────────────────────────────
namespace itch {
    inline constexpr uint8_t kAddOrder       = 'A';  // 0x41 — Add Order (no MPID)
    inline constexpr uint8_t kAddOrderMPID   = 'F';  // 0x46 — Add Order (with MPID)
    inline constexpr uint8_t kOrderExecuted  = 'E';  // 0x45 — Order Executed
    inline constexpr uint8_t kOrderExecPrice = 'C';  // 0x43 — Order Executed w/ Price
    inline constexpr uint8_t kOrderCancel    = 'X';  // 0x58 — Order Cancel
    inline constexpr uint8_t kOrderDelete    = 'D';  // 0x44 — Order Delete
    inline constexpr uint8_t kOrderReplace   = 'U';  // 0x55 — Order Replace
    inline constexpr uint8_t kTrade          = 'P';  // 0x50 — Trade (non-cross)

    // ── Minimum message lengths (bytes, AFTER the 2-byte length prefix) ──
    inline constexpr size_t kLenAddOrder      = 36;
    inline constexpr size_t kLenAddOrderMPID  = 40;  // 36 + 4 bytes MPID
    inline constexpr size_t kLenOrderExecuted = 31;
    inline constexpr size_t kLenOrderExecPrice = 36;
    inline constexpr size_t kLenOrderCancel   = 23;
    inline constexpr size_t kLenOrderDelete   = 19;
    inline constexpr size_t kLenOrderReplace  = 35;
    inline constexpr size_t kLenTrade         = 44;
}

// ─────────────────────────────────────────────────────────────────────────────
//  decode_itch() — Stateless ITCH 5.0 decoder
//
//  Parameters:
//    raw     — pointer to the ITCH message body (AFTER the 2-byte length prefix)
//    len     — number of bytes in the message body
//    symbols — pre-populated symbol table for ticker → index resolution
//    out     — output TickMsg (written on success, untouched on skip)
//
//  Returns:
//    true  — message decoded into `out`
//    false — message type not handled, or malformed (silently skipped)
//
//  Wire format byte offsets (all from message body start):
//
//  Common header (all message types):
//    Byte  0:     Message Type         (char)
//    Bytes 1–2:   Stock Locate         (uint16, big-endian)
//    Bytes 3–4:   Tracking Number      (uint16, big-endian)
//    Bytes 5–10:  Timestamp            (48-bit, big-endian, nanoseconds)
// ─────────────────────────────────────────────────────────────────────────────
[[nodiscard]] inline bool decode_itch(
    const uint8_t*    raw,
    size_t            len,
    const SymbolTable& symbols,
    TickMsg&          out) noexcept
{
    // ── Minimum length check (common header = 11 bytes) ──────────────────
    if (len < 11) [[unlikely]] return false;

    const uint8_t msg_type = raw[0];

    // ── Parse common header fields ───────────────────────────────────────
    //
    //  Byte 0:    msg_type  (already read)
    //  Bytes 1-2: stock_locate (uint16 big-endian)  — used as symbol_idx
    //             for messages without an explicit stock field
    //  Bytes 3-4: tracking_number (uint16, ignored)
    //  Bytes 5-10: timestamp (48-bit nanoseconds since midnight)
    //
    const uint64_t timestamp = detail::be_timestamp48(raw + 5);

    // Zero the output struct in one shot (64 bytes, one cache line)
    std::memset(&out, 0, sizeof(TickMsg));
    out.msg_type  = msg_type;
    out.timestamp = timestamp;

    switch (msg_type) {

    // ═══════════════════════════════════════════════════════════════════════
    //  Add Order — 'A' (36 bytes) / 'F' (40 bytes, +4 MPID appended)
    //
    //  Byte  0:      Message Type ('A' or 'F')
    //  Bytes 1–2:    Stock Locate
    //  Bytes 3–4:    Tracking Number
    //  Bytes 5–10:   Timestamp
    //  Bytes 11–18:  Order Reference Number   (uint64, big-endian)
    //  Byte  19:     Buy/Sell Indicator        ('B' = buy, 'S' = sell)
    //  Bytes 20–23:  Shares                   (uint32, big-endian)
    //  Bytes 24–31:  Stock                    (8 chars, right-padded spaces)
    //  Bytes 32–35:  Price                    (uint32, big-endian, ×10⁴)
    //  [F only] Bytes 36–39: MPID             (4 chars, ignored)
    // ═══════════════════════════════════════════════════════════════════════
    case itch::kAddOrder:
    case itch::kAddOrderMPID: {
        const size_t min_len = (msg_type == itch::kAddOrder)
                                 ? itch::kLenAddOrder
                                 : itch::kLenAddOrderMPID;
        if (len < min_len) [[unlikely]] return false;

        // Resolve symbol from the 8-byte stock ticker at offset 24
        const uint16_t sym_idx = symbols.lookup(raw + 24);
        if (sym_idx == SymbolTable::kNotFound) [[unlikely]] return false;

        out.symbol_idx = sym_idx;
        out.order_ref  = detail::be64(raw + 11);
        out.qty        = static_cast<int64_t>(detail::be32(raw + 20));
        out.price      = static_cast<int64_t>(detail::be32(raw + 32));
        out.flags      = (raw[19] == 'B') ? tick_flags::kBuy : 0;
        return true;
    }

    // ═══════════════════════════════════════════════════════════════════════
    //  Order Executed — 'E' (31 bytes)
    //
    //  Byte  0:      Message Type ('E')
    //  Bytes 1–2:    Stock Locate
    //  Bytes 3–4:    Tracking Number
    //  Bytes 5–10:   Timestamp
    //  Bytes 11–18:  Order Reference Number   (uint64, big-endian)
    //  Bytes 19–22:  Executed Shares           (uint32, big-endian)
    //  Bytes 23–30:  Match Number              (uint64, big-endian)
    // ═══════════════════════════════════════════════════════════════════════
    case itch::kOrderExecuted: {
        if (len < itch::kLenOrderExecuted) [[unlikely]] return false;

        // No stock field — use Stock Locate as symbol_idx
        out.symbol_idx = detail::be16(raw + 1);
        out.order_ref  = detail::be64(raw + 11);
        out.qty        = static_cast<int64_t>(detail::be32(raw + 19));
        out.match_num  = detail::be64(raw + 23);
        out.flags      = tick_flags::kPrintable;
        return true;
    }

    // ═══════════════════════════════════════════════════════════════════════
    //  Order Executed with Price — 'C' (36 bytes)
    //
    //  Byte  0:      Message Type ('C')
    //  Bytes 1–2:    Stock Locate
    //  Bytes 3–4:    Tracking Number
    //  Bytes 5–10:   Timestamp
    //  Bytes 11–18:  Order Reference Number   (uint64, big-endian)
    //  Bytes 19–22:  Executed Shares           (uint32, big-endian)
    //  Bytes 23–30:  Match Number              (uint64, big-endian)
    //  Byte  31:     Printable                 ('Y' or 'N')
    //  Bytes 32–35:  Execution Price           (uint32, big-endian, ×10⁴)
    // ═══════════════════════════════════════════════════════════════════════
    case itch::kOrderExecPrice: {
        if (len < itch::kLenOrderExecPrice) [[unlikely]] return false;

        out.symbol_idx = detail::be16(raw + 1);
        out.order_ref  = detail::be64(raw + 11);
        out.qty        = static_cast<int64_t>(detail::be32(raw + 19));
        out.match_num  = detail::be64(raw + 23);
        out.price      = static_cast<int64_t>(detail::be32(raw + 32));
        // Set printable flag if the printable field == 'Y'
        out.flags      = (raw[31] == 'Y') ? tick_flags::kPrintable : 0;
        return true;
    }

    // ═══════════════════════════════════════════════════════════════════════
    //  Order Cancel — 'X' (23 bytes)
    //
    //  Byte  0:      Message Type ('X')
    //  Bytes 1–2:    Stock Locate
    //  Bytes 3–4:    Tracking Number
    //  Bytes 5–10:   Timestamp
    //  Bytes 11–18:  Order Reference Number   (uint64, big-endian)
    //  Bytes 19–22:  Cancelled Shares          (uint32, big-endian)
    // ═══════════════════════════════════════════════════════════════════════
    case itch::kOrderCancel: {
        if (len < itch::kLenOrderCancel) [[unlikely]] return false;

        out.symbol_idx = detail::be16(raw + 1);
        out.order_ref  = detail::be64(raw + 11);
        out.qty        = static_cast<int64_t>(detail::be32(raw + 19));
        return true;
    }

    // ═══════════════════════════════════════════════════════════════════════
    //  Order Delete — 'D' (19 bytes)
    //
    //  Byte  0:      Message Type ('D')
    //  Bytes 1–2:    Stock Locate
    //  Bytes 3–4:    Tracking Number
    //  Bytes 5–10:   Timestamp
    //  Bytes 11–18:  Order Reference Number   (uint64, big-endian)
    // ═══════════════════════════════════════════════════════════════════════
    case itch::kOrderDelete: {
        if (len < itch::kLenOrderDelete) [[unlikely]] return false;

        out.symbol_idx = detail::be16(raw + 1);
        out.order_ref  = detail::be64(raw + 11);
        return true;
    }

    // ═══════════════════════════════════════════════════════════════════════
    //  Order Replace — 'U' (35 bytes)
    //
    //  Byte  0:      Message Type ('U')
    //  Bytes 1–2:    Stock Locate
    //  Bytes 3–4:    Tracking Number
    //  Bytes 5–10:   Timestamp
    //  Bytes 11–18:  Original Order Ref Number (uint64, big-endian)
    //  Bytes 19–26:  New Order Ref Number      (uint64, big-endian)
    //  Bytes 27–30:  Shares                    (uint32, big-endian)
    //  Bytes 31–34:  Price                     (uint32, big-endian, ×10⁴)
    //
    //  We store the ORIGINAL order ref in order_ref and the NEW order ref
    //  in match_num (repurposed — the consumer must be aware of this).
    // ═══════════════════════════════════════════════════════════════════════
    case itch::kOrderReplace: {
        if (len < itch::kLenOrderReplace) [[unlikely]] return false;

        out.symbol_idx = detail::be16(raw + 1);
        out.order_ref  = detail::be64(raw + 11);  // original ref
        out.match_num  = detail::be64(raw + 19);  // new ref (packed here)
        out.qty        = static_cast<int64_t>(detail::be32(raw + 27));
        out.price      = static_cast<int64_t>(detail::be32(raw + 31));
        return true;
    }

    // ═══════════════════════════════════════════════════════════════════════
    //  Trade (non-cross) — 'P' (44 bytes)
    //
    //  Byte  0:      Message Type ('P')
    //  Bytes 1–2:    Stock Locate
    //  Bytes 3–4:    Tracking Number
    //  Bytes 5–10:   Timestamp
    //  Bytes 11–18:  Order Reference Number   (uint64, big-endian)
    //  Byte  19:     Buy/Sell Indicator        ('B' or 'S')
    //  Bytes 20–23:  Shares                   (uint32, big-endian)
    //  Bytes 24–31:  Stock                    (8 chars, right-padded spaces)
    //  Bytes 32–35:  Price                    (uint32, big-endian, ×10⁴)
    //  Bytes 36–43:  Match Number             (uint64, big-endian)
    // ═══════════════════════════════════════════════════════════════════════
    case itch::kTrade: {
        if (len < itch::kLenTrade) [[unlikely]] return false;

        // Resolve symbol from the 8-byte stock ticker at offset 24
        const uint16_t sym_idx = symbols.lookup(raw + 24);
        if (sym_idx == SymbolTable::kNotFound) [[unlikely]] return false;

        out.symbol_idx = sym_idx;
        out.order_ref  = detail::be64(raw + 11);
        out.qty        = static_cast<int64_t>(detail::be32(raw + 20));
        out.price      = static_cast<int64_t>(detail::be32(raw + 32));
        out.match_num  = detail::be64(raw + 36);
        out.flags      = tick_flags::kPrintable
                       | ((raw[19] == 'B') ? tick_flags::kBuy : 0);
        return true;
    }

    // ── All other message types: silently skip ───────────────────────────
    default:
        return false;
    }
}

}  // namespace luv
