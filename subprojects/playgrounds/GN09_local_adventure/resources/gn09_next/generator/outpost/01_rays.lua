-- Dock search rays from the main node: horizontal directions at a seeded angle offset, plus up and down.
local W = modules.world
return function(step)
  local p = W.get(step.reads.network_node:field('centre'), 0)
  local rays = step.writes.outpost_rays
  local n = constants.outpost_directions
  assert(n + 2 <= rays:count(), 'outpost ray capacity is below the declared directions')
  local offset = base.prng64_normalize(base.mix(step.world_seed, step.key[1], step.key[2], 930))
  local directions = {{x = 0, y = 1, z = 0}, {x = 0, y = -1, z = 0}}
  for i = 0, n - 1 do
    local angle = (i + offset) * math.pi * 2 / n
    directions[#directions + 1] = {x = math.cos(angle), y = 0, z = math.sin(angle)}
  end
  for i, d in ipairs(directions) do
    W.put(rays:field('origin'), i - 1, p)
    W.put(rays:field('direction'), i - 1, d)
  end
  step.writes.outpost_meta:field('rays'):set(0, #directions)
end
