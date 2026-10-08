// Starts the campaign, waits for the player, then holds W / moves the mouse
// and dumps the input pipeline state (Rebel_Debug_Input) along the way.
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
  if (/\[debug\]|DirectInput|Input::|\[d3d9\] frame|abort|debug break|damag|Assert|PAGEERROR|RuntimeError/.test(t)) console.log(`${stamp()} ${t}`.slice(0, 240));
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
for (let i = 0; i < 60 && !starSeen; i++) { await page.waitForTimeout(5000); await dump(); }
let cinematic = true;
page.on('console', (m) => { const r = /cinematic=(\d)/.exec(m.text()); if (r) cinematic = r[1] === '1'; });
for (let i = 0; i < 40 && cinematic; i++) { await page.waitForTimeout(10000); await dump(); if (i % 3 == 0) await page.screenshot({ path: `${outDir}/input-wait-${i}.png` }); }
await page.screenshot({ path: `${outDir}/input-before.png` });
await page.mouse.click(640, 400);
await page.keyboard.down('KeyW');
for (let i = 0; i < 6; i++) { await page.waitForTimeout(500); await dump(); }
await page.keyboard.up('KeyW');
await page.screenshot({ path: `${outDir}/input-after-w.png` });
for (let i = 0; i < 20; i++) { await page.evaluate(() => Module._Rebel_Input_Mouse_Move(20, 0)); await page.waitForTimeout(100); }
await dump();
await page.screenshot({ path: `${outDir}/input-after-mouse.png` });
// Fire: hold the left button (aimed at whatever is in front), then check
// the game keeps presenting frames.
for (let round = 0; round < 3; round++) {
  await page.evaluate(() => setTimeout(() => Module._Rebel_Debug_Bring_Target(), 0));
  await page.waitForTimeout(1500);
  await page.screenshot({ path: `${outDir}/input-target-${round}.png`, timeout: 60000 }).catch(() => {});
  await page.mouse.down();
  for (let i = 0; i < 6; i++) { await page.waitForTimeout(500); await dump(); }
  await page.mouse.up();
  const amount = [30, 500, 500][round];
  await page.evaluate((a) => setTimeout(() => Module._Rebel_Debug_Damage_Target(a), 0), amount);
  await page.waitForTimeout(3000);
  await page.screenshot({ path: `${outDir}/input-shoot-${round}.png`, timeout: 60000 }).catch(() => console.log('screenshot timed out'));
}
await page.waitForTimeout(15000);
await dump();
clearInterval(skipper);
clearInterval(watchdog);
writeFileSync(`${outDir}/input-console.log`, lines.join('\n'));
await ctx.close();
