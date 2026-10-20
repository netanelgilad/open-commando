// Two players over the virtual network (web/net.js): page A hosts a LAN C&C
// game from the menus, page B finds it in the LAN game list and joins.
// Each player is its own browser profile (two tabs of one profile can't both
// run the game: they share the OPFS game files, see WEB_PORT_GAPS.md), so
// they meet through the hub: NET=ws://127.0.0.1:8787/room/<name> (default;
// services/hub under `wrangler dev`). Reports what each side logs about the
// network, and screenshots both once the client is in the game.
//
//   BASE=http://127.0.0.1:8461/dbg/ node lan.mjs
//   NETLAG=100 NETLOSS=0.05 ...       simulate a worse connection
//   NETRTC=1 ...                      clients reach the server over a WebRTC data channel
//   JOIN=connect ...                  clients join with +connect 10.0.0.1:4848 (the way
//                                     matchmaking will) instead of the LAN game list
//   ONLINE=<match service URL>        no host: clients press "Play online", the
//                                     matchmaker starts a server and sends them in
//   SERVER=node ...                   the host is the headless dedicated server
//                                     (tools/server/run-server.mjs) instead of a page;
//                                     SERVER_BIN=<dir> picks its build
import { chromium } from 'playwright';
import { mkdirSync, writeFileSync } from 'node:fs';
import { execFileSync, spawn } from 'node:child_process';
const outDir = new URL('./out/', import.meta.url).pathname;
mkdirSync(outDir, { recursive: true });
const base = process.env.BASE || 'http://127.0.0.1:8461/';
const net = process.env.NET || 'ws://127.0.0.1:8787/room/lan-test-' + Date.now();
const extra = (process.env.NETRTC ? '&netrtc' : '') + ['netlag', 'netjitter', 'netloss'].filter((k) => process.env[k.toUpperCase()]).map((k) => `&${k}=${process.env[k.toUpperCase()]}`).join('');
// The client profile gets the game files from the local server on first use.
const contexts = [];
const browserFor = async (profile) => {
  // PROFILES=<dir>: where the extra players' browser profiles live (each
  // imports its own ~2 GB copy of the game); default next to this script.
  const dir = profile !== 'profile' && process.env.PROFILES ? `${process.env.PROFILES}/${profile}/` : new URL(`./${profile}/`, import.meta.url).pathname;
  const ctx = await chromium.launchPersistentContext(dir, {
    channel: 'chromium', headless: true, viewport: { width: 1024, height: 768 },
    args: ['--use-angle=metal', '--enable-gpu', '--ignore-gpu-blocklist', '--autoplay-policy=no-user-gesture-required',
      '--disable-background-timer-throttling', '--disable-renderer-backgrounding', '--disable-backgrounding-occluded-windows'],
  });
  contexts.push(ctx);
  return ctx;
};
const t0 = Date.now();
const stamp = () => ((Date.now() - t0) / 1000).toFixed(1);
const NETWORK = /\[bots\]|suicide|Lobby|Match starting|\[net\]|\[debug\]|abort|Assert|RuntimeError|PAGEERROR|cNetwork|cConnection|[Cc]onnect|[Ss]erver|[Cc]lient|LAN|refus|Accept|Bound|socket|WSA/;

async function player(name, profile) {
  const ctx = await browserFor(profile);
  const page = ctx.pages()[0] || (await ctx.newPage());
  const p = { name, nick: { host: 'Alpha', client: 'Bravo', client2: 'Charlie', client3: 'Delta' }[name], page, lines: [], menuAt: 0, listing: null, star: null };
  page.on('console', (m) => {
    const t = m.text();
    p.lines.push(`${stamp()} ${t}`);
    if (/MainMenuDialogClass: started/.test(t)) p.menuAt = Date.now();
    const head = /\[debug\] controls of dialog (\d+)/.exec(t);
    if (head) p.listing = { id: +head[1], controls: [] };
    const c = /\[debug\]   ctl (-?\d+) (\S+) at (\S+) (disabled )?(hidden )?'(.*)'/.exec(t);
    if (c && p.listing) p.listing.controls.push({ id: +c[1], type: c[2], disabled: !!c[4], hidden: !!c[5], text: c[6] });
    const s = /\[debug\] star at (.*)/.exec(t);
    if (s) p.star = s[1];
    if (/RuntimeError|abort/.test(t)) { p.crashed = true; console.log(`${stamp()} ${name}: ${t}`); }
    else if (NETWORK.test(t) && !/\[debug\]   ctl/.test(t)) console.log(`${stamp()} ${name}: ${t}`.slice(0, 260));
  });
  page.on("pageerror", (e) => { p.crashed = true; console.log(`${stamp()} ${name}: PAGEERROR ${e.message}\n${e.stack}`); });
  // CLIENT_BASE: the first client from another build (e.g. /dbg/ for its logs).
  const from = name === 'client' && process.env.CLIENT_BASE ? process.env.CLIENT_BASE : base;
  const args = name !== 'host' && process.env.JOIN === 'connect' ? `&args=${encodeURIComponent(`+connect 10.0.0.1:4848 +netplayername ${p.nick}`)}` : '';
  const where = process.env.ONLINE ? `&online=${encodeURIComponent(process.env.ONLINE)}` : `&net=${encodeURIComponent(net)}`;
  await page.goto(`${from}?nolock${from === base ? '&quiet' : ''}&noguard&name=${p.nick}${where}${extra}${args}`);
  await page.waitForSelector('#start:not([hidden])', { timeout: 900000 });
  // HOST_PLAYS=1: the page host also plays (the install's default host
  // settings, Data/svrcfg_cnc.ini, say IsDedicated=yes). Edits this
  // profile's own copy of the file.
  if (name === 'host' && process.env.HOST_PLAYS) {
    console.log(await page.evaluate(async () => {
      const root = await navigator.storage.getDirectory();
      const data = await (await root.getDirectoryHandle('renegade')).getDirectoryHandle('Data');
      const fh = await data.getFileHandle('svrcfg_cnc.ini');
      const text = await (await fh.getFile()).text();
      const w = await fh.createWritable();
      await w.write(text.replace(/IsDedicated=yes/i, 'IsDedicated=no'));
      await w.close();
      return 'host: IsDedicated=no';
    }));
  }
  await page.evaluate((online) => setTimeout(() => document.getElementById(online ? 'online' : 'start').click(), 0), !!process.env.ONLINE);
  p.skipper = setInterval(() => page.evaluate(() => { if (window.rebelMovieActive) Module.rebelStopMovie(); }).catch(() => {}), 500);
  while (!p.menuAt) {
    if (p.crashed) throw new Error(name + ' crashed while starting');
    await new Promise((r) => setTimeout(r, 500));
  }
  await page.waitForTimeout(3000);
  return p;
}

async function list(p) {
  for (let i = 0; i < 20; i++) {
    p.listing = null;
    await p.page.evaluate(() => setTimeout(() => Module._Rebel_Debug_List_Controls(), 0));
    for (let k = 0; k < 12 && !p.listing; k++) await p.page.waitForTimeout(250);
    await p.page.waitForTimeout(400);
    if (p.listing) return p.listing;
    await p.page.waitForTimeout(1000);
  }
  throw new Error(p.name + ': no active dialog');
}
const show = (p, l) => console.log(`${stamp()} ${p.name}: dialog ${l.id}: ` + l.controls.filter((c) => !c.hidden && c.text).map((c) => `${c.id}${c.disabled ? '(x)' : ''}='${c.text}'`).join(' '));
const press = (p, id) => p.page.evaluate((i) => setTimeout(() => Module._Rebel_Debug_Command(i), 0), id);
// Presses the enabled control whose text matches re (in the active dialog).
async function choose(p, re) {
  const l = await list(p);
  show(p, l);
  const c = l.controls.find((x) => !x.disabled && !x.hidden && re.test(x.text));
  if (!c) throw new Error(`${p.name}: nothing matches ${re} in dialog ${l.id}`);
  console.log(`${stamp()} ${p.name}: -> ${c.text}`);
  await press(p, c.id);
  await p.page.waitForTimeout(2500);
  return c;
}
// Spawns face a purchase terminal: turn around and step out before a
// screenshot, so it shows the map rather than the terminal's screen.
async function lookAround(p) {
  if (!p.page.keyboard) return;
  await p.page.mouse.click(512, 384);
  await p.page.keyboard.down('ArrowLeft');
  await p.page.waitForTimeout(1300);
  await p.page.keyboard.up('ArrowLeft');
  await p.page.keyboard.down('KeyW');
  await p.page.waitForTimeout(1500);
  await p.page.keyboard.up('KeyW');
  await p.page.waitForTimeout(500);
}
const shot = (p, n) => p.page.screenshot({ path: `${outDir}/lan-${p.name}-${n}.png`, timeout: 60000 }).catch(() => console.log('screenshot timed out'));
const dump = (p) => p.page.evaluate(() => setTimeout(() => Module._Rebel_Debug_Input(), 0));
const save = () => {
  for (const p of players) writeFileSync(`${outDir}/lan-${p.name}-console.log`, p.lines.join('\n'));
};
// The headless server as the host: same log/screenshot interface as a page.
async function nodeServer() {
  const p = { name: 'host', lines: [], star: null };
  const args = [new URL('../server/run-server.mjs', import.meta.url).pathname, '--net', net];
  if (process.env.SERVER_BIN) args.push('--bin', process.env.SERVER_BIN); // e.g. build/wasm/bin for the debug build's logs
  const proc = spawn(process.execPath, args, { stdio: ['pipe', 'pipe', 'pipe'] });
  let ready = false;
  const onData = (d) => {
    for (const t of d.toString().split('\n').filter(Boolean)) {
      p.lines.push(`${stamp()} ${t}`);
      if (/Gameplay Pending|Gameplay in progress/i.test(t)) ready = true;
      if (/\[net\]|\[perf\]|\[brain\]|\[nav\]|Loading level|abort|Assert|[Cc]lient|[Pp]layer|GDI :|NOD :|Gameplay/.test(t)) console.log(`${stamp()} server: ${t}`.slice(0, 260));
    }
  };
  proc.stdout.on('data', onData);
  proc.stderr.on('data', onData);
  p.proc = proc;
  p.page = { screenshot: async () => {} };
  for (let i = 0; i < 300 && !ready; i++) await new Promise((r) => setTimeout(r, 1000));
  if (!ready) throw new Error('server did not start: ' + p.lines.slice(-3).join(' | '));
  return p;
}
const players = [];
// Joins the first game in the LAN list.
async function join(c) {
  for (const step of (process.env.JOIN_STEPS || 'multiplay lan').split(';')) await choose(c, new RegExp(step, 'i'));
  // Wait for the host's game to appear in the list (it broadcasts every second).
  await c.page.waitForTimeout(5000);
  show(c, await list(c));
  await shot(c, 'list');
  // Select the first game (a click on its row, held long enough for slow
  // frames), then Join.
  const [x, y] = (process.env.JOIN_CLICK || '400,199').split(',').map(Number);
  await c.page.mouse.move(x, y);
  await c.page.mouse.down();
  await c.page.waitForTimeout(300);
  await c.page.mouse.up();
  await c.page.waitForTimeout(1500);
  const l = await list(c).catch(() => null); // a click can already join (double click)
  if (l && l.id === 174) await choose(c, /^join game/i);
}
const netStats = async (p) => (p.proc ? null : p.page.evaluate(() => JSON.stringify(Module.rebelNet.stats)));
try {
  let host;
  if (process.env.ONLINE) {
    host = { name: 'matchmaker', lines: [], page: { screenshot: async () => {} } };
  } else if (process.env.SERVER === 'external') {
    // A server already running on NET (e.g. the match image under docker).
    host = { name: 'external', lines: [], page: { screenshot: async () => {} } };
  } else if (process.env.SERVER === 'node') {
    host = await nodeServer();
    players.push(host);
  } else {
    host = await player('host', 'profile');
    players.push(host);
    // Main menu -> Multiplay LAN -> Host Game -> Start Game (default options).
    for (const step of (process.env.HOST_STEPS || 'multiplay lan;^host game').split(';')) await choose(host, new RegExp(step, 'i'));
    await shot(host, 'options');
    await choose(host, new RegExp(process.env.HOST_START || '^start|^ok|launch', 'i'));
    await host.page.waitForTimeout(15000);
    await shot(host, 'hosting');
    // HOST_CMDS="bot_add nod 2;bot_list": console commands on the host page,
    // then HOST_WATCH=<command> every 10 s for a minute.
    const hostCmd = (c) => host.page.evaluate((c) => setTimeout(() => Module.ccall('Rebel_Console_Command', null, ['string'], [c]), 0), c);
    if (process.env.HOST_CMDS) for (const c of process.env.HOST_CMDS.split(';')) await hostCmd(c);
    if (process.env.HOST_WATCH) for (let i = 0; i < 6; i++) { await host.page.waitForTimeout(10000); await hostCmd(process.env.HOST_WATCH); }
  }
  // C&C gameplay starts once both teams have a player: a dedicated server
  // needs two clients.
  const count = +(process.env.CLIENTS || (host.proc || process.env.ONLINE ? 2 : 1));
  const clients = [];
  const names = (i) => [i ? `client${i + 1}` : 'client', ['profile-b', 'profile-c', 'profile-d'][i]];
  if (process.env.ONLINE) {
    // Everyone queues at once (the matchmaker needs them together).
    clients.push(...(await Promise.all([...Array(count).keys()].map((i) => player(...names(i))))));
    players.push(...clients);
  } else {
    for (let i = 0; i < count; i++) {
      const c = await player(...names(i));
      players.push(c);
      clients.push(c);
      if (process.env.JOIN !== 'connect') await join(c);
    }
  }
  // SERVER_CMDS="a;b": console commands for the Node server once everyone joined.
  if (host.proc && process.env.SERVER_CMDS) host.proc.stdin.write(process.env.SERVER_CMDS.split(';').join('\n') + '\n');
  for (let i = 0; i < 36 && clients.some((c) => !c.star); i++) {
    await clients[0].page.waitForTimeout(5000);
    for (const c of clients) await dump(c);
    if (host.proc && i % 4 === 0) host.proc.stdin.write('game_info\nplayer_info\n');
  }
  // CLIENT_CMDS="a;b": console commands on the clients once they're in.
  if (process.env.CLIENT_CMDS) for (const c of clients) for (const cmd of process.env.CLIENT_CMDS.split(';')) await c.page.evaluate((x) => setTimeout(() => Module.ccall('Rebel_Console_Command', null, ['string'], [x]), 0), cmd);
  // The ping the server measures for each client (ms), and how far off other
  // soldiers' positions were at each server update, a few samples.
  for (let i = 0; i < (+process.env.NET_SAMPLES || 4) && clients.some((c) => c.star); i++) {
    await clients[0].page.waitForTimeout(5000);
    for (const c of clients) {
      // A page that stops answering (hung, crashed) is a finding: say so and
      // take a picture, instead of waiting on it.
      const answer = c.page.evaluate(() => [Module._Rebel_My_Ping ? Module._Rebel_My_Ping() : -1, Module._Rebel_Net_Corrections ? Module.UTF8ToString(Module._Rebel_Net_Corrections()) : '']);
      const r = await Promise.race([answer, new Promise((res) => setTimeout(() => res(null), 15000))]);
      if (!r) {
        console.log(`${stamp()} ${c.name} PAGE UNRESPONSIVE (no answer in 15 s)`);
        // Where it's stuck: pause it through the DevTools protocol and print
        // the call stack (JS and wasm frames), once.
        if (!c.stackDumped) {
          c.stackDumped = true;
          const dump = async () => {
            const cdp = await c.page.context().newCDPSession(c.page);
            const paused = new Promise((res) => cdp.once('Debugger.paused', res));
            await cdp.send('Debugger.enable');
            await cdp.send('Debugger.pause');
            const ev = await Promise.race([paused, new Promise((res) => setTimeout(() => res(null), 10000))]);
            if (ev) for (const f of ev.callFrames.slice(0, 40)) console.log(`${stamp()}   at ${f.functionName || '(anonymous)'} ${f.url.split('/').pop()}:${f.location.lineNumber}:${f.location.columnNumber}`);
            else console.log(`${stamp()}   (could not pause it)`);
            await cdp.send('Debugger.resume').catch(() => {});
          };
          // A page frozen hard enough doesn't answer DevTools either: give up after 20 s.
          await Promise.race([dump().catch((e) => console.log(`${stamp()}   stack: ${e.message}`)), new Promise((res) => setTimeout(() => res(console.log(`${stamp()}   (DevTools did not answer)`)), 20000))]);
        }
        await shot(c, 'unresponsive');
        save();
        continue;
      }
      console.log(`${stamp()} ${c.name} ping ${r[0]} ms corrections ${r[1]}`);
    }
  }
  // Let textures and the spawn settle, then walk forward a little and check
  // the soldier moved.
  if (clients.every((c) => c.star)) await clients[0].page.waitForTimeout(20000);
  for (const c of clients.filter((x) => x.star)) {
    const before = c.star;
    await c.page.mouse.click(512, 384);
    await c.page.keyboard.down('KeyW');
    await c.page.waitForTimeout(3000);
    await c.page.keyboard.up('KeyW');
    await dump(c);
    await c.page.waitForTimeout(1500);
    console.log(`${stamp()} ${c.name} star: ${before} -> ${c.star}`);
  }
  for (const p of players) await shot(p, 'joined');
  // ONLINE: they're straight in a real match with bots: wait a moment, check
  // a soldier that dies respawns, and look at the game.
  if (process.env.ONLINE) {
    const status = async () => (await fetch(process.env.ONLINE.replace(/\/$/, '') + '/status')).json();
    console.log(`${stamp()} service: ${JSON.stringify((await status()).matches)}`);
    await clients[0].page.waitForTimeout(16000);
    const c = clients[0];
    // Walk away from the spawn point first, so a respawn is visible.
    await c.page.mouse.click(512, 384);
    await c.page.keyboard.down('KeyW');
    await c.page.waitForTimeout(4000);
    await c.page.keyboard.up('KeyW');
    await dump(c);
    await c.page.waitForTimeout(1500);
    const before = c.star;
    await c.page.evaluate(() => setTimeout(() => Module.ccall('Rebel_Console_Command', null, ['string'], ['suicide']), 0));
    await c.page.waitForTimeout(3000);
    c.star = null;
    for (let i = 0; i < 12 && !c.star; i++) { await c.page.waitForTimeout(2500); await dump(c); }
    console.log(`${stamp()} ${c.name} in the match: suicide at ${before} -> respawned at ${c.star}`);
    await c.page.waitForTimeout(20000);
    for (const x of clients) await lookAround(x);
    for (const p of players) await shot(p, 'match');
    console.log(`${stamp()} service: ${JSON.stringify((await status()).matches)}`);
  }
  for (const p of players) console.log(`${stamp()} ${p.name} star: ${p.star} net ${await netStats(p)}`);
  if (host.proc) console.log(`${stamp()} server process: ${execFileSync('ps', ['-o', 'time=,rss=', '-p', String(host.proc.pid)]).toString().trim()} (cpu time, rss KB)`);
  if (clients.some((c) => !c.star)) process.exitCode = 1;
} catch (e) {
  console.log('FAILED: ' + e.message);
  for (const p of players) await shot(p, 'failed');
  process.exitCode = 1;
}
save();
for (const p of players) if (p.proc) p.proc.kill();
// Closing a browser can hang (pages mid-game); don't let it hold the run.
await Promise.race([Promise.all(contexts.map((c) => c.close())), new Promise((r) => setTimeout(r, 15000))]);
process.exit(process.exitCode || 0);
