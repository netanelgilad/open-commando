// Under Node (the headless dedicated server, tools/server/run-server.mjs)
// renegade.js is loaded with require(), where its `var Module` is local to the
// CommonJS module: the launcher hands its settings over in a global instead.
if (typeof globalThis.rebelNodeModule === 'object') Object.assign(Module, globalThis.rebelNodeModule);
