// Profiles the game's pthreads (dedicated workers) during a level load, via
// a raw DevTools connection: Playwright's CDP sessions can't reach workers.
// Prints the hottest functions per busy worker.
import { chromium } from 'playwright';
import { writeFileSync } from 'node:fs';
const secs = Number(process.env.SECS || 20);
const port = 9333;
const ctx = await chromium.launchPersistentContext(new URL('./profile/', import.meta.url).pathname, {
  channel: 'chromium', headless: true, viewport: { width: 1280, height: 800 },
  args: ['--use-angle=metal', '--enable-gpu', '--ignore-gpu-blocklist', '--autoplay-policy=no-user-gesture-required', `--remote-debugging-port=${port}`],
});
const page = ctx.pages()[0] || (await ctx.newPage());
let menuAt = 0, preload = false;
const t0 = Date.now();
page.on('console', (m) => {
  const t = m.text();
  if (/MainMenuDialogClass: started/.test(t)) menuAt = Date.now();
  if (/^\[(load|debug)\]/.test(t)) console.log(((Date.now() - t0) / 1000).toFixed(1), t.slice(0, 120));
});
await page.goto((process.env.BASE || 'http://127.0.0.1:8461/') + '?nolock&quiet');
await page.waitForSelector('#start:not([hidden])', { timeout: 900000 });
await page.evaluate(() => setTimeout(() => document.getElementById('start').click(), 0));
const skipper = setInterval(() => page.evaluate(() => { if (document.getElementById('movie').style.display === 'block') Module.rebelStopMovie(); }).catch(() => {}), 500);
while (!menuAt) await new Promise((r) => setTimeout(r, 200));
await page.waitForTimeout(3000);

// Minimal CDP client on the browser endpoint (flattened sessions).
const { webSocketDebuggerUrl } = await (await fetch(`http://127.0.0.1:${port}/json/version`)).json();
const ws = new WebSocket(webSocketDebuggerUrl);
await new Promise((r) => (ws.onopen = r));
let id = 0;
const pending = new Map(), attached = [];
ws.onmessage = (ev) => {
  const m = JSON.parse(ev.data);
  if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); }
  if (m.method === 'Target.attachedToTarget') attached.push(m.params);
};
const send = (method, params = {}, sessionId) => new Promise((r) => { const i = ++id; pending.set(i, r); ws.send(JSON.stringify({ id: i, method, params, ...(sessionId ? { sessionId } : {}) })); });
const { result } = await send('Target.getTargets');
const workers = result.targetInfos.filter((t) => t.type === 'worker');
console.log(`workers: ${workers.length}`);
const sessions = [];
for (const w of workers) {
  const r = await send('Target.attachToTarget', { targetId: w.targetId, flatten: true });
  const sid = r.result.sessionId;
  await send('Profiler.enable', {}, sid);
  await send('Profiler.setSamplingInterval', { interval: 1000 }, sid);
  await send('Profiler.start', {}, sid);
  sessions.push({ sid, w });
}
await page.evaluate(() => setTimeout(() => Module._Rebel_Debug_Start_Campaign(0), 0));
await page.waitForTimeout(secs * 1000);
let n = 0;
for (const { sid, w } of sessions) {
  const r = await send('Profiler.stop', {}, sid);
  const profile = r.result && r.result.profile;
  if (!profile) continue;
  const total = profile.samples.length;
  const self = new Map();
  const byId = new Map(profile.nodes.map((x) => [x.id, x]));
  for (const s of profile.samples) { const f = byId.get(s).callFrame.functionName || '(anon)'; self.set(f, (self.get(f) || 0) + 1); }
  const idle = (self.get('(idle)') || 0) + (self.get('(program)') || 0);
  if (total - idle < total * 0.05) continue;
  writeFileSync(new URL(`./out/worker-${n}.cpuprofile`, import.meta.url).pathname, JSON.stringify(profile));
  console.log(`--- worker ${n++} ${w.url.split('/').pop()}: ${total} samples, busy ${(100 * (total - idle) / total).toFixed(0)}%`);
  for (const [f, c] of [...self.entries()].sort((a, b) => b[1] - a[1]).slice(0, 25)) console.log(`  ${(100 * c / total).toFixed(1).padStart(5)}%  ${f.slice(0, 110)}`);
}
clearInterval(skipper);
ws.close();
await ctx.close();
