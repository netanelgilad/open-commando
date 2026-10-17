#!/usr/bin/env node
// The online service on this machine, for development: what services/match
// does on Cloudflare (matchmaker, rooms, a server per match), with local
// processes instead of Durable Objects and containers.
//
//   node tools/server/local-service.mjs [--port 8790] [--bots 6] [--map C&C_Volcano.mix]
//
//   GET /play?name=<nick>   WebSocket: queue; {"type":"match","room",...} when placed
//   GET /room/<id>          WebSocket: a match room (web/net.js frames)
//   GET /status             JSON: queue and matches
//
// Players are placed at once in a running match with room for another human,
// or a new one. Match servers play real C&C games map after map; bots fill
// each game to --bots players and each human takes a bot's place.
// Pages: http://127.0.0.1:8461/?online=http://127.0.0.1:8790
import { createServer } from 'node:http';
import { spawn } from 'node:child_process';
import { parseArgs } from 'node:util';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { WebSocketServer } from 'ws';
import { createHub } from './net-hub.mjs';
import { pass } from '../../services/match/src/matchmaker.js';
import { mapRotation } from '../../services/match/src/maps.js';

const here = dirname(fileURLToPath(import.meta.url));
const { values: opt } = parseArgs({
  options: {
    port: { type: 'string', default: '8790' },
    bots: { type: 'string', default: '6' }, // bots keep each game at this many players (0: none)
    map: { type: 'string' }, // the first map (default: a random rotation)
    bin: { type: 'string' },
  },
});
const HUMANS_PER_MATCH = 8;
const t0 = Date.now();
const log = (t) => console.log(`${((Date.now() - t0) / 1000).toFixed(1)} ${t}`);

const hub = createHub();
const tickets = new Map(); // id -> { ticket, ws }
const matches = new Map(); // id -> { id, maps, startedAt, maxPlayers, region, mus, proc, players: Set, joining, waiting, ready }

function startServer(match) {
  const args = [join(here, 'run-server.mjs'), '--net', `ws://127.0.0.1:${opt.port}/room/${match.id}`,
    '--maps', match.maps.join(','), '--work', `/tmp/rebel-server-${match.id}`, '--name', `Open Commando ${match.id}`];
  if (opt.bin) args.push('--bin', opt.bin);
  const proc = spawn(process.execPath, args, { stdio: ['pipe', 'pipe', 'pipe'] });
  match.proc = proc;
  let buffer = '';
  const onLine = (line) => {
    if (/\[net\] rtt|Load \d+%/.test(line)) return;
    if (/joined the game|left the game|\[BOT\]|Loading level|Gameplay|abort|Assert|\[brain\]/.test(line)) log(`${match.id}: ${line}`);
    let m;
    if ((m = /Player (\S+) joined the game/.exec(line))) {
      match.players.add(m[1]);
      match.joining.shift();
    }
    if ((m = /Player (\S+) left the game/.exec(line))) match.players.delete(m[1]);
    if ((m = /Loading level (\S+)/.exec(line))) match.map = m[1];
    if (/Level loaded OK/.test(line) && !match.ready) {
      if (opt.bots > 0) proc.stdin.write(`bot_fill ${opt.bots}\n`); // once loaded: the console is up by then
      match.ready = true;
      for (const send of match.waiting.splice(0)) send(); // players waiting for the server
    }
  };
  const read = (d) => {
    buffer += d.toString();
    const lines = buffer.split('\n');
    buffer = lines.pop();
    lines.forEach(onLine);
  };
  proc.stdout.on('data', read);
  proc.stderr.on('data', read);
  proc.on('exit', (code) => {
    log(`${match.id}: server exited (${code})`);
    matches.delete(match.id);
  });
}

// Tell a player where to go once the match's server has its map loaded
// (joining earlier works too, but the engine's connect attempts time out).
function place(entry, match, team) {
  tickets.delete(entry.ticket.id);
  match.joining.push(Date.now());
  const send = () => {
    entry.ws.send(JSON.stringify({ type: 'match', room: match.id, server: '10.0.0.1:4848', team }));
    entry.ws.close(1000, 'matched');
  };
  if (match.ready) send();
  else match.waiting.push(send);
}

setInterval(() => {
  const now = Date.now();
  // Open to new players: room for another human (in it, or on their way).
  for (const m of matches.values()) m.joining = m.joining.filter((at) => now - at < 3 * 60000);
  const open = [...matches.values()].map((m) => {
    const humans = [...m.players, ...m.joining.map((_, i) => `joining${i}`)];
    return { ...m, teams: [humans.filter((_, i) => i % 2 === 0), humans.filter((_, i) => i % 2 === 1)] };
  });
  const { backfills, newMatches } = pass({
    tickets: [...tickets.values()].map((e) => e.ticket),
    matches: open,
    now,
    config: { minPlayers: 1, botFillAfterMs: 0, minHumansForBots: 1, pingStartMs: 1000, pingMaxMs: 1000 },
  });
  for (const b of backfills) {
    const m = matches.get(b.matchId), e = tickets.get(b.ticketId);
    if (!m || !e) continue;
    log(`${e.ticket.players[0].name} -> ${m.id} (${m.ready ? m.map : 'booting'})`);
    place(e, m, b.team);
  }
  for (const nm of newMatches) {
    const id = 'm' + Math.random().toString(36).slice(2, 8);
    const maps = mapRotation();
    if (opt.map) maps.unshift(opt.map);
    const match = { maps, id, region: nm.region, mus: {}, startedAt: now, durationMs: 0, maxPlayers: HUMANS_PER_MATCH, players: new Set(), joining: [], waiting: [], ready: false };
    matches.set(id, match);
    log(`new match ${id}, starting its server (${maps[0]})`);
    startServer(match);
    for (const tid of nm.tickets) {
      const e = tickets.get(tid);
      if (e) {
        log(`${e.ticket.players[0].name} -> ${id}`);
        place(e, match, nm.teams[0].includes(e.ticket.players[0].id) ? 0 : 1);
      }
    }
  }
  // No humans: stop the server (the first ones get 90 s to load in, then 30 s).
  for (const m of matches.values()) {
    if (m.players.size || m.joining.length) m.lastSeenAt = now;
    if (m.players.size) m.hadPlayers = true;
    if (now - (m.lastSeenAt || m.startedAt) > (m.hadPlayers ? 30000 : 90000)) {
      log(`${m.id}: no players, stopping`);
      m.proc?.kill();
      matches.delete(m.id);
    }
  }
  // Waiting players hear how it's going.
  if (Math.floor(now / 1000) % 3 === 0) {
    for (const e of tickets.values()) e.ws.send(JSON.stringify({ type: 'status', waitedS: Math.round((now - e.ticket.enqueuedAt) / 1000), inQueue: tickets.size }));
  }
}, 1000);

const http = createServer((req, res) => {
  if (req.url === '/status') {
    res.writeHead(200, { 'content-type': 'application/json', 'access-control-allow-origin': '*' });
    return res.end(JSON.stringify({
      queue: [...tickets.values()].map((e) => e.ticket.players[0].name),
      matches: [...matches.values()].map((m) => ({ id: m.id, map: m.map, ready: m.ready, players: [...m.players], joining: m.joining.length })),
    }, null, 2));
  }
  res.writeHead(404);
  res.end();
});
const wss = new WebSocketServer({ noServer: true });
http.on('upgrade', (req, socket, head) => {
  wss.handleUpgrade(req, socket, head, (ws) => {
    const url = new URL(req.url, 'ws://local');
    if (url.pathname !== '/play') return hub.accept(ws, url);
    const id = 't' + Math.random().toString(36).slice(2, 10);
    const name = (url.searchParams.get('name') || 'Player').replace(/[^\w\-[\]]/g, '').slice(0, 20) || 'Player';
    tickets.set(id, { ticket: { id, players: [{ id, name, mu: 25 }], pings: { WEUR: 30 }, enqueuedAt: Date.now() }, ws });
    ws.send(JSON.stringify({ type: 'queued', region: 'local' }));
    ws.on('close', () => tickets.delete(id));
    log(`${name} queued`);
  });
});
http.listen(+opt.port, '127.0.0.1', () => log(`online service on http://127.0.0.1:${opt.port} (pages: ?online=http://127.0.0.1:${opt.port})`));
process.on('SIGINT', () => {
  for (const m of matches.values()) m.proc?.kill();
  process.exit(0);
});
