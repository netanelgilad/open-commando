// Cutscene check: boots without skipping movies, screenshots the intro
// cutscene (decoded from the imported .bik) and measures its audio output.
import { chromium } from 'playwright';
import { mkdirSync } from 'node:fs';
const outDir = new URL('./out/', import.meta.url).pathname;
mkdirSync(outDir, { recursive: true });
const ctx = await chromium.launchPersistentContext(new URL(process.env.PROFILE || './profile/', import.meta.url).pathname, {
  channel: 'chromium', headless: true, viewport: { width: 1280, height: 800 },
  args: ['--use-angle=metal', '--enable-gpu', '--ignore-gpu-blocklist', '--autoplay-policy=no-user-gesture-required'],
});
const page = ctx.pages()[0] || (await ctx.newPage());
await page.addInitScript(() => {
  const orig = AudioNode.prototype.connect, meters = new Map();
  AudioNode.prototype.connect = function (dest, ...rest) {
    if (dest instanceof AudioDestinationNode) {
      let m = meters.get(dest.context);
      if (!m) { m = dest.context.createAnalyser(); m.fftSize = 2048; meters.set(dest.context, m); }
      orig.call(this, m);
    }
    return orig.call(this, dest, ...rest);
  };
  window.__audioPeak = () => { let p = 0; for (const m of meters.values()) { const b = new Float32Array(m.fftSize); m.getFloatTimeDomainData(b); for (const v of b) p = Math.max(p, Math.abs(v)); } return p; };
});
const t0 = Date.now();
page.on('console', (m) => { const t = m.text(); if (/\[movie\]|MainMenu|rror/i.test(t)) console.log(((Date.now() - t0) / 1000).toFixed(1), t.slice(0, 200)); });
page.on('pageerror', (e) => console.log('PAGEERROR', e.message));
await page.goto((process.env.BASE || 'http://127.0.0.1:8461/') + '?nolock&quiet');
await page.waitForFunction(() => !document.getElementById('start').hidden || /Import failed|Startup failed/.test(document.getElementById('status').textContent), null, { timeout: 1800000, polling: 2000 });
console.log(((Date.now() - t0) / 1000).toFixed(1), 'status:', await page.textContent('#status'));
await page.evaluate(() => setTimeout(() => document.getElementById('start').click(), 0));
await page.waitForFunction(() => document.getElementById('moviecanvas').style.display === 'block', null, { timeout: 60000 });
for (let i = 0; i < 3; i++) {
  await page.waitForTimeout(4000);
  let peak = 0;
  for (let k = 0; k < 10; k++) { peak = Math.max(peak, await page.evaluate(() => window.__audioPeak())); await page.waitForTimeout(100); }
  await page.screenshot({ path: `${outDir}/movie-${i}.png` });
  console.log(((Date.now() - t0) / 1000).toFixed(1), `shot ${i}: audio peak ${peak.toFixed(3)}, canvas shown: ${await page.evaluate(() => document.getElementById('moviecanvas').style.display)}`);
}
await ctx.close();
