// Walks the menus from the main menu: opens the buttons named by MENU (default
// "Options"; "Options/Configuration" goes two deep), then every button inside it, listing each dialog's controls
// (Rebel_Debug_List_Controls) and taking a screenshot, then backs out.
import { chromium } from 'playwright';
import { mkdirSync, writeFileSync } from 'node:fs';
const outDir = new URL('./out/', import.meta.url).pathname;
mkdirSync(outDir, { recursive: true });
const ctx = await chromium.launchPersistentContext(new URL('./profile/', import.meta.url).pathname, {
  channel: 'chromium', headless: true, viewport: { width: 1280, height: 800 },
  args: ['--use-angle=metal', '--enable-gpu', '--ignore-gpu-blocklist', '--autoplay-policy=no-user-gesture-required'],
});
const page = ctx.pages()[0] || (await ctx.newPage());
let menuAt = 0, listing = null;
const all = [];
page.on('console', (m) => {
  const t = m.text();
  all.push(t);
  if (/MainMenuDialogClass: started/.test(t)) menuAt = Date.now();
  const head = /\[debug\] controls of dialog (\d+)/.exec(t);
  if (head) listing = { id: +head[1], controls: [] };
  const c = /\[debug\]   ctl (-?\d+) (\S+) at (\S+) (disabled )?(hidden )?'(.*)'/.exec(t);
  if (c && listing) listing.controls.push({ id: +c[1], type: c[2], rect: c[3], disabled: !!c[4], hidden: !!c[5], text: c[6] });
  if (/\[debug\]|abort|Assert|RuntimeError|MessageBox/.test(t)) console.log(t.slice(0, 200));
});
page.on('pageerror', (e) => console.log(`PAGEERROR ${e.message}\n${e.stack}`));
await page.goto('http://127.0.0.1:8461/?nolock&quiet');
await page.waitForSelector('#start:not([hidden])', { timeout: 900000 });
await page.evaluate(() => setTimeout(() => document.getElementById('start').click(), 0));
const skipper = setInterval(() => page.evaluate(() => { if (document.getElementById('movie').style.display === 'block') Module.rebelStopMovie(); }).catch(() => {}), 500);
while (!menuAt) await new Promise((r) => setTimeout(r, 500));
await page.waitForTimeout(3000);
async function list() {
  listing = null;
  await page.evaluate(() => setTimeout(() => Module._Rebel_Debug_List_Controls(), 0));
  for (let i = 0; i < 20 && !listing; i++) await page.waitForTimeout(250);
  await page.waitForTimeout(500);
  return listing;
}
const press = (id) => page.evaluate((i) => setTimeout(() => Module._Rebel_Debug_Command(i), 0), id);
const buttons = (l) => l.controls.filter((c) => /Button|MenuEntry/.test(c.type) && !c.disabled && !c.hidden && c.text);
for (const name of (process.env.MENU || 'Options').split('/')) {
  const cur = await list();
  const target = buttons(cur).find((b) => b.text.toLowerCase().includes(name.toLowerCase()));
  if (!target) { console.log('no such button: ' + name); process.exit(1); }
  await press(target.id);
  await page.waitForTimeout(3000);
}
const opts = await list();
// CLICKS="x,y;x,y" clicks inside the opened menu (screenshots after each)
// instead of walking its buttons.
if (process.env.CLICKS) {
  let n = 0;
  for (const xy of process.env.CLICKS.split(';')) {
    const [x, y] = xy.split(',').map(Number);
    await page.mouse.move(x, y);
    await page.waitForTimeout(300);
    await page.mouse.down(); await page.waitForTimeout(100); await page.mouse.up();
    await page.waitForTimeout(2500);
    await list();
    await page.screenshot({ path: `${outDir}/menu-click-${n++}.png` });
  }
  writeFileSync(`${outDir}/menus-console.log`, all.join('\n'));
  await ctx.close();
  process.exit(0);
}
await page.screenshot({ path: `${outDir}/menu-${opts.id}.png` });
for (const b of buttons(opts)) {
  if (/back|quit|exit|ok|cancel|credits/i.test(b.text)) continue;
  console.log(`---- ${b.text}`);
  await press(b.id);
  await page.waitForTimeout(3000);
  const sub = await list();
  if (sub && sub.id !== opts.id) {
    await page.screenshot({ path: `${outDir}/menu-${sub.id}.png` });
    await page.keyboard.press('Escape');
    await page.waitForTimeout(2500);
    const back = await list();
    if (back.id !== opts.id) console.log(`!!!! Escape from ${sub.id} went to ${back.id}`);
  }
}
writeFileSync(`${outDir}/menus-console.log`, all.join('\n'));
clearInterval(skipper);
await ctx.close();
