local W = modules.world
local extensions = {0, 0.1, 0.23, 0.42, 0.68, 1}
return function(step)
  local raw = step.key[3] == 0 and step.reads.ribs or step.reads.buttresses
  local attempts, probes = step.writes.attempts, step.writes.probes
  local na, nv = 0, 0
  local groups, order = {}, {}
  if step.key[3] == 1 then
    for i = 0, raw:count() - 1 do
      local key = raw:field('chain'):get(i)
      if not groups[key] then
        groups[key] = {}
        order[#order + 1] = key
      end
      groups[key][#groups[key] + 1] = i
    end
  end
  local function attempt(i, ending)
    assert(na < attempts:count(), 'feature attempt capacity exceeded')
    local a, b = W.get(raw:field('a'), i), W.get(raw:field('b'), i)
    if ending then a, b = b, a end
    local d = W.normal(W.sub(a, b))
    local radius = raw:field('radius'):get(i)
    W.put(attempts:field('origin'), na, a)
    W.put(attempts:field('direction'), na, d)
    attempts:field('radius'):set(na, radius)
    attempts:field('source'):set(na, i)
    attempts:field('end'):set(na, ending and 1 or 0)
    local right = math.abs(d.y) > 0.9 and {x = 1, y = 0, z = 0} or W.normal({x = -d.z, y = 0, z = d.x})
    local up =
        W.normal(
          {x = right.y * d.z - right.z * d.y, y = right.z * d.x - right.x * d.z, z = right.x * d.y - right.y * d.x})
    for _, distance in ipairs(extensions) do
      local p = W.add(a, W.scale(d, distance * constants.feature_root_search + constants.feature_root_embed))
      local points =
          {p, W.add(p, W.scale(right, radius * 0.6)), W.add(p, W.scale(right, -radius * 0.6)),
            W.add(p, W.scale(up, radius * 0.6)), W.add(p, W.scale(up, -radius * 0.6))}
      -- Last probe is a visible heap tip, or a witness of material removed above it.
      points[6] = raw:field('class'):get(i) == 1 and W.get(raw:field('scar'), i) or p
      for _, q in ipairs(points) do
        assert(nv < probes:count(), 'feature probe capacity exceeded')
        W.put(probes:field('position'), nv, q)
        nv = nv + 1
      end
    end
    na = na + 1
  end
  if step.key[3] == 0 then
    for i = 0, raw:count() - 1 do attempt(i, false) end
  else
    for _, key in ipairs(order) do
      local group = groups[key]
      attempt(group[1], false)
      attempt(group[#group], true)
    end
  end
  step.writes.features_meta:field('attempts'):set(0, na)
  step.writes.features_meta:field('probes'):set(0, nv)
end
