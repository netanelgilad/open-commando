#!/usr/bin/env node
// Scripted checks of single bot mechanics on a real server (no models, no
// browser): a hub and a server, bots added by console, then steps that put
// them somewhere and tell them what to do, reading the result back from the
// server console. Each scenario prints PASS or FAIL.
//
//   node tools/server/scenario.mjs [c4|repair|all] [--map C&C_Field.mix]
import { spawn } from 'node:child_process';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { parseArgs } from 'node:util';

const here = dirname(fileURLToPath(import.meta.url));
const { values: opt, positionals } = parseArgs({ allowPositionals: true, options: { map: { type: 'string', default: 'C&C_Field.mix' }, port: { type: 'string', default: '8796' } } });
const which = positionals[0] || 'all';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const hub = spawn(process.execPath, [join(here, 'net-hub.mjs'), '--port', opt.port], { stdio: 'ignore' });
await sleep(800);
const server = spawn(process.execPath, [join(here, 'run-server.mjs'), '--net', `ws://127.0.0.1:${opt.port}/room/scenario`, '--maps', opt.map, '--work', '/tmp/rebel-server-scenario'], { stdio: ['pipe', 'pipe', 'pipe'] });
let log = '';
server.stdout.on('data', (d) => (log += d));
server.stderr.on('data', (d) => (log += d));
const done = () => { server.kill('SIGKILL'); hub.kill(); };
process.on('exit', done);
const say = (line) => server.stdin.write(line + '\n');
const until = async (re, ms = 120000) => {
  const t0 = Date.now();
  while (!re.test(log) && Date.now() - t0 < ms) await sleep(250);
  return re.test(log);
};
// Runs a console command and returns what it printed.
const ask = async (cmd, wait = 1200) => {
  const from = log.length;
  say(cmd);
  await sleep(wait);
  return log.slice(from);
};
const buildings = async () => [...(await ask('bot_buildings')).matchAll(/building (\d+) (\S+) (\S+) hp ([\d.]+)[^\n]*at (\S+) (\S+) (\S+)(?:\n\S* *mct \d+ at (\S+) (\S+) (\S+))?/g)].map((m) => ({ id: +m[1], name: m[2], team: m[3], hp: +m[4], pos: [+m[5], +m[6], +m[7]], mct: m[8] ? [+m[8], +m[9], +m[10]] : null }));

if (!(await until(/Level loaded OK/))) {
  console.log('FAIL: the level did not load');
  process.exit(1);
}
await sleep(3000);
say('bot_brain on'); // bots take bot_do orders only
const results = [];
// Gameplay (and so damage to buildings) needs a player on each team
// (cGameDataCnc::Is_Gameplay_Permitted): a Nod bot to play against.
await ask('bot_add nod 1', 3000);

async function addBot(team) {
  await ask(`bot_add ${team} 1`, 4000);
  const list = await ask("bot_list", 1500);
  if (process.env.DEBUG) console.log("bot_add/list:", JSON.stringify(list.slice(0, 600)));
  const ids = [...list.matchAll(/bot (-\d+) \[BOT\][^\n]*? (GDI|Nod) /g)].map((m) => ({ id: +m[1], team: m[2] }));
  return ids.filter((x) => x.team.toLowerCase() === team.toLowerCase()).at(-1)?.id;
}

if (which === 'c4' || which === 'all') {
  // A GDI bot next to the Nod Refinery's MCT plants its timed C4; the
  // building loses health when it goes off.
  const id = await addBot('gdi');
  const ref = (await buildings()).find((b) => /Nod_Refinery/.test(b.name));
  if (!id || !ref?.mct) results.push(`c4: FAIL (bot ${id}, refinery mct ${JSON.stringify(ref?.mct)})`);
  else {
    await sleep(3000); // spawned
    const at = ref.mct;
    console.log(`c4: bot ${id}, Nod Refinery ${ref.id} at ${ref.hp}, MCT at ${at.map((v) => v.toFixed(1)).join(' ')}`);
    console.log('refinery before:', (await ask('bot_buildings', 1000)).split('\n').find((l) => l.includes(String(ref.id)) && l.includes('health'))?.replace(/^\S+ /, ''));
    console.log((await ask(`bot_do ${id} place ${at[0] + 1.5} ${at[1]} ${at[2]}`, 2000)).trim().split('\n').slice(-2).join(' | '));
    const carried = async (when) => console.log(when, (await ask('bot_list', 1000)).split('\n').filter((l) => /in hand/.test(l)).join(' | '));
    await carried('before:');
    say(`bot_do ${id} c4 ${at[0]} ${at[1]} ${at[2]}`);
    for (let i = 1; i <= 3; i++) {
      await sleep(2000);
      await carried(`+${i * 2} s:`);
    }
    console.log('C4:', (await ask('bot_c4', 1000)).trim().split('\n').map((l) => l.replace(/^\S+ /, '')).join(' | '));
    // Timed C4 goes off after its fuse (some 30 s); the bot walks away meanwhile.
    await ask(`bot_do ${id} go ${at[0] + 40} ${at[1]} ${at[2]}`, 500);
    let after = ref.hp;
    for (let i = 0; i < 12 && after >= ref.hp; i++) {
      await sleep(5000);
      after = (await buildings()).find((b) => b.id === ref.id).hp;
      if (i % 3 === 0) console.log(`  +${(i + 1) * 5} s: refinery ${after};`, (await ask('bot_c4', 800)).trim().split('\n').map((l) => l.replace(/^\S+ /, '')).join(' | '));
    }
    const raw = (await ask('bot_buildings', 1000)).split('\n').find((l) => l.includes(String(ref.id)) && l.includes('health'));
    console.log('refinery now:', raw?.replace(/^\S+ /, ''));
    results.push(`c4: ${after < ref.hp ? 'PASS' : 'FAIL'} (Nod Refinery ${ref.hp} -> ${after})`);
  }
}

if (which === 'walk' || which === 'all') {
  // Can a bot walk into a building to its MCT? Placed 30 m out, told to go.
  const id = await addBot('gdi');
  const all = await buildings();
  // (Its own team's buildings: the enemy's defense kills it on the way.)
  for (const name of ['GDI_Refinery', 'GDI_Barracks', 'GDI_War_Factory', 'GDI_Advanced_Guard_Tower']) {
    const b = all.find((x) => x.name.includes(name));
    if (!b?.mct) continue;
    const start = [b.mct[0] + 30, b.mct[1], b.mct[2] + 1];
    await ask(`bot_do ${id} place ${start.join(' ')}`, 1500);
    await ask(`bot_do ${id} go ${b.mct.join(' ')}`, 500);
    let best = 1e9, at = null;
    for (let i = 0; i < 12; i++) {
      await sleep(2500);
      const line = (await ask('bot_list', 600)).split('\n').find((l) => l.includes(`bot ${id} `));
      const m = / at (\S+) (\S+) (\S+), speed (\S+), waypoint (\d+)\/(\d+)/.exec(line || '');
      if (m) { at = [+m[1], +m[2], +m[3]]; best = Math.min(best, Math.hypot(at[0] - b.mct[0], at[1] - b.mct[1])); if (best < 3) break; }
    }
    results.push(`walk to ${name} MCT: ${best < 4 ? 'PASS' : 'FAIL'} (closest ${best.toFixed(1)} m${at ? `, ended at ${at.map((v) => v.toFixed(1)).join(' ')}` : ''})`);
  }
}

if (which === 'repair' || which === 'all') {
  // A damaged GDI building and a GDI engineer type (Hotwire) by its MCT
  // repairing it: health goes up.
  results.push('repair: (not scripted yet)');
}

for (const r of results) console.log(r);
done();
process.exit(results.some((r) => r.includes('FAIL')) ? 1 : 0);
