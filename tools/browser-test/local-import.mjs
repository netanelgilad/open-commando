// Public-site import check: fresh browser profile, a server with no game
// data, the player picks their install folder (the <input webkitdirectory>
// path; Playwright can't drive the native directory picker). Then plays to
// the main menu and starts the campaign.
import { chromium } from 'playwright';
import { mkdirSync, rmSync } from 'node:fs';
const outDir = new URL('./out/', import.meta.url).pathname;
mkdirSync(outDir, { recursive: true });
const profile = new URL('./profile-fresh/', import.meta.url).pathname;
if (!process.env.KEEP) rmSync(profile, { recursive: true, force: true });
const gameDir = process.env.GAME_DIR || new URL('../../../game-data/renegade', import.meta.url).pathname;
const ctx = await chromium.launchPersistentContext(profile, {
  channel: 'chromium', headless: true, viewport: { width: 1280, height: 800 },
  args: ['--use-angle=metal', '--enable-gpu', '--ignore-gpu-blocklist', '--autoplay-policy=no-user-gesture-required'],
});
const page = ctx.pages()[0] || (await ctx.newPage());
await page.addInitScript(() => { delete window.showDirectoryPicker; });
const t0 = Date.now();
const stamp = () => ((Date.now() - t0) / 1000).toFixed(1);
let menu = false;
page.on('console', (m) => { const t = m.text(); if (/MainMenuDialogClass: started/.test(t)) menu = true; if (/\[movie\]|\[load\]|MainMenu|rror|\[hang\]/i.test(t)) console.log(stamp(), t.slice(0, 160)); });
page.on('pageerror', (e) => console.log('PAGEERROR', e.message));
await page.goto((process.env.BASE || 'http://127.0.0.1:8471/') + '?nolock&quiet');
await page.waitForSelector('#choose:not([hidden])', { timeout: 60000 });
await page.screenshot({ path: `${outDir}/import-choose.png` });
console.log(stamp(), 'chooser shown:', (await page.textContent('#status')).trim());
// A wrong folder first: the Data folder itself.
await page.setInputFiles('#pickinput', gameDir + '/Data');
await page.waitForFunction(() => document.getElementById('pickerror').textContent.length > 0, null, { timeout: 120000 });
console.log(stamp(), 'wrong folder error:', await page.textContent('#pickerror'));
await page.setInputFiles('#pickinput', gameDir);
await page.waitForFunction(() => !document.getElementById('start').hidden || /failed/i.test(document.getElementById('status').textContent), null, { timeout: 1800000, polling: 5000 });
console.log(stamp(), 'status:', (await page.textContent('#status')).trim());
await page.screenshot({ path: `${outDir}/import-ready.png` });
await page.evaluate(() => setTimeout(() => document.getElementById('start').click(), 0));
const skipper = setInterval(() => page.evaluate(() => { if (window.rebelMovieActive) Module.rebelStopMovie(); }).catch(() => {}), 500);
while (!menu) await new Promise((r) => setTimeout(r, 500));
await page.waitForTimeout(3000);
await page.evaluate(() => setTimeout(() => Module._Rebel_Debug_Start_Campaign(0), 0));
await page.waitForTimeout(25000);
await page.screenshot({ path: `${outDir}/import-mission.png` });
clearInterval(skipper);
await ctx.close();
