// The lobby around a match (pure; used by the local match service and the
// Durable Objects). A match server opens in warmup on the lobby map: players
// join as soon as they're matched, fight and respawn while it fills. When it
// is full (or has had at least two players for long enough), a short
// countdown runs, then the server ends the warmup and changes to the match
// map; everyone connected moves with it.
//
//   lobbyTick(lobby, now, config) -> actions
//   lobby: { state: 'lobby'|'countdown'|'live', players, openedAt,
//            enoughAt (when it first had minPlayers), countdownAt, lastMessageAt }
//   actions: [{ type: 'warmup' }, { type: 'message', text }, { type: 'start' }]
export const LOBBY_DEFAULTS = {
  startPlayers: 8, // start as soon as this many are in
  minPlayers: 2, // ...or once it has had at least this many for lobbyWaitMs
  lobbyWaitMs: 90000,
  countdownMs: 10000,
  messageEveryMs: 15000,
  botFill: 0, // bots keep the game at this many players (the server does it)
};

const clock = (ms) => {
  const s = Math.max(0, Math.ceil(ms / 1000));
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
};

export function lobbyTick(lobby, now, config = {}) {
  const c = { ...LOBBY_DEFAULTS, ...config };
  const actions = [];
  const say = (text) => {
    actions.push({ type: 'message', text });
    lobby.lastMessageAt = now;
  };
  if (lobby.players >= c.minPlayers) lobby.enoughAt ??= now;
  else lobby.enoughAt = undefined;
  if (lobby.state === 'lobby') {
    const waited = lobby.enoughAt === undefined ? 0 : now - lobby.enoughAt;
    const ready = lobby.players >= c.startPlayers || (lobby.enoughAt !== undefined && waited >= c.lobbyWaitMs);
    if (ready) {
      lobby.state = 'countdown';
      lobby.countdownAt = now;
      say(`Match starting in ${Math.round(c.countdownMs / 1000)} seconds`);
    } else if (lobby.lastMessageAt === undefined || now - lobby.lastMessageAt >= c.messageEveryMs) {
      const need = lobby.players < c.minPlayers
        ? `waiting for ${c.minPlayers - lobby.players} more`
        : `starts at ${c.startPlayers} players or in ${clock(c.lobbyWaitMs - waited)}`;
      const bots = c.botFill > lobby.players ? ' Bots fill the empty slots.' : '';
      say(`Lobby: ${lobby.players} player${lobby.players === 1 ? '' : 's'}, ${need}. Warmup: fight, respawn, bases can't be damaged.${bots}`);
    }
  } else if (lobby.state === 'countdown') {
    if (lobby.players < c.minPlayers) {
      lobby.state = 'lobby';
      say('Not enough players, back to the lobby');
    } else if (now - lobby.countdownAt >= c.countdownMs) {
      lobby.state = 'live';
      actions.push({ type: 'start' });
    }
  }
  return actions;
}
