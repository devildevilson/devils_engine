local F = {}
function F.write(step, buffer, meta, count, kind)
  meta:field('feature_key'):set(0, step.key[1], 0)
  meta:field('feature_key'):set(0, step.key[2], 1)
  meta:field('feature_key'):set(0, kind, 2)
  local low, high = {x = 1e30, y = 1e30, z = 1e30}, {x = -1e30, y = -1e30, z = -1e30}
  local W = modules.world
  for i = 0, count - 1 do
    local a, b = W.get(buffer:field('a'), i), W.get(buffer:field('b'), i)
    local pad = constants.feature_root_search + buffer:field('radius'):get(i) + 64
    for _, axis in ipairs({'x', 'y', 'z'}) do
      low[axis] = math.min(low[axis], a[axis] - pad, b[axis] - pad)
      high[axis] = math.max(high[axis], a[axis] + pad, b[axis] + pad)
    end
  end
  W.put(meta:field('feature_low'), 0, low)
  W.put(meta:field('feature_high'), 0, high)
end
return F
