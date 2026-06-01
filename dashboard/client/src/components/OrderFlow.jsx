import React, { useRef, useEffect } from 'react';

export function OrderFlow({ latest }) {
  const prevFills = useRef(0);
  const prevRejects = useRef(0);

  useEffect(() => {
    if (latest) {
      prevFills.current = latest.fill_count;
      prevRejects.current = latest.reject_count;
    }
  }, [latest]);

  if (!latest) return <div className="bottom-panel">Waiting...</div>;

  const fills = latest.fill_count || 0;
  const rejects = latest.reject_count || 0;
  const total = fills + rejects;
  const rejectRate = total > 0 ? (rejects / total * 100) : 0;

  const fillsUp = fills > prevFills.current;
  const rejectsUp = rejects > prevRejects.current;

  return (
    <div className="bottom-panel" style={{ display: 'flex', gap: '20px' }}>
      <div style={{ flex: 1 }}>
        <div className="panel-header">FILLS</div>
        <div className="data-val green">
          {fills} {fillsUp ? '↑' : ''}
        </div>
        <div className="bar-container" style={{ height: '10px' }}>
          <div className="bar-fill" style={{ width: '100%', backgroundColor: 'var(--green)' }}></div>
        </div>
      </div>

      <div style={{ width: '1px', backgroundColor: 'var(--border)' }}></div>

      <div style={{ flex: 1 }}>
        <div className="panel-header">REJECTS</div>
        <div className="data-val red">
          {rejects} <span style={{ fontSize: '14px', color: 'var(--text-dim)' }}>({rejectRate.toFixed(1)}%)</span> {rejectsUp ? '↑' : ''}
        </div>
        <div className="bar-container" style={{ height: '10px' }}>
          <div className="bar-fill" style={{ width: `${rejectRate}%`, backgroundColor: 'var(--red)' }}></div>
        </div>
      </div>
    </div>
  );
}
