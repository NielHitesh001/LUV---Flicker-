import React from 'react';

export function LOBPanel({ latest }) {
  if (!latest) return <div className="panel lob-area">Waiting...</div>;

  const fills = latest.fill_count || 0;
  const rejects = latest.reject_count || 0;
  const total = fills + rejects;
  const fillRatio = total > 0 ? (fills / total) * 100 : 50;

  // Active orders proxy representation
  const maxOrders = 64;
  const activeOrders = latest.active_orders || 0;

  return (
    <div className="panel lob-area">
      <div className="panel-header">LOB DEPTH (AGGREGATE)</div>

      <div style={{ marginTop: '10px' }}>
        <div style={{ display: 'flex', justifyContent: 'space-between', color: 'var(--text-dim)' }}>
          <span>BID PRESSURE (PROXY)</span>
          <span>{fillRatio.toFixed(1)}%</span>
        </div>
        <div className="bar-container">
          <div className="bar-fill" style={{ width: `${fillRatio}%`, backgroundColor: 'var(--green)' }}></div>
        </div>
      </div>

      <div style={{ marginTop: '30px', textAlign: 'center', color: 'var(--text-dim)' }}>
        SPREAD: N/A (aggregate feed)
      </div>

      <div style={{ marginTop: '30px' }}>
        <div style={{ color: 'var(--text-dim)', marginBottom: '5px' }}>ACTIVE ORDERS ({activeOrders}/{maxOrders})</div>
        <div style={{ display: 'flex', gap: '2px', height: '20px' }}>
          {Array.from({ length: maxOrders }).map((_, i) => (
            <div
              key={i}
              style={{
                flex: 1,
                backgroundColor: i < activeOrders ? 'var(--text-primary)' : 'var(--bg-highlight)'
              }}
            ></div>
          ))}
        </div>
      </div>

      <div style={{ marginTop: 'auto', fontSize: '10px', color: 'var(--text-dim)', textAlign: 'center' }}>
        Per-symbol LOB state requires a separate IPC channel (Phase B.2)<br/>
        This panel shows aggregate proxies until that is implemented
      </div>
    </div>
  );
}
