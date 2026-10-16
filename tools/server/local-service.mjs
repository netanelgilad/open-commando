#!/usr/bin/env node
// The online service on this machine, for development: what services/match
// does on Cloudflare (matchmaker, rooms, a server per match), with local
// processes instead of Durable Objects and containers.
//
//   node tools/server/local-service.mjs [--port 8790] [--start-players 8]
//        [--lobby-wait 90] [--lobby-map C&C_Field.mix] [--map C&C_Volcano.mix]
//
//   GET /play?name=<nick>   WebSocket: queue; {"type":"match","room",...} when placed
//   GET /room/<id>          WebSocket: a match room (web/net.js frames)
//   GET /status             JSON: queue, matches and their lobbies
//
// Players are placed in a match at once (a new one, or one still in its
// lobby or early in play: backfill). The match server opens in warmup on
// the lobby map (services/match/src/lobby.js): fight and respawn while it
// fills; then "warmup off" + "gameover" move everyone to the match map.
// Pages: http://127.0.0.1:8461/?online=http://127.0.0.1:8790
import { createServer } from 'node:http';
import { spawn } from 'node:child_process';
import { parseArgs } from 'node:util';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { WebSocketServer } from 'ws';
import { createHub } from './net-hub.mjs';
import { pass } from '../../services/match/src/matchmaker.js';
import { lobbyTick } from '../../services/match/src/lobby.js';
import { LOBBY_MAP, pickMatchMap } from '../../services/match/src/maps.js';

const here = dirname(fileURLToPath(import.meta.url));
const { values: opt } = parseArgs({
  options: {
    port: { type: 'string', default: '8790' },
    'start-players': { type: 'string', default: '8' },
    'min-players': { type: 'string', default: '2' },
    'lobby-wait': { type: 'string', default: '90' },
    countdown: { type: 'string', default: '10' },
    bots: { type: 'string', default: '6' }, // bots keep each game at this many players (0: none)
    'lobby-map': { type: 'string', default: LOBBY_MAP },
    map: { type: 'string' }, // default: a random C&C map per match
    bin: { type: 'string' },
  },
});
const lobbyConfig = {
  startPlayers: +opt['start-players'],
  minPlayers: +opt['min-players'],
  lobbyWaitMs: +opt['lobby-wait'] * 1000,
  countdownMs: +opt.countdown * 1000,
  botFill: +opt.bots,
};
const t0 = Date.now();
const log = (t) => console.log(`${((Date.now() - t0) / 1000).toFixed(1)} ${t}`);

const hub = createHub();
const tickets = new Map(); // id -> { ticket, ws }
const matches = new Map(); // id -> { id, teams, startedAt, durationMs, maxPlayers, region, mus, proc, lobby, players:Set }

function startServer(match) {
  const args = [join(here, 'run-server.mjs'), '--net', `ws://127.0.0.1:${opt.port}/room/${match.id}`,
    '--map', opt['lobby-map'], '--next-map', match.matchMap, '--work', `/tmp/rebel-server-${match.id}`, '--name', `Open Commando ${match.id}`];
  if (opt.bin) args.push('--bin', opt.bin);
  const proc = spawn(process.execPath, args, { stdio: ['pipe', 'pipe', 'pipe'] });
  match.proc = proc;
  const lobbyMap = opt['lobby-map'].replace(/\.mix$/i, '');
  let buffer = '';
  const onLine = (line) => {
    if (/\[net\] rtt|Load \d+%/.test(line)) return;
    if (/joined the game|left the game|Loading level|Warmup|Gameplay|Match|Lobby|abort|Assert/.test(line)) log(`${match.id}: ${line}`);
    let m;
    if ((m = /Player (\S+) joined the game/.exec(line))) match.players.add(m[1]);
    if ((m = /Player (\S+) left the game/.exec(line))) match.players.delete(m[1]);
    if ((m = /Loading level (\S+)/.exec(line))) {
      match.map = m[1];
      if (m[1].replace(/\.mix$/i, '') === lobbyMap) {
        // The lobby map (first start, and again after each match).
        match.lobby = { state: 'lobby', openedAt: Date.now(), players: match.players.size };
      }
    }
    if (/Level loaded OK/.test(line) && match.lobby?.state === 'lobby') {
      proc.stdin.write('warmup on\n'); // once loaded: the console is up by then
      if (lobbyConfig.botFill) proc.stdin.write(`bot_fill ${lobbyConfig.botFill}\n`);
      if (!match.ready) {
        match.ready = true;
        for (const send of match.waiting.splice(0)) send(); // players waiting for the server
      }
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

const console_ = (match, line) => match.proc && match.proc.stdin.write(line + '\n');

// Tell a player where to go once the match's server has its map loaded
// (joining earlier works too, but the engine's connect attempts time out).
function place(entry, match, team) {
  tickets.delete(entry.ticket.id);
  const send = () => {
    entry.ws.send(JSON.stringify({ type: 'match', room: match.id, server: '10.0.0.1:4848', team }));
    entry.ws.close(1000, 'matched');
  };
  if (match.ready) send();
  else match.waiting.push(send);
}

setInterval(() => {
  const now = Date.now();
  // Matches still open to new players: in their lobby, or early in play.
  const open = [...matches.values()].filter((m) => !m.lobby || m.lobby.state !== 'live' || now - m.liveAt < m.durationMs / 2);
  const { backfills, newMatches } = pass({
    tickets: [...tickets.values()].map((e) => e.ticket),
    matches: open,
    now,
    config: { minPlayers: 1, botFillAfterMs: 0, minHumansForBots: 1, pingStartMs: 1000, pingMaxMs: 1000 },
  });
  for (const b of backfills) {
    const m = matches.get(b.matchId), e = tickets.get(b.ticketId);
    if (!m || !e) continue;
    m.teams[b.team].push(...e.ticket.players.map((p) => p.id));
    log(`${e.ticket.players[0].name} -> ${m.id} (joins the ${m.lobby?.state || 'booting'} match)`);
    place(e, m, b.team);
  }
  for (const nm of newMatches) {
    const id = 'm' + Math.random().toString(36).slice(2, 8);
    const match = { matchMap: opt.map || pickMatchMap(), id, region: nm.region, teams: nm.teams, mus: {}, startedAt: now, durationMs: 30 * 60000, maxPlayers: 16, players: new Set(), waiting: [], lobby: null, ready: false };
    matches.set(id, match);
    log(`new match ${id}, starting its server (lobby ${opt['lobby-map']}, then ${match.matchMap})`);
    startServer(match);
    for (const tid of nm.tickets) {
      const e = tickets.get(tid);
      if (e) {
        log(`${e.ticket.players[0].name} -> ${id}`);
        place(e, match, nm.teams[0].includes(e.ticket.players[0].id) ? 0 : 1);
      }
    }
  }
  // Lobbies.
  for (const m of matches.values()) {
    if (!m.lobby) continue;
    m.lobby.players = m.players.size;
    for (const a of lobbyTick(m.lobby, now, lobbyConfig)) {
      if (a.type === 'message') console_(m, `msg ${a.text}`); // chat line (amsg is a modal popup)
      if (a.type === 'start') {
        log(`${m.id}: match starts (${m.players.size} players)`);
        m.liveAt = now;
        console_(m, 'warmup off');
        console_(m, 'gameover');
      }
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
      lobbyMap: opt['lobby-map'],
      matches: [...matches.values()].map((m) => ({ id: m.id, map: m.map, matchMap: m.matchMap, ready: m.ready, lobby: m.lobby && { state: m.lobby.state, players: m.lobby.players }, players: [...m.players] })),
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
