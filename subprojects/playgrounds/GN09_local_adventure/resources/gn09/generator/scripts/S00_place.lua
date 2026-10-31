-- Тело шага find: свой узел в графе канонической окрестности, вход пещеры и старшие соседи.
--
-- Старший сосед — природное место (не база и не пустой узел) с МЕНЬШИМ ключом, центр которого не
-- дальше досягаемости пещер (limits.reach): его пещера выбрана раньше, и эта её обходит. Ключи
-- сравниваются как беззнаковые 64-битные (math.ult). Порядок соседей — порядок узлов графа: он и есть
-- порядок fan-in. Рецепт и биом места — те же хеши, что у хоста.

local W = modules.world
local sector_size = W.sector_size
local natural = W.natural

-- 0 ore_cave, 1 nest_cave, 2 thermal_cave
local function recipe(seed, kind, key)
  if kind == 4 then return 2 end
  return base.umod(base.mix(seed, key, 0x524543495045), 2) ~= 0 and 1 or 0
end

-- 0 karst, 1 rift, 2 chimney: крупные пятна по 2x2 сектора.
local function biome(seed, x, z)
  local span = 2.0 * sector_size
  return base.umod(base.mix(seed, math.floor(x / span), math.floor(z / span), 0x42494f4d45), 3)
end

return function(step)
  local seed = step.world_seed
  local nodes = step.reads.nodes
  local limits = step.reads.limits
  local site = step.writes.site
  local elders = step.writes.elders

  local count = math.tointeger(step.reads.graph_meta:field("nodes"):get(0))
  local own = step.key[3]
  local n_key, n_centre, n_kind = nodes:field("key"), nodes:field("centre"), nodes:field("kind")

  local at
  for i = 0, count - 1 do
    if n_key:get(i) == own then at = i break end
  end
  if at == nil then error(string.format("place: node %x is not in the graph around sector (%d, %d)", own, step.key[1], step.key[2])) end

  local cx, cy, cz = n_centre:get(at, 0), n_centre:get(at, 1), n_centre:get(at, 2)
  if math.floor(cx / sector_size) ~= step.key[1] or math.floor(cz / sector_size) ~= step.key[2] then
    error(string.format("place: node %x lies in another sector than its key says", own))
  end
  local kind = math.tointeger(n_kind:get(at))
  local s_centre = site:field("centre")
  s_centre:set(0, cx, 0); s_centre:set(0, cy, 1); s_centre:set(0, cz, 2)
  for _, name in ipairs({ "core", "relief", "main_radius", "extent" }) do
    site:field(name):set(0, limits:field(name):get(0))
  end
  site:field("biome"):set(0, biome(seed, cx, cz))
  site:field("recipe"):set(0, natural(kind) and recipe(seed, kind, own) or 0)

  local reach = limits:field("reach"):get(0)
  local e_sector, e_node = elders:field("sector"), elders:field("node")
  local made = 0
  for j = 0, count - 1 do
    local key = n_key:get(j)
    if natural(math.tointeger(n_kind:get(j))) and math.ult(key, own) then
      local x, y, z = n_centre:get(j, 0), n_centre:get(j, 1), n_centre:get(j, 2)
      local dx, dy, dz = x - cx, y - cy, z - cz
      if math.sqrt(dx * dx + dy * dy + dz * dz) <= reach then
        if made >= elders:count() then error("place: elder capacity " .. elders:count() .. " exceeded") end
        e_sector:set(made, math.floor(x / sector_size), 0)
        e_sector:set(made, math.floor(z / sector_size), 1)
        e_node:set(made, key)
        made = made + 1
      end
    end
  end
  site:field("elders"):set(0, made)
end
