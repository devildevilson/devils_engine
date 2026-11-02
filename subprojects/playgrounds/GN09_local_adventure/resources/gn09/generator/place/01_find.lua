-- Тело шага find: свой узел в графе канонической окрестности и вход пещеры. Рецепт и биом места — те же
-- хеши, что у хоста.

local W = modules.world
local sector_size = W.sector_size

-- 0 ore_cave, 1 nest_cave, 2 thermal_cave
local function recipe(seed, kind, key)
  if kind == 4 then return 2 end
  return base.umod(base.mix(seed, key, 0x524543495045), 2) ~= 0 and 1 or 0
end

return function(step)
  local seed = step.world_seed
  local nodes = step.reads.nodes
  local limits = step.reads.limits
  local site = step.writes.site

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
  for _, name in ipairs({ "core", "main_radius", "extent" }) do
    site:field(name):set(0, limits:field(name):get(0))
  end
  site:field("biome"):set(0, W.biome_at(seed, { x = cx, y = cy, z = cz }))
  site:field("recipe"):set(0, W.natural(kind) and recipe(seed, kind, own) or 0)
end
