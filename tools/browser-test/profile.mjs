// Boots to the main menu, then records a 15 s CPU profile of the page and
// prints the functions with the most self time.
import { chromium } from 'playwright';
const userData = new URL('./profile/', import.meta.url).pathname;
const ctx = await chromium.launchPersistentContext(userData, {
  channel: 'chromium', headless: true, viewport: { width: 1280, height: 800 },
  args: ['--use-angle=metal', '--enable-gpu', '--ignore-gpu-blocklist', '--autoplay-policy=no-user-gesture-required'],
});
const page = ctx.pages()[0] || (await ctx.newPage());
let menuAt = 0;
page.on('console', (m) => { if (/MainMenuDialogClass: started/.test(m.text())) menuAt = Date.now(); });
await page.goto('http://127.0.0.1:8461/');
await page.waitForSelector('#start:not([hidden])', { timeout: 900000 });
await page.evaluate(() => setTimeout(() => document.getElementById('start').click(), 0));
// skip the intro movies as soon as they start
const skipper = setInterval(() => page.evaluate(() => { if (document.getElementById('movie').style.display === 'block') Module.rebelStopMovie(); }).catch(() => {}), 1000);
while (!menuAt) await new Promise((r) => setTimeout(r, 1000));
clearInterval(skipper);
await new Promise((r) => setTimeout(r, 15000)); // let the menu settle
const cdp = await ctx.newCDPSession(page);
await cdp.send('Profiler.enable');
await cdp.send('Profiler.setSamplingInterval', { interval: 200 });
await cdp.send('Profiler.start');
await new Promise((r) => setTimeout(r, 15000));
const { profile } = await cdp.send('Profiler.stop');
const self = new Map();
const total = profile.samples.length;
const counts = new Map();
for (const id of profile.samples) counts.set(id, (counts.get(id) || 0) + 1);
for (const n of profile.nodes) {
  const c = counts.get(n.id) || 0;
  if (!c) continue;
  const name = n.callFrame.functionName || `(${n.callFrame.url.split('/').pop() || 'native'})`;
  self.set(name, (self.get(name) || 0) + c);
}
const top = [...self.entries()].sort((a, b) => b[1] - a[1]).slice(0, 40);
for (const [name, c] of top) console.log(`${(100 * c / total).toFixed(1).padStart(5)}%  ${name.slice(0, 150)}`);
await ctx.close();
