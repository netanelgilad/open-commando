// The page side of the game's virtual datagram network
// (Code/Platform/net/vnet.cpp). The game sends and receives UDP-style
// datagrams addressed by virtual IPv4 address and port; this file carries
// them over a pluggable transport:
//
//   tab:<room>        other tabs of this browser (BroadcastChannel), no server
//   ws(s)://.../<room> a hub that routes datagrams between its endpoints
//                     (tools/net-hub, a Cloudflare Durable Object)
//
// Every transport moves the same frames, so the game doesn't know or care
// which one is in use:
//
//   u8  kind      1 = datagram
//   u32 dstIp     as the game's s_addr (network byte order in memory)
//   u16 dstPort
//   u32 srcIp
//   u16 srcPort
//   ... payload
//
// (little-endian fields). A hub rewrites srcIp with the sender's assigned
// address, so endpoints can't spoof each other. Control messages travel as
// JSON text: the hub's {"type":"hello","address":"10.x.y.z"} assigns the
// endpoint its address.
'use strict';

const RebelNet = (() => {
  const KIND_DATAGRAM = 1;
  const HEADER = 13;
  const BROADCAST = 0xffffffff;

  // s_addr (as read from wasm memory, little-endian) <-> dotted quad.
  const ipToString = (ip) => [ip & 255, (ip >>> 8) & 255, (ip >>> 16) & 255, ip >>> 24].join('.');
  const ipFromString = (s) => {
    const p = s.split('.').map(Number);
    return (p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24)) >>> 0;
  };

  function encode(dstIp, dstPort, srcIp, srcPort, payload) {
    const f = new Uint8Array(HEADER + payload.length);
    const v = new DataView(f.buffer);
    v.setUint8(0, KIND_DATAGRAM);
    v.setUint32(1, dstIp, true);
    v.setUint16(5, dstPort, true);
    v.setUint32(7, srcIp, true);
    v.setUint16(11, srcPort, true);
    f.set(payload, HEADER);
    return f;
  }

  function decode(frame) {
    if (frame.length < HEADER || frame[0] !== KIND_DATAGRAM) return null;
    const v = new DataView(frame.buffer, frame.byteOffset, frame.byteLength);
    return {
      toIp: v.getUint32(1, true), toPort: v.getUint16(5, true),
      fromIp: v.getUint32(7, true), fromPort: v.getUint16(11, true),
      data: frame.subarray(HEADER),
    };
  }

  // Other tabs of this browser: every tab sees every frame and keeps its own.
  function tabTransport(room) {
    const t = { name: 'tab:' + room };
    let channel = null;
    t.open = (onFrame) => {
      channel = new BroadcastChannel('rebel-net:' + room);
      channel.onmessage = (e) => onFrame(new Uint8Array(e.data));
      // No one hands out addresses here: pick one at random in 10.77/16.
      const r = crypto.getRandomValues(new Uint8Array(2));
      return Promise.resolve(`10.77.${r[0]}.${(r[1] % 254) + 1}`);
    };
    t.send = (frame) => channel.postMessage(frame.buffer);
    t.close = () => channel && channel.close();
    return t;
  }

  // A routing hub over WebSocket (reliable and ordered: a lost packet delays
  // the ones behind it; see docs/ONLINE_PLAN.md, transport T1).
  function wsTransport(url) {
    const t = { name: url };
    let ws = null;
    t.open = (onFrame, onClose) => new Promise((resolve, reject) => {
      ws = new WebSocket(url);
      ws.binaryType = 'arraybuffer';
      let hello = false;
      ws.onmessage = (e) => {
        if (typeof e.data === 'string') {
          const m = JSON.parse(e.data);
          if (m.type === 'hello') { hello = true; resolve(m.address); }
          return;
        }
        onFrame(new Uint8Array(e.data));
      };
      ws.onerror = () => { if (!hello) reject(new Error('could not reach ' + url)); };
      ws.onclose = () => { if (hello) onClose(); };
    });
    t.send = (frame) => { if (ws.readyState === 1) ws.send(frame); };
    t.close = () => ws && ws.close();
    return t;
  }

  function transportFor(spec) {
    if (spec.startsWith('tab:')) return tabTransport(spec.slice(4) || 'default');
    if (/^wss?:\/\//.test(spec)) return wsTransport(spec);
    throw new Error('unknown network: ' + spec);
  }

  // The object the game talks to (Module.rebelNet). `conditions` simulates
  // a worse network for testing: { latency, jitter (ms), loss (0..1) }.
  function create(spec, conditions = {}) {
    const net = {
      address: 0, // s_addr; 0 until the transport is up
      incoming: [], // { fromIp, fromPort, toPort, data }, drained by vnet.cpp
      stats: { sentPackets: 0, sentBytes: 0, receivedPackets: 0, receivedBytes: 0, dropped: 0 },
      conditions,
      transport: transportFor(spec),
      log: (t) => console.log('[net] ' + t),
    };

    const deliver = (d) => {
      net.stats.receivedPackets++;
      net.stats.receivedBytes += d.data.length;
      net.incoming.push({ fromIp: d.fromIp, fromPort: d.fromPort, toPort: d.toPort, data: d.data });
    };
    const impair = (fn) => {
      const c = net.conditions;
      if (c.loss && Math.random() < c.loss) { net.stats.dropped++; return; }
      const delay = (c.latency || 0) + (c.jitter ? Math.random() * c.jitter : 0);
      if (delay > 0) setTimeout(fn, delay); else fn();
    };

    // Latency probe: a frame to our own address, port 0, carrying the send
    // time; the hub routes it back like any datagram.
    const probe = { sent: 0, rtts: [] };
    net.probe = () => {
      if (!net.address) return;
      const payload = new Uint8Array(8);
      new DataView(payload.buffer).setFloat64(0, performance.now(), true);
      net.transport.send(encode(net.address, 0, net.address, 0, payload));
      probe.sent++;
    };
    // Median and worst round trip since the last call (ms), and how many came back.
    net.probeStats = () => {
      const r = probe.rtts.sort((a, b) => a - b);
      const out = { n: r.length, sent: probe.sent, median: r.length ? +r[r.length >> 1].toFixed(1) : null, max: r.length ? +r[r.length - 1].toFixed(1) : null };
      probe.rtts = [];
      probe.sent = 0;
      return out;
    };

    const onFrame = (frame) => {
      const d = decode(frame);
      if (!d || !net.address) return;
      if (d.toPort === 0 && d.fromIp === net.address && d.data.length === 8) {
        probe.rtts.push(performance.now() - new DataView(d.data.buffer, d.data.byteOffset, 8).getFloat64(0, true));
        return;
      }
      if (d.toIp !== net.address && d.toIp !== BROADCAST) return;
      if (d.fromIp === net.address) return; // our own broadcast, already looped back
      impair(() => deliver(d));
    };

    net.send = (toIp, toPort, fromPort, data) => {
      if (!net.address) return;
      net.stats.sentPackets++;
      net.stats.sentBytes += data.length;
      const frame = encode(toIp, toPort, net.address, fromPort, data);
      impair(() => net.transport.send(frame));
      // Broadcasts reach this endpoint too, as on a real LAN.
      if (toIp === BROADCAST) deliver({ fromIp: net.address, fromPort, toPort, data });
    };

    net.ready = net.transport.open(onFrame, () => {
      net.log('connection to ' + net.transport.name + ' closed');
    }).then((address) => {
      net.address = ipFromString(address);
      net.log(`${net.transport.name}: this endpoint is ${address}`);
      return address;
    });
    net.close = () => net.transport.close();
    return net;
  }

  return { create, ipToString, ipFromString, encode, decode };
})();

if (typeof module !== 'undefined') module.exports = RebelNet;
