// The bots' brain (run-server.mjs --bot-brain): decides for every bot on the
// server, outside the game. The engine streams the world 4 times a second and
// every chat line (bots_web.cpp, "brain mode"); the brain answers with bot_do
// console commands. Each bot has three layers:
//
//   planner   slow (seconds; on events): candidate plans, standing orders,
//             what to say. A language model later; scripted now.
//   decider   fast (every 250-500 ms): typed questions about the moment (which
//             plan, how to move, what to shoot, what to call out, rethink?)
//             answered with probabilities. A decision model (Clef) later,
//             scripted now; the questions are already in Clef's format.
//   engine    every frame: walking grid, aiming, shooting, doors.
//
// The planner and decider share a blackboard per bot; the decider reads it as
// part of its state, and can wake the planner (rethink).
import { appendFileSync } from 'node:fs';

const DECIDE_MS = [300, 500]; // per bot, jittered
const SAY_EVERY_MS = 6000; // a bot's callouts at most this often

// Names of things as players say them.
const BUILDING_NAMES = [
  [/barracks|hand of nod|hon/i, (t) => (t === 'GDI' ? 'Barracks' : 'Hand of Nod')],
  [/refinery|ref\b|tib/i, () => 'Refinery'],
  [/power|pp\b/i, () => 'Power Plant'],
  [/factory|weapons|wf\b|airstrip|air/i, (t) => (t === 'GDI' ? 'Weapons Factory' : 'Airstrip')],
  [/obelisk|obl/i, () => 'Obelisk'],
  [/guard|agt|advanced/i, () => 'Advanced Guard Tower'],
  [/silo/i, () => 'Silo'],
  [/repair/i, () => 'Repair Pad'],
  [/com|radar/i, () => 'Communications Center'],
];
const buildingName = (b) => {
  for (const [re, name] of BUILDING_NAMES) if (re.test(b.name)) return name(b.team);
  return b.name.replace(/^mp_|_/gi, ' ').trim();
};
const characterName = (preset) =>
  (preset || 'soldier').replace(/^CnC_|^mp_/i, '').replace(/^(GDI|Nod)_/i, '').replace(/_\d+$/, '').replace(/_/g, ' ').toLowerCase();
const dist = (a, b) => Math.hypot(a[0] - b[0], a[1] - b[1], (a[2] - b[2]) * 0.5);
const enemyOf = (team) => (team === 'GDI' ? 'Nod' : 'GDI');
const pct = (hp) => `${Math.round(hp * 100)}%`;

// Named places from the map's buildings: each building, each base, the middle.
function placesOf(world) {
  const places = [];
  const bases = {};
  for (const b of world.buildings) {
    if (b.team === 'none') continue;
    places.push({ name: `the ${b.team} ${buildingName(b)}`, pos: b.pos, building: b.id, team: b.team });
    (bases[b.team] ??= []).push(b.pos);
  }
  const centroid = (ps) => [0, 1, 2].map((i) => ps.reduce((s, p) => s + p[i], 0) / ps.length);
  for (const [team, ps] of Object.entries(bases)) places.push({ name: `the ${team} base`, pos: centroid(ps), team, base: true });
  if (bases.GDI && bases.Nod) places.push({ name: 'the middle', pos: centroid([centroid(bases.GDI), centroid(bases.Nod)]) });
  return places;
}
function placeOf(places, pos) {
  let best = null;
  for (const p of places) {
    const d = dist(p.pos, pos) - (p.base ? 40 : p.building ? 0 : 30);
    if (!best || d < best.d) best = { p, d };
  }
  if (!best) return 'somewhere';
  return best.d < 25 ? `${best.p.building ? 'at' : 'in'} ${best.p.name}` : `toward ${best.p.name}`;
}

// --- The scripted planner (a language model takes this seat later) ----------
// Plans are what the decider picks between, moment to moment.
function scriptedPlan(view) {
  const enemy = enemyOf(view.me.team);
  const targets = view.world.buildings.filter((b) => b.team === enemy && b.hp > 0);
  const own = view.world.buildings.filter((b) => b.team === view.me.team && b.hp > 0);
  const weakest = targets.sort((a, b) => a.hp - b.hp)[0];
  const hurt = own.filter((b) => b.hp < 0.9).sort((a, b) => a.hp - b.hp)[0];
  const plans = [];
  if (weakest) plans.push({ id: 'attack', text: `attack the enemy ${buildingName(weakest)} (${pct(weakest.hp)})`, building: weakest.id });
  if (own.length) {
    const b = hurt || own[Math.floor(Math.random() * own.length)];
    plans.push({ id: 'defend', text: `defend our ${buildingName(b)} (${pct(b.hp)})`, building: b.id });
  }
  plans.push({ id: 'hunt', text: 'hunt enemy soldiers in the middle of the map', place: 'the middle' });
  return { plans, orders: [] };
}

// --- The scripted decider (a decision model takes this seat later) ---------
// Answers the same typed questions the model will get, with probabilities.
function scriptedDecide(state, questions) {
  const one = (options, pick) => ({ choice: pick, probabilities: Object.fromEntries(options.map((o) => [o, o === pick ? 1 : 0])) });
  const answers = {};
  const me = state.me;
  const seen = state.enemies_in_sight;
  const baseAttacked = state.alerts.some((a) => a.kind === 'base_under_attack');
  for (const [id, q] of Object.entries(questions)) {
    const opts = Object.keys(q.criteria || {});
    if (id === 'plan') {
      const want = baseAttacked && opts.includes('defend') ? 'defend' : opts.includes(state.blackboard.current) ? state.blackboard.current : opts[0];
      answers[id] = one(opts, want);
    } else if (id === 'target') {
      const close = seen.filter((e) => e.distance < 70).sort((a, b) => a.distance - b.distance)[0];
      answers[id] = one(opts, close ? `enemy_${close.id}` : opts.includes('building') ? 'building' : 'none');
    } else if (id === 'move') {
      const pick = me.health < 0.25 && opts.includes('fall_back') ? 'fall_back' : seen.some((e) => e.distance < 25) && opts.includes('hold') ? 'hold' : 'advance';
      answers[id] = one(opts, pick);
    } else if (id === 'callout') {
      const pick = baseAttacked && opts.includes('base_under_attack') ? 'base_under_attack' : me.health < 0.3 && seen.length ? 'need_help' : seen.length >= 2 ? 'enemies_spotted' : 'none';
      answers[id] = one(opts, opts.includes(pick) ? pick : 'none');
    } else if (q.type === 'noul') {
      answers[id] = { probability: 0 };
    }
  }
  return answers;
}

// --- Clef-flash (Workers AI, through the match service's /ai/<match>) -------
// A call that fails or takes too long gets the scripted answers instead, so a
// bot never waits on the model.
const CLEF_TIMEOUT_MS = 1500;
function clefDecider(ai, stats) {
  return async (state, questions) => {
    const t0 = Date.now();
    try {
      const res = await fetch(ai.url, {
        method: 'POST',
        headers: { authorization: `Bearer ${ai.key}`, 'content-type': 'application/json' },
        body: JSON.stringify({ model: 'clef-flash', input: { state, questions } }),
        signal: AbortSignal.timeout(CLEF_TIMEOUT_MS),
      });
      const body = await res.json();
      if (!res.ok || !body.output?.answers) throw new Error(body.error || `HTTP ${res.status}`);
      stats.calls++;
      stats.ms.push(Date.now() - t0);
      stats.tokens += body.output.usage?.input_tokens || 0;
      const answers = {};
      for (const [id, a] of Object.entries(body.output.answers)) {
        answers[id] = a.type === 'noul' ? { probability: a.noul } : { choice: a.choice, probabilities: a.probabilities, confidence: a.confidence };
      }
      return answers;
    } catch (e) {
      stats.failed++;
      stats.lastError = String(e.message || e);
      return null;
    }
  };
}

const CALLOUTS = {
  enemies_spotted: (s) => (s.enemies_in_sight.length === 1 ? `Enemy ${s.enemies_in_sight[0].kind} ${s.enemies_in_sight[0].place}` : `${s.enemies_in_sight.length} enemies ${s.me.place}`),
  need_help: (s) => `Need help ${s.me.place}!`,
  base_under_attack: (s) => `${s.alerts.find((a) => a.kind === 'base_under_attack')?.what} under attack!`,
  on_my_way: (s) => `On my way to ${s.blackboard.planText}`,
};

export function createBrain({ mode = 'scripted', send, log, out = console.log, ai = null }) {
  let world = null;
  let places = [];
  let pending = false;
  const chat = []; // {t, from, team, text, to: 'all'|team}
  const bots = new Map(); // id -> blackboard and memory
  const record = (entry) => log && appendFileSync(log, JSON.stringify(entry) + '\n');
  const stats = { calls: 0, failed: 0, tokens: 0, ms: [], lastError: null };
  const model = mode === 'clef' && ai?.url ? clefDecider(ai, stats) : null;
  if (mode === 'clef' && !model) out('[brain] clef: no REBEL_AI_URL/REBEL_AI_KEY, deciding scripted');
  if (model) {
    setInterval(() => {
      const ms = stats.ms.splice(0).sort((a, b) => a - b);
      const q = (f) => ms[Math.min(ms.length - 1, Math.floor(ms.length * f))] ?? 0;
      out(`[brain] clef: ${stats.calls} calls, ${stats.failed} failed${stats.lastError ? ` (${stats.lastError})` : ''}, ${stats.tokens} tokens; latency p50 ${q(0.5)} ms, p90 ${q(0.9)} ms`);
      stats.lastError = null;
    }, 30000).unref();
  }

  function blackboard(id) {
    let b = bots.get(id);
    if (!b) {
      b = { plans: [], orders: [], current: null, planText: '', nextDecide: 0, nextPlan: 0, lastSay: 0, sent: {}, events: [], last: null };
      bots.set(id, b);
    }
    return b;
  }

  // What one bot knows right now: the decider's state.
  function view(bot, me) {
    const bb = blackboard(bot.id);
    const byObj = new Map(world.players.filter((p) => p.obj).map((p) => [p.obj, p]));
    const vehicles = new Map(world.vehicles.map((v) => [v.id, v]));
    const enemies = bot.sees.map((id) => {
      const p = byObj.get(id), v = vehicles.get(id);
      const pos = (p || v).pos;
      return { id, name: p ? p.name : null, kind: p ? characterName(p.char) : characterName(v.name), health: +(p || v).hp.toFixed(2), distance: Math.round(dist(me.pos, pos)), place: placeOf(places, pos), pos };
    });
    const alerts = world.buildings
      .filter((b) => b.team === me.team && b.hp > 0 && bb.lastHp?.[b.id] !== undefined && b.hp < bb.lastHp[b.id] - 0.005)
      .map((b) => ({ kind: 'base_under_attack', what: `Our ${buildingName(b)}`, health: pct(b.hp) }));
    bb.lastHp = Object.fromEntries(world.buildings.map((b) => [b.id, b.hp]));
    return {
      world,
      me: { id: me.id, name: me.name, team: me.team, health: +me.hp.toFixed(2), credits: me.credits, character: characterName(me.char), place: placeOf(places, me.pos), pos: me.pos, kills: me.kills, deaths: me.deaths },
      enemies_in_sight: enemies,
      teammates: world.players.filter((p) => p.team === me.team && p.id !== me.id && p.alive).map((p) => ({ name: p.name, place: placeOf(places, p.pos), distance: Math.round(dist(me.pos, p.pos)), health: +p.hp.toFixed(2) })),
      buildings: world.buildings.filter((b) => b.team !== 'none').map((b) => ({ side: b.team === me.team ? 'ours' : 'theirs', name: buildingName(b), health: pct(b.hp) })),
      alerts,
      chat: chat.filter((c) => c.to === 'all' || c.to === me.team).slice(-6).map((c) => `${c.from}${c.to === 'all' ? '' : ' (team)'}: ${c.text}`),
      events: bb.events.slice(-5),
      blackboard: { current: bb.current, planText: bb.planText, plans: bb.plans.map((p) => `${p.id}: ${p.text}`), orders: bb.orders },
      stuck: bot.stuck,
    };
  }

  // The moment's questions, in the decision model's format: every option is
  // something the bot can do right now.
  function questionsFor(state, bb, objective, all = false) {
    const plans = Object.fromEntries(bb.plans.map((p) => [p.id, p.text]));
    const target = { none: 'Shoot at nothing right now.' };
    for (const e of state.enemies_in_sight) target[`enemy_${e.id}`] = `Shoot the ${e.kind}${e.name ? ` (${e.name})` : ''}, ${e.distance} m away, health ${pct(e.health)}.`;
    if (objective?.building && objective.distance < 45 && objective.enemy) target.building = `Shoot the enemy ${objective.name}.`;
    // Only what is true now can be said.
    const callouts = { none: 'Say nothing.', on_my_way: 'Tell the team where you are heading.' };
    if (state.enemies_in_sight.length) callouts.enemies_spotted = 'Report the enemies you see.';
    if (state.enemies_in_sight.length && state.me.health < 0.6) callouts.need_help = 'Ask for help.';
    if (state.alerts.length) callouts.base_under_attack = 'Warn that our base is under attack.';
    const questions = {
      plan: { type: 'choice', instructions: 'Which of your plans fits the situation now?', criteria: plans },
      move: {
        type: 'choice',
        instructions: 'How do you move right now?',
        criteria: {
          advance: `Go on with the plan: move toward ${objective?.name || 'the objective'}.`,
          hold: 'Stand your ground and fight where you are.',
          fall_back: 'Fall back toward your own base.',
          regroup: 'Move to your nearest teammate.',
        },
      },
      target: { type: 'choice', instructions: 'What do you shoot at?', criteria: target },
      callout: { type: 'choice', instructions: 'Do you tell your team something now?', criteria: callouts },
      rethink: { type: 'noul', instructions: 'Has something happened that none of your plans covers?' },
    };
    // A choice needs two options at least (nothing in sight: no target question).
    if (!all) for (const [id, q] of Object.entries(questions)) if (q.type === 'choice' && Object.keys(q.criteria).length < 2) delete questions[id];
    return questions;
  }

  function objectiveOf(plan, me) {
    if (!plan) return null;
    if (plan.building) {
      const b = world.buildings.find((x) => x.id === plan.building);
      if (b) return { name: `${b.team === me.team ? 'our' : 'the enemy'} ${buildingName(b)}`, pos: b.pos, building: b.id, enemy: b.team !== me.team, distance: dist(me.pos, b.pos) };
    }
    const p = places.find((x) => x.name === plan.place);
    return p ? { name: p.name, pos: p.pos, distance: dist(me.pos, p.pos) } : null;
  }

  // Answers -> bot_do commands (only what changed).
  function act(bot, me, state, bb, answers, objective) {
    const cmd = (key, line, force) => {
      if (!force && bb.sent[key] === line) return;
      bb.sent[key] = line;
      send(`bot_do ${bot.id} ${line}`);
    };
    const plan = bb.plans.find((p) => p.id === answers.plan?.choice) || bb.plans[0];
    if (plan && plan.id !== bb.current) {
      bb.current = plan.id;
      bb.planText = plan.text;
    }
    // Where to.
    const move = answers.move?.choice || 'advance';
    let to = null;
    if (move === 'advance' && objective) to = objective.pos;
    if (move === 'fall_back') to = places.find((p) => p.base && p.team === me.team)?.pos;
    if (move === 'regroup') {
      const mate = world.players.filter((p) => p.team === me.team && p.id !== me.id && p.alive).sort((a, b) => dist(me.pos, a.pos) - dist(me.pos, b.pos))[0];
      to = mate?.pos;
    }
    if (move === 'hold' || !to) cmd('move', 'stop');
    else {
      // Re-send only for a new destination (5 m), or when the walk ended.
      const last = bb.goTo;
      if (!last || dist(last, to) > 5 || (bot.waypoint >= bot.path && dist(me.pos, to) > 8)) {
        bb.goTo = to;
        cmd('move', `go ${to.map((v) => v.toFixed(1)).join(' ')}`, true);
      }
    }
    // What to shoot.
    const t = answers.target?.choice || 'none';
    if (t.startsWith('enemy_')) cmd('fire', `target ${t.slice(6)}`);
    else if (t === 'building' && objective) cmd('fire', `shoot ${objective.pos[0].toFixed(1)} ${objective.pos[1].toFixed(1)} ${(objective.pos[2] + 2).toFixed(1)}`);
    else cmd('fire', 'hold_fire');
    // What to say.
    const c = answers.callout?.choice;
    const now = Date.now();
    if (c && c !== 'none' && CALLOUTS[c] && now - bb.lastSay > SAY_EVERY_MS && bb.lastCallout !== c) {
      bb.lastSay = now;
      bb.lastCallout = c;
      cmd('say', `say team ${CALLOUTS[c](state)}`, true);
    } else if (c === 'none' && now - bb.lastSay > 30000) bb.lastCallout = null;
  }

  async function think(bot, me, now) {
    const bb = blackboard(bot.id);
    // Life events, from the scoreboard.
    if (bb.last) {
      if (me.deaths > bb.last.deaths) bb.events.push(`you died ${placeOf(places, bb.last.pos)}`);
      if (me.kills > bb.last.kills) bb.events.push('you killed an enemy');
    }
    const wasAlive = bb.last?.alive;
    bb.last = { kills: me.kills, deaths: me.deaths, pos: me.pos, alive: me.alive };
    if (!me.alive) return;
    const state = view(bot, me);
    // Planner: on spawn, on rethink, or every 20 s.
    if (!bb.plans.length || !wasAlive || bb.wantPlan || now >= bb.nextPlan) {
      const p = scriptedPlan(state);
      bb.plans = p.plans;
      bb.orders = p.orders;
      bb.nextPlan = now + 20000;
      bb.wantPlan = false;
      state.blackboard.plans = bb.plans.map((x) => `${x.id}: ${x.text}`);
      record({ t: now, bot: me.name, planner: p });
    }
    const plan = bb.plans.find((p) => p.id === bb.current) || bb.plans[0];
    const objective = objectiveOf(plan, me);
    const questions = questionsFor(state, bb, objective);
    const { world: _w, ...decisionState } = state;
    const t0 = Date.now();
    const modelState = JSON.parse(JSON.stringify(decisionState, (k, v) => (k === 'pos' || k === 'id' ? undefined : v)));
    const decided = model ? await model(modelState, questions) : null;
    const by = decided ? 'clef' : 'scripted';
    // Questions left out (one option) default to the scripted answer.
    const answers = { ...scriptedDecide(state, questionsFor(state, bb, objective, true)), ...decided };
    const ms = Date.now() - t0;
    if (!bb.alive?.()) return; // the world moved on (bot gone)
    if ((answers.rethink?.probability || 0) > 0.7) bb.wantPlan = true;
    act(bot, me, state, bb, answers, objectiveOf(bb.plans.find((p) => p.id === answers.plan?.choice) || plan, me));
    record({ t: now, bot: me.name, by, ms, state: modelState, questions: Object.fromEntries(Object.entries(questions).map(([k, q]) => [k, Object.keys(q.criteria || { yes: 1 })])), answers: Object.fromEntries(Object.entries(answers).map(([k, a]) => [k, a.probabilities ?? a.probability])) });
  }

  function tick() {
    pending = false;
    if (!world) return;
    const now = Date.now();
    places = placesOf(world);
    const players = new Map(world.players.map((p) => [p.id, p]));
    for (const bot of world.bots) {
      const me = players.get(bot.id);
      const bb = blackboard(bot.id);
      if (!me || now < bb.nextDecide || bb.busy) continue;
      bb.nextDecide = now + DECIDE_MS[0] + Math.random() * (DECIDE_MS[1] - DECIDE_MS[0]);
      bb.busy = true;
      bb.alive = () => bots.get(bot.id) === bb;
      // Bots think in parallel; each waits only for its own decision.
      think(bot, me, now)
        .catch((e) => out(`[brain] ${me.name}: ${e.stack || e}`))
        .finally(() => (bb.busy = false));
    }
    for (const id of bots.keys()) if (!players.has(id)) bots.delete(id);
  }

  out(`[brain] ${mode} brain for the bots${log ? `, decisions logged to ${log}` : ''}`);
  return {
    onState(json) {
      world = JSON.parse(json);
      if (!pending) {
        pending = true;
        setImmediate(tick); // outside the game's frame
      }
    },
    onChat(sender, type, text) {
      const from = world?.players.find((p) => p.id === sender);
      chat.push({ t: Date.now(), from: from ? from.name : 'Host', to: type === 1 && from ? from.team : 'all', text });
      out(`[chat] ${chat.at(-1).from}${type === 1 ? ' (team)' : ''}: ${text}`);
      if (chat.length > 50) chat.shift();
      // Named in chat: the bot rethinks.
      for (const p of world?.players || []) if (p.bot && text.toLowerCase().includes(p.name.replace(/^\[BOT\]\s*/, '').split(' ').pop().toLowerCase())) blackboard(p.id).wantPlan = true;
    },
  };
}

