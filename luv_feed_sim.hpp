#pragma once

// ╔══════════════════════════════════════════════════════════════════════════╗
// ║  LUV — Zero-Copy AI Inference Engine                                    ║
// ║  luv_feed_sim.hpp — Simulated / replay feed source                      ║
// ║                                                                          ║
// ║  Two modes:                                                              ║
// ║    SYNTHETIC — Generates random ITCH-format messages in a pre-allocated ║
// ║                buffer, then decodes them through decode_itch() so the   ║
// ║                simulated path exercises the exact same decode code as   ║
// ║                production.                                               ║
// ║    REPLAY    — Memory-maps a binary file of raw ITCH messages and       ║
// ║                replays them through the same decode_itch() path.        ║
// ║                                                                          ║
// ║  After init(), zero dynamic allocation.  Rate limiting via              ║
// ║  clock_gettime(CLOCK_MONOTONIC) busy-spin.                              ║
// ╚══════════════════════════════════════════════════════════════════════════╝

#include "luv_feed.hpp"
#include "luv_decode_itch.hpp"

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace luv {

// ─────────────────────────────────────────────────────────────────────────────
//  SimConfig — all knobs for the simulated feed
// ─────────────────────────────────────────────────────────────────────────────
struct SimConfig {
    // Operating mode
    enum class Mode : uint8_t {
        kSynthetic = 0,   // Generate random ITCH messages
        kReplay    = 1,   // Replay from a binary file
    };

    Mode     mode              = Mode::kSynthetic;
    uint32_t synthetic_symbols = 128;        // how many distinct symbols to generate
    uint64_t target_rate_hz    = 1'000'000;  // target messages per second (0 = unlimited)
    uint64_t seed              = 0xDEAD'BEEF'CAFE'BABEull;  // PRNG seed
    uint32_t prebuf_count      = 1u << 16;   // 65536 pre-generated raw ITCH messages

    // Replay mode: path to a binary file of concatenated raw ITCH messages
    const char* replay_path    = nullptr;

    // Synthetic price/qty ranges (fixed-point ×10^4)
    int64_t  min_price         = 100'0000;   // $100.00
    int64_t  max_price         = 500'0000;   // $500.00
    int64_t  min_qty           = 1;
    int64_t  max_qty           = 10'000;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Minimal xoshiro256** PRNG — deterministic, no allocations, fast
// ─────────────────────────────────────────────────────────────────────────────
namespace detail {

struct Xoshiro256 {
    uint64_t s[4];

    explicit Xoshiro256(uint64_t seed) noexcept {
        // SplitMix64 seeding
        for (auto& v : s) {
            seed += 0x9E3779B97F4A7C15ull;
            uint64_t z = seed;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            v = z ^ (z >> 31);
        }
    }

    [[nodiscard]] uint64_t next() noexcept {
        const uint64_t result = rotl(s[1] * 5, 7) * 9;
        const uint64_t t = s[1] << 17;
        s[2] ^= s[0];  s[3] ^= s[1];  s[1] ^= s[2];  s[0] ^= s[3];
        s[2] ^= t;
        s[3] = rotl(s[3], 45);
        return result;
    }

    // Uniform in [lo, hi] inclusive
    [[nodiscard]] int64_t uniform(int64_t lo, int64_t hi) noexcept {
        const uint64_t range = static_cast<uint64_t>(hi - lo) + 1;
        return lo + static_cast<int64_t>(next() % range);
    }

    [[nodiscard]] uint32_t uniform_u32(uint32_t lo, uint32_t hi) noexcept {
        const uint32_t range = hi - lo + 1;
        return lo + static_cast<uint32_t>(next() % range);
    }

private:
    [[nodiscard]] static uint64_t rotl(uint64_t x, int k) noexcept {
        return (x << k) | (x >> (64 - k));
    }
};

}  // namespace detail

// Maximum raw ITCH message length we will generate
static constexpr uint32_t kSimMaxMsgLen = 50;

// ─────────────────────────────────────────────────────────────────────────────
//  SimFeedSource — implements IFeedSource
// ─────────────────────────────────────────────────────────────────────────────
class SimFeedSource final : public IFeedSource {
public:
    SimFeedSource() = default;
    explicit SimFeedSource(const SimConfig& cfg) noexcept : _cfg(cfg) {}

    ~SimFeedSource() override {
        if (_replay_base && _replay_base != MAP_FAILED) {
            ::munmap(const_cast<uint8_t*>(_replay_base), _replay_size);
        }
    }

    // No copy / move (holds mmap and arena reference)
    SimFeedSource(const SimFeedSource&) = delete;
    SimFeedSource& operator=(const SimFeedSource&) = delete;

    // ── IFeedSource interface ────────────────────────────────────────────

    [[nodiscard]] bool init(Arena& arena) noexcept override {
        if (!arena.is_initialised()) return false;
        _arena = &arena;

        if (_cfg.mode == SimConfig::Mode::kSynthetic) {
            return init_synthetic();
        } else {
            return init_replay();
        }
    }

    [[nodiscard]] uint32_t poll() noexcept override {
        if (_cfg.mode == SimConfig::Mode::kSynthetic) {
            return poll_synthetic();
        } else {
            return poll_replay();
        }
    }

    [[nodiscard]] uint64_t total_messages() const noexcept override {
        return _total_msgs;
    }

    [[nodiscard]] uint64_t total_bytes() const noexcept override {
        return _total_bytes;
    }

private:
    // ── Configuration and state ──────────────────────────────────────────
    SimConfig       _cfg{};
    Arena*          _arena         = nullptr;

    // Counters (written only by the ingestion thread)
    uint64_t        _total_msgs    = 0;
    uint64_t        _total_bytes   = 0;

    // ── Synthetic mode state ─────────────────────────────────────────────
    //
    // Pre-allocated buffer of raw ITCH-format messages.  At init time we
    // fill this buffer with realistic random messages.  At poll time we
    // index into this buffer round-robin and run each message through
    // decode_itch(), writing the result into tick_ring.

    // Raw message buffer — contiguous block, each entry is kSimMaxMsgLen bytes
    // (padded; actual message length stored in _msg_lengths[])
    uint8_t*        _raw_buf       = nullptr;   // carved from a single mmap
    uint32_t*       _msg_lengths   = nullptr;   // length of each message
    uint32_t        _prebuf_count  = 0;
    uint32_t        _prebuf_idx    = 0;         // round-robin cursor
    size_t          _raw_buf_total = 0;         // total mmap size

    detail::Xoshiro256 _rng{0};

    // Symbol table for decode_itch() — populated at init with synthetic tickers
    SymbolTable     _symbols{};

    // Rate limiting
    uint64_t        _ns_per_msg    = 0;         // 0 = unlimited
    uint64_t        _last_emit_ns  = 0;         // last message timestamp

    // ── Replay mode state ────────────────────────────────────────────────
    const uint8_t*  _replay_base   = nullptr;
    size_t          _replay_size   = 0;
    size_t          _replay_cursor = 0;

    // ── Synthetic initialisation ─────────────────────────────────────────

    [[nodiscard]] bool init_synthetic() noexcept {
        _rng = detail::Xoshiro256(_cfg.seed);
        _prebuf_count = _cfg.prebuf_count;

        // Compute rate limit
        if (_cfg.target_rate_hz > 0) {
            _ns_per_msg = 1'000'000'000ULL / _cfg.target_rate_hz;
        }

        // Populate symbol table with the same "S###    " tickers we generate
        for (uint32_t i = 0; i < _cfg.synthetic_symbols; ++i) {
            char ticker[9] = "        ";
            std::snprintf(ticker, sizeof(ticker), "S%03u    ", i);
            (void)_symbols.insert(ticker, static_cast<uint16_t>(i));
        }

        // Allocate raw message buffer + length array in one mmap
        const size_t msg_slab = static_cast<size_t>(_prebuf_count) * kSimMaxMsgLen;
        const size_t len_slab = static_cast<size_t>(_prebuf_count) * sizeof(uint32_t);
        _raw_buf_total = align_up(msg_slab + len_slab, 4096);

        void* mem = ::mmap(nullptr, _raw_buf_total,
                           PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED) return false;

        _raw_buf     = static_cast<uint8_t*>(mem);
        _msg_lengths = reinterpret_cast<uint32_t*>(_raw_buf + msg_slab);

        // Fill buffer with synthetic ITCH messages
        generate_synthetic_messages();

        // Initialise timestamp
        _last_emit_ns = now_ns();

        return true;
    }

    // ── Generate synthetic ITCH messages ─────────────────────────────────
    //
    // We build realistic-looking raw ITCH binary messages that will be
    // decoded through the standard decode_itch() path.  This ensures the
    // simulated path exercises the exact same code as production.
    //
    // ITCH 5.0 binary format (big-endian network byte order):
    //
    //   Add Order ('A', 36 bytes):
    //     [0]     msg_type      1 byte
    //     [1..2]  stock_locate  2 bytes  (uint16, big-endian)
    //     [3..4]  tracking_num  2 bytes
    //     [5..10] timestamp     6 bytes  (uint48, nanoseconds since midnight)
    //     [11..18] order_ref    8 bytes  (uint64)
    //     [19]    buy_sell      1 byte   ('B' or 'S')
    //     [20..23] shares       4 bytes  (uint32)
    //     [24..31] stock        8 bytes  (ASCII, space-padded)
    //     [32..35] price        4 bytes  (uint32, price × 10^4)
    //
    //   Order Executed ('E', 31 bytes):
    //     [0]     msg_type      1 byte
    //     [1..2]  stock_locate  2 bytes
    //     [3..4]  tracking_num  2 bytes
    //     [5..10] timestamp     6 bytes
    //     [11..18] order_ref    8 bytes
    //     [19..22] shares       4 bytes  (executed qty)
    //     [23..30] match_number 8 bytes
    //
    //   Order Cancel ('X', 23 bytes):
    //     [0]     msg_type      1 byte
    //     [1..2]  stock_locate  2 bytes
    //     [3..4]  tracking_num  2 bytes
    //     [5..10] timestamp     6 bytes
    //     [11..18] order_ref    8 bytes
    //     [19..22] cancelled_shares 4 bytes
    //
    //   Order Delete ('D', 19 bytes):
    //     [0]     msg_type      1 byte
    //     [1..2]  stock_locate  2 bytes
    //     [3..4]  tracking_num  2 bytes
    //     [5..10] timestamp     6 bytes
    //     [11..18] order_ref    8 bytes

    void generate_synthetic_messages() noexcept {
        uint64_t ts = 34'200'000'000'000ULL;  // 09:30:00.000 in nanoseconds
        uint64_t order_ref_seq = 1;

        for (uint32_t i = 0; i < _prebuf_count; ++i) {
            uint8_t* dst = _raw_buf + static_cast<size_t>(i) * kSimMaxMsgLen;
            std::memset(dst, 0, kSimMaxMsgLen);

            // Pick message type with realistic distribution:
            //   60% Add, 15% Execute, 15% Cancel, 10% Delete
            const uint32_t roll = _rng.uniform_u32(0, 99);
            uint32_t len = 0;

            if (roll < 60) {
                len = gen_add_order(dst, ts, order_ref_seq++);
            } else if (roll < 75) {
                len = gen_order_executed(dst, ts, order_ref_seq);
            } else if (roll < 90) {
                len = gen_order_cancel(dst, ts, order_ref_seq);
            } else {
                len = gen_order_delete(dst, ts, order_ref_seq);
            }

            _msg_lengths[i] = len;

            // Advance timestamp by a random 100ns–10µs gap
            ts += _rng.uniform(100, 10'000);
        }
    }

    // ── Big-endian encoding helpers ──────────────────────────────────────

    static void put_u16_be(uint8_t* p, uint16_t v) noexcept {
        p[0] = static_cast<uint8_t>(v >> 8);
        p[1] = static_cast<uint8_t>(v);
    }

    static void put_u32_be(uint8_t* p, uint32_t v) noexcept {
        p[0] = static_cast<uint8_t>(v >> 24);
        p[1] = static_cast<uint8_t>(v >> 16);
        p[2] = static_cast<uint8_t>(v >> 8);
        p[3] = static_cast<uint8_t>(v);
    }

    static void put_u48_be(uint8_t* p, uint64_t v) noexcept {
        p[0] = static_cast<uint8_t>(v >> 40);
        p[1] = static_cast<uint8_t>(v >> 32);
        p[2] = static_cast<uint8_t>(v >> 24);
        p[3] = static_cast<uint8_t>(v >> 16);
        p[4] = static_cast<uint8_t>(v >> 8);
        p[5] = static_cast<uint8_t>(v);
    }

    static void put_u64_be(uint8_t* p, uint64_t v) noexcept {
        p[0] = static_cast<uint8_t>(v >> 56);
        p[1] = static_cast<uint8_t>(v >> 48);
        p[2] = static_cast<uint8_t>(v >> 40);
        p[3] = static_cast<uint8_t>(v >> 32);
        p[4] = static_cast<uint8_t>(v >> 24);
        p[5] = static_cast<uint8_t>(v >> 16);
        p[6] = static_cast<uint8_t>(v >> 8);
        p[7] = static_cast<uint8_t>(v);
    }

    // Write the common ITCH header: msg_type, stock_locate, tracking_num, timestamp
    void write_itch_header(uint8_t* dst, uint8_t msg_type, uint16_t locate,
                           uint64_t ts) noexcept {
        dst[0] = msg_type;
        put_u16_be(dst + 1, locate);
        put_u16_be(dst + 3, 0);        // tracking number
        put_u48_be(dst + 5, ts);
    }

    // ── Individual message generators ────────────────────────────────────

    // Add Order ('A') — 36 bytes
    uint32_t gen_add_order(uint8_t* dst, uint64_t ts, uint64_t order_ref) noexcept {
        const uint16_t sym_idx = static_cast<uint16_t>(
            _rng.uniform_u32(0, _cfg.synthetic_symbols - 1));

        write_itch_header(dst, itch::kAddOrder, sym_idx, ts);
        put_u64_be(dst + 11, order_ref);
        dst[19] = (_rng.next() & 1) ? 'B' : 'S';

        const uint32_t shares = static_cast<uint32_t>(
            _rng.uniform(_cfg.min_qty, _cfg.max_qty));
        put_u32_be(dst + 20, shares);

        // Stock symbol — 8 bytes, ASCII, space-padded
        // Use "SYMnnnnn" pattern so symbols are identifiable
        char stock[9] = "        ";
        int n = std::snprintf(stock, sizeof(stock), "S%03u    ", sym_idx);
        (void)n;
        std::memcpy(dst + 24, stock, 8);

        const uint32_t price = static_cast<uint32_t>(
            _rng.uniform(_cfg.min_price, _cfg.max_price));
        put_u32_be(dst + 32, price);

        return 36;
    }

    // Order Executed ('E') — 31 bytes
    uint32_t gen_order_executed(uint8_t* dst, uint64_t ts,
                                uint64_t order_ref_max) noexcept {
        const uint16_t sym_idx = static_cast<uint16_t>(
            _rng.uniform_u32(0, _cfg.synthetic_symbols - 1));

        write_itch_header(dst, itch::kOrderExecuted, sym_idx, ts);

        // Pick a plausible existing order ref
        const uint64_t ref = (order_ref_max > 1)
            ? static_cast<uint64_t>(_rng.uniform(1, static_cast<int64_t>(order_ref_max - 1)))
            : 1;
        put_u64_be(dst + 11, ref);

        const uint32_t exec_shares = static_cast<uint32_t>(
            _rng.uniform(1, _cfg.max_qty / 2));
        put_u32_be(dst + 19, exec_shares);

        const uint64_t match_num = _rng.next();
        put_u64_be(dst + 23, match_num);

        return 31;
    }

    // Order Cancel ('X') — 23 bytes
    uint32_t gen_order_cancel(uint8_t* dst, uint64_t ts,
                              uint64_t order_ref_max) noexcept {
        const uint16_t sym_idx = static_cast<uint16_t>(
            _rng.uniform_u32(0, _cfg.synthetic_symbols - 1));

        write_itch_header(dst, itch::kOrderCancel, sym_idx, ts);

        const uint64_t ref = (order_ref_max > 1)
            ? static_cast<uint64_t>(_rng.uniform(1, static_cast<int64_t>(order_ref_max - 1)))
            : 1;
        put_u64_be(dst + 11, ref);

        const uint32_t cancel_shares = static_cast<uint32_t>(
            _rng.uniform(1, _cfg.max_qty));
        put_u32_be(dst + 19, cancel_shares);

        return 23;
    }

    // Order Delete ('D') — 19 bytes
    uint32_t gen_order_delete(uint8_t* dst, uint64_t ts,
                              uint64_t order_ref_max) noexcept {
        const uint16_t sym_idx = static_cast<uint16_t>(
            _rng.uniform_u32(0, _cfg.synthetic_symbols - 1));

        write_itch_header(dst, itch::kOrderDelete, sym_idx, ts);

        const uint64_t ref = (order_ref_max > 1)
            ? static_cast<uint64_t>(_rng.uniform(1, static_cast<int64_t>(order_ref_max - 1)))
            : 1;
        put_u64_be(dst + 11, ref);

        return 19;
    }

    // ── Synthetic poll ───────────────────────────────────────────────────
    //
    // On each call, we attempt to decode one pre-generated raw ITCH
    // message through the standard decode_itch() path and write the
    // resulting TickMsg into the tick ring.  Rate limiting is enforced
    // by comparing elapsed nanoseconds against _ns_per_msg.

    [[nodiscard]] uint32_t poll_synthetic() noexcept {
        // Rate limit check
        if (_ns_per_msg > 0) [[likely]] {
            const uint64_t now = now_ns();
            if (now - _last_emit_ns < _ns_per_msg) {
                return 0;  // too soon
            }
            _last_emit_ns = now;
        }

        // Claim a slot in the tick ring
        TickMsg* slot = _arena->tick_ring.try_claim();
        if (!slot) [[unlikely]] {
            return 0;  // ring full — consumer is behind
        }

        // Get the next pre-generated raw message (round-robin)
        const uint8_t* raw = _raw_buf +
            static_cast<size_t>(_prebuf_idx) * kSimMaxMsgLen;
        const uint32_t len = _msg_lengths[_prebuf_idx];

        _prebuf_idx = (_prebuf_idx + 1) % _prebuf_count;

        // Decode through the standard production path.
        // decode_itch() writes directly into *slot.
        const bool ok = decode_itch(raw, len, _symbols, *slot);

        if (ok) [[likely]] {
            _arena->tick_ring.commit();
            ++_total_msgs;
            _total_bytes += len;
            return 1;
        }

        // Decode failure on synthetic data is a bug, but we don't crash —
        // just skip the message and don't commit the slot.
        _total_bytes += len;
        return 0;
    }

    // ── Replay initialisation ────────────────────────────────────────────

    [[nodiscard]] bool init_replay() noexcept {
        if (!_cfg.replay_path) return false;

        int fd = ::open(_cfg.replay_path, O_RDONLY);
        if (fd < 0) return false;

        struct stat st{};
        if (::fstat(fd, &st) != 0) { ::close(fd); return false; }

        _replay_size = static_cast<size_t>(st.st_size);
        if (_replay_size == 0) { ::close(fd); return false; }

        void* mem = ::mmap(nullptr, _replay_size, PROT_READ,
                           MAP_PRIVATE, fd, 0);
        ::close(fd);

        if (mem == MAP_FAILED) return false;

        // Advise sequential access for read-ahead
        ::madvise(mem, _replay_size, MADV_SEQUENTIAL);

        _replay_base   = static_cast<const uint8_t*>(mem);
        _replay_cursor = 0;

        // Compute rate limit
        if (_cfg.target_rate_hz > 0) {
            _ns_per_msg = 1'000'000'000ULL / _cfg.target_rate_hz;
        }

        _last_emit_ns = now_ns();
        return true;
    }

    // ── Replay poll ──────────────────────────────────────────────────────
    //
    // The replay file format is a simple stream of:
    //   [uint16_t big-endian length] [raw ITCH message bytes]
    //
    // This matches the standard NASDAQ TotalView-ITCH file format where
    // each message is prefixed with a 2-byte big-endian length.

    [[nodiscard]] uint32_t poll_replay() noexcept {
        // Rate limit check
        if (_ns_per_msg > 0) [[likely]] {
            const uint64_t now = now_ns();
            if (now - _last_emit_ns < _ns_per_msg) {
                return 0;
            }
            _last_emit_ns = now;
        }

        // Check if we have at least the 2-byte length prefix remaining
        if (_replay_cursor + 2 > _replay_size) {
            // End of file — loop back to start for continuous replay
            _replay_cursor = 0;
            if (_replay_cursor + 2 > _replay_size) return 0;
        }

        // Read message length (big-endian uint16)
        const uint8_t* p = _replay_base + _replay_cursor;
        const uint16_t msg_len = static_cast<uint16_t>(
            (static_cast<uint16_t>(p[0]) << 8) | p[1]);

        if (msg_len == 0 || _replay_cursor + 2 + msg_len > _replay_size) {
            // Malformed or truncated — wrap around
            _replay_cursor = 0;
            return 0;
        }

        const uint8_t* raw = p + 2;

        // Claim a slot
        TickMsg* slot = _arena->tick_ring.try_claim();
        if (!slot) [[unlikely]] {
            return 0;  // ring full
        }

        const bool ok = decode_itch(raw, msg_len, _symbols, *slot);

        if (ok) [[likely]] {
            _arena->tick_ring.commit();
            ++_total_msgs;
        }

        _total_bytes += 2 + msg_len;
        _replay_cursor += 2 + msg_len;
        return ok ? 1 : 0;
    }

    // ── Clock helper ─────────────────────────────────────────────────────

    [[nodiscard]] static uint64_t now_ns() noexcept {
        struct timespec ts{};
        ::clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL
             + static_cast<uint64_t>(ts.tv_nsec);
    }
};

}  // namespace luv
