-- A coarse preference for usable rock, with one entrance ray and no retries for adventure metrics.
local W, C = modules.world, modules.caves
return function(step)
  local origin = C.locate(step.world_seed, step.key[1], step.key[2], step.key[3])
  local probe, site = step.writes.cave_site_probe, step.writes.cave_site
  local points = {}
  local function index(x, y, z) return x + 5 * (y + 5 * z) + 1 end
  for z = 0, 4 do
    for y = 0, 4 do
      for x = 0, 4 do
        local p = W.add(origin, {x = (x - 2) * 40, y = (y - 2) * 40, z = (z - 2) * 40})
        points[#points + 1] = p
        W.put(probe:field('position'), #points - 1, p)
      end
    end
  end
  originator.field_sample{inputs = {probe:field('position')}, outputs = {probe:field('value')},
    params = {field = 'base', count = #points, half = 32}}
  local water = {}
  for i, p in ipairs(points) do
    if probe:field('value'):get(i - 1) < 0 then water[#water + 1] = p end
  end
  local best, quality, chosen, approach = -math.huge, 0, origin, nil
  for z = 1, 3 do
    for y = 1, 3 do
      for x = 1, 3 do
        local i = index(x, y, z)
        if probe:field('value'):get(i - 1) > 0 then
          local rock = 0
          for dz = -1, 1 do
            for dy = -1, 1 do
              for dx = -1, 1 do
                if probe:field('value'):get(index(x + dx, y + dy, z + dz) - 1) > 0 then rock = rock + 1 end
              end
            end
          end
          local nearest, distance = nil, math.huge
          for _, p in ipairs(water) do
            local length = W.length(W.sub(points[i], p))
            if length < distance then nearest, distance = p, length end
          end
          local rank = rock / 27 * 4 - W.length(W.sub(points[i], origin)) * 0.002 + (nearest and -distance * 0.001 or 0)
          rank = rank + C.random(step.world_seed, step.key[1], step.key[2], step.key[3], 7000 + i) * 0.15
          if rank > best then
            best = rank
            quality = rock / 27
            chosen = points[i]
            approach = nearest
          end
        end
      end
    end
  end
  local r = C.random(step.world_seed, step.key[1], step.key[2], step.key[3], 7200) * math.pi * 2
  local direction = {x = math.cos(r), y = 0, z = math.sin(r)}
  local entry = W.add(chosen, W.scale(direction, -30))
  if approach then
    local length = W.length(W.sub(chosen, approach))
    direction = W.normal(W.sub(chosen, approach))
    W.put(probe:field('position'), 0, approach)
    W.put(probe:field('direction'), 0, direction)
    originator.field_march{inputs = {probe:field('position'), probe:field('direction')},
      outputs = {probe:field('hit')},
      params = {field = 'base', count = 1, from = 0, to = length + 2, step = 1.5, refine = 5, want = 'rock', half = 16}}
    local hit = probe:field('hit'):get(0)
    entry = hit >= 0 and W.add(approach, W.scale(direction, hit)) or approach
  end
  W.put(site:field('origin'), 0, origin)
  W.put(site:field('centre'), 0, chosen)
  W.put(site:field('entry'), 0, entry)
  W.put(site:field('direction'), 0, direction)
  site:field('quality'):set(0, quality)
  site:field('has_entry'):set(0, approach and 1 or 0)
end
