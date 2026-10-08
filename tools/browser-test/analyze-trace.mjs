// node analyze-trace.mjs out/mission-trace.json — total duration per event name, per thread.
import { readFileSync } from 'node:fs';
const { traceEvents: ev } = JSON.parse(readFileSync(process.argv[2]));
const threads = new Map(), procs = new Map();
for (const e of ev) {
  if (e.ph === 'M' && e.name === 'thread_name') threads.set(`${e.pid}:${e.tid}`, e.args.name);
  if (e.ph === 'M' && e.name === 'process_name') procs.set(e.pid, e.args.name);
}
const agg = new Map();
let t0 = Infinity, t1 = 0;
for (const e of ev) {
  if (e.ph !== 'X' || !e.dur) continue;
  t0 = Math.min(t0, e.ts); t1 = Math.max(t1, e.ts + e.dur);
  const key = `${procs.get(e.pid) || e.pid} / ${threads.get(`${e.pid}:${e.tid}`) || e.tid} / ${e.name}`;
  const a = agg.get(key) || { dur: 0, n: 0, max: 0 };
  a.dur += e.dur; a.n++; a.max = Math.max(a.max, e.dur);
  agg.set(key, a);
}
const span = (t1 - t0) / 1000;
console.log(`span ${span.toFixed(0)} ms`);
for (const [k, a] of [...agg.entries()].sort((x, y) => y[1].dur - x[1].dur).slice(0, Number(process.argv[3] || 45)))
  console.log(`${(a.dur / 1000).toFixed(0).padStart(7)} ms  n=${String(a.n).padStart(6)}  max ${(a.max / 1000).toFixed(1).padStart(6)}  ${k}`.slice(0, 200));
