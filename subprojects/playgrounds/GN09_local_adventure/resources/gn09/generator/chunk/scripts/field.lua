-- Сэмплы поля чанка: узлы решётки — мировые метры от угла чанка минус один узел полосы, порядок x, затем
-- y, затем z.

return function(step)
  local cells = math.tointeger(step.params.chunk_cells)
  local lattice = step.writes.lattice:field("position")
  local samples = step.writes.samples:field("value")
  originator.position_grid{ outputs = { lattice },
                            params = { origin_x = step.key[1] * cells - 1, origin_y = step.key[2] * cells - 1,
                                       origin_z = step.key[3] * cells - 1, cell_size = 1 } }
  originator.field_sample{ inputs = { lattice }, outputs = { samples }, params = { field = "solid", half = cells + 2 } }
end
