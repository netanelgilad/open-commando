// Timer latency probe: at the main menu, measures how late setTimeout(10)
// and a MessageChannel round trip fire while the game runs, and the game's
// own frame stats, with and without a CPU profiler attached.
import { chromium } from 'playwright';
const ctx = await chromium.launchPersistentContext(new URL('./profile/', import.meta.url).pathname, {
  channel: 'chromium', headless: !process.env.HEADED, viewport: { width: 1280, height: 800 },
  args: ['--use-angle=metal', '--enable-gpu', '--ignore-gpu-blocklist', '--autoplay-policy=no-user-gesture-required'],
});
const page = ctx.pages()[0] || (await ctx.newPage());
let menuAt = 0;
page.on('console', (m) => {
  const t = m.text();
  if (/MainMenuDialogClass: started/.test(t)) menuAt = Date.now();
  if (/\[d3d9\] frame/.test(t)) console.log(t.slice(0, 190));
});
if (process.env.COUNT) await page.addInitScript(() => {
  const counts = window.__glCounts = {};
  for (const C of [WebGL2RenderingContext]) for (const k of Object.getOwnPropertyNames(C.prototype)) {
    const d = Object.getOwnPropertyDescriptor(C.prototype, k);
    if (typeof d.value !== 'function') continue;
    const f = d.value;
    C.prototype[k] = function (...a) { counts[k] = (counts[k] || 0) + 1; return f.apply(this, a); };
  }
});
await page.goto((process.env.BASE || 'http://127.0.0.1:8461/') + '?nolock&quiet');
await page.waitForSelector('#start:not([hidden])', { timeout: 900000 });
await page.evaluate(() => setTimeout(() => document.getElementById('start').click(), 0));
const skipper = setInterval(() => page.evaluate(() => { if (document.getElementById('movie').style.display === 'block') Module.rebelStopMovie(); }).catch(() => {}), 500);
while (!menuAt) await new Promise((r) => setTimeout(r, 500));
await page.waitForTimeout(8000);
const probe = () => page.evaluate(async () => {
  const late = [], mc = [];
  for (let i = 0; i < 20; i++) {
    const t = performance.now();
    await new Promise((r) => setTimeout(r, 10));
    late.push(performance.now() - t - 10);
    const t2 = performance.now();
    await new Promise((r) => { const c = new MessageChannel(); c.port1.onmessage = r; c.port2.postMessage(0); });
    mc.push(performance.now() - t2);
  }
  const avg = (a) => (a.reduce((x, y) => x + y, 0) / a.length).toFixed(1);
  return `setTimeout(10) late by ${avg(late)} ms avg (max ${Math.max(...late).toFixed(0)}), MessageChannel ${avg(mc)} ms avg (max ${Math.max(...mc).toFixed(0)})`;
});
if (process.env.COUNT) {
  const f0 = await page.evaluate(() => { for (const k in window.__glCounts) window.__glCounts[k] = 0; return window.__frames = 0; });
  await page.evaluate(() => { const o = Module.rebelFrame; Module.rebelFrame = () => { window.__frames++; o(); }; });
  await page.waitForTimeout(3000);
  const r = await page.evaluate(() => ({ frames: window.__frames, counts: window.__glCounts }));
  const tot = Object.values(r.counts).reduce((a, b) => a + b, 0);
  console.log(`frames ${r.frames}, GL calls per frame ${(tot / r.frames).toFixed(0)}`);
  for (const [k, v] of Object.entries(r.counts).sort((a, b) => b[1] - a[1]).slice(0, 25)) console.log(`  ${(v / r.frames).toFixed(1).padStart(8)}  ${k}`);
  await ctx.close(); process.exit(0);
}
console.log('no profiler:', await probe());
await page.waitForTimeout(6000);
const cdp = await ctx.newCDPSession(page);
if (process.env.TRACE) {
  const chunks = [];
  cdp.on('Tracing.dataCollected', (e) => chunks.push(...e.value));
  const done = new Promise((r) => cdp.once('Tracing.tracingComplete', r));
  await cdp.send('Tracing.start', { transferMode: 'ReportEvents', traceConfig: { includedCategories: ['toplevel', 'gpu', 'viz', 'cc', 'blink', 'v8', 'disabled-by-default-gpu.service', 'disabled-by-default-gpu.device', 'disabled-by-default-devtools.timeline'] } });
  await page.waitForTimeout(5000);
  await cdp.send('Tracing.end');
  await done;
  (await import('node:fs')).writeFileSync(new URL('./out/menu-trace.json', import.meta.url).pathname, JSON.stringify({ traceEvents: chunks }));
  console.log('trace events', chunks.length);
  await ctx.close();
  process.exit(0);
}
await cdp.send('Profiler.enable');
await cdp.send('Profiler.setSamplingInterval', { interval: 200 });
await cdp.send('Profiler.start');
console.log('profiler on:', await probe());
await page.waitForTimeout(6000);
const { profile } = await cdp.send('Profiler.stop');
(await import('node:fs')).writeFileSync(new URL('./out/menu.cpuprofile', import.meta.url).pathname, JSON.stringify(profile));
console.log('profiler off:', await probe());
await page.waitForTimeout(6000);
clearInterval(skipper);
await ctx.close();
