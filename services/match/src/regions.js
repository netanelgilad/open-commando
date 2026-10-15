// Regions: Cloudflare's container placement regions (the match servers can
// only be pinned to these), with the Durable Object location hint for the
// region's matchmaker and rooms.
export const REGIONS = {
  weur: { name: 'Europe West', container: 'WEUR', binding: 'MATCH_SERVER_WEUR' },
  eeur: { name: 'Europe East', container: 'EEUR', binding: 'MATCH_SERVER_EEUR' },
  enam: { name: 'North America East', container: 'ENAM', binding: 'MATCH_SERVER_ENAM' },
  wnam: { name: 'North America West', container: 'WNAM', binding: 'MATCH_SERVER_WNAM' },
};
// "m-weur-1a2b3c4d" -> "weur" (older ids without a region: weur).
export const regionOf = (id) => {
  const r = /^m-([a-z]{4})-/.exec(id || '')?.[1];
  return REGIONS[r] ? r : 'weur';
};
