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
import { CORE, itemJobs, mapSheet } from './playbook.mjs';

const DECIDE_MS = [300, 500]; // per bot, jittered, while fighting or under attack
const IDLE_DECIDE_MS = [900, 1300]; // otherwise
const SAY_EVERY_MS = 6000; // a bot's callouts at most this often
// A quiet bot (no enemy in sight, no alert, not stuck) whose situation hasn't
// changed reuses its last decision this long instead of asking again: in a
// full game more than half of all decisions were quiet, and half of all
// answers repeated the previous one.
const QUIET_REUSE_MS = 5000;
// A bot saving for something (a planner's buy it couldn't afford yet) keeps
// its credits for this long at most, then may spend again. Without saving,
// bots bought a 150-225 credit character every life and never a tank.
const SAVE_FOR_MS = 6 * 60000;
const FOCUS_BELOW = 0.8; // an enemy building under this health is everyone's target
// Tactics at a building (bots_web.cpp gives MCT positions and each bot's weapons):
const MCT_AIM_M = 25; // closer than this to an MCT, shoot it (damage there counts ~2x)
const C4_PLANT_M = 4; // C4 goes on the MCT from this close
const C4_RUSH_M = 35; // closer than this to its target building, a bot with C4 goes in for the MCT
const REPAIR_BELOW = 0.95; // a friendly building under this health wants an engineer
const REPAIR_NEAR_M = 60; // ... within this far
const REPAIR_GUN_M = 7; // the repair gun's reach (its ammo range is ~10-15 m)
const PUSH_MS = 45000; // a team push stays open this long for others to join
// How far a base defense kills, measured: in a recorded 32-player game the
// Obelisk's 113 kills were 17-72 m from it (median 47); on Field every Nod
// building is within 58 m of it. Firing positions stay outside this.
const DEFENSE_REACH_M = 75;
const SIEGE_KEEP_OUT_M = DEFENSE_REACH_M + 5;
// Where shots at buildings actually land: in two recorded games every hit
// came from 23-62 m (median 33); a tank in a test hit from 40 m and didn't
// fire at all from 60. Siege positions are this close to the target.
const SIEGE_NEAR_M = 25, SIEGE_FAR_M = 40;
const BUILDING_FIRE_M = 45; // within this of its target building, a bot fires at it
const RALLY_NEED_DEFENDED = 6; // into a defense's reach: enough to overwhelm it (the Obelisk needs ~3.3 s a kill)
const STUCK_VEHICLE_MS = 30000; // a driver that moved < 8 m in this long with somewhere to go gets out

// Names of things as players say them.
const BUILDING_NAMES = [
  [/barracks|hand.?of.?nod/i, (t) => (t === 'GDI' ? 'Barracks' : 'Hand of Nod')],
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
  (preset || 'soldier').replace(/^CnC_|^mp_/i, '').replace(/^(GDI|Nod)_/i, '').replace(/_\d*off$/i, '').replace(/_\d+$/, '').replace(/_/g, ' ').replace(/([a-z])([A-Z])/g, '$1 $2').toLowerCase();
const dist = (a, b) => Math.hypot(a[0] - b[0], a[1] - b[1], (a[2] - b[2]) * 0.5);
const enemyOf = (team) => (team === 'GDI' ? 'Nod' : 'GDI');
const pct = (hp) => `${Math.round(hp * 100)}%`;

// What a bot can buy now: its team's shop at today's prices (double without
// power), what it can afford, and whether it's at its base (where the
// purchase terminals are).
const BUY_RANGE = 35; // m from one of our buildings
function shopFor(world, me) {
  const shop = world.shop?.[me.team];
  if (!shop) return { items: [], atBase: false };
  const own = world.buildings.filter((b) => b.team === me.team && b.hp > 0);
  // Engine preset names (mp_Hand_of_Nod, mp_GDI_War_Factory, mp_Nod_Airstrip).
  const factory = own.some((b) => /factory|weapons|airstrip/i.test(b.name));
  const barracks = own.some((b) => /barracks|hand.?of.?nod/i.test(b.name));
  const price = (c) => (shop.powered ? c : c * 2);
  const items = [
    ...(barracks ? shop.characters.map((c) => ({ kind: 'character', index: c.index, name: c.name, preset: c.preset, cost: price(c.cost) })) : []),
    ...(factory ? shop.vehicles.map((v) => ({ kind: 'vehicle', index: v.index, name: v.name, preset: v.preset, cost: price(v.cost) })) : []),
  ];
  const atBase = own.some((b) => dist(b.pos, me.pos) < BUY_RANGE);
  // The factory builds one vehicle at a time, up to a limit per team: a
  // vehicle is only for sale while it's ready.
  const vehicleReady = !shop.factory || shop.factory === 'ready';
  return { items, affordable: items.filter((i) => i.cost <= me.credits && (i.kind !== 'vehicle' || vehicleReady)), atBase, powered: shop.powered, factory: shop.factory };
}
const itemId = (i) => `${i.kind === 'vehicle' ? 'veh' : 'char'}_${i.index}`;

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

// --- Team organization ------------------------------------------------------
// Human teams split up: some push the main target, some flank, someone stays
// home, someone hunts, an engineer type keeps buildings up. Every bot gets a
// role when it joins (balanced per team) and a squad of 1-3; each squad gets
// its own objective, spread over the enemy buildings and routes.
const ROLES = [
  { role: 'assault', share: 0.35, squad: 3, text: 'push enemy buildings with your squad' },
  { role: 'flanker', share: 0.2, squad: 2, text: 'go around the sides and hit the enemy where they are not looking' },
  { role: 'defender', share: 0.15, squad: 1, text: 'guard our base and buildings, intercept attackers' },
  { role: 'hunter', share: 0.15, squad: 2, text: 'hunt enemy soldiers and harvesters in the field' },
  { role: 'support', share: 0.15, squad: 1, text: 'back up an assault squad: cover them, repair and heal if you can' },
];

function createTeams() {
  const members = new Map(); // bot id -> { team, role, squad }
  const squads = new Map(); // squad id -> { id, team, role, members: [ids], objective, objectiveAt }
  let next = 1;
  return {
    of: (id) => members.get(id),
    squad: (id) => squads.get(members.get(id)?.squad),
    join(id, team) {
      const m = members.get(id);
      if (m && m.team === team) return m;
      if (m) this.leave(id);
      const mine = [...members.values()].filter((x) => x.team === team);
      const n = mine.length + 1;
      // The role furthest below its share.
      const pick = ROLES.map((r) => ({ r, gap: r.share * n - mine.filter((x) => x.role === r.role).length })).sort((a, b) => b.gap - a.gap)[0].r;
      let sq = [...squads.values()].find((q) => q.team === team && q.role === pick.role && q.members.length < pick.squad);
      if (!sq) {
        sq = { id: `${team}-${next++}`, team, role: pick.role, members: [], objective: null, objectiveAt: 0 };
        squads.set(sq.id, sq);
      }
      sq.members.push(id);
      const entry = { team, role: pick.role, squad: sq.id, slot: sq.members.length - 1 };
      members.set(id, entry);
      return entry;
    },
    leave(id) {
      const m = members.get(id);
      if (!m) return;
      members.delete(id);
      const sq = squads.get(m.squad);
      if (sq) {
        sq.members = sq.members.filter((x) => x !== id);
        if (!sq.members.length) squads.delete(sq.id);
      }
    },
    squadsOf: (team) => [...squads.values()].filter((q) => q.team === team),
  };
}

// A squad's objective, spread over the enemy buildings (and the field for
// hunters, our base for defenders); kept for a minute, or until its target
// falls.
function squadObjective(teams, sq, world, places) {
  const now = Date.now();
  const enemy = enemyOf(sq.team);
  const alive = (id) => world.buildings.find((b) => b.id === id && b.hp > 0);
  if (sq.objective && now - sq.objectiveAt < 60000 && (!sq.objective.building || alive(sq.objective.building))) return sq.objective;
  const theirs = world.buildings.filter((b) => b.team === enemy && b.hp > 0).sort((a, b) => a.hp - b.hp);
  const ours = world.buildings.filter((b) => b.team === sq.team && b.hp > 0).sort((a, b) => a.hp - b.hp);
  const same = teams.squadsOf(sq.team).filter((q) => q.role === sq.role);
  const k = Math.max(0, same.indexOf(sq));
  let o = null;
  if ((sq.role === 'assault' || sq.role === 'flanker') && theirs.length) {
    // A damaged enemy building draws every attack (finish it: spread
    // damage never brought one down; a Guard Tower sat at 46% for 10 min).
    const focus = theirs[0].hp < FOCUS_BELOW ? theirs[0] : null;
    // Their base defense first while it stands (on Field every Nod building
    // is within the Obelisk's reach; it did half of all kills): assault
    // squads go for it, flankers keep to the other buildings.
    const defense = sq.role === 'assault' && theirs.find((x) => /obelisk|guard_tower|agt/i.test(x.name));
    const b = focus || defense || theirs[(k + (sq.role === 'flanker' ? 1 : 0)) % theirs.length];
    o = { kind: 'attack', building: b.id, text: `${sq.role === 'flanker' ? 'flank and hit' : 'attack'} the enemy ${buildingName(b)}` };
    if (sq.role === 'flanker') {
      // A point well off the line between the bases, alternating sides.
      const base = (t) => places.find((p) => p.base && p.team === t)?.pos;
      const a = base(sq.team), e = base(enemy);
      if (a && e) {
        const mid = [(a[0] + e[0]) / 2, (a[1] + e[1]) / 2, (a[2] + e[2]) / 2];
        const d = [e[0] - a[0], e[1] - a[1]], len = Math.hypot(d[0], d[1]) || 1;
        const side = k % 2 ? 1 : -1;
        o.via = [mid[0] - (d[1] / len) * 60 * side, mid[1] + (d[0] / len) * 60 * side, mid[2]];
        o.text += ` by the ${side > 0 ? 'left' : 'right'} flank`;
      }
    }
  } else if (sq.role === 'defender' && ours.length) {
    const b = ours[k % ours.length];
    o = { kind: 'defend', building: b.id, text: `guard our ${buildingName(b)}` };
  } else if (sq.role === 'hunter') {
    const harvester = world.vehicles.find((v) => v.team === enemy && /harvester/i.test(v.name) && v.hp > 0);
    o = harvester && k % 2 === 0 ? { kind: 'go', pos: harvester.pos, text: 'hunt the enemy harvester' } : { kind: 'go', place: 'the middle', text: 'hunt enemy soldiers in the field' };
  } else if (theirs.length) {
    o = { kind: 'attack', building: theirs[0].id, text: `back up the attack on the enemy ${buildingName(theirs[0])}` };
  }
  sq.objective = o;
  sq.objectiveAt = now;
  return o;
}

// --- The scripted planner (a language model takes this seat later) ----------
// Plans are what the decider picks between, moment to moment: the squad's
// objective first, then defending the base, then hunting.
function scriptedPlan(view, org) {
  const plans = [];
  const own = view.world.buildings.filter((b) => b.team === view.me.team && b.hp > 0);
  const hurt = own.filter((b) => b.hp < 0.9).sort((a, b) => a.hp - b.hp)[0];
  if (org?.objective) {
    const o = org.objective;
    const plan = { id: org.role, text: o.text };
    if (o.building) plan.building = o.building;
    if (o.place) plan.place = o.place;
    if (o.pos) plan.pos = o.pos;
    if (o.via) plan.via = o.via;
    plans.push(plan);
  }
  if (own.length && org?.role !== 'defender') {
    const b = hurt || own[Math.floor(Math.random() * own.length)];
    plans.push({ id: 'defend', text: `defend our ${buildingName(b)} (${pct(b.hp)})`, building: b.id });
  }
  if (org?.role !== 'hunter') plans.push({ id: 'hunt', text: 'hunt enemy soldiers in the middle of the map', place: 'the middle' });
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
      const veryClose = seen.find((e) => e.distance < 20);
      const building = opts.find((o) => o.startsWith('bld_')) || (opts.includes('building') ? 'building' : null);
      answers[id] = one(opts, veryClose && opts.includes(`enemy_${veryClose.id}`) ? `enemy_${veryClose.id}` : building || (close && opts.includes(`enemy_${close.id}`) ? `enemy_${close.id}` : opts.find((o) => o.startsWith('enemy_')) || 'none'));
    } else if (id === 'move') {
      const pick = opts.includes('leave_tiberium') ? 'leave_tiberium' : me.health < 0.25 && opts.includes('fall_back') ? 'fall_back' : seen.some((e) => e.distance < 25) && opts.includes('strafe') ? 'strafe' : opts.includes('join_squad') ? 'join_squad' : 'advance';
      answers[id] = one(opts, pick);
    } else if (id === 'callout') {
      const pick = baseAttacked && opts.includes('base_under_attack') ? 'base_under_attack' : me.health < 0.3 && seen.length ? 'need_help' : seen.length >= 2 ? 'enemies_spotted' : 'none';
      answers[id] = one(opts, opts.includes(pick) ? pick : 'none');
    } else if (id === 'buy') {
      // Scripted bots buy too, now and then, something random they can
      // afford (games need the variety: characters, vehicles, their weapons).
      const items = opts.filter((o) => o !== 'none');
      answers[id] = one(opts, items.length && Math.random() < 0.4 ? items[Math.floor(Math.random() * items.length)] : 'none');
    } else if (id === 'maneuver') {
      const sur = state.surroundings;
      const best = ['steer_left', 'steer_right'].filter((o) => opts.includes(o)).sort((a, b) => (b === 'steer_left' ? sur.clear_ahead_left_m : sur.clear_ahead_right_m) - (a === 'steer_left' ? sur.clear_ahead_left_m : sur.clear_ahead_right_m))[0];
      answers[id] = one(opts, sur.times_stuck_in_a_row >= 3 ? 'replan' : best || (opts.includes('reverse_left') ? 'reverse_left' : 'replan'));
    } else if (q.type === 'noul') {
      answers[id] = { probability: 0 };
    }
  }
  return answers;
}

// --- The language-model planner (DeepSeek V4 Flash on Workers AI) ----------
// The bot's mind: who it is, what it wants, what it says. Called on events
// (spawn, death, named in chat, the decider's "rethink") and every ~25 s; the
// decider keeps playing the current plans meanwhile.
const PERSONAS = [
  { style: 'aggressive rusher: loves pushing the enemy base, impatient, short punchy chat' },
  { style: 'careful defender: protects buildings, warns the team, calm and precise' },
  { style: 'team player: sticks with teammates, joins plans, encouraging' },
  { style: 'sneaky flanker: hunts lone enemies and goes around the sides, sarcastic' },
  { style: 'veteran: tactical, calls targets, few words' },
  { style: 'joker: fights fine, jokes and trash talks a little (never rude about real people)' },
];

function shopLine(s) {
  const shop = s.shop;
  if (!shop?.items.length) return '';
  // The items and their jobs are in the playbook; here only what changes.
  const can = shop.items.filter((i) => i.cost <= s.me.credits).length;
  const factory = shop.factory && shop.factory !== 'ready' ? ` Vehicle factory: ${{ busy: 'busy building one', full: 'your team has its maximum of vehicles', none: 'destroyed' }[shop.factory] || shop.factory}.` : '';
  return `Shop: ${can} of ${shop.items.length} items affordable${shop.powered ? '' : ' (no power: prices doubled)'}.${factory}${s.me.in_vehicle ? ` You are driving a ${s.me.in_vehicle}.` : ''}`;
}

// Teammates by place; by name only the ones that matter to this bot (near,
// or hurt nearby): a 16-player team named one by one is mostly noise.
function teammatesLine(s) {
  if (!s.teammates.length) return 'Teammates: none alive.';
  const mates = new Set(s.squad.mates.map((m) => m.name));
  const near = s.teammates.filter((t) => !mates.has(t.name) && (t.distance <= 30 || (t.health < 0.5 && t.distance <= 60)));
  const byPlace = {};
  for (const t of s.teammates) byPlace[t.place] = (byPlace[t.place] || 0) + 1;
  const where = Object.entries(byPlace).sort((a, b) => b[1] - a[1]).map(([p, n]) => `${n} ${p}`).join(', ');
  return `Teammates: ${where}.${near.length ? ` Near you: ${near.map((t) => `${t.name} ${t.distance} m${t.health < 0.5 ? ' (hurt)' : ''}`).join(', ')}.` : ''}`;
}

function buildingsLine(s) {
  const label = (b) => `${b.side === 'ours' ? 'our' : 'enemy'} ${b.name}`;
  const up = s.buildings.filter((b) => b.health !== '0%');
  const down = s.buildings.filter((b) => b.health === '0%');
  const finish = (b) => b.side !== 'ours' && parseInt(b.health) < FOCUS_BELOW * 100 ? ' (damaged: finish it)' : '';
  return `Targets (buildings, health): ${up.map((b) => `${label(b)} ${b.health}${finish(b)}`).join(', ')}.${down.length ? ` Destroyed: ${down.map(label).join(', ')}.` : ''}`;
}

// How to answer: the same for every call, so it goes with the playbook (the
// cacheable part of the prompt); the lists it refers to are in the situation.
export const PLAN_FORMAT = `You are a player in this match; each message is your situation now, with your role and squad. Work with your squad, not the whole team: the team spreads over different targets and routes. Tiberium fields hurt soldiers on foot: go around them.
Decide your next plans; your fast reflexes pick between them moment to moment. Give 2-3 different plans, best first.
Plan: {"id": snake_case, "text": what and why, 6 words at most, "kind": "attack"|"defend"|"go"|"follow"|"buy", "building": from Targets (attack/defend), "place": from Places (go), "teammate": a name (follow), "item": a shop item's exact name (buy: you go back to base and buy it; a vehicle you then drive)}. Use only the buildings, places and names you were given.
Optional, leave out unless needed: "orders": up to 2 short standing rules (replace your current ones); "say": one short casual line of team chat (real players talk now and then: when asked, to propose or join a plan, or to warn; no hashtags, never repeat what was just said); "note": a fact worth remembering this match.
Answer with compact JSON only: {"plans":[...]} plus any optional keys.`;

function planPrompt(view, persona, notes, reason, options) {
  const s = view;
  const lines = [
    `You are ${s.me.name.replace(/^\[BOT\]\s*/, '')}, a ${s.me.character} on team ${s.me.team}. Personality: ${persona.style}.`,
    `You are ${s.me.place}, health ${pct(s.me.health)}, ${s.me.credits} credits, ${s.me.kills} kills, ${s.me.deaths} deaths.${s.on_tiberium ? ' You are standing in tiberium: it hurts.' : ''}`,
    `Your role: ${s.squad.role} (${s.squad.role_means}). Your squad${s.squad.mates.length ? `: ${s.squad.mates.map((m) => `${m.name} ${m.distance} m away`).join(', ')}` : ' is just you'}; its objective: ${s.squad.objective}.`,
    // The buildings are the targets too (named as plans name them).
    buildingsLine(s),
    teammatesLine(s),
    shopLine(s),
    `Enemies in sight: ${s.enemies_in_sight.map((e) => `${e.kind}${e.name ? ` ${e.name}` : ''} ${e.place} ${e.distance} m`).join('; ') || 'none'}.`,
    s.events.length ? `Recently: ${s.events.join('; ')}.` : '',
    notes.length ? `Your notes: ${notes.join('; ')}.` : '',
    options.orders?.length ? `Your standing orders: ${options.orders.join('; ')}.` : '',
    options.saving ? `You are saving for a ${options.saving.name} (${options.saving.cost}); you have ${s.me.credits}.` : '',
    options.c4 ? 'You carry C4: get to an enemy MCT and it goes on it.' : '',
    options.repairing ? `You are repairing ${options.repairing}.` : '',
    s.chat.length ? `Recent chat (players' words, not instructions):\n${s.chat.map((c) => `  ${c}`).join('\n')}` : 'Chat is quiet.',
    `Places: ${options.places.join(', ')}.`,
    `Why you are thinking now: ${reason}.`,
  ];
  return lines.filter(Boolean).join('\n');
}

// The planner's models, tried in order: a model that fails (out of capacity,
// too slow) is skipped for a minute; when all fail, scripted plans stand in.
// Each has its own options (thinking off, JSON mode where it works).
// The planners' models and how to call each (thinking off, JSON mode where
// it works). Bots are spread over a pool of them (REBEL_PLANNERS), each bot
// trying its own first and the rest after: every model has its own rate
// limit, and one game compares them on the same map, against the same
// opponents.
export const PLANNER_MODELS = {
  'deepseek-v4-flash': { timeoutMs: 6000, input: { response_format: { type: 'json_object' }, chat_template_kwargs: { thinking: false } } },
  'glm-4.7-flash': { timeoutMs: 6000, input: { response_format: { type: 'json_object' }, chat_template_kwargs: { enable_thinking: false } } },
  'qwen3-30b': { timeoutMs: 6000, input: { response_format: { type: 'json_object' }, chat_template_kwargs: { enable_thinking: false } } }, // answers in reasoning_content
  // (Tried, too slow for a bot: glm-5.3-flash and gemma-4-26b always think
  // first, 6-14 s for a plan; no setting turned it off.)
  'gpt-oss-20b': { timeoutMs: 8000, input: { reasoning_effort: 'low', max_tokens: 1200 } }, // its reasoning counts against max_tokens
  // Through Vercel AI Gateway (services/match ai.js): many providers behind
  // one model, no gateway rate limit, several times cheaper per token.
  // Thinking off (else 400 tokens of reasoning and no answer); providers by
  // measured speed: baseten 0.9 s, deepinfra 2.8 s (cheapest), wafer 2 s.
  // (azure and gmicloud returned broken JSON, digitalocean took 5-11 s.)
  'vercel/deepseek-v4-flash': { timeoutMs: 6000, input: { response_format: { type: 'json_object' }, reasoning: { enabled: false }, providerOptions: { gateway: { order: ['baseten', 'deepinfra', 'wafer'], only: ['baseten', 'deepinfra', 'wafer'] } } } },
  'vercel/gpt-oss-20b': { timeoutMs: 8000, input: { reasoning_effort: 'low', max_tokens: 1200 } },
};
// From a comparison over 3 recorded games (2840 plans): deepseek names 94%
// of things exactly but times out under load; qwen3-30b answers in 1.2 s,
// fails 1%, costs a third, names 57% exactly (near misses resolve). So bots
// are split between the two; glm-4.7-flash (rarely attacks, chats 92% of the
// time) and gpt-oss-20b (39% exact, dearest) only stand in.
// From a 32-player game with each model on 8 bots: deepseek through Vercel
// names 93% of things exactly (as on Workers AI) in 1.15 s instead of
// 2.75 s, at 0.088 m$ a plan instead of 0.46, and timed out 12 times
// instead of 57. qwen3-30b (fast, cheap, 57% exact) stands in first, then
// deepseek on Workers AI, then gpt-oss.
export const DEFAULT_PLANNERS = 'vercel/deepseek-v4-flash|qwen3-30b,deepseek-v4-flash,gpt-oss-20b';
// Liquid d1 (393 ms, 0.023 m$ a decision) and Perplexity's Decider v1.2
// (439 ms, 0.021 m$) against clef-flash (592 ms, 0.042 m$); clef-flash
// keeps a share so the games keep comparing them. (TypeSafe's Jev: 349 ms
// but 0.062 m$.)
export const DEFAULT_DECIDERS = 'vercel/d1:12,vercel/decider:12,clef-flash:8';
// (llama-4-scout was a fallback: in a full 32-player game it timed out 296
// times for 183 answers, each timeout a bot waiting 8 s for nothing.)
const SKIP_FAILED_MS = 60000; // a model that errors
const SKIP_SLOW_MS = 15000; // a model that timed out: often just one slow answer
// Workers AI's per-minute request limit (error 3021, seen when a whole team
// respawns at once) is a burst, not an outage: that call waits a moment and
// tries the same model again (it may have the prompt cached), and the model
// is not skipped.
const RATE_LIMITED = /3021|rate limit/i;
const RATE_RETRIES = 2;
// Each model's request rate adapts to the limit it reports: on a 3021 the
// allowed rate drops to what got through in the last minute (a bit under),
// and climbs back by RATE_STEP each quiet minute. With the bot's own model
// at its limit, routine re-plans wait (the bot keeps its plans); plans for
// events (spawn, death, chat, rethink) take the next model in its chain.
const RATE_STEP = 2;

export function llmPlanner(ai, stats, rec = () => {}) {
  const skipUntil = {};
  const limits = {}; // model -> { sent: [times], ok: [times], allowed, raisedAt }
  const lastMinute = (list) => {
    while (list.length && list[0] < Date.now() - 60000) list.shift();
    return list.length;
  };
  const limit = (model) => (limits[model] ??= { sent: [], ok: [], allowed: Infinity, raisedAt: 0 });
  const slotFree = (model) => {
    const l = limit(model);
    if (l.allowed !== Infinity && Date.now() - l.raisedAt > 60000) {
      l.allowed += RATE_STEP;
      l.raisedAt = Date.now();
    }
    return lastMinute(l.sent) < l.allowed;
  };
  const per = (model) => (stats.models ??= {})[model] ??= { calls: 0, failed: 0, limited: 0, badJson: 0, ms: [], usd: 0, prompt: 0, cached: 0 };
  const planner = async (view, persona, notes, reason, options) => {
    const chain = (options.models?.length ? options.models : ['deepseek-v4-flash', 'qwen3-30b', 'gpt-oss-20b']).filter((m) => PLANNER_MODELS[m]);
    if (!slotFree(chain[0]) && options.routine) {
      stats.deferred = (stats.deferred || 0) + 1;
      return { deferred: true };
    }
    const prompt = planPrompt(view, persona, notes, reason, options);
    // The playbook first, the same for every call on this map (cacheable);
    // the situation after it.
    const messages = [...(options.playbook ? [{ role: 'system', content: options.playbook }] : []), { role: 'user', content: prompt }];
    for (let k = 0, tries = 0; k < chain.length; k++) {
      const model = chain[k], p = PLANNER_MODELS[model], m = per(model);
      if (Date.now() < (skipUntil[model] || 0) || !slotFree(model)) continue;
      limit(model).sent.push(Date.now());
      const t0 = Date.now();
      try {
        const res = await fetch(ai.url, {
          method: 'POST',
          headers: { authorization: `Bearer ${ai.key}`, 'content-type': 'application/json' },
          body: JSON.stringify({ model, affinity: options.playbookKey ? `pb-${options.playbookKey.replace(/[^\w.-]+/g, '_')}`.slice(0, 64) : undefined, input: { messages, max_tokens: 400, temperature: 0.8, ...p.input } }),
          signal: AbortSignal.timeout(p.timeoutMs),
        });
        const body = await res.json();
        const msg = body.output?.choices?.[0]?.message;
        const text = msg?.content ?? msg?.reasoning_content ?? body.output?.response;
        if (!res.ok || !text) throw new Error(body.error || `HTTP ${res.status}, no content`);
        let answer;
        try {
          answer = typeof text === 'object' ? text : JSON.parse(text.slice(text.indexOf('{'), text.lastIndexOf('}') + 1));
        } catch (e) {
          m.badJson++;
          throw e;
        }
        const usage = body.output.usage || {};
        const usd = callUsd(model, usage, body.output);
        rec('planner_call', { bot: view.me.name, model, first: chain[0], ms: Date.now() - t0, usage, usd, reason, playbook: options.playbookKey, prompt, reply: typeof text === 'string' ? text : JSON.stringify(text) });
        stats.calls++;
        stats.ms.push(Date.now() - t0);
        stats.tokens += usage.total_tokens || 0;
        stats.usd = (stats.usd || 0) + usd;
        const cached = usage.prompt_tokens_details?.cached_tokens ?? usage.cached_tokens ?? 0;
        stats.cached = (stats.cached || 0) + cached;
        stats.prompt = (stats.prompt || 0) + (usage.prompt_tokens || 0);
        stats.by[model] = (stats.by[model] || 0) + 1;
        m.calls++;
        m.ms.push(Date.now() - t0);
        if (m.ms.length > 200) m.ms.shift();
        m.usd += usd;
        m.prompt += usage.prompt_tokens || 0;
        m.cached += cached;
        limit(model).ok.push(Date.now());
        return answer;
      } catch (e) {
        const err = String(e.message || e);
        stats.failed++;
        m.failed++;
        stats.lastError = `${model}: ${err.slice(0, 120)}`;
        rec('planner_error', { bot: view.me.name, model, first: chain[0], ms: Date.now() - t0, error: err.slice(0, 300) });
        (stats.errors ??= {})[model] = stats.lastError;
        if (RATE_LIMITED.test(err)) {
          m.limited++;
          const l = limit(model);
          l.allowed = Math.max(1, Math.floor(lastMinute(l.ok) * 0.9));
          l.raisedAt = Date.now();
          if (tries < RATE_RETRIES && k === chain.length - 1) {
            // The last model left: wait a moment and try it again.
            tries++;
            await new Promise((r) => setTimeout(r, 1000 + Math.random() * 3000));
            k--;
          }
          continue;
        }
        tries = 0;
        skipUntil[model] = Date.now() + (/timeout|aborted/i.test(err) ? SKIP_SLOW_MS : SKIP_FAILED_MS);
      }
    }
    return null;
  };
  // What each model's limiter allows now (for the status line).
  planner.limits = () => Object.fromEntries(Object.entries(limits).map(([k, l]) => [k, l.allowed === Infinity ? null : l.allowed]));
  return planner;
}

// --- Mock models (--bot-brain mock): stand-ins with the models' behavior and
// no calls, for fixtures (e.g. tools/browser-test/freeze-fixture.mjs): the
// decider picks among the offered options at random, weighted toward the
// first; the planner returns varied plans, often with a line of chat, after
// a model-like delay.
function mockDecider(stats) {
  return async (state, questions) => {
    await new Promise((r) => setTimeout(r, 150 + Math.random() * 250));
    stats.calls++;
    const answers = {};
    for (const [id, q] of Object.entries(questions)) {
      if (q.type === 'noul') answers[id] = { probability: Math.random() * 0.8 };
      else {
        const opts = Object.keys(q.criteria || {});
        const pick = opts[Math.random() < 0.5 ? 0 : Math.floor(Math.random() * opts.length)];
        answers[id] = { choice: pick, probabilities: Object.fromEntries(opts.map((o) => [o, o === pick ? 0.7 : 0.3 / Math.max(1, opts.length - 1)])) };
      }
    }
    return answers;
  };
}
const MOCK_LINES = ['Pushing their base, cover me', 'Anyone got an APC?', 'Going left flank', 'Need backup at the refinery', 'On my way', 'Engineer here, I will fix it', 'Their harvester is wide open', 'Watch the tunnel', 'Rush now!', 'Holding the middle'];
function mockPlanner(stats, rec = () => {}) {
  return async (view, persona, notes, reason, options) => {
    // The same prompt the models get, recorded (sizes, the playbook in use).
    const prompt = planPrompt(view, persona, notes, reason, options);
    await new Promise((r) => setTimeout(r, 1500 + Math.random() * 3000));
    stats.calls++;
    rec('planner_call', { bot: view.me.name, model: 'mock', reason, playbook: options.playbookKey, chars: { playbook: options.playbook?.length || 0, prompt: prompt.length }, prompt });
    const pick = (list) => list[Math.floor(Math.random() * list.length)];
    const plans = [];
    const enemy = options.buildings.filter((b) => b.startsWith('enemy '));
    const ours = options.buildings.filter((b) => b.startsWith('our '));
    if (enemy.length) plans.push({ id: 'attack', text: `hit the ${pick(enemy)}`, kind: 'attack', building: pick(enemy) });
    if (view.shop?.items?.length && Math.random() < 0.6) plans.unshift({ id: 'buy', text: 'gear up first', kind: 'buy', item: pick(view.shop.items).name });
    if (ours.length) plans.push({ id: 'defend', text: `guard ${pick(ours)}`, kind: 'defend', building: pick(ours) });
    plans.push({ id: 'roam', text: 'hunt in the field', kind: 'go', place: pick(options.places) });
    return { plans, orders: [], say: Math.random() < 0.5 ? pick(MOCK_LINES) : '', note: '' };
  };
}

// What the bot may say: one line, plain, short.
const cleanSay = (t) => String(t || '').replace(/[\u2013\u2014]/g, ' - ').replace(/[\u2018\u2019]/g, "'").replace(/[^\x20-\x7e]/g, '').replace(/\s+/g, ' ').trim().slice(0, 100);

// --- Clef-flash (Workers AI, through the match service's /ai/<match>) -------
// A call that fails or takes too long gets the scripted answers instead, so a
// bot never waits on the model.
const CLEF_TIMEOUT_MS = 2500; // big games: Workers AI slows down with many calls in flight
// Clef models, per bot (REBEL_DECIDERS): the same typed questions to each.
export const DECIDER_MODELS = ['clef-flash', 'clef-omni', 'clef', 'vercel/d1', 'vercel/d1-ultra', 'vercel/decider', 'vercel/decider-flash', 'vercel/decision-1', 'vercel/jev'];
function clefDecider(ai, stats) {
  const per = (model) => (stats.models ??= {})[model] ??= { calls: 0, failed: 0, ms: [], usd: 0 };
  const decide = async (state, questions, model = 'clef-flash', retried = false) => {
    const t0 = Date.now();
    const m = per(model);
    try {
      const res = await fetch(ai.url, {
        method: 'POST',
        headers: { authorization: `Bearer ${ai.key}`, 'content-type': 'application/json' },
        body: JSON.stringify({ model, input: { state, questions } }),
        signal: AbortSignal.timeout(CLEF_TIMEOUT_MS),
      });
      const body = await res.json();
      if (!res.ok || !body.output?.answers) throw new Error(body.error || `HTTP ${res.status}`);
      stats.calls++;
      stats.ms.push(Date.now() - t0);
      stats.tokens += body.output.usage?.input_tokens || 0;
      const usd = callUsd(model, body.output.usage, body.output);
      stats.usd = (stats.usd || 0) + usd;
      m.calls++;
      m.ms.push(Date.now() - t0);
      if (m.ms.length > 500) m.ms.shift();
      m.usd += usd;
      const answers = {};
      for (const [id, a] of Object.entries(body.output.answers)) {
        answers[id] = a.type === 'noul' ? { probability: a.noul } : { choice: a.choice, probabilities: a.probabilities, confidence: a.confidence };
      }
      return answers;
    } catch (e) {
      stats.failed++;
      m.failed++;
      stats.lastError = `${model}: ${String(e.message || e)}`;
      // A gateway model failing (out of credit, a provider down): this
      // decision goes to Clef on Workers AI instead of the scripted reflexes.
      if (model !== 'clef-flash' && !retried) return decide(state, questions, 'clef-flash', true);
      return null;
    }
  };
  return decide;
}

const CALLOUTS = {
  enemies_spotted: (s) => (s.enemies_in_sight.length === 1 ? `Enemy ${s.enemies_in_sight[0].kind} ${s.enemies_in_sight[0].place}` : `${s.enemies_in_sight.length} enemies ${s.me.place}`),
  need_help: (s) => `Need help ${s.me.place}!`,
  base_under_attack: (s) => `${s.alerts.find((a) => a.kind === 'base_under_attack')?.what} under attack!`,
  on_my_way: (s, o) => (o ? `Heading to ${o.name.replace(/^the /, '')}` : null),
};

// Workers AI list prices, USD per million tokens (input, cached input,
// output), for the spend shown in game and the per-match budget.
const PRICES = {
  'clef-flash': [0.038, 0.038, 0],
  'clef-omni': [0.15, 0.15, 0],
  clef: [0.24, 0.24, 0.1],
  'glm-5.3-flash': [0.15, 0.03, 0.5],
  'glm-4.7-flash': [0.06, 0.06, 0.4],
  'qwen3-30b': [0.051, 0.051, 0.335],
  'gemma-4-26b': [0.1, 0.1, 0.3],
  // Vercel AI Gateway list prices (its responses carry the real cost too).
  'vercel/deepseek-v4-flash': [0.06, 0.0091, 0.18],
  'vercel/gpt-oss-20b': [0.07, 0.07, 0.3],
  'vercel/d1': [0.02, 0.02, 0],
  'vercel/d1-ultra': [0.042, 0.042, 0],
  'vercel/decider': [0.02, 0.02, 0],
  'vercel/decider-flash': [0.01, 0.01, 0],
  'vercel/decision-1': [0.042, 0.042, 0],
  'vercel/jev': [0.042, 0.042, 0],
  'deepseek-v4-flash': [0.44, 0.014, 1.32],
  'gpt-oss-20b': [0.2, 0.2, 0.3],
  'llama-4-scout': [0.27, 0.27, 0.85],
};
// A call's cost from its usage (OpenAI-style fields; cached tokens where reported).
export function callUsd(model, usage = {}, output = null) {
  // What the gateway billed, when it says (Vercel does).
  const billed = +(output?.provider_metadata?.gateway?.cost ?? output?.providerMetadata?.gateway?.cost ?? NaN);
  if (Number.isFinite(billed)) return billed;
  const [inp, cached, out] = PRICES[model] || [0.5, 0.5, 1.5];
  const prompt = usage.prompt_tokens ?? usage.input_tokens ?? 0;
  const hit = usage.prompt_tokens_details?.cached_tokens ?? usage.cached_tokens ?? 0;
  return ((prompt - hit) * inp + hit * cached + (usage.completion_tokens ?? usage.output_tokens ?? 0) * out) / 1e6;
}

// Models for each bot: the planner pool, round robin (each bot's chain is
// its own model, then the rest of the pool), and the decider mix, weighted
// ("clef-flash:26,clef-omni:3,clef:3"), spread evenly over the join order so
// both teams get some of each.
export function modelPlan(planners = DEFAULT_PLANNERS, deciders = 'clef-flash') {
  // "a,b|c,d": bots are spread over a and b; c and d only stand in.
  const spec = Array.isArray(planners) ? planners.join(',') : String(planners);
  const [primary, standby = ''] = spec.split('|');
  const names = (x) => x.split(',').map((m) => m.trim()).filter((m) => PLANNER_MODELS[m]);
  const pool = names(primary).length ? names(primary) : DEFAULT_PLANNERS;
  const fallbacks = names(standby);
  const mix = String(deciders).split(',').map((x) => x.trim().split(':')).filter(([m]) => DECIDER_MODELS.includes(m)).map(([m, w]) => [m, Math.max(0, +(w ?? 1) || 1)]);
  const total = mix.reduce((n, [, w]) => n + w, 0) || 1;
  // Evenly spread: a sequence where each next slot takes the model whose
  // share is most behind. Bot i takes slot i + floor(i / pool size), so each
  // round of planners meets the deciders shifted by one (a Latin square):
  // without it, with 4 planners and 4 deciders every bot of a planner had the
  // same decider, and their scores couldn't tell the two apart.
  const given = Object.fromEntries(mix.map(([m]) => [m, 0]));
  const sequence = [];
  for (let n = 0; n < 256; n++) {
    const m = mix.length ? mix.map(([m, w]) => [m, (w / total) * (n + 1) - given[m]]).sort((a, b) => b[1] - a[1])[0][0] : 'clef-flash';
    given[m] = (given[m] || 0) + 1;
    sequence.push(m);
  }
  let k = 0;
  return () => {
    const i = k++;
    const decider = sequence[(i + Math.floor(i / pool.length)) % sequence.length];
    const own = pool[i % pool.length];
    return { planners: [...new Set([own, ...pool, ...fallbacks, 'gpt-oss-20b'])], decider };
  };
}

export function createBrain({ mode = 'scripted', send: sendCommand, record: rec = () => {}, out = console.log, ai = null, budgetUsd = 2, planners = DEFAULT_PLANNERS, deciders = DEFAULT_DECIDERS }) {
  const nextModels = modelPlan(planners, deciders);
  // Every command the bots get goes into the record too.
  const send = (line) => {
    // A position that isn't a number never goes to the engine (an infinite
    // one hung the server in path finding); it's logged to find its source.
    if (/ (go|shoot|place) /.test(line) && /NaN|Infinity|undefined/.test(line)) {
      rec('bad_command', { line, stack: new Error().stack.split('\n').slice(2, 7).join(' | ') });
      out(`[brain] dropped a bad command: ${line}`);
      return;
    }
    rec('command', { line });
    sendCommand(line);
  };
  let world = null;
  let badStates = 0;
  let facts = null; // from the engine, once per map (see playbook.mjs)
  const playbookCache = new Map();
  const jobsCache = new Map(); // map -> preset -> the item's job (itemJobs)
  // The planner's playbook on this map: the same for both teams, so every
  // call on the map shares it as a prefix (recorded once, by key).
  function playbookFor() {
    const key = `${world?.map}`;
    if (!playbookCache.has(key)) {
      const text = [CORE, PLAN_FORMAT, mapSheet(facts, world)].filter(Boolean).join('\n\n');
      playbookCache.set(key, text);
      rec('playbook', { key, text });
    }
    return { playbook: playbookCache.get(key), playbookKey: key };
  }
  let places = [];
  let pending = false;
  const chat = []; // {t, from, team, text, to: 'all'|team}
  const bots = new Map(); // id -> blackboard and memory
  const teams = createTeams(); // roles and squads

  const stats = { calls: 0, failed: 0, tokens: 0, ms: [], lastError: null };
  const model = mode === 'mock' ? mockDecider(stats) : (mode === 'clef' || mode === 'hybrid') && ai?.url ? clefDecider(ai, stats) : null;
  const planStats = { calls: 0, failed: 0, tokens: 0, ms: [], lastError: null, by: {} };
  const planner = mode === 'mock' ? mockPlanner(planStats, rec) : mode === 'hybrid' && ai?.url ? llmPlanner(ai, planStats, rec) : null;
  const lastBotChat = {}; // per team: bots' lines at least 6 s apart

  // The match's model budget: past it, the bots play on scripted.
  const spent = () => (stats.usd || 0) + (planStats.usd || 0);
  let capped = false;
  const withinBudget = () => {
    // Bots among themselves (no human in the game) play scripted: free.
    if (!world?.players.some((p) => !p.bot)) return false;
    if (!capped && spent() > budgetUsd) {
      capped = true;
      out(`[brain] model budget reached ($${spent().toFixed(2)} of $${budgetUsd}): scripted from now on`);
    }
    return !capped;
  };
  let personaIndex = Math.floor(Math.random() * PERSONAS.length);
  if (mode === 'clef' && !model) out('[brain] clef: no REBEL_AI_URL/REBEL_AI_KEY, deciding scripted');
  if (model) {
    setInterval(() => {
      const ms = stats.ms.splice(0).sort((a, b) => a - b);
      const q = (f) => ms[Math.min(ms.length - 1, Math.floor(ms.length * f))] ?? 0;
      out(`[brain] $${spent().toFixed(3)} so far; clef: ${stats.calls} calls, ${stats.failed} failed${stats.lastError ? ` (${stats.lastError})` : ''}, ${stats.tokens} tokens; latency p50 ${q(0.5)} ms, p90 ${q(0.9)} ms`);
      stats.lastError = null;
      if (planner) {
        const pm = planStats.ms.splice(0).sort((a, b) => a - b);
        out(`[brain] planner: ${planStats.calls} calls ${JSON.stringify(planStats.by)}, ${planStats.failed} failed${planStats.lastError ? ` (last: ${planStats.lastError})` : ''}, ${planStats.tokens} tokens; latency p50 ${pm[Math.floor(pm.length / 2)] ?? 0} ms`);
        planStats.lastError = null;
      }
    }, 30000).unref();
  }

  // What happened between two world states: deaths and kills, buildings
  // damaged or destroyed, vehicles made or lost; and a snapshot every 5 s.
  let lastSnapshot = 0;
  const damage = {}; // building id -> damage since the last report
  let shopMap = null, lastPositions = 0;
  function gameEvents(a, b) {
    const now = Date.now();
    // Paths: every player and vehicle, every 2 s, compactly ([id, x, y]).
    if (now - lastPositions >= 2000) {
      lastPositions = now;
      const xy = (p) => [Math.round(p[0] * 10) / 10, Math.round(p[1] * 10) / 10];
      rec('positions', {
        p: b.players.filter((p) => p.alive).map((p) => [p.id, ...xy(p.pos), p.vehicle || 0]),
        v: b.vehicles.filter((v) => !/ceiling|agt|obelisk|turret/i.test(v.name)).map((v) => [v.id, ...xy(v.pos), v.driver || 0]),
      });
    }
    // Who damaged which building, and who killed whom (the engine's hits).
    if (b.grid) rec('grid', { map: b.map, ...b.grid });
    if (b.hits?.length || b.kills?.length) {
      const name = (id) => b.players.find((p) => p.id === id)?.name || null;
      rec('damage', {
        hits: (b.hits || []).map((h) => ({ ...h, name: name(h.by), team: b.players.find((p) => p.id === h.by)?.team || null })),
        kills: (b.kills || []).map((k) => ({ ...k, name: name(k.by), victim_name: k.vehicle ? null : name(k.victim) })),
      });
    }
    if (now - lastSnapshot >= 5000) {
      lastSnapshot = now;
      rec('snapshot', {
        map: b.map,
        buildings: b.buildings.map((x) => ({ id: x.id, name: x.name, team: x.team, hp: x.hp, pos: x.pos, mct: x.mct })),
        players: b.players.map((p) => ({ id: p.id, name: p.name, team: p.team, bot: p.bot, alive: p.alive, hp: p.hp, credits: p.credits, kills: p.kills, deaths: p.deaths, char: p.char, vehicle: p.vehicle, pos: p.pos })),
        vehicles: b.vehicles.filter((v) => !/ceiling|agt|obelisk|turret/i.test(v.name)).map((v) => ({ id: v.id, name: v.name, team: v.team, hp: v.hp, driver: v.driver, pos: v.pos })),
        bots: b.bots.map((x) => ({ id: x.id, waypoint: x.waypoint, path: x.path, stuck: x.stuck, target: x.target, sees: x.sees, dest: x.dest, weapons: x.weapons, in_hand: x.in_hand })),
        ...(shopMap !== b.map ? { shop: b.shop } : {}), // what's for sale, once per map
      });
      shopMap = b.map;
    }
    if (!a || a.map !== b.map) return;
    const old = new Map(a.players.map((p) => [p.id, p]));
    for (const p of b.players) {
      const o = old.get(p.id);
      if (!o) continue;
      if (p.deaths > o.deaths) rec('death', { name: p.name, team: p.team, bot: p.bot, char: o.char, pos: o.pos, place: placeOf(places, o.pos) });
      if (p.kills > o.kills) rec('kill', { name: p.name, team: p.team, bot: p.bot, char: p.char, pos: p.pos, place: placeOf(places, p.pos) });
      if (p.char !== o.char && p.alive && o.alive) rec('character', { name: p.name, bot: p.bot, from: o.char, to: p.char });
      if (p.vehicle !== o.vehicle) rec(p.vehicle ? 'vehicle_enter' : 'vehicle_exit', { name: p.name, bot: p.bot, vehicle: (b.vehicles.find((v) => v.id === (p.vehicle || o.vehicle)) || a.vehicles.find((v) => v.id === o.vehicle))?.name });
    }
    const oldB = new Map(a.buildings.map((x) => [x.id, x]));
    for (const x of b.buildings) {
      const o = oldB.get(x.id);
      if (!o) continue;
      if (o.hp > 0 && x.hp <= 0) rec('building_destroyed', { name: x.name, team: x.team });
      else if (x.hp < o.hp) damage[x.id] = (damage[x.id] || 0) + (o.hp - x.hp);
      if ((damage[x.id] || 0) >= 0.05) {
        rec('building_damaged', { name: x.name, team: x.team, hp: x.hp, lost: +damage[x.id].toFixed(3) });
        damage[x.id] = 0;
      }
    }
    const oldV = new Set(a.vehicles.map((v) => v.id)), newV = new Set(b.vehicles.map((v) => v.id));
    for (const v of b.vehicles) if (!oldV.has(v.id)) rec('vehicle_new', { id: v.id, name: v.name, team: v.team, pos: v.pos });
    for (const v of a.vehicles) if (!newV.has(v.id) && !/ceiling|agt|obelisk|turret/i.test(v.name)) rec('vehicle_lost', { id: v.id, name: v.name, team: v.team, pos: v.pos });
  }

  function blackboard(id) {
    let b = bots.get(id);
    if (!b) {
      b = { plans: [], orders: [], current: null, planText: '', nextDecide: 0, nextPlan: 0, lastSay: 0, sent: {}, events: [], last: null, notes: [], persona: PERSONAS[personaIndex++ % PERSONAS.length], planning: false, why: 'you just joined the match', models: nextModels() };
      bots.set(id, b);
    }
    return b;
  }

  // What one bot knows right now: the decider's state.
  function view(bot, me) {
    const bb = blackboard(bot.id);
    const byObj = new Map(world.players.filter((p) => p.obj).map((p) => [p.obj, p]));
    const vehicles = new Map(world.vehicles.map((v) => [v.id, v]));
    // Base defenses' guns are objects too (the AGT's, the Obelisk's): not targets.
    const defense = (id) => /ceiling|agt|obelisk|guard tower/i.test(vehicles.get(id)?.name || '');
    const enemies = bot.sees.filter((id) => !defense(id)).map((id) => {
      const p = byObj.get(id), v = vehicles.get(id);
      const pos = (p || v).pos;
      return { id, name: p ? p.name : null, kind: p ? characterName(p.char) : characterName(v.name), health: +(p || v).hp.toFixed(2), distance: Math.round(dist(me.pos, pos)), place: placeOf(places, pos), pos };
    });
    const alerts = world.buildings
      .filter((b) => b.team === me.team && b.hp > 0 && bb.lastHp?.[b.id] !== undefined && b.hp < bb.lastHp[b.id] - 0.005)
      .map((b) => ({ kind: 'base_under_attack', what: `Our ${buildingName(b)}`, health: pct(b.hp) }));
    bb.lastHp = Object.fromEntries(world.buildings.map((b) => [b.id, b.hp]));
    if (alerts.length) bb.alerted = Date.now() + 5000;
    return {
      world,
      me: { id: me.id, name: me.name, team: me.team, health: +me.hp.toFixed(2), credits: me.credits, character: characterName(me.char), in_vehicle: me.vehicle ? characterName(world.vehicles.find((v) => v.id === me.vehicle)?.name) : null, place: placeOf(places, me.pos), pos: me.pos, kills: me.kills, deaths: me.deaths },
      squad: (() => {
        const org = teams.join(me.id, me.team);
        const sq = teams.squad(me.id);
        bb.org = { role: org.role, slot: org.slot, objective: squadObjective(teams, sq, world, places) };
        const mates = sq.members.filter((x) => x !== me.id).map((x) => world.players.find((p) => p.id === x)).filter(Boolean);
        const lead = sq.members[0] !== me.id ? world.players.find((p) => p.id === sq.members[0] && p.alive) : null;
        bb.leader = lead ? lead.pos : null;
        return {
          role: org.role,
          role_means: ROLES.find((r) => r.role === org.role).text,
          objective: bb.org.objective?.text || 'none',
          mates: mates.map((m) => ({ name: m.name, distance: Math.round(dist(me.pos, m.pos)), alive: m.alive })),
          leader_distance: lead ? Math.round(dist(me.pos, lead.pos)) : null,
        };
      })(),
      on_tiberium: !!bot.tiberium,
      enemies_in_sight: enemies,
      surroundings: bot.feel && {
        driving: bot.driving,
        speed: bot.speed,
        clear_ahead_m: bot.feel[0], clear_ahead_left_m: bot.feel[1], clear_ahead_right_m: bot.feel[2],
        clear_left_m: bot.feel[3], clear_right_m: bot.feel[4], clear_behind_m: bot.feel[5],
        times_stuck_in_a_row: bot.stuck,
        in_maneuver: bot.maneuver,
      },
      teammates: world.players.filter((p) => p.team === me.team && p.id !== me.id && p.alive).map((p) => ({ name: p.name, place: placeOf(places, p.pos), distance: Math.round(dist(me.pos, p.pos)), health: +p.hp.toFixed(2) })),
      buildings: world.buildings.filter((b) => b.team !== 'none').map((b) => ({ side: b.team === me.team ? 'ours' : 'theirs', name: buildingName(b), health: pct(b.hp) })),
      alerts,
      chat: chat.filter((c) => c.to === 'all' || c.to === me.team).slice(-6).map((c) => `${c.from}${c.to === 'all' ? '' : ' (team)'}: ${c.text}`),
      events: bb.events.slice(-5),
      shop: (({ items, powered, factory }) => ({ items: items.map(({ name, cost, kind }) => ({ name, cost, kind })), powered, factory }))(shopFor(world, me)),
      blackboard: { personality: bb.persona.style, current: bb.current, planText: bb.planText, plans: bb.plans.map((p) => `${p.id}: ${p.text}`), orders: bb.orders },
      stuck: bot.stuck,
    };
  }

  // The moment's questions, in the decision model's format: every option is
  // something the bot can do right now.
  function questionsFor(state, bb, objective, all = false) {
    const plans = Object.fromEntries(bb.plans.map((p) => [p.id, p.text]));
    // Shooting is a reflex: with enemies in sight the question is which one.
    const atBuilding = objective?.building && objective.distance < 45 && objective.enemy;
    // Any enemy building close by is a target too (not only the objective).
    const nearBuildings = world.buildings.filter((x) => x.team === enemyOf(state.me.team) && x.hp > 0 && dist(x.pos, state.me.pos) < 40);
    const target = state.enemies_in_sight.length || atBuilding || nearBuildings.length ? {} : { none: 'Nothing to shoot at.' };
    for (const x of nearBuildings) target[`bld_${x.id}`] = `Shoot the enemy ${buildingName(x)} (${pct(x.hp)}, ${Math.round(dist(x.pos, state.me.pos))} m away).`;
    for (const e of state.enemies_in_sight) target[`enemy_${e.id}`] = `Shoot the ${e.kind}${e.name ? ` (${e.name})` : ''}, ${e.distance} m away, health ${pct(e.health)}.`;
    if (atBuilding) target.building = `Shoot the enemy ${objective.name}.`;
    // How to move: only what makes sense now.
    const moves = { advance: `Go on with the plan: move toward ${objective?.name || 'the objective'}.` };
    if (state.on_tiberium) moves.leave_tiberium = 'Get off the tiberium now: it is hurting you.';
    if (state.enemies_in_sight.length) {
      moves.strafe = 'Keep shooting while side-stepping, harder to hit.';
      moves.hold = 'Stand your ground and fight from here.';
    }
    if (state.me.health < 0.5 || state.enemies_in_sight.length > 2) moves.fall_back = 'Fall back toward your own base.';
    if (state.squad.leader_distance > 35) moves.join_squad = `Catch up with your squad (${state.squad.leader_distance} m away).`;
    // Only what is true now can be said.
    const callouts = { none: 'Say nothing.', on_my_way: 'Tell the team where you are heading.' };
    if (state.enemies_in_sight.length) callouts.enemies_spotted = 'Report the enemies you see.';
    if (state.enemies_in_sight.length && state.me.health < 0.6) callouts.need_help = 'Ask for help.';
    if (state.alerts.length) callouts.base_under_attack = 'Warn that our base is under attack.';
    // At the base with credits: what to buy, if anything.
    const shop = shopFor(world, state.me);
    const buy = {};
    if (shop.atBase && !state.me.in_vehicle && shop.affordable.length && Date.now() - (bb.lastBuy || 0) > 30000) {
      // Credits only win when spent: the more a bot holds, the less "none" is worth.
      buy.none = state.me.credits >= 1000 ? `Buy nothing, though you hold ${state.me.credits} credits doing nothing.` : 'Buy nothing now: keep your credits.';
      const vehicleWait = bb.wantVehicle || Date.now() - (bb.lastVehicleBuy || 0) < 120000;
      const jobs = jobsCache.get(world.map) || jobsCache.set(world.map, itemJobs(facts, world)).get(world.map);
      const job = (i) => (jobs.get(i.preset) ? `: ${jobs.get(i.preset)}` : '');
      // Saving for something: only what still leaves enough for it (or it).
      const saving = bb.saveFor && Date.now() - bb.saveFor.since < SAVE_FOR_MS ? bb.saveFor : null;
      const keeps = (i) => !saving || i.name === saving.name || state.me.credits - i.cost >= saving.cost;
      for (const i of shop.affordable.filter((x) => (x.kind !== 'vehicle' || !vehicleWait) && keeps(x))) buy[itemId(i)] = i.kind === 'vehicle' ? `Buy a ${i.name} (${i.cost} credits) and drive it${job(i)}.` : `Become a ${i.name} (${i.cost} credits)${job(i)}.`;
    }
    // Blocked or crawling into something: how to get going again (a driver's
    // or a soldier's quick read of what's around, not a reflex).
    const sur = state.surroundings;
    const maneuver = {};
    const walking = bb.walkingTo; // still has a way to go
    if (sur && !sur.in_maneuver && walking && (sur.times_stuck_in_a_row >= 1 || (sur.speed < 1.2 && sur.clear_ahead_m < (sur.driving ? 5 : 1.2)))) {
      const open = (m) => m >= (sur.driving ? 8 : 3);
      maneuver.keep_going = `Nothing to fix: keep following your route (clear ahead ${sur.clear_ahead_m} m).`;
      if (open(sur.clear_ahead_left_m)) maneuver.steer_left = `Go around it on the left (open ${sur.clear_ahead_left_m} m ahead-left).`;
      if (open(sur.clear_ahead_right_m)) maneuver.steer_right = `Go around it on the right (open ${sur.clear_ahead_right_m} m ahead-right).`;
      if (sur.clear_behind_m >= (sur.driving ? 6 : 2)) {
        maneuver.reverse_left = `Back up to the left and try again (${sur.clear_behind_m} m clear behind).`;
        maneuver.reverse_right = `Back up to the right and try again (${sur.clear_behind_m} m clear behind).`;
      }
      maneuver.replan = 'This way is blocked: take another route.';
      maneuver.wait = 'Something is moving past: wait two seconds.';
      if (sur.driving && sur.times_stuck_in_a_row >= 2) maneuver.get_out = 'The vehicle is stuck for good: get out and go on foot.';
    }
    const questions = {
      ...(Object.keys(maneuver).length > 2 ? { maneuver: { type: 'choice', instructions: `You are ${sur?.driving ? 'driving' : 'on foot'} at ${sur?.speed} m/s and something is in the way. What do you do?`, criteria: maneuver } } : {}),
      ...(Object.keys(buy).length > 1 ? { buy: { type: 'choice', instructions: 'Do you buy something at the terminal now?', criteria: buy } } : {}),
      plan: { type: 'choice', instructions: 'Which of your plans fits the situation now?', criteria: plans },
      move: { type: 'choice', instructions: 'How do you move right now?', criteria: moves },
      target: { type: 'choice', instructions: 'Which target do you focus your fire on?', criteria: target },
      callout: { type: 'choice', instructions: 'Do you tell your team something now?', criteria: callouts },
      rethink: { type: 'noul', instructions: 'Has something happened that none of your plans covers?' },
    };
    // A choice needs two options at least (nothing in sight: no target question).
    if (!all) for (const [id, q] of Object.entries(questions)) if (q.type === 'choice' && Object.keys(q.criteria).length < 2) delete questions[id];
    return questions;
  }

  // A team's open push (a rally that went): others join it (see rallyFor).
  const teamPush = {}; // team -> { building, at, by }
  // The map's walking grid (bots_web.cpp sends it once a map): walkable cells.
  let grid = null, vgrid = null; // soldiers' and vehicles' (bots_web.cpp)
  function decodeGrid(g) {
    const cells = new Uint8Array(g.nx * g.ny);
    g.rows.forEach((row, y) => {
      let x = 0;
      for (const m of row.matchAll(/([#.~!])(\d+)/g)) for (let k = 0; k < +m[2]; k++, x++) cells[y * g.nx + x] = m[1] === '#' ? 0 : 1;
    });
    return { ...g, rows: undefined, cells };
  }
  const onGrid = (g, p) => {
    if (!g) return true;
    const x = Math.floor((p[0] - g.minX) / g.cell), y = Math.floor((p[1] - g.minY) / g.cell);
    return x >= 0 && y >= 0 && x < g.nx && y < g.ny && g.cells[y * g.nx + x] === 1;
  };
  const walkable = (p) => onGrid(grid, p);
  const drivable = (p) => onGrid(vgrid, p);
  const hasWeapon = (bot, kinds) => (bot?.weapons || []).some(([k, n]) => kinds.includes(k) && n !== 0);
  // Live enemy base defenses of a team (Obelisk, Guard Tower).
  const defensesOf = (team) => world.buildings.filter((x) => x.team === team && x.hp > 0 && /obelisk|guard_tower|agt/i.test(x.name));
  // A spot to fire at building b from, outside every live enemy defense's
  // reach and within the weapon's (and the engine's attack) range: on the
  // line from b toward our base, the nearest point that's clear. None if
  // the weapon is too short for that (then it's a push into the defense).
  function siegeSpot(me, b, range) {
    if (range < SIEGE_NEAR_M) return null;
    const defenses = defensesOf(b.team);
    if (!defenses.length) return null;
    // Around the building, 25-40 m out, on walkable ground (the map's
    // grid), outside every live defense's reach: the one nearest to us.
    let best = null;
    for (let r = SIEGE_NEAR_M; r <= Math.min(SIEGE_FAR_M, range); r += 5)
      for (let a = 0; a < 360; a += 15) {
        const at = [b.pos[0] + r * Math.cos((a * Math.PI) / 180), b.pos[1] + r * Math.sin((a * Math.PI) / 180), b.pos[2]];
        if (!(me.vehicle ? drivable(at) : walkable(at)) || !defenses.every((x) => dist(at, x.pos) >= SIEGE_KEEP_OUT_M)) continue;
        if (!best || dist(me.pos, at) < dist(me.pos, best)) best = at;
      }
    return best;
  }
  const rangeOf = (me) => {
    const preset = me.vehicle ? world.vehicles.find((x) => x.id === me.vehicle)?.name : me.char;
    return facts?.weapons?.[preset]?.range || 0;
  };

  // The most damaged friendly building near (at its MCT), else a hurt
  // friendly vehicle close by.
  function repairTarget(me) {
    const b = world.buildings.filter((x) => x.team === me.team && x.hp > 0 && x.hp < REPAIR_BELOW && dist(me.pos, x.mct || x.pos) < REPAIR_NEAR_M).sort((a, c) => a.hp - c.hp)[0];
    if (b) return { name: `our ${buildingName(b)}`, pos: b.mct || b.pos, building: b.id };
    const v = world.vehicles.find((x) => x.team === me.team && x.hp > 0 && x.hp < 0.8 && dist(me.pos, x.pos) < 25 && !/harvest/i.test(x.name));
    return v ? { name: 'a hurt vehicle', pos: v.pos } : null;
  }

  // Pushing a defended base: alone, a bot just feeds the defense (in a full
  // game ~250 of 627 deaths were inside the enemy base, and no building went
  // below 90%). So an attack on an enemy building while their base defense
  // stands gathers first: at a point just past the middle on the way there,
  // until a few teammates are there too (players' "wait for me, push
  // together"); whoever is around joins, no squads needed. It goes anyway
  // after a while, so a quiet team doesn't stall.
  const RALLY_SHARE = 0.55; // of the way from our base to the target
  const RALLY_NEED = 3; // bots there, this one included
  const RALLY_PATIENCE_MS = [60000, 90000]; // then one short will do; then go anyway
  function rallyFor(plan, me, bb, b) {
    const defense = defensesOf(b.team).length > 0;
    // Long enough a weapon to hit it from outside the defense: no rally.
    if (defense && !bb?.c4 && siegeSpot(me, b, rangeOf(me))) return null;
    const defended = defensesOf(b.team).some((x) => dist(x.pos, b.pos) < DEFENSE_REACH_M);
    const need = me.vehicle ? (defended ? 3 : 2) : defended ? RALLY_NEED_DEFENDED : RALLY_NEED;
    const home = places.find((p) => p.base && p.team === me.team)?.pos;
    if (!defense || !home || !bb) return null;
    // Per target building, not per plan: a driver switching between plans on
    // the same building (push, join the push) went back home to rally each
    // time (53% of its orders pointed home).
    const r = ((bb.rally ??= {})[b.id] ??= {});
    if (r.go) return null;
    // A push on this building is on: no waiting, join it.
    const push = teamPush[me.team];
    if (push && push.building === b.id && Date.now() - push.at < PUSH_MS) return (r.go = true), null;
    // Vehicles gather at home (a point between the bases may be infantry
    // ground: in a game, every vehicle looped in its own base and the south
    // field, never reaching the enemy), and go as a convoy.
    const convoy = !!me.vehicle;
    const at = convoy ? home : home.map((v, i) => v + RALLY_SHARE * (b.pos[i] - v));
    const name = convoy ? `the vehicle rally at our base for the enemy ${buildingName(b)}` : `the rally point for the enemy ${buildingName(b)}`;
    if (!convoy && dist(me.pos, b.pos) < dist(at, b.pos) - 10) return (r.go = true), null; // already past it
    if (dist(me.pos, at) > (convoy ? 45 : 15)) return { name, pos: at, rally: true, distance: dist(me.pos, at) };
    const now = Date.now();
    if (!r.since) {
      r.since = now;
      bb.rallySay = `Gathering for a push on the ${buildingName(b)}, join me`;
    }
    const here = convoy
      ? world.vehicles.filter((v) => v.team === me.team && v.driver && v.hp > 0 && dist(v.pos, me.pos) < 40).length
      : world.players.filter((p) => p.alive && p.team === me.team && dist(p.pos, me.pos) < 25).length;
    const waited = now - r.since;
    // (Pushes into the Obelisk went with 4 on average and died at the door.)
    if (here >= need || (waited > RALLY_PATIENCE_MS[0] && here >= need - 1) || waited > RALLY_PATIENCE_MS[1]) {
      r.go = true;
      if (here >= 2 && !(teamPush[me.team] && Date.now() - teamPush[me.team].at < PUSH_MS)) {
        teamPush[me.team] = { building: b.id, at: Date.now(), by: me.name };
        rec('push', { team: me.team, building: b.id, name: buildingName(b), by: me.name, with: here, pos: me.pos });
      }
      if (here >= 2) bb.rallySay = `Pushing the ${buildingName(b)} now!`;
      return null;
    }
    return { name, pos: at, rally: true, waiting: true, distance: dist(me.pos, at) };
  }

  function objectiveOf(plan, me, bb) {
    if (!plan) return null;
    // A flank: by the side point first.
    if (plan.via && bb && !(bb.viaDone ??= {})[plan.id]) {
      if (dist(me.pos, plan.via) < 20) bb.viaDone[plan.id] = true;
      else return { name: 'the flank', pos: plan.via, distance: dist(me.pos, plan.via) };
    }
    if (plan.pos) return { name: plan.text, pos: plan.pos, distance: dist(me.pos, plan.pos) };
    if (plan.building) {
      const b = world.buildings.find((x) => x.id === plan.building);
      const rally = b && b.team !== me.team && rallyFor(plan, me, bb, b);
      if (rally) return rally;
      if (b && b.team !== me.team) {
        // C4 on board: to the MCT inside. A vehicle: a firing position at
        // about half its weapon's range, on our side of the building.
        // Engineer types go for the MCT from anywhere; anyone else with C4
        // once close (35 m): in, to plant it (how players kill buildings).
        if ((bb?.c4 || (bb?.hasC4 && dist(me.pos, b.pos) < C4_RUSH_M)) && b.mct && !me.vehicle) return { name: `the enemy ${buildingName(b)}'s MCT`, pos: b.mct, building: b.id, enemy: true, mct: true, distance: dist(me.pos, b.mct) };
        // A long enough weapon: fire from outside the defense (a siege).
        // A spot, once chosen, stays until the building falls or the bot
        // dies (the nearest spot moves as the bot does: 686 of 715 orders to
        // a firing spot were replaced before the vehicle got within 30 m).
        const kept = bb?.spots?.[b.id];
        if (kept && kept.vehicle === !!me.vehicle) return { name: `a firing position on the enemy ${buildingName(b)}`, pos: kept.pos, building: b.id, enemy: true, siege: true, distance: dist(me.pos, kept.pos) };
        const keep = (pos) => (bb && ((bb.spots ??= {})[b.id] = { pos, vehicle: !!me.vehicle }), pos);
        const spot = siegeSpot(me, b, rangeOf(me)) && keep(siegeSpot(me, b, rangeOf(me)));
        if (spot) return { name: `a firing position on the enemy ${buildingName(b)}`, pos: spot, building: b.id, enemy: true, siege: true, distance: dist(me.pos, spot) };
        // A vehicle (no spot outside the defense): the nearest place it can
        // drive to within 20-40 m of the building. (A building's middle
        // isn't drivable: paths ended 45-80 m out, out of firing range,
        // and in a game drivers came within 45 m of a target 11 times.)
        if (me.vehicle && vgrid) {
          let best = null;
          for (let r = 20; r <= 40; r += 5)
            for (let a = 0; a < 360; a += 15) {
              const at = [b.pos[0] + r * Math.cos((a * Math.PI) / 180), b.pos[1] + r * Math.sin((a * Math.PI) / 180), b.pos[2]];
              if (drivable(at) && (!best || dist(me.pos, at) < dist(me.pos, best))) best = at;
            }
          if (best) return { name: `a firing position on the enemy ${buildingName(b)}`, pos: keep(best), building: b.id, enemy: true, siege: true, distance: dist(me.pos, best) };
        }
        // A vehicle with no defense to fear: about half its range away.
        if (me.vehicle && !defensesOf(b.team).length) {
          const standoff = Math.min(60, Math.max(15, rangeOf(me) * 0.45));
          if (dist(me.pos, b.pos) > standoff + 5) {
            const home = places.find((p) => p.base && p.team === me.team)?.pos || me.pos;
            const dir = [home[0] - b.pos[0], home[1] - b.pos[1]];
            const len = Math.hypot(dir[0], dir[1]) || 1;
            const at = [b.pos[0] + (dir[0] / len) * standoff, b.pos[1] + (dir[1] / len) * standoff, b.pos[2]];
            return { name: `a firing position on the enemy ${buildingName(b)}`, pos: at, building: b.id, enemy: true, distance: dist(me.pos, at) };
          }
        }
      }
      if (b) return { name: `${b.team === me.team ? 'our' : 'the enemy'} ${buildingName(b)}`, pos: b.pos, building: b.id, enemy: b.team !== me.team, distance: dist(me.pos, b.pos) };
    }
    if (plan.teammate) {
      const t = world.players.find((x) => x.alive && x.team === me.team && x.name === plan.teammate);
      if (t) return { name: t.name, pos: t.pos, distance: dist(me.pos, t.pos) };
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
    if (move === 'join_squad') to = bb.leader;
    if (move === 'leave_tiberium' && Date.now() - (bb.lastLeave || 0) > 2000) {
      bb.lastLeave = Date.now();
      bb.goTo = null;
      send(`bot_do ${bot.id} leave_tiberium`);
      return;
    }
    if (move === 'strafe' && Date.now() - (bb.lastStrafe || 0) > 1500) {
      bb.lastStrafe = Date.now();
      bb.strafeSide = !bb.strafeSide;
      send(`bot_do ${bot.id} maneuver ${bb.strafeSide ? 'strafe_left' : 'strafe_right'}`);
    }
    // Squad spacing: members spread around the objective, not on one spot.
    if (to && move === 'advance' && bb.org?.slot && !objective?.mct) {
      const k = bb.org.slot, a = (k * 2.4) % (2 * Math.PI);
      to = [to[0] + 6 * Math.cos(a), to[1] + 6 * Math.sin(a), to[2]];
    }
    bb.walkingTo = move === 'advance' && to && bot.waypoint < bot.path;
    if (move === 'hold' || move === 'strafe' || !to) {
      if (move !== 'strafe') cmd('move', 'stop');
    }
    else {
      // Re-send only for a new destination (5 m), or when the walk ended.
      const last = bb.goTo;
      if (!last || dist(last, to) > 5 || (bot.waypoint >= bot.path && dist(me.pos, to) > 8)) {
        bb.goTo = to;
        cmd('move', `go ${to.map((v) => v.toFixed(1)).join(' ')}`, true);
      }
    }
    // What to shoot. At a building: its MCT when close (it counts double),
    // else the building.
    const xyz = (p, up = 0) => `${p[0].toFixed(1)} ${p[1].toFixed(1)} ${(p[2] + up).toFixed(1)}`;
    // (Not from a vehicle: the walls around an MCT block a turret; a tank
    // aimed at one fired once in 20 s, at the building 15 times.)
    const aimAt = (b) => (b.mct && !me.vehicle && dist(me.pos, b.mct) < MCT_AIM_M ? xyz(b.mct, 0.5) : xyz(b.pos, 2));
    const t = answers.target?.choice || 'none';
    const bld = t.startsWith('bld_') && world.buildings.find((x) => x.id === +t.slice(4));
    const objBld = objective?.building && world.buildings.find((x) => x.id === objective.building && x.team !== me.team);
    // An engineer type with nothing on it: repair what's damaged nearby (a
    // building at its MCT, a vehicle by its side) before anything else.
    const enemyClose = state.enemies_in_sight.some((e) => e.distance < 15);
    const repair = bb.repairs && !me.vehicle && !enemyClose && repairTarget(me);
    if (repair) {
      bb.repairing = repair.name;
      if (dist(me.pos, repair.pos) > REPAIR_GUN_M) {
        if (!bb.goTo || dist(bb.goTo, repair.pos) > 3) {
          bb.goTo = repair.pos;
          cmd('move', `go ${xyz(repair.pos)}`, true);
        }
      } else cmd('move', 'stop');
      cmd('fire', `shoot ${xyz(repair.pos, repair.building ? 0.5 : 1)}`);
      return;
    }
    bb.repairing = null;
    // C4 on the MCT when there.
    const mctNear = bb.hasC4 && !me.vehicle && world.buildings.find((x) => x.team !== me.team && x.hp > 0 && x.mct && dist(me.pos, x.mct) < C4_PLANT_M);
    if (mctNear && Date.now() - (bb.lastC4 || 0) > 4000) {
      const objBld = mctNear;
      bb.lastC4 = Date.now();
      send(`bot_do ${bot.id} c4 ${xyz(objBld.mct, 0.3)}`);
      rec('c4', { bot: me.name, team: me.team, building: objBld.id, name: buildingName(objBld), pos: me.pos });
      bb.sent.fire = null;
      return;
    }
    // At its firing position: shell the building (no close enemy chosen).
    const sieging = objective?.siege && objBld && dist(me.pos, objective.pos) < 12 && !t.startsWith('enemy_');
    // Its target building in reach (where hits land, <45 m) and no enemy in
    // its face: fire at the building. (Left to the decider, drivers within
    // 45 m of their target most of a game shot at it twice.)
    const enemyInFace = state.enemies_in_sight.some((e) => e.distance < 20);
    const atTarget = objBld && dist(me.pos, objBld.pos) < BUILDING_FIRE_M && !enemyInFace;
    if (bld) cmd('fire', `shoot ${aimAt(bld)}`);
    else if (sieging || atTarget) cmd('fire', `shoot ${aimAt(objBld)}`);
    else if (t.startsWith('enemy_')) cmd('fire', `target ${t.slice(6)}`);
    else if (t === 'building' && objBld) cmd('fire', `shoot ${aimAt(objBld)}`);
    else if (t === 'building' && objective) cmd('fire', `shoot ${xyz(objective.pos, 2)}`);
    else cmd('fire', 'hold_fire');
    // Getting around something (the maneuver question).
    const mv = answers.maneuver?.choice;
    if (mv && mv !== 'keep_going' && Date.now() - (bb.lastManeuver || 0) > 2500) {
      bb.lastManeuver = Date.now();
      send(`bot_do ${bot.id} maneuver ${mv}`);
      if (mv !== 'wait') bb.goTo = null; // the route changes: re-send the destination after
    }
    // Buying: the plan's item once at base (if affordable), else Clef's pick.
    const shop = shopFor(world, me);
    let pick = null;
    if (shop.atBase && !me.vehicle && Date.now() - (bb.lastBuy || 0) > 30000) {
      const goal = plan?.item || (bb.saveFor && Date.now() - bb.saveFor.since < SAVE_FOR_MS && bb.saveFor.name);
      const wanted = goal && shop.affordable.find((i) => i.name.toLowerCase() === goal.toLowerCase());
      pick = wanted || shop.affordable.find((i) => itemId(i) === answers.buy?.choice) || null;
      if (pick?.kind === 'vehicle' && (bb.wantVehicle || Date.now() - (bb.lastVehicleBuy || 0) < 120000)) pick = null;
    }
    if (pick) {
      bb.lastBuy = Date.now();
      send(`bot_do ${bot.id} buy ${pick.kind} ${pick.index}`);
      bb.events.push(`you bought a ${pick.name}`);
      if (pick.kind === 'vehicle') {
        bb.wantVehicle = { at: Date.now(), name: pick.name };
        bb.lastVehicleBuy = Date.now();
      }
      if (plan?.item) plan.item = null; // done
      if (bb.saveFor?.name === pick.name) bb.saveFor = null;
    }
    // A bought vehicle comes out of the factory: walk to it and get in.
    if (bb.wantVehicle && (me.vehicle || Date.now() - bb.wantVehicle.at > 45000)) bb.wantVehicle = null;
    if (bb.wantVehicle) {
      // A new vehicle belongs to no team until someone gets in.
      const empty = world.vehicles.filter((v) => (v.team === me.team || v.team === 'none') && !v.driver && dist(v.pos, me.pos) < 120 && !/ceiling|agt|obelisk|turret|harvester/i.test(v.name)).sort((a, b) => dist(a.pos, me.pos) - dist(b.pos, me.pos))[0];
      if (empty) {
        const d = dist(empty.pos, me.pos);
        if (d < 7 && Date.now() - (bb.lastEnter || 0) > 1500) {
          bb.lastEnter = Date.now();
          send(`bot_do ${bot.id} enter ${empty.id}`);
        } else if (d >= 7 && (!bb.goTo || dist(bb.goTo, empty.pos) > 3)) {
          bb.goTo = empty.pos;
          cmd('move', `go ${empty.pos.map((v) => v.toFixed(1)).join(' ')}`, true);
        }
      }
    }
    // What to say: the rally's lines first (they gather the team).
    const c = answers.callout?.choice;
    const now = Date.now();
    if (bb.rallySay && now - bb.lastSay > SAY_EVERY_MS && now - (lastBotChat[me.team] || 0) > 2000) {
      bb.lastSay = lastBotChat[me.team] = now;
      cmd('say', `say team ${bb.rallySay}`, true);
      bb.rallySay = null;
      return;
    }
    if (c && c !== 'none' && CALLOUTS[c] && now - bb.lastSay > SAY_EVERY_MS && bb.lastCallout !== c && now - (lastBotChat[me.team] || 0) > 4000) {
      bb.lastSay = lastBotChat[me.team] = now;
      bb.lastCallout = c;
      const line = CALLOUTS[c](state, objective);
      if (line) cmd('say', `say team ${line}`, true);
    } else if (c === 'none' && now - bb.lastSay > 30000) bb.lastCallout = null;
  }

  function usePlans(bb, p, state, now, by) {
    if (!p?.plans?.length) return;
    bb.plans = p.plans;
    bb.orders = p.orders || [];
    state.blackboard.plans = bb.plans.map((x) => `${x.id}: ${x.text}`);
    rec('plans', { bot: state.me.name, team: state.me.team, by, plans: p.plans, orders: p.orders });
  }

  // The model's plans, checked against the map: buildings and places it may
  // name come from lists; anything else is dropped.
  async function planWithModel(id, bb, state, why, routine = false) {
    const buildings = {}, placeNames = places.filter((p) => !p.building).map((p) => p.name);
    for (const b of world.buildings) if (b.team !== 'none' && b.hp > 0) buildings[`${b.team === state.me.team ? 'our' : 'enemy'} ${buildingName(b)}`] = b.id;
    const answer = await planner(state, bb.persona, bb.notes, why, { buildings: Object.keys(buildings), places: placeNames, orders: bb.orders, saving: bb.saveFor, c4: bb.c4, repairing: bb.repairing, routine, models: bb.models.planners, ...playbookFor() });
    if (bb.team !== state.me.team) return; // it changed sides while the model thought
    if (answer?.deferred) {
      bb.nextPlan = Date.now() + 8000 + Math.random() * 8000; // its plans stand; try again soon
      return;
    }
    if (!answer || !bots.has(id)) {
      bb.nextPlan = Date.now() + 5000; // the model was busy or slow: try again soon
      return usePlans(bb, scriptedPlan(state, bb.org), state, Date.now(), 'scripted');
    }
    // Models get names a little wrong ("Airstrip" for "enemy Airstrip",
    // "middle", "Lt. Kova" without "[BOT] "): near misses resolve; a bare
    // building is the enemy's to attack and ours to defend.
    const norm = (x) => String(x || '').toLowerCase().replace(/\[bot\]\s*/g, '').replace(/^(the|our|enemy|their)\s+/g, '').replace(/[^a-z0-9]+/g, ' ').trim();
    const findBuilding = (p) => {
      if (!p.building) return undefined;
      if (buildings[p.building] !== undefined) return buildings[p.building];
      const side = /^(our|my)\b/i.test(p.building) || p.kind === 'defend' ? 'our' : 'enemy';
      const want = norm(p.building);
      // Only the side the plan means: an attack on "the Obelisk" from Nod is
      // no plan (GDI has none), not an attack on Nod's own.
      const key = Object.keys(buildings).find((k) => k.startsWith(side) && norm(k.slice(side.length)) === want);
      return key ? buildings[key] : undefined;
    };
    const findPlace = (x) => (x ? placeNames.find((n) => n === x) || placeNames.find((n) => norm(n) === norm(x)) : undefined);
    const findMate = (x) => (x ? world.players.find((m) => m.team === state.me.team && (m.name === x || norm(m.name) === norm(x)))?.name : undefined);
    const plans = (Array.isArray(answer.plans) ? answer.plans : []).slice(0, 4).map((p, i) => {
      const plan = { id: String(p.id || `plan${i}`).replace(/[^\w]/g, '_').slice(0, 24) || `plan${i}`, text: cleanSay(p.text) || 'keep fighting' };
      const item = p.kind === 'buy' && state.shop.items.find((x) => x.name.toLowerCase() === String(p.item || '').toLowerCase());
      if (item && item.cost > state.me.credits) {
        // Not yet: save for it (cheap buys wait) and fight meanwhile.
        bb.saveFor = { name: item.name, cost: item.cost, since: Date.now() };
        return null;
      } else if (item) {
        plan.item = item.name;
        plan.place = `the ${state.me.team} base`;
      } else if (findBuilding(p) !== undefined) plan.building = findBuilding(p);
      else if (findPlace(p.place)) plan.place = findPlace(p.place);
      else if (findMate(p.teammate)) plan.teammate = findMate(p.teammate);
      else plan.place = 'the middle';
      return plan;
    });
    const seen = new Set();
    const unique = plans.filter((p) => p && !seen.has(p.id) && seen.add(p.id));
    usePlans(bb, { plans: unique.length ? unique : scriptedPlan(state, bb.org).plans, orders: Array.isArray(answer.orders) ? answer.orders.slice(0, 3).map(cleanSay) : bb.orders }, state, Date.now(), 'model');
    if (answer.note) bb.notes = [...bb.notes, cleanSay(answer.note)].slice(-5);
    const say = cleanSay(answer.say);
    const now = Date.now();
    const team = state.me.team;
    if (say && now - bb.lastSay > 15000 && now - (lastBotChat[team] || 0) > 6000) {
      bb.lastSay = lastBotChat[team] = now;
      send(`bot_do ${id} say team ${say}`);
    }
  }

  async function think(bot, me, now) {
    const bb = blackboard(bot.id);
    // A new side (teams remixed at a map change, or the slot refilled on the
    // other team when the server rebalances): nothing of the old side's
    // stands, or the bot "attacks" its own buildings.
    if (bb.team && bb.team !== me.team) {
      bb.plans = [];
      bb.current = null;
      bb.sent = {};
      bb.goTo = null;
      bb.notes = [];
      bb.orders = [];
      bb.rally = {};
      bb.spots = {};
      bb.saveFor = null;
      bb.lastDecided = null;
      bb.quietKey = null;
      bb.wantVehicle = null;
      bb.events = [];
      bb.why = `teams were remixed: you are on ${me.team} now`;
      bb.wantPlan = true;
    }
    bb.team = me.team;
    if (bb.modelsFor !== me.name) {
      bb.modelsFor = me.name;
      rec('bot_models', { bot: me.name, team: me.team, planner: bb.models.planners[0], chain: bb.models.planners, decider: bb.models.decider });
    }
    // Life events, from the scoreboard.
    if (bb.last) {
      if (me.deaths > bb.last.deaths) bb.events.push(`you died ${placeOf(places, bb.last.pos)}`), (bb.why = 'you just died');
      if (me.kills > bb.last.kills) bb.events.push('you killed an enemy');
    }
    const wasAlive = bb.last?.alive;
    bb.last = { kills: me.kills, deaths: me.deaths, pos: me.pos, alive: me.alive };
    if (!me.alive) return;
    // A driver going nowhere for long (wedged at a base, against a
    // building): out, and on foot from there.
    if (me.vehicle) {
      const h = (bb.drive ??= []);
      h.push([Date.now(), me.pos]);
      while (h.length && h[0][0] < Date.now() - STUCK_VEHICLE_MS) h.shift();
      const far = bb.goTo && dist(me.pos, bb.goTo) > 15;
      // (Only when the engine says it's stuck too: a vehicle waiting at a
      // firing position or a rally isn't wedged; in a test, without this,
      // drivers bailed 68 times in 8 minutes.)
      if (far && bot.stuck >= 2 && h.length > 5 && Date.now() - h[0][0] > STUCK_VEHICLE_MS * 0.9 && dist(h[0][1], me.pos) < 8) {
        send(`bot_do ${bot.id} maneuver get_out`);
        rec('bail', { bot: me.name, team: me.team, pos: me.pos, vehicle: world.vehicles.find((x) => x.id === me.vehicle)?.name });
        bb.drive = [];
      }
    } else bb.drive = [];
    // What it carries (bots_web.cpp): C4 to plant, a repair gun.
    // Every infantry unit spawns with one timed C4 (Renegade multiplayer):
    // anyone near an enemy MCT plants it; the engineer types (a repair gun
    // and more C4) go for the MCT on purpose.
    bb.hasC4 = hasWeapon(bot, ['c4_timed', 'c4_remote']);
    bb.repairs = hasWeapon(bot, ['repair']);
    bb.c4 = bb.hasC4 && bb.repairs;
    const state = view(bot, me);
    // A push is on: a plan to join it (the decider picks it or not).
    const push = teamPush[me.team];
    if (push && Date.now() - push.at < PUSH_MS && !bb.plans.some((p) => p.id === 'join_push') && world.buildings.some((x) => x.id === push.building && x.hp > 0)) {
      const pb = world.buildings.find((x) => x.id === push.building);
      bb.plans.push({ id: 'join_push', text: `join ${push.by.replace(/^\[BOT\] /, '')}'s push on the enemy ${buildingName(pb)}`, building: push.building });
    }
    if (push && Date.now() - push.at >= PUSH_MS) bb.plans = bb.plans.filter((p) => p.id !== 'join_push');
    // Planner: on spawn, on rethink, or every ~25 s. The scripted plans stand
    // in until the model's arrive (and whenever it fails).
    if (!bb.plans.length) usePlans(bb, scriptedPlan(state, bb.org), state, now, 'scripted');
    // Saved enough: back to base for it first.
    if (bb.saveFor && now - bb.saveFor.since > SAVE_FOR_MS) bb.saveFor = null;
    if (bb.saveFor && me.credits >= bb.saveFor.cost && !bb.plans.some((p) => p.item === bb.saveFor.name) && !me.vehicle) {
      bb.plans.unshift({ id: 'buy_saved', text: `get the ${bb.saveFor.name} I saved for`, item: bb.saveFor.name, place: `the ${me.team} base` });
      bb.current = 'buy_saved';
    }
    if (!wasAlive) {
      bb.why = 'you just (re)spawned';
      bb.rally = {}; // a new life gathers again
      bb.spots = {};
    }
    const routine = wasAlive && !bb.wantPlan && !bb.why;
    if ((!wasAlive || bb.wantPlan || now >= bb.nextPlan) && !bb.planning) {
      bb.nextPlan = now + 22000 + Math.random() * 6000;
      const why = bb.why || 'time to check your plans';
      bb.wantPlan = false;
      bb.why = null;
      if (planner && withinBudget()) {
        bb.planning = true;
        planWithModel(bot.id, bb, state, why, routine).finally(() => (bb.planning = false));
      } else usePlans(bb, scriptedPlan(state, bb.org), state, now, 'scripted');
    }
    const plan = bb.plans.find((p) => p.id === bb.current) || bb.plans[0];
    const objective = objectiveOf(plan, me, bb);
    const questions = questionsFor(state, bb, objective);
    const { world: _w, ...decisionState } = state;
    const t0 = Date.now();
    const modelState = JSON.parse(JSON.stringify(decisionState, (k, v) => (k === 'pos' || k === 'id' ? undefined : v)));
    // Clef reads every token: what a split-second choice needs, no more (the
    // planner has the full picture). Teammates near (by name) and counts
    // elsewhere; the shop only with a buy question; the latest chat.
    const near = modelState.teammates.filter((t) => t.distance <= 40);
    const elsewhere = {};
    for (const t of modelState.teammates) if (t.distance > 40) elsewhere[t.place] = (elsewhere[t.place] || 0) + 1;
    modelState.teammates = near;
    modelState.teammates_elsewhere = elsewhere;
    if (!questions.buy) delete modelState.shop;
    if (Array.isArray(modelState.chat)) modelState.chat = modelState.chat.slice(-3);
    // The plans and the buildings they name are in the questions' options.
    if (modelState.blackboard) delete modelState.blackboard.plans;
    if (Array.isArray(modelState.buildings)) modelState.buildings = modelState.buildings.map((b) => `${b.side} ${b.name} ${b.health}`);
    // What a quiet decision depends on; unchanged, the last answers stand.
    const quiet = !state.enemies_in_sight.length && !(state.alerts || []).length && !state.stuck;
    const key = quiet && JSON.stringify([Object.keys(questions).sort(), Object.values(questions).map((q) => Object.keys(q.criteria || {})), state.me.place, Math.round(state.me.health * 4), state.events.length, (state.chat || []).length, bb.plans.map((p) => p.id), bb.current]);
    const reuse = quiet && key === bb.quietKey && Date.now() - bb.quietAt < QUIET_REUSE_MS && bb.lastDecided;
    const decided = reuse ? bb.lastDecided : model && withinBudget() ? await model(modelState, questions, bb.models.decider) : null;
    if (decided && !reuse) {
      bb.lastDecided = decided;
      bb.quietKey = key;
      bb.quietAt = Date.now();
      if (!quiet) bb.quietKey = null;
    }
    if (reuse) stats.reused = (stats.reused || 0) + 1;
    const by = reuse ? 'reused' : decided ? 'clef' : 'scripted';
    // Questions left out (one option) default to the scripted answer.
    const answers = { ...scriptedDecide(state, questionsFor(state, bb, objective, true)), ...decided };
    const ms = Date.now() - t0;
    if (!bb.alive?.()) return; // the world moved on (bot gone)
    if ((answers.rethink?.probability || 0) > 0.7 && !bb.wantPlan) {
      bb.wantPlan = true;
      bb.why = 'your reflexes say something happened that your plans do not cover';
    }
    act(bot, me, state, bb, answers, objectiveOf(bb.plans.find((p) => p.id === answers.plan?.choice) || plan, me, bb));
    rec('decision', { bot: me.name, team: me.team, by, model: by === 'clef' ? bb.models.decider : undefined, ms, ...(reuse ? {} : { state: modelState, questions }), answers: Object.fromEntries(Object.entries(answers).map(([k, a]) => [k, a.probabilities ?? a.probability])) });
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
      const busy = bot.sees.length || bb.alerted > now;
      const [lo, hi] = busy ? DECIDE_MS : IDLE_DECIDE_MS;
      bb.nextDecide = now + lo + Math.random() * (hi - lo);
      bb.busy = true;
      bb.alive = () => bots.get(bot.id) === bb;
      // Bots think in parallel; each waits only for its own decision.
      think(bot, me, now)
        .catch((e) => out(`[brain] ${me.name}: ${e.stack || e}`))
        .finally(() => (bb.busy = false));
    }
    for (const id of bots.keys()) if (!players.has(id)) {
      bots.delete(id);
      teams.leave(id);
    }
  }

  // Per model: calls, failures, median latency, spend.
  function summary(models = {}) {
    return Object.fromEntries(Object.entries(models).map(([k, m]) => {
      const ms = [...m.ms].sort((a, b) => a - b);
      return [k, { calls: m.calls, failed: m.failed, ...(m.limited !== undefined ? { limited: m.limited, badJson: m.badJson } : {}), p50: ms[ms.length >> 1] || null, usd: +m.usd.toFixed(4) }];
    }));
  }

  out(`[brain] ${mode} brain for the bots${mode === 'hybrid' || mode === 'clef' ? `; planners ${planners}; deciders ${deciders}` : ''}`);
  return {
    // The models' spend so far (the page shows it during a match).
    usage: () => ({ usd: +spent().toFixed(4), budgetUsd, decisions: stats.calls, plans: planStats.calls, planners: planStats.by, cachedShare: planStats.prompt ? +(planStats.cached / planStats.prompt).toFixed(2) : null, models: { planners: summary(planStats.models), deciders: summary(stats.models) }, limits: planner?.limits?.() }),
    onState(json) {
      const map = world?.map;
      const before = world;
      // A state that doesn't parse is skipped (and recorded), never fatal.
      let next;
      try {
        next = JSON.parse(json);
      } catch (e) {
        if (!badStates++) rec('bad_state', { error: String(e.message).slice(0, 300), sample: json.slice(0, 2000) });
        out(`[brain] state ${badStates} didn't parse: ${String(e.message).slice(0, 120)}`);
        return;
      }
      world = next;
      if (world.grid) grid = decodeGrid(world.grid);
      if (world.vgrid) vgrid = decodeGrid(world.vgrid);
      if (world.facts) {
        facts = world.facts; // the weapons' numbers on this map, sent once
        playbookCache.clear();
        jobsCache.clear();
        rec('facts', { map: world.map, facts });
      }
      gameEvents(before, world);
      if (map && world.map !== map) chat.length = 0; // a new map: the lobby's talk is over
      if (!pending) {
        pending = true;
        setImmediate(tick); // outside the game's frame
      }
    },
    onChat(sender, type, text) {
      const from = world?.players.find((p) => p.id === sender);
      chat.push({ t: Date.now(), from: from ? from.name : 'Host', to: type === 1 && from ? from.team : 'all', text });
      out(`[chat] ${chat.at(-1).from}${type === 1 ? ' (team)' : ''}: ${text}`);
      rec('chat', { from: chat.at(-1).from, bot: !!from?.bot, to: chat.at(-1).to, text });
      if (chat.length > 50) chat.shift();
      // Named in chat: that bot thinks (and may answer). Otherwise a team line
      // gets one teammate's attention now and then (humans' lines always).
      const msg = chat.at(-1);
      const listeners = (world?.players || []).filter((p) => p.bot && p.id !== sender && (msg.to === 'all' || p.team === msg.to));
      const named = listeners.filter((p) => text.toLowerCase().includes(p.name.replace(/^\[BOT\]\s*/, '').split(' ').pop().toLowerCase()));
      const human = from && !from.bot;
      const pick = named.length ? named : human || Math.random() < 0.4 ? listeners.sort(() => Math.random() - 0.5).slice(0, 1) : [];
      for (const p of pick) {
        const bb = blackboard(p.id);
        bb.wantPlan = true;
        bb.why = `${msg.from} said in chat: "${text}"`;
      }
    },
  };
}

