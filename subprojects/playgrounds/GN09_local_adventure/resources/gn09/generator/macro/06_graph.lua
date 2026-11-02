-- Граф окна: базы секторов по строкам, затем маршруты в порядке секторов (восточный, южный), если
-- оба конца в окне. Номера узлов — порядок добавления, ровно как у хоста.
--
-- Ранг базы — число её соседей ВНУТРИ окна: граф на краю окна обрезан, внутренность от окна не
-- зависит. Узел зала в угловом секторе (ранг 2) может лечь на хорду между соседями — тогда он
-- сдвигается поперёк хорды, оставаясь в зале.

local is_base = modules.world.is_base

return function(step)
  local counts = step.reads.area_counts
  local min_x, min_z = math.tointeger(counts:field("min_x"):get(0)), math.tointeger(counts:field("min_z"):get(0))
  local max_x, max_z = math.tointeger(counts:field("max_x"):get(0)), math.tointeger(counts:field("max_z"):get(0))
  local columns = max_x - min_x

  local nodes = step.writes.nodes
  local n_key, n_centre, n_kind, n_rank = nodes:field("key"), nodes:field("centre"), nodes:field("kind"), nodes:field("rank")
  local edges = step.writes.edges
  local e_a, e_b = edges:field("a"), edges:field("b")
  local list, links = {}, {}

  local function append(key, x, y, z, kind, rank)
    list[#list + 1] = { key = key, x = x, y = y, z = z, kind = kind, rank = rank }
    return #list - 1
  end
  local function base_id(x, z) return (z - min_z) * columns + (x - min_x) end

  local bases = step.links.bases
  for z = min_z, max_z - 1 do
    for x = min_x, max_x - 1 do
      local b = bases[base_id(x, z) + 1].base
      local rank = 0
      if x > min_x then rank = rank + 1 end
      if x + 1 < max_x then rank = rank + 1 end
      if z > min_z then rank = rank + 1 end
      if z + 1 < max_z then rank = rank + 1 end
      local centre = b:field("centre")
      append(b:field("key"):get(0), centre:get(0, 0), centre:get(0, 1), centre:get(0, 2),
             math.tointeger(b:field("kind"):get(0)), rank)
    end
  end

  -- Вход и цель — первая и последняя база окна: сектор зала базы не имеет.
  local entry, destination = 0, base_id(max_x - 1, max_z - 1)
  for i = 0, #list - 1 do
    if is_base(list[i + 1].kind) then
      if not is_base(list[entry + 1].kind) then entry = i end
      destination = i
    end
  end

  local routes = step.links.routes
  for z = min_z, max_z - 1 do
    for x = min_x, max_x - 1 do
      local r = routes[base_id(x, z) + 1]
      local count = math.tointeger(r.route_counts:field("nodes"):get(0))
      local edge_count = math.tointeger(r.route_counts:field("edges"):get(0))
      local rn, re = r.route_nodes, r.route_edges
      local r_key, r_centre, r_kind, r_rank, r_dir =
        rn:field("key"), rn:field("centre"), rn:field("kind"), rn:field("rank"), rn:field("direction")
      local r_a, r_b, r_edir = re:field("a"), re:field("b"), re:field("direction")
      for direction = 1, 2 do
        local inside = direction == 1 and x + 1 < max_x or direction == 2 and z + 1 < max_z
        if inside then
          local from = base_id(x, z)
          local to = base_id(x + (direction == 1 and 1 or 0), z + (direction == 2 and 1 or 0))
          local ids = {}
          for i = 0, count - 1 do
            if math.tointeger(r_dir:get(i)) == direction then
              ids[#ids + 1] = append(r_key:get(i), r_centre:get(i, 0), r_centre:get(i, 1), r_centre:get(i, 2),
                                     math.tointeger(r_kind:get(i)), math.tointeger(r_rank:get(i)))
            end
          end
          local function global(local_index)
            if local_index == -1 then return from end
            if local_index == -2 then return to end
            return ids[local_index + 1]
          end
          for e = 0, edge_count - 1 do
            if math.tointeger(r_edir:get(e)) == direction then
              links[#links + 1] = { global(math.tointeger(r_a:get(e))), global(math.tointeger(r_b:get(e))) }
            end
          end
        end
      end
    end
  end

  -- Узел зала в угловом секторе, лёгший на хорду соседей, сдвигается поперёк неё на 6 м.
  local sectors = columns * (max_z - min_z)
  for i = 0, sectors - 1 do
    local n = list[i + 1]
    if n.kind == 0 and n.rank == 2 then
      local around, found = {}, 0
      for _, link in ipairs(links) do
        if found < 2 and (link[1] == i or link[2] == i) then
          found = found + 1
          around[found] = link[1] == i and link[2] or link[1]
        end
      end
      if found == 2 then
        local a, b = list[around[1] + 1], list[around[2] + 1]
        local abx, aby, abz = b.x - a.x, b.y - a.y, b.z - a.z
        local apx, apy, apz = n.x - a.x, n.y - a.y, n.z - a.z
        local t = (apx * abx + apy * aby + apz * abz) / (abx * abx + aby * aby + abz * abz)
        if t < 0.0 then t = 0.0 elseif 1.0 < t then t = 1.0 end
        local qx, qy, qz = n.x - (a.x + abx * t), n.y - (a.y + aby * t), n.z - (a.z + abz * t)
        if math.sqrt(qx * qx + qy * qy + qz * qz) < 4.0 then
          local run = base.hypot(abx, abz)
          local cx, cz = 1.0, 0.0
          if run > 1e-6 then cx, cz = -abz / run, abx / run end
          n.x = n.x + cx * 6.0
          n.y = n.y + 0.0 * 6.0
          n.z = n.z + cz * 6.0
        end
      end
    end
  end

  if #list > nodes:count() then error("graph: node capacity " .. nodes:count() .. " exceeded") end
  if #links > edges:count() then error("graph: edge capacity " .. edges:count() .. " exceeded") end
  for i, n in ipairs(list) do
    n_key:set(i - 1, n.key)
    n_centre:set(i - 1, n.x, 0); n_centre:set(i - 1, n.y, 1); n_centre:set(i - 1, n.z, 2)
    n_kind:set(i - 1, n.kind)
    n_rank:set(i - 1, n.rank)
  end
  for i, link in ipairs(links) do
    e_a:set(i - 1, link[1])
    e_b:set(i - 1, link[2])
  end
  local meta = step.writes.graph_meta
  meta:field("nodes"):set(0, #list)
  meta:field("edges"):set(0, #links)
  meta:field("entry"):set(0, entry)
  meta:field("destination"):set(0, destination)
  meta:field("min_x"):set(0, min_x)
  meta:field("min_z"):set(0, min_z)
  meta:field("max_x"):set(0, max_x)
  meta:field("max_z"):set(0, max_z)
end
