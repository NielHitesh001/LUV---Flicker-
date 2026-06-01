require('dotenv').config();
const dgram = require('dgram');
const { WebSocketServer, WebSocket } = require('ws');
const { decode } = require('./parser');

const UDP_PORT = process.env.UDP_PORT || 7777;
const WS_PORT = process.env.WS_PORT || 8080;

const udpSocket = dgram.createSocket('udp4');
const wss = new WebSocketServer({ port: WS_PORT });

let watchdogTimer = null;

function resetWatchdog() {
  if (watchdogTimer) clearTimeout(watchdogTimer);
  watchdogTimer = setTimeout(() => {
    const msg = JSON.stringify({ type: 'luv_disconnected' });
    wss.clients.forEach(client => {
      if (client.readyState === WebSocket.OPEN) {
        client.send(msg);
      }
    });
  }, 3000);
}

udpSocket.on('message', (buf) => {
  const packet = decode(buf);
  if (!packet) return;

  const msg = JSON.stringify(packet);
  wss.clients.forEach(client => {
    if (client.readyState === WebSocket.OPEN) {
      client.send(msg);
    }
  });

  resetWatchdog();
});

udpSocket.on('error', (err) => {
  console.error(`UDP socket error:\n${err.stack}`);
  udpSocket.close();
});

udpSocket.bind(UDP_PORT, () => {
  console.log(`UDP Server listening on 0.0.0.0:${UDP_PORT}`);
});

wss.on('connection', (ws) => {
  console.log('Client connected to WebSocket.');
  ws.on('close', () => console.log('Client disconnected.'));
});

console.log(`WebSocket Server broadcasting on ws://localhost:${WS_PORT}`);
resetWatchdog(); // Start the initial watchdog
