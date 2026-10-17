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
    const url = new URL(request.url);
    const isServer = url.searchParams.get('role') === 'server';
    // A reconnect takes its address back with the token its hello gave it
    // (kept in storage: the old socket may be long gone).
    const claim = url.searchParams.get('claim'), token = url.searchParams.get('token');
    let ip = null, reclaimed = false;
    if (claim && token && (await this.ctx.storage.get(`token:${claim}`)) === token) {
      ip = ipFromString(claim);
      reclaimed = true;
      const old = this.byAddress.get(ip);
      if (old) {
        this.byAddress.delete(ip);
        try { old.close(1000, 'replaced by a reconnect'); } catch {}
      }
    } else {
      if (isServer && this.byAddress.has(addressFor(1))) return new Response('room already has a server', { status: 409 });
      let n = isServer ? 1 : 2;
      while (!isServer && this.byAddress.has(addressFor(n))) n++;
      ip = addressFor(n);
    }
    const secret = reclaimed ? token : [...crypto.getRandomValues(new Uint8Array(12))].map((b) => b.toString(16).padStart(2, '0')).join('');
    await this.ctx.storage.put(`token:${ipToString(ip)}`, secret);
    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);
    this.ctx.acceptWebSocket(server);
    server.serializeAttachment({ ip });
    this.byAddress.set(ip, server);
    server.send(JSON.stringify({ type: 'hello', address: ipToString(ip), token: secret }));
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

  webSocketClose(ws, code) {
    this.drop(ws);
    // Complete the close handshake (hibernating sockets don't on their own;
    // the other side would wait for it).
    try { ws.close(code === 1005 || code === 1006 ? 1000 : code, 'closed'); } catch {}
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
