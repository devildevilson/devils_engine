-- Общий модуль цепочки GN09: правила, которые нужны нескольким генераторам, в ОДНОЙ копии. Тело шага
-- видит его как `modules.world` (документ цепочки объявляет модули; `require` у генератора нет).
--
-- Хеши и шум повторяют мир хоста бит в бит: тот же utils::mix (base.mix), тот же порядок операций с
-- double. Формулы записаны ровно в том порядке, в каком их считал C++.

local world = {}

world.sector_size = 96
world.region_size = 768
world.world_y_min, world.world_y_max = -500, 500
-- Самый длинный зал с запасом на рельеф и ниши: дальше этого его поле — скала.
world.hall_reach = 520.0
-- Колонна зала у пола и потолка шире: radius * (1 + 0.35 (y/H)^2).
world.pillar_flare = 0.35

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

-- Ключ экземпляра станции: сектор центра якоря и ключ якоря.
function world.station_key(anchor)
  return math.floor(anchor.centre.x / world.sector_size), math.floor(anchor.centre.z / world.sector_size), anchor.key
end

return world
