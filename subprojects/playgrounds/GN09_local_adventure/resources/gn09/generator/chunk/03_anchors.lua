-- Якоря в досягаемости чанка: центр якоря в коробке чанка, раздутой на station_reach, — то же правило, что у
-- хоста structures_near и tunnels_near (станция и её туннель целиком в кубе station_reach вокруг якоря).

local W = modules.world

return function(step)
  local cells = math.tointeger(step.params.chunk_cells)
  local reach = step.params.station_reach
  local low, high = {}, {}
  for axis = 1, 3 do
    low[axis] = step.key[axis] * cells - 1.0
    high[axis] = (step.key[axis] + 1) * cells + 1.0
  end
  local out, made = step.writes.anchors, 0
  local station = out:field("station")
  for _, a in ipairs(W.station_anchors(step.world_seed, step.reads.nodes, step.reads.graph_meta, step.links.regions)) do
    local c = { a.centre.x, a.centre.y, a.centre.z }
    local inside = true
    for axis = 1, 3 do
      if c[axis] < low[axis] - reach or c[axis] > high[axis] + reach then inside = false end
    end
    if inside then
      if made >= out:count() then error("chunk_anchors: capacity exceeded") end
      local sx, sz, key = W.station_key(a)
      station:set(made, sx, 0); station:set(made, sz, 1); station:set(made, key, 2)
      made = made + 1
    end
  end
  step.writes.anchor_counts:field("anchors"):set(0, made)
end
