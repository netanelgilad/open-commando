#!/usr/bin/env node
// The datagram hub (services/hub) as a plain Node WebSocket server: same
// protocol (web/net.js frames, {"type":"hello"} address assignment, routing
// by destination address, sender stamped by the hub; the match server,
// ?role=server, is always 10.0.0.1; WebRTC signalling forwarded). For local
// tests without wrangler, and the local match service (local-service.mjs).
//
//   node tools/server/net-hub.mjs [--port 8790]   ->  ws://127.0.0.1:8790/room/<name>
import { WebSocketServer } from 'ws';
import { randomBytes } from 'node:crypto';
import { parseArgs } from 'node:util';
import { fileURLToPath } from 'node:url';

const KIND_DATAGRAM = 1;
const HEADER = 13;
const BROADCAST = 0xffffffff;
const ipToString = (ip) => [ip & 255, (ip >>> 8) & 255, (ip >>> 16) & 255, ip >>> 24].join('.');
const ipFromString = (s) => {
  const p = s.split('.').map(Number);
  return (p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24)) >>> 0;
};
const addressFor = (n) => (10 | ((n >> 8) << 16) | ((n & 255) << 24)) >>> 0; // 10.0.x.y

// Rooms on a WebSocketServer (noServer mode: call handleUpgrade yourself, or
// let listen() own the port). Returns { rooms, accept(ws, url) }.
export function createHub() {
  const rooms = new Map(); // name -> Map(address -> ws)
  const tokens = new Map(); // "room/address" -> reconnect token
  const accept = (ws, url) => {
    const m = /^\/room\/([\w-]{1,64})$/.exec(url.pathname);
    if (!m) return ws.close(1008, 'not found');
    let room = rooms.get(m[1]);
    if (!room) rooms.set(m[1], (room = new Map()));
    const isServer = url.searchParams.get('role') === 'server';
    // A reconnect takes its address back with the token its hello gave it.
    const claim = url.searchParams.get('claim'), token = url.searchParams.get('token');
    let ip;
    if (claim && token && tokens.get(`${m[1]}/${claim}`) === token) {
      ip = ipFromString(claim);
      const old = room.get(ip);
      if (old && old !== ws) { room.delete(ip); old.terminate?.(); }
    } else {
      if (isServer && room.has(addressFor(1))) return ws.close(1008, 'room already has a server');
      let n = isServer ? 1 : 2;
      while (!isServer && room.has(addressFor(n))) n++;
      ip = addressFor(n);
    }
    const secret = token && tokens.get(`${m[1]}/${ipToString(ip)}`) === token ? token : randomBytes(12).toString('hex');
    tokens.set(`${m[1]}/${ipToString(ip)}`, secret);
    room.set(ip, ws);
    ws.send(JSON.stringify({ type: 'hello', address: ipToString(ip), token: secret }));
    ws.on('message', (data, isBinary) => {
      if (!isBinary) {
        // WebRTC signalling between endpoints, forwarded with the sender stamped.
        let msg;
        try { msg = JSON.parse(data.toString()); } catch { return; }
        if (msg.type !== 'signal' || typeof msg.to !== 'string') return;
        const peer = room.get(ipFromString(msg.to));
        if (!peer) return;
        msg.from = ipToString(ip);
        delete msg.to;
        return peer.send(JSON.stringify(msg));
      }
      if (data.length < HEADER || data[0] !== KIND_DATAGRAM) return;
      data.writeUInt32LE(ip, 7);
      const to = data.readUInt32LE(1);
      if (to === BROADCAST) {
        for (const [a, peer] of room) if (a !== ip) peer.send(data);
      } else {
        room.get(to)?.send(data);
      }
    });
    ws.on('close', () => {
      if (room.get(ip) !== ws) return; // replaced by its reconnect
      room.delete(ip);
      if (!room.size) rooms.delete(m[1]);
    });
  };
  return { rooms, accept };
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const { values: opt } = parseArgs({ options: { port: { type: 'string', default: '8790' } } });
  const hub = createHub();
  const wss = new WebSocketServer({ port: +opt.port, host: '127.0.0.1' });
  wss.on('connection', (ws, req) => hub.accept(ws, new URL(req.url, 'ws://hub')));
  console.log(`hub on ws://127.0.0.1:${opt.port}/room/<name>`);
}
