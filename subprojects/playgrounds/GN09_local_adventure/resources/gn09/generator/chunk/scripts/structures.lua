-- Треугольники станций и туннелей чанка (блок structure_surface): станции якорей чанка без проёмов своего
-- туннеля, затем туннели чанка, всё обрезано по коробке чанка.

return function(step)
  step.writes.chunk_meta:field("seed"):set(0, step.world_seed)
  local out = step.writes.structure_surface
  originator.structure_surface{
    inputs = { step.reads.anchors:field("station"), step.reads.chunk_tunnels:field("tunnel"),
               step.writes.chunk_meta:field("seed") },
    outputs = { out:field("position"), out:field("normal"), out:field("shade"),
                step.writes.surface_counts:field("structure") },
    params = { x = step.key[1], y = step.key[2], z = step.key[3], cells = step.params.chunk_cells,
               anchors = math.tointeger(step.reads.anchor_counts:field("anchors"):get(0)),
               tunnels = math.tointeger(step.reads.tunnel_counts:field("tunnels"):get(0)),
               clearance = step.params.clearance_radius } }
end
