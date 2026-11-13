-- Areas are meaningful spaces; intermediate curve samples do not become graph nodes.
local W, C = modules.world, modules.caves
return function(step)
  local r = function(salt) return C.random(step.world_seed, step.key[1], step.key[2], step.key[3], salt) end
  local site = step.reads.cave_site
  local centre = W.get(site:field('centre'), 0)
  local entry = W.get(site:field('entry'), 0)
  local forward = W.get(site:field('direction'), 0)
  local angle = math.atan(forward.z, forward.x)
  if math.abs(forward.x) + math.abs(forward.z) < 0.1 then angle = r(1110) * math.pi * 2 end
  local nodes, edges, linked = {}, {}, {}
  local function area(p, heading, kind, parent)
    local delta = W.sub(p, centre)
    local scale = math.max(1, math.abs(delta.x) / 165, math.abs(delta.y) / 100, math.abs(delta.z) / 165)
    p = W.add(centre, W.scale(delta, 1 / scale))
    local i = #nodes
    local half
    if kind == 0 then
      half = {x = 5, y = 4, z = 6}
    elseif kind == 1 then
      half = {x = 19 + 12 * r(1200 + i), y = 10 + 11 * r(1300 + i), z = 19 + 12 * r(1400 + i)}
    elseif kind == 2 then
      half = {x = 8 + 7 * r(1200 + i), y = 6 + 7 * r(1300 + i), z = 20 + 11 * r(1400 + i)}
    elseif kind == 3 then
      half = {x = 6 + 4 * r(1200 + i), y = 19 + 10 * r(1300 + i), z = 7 + 5 * r(1400 + i)}
    elseif kind == 4 then
      half = {x = 7 + 7 * r(1200 + i), y = 5 + 6 * r(1300 + i), z = 9 + 8 * r(1400 + i)}
    else
      half = {x = 18 + 10 * r(1200 + i), y = 2.8 + 2 * r(1300 + i), z = 16 + 10 * r(1400 + i)}
    end
    nodes[#nodes + 1] =
        {p = p, heading = heading, kind = kind, parent = parent, degree = 0, half = half,
          roll = (r(1500 + i) - 0.5) * 0.65}
    return i
  end
  local function connect(a, b, loop, kind)
    local key = math.min(a, b) .. ':' .. math.max(a, b)
    if a == b or linked[key] or W.length(W.sub(nodes[a + 1].p, nodes[b + 1].p)) < 0.1 then return end
    linked[key] = true
    edges[#edges + 1] = {a = a, b = b, loop = loop, kind = kind}
    nodes[a + 1].degree = nodes[a + 1].degree + 1
    nodes[b + 1].degree = nodes[b + 1].degree + 1
    return true
  end
  area(entry, forward, 0, -1)
  area(centre, forward, 1, 0)
  connect(0, 1, false, 3)
  local count = 3 + math.floor(r(1600) * 3)
  local vertical = 2 + math.floor(r(1601) * (count - 1))
  local sign = r(1602) < 0.5 and -1 or 1
  for i = 2, count do
    angle = angle + (r(1700 + i) - 0.5) * 1.4
    local horizontal = vertical == i and 12 + 12 * r(1800 + i) or 30 + 18 * r(1800 + i)
    local dy = vertical == i and sign * (28 + 20 * r(1900 + i)) or (r(1900 + i) - 0.5) * 18
    local delta = {x = math.cos(angle) * horizontal, y = dy, z = math.sin(angle) * horizontal}
    local previous = nodes[#nodes]
    local kind = vertical == i and 3 or (i % 2 == 0 and 2 or 1)
    local index = area(W.add(previous.p, delta), W.normal(delta), kind, #nodes - 1)
    connect(index - 1, index, false, kind == 2 and 1 or 0)
  end
  local branches = {}
  for i = 1, 1 + math.floor(r(2000) * 3) do
    local parent = 1 + math.floor(r(2100 + i) * count)
    local previous = nodes[parent + 1]
    local turn = angle + (i % 2 == 0 and -1 or 1) * (1.1 + 0.6 * r(2200 + i))
    local delta =
        {x = math.cos(turn) * (28 + 18 * r(2300 + i)), y = (r(2400 + i) - 0.5) * 48,
          z = math.sin(turn) * (28 + 18 * r(2500 + i))}
    local kind = r(2600 + i) < constants.cave_low_room_probability and 5 or 4
    local index = area(W.add(previous.p, delta), W.normal(delta), kind, parent)
    local slit = r(2700 + i) < constants.cave_slit_probability
    connect(parent, index, false, slit and 2 or 0)
    branches[#branches + 1] = index
  end
  local loops = 0
  for _, a in ipairs(branches) do
    for b = 1, count do
      if b ~= nodes[a + 1].parent and math.abs(b - nodes[a + 1].parent) >= 2
          and W.length(W.sub(nodes[a + 1].p, nodes[b + 1].p)) < 90 and r(3000 + a * 31 + b) < 0.45 then
        if connect(a, b, true, 0) then loops = loops + 1 end
        break
      end
    end
  end
  assert(#nodes <= step.writes.cave_nodes:count() and #edges <= step.writes.cave_edges:count(),
    'cave composition capacity exceeded')
  for i, node in ipairs(nodes) do
    W.put(step.writes.cave_nodes:field('centre'), i - 1, node.p)
    W.put(step.writes.cave_nodes:field('heading'), i - 1, node.heading)
    W.put(step.writes.cave_nodes:field('half'), i - 1, node.half)
    for _, name in ipairs({'parent', 'degree', 'kind', 'roll'}) do
      step.writes.cave_nodes:field(name):set(i - 1, node[name])
    end
  end
  for i, e in ipairs(edges) do
    step.writes.cave_edges:field('a'):set(i - 1, e.a)
    step.writes.cave_edges:field('b'):set(i - 1, e.b)
    step.writes.cave_edges:field('loop'):set(i - 1, e.loop and 1 or 0)
    step.writes.cave_edges:field('kind'):set(i - 1, e.kind)
  end
  local meta = step.writes.cave_meta
  for _, name in ipairs({'origin', 'centre', 'entry', 'direction'}) do
    W.put(meta:field(name), 0, W.get(site:field(name), 0))
  end
  meta:field('has_entry'):set(0, site:field('has_entry'):get(0))
  meta:field('site_quality'):set(0, site:field('quality'):get(0))
  W.put(meta:field('low'), 0, {x = 1e30, y = 1e30, z = 1e30})
  W.put(meta:field('high'), 0, {x = -1e30, y = -1e30, z = -1e30})
  meta:field('nodes'):set(0, #nodes)
  meta:field('edges'):set(0, #edges)
  meta:field('loops'):set(0, loops)
end
