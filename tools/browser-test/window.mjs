// node window.mjs profile.cpuprofile fromSec toSec: inclusive time per function in a time window.
import { readFileSync } from 'node:fs';
const p = JSON.parse(readFileSync(process.argv[2]));
const from = Number(process.argv[3]) * 1e6, to = Number(process.argv[4]) * 1e6;
const byId = new Map(p.nodes.map((n) => [n.id, n]));
const parent = new Map();
for (const n of p.nodes) for (const c of n.children || []) parent.set(c, n.id);
const incl = new Map(), self = new Map();
let t = 0, n = 0;
for (let i = 0; i < p.samples.length; i++) {
  t += p.timeDeltas[i];
  if (t < from || t > to) continue;
  n++;
  const s = p.samples[i];
  const sn = byId.get(s).callFrame.functionName || '(anon)';
  self.set(sn, (self.get(sn) || 0) + 1);
  const seen = new Set();
  for (let cur = s; cur !== undefined; cur = parent.get(cur)) {
    const f = byId.get(cur).callFrame.functionName || '(anon)';
    if (seen.has(f)) continue;
    seen.add(f);
    incl.set(f, (incl.get(f) || 0) + 1);
  }
}
console.log(`${n} samples`);
console.log('--- self'); for (const [f, c] of [...self].sort((a, b) => b[1] - a[1]).slice(0, 15)) console.log(`${(100 * c / n).toFixed(1).padStart(6)}%  ${f.slice(0, 120)}`);
console.log('--- inclusive'); for (const [f, c] of [...incl].sort((a, b) => b[1] - a[1]).slice(0, 40)) console.log(`${(100 * c / n).toFixed(1).padStart(6)}%  ${f.slice(0, 120)}`);
