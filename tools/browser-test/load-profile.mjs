// CPU profile of a level load: main menu -> campaign start, profiles the page
// main thread for SECS (default 35) seconds and prints the load timeline.
// BOOT=1 profiles from the Play click to the main menu instead.
import { chromium } from 'playwright';
import { writeFileSync } from 'node:fs';
const secs = Number(process.env.SECS || 35);
const ctx = await chromium.launchPersistentContext(new URL('./profile/', import.meta.url).pathname, {
  channel: 'chromium', headless: true, viewport: { width: 1280, height: 800 },
  args: ['--use-angle=metal', '--enable-gpu', '--ignore-gpu-blocklist', '--autoplay-policy=no-user-gesture-required'],
});
const page = ctx.pages()[0] || (await ctx.newPage());
let menuAt = 0;
const t0 = Date.now();
page.on('console', (m) => {
  const t = m.text();
  if (/MainMenuDialogClass: started/.test(t)) menuAt = Date.now();
  if (/^\[(rebel|debug|load|movie)\]/.test(t)) console.log(((Date.now() - t0) / 1000).toFixed(1), t.slice(0, 160));
});
await page.goto((process.env.BASE || 'http://127.0.0.1:8461/') + '?nolock&quiet');
await page.waitForSelector('#start:not([hidden])', { timeout: 900000 });
let bootCdp = null;
if (process.env.BOOT) {
  bootCdp = await ctx.newCDPSession(page);
  await bootCdp.send('Profiler.enable');
  await bootCdp.send('Profiler.setSamplingInterval', { interval: 1000 });
  await bootCdp.send('Profiler.start');
}
await page.evaluate(() => setTimeout(() => document.getElementById('start').click(), 0));
const skipper = setInterval(() => page.evaluate(() => { if (window.rebelMovieActive) Module.rebelStopMovie(); }).catch(() => {}), 500);
while (!menuAt) await new Promise((r) => setTimeout(r, 200));
if (bootCdp) {
  const { profile } = await bootCdp.send('Profiler.stop');
  writeFileSync(new URL('./out/boot.cpuprofile', import.meta.url).pathname, JSON.stringify(profile));
  await ctx.close();
  process.exit(0);
}
await page.waitForTimeout(3000);
const cdp = await ctx.newCDPSession(page);
await cdp.send('Profiler.enable');
await cdp.send('Profiler.setSamplingInterval', { interval: 1000 });
await cdp.send('Profiler.start');
await page.evaluate(() => setTimeout(() => Module._Rebel_Debug_Start_Campaign(0), 0));
await page.waitForTimeout(secs * 1000);
const { profile } = await cdp.send('Profiler.stop');
writeFileSync(new URL('./out/load.cpuprofile', import.meta.url).pathname, JSON.stringify(profile));
clearInterval(skipper);
await ctx.close();
