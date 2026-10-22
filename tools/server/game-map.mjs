#!/usr/bin/env node
// A game record as a page you can look at (recorder.mjs JSONL in, one
// self-contained HTML file out): the map from the bots' walking grid, with
// layers to toggle (where each team spent its time, deaths, where building
// damage came from, vehicle paths, stuck spots), playback of everyone's
// positions, any one player's path, and charts over time (building health,
// attackers at each enemy base, characters and vehicles in play).
//
//   node tools/server/game-map.mjs <record.jsonl> [out.html]
import { readFileSync, writeFileSync } from 'node:fs';

const [input, output = input.replace(/\.jsonl$/, '') + '-map.html'] = process.argv.slice(2);
const lines = readFileSync(input, 'utf8').split('\n').filter(Boolean).map((l) => JSON.parse(l));
const of = (type) => lines.filter((x) => x.type === type);
const t0 = lines[0]?.t || 0;
const sec = (t) => Math.round((t - t0) / 1000);

// The first map only (a map change starts a new layout).
const snaps = of('snapshot');
const map = snaps[0]?.map;
const end = (snaps.find((s) => s.map !== map) || lines.at(-1)).t;
const inMap = (x) => x.t <= end;
const grid = of('grid').find((g) => g.map === map) || null;

// Names, teams and kinds by id (players are negative for bots).
const who = {};
for (const s of snaps.filter(inMap)) for (const p of s.players) who[p.id] = { name: p.name, team: p.team, bot: p.bot };
const vehicleNames = {};
for (const s of snaps.filter(inMap)) for (const v of s.vehicles) vehicleNames[v.id] = { name: v.name.replace(/^CnC_|^mp_/, ''), team: v.team };

const data = {
  map,
  minutes: Math.round((end - t0) / 60000),
  grid,
  buildings: (snaps[0]?.buildings || []).filter((b) => b.pos).map((b) => ({ id: b.id, team: b.team, name: b.name.replace(/^mp_(GDI_|Nod_)?/, ''), pos: b.pos })),
  // Building health over time.
  health: snaps.filter(inMap).map((s) => [sec(s.t), Object.fromEntries(s.buildings.map((b) => [b.id, Math.round(b.hp * 100)]))]),
  // Positions every 2 s: [t, [[id, x, y, vehicle]...], [[vid, x, y, driver]...]].
  positions: of('positions').filter(inMap).map((p) => [sec(p.t), p.p, p.v]),
  who,
  vehicleNames,
  deaths: of('death').filter(inMap).filter((d) => d.pos).map((d) => [sec(d.t), d.team, d.pos[0], d.pos[1], (d.char || '').replace(/^CnC_/, '')]),
  // Damage to buildings: who (and where they were, from the nearest positions record).
  hits: [],
  kills: [],
  characters: snaps.filter(inMap).map((s) => [sec(s.t), s.players.filter((p) => p.alive).map((p) => [p.team, (p.char || '').replace(/^CnC_/, ''), p.vehicle ? (vehicleNames[p.vehicle]?.name || 'vehicle') : ''])]),
  // Tactics: team pushes, C4 plants, drivers bailing out of stuck vehicles.
  tactics: ['push', 'c4', 'bail'].flatMap((type) => of(type).filter(inMap).filter((x) => x.pos).map((x) => [sec(x.t), type, x.team, x.pos[0], x.pos[1], x.name || x.vehicle || ''])),
  stuck: snaps.filter(inMap).flatMap((s) => s.bots.filter((b) => b.stuck >= 3).map((b) => s.players.find((p) => p.id === b.id)).filter(Boolean).map((p) => [sec(s.t), p.pos[0], p.pos[1], p.vehicle ? 1 : 0])),
};
const posAt = (t, id) => {
  let best = null;
  for (const p of data.positions) {
    if (p[0] > t + 3) break;
    const e = p[1].find((x) => x[0] === id);
    if (e) best = e;
  }
  return best ? [best[1], best[2]] : null;
};
for (const d of of('damage').filter(inMap)) {
  for (const h of d.hits) data.hits.push([sec(d.t), h.building, h.by, h.team, Math.round(h.dealt), Math.round(h.mct), h.what.replace(/^CnC_/, ''), posAt(sec(d.t), h.by)]);
  for (const k of d.kills) data.kills.push([sec(d.t), k.vehicle, k.victim, k.by, k.what.replace(/^CnC_/, ''), k.preset.replace(/^CnC_/, '')]);
}

const html = `<!doctype html><meta charset="utf-8"><title>${map} game map</title>
<style>
body{font:13px system-ui,sans-serif;margin:0;display:flex;height:100vh;background:#111;color:#ddd}
#side{width:340px;overflow:auto;padding:10px;box-sizing:border-box;border-right:1px solid #333}
#main{flex:1;display:flex;flex-direction:column}
canvas{background:#000}
label{display:block;margin:2px 0}
h3{margin:12px 0 4px;font-size:13px;color:#fff}
.small{color:#999;font-size:12px}
table{border-collapse:collapse;font-size:12px}td{padding:1px 6px 1px 0}
select{width:100%}
</style>
<div id="side">
  <b>${map}</b> <span class="small">${data.minutes} min</span>
  <h3>Layers</h3>
  <label><input type="checkbox" id="L_gdi" checked> GDI presence (heat)</label>
  <label><input type="checkbox" id="L_nod" checked> Nod presence (heat)</label>
  <label><input type="checkbox" id="L_deaths" checked> Deaths (dots, team color)</label>
  <label><input type="checkbox" id="L_hits" checked> Building damage: where attackers stood</label>
  <label><input type="checkbox" id="L_vehicles"> Vehicle paths</label>
  <label><input type="checkbox" id="L_stuck"> Stuck spots</label>
  <label><input type="checkbox" id="L_tactics" checked> Pushes (★), C4 (C4), bail-outs (×)</label>
  <label><input type="checkbox" id="L_now" checked> Positions at the time below</label>
  <h3>Time <span id="tlabel"></span></h3>
  <input type="range" id="time" min="0" max="${data.positions.at(-1)?.[0] || 0}" value="0" style="width:100%">
  <button id="play">play</button> <span class="small">window: <input id="win" type="number" value="0" style="width:50px"> s (0 = whole game for the heat)</span>
  <h3>One player's path</h3>
  <select id="who"><option value="">(none)</option></select>
  <h3>Building damage by attacker</h3><div id="dmg"></div>
  <h3>Kills by weapon</h3><div id="kills"></div>
</div>
<div id="main"><canvas id="map"></canvas><canvas id="chart" height="220"></canvas></div>
<script>
const D = ${JSON.stringify(data)};
const $ = (id) => document.getElementById(id);
const map = $('map'), chart = $('chart');
const team = { GDI: [255, 200, 40], Nod: [230, 40, 40] };
// Grid -> world bounds.
const G = D.grid;
const bounds = G ? { minX: G.minX, minY: G.minY, w: G.nx * G.cell, h: G.ny * G.cell } : (() => {
  const xs = D.positions.flatMap((p) => p[1].map((e) => e[1])), ys = D.positions.flatMap((p) => p[1].map((e) => e[2]));
  return { minX: Math.min(...xs) - 20, minY: Math.min(...ys) - 20, w: Math.max(...xs) - Math.min(...xs) + 40, h: Math.max(...ys) - Math.min(...ys) + 40 };
})();
let scale = 1;
function resize() {
  const w = window.innerWidth - 340, h = window.innerHeight - 230;
  scale = Math.min(w / bounds.w, h / bounds.h);
  map.width = Math.round(bounds.w * scale); map.height = Math.round(bounds.h * scale);
  chart.width = w;
  draw();
}
const px = (x, y) => [(x - bounds.minX) * scale, map.height - (y - bounds.minY) * scale];
// The grid as an image, once.
let gridImage = null;
if (G) {
  const c = document.createElement('canvas'); c.width = G.nx; c.height = G.ny;
  const ctx = c.getContext('2d'), img = ctx.createImageData(G.nx, G.ny);
  const color = { '#': [18, 18, 18], '.': [70, 70, 70], '~': [40, 110, 40], '!': [120, 60, 120] };
  G.rows.forEach((row, y) => {
    let x = 0;
    for (const m of row.matchAll(/([#.~!])(\\d+)/g)) for (let k = 0; k < +m[2]; k++, x++) {
      const i = ((G.ny - 1 - y) * G.nx + x) * 4, col = color[m[1]];
      img.data[i] = col[0]; img.data[i + 1] = col[1]; img.data[i + 2] = col[2]; img.data[i + 3] = 255;
    }
  });
  ctx.putImageData(img, 0, 0);
  gridImage = c;
}
function heat(ctx, points, rgb, radius) {
  const layer = document.createElement('canvas'); layer.width = map.width; layer.height = map.height;
  const l = layer.getContext('2d');
  for (const [x, y] of points) {
    const [a, b] = px(x, y);
    const g = l.createRadialGradient(a, b, 0, a, b, radius);
    g.addColorStop(0, 'rgba(255,255,255,0.08)'); g.addColorStop(1, 'rgba(255,255,255,0)');
    l.fillStyle = g; l.fillRect(a - radius, b - radius, radius * 2, radius * 2);
  }
  // Tint the white accumulation.
  l.globalCompositeOperation = 'source-in'; l.fillStyle = 'rgb(' + rgb + ')'; l.fillRect(0, 0, layer.width, layer.height);
  ctx.globalAlpha = 0.75; ctx.drawImage(layer, 0, 0); ctx.globalAlpha = 1;
}
function inWindow(t) { const w = +$('win').value; const now = +$('time').value; return !w || (t <= now && t > now - w); }
function draw() {
  const ctx = map.getContext('2d');
  ctx.fillStyle = '#000'; ctx.fillRect(0, 0, map.width, map.height);
  if (gridImage) { ctx.imageSmoothingEnabled = false; ctx.drawImage(gridImage, 0, 0, map.width, map.height); }
  const now = +$('time').value;
  for (const [name, key] of [['GDI', 'L_gdi'], ['Nod', 'L_nod']]) if ($(key).checked) {
    const pts = [];
    for (const [t, ps] of D.positions) if (inWindow(t)) for (const e of ps) if (D.who[e[0]]?.team === name) pts.push([e[1], e[2]]);
    heat(ctx, pts, team[name].join(','), 10 * scale);
  }
  if ($('L_vehicles').checked) {
    const paths = {};
    for (const [t, , vs] of D.positions) if (inWindow(t)) for (const v of vs) if (v[3]) (paths[v[0]] ||= []).push(v);
    for (const [id, pts] of Object.entries(paths)) {
      const t = D.vehicleNames[id]?.team; ctx.strokeStyle = 'rgba(' + (team[t] || [120, 200, 255]).join(',') + ',0.8)'; ctx.lineWidth = 1.5;
      ctx.beginPath(); pts.forEach((v, i) => { const [a, b] = px(v[1], v[2]); i ? ctx.lineTo(a, b) : ctx.moveTo(a, b); }); ctx.stroke();
    }
  }
  if ($('L_stuck').checked) for (const [t, x, y, veh] of D.stuck) if (inWindow(t)) { const [a, b] = px(x, y); ctx.fillStyle = veh ? '#0ff' : '#f0f'; ctx.fillRect(a - 2, b - 2, 4, 4); }
  if ($('L_tactics').checked) for (const [t, type, tm, x, y] of D.tactics) if (inWindow(t)) {
    const [a, b] = px(x, y); ctx.fillStyle = 'rgb(' + team[tm] + ')'; ctx.font = 'bold 12px system-ui';
    ctx.fillText(type === 'push' ? '★' : type === 'c4' ? 'C4' : '×', a - 4, b + 4);
  }
  if ($('L_deaths').checked) for (const [t, tm, x, y] of D.deaths) if (inWindow(t)) { const [a, b] = px(x, y); ctx.fillStyle = 'rgba(' + team[tm] + ',0.9)'; ctx.beginPath(); ctx.arc(a, b, 2.5, 0, 7); ctx.fill(); }
  if ($('L_hits').checked) for (const h of D.hits) if (inWindow(h[0]) && h[7]) {
    const b = D.buildings.find((x) => x.id === h[1]); const [a1, b1] = px(h[7][0], h[7][1]);
    ctx.strokeStyle = 'rgba(255,255,255,' + Math.min(0.9, 0.1 + h[4] / 200) + ')'; ctx.lineWidth = 1;
    if (b) { const [a2, b2] = px(b.pos[0], b.pos[1]); ctx.beginPath(); ctx.moveTo(a1, b1); ctx.lineTo(a2, b2); ctx.stroke(); }
    ctx.fillStyle = '#fff'; ctx.fillRect(a1 - 1.5, b1 - 1.5, 3, 3);
  }
  // Buildings with health at the time.
  const hp = (D.health.findLast?.((h) => h[0] <= now) || D.health[0] || [0, {}])[1];
  for (const b of D.buildings) {
    const [a, c] = px(b.pos[0], b.pos[1]);
    ctx.fillStyle = 'rgb(' + team[b.team] + ')'; ctx.fillRect(a - 5, c - 5, 10, 10);
    ctx.fillStyle = '#fff'; ctx.font = '11px system-ui'; ctx.fillText(b.name + ' ' + (hp[b.id] ?? '?') + '%', a + 7, c + 4);
  }
  if ($('L_now').checked) {
    const p = D.positions.findLast?.((p) => p[0] <= now);
    if (p) for (const e of p[1]) { const [a, b] = px(e[1], e[2]); const t = D.who[e[0]]?.team; ctx.fillStyle = 'rgb(' + (team[t] || [200, 200, 200]) + ')'; ctx.strokeStyle = '#000';
      ctx.beginPath(); e[3] ? ctx.rect(a - 4, b - 4, 8, 8) : ctx.arc(a, b, 3, 0, 7); ctx.fill(); ctx.stroke(); }
  }
  const one = $('who').value;
  if (one) {
    const pts = D.positions.map((p) => p[1].find((e) => String(e[0]) === one)).filter(Boolean);
    ctx.strokeStyle = '#0f0'; ctx.lineWidth = 2; ctx.beginPath();
    let last = null;
    for (const e of pts) { const [a, b] = px(e[1], e[2]); if (last && Math.hypot(e[1] - last[1], e[2] - last[2]) > 40) ctx.moveTo(a, b); else last ? ctx.lineTo(a, b) : ctx.moveTo(a, b); last = e; }
    ctx.stroke();
  }
  $('tlabel').textContent = Math.floor(now / 60) + ':' + String(now % 60).padStart(2, '0');
  drawChart(now);
}
function drawChart(now) {
  const ctx = chart.getContext('2d'), W = chart.width, H = chart.height, T = D.positions.at(-1)?.[0] || 1;
  ctx.fillStyle = '#151515'; ctx.fillRect(0, 0, W, H);
  const x = (t) => 40 + (t / T) * (W - 50);
  // Building health (lines), 0-100%.
  for (const b of D.buildings) {
    ctx.strokeStyle = 'rgba(' + team[b.team] + ',0.8)'; ctx.beginPath();
    D.health.forEach(([t, h], i) => { const y = 10 + (1 - (h[b.id] ?? 100) / 100) * 90; i ? ctx.lineTo(x(t), y) : ctx.moveTo(x(t), y); }); ctx.stroke();
  }
  ctx.fillStyle = '#999'; ctx.font = '11px system-ui'; ctx.fillText('building health', 2, 12);
  // Attackers near each enemy base (within 60 m of an enemy building): rushes.
  const near = (tm, x0, y0) => D.buildings.some((b) => b.team !== tm && Math.hypot(b.pos[0] - x0, b.pos[1] - y0) < 60);
  for (const tm of ['GDI', 'Nod']) {
    ctx.strokeStyle = 'rgb(' + team[tm] + ')'; ctx.beginPath();
    D.positions.forEach(([t, ps], i) => { const n = ps.filter((e) => D.who[e[0]]?.team === tm && near(tm, e[1], e[2])).length; const y = H - 10 - n * 5; i ? ctx.lineTo(x(t), y) : ctx.moveTo(x(t), y); });
    ctx.stroke();
  }
  ctx.fillStyle = '#999'; ctx.fillText('attackers at the enemy base (5 px each)', 2, H - 95);
  ctx.strokeStyle = '#fff'; ctx.beginPath(); ctx.moveTo(x(now), 0); ctx.lineTo(x(now), H); ctx.stroke();
}
// Side tables.
(function tables() {
  const by = {};
  for (const h of D.hits) { const k = (D.who[h[2]]?.team || '?') + ' ' + h[6]; by[k] = (by[k] || 0) + h[4]; }
  $('dmg').innerHTML = '<table>' + Object.entries(by).sort((a, b) => b[1] - a[1]).slice(0, 25).map(([k, v]) => '<tr><td>' + k + '</td><td>' + v + '</td></tr>').join('') + '</table>';
  const kw = {};
  for (const k of D.kills) { const key = (D.who[k[3]]?.team || '?') + ' ' + k[4]; kw[key] = (kw[key] || 0) + 1; }
  $('kills').innerHTML = '<table>' + Object.entries(kw).sort((a, b) => b[1] - a[1]).slice(0, 25).map(([k, v]) => '<tr><td>' + k + '</td><td>' + v + '</td></tr>').join('') + '</table>';
  const sel = $('who');
  for (const [id, w] of Object.entries(D.who).sort((a, b) => a[1].name.localeCompare(b[1].name))) sel.add(new Option(w.team + ' ' + w.name, id));
})();
for (const id of ['L_gdi', 'L_nod', 'L_deaths', 'L_hits', 'L_vehicles', 'L_stuck', 'L_tactics', 'L_now', 'time', 'win', 'who']) $(id).addEventListener('input', draw);
let timer = null;
$('play').onclick = () => { if (timer) { clearInterval(timer); timer = null; return; } timer = setInterval(() => { const t = $('time'); t.value = Math.min(+t.max, +t.value + 10); draw(); if (+t.value >= +t.max) { clearInterval(timer); timer = null; } }, 100); };
window.onresize = resize;
resize();
</script>`;
writeFileSync(output, html);
console.log(`wrote ${output} (${(html.length / 1e6).toFixed(1)} MB): ${data.positions.length} position samples, ${data.deaths.length} deaths, ${data.hits.length} hits, ${data.kills.length} kills${grid ? '' : ', no grid (an older record)'}`);
