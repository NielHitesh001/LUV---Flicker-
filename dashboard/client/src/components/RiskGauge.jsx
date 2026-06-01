import React from 'react';

export function RiskGauge({ latest }) {
  if (!latest) return <div className="bottom-panel">Waiting...</div>;

  const MAX_EXPOSURE = 10000000; // $1M fixed-point
  const exposure = latest.gross_exposure || 0;
  const fraction = Math.min(exposure / MAX_EXPOSURE, 1.0);
  const percent = fraction * 100;

  let color = 'var(--green)';
  if (percent > 90) color = 'var(--red)';
  else if (percent > 70) color = 'var(--amber)';

  const halted = latest.halted === 1;

  return (
    <div className="bottom-panel" style={{ display: 'flex', flexDirection: 'column' }}>
      {halted && (
        <div style={{ backgroundColor: 'var(--red)', color: '#000', textAlign: 'center', fontWeight: 'bold', padding: '2px', marginBottom: '10px' }}>
          ⚠ TRADING HALTED
        </div>
      )}

      <div className="panel-header">GROSS EXPOSURE</div>
      <div className="data-val" style={{ color }}>
        ${(exposure / 10000).toLocaleString(undefined, { minimumFractionDigits: 2, maximumFractionDigits: 2 })}
      </div>

      <div className="bar-container" style={{ marginTop: 'auto' }}>
        <div className="bar-fill" style={{ width: `${percent}%`, backgroundColor: color }}></div>
      </div>
    </div>
  );
}
