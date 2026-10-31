-- Поверхность чанка: марширующие тетраэдры по решётке с полосой; порода клетки — метка поля `solid` у
-- середины поверхности клетки, она же у каждой вершины клетки.

return function(step)
  local cells = math.tointeger(step.params.chunk_cells)
  local surface, middles, counts = step.writes.surface, step.writes.middles, step.writes.surface_counts
  originator.marching_tetrahedra{
    inputs = { step.reads.samples:field("value"), step.reads.lattice:field("position") },
    outputs = { surface:field("position"), surface:field("normal"), surface:field("cell"), middles:field("centre"),
                counts:field("vertices"), counts:field("middles") },
    params = { border = 1 } }
  local made = math.tointeger(counts:field("middles"):get(0))
  if made == 0 then return end
  originator.field_sample{ inputs = { middles:field("centre") }, outputs = { middles:field("value"), middles:field("tag") }, range = { count = counts:field("middles") },
                           params = { field = "solid", half = cells + 2 } }
  originator.lookup{ inputs = { surface:field("cell"), middles:field("tag") }, outputs = { surface:field("shade") },
                     range = { count = counts:field("vertices") } }
end
