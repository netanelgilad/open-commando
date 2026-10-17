// The match maps (the C&C maps of the retail game; the "Flying" variants
// need flying vehicles to make sense and are left out for now). Each match
// server plays them in its own random order, looping.
export const MATCH_MAPS = [
  'C&C_Canyon.mix', 'C&C_City.mix', 'C&C_Complex.mix', 'C&C_Field.mix', 'C&C_Hourglass.mix',
  'C&C_Islands.mix', 'C&C_Mesa.mix', 'C&C_Under.mix', 'C&C_Volcano.mix', 'C&C_Walls.mix',
];
export function mapRotation(random = Math.random) {
  const maps = [...MATCH_MAPS];
  for (let i = maps.length - 1; i > 0; i--) {
    const j = Math.floor(random() * (i + 1));
    [maps[i], maps[j]] = [maps[j], maps[i]];
  }
  return maps;
}
