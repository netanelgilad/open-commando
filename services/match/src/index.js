// Match service: matchmakers, rooms and match servers, per region.
//
//   GET  /regions                  the regions and their names
//   GET  /ping?region=<r>          WebSocket echo from that region (the page
//                                  measures its round trip to each)
//   GET  /play?name=<n>&region=<r> WebSocket: wait for a match (matchmaker-do.js)
//   GET  /status                   every region's matchmaker: queue, matches, lobbies
//   GET  /stats                    counts for the front screen (online, in lobbies, matches)
//   GET  /presence                 WebSocket held open by a front screen (counts visitors)
//   GET  /room/<id>                WebSocket: join match room <id> (web/net.js frames)
//   POST /match/<id>/start|stop    start/stop a match server by hand (START_TOKEN)
//   POST /ai/<id>                  the bots' models for match <id> (Workers AI), for its
//                                  server: {model, input} with the match's key (aiKey)
//   GET  /match/<id>               a match server's status
//
// Regions are Cloudflare's container placement regions. A match id carries
// its region ("m-weur-1a2b3c4d"); the region's matchmaker, the match's room
// and its server all run there (Durable Object location hints; a container
// class per region pinned with placement constraints, see wrangler.jsonc).
import { Container } from '@cloudflare/containers';
import { DurableObject } from 'cloudflare:workers';
import { Room } from '../../hub/src/index.js';
import { Matchmaker } from './matchmaker-do.js';
import { REGIONS, regionOf } from './regions.js';
import { aiKey, AI_MODELS } from './ai.js';

export { Room, Matchmaker };

class MatchServer extends Container {
  defaultPort = 8080; // run-server.mjs status page
  sleepAfter = '20m'; // no status requests for this long: stop
}
export class MatchServerWEUR extends MatchServer {}
export class MatchServerEEUR extends MatchServer {}
export class MatchServerENAM extends MatchServer {}
export class MatchServerWNAM extends MatchServer {}

// Answers pings from where it was placed (one per region).
export class Ping extends DurableObject {
  async fetch() {
    const [client, server] = Object.values(new WebSocketPair());
    server.accept();
    server.addEventListener('message', (e) => server.send(e.data));
    return new Response(null, { status: 101, webSocket: client });
  }
}

// Front-screen visitors: one WebSocket each, held open (hibernating, so idle
// sockets cost nothing); the count is the number of sockets.
export class Presence extends DurableObject {
  async fetch(request) {
    if (new URL(request.url).pathname === '/count') return Response.json({ visitors: this.ctx.getWebSockets().length });
    const [client, server] = Object.values(new WebSocketPair());
    this.ctx.acceptWebSocket(server);
    return new Response(null, { status: 101, webSocket: client });
  }
  webSocketMessage() {}
  webSocketClose(ws) { try { ws.close(); } catch {} }
}

const json = (v, status = 200) => new Response(JSON.stringify(v, null, 2), { status, headers: { 'content-type': 'application/json', 'access-control-allow-origin': '*' } });
const wantsSocket = (request) => request.headers.get('Upgrade') === 'websocket';
const placed = (ns, name, region) => ns.get(ns.idFromName(name), { locationHint: region });
const matchmaker = (env, region) => placed(env.MATCHMAKER, `mm-${region}`, region);

// pings=weur:78,eeur:74,... (the page's round trips to each region).
const parsePings = (url) => Object.fromEntries((url.searchParams.get('pings') || '').split(',').map((p) => p.split(':')).filter(([r, ms]) => REGIONS[r] && +ms > 0).map(([r, ms]) => [r, +ms]));

// Where a player queues: among the regions they reach about as well as their
// best (within 60 ms, and under 150 ms), the one with the most players
// already waiting or in open lobbies; else their closest. Keeps players near
// two regions together instead of splitting them on ping noise.
async function chooseRegion(env, url) {
  const asked = url.searchParams.get('region');
  if (REGIONS[asked] && !url.searchParams.get('pings')) return asked;
  const pings = parsePings(url);
  const ranked = Object.keys(pings).sort((a, b) => pings[a] - pings[b]);
  if (!ranked.length) return 'weur';
  const best = pings[ranked[0]];
  const near = ranked.filter((r) => pings[r] <= best + 60 && pings[r] <= Math.max(150, best));
  if (near.length < 2) return ranked[0];
  const busy = await Promise.all(near.map((r) => matchmaker(env, r).fetch(new Request(`${url.origin}/status?region=${r}`)).then((res) => res.json()).then((s) => s.queue.length + s.matches.filter((m) => m.lobby?.state !== 'live').reduce((n, m) => n + m.players.length + 1, 0)).catch(() => 0)));
  // Most players wins; a tie goes to the earlier region in REGIONS (so
  // players queueing at the same moment agree).
  const order = Object.keys(REGIONS);
  let pick = 0;
  for (let i = 1; i < near.length; i++) {
    if (busy[i] > busy[pick] || (busy[i] === busy[pick] && order.indexOf(near[i]) < order.indexOf(near[pick]))) pick = i;
  }
  return near[pick];
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === '/regions') return json(REGIONS);
    if (url.pathname === '/ping') {
      const region = url.searchParams.get('region');
      if (!REGIONS[region] || !wantsSocket(request)) return new Response('bad request', { status: 400 });
      return placed(env.PING, `ping-${region}`, region).fetch(request);
    }
    if (url.pathname === '/play') {
      if (!wantsSocket(request)) return new Response('websocket expected', { status: 426 });
      const region = await chooseRegion(env, url);
      const q = new URLSearchParams(url.search);
      q.set('region', region);
      q.set('ping', String(parsePings(url)[region] ?? 50));
      return matchmaker(env, region).fetch(new Request(`${url.origin}/play?${q}`, request));
    }
    if (url.pathname === '/presence') {
      if (!wantsSocket(request)) return new Response('websocket expected', { status: 426 });
      return env.PRESENCE.getByName('global').fetch(request);
    }
    if (url.pathname === '/stats') {
      const [regions, presence] = await Promise.all([
        Promise.all(Object.keys(REGIONS).map((r) => matchmaker(env, r).fetch(new Request(`${url.origin}/status?region=${r}`)).then((res) => res.json()).catch(() => null))),
        env.PRESENCE.getByName('global').fetch(new Request(`${url.origin}/count`)).then((res) => res.json()).catch(() => ({ visitors: 0 })),
      ]);
      const matches = regions.flatMap((s) => s?.matches || []);
      const queued = regions.reduce((n, s) => n + (s?.queue.length || 0), 0);
      const inLobbies = matches.filter((m) => m.lobby && m.lobby.state !== 'live').reduce((n, m) => n + m.players.length, 0);
      const live = matches.filter((m) => m.lobby?.state === 'live');
      const inMatches = live.reduce((n, m) => n + m.players.length, 0);
      return json({
        online: presence.visitors + queued + inLobbies + inMatches,
        onFrontScreen: presence.visitors,
        queued,
        inLobbies,
        lobbies: matches.filter((m) => m.lobby && m.lobby.state !== 'live').length,
        inMatches,
        matches: live.length,
        regions: Object.fromEntries(Object.keys(REGIONS).map((r, i) => [r, (regions[i]?.matches || []).reduce((n, m) => n + m.players.length, 0)])),
      });
    }
    if (url.pathname === '/status') {
      const all = await Promise.all(Object.keys(REGIONS).map((r) => matchmaker(env, r).fetch(new Request(`${url.origin}/status?region=${r}`)).then((res) => res.json()).catch(() => null)));
      return json({ regions: Object.keys(REGIONS), queue: all.flatMap((s) => s?.queue || []), matches: all.flatMap((s) => s?.matches || []) });
    }
    const ai = /^\/ai\/([\w-]{1,64})$/.exec(url.pathname);
    if (ai) {
      if (request.method !== 'POST') return new Response('POST expected', { status: 405 });
      if (!env.START_TOKEN || request.headers.get('Authorization') !== `Bearer ${await aiKey(env.START_TOKEN, ai[1])}`) return new Response('forbidden', { status: 403 });
      const { model, input } = await request.json().catch(() => ({}));
      if (!AI_MODELS[model] || !input) return new Response('bad request', { status: 400 });
      const t0 = Date.now();
      try {
        const output = await env.AI.run(AI_MODELS[model], input);
        return json({ ms: Date.now() - t0, output });
      } catch (e) {
        return json({ ms: Date.now() - t0, error: String(e) }, 502);
      }
    }
    const room = /^\/room\/([\w-]{1,64})$/.exec(url.pathname);
    if (room) {
      if (!wantsSocket(request)) return new Response('websocket expected', { status: 426 });
      return placed(env.ROOMS, room[1], regionOf(room[1])).fetch(request);
    }
    const m = /^\/match\/([\w-]{1,64})(\/start|\/stop)?$/.exec(url.pathname);
    if (!m) return new Response('not found', { status: 404 });
    const [, id, action] = m;
    const region = regionOf(id);
    const server = env[REGIONS[region].binding].getByName(id);
    if (action) {
      if (request.method !== 'POST') return new Response('POST expected', { status: 405 });
      if (!env.START_TOKEN || request.headers.get('Authorization') !== `Bearer ${env.START_TOKEN}`) return new Response('forbidden', { status: 403 });
      if (action === '/stop') {
        // stop() asks nicely (SIGTERM); destroy() if it is still up.
        await server.stop().catch(() => {});
        if ((await server.getState()).status !== 'stopped') await server.destroy().catch(() => {});
        return json({ stopped: id, state: await server.getState() });
      }
      const t0 = Date.now();
      await server.start({ envVars: { REBEL_NET: `wss://${url.host}/room/${id}` } }, { portReadyTimeoutMS: 120000 });
      return json({ started: id, region, ms: Date.now() - t0 });
    }
    // Status: the server's own page, if it is running.
    const state = await server.getState();
    if (state.status !== 'healthy' && state.status !== 'running') return json({ id, region, state });
    const res = await server.fetch(new Request('http://container/status'));
    return json({ id, region, state, server: await res.json().catch(() => null) });
  },
};
