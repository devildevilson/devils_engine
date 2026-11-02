-- Общий модуль цепочки GN09: правила, которые нужны нескольким генераторам, в ОДНОЙ копии. Тело шага
-- видит его как `modules.world` (документ цепочки объявляет модули; `require` у генератора нет).
--
-- Хеши и шум повторяют мир хоста бит в бит: тот же utils::mix (base.mix), тот же порядок операций с
-- double. Формулы записаны ровно в том порядке, в каком их считал C++.

local world = {}

world.sector_size = constants.sector_size
world.region_size = constants.region_size
world.world_y_min, world.world_y_max = constants.world_y_min, constants.world_y_max
-- Самый длинный зал с запасом на рельеф и ниши: дальше этого его поле — скала.
world.hall_reach = constants.hall_reach
-- Колонна зала у пола и потолка шире: radius * (1 + 0.35 (y/H)^2).
world.pillar_flare = constants.pillar_flare

function world.clamp(v, lo, hi)
  if v < lo then return lo end
  if hi < v then return hi end
  return v
end

function world.node_key(x, z, direction, step, branch)
  return base.mix(0x474e3039, x, z, direction, step, branch or 0)
end

function world.random01(seed, a, b)
  return base.prng64_normalize(base.mix(seed, a, b or 0))
end

function world.is_base(kind) return kind == 1 or kind == 2 end
function world.natural(kind) return kind == 3 or kind == 4 end

-- 3D value noise с ключом — мировым узлом решётки.
function world.value_noise(seed, x, y, z, period)
  local px, py, pz = x / period, y / period, z / period
  local ix, iy, iz = math.floor(px), math.floor(py), math.floor(pz)
  local function smooth(t) return t * t * (3.0 - 2.0 * t) end
  local fx, fy, fz = smooth(px - ix), smooth(py - iy), smooth(pz - iz)
  local function sample(dx, dy, dz)
    return 2.0 * base.prng64_normalize(base.mix(seed, ix + dx, iy + dy, iz + dz)) - 1.0
  end
  local function lerp(a, b, t) return a + (b - a) * t end
  local bottom = lerp(lerp(sample(0, 0, 0), sample(1, 0, 0), fx), lerp(sample(0, 1, 0), sample(1, 1, 0), fx), fy)
  local top = lerp(lerp(sample(0, 0, 1), sample(1, 0, 1), fx), lerp(sample(0, 1, 1), sample(1, 1, 1), fx), fy)
  return lerp(bottom, top, fz)
end

-- Рельеф слоя: 200/90/30 м на периодах 1500/520/190 м. По нему стоят базы, узлы и залы.
function world.layer_height(seed, x, z)
  return 200.0 * world.value_noise(seed + 11, x, 0.0, z, 1500.0) +
         90.0 * world.value_noise(seed + 12, x, 0.0, z, 520.0) +
         30.0 * world.value_noise(seed + 13, x, 0.0, z, 190.0)
end

-- Залы из буферов генератора halls (по экземпляру на регион): поля, нужные правилам макрографа.
function world.read_halls(links)
  local out = {}
  for _, region in ipairs(links) do
    local halls, pillars = region.halls, region.hall_pillars
    local count = math.tointeger(region.hall_counts:field("halls"):get(0))
    local centre, forward, half = halls:field("centre"), halls:field("forward"), halls:field("half")
    local floor, first, many = halls:field("floor"), halls:field("first_pillar"), halls:field("pillar_count")
    local pu, pv, pr = pillars:field("u"), pillars:field("v"), pillars:field("radius")
    for i = 0, count - 1 do
      local h = {
        cx = centre:get(i, 0), cy = centre:get(i, 1), cz = centre:get(i, 2),
        fx = forward:get(i, 0), fy = forward:get(i, 1), fz = forward:get(i, 2),
        hl = half:get(i, 0), hw = half:get(i, 1), hh = half:get(i, 2),
        floor = floor:get(i), pillars = {},
      }
      local f0 = math.tointeger(first:get(i))
      for k = f0, f0 + math.tointeger(many:get(i)) - 1 do
        h.pillars[#h.pillars + 1] = { u = pu:get(k), v = pv:get(k), radius = pr:get(k) }
      end
      out[#out + 1] = h
    end
  end
  return out
end

-- to_local: (dot(d, forward), d.y, dot(d, right)), right = (-forward.z, 0, forward.x).
function world.hall_to_local(h, x, y, z)
  local dx, dy, dz = x - h.cx, y - h.cy, z - h.cz
  local u = dx * h.fx + dy * h.fy + dz * h.fz
  local v = dx * -h.fz + dy * 0.0 + dz * h.fx
  return u, dy, v
end

-- to_world: centre + ((forward * u + right * v) + (0, y, 0)).
function world.hall_to_world(h, u, y, v)
  local rx, ry, rz = -h.fz, 0.0, h.fx
  return h.cx + ((h.fx * u + rx * v) + 0.0), h.cy + ((h.fy * u + ry * v) + y), h.cz + ((h.fz * u + rz * v) + 0.0)
end

function world.inside_hall(h, x, y, z)
  local u, dy, v = world.hall_to_local(h, x, y, z)
  local a, b, c = u / h.hl, dy / h.hh, v / h.hw
  return a * a + b * b + c * c < 1.0 and dy > h.floor
end

-- Якоря станций окна, как у хоста station_anchors: базы графа в порядке узлов, затем аванпосты залов
-- окна в порядке регионов. nodes/meta — буферы графа, regions — fan-in генератора halls.
-- Якорь: key, centre, kind (1 outpost, 2 derelict), recipe (3 mining, 4 residential, 5 military),
-- preferred (-1 дно, 0 стена, 1 свод), hall (0 у базы).
function world.station_anchors(seed, nodes, meta, regions)
  local out = {}
  local n_key, n_centre, n_kind = nodes:field("key"), nodes:field("centre"), nodes:field("kind")
  for i = 0, math.tointeger(meta:field("nodes"):get(0)) - 1 do
    local kind = math.tointeger(n_kind:get(i))
    if world.is_base(kind) then
      local key = n_key:get(i)
      local centre = { x = n_centre:get(i, 0), y = n_centre:get(i, 1), z = n_centre:get(i, 2) }
      local recipe = 3 + base.umod(base.mix(seed, key, 0x524543495045), 3)
      local pose = base.umod(base.mix(seed, key, 0x444f434b), 10)
      local preferred
      if recipe == 3 then preferred = pose < 6 and 0 or 1
      elseif recipe == 4 then preferred = pose < 8 and -1 or 0
      else
        local span = 2.0 * world.sector_size
        local biome = base.umod(base.mix(seed, math.floor(centre.x / span), math.floor(centre.z / span), 0x42494f4d45), 3)
        local side_limit = biome == 0 and 6 or (biome == 1 and 4 or 2)
        preferred = pose < side_limit and 0 or (pose < side_limit + (10 - side_limit) // 2 and 1 or -1)
      end
      out[#out + 1] = { key = key, centre = centre, kind = kind, recipe = recipe, preferred = preferred, hall = 0 }
    end
  end
  for _, region in ipairs(regions) do
    local o = region.hall_outposts
    local ok, oc = o:field("key"), o:field("centre")
    for k = 0, math.tointeger(region.hall_counts:field("outposts"):get(0)) - 1 do
      out[#out + 1] = { key = ok:get(k), centre = { x = oc:get(k, 0), y = oc:get(k, 1), z = oc:get(k, 2) },
                        kind = math.tointeger(o:field("kind"):get(k)), recipe = math.tointeger(o:field("recipe"):get(k)),
                        preferred = math.tointeger(o:field("preferred"):get(k)), hall = o:field("hall"):get(k) }
    end
  end
  return out
end

-- ------------------------------------------------------------------------------------------------
-- ГАЛЕРЕЯ — один генератор ходов на два масштаба: маршрут лодки (scale = 1) и ходы пещер ныряльщиков
-- (меньше). Строка — поля буфера `galleries` (формула — shared/field/01_gallery.ds), плюс две смещённые полости
-- (буфер `lobes`, shared/field/03_lobe.ds).
--   a, b        концы оси (порты); key — хеш ребра (вся случайность из него);
--   o.scale     масштаб сечения и полостей;
--   o.core      гарантированное ядро (радиус капсулы, всегда свободной);
--   o.biome     0 karst, 1 rift, 2 chimney; o.rock — вид породы для палитры;
--   o.section   { width, roof } — ТОЧНОЕ сечение без импульсов и без полостей (горло пещеры);
--   o.constriction  true/false — сужение (неровность стенки в четверть); nil — по хешу;
--   o.lobes     false — без смещённых полостей.
local function v_add(a, b) return { x = a.x + b.x, y = a.y + b.y, z = a.z + b.z } end
local function v_sub(a, b) return { x = a.x - b.x, y = a.y - b.y, z = a.z - b.z } end
local function v_scale(a, s) return { x = a.x * s, y = a.y * s, z = a.z * s } end
local function v_dot(a, b) return a.x * b.x + a.y * b.y + a.z * b.z end
local function v_length(a) return math.sqrt(v_dot(a, a)) end
local function v_cross(a, b) return { x = a.y * b.z - a.z * b.y, y = a.z * b.x - a.x * b.z, z = a.x * b.y - a.y * b.x } end
world.v_add, world.v_sub, world.v_scale, world.v_dot, world.v_length, world.v_cross =
  v_add, v_sub, v_scale, v_dot, v_length, v_cross

function world.gallery(seed, key, a, b, o)
  local s = o.scale or 1.0
  local function r(n) return world.random01(seed, key, n) end
  local ab = v_sub(b, a)
  local distance = v_length(ab)
  local axis = v_scale(ab, 1.0 / distance)
  local reference = math.abs(axis.y) < 0.85 and { x = 0.0, y = 1.0, z = 0.0 } or { x = 1.0, y = 0.0, z = 0.0 }
  local right = v_scale(v_cross(axis, reference), 1.0 / v_length(v_cross(axis, reference)))
  local up = v_cross(right, axis)
  local biome = o.biome or 0
  local row = { port_a = a, port_b = b, axis = axis, right = right, up = up, distance = distance,
                core_radius = o.core, biome = biome, rock = o.rock or (1 + biome), lobes = {}, route_radius = 0.0 }
  if o.section then
    row.first_t, row.second_t = -10.0, -10.0
    row.base_width, row.first_width, row.second_width = o.section.width, 0.0, 0.0
    row.base_roof, row.vertical_gain, row.biome_width, row.pulse = o.section.roof, 0.0, 1.0, 0.0
    row.constriction = 1
    -- Дальше коробки значение формы больше фоновой скалы (3.46) с запасом на неровность сужения.
    local least = math.min(o.section.width, o.section.roof)
    row.reach = math.max(o.section.width, o.section.roof) * (1.0 + 3.8 / least) + 1.0
    return row
  end
  local vertical_gain = (biome == 0 and 0.0 or (biome == 1 and 14.0 or 22.0)) * s
  local biome_width = biome == 2 and 0.82 or 1.0
  local constriction = o.constriction
  if constriction == nil then constriction = base.umod(base.mix(key, 1), 6) == 0 end
  local first_t = 0.25 + 0.12 * r(2)
  local second_t = 0.65 + 0.12 * r(3)
  local base_width = (constriction and 7.4 or 9.0 + 2.0 * r(4)) * s
  local first_width = (3.0 + 2.0 * r(5)) * s
  local second_width = (3.0 + 2.0 * r(6)) * s
  local base_roof = (constriction and 7.2 or 7.5 + 1.5 * r(7)) * s
  if o.lobes ~= false then
    for l = 0, 1 do
      local place = l == 0 and first_t or second_t
      local shift = (l == 0 and 1.0 or -1.0) * (3.0 + 2.5 * r(10 + l)) * s
      local rise = (r(12 + l) - 0.5) * (4.0 + vertical_gain) * s
      row.lobes[l + 1] = {
        centre = v_add(v_add(a, v_scale(axis, place * distance)), v_add(v_scale(right, shift), v_scale(up, rise))),
        axial = (11.0 + 4.0 * r(14 + l)) * s, lateral = (10.0 + 3.0 * r(16 + l)) * s,
        vertical = (7.0 + 2.0 * r(18 + l)) * s + vertical_gain * 0.8 }
    end
  end
  -- Коробка трубы: сечение раздуто до ширины и свода, плюс запас неровности (стенка ниже фона только
  -- внутри неё). Импульсы свода в формуле — постоянные метры, поэтому они в запасе без масштаба.
  local width = base_width + first_width + second_width
  -- Импульсы свода (2 и 2.5 м у маршрута) — тоже в масштабе.
  local roof = base_roof + 4.5 * s + vertical_gain
  local gallery_min = math.min(base_width * biome_width, base_roof)
  row.reach = math.max(width, roof) * (1.0 + 4.7 / gallery_min) + 1.0
  row.first_t, row.second_t = first_t, second_t
  row.base_width, row.first_width, row.second_width = base_width, first_width, second_width
  row.base_roof, row.vertical_gain, row.biome_width, row.pulse = base_roof, vertical_gain, biome_width, s
  row.constriction = constriction and 1 or 0
  return row
end

-- ------------------------------------------------------------------------------------------------
-- МАКРОМИР ОКНА как функции графа: галерея ребра, шар поворота узла, декоративный карман ребра. Окно
-- пишет их буферами; пещера места пересчитывает те же строки у своего узла (обход чужой воды) без
-- связи с окном. Узел — { key, centre, kind, rank }.
world.site_radius = 8.0

-- 0 karst, 1 rift, 2 chimney: крупные пятна по 2x2 сектора.
function world.biome_at(seed, p)
  local span = 2.0 * world.sector_size
  return base.umod(base.mix(seed, math.floor(p.x / span), math.floor(p.z / span), 0x42494f4d45), 3)
end

function world.port_position(from, toward)
  if from.kind == 0 then return from.centre end
  local direction = v_sub(toward.centre, from.centre)
  return v_add(from.centre, v_scale(direction, world.site_radius / v_length(direction)))
end

-- p — параметры шага: масштаб галерей (macro_scale) и радиус хода лодки (sub_path_radius) — константы
-- хоста о корпусе.
function world.macro_gallery(seed, from, to, core, p)
  local first, second = from.key, to.key
  if math.ult(second, first) then first, second = second, first end
  local key = base.mix(seed, first, second, 0x43415645)
  local biome = world.biome_at(seed, v_scale(v_add(from.centre, to.centre), 0.5))
  local row = world.gallery(seed, key, world.port_position(from, to), world.port_position(to, from),
                            { scale = p.macro_scale, core = core, biome = biome, rock = 1 + biome })
  row.route_radius = p.sub_path_radius
  return row
end

-- Шар поворота: полость базы, у пустого узла (не листа) чуть шире.
function world.turn_radius(seed, n, p)
  if world.is_base(n.kind) then return p.base_cavern_radius end
  if n.kind == 0 and n.rank ~= 1 then return p.sub_turn_radius + 2.5 * world.random01(seed, n.key, 601) end
  return p.sub_turn_radius
end
-- rock_kind хоста: 1 karst, 2 rift, 3 chimney, 4 base_cavern.
function world.turn_rock(seed, n)
  return world.is_base(n.kind) and 4 or 1 + world.biome_at(seed, n.centre)
end
world.turn_relief = 2.5

-- Декоративный карман ребра (треть рёбер): { mouth, centre, radius } или nil.
function world.edge_pocket(seed, from, to)
  local pf, ps = from, to
  if not math.ult(from.key, to.key) then pf, ps = to, from end
  local pocket_key = base.mix(seed, pf.key, ps.key, 0x504f434b4554)
  if base.umod(pocket_key, 3) ~= 0 then return nil end
  local function q(n) return world.random01(seed, pocket_key, n) end
  local span = v_sub(ps.centre, pf.centre)
  local t = 0.35 + 0.3 * q(1)
  local mouth = v_add(pf.centre, v_scale(span, t))
  local side = { x = -span.z, y = 0.0, z = span.x }
  if v_length(side) < 0.1 then side = { x = 1.0, y = 0.0, z = 0.0 } end
  side = v_scale(side, 1.0 / v_length(side))
  local sign = (pocket_key & 8) ~= 0 and 1.0 or -1.0
  local centre = v_add(mouth, v_add(v_scale(side, sign * (8.0 + 3.0 * q(2))), { x = 0.0, y = (q(3) - 0.5) * 3.0, z = 0.0 }))
  return { mouth = mouth, centre = centre, radius = 3.8 + 1.2 * q(4) }
end

-- ------------------------------------------------------------------------------------------------
-- ЧУЖАЯ ВОДА ОКНА у места — без поля: формы окна (галереи, выступы, ходы лодки, карманы, шары поворота)
-- пересчитываются из графа теми же правилами, и у каждой есть нижняя оценка значения поля (отрицательно —
-- возможно вода, положительно — заведомо порода). Так пещера обходит чужую воду, а станция находит стену
-- полости. route — узлы с индекса 0, edges — пары { a, b }.
local function point_segment(p, a, b)
  local ab = v_sub(b, a)
  local t = world.clamp(v_dot(v_sub(p, a), ab) / v_dot(ab, ab), 0.0, 1.0)
  return v_length(v_sub(p, v_add(a, v_scale(ab, t))))
end
world.point_segment = point_segment

-- Нижние оценки форм окна: { centre, radius (дальше — форма заведомо не ниже bound), bound(p) }.
local function macro_shapes(seed, p, route, edges, centre, reach)
  local shapes = {}
  local function keep(c, r, bound)
    if v_length(v_sub(c, centre)) - r < reach then shapes[#shapes + 1] = { centre = c, radius = r, bound = bound } end
  end
  for _, e in ipairs(edges) do
    local from, to = route[e.a], route[e.b]
    if point_segment(centre, from.centre, to.centre) < reach + 120.0 then
      local g = world.macro_gallery(seed, from, to, p.cave_core, p)
      local wide = (g.base_width + math.max(g.first_width, g.second_width)) * g.biome_width
      local tall = g.base_roof + 2.5 * g.pulse + g.vertical_gain
      local mid = v_scale(v_add(g.port_a, g.port_b), 0.5)
      -- Сечение в точке — как у shared/field/01_gallery.ds (вздутия по t), стенка ниже на наибольший шум.
      keep(mid, 0.5 * g.distance + math.max(wide, tall) + 2.0, function(q)
        local a = v_sub(q, g.port_a)
        local along = v_dot(a, g.axis)
        local t = world.clamp(along / g.distance, 0.0, 1.0)
        local c = world.clamp(along, 0.0, g.distance)
        local o = v_sub(a, v_scale(g.axis, c))
        local side, height = v_dot(o, g.right), v_dot(o, g.up)
        local beyond = math.max(-along, along - g.distance, 0.0)
        local q1 = math.max(0.0, 1.0 - math.abs(t - g.first_t) / 0.27)
        local q2 = math.max(0.0, 1.0 - math.abs(t - g.second_t) / 0.27)
        local p1, p2 = q1 * q1 * (3.0 - 2.0 * q1), q2 * q2 * (3.0 - 2.0 * q2)
        local bw = (g.base_width + g.first_width * p1 + g.second_width * p2) * g.biome_width
        local roof = g.base_roof + g.pulse * (2.0 * p1 + 2.5 * p2) + g.vertical_gain
        local m = math.min(bw, roof)
        local e1 = math.sqrt((side / bw) ^ 2 + (height / roof) ^ 2 + (beyond / m) ^ 2)
        return math.min((e1 - 1.0) * m - 1.2, v_length(o) - g.core_radius)
      end)
      for _, lobe in ipairs(g.lobes) do
        local big = math.max(lobe.axial, lobe.lateral, lobe.vertical)
        keep(lobe.centre, big + 2.0, function(q)
          local d = v_sub(q, lobe.centre)
          local s = math.sqrt((v_dot(d, g.axis) / lobe.axial) ^ 2 + (v_dot(d, g.right) / lobe.lateral) ^ 2 + (v_dot(d, g.up) / lobe.vertical) ^ 2)
          return (s - 1.0) * 7.0 - 1.2
        end)
      end
      -- Ход корпуса по ребру (shared/field/02_route.ds): капсула хода лодки с неровностью до 3 м наружу.
      local route_reach = g.route_radius + 3.0
      keep(v_scale(v_add(from.centre, to.centre), 0.5), 0.5 * v_length(v_sub(to.centre, from.centre)) + route_reach, function(q)
        return point_segment(q, from.centre, to.centre) - route_reach
      end)
      local pocket = world.edge_pocket(seed, from, to)
      if pocket then
        keep(pocket.centre, v_length(v_sub(pocket.centre, pocket.mouth)) + pocket.radius + 2.0, function(q)
          return math.min(point_segment(q, pocket.mouth, pocket.centre) - p.cave_core,
                          v_length(v_sub(q, pocket.centre)) - pocket.radius) - 1.5
        end)
      end
    end
  end
  for i = 0, #route do
    local n = route[i]
    if n then
      local radius = world.turn_radius(seed, n, p) + 2.0 * world.turn_relief
      keep(n.centre, radius, function(q) return v_length(v_sub(q, n.centre)) - radius end)
    end
  end
  return shapes
end


-- water(q, need, stop): минимум оценок форм ближе need к точке (need не больше 32 м); досрочный выход —
-- когда минимум уже не выше порога решения stop.
function world.macro_water(seed, p, route, edges, centre, reach)
  local shapes = macro_shapes(seed, p, route, edges, centre, reach)
  -- Сетка по формам: клетка 32 м, форма записана во все клетки, до которых дотягивается её шар плюс
  -- наибольший запрос.
  local cell, widest = 32.0, 32.0
  local grid = {}
  local function cell_key(x, y, z) return (x * 4096 + y) * 4096 + z end
  for _, shape in ipairs(shapes) do
    local r = shape.radius + widest
    for x = math.floor((shape.centre.x - r) / cell), math.floor((shape.centre.x + r) / cell) do
      for y = math.floor((shape.centre.y - r) / cell), math.floor((shape.centre.y + r) / cell) do
        for z = math.floor((shape.centre.z - r) / cell), math.floor((shape.centre.z + r) / cell) do
          local k = cell_key(x, y, z)
          local list = grid[k]
          if list == nil then list = {}; grid[k] = list end
          list[#list + 1] = shape
        end
      end
    end
  end
  local none = {}
  return function(q, need, stop)
    if need > widest then need = widest end
    stop = stop or -1e30
    local best = 1e30
    for _, s in ipairs(grid[cell_key(math.floor(q.x / cell), math.floor(q.y / cell), math.floor(q.z / cell))] or none) do
      if v_length(v_sub(q, s.centre)) - s.radius < need then
        best = math.min(best, s.bound(q))
        if best <= stop then return best end
      end
    end
    return best
  end
end

-- Пишущие поля буферов строк формы (галереи, полости, шары поворота) — в одном месте для окна и пещер.
function world.shape_writers(step)
  local w = { galleries = step.writes.galleries, lobes = step.writes.lobes, turns = step.writes.turns,
              pockets = step.writes.pockets, counts = step.writes.environment_counts,
              made = { galleries = 0, lobes = 0, turns = 0, pockets = 0 } }
  w.g, w.l, w.t = {}, {}, {}
  for _, name in ipairs({ "ends_a", "ends_b", "port_a", "port_b", "axis", "right", "up", "distance", "core_radius", "reach",
                          "first_t", "second_t", "base_width", "first_width", "second_width", "base_roof",
                          "vertical_gain", "biome_width", "pulse", "route_radius", "constriction", "biome", "rock" }) do
    w.g[name] = w.galleries:field(name)
  end
  for _, name in ipairs({ "centre", "axis", "right", "up", "size", "rock" }) do w.l[name] = w.lobes:field(name) end
  for _, name in ipairs({ "centre", "radius", "rock", "relief" }) do w.t[name] = w.turns:field(name) end
  return w
end

local function set3(field, i, v) field:set(i, v.x, 0); field:set(i, v.y, 1); field:set(i, v.z, 2) end
world.set3 = set3

-- Галерея (и её полости) в буферы; ends — концы гарантированной капсулы хода лодки.
function world.write_gallery(w, row, ends_a, ends_b)
  local i = w.made.galleries
  if i >= w.galleries:count() then error("gallery capacity " .. w.galleries:count() .. " exceeded") end
  local g = w.g
  set3(g.ends_a, i, ends_a); set3(g.ends_b, i, ends_b)
  set3(g.port_a, i, row.port_a); set3(g.port_b, i, row.port_b)
  set3(g.axis, i, row.axis); set3(g.right, i, row.right); set3(g.up, i, row.up)
  g.distance:set(i, row.distance); g.core_radius:set(i, row.core_radius); g.reach:set(i, row.reach)
  g.first_t:set(i, row.first_t); g.second_t:set(i, row.second_t)
  g.base_width:set(i, row.base_width); g.first_width:set(i, row.first_width); g.second_width:set(i, row.second_width)
  g.base_roof:set(i, row.base_roof); g.vertical_gain:set(i, row.vertical_gain); g.biome_width:set(i, row.biome_width)
  g.pulse:set(i, row.pulse); g.route_radius:set(i, row.route_radius)
  g.constriction:set(i, row.constriction); g.biome:set(i, row.biome); g.rock:set(i, row.rock)
  w.made.galleries = i + 1
  for _, lobe in ipairs(row.lobes) do
    local k = w.made.lobes
    if k >= w.lobes:count() then error("lobe capacity " .. w.lobes:count() .. " exceeded") end
    set3(w.l.centre, k, lobe.centre)
    set3(w.l.axis, k, row.axis); set3(w.l.right, k, row.right); set3(w.l.up, k, row.up)
    set3(w.l.size, k, { x = lobe.axial, y = lobe.lateral, z = lobe.vertical })
    w.l.rock:set(k, row.rock)
    w.made.lobes = k + 1
  end
end

-- Шар: в нём проходит тело при любой ориентации; relief — неровность стенки только наружу.
function world.write_turn(w, centre, radius, rock, relief)
  local i = w.made.turns
  if i >= w.turns:count() then error("turn capacity " .. w.turns:count() .. " exceeded") end
  set3(w.t.centre, i, centre)
  w.t.radius:set(i, radius); w.t.rock:set(i, rock); w.t.relief:set(i, relief)
  w.made.turns = i + 1
end

function world.write_pocket(w, pocket, core)
  local i = w.made.pockets
  if i >= w.pockets:count() then error("pocket capacity " .. w.pockets:count() .. " exceeded") end
  w.p = w.p or { mouth = w.pockets:field("mouth"), centre = w.pockets:field("centre"), radius = w.pockets:field("radius"),
                 core_radius = w.pockets:field("core_radius") }
  set3(w.p.mouth, i, pocket.mouth); set3(w.p.centre, i, pocket.centre)
  w.p.radius:set(i, pocket.radius); w.p.core_radius:set(i, core)
  w.made.pockets = i + 1
end

function world.finish_shapes(w)
  w.counts:field("galleries"):set(0, w.made.galleries)
  w.counts:field("turns"):set(0, w.made.turns)
  w.counts:field("lobes"):set(0, w.made.lobes)
  w.counts:field("pockets"):set(0, w.made.pockets)
end

-- Ключ экземпляра станции: сектор центра якоря и ключ якоря.
function world.station_key(anchor)
  return math.floor(anchor.centre.x / world.sector_size), math.floor(anchor.centre.z / world.sector_size), anchor.key
end

return world
