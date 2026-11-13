-- The same elliptical gallery field as the macro terrain; profiles vary at metre scale.
local W, C = modules.world, modules.caves
return function(step)
  local nodes, edges = step.reads.cave_nodes, step.reads.cave_edges
  local passages, meta = step.writes.passages, step.writes.cave_meta
  local r = function(salt) return C.random(step.world_seed, step.key[1], step.key[2], step.key[3], salt) end
  local low, high = W.get(meta:field('low'), 0), W.get(meta:field('high'), 0)
  local made, slits, details = 0, 0, 0
  local detail_low, detail_high = {x = math.huge, y = math.huge, z = math.huge},
      {x = -math.huge, y = -math.huge, z = -math.huge}
  local write_segment = originator.algorithms.segment_writer(passages)
  for edge = 0, meta:field('edges'):get(0) - 1 do
    local a = W.get(nodes:field('centre'), edges:field('a'):get(edge))
    local b = W.get(nodes:field('centre'), edges:field('b'):get(edge))
    local delta = W.sub(b, a)
    local length = W.length(delta)
    local right, up = C.frame(delta)
    local phase = r(3000 + edge) * math.pi * 2
    local kind = edges:field('kind'):get(edge)
    local width, roof, floor
    if kind == 2 then
      width = 0.6 + 0.6 * r(3200 + edge)
      roof = 0.36 + 0.18 * r(3300 + edge)
      floor = 0.36 + 0.14 * r(3400 + edge)
      slits = slits + 1
    elseif kind == 3 then
      local shape = r(5000)
      if shape < 0.33 then
        width = 2.8 + r(3200 + edge)
        roof = 6 + 2 * r(3300 + edge)
        floor = 4 + 2 * r(3400 + edge)
      elseif shape < 0.66 then
        width = 7 + 3 * r(3200 + edge)
        roof = 2.5 + r(3300 + edge)
        floor = 2.4 + r(3400 + edge)
      else
        width = 4 + 3 * r(3200 + edge)
        roof = 3 + 2 * r(3300 + edge)
        floor = 2.5 + 2 * r(3400 + edge)
      end
    elseif kind == 1 then
      width = 6 + 3 * r(3200 + edge)
      roof = 4 + 3 * r(3300 + edge)
      floor = 3.5 + 2 * r(3400 + edge)
    else
      width = 2.6 + 3 * r(3200 + edge)
      roof = 2.3 + 2 * r(3300 + edge)
      floor = 2.1 + 1.8 * r(3400 + edge)
    end
    local steps = math.max(3, math.ceil(length / 2.5))
    local function point(t)
      local bend = math.sin(t * math.pi) * math.sin(t * math.pi + phase) * length * 0.12
      return W.add(W.add(a, W.scale(delta, t)),
        W.add(W.scale(right, bend), W.scale(up, math.sin(t * math.pi) * math.sin(phase) * length * 0.07)))
    end
    local function profile(t)
      local gain = 1 + 0.12 * math.sin(t * length / 3.7 + phase)
      local flare = kind == 3 and (1 + 0.45 * (1 - t)) or 1
      return width * gain * flare, math.max(0.34, roof * (1 + 0.1 * math.sin(t * length / 4.3 + phase))),
          math.max(0.34, floor * (1 + 0.1 * math.cos(t * length / 4.8 + phase)))
    end
    local last = point(0)
    for j = 1, steps do
      assert(made < passages:count(), 'cave passage capacity exceeded')
      local p = point(j / steps)
      local wa, ra, fa = profile((j - 1) / steps)
      local wb, rb, fb = profile(j / steps)
      local local_right, local_up, tangent = write_segment(made, last, p)
      for _, item in ipairs(
        {{'width_a', wa}, {'width_b', wb}, {'roof_a', ra}, {'roof_b', rb}, {'floor_a', fa}, {'floor_b', fb}}) do
        passages:field(item[1]):set(made, item[2])
      end
      local maximum, minimum = math.max(wa, wb, ra, rb, fa, fb), math.min(wa, wb, ra, rb, fa, fb)
      local relief = math.min(0.5, minimum * 0.08)
      local reach = maximum * (1 + (relief + 3.5) / minimum) + 1
      passages:field('relief'):set(made, relief)
      passages:field('phase'):set(made, phase)
      passages:field('reach'):set(made, reach)
      passages:field('tag'):set(made, 5)
      for _, q in ipairs({last, p}) do
        for _, axis in ipairs({'x', 'y', 'z'}) do
          low[axis] = math.min(low[axis], q[axis] - reach)
          high[axis] = math.max(high[axis], q[axis] + reach)
        end
      end
      if minimum < 0.8 then
        assert(details < step.writes.cave_details:count(), 'cave refinement capacity exceeded')
        -- Zero-surface bounds, not the much larger positive SDF influence envelope.
        local lo, hi = {}, {}
        local max_width, max_height = math.max(wa, wb), math.max(ra, rb, fa, fb)
        local cap = math.min(max_width, math.max(ra, rb), math.max(fa, fb))
        for _, axis in ipairs({'x', 'y', 'z'}) do
          local pad =
              math.sqrt((local_right[axis] * max_width) ^ 2 + (local_up[axis] * max_height) ^ 2
                + (tangent[axis] * cap) ^ 2) * (1 + relief / minimum) + 0.8
          lo[axis] = math.min(last[axis], p[axis]) - pad
          hi[axis] = math.max(last[axis], p[axis]) + pad
        end
        W.put(step.writes.cave_details:field('low'), details, lo)
        W.put(step.writes.cave_details:field('high'), details, hi)
        details = details + 1
        for _, axis in ipairs({'x', 'y', 'z'}) do
          detail_low[axis] = math.min(detail_low[axis], lo[axis])
          detail_high[axis] = math.max(detail_high[axis], hi[axis])
        end
      end
      made = made + 1
      last = p
    end
  end
  meta:field('passages'):set(0, made)
  meta:field('slits'):set(0, slits)
  W.put(meta:field('low'), 0, low)
  W.put(meta:field('high'), 0, high)
  step.writes.cave_detail_meta:field('count'):set(0, details)
  -- Bounds of all refinement boxes: a chunk far from them skips the whole list.
  W.put(step.writes.cave_detail_meta:field('low'), 0, detail_low)
  W.put(step.writes.cave_detail_meta:field('high'), 0, detail_high)
end
