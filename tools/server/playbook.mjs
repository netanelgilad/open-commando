// The planner's Renegade playbook: what a good player knows, so the model
// doesn't have to (small models know little of this game, and what they
// "remember" is often wrong). Two parts:
//
// - CORE: the rules and the common strategies, short, the same for every
//   call (first in the prompt, so it can be cached).
// - The map's sheet (both teams), built once per map from the engine's facts
//   (bots_web.cpp Facts_Json: each weapon's seconds to kill infantry, a
//   Medium Tank and a building, from the game's own damage tables) and the
//   buildings on the map: what each shop item is good for, what the enemy's
//   defenses do, what each enemy building is worth.
//
// Order matters for cost: Workers AI caches a prompt's prefix (cached input
// is ~30x cheaper), so the prompt goes from the most shared to the least:
// CORE and the answer format (every bot, every map), the map's sheet (every
// bot on the map, both teams), then the bot's situation (bot-brain.mjs).

export const CORE = `Renegade C&C mode playbook.
Win: destroy every enemy building (or lead on points at time limit). Buildings never heal by themselves; only engineers repair them.
Buildings (lose one, lose its effect for the rest of the game):
- Power Plant: without it base defenses go dark and everything costs double.
- Refinery (and its harvester): the team's income. A dead harvester pauses it; a dead refinery ends it.
- Barracks / Hand of Nod: without it only the free characters are left.
- Weapons Factory / Airstrip: without it no more vehicles.
- Advanced Guard Tower / Obelisk of Light: the base defense; it shreds anyone in its sight, infantry fastest.
- Every building has a Master Control Terminal (MCT) inside: damage there counts much more, and repairs there are fastest.
Credits: a steady trickle from the refinery plus a share for damage dealt; kept when you die. Characters and vehicles are lost on death (you respawn as the free soldier). Spend on what wins: a tank or artillery is worth many cheap characters, so save for one (a buy plan you can't afford yet makes you save for it); idle credits win nothing.
What works:
- Never walk alone into a defended base; the defense out-ranges and out-damages any single unit. Go with 3+ teammates, from cover, by the routes the defense can't see.
- Take out the base defense first, or the Power Plant (that turns the defense off), then the rest falls fast.
- Finish what's damaged: the whole team on the one most damaged enemy building beats damage spread over many; a building at half health left alone just stays at half.
- Tanks in a group with an engineer repairing them beat a defense; a lone tank dies.
- An APC carries infantry to a building's door; hold the inside, hit the MCT.
- Long-range units (artillery, rocket launchers) can hit buildings from outside the defense's reach; guard them.
- Kill the enemy harvester for cheap: their income stops while it's gone.
- When a building of yours is under attack: kill the attackers first, then repair (engineers).
- Snipers and ramjets kill infantry; they barely scratch buildings and tanks. Rockets, flame, tank shells and volt hit buildings.
- Mix your team: some anti-infantry, some anti-armor; follow up a teammate's push instead of starting your own.`;

// Building names on the map (engine presets) as players say them.
const BUILDING = [
  [/power/i, 'Power Plant'],
  [/refinery/i, 'Refinery'],
  [/barracks/i, 'Barracks'],
  [/hand_of_nod/i, 'Hand of Nod'],
  [/war_factory/i, 'Weapons Factory'],
  [/airstrip/i, 'Airstrip'],
  [/guard_tower|agt/i, 'Advanced Guard Tower'],
  [/obelisk/i, 'Obelisk'],
  [/con(struction)?_?yard/i, 'Construction Yard'],
  [/repair/i, 'Repair Pad'],
  [/helipad/i, 'Helipad'],
  [/silo/i, 'Tiberium Silo'],
  [/comm/i, 'Communications Center'],
];
const buildingName = (preset) => (BUILDING.find(([re]) => re.test(preset)) || [null, preset.replace(/^mp_(GDI_|Nod_)?/, '').replace(/_/g, ' ')])[1];

const fmt = (s) => (s < 0 || s >= 600 ? 'no' : s < 10 ? +s.toFixed(1) : Math.round(s));
const buildingSeconds = (f, i = 0) => Math.min(...Object.values(f.buildings).map((b) => (b[i] < 0 ? Infinity : b[i])));

// The best third of a column (lower is better), among items of one kind.
const third = (values) => {
  const v = values.filter((x) => x >= 0 && Number.isFinite(x)).sort((a, b) => a - b);
  return v.length ? v[Math.floor((v.length - 1) / 3)] : 0;
};

// Items of one kind, one line each: "name price: soldier/tank/building s, tags".
function itemLines(items, facts) {
  const fs = items.map((i) => facts.weapons[i.preset]);
  const live = fs.filter((f) => f.dps >= 0);
  const best = { inf: third(live.map((f) => f.infantry_s)), tank: third(live.map((f) => f.tank_s)), bld: third(live.map((f) => buildingSeconds(f))) };
  return items.map((i, k) => {
    const f = fs[k];
    if (f.dps < 0) return `- ${i.name} ${i.cost}: repairs ${-f.dps} hp/s`;
    const tags = [];
    if (f.infantry_s >= 0 && f.infantry_s <= best.inf) tags.push('anti-infantry');
    if (f.tank_s >= 0 && f.tank_s <= best.tank) tags.push('anti-tank');
    if (buildingSeconds(f) <= best.bld) tags.push('anti-building');
    if (f.range >= 200) tags.push('long range');
    else if (f.range <= 40) tags.push('short range');
    return `- ${i.name} ${i.cost}: ${fmt(f.infantry_s)}/${fmt(f.tank_s)}/${fmt(buildingSeconds(f))}${tags.length ? `, ${tags.join(', ')}` : ''}`;
  });
}

// The map's sheet, the same for both teams (so every bot on the map shares
// the whole prefix): each team's shop (names and prices as the terminal
// shows them) with each item's job, each base's defense and buildings.
export function mapSheet(facts, world) {
  if (!facts?.weapons) return '';
  const lines = ['This map, both teams ("your team" is in your situation). Seconds to kill a soldier/a Medium Tank/a building ("no": not worth trying):'];
  for (const team of ['GDI', 'Nod']) {
    const shop = world.shop?.[team];
    if (!shop) continue;
    const has = (i) => facts.weapons[i.preset];
    // Names repeat in the terminal (two Sydneys, three Black Hands): the price tells them apart.
    lines.push(`${team} characters:`, ...itemLines(shop.characters.filter(has), facts));
    const vehicles = shop.vehicles.filter(has);
    if (vehicles.length) lines.push(`${team} vehicles:`, ...itemLines(vehicles, facts));
    const defenses = Object.entries(facts.weapons).filter(([k]) => (team === 'Nod' ? /obelisk|nod_turret/i : /agt|guard_tower/i).test(k));
    for (const [k, f] of defenses) {
      const what = /obelisk/i.test(k) ? 'Obelisk of Light' : /turret/i.test(k) ? 'turret' : /ceiling/i.test(k) ? 'Guard Tower chaingun' : 'Guard Tower missiles';
      lines.push(`${team} defense, ${what}: kills a soldier in ${fmt(f.infantry_s)} s, a Medium Tank in ${fmt(f.tank_s)} s.`);
    }
    const own = [...new Set((world.buildings || []).filter((b) => b.team === team).map((b) => buildingName(b.name)))];
    if (own.length) lines.push(`${team} buildings: ${own.join(', ')}.${own.includes('Power Plant') ? '' : ' (no Power Plant: the defense can only be destroyed, not switched off)'}`);
  }
  lines.push('Free characters, both teams: soldier, shotgunner, grenadier, engineer (repairs).');
  // How much the MCT multiplies damage, from the same tables.
  const ratios = Object.values(facts.weapons).map((f) => buildingSeconds(f) / buildingSeconds(f, 1)).filter((r) => Number.isFinite(r) && r > 0).sort((a, b) => a - b);
  if (ratios.length) lines.push(`Damage on an MCT counts about ${ratios[Math.floor(ratios.length / 2)].toFixed(0)}x.`);
  return lines.join('\n');
}

// Each shop item's job in a few words, by preset (for the fast decider's
// buy options): the same tags as the sheet.
export function itemJobs(facts, world) {
  const jobs = new Map();
  if (!facts?.weapons) return jobs;
  for (const team of ['GDI', 'Nod']) {
    const shop = world.shop?.[team];
    if (!shop) continue;
    for (const list of [shop.characters, shop.vehicles]) {
      const items = list.filter((i) => facts.weapons[i.preset]);
      itemLines(items, facts).forEach((line, k) => {
        const tags = line.replace(/^- .*?: /, '').replace(/^[\d.no/]+(, )?/, '');
        jobs.set(items[k].preset, tags || 'basic');
      });
    }
  }
  return jobs;
}
