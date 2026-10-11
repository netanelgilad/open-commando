// Match service (spike S6): rooms and their match servers.
//
//   GET  /play?name=<nick>   WebSocket: wait for a match (matchmaker-do.js)
//   GET  /room/<id>          WebSocket: join room <id> (web/net.js frames)
//   POST /match/<id>/start   start the match server for room <id>
//   GET  /match/<id>         the server's status (map loaded?, recent log)
//   POST /match/<id>/stop    stop it
//
// Starting and stopping need the START_TOKEN secret (Authorization: Bearer).
import { Container } from '@cloudflare/containers';
import { Room } from '../../hub/src/index.js';
import { Matchmaker } from './matchmaker-do.js';

export { Room, Matchmaker };

export class MatchServer extends Container {
  defaultPort = 8080; // run-server.mjs status page
  sleepAfter = '20m'; // no status requests for this long: stop
}

const json = (v, status = 200) => new Response(JSON.stringify(v, null, 2), { status, headers: { 'content-type': 'application/json' } });

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === '/play') {
      if (request.headers.get('Upgrade') !== 'websocket') return new Response('websocket expected', { status: 426 });
      return env.MATCHMAKER.getByName(env.REGION || 'WEUR').fetch(request);
    }
    const room = /^\/room\/([\w-]{1,64})$/.exec(url.pathname);
    if (room) {
      if (request.headers.get('Upgrade') !== 'websocket') return new Response('websocket expected', { status: 426 });
      return env.ROOMS.get(env.ROOMS.idFromName(room[1])).fetch(request);
    }
    const m = /^\/match\/([\w-]{1,64})(\/start|\/stop)?$/.exec(url.pathname);
    if (!m) return new Response('not found', { status: 404 });
    const [, id, action] = m;
    const server = env.MATCH_SERVER.getByName(id);
    if (action) {
      if (request.method !== 'POST') return new Response('POST expected', { status: 405 });
      if (!env.START_TOKEN || request.headers.get('Authorization') !== `Bearer ${env.START_TOKEN}`) return new Response('forbidden', { status: 403 });
      if (action === '/stop') {
        await server.stop();
        return json({ stopped: id });
      }
      const t0 = Date.now();
      await server.start({ envVars: { REBEL_NET: `wss://${url.host}/room/${id}` } }, { portReadyTimeoutMS: 120000 });
      return json({ started: id, ms: Date.now() - t0 });
    }
    // Status: the server's own page, if it is running.
    const state = await server.getState();
    if (state.status !== 'healthy' && state.status !== 'running') return json({ id, state });
    const res = await server.fetch(new Request('http://container/status'));
    return json({ id, state, server: await res.json().catch(() => null) });
  },
};
