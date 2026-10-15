#!/usr/bin/env node
// Headless dedicated server: the browser build (renegade.js/.wasm) run under
// Node in its renderless "console" mode (NODX), hosting one C&C game that
// clients join with +connect. Its datagram sockets are the same virtual
// network as the page's (web/net.js), connected to a hub room.
//
//   node tools/server/run-server.mjs --net ws://127.0.0.1:8787/room/<name> \
//     [--game <install dir>] [--bin build/wasm-release/bin] [--map C&C_Field.mix]
//     [--players 16] [--name "Open Commando"] [--work /tmp/rebel-server]
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
const { values: opt } = parseArgs({
  options: {
    net: { type: 'string', default: process.env.REBEL_NET },
    game: { type: 'string', default: process.env.REBEL_GAME_DIR || resolve(engine, '../game-data/renegade') },
    bin: { type: 'string', default: join(engine, 'build/wasm-release/bin') },
    work: { type: 'string', default: '/tmp/rebel-server' },
    map: { type: 'string', default: process.env.REBEL_MAP || 'C&C_Field.mix' },
    // With a lobby (local-service.mjs): --map is the warmup map and the
    // rotation continues to --next-map, the match itself.
    'next-map': { type: 'string', default: process.env.REBEL_NEXT_MAP },
    players: { type: 'string', default: '16' },
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

writeFileSync(join(game, 'server.ini'), `[Server]
Config = svrcfg_open.ini
GameType = LAN
Nickname = Host
Port = ${opt.port}
GameSpyGamePort = 0
GameSpyQueryPort = 0
BandwidthUp = 0
NetUpdateRate = 10
AllowRemoteAdmin = false
`);
writeFileSync(join(game, 'Data', 'svrcfg_open.ini'), `[Settings]
ConfigName=Open Commando
MapName=${opt.map}
MapName00=${opt.map}
${opt['next-map'] ? `MapName01=${opt['next-map']}
` : ''}TimeLimitMinutes=${opt.minutes}
RadarMode=1
IsAutoRestart=yes
IsPassworded=no
IsDedicated=yes
IsQuickMatch=no
IsLaddered=no
UseLagReduction=yes
RemixTeams=yes
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
const state = { ready: false, map: null, loaded: false, warmup: false, players: [] };
let gameModule = null; // the game's Module once running
const console_ = (line) => gameModule && gameModule.ccall('Rebel_Console_Command', null, ['string'], [line]);
const out = (t) => {
  const line = `${stamp()} ${t}`;
  console.log(line);
  if (/\[net\] rtt|Load \d+% complete/.test(t)) return;
  recent.push(line);
  if (recent.length > 200) recent.shift();
  let m;
  if (/Gameplay Pending|Gameplay in progress/.test(t)) state.ready = true;
  if ((m = /Loading level (\S+)/.exec(t))) { state.map = m[1]; state.loaded = false; }
  if (/Level loaded OK/.test(t)) state.loaded = true;
  if ((m = /^Warmup (on|off)/.exec(t))) state.warmup = m[1] === 'on';
  if ((m = /Player (\S+) joined the game/.exec(t)) && !state.players.includes(m[1])) state.players.push(m[1]);
  if ((m = /Player (\S+) left the game/.exec(t))) state.players = state.players.filter((p) => p !== m[1]);
};
if (opt['status-port']) {
  //   GET  /         {"ready", "map", "loaded", "warmup", "players", "log": [...]}
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
    res.end(JSON.stringify({ ...state, uptime: +stamp(), net: net.stats, log: recent.slice(-40) }));
  }).listen(+opt['status-port']);
}
net.log = (t) => out(`[net] ${t}`);
await net.ready;

globalThis.rebelNodeModule = {
  rebelNet: net,
  rebelPlayerName: 'Host',
  print: out,
  printErr: out,
  locateFile: (p) => join(opt.bin, p),
  onAbort: (what) => { console.log(`${stamp()} [abort] ${what && what.stack || what}`); process.exit(1); },
  onRuntimeInitialized() { // `this` is the game's Module
    gameModule = this;
    // The server console: each line on stdin is a game console command.
    createInterface({ input: process.stdin }).on('line', (line) => {
      if (line.trim()) console_(line.trim());
    });
    this.callMain([`--root=${root}`, 'STARTSERVER=server.ini', 'NODX']);
  },
};
require(join(opt.bin, 'renegade.js'));
// Round trips through the hub (and this event loop, which the game shares)
// and traffic, every 10 s.
setInterval(() => net.probe(), 200).unref();
setInterval(() => out(`[net] rtt ${JSON.stringify(net.probeStats())} ${JSON.stringify(net.stats)}`), 10000).unref();
