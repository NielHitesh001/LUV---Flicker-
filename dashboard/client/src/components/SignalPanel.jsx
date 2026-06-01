import React from 'react';
import { LineChart, Line, ResponsiveContainer, YAxis } from 'recharts';

export function SignalPanel({ frames, latest }) {
  if (!latest) return <div className="bottom-panel">Waiting...</div>;

  const chartData = frames.map((f, i) => ({
    time: i,
    inf: f.inference_us
  }));

  return (
    <div className="bottom-panel" style={{ display: 'flex', flexDirection: 'column' }}>
      <div style={{ display: 'flex', justifyContent: 'space-between' }}>
        <div>
          <div className="panel-header">RISK LATENCY</div>
          <div className="data-val amber">{(latest.risk_ns || 0).toFixed(2)} ns</div>
        </div>
        <div style={{ textAlign: 'right' }}>
          <div className="panel-header">INF LATENCY</div>
          <div style={{ color: 'var(--text-dim)' }}>{(latest.inference_us || 0).toFixed(2)} μs</div>
        </div>
      </div>

      <div style={{ height: '40px', marginTop: '10px', marginBottom: '10px' }}>
        <ResponsiveContainer width="100%" height="100%">
          <LineChart data={chartData}>
            <YAxis domain={['auto', 'auto']} hide />
            <Line
              type="monotone"
              dataKey="inf"
              stroke="var(--blue)"
              strokeWidth={1}
              dot={false}
              isAnimationActive={false}
            />
          </LineChart>
        </ResponsiveContainer>
      </div>

      <div style={{ marginTop: 'auto', color: 'var(--text-dim)', textAlign: 'center' }}>
        <span style={{ color: 'var(--text-primary)' }}>SIGNAL: aggregate feed only</span><br/>
        <span style={{ fontSize: '10px' }}>per-symbol confidence requires Phase B.2 IPC</span>
      </div>
    </div>
  );
}
