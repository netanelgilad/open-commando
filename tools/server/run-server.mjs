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

const here = dirname(fileURLToPath(import.meta.url));
const engine = resolve(here, '../..');
const { values: opt } = parseArgs({
  options: {
    net: { type: 'string' },
    game: { type: 'string', default: process.env.REBEL_GAME_DIR || resolve(engine, '../game-data/renegade') },
    bin: { type: 'string', default: join(engine, 'build/wasm-release/bin') },
    work: { type: 'string', default: '/tmp/rebel-server' },
    map: { type: 'string', default: 'C&C_Field.mix' },
    players: { type: 'string', default: '16' },
    name: { type: 'string', default: 'Open Commando' },
    port: { type: 'string', default: '4848' },
    minutes: { type: 'string', default: '30' },
  },
});
if (!opt.net) {
  console.error('--net <hub room URL> is required');
  process.exit(2);
}

// Work directory: renegade/ mirrors the install, persist/ holds the registry.
const root = resolve(opt.work);
const game = join(root, 'renegade');
rmSync(game, { recursive: true, force: true });
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
mirror(opt.game, game);

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
TimeLimitMinutes=${opt.minutes}
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
const t0 = Date.now();
const stamp = () => ((Date.now() - t0) / 1000).toFixed(1);
net.log = (t) => console.log(`${stamp()} [net] ${t}`);
await net.ready;

globalThis.rebelNodeModule = {
  rebelNet: net,
  rebelPlayerName: 'Host',
  print: (t) => console.log(`${stamp()} ${t}`),
  printErr: (t) => console.log(`${stamp()} ${t}`),
  locateFile: (p) => join(opt.bin, p),
  onAbort: (what) => { console.log(`${stamp()} [abort] ${what && what.stack || what}`); process.exit(1); },
  onRuntimeInitialized() { // `this` is the game's Module
    const game = this;
    // The server console: each line on stdin is a game console command.
    createInterface({ input: process.stdin }).on('line', (line) => {
      if (line.trim()) game.ccall('Rebel_Console_Command', null, ['string'], [line.trim()]);
    });
    this.callMain([`--root=${root}`, 'STARTSERVER=server.ini', 'NODX']);
  },
};
require(join(opt.bin, 'renegade.js'));
// Round trips through the hub (and this event loop, which the game shares)
// and traffic, every 10 s.
setInterval(() => net.probe(), 200).unref();
setInterval(() => console.log(`${stamp()} [net] rtt ${JSON.stringify(net.probeStats())} ${JSON.stringify(net.stats)}`), 10000).unref();
