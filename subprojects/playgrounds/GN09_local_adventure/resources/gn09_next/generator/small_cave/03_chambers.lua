-- Volumes follow the semantic areas: inclined galleries, shafts and broad chambers.
local W, C = modules.world, modules.caves
return function(step)
  local nodes, meta, rooms = step.reads.cave_nodes, step.writes.cave_meta, step.writes.cave_chambers
  local r = function(salt) return C.random(step.world_seed, step.key[1], step.key[2], step.key[3], salt) end
  local count, halls, current_kind = 0, 0, 0
  local low, high = W.get(meta:field('low'), 0), W.get(meta:field('high'), 0)
  local function chamber(p, half, right, up, forward, relief)
    assert(count < rooms:count(), 'cave chamber capacity exceeded')
    W.ball(rooms, count, p, half, 5)
    rooms:field('kind'):set(count, current_kind)
    W.put(rooms:field('right'), count, right)
    W.put(rooms:field('up'), count, up)
    W.put(rooms:field('forward'), count, forward)
    rooms:field('relief'):set(count, relief)
    rooms:field('phase'):set(count, 0)
    count = count + 1
    local reach = math.max(half.x, half.y, half.z) * (1 + (relief + 3.5) / math.min(half.x, half.y, half.z)) + 1
    for _, axis in ipairs({'x', 'y', 'z'}) do
      low[axis] = math.min(low[axis], p[axis] - reach)
      high[axis] = math.max(high[axis], p[axis] + reach)
    end
  end
  for i = 0, meta:field('nodes'):get(0) - 1 do
    local kind = nodes:field('kind'):get(i)
    current_kind = kind
    local p, half = W.get(nodes:field('centre'), i), W.get(nodes:field('half'), i)
    local heading = W.get(nodes:field('heading'), i)
    if kind == 0 then
      -- An irregular flare blends into the existing rock; no compulsory narrow throat.
      p = W.add(p, W.scale(heading, 4))
      local shape = r(5000)
      if shape < 0.33 then
        half = {x = 2.8, y = 7, z = 6}
      elseif shape < 0.66 then
        half = {x = 9, y = 2.8, z = 7}
      else
        half = {x = 6, y = 4.8, z = 7}
      end
    end
    if kind == 3 then
      heading = {x = heading.x, y = 0, z = heading.z}
    elseif kind ~= 2 then
      heading = {x = heading.x, y = heading.y * 0.3, z = heading.z}
    end
    if W.length(heading) < 0.1 then heading = {x = 1, y = 0, z = 0} end
    local right, up, forward = C.frame(heading)
    local roll = nodes:field('roll'):get(i)
    local tilted_right = W.add(W.scale(right, math.cos(roll)), W.scale(up, math.sin(roll)))
    up = W.add(W.scale(up, math.cos(roll)), W.scale(right, -math.sin(roll)))
    right = tilted_right
    local relief = math.min(0.9, math.min(half.x, half.y, half.z) * 0.08)
    chamber(p, half, right, up, forward, relief)
    if kind ~= 0 then
      if kind == 1 or kind == 5 then halls = halls + 1 end
      local phase = r(5100 + i) * math.pi * 2
      for lobe = 1, 2 do
        local offset =
            W.add(W.scale(right, math.cos(phase + lobe) * half.x * 0.55),
              W.scale(forward, math.sin(phase + lobe) * half.z * 0.55))
        offset = W.add(offset, W.scale(up, (r(5200 + i * 3 + lobe) - 0.5) * half.y * 0.55))
        local h =
            {x = half.x * (0.6 + 0.2 * r(5300 + i * 3 + lobe)), y = half.y * (0.7 + 0.3 * r(5400 + i * 3 + lobe)),
              z = half.z * 0.7}
        chamber(W.add(p, offset), h, right, up, forward, relief * 0.8)
      end
    end
  end
  W.put(meta:field('low'), 0, low)
  W.put(meta:field('high'), 0, high)
  local origin = W.get(meta:field('origin'), 0)
  for _, axis in ipairs({'x', 'y', 'z'}) do
    local limit = axis == 'y' and constants.cave_system_height or constants.cave_system_reach
    assert(low[axis] >= origin[axis] - limit and high[axis] <= origin[axis] + limit,
      'cave exceeds declared streaming reach')
  end
  meta:field('chambers'):set(0, count)
  meta:field('halls'):set(0, halls)
end
