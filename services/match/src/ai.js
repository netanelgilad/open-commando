// The bots' models (tools/server/bot-brain.mjs), run through this Worker's
// Workers AI binding so match servers hold no Cloudflare credentials: each
// server gets a key for its own match only (HMAC of the match id with
// START_TOKEN), passed in when the matchmaker starts it.
export const AI_MODELS = {
  'clef-flash': '@cf/cloudflare/clef-flash', // decision model: the moment, 2-4 times a second
  clef: '@cf/cloudflare/clef',
  'clef-omni': '@cf/cloudflare/clef-omni',
  'deepseek-v4-flash': '@cf/deepseek-ai/deepseek-v4-flash-0731', // language model: plans and talk
  // Stand-ins when it's out of capacity (bot-brain.mjs tries them in order).
  'qwen3-30b': '@cf/qwen/qwen3-30b-a3b-fp8',
  'gpt-oss-20b': '@cf/openai/gpt-oss-20b',
  'llama-3.3-70b': '@cf/meta/llama-3.3-70b-instruct-fp8-fast',
  'llama-4-scout': '@cf/meta/llama-4-scout-17b-16e-instruct',
  // More planners: bots are spread over several, each with its own rate limit.
  'glm-5.3-flash': '@cf/zai-org/glm-5.3-flash',
  'glm-4.7-flash': '@cf/zai-org/glm-4.7-flash',
  'gemma-4-26b': '@cf/google/gemma-4-26b-a4b-it',
};

export async function aiKey(secret, matchId) {
  const key = await crypto.subtle.importKey('raw', new TextEncoder().encode(secret), { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
  const mac = await crypto.subtle.sign('HMAC', key, new TextEncoder().encode(`ai:${matchId}`));
  return [...new Uint8Array(mac)].map((b) => b.toString(16).padStart(2, '0')).join('');
}
