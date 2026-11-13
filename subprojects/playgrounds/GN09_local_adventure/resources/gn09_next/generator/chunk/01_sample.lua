-- Биом чанка и быстрый отказ: узлы решётки — мировые метры от угла чанка минус один узел полосы,
-- порядок x, затем y, затем z. Если над решёткой нет ни одной формы, а фон — порода, поверхности нет.
return function(step)
  step.writes.surface_counts:field("props"):set(0, 0)
  local side = step.params.chunk_cells
  local biome = modules.biomes.sample(step.world_seed, (step.key[1] + 0.5) * side, (step.key[3] + 0.5) * side)
  step.writes.chunk_meta:field('biome'):set(0, biome.kind)
  local cells = math.tointeger(step.params.chunk_cells)
  local lattice = step.writes.lattice:field("position")
  originator.position_grid{outputs = {lattice},
    params = {origin_x = step.key[1] * cells - 1, origin_y = step.key[2] * cells - 1,
      origin_z = step.key[3] * cells - 1, cell_size = 1}}
  local counts = step.writes.surface_counts
  local lower = step.writes.chunk_meta:field("lower")
  originator.field_bound{inputs = {lattice}, outputs = {counts:field("shapes"), lower}, params = {field = "solid"}}
  local solid = counts:field("shapes"):get(0) == 0 and lower:get(0) > 0
  counts:field("sampled"):set(0, solid and 0 or 1)
end
