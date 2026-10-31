-- Надмножество проверок существа. Домен растёт от узлов в своих залах (до трёх) через небазовые узлы, глубина
-- сбрасывается в зале и не больше четырёх; проход проверяется для каждого такого ребра. Если считать
-- залом ЛЮБОЙ зал окрестности и не спрашивать поле, глубины не больше, а достижимых рёбер не меньше —
-- значит, все рёбра, которые проверит creature, здесь есть. Пещера места дотягивается до пробы ребра,
-- только если центр места в коробке ребра, раздутой на пробу тела, метр запаса и досягаемость пещеры
-- (site_reach) — то же правило, по которому поле отбирает пещеры.

local W = modules.world

return function(step)
  local own_key = step.key[3]
  local near, body = {}, nil
  for _, region in ipairs(step.links.regions) do
    local halls = region.halls
    local count = math.tointeger(region.hall_counts:field("halls"):get(0))
    local key, centre, forward, half, floor = halls:field("key"), halls:field("centre"), halls:field("forward"),
                                              halls:field("half"), halls:field("floor")
    for i = 0, count - 1 do
      near[#near + 1] = { cx = centre:get(i, 0), cy = centre:get(i, 1), cz = centre:get(i, 2),
                          fx = forward:get(i, 0), fy = forward:get(i, 1), fz = forward:get(i, 2),
                          hl = half:get(i, 0), hw = half:get(i, 1), hh = half:get(i, 2), floor = floor:get(i) }
      if key:get(i) == own_key then body = halls:field("body_radius"):get(i) end
    end
  end
  if body == nil then error("creature_scope: its hall is not among the halls near the window") end
  local reach = body + math.sqrt(3.0) + 1.0 + step.params.site_reach

  local nodes = step.reads.nodes
  local n_key, n_centre, n_kind = nodes:field("key"), nodes:field("centre"), nodes:field("kind")
  local node_count = math.tointeger(step.reads.graph_meta:field("nodes"):get(0))
  local edge_count = math.tointeger(step.reads.graph_meta:field("edges"):get(0))
  local e_a, e_b = step.reads.edges:field("a"), step.reads.edges:field("b")
  local route, around, depth = {}, {}, {}
  for i = 0, node_count - 1 do
    route[i] = { key = n_key:get(i), x = n_centre:get(i, 0), y = n_centre:get(i, 1), z = n_centre:get(i, 2),
                 kind = math.tointeger(n_kind:get(i)) }
    around[i] = {}
    depth[i] = -1
  end
  for i = 0, edge_count - 1 do
    local a, b = math.tointeger(e_a:get(i)), math.tointeger(e_b:get(i))
    around[a][#around[a] + 1] = b
    around[b][#around[b] + 1] = a
  end
  local function in_any_hall(n)
    for _, h in ipairs(near) do if W.inside_hall(h, n.x, n.y, n.z) then return true end end
    return false
  end
  local queue, head = {}, 1
  for i = 0, node_count - 1 do
    if not W.is_base(route[i].kind) and in_any_hall(route[i]) then depth[i] = 0; queue[#queue + 1] = i end
  end
  local edges = {}
  while head <= #queue do
    local current = queue[head]
    head = head + 1
    for _, nxt in ipairs(around[current]) do
      if not W.is_base(route[nxt].kind) then
        local next_depth = in_any_hall(route[nxt]) and 0 or depth[current] + 1
        if next_depth <= 4 then
          edges[#edges + 1] = { route[current], route[nxt] }
          if depth[nxt] < 0 or next_depth < depth[nxt] then
            depth[nxt] = next_depth
            queue[#queue + 1] = nxt
          end
        end
      end
    end
  end

  local places = step.writes.places
  local p_sector, p_node = places:field("sector"), places:field("node")
  local made = 0
  for i = 0, node_count - 1 do
    local n = route[i]
    if W.natural(n.kind) then
      local touched = false
      for _, e in ipairs(edges) do
        local a, b = e[1], e[2]
        if n.x >= math.min(a.x, b.x) - reach and n.x <= math.max(a.x, b.x) + reach and
           n.y >= math.min(a.y, b.y) - reach and n.y <= math.max(a.y, b.y) + reach and
           n.z >= math.min(a.z, b.z) - reach and n.z <= math.max(a.z, b.z) + reach then
          touched = true
          break
        end
      end
      if touched then
        if made >= places:count() then error("creature_scope: place capacity " .. places:count() .. " exceeded") end
        p_sector:set(made, math.floor(n.x / W.sector_size), 0)
        p_sector:set(made, math.floor(n.z / W.sector_size), 1)
        p_node:set(made, n.key)
        made = made + 1
      end
    end
  end
  step.writes.place_counts:field("places"):set(0, made)
end
