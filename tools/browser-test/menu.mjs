// Boots to the main menu (skipping movies), then performs clicks given as
// canvas coordinates in the game's 800x600 space: node menu.mjs "x,y;x,y;..."
// A screenshot is taken after each step.
import { chromium } from 'playwright';
import { mkdirSync, writeFileSync } from 'node:fs';
const steps = (process.argv[2] || '').split(';').filter(Boolean).map((s) => s.split(',').map(Number));
const wait = Number(process.argv[3] || 6000);
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
page.on('console', (m) => {
  const t = m.text();
  if (/MainMenuDialogClass: started/.test(t)) menuAt = Date.now();
  if (!/\[d3d9\] frame|DefinitionHash|Hits: 0/.test(t)) lines.push(`${((Date.now() - t0) / 1000).toFixed(1)} ${t}`);
});
page.on('pageerror', (e) => lines.push(`PAGEERROR ${e.message}\n${e.stack}`));
await page.goto((process.env.BASE || 'http://127.0.0.1:8461/') + '?nolock');
await page.waitForSelector('#start:not([hidden])', { timeout: 900000 });
await page.evaluate(() => setTimeout(() => document.getElementById('start').click(), 0));
const skipper = setInterval(() => page.evaluate(() => { if (window.rebelMovieActive) Module.rebelStopMovie(); }).catch(() => {}), 500);
while (!menuAt) await new Promise((r) => setTimeout(r, 500));
clearInterval(skipper);
await page.waitForTimeout(8000);
// map game coords (800x600 canvas, object-fit: contain) to page coords
const toPage = async (x, y) => page.evaluate(([x, y]) => {
  const c = document.getElementById('canvas'), r = c.getBoundingClientRect();
  const s = Math.min(r.width / c.width, r.height / c.height);
  return [r.left + (r.width - c.width * s) / 2 + x * s, r.top + (r.height - c.height * s) / 2 + y * s];
}, [x, y]);
await page.screenshot({ path: `${outDir}/menu-0.png` });
let i = 1;
for (const [x, y] of steps) {
  const [px, py] = await toPage(x, y);
  await page.mouse.move(px, py);
  await page.waitForTimeout(300);
  await page.mouse.down();
  await page.waitForTimeout(120);
  await page.mouse.up();
  await page.waitForTimeout(wait);
  await page.screenshot({ path: `${outDir}/menu-${i++}.png` });
}
writeFileSync(`${outDir}/menu-console.log`, lines.join('\n'));
console.log(lines.slice(-40).join('\n'));
await ctx.close();
