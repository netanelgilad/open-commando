// Opens the EVA menu (the in-mission Escape menu) from the main menu and
// reports crashes. TABS env picks the tabs (default all, 0-6).
import { chromium } from 'playwright';
import { mkdirSync } from 'node:fs';
const outDir = new URL('./out/', import.meta.url).pathname;
mkdirSync(outDir, { recursive: true });
const ctx = await chromium.launchPersistentContext(new URL('./profile/', import.meta.url).pathname, {
  channel: 'chromium', headless: true, viewport: { width: 1280, height: 800 },
  args: ['--use-angle=metal', '--enable-gpu', '--ignore-gpu-blocklist', '--autoplay-policy=no-user-gesture-required'],
});
const page = ctx.pages()[0] || (await ctx.newPage());
let menuAt = 0;
page.on('console', (m) => {
  const t = m.text();
  if (/MainMenuDialogClass: started/.test(t)) menuAt = Date.now();
  if (/\[debug\]|\[trace\]|abort|Assert|RuntimeError/.test(t)) console.log(t.slice(0, 300));
});
page.on('pageerror', (e) => console.log(`PAGEERROR ${e.message}\n${e.stack}`));
await page.goto((process.env.BASE || 'http://127.0.0.1:8461/') + '?nolock&quiet');
await page.waitForSelector('#start:not([hidden])', { timeout: 900000 });
await page.evaluate(() => setTimeout(() => document.getElementById('start').click(), 0));
const skipper = setInterval(() => page.evaluate(() => { if (document.getElementById('movie').style.display === 'block') Module.rebelStopMovie(); }).catch(() => {}), 500);
while (!menuAt) await new Promise((r) => setTimeout(r, 500));
await page.waitForTimeout(3000);
for (const tab of (process.env.TABS || '0,1,2,3,4,5,6').split(',').map(Number)) {
  await page.evaluate((t) => setTimeout(() => Module._Rebel_Debug_Open_Eva(t), 0), tab);
  await page.waitForTimeout(5000);
  await page.screenshot({ path: `${outDir}/eva-${tab}.png` });
  await page.keyboard.press('Escape');
  await page.waitForTimeout(2000);
}
clearInterval(skipper);
await ctx.close();
