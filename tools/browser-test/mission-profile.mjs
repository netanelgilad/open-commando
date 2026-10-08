// Starts the campaign, waits until the intro cinematic hands control to the
// player, then records a CPU profile and prints self and inclusive time.
import { chromium } from 'playwright';
import { writeFileSync, mkdirSync } from 'node:fs';
const outDir = new URL('./out/', import.meta.url).pathname;
mkdirSync(outDir, { recursive: true });
const secs = Number(process.argv[2] || 15);
const ctx = await chromium.launchPersistentContext(new URL('./profile/', import.meta.url).pathname, {
  channel: 'chromium', headless: true, viewport: { width: 1280, height: 800 },
  args: ['--use-angle=metal', '--enable-gpu', '--ignore-gpu-blocklist', '--autoplay-policy=no-user-gesture-required'],
});
const page = ctx.pages()[0] || (await ctx.newPage());
let menuAt = 0, playing = false;
const t0 = Date.now();
page.on('console', (m) => {
  const t = m.text();
  if (/MainMenuDialogClass: started/.test(t)) menuAt = Date.now();
  if (/cinematic=0 .*sync=([1-9]\d{4,})/.test(t)) playing = true;
  if (/\[d3d9\] frame|fps/i.test(t)) console.log(`${((Date.now() - t0) / 1000).toFixed(0)} ${t}`.slice(0, 200));
});
await page.goto('http://127.0.0.1:8461/?nolock');
await page.waitForSelector('#start:not([hidden])', { timeout: 900000 });
await page.evaluate(() => setTimeout(() => document.getElementById('start').click(), 0));
const skipper = setInterval(() => page.evaluate(() => { if (document.getElementById('movie').style.display === 'block') Module.rebelStopMovie(); }).catch(() => {}), 500);
while (!menuAt) await new Promise((r) => setTimeout(r, 500));
await page.waitForTimeout(5000);
await page.evaluate(() => setTimeout(() => Module._Rebel_Debug_Start_Campaign(0), 0));
for (let i = 0; i < 80 && !playing; i++) { await page.waitForTimeout(5000); await page.evaluate(() => setTimeout(() => Module._Rebel_Debug_Input(), 0)); }
clearInterval(skipper);
console.log('gameplay reached');
if (process.env.NOFILTER_TEST) { await page.waitForTimeout(12000); console.log('--- disabling canvas filter'); await page.addStyleTag({ content: 'canvas { filter: none !important; }' }); await page.waitForTimeout(12000); }
console.log('profiling', secs, 's');
const cdp = await ctx.newCDPSession(page);
if (process.env.TRACE) {
  // Chrome trace (all processes, including the GPU process) for chrome://tracing / analyze-trace.mjs.
  const chunks = [];
  cdp.on('Tracing.dataCollected', (e) => chunks.push(...e.value));
  const done = new Promise((r) => cdp.once('Tracing.tracingComplete', r));
  await cdp.send('Tracing.start', { transferMode: 'ReportEvents', traceConfig: { includedCategories: ['toplevel', 'gpu', 'viz', 'cc', 'blink', 'v8', 'disabled-by-default-gpu.service', 'disabled-by-default-gpu.device', 'disabled-by-default-devtools.timeline'] } });
  await page.waitForTimeout(secs * 1000);
  await cdp.send('Tracing.end');
  await done;
  writeFileSync(`${outDir}/mission-trace.json`, JSON.stringify({ traceEvents: chunks }));
  console.log('trace events', chunks.length);
  await ctx.close();
  process.exit(0);
}
await cdp.send('Profiler.enable');
await cdp.send('Profiler.setSamplingInterval', { interval: 500 });
await cdp.send('Profiler.start');
await page.waitForTimeout(secs * 1000);
const { profile } = await cdp.send('Profiler.stop');
writeFileSync(`${outDir}/mission.cpuprofile`, JSON.stringify(profile));
await page.screenshot({ path: `${outDir}/mission-profile.png` });
await ctx.close();
