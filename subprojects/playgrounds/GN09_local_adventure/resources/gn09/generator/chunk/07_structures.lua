-- Порядок поверхностей задаёт GN09: станции якорей, затем туннели чанка. Общий
-- clip_triangle_mesh обрезает треугольники по полуоткрытому чанку и выкидывает
-- стену станции внутри ноги её туннеля (ориентированные исключающие коробки).

return function(step)
  step.writes.chunk_meta:field("seed"):set(0, step.world_seed)
  local out = step.writes.structure_surface
  local count = step.writes.surface_counts:field("structure")
  count:set(0, 0)
  local side = step.params.chunk_cells
  local low = { step.key[1] * side, step.key[2] * side, step.key[3] * side }
  local high = { low[1] + side, low[2] + side, low[3] + side }
  local params = { x = step.key[1], y = step.key[2], z = step.key[3], side = side }
  local function reaches(meta)
    local a, b = meta:field("low"), meta:field("high")
    for axis = 0, 2 do
      if a:get(0, axis) > high[axis + 1] or b:get(0, axis) < low[axis + 1] then return false end
    end
    return true
  end
  local function clip(mesh, boxes, key, exclusions)
    params.tag, params.exclusions = key, exclusions
    originator.clip_triangle_mesh{
      inputs = { mesh:field("a"), mesh:field("b"), mesh:field("c"), mesh:field("normal"), mesh:field("shade"),
                 boxes:field("centre"), boxes:field("x"), boxes:field("y"), boxes:field("z"),
                 boxes:field("half"), boxes:field("station") },
      outputs = { out:field("position"), out:field("normal"), out:field("shade"), count },
      params = params }
  end
  for _, station in ipairs(step.links.stations) do
    local meta = station.station_meta
    if meta:field("built"):get(0) ~= 0 and reaches(meta) then
      local key = meta:field("anchor_key"):get(0)
      local tunnel = nil
      for _, candidate in ipairs(step.links.tunnels) do
        if candidate.tunnel_meta:field("built"):get(0) ~= 0 then
          local pieces = candidate.tunnel_pieces
          local tags = pieces:field("station")
          for i = 0, pieces:count() - 1 do
            if tags:get(i) == key then tunnel = candidate break end
          end
        end
        if tunnel then break end
      end
      if tunnel then
        clip(station.station_mesh, tunnel.tunnel_pieces, key, tunnel.tunnel_pieces:count())
      else
        -- При пустом списке исключений поля нужны только для формы вызова.
        local dummy = step.links.tunnels[1] and step.links.tunnels[1].tunnel_pieces
        if dummy then clip(station.station_mesh, dummy, key, 0)
        else
          local mesh = station.station_mesh
          params.tag, params.exclusions = key, 0
          originator.clip_triangle_mesh{
            inputs = { mesh:field("a"), mesh:field("b"), mesh:field("c"), mesh:field("normal"), mesh:field("shade"),
                       station.station_rock:field("centre"), station.station_rock:field("axis_u"),
                       station.station_rock:field("axis_v"), station.station_rock:field("centre"),
                       station.station_rock:field("half"), meta:field("anchor_key") },
            outputs = { out:field("position"), out:field("normal"), out:field("shade"), count },
            params = params }
        end
      end
    end
  end
  for _, tunnel in ipairs(step.links.tunnels) do
    local meta = tunnel.tunnel_meta
    if meta:field("built"):get(0) ~= 0 and reaches(meta) then
      clip(tunnel.tunnel_mesh, tunnel.tunnel_pieces, 0, 0)
    end
  end
end
