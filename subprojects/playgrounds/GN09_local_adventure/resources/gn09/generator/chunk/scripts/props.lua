-- Предметы чанка: залежи, кладки и метки пещер мест (выход features генератора cave), чей центр лежит в
-- коробке чанка [угол, угол + cells). Порядок — места по порядку, в месте предметы по порядку.

return function(step)
  local cells = math.tointeger(step.params.chunk_cells)
  local origin = { step.key[1] * cells, step.key[2] * cells, step.key[3] * cells }
  local places = step.reads.places:field("node")
  local out, made = step.writes.props, 0
  local position, kind, node, index = out:field("position"), out:field("kind"), out:field("node"), out:field("index")
  for i, cave in ipairs(step.links.caves) do
    local features = cave.features
    local centre, code = features:field("centre"), features:field("kind")
    for f = 0, features:count() - 1 do
      local inside = true
      for axis = 1, 3 do
        local local_axis = centre:get(f, axis - 1) - origin[axis]
        if local_axis < 0.0 or local_axis >= cells then inside = false end
      end
      if inside then
        if made >= out:count() then error("chunk props: capacity " .. out:count() .. " exceeded") end
        for axis = 0, 2 do position:set(made, centre:get(f, axis), axis) end
        kind:set(made, code:get(f))
        node:set(made, places:get(i - 1))
        index:set(made, f)
        made = made + 1
      end
    end
  end
  step.writes.surface_counts:field("props"):set(0, made)
end
