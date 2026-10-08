// node analyze.mjs out/mission.cpuprofile [N] — self and inclusive time per function.
import { readFileSync } from 'node:fs';
const profile = JSON.parse(readFileSync(process.argv[2]));
const N = Number(process.argv[3] || 40);
const byId = new Map(profile.nodes.map((n) => [n.id, n]));
const parent = new Map();
for (const n of profile.nodes) for (const c of n.children || []) parent.set(c, n.id);
const name = (n) => n.callFrame.functionName || `(${n.callFrame.url.split('/').pop() || 'native'})`;
const self = new Map(), incl = new Map();
const total = profile.samples.length;
for (const id of profile.samples) {
  let n = byId.get(id);
  self.set(name(n), (self.get(name(n)) || 0) + 1);
  const seen = new Set();
  for (let cur = id; cur !== undefined; cur = parent.get(cur)) {
    const nm = name(byId.get(cur));
    if (!seen.has(nm)) { seen.add(nm); incl.set(nm, (incl.get(nm) || 0) + 1); }
  }
}
const show = (m, title) => {
  console.log(`--- ${title}`);
  for (const [k, c] of [...m.entries()].sort((a, b) => b[1] - a[1]).slice(0, N)) console.log(`${(100 * c / total).toFixed(1).padStart(5)}%  ${k.slice(0, 140)}`);
};
show(self, 'self');
show(incl, 'inclusive');
