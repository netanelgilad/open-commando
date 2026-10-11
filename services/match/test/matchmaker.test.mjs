// node --test services/match/test
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { pass, balanceTeams, pingLimit, DEFAULTS } from '../src/matchmaker.js';

let n = 0;
const ticket = (players, ping = 40, waitedS = 0, now = 1e6) => ({
  id: 't' + ++n,
  players: (Array.isArray(players) ? players : [players]).map((mu) => ({ id: 'p' + ++n, mu })),
  pings: { WEUR: ping },
  enqueuedAt: now - waitedS * 1000,
});
const now = 1e6;

test('waits below the minimum, starts at it', () => {
  const few = [25, 25, 25].map((mu) => ticket(mu));
  assert.equal(pass({ tickets: few, matches: [], now }).newMatches.length, 0);
  const eight = [20, 22, 24, 26, 28, 30, 32, 34].map((mu) => ticket(mu));
  const r = pass({ tickets: eight, matches: [], now });
  assert.equal(r.newMatches.length, 1);
  assert.equal(r.newMatches[0].teams[0].length, 4);
  assert.equal(r.newMatches[0].bots, 0);
});

test('starts with bots after waiting, but not for one player alone', () => {
  assert.equal(pass({ tickets: [ticket(25, 40, 60)], matches: [], now }).newMatches.length, 0);
  const r = pass({ tickets: [ticket(25, 40, 60), ticket(25, 40, 10)], matches: [], now });
  assert.equal(r.newMatches.length, 1);
  assert.equal(r.newMatches[0].bots, DEFAULTS.minPlayers - 2);
});

test('ping limit grows while waiting, up to the cap', () => {
  const c = DEFAULTS;
  assert.equal(pingLimit(ticket(25, 0, 0), now, c), 60);
  assert.equal(pingLimit(ticket(25, 0, 25), now, c), 100);
  assert.equal(pingLimit(ticket(25, 0, 600), now, c), 150);
  const far = [25, 25, 25, 25, 25, 25, 25, 25].map((mu) => ticket(mu, 90));
  assert.equal(pass({ tickets: far, matches: [], now }).newMatches.length, 0); // 90 > 60 at first
  const later = far.map((t) => ({ ...t, enqueuedAt: now - 20000 }));
  assert.equal(pass({ tickets: later, matches: [], now }).newMatches.length, 1); // limit 100 after 20 s
});

test('backfill goes first, onto the weaker team', () => {
  const match = { id: 'm1', region: 'WEUR', teams: [['a', 'b'], ['c', 'd']], mus: { a: 40, b: 40, c: 20, d: 20 }, startedAt: now - 60000, durationMs: 30 * 60000, maxPlayers: 16 };
  const r = pass({ tickets: [ticket(25)], matches: [match], now });
  assert.deepEqual(r.backfills.map((b) => [b.matchId, b.team]), [['m1', 1]]);
  assert.equal(r.newMatches.length, 0);
});

test('no backfill into a match more than half played', () => {
  const match = { id: 'm1', region: 'WEUR', teams: [['a'], ['b']], mus: {}, startedAt: now - 20 * 60000, durationMs: 30 * 60000, maxPlayers: 16 };
  assert.equal(pass({ tickets: [ticket(25)], matches: [match], now }).backfills.length, 0);
});

test('parties stay together and teams balance', () => {
  const party = ticket([30, 30]);
  const solos = [10, 50, 20, 40, 25, 35].map((mu) => ticket(mu));
  const teams = balanceTeams([party, ...solos]);
  const ids = teams.map((t) => t.map((x) => x.id));
  assert.ok(ids[0].includes(party.id) || ids[1].includes(party.id));
  const count = (t) => t.reduce((s, x) => s + x.players.length, 0);
  assert.equal(count(teams[0]), 4);
  assert.equal(count(teams[1]), 4);
  const mu = (t) => t.reduce((s, x) => s + x.players.reduce((a, p) => a + p.mu, 0), 0);
  assert.ok(Math.abs(mu(teams[0]) - mu(teams[1])) <= 10, `${mu(teams[0])} vs ${mu(teams[1])}`);
});
