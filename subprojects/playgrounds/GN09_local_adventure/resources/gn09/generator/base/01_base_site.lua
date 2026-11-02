-- Программа места базы. Перенос C++ (adventure.cpp, generate_site для баз) бит в бит: док радиусом с шар
-- поворота лодки (не меньше ядра прохода с запасом), модули по рецепту — 6 у рудной и жилой, 4 у
-- военной; у брошенной базы приставка ruined_.

local W = modules.world

return function(step)
  local p = step.params
  local nodes = step.reads.nodes
  local n_key, n_centre, n_kind = nodes:field("key"), nodes:field("centre"), nodes:field("kind")
  local node = nil
  for i = 0, math.tointeger(step.reads.graph_meta:field("nodes"):get(0)) - 1 do
    if n_key:get(i) == step.key[3] then
      node = { kind = math.tointeger(n_kind:get(i)), x = n_centre:get(i, 0), y = n_centre:get(i, 1), z = n_centre:get(i, 2) }
      break
    end
  end
  if node == nil or not W.is_base(node.kind) then error("base_site: no base node with this key in the canonical window") end
  local codes, index = {}, 0
  for word in string.gmatch(p.purposes, "%S+") do codes[word] = index; index = index + 1 end
  local recipe = 3 + base.umod(base.mix(step.world_seed, step.key[3], 0x524543495045), 3)
  local uses = recipe == 3 and p.mining_uses or (recipe == 4 and p.residential_uses or p.military_uses)
  local list = {}
  for word in string.gmatch(uses, "%S+") do list[#list + 1] = word end
  local count = recipe == 5 and 4 or 6
  local rooms = step.writes.base_rooms
  local function put(i, radius, purpose)
    local c = rooms:field("centre")
    c:set(i, node.x, 0); c:set(i, node.y, 1); c:set(i, node.z, 2)
    rooms:field("radius"):set(i, radius)
    local code = codes[purpose]
    if code == nil then error("base_site: purpose '" .. purpose .. "' is not in the purposes list") end
    rooms:field("purpose"):set(i, code)
  end
  put(0, math.max(p.sub_turn_radius, p.cave_core + 0.5), "dock")
  for i = 1, count do put(i, 0.0, (node.kind == 2 and "ruined_" or "") .. list[i]) end
  step.writes.base_counts:field("rooms"):set(0, count + 1)
end
