import React from 'react';
import { useTelemetry } from './hooks/useTelemetry';
import { Header } from './components/Header';
import { PnLChart } from './components/PnLChart';
import { LOBPanel } from './components/LOBPanel';
import { OrderFlow } from './components/OrderFlow';
import { RiskGauge } from './components/RiskGauge';
import { SignalPanel } from './components/SignalPanel';
import './styles/terminal.css';

// Default WS port
const WS_PORT = import.meta.env.VITE_WS_PORT || 8080;

function App() {
  const { frames, latest, connected, feedLost } = useTelemetry(`ws://localhost:${WS_PORT}`);

  return (
    <div className="dashboard-container">
      <Header latest={latest} connected={connected} feedLost={feedLost} />

      <PnLChart frames={frames} latest={latest} />
      <LOBPanel latest={latest} />

      <div className="bottom-area">
        <OrderFlow latest={latest} />
        <RiskGauge latest={latest} />
        <SignalPanel frames={frames} latest={latest} />
      </div>

      {feedLost && (
        <div className="feed-lost-overlay">
          <div className="feed-lost-box">
            <div className="feed-lost-title">⚠ FEED DISCONNECTED</div>
            <div className="feed-lost-desc">
              Waiting for LUV engine...<br/><br/>
              Last packet: {(frames.length > 0) ? `${((Date.now() - (frames[frames.length-1].timestamp_ns / 1000000)) / 1000).toFixed(1)}s ago` : 'Unknown'}
            </div>
          </div>
        </div>
      )}
    </div>
  );
}

export default App;
