// Match service: matchmakers, rooms and match servers, per region.
//
//   GET  /regions                  the regions and their names
//   GET  /ping?region=<r>          WebSocket echo from that region (the page
//                                  measures its round trip to each)
//   GET  /play?name=<n>&region=<r> WebSocket: wait for a match (matchmaker-do.js)
//   GET  /status                   every region's matchmaker: queue, matches, lobbies
//   GET  /room/<id>                WebSocket: join match room <id> (web/net.js frames)
//   POST /match/<id>/start|stop    start/stop a match server by hand (START_TOKEN)
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

const json = (v, status = 200) => new Response(JSON.stringify(v, null, 2), { status, headers: { 'content-type': 'application/json', 'access-control-allow-origin': '*' } });
const wantsSocket = (request) => request.headers.get('Upgrade') === 'websocket';
const placed = (ns, name, region) => ns.get(ns.idFromName(name), { locationHint: region });
const matchmaker = (env, region) => placed(env.MATCHMAKER, `mm-${region}`, region);

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
      const region = REGIONS[url.searchParams.get('region')] ? url.searchParams.get('region') : 'weur';
      if (!wantsSocket(request)) return new Response('websocket expected', { status: 426 });
      return matchmaker(env, region).fetch(new Request(`${url.origin}/play${url.search.includes('region=') ? url.search : `${url.search}${url.search ? '&' : '?'}region=${region}`}`, request));
    }
    if (url.pathname === '/status') {
      const all = await Promise.all(Object.keys(REGIONS).map((r) => matchmaker(env, r).fetch(new Request(`${url.origin}/status?region=${r}`)).then((res) => res.json()).catch(() => null)));
      return json({ regions: Object.keys(REGIONS), queue: all.flatMap((s) => s?.queue || []), matches: all.flatMap((s) => s?.matches || []) });
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
