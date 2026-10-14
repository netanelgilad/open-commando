import { test } from 'node:test';
import assert from 'node:assert/strict';
import { lobbyTick } from '../src/lobby.js';

const cfg = { startPlayers: 4, minPlayers: 2, lobbyWaitMs: 60000, countdownMs: 10000, messageEveryMs: 15000 };
const types = (a) => a.map((x) => x.type);

test('one player waits, with a status message', () => {
  const l = { state: 'lobby', players: 1, openedAt: 0 };
  const a = lobbyTick(l, 1000, cfg);
  assert.deepEqual(types(a), ['message']);
  assert.match(a[0].text, /waiting for 1 more/);
  assert.deepEqual(lobbyTick(l, 5000, cfg), []); // not again so soon
  assert.equal(lobbyTick(l, 120000, cfg)[0].type, 'message'); // still one player: keeps waiting
  assert.equal(l.state, 'lobby');
});

test('two players start after the lobby wait (counted from the second player), via a countdown', () => {
  const l = { state: 'lobby', players: 1, openedAt: 0 };
  lobbyTick(l, 1000, cfg);
  l.players = 2;
  assert.match(lobbyTick(l, 100000, cfg)[0].text, /starts at 4 players or in 1:00/);
  lobbyTick(l, 150000, cfg);
  assert.equal(l.state, 'lobby');
  assert.match(lobbyTick(l, 160000, cfg)[0].text, /starting in 10 seconds/);
  assert.equal(l.state, 'countdown');
  assert.deepEqual(lobbyTick(l, 165000, cfg), []);
  assert.deepEqual(types(lobbyTick(l, 170000, cfg)), ['start']);
  assert.equal(l.state, 'live');
});

test('a full lobby starts at once; a player leaving during the countdown cancels it', () => {
  const l = { state: 'lobby', players: 4, openedAt: 0 };
  lobbyTick(l, 1000, cfg);
  assert.equal(l.state, 'countdown');
  l.players = 1;
  assert.match(lobbyTick(l, 2000, cfg)[0].text, /back to the lobby/);
  assert.equal(l.state, 'lobby');
});
