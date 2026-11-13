local W = modules.world
return function(step)
  local raw = step.key[3] == 0 and step.reads.ribs or step.reads.buttresses
  local attempts, probes = step.reads.attempts, step.writes.probes
  local meta = step.writes.features_meta
  local chosen = {}
  local nr, nd, nh, rejected, curtains, severed = 0, 0, 0, 0, 0, 0
  for i = 0, meta:field('attempts'):get(0) - 1 do
    for option = 0, 5 do
      local first = i * 36 + option * 6
      local solid = true
      for j = 0, 4 do solid = solid and probes:field('value'):get(first + j) > 0.25 end
      local source = attempts:field('source'):get(i)
      if raw:field('class'):get(source) == 1 then solid = solid and probes:field('raw'):get(first + 5) > 0.25 end
      if solid then
        local p = W.get(probes:field('position'), first)
        chosen[i] = p
        break
      end
    end
    if not chosen[i] then rejected = rejected + 1 end
  end
  local function record_root(index)
    if not chosen[index] then return end
    W.put(step.writes.roots:field('position'), nr, chosen[index])
    W.put(step.writes.roots:field('direction'), nr, W.get(attempts:field('direction'), index))
    step.writes.roots:field('radius'):set(nr, attempts:field('radius'):get(index))
    nr = nr + 1
  end
  local scratch = step.writes.probes
  local function continuous(a, b)
    local delta = W.sub(b, a)
    local count = math.max(1, math.ceil(W.length(delta) / 3))
    assert(count + 1 <= scratch:count(), 'feature continuity probe capacity exceeded')
    for j = 0, count do W.put(scratch:field('position'), j, W.add(a, W.scale(delta, j / count))) end
    originator.field_sample{inputs = {scratch:field('position')}, outputs = {scratch:field('value')},
      params = {field = 'protected', count = count + 1, half = 32}}
    for j = 0, count do
      if scratch:field('value'):get(j) < 0.5 then return false end
    end
    return true
  end
  local function detail(a, b, r, sa, sb, tag)
    assert(nd < step.writes.details:count(), 'detail capacity exceeded')
    W.capsule(step.writes.details, nd, a, b, r, tag)
    step.writes.details:field('start'):set(nd, sa)
    step.writes.details:field('finish'):set(nd, sb)
    nd = nd + 1
  end
  local function heap(p, r, key, tag)
    local function block(c, h, angular, phase)
      assert(nh < step.writes.heaps:count(), 'heap capacity exceeded')
      W.ball(step.writes.heaps, nh, c, h, tag)
      step.writes.heaps:field('angular'):set(nh, angular)
      step.writes.heaps:field('phase'):set(nh, phase)
      step.writes.heaps:field('tilt'):set(nh, angular * 0.6 * math.sin(phase * 2.3), 0)
      step.writes.heaps:field('tilt'):set(nh, angular * 0.55 * math.cos(phase * 1.7), 1)
      nh = nh + 1
    end
    block(W.add(p, {x = 0, y = 9, z = 0}), {x = r * 1.1, y = 7, z = r * 0.7}, 0, 0)
    for j = 0, 5 do
      local phase = base.prng64_normalize(base.mix(step.world_seed, key, j, 1010)) * math.pi * 2
      local dx, dz = math.cos(phase) * r * 0.65, math.sin(phase) * r * 0.45
      local h = {x = 5 + j % 3, y = 6 + j % 4, z = 4 + (j + 1) % 3}
      local mound = 9 + 7 * math.sqrt(math.max(0, 1 - (dx / (r * 1.1)) ^ 2 - (dz / (r * 0.7)) ^ 2))
      -- Each block penetrates the mound instead of floating above its curved surface.
      local c = W.add(p, {x = dx, y = mound + h.y * 0.5, z = dz})
      if continuous(W.add(p, {x = dx, y = 0, z = dz}), c) then block(c, h, 0.8, phase) end
    end
  end
  if step.key[3] == 0 then
    local previous = {}
    for i = 0, meta:field('attempts'):get(0) - 1 do
      local source = attempts:field('source'):get(i)
      local root = chosen[i]
      if root then
        local b = W.get(raw:field('b'), source)
        local radius = raw:field('radius'):get(source)
        local tag = raw:field('tag'):get(source)
        local key = raw:field('chain'):get(source)
        if raw:field('class'):get(source) == 1 then
          heap(root, radius, key, tag)
          record_root(i)
        elseif continuous(root, b) then
          record_root(i)
          detail(root, b, radius, 1, raw:field('finish'):get(source), tag)
          local ridge = W.add(W.scale(root, 0.35), W.scale(b, 0.65))
          local old = previous[key]
          if old and W.length(W.sub(old, ridge)) < constants.feature_chain_gap and continuous(old, ridge) then
            detail(old, ridge, radius * 0.55, 0.85, 0.85, tag)
            curtains = curtains + 1
          end
          previous[key] = ridge
        else
          severed = severed + 1
          previous[key] = nil
        end
      end
    end
  else
    local groups, order = {}, {}
    for i = 0, raw:count() - 1 do
      local key = raw:field('chain'):get(i)
      if not groups[key] then
        groups[key] = {}
        order[#order + 1] = key
      end
      groups[key][#groups[key] + 1] = i
    end
    for group, key in ipairs(order) do
      local rows = groups[key]
      local first, last = chosen[(group - 1) * 2], chosen[(group - 1) * 2 + 1]
      local keep = first or last
      for j, i in ipairs(rows) do
        local a = j == 1 and first or W.get(raw:field('a'), i)
        local b = j == #rows and last or W.get(raw:field('b'), i)
        if keep and not continuous(a, b) then
          keep = false
          severed = severed + 1
        end
      end
      if keep then
        record_root((group - 1) * 2)
        record_root((group - 1) * 2 + 1)
        for j, i in ipairs(rows) do
          local a = j == 1 and first or W.get(raw:field('a'), i)
          local b = j == #rows and last or W.get(raw:field('b'), i)
          detail(a, b, raw:field('radius'):get(i), raw:field('start'):get(i), raw:field('finish'):get(i),
            raw:field('tag'):get(i))
        end
      end
    end
  end
  meta:field('details'):set(0, nd)
  meta:field('heaps'):set(0, nh)
  meta:field('roots'):set(0, nr)
  meta:field('rejected'):set(0, rejected)
  meta:field('curtains'):set(0, curtains)
  meta:field('severed'):set(0, severed)
end
