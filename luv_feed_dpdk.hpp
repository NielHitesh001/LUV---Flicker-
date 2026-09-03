#pragma once

// ╔══════════════════════════════════════════════════════════════════════════╗
// ║  LUV — Zero-Copy AI Inference Engine                                    ║
// ║  luv_feed_dpdk.hpp — DPDK-based market data feed source                 ║
// ║                                                                          ║
// ║  When LUV_USE_DPDK is defined at compile time:                          ║
// ║    - Initialises the DPDK EAL (Environment Abstraction Layer)           ║
// ║    - Configures one RX queue on the specified NIC port                   ║
// ║    - poll() calls rte_eth_rx_burst(), iterates mbufs, decodes each      ║
// ║      raw ITCH message through decode_itch(), writes to tick_ring        ║
// ║    - Frees mbufs after decoding                                          ║
// ║                                                                          ║
// ║  When LUV_USE_DPDK is NOT defined:                                      ║
// ║    - Provides a stub implementation whose init() always returns false   ║
// ║    - This allows the rest of the system to compile and link without     ║
// ║      DPDK libraries installed                                            ║
// ╚══════════════════════════════════════════════════════════════════════════╝

#include "luv_feed.hpp"
#include "luv_decode_itch.hpp"

#include <cstdint>
#include <cstring>

namespace luv {

// ─────────────────────────────────────────────────────────────────────────────
//  DpdkConfig — configuration for the DPDK feed source
// ─────────────────────────────────────────────────────────────────────────────
struct DpdkConfig {
    uint16_t port_id        = 0;      // DPDK port (NIC) to receive from
    uint16_t rx_queue_id    = 0;      // RX queue index on that port
    uint16_t burst_size     = 32;     // max mbufs per rte_eth_rx_burst()
    uint16_t nb_rx_desc     = 1024;   // number of RX descriptors
    uint32_t mempool_size   = 8191;   // mbuf pool size (should be 2^n - 1)
    uint16_t mbuf_cache     = 256;    // per-core mbuf cache
    uint16_t mtu            = 1500;   // MTU for the port

    // DPDK EAL arguments.
    // Caller must ensure these pointers remain valid through init().
    // Typical: { "luv", "-l", "0", "--proc-type=primary", "--log-level=5" }
    int         eal_argc    = 0;
    char**      eal_argv    = nullptr;

    // Payload offset: number of bytes from the start of the mbuf data
    // to the first ITCH message byte (typically UDP header size = 42 for
    // Ethernet + IPv4 + UDP, plus any MoldUDP64 header).
    //
    // For NASDAQ TotalView via MoldUDP64:
    //   Ethernet(14) + IP(20) + UDP(8) + MoldUDP64 header(20) = 62 bytes
    //   Then each message within the MoldUDP64 session block is:
    //     [uint16_t big-endian length] [raw ITCH payload]
    uint32_t    payload_offset = 62;
};

// ═══════════════════════════════════════════════════════════════════════════
//  DPDK-ENABLED BUILD
// ═══════════════════════════════════════════════════════════════════════════

#ifdef LUV_USE_DPDK

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

class DpdkFeedSource final : public IFeedSource {
public:
    DpdkFeedSource() = default;
    explicit DpdkFeedSource(const DpdkConfig& cfg) noexcept : _cfg(cfg) {}

    ~DpdkFeedSource() override {
        shutdown();
    }

    // No copy / move
    DpdkFeedSource(const DpdkFeedSource&) = delete;
    DpdkFeedSource& operator=(const DpdkFeedSource&) = delete;

    // ── IFeedSource interface ────────────────────────────────────────────

    // Access the symbol table to populate it before calling init().
    // The caller must insert all traded symbols (ticker → index) before
    // the first poll() call.
    [[nodiscard]] SymbolTable& symbols() noexcept { return _symbols; }

    [[nodiscard]] bool init(Arena& arena) noexcept override {
        if (!arena.is_initialised()) return false;
        _arena = &arena;

        // ── 1. Initialise DPDK EAL ──────────────────────────────────────
        int ret = rte_eal_init(_cfg.eal_argc, _cfg.eal_argv);
        if (ret < 0) return false;

        // ── 2. Validate port ────────────────────────────────────────────
        if (!rte_eth_dev_is_valid_port(_cfg.port_id)) return false;

        // ── 3. Create mbuf memory pool ──────────────────────────────────
        _mbuf_pool = rte_pktmbuf_pool_create(
            "LUV_MBUF_POOL",
            _cfg.mempool_size,
            _cfg.mbuf_cache,
            0,                              // priv_size
            RTE_MBUF_DEFAULT_BUF_SIZE,
            rte_socket_id()
        );
        if (!_mbuf_pool) return false;

        // ── 4. Configure the Ethernet device ────────────────────────────
        struct rte_eth_conf port_conf{};
        std::memset(&port_conf, 0, sizeof(port_conf));

        // Minimal configuration — no RSS, no offloads
        // Production may want to enable RSS, checksum offload, etc.
        port_conf.rxmode.mtu = _cfg.mtu;

        ret = rte_eth_dev_configure(_cfg.port_id,
                                    1,          // nb_rx_queues
                                    0,          // nb_tx_queues (we don't transmit)
                                    &port_conf);
        if (ret < 0) return false;

        // ── 5. Set up the RX queue ──────────────────────────────────────
        ret = rte_eth_rx_queue_setup(
            _cfg.port_id,
            _cfg.rx_queue_id,
            _cfg.nb_rx_desc,
            rte_eth_dev_socket_id(_cfg.port_id),
            nullptr,            // default RX conf
            _mbuf_pool
        );
        if (ret < 0) return false;

        // ── 6. Start the port ───────────────────────────────────────────
        ret = rte_eth_dev_start(_cfg.port_id);
        if (ret < 0) return false;

        // Enable promiscuous mode so we receive multicast market data
        rte_eth_promiscuous_enable(_cfg.port_id);

        _port_started = true;
        return true;
    }

    [[nodiscard]] uint32_t poll() noexcept override {
        // Burst receive from the NIC
        struct rte_mbuf* pkts[kMaxBurst];

        const uint16_t nb_rx = rte_eth_rx_burst(
            _cfg.port_id,
            _cfg.rx_queue_id,
            pkts,
            _cfg.burst_size > kMaxBurst
                ? kMaxBurst
                : _cfg.burst_size
        );

        if (nb_rx == 0) [[unlikely]] {
            return 0;
        }

        uint32_t decoded = 0;

        for (uint16_t i = 0; i < nb_rx; ++i) {
            decoded += process_mbuf(pkts[i]);
            rte_pktmbuf_free(pkts[i]);
        }

        return decoded;
    }

    [[nodiscard]] uint64_t total_messages() const noexcept override {
        return _total_msgs;
    }

    [[nodiscard]] uint64_t total_bytes() const noexcept override {
        return _total_bytes;
    }

private:
    // ── Constants ────────────────────────────────────────────────────────
    static constexpr uint16_t kMaxBurst = 64;

    // ── State ────────────────────────────────────────────────────────────
    DpdkConfig          _cfg{};
    Arena*              _arena        = nullptr;
    SymbolTable         _symbols{};
    struct rte_mempool* _mbuf_pool    = nullptr;
    bool                _port_started = false;

    uint64_t            _total_msgs   = 0;
    uint64_t            _total_bytes  = 0;

    // ── Process a single mbuf ────────────────────────────────────────────
    //
    // The mbuf contains a full Ethernet frame.  We skip past the
    // transport headers (Ethernet + IPv4 + UDP + MoldUDP64) and then
    // iterate over the ITCH messages within the MoldUDP64 session block.
    //
    // MoldUDP64 message block format (after the 20-byte MoldUDP64 header):
    //   Repeated:
    //     [uint16_t big-endian length] [raw ITCH message]
    //
    // We decode each ITCH message through decode_itch() and write the
    // result into the tick ring.

    uint32_t process_mbuf(struct rte_mbuf* mbuf) noexcept {
        // rte_pktmbuf_mtod exposes only the first segment.  Parsing a chained
        // mbuf as contiguous data would read beyond that segment.
        if (!rte_pktmbuf_is_contiguous(mbuf)) [[unlikely]] {
            return 0;
        }
        const uint32_t pkt_len = rte_pktmbuf_pkt_len(mbuf);
        if (pkt_len <= _cfg.payload_offset) [[unlikely]] {
            return 0;  // runt packet or header-only
        }

        const uint8_t* data = rte_pktmbuf_mtod(mbuf, const uint8_t*);
        const uint8_t* payload = data + _cfg.payload_offset;
        const uint32_t payload_len = pkt_len - _cfg.payload_offset;

        _total_bytes += pkt_len;

        // Iterate over MoldUDP64 message block
        uint32_t offset = 0;
        uint32_t decoded = 0;

        while (offset + 2 <= payload_len) {
            // Read 2-byte big-endian message length
            const uint16_t msg_len = static_cast<uint16_t>(
                (static_cast<uint16_t>(payload[offset]) << 8) |
                 payload[offset + 1]);

            offset += 2;

            if (msg_len == 0 || offset + msg_len > payload_len) {
                break;  // malformed or truncated
            }

            const uint8_t* raw_msg = payload + offset;

            // Claim a slot in the tick ring
            TickMsg* slot = _arena->tick_ring.try_claim();
            if (!slot) [[unlikely]] {
                // Ring full — drop remaining messages in this burst.
                // The consumer must keep up; this is the back-pressure signal.
                break;
            }

            if (decode_itch(raw_msg, msg_len, _symbols, *slot)) [[likely]] {
                _arena->tick_ring.commit();
                ++_total_msgs;
                ++decoded;
            }
            // If decode fails (e.g. message type we don't handle),
            // we simply don't commit the slot — it's a no-op.

            offset += msg_len;
        }

        return decoded;
    }

    void shutdown() noexcept {
        if (_port_started) {
            rte_eth_dev_stop(_cfg.port_id);
            rte_eth_dev_close(_cfg.port_id);
            _port_started = false;
        }
        // rte_eal_cleanup() should be called once globally, not per-source.
        // The caller is responsible for that.
    }
};

// ═══════════════════════════════════════════════════════════════════════════
//  STUB BUILD — DPDK NOT AVAILABLE
// ═══════════════════════════════════════════════════════════════════════════

#else  // !LUV_USE_DPDK

// Stub implementation when DPDK is not linked.
// init() always returns false; poll() is a no-op.
// This lets the rest of the system compile and link cleanly.
class DpdkFeedSource final : public IFeedSource {
public:
    DpdkFeedSource() = default;
    explicit DpdkFeedSource(const DpdkConfig& /*cfg*/) noexcept {}

    ~DpdkFeedSource() override = default;

    DpdkFeedSource(const DpdkFeedSource&) = delete;
    DpdkFeedSource& operator=(const DpdkFeedSource&) = delete;

    [[nodiscard]] bool init(Arena& /*arena*/) noexcept override {
        // DPDK is not available in this build.
        // Caller should use SimFeedSource or a kernel-based feed instead.
        return false;
    }

    [[nodiscard]] uint32_t poll() noexcept override {
        return 0;
    }

    [[nodiscard]] uint64_t total_messages() const noexcept override {
        return 0;
    }

    [[nodiscard]] uint64_t total_bytes() const noexcept override {
        return 0;
    }
};

#endif  // LUV_USE_DPDK

}  // namespace luv
