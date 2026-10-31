-- Окружение окна: галерея на каждое ребро графа (порты у мест, ширины и свод по биому, две смещённые
-- полости, иногда сужение, иногда декоративный карман) и шар поворота лодки в каждом узле. Перенос C++
-- (adventure.cpp: shape_edge, materialize_edge_pocket, node_radius) бит в бит. Формулы поля по этим строкам
-- — в словаре форм хоста; здесь только сами строки.

local W = modules.world

local site_radius = 8.0
local base_cavern_radius = 19.5

local function add(a, b) return { x = a.x + b.x, y = a.y + b.y, z = a.z + b.z } end
local function sub(a, b) return { x = a.x - b.x, y = a.y - b.y, z = a.z - b.z } end
local function scale(a, s) return { x = a.x * s, y = a.y * s, z = a.z * s } end
local function dot(a, b) return a.x * b.x + a.y * b.y + a.z * b.z end
local function length(a) return math.sqrt(dot(a, a)) end
local function cross(a, b) return { x = a.y * b.z - a.z * b.y, y = a.z * b.x - a.x * b.z, z = a.x * b.y - a.y * b.x } end

local function biome_at(seed, p)
  local span = 2.0 * W.sector_size
  return base.umod(base.mix(seed, math.floor(p.x / span), math.floor(p.z / span), 0x42494f4d45), 3)
end

local function port_position(from, toward)
  if from.kind == 0 then return from.centre end
  local direction = sub(toward.centre, from.centre)
  return add(from.centre, scale(direction, site_radius / length(direction)))
end

return function(step)
  local p = step.params
  local seed = step.world_seed
  local limits = step.writes.limits
  limits:field("core"):set(0, p.cave_core)
  limits:field("relief"):set(0, p.cave_relief)
  limits:field("main_radius"):set(0, math.max(p.sub_turn_radius, p.cave_core + 0.5))
  limits:field("extent"):set(0, p.cave_extent)
  limits:field("reach"):set(0, 2.0 * p.cave_extent + p.cave_rock_ring)
  limits:field("clearance"):set(0, p.clearance_radius)

  local nodes = step.reads.nodes
  local n_key, n_centre, n_kind, n_rank = nodes:field("key"), nodes:field("centre"), nodes:field("kind"), nodes:field("rank")
  local meta = step.reads.graph_meta
  local node_count = math.tointeger(meta:field("nodes"):get(0))
  local edge_count = math.tointeger(meta:field("edges"):get(0))
  local route = {}
  for i = 0, node_count - 1 do
    route[i] = { key = n_key:get(i), centre = { x = n_centre:get(i, 0), y = n_centre:get(i, 1), z = n_centre:get(i, 2) },
                 kind = math.tointeger(n_kind:get(i)), rank = math.tointeger(n_rank:get(i)) }
  end
  local e_a, e_b = step.reads.edges:field("a"), step.reads.edges:field("b")

  local galleries = step.writes.galleries
  if edge_count > galleries:count() then error("window: " .. edge_count .. " edges, gallery capacity " .. galleries:count()) end
  local g = {}
  for _, name in ipairs({ "ends_a", "ends_b", "port_a", "port_b", "axis", "right", "up", "distance", "core_radius", "reach",
                          "first_t", "second_t", "base_width", "first_width", "second_width", "base_roof",
                          "vertical_gain", "biome_width", "constriction", "biome", "lobe0", "lobe0_size", "lobe1",
                          "lobe1_size", "pocket", "pocket_mouth", "pocket_centre", "pocket_radius" }) do
    g[name] = galleries:field(name)
  end
  local function set3(field, i, v) field:set(i, v.x, 0); field:set(i, v.y, 1); field:set(i, v.z, 2) end

  for i = 0, edge_count - 1 do
    local from, to = route[math.tointeger(e_a:get(i))], route[math.tointeger(e_b:get(i))]
    -- Форма галереи ребра (shape_edge).
    local a, b = port_position(from, to), port_position(to, from)
    local ab = sub(b, a)
    local distance = length(ab)
    local core_radius = p.cave_core
    local biome = biome_at(seed, scale(add(from.centre, to.centre), 0.5))
    local vertical_gain = biome == 0 and 0.0 or (biome == 1 and 14.0 or 22.0)
    local biome_width = biome == 2 and 0.82 or 1.0
    local first, second = from.key, to.key
    if math.ult(second, first) then first, second = second, first end
    local key = base.mix(seed, first, second, 0x43415645)
    local function r(n) return W.random01(seed, key, n) end
    local constriction = base.umod(base.mix(key, 1), 6) == 0
    local axis = scale(ab, 1.0 / distance)
    local reference = math.abs(axis.y) < 0.85 and { x = 0.0, y = 1.0, z = 0.0 } or { x = 1.0, y = 0.0, z = 0.0 }
    local right = scale(cross(axis, reference), 1.0 / length(cross(axis, reference)))
    local up = cross(right, axis)
    local first_t = 0.25 + 0.12 * r(2)
    local second_t = 0.65 + 0.12 * r(3)
    local base_width = constriction and 7.4 or 9.0 + 2.0 * r(4)
    local first_width = 3.0 + 2.0 * r(5)
    local second_width = 3.0 + 2.0 * r(6)
    local base_roof = constriction and 7.2 or 7.5 + 1.5 * r(7)
    local lobes = {}
    for l = 0, 1 do
      local place = l == 0 and first_t or second_t
      local shift = (l == 0 and 1.0 or -1.0) * (3.0 + 2.5 * r(10 + l))
      local rise = (r(12 + l) - 0.5) * (4.0 + vertical_gain)
      lobes[l + 1] = {
        centre = add(add(a, scale(axis, place * distance)), add(scale(right, shift), scale(up, rise))),
        axial = 11.0 + 4.0 * r(14 + l), lateral = 10.0 + 3.0 * r(16 + l),
        vertical = 7.0 + 2.0 * r(18 + l) + vertical_gain * 0.8 }
    end
    local width = base_width + first_width + second_width
    local roof = base_roof + 4.5 + vertical_gain
    local gallery_min = math.min(base_width * biome_width, base_roof)
    local extent = math.max(width, roof) * (1.0 + 4.7 / gallery_min)
    for _, lobe in ipairs(lobes) do
      local offset = sub(lobe.centre, add(a, scale(axis, dot(sub(lobe.centre, a), axis))))
      extent = math.max(extent, length(offset) + math.max(lobe.axial, lobe.lateral, lobe.vertical) * (1.0 + 4.7 / 7.0))
    end
    local reach = math.max(24.0, extent + 1.0)

    set3(g.ends_a, i, from.centre); set3(g.ends_b, i, to.centre)
    set3(g.port_a, i, a); set3(g.port_b, i, b)
    set3(g.axis, i, axis); set3(g.right, i, right); set3(g.up, i, up)
    g.distance:set(i, distance); g.core_radius:set(i, core_radius); g.reach:set(i, reach)
    g.first_t:set(i, first_t); g.second_t:set(i, second_t)
    g.base_width:set(i, base_width); g.first_width:set(i, first_width); g.second_width:set(i, second_width)
    g.base_roof:set(i, base_roof); g.vertical_gain:set(i, vertical_gain); g.biome_width:set(i, biome_width)
    g.constriction:set(i, constriction and 1 or 0); g.biome:set(i, biome)
    set3(g.lobe0, i, lobes[1].centre); set3(g.lobe0_size, i, { x = lobes[1].axial, y = lobes[1].lateral, z = lobes[1].vertical })
    set3(g.lobe1, i, lobes[2].centre); set3(g.lobe1_size, i, { x = lobes[2].axial, y = lobes[2].lateral, z = lobes[2].vertical })

    -- Декоративный карман ребра (materialize_edge_pocket): треть рёбер.
    local pf, ps = from, to
    if not math.ult(from.key, to.key) then pf, ps = to, from end
    local pocket_key = base.mix(seed, pf.key, ps.key, 0x504f434b4554)
    if base.umod(pocket_key, 3) == 0 then
      local function q(n) return W.random01(seed, pocket_key, n) end
      local span = sub(ps.centre, pf.centre)
      local t = 0.35 + 0.3 * q(1)
      local mouth = add(pf.centre, scale(span, t))
      local side = { x = -span.z, y = 0.0, z = span.x }
      if length(side) < 0.1 then side = { x = 1.0, y = 0.0, z = 0.0 } end
      side = scale(side, 1.0 / length(side))
      local sign = (pocket_key & 8) ~= 0 and 1.0 or -1.0
      local centre = add(mouth, add(scale(side, sign * (8.0 + 3.0 * q(2))), { x = 0.0, y = (q(3) - 0.5) * 3.0, z = 0.0 }))
      g.pocket:set(i, 1)
      set3(g.pocket_mouth, i, mouth)
      set3(g.pocket_centre, i, centre)
      g.pocket_radius:set(i, 3.8 + 1.2 * q(4))
    end
  end

  -- Шары поворота: полость базы, у пустого узла (не листа) чуть шире, вид породы по биому.
  local turns = step.writes.turns
  if node_count > turns:count() then error("window: " .. node_count .. " nodes, turn capacity " .. turns:count()) end
  local t_centre, t_radius, t_rock = turns:field("centre"), turns:field("radius"), turns:field("rock")
  for i = 0, node_count - 1 do
    local n = route[i]
    local radius = p.sub_turn_radius
    if W.is_base(n.kind) then radius = base_cavern_radius
    elseif n.kind == 0 and n.rank ~= 1 then radius = p.sub_turn_radius + 2.5 * W.random01(seed, n.key, 601) end
    set3(t_centre, i, n.centre)
    t_radius:set(i, radius)
    -- rock_kind хоста: 1 karst, 2 rift, 3 chimney, 4 base_cavern.
    t_rock:set(i, W.is_base(n.kind) and 4 or 1 + biome_at(seed, n.centre))
  end
  local counts = step.writes.environment_counts
  counts:field("galleries"):set(0, edge_count)
  counts:field("turns"):set(0, node_count)
end
