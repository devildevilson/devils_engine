local W, F = modules.world, modules.frame
return function(step)
  local cut = step.reads.station_cut
  local meta = step.writes.station_meta
  local frame = F.from_meta(meta)
  local low, high = {x = math.huge, y = math.huge, z = math.huge}, {x = -math.huge, y = -math.huge, z = -math.huge}
  -- Influence of the cuts on the field: the sphere of the station_cut shape reach (world.tavl).
  local reach_low, reach_high = {x = math.huge, z = math.huge}, {x = -math.huge, z = -math.huge}
  for i = 0, meta:field('cuts'):get(0) - 1 do
    local half = W.get(cut:field('half'), i)
    local c, h = W.get(cut:field('centre'), i), frame.extent(half)
    for _, axis in ipairs({'x', 'y', 'z'}) do
      low[axis] = math.min(low[axis], c[axis] - h[axis] - 1)
      high[axis] = math.max(high[axis], c[axis] + h[axis] + 1)
    end
    local r = W.length(half) + 3.5
    for _, axis in ipairs({'x', 'z'}) do
      reach_low[axis] = math.min(reach_low[axis], c[axis] - r)
      reach_high[axis] = math.max(reach_high[axis], c[axis] + r)
    end
  end
  -- Chunks find outposts through the terrain scope, whose halo is outpost_search_reach+80 around main nodes.
  local node = modules.network.main(step.world_seed, step.key[1], step.key[2])
  for _, axis in ipairs({'x', 'y', 'z'}) do
    assert(high[axis] - low[axis] <= 2 * constants.station_reach, 'outpost extent exceeds declared station_reach')
  end
  for _, axis in ipairs({'x', 'z'}) do
    assert(math.max(math.abs(low[axis] - node[axis]), math.abs(high[axis] - node[axis]))
      <= constants.outpost_search_reach + 60,
      'outpost leaves the terrain scope halo of its main node')
    -- Chunks link outposts by this bound (chunk/04_station_scope.lua).
    assert(math.max(math.abs(reach_low[axis] - node[axis]), math.abs(reach_high[axis] - node[axis]))
      <= constants.outpost_search_reach + 80,
      'outpost cut influence leaves the station scope bound of its main node')
  end
  W.put(meta:field('low'), 0, low)
  W.put(meta:field('high'), 0, high)
end
