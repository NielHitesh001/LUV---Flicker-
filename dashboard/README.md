# LUV Engine Dashboard

A fully decoupled, real-time trading terminal UI that consumes live UDP telemetry from the C++ LUV engine.

## Architecture

```
C++ engine → UDP :7777 → Node server → WebSocket :8080 → Browser
```

## Setup

1. From the repo root, go to `dashboard/` and install dependencies:
   ```bash
   cd dashboard
   npm install
   ```
2. Copy `.env.example` to `.env` if you want to override default ports (`UDP_PORT=7777`, `WS_PORT=8080`).

## Run

To run the full stack (Node server + Vite client) pointed at a live C++ engine on UDP port 7777:

```bash
npm run dev &
```

To test the UI *without* the C++ engine, using a local mock UDP feed generating synthetic 60hz packets:

```bash
npm run mock &
```

## Limitations

- **Aggregate Telemetry Only:** The current `TelemetryPacket` format strictly sends aggregate statistics (session P&L, gross exposure, total fills). It does not send per-symbol limit order book depth or model confidence arrays. Panels displaying LOB or Signal confidence use aggregate proxies or placeholders, pending a future Phase B.2 dedicated IPC channel.
- **Binary Format Parsing:** The binary parser is implemented for the 192-byte `TelemetryPacket` little-endian struct, but might require alignment verification based on the compiler output of `luv_arena.hpp`.
