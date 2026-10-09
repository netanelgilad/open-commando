// node callers.mjs profile.cpuprofile funcName [depth]: who calls funcName (sampled stacks).
import { readFileSync } from 'node:fs';
const p = JSON.parse(readFileSync(process.argv[2]));
const target = process.argv[3], depth = Number(process.argv[4] || 6);
const byId = new Map(p.nodes.map((n) => [n.id, n]));
const parent = new Map();
for (const n of p.nodes) for (const c of n.children || []) parent.set(c, n.id);
const agg = new Map();
let total = 0;
for (const s of p.samples) {
  if (byId.get(s).callFrame.functionName !== target) continue;
  total++;
  const chain = [];
  for (let cur = parent.get(s); cur !== undefined && chain.length < depth; cur = parent.get(cur)) chain.push(byId.get(cur).callFrame.functionName.replace(/\(.*$/, ''));
  const k = chain.join(' <- ');
  agg.set(k, (agg.get(k) || 0) + 1);
}
console.log(`${total} samples in ${target}`);
for (const [k, c] of [...agg.entries()].sort((a, b) => b[1] - a[1]).slice(0, 15)) console.log(`${String(c).padStart(5)}  ${k.slice(0, 300)}`);
