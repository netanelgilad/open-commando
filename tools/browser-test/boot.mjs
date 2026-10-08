// Headless boot test: opens the served page, waits for the OPFS import,
// clicks Play, then records console output and canvas screenshots.
// Usage: node boot.mjs [url] [seconds]
import { chromium } from 'playwright';
import { mkdirSync, writeFileSync } from 'node:fs';

const url = process.argv[2] || 'http://127.0.0.1:8461/?log';
const seconds = Number(process.argv[3] || 60);
const outDir = new URL('./out/', import.meta.url).pathname;
mkdirSync(outDir, { recursive: true });
const userData = new URL('./profile/', import.meta.url).pathname; // keeps OPFS between runs

const ctx = await chromium.launchPersistentContext(userData, {
  headless: true,
  viewport: { width: 1280, height: 800 },
  args: ['--enable-unsafe-swiftshader', '--use-angle=swiftshader', '--ignore-gpu-blocklist', '--autoplay-policy=no-user-gesture-required'],
});
const page = ctx.pages()[0] || (await ctx.newPage());
const lines = [];
const t0 = Date.now();
const stamp = () => ((Date.now() - t0) / 1000).toFixed(1).padStart(6);
page.on('console', (m) => { const l = `${stamp()} [${m.type()}] ${m.text()}`; lines.push(l); if (!/progress/.test(l)) console.log(l); });
page.on('pageerror', (e) => { const l = `${stamp()} [pageerror] ${e.message}`; lines.push(l); console.log(l); });
await page.goto(url);
// import + engine load
await page.waitForSelector('#start:not([hidden])', { timeout: 30 * 60 * 1000 });
console.log(`${stamp()} ready, clicking Play`);
await page.click('#start');
for (let i = 1; i * 5 <= seconds; i++) {
  await page.waitForTimeout(5000);
  await page.screenshot({ path: `${outDir}/shot-${String(i * 5).padStart(3, '0')}s.png` });
  const status = await page.$eval('#status', (e) => e.textContent).catch(() => '');
  if (/stopped|failed/i.test(status)) { console.log(`${stamp()} status: ${status}`); break; }
}
writeFileSync(`${outDir}/console.log`, lines.join('\n'));
await ctx.close();
