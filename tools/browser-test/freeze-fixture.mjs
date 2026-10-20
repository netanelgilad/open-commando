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
import { copyFileSync, readFileSync, writeFileSync } from 'node:fs';
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
    brain: { type: 'string', default: 'scripted' }, // or mock: model-like behavior, no AI calls
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

async function run(n) {
  const room = `freeze-${Date.now()}`;
  const net = `ws://127.0.0.1:${opt.port}/room/${room}`;
  const t0 = Date.now();
  const s = () => ((Date.now() - t0) / 1000).toFixed(0);
  // The server: scripted brain, bots fill the game once the map is up.
  const server = spawn(process.execPath, [join(engine, 'tools/server/run-server.mjs'), '--net', net, '--maps', opt.map, '--bot-brain', opt.brain, '--work', '/tmp/rebel-server-fixture'], { stdio: ['pipe', 'pipe', 'pipe'] });
  children.add(server);
  let serverLog = '';
  server.stdout.on('data', (d) => (serverLog += d));
  server.stderr.on('data', (d) => (serverLog += d));
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
  // A run that neither ends nor freezes (a stuck page load, a wedged
  // browser) is cut off, so the batch always finishes.
  const deadline = Date.now() + (+opt.minutes + 6) * 60000;
  while (!result) {
    await sleep(2000);
    if (Date.now() > deadline) result = 'timeout';
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
  const saved = join(here, `out/freeze-run-${n}-client.log`);
  try { copyFileSync(join(here, 'out/lan-client-console.log'), saved); } catch {}
  writeFileSync(join(here, `out/freeze-run-${n}-server.log`), serverLog);
  writeFileSync(join(here, `out/freeze-run-${n}-lan.log`), clientLog);
  // The profile of the frozen page (lan.mjs): the hottest functions and the call chain.
  const profile = clientLog.split('\n').filter((l) => /^\S+   (hot|at) /.test(l));
  try {
    const console_ = readFileSync(saved, 'utf8');
    const hang = console_.split('\n').filter((l) => /\[hang\] main thread stuck/.test(l) && !/last step: start/.test(l)).pop();
    if (hang) where = hang.replace(/^.*scopes: /, '');
  } catch {}
  const buys = (serverLog.match(/ buys /g) || []).length;
  const what = { froze: `FROZE after ~${pings * 5 + 60} s in: ${where || '(no watchdog line)'}`, ended: `clean for ${opt.minutes} min`, timeout: 'did not finish (timed out)' }[result];
  console.log(`run ${n}: ${what} (bot purchases: ${buys})`);
  for (const l of profile.slice(0, 30)) console.log(`  ${l}`);
  await sleep(3000);
  return result === 'froze';
}

let frozen = 0;
for (let i = 1; i <= +opt.runs; i++) frozen += (await run(i)) ? 1 : 0;
console.log(`${frozen} of ${opt.runs} runs froze`);
hubProc.kill();
process.exit(0);
