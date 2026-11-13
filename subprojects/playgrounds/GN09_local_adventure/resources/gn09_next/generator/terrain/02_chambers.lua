-- Voronoi masks are diagnostic; physical chambers have their own curved boundaries.
local W, B = modules.world, modules.biomes
return function(step)
  local biome = step.reads.biome
  local kind = biome:field('kind'):get(0)
  local centre = W.get(biome:field('centre'), 0)
  local span = B.span()
  local reach = math.sqrt(2) * (0.5 + constants.biome_jitter) * span + constants.biome_warp
      + constants.biome_transition * 4
  local low = W.add(centre, {x = -reach, y = -600, z = -reach})
  local high = W.add(centre, {x = reach, y = 600, z = reach})
  local phase_a = base.prng64_normalize(base.mix(step.world_seed, 703)) * math.pi * 2
  local phase_b = base.prng64_normalize(base.mix(step.world_seed, 704)) * math.pi * 2
  local clips = step.writes.biome_clips
  local nc = 0
  for dz = -2, 2 do
    for dx = -2, 2 do
      if dx ~= 0 or dz ~= 0 then
        local other = B.site(step.world_seed, step.key[1] + dx, step.key[2] + dz)
        W.put(clips:field('low'), nc, low)
        W.put(clips:field('high'), nc, high)
        clips:field('owner'):set(nc, centre.x, 0)
        clips:field('owner'):set(nc, centre.z, 1)
        clips:field('other'):set(nc, other.x, 0)
        clips:field('other'):set(nc, other.z, 1)
        clips:field('phase_a'):set(nc, phase_a)
        clips:field('phase_b'):set(nc, phase_b)
        clips:field('span'):set(nc, span)
        clips:field('warp'):set(nc, constants.biome_warp)
        clips:field('margin'):set(nc, constants.biome_transition / 4)
        nc = nc + 1
      end
    end
  end
  local cell = step.writes.biome_cell
  -- Leave a halo for the bounded domain SDF; child planes cover this whole support.
  W.put(cell:field('low'), 0, W.add(low, {x = 4, y = 4, z = 4}))
  W.put(cell:field('high'), 0, W.add(high, {x = -4, y = -4, z = -4}))
  cell:field('first_clip'):set(0, 0)
  cell:field('clip_count'):set(0, nc)

  local count, ribs, nocean = 0, 0, 0
  local function chamber(p, half, relief, phase)
    local bound = math.max(half.x, half.y, half.z) * (1 + (relief + 3.5) / math.min(half.x, half.y, half.z)) + 1
    assert(W.length(W.sub(p, centre)) + bound <= constants.biome_max_reach, 'chamber exceeds its declared reach')
    W.ball(step.writes.caverns, count, p, half, 3)
    step.writes.caverns:field('first_clip'):set(count, 0)
    step.writes.caverns:field('clip_count'):set(count, 0)
    step.writes.caverns:field('relief'):set(count, relief)
    step.writes.caverns:field('phase'):set(count, phase)
    count = count + 1
  end
  if kind == 3 then
    -- A single basin avoids scalloped or foaming bottoms made from overlapping balls.
    W.put(step.writes.ocean:field('centre'), 0, centre)
    local roof = constants.world_y_max - centre.y + 40
    W.put(step.writes.ocean:field('half'), 0, {x = 235, y = roof, z = 210})
    W.put(step.writes.ocean:field('low'), 0, W.add(centre, {x = -285, y = -115, z = -260}))
    W.put(step.writes.ocean:field('high'), 0, W.add(centre, {x = 285, y = roof * 1.17 + 4, z = 250}))
    step.writes.ocean:field('depth'):set(0, 70)
    step.writes.ocean:field('relief'):set(0, 3)
    nocean = 1
    -- Sparse bent sea stacks rise from below the seabed, never from an invisible ceiling.
    for stack = 0, 2 do
      local phase = base.prng64_normalize(base.mix(step.world_seed, step.key[1], step.key[2], stack, 954)) * math.pi * 2
      local root = W.add(centre, {x = math.cos(phase) * 130, y = -105, z = math.sin(phase) * 115})
      local previous = root
      for j = 1, 4 do
        local t = j / 4
        local q =
            W.add(root,
              {x = 14 * math.sin(t * 2.4 + phase) - 14 * math.sin(phase), y = 85 * t, z = 12 * math.sin(t * 3.1)})
        W.capsule(step.writes.buttresses, ribs, previous, q, 18, 4)
        step.writes.buttresses:field('chain'):set(ribs,
          base.mix(step.world_seed, step.key[1], step.key[2], stack, 0, 1003))
        step.writes.buttresses:field('start'):set(ribs, 1 - 0.85 * (j - 1) / 4)
        step.writes.buttresses:field('finish'):set(ribs, 1 - 0.85 * t)
        ribs = ribs + 1
        previous = q
      end
    end
  elseif kind == 2 then
    local tall = base.umod(base.mix(step.world_seed, step.key[1], step.key[2], 951), 5) == 0
    chamber(centre, {x = 185, y = tall and 78 or 20, z = 155}, tall and 5 or 2, 0)
    for i = 0, 2 do
      local phase = base.prng64_normalize(base.mix(step.world_seed, step.key[1], step.key[2], i, 952)) * math.pi * 2
      local p = W.add(centre, {x = math.cos(phase) * 95, y = math.sin(phase) * 6, z = math.sin(phase) * 85})
      chamber(p, {x = 100, y = tall and 50 or 17, z = 85}, 2, phase)
    end
    -- Bent rock bridges and supports, tapered continuously over their full height.
    for pillar = 0, (tall and 4 or 2) do
      local phase =
          base.prng64_normalize(base.mix(step.world_seed, step.key[1], step.key[2], pillar, 953)) * math.pi * 2
      local height = tall and 125 or 55
      local anchor = W.add(centre, {x = math.cos(phase) * 110, y = -height, z = math.sin(phase) * 95})
      local previous = anchor
      for j = 1, 5 do
        local t = j / 5
        local q =
            W.add(anchor,
              {x = 22 * math.sin(t * math.pi) * math.cos(phase), y = 2 * height * t,
                z = 18 * math.sin(t * math.pi * 1.7 + phase) - 18 * math.sin(phase)})
        W.capsule(step.writes.buttresses, ribs, previous, q, 13, 3)
        local function thickness(u) return 0.4 + 0.6 * math.abs(2 * u - 1) end
        step.writes.buttresses:field('chain'):set(ribs,
          base.mix(step.world_seed, step.key[1], step.key[2], 0, pillar, 1003))
        step.writes.buttresses:field('start'):set(ribs, thickness((j - 1) / 5))
        step.writes.buttresses:field('finish'):set(ribs, thickness(t))
        ribs = ribs + 1
        previous = q
      end
    end
  end
  local meta = step.writes.cavern_meta
  meta:field('caverns'):set(0, count)
  meta:field('ocean'):set(0, nocean)
  meta:field('buttresses'):set(0, ribs)
  meta:field('clips'):set(0, nc)
  meta:field('cells'):set(0, 1)
  modules.feature_bounds.write(step, step.writes.buttresses, meta, ribs, 1)
end
