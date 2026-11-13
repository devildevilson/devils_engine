local W, M = modules.world, modules.network
return function(step)
  local x, z, seed = step.key[1], step.key[2], step.world_seed
  local a, neighbours = M.neighbours(seed, x, z)
  local main = step.writes.network_node
  W.put(main:field('centre'), 0, a)
  main:field('key'):set(0, a.key)
  main:field('degree'):set(0, #neighbours)
  main:field('address'):set(0, x, 0)
  main:field('address'):set(0, z, 1)
  main:field('biome'):set(0, step.reads.biome_sample:field('kind'):get(0))
  main:field('blend'):set(0, step.reads.biome_sample:field('blend'):get(0))
  local nodes, edges, paths = step.writes.network_nodes, step.writes.network_edges, step.writes.network_paths
  local nn, ne, nr = 0, 0, 0
  local seen = {}
  local function append(p, is_main)
    if seen[p.key] then return end
    assert(nn < nodes:count(), 'network node capacity exceeded')
    W.put(nodes:field('centre'), nn, p)
    nodes:field('key'):set(nn, p.key)
    nodes:field('main'):set(nn, is_main and 1 or 0)
    nodes:field('owner'):set(nn, p.cx or x, 0)
    nodes:field('owner'):set(nn, p.cz or z, 1)
    seen[p.key] = true
    nn = nn + 1
  end
  local function connect(p, q, route)
    assert(ne < edges:count(), 'network edge capacity exceeded')
    edges:field('a'):set(ne, p.key)
    edges:field('b'):set(ne, q.key)
    edges:field('route'):set(ne, route)
    W.put(paths:field('a'), ne, p)
    W.put(paths:field('b'), ne, q)
    paths:field('route'):set(ne, route)
    ne = ne + 1
  end
  append(a, true)
  for _, candidate in ipairs(neighbours) do
    local b = candidate.point
    -- The lexicographically smaller address owns the entire route.
    if x < b.cx or (x == b.cx and z < b.cz) then
      append(b, true)
      local route = base.mix(seed, x, z, b.cx, b.cz, 805)
      local minimum = math.tointeger(constants.route_min_ordinary)
      assert(minimum >= 1, 'routes must contain ordinary nodes')
      local count = minimum + base.umod(base.mix(route, 806), math.tointeger(constants.route_extra_ordinary) + 1)
      local d = W.sub(b, a)
      local horizontal = math.sqrt(d.x * d.x + d.z * d.z)
      local right = {x = -d.z / horizontal, y = 0, z = d.x / horizontal}
      local sign = base.umod(route, 2) == 0 and -1 or 1
      local previous = a
      local points = {}
      local primary = count
      for i = 1, count do
        local t = i / (count + 1)
        local q = W.add(a, W.scale(d, t))
        q = W.add(q, W.scale(right, sign * constants.route_bend * math.sin(t * math.pi * 2)))
        q.y = q.y + sign * constants.route_rise * math.sin(t * math.pi)
        local sea = M.ocean_weight(seed, q.x, q.z)
        q.y = q.y * (1 - sea) + (W.height(q.x, q.z) - 8) * sea
        q.key = base.mix(route, i, 807)
        append(q, false)
        connect(previous, q, route)
        points[#points + 1] = q
        previous = q
        if i < count and i % 2 == 1 then
          local shaft =
              W.add(q, {x = 0, y = sign * (55 + base.umod(base.mix(route, i, 811), 45)) * (1 - 0.8 * sea), z = 0})
          shaft.key = base.mix(route, i, 812)
          append(shaft, false)
          connect(previous, shaft, route)
          previous = shaft
          primary = primary + 1
        end
      end
      connect(previous, b, route)
      -- An overhead or lower bypass between adjacent ordinary places adds a real
      -- loop without shortening the minimum number of ordinary places between bases.
      local p, q = points[1], points[2]
      local u = W.add(p, {x = 18, y = -sign * 75 * (1 - 0.75 * M.ocean_weight(seed, p.x, p.z)), z = -12})
      u.key = base.mix(route, 813)
      local v = W.add(q, {x = -12, y = -sign * 75 * (1 - 0.75 * M.ocean_weight(seed, q.x, q.z)), z = 18})
      v.key = base.mix(route, 814)
      append(u, false)
      append(v, false)
      connect(p, u, route)
      connect(u, v, route)
      connect(v, q, route)
      -- Optional dead-end adventures add degree-3 junctions without shortening
      -- any route between main nodes. Ordinary nodes of different routes do not merge.
      if base.umod(base.mix(route, 808), 3) ~= 0 then
        local parent = points[1 + base.umod(base.mix(route, 809), #points)]
        local q = W.add(parent, W.scale(right, sign * constants.route_branch_length))
        q.y = q.y + sign * 12
        q.key = base.mix(route, 810)
        append(q, false)
        connect(parent, q, route)
      end
      local routes = step.writes.network_routes
      assert(nr < routes:count(), 'network route capacity exceeded')
      routes:field('to'):set(nr, b.cx, 0)
      routes:field('to'):set(nr, b.cz, 1)
      routes:field('ordinary'):set(nr, primary)
      routes:field('mandatory'):set(nr, candidate.mandatory and 1 or 0)
      routes:field('key'):set(nr, route)
      nr = nr + 1
    end
  end
  local meta = step.writes.network_meta
  meta:field('nodes'):set(0, nn)
  meta:field('edges'):set(0, ne)
  meta:field('routes'):set(0, nr)
  meta:field('degree'):set(0, #neighbours)
end
