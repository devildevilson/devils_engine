-- Outpost building grammar: from the wet dock a tower or a trunk leads to the core.
-- Rooms are boxes in the outpost frame; doors are the edges of a spanning tree rooted at the lock.
local W, F, O = modules.world, modules.frame, modules.outpost
local names = {'x', 'y', 'z'}

local function random(step, salt)
  return base.prng64_normalize(base.mix(step.world_seed, step.key[1], step.key[2], salt))
end

-- Box spanning [t0,t1] along the transition axis from the hatch, with a lateral offset.
local function along_box(a, axis, sign, t0, t1, half, offset)
  local c, h = {x = a.x, y = a.y, z = a.z}, {x = half.x or 0, y = half.y or 0, z = half.z or 0}
  for name, value in pairs(offset or {}) do c[name] = c[name] + value end
  c[axis] = a[axis] + sign * (t0 + t1) / 2
  h[axis] = (t1 - t0) / 2
  return c, h
end

-- Tower block: cells on a grid whose lateral placement puts one cell column on the hatch axis.
local function tower_block(pose, a, shape, place, t0)
  local axis, sign = O.along(pose)
  local u, v = O.lateral(pose)
  local size = {x = O.cell(), y = O.deck(), z = O.cell()}
  local n = pose == O.side and {x = shape[1], y = shape[2], z = shape[3]} or {x = shape[1], z = shape[2], y = shape[3]}
  local low = {}
  low[u] = a[u] - (place[1] + 0.5) * size[u]
  low[v] = a[v] - (place[2] + 0.5) * size[v]
  local depth = n[axis] * size[axis]
  low[axis] = sign > 0 and a[axis] + t0 or a[axis] - t0 - depth
  local high = {x = low.x + n.x * size.x, y = low.y + n.y * size.y, z = low.z + n.z * size.z}
  local dock = {}
  dock[u], dock[v], dock[axis] = place[1], place[2], sign > 0 and 0 or n[axis] - 1
  local exit = {x = dock.x, y = dock.y, z = dock.z}
  exit[axis] = sign > 0 and n[axis] - 1 or 0
  return {n = n, size = size, low = low, high = high, depth = depth, t0 = t0, dock = dock, exit = exit, place = place}
end

local function tower_candidates(pose, a, length, standoff)
  local axis, sign = O.along(pose)
  local out = {}
  local t_rock = standoff - 1
  for s, shape in ipairs(O.tower_shapes(pose)) do
    -- Lateral placements: every cell column on the hatch axis; a side lock enters one of the two lowest decks.
    local places = {}
    local nu, nv = shape[1], shape[2]
    for i = 0, nu - 1 do
      if pose == O.side then
        for k = 0, math.min(1, nv - 1) do places[#places + 1] = {i, k} end
      else
        for j = 0, nv - 1 do places[#places + 1] = {i, j} end
      end
    end
    local depth = shape[3] * (pose == O.side and O.cell() or O.deck())
    -- The far face enters the rock: barely, or up to half a cell when the water room is short.
    for _, embed in ipairs({1.5, 3, 6}) do
      local t0 = t_rock + embed - depth
      if t0 >= 2 and t0 <= 8 and length - (t0 + depth) >= 2 then
        for _, place in ipairs(places) do
          out[#out + 1] = {shape = s, block = tower_block(pose, a, shape, place, t0)}
        end
      end
    end
  end
  return out
end

-- Probes of a tower block: a 3x3x3 lattice inside and a 3x3 patch just beyond the far face.
local function tower_probes(pose, block)
  local axis, sign = O.along(pose)
  local u, v = O.lateral(pose)
  local c, h = {}, {}
  for _, name in ipairs(names) do
    c[name] = (block.low[name] + block.high[name]) / 2
    h[name] = (block.high[name] - block.low[name]) / 2
  end
  local lattice, contact = {}, {}
  for k = -1, 1 do
    for j = -1, 1 do
      for i = -1, 1 do
        lattice[#lattice + 1] = {x = c.x + i * (h.x - 1), y = c.y + j * (h.y - 1), z = c.z + k * (h.z - 1)}
      end
    end
  end
  local far = sign > 0 and block.high[axis] + 0.75 or block.low[axis] - 0.75
  for j = -1, 1 do
    for i = -1, 1 do
      local p = {}
      p[u] = c[u] + i * (h[u] - 1)
      p[v] = c[v] + j * (h[v] - 1)
      p[axis] = far
      contact[#contact + 1] = p
    end
  end
  return lattice, contact
end

return function(step)
  local source = step.reads.outpost_frame
  local frame = F.new(W.get(source:field('origin'), 0), source:field('yaw_c'):get(0), source:field('yaw_s'):get(0))
  local pose = source:field('pose'):get(0)
  local dock, hatch, front = W.get(source:field('dock'), 0), W.get(source:field('hatch'), 0),
      W.get(source:field('front'), 0)
  local omega = step.writes.outpost_meta
  local floors = O.floors(step.world_seed, step.key[1], step.key[2])
  local a, b = frame.to_local(hatch), frame.to_local(front)
  local axis, sign = O.along(pose)
  local u, v = O.lateral(pose)
  local length = math.abs(b[axis] - a[axis])
  local tube = O.tube_half(pose)

  local rooms, edges = {}, {}
  local function room(c, h, label, open)
    rooms[#rooms + 1] = {c = c, h = h, label = label, open = open}
    return #rooms
  end
  local function link(i, j) edges[#edges + 1] = {i, j} end
  local function tube_room(t0, t1, label, open)
    local c, h = along_box(a, axis, sign, t0, t1, tube)
    return room(c, h, label, open)
  end
  -- The lock opens towards the boat: its near end face stays without a wall.
  local open_end = {axis = axis, sign = -sign}

  -- Tower: probe every candidate block, keep those touching rock and standing partly in water.
  local candidates = tower_candidates(pose, a, length, omega:field('standoff'):get(0))
  local probes = step.writes.outpost_probes
  local np = 0
  for _, candidate in ipairs(candidates) do
    local lattice, contact = tower_probes(pose, candidate.block)
    candidate.first = np
    for _, p in ipairs(lattice) do
      assert(np < probes:count(), 'outpost probe capacity exceeded by tower candidates')
      W.put(probes:field('position'), np, frame.point(p))
      np = np + 1
    end
    for _, p in ipairs(contact) do
      assert(np < probes:count(), 'outpost probe capacity exceeded by tower candidates')
      W.put(probes:field('position'), np, frame.point(p))
      np = np + 1
    end
  end
  if np > 0 then
    originator.field_sample{inputs = {probes:field('position')}, outputs = {probes:field('value')},
      params = {field = 'natural', count = np, half = 24}}
    originator.field_sample{inputs = {probes:field('position')}, outputs = {probes:field('clearance')},
      params = {field = 'clearance', count = np, half = 24}}
  end
  local best, best_score, valid = nil, -math.huge, 0
  local refused = {0, 0, 0}
  for c, candidate in ipairs(candidates) do
    local water, touch, protected = 0, 0, false
    for j = candidate.first, candidate.first + 26 do
      if probes:field('value'):get(j) < 0 then water = water + 1 end
      protected = protected or probes:field('clearance'):get(j) < 2.9
    end
    for j = candidate.first + 27, candidate.first + 35 do
      if probes:field('value'):get(j) >= 0 then touch = touch + 1 end
    end
    if water < 9 then
      refused[1] = refused[1] + 1
    elseif touch < 3 then
      refused[2] = refused[2] + 1
    elseif protected then
      refused[3] = refused[3] + 1
    end
    if water >= 9 and touch >= 3 and not protected then
      valid = valid + 1
      local score = -math.abs(water / 27 - 0.55) * 10 + touch * 0.5 + random(step, 950 + c) * 4
      if score > best_score then best, best_score = candidate, score end
    end
  end
  omega:field('tower_candidates'):set(0, #candidates)
  omega:field('tower_valid'):set(0, valid)
  omega:field('tower_buried'):set(0, refused[1])
  omega:field('tower_floating'):set(0, refused[2])
  omega:field('tower_protected'):set(0, refused[3])
  local kind = (best and random(step, 940) < constants.outpost_tower_chance) and O.tower or O.trunk

  local lock, tail
  if kind == O.tower then
    local block = best.block
    lock = tube_room(0, block.t0, O.lock, open_end)
    local n, size, low = block.n, block.size, block.low
    local index = {}
    local function key(p) return p.x .. ',' .. p.y .. ',' .. p.z end
    for iy = 0, n.y - 1 do
      for iz = 0, n.z - 1 do
        for ix = 0, n.x - 1 do
          local c = {x = low.x + (ix + 0.5) * size.x, y = low.y + (iy + 0.5) * size.y, z = low.z + (iz + 0.5) * size.z}
          index[key({x = ix, y = iy, z = iz})] = room(c, {x = size.x / 2, y = size.y / 2, z = size.z / 2}, O.technical)
        end
      end
    end
    -- Decks join through one column: the hatch axis for vertical towers, a side column otherwise.
    local stair = {x = block.dock.x, z = block.dock.z}
    if pose == O.side then
      local columns = {}
      for ix = 0, n.x - 1 do
        if ix ~= block.dock.x then
          for iz = 0, n.z - 1 do columns[#columns + 1] = {x = ix, z = iz} end
        end
      end
      stair = columns[1 + math.floor(random(step, 941) * #columns)]
    end
    local start = index[key(block.dock)]
    rooms[start].label = O.dock
    link(lock, start)
    local seen, queue, cell_of = {[start] = true}, {block.dock}, {}
    local head, salt = 1, 0
    while head <= #queue do
      local p = queue[head]
      head = head + 1
      local here = index[key(p)]
      local around =
          {{x = p.x + 1, y = p.y, z = p.z}, {x = p.x - 1, y = p.y, z = p.z}, {x = p.x, y = p.y, z = p.z + 1},
            {x = p.x, y = p.y, z = p.z - 1}}
      if p.x == stair.x and p.z == stair.z then
        around[#around + 1] = {x = p.x, y = p.y + 1, z = p.z}
        around[#around + 1] = {x = p.x, y = p.y - 1, z = p.z}
      end
      -- Seeded neighbour order varies which partitions get doors.
      for i = #around, 2, -1 do
        salt = salt + 1
        local j = 1 + math.floor(random(step, 960 + salt) * i)
        around[i], around[j] = around[j], around[i]
      end
      for _, q in ipairs(around) do
        local there = index[key(q)]
        if there and not seen[there] then
          seen[there] = true
          link(here, there)
          if here == start and q.y == p.y and rooms[start].label == O.dock and not cell_of.guard then
            rooms[there].label = O.guard
            cell_of.guard = there
          end
          queue[#queue + 1] = q
        end
      end
    end
    local exit = index[key(block.exit)]
    tail = tube_room(block.t0 + block.depth, length, O.passage)
    link(exit, tail)
  else
    -- Trunk: a thin lock that widens near the core: guard post, then a technical section.
    local gap, lock_min = 2, 4
    local sequence = {}
    for _, label in ipairs({O.guard, O.technical}) do
      local _, len = O.trunk_room(pose, label)
      sequence[#sequence + 1] = {label = label, length = len}
    end
    local function need(k)
      local s = lock_min
      for i = 1, k do s = s + sequence[i].length + gap end
      return s
    end
    local count = length >= need(2) and 2 or (length >= need(1) and 1 or 0)
    local placed, t = {}, length
    for i = count, 1, -1 do
      local t1 = t - gap
      placed[i] = {label = sequence[i].label, t0 = t1 - sequence[i].length, t1 = t1}
      t = placed[i].t0
    end
    lock = tube_room(0, count > 0 and placed[1].t0 or length, O.lock, open_end)
    tail = lock
    for i = 1, count do
      local half = O.trunk_room(pose, placed[i].label)
      local offset = {}
      for k, name in ipairs({u, v}) do
        -- A side trunk keeps its rooms on the lock deck; lateral shifts keep the tube inside the face.
        if not (pose == O.side and name == 'y') then
          local room_half = half[name]
          local limit = room_half - tube[name] - 0.5
          offset[name] = (math.floor(random(step, 970 + i * 4 + k) * 5) - 2) / 2 * limit
        end
      end
      local c, h = along_box(a, axis, sign, placed[i].t0, placed[i].t1, half, offset)
      local here = room(c, h, placed[i].label)
      link(tail, here)
      local after = i < count and placed[i + 1].t0 or length
      tail = tube_room(placed[i].t1, after, O.passage)
      link(here, tail)
    end
  end

  -- Core: a seeded plan of candidate rooms; every option is probed in one batch, then candidates are
  -- accepted in plan order when their parent is in, they hold enough rock, stay off the protected
  -- route and touch, but do not overlap, the rooms and the docked boat.
  local plan = modules.core.plan(pose, b, function(salt) return random(step, salt) end)
  np = 0
  for _, candidate in ipairs(plan) do
    for _, option in ipairs(candidate.options) do
      option.first = np
      for k = -1, 1 do
        for j = -1, 1 do
          for i = -1, 1 do
            assert(np < probes:count(), 'outpost probe capacity exceeded by core candidates')
            local p =
                {x = option.c.x + i * (option.h.x - 0.5), y = option.c.y + j * (option.h.y - 0.5),
                  z = option.c.z + k * (option.h.z - 0.5)}
            W.put(probes:field('position'), np, frame.point(p))
            np = np + 1
          end
        end
      end
    end
  end
  originator.field_sample{inputs = {probes:field('position')}, outputs = {probes:field('value')},
    params = {field = 'natural', count = np, half = 24}}
  originator.field_sample{inputs = {probes:field('position')}, outputs = {probes:field('clearance')},
    params = {field = 'clearance', count = np, half = 24}}
  local boat =
      {c = frame.to_local(dock),
        h = {x = constants.sub_length / 2 + 1, y = constants.sub_beam / 2 + 1, z = constants.sub_beam / 2 + 1}}
  local function overlaps(c, h, q)
    for _, name in ipairs(names) do
      if math.abs(c[name] - q.c[name]) >= h[name] + q.h[name] - 0.05 then return false end
    end
    return true
  end
  local placed, refused = {tail = tail}, {0, 0, 0}
  local entry
  for _, candidate in ipairs(plan) do
    local parent = placed[candidate.parent]
    if parent then
      for _, option in ipairs(candidate.options) do
        local rock, protected = 0, false
        for j = option.first, option.first + 26 do
          if probes:field('value'):get(j) >= 0 then rock = rock + 1 end
          protected = protected or probes:field('clearance'):get(j) < 2.9
        end
        local blocked = overlaps(option.c, option.h, boat)
        for _, q in ipairs(rooms) do blocked = blocked or overlaps(option.c, option.h, q) end
        if candidate.required then
          if blocked then
            error(
              string.format('outpost %d,%d core entry %s overlaps the transition or the boat', step.key[1],
                step.key[2], candidate.id))
          end
          rock, protected = 27, false
        end
        if rock < candidate.min_rock then
          refused[1] = refused[1] + 1
        elseif protected then
          refused[2] = refused[2] + 1
        elseif blocked then
          refused[3] = refused[3] + 1
        else
          local index = room(option.c, option.h, candidate.label)
          link(parent, index)
          placed[candidate.id] = index
          entry = entry or index
          break
        end
      end
    end
  end
  omega:field('core_rooms'):set(0, #rooms - entry + 1)
  omega:field('core_water'):set(0, refused[1])
  omega:field('core_protected'):set(0, refused[2])
  omega:field('core_overlap'):set(0, refused[3])

  -- Sanity: rooms may touch but never overlap; the tree reaches every room from the lock.
  for i = 1, #rooms do
    for j = i + 1, #rooms do
      local overlap = true
      for _, name in ipairs(names) do
        overlap = overlap and math.abs(rooms[i].c[name] - rooms[j].c[name]) < rooms[i].h[name] + rooms[j].h[name] - 0.05
      end
      if overlap then
        error(
          string.format('outpost %d,%d rooms %d (label %d) and %d (label %d) overlap', step.key[1], step.key[2], i - 1,
            rooms[i].label, j - 1, rooms[j].label))
      end
    end
  end
  local around = {}
  for _, e in ipairs(edges) do
    around[e[1]] = around[e[1]] or {}
    around[e[2]] = around[e[2]] or {}
    table.insert(around[e[1]], e[2])
    table.insert(around[e[2]], e[1])
  end
  local parent = {[lock] = -1}
  local queue, head = {lock}, 1
  while head <= #queue do
    local i = queue[head]
    head = head + 1
    for _, j in ipairs(around[i] or {}) do
      if parent[j] == nil then
        parent[j] = i - 1
        queue[#queue + 1] = j
      end
    end
  end
  assert(lock == 1, 'outpost lock must be the first room')
  for i = 1, #rooms do
    if parent[i] == nil then
      error(
        string.format('outpost %d,%d room %d (label %d) is not linked to the lock', step.key[1], step.key[2], i - 1,
          rooms[i].label))
    end
  end

  local out, cuts = step.writes.station_rooms, step.writes.station_cut
  assert(#rooms <= out:count() and #rooms <= cuts:count(), 'outpost room capacity exceeded')
  for i, q in ipairs(rooms) do
    W.put(out:field('centre'), i - 1, frame.point(q.c))
    W.put(out:field('local_centre'), i - 1, q.c)
    W.put(out:field('half'), i - 1, q.h)
    out:field('label'):set(i - 1, q.label)
    out:field('parent'):set(i - 1, parent[i])
    out:field('open_axis'):set(i - 1, q.open and (q.open.axis == 'x' and 1 or (q.open.axis == 'y' and 2 or 3)) or 0)
    out:field('open_sign'):set(i - 1, q.open and q.open.sign or 0)
    F.box(frame, cuts, i - 1, q.c, {x = q.h.x + 0.1, y = q.h.y + 0.1, z = q.h.z + 0.1}, 6)
  end
  local meta = step.writes.station_meta
  W.put(meta:field('origin'), 0, frame.origin)
  meta:field('yaw_c'):set(0, frame.c)
  meta:field('yaw_s'):set(0, frame.s)
  W.put(meta:field('dock'), 0, dock)
  W.put(meta:field('hatch'), 0, hatch)
  W.put(meta:field('entry'), 0, front)
  meta:field('pose'):set(0, pose)
  meta:field('transition'):set(0, kind)
  meta:field('built'):set(0, 1)
  meta:field('rooms'):set(0, #rooms)
  meta:field('cuts'):set(0, #rooms)
  meta:field('floors'):set(0, floors)
  meta:field('entry_room'):set(0, entry - 1)
  meta:field('trunk_length'):set(0, 2 * rooms[lock].h[axis])
end
