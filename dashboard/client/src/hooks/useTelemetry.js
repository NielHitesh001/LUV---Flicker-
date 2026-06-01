import { useState, useEffect } from 'react';

export function useTelemetry(wsUrl = 'ws://localhost:8080') {
  const [frames, setFrames] = useState([]);        // last 300 packets
  const [connected, setConnected] = useState(false);
  const [feedLost, setFeedLost] = useState(false);
  const [latest, setLatest] = useState(null);

  useEffect(() => {
    const ws = new WebSocket(wsUrl);

    ws.onmessage = (e) => {
      const packet = JSON.parse(e.data);

      if (packet.type === 'luv_disconnected') {
        setFeedLost(true);
        return;
      }

      setFeedLost(false);
      setLatest(packet);
      setFrames(prev => {
        const next = [...prev, packet];
        return next.length > 300 ? next.slice(-300) : next;
      });
    };

    ws.onopen  = () => setConnected(true);
    ws.onclose = () => setConnected(false);
    return () => ws.close();
  }, [wsUrl]);

  return { frames, latest, connected, feedLost };
}
