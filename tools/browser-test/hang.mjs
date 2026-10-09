// Hang catcher for the M13 intro: records V8 CPU samples through Chrome
// tracing from the campaign start; if the page stops logging for 20 s, ends
// the trace (works while the main thread is stuck) and prints the hottest
// recent stacks. TRIES runs, default 3.
import { chromium } from 'playwright';
const tries = Number(process.env.TRIES || 3);
for (let attempt = 1; attempt <= tries; attempt++) {
  const ctx = await chromium.launchPersistentContext(new URL('./profile/', import.meta.url).pathname, {
    channel: 'chromium', headless: true, viewport: { width: 1280, height: 800 },
    args: ['--use-angle=metal', '--enable-gpu', '--ignore-gpu-blocklist', '--autoplay-policy=no-user-gesture-required'],
  });
  const page = ctx.pages()[0] || (await ctx.newPage());
  let menuAt = 0, last = Date.now();
  const t0 = Date.now();
  page.on('console', (m) => { last = Date.now(); const t = m.text(); if (/MainMenuDialogClass: started/.test(t)) menuAt = Date.now(); });
  const cdp = await ctx.newCDPSession(page);
  await page.goto((process.env.BASE || 'http://127.0.0.1:8461/') + '?nolock&quiet');
  await page.waitForSelector('#start:not([hidden])', { timeout: 900000 });
  // V8 CPU samples via tracing: collected outside the renderer main thread,
  // so they can be read while the page is stuck in a loop.
  const chunks = [];
  cdp.on('Tracing.dataCollected', (e) => chunks.push(...e.value));
  await cdp.send('Tracing.start', { transferMode: 'ReportEvents', traceConfig: { recordMode: 'recordContinuously', includedCategories: ['disabled-by-default-v8.cpu_profiler', 'v8'] } });
  await page.evaluate(() => setTimeout(() => document.getElementById('start').click(), 0));
  const skipper = setInterval(() => page.evaluate(() => { if (window.rebelMovieActive) Module.rebelStopMovie(); }).catch(() => {}), 500);
  while (!menuAt) await new Promise((r) => setTimeout(r, 200));
  await page.waitForTimeout(2000);
  await page.evaluate(() => setTimeout(() => Module._Rebel_Debug_Start_Campaign(0), 0));
  let hung = false;
  const deadline = Date.now() + 150000;
  while (Date.now() < deadline) {
    await new Promise((r) => setTimeout(r, 2000));
    if (Date.now() - last > 20000) { hung = true; break; }
  }
  clearInterval(skipper);
  if (!hung) { console.log(`attempt ${attempt}: no hang in 150 s`); await ctx.close(); continue; }
  console.log(`attempt ${attempt}: HANG at ${((Date.now() - t0) / 1000).toFixed(0)} s, collecting trace`);
  const done = new Promise((r) => cdp.once('Tracing.tracingComplete', r));
  await cdp.send('Tracing.end');
  await done;
  (await import('node:fs')).writeFileSync(new URL('./out/hang-trace.json', import.meta.url).pathname, JSON.stringify({ traceEvents: chunks }));
  // Rebuild the sampled stacks from ProfileChunk events (main thread profile).
  const nodes = new Map(), parent = new Map(), samples = [];
  for (const e of chunks) {
    if (e.name !== 'ProfileChunk' || !e.args || !e.args.data) continue;
    const d = e.args.data;
    const cp = d.cpuProfile || {};
    for (const n of cp.nodes || []) { nodes.set(`${e.id}:${n.id}`, n.callFrame.functionName || '(anon)'); if (n.parent) parent.set(`${e.id}:${n.id}`, `${e.id}:${n.parent}`); }
    let t = 0;
    (cp.samples || []).forEach((s, i) => { t += (d.timeDeltas || [])[i] || 0; samples.push(`${e.id}:${s}`); });
  }
  const recent = samples.slice(-2000);
  const agg = new Map();
  for (const s of recent) {
    const names = [];
    for (let cur = s; cur && names.length < 30; cur = parent.get(cur)) names.push(nodes.get(cur));
    const k = names.join('\n    <- ');
    agg.set(k, (agg.get(k) || 0) + 1);
  }
  console.log(`${samples.length} samples, last ${recent.length}:`);
  for (const [k, c] of [...agg.entries()].sort((a, b) => b[1] - a[1]).slice(0, 4)) console.log(`x${c}:\n    ${k}\n`);
  process.exit(2);
}
