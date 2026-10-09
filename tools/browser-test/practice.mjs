// Multiplayer Practice (skirmish against bots, from the main menu): starts
// it the way the button does, waits until the player can move, walks and
// shoots, and reports crashes and network errors.
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
  if (/\[debug\]|\[hang\]|\[trace\]|\[audio\]|\[d3d9\] frame|abort|debug break|Assert|PAGEERROR|RuntimeError|Loading|Load_|Save|save|ScoreScreen|Mission|[Nn]etwork|socket|Winsock|WSA|cNetwork|server|Server/.test(t)) console.log(`${stamp()} ${t}`.slice(0, 240));
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
await page.goto((process.env.BASE || 'http://127.0.0.1:8461/') + '?nolock&quiet');
await page.waitForSelector('#start:not([hidden])', { timeout: 900000 });
await page.evaluate(() => setTimeout(() => document.getElementById('start').click(), 0));
const skipper = setInterval(() => page.evaluate(() => { if (window.rebelMovieActive) Module.rebelStopMovie(); }).catch(() => {}), 500);
while (!menuAt) await new Promise((r) => setTimeout(r, 500));
await page.waitForTimeout(5000);
await page.evaluate(() => setTimeout(() => Module._Rebel_Debug_Command(1563), 0)); // IDC_MENU_START_PRACTICE_GAME_BUTTON
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
await waitPlayable('practice');
await shot('practice');
await page.mouse.click(640, 400);
await page.keyboard.down('KeyW'); await page.waitForTimeout(3000); await page.keyboard.up('KeyW');
await page.mouse.down(); await page.waitForTimeout(2000); await page.mouse.up();
await dump();
await call('_Rebel_Audio_Stats');
await shot('practice-after');
await page.waitForTimeout(20000);
await dump();
await shot('practice-later');
clearInterval(skipper);
clearInterval(watchdog);
writeFileSync(`${outDir}/practice-console.log`, lines.join('\n'));
await ctx.close();
