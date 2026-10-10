// Checks the WebRTC path of web/net.js between two Node endpoints through a
// hub: a "server" (10.0.0.1, accepts) and a client that opens a data channel
// to it; datagrams then flow over the channel, round trips measured.
//   node tools/server/rtc-test.mjs ws://127.0.0.1:8790/room/<name>
import { createRequire } from 'node:module';
const require = createRequire(import.meta.url);
const { RTCPeerConnection } = require('node-datachannel/polyfill');
globalThis.RTCPeerConnection = RTCPeerConnection;
const RebelNet = require('../../web/net.js');
const url = process.argv[2] || `ws://127.0.0.1:8790/room/rtc-${Date.now()}`;
const server = RebelNet.create(url + '?role=server');
const client = RebelNet.create(url);
await Promise.all([server.ready, client.ready]);
server.enableRtc({ accept: true });
await client.enableRtc({ connectTo: '10.0.0.1' });
for (let i = 0; i < 100 && !client.directPeers().length; i++) await new Promise((r) => setTimeout(r, 50));
console.log('direct peers:', client.directPeers());
// Echo on the server, ping from the client.
setInterval(() => { for (const d of server.incoming.splice(0)) server.send(d.fromIp, d.fromPort, d.toPort, d.data); }, 1);
const rtts = [];
for (let i = 0; i < 200; i++) {
  const t = performance.now();
  client.send(server.address, 4848, 1000, new Uint8Array(200));
  while (!client.incoming.length) await new Promise((r) => setTimeout(r, 0));
  client.incoming.shift();
  rtts.push(performance.now() - t);
}
rtts.sort((a, b) => a - b);
console.log(`rtt over ${client.directPeers().length ? 'data channel' : 'hub'}: median ${rtts[100].toFixed(2)} ms, p99 ${rtts[198].toFixed(2)} ms`);
client.close();
server.close();
process.exit(0);
