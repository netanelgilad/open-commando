// The region's matchmaker: players wait here over a WebSocket; every second
// the matching pass (matchmaker.js) places them at once in a running match
// with room for another human, or starts a new one. A match server plays
// real C&C games straight away, map after map (a random rotation); bots fill
// every game to BOT_FILL players and each human who joins takes a bot's
// place, teams kept even (bots_web.cpp, bot_fill). Match servers are
// MatchServer containers, driven through their status page and console
// (tools/server/run-server.mjs --status-port).
//
//   GET /play?name=<nick>  (WebSocket)  ->  {"type":"queued"}, then
//     {"type":"status","waitedS":..,"inQueue":..} every few seconds, then
//     {"type":"match","room":"<id>","server":"10.0.0.1:4848","team":0|1}
//     once the match's server has its map loaded.
//
// The page then opens /room/<id> and starts the game with
// +connect 10.0.0.1:4848 (docs/NET_SPIKES.md, S4).
//
//   GET /history         the region's last matches (a summary each)
//   GET /history/<id>    one match: a sample every 10 s (map, humans, bots,
//                        fps, timer lateness, memory, AI spend) and, once it
//                        ended, how and the server's last log lines
import { DurableObject } from 'cloudflare:workers';
import { pass } from './matchmaker.js';
import { mapRotation } from './maps.js';
import { REGIONS } from './regions.js';
import { aiKey } from './ai.js';

const HUMANS_PER_MATCH = 32; // as C&C servers commonly ran (16 a side); bots top games up to BOT_FILL
const SAMPLE_EVERY_MS = 10000; // match history: one sample every 10 s
const HISTORY_MATCHES = 50; // matches kept per region

export class Matchmaker extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.tickets = new Map(); // ticket id -> { ticket, ws }
    this.matches = new Map(); // match id -> { ...match for pass(), ready, waiting, joining, players }
    this.region = null; // set by the first request (?region=), see index.js
    this.botFill = +(env.BOT_FILL || 0);
  }

  async fetch(request) {
    const url = new URL(request.url);
    this.region ??= REGIONS[url.searchParams.get('region')] ? url.searchParams.get('region') : 'weur';
    if (url.pathname === '/status') return Response.json(this.status());
    if (url.pathname === '/history') return Response.json((await this.ctx.storage.get('history')) || []);
    const h = /^\/history\/([\w-]+)$/.exec(url.pathname);
    if (h) {
      const live = this.matches.get(h[1]);
      const rec = live ? this.record(live) : await this.ctx.storage.get(`match:${h[1]}`);
      return rec ? Response.json(rec) : new Response('not found', { status: 404 });
    }
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
      queue: [...this.tickets.values()].map((e) => e.ticket.players[0].name),
      matches: [...this.matches.values()].map((m) => ({ id: m.id, region: this.region, map: m.map, ready: m.ready, players: m.players, joining: m.joining.length, perf: m.perf, ai: m.ai })),
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
    await Promise.all([...this.matches.values()].map((m) => this.runMatch(m, now)));
    if (Math.floor(now / 1000) % 3 === 0) {
      for (const e of this.tickets.values()) e.ws.send(JSON.stringify({ type: 'status', waitedS: Math.round((now - e.ticket.enqueuedAt) / 1000), inQueue: this.tickets.size }));
    }
    if (this.tickets.size || this.matches.size) await this.ctx.storage.setAlarm(Date.now() + 1000);
  }

  // Queue -> matches.
  place(now) {
    // Open to new players: a server that answers (or is still booting) with
    // room for another human. Its teams, for the pass, are the humans in it
    // and those on their way, split evenly.
    const alive = (m) => (m.ready ? now - (m.lastOkAt || 0) < 10000 : now - m.startedAt < 2 * 60000);
    const open = [...this.matches.values()].filter(alive).map((m) => {
      const humans = [...m.players, ...m.joining.map((j) => j.id)];
      return { ...m, teams: [humans.filter((_, i) => i % 2 === 0), humans.filter((_, i) => i % 2 === 1)] };
    });
    const { backfills, newMatches } = pass({
      tickets: [...this.tickets.values()].map((e) => e.ticket),
      matches: open,
      now,
      config: { region: this.region, minPlayers: 1, botFillAfterMs: 0, minHumansForBots: 1, pingStartMs: 1000, pingMaxMs: 1000 },
    });
    for (const b of backfills) {
      const m = this.matches.get(b.matchId), e = this.tickets.get(b.ticketId);
      if (m && e) this.send(e, m, b.team, now);
    }
    for (const nm of newMatches) {
      const id = `m-${this.region}-${crypto.randomUUID().slice(0, 8)}`;
      const match = { id, region: nm.region, mus: {}, startedAt: now, durationMs: 0, maxPlayers: HUMANS_PER_MATCH, ready: false, waiting: [], failing: [], joining: [], players: [], map: null, maps: mapRotation() };
      this.matches.set(id, match);
      console.log(`new match ${id} (${match.maps[0]}): starting its server`);
      this.startServer(match, now);
      for (const tid of nm.tickets) {
        const e = this.tickets.get(tid);
        if (e) this.send(e, match, nm.teams[0].includes(e.ticket.players[0].id) ? 0 : 1, now);
      }
    }
  }

  // Start (or retry starting) a match's server; starts can fail for a while
  // after a deploy, while the container rolls out.
  startServer(match, now) {
    match.lastStartAt = now;
    const start = async () => {
      const https = this.env.PUBLIC_WSS.replace(/^wss:/, 'https:');
      const key = await aiKey(this.env.START_TOKEN, match.id); // this match's key (ai.js)
      const envVars = { REBEL_NET: `${this.env.PUBLIC_WSS}/room/${match.id}`, REBEL_MAPS: match.maps.join(','), REBEL_RECORD_URL: `${https}/record/${match.id}`, REBEL_MATCH_KEY: key };
      if (this.env.BOT_BRAIN && this.env.BOT_BRAIN !== 'off') {
        // The bots' brain calls the models through /ai/<match> (ai.js).
        envVars.REBEL_BOT_BRAIN = this.env.BOT_BRAIN;
        envVars.REBEL_AI_URL = `${https}/ai/${match.id}`;
        envVars.REBEL_AI_KEY = key;
      }
      await this.server(match.id).start({ envVars });
    };
    this.ctx.waitUntil(start().catch((err) => console.log(`start ${match.id}: ${err}`)));
  }

  // A match whose server never came up (no capacity, a broken start) or
  // died: the waiting players hear why, and the server is cleaned up.
  abandon(m, reason) {
    console.log(`${m.id}: abandoned (${reason})`);
    if (m.lastLog) console.log(`${m.id}: its last log lines:\n${m.lastLog.slice(-25).join('\n')}`);
    this.end(m, reason);
    for (const fail of m.failing.splice(0)) fail(reason);
    m.waiting.length = 0;
    this.ctx.waitUntil(this.server(m.id).destroy().catch(() => {}));
  }

  // A player goes to their match once its server has the map loaded.
  send(entry, match, team, now) {
    this.tickets.delete(entry.ticket.id);
    match.joining.push({ id: entry.ticket.id, at: now });
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

  // A match's history record: its samples, and how it ended.
  record(m) {
    const samples = m.samples || [];
    return {
      id: m.id,
      region: this.region,
      startedAt: m.startedAt,
      endedAt: m.endedAt || null,
      end: m.endReason || null,
      maps: [...new Set(samples.map((x) => x.map).filter(Boolean))],
      peakHumans: Math.max(0, ...samples.map((x) => x.humans)),
      aiUsd: samples.at(-1)?.aiUsd ?? null,
      minFps: samples.length ? Math.min(...samples.map((x) => x.fps ?? 99)) : null,
      samples,
      lastLog: m.endedAt ? m.lastLog || [] : undefined,
    };
  }

  // A match is over: keep its record (the last HISTORY_MATCHES), forget it.
  end(m, reason) {
    this.matches.delete(m.id);
    m.endedAt = Date.now();
    m.endReason = reason;
    const rec = this.record(m);
    const save = async () => {
      await this.ctx.storage.put(`match:${m.id}`, rec);
      await this.env.GAMES?.put(`matches/${m.id}/summary.json`, JSON.stringify(rec)); // next to its record
      const { samples, lastLog, ...summary } = rec;
      const history = [summary, ...((await this.ctx.storage.get('history')) || []).filter((x) => x.id !== m.id)];
      for (const old of history.splice(HISTORY_MATCHES)) await this.ctx.storage.delete(`match:${old.id}`);
      await this.ctx.storage.put('history', history);
    };
    this.ctx.waitUntil(save().catch((e) => console.log(`${m.id}: history: ${e}`)));
  }

  // One match: read its server's status; bots on once it's up; stop it when
  // no humans are left.
  async runMatch(m, now) {
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
    m.lastLog = s.log; // kept for when the server dies (printed by abandon)
    if (now - (m.lastSampleAt || 0) >= SAMPLE_EVERY_MS) {
      m.lastSampleAt = now;
      (m.samples ??= []).push({ t: now, map: s.map, humans: (s.players || []).length, bots: s.bots ?? null, fps: s.perf?.fps ?? null, lateMs: s.perf?.lagMaxMs ?? null, rssMb: s.perf?.rssMb ?? null, aiUsd: s.ai?.usd ?? null });
      if (m.samples.length % 6 === 0) this.ctx.waitUntil(this.ctx.storage.put(`match:${m.id}`, this.record(m))); // a minute's worth
    }
    m.map = s.map;
    m.perf = s.perf;
    m.ai = s.ai;
    // Humans on their way: until they show up in the game (3 minutes at most).
    const arrived = (s.players || []).length - m.players.length;
    if (arrived > 0) m.joining.splice(0, arrived);
    m.joining = m.joining.filter((j) => now - j.at < 3 * 60000);
    m.players = s.players || [];
    if (s.loaded && !m.ready) {
      m.ready = true;
      if (this.botFill) await this.console(m, `bot_fill ${this.botFill}`);
      for (const go of m.waiting.splice(0)) go();
      m.failing.length = 0;
    }
    // No humans: stop the server (it costs while it runs; bots only play with
    // humans). The first ones get 90 s to load in; after that, 30 s.
    if (m.players.length || m.joining.length) m.lastSeenPlayersAt = now;
    if (m.players.length) m.hadPlayers = true;
    if (now - (m.lastSeenPlayersAt || m.startedAt) > (m.hadPlayers ? 30000 : 90000)) {
      console.log(`${m.id}: no players, stopping`);
      this.end(m, 'no players left');
      await this.server(m.id).stop().catch(() => {});
      this.ctx.waitUntil(this.server(m.id).destroy().catch(() => {}));
    }
  }
}
