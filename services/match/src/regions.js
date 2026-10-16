// Regions: Cloudflare's container placement regions (the match servers can
// only be pinned to these), with the Durable Object location hint for the
// region's matchmaker and rooms. In order of preference: the page picks the
// first one about as close as its closest (web/rebel.js closestRegion).
export const REGIONS = {
  weur: { name: 'Europe West', container: 'WEUR', binding: 'MATCH_SERVER_WEUR' },
  enam: { name: 'North America East', container: 'ENAM', binding: 'MATCH_SERVER_ENAM' },
  wnam: { name: 'North America West', container: 'WNAM', binding: 'MATCH_SERVER_WNAM' },
  eeur: { name: 'Europe East', container: 'EEUR', binding: 'MATCH_SERVER_EEUR' },
};
// "m-weur-1a2b3c4d" -> "weur" (older ids without a region: weur).
export const regionOf = (id) => {
  const r = /^m-([a-z]{4})-/.exec(id || '')?.[1];
  return REGIONS[r] ? r : 'weur';
};
