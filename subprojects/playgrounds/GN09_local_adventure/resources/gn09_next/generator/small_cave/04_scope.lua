local C = modules.caves
return function(step)
  local side = step.params.chunk_cells
  local x, y, z = (step.key[1] + 0.5) * side, (step.key[2] + 0.5) * side, (step.key[3] + 0.5) * side
  local span = constants.cave_system_spacing
  local radius = math.ceil(constants.cave_system_reach / span + 0.225)
  local count = 0
  for iz = math.floor(z / span) - radius, math.floor(z / span) + radius do
    for ix = math.floor(x / span) - radius, math.floor(x / span) + radius do
      local base = C.locate(step.world_seed, ix, iz, 0)
      local first = math.ceil((y - side - constants.cave_system_height - base.y - 80) / constants.cave_layer_spacing)
      local last = math.floor((y + side + constants.cave_system_height - base.y + 80) / constants.cave_layer_spacing)
      for layer = first, last do
        local p = C.locate(step.world_seed, ix, iz, layer)
        if math.abs(p.x - x) <= constants.cave_system_reach + side
            and math.abs(p.z - z) <= constants.cave_system_reach + side
            and math.abs(p.y - y) <= constants.cave_system_height + side then
          assert(count < step.writes.small_cave_keys:count(), 'cave scope capacity exceeded')
          local keys = step.writes.small_cave_keys:field('key')
          keys:set(count, ix, 0)
          keys:set(count, iz, 1)
          keys:set(count, layer, 2)
          count = count + 1
        end
      end
    end
  end
  step.writes.small_cave_count:field('value'):set(0, count)
end
