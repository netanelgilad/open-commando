// The match maps (the C&C maps of the retail game; the "Flying" variants
// need flying vehicles to make sense and are left out for now). The lobby
// is always LOBBY_MAP; each match picks its map at random.
export const LOBBY_MAP = 'C&C_Field.mix';
export const MATCH_MAPS = [
  'C&C_Canyon.mix', 'C&C_City.mix', 'C&C_Complex.mix', 'C&C_Field.mix', 'C&C_Hourglass.mix',
  'C&C_Islands.mix', 'C&C_Mesa.mix', 'C&C_Under.mix', 'C&C_Volcano.mix', 'C&C_Walls.mix',
];
// Not the lobby map: the lobby tells "back in the lobby" from "in the match"
// by the map the server has loaded.
const PICKS = MATCH_MAPS.filter((m) => m !== LOBBY_MAP);
export const pickMatchMap = (random = Math.random) => PICKS[Math.floor(random() * PICKS.length)];
