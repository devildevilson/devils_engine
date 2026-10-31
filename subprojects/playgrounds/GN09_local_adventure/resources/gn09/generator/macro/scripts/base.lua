-- База сектора: позиция на террасе рельефа слоя и вид; если позиция попала в ярус зала — пустой узел
-- внутри зала вместо базы.
--
-- Хеши и шум здесь ПОВТОРЯЮТ мир хоста бит в бит: тот же utils::mix (base.mix), тот же порядок
-- операций с double. Поэтому формулы записаны ровно в том порядке, в каком их считает C++, и
-- беззнаковые остатки — через base.umod.

local W = modules.world
local sector_size = W.sector_size
local world_y_min, world_y_max = W.world_y_min, W.world_y_max
local pillar_flare = W.pillar_flare
local clamp, node_key, value_noise, layer_height = W.clamp, W.node_key, W.value_noise, W.layer_height
local read_halls, to_local, to_world = W.read_halls, W.hall_to_local, W.hall_to_world

local function base_position(seed, x, z)
  local key = node_key(x, z, 0, 0)
  local jx = base.umod(base.mix(seed, key, 11), 49) - 24
  local jz = base.umod(base.mix(seed, key, 29), 49) - 24
  local jy = base.umod(base.mix(seed, key, 37), 29) - 14
  local px = 0.0 + (x * sector_size + sector_size // 2 + jx)
  local pz = 0.0 + (z * sector_size + sector_size // 2 + jz)
  local terrace = 45.0 * value_noise(seed + 14, px, 0.0, pz, 260.0)
  local y = clamp(layer_height(seed, px, pz) + terrace + jy, world_y_min + 60.0, world_y_max - 60.0)
  return px, y, pz
end

-- Зал из буферов региона: те же поля, что у C++-структуры, нужные макрографу.

-- to_local: (dot(d, forward), d.y, dot(d, right)), right = (-forward.z, 0, forward.x).

-- to_world: centre + ((forward * u + right * v) + (0, y, 0)).

local function owns(h, x, y, z)
  local u, _, v = to_local(h, x, y, z)
  local a, b = u / (h.hl + 60.0), v / (h.hw + 60.0)
  if a * a + b * b > 1.0 then return false end
  local layer = y - h.cy
  return layer >= h.floor - 40.0 and layer <= 0.8 * h.hh
end

local function hall_node_position(h, seed, x, z)
  local u, _, v = to_local(h, x, 0.0, z)
  local inner_u = math.max(10.0, h.hl - 45.0)
  local inner_v = math.max(10.0, h.hw - 45.0)
  local level = (u / inner_u) * (u / inner_u) + (v / inner_v) * (v / inner_v)
  if level > 0.9 then
    local shrink = math.sqrt(0.9 / level)
    u = u * shrink
    v = v * shrink
  end
  for _, pillar in ipairs(h.pillars) do
    local du, dv = u - pillar.u, v - pillar.v
    local distance = base.hypot(du, dv)
    local keep = pillar.radius * (1.0 + pillar_flare) + 14.0
    if distance < keep then
      local ux = distance > 1e-6 and du / distance or 0.0
      local uz = distance > 1e-6 and dv / distance or 1.0
      u = pillar.u + ux * keep
      v = pillar.v + uz * keep
    end
  end
  local wx, _, wz = to_world(h, u, 0.0, v)
  local ellipse = 1.0 - (u / h.hl) * (u / h.hl) - (v / h.hw) * (v / h.hw)
  local ceiling = h.hh * math.sqrt(math.max(0.05, ellipse))
  local low, high = h.floor + 18.0, ceiling - 18.0
  local layer = layer_height(seed, wx, wz) - h.cy
  local y
  if low < high then y = clamp(layer, low, high) else y = 0.5 * (low + high) end
  return to_world(h, u, y, v)
end

return function(step)
  local seed = step.world_seed
  local x, z = step.key[1], step.key[2]
  local key = node_key(x, z, 0, 0)
  local rx, ry, rz = base_position(seed, x, z)

  local cx, cy, cz, kind = rx, ry, rz, nil
  for _, h in ipairs(read_halls(step.links.halls)) do
    if owns(h, rx, ry, rz) then
      cx, cy, cz = hall_node_position(h, seed, rx, rz)
      kind = 0
      break
    end
  end
  if kind == nil then kind = base.umod(base.mix(seed, key, 41), 2) == 0 and 1 or 2 end

  local out = step.writes.base
  out:field("key"):set(0, key)
  local raw, centre = out:field("raw"), out:field("centre")
  raw:set(0, rx, 0); raw:set(0, ry, 1); raw:set(0, rz, 2)
  centre:set(0, cx, 0); centre:set(0, cy, 1); centre:set(0, cz, 2)
  out:field("kind"):set(0, kind)
end
