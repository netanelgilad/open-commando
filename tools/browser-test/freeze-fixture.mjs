#!/usr/bin/env node
// Fixture for the client freeze in big fights (no AI: the scripted brain, or
// mock models that behave like the real ones):
// a hub, a 32-player server where bots fight, buy and drive (warmup on, so
// everyone has 1500 credits and new characters, vehicles and their weapons
// show up at once), and a browser player (lan.mjs) watching. Each run ends
// at the first freeze (the page stops answering) or after --minutes, and
// reports where the engine's watchdog saw the main thread stuck.
//
//   node tools/browser-test/freeze-fixture.mjs [--runs 3] [--minutes 8] [--bots 32]
//        [--map C&C_Field.mix] [--no-warmup] [--brain scripted|mock]
import { spawn } from 'node:child_process';
import { copyFileSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { parseArgs } from 'node:util';

const here = dirname(fileURLToPath(import.meta.url));
const engine = join(here, '../..');
const { values: opt } = parseArgs({
  options: {
    runs: { type: 'string', default: '3' },
    minutes: { type: 'string', default: '8' },
    bots: { type: 'string', default: '32' },
    map: { type: 'string', default: 'C&C_Field.mix' },
    'no-warmup': { type: 'boolean', default: false },
    port: { type: 'string', default: '8791' },
    brain: { type: 'string', default: 'scripted' },
    'server-minutes': { type: 'string' }, // the server's time limit (map rotation), default its own // or mock: model-like behavior, no AI calls
  },
});

// The hub (tools/server/net-hub.mjs) on its own port.
const children = new Set();
process.on('exit', () => { for (const c of children) c.kill('SIGKILL'); });
for (const sig of ['SIGINT', 'SIGTERM']) process.on(sig, () => process.exit(1));
const hubProc = spawn(process.execPath, [join(engine, 'tools/server/net-hub.mjs'), '--port', opt.port], { stdio: 'ignore' });
children.add(hubProc);
await new Promise((r) => setTimeout(r, 1000));

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
// Each batch keeps its logs in a folder of its own (out/fixture-<time>).
const outDir = join(here, `out/fixture-${new Date().toISOString().replace(/[:.]/g, '-')}`);
mkdirSync(outDir, { recursive: true });

// A DevTools session to a Node process (its --inspect URL), with the
// profiler enabled up front.
async function inspect(url) {
  if (!url) return null;
  const ws = new WebSocket(url);
  await new Promise((r, j) => ((ws.onopen = r), (ws.onerror = j)));
  let id = 0;
  const waiting = new Map();
  ws.onmessage = (e) => {
    const m = JSON.parse(e.data);
    if (m.id && waiting.has(m.id)) waiting.get(m.id)(m.result || { error: m.error });
  };
  const send = (method, params = {}) => new Promise((r) => { waiting.set(++id, r); ws.send(JSON.stringify({ id, method, params })); });
  await send('Profiler.enable');
  await send('Profiler.setSamplingInterval', { interval: 1000 });
  return { send, close: () => ws.close() };
}

// 4 s of CPU samples: the hottest functions and the hottest one's call chain.
async function cpuProfile(inspector) {
  if (!inspector) return ['(no inspector)'];
  const answer = await Promise.race([(async () => {
    await inspector.send('Profiler.start');
    await sleep(4000);
    return (await inspector.send('Profiler.stop')).profile;
  })(), sleep(20000).then(() => null)]);
  if (!answer) return ['(the inspector did not answer)'];
  const byId = new Map(answer.nodes.map((n) => [n.id, n]));
  const parent = new Map();
  for (const n of answer.nodes) for (const k of n.children || []) parent.set(k, n.id);
  const hits = new Map();
  for (const id of answer.samples) hits.set(id, (hits.get(id) || 0) + 1);
  const top = [...hits].sort((a, b) => b[1] - a[1]);
  const name = (n) => `${n.callFrame.functionName || '(anonymous)'} ${n.callFrame.url.split('/').pop()}`;
  const out = top.slice(0, 8).map(([id, k]) => `hot ${k} ${name(byId.get(id))}`);
  for (let id = top[0]?.[0], d = 0; id && d < 40; id = parent.get(id), d++) out.push(`at ${name(byId.get(id))}`);
  return out;
}

async function run(n) {
  const room = `freeze-${Date.now()}`;
  // The engine's hang watchdog writes here under Node (main_web.cpp).
  const hangLog = '/tmp/rebel-server-fixture/persist/hang.log';
  try { rmSync(hangLog); } catch {}
  const net = `ws://127.0.0.1:${opt.port}/room/${room}`;
  const t0 = Date.now();
  const s = () => ((Date.now() - t0) / 1000).toFixed(0);
  // The server: scripted brain, bots fill the game once the map is up.
  // The server under the inspector: if it stalls (no [perf] line for 30 s),
  // a CPU profile of its main thread says where (its own hang watchdog can't
  // print: a Node worker's output goes through the stuck main thread).
  const server = spawn(process.execPath, ['--inspect=127.0.0.1:0', join(engine, 'tools/server/run-server.mjs'), '--net', net, '--maps', opt.map, '--bot-brain', opt.brain, '--work', '/tmp/rebel-server-fixture', ...(opt['server-minutes'] ? ['--minutes', opt['server-minutes']] : [])], { stdio: ['pipe', 'pipe', 'pipe'] });
  children.add(server);
  let serverLog = '';
  server.stdout.on('data', (d) => (serverLog += d));
  server.stderr.on('data', (d) => (serverLog += d));
  while (!/Debugger listening on ws:/.test(serverLog) && Date.now() - t0 < 10000) await sleep(200);
  const inspector = await inspect(/Debugger listening on (ws:\S+)/.exec(serverLog)?.[1]);
  while (!/Level loaded OK/.test(serverLog) && Date.now() - t0 < 120000) await sleep(1000);
  server.stdin.write(`bot_fill ${opt.bots}\n`);
  if (!opt['no-warmup']) server.stdin.write('warmup on\n');
  // The player: lan.mjs joins and reports every 5 s; a frozen page shows as UNRESPONSIVE.
  const samples = Math.ceil((+opt.minutes * 60) / 5);
  const client = spawn(process.execPath, [join(here, 'lan.mjs')], {
    env: { ...process.env, NET: net, SERVER: 'external', JOIN: 'connect', CLIENTS: '1', NET_SAMPLES: String(samples) },
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  children.add(client);
  let clientLog = '';
  client.stdout.on('data', (d) => (clientLog += d));
  client.stderr.on('data', (d) => (clientLog += d));
  const done = new Promise((r) => client.on('exit', r));
  let result = null;
  let lastPerf = Date.now(), perfSeen = 0, serverStall = null;
  // A run that neither ends nor freezes (a stuck page load, a wedged
  // browser) is cut off, so the batch always finishes.
  const deadline = Date.now() + (+opt.minutes + 6) * 60000;
  while (!result) {
    await sleep(2000);
    if (Date.now() > deadline) result = 'timeout';
    const perfs = (serverLog.match(/\[perf\]/g) || []).length;
    if (perfs !== perfSeen) (perfSeen = perfs), (lastPerf = Date.now());
    // A map change loads for a while without [perf] lines: up to 2 min then.
    else if (!serverStall && perfSeen && Date.now() - lastPerf > (/\[load\]/.test(serverLog.slice(-20000)) ? 120000 : 30000)) {
      serverStall = { at: s(), profile: await cpuProfile(inspector) };
      await sleep(15000); // the watchdog reports at 10 s, then every 30 s
      try { serverStall.profile.push(...readFileSync(hangLog, 'utf8').trim().split('\n').map((l) => `watchdog: ${l}`)); } catch { serverStall.profile.push('watchdog: (no hang log)'); }
      result = 'server stalled';
    }
    if (/UNRESPONSIVE/.test(clientLog)) result = 'froze';
    else if (await Promise.race([done.then(() => true), sleep(0).then(() => false)])) result = 'ended';
  }
  if (result === 'froze') {
    // lan.mjs saves the page's console log right after (it waits for DevTools first).
    await Promise.race([new Promise((r) => { const t = setInterval(() => { if (/console saved/.test(clientLog)) { clearInterval(t); r(); } }, 1000); }), sleep(90000)]);
  }
  client.kill('SIGKILL');
  server.kill('SIGINT');
  const pings = (clientLog.match(/client ping/g) || []).length;
  let where = '';
  // Keep this run's logs (the next run overwrites lan.mjs's).
  const saved = join(outDir, `run-${n}-client.log`);
  try { copyFileSync(join(here, 'out/lan-client-console.log'), saved); } catch {}
  writeFileSync(join(outDir, `run-${n}-server.log`), serverLog);
  writeFileSync(join(outDir, `run-${n}-lan.log`), clientLog);
  // The profile of the frozen page (lan.mjs): the hottest functions and the call chain.
  const profile = clientLog.split('\n').filter((l) => /^\S+   (hot|at) /.test(l));
  try {
    const console_ = readFileSync(saved, 'utf8');
    const hang = console_.split('\n').filter((l) => /\[hang\] main thread stuck/.test(l) && !/last step: start/.test(l)).pop();
    if (hang) where = hang.replace(/^.*scopes: /, '');
  } catch {}
  const buys = (serverLog.match(/ buys /g) || []).length;
  const what = { froze: `FROZE after ~${pings * 5 + 60} s in: ${where || '(no watchdog line)'}`, ended: `clean for ${opt.minutes} min`, timeout: 'did not finish (timed out)', 'server stalled': `SERVER STALLED at ~${serverStall?.at} s` }[result];
  console.log(`run ${n}: ${what} (bot purchases: ${buys})`);
  for (const l of profile.slice(0, 30)) console.log(`  ${l}`);
  if (serverStall) {
    for (const l of serverStall.profile) console.log(`  server ${l}`);
    writeFileSync(join(outDir, `run-${n}-server-stall.txt`), serverStall.profile.join('\n'));
  }
  inspector?.close();
  await sleep(3000);
  return result === 'froze';
}

let frozen = 0;
for (let i = 1; i <= +opt.runs; i++) frozen += (await run(i)) ? 1 : 0;
console.log(`${frozen} of ${opt.runs} runs froze; logs in ${outDir}`);
hubProc.kill();
process.exit(0);
