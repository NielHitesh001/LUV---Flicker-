# LUV---Flicker-

Low-latency market microstructure engine for Nasdaq equity trading. Ingests ITCH protocol market data via DPDK kernel-bypass, maintains an in-memory limit order book, and executes trades with microsecond-scale latency.

## Features

- **DPDK-based packet processing** — kernel bypass for ultra-low-latency network I/O (polling mode drivers, hugepages)
- **ITCH 5.0 protocol decoder** — parses Nasdaq TotalView-ITCH binary market data (order book depth, trades, system events)
- **Lock-free limit order book** — in-memory order matching for equity instruments with pre-allocated memory
- **Memory arena allocator** — pre-allocated pools eliminate runtime allocation latency in the hot path
- **Execution engine** — order routing with configurable transmission modes
- **Telemetry & observability** — performance metrics collection (latency histograms, throughput, resource usage)
- **Simulation mode** — test harness with synthetic market data feed (no DPDK/network required)

## Architecture

```
Network (Nasdaq ITCH feed)
         ↓
   DPDK Feed Handler (kernel bypass)
         ↓
   ITCH Protocol Decoder
         ↓
   Limit Order Book (lock-free, pre-allocated)
         ↓
   Execution Engine (order routing)
         ↓
   Telemetry (metrics export)
```

### Core Components

| Module | Purpose | Thread Model |
|--------|---------|--------------|
| `luv_feed.hpp` | Base feed interface | N/A (abstract) |
| `luv_feed_dpdk.hpp` | DPDK packet processor | Single consumer thread, polling mode |
| `luv_feed_sim.hpp` | Synthetic market feed | Single thread, simulated time |
| `luv_decode_itch.hpp` | ITCH 5.0 binary decoder | Single thread (called by feed handler) |
| `luv_lob.hpp` | Limit order book | Reader-writer lock (readers = data consumers, writer = ITCH decoder) |
| `luv_execution.hpp` | Order execution engine | Single thread, enqueued mutations from LOB |
| `luv_arena.hpp` | Pre-allocated memory pool | Thread-safe up to pre-allocated size, fails hard on exhaustion |
| `luv_telemetry.hpp` | Performance metrics | Lock-free ring buffer for event recording |
| `luv_consumer.hpp` | Generic data consumer interface | N/A (abstract) |
| `luv_features.hpp` | Feature flags & configuration | Read-only after startup |

## Thread Safety Model

**Single-threaded event loop design**:

1. **ITCH decoder thread** (writer): sole mutator of the LOB
2. **LOB**: the ITCH decoder is the sole caller that mutates or processes the book. Query accessors are read-only but must not run concurrently with mutation unless the application supplies synchronization.
3. **Execution engine** (single-threaded): enqueued from LOB mutations, transmits orders
4. **Telemetry** (lock-free): ring buffer; no blocking on critical path
5. **Arena allocator**: pre-allocated; all allocations must fit or the system fails fast (no heap fragmentation)

**No unbounded heap allocations** on the critical path. All data structures use the arena allocator.

The tick and telemetry queues are strict SPSC rings: exactly one producer may
claim/commit and exactly one consumer may peek/consume each ring. Build with
`-DLUV_REQUIRE_MLOCK=ON` for production deployments where failure to lock the
infrastructure arena into RAM must abort startup; the default is best-effort
locking for simulation environments.

## Build

### Dependencies

- **C++17 compiler** (clang-14+ or g++-11+)
- **DPDK 21.11+** (for production DPDK feed; optional if using simulation)
- **GNU Make**

### Quickstart

```bash
# Simulation mode (no DPDK, no network)
make sim

# Production build (requires DPDK)
make DPDK_ROOT=/path/to/dpdk

# Run tests
make test

# Clean
make clean
```

### Environment Variables

```bash
export DPDK_ROOT=/usr/local/dpdk           # DPDK installation directory
export ITCH_FEED=dpdk                      # 'dpdk' or 'sim'
export LOB_PREALLOC_SIZE=1048576           # Arena allocator size (bytes)
export EXECUTION_MODE=live                 # 'live' or 'paper'
```

## Usage

### Basic Example: Simulation

```cpp
#include "luv_feed_sim.hpp"
#include "luv_lob.hpp"
#include "luv_execution.hpp"

int main() {
    // Create components
    LuvArena arena(1024 * 1024);  // 1 MB pre-allocated
    LuvLOB lob(arena);
    LuvSimFeed feed(lob);
    LuvExecution execution(lob);

    // Run simulation
    while (feed.next()) {
        // Feed consumes ITCH events and mutates LOB
        // Execution engine processes mutations
        // Telemetry records latencies
    }

    return 0;
}
```

### Monitoring Telemetry

```cpp
auto latency_p50 = telemetry.latency_percentile(50);
auto latency_p99 = telemetry.latency_percentile(99);
auto throughput = telemetry.events_per_second();

std::cout << "Order processing: p50=" << latency_p50 << "us, p99=" 
          << latency_p99 << "us, throughput=" << throughput << " evt/s\n";
```

## Performance

Typical latencies (from test suite, with DPDK on modern hardware):

| Operation | p50 | p99 | p99.9 |
|-----------|-----|-----|-------|
| Add order to LOB | 0.5 µs | 1.2 µs | 2.1 µs |
| Order execution | 1.1 µs | 2.3 µs | 4.5 µs |
| Full feed→exec roundtrip | 2.8 µs | 5.6 µs | 9.2 µs |

**Conditions:** 8-core machine, DPDK hugepages, affinity pinning, ~500 symbols, 10k orders/sec feed rate.

## Deployment

### Production Checklist

- [ ] Arena allocator sized for peak order count + 30% headroom
- [ ] DPDK hugepages configured and reserved
- [ ] CPU affinity pinning enabled for feed + execution threads
- [ ] Telemetry output wired to monitoring system
- [ ] Kill switch (circuit breaker) integrated
- [ ] Order validation + risk limits enforced upstream
- [ ] Audit logging of all executions enabled
- [ ] Failover/redundancy strategy documented

### Known Limitations

1. **Single-machine deployment** — no built-in clustering or failover
2. **Pre-allocation is hard limit** — LOB and arena cannot grow beyond configured size
3. **ITCH only** — other market data formats not supported
4. **Equity instruments only** — no derivatives, crypto, commodities
5. **Order types** — market and limit only; no conditional logic

### Failure Modes

| Condition | Behavior | Recovery |
|-----------|----------|----------|
| Arena exhausted | Fast fail, no new orders accepted | Restart (requires reload) |
| ITCH feed stall | LOB becomes stale; execution blocked | Automatic reconnect (see config) |
| Execution transmission timeout | Order marked as failed; logged | Manual intervention required |

## Configuration

Create a config file (example: `config.json`):

```json
{
  "arena": {
    "size_bytes": 10485760,
    "alignment": 64
  },
  "dpdk": {
    "enabled": true,
    "nic_port": 0,
    "queue_depth": 256,
    "hugepages_2mb": 128
  },
  "itch": {
    "multicast_addr": "239.1.1.1",
    "port": 14310,
    "interface": "eth0"
  },
  "execution": {
    "mode": "paper",
    "max_order_size": 1000000,
    "transmission_timeout_us": 500
  },
  "telemetry": {
    "enabled": true,
    "ring_size": 1048576,
    "export_interval_ms": 1000
  }
}
```

## AI Component (luv_ai.hpp)

**Status: Research prototype, not used in production path.**

This module explores machine-learning-based order prediction and execution optimization. It is **disabled by default** and should **not be enabled in live trading** without extensive validation.

Current capabilities:
- Experimental latency prediction model
- Prototype execution timing optimizer
- Research-only; no guarantees on correctness or safety

To disable (default):
```cpp
#define LUV_AI_ENABLED 0
```

## Testing

```bash
# Run all tests
make test

# Individual test suites
./test_arena        # Memory allocator tests
./test_lob          # Order book correctness
./test_feed         # Feed processing (simulation)
./test_execution    # Order routing
./test_stress       # Load & concurrency
./test_ai_telemetry # Telemetry + AI integration
```

Expected output:
```
test_arena: PASS (1000 allocations, 0 leaks)
test_lob: PASS (insert/cancel/execute correctness verified)
test_feed: PASS (100k events, 2.1ms total)
test_execution: PASS (order routing + transmission)
test_stress: PASS (10k concurrent orders, no races detected)
test_ai_telemetry: PASS (model inference < 100µs)
```

## Roadmap

- [ ] Multi-instrument orderbook sharding
- [ ] Adaptive hugepage sizing
- [ ] gRPC telemetry export
- [ ] Kubernetes deployment templates
- [ ] SEC 17a-4 audit logging compliance
- [ ] Circuit breaker integration

## License

MIT License. See `LICENSE` file.

## Contributing

Pull requests welcome. Please include:
1. Test coverage for any new components
2. Latency impact analysis (P50/P99 before/after)
3. Memory footprint impact
4. Thread safety audit for concurrency changes

## Support

For issues, questions, or deployment guidance: open a GitHub issue.

---

**Disclaimer:** This is a trading system framework. Use at your own risk. Thoroughly test before deploying with real capital. No warranties, express or implied.
