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
  // Through Vercel AI Gateway (AI_GATEWAY_API_KEY): planners on its chat
  // endpoint, decision models on its TypeSafe-compatible one (the same typed
  // questions and answers as Clef, noul included).
  'vercel/deepseek-v4-flash': { gateway: 'vercel', kind: 'chat', id: 'deepseek/deepseek-v4-flash' },
  'vercel/gpt-oss-20b': { gateway: 'vercel', kind: 'chat', id: 'openai/gpt-oss-20b' },
  'vercel/d1': { gateway: 'vercel', kind: 'decide', id: 'liquid/d1' },
  'vercel/d1-ultra': { gateway: 'vercel', kind: 'decide', id: 'liquid/d1-ultra' },
  'vercel/decider': { gateway: 'vercel', kind: 'decide', id: 'perplexity/pplx-decider-v1.2' },
  'vercel/decider-flash': { gateway: 'vercel', kind: 'decide', id: 'perplexity/pplx-decider-flash-v1.2' },
  'vercel/decision-1': { gateway: 'vercel', kind: 'decide', id: 'microsoft/microsoft-decision-1' },
  'vercel/jev': { gateway: 'vercel', kind: 'decide', id: 'typesafe-ai/jev' },
};

const VERCEL = 'https://ai-gateway.vercel.sh';

// Runs a model: Workers AI through the binding, Vercel's over HTTPS. Returns
// the model's output in the shape the brain reads (chat: choices + usage;
// decide: answers + usage), or throws with the provider's error.
export async function runModel(env, name, input, options = {}) {
  const m = AI_MODELS[name];
  if (typeof m === 'string') return env.AI.run(m, input, options);
  if (!env.AI_GATEWAY_API_KEY) throw new Error('AI_GATEWAY_API_KEY is not set');
  const headers = { authorization: `Bearer ${env.AI_GATEWAY_API_KEY}`, 'content-type': 'application/json' };
  const res = m.kind === 'chat'
    ? await fetch(`${VERCEL}/v1/chat/completions`, { method: 'POST', headers, body: JSON.stringify({ ...input, model: m.id }) })
    : await fetch(`${VERCEL}/typesafe/v1/systemone`, { method: 'POST', headers, body: JSON.stringify({ model: m.id, state: input.state, questions: input.questions }) });
  const body = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error(`${res.status} ${body.error?.message || body.message || JSON.stringify(body).slice(0, 200)}`);
  return body;
}

export async function aiKey(secret, matchId) {
  const key = await crypto.subtle.importKey('raw', new TextEncoder().encode(secret), { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
  const mac = await crypto.subtle.sign('HMAC', key, new TextEncoder().encode(`ai:${matchId}`));
  return [...new Uint8Array(mac)].map((b) => b.toString(16).padStart(2, '0')).join('');
}
