// The region's matchmaker: players wait here over a WebSocket; every second
// the matching pass (matchmaker.js) places them in a match at once (a new
// one, or backfill into one still in its lobby or early in play), and each
// match's lobby runs (lobby.js): its server opens in warmup on the lobby map,
// and when the lobby is ready "warmup off" + "gameover" move everyone to the
// match map. Match servers are MatchServer containers, driven through their
// status page and console (tools/server/run-server.mjs --status-port).
//
//   GET /play?name=<nick>  (WebSocket)  ->  {"type":"queued"}, then
//     {"type":"status","waitedS":..,"inQueue":..} every few seconds, then
//     {"type":"match","room":"<id>","server":"10.0.0.1:4848","team":0|1}
//     once the match's server has its map loaded.
//
// The page then opens /room/<id> and starts the game with
// +connect 10.0.0.1:4848 (docs/NET_SPIKES.md, S4).
import { DurableObject } from 'cloudflare:workers';
import { pass } from './matchmaker.js';
import { lobbyTick } from './lobby.js';
import { LOBBY_MAP, pickMatchMap } from './maps.js';
import { REGIONS } from './regions.js';
import { aiKey } from './ai.js';

const MATCH_MINUTES = 30;

export class Matchmaker extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.tickets = new Map(); // ticket id -> { ticket, ws }
    this.matches = new Map(); // match id -> { ...match for pass(), lobby, ready, waiting, players }
    this.region = null; // set by the first request (?region=), see index.js
    this.lobbyConfig = {
      startPlayers: +(env.START_PLAYERS || 8),
      minPlayers: +(env.MIN_PLAYERS || 2),
      lobbyWaitMs: +(env.LOBBY_WAIT_S || 90) * 1000,
      countdownMs: 10000,
      botFill: +(env.BOT_FILL || 0),
    };
  }

  async fetch(request) {
    const url = new URL(request.url);
    this.region ??= REGIONS[url.searchParams.get('region')] ? url.searchParams.get('region') : 'weur';
    if (url.pathname === '/status') return Response.json(this.status());
    const [client, server] = Object.values(new WebSocketPair());
    server.accept();
    const id = crypto.randomUUID();
    const name = (url.searchParams.get('name') || 'Player').replace(/[^\w\-[\]]/g, '').slice(0, 20) || 'Player';
    const ping = +url.searchParams.get('ping') || 50;
    this.tickets.set(id, {
      ticket: { id, players: [{ id, name, mu: 25 }], pings: { [this.region]: ping }, enqueuedAt: Date.now() },
      ws: server,
    });
    server.addEventListener('close', () => this.tickets.delete(id));
    server.addEventListener('error', () => this.tickets.delete(id));
    server.send(JSON.stringify({ type: 'queued', region: this.region }));
    await this.wake();
    return new Response(null, { status: 101, webSocket: client });
  }

  status() {
    return {
      region: this.region,
      lobbyMap: LOBBY_MAP,
      queue: [...this.tickets.values()].map((e) => e.ticket.players[0].name),
      matches: [...this.matches.values()].map((m) => ({ id: m.id, region: this.region, map: m.map, matchMap: m.matchMap, ready: m.ready, lobby: m.lobby && { state: m.lobby.state, players: m.lobby.players }, players: m.players })),
    };
  }

  async wake() {
    if (!(await this.ctx.storage.getAlarm())) await this.ctx.storage.setAlarm(Date.now() + 1000);
  }

  server(id) {
    return this.env[REGIONS[this.region].binding].getByName(id);
  }

  async console(match, line) {
    try {
      await this.server(match.id).fetch(new Request('http://container/console', { method: 'POST', body: line }));
    } catch (e) {
      console.log(`${match.id}: console "${line}": ${e}`);
    }
  }

  async alarm() {
    const now = Date.now();
    this.place(now);
    await Promise.all([...this.matches.values()].map((m) => this.runLobby(m, now)));
    if (Math.floor(now / 1000) % 3 === 0) {
      for (const e of this.tickets.values()) e.ws.send(JSON.stringify({ type: 'status', waitedS: Math.round((now - e.ticket.enqueuedAt) / 1000), inQueue: this.tickets.size }));
    }
    if (this.tickets.size || this.matches.size) await this.ctx.storage.setAlarm(Date.now() + 1000);
  }

  // Queue -> matches.
  place(now) {
    // Open to new players: a server that answers (or is still booting), in its
    // lobby or early in play.
    const alive = (m) => (m.ready ? now - (m.lastOkAt || 0) < 10000 : now - m.startedAt < 2 * 60000);
    const open = [...this.matches.values()].filter((m) => alive(m) && (!m.lobby || m.lobby.state !== 'live' || now - m.liveAt < m.durationMs / 2));
    const { backfills, newMatches } = pass({
      tickets: [...this.tickets.values()].map((e) => e.ticket),
      matches: open,
      now,
      // Place everyone at once: lobbies fill up; the lobby decides when to start.
      config: { region: this.region, minPlayers: 1, botFillAfterMs: 0, minHumansForBots: 1, pingStartMs: 1000, pingMaxMs: 1000 },
    });
    for (const b of backfills) {
      const m = this.matches.get(b.matchId), e = this.tickets.get(b.ticketId);
      if (!m || !e) continue;
      m.teams[b.team].push(...e.ticket.players.map((p) => p.id));
      this.send(e, m, b.team);
    }
    for (const nm of newMatches) {
      const id = `m-${this.region}-${crypto.randomUUID().slice(0, 8)}`;
      const match = { id, region: nm.region, teams: nm.teams, mus: {}, startedAt: now, durationMs: MATCH_MINUTES * 60000, maxPlayers: 16, lobby: null, ready: false, waiting: [], failing: [], players: [], map: null, matchMap: pickMatchMap() };
      this.matches.set(id, match);
      console.log(`new match ${id} (${match.matchMap}): starting its server`);
      this.startServer(match, now);
      for (const tid of nm.tickets) {
        const e = this.tickets.get(tid);
        if (e) this.send(e, match, nm.teams[0].includes(e.ticket.players[0].id) ? 0 : 1);
      }
    }
  }

  // Start (or retry starting) a match's server; starts can fail for a while
  // after a deploy, while the container rolls out.
  startServer(match, now) {
    match.lastStartAt = now;
    const start = async () => {
      const envVars = { REBEL_NET: `${this.env.PUBLIC_WSS}/room/${match.id}`, REBEL_MAP: LOBBY_MAP, REBEL_NEXT_MAP: match.matchMap };
      if (this.env.BOT_BRAIN && this.env.BOT_BRAIN !== 'off') {
        // The bots' brain calls the models through /ai/<match> (ai.js).
        envVars.REBEL_BOT_BRAIN = this.env.BOT_BRAIN;
        envVars.REBEL_AI_URL = `${this.env.PUBLIC_WSS.replace(/^wss:/, 'https:')}/ai/${match.id}`;
        envVars.REBEL_AI_KEY = await aiKey(this.env.START_TOKEN, match.id);
      }
      await this.server(match.id).start({ envVars });
    };
    this.ctx.waitUntil(start().catch((err) => console.log(`start ${match.id}: ${err}`)));
  }

  // A match whose server never came up (no capacity, a broken start): the
  // waiting players hear why, and the server is cleaned up.
  abandon(m, reason) {
    console.log(`${m.id}: abandoned (${reason})`);
    this.matches.delete(m.id);
    for (const fail of m.failing.splice(0)) fail(reason);
    m.waiting.length = 0;
    this.ctx.waitUntil(this.server(m.id).destroy().catch(() => {}));
  }

  // A player goes to their match once its server has the map loaded.
  send(entry, match, team) {
    this.tickets.delete(entry.ticket.id);
    const go = () => {
      try {
        entry.ws.send(JSON.stringify({ type: 'match', room: match.id, server: '10.0.0.1:4848', team }));
        entry.ws.close(1000, 'matched');
      } catch {}
    };
    if (match.ready) go();
    else {
      match.waiting.push(go);
      match.failing.push((reason) => {
        try {
          entry.ws.send(JSON.stringify({ type: 'error', message: reason }));
          entry.ws.close(1000, 'failed');
        } catch {}
      });
    }
  }

  // One match: read its server's status, run its lobby.
  async runLobby(m, now) {
    let s;
    try {
      const res = await this.server(m.id).fetch(new Request('http://container/'));
      s = await res.json();
    } catch {
      if (m.ready && now - (m.lastOkAt || 0) > 30000) return this.abandon(m, 'The game server stopped.'); // crashed or stopped
      if (!m.ready && now - m.startedAt > 2 * 60000) this.abandon(m, 'No game server became available, please try again.');
      else if (!m.ready && now - m.lastStartAt > 15000) this.startServer(m, now);
      return;
    }
    m.lastOkAt = now;
    m.players = s.players || [];
    if (s.map !== m.map) {
      m.map = s.map;
      if (s.map === LOBBY_MAP) m.lobby = { state: 'lobby', openedAt: now, players: m.players.length };
    }
    if (s.map === LOBBY_MAP && s.loaded && !s.warmup) {
      await this.console(m, 'warmup on');
      if (this.lobbyConfig.botFill) await this.console(m, `bot_fill ${this.lobbyConfig.botFill}`);
    }
    if (s.loaded && !m.ready) {
      m.ready = true;
      for (const go of m.waiting.splice(0)) go();
      m.failing.length = 0;
    }
    if (!m.lobby) return;
    m.lobby.players = m.players.length;
    for (const a of lobbyTick(m.lobby, now, this.lobbyConfig)) {
      if (a.type === 'message') await this.console(m, `msg ${a.text}`);
      if (a.type === 'start') {
        console.log(`${m.id}: match starts (${m.players.length} players)`);
        m.liveAt = now;
        await this.console(m, 'warmup off');
        await this.console(m, 'gameover');
      }
    }
    // No humans: stop the server soon (it costs while it runs; bots only play
    // with humans). A lobby gets 90 s for the players sent to it to load in.
    if (m.players.length || m.waiting.length) m.lastSeenPlayersAt = now;
    if (m.lobby.state !== 'live' && now - (m.lastSeenPlayersAt || m.startedAt) > 90000) {
      console.log(`${m.id}: lobby empty, stopping`);
      this.matches.delete(m.id);
      this.ctx.waitUntil(this.server(m.id).destroy().catch(() => {}));
      return;
    }
    // Everyone left the match: stop the server.
    if (m.lobby.state === 'live' && !m.players.length && now - m.liveAt > 60000 && now - (m.lastSeenPlayersAt || 0) > 30000) {
      console.log(`${m.id}: empty, stopping`);
      this.matches.delete(m.id);
      await this.server(m.id).stop().catch(() => {});
      this.ctx.waitUntil(this.server(m.id).destroy().catch(() => {}));
    }
  }
}
