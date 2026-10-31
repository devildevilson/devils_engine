-- Все природные места окна: ключи (сектор, узел) — те же, что у генераторов place и cave.

local W = modules.world

return function(step)
  local nodes = step.reads.nodes
  local n_key, n_centre, n_kind = nodes:field("key"), nodes:field("centre"), nodes:field("kind")
  local count = math.tointeger(step.reads.graph_meta:field("nodes"):get(0))
  local places = step.writes.places
  local p_sector, p_node = places:field("sector"), places:field("node")
  local made = 0
  for i = 0, count - 1 do
    if W.natural(math.tointeger(n_kind:get(i))) then
      if made >= places:count() then error("window places: capacity " .. places:count() .. " exceeded") end
      p_sector:set(made, math.floor(n_centre:get(i, 0) / W.sector_size), 0)
      p_sector:set(made, math.floor(n_centre:get(i, 2) / W.sector_size), 1)
      p_node:set(made, n_key:get(i))
      made = made + 1
    end
  end
  step.writes.place_counts:field("places"):set(0, made)
end
