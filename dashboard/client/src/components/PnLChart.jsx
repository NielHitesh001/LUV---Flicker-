import React from 'react';
import { LineChart, Line, ReferenceLine, ResponsiveContainer, YAxis } from 'recharts';

export function PnLChart({ frames, latest }) {
  const currentPnl = latest ? latest.session_pnl / 10000 : 0;
  const isPositive = currentPnl >= 0;

  const chartData = frames.map((f, i) => ({
    time: i,
    pnl: f.session_pnl / 10000
  }));

  return (
    <div className="panel chart-area">
      <div className="flex-row">
        <div className="panel-header">SESSION P&L</div>
        <div className={`data-val ${isPositive ? 'green' : 'red'}`}>
          ${currentPnl.toFixed(2)}
        </div>
      </div>
      <div style={{ flex: 1, minHeight: 0 }}>
        <ResponsiveContainer width="100%" height="100%">
          <LineChart data={chartData}>
            <YAxis domain={['auto', 'auto']} hide />
            <ReferenceLine y={0} stroke="#666655" strokeDasharray="3 3" />
            <Line
              type="step"
              dataKey="pnl"
              stroke={isPositive ? '#00cc66' : '#ff3333'}
              strokeWidth={2}
              dot={false}
              isAnimationActive={false}
            />
          </LineChart>
        </ResponsiveContainer>
      </div>
    </div>
  );
}
