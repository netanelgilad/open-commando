#!/usr/bin/env node
// Headless dedicated server: the browser build (renegade.js/.wasm) run under
// Node in its renderless "console" mode (NODX), hosting one C&C game that
// clients join with +connect. Its datagram sockets are the same virtual
// network as the page's (web/net.js), connected to a hub room.
//
//   node tools/server/run-server.mjs --net ws://127.0.0.1:8787/room/<name> \
//     [--game <install dir>] [--bin build/wasm-release/bin] [--map C&C_Field.mix]
//     [--players 16] [--name "Open Commando"] [--work /tmp/rebel-server]
//     [--bot-brain scripted|clef|hybrid] [--record-dir <dir>]
//
// Type console commands on stdin (game_info, player_info, amsg <text>, kick
// <name>, quit...), as at the original server's console.
//
// The game files are never modified: the work directory mirrors the install,
// plus the server's own settings (server.ini, Data/svrcfg_open.ini). Large
// assets are hard links (WasmFS's Node backend doesn't follow symlinks);
// small files are copies, because the game rewrites some of them in place
// (commands.txt at every start) and a link would carry that into the install.
import { createRequire } from 'node:module';
import { copyFileSync, linkSync, mkdirSync, readdirSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { parseArgs } from 'node:util';
import { createInterface } from 'node:readline';
import { createServer } from 'node:http';

const here = dirname(fileURLToPath(import.meta.url));
const engine = resolve(here, '../..');
// Nothing ends the server silently: print what did (game records and the
// log are all we get from a live server).
// A fatal error ends it, after the record is saved: a server stuck at 0 fps
// would hold its players.
process.on('unhandledRejection', (e) => {
  console.log(`[fatal] unhandled rejection: ${e?.stack || e?.message || e} (${e?.constructor?.name}${e?.status !== undefined ? `, status ${e.status}` : ''})`);
  let saved = Promise.resolve();
  try {
    recorder.add('fatal', { error: String(e?.stack || e).slice(0, 4000) });
    saved = recorder.flush();
  } catch {} // (before the recorder exists)
  Promise.race([saved, new Promise((r) => setTimeout(r, 4000))]).finally(() => process.exit(1));
});
process.on('exit', (code) => console.log(`[exit] server process exiting with code ${code}`));
const { values: opt } = parseArgs({
  options: {
    net: { type: 'string', default: process.env.REBEL_NET },
    // Bots' brain (bot-brain.mjs): off (the engine's own goal loop), scripted, or
    // clef (decisions by Clef-flash via REBEL_AI_URL with REBEL_AI_KEY), or hybrid
    // (Clef-flash decides, DeepSeek V4 Flash plans and talks).
    'bot-brain': { type: 'string', default: process.env.REBEL_BOT_BRAIN || 'off' },
    // The game's record (recorder.mjs): to the match service (REBEL_RECORD_URL
    // with REBEL_MATCH_KEY), or to <dir>/record.jsonl.
    'record-dir': { type: 'string', default: process.env.REBEL_RECORD_DIR },
    game: { type: 'string', default: process.env.REBEL_GAME_DIR || resolve(engine, '../game-data/renegade') },
    bin: { type: 'string', default: join(engine, 'build/wasm-release/bin') },
    work: { type: 'string', default: '/tmp/rebel-server' },
    map: { type: 'string', default: process.env.REBEL_MAP || 'C&C_Field.mix' },
    'next-map': { type: 'string', default: process.env.REBEL_NEXT_MAP },
    // The rotation, comma-separated (the matchmaker's random order; loops).
    // Default: --map, then --next-map.
    maps: { type: 'string', default: process.env.REBEL_MAPS },
    players: { type: 'string', default: '32' }, // C&C servers commonly ran 32 (16 a side)
    name: { type: 'string', default: 'Open Commando' },
    port: { type: 'string', default: '4848' },
    minutes: { type: 'string', default: '30' },
    // The work directory already holds the game as renegade/ (our own server
    // image): use it as is instead of mirroring --game.
    'in-place': { type: 'boolean', default: false },
    // HTTP status on this port (a container's health check): {"ready":
    // true once the map is loaded, "log": recent lines}.
    'status-port': { type: 'string', default: process.env.REBEL_STATUS_PORT },
  },
});
if (!opt.net) {
  console.error('--net <hub room URL> (or REBEL_NET) is required');
  process.exit(2);
}

// Work directory: renegade/ mirrors the install, persist/ holds the registry.
const root = resolve(opt.work);
const game = join(root, 'renegade');
if (!opt['in-place']) rmSync(game, { recursive: true, force: true });
mkdirSync(join(root, 'persist'), { recursive: true });
const LINK_MIN_BYTES = 1 << 20;
const mirror = (from, to) => {
  mkdirSync(to, { recursive: true });
  for (const e of readdirSync(from, { withFileTypes: true })) {
    if (/^server\.ini$/i.test(e.name)) continue;
    const src = join(from, e.name), dst = join(to, /^data$/i.test(e.name) && e.isDirectory() ? 'Data' : e.name);
    if (e.isDirectory()) mirror(src, dst);
    else if (statSync(src).size < LINK_MIN_BYTES) copyFileSync(src, dst);
    else {
      try { linkSync(src, dst); } catch { copyFileSync(src, dst); } // another volume: copy
    }
  }
};
if (!opt['in-place']) mirror(opt.game, game);

const rotation = opt.maps ? opt.maps.split(',').filter(Boolean) : [opt.map, opt['next-map']].filter(Boolean);
writeFileSync(join(game, 'server.ini'), `[Server]
Config = svrcfg_open.ini
GameType = LAN
Nickname = Host
Port = ${opt.port}
GameSpyGamePort = 0
GameSpyQueryPort = 0
BandwidthUp = 0
NetUpdateRate = 20
AllowRemoteAdmin = false
`);
writeFileSync(join(game, 'Data', 'svrcfg_open.ini'), `[Settings]
ConfigName=Open Commando
MapName=${rotation[0]}
${rotation.map((m, i) => `MapName${String(i).padStart(2, '0')}=${m}\n`).join('')}TimeLimitMinutes=${opt.minutes}
RadarMode=1
IsAutoRestart=yes
IsPassworded=no
IsDedicated=yes
IsQuickMatch=no
IsLaddered=no
UseLagReduction=yes
RemixTeams=no
CanRepairBuildings=yes
DriverIsAlwaysGunner=yes
SpawnWeapons=no
IsFriendlyFirePermitted=no
DoMapsLoop=yes
IsTeamChangingAllowed=no
IsClanGame=no
MaxPlayers=${opt.players}
BaseDestructionEndsGame=yes
BeaconPlacementEndsGame=yes
StartingCredits=0
bGameTitle=${opt.name}
bMotd=
`);

const require = createRequire(import.meta.url);
const RebelNet = require(join(engine, 'web/net.js'));
// The match server takes the room's fixed address (10.0.0.1, see the hub).
const net = RebelNet.create(opt.net + (opt.net.includes('?') ? '&' : '?') + 'role=server');
// Clients may open WebRTC data channels to the server (web/net.js, ?netrtc).
try {
  globalThis.RTCPeerConnection = require('node-datachannel/polyfill').RTCPeerConnection;
  net.enableRtc({ accept: true, iceServers: process.env.REBEL_ICE_SERVERS ? JSON.parse(process.env.REBEL_ICE_SERVERS) : [] });
} catch (e) {
  console.log(`[net] no WebRTC (${e.message}): clients stay on the hub`);
}
const t0 = Date.now();
const stamp = () => ((Date.now() - t0) / 1000).toFixed(1);
const recent = [];
// What a lobby/matchmaker needs to know, from the server's own console output.
const state = { ready: false, map: null, loaded: false, warmup: false, players: [], bots: 0 };
let gameModule = null; // the game's Module once running
let brain = null; // the bots' brain, if on (see below)
let recorder = { add() {}, flush: async () => {} }; // the game's record, see below
// Console lines are queued here and taken by the game inside its own frame
// (debug_web.cpp Take_Console_Queue): a ccall could land while the game is
// suspended in a sleep (Asyncify) and corrupt it; the server hung in
// "Server Sleep" with the bots' commands arriving all the time.
const console_ = (line) => gameModule && (gameModule.rebelConsoleQueue ??= []).push(line);
const out = (t) => {
  const line = `${stamp()} ${t}`;
  console.log(line);
  if (/\[net\] rtt|Load \d+% complete/.test(t)) return;
  if (!/^\[(perf|chat|record)\]/.test(t)) recorder.add('log', { line: t });
  recent.push(line);
  if (recent.length > 200) recent.shift();
  let m;
  if (/Gameplay Pending|Gameplay in progress/.test(t)) state.ready = true;
  if ((m = /Loading level (\S+)/.exec(t))) { state.map = m[1]; state.loaded = false; }
  if (/Level loaded OK/.test(t)) {
    state.loaded = true;
    if (brain) setTimeout(() => console_('bot_brain on'), 0); // between frames
  }
  if ((m = /^Warmup (on|off)/.exec(t))) state.warmup = m[1] === 'on';
  if ((m = /Player (\S+) joined the game/.exec(t)) && !state.players.includes(m[1])) state.players.push(m[1]);
  if ((m = /Player (\S+) left the game/.exec(t))) state.players = state.players.filter((p) => p !== m[1]);
  if (/^\[BOT\] .* joined (GDI|Nod)$/.test(t)) state.bots++;
  if (/^\[BOT\] .* left$/.test(t)) state.bots = Math.max(0, state.bots - 1);
};
if (opt['status-port']) {
  //   GET  /         {"ready", "map", "loaded", "warmup", "players", "perf", "ai", "log": [...]}
  //   POST /console  body: one console command (warmup on, msg ..., gameover)
  createServer((req, res) => {
    if (req.method === 'POST' && req.url === '/console') {
      let body = '';
      req.on('data', (d) => (body += d));
      req.on('end', () => {
        const line = body.trim().slice(0, 500);
        if (line) console_(line);
        res.writeHead(line ? 202 : 400).end();
      });
      return;
    }
    res.writeHead(state.ready ? 200 : 503, { 'content-type': 'application/json' });
    res.end(JSON.stringify({ ...state, uptime: +stamp(), net: net.stats, ai: brain?.usage?.() ?? null, log: recent.slice(-40) }));
  }).listen(+opt['status-port']);
}
net.log = (t) => out(`[net] ${t}`);
await net.ready;

{
  const { createRecorder } = await import('./recorder.mjs');
  recorder = createRecorder({ url: process.env.REBEL_RECORD_URL, key: process.env.REBEL_MATCH_KEY || process.env.REBEL_AI_KEY, dir: opt['record-dir'], out });
  recorder.add('server', { maps: rotation, players: +opt.players, brain: opt['bot-brain'], net: opt.net });
}
if (opt['bot-brain'] !== 'off') {
  const { createBrain } = await import('./bot-brain.mjs');
  const ai = process.env.REBEL_AI_URL && { url: process.env.REBEL_AI_URL, key: process.env.REBEL_AI_KEY };
  const budgetUsd = +(process.env.REBEL_AI_BUDGET_USD || 2); // per server (one match)
  // The models, per bot (bot-brain.mjs modelPlan): REBEL_PLANNERS a pool
  // ("deepseek-v4-flash,glm-5.3-flash"), REBEL_DECIDERS a weighted Clef mix.
  const planners = process.env.REBEL_PLANNERS || undefined; // "a,b|c,d": spread over a and b, c and d stand in
  const deciders = process.env.REBEL_DECIDERS || undefined;
  brain = createBrain({ mode: opt['bot-brain'], send: (line) => console_(line), record: recorder.add, out, ai, budgetUsd, planners, deciders });
}

globalThis.rebelNodeModule = {
  rebelNet: net,
  rebelBotState: (json) => brain?.onState(json),
  rebelBotChat: (sender, type, text) => brain?.onChat(sender, type, text),
  rebelPlayerName: 'Host',
  print: out,
  printErr: out,
  locateFile: (p) => join(opt.bin, p),
  onAbort: (what) => { console.log(`${stamp()} [abort] ${what && what.stack || what}`); process.exit(1); },
  // The game ending itself (exit() in the engine): say so, with where from.
  onExit: (status) => console.log(`${stamp()} [exit] the game exited with status ${status}\n${new Error('exit').stack.split('\n').slice(2, 30).join('\n')}`),
  onRuntimeInitialized() { // `this` is the game's Module
    gameModule = this;
    // The server console: each line on stdin is a game console command.
    createInterface({ input: process.stdin }).on('line', (line) => {
      // Test hook: block the main thread natively for <s> seconds (a stand-in
      // for a deadlock), to check the hang watchdog still reports.
      const block = /^!block (\d+)$/.exec(line.trim());
      if (block) return Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, +block[1] * 1000);
      if (line.trim()) console_(line.trim());
    });
    this.callMain([`--root=${root}`, 'STARTSERVER=server.ini', 'NODX']);
  },
};
require(join(opt.bin, 'renegade.js'));
// As a container's first process (PID 1) nothing ends us on SIGTERM unless
// we do: the container platform stops idle servers that way.
for (const sig of ['SIGTERM', 'SIGINT']) process.on(sig, () => {
  console.log(`${stamp()} ${sig}: exiting`);
  recorder.add('stop', { signal: sig });
  // The record's last batch, then out (a few seconds at most).
  Promise.race([recorder.flush(), new Promise((r) => setTimeout(r, 4000))]).finally(() => process.exit(0));
});
// Round trips through the hub (and this event loop, which the game shares)
// and traffic, every 10 s.
setInterval(() => net.probe(), 200).unref();
setInterval(() => out(`[net] rtt ${JSON.stringify(net.probeStats())} ${JSON.stringify(net.stats)}`), 10000).unref();
// Performance, every 10 s: the game's frames a second (a slow server is laggy
// for everyone, whatever their ping), how late this event loop's timers ran
// (the game, the network and the bots' brain all share it), and memory
// (containers have a fixed allowance; wasm memory only grows).
const perf = { frames: 0, at: Date.now(), lagMax: 0, fps: 0 };
let tickAt = Date.now();
setInterval(() => {
  const now = Date.now();
  perf.lagMax = Math.max(perf.lagMax, now - tickAt - 50);
  tickAt = now;
}, 50).unref();
setInterval(() => {
  const m = process.memoryUsage();
  const frames = gameModule?.rebelFrames || 0; // published by the game (no call into it)
  const now = Date.now();
  perf.fps = Math.round(((frames - perf.frames) * 1000) / (now - perf.at));
  const cpu = process.cpuUsage();
  const cpuPct = perf.cpu ? Math.round(((cpu.user - perf.cpu.user + cpu.system - perf.cpu.system) / 1000 / (now - perf.at)) * 100) : null;
  perf.cpu = cpu;
  state.perf = { fps: perf.fps, lagMaxMs: perf.lagMax, rssMb: Math.round(m.rss / 2 ** 20), cpuPct };
  recorder.add('perf', { ...state.perf, wasmMb: Math.round((gameModule?.HEAPU16?.buffer.byteLength || 0) / 2 ** 20), heapMb: Math.round(m.heapUsed / 2 ** 20), players: state.players.length, bots: state.bots, map: state.map, ai: brain?.usage?.() ?? null, net: net.stats });
  out(`[perf] ${perf.fps} fps, cpu ${cpuPct ?? '-'}%, timers up to ${perf.lagMax} ms late; rss ${Math.round(m.rss / 2 ** 20)} MB, wasm ${Math.round((gameModule?.HEAPU16?.buffer.byteLength || 0) / 2 ** 20)} MB, js heap ${Math.round(m.heapUsed / 2 ** 20)} MB`);
  Object.assign(perf, { frames, at: now, lagMax: 0 });
}, 10000).unref();
