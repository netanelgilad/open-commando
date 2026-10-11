// Round trips between two endpoints of a hub room: A sends a datagram to B,
// B echoes it, every 50 ms for a while; prints the distribution.
//   node tools/server/hub-probe.mjs wss://<hub>/room/<name> [seconds]
import { createRequire } from 'node:module';
const RebelNet = createRequire(import.meta.url)('../../web/net.js');
const url = process.argv[2];
const seconds = +(process.argv[3] || 20);
const t0 = performance.now();
const a = RebelNet.create(url), b = RebelNet.create(url);
await Promise.all([a.ready, b.ready]);
console.log(`both joined in ${(performance.now() - t0).toFixed(0)} ms`);
a.log = b.log = () => {};
const echo = setInterval(() => { for (const d of b.incoming.splice(0)) b.send(d.fromIp, d.fromPort, d.toPort, d.data); }, 1);
const sent = new Map();
const rtts = [];
const poll = setInterval(() => {
  for (const d of a.incoming.splice(0)) {
    const id = new DataView(d.data.buffer, d.data.byteOffset, 4).getUint32(0, true);
    if (sent.has(id)) { rtts.push(performance.now() - sent.get(id)); sent.delete(id); }
  }
}, 1);
let id = 0;
const end = Date.now() + seconds * 1000;
while (Date.now() < end) {
  const payload = new Uint8Array(200);
  new DataView(payload.buffer).setUint32(0, ++id, true);
  sent.set(id, performance.now());
  a.send(b.address, 1, 1, payload);
  await new Promise((r) => setTimeout(r, 50));
}
await new Promise((r) => setTimeout(r, 1000));
clearInterval(echo);
clearInterval(poll);
rtts.sort((x, y) => x - y);
const q = (p) => rtts[Math.min(rtts.length - 1, Math.floor(p * rtts.length))].toFixed(1);
console.log(`${rtts.length}/${id} round trips: min ${q(0)} median ${q(0.5)} p90 ${q(0.9)} p99 ${q(0.99)} max ${q(1)} ms (lost ${sent.size})`);
a.close();
b.close();
process.exit(0);
