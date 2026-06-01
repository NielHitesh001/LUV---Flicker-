import React from 'react';

export function Header({ latest, connected, feedLost }) {
  if (!latest) {
    return (
      <div className="header-bar">
        <div><span className="status-dot dead"></span> LUV ENGINE WAITING...</div>
      </div>
    );
  }

  const formatHz = (hz) => (hz || 0).toLocaleString();
  const halted = latest.halted === 1;

  return (
    <div className={`header-bar ${halted ? 'halted' : ''}`}>
      <div>
        <span className={`status-dot ${connected && !feedLost ? 'live' : 'dead'}`}></span>
        [ {connected && !feedLost ? 'LIVE' : 'DEAD'} ] LUV ENGINE
      </div>
      <div className="header-stats">
        <span>TICK: {formatHz(latest.tick_rate_hz)} hz</span>
        <span>INF: {(latest.inference_us || 0).toFixed(2)} μs</span>
        <span>RISK: {(latest.risk_ns || 0).toFixed(2)} ns</span>
        <span>ORDERS: {latest.active_orders || 0}</span>
        <span>HALTED: {halted ? 'YES' : 'NO'}</span>
      </div>
    </div>
  );
}
