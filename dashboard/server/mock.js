const dgram = require('dgram');

const UDP_PORT = process.env.UDP_PORT || 7777;
const client = dgram.createSocket('udp4');

let seq = 0;
let sessionPnl = 0;
let fillCount = 0;
let rejectCount = 0;

setInterval(() => {
  sessionPnl += (Math.random() - 0.49) * 100;
  if (Math.random() < 0.3) fillCount++;
  if (Math.random() < 0.02) rejectCount++;

  // Test halt flash by randomly halting
  const halted = Math.random() < 0.05 ? 1 : 0;

  const packet = {
    type: "luv_heartbeat",
    seq: seq++,
    timestamp_ns: Date.now() * 1000000,
    session_pnl: Math.round(sessionPnl),
    gross_exposure: 5000000 + Math.round(Math.random() * 2000000),
    fill_count: fillCount,
    reject_count: rejectCount,
    tick_rate_hz: 40000 + Math.round(Math.random() * 4000),
    active_orders: 2 + Math.floor(Math.random() * 4),
    inference_us: 3.2 + Math.random() * 0.8,
    risk_ns: 2.1 + Math.random() * 0.2,
    halted: halted,
  };

  const msg = Buffer.from(JSON.stringify(packet));
  client.send(msg, UDP_PORT, 'localhost', (err) => {
    if (err) console.error(err);
  });
}, 1000 / 60);

console.log(`Mock UDP telemetry sending to localhost:${UDP_PORT} at 60Hz...`);
