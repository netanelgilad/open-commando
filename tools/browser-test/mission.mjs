// Boots to the main menu (skipping movies), starts the campaign through the
// debug hook, and screenshots the level load and the first gameplay frames.
import { chromium } from 'playwright';
import { mkdirSync, writeFileSync } from 'node:fs';
const seconds = Number(process.argv[2] || 240);
const outDir = new URL('./out/', import.meta.url).pathname;
mkdirSync(outDir, { recursive: true });
const ctx = await chromium.launchPersistentContext(new URL('./profile/', import.meta.url).pathname, {
  channel: 'chromium', headless: true, viewport: { width: 1280, height: 800 },
  args: ['--use-angle=metal', '--enable-gpu', '--ignore-gpu-blocklist', '--autoplay-policy=no-user-gesture-required'],
});
const page = ctx.pages()[0] || (await ctx.newPage());
const lines = [];
let menuAt = 0;
const t0 = Date.now();
const stamp = () => ((Date.now() - t0) / 1000).toFixed(1);
page.on('console', (m) => {
  const t = m.text();
  if (/MainMenuDialogClass: started/.test(t)) menuAt = Date.now();
  if (!/DefinitionHash|Hits: 0|Status Count/.test(t)) { lines.push(`${stamp()} ${t}`); if (!/\[d3d9\]/.test(t)) console.log(`${stamp()} ${t}`.slice(0, 220)); }
});
page.on('pageerror', (e) => { const l = `PAGEERROR ${e.message}\n${(e.stack || '').split('\n').slice(0, 12).join('\n')}`; lines.push(l); console.log(l); });
await page.goto((process.env.BASE || 'http://127.0.0.1:8461/') + '?nolock');
await page.waitForSelector('#start:not([hidden])', { timeout: 900000 });
await page.evaluate(() => setTimeout(() => document.getElementById('start').click(), 0));
const skipper = setInterval(() => page.evaluate(() => { if (window.rebelMovieActive) Module.rebelStopMovie(); }).catch(() => {}), 500);
while (!menuAt) await new Promise((r) => setTimeout(r, 500));
await page.waitForTimeout(5000);
await page.evaluate(() => setTimeout(() => Module._Rebel_Debug_Start_Campaign(0), 0));
for (let i = 1; i * 10 <= seconds; i++) {
  await page.waitForTimeout(10000);
  await page.screenshot({ path: `${outDir}/mission-${String(i * 10).padStart(3, '0')}s.png`, timeout: 120000 }).catch(() => {});
}
clearInterval(skipper);
writeFileSync(`${outDir}/mission-console.log`, lines.join('\n'));
await ctx.close();
