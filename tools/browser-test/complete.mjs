// Completeness pass in a mission: audio state, pause menu (Esc), save,
// walk away, load (position must come back), then win the mission and
// follow what comes next (score screen, next level).
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
// Audio meter: everything connected to an AudioContext's destination is also
// fed to an analyser, so the test can tell whether sound actually comes out.
await page.addInitScript(() => {
  const orig = AudioNode.prototype.connect;
  const meters = new Map();
  AudioNode.prototype.connect = function (dest, ...rest) {
    if (dest instanceof AudioDestinationNode) {
      let m = meters.get(dest.context);
      if (!m) { m = dest.context.createAnalyser(); m.fftSize = 2048; meters.set(dest.context, m); }
      orig.call(this, m);
    }
    return orig.call(this, dest, ...rest);
  };
  window.__audioLevel = () => {
    let peak = 0;
    for (const m of meters.values()) {
      const buf = new Float32Array(m.fftSize);
      m.getFloatTimeDomainData(buf);
      for (const v of buf) peak = Math.max(peak, Math.abs(v));
    }
    return { contexts: meters.size, peak };
  };
});
const audioLevel = async (label) => {
  let peak = 0, contexts = 0;
  for (let i = 0; i < 20; i++) { const r = await page.evaluate(() => window.__audioLevel()); peak = Math.max(peak, r.peak); contexts = r.contexts; await page.waitForTimeout(100); }
  console.log(`${stamp()} [audio] ${label}: output peak ${peak.toFixed(3)} (${contexts} contexts)`);
};
page.on("pageerror", (e) => { const l = `PAGEERROR ${e.message}\n${e.stack}`; lines.push(l); console.log(l); });
await page.goto('http://127.0.0.1:8461/?nolock&quiet');
await page.waitForSelector('#start:not([hidden])', { timeout: 900000 });
await page.evaluate(() => setTimeout(() => document.getElementById('start').click(), 0));
const skipper = setInterval(() => page.evaluate(() => { if (document.getElementById('movie').style.display === 'block') Module.rebelStopMovie(); }).catch(() => {}), 500);
while (!menuAt) await new Promise((r) => setTimeout(r, 500));
await page.waitForTimeout(5000);
await audioLevel('main menu');
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
const step = process.env.STEP || 'all';
await waitPlayable('mission');
await call('_Rebel_Audio_Stats');
await audioLevel('mission');
await shot('playing');
// Pause menu.
await page.keyboard.press('Escape');
await page.waitForTimeout(3000);
await call('_Rebel_Debug_Dump');
await shot('esc');
await page.keyboard.press('Escape');
await page.waitForTimeout(3000);
await call('_Rebel_Debug_Dump');
await shot('esc-closed');
if (step === 'esc') { writeFileSync(`${outDir}/complete-console.log`, lines.join('\n')); process.exit(0); }
// Save, walk away, load.
await dump();
await call('_Rebel_Debug_Save');
await page.waitForTimeout(5000);
await page.mouse.click(640, 400);
await page.keyboard.down('KeyW');
await page.waitForTimeout(4000);
await page.keyboard.up('KeyW');
await dump();
await page.waitForTimeout(1000);
await call('_Rebel_Debug_Load');
await waitPlayable('loaded game');
await call('_Rebel_Audio_Stats');
await shot('loaded');
// Win and follow.
await call('_Rebel_Debug_Win_Mission');
for (let i = 0; i < 12; i++) { await page.waitForTimeout(10000); await call('_Rebel_Debug_Dump'); await shot(`won-${i}`); }
await page.keyboard.press('Enter');
await waitPlayable('next mission');
await call('_Rebel_Audio_Stats');
await shot('next');
clearInterval(skipper);
clearInterval(watchdog);
writeFileSync(`${outDir}/complete-console.log`, lines.join('\n'));
await ctx.close();
