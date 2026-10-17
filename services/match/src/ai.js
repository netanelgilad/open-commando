// The bots' models (tools/server/bot-brain.mjs), run through this Worker's
// Workers AI binding so match servers hold no Cloudflare credentials: each
// server gets a key for its own match only (HMAC of the match id with
// START_TOKEN), passed in when the matchmaker starts it.
export const AI_MODELS = {
  'clef-flash': '@cf/cloudflare/clef-flash', // decision model: the moment, 2-4 times a second
  clef: '@cf/cloudflare/clef',
  'deepseek-v4-flash': '@cf/deepseek-ai/deepseek-v4-flash-0731', // language model: plans and talk
};

export async function aiKey(secret, matchId) {
  const key = await crypto.subtle.importKey('raw', new TextEncoder().encode(secret), { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
  const mac = await crypto.subtle.sign('HMAC', key, new TextEncoder().encode(`ai:${matchId}`));
  return [...new Uint8Array(mac)].map((b) => b.toString(16).padStart(2, '0')).join('');
}
