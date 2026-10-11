// The matching pass (docs/ONLINE_PLAN.md, "Matchmaking design"): a pure
// function the region's matchmaker Durable Object runs every ~1 s.
//
//   pass({ tickets, matches, now, config }) -> { backfills, newMatches, waiting }
//
// ticket: { id, players: [{ id, mu }], pings: { [region]: ms }, enqueuedAt }
//         (a party is one ticket with several players; it is never split)
// match:  { id, region, teams: [[playerId...], [playerId...]], mus: { playerId: mu },
//           startedAt, durationMs, maxPlayers }
//
// Priorities, as Activision describes them: connection first (ping limit),
// then time to match, then skill (only for team balance at low population).
export const DEFAULTS = {
  region: 'WEUR',
  minPlayers: 8, // start a new match at this many humans...
  maxPlayers: 16,
  botFillAfterMs: 45000, // ...or after this long, filling with bots
  minHumansForBots: 2, // never start a match for one human alone
  pingStartMs: 60, // allowed ping to the region, growing while waiting
  pingGrowMsPer10s: 20,
  pingMaxMs: 150,
  backfillUntil: 0.5, // join matches less than half played
};

const size = (t) => t.players.length;
const waitedMs = (t, now) => now - t.enqueuedAt;
export const pingLimit = (t, now, c) => Math.min(c.pingMaxMs, c.pingStartMs + Math.floor(waitedMs(t, now) / 10000) * c.pingGrowMsPer10s);
const fitsPing = (t, now, c) => (t.pings[c.region] ?? Infinity) <= pingLimit(t, now, c);
const teamMu = (team, mus) => team.reduce((s, p) => s + (mus[p] ?? 25), 0);

// Splits tickets into two teams of (nearly) equal size, minimizing the
// difference in summed skill. Parties stay together: greedy by party size
// and skill, then a few swaps of single players that improve the balance.
export function balanceTeams(tickets) {
  const sorted = [...tickets].sort((a, b) => size(b) - size(a) || sumMu(b) - sumMu(a));
  const teams = [[], []];
  const total = tickets.reduce((s, t) => s + size(t), 0);
  const cap = Math.ceil(total / 2);
  const count = (team) => team.reduce((s, t) => s + size(t), 0);
  const mu = (team) => team.reduce((s, t) => s + sumMu(t), 0);
  for (const t of sorted) {
    const fits = [0, 1].filter((i) => count(teams[i]) + size(t) <= cap);
    const i = fits.length === 2 ? (mu(teams[0]) <= mu(teams[1]) ? 0 : 1) : fits.length ? fits[0] : count(teams[0]) <= count(teams[1]) ? 0 : 1;
    teams[i].push(t);
  }
  for (let round = 0; round < 8; round++) {
    let best = null, bestDiff = Math.abs(mu(teams[0]) - mu(teams[1]));
    for (const a of teams[0]) {
      for (const b of teams[1]) {
        if (size(a) !== size(b)) continue;
        const diff = Math.abs(mu(teams[0]) - sumMu(a) + sumMu(b) - (mu(teams[1]) - sumMu(b) + sumMu(a)));
        if (diff < bestDiff - 1e-9) { bestDiff = diff; best = [a, b]; }
      }
    }
    if (!best) break;
    teams[0][teams[0].indexOf(best[0])] = best[1];
    teams[1][teams[1].indexOf(best[1])] = best[0];
  }
  return teams;
}
const sumMu = (t) => t.players.reduce((s, p) => s + (p.mu ?? 25), 0);

export function pass({ tickets, matches, now, config = {} }) {
  const c = { ...DEFAULTS, ...config };
  const waiting = [...tickets].sort((a, b) => a.enqueuedAt - b.enqueuedAt); // oldest first
  const taken = new Set();
  const backfills = [];

  // 1. Backfill: matches with open slots, early enough to be worth joining.
  for (const m of matches) {
    if (m.region !== c.region) continue;
    if (m.durationMs && now - m.startedAt > m.durationMs * c.backfillUntil) continue;
    const teams = m.teams.map((t) => [...t]);
    const mus = { ...m.mus };
    for (const t of waiting) {
      if (taken.has(t.id) || !fitsPing(t, now, c)) continue;
      const humans = teams[0].length + teams[1].length;
      if (humans + size(t) > (m.maxPlayers || c.maxPlayers)) continue;
      // The weaker team (by summed skill; fewer players first).
      const i = teams[0].length !== teams[1].length ? (teams[0].length < teams[1].length ? 0 : 1) : teamMu(teams[0], mus) <= teamMu(teams[1], mus) ? 0 : 1;
      for (const p of t.players) {
        teams[i].push(p.id);
        mus[p.id] = p.mu ?? 25;
      }
      taken.add(t.id);
      backfills.push({ matchId: m.id, ticketId: t.id, team: i });
    }
  }

  // 2. New matches from what's left, oldest ticket first.
  const newMatches = [];
  for (const anchor of waiting) {
    if (taken.has(anchor.id) || !fitsPing(anchor, now, c)) continue;
    const group = [anchor];
    let humans = size(anchor);
    for (const t of waiting) {
      if (t === anchor || taken.has(t.id) || !fitsPing(t, now, c)) continue;
      if (humans + size(t) > c.maxPlayers) continue;
      group.push(t);
      humans += size(t);
    }
    const oldEnough = waitedMs(anchor, now) >= c.botFillAfterMs && humans >= c.minHumansForBots;
    if (humans < c.minPlayers && !oldEnough) continue;
    for (const t of group) taken.add(t.id);
    const teams = balanceTeams(group);
    const bots = Math.max(0, c.minPlayers - humans);
    newMatches.push({
      region: c.region,
      teams: teams.map((team) => team.flatMap((t) => t.players.map((p) => p.id))),
      tickets: group.map((t) => t.id),
      bots, // the server fills these slots with the engine's AI, removed as humans join
    });
  }

  return { backfills, newMatches, waiting: waiting.filter((t) => !taken.has(t.id)).map((t) => t.id) };
}
