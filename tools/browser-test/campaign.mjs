// Campaign smoke test: plays through every mission by winning each one
// (debug hook) once it is playable, through the score screen to the next.
// Reports the level, frame stats and any crash per mission.
import { chromium } from 'playwright';
import { mkdirSync, writeFileSync } from 'node:fs';
const outDir = new URL('./out/', import.meta.url).pathname;
mkdirSync(outDir, { recursive: true });
const ctx = await chromium.launchPersistentContext(new URL('./profile/', import.meta.url).pathname, {
  channel: 'chromium', headless: true, viewport: { width: 1280, height: 800 },
  args: ['--use-angle=metal', '--enable-gpu', '--ignore-gpu-blocklist', '--autoplay-policy=no-user-gesture-required'],
});
const page = ctx.pages()[0] || (await ctx.newPage());
const lines = [];
let menuAt = 0, starSeen = false;
const t0 = Date.now();
const stamp = () => ((Date.now() - t0) / 1000).toFixed(1);
page.on('console', (m) => {
  const t = m.text();
  if (/MainMenuDialogClass: started/.test(t)) menuAt = Date.now();
  if (/\[debug\] star at/.test(t)) starSeen = true;
  if (/\[debug\]|\[trace\]|\[audio\]|\[d3d9\] frame|abort|debug break|Assert|PAGEERROR|RuntimeError|Loading|Load_|Save|save|ScoreScreen|Mission/.test(t)) console.log(`${stamp()} ${t}`.slice(0, 240));
  lines.push(`${stamp()} ${t}`);
});
// Hang watchdog: a CPU profile runs from the campaign start; if the page logs
// nothing for 25 s, the profile is stopped and the hottest stacks of the last
// 10 s are printed (the debugger cannot attach to a busy page in time).
let lastLine = Date.now(), hangReported = false, profCdp = null;
page.on('console', () => { lastLine = Date.now(); });
async function startProfiler() {
  profCdp = await ctx.newCDPSession(page);
  await profCdp.send('Profiler.enable');
  await profCdp.send('Profiler.setSamplingInterval', { interval: 2000 });
  await profCdp.send('Profiler.start');
}
const watchdog = setInterval(async () => {
  if (hangReported || !profCdp || Date.now() - lastLine < 25000) return;
  hangReported = true;
  console.log(`${stamp()} HANG: no output for 25 s, stopping profiler`);
  const res = await Promise.race([profCdp.send('Profiler.stop'), new Promise((r) => setTimeout(() => r(null), 120000))]);
  if (!res) { console.log('HANG: profiler did not answer'); process.exit(3); }
  const { profile } = res;
  writeFileSync(`${outDir}/hang.cpuprofile`, JSON.stringify(profile));
  const byId = new Map(profile.nodes.map((n) => [n.id, n]));
  const parent = new Map();
  for (const n of profile.nodes) for (const c of n.children || []) parent.set(c, n.id);
  let t = profile.endTime; const cutoff = profile.endTime - 10e6;
  const stacks = new Map();
  for (let k = profile.samples.length - 1; k >= 0 && t > cutoff; k--) {
    t -= profile.timeDeltas[k];
    const names = [];
    for (let cur = profile.samples[k]; cur !== undefined; cur = parent.get(cur)) names.push(byId.get(cur).callFrame.functionName || '(anon)');
    const key = names.slice(0, 40).join('\n    <- ');
    stacks.set(key, (stacks.get(key) || 0) + 1);
  }
  for (const [k, c] of [...stacks.entries()].sort((a, b) => b[1] - a[1]).slice(0, 4)) console.log(`HANG stack x${c}:\n    ${k}\n`);
  process.exit(3);
}, 2000);
page.on("pageerror", (e) => { const l = `PAGEERROR ${e.message}\n${e.stack}`; lines.push(l); console.log(l); });
await page.goto('http://127.0.0.1:8461/?nolock&quiet');
await page.waitForSelector('#start:not([hidden])', { timeout: 900000 });
await page.evaluate(() => setTimeout(() => document.getElementById('start').click(), 0));
const skipper = setInterval(() => page.evaluate(() => { if (document.getElementById('movie').style.display === 'block') Module.rebelStopMovie(); }).catch(() => {}), 500);
while (!menuAt) await new Promise((r) => setTimeout(r, 500));
await page.waitForTimeout(5000);
await page.evaluate(() => setTimeout(() => Module._Rebel_Debug_Start_Campaign(0), 0));
await startProfiler();
const dump = () => page.evaluate(() => setTimeout(() => Module._Rebel_Debug_Input(), 0));
const call = (fn) => page.evaluate((f) => setTimeout(() => Module[f](), 0), fn);
const shot = (n) => page.screenshot({ path: `${outDir}/complete-${n}.png`, timeout: 60000 }).catch(() => console.log('screenshot timed out'));
async function waitPlayable(label) {
  starSeen = false;
  for (let i = 0; i < 60 && !starSeen; i++) { await page.waitForTimeout(5000); await dump(); }
  let cinematic = true;
  const h = (m) => { const r = /cinematic=(\d)/.exec(m.text()); if (r) cinematic = r[1] === '1'; };
  page.on('console', h);
  for (let i = 0; i < 40 && cinematic; i++) { await page.waitForTimeout(10000); await dump(); }
  page.off('console', h);
  console.log(`${stamp()} ---- ${label}: playable`);
}
let level = '';
page.on('console', (m) => { const r = /Load_Level.*?(M\d\d)|loading level (\S+)|Loading (M\d\d)\.mix/i.exec(m.text()); if (r) level = r[1] || r[2] || r[3]; });
const t0m = Date.now();
// M13 then M01-M11: 12 missions, then the finale and credits.
for (let mission = 0; mission < 12; mission++) {
  const started = Date.now();
  await waitPlayable(`mission ${mission}`);
  await call('_Rebel_Audio_Stats');
  await shot(`campaign-${mission}`);
  await page.mouse.click(640, 400);
  await page.keyboard.down('KeyW'); await page.waitForTimeout(3000); await page.keyboard.up('KeyW');
  await dump();
  console.log(`${stamp()} ==== mission ${mission} ok (${((Date.now() - started) / 1000).toFixed(0)} s to playable)`);
  await call('_Rebel_Debug_Win_Mission');
  await page.waitForTimeout(12000);
  await call('_Rebel_Debug_Dump');
  await shot(`campaign-${mission}-won`);
  await page.keyboard.press('Enter');
}
await page.waitForTimeout(60000);
await call('_Rebel_Debug_Dump');
await shot('campaign-end');
console.log(`campaign done in ${((Date.now() - t0m) / 60000).toFixed(1)} min`);
clearInterval(skipper);
clearInterval(watchdog);
writeFileSync(`${outDir}/campaign-console.log`, lines.join('\n'));
await ctx.close();
