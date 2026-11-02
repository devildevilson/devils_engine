#include "graph_view.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

// Визуализатор графа окна: один самодостаточный HTML без внешних библиотек.
//
// Программа пишет данные окна (узлы, рёбра, залы, присутствие существ) как JSON, а
// страница рисует их в canvas в 3D: вращение мышью, приближение колесом, подсказка
// при наведении и подсветка соседей по щелчку. Это инструмент осмотра, а не часть
// генератора: он только читает уже построенное окно и ничего не проверяет.
//
// Особенности:
// - 64-битные ключи узлов уходят строками в шестнадцатеричном виде: число JS теряет
//   точность после 2^53, а домены существ ссылаются на узлы именно по ключу;
// - зал рисуется тремя сечениями номинального эллипсоида (без рельефа) и своей петлёй,
//   ниши — ломаной вход–колено–отвод; всё в мировых координатах;
// - присутствие существа берётся из того же creature_of, что и проверка, поэтому
//   домен на картинке тот же, что проверяется в --verify.

namespace gn09 {

namespace {

point right_of(const hall& h) { return {-h.forward.z, 0, h.forward.x}; }

point to_world(const hall& h, const point local) {
  const point r = right_of(h);
  return {h.centre.x + h.forward.x * local.x + r.x * local.z,
          h.centre.y + local.y,
          h.centre.z + h.forward.z * local.x + r.z * local.z};
}

std::string hex(const uint64_t value) {
  char text[24];
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
  return text;
}

void write_point(std::ostream& out, const point p) {
  out << '[' << std::round(p.x * 100.0) / 100.0 << ',' << std::round(p.y * 100.0) / 100.0 << ','
      << std::round(p.z * 100.0) / 100.0 << ']';
}

void write_points(std::ostream& out, const std::vector<point>& points) {
  out << '[';
  for (size_t i = 0; i < points.size(); ++i) {
    if (i) out << ',';
    write_point(out, points[i]);
  }
  out << ']';
}

// Залы, к которым окно имеет отношение: внутри есть узел окна или центр лежит в окне.
// Остальные залы halls_near лишь дотягиваются полем до края и только загромождают вид.
std::vector<const hall*> visible_halls(const adventure& world) {
  const graph::window& area = world.route.area;
  std::vector<const hall*> result;
  for (const hall& h : world.halls) {
    bool keep = h.centre.x >= area.min_x * sector_size() && h.centre.x < area.max_x * sector_size() &&
                h.centre.z >= area.min_z * sector_size() && h.centre.z < area.max_z * sector_size();
    for (const node& n : world.route.nodes) keep = keep || inside_hall(h, n.centre);
    if (keep) result.push_back(&h);
  }
  return result;
}

std::string world_json(const adventure& world) {
  const std::vector<const hall*> halls = visible_halls(world);
  std::ostringstream out;
  out.precision(10);
  const graph& route = world.route;
  out << "{\"seed\":" << world.seed << ",\"window\":[" << route.area.min_x << ','
      << route.area.min_z << ',' << route.area.max_x << ',' << route.area.max_z
      << "],\"sector\":" << sector_size() << ",\"entry\":" << route.entry
      << ",\"destination\":" << route.destination << ",\"nodes\":[";
  for (size_t i = 0; i < route.nodes.size(); ++i) {
    const node& n = route.nodes[i];
    if (i) out << ',';
    out << "{\"id\":" << n.id << ",\"key\":\"" << hex(n.key) << "\",\"kind\":\"" << name(n.kind)
        << "\",\"rank\":" << int(n.rank) << ",\"biome\":\"" << name(biome_at(world.seed, n.centre))
        << '"';
    if (n.kind != theme::empty) out << ",\"recipe\":\"" << name(recipe_of(world.seed, n)) << '"';
    int inside = -1;
    for (size_t h = 0; h < halls.size() && inside < 0; ++h)
      if (inside_hall(*halls[h], n.centre)) inside = int(h);
    out << ",\"hall\":" << inside << ",\"p\":";
    write_point(out, n.centre);
    out << '}';
  }
  out << "],\"edges\":[";
  for (size_t i = 0; i < route.edges.size(); ++i) {
    if (i) out << ',';
    out << '[' << route.edges[i].a << ',' << route.edges[i].b << ']';
  }
  out << "],\"halls\":[";
  for (size_t i = 0; i < halls.size(); ++i) {
    const hall& h = *halls[i];
    if (i) out << ',';
    out << "{\"key\":\"" << hex(h.key) << "\",\"region\":[" << h.region_x << ',' << h.region_z
        << "],\"kind\":\"" << name(h.kind) << "\",\"resident\":\"" << name(h.resident)
        << "\",\"body\":" << h.body_radius << ",\"size\":[" << h.half_length << ','
        << h.half_height << ',' << h.half_width << "],\"floor\":" << h.floor << ",\"centre\":";
    write_point(out, h.centre);
    out << ",\"forward\":";
    write_point(out, h.forward);
    std::vector<point> loop;
    for (const point& p : h.loop) loop.push_back(to_world(h, p));
    out << ",\"loop\":";
    write_points(out, loop);
    out << ",\"niches\":[";
    for (size_t k = 0; k < h.niches.size(); ++k) {
      const hall_niche& niche = h.niches[k];
      if (k) out << ',';
      out << "{\"overhang\":" << (niche.overhang ? "true" : "false") << ",\"path\":";
      write_points(out, {to_world(h, niche.mouth), to_world(h, niche.elbow), to_world(h, niche.end)});
      out << '}';
    }
    out << "],\"pillars\":" << h.pillars.size();
    const creature_presence presence = creature_of(world.seed, h);
    out << ",\"domain\":{\"halls\":[";
    for (size_t k = 0; k < presence.halls.size(); ++k)
      out << (k ? "," : "") << '"' << hex(presence.halls[k]) << '"';
    out << "],\"nodes\":[";
    for (size_t k = 0; k < presence.nodes.size(); ++k)
      out << (k ? "," : "") << '"' << hex(presence.nodes[k]) << '"';
    out << "],\"passages\":[";
    for (size_t k = 0; k < presence.passages.size(); ++k)
      out << (k ? "," : "") << "[\"" << hex(presence.passages[k].first) << "\",\""
          << hex(presence.passages[k].second) << "\"]";
    out << "],\"refuges\":[";
    for (size_t k = 0; k < presence.refuges.size(); ++k)
      out << (k ? "," : "") << '"' << hex(presence.refuges[k]) << '"';
    out << "]}}";
  }
  out << "]}";
  return out.str();
}

constexpr const char* page_head = R"HTML(<!doctype html>
<html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>GN09 route graph</title>
<style>
:root { --bg:#0b1014; --panel:#121a20; --line:#26343e; --text:#c8d3da; --dim:#7d8b95; --accent:#e6b450; }
html,body { margin:0; height:100%; background:var(--bg); color:var(--text);
  font:13px/1.4 system-ui, sans-serif; overflow:hidden; }
canvas { display:block; width:100vw; height:100vh; cursor:grab; }
canvas.drag { cursor:grabbing; }
#panel { position:fixed; top:12px; left:12px; width:290px; max-height:calc(100vh - 24px); overflow:auto;
  background:var(--panel); border:1px solid var(--line); border-radius:6px; padding:10px 12px; }
#panel h1 { font-size:15px; margin:0 0 4px; }
#panel .sub { color:var(--dim); margin-bottom:8px; }
#panel label { display:block; margin:2px 0; cursor:pointer; }
#panel button { background:#1b2730; color:var(--text); border:1px solid var(--line); border-radius:4px;
  padding:3px 8px; margin:2px 2px 2px 0; cursor:pointer; }
#panel button:hover { border-color:var(--accent); }
#panel input[type=range] { width:100%; }
#legend span { display:inline-block; width:10px; height:10px; border-radius:50%; margin-right:5px; vertical-align:-1px; }
#info { margin-top:8px; border-top:1px solid var(--line); padding-top:8px; white-space:pre-wrap; }
#info a { color:var(--accent); cursor:pointer; text-decoration:none; }
#tip { position:fixed; pointer-events:none; background:#000c; border:1px solid var(--line); border-radius:4px;
  padding:4px 7px; white-space:pre; display:none; }
</style></head><body>
<canvas id="view"></canvas>
<div id="panel">
  <h1>GN09 route graph</h1>
  <div class="sub" id="summary"></div>
  <div id="legend"></div>
  <div style="margin-top:8px">
    <button id="iso">3D</button><button id="top">Top (X/Z)</button><button id="side">Side (X/Y)</button><button id="fit">Fit</button>
  </div>
  <label>Vertical scale <span id="vlabel">1.0</span>x<input id="vscale" type="range" min="0.25" max="4" step="0.25" value="1"></label>
  <label><input type="checkbox" id="showHalls" checked> Halls</label>
  <label><input type="checkbox" id="showLoops" checked> Creature loops</label>
  <label><input type="checkbox" id="showNiches" checked> Niches</label>
  <label><input type="checkbox" id="showDomains"> Creature domains</label>
  <label><input type="checkbox" id="showSectors" checked> Sector grid</label>
  <label><input type="checkbox" id="showIds"> Node ids</label>
  <div style="color:var(--dim);margin-top:6px">Drag: rotate · Shift+drag: pan · Wheel: zoom · Click a node: neighbours</div>
  <div id="info"></div>
</div>
<div id="tip"></div>
<script>
const DATA = )HTML";

constexpr const char* page_tail = R"HTML(;
const KIND = { empty:'#8a959e', outpost:'#5fc46a', derelict:'#c4683f', grotto:'#4f9be0', vent:'#e0a03a' };
const RESIDENT = { lord:'#d94fd0', beast:'#e8873a', none:'#6fb7c9' };
const nodes = DATA.nodes, edges = DATA.edges, halls = DATA.halls;
const byKey = new Map(nodes.map(n => [n.key, n]));
const adjacent = nodes.map(() => []);
edges.forEach(([a, b], i) => { adjacent[a].push({ other:b, edge:i }); adjacent[b].push({ other:a, edge:i }); });

const canvas = document.getElementById('view'), ctx = canvas.getContext('2d');
const $ = id => document.getElementById(id);
const home = () => [160 * devicePixelRatio, 0];
let yaw = -0.7, pitch = 0.55, zoom = 1, pan = home(), vscale = 1, selected = -1, hovered = -1;

// Centre and extent of everything drawn.
const all = nodes.map(n => n.p);
const lo = [0,1,2].map(k => Math.min(...all.map(p => p[k]))), hi = [0,1,2].map(k => Math.max(...all.map(p => p[k])));
const centre = [0,1,2].map(k => (lo[k] + hi[k]) / 2);
const extent = Math.max(hi[0] - lo[0], hi[2] - lo[2], (hi[1] - lo[1]) * 1.2, 50);

$('summary').textContent = `seed ${DATA.seed} · sectors [${DATA.window[0]},${DATA.window[1]})–[${DATA.window[2]},${DATA.window[3]}) · `
  + `${nodes.length} nodes · ${edges.length} edges · ${halls.length} halls · y ${lo[1].toFixed(0)}..${hi[1].toFixed(0)} m`;
$('legend').innerHTML = Object.entries(KIND).map(([k, c]) => `<div><span style="background:${c}"></span>${k}</div>`).join('')
  + `<div><span style="background:#fff;outline:2px solid var(--accent)"></span>entry / destination</div>`
  + `<div><span style="background:transparent;outline:1px solid #6fb7c9"></span>node inside a hall</div>`;

function project(p) {
  const x = p[0] - centre[0], y = (p[1] - centre[1]) * vscale, z = p[2] - centre[2];
  const cy = Math.cos(yaw), sy = Math.sin(yaw), cp = Math.cos(pitch), sp = Math.sin(pitch);
  const rx = x * cy - z * sy, rz = x * sy + z * cy;
  const ry = y * cp - rz * sp, depth = y * sp + rz * cp;
  const scale = Math.min(canvas.width, canvas.height) / extent * 0.9 * zoom;
  return [canvas.width / 2 + rx * scale + pan[0], canvas.height / 2 - ry * scale + pan[1], depth, scale];
}

function hallPoint(h, u, y, v) {
  const f = h.forward, r = [-f[2], 0, f[0]];
  return [h.centre[0] + f[0] * u + r[0] * v, h.centre[1] + y, h.centre[2] + f[2] * u + r[2] * v];
}

function polyline(points, closed, colour, width, dash) {
  ctx.beginPath();
  points.forEach((p, i) => { const s = project(p); i ? ctx.lineTo(s[0], s[1]) : ctx.moveTo(s[0], s[1]); });
  if (closed) ctx.closePath();
  ctx.strokeStyle = colour; ctx.lineWidth = width; ctx.setLineDash(dash || []); ctx.stroke(); ctx.setLineDash([]);
}

function ellipse(h, plane) {
  const [L, H, W] = h.size, points = [];
  for (let i = 0; i < 48; ++i) {
    const a = i / 48 * Math.PI * 2, c = Math.cos(a), s = Math.sin(a);
    points.push(plane === 0 ? hallPoint(h, L * c, 0, W * s) : plane === 1 ? hallPoint(h, L * c, H * s, 0) : hallPoint(h, 0, H * s, W * c));
  }
  return points;
}

function drawSectors() {
  const [x0, z0, x1, z1] = DATA.window, size = DATA.sector, y = lo[1];
  ctx.globalAlpha = 0.35;
  for (let x = x0; x <= x1; ++x) polyline([[x * size, y, z0 * size], [x * size, y, z1 * size]], false, '#2c3b45', 1);
  for (let z = z0; z <= z1; ++z) polyline([[x0 * size, y, z * size], [x1 * size, y, z * size]], false, '#2c3b45', 1);
  ctx.globalAlpha = 1;
}

function domainOf(h) { return h.domain; }

function draw() {
  ctx.fillStyle = '#0b1014'; ctx.fillRect(0, 0, canvas.width, canvas.height);
  if ($('showSectors').checked) drawSectors();

  if ($('showHalls').checked) halls.forEach(h => {
    const colour = RESIDENT[h.resident];
    ctx.globalAlpha = 0.55;
    for (let k = 0; k < 3; ++k) polyline(ellipse(h, k), true, colour, 1, k ? [4, 4] : []);
    ctx.globalAlpha = 1;
    if ($('showLoops').checked && h.loop.length) polyline(h.loop, true, colour, 2);
    if ($('showNiches').checked) h.niches.forEach(n => polyline(n.path, false, n.overhang ? '#f2d16b' : '#9fd36b', 1.5));
  });

  // Domain passages under the ordinary edges.
  const passage = new Map();
  if ($('showDomains').checked) halls.forEach(h => domainOf(h).passages.forEach(([a, b]) => {
    passage.set(a + b, RESIDENT[h.resident]); passage.set(b + a, RESIDENT[h.resident]);
  }));

  const highlight = new Set(), highlightEdges = new Set();
  if (selected >= 0) { highlight.add(selected); adjacent[selected].forEach(x => { highlight.add(x.other); highlightEdges.add(x.edge); }); }

  edges.forEach(([a, b], i) => {
    const pa = nodes[a].p, pb = nodes[b].p, key = nodes[a].key + nodes[b].key;
    let colour = '#4c5d68', width = 1.5;
    if (passage.has(key)) { colour = passage.get(key); width = 4; }
    if (selected >= 0) { if (highlightEdges.has(i)) { colour = '#e6b450'; width = 3; } else { ctx.globalAlpha = 0.25; } }
    polyline([pa, pb], false, colour, width);
    ctx.globalAlpha = 1;
  });

  const refuges = new Set(), domainNodes = new Set();
  if ($('showDomains').checked) halls.forEach(h => { domainOf(h).refuges.forEach(k => refuges.add(k)); domainOf(h).nodes.forEach(k => domainNodes.add(k)); });

  const order = nodes.map((n, i) => [project(n.p)[2], i]).sort((x, y) => x[0] - y[0]);
  for (const [, i] of order) {
    const n = nodes[i], s = project(n.p);
    const base = n.kind === 'outpost' || n.kind === 'derelict';
    let r = base ? 6 : n.kind === 'empty' ? 3 : 4.5;
    if (i === hovered) r += 2;
    ctx.globalAlpha = selected >= 0 && !highlight.has(i) ? 0.3 : 1;
    ctx.beginPath(); ctx.arc(s[0], s[1], r, 0, Math.PI * 2);
    ctx.fillStyle = KIND[n.kind] || '#fff'; ctx.fill();
    if (n.hall >= 0) { ctx.strokeStyle = RESIDENT[halls[n.hall].resident]; ctx.lineWidth = 1.5; ctx.stroke(); }
    if (i === DATA.entry || i === DATA.destination) {
      ctx.beginPath(); ctx.arc(s[0], s[1], r + 4, 0, Math.PI * 2); ctx.strokeStyle = '#e6b450'; ctx.lineWidth = 2; ctx.stroke();
    }
    if (refuges.has(n.key)) { ctx.beginPath(); ctx.arc(s[0], s[1], r + 3, 0, Math.PI * 2); ctx.strokeStyle = '#7ee08a'; ctx.lineWidth = 2; ctx.setLineDash([3, 2]); ctx.stroke(); ctx.setLineDash([]); }
    else if (domainNodes.has(n.key)) { ctx.beginPath(); ctx.arc(s[0], s[1], r + 2, 0, Math.PI * 2); ctx.strokeStyle = '#d94fd0'; ctx.lineWidth = 1; ctx.stroke(); }
    if ($('showIds').checked || i === selected) { ctx.fillStyle = '#c8d3da'; ctx.fillText(n.id, s[0] + r + 2, s[1] - r); }
    ctx.globalAlpha = 1;
  }
}

function describe(i) {
  const n = nodes[i];
  let text = `node ${n.id}  ${n.kind}${n.recipe ? ' (' + n.recipe + ')' : ''}\n`
    + `key ${n.key}\nrank ${n.rank}, degree ${adjacent[i].length}\nbiome ${n.biome}\n`
    + `x ${n.p[0].toFixed(1)}  y ${n.p[1].toFixed(1)}  z ${n.p[2].toFixed(1)}\n`
    + `sector ${Math.floor(n.p[0] / DATA.sector)},${Math.floor(n.p[2] / DATA.sector)}`;
  if (n.hall >= 0) { const h = halls[n.hall]; text += `\ninside hall ${n.hall}: ${h.kind}, resident ${h.resident}`; }
  if (i === DATA.entry) text += '\nentry';
  if (i === DATA.destination) text += '\ndestination';
  return text;
}

function hallText(h, k) {
  const d = h.domain;
  return `hall ${k}: ${h.kind}, region ${h.region}, resident ${h.resident} (body ${h.body.toFixed(1)} m)\n`
    + `  ${(2 * h.size[0]).toFixed(0)} x ${(2 * h.size[2]).toFixed(0)} x ${(2 * h.size[1]).toFixed(0)} m, y ${h.centre[1].toFixed(0)}, `
    + `${h.pillars} pillars, ${h.niches.length} niches\n  domain: ${d.halls.length} halls, ${d.nodes.length} nodes, `
    + `${d.passages.length} passages, ${d.refuges.length} refuges`;
}

function showInfo() {
  const info = $('info');
  if (selected < 0) { info.textContent = halls.map(hallText).join('\n'); return; }
  info.innerHTML = '';
  info.appendChild(document.createTextNode(describe(selected) + '\n\nneighbours:\n'));
  adjacent[selected].forEach(({ other }) => {
    const a = document.createElement('a'), n = nodes[other];
    const p = nodes[selected].p, q = n.p, length = Math.hypot(q[0] - p[0], q[1] - p[1], q[2] - p[2]);
    a.textContent = `  ${n.id} ${n.kind}  ${length.toFixed(0)} m, dy ${(q[1] - p[1]).toFixed(0)}\n`;
    a.onclick = () => { selected = other; showInfo(); draw(); };
    info.appendChild(a);
  });
}

function pick(x, y) {
  let best = -1, distance = 10;
  nodes.forEach((n, i) => { const s = project(n.p), d = Math.hypot(s[0] - x, s[1] - y); if (d < distance) { distance = d; best = i; } });
  return best;
}

function resize() { canvas.width = innerWidth * devicePixelRatio; canvas.height = innerHeight * devicePixelRatio; ctx.font = `${11 * devicePixelRatio}px system-ui`; draw(); }
addEventListener('resize', resize);

let drag = null, moved = false;
canvas.addEventListener('mousedown', e => { drag = [e.clientX, e.clientY, e.shiftKey]; moved = false; canvas.classList.add('drag'); });
addEventListener('mouseup', e => {
  canvas.classList.remove('drag');
  if (drag && !moved && e.target === canvas) { selected = pick(e.clientX * devicePixelRatio, e.clientY * devicePixelRatio); showInfo(); draw(); }
  drag = null;
});
addEventListener('mousemove', e => {
  if (drag) {
    const dx = e.clientX - drag[0], dy = e.clientY - drag[1];
    if (Math.abs(dx) + Math.abs(dy) > 2) moved = true;
    if (drag[2]) { pan[0] += dx * devicePixelRatio; pan[1] += dy * devicePixelRatio; }
    else { yaw += dx * 0.008; pitch = Math.max(-1.55, Math.min(1.55, pitch + dy * 0.008)); }
    drag[0] = e.clientX; drag[1] = e.clientY; draw(); return;
  }
  const i = pick(e.clientX * devicePixelRatio, e.clientY * devicePixelRatio), tip = $('tip');
  if (i !== hovered) { hovered = i; draw(); }
  if (i >= 0) { tip.style.display = 'block'; tip.style.left = e.clientX + 14 + 'px'; tip.style.top = e.clientY + 10 + 'px'; tip.textContent = describe(i); }
  else tip.style.display = 'none';
});
canvas.addEventListener('wheel', e => { e.preventDefault(); zoom *= Math.exp(-e.deltaY * 0.0015); draw(); }, { passive:false });
$('vscale').oninput = e => { vscale = +e.target.value; $('vlabel').textContent = vscale.toFixed(2); draw(); };
['showHalls', 'showLoops', 'showNiches', 'showDomains', 'showSectors', 'showIds'].forEach(id => $(id).onchange = draw);
$('iso').onclick = () => { yaw = -0.7; pitch = 0.55; draw(); };
$('top').onclick = () => { yaw = 0; pitch = Math.PI / 2 - 0.001; draw(); };
$('side').onclick = () => { yaw = 0; pitch = 0; draw(); };
$('fit').onclick = () => { zoom = 1; pan = home(); draw(); };
addEventListener('keydown', e => { if (e.key === 'Escape') { selected = -1; showInfo(); draw(); } });
showInfo(); resize();
</script></body></html>
)HTML";

} // namespace

bool write_graph_html(const adventure& world, const std::string& path) {
  std::ofstream out(path, std::ios::binary);
  if (!out) return false;
  out << page_head << world_json(world) << page_tail;
  return bool(out);
}

} // namespace gn09
