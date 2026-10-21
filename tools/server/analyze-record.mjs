#!/usr/bin/env node
// Summarises a game record (recorder.mjs JSONL: a local --record-dir file, or
// a match's record fetched from /record/<id>) for tuning the bots: how the
// match went, what the bots did and planned, what it cost.
//
//   node tools/server/analyze-record.mjs <record.jsonl> [--samples 3]
import { readFileSync } from 'node:fs';
import { parseArgs } from 'node:util';
import { callUsd } from './bot-brain.mjs';

const { values: opt, positionals } = parseArgs({ allowPositionals: true, options: { samples: { type: 'string', default: '3' } } });
const lines = readFileSync(positionals[0], 'utf8').split('\n').filter(Boolean).map((l) => JSON.parse(l));
const of = (type) => lines.filter((x) => x.type === type);
const count = (list, key) => Object.entries(list.reduce((m, x) => ((m[key(x)] = (m[key(x)] || 0) + 1), m), {})).sort((a, b) => b[1] - a[1]);
const top = (pairs, n = 8) => pairs.slice(0, n).map(([k, v]) => `${k} ${v}`).join(', ');
const t0 = lines[0]?.t || 0;
const min = (t) => ((t - t0) / 60000).toFixed(1);
const out = [];
const say = (s = '') => out.push(s);

const snaps = of('snapshot');
const last = snaps.at(-1);
say(`Record: ${lines.length} entries over ${min(lines.at(-1)?.t || t0)} min; map ${snaps[0]?.map || '?'}`);

// The match: buildings, kills, who won.
if (last) {
  say('\n== Buildings (final health)');
  for (const team of ['GDI', 'Nod']) say(`${team}: ${last.buildings.filter((b) => b.team === team).map((b) => `${b.name.replace(/^mp_(GDI_|Nod_)?/, '')} ${Math.round(b.hp * 100)}%`).join(', ')}`);
  const bevents = lines.filter((x) => /^building_/.test(x.type));
  for (const e of bevents.slice(0, 30)) say(`  ${min(e.t)} min ${e.type} ${e.team || ''} ${e.name || e.building || ''} ${e.hp !== undefined ? Math.round(e.hp * 100) + '%' : ''}`);
  // Health over time: when each building first took damage, and its lowest.
  const low = {};
  for (const s of snaps) for (const b of s.buildings) {
    const k = `${b.team} ${b.name.replace(/^mp_(GDI_|Nod_)?/, '')}`;
    if (!low[k]) low[k] = { min: 1, first: null };
    if (b.hp < low[k].min) low[k].min = b.hp;
    if (b.hp < 0.99 && low[k].first === null) low[k].first = s.t;
  }
  say(`Damage: ${Object.entries(low).filter(([, v]) => v.first).map(([k, v]) => `${k} hit at ${min(v.first)} min, low ${Math.round(v.min * 100)}%`).join('; ') || 'no building was damaged'}`);
  say('\n== Players (final)');
  for (const team of ['GDI', 'Nod']) {
    const ps = last.players.filter((p) => p.team === team);
    const k = ps.reduce((n, p) => n + p.kills, 0), d = ps.reduce((n, p) => n + p.deaths, 0);
    say(`${team}: ${ps.length} players, ${k} kills, ${d} deaths, credits avg ${Math.round(ps.reduce((n, p) => n + p.credits, 0) / Math.max(1, ps.length))}`);
  }
}

// Deaths: where, as what.
const deaths = of('death');
if (deaths.length) {
  say(`\n== Deaths (${deaths.length})`);
  say(`where: ${top(count(deaths, (d) => `${d.team} in ${d.place}`))}`);
  say(`as: ${top(count(deaths, (d) => d.char || '?'))}`);
}

// Buying, vehicles.
const buys = lines.filter((x) => x.type === 'command' && / buy /.test(x.line || ''));
const chars = of('character'), enters = of('vehicle_enter');
say(`\n== Spending: ${buys.length} buy orders; ${chars.length} character changes; ${enters.length} vehicle entries`);
say(`became: ${top(count(chars.filter((c) => c.bot), (c) => c.to))}`);
say(`drove: ${top(count(enters.filter((c) => c.bot), (c) => c.vehicle))}`);

// Plans: what the bots chose to do.
const plans = of('plans');
if (plans.length) {
  // Plans as used: buildings by id (names from the snapshots), no kind field.
  const names = new Map(snaps.flatMap((s) => s.buildings.map((b) => [b.id, `${b.team} ${b.name.replace(/^mp_(GDI_|Nod_)?/, '')}`])));
  const kindOf = (x) => x.kind || (x.item ? 'buy' : x.building ? (/^defend|guard|protect/i.test(`${x.id} ${x.text}`) ? 'defend' : 'attack') : x.place ? 'go' : x.teammate ? 'follow' : '?');
  const all = plans.flatMap((p) => (p.plans || []).map((x) => ({ ...x, kind: kindOf(x), building_name: names.get(x.building) || x.building, by: p.by, team: p.team })));
  say(`\n== Plans: ${plans.length} plan sets (${top(count(plans, (p) => p.by))})`);
  say(`kinds: ${top(count(all, (x) => x.kind))}`);
  say(`attack targets: ${top(count(all.filter((x) => x.kind === 'attack'), (x) => `${x.team}->${x.building_name}`))}`);
  for (const by of ['model', 'scripted']) {
    const mine = all.filter((x) => x.by === by);
    if (mine.length) say(`  ${by}: ${top(count(mine, (x) => x.kind))}`);
  }
  say(`buy items: ${top(count(all.filter((x) => x.kind === 'buy'), (x) => x.item))}`);
}

// The planner: calls, errors, cost, cache.
const calls = of('planner_call'), errors = of('planner_error');
if (calls.length || errors.length) {
  say(`\n== Planner: ${calls.length} calls, ${errors.length} errors`);
  say(`by model: ${top(count(calls, (c) => c.model))}`);
  if (errors.length) say(`errors: ${top(count(errors, (e) => `${e.model}: ${String(e.error).slice(0, 60)}`), 5)}`);
  const ms = calls.map((c) => c.ms).filter(Boolean).sort((a, b) => a - b);
  if (ms.length) say(`latency ms: median ${ms[ms.length >> 1]}, p90 ${ms[Math.floor(ms.length * 0.9)]}`);
  const u = calls.map((c) => c.usage || {});
  const sum = (f) => u.reduce((n, x) => n + (f(x) || 0), 0);
  const prompt = sum((x) => x.prompt_tokens), cached = sum((x) => x.prompt_tokens_details?.cached_tokens ?? x.cached_tokens), outT = sum((x) => x.completion_tokens);
  const usd = calls.reduce((n, c) => n + callUsd(c.model, c.usage), 0);
  say(`tokens: prompt ${prompt} (cached ${cached}, ${prompt ? Math.round((100 * cached) / prompt) : 0}%), output ${outT}; per call: prompt ${Math.round(prompt / Math.max(1, calls.length))}, output ${Math.round(outT / Math.max(1, calls.length))}`);
  say(`cost: $${usd.toFixed(3)} planner (${((usd / Math.max(1, calls.length)) * 1000).toFixed(3)} m$ per call)`);
  say(`reasons: ${top(count(calls, (c) => String(c.reason).slice(0, 50)), 6)}`);
  const n = +opt.samples;
  for (const c of calls.filter((_, i) => i % Math.max(1, Math.floor(calls.length / n)) === 0).slice(0, n)) say(`  sample ${min(c.t)} min ${c.bot} (${c.model}, ${c.ms} ms): ${String(c.reply).replace(/\s+/g, ' ').slice(0, 400)}`);
}

// The whole AI bill (the brain's own count, Clef included) and decisions.
const ai = of('perf').map((p) => p.ai).filter(Boolean).at(-1);
if (ai) say(`\n== AI spend (brain's count): $${ai.usd} of $${ai.budgetUsd} budget; ${ai.decisions} Clef decisions, ${ai.plans} plans; prompt cached share ${ai.cachedShare}`);
const decisions = of('decision');
if (decisions.length) say(`decisions: ${top(count(decisions, (d) => d.by))}`);
// The planner per minute: answers, rate limits, timeouts by model.
if (calls.length || errors.length) {
  const per = {};
  for (const x of [...calls, ...errors]) {
    const m = Math.floor((x.t - t0) / 60000);
    const k = x.type === 'planner_call' ? `${x.model} ok` : /3021|rate/i.test(x.error) ? `${x.model} limited` : `${x.model} failed`;
    ((per[m] ??= {})[k] = (per[m][k] || 0) + 1);
  }
  say(`planner per minute: ${Object.entries(per).map(([m, v]) => `${m}: ${Object.entries(v).map(([k, n]) => `${k} ${n}`).join(', ')}`).filter((_, i) => i % 5 === 0).join(' | ')}`);
}

// Chat.
const chat = of('chat');
if (chat.length) {
  say(`\n== Chat: ${chat.length} lines (${chat.filter((c) => c.bot).length} by bots); rallies: ${chat.filter((c) => /^Gathering for a push/.test(c.text)).length} called, ${chat.filter((c) => /^Pushing the/.test(c.text)).length} pushes`);
  for (const c of chat.filter((_, i) => i % Math.max(1, Math.floor(chat.length / 8)) === 0).slice(0, 8)) say(`  ${min(c.t)} ${c.from}: ${c.text}`);
}

// Bots stuck: how often a bot reported being stuck (snapshots).
if (snaps.length) {
  const stuck = snaps.map((s) => s.bots.filter((b) => b.stuck >= 3).length);
  say(`\n== Stuck bots per snapshot: avg ${(stuck.reduce((a, b) => a + b, 0) / stuck.length).toFixed(1)}, max ${Math.max(...stuck)}`);
}

// Server resources.
const perf = of('perf');
if (perf.length) {
  const avg = (f) => (perf.reduce((n, p) => n + (f(p) || 0), 0) / perf.length).toFixed(0);
  say(`\n== Server: fps avg ${avg((p) => p.fps)}, cpu avg ${avg((p) => p.cpu)}%, rss avg ${avg((p) => p.rss)} MB, wasm ${perf.at(-1).wasmMb} MB`);
}
// Models compared (when bots had different ones, see bot-brain modelPlan):
// each planner and decider model's calls, latency, failures and cost, and
// how its bots did (kills, deaths).
const assigned = new Map(of('bot_models').map((x) => [x.bot, x]));
if (assigned.size) {
  say('\n== Models');
  // The score at the end of the first map (a map change resets it).
  const firstMap = snaps.filter((x) => x.map === snaps[0]?.map);
  const end = firstMap.at(-1);
  const score = (bots) => {
    let k = 0, d = 0;
    for (const p of end?.players || []) if (bots.has(p.name)) (k += p.kills), (d += p.deaths);
    return `${k} kills / ${d} deaths`;
  };
  const med = (xs) => (xs.length ? xs.sort((a, b) => a - b)[xs.length >> 1] : '-');
  const planners = [...new Set([...assigned.values()].map((x) => x.planner))];
  for (const model of planners) {
    const bots = new Set([...assigned.values()].filter((x) => x.planner === model).map((x) => x.bot));
    const own = calls.filter((c) => c.model === model);
    const fell = calls.filter((c) => c.first === model && c.model !== model);
    const errs = errors.filter((e) => e.model === model);
    const usd = own.reduce((n, c) => n + (c.usd ?? callUsd(c.model, c.usage)), 0);
    const prompt = own.reduce((n, c) => n + (c.usage?.prompt_tokens || 0), 0), cached = own.reduce((n, c) => n + (c.usage?.prompt_tokens_details?.cached_tokens || 0), 0);
    say(`planner ${model}: ${bots.size} bots, ${own.length} answers (${fell.length} of its bots' calls went elsewhere), errors ${errs.length} (${top(count(errs, (e) => (/3021|rate/i.test(e.error) ? 'rate limit' : /JSON/i.test(e.error) ? 'bad JSON' : /timeout|abort/i.test(e.error) ? 'timeout' : 'other')), 4)}), p50 ${med(own.map((c) => c.ms))} ms, ${own.length ? ((usd / own.length) * 1000).toFixed(3) : '-'} m$/answer, cached ${prompt ? Math.round((100 * cached) / prompt) : 0}%, output ${own.length ? Math.round(own.reduce((n, c) => n + (c.usage?.completion_tokens || 0), 0) / own.length) : '-'} tok; its bots: ${score(bots)}`);
  }
  const deciders = [...new Set([...assigned.values()].map((x) => x.decider))];
  const fin = ai?.models?.deciders || {};
  for (const model of deciders) {
    const bots = new Set([...assigned.values()].filter((x) => x.decider === model).map((x) => x.bot));
    const ds = decisions.filter((d) => d.model === model);
    const f = fin[model] || {};
    say(`decider ${model}: ${bots.size} bots, ${ds.length} decisions, p50 ${med(ds.map((d) => d.ms))} ms, failed ${f.failed ?? '?'}, $${f.usd ?? '?'} (${ds.length ? ((f.usd / ds.length) * 1000).toFixed(3) : '-'} m$/decision); its bots: ${score(bots)}`);
  }
}

const fatal = of('fatal');
if (fatal.length) say(`\n== FATAL: ${fatal[0].error.split('\n').slice(0, 6).join(' | ')}`);
console.log(out.join('\n'));
