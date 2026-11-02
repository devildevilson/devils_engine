-- Отбор мест чанка: природный узел (grotto, vent), чей центр не дальше site_reach от коробки чанка,
-- расширенной на соседний слой сэмплов. То же правило, что у prepare_field: пещера места целиком лежит
-- в кубе site_reach вокруг узла, поэтому дальнее место до чанка не дотягивается.

local natural = modules.world.natural

return function(step)
  local cells = math.tointeger(step.params.chunk_cells)
  local sector = math.tointeger(step.params.sector_size)
  local reach = step.params.site_reach
  local low, high = {}, {}
  for axis = 1, 3 do
    low[axis] = step.key[axis] * cells - 1.0
    high[axis] = (step.key[axis] + 1) * cells + 1.0
  end
  local nodes = step.reads.nodes
  local n_key, n_centre, n_kind = nodes:field("key"), nodes:field("centre"), nodes:field("kind")
  local count = math.tointeger(step.reads.graph_meta:field("nodes"):get(0))
  local places = step.writes.places
  local p_sector, p_node = places:field("sector"), places:field("node")
  local made = 0
  for i = 0, count - 1 do
    if natural(math.tointeger(n_kind:get(i))) then
      local inside = true
      for axis = 1, 3 do
        local c = n_centre:get(i, axis - 1)
        if c < low[axis] - reach or c > high[axis] + reach then inside = false end
      end
      if inside then
        if made >= places:count() then error("chunk places: capacity " .. places:count() .. " exceeded") end
        p_sector:set(made, math.floor(n_centre:get(i, 0) / sector), 0)
        p_sector:set(made, math.floor(n_centre:get(i, 2) / sector), 1)
        p_node:set(made, n_key:get(i))
        made = made + 1
      end
    end
  end
  step.writes.chunk_counts:field("places"):set(0, made)
end
