// The game's record (run-server.mjs): everything that happens in a match
// server's games, for tuning the bots afterwards. One JSON object a line,
// {t, type, ...}: map changes, joins and leaves, kills and deaths, building
// damage and destruction, purchases, vehicles, chat, a snapshot of the world
// every few seconds, the bots' decisions (state, questions, probabilities,
// actions) and their planner calls (prompt, reply, model, latency, tokens),
// and the server's resources (fps, timer lateness, CPU, memory, AI spend).
//
// Sent every 30 s as a gzipped batch to the match service (POST
// <url>?seq=<n>, the match's key; services/match stores it in R2), or written
// to a directory (--record-dir) when developing.
import { gzipSync } from 'node:zlib';
import { appendFileSync, mkdirSync } from 'node:fs';
import { join } from 'node:path';

const FLUSH_MS = 30000;

export function createRecorder({ url, key, dir, out = console.log }) {
  if (!url && !dir) return { add() {}, flush: async () => {} };
  if (dir) mkdirSync(dir, { recursive: true });
  let lines = [];
  let seq = 0;
  let sending = Promise.resolve();
  const stats = { batches: 0, bytes: 0, failed: 0 };

  const flush = () => {
    if (!lines.length) return sending;
    const batch = lines.join('\n') + '\n';
    lines = [];
    const n = seq++;
    if (dir) {
      appendFileSync(join(dir, 'record.jsonl'), batch);
      return sending;
    }
    const body = gzipSync(batch);
    sending = sending.then(async () => {
      for (let attempt = 0; attempt < 3; attempt++) {
        try {
          const res = await fetch(`${url}?seq=${n}`, { method: 'POST', headers: { authorization: `Bearer ${key}`, 'content-type': 'application/gzip' }, body, signal: AbortSignal.timeout(15000) });
          if (res.ok) {
            stats.batches++;
            stats.bytes += body.length;
            return;
          }
          throw new Error(`HTTP ${res.status}`);
        } catch (e) {
          if (attempt === 2) {
            stats.failed++;
            out(`[record] batch ${n} lost: ${e.message}`);
          }
        }
      }
    });
    return sending;
  };
  setInterval(flush, FLUSH_MS).unref();
  out(`[record] recording to ${dir || url}`);
  return {
    add(type, data = {}) {
      lines.push(JSON.stringify({ t: Date.now(), type, ...data }));
    },
    flush,
    stats,
  };
}
