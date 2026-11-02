-- Поверхность чанка: марширующие тетраэдры по решётке с полосой; порода клетки — метка поля `solid` у
-- середины поверхности клетки, она же у каждой вершины клетки.

return function(step)
  local cells = math.tointeger(step.params.chunk_cells)
  local surface, middles, counts = step.writes.surface, step.writes.middles, step.writes.surface_counts
  if counts:field("sampled"):get(0) == 0 then
    counts:field("vertices"):set(0, 0)
    counts:field("middles"):set(0, 0)
    return
  end
  originator.marching_tetrahedra{
    inputs = { step.reads.samples:field("value"), step.reads.lattice:field("position") },
    outputs = { surface:field("position"), surface:field("normal"), surface:field("cell"), middles:field("centre"),
                counts:field("vertices"), counts:field("middles") },
    params = { border = 1 } }
  local made = math.tointeger(counts:field("middles"):get(0))
  if made == 0 then return end
  originator.field_sample{ inputs = { middles:field("centre") }, outputs = { middles:field("value"), middles:field("tag") },
                           params = { field = "solid", half = cells + 2, count = made } }
  originator.lookup{ inputs = { surface:field("cell"), middles:field("tag") }, outputs = { surface:field("shade") },
                     range = { count = counts:field("vertices") } }
end
