#!/usr/bin/env node
// The datagram hub (services/hub) as a plain Node WebSocket server: same
// protocol (web/net.js frames, {"type":"hello"} address assignment, routing
// by destination address, sender stamped by the hub). For local tests
// without wrangler, and to compare against the Durable Object.
//
//   node tools/server/net-hub.mjs [--port 8790]   ->  ws://127.0.0.1:8790/room/<name>
import { WebSocketServer } from 'ws';
import { parseArgs } from 'node:util';

const { values: opt } = parseArgs({ options: { port: { type: 'string', default: '8790' } } });
const KIND_DATAGRAM = 1;
const HEADER = 13;
const BROADCAST = 0xffffffff;
const ipToString = (ip) => [ip & 255, (ip >>> 8) & 255, (ip >>> 16) & 255, ip >>> 24].join('.');
const addressFor = (n) => (10 | ((n >> 8) << 16) | ((n & 255) << 24)) >>> 0; // 10.0.x.y

const rooms = new Map(); // name -> Map(address -> ws)
const wss = new WebSocketServer({ port: +opt.port, host: '127.0.0.1' });
wss.on('connection', (ws, req) => {
  const url = new URL(req.url, 'ws://hub');
  const m = /^\/room\/([\w-]{1,64})$/.exec(url.pathname);
  if (!m) return ws.close(1008, 'not found');
  let room = rooms.get(m[1]);
  if (!room) rooms.set(m[1], (room = new Map()));
  // The match server (?role=server) is always 10.0.0.1.
  const isServer = url.searchParams.get('role') === 'server';
  if (isServer && room.has(addressFor(1))) return ws.close(1008, 'room already has a server');
  let n = isServer ? 1 : 2;
  while (!isServer && room.has(addressFor(n))) n++;
  const ip = addressFor(n);
  room.set(ip, ws);
  ws.send(JSON.stringify({ type: 'hello', address: ipToString(ip) }));
  ws.on('message', (data, isBinary) => {
    if (!isBinary || data.length < HEADER || data[0] !== KIND_DATAGRAM) return;
    data.writeUInt32LE(ip, 7);
    const to = data.readUInt32LE(1);
    if (to === BROADCAST) {
      for (const [a, peer] of room) if (a !== ip) peer.send(data);
    } else {
      room.get(to)?.send(data);
    }
  });
  ws.on('close', () => {
    room.delete(ip);
    if (!room.size) rooms.delete(m[1]);
  });
});
console.log(`hub on ws://127.0.0.1:${opt.port}/room/<name>`);
