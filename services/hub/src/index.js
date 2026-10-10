// Datagram hub: the routing half of the game's virtual network (see
// web/net.js for the frame format). Each room is a Durable Object that
// works like a network switch: it gives every endpoint that connects an
// address (10.0.x.y; the match server, ?role=server, is always 10.0.0.1 so
// clients can be told where to +connect), then forwards each datagram frame
// to the endpoint with the destination address, or to all others for a
// broadcast. The sender's address is stamped by the hub, so endpoints can't
// spoof it.
//
//   GET /room/<name>[?role=server]  (WebSocket upgrade)  join a room
//   GET /ping         (WebSocket upgrade)  echo, for measuring latency
import { DurableObject } from 'cloudflare:workers';

const KIND_DATAGRAM = 1;
const HEADER = 13;
const BROADCAST = 0xffffffff;

const ipToString = (ip) => [ip & 255, (ip >>> 8) & 255, (ip >>> 16) & 255, ip >>> 24].join('.');
const ipFromString = (s) => {
  const p = s.split('.').map(Number);
  return (p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24)) >>> 0;
};
const addressFor = (n) => (10 | (0 << 8) | ((n >> 8) << 16) | ((n & 255) << 24)) >>> 0; // 10.0.x.y

export class Room extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    // address -> WebSocket, rebuilt after hibernation from the attachments.
    this.byAddress = new Map();
    for (const ws of this.ctx.getWebSockets()) {
      const a = ws.deserializeAttachment();
      if (a) this.byAddress.set(a.ip, ws);
    }
  }

  async fetch(request) {
    const isServer = new URL(request.url).searchParams.get('role') === 'server';
    if (isServer && this.byAddress.has(addressFor(1))) return new Response('room already has a server', { status: 409 });
    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);
    this.ctx.acceptWebSocket(server);
    let n = isServer ? 1 : 2;
    while (!isServer && this.byAddress.has(addressFor(n))) n++;
    const ip = addressFor(n);
    server.serializeAttachment({ ip });
    this.byAddress.set(ip, server);
    server.send(JSON.stringify({ type: 'hello', address: ipToString(ip) }));
    return new Response(null, { status: 101, webSocket: client });
  }

  webSocketMessage(ws, message) {
    if (typeof message === 'string') return this.control(ws, message);
    const frame = new Uint8Array(message);
    if (frame.length < HEADER || frame[0] !== KIND_DATAGRAM) return;
    const v = new DataView(message);
    const from = ws.deserializeAttachment().ip;
    v.setUint32(7, from, true);
    const to = v.getUint32(1, true);
    if (to === BROADCAST) {
      for (const [ip, peer] of this.byAddress) if (ip !== from) this.trySend(peer, message);
    } else {
      const peer = this.byAddress.get(to);
      if (peer) this.trySend(peer, message);
    }
  }

  // Control messages between endpoints: WebRTC signalling
  // ({"type":"signal","to":ip,...}), forwarded with the sender stamped.
  control(ws, text) {
    let m;
    try {
      m = JSON.parse(text);
    } catch {
      return;
    }
    if (m.type !== 'signal' || typeof m.to !== 'string' || text.length > 16384) return;
    const peer = this.byAddress.get(ipFromString(m.to));
    if (!peer) return;
    m.from = ipToString(ws.deserializeAttachment().ip);
    delete m.to;
    this.trySend(peer, JSON.stringify(m));
  }

  trySend(ws, message) {
    try {
      ws.send(message);
    } catch (e) {
      this.drop(ws);
    }
  }

  drop(ws) {
    const a = ws.deserializeAttachment();
    if (a && this.byAddress.get(a.ip) === ws) this.byAddress.delete(a.ip);
  }

  webSocketClose(ws) {
    this.drop(ws);
  }

  webSocketError(ws) {
    this.drop(ws);
  }
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (request.headers.get('Upgrade') !== 'websocket') return new Response('websocket expected', { status: 426 });
    if (url.pathname === '/ping') {
      const [client, server] = Object.values(new WebSocketPair());
      server.accept();
      server.addEventListener('message', (e) => server.send(e.data));
      return new Response(null, { status: 101, webSocket: client });
    }
    const m = /^\/room\/([\w-]{1,64})$/.exec(url.pathname);
    if (!m) return new Response('not found', { status: 404 });
    return env.ROOMS.get(env.ROOMS.idFromName(m[1])).fetch(request);
  },
};
