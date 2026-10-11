// The region's matchmaker (spike S8b): players wait here over a WebSocket;
// every second the matching pass (matchmaker.js) runs; new matches get a
// server (MatchServer container) and each player is told where to go.
//
//   GET /play?name=<nick>  (WebSocket)  ->  {"type":"queued"}, then
//     {"type":"status","waitedS":..,"inQueue":..} every few seconds, then
//     {"type":"match","room":"<id>","server":"10.0.0.1:4848","team":0|1}
//
// The page then opens /room/<id> and starts the game with
// +connect 10.0.0.1:4848 (docs/NET_SPIKES.md, S4).
import { DurableObject } from 'cloudflare:workers';
import { pass } from './matchmaker.js';

const MATCH_MINUTES = 30;

export class Matchmaker extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.tickets = new Map(); // ticket id -> { ticket, ws }
    this.matches = new Map(); // match id -> match (for backfill)
    this.config = { region: env.REGION || 'WEUR', minPlayers: +(env.MIN_PLAYERS || 8), botFillAfterMs: +(env.BOT_FILL_AFTER_MS || 45000) };
  }

  async fetch(request) {
    const url = new URL(request.url);
    const [client, server] = Object.values(new WebSocketPair());
    server.accept();
    const id = crypto.randomUUID();
    const name = (url.searchParams.get('name') || 'Player').replace(/[^\w\-[\]]/g, '').slice(0, 20) || 'Player';
    const ticket = {
      id,
      players: [{ id, name, mu: 25 }],
      // Pings per region come from the page later; one region for now.
      pings: { [this.config.region]: +(url.searchParams.get('ping') || 50) },
      enqueuedAt: Date.now(),
    };
    this.tickets.set(id, { ticket, ws: server });
    server.addEventListener('close', () => this.tickets.delete(id));
    server.addEventListener('error', () => this.tickets.delete(id));
    server.send(JSON.stringify({ type: 'queued', region: this.config.region }));
    if (!(await this.ctx.storage.getAlarm())) await this.ctx.storage.setAlarm(Date.now() + 1000);
    return new Response(null, { status: 101, webSocket: client });
  }

  async alarm() {
    const now = Date.now();
    for (const [mid, m] of this.matches) if (now - m.startedAt > m.durationMs) this.matches.delete(mid);
    const tickets = [...this.tickets.values()].map((e) => e.ticket);
    const { backfills, newMatches } = pass({ tickets, matches: [...this.matches.values()], now, config: this.config });

    for (const b of backfills) {
      const m = this.matches.get(b.matchId);
      const e = this.tickets.get(b.ticketId);
      if (!m || !e) continue;
      for (const p of e.ticket.players) m.teams[b.team].push(p.id);
      this.send(e, m.id, b.team);
    }
    for (const nm of newMatches) {
      const id = 'm-' + crypto.randomUUID().slice(0, 8);
      const match = { id, region: nm.region, teams: nm.teams, mus: {}, startedAt: now, durationMs: MATCH_MINUTES * 60000, maxPlayers: 16 };
      this.matches.set(id, match);
      // Start the server; players connect to the room meanwhile and the
      // engine retries its connect until the server answers.
      this.ctx.waitUntil(this.env.MATCH_SERVER.getByName(id).start({ envVars: { REBEL_NET: `${this.env.PUBLIC_WSS}/room/${id}` } }).catch((err) => console.log(`start ${id}: ${err}`)));
      for (const tid of nm.tickets) {
        const e = this.tickets.get(tid);
        if (e) this.send(e, id, nm.teams[0].includes(e.ticket.players[0].id) ? 0 : 1);
      }
    }
    // Waiting players hear how it's going.
    if (Math.floor(now / 1000) % 3 === 0) {
      for (const e of this.tickets.values()) e.ws.send(JSON.stringify({ type: 'status', waitedS: Math.round((now - e.ticket.enqueuedAt) / 1000), inQueue: this.tickets.size }));
    }
    if (this.tickets.size) await this.ctx.storage.setAlarm(Date.now() + 1000);
  }

  send(entry, room, team) {
    entry.ws.send(JSON.stringify({ type: 'match', room, server: '10.0.0.1:4848', team }));
    this.tickets.delete(entry.ticket.id);
    entry.ws.close(1000, 'matched');
  }
}
