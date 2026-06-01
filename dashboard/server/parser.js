function decodeBinary(buf) {
  if (buf.length < 192) return null;
  const magic = buf.readUInt32LE(0);
  if (magic !== 0x3156554C) return null;

  const version = buf.readUInt16LE(4);
  const bytes = buf.readUInt16LE(6);
  if (bytes !== 192) return null;

  const sequence = Number(buf.readBigUInt64LE(8));
  const timestamp_ns = Number(buf.readBigUInt64LE(16));
  const session_pnl = Number(buf.readBigInt64LE(24));
  const gross_exposure = Number(buf.readBigInt64LE(32));
  const fill_count = buf.readInt32LE(40);
  const reject_count = buf.readInt32LE(44);
  const tick_rate_hz = buf.readUInt32LE(48);
  const active_orders = buf.readUInt32LE(52);
  const inference_us = buf.readFloatLE(56);
  const risk_ns = buf.readFloatLE(60);
  const halted = buf.readUInt8(64);

  return {
    type: "luv_heartbeat",
    seq: sequence,
    timestamp_ns: timestamp_ns,
    session_pnl: session_pnl,
    gross_exposure: gross_exposure,
    fill_count: fill_count,
    reject_count: reject_count,
    tick_rate_hz: tick_rate_hz,
    active_orders: active_orders,
    inference_us: inference_us,
    risk_ns: risk_ns,
    halted: halted,
  };
}

function decode(buf) {
  if (buf.length > 0 && buf[0] === 0x7B) { // '{' character -> JSON
    try {
      return JSON.parse(buf.toString('utf-8'));
    } catch (err) {
      return null;
    }
  } else {
    return decodeBinary(buf);
  }
}

module.exports = { decode };
