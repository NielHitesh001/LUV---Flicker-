#pragma once

// LUV execution and pre-trade risk gateway.
//
// The critical path is intentionally boring: integer comparisons, bitwise
// combination of pass bits, then fixed-offset binary packet patching. Setup can
// do richer work; order-time code must not allocate or build strings.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>

#include "luv_arena.hpp"

namespace luv {

namespace exec {

enum Side : uint8_t {
    kBuy = 0,
    kSell = 1,
};

enum Reject : uint8_t {
    kRejectNone = 0,
    kRejectQty = 1u << 0,
    kRejectPosition = 1u << 1,
    kRejectStaleAlpha = 1u << 2,
    kRejectHalted = 1u << 3,
    kRejectFlatSignal = 1u << 4,
    kRejectOrderCapacity = 1u << 5,
    kRejectInvalidSymbol = 1u << 6,
    kRejectPrice = 1u << 7,
};

struct RiskLimits {
    int64_t max_order_qty = 1'000;
    int64_t max_abs_position = 100'000;
    uint64_t max_alpha_age_ns = 250'000; // 250 us
};

struct OrderIntent {
    uint16_t symbol_idx = 0;
    uint8_t side = kBuy;
    int64_t qty = 0;
    int64_t price = 0; // fixed point x 10^4
    uint64_t alpha_timestamp_ns = 0;
    uint64_t now_ns = 0;
    uint32_t client_order_id = 0;
};

struct RiskDecision {
    uint8_t pass = 0;        // 1 = accepted, 0 = rejected
    uint8_t reject_mask = 0; // exec::Reject bits
};

// Minimal Nasdaq OUCH-style Enter Order packet. Exact production venues may
// need extra fields, but the design is the important part: all mutable fields
// are fixed-offset binary writes.
struct alignas(64) OutboundPacket {
    uint16_t len = 0;
    uint8_t bytes[64] = {};
};
static_assert(sizeof(OutboundPacket) == 128,
              "OutboundPacket stays cache-line aligned for NIC handoff");

namespace ouch {
inline constexpr uint16_t kEnterOrderLen = 48;
inline constexpr uint32_t kMaxSymbolText = 8;

inline constexpr size_t kMsgTypeOffset = 0;
inline constexpr size_t kTokenOffset = 1;     // uint32 be
inline constexpr size_t kSideOffset = 5;      // 'B' or 'S'
inline constexpr size_t kQtyOffset = 6;       // uint32 be
inline constexpr size_t kSymbolOffset = 10;   // 8 ASCII bytes
inline constexpr size_t kPriceOffset = 18;    // uint32 be
inline constexpr size_t kTimeInForceOffset = 22;
inline constexpr size_t kFirmOffset = 26;
inline constexpr size_t kDisplayOffset = 30;
inline constexpr size_t kCapacityOffset = 31;
inline constexpr size_t kIntermarketSweepOffset = 32;
inline constexpr size_t kMinQtyOffset = 33;   // uint32 be
inline constexpr size_t kCrossTypeOffset = 37;
inline constexpr size_t kCustomerTypeOffset = 38;
inline constexpr size_t kReservedOffset = 39;
}  // namespace ouch

inline void store_u32_be(uint8_t* dst, uint32_t v) noexcept {
    dst[0] = static_cast<uint8_t>(v >> 24);
    dst[1] = static_cast<uint8_t>(v >> 16);
    dst[2] = static_cast<uint8_t>(v >> 8);
    dst[3] = static_cast<uint8_t>(v);
}

inline uint32_t clamp_u32(int64_t v) noexcept {
    const uint8_t non_negative = static_cast<uint8_t>(v >= 0);
    const uint8_t fits_u32 = static_cast<uint8_t>(
        static_cast<uint64_t>(v) <= 0xFFFF'FFFFULL);
    const uint8_t ok = non_negative & fits_u32;
    const uint32_t clipped = (v < 0) ? 0u : 0xFFFF'FFFFu;
    return ok ? static_cast<uint32_t>(v) : clipped;
}

inline int64_t signed_qty_delta(uint8_t side, int64_t qty) noexcept {
    const int64_t sign_mask = -static_cast<int64_t>(side & 1u);
    return (qty ^ sign_mask) - sign_mask; // buy=+qty, sell=-qty
}

inline uint64_t abs_i64(int64_t v) noexcept {
    const uint64_t bits = static_cast<uint64_t>(v);
    const uint64_t sign = bits >> 63;
    return (bits ^ (0u - sign)) + sign;
}

}  // namespace exec

using OutboundPacket = exec::OutboundPacket;

class PreTradeRisk {
public:
    PreTradeRisk() = default;
    PreTradeRisk(const PreTradeRisk&) = delete;
    PreTradeRisk& operator=(const PreTradeRisk&) = delete;

    [[nodiscard]] bool init(Arena& arena) noexcept {
        if (!arena.is_initialised()) return false;
        _arena = &arena;
        for (auto& lim : _limits) lim = exec::RiskLimits{};
        return true;
    }

    void set_limits(uint16_t sym, const exec::RiskLimits& limits) noexcept {
        if (sym >= Config::kSymbols) return;
        _limits[sym] = limits;
    }

    [[nodiscard]] exec::RiskDecision evaluate(
        const exec::OrderIntent& intent) const noexcept
    {
        if (!_arena || intent.symbol_idx >= Config::kSymbols) [[unlikely]] {
            return {0, exec::kRejectInvalidSymbol};
        }
        const exec::RiskLimits& limits = _limits[intent.symbol_idx];
        const RiskState& risk = _arena->exec_states[intent.symbol_idx].risk;

        const uint8_t valid_qty =
            static_cast<uint8_t>((intent.qty > 0) &
                                 (intent.qty <= limits.max_order_qty));
        // Never feed an invalid signed quantity into arithmetic that could
        // overflow before the order is rejected.
        const int64_t safe_qty = valid_qty ? intent.qty : 0;
        const int64_t signed_delta = exec::signed_qty_delta(intent.side, safe_qty);
        int64_t projected = 0;
        const uint8_t position_add_ok = static_cast<uint8_t>(
            !__builtin_add_overflow(risk.net_position, signed_delta, &projected));
        const uint64_t age_ns = intent.now_ns - intent.alpha_timestamp_ns;

        const uint8_t valid_pos =
            static_cast<uint8_t>(position_add_ok &
                (limits.max_abs_position >= 0) &
                (exec::abs_i64(projected) <=
                 static_cast<uint64_t>(limits.max_abs_position)));
        const uint8_t valid_alpha =
            static_cast<uint8_t>(age_ns <= limits.max_alpha_age_ns);
        const uint8_t valid_halt =
            static_cast<uint8_t>(risk.halted == 0);
        const uint8_t valid_price = static_cast<uint8_t>(
            (intent.price > 0) &
            (static_cast<uint64_t>(intent.price) <= 0xFFFF'FFFFULL));
        int64_t order_notional = 0;
        const uint8_t notional_mul_ok = static_cast<uint8_t>(
            !__builtin_mul_overflow(intent.price, safe_qty, &order_notional));
        int64_t gross_after_order = 0;
        const uint8_t exposure_add_ok = static_cast<uint8_t>(
            !__builtin_add_overflow(risk.gross_exposure, order_notional,
                                    &gross_after_order));
        const uint8_t valid_notional = static_cast<uint8_t>(
            valid_price & notional_mul_ok & exposure_add_ok &
            (risk.gross_exposure >= 0));

        const uint8_t pass = valid_qty & valid_pos & valid_alpha & valid_halt &
                             valid_price & valid_notional;
        const uint8_t reject =
            static_cast<uint8_t>((valid_qty ^ 1u) * exec::kRejectQty) |
            static_cast<uint8_t>((valid_pos ^ 1u) * exec::kRejectPosition) |
            static_cast<uint8_t>((valid_alpha ^ 1u) * exec::kRejectStaleAlpha) |
            static_cast<uint8_t>((valid_halt ^ 1u) * exec::kRejectHalted) |
            static_cast<uint8_t>(((valid_price & valid_notional) ^ 1u) *
                                 exec::kRejectPrice);
        const uint8_t fail_mask = static_cast<uint8_t>(pass - 1u);

        return {pass, static_cast<uint8_t>(reject & fail_mask)};
    }

private:
    Arena* _arena = nullptr;
    exec::RiskLimits _limits[Config::kSymbols];
};

class OuchPacketTemplates {
public:
    [[nodiscard]] bool init() noexcept {
        for (uint16_t sym = 0; sym < Config::kSymbols; ++sym) {
            init_template(sym, exec::kBuy);
            init_template(sym, exec::kSell);
        }
        _initialised = true;
        return true;
    }

    [[nodiscard]] bool build_enter_order(
        const exec::OrderIntent& intent,
        OutboundPacket& out) const noexcept
    {
        if (!_initialised || intent.symbol_idx >= Config::kSymbols)
            return false;

        const uint8_t side = intent.side & 1u;
        const auto& tmpl = _templates[intent.symbol_idx][side];
        std::memcpy(out.bytes, tmpl.data(), exec::ouch::kEnterOrderLen);
        out.len = exec::ouch::kEnterOrderLen;

        exec::store_u32_be(out.bytes + exec::ouch::kTokenOffset,
                           intent.client_order_id);
        exec::store_u32_be(out.bytes + exec::ouch::kQtyOffset,
                           exec::clamp_u32(intent.qty));
        exec::store_u32_be(out.bytes + exec::ouch::kPriceOffset,
                           exec::clamp_u32(intent.price));
        return true;
    }

private:
    using Template = std::array<uint8_t, exec::ouch::kEnterOrderLen>;

    void init_template(uint16_t sym, uint8_t side) noexcept {
        Template& t = _templates[sym][side];
        t.fill(0);

        t[exec::ouch::kMsgTypeOffset] = 'O';
        t[exec::ouch::kSideOffset] = (side == exec::kBuy) ? 'B' : 'S';
        make_symbol(sym, t.data() + exec::ouch::kSymbolOffset);

        exec::store_u32_be(t.data() + exec::ouch::kTimeInForceOffset, 0);
        t[exec::ouch::kFirmOffset + 0] = 'L';
        t[exec::ouch::kFirmOffset + 1] = 'U';
        t[exec::ouch::kFirmOffset + 2] = 'V';
        t[exec::ouch::kFirmOffset + 3] = ' ';
        t[exec::ouch::kDisplayOffset] = 'Y';
        t[exec::ouch::kCapacityOffset] = 'A';
        t[exec::ouch::kIntermarketSweepOffset] = 'N';
        exec::store_u32_be(t.data() + exec::ouch::kMinQtyOffset, 0);
        t[exec::ouch::kCrossTypeOffset] = 'N';
        t[exec::ouch::kCustomerTypeOffset] = ' ';
    }

    static void make_symbol(uint16_t sym, uint8_t* dst) noexcept {
        char tmp[exec::ouch::kMaxSymbolText + 1] = "        ";
        std::snprintf(tmp, sizeof(tmp), "S%03u    ", static_cast<unsigned>(sym));
        std::memcpy(dst, tmp, exec::ouch::kMaxSymbolText);
    }

    Template _templates[Config::kSymbols][2];
    bool _initialised = false;
};

class ExecutionGateway {
public:
    [[nodiscard]] bool init(Arena& arena) noexcept {
        if (!arena.is_initialised()) return false;
        _arena = &arena;
        if (!_risk.init(arena)) return false;
        if (!_ouch.init()) return false;
        return true;
    }

    PreTradeRisk& risk() noexcept { return _risk; }
    const PreTradeRisk& risk() const noexcept { return _risk; }

    [[nodiscard]] exec::RiskDecision try_build(
        const exec::OrderIntent& intent,
        OutboundPacket& packet) noexcept
    {
        packet.len = 0;
        if (!_arena || intent.symbol_idx >= Config::kSymbols) [[unlikely]] {
            return {0, exec::kRejectInvalidSymbol};
        }
        exec::RiskDecision decision = _risk.evaluate(intent);
        const RiskState& risk = _arena->exec_states[intent.symbol_idx].risk;
        const uint8_t capacity_ok = static_cast<uint8_t>(
            risk.order_count < Config::kMaxActiveOrders);
        decision.pass &= capacity_ok;
        decision.reject_mask |= static_cast<uint8_t>(
            (capacity_ok ^ 1u) * exec::kRejectOrderCapacity);

        const bool built = decision.pass && _ouch.build_enter_order(intent, packet);
        const uint8_t built_bit = static_cast<uint8_t>(built);

        decision.pass &= built_bit;
        decision.reject_mask |= static_cast<uint8_t>(
            (built_bit ^ 1u) * exec::kRejectFlatSignal);

        const uint8_t pass_mask =
            static_cast<uint8_t>(-static_cast<int8_t>(decision.pass));
        packet.len = static_cast<uint16_t>(packet.len & pass_mask);

        if (decision.pass) [[likely]] {
            reserve_order_slot(intent);
        } else {
            ++_arena->exec_states[intent.symbol_idx].risk.reject_count;
        }

        return decision;
    }

private:
    void reserve_order_slot(const exec::OrderIntent& intent) noexcept {
        SymbolExecState& state = _arena->exec_states[intent.symbol_idx];
        const uint32_t slot =
            state.risk.order_count & (Config::kMaxActiveOrders - 1u);
        ActiveOrder& order = state.orders[slot];
        order.order_id = intent.client_order_id;
        order.price = intent.price;
        order.qty = intent.qty;
        order.filled_qty = 0;
        order.symbol_idx = intent.symbol_idx;
        order.side = intent.side & 1u;
        order.state = 1;

        state.risk.net_position +=
            exec::signed_qty_delta(intent.side, intent.qty);
        state.risk.gross_exposure += intent.price * intent.qty;
        ++state.risk.order_count;
    }

    Arena* _arena = nullptr;
    PreTradeRisk _risk;
    OuchPacketTemplates _ouch;
};

}  // namespace luv
