-- Домен существа зала и его убежища. Перенос C++ (hall.cpp, compute_creature) бит в бит: тот же обход,
-- тот же порядок узлов, рёбер и залов.
--
-- Домен — залы (свой и ярусы, с которыми он сообщается, не больше трёх) и узлы графа, до которых тело
-- доплывает проходами не дальше четырёх рёбер от залов. Рёбра к базам в домен не входят: стены станции не
-- входят в поле. «Тело проходит» — поле `passage` (галереи, повороты, пещеры мест, залы окна) отрицательно
-- в центре и на 14 направлениях на радиусе тела плюс диагональ клетки, через каждые 2 м отрезка.
--
-- Убежища — внешние концы входов в залы домена, сами вне домена, и только те, что лежат в ОДНОЙ
-- компоненте графа вне домена с наибольшим их числом: лист тупиковой ветки у зала обходом не служит.

local W = modules.world

return function(step)
  local home = step.reads.creature_home
  local own_key = step.key[3]

  -- Залы окрестности в порядке регионов окна: так их перебирал хост (halls_near).
  local near = {}
  for _, region in ipairs(step.links.regions) do
    local halls = region.halls
    local count = math.tointeger(region.hall_counts:field("halls"):get(0))
    local key, centre, forward, half, floor = halls:field("key"), halls:field("centre"), halls:field("forward"),
                                              halls:field("half"), halls:field("floor")
    local resident, body = halls:field("resident"), halls:field("body_radius")
    for i = 0, count - 1 do
      near[#near + 1] = {
        key = key:get(i), cx = centre:get(i, 0), cy = centre:get(i, 1), cz = centre:get(i, 2),
        fx = forward:get(i, 0), fy = forward:get(i, 1), fz = forward:get(i, 2),
        hl = half:get(i, 0), hw = half:get(i, 1), hh = half:get(i, 2), floor = floor:get(i),
        resident = math.tointeger(resident:get(i)), body_radius = body:get(i),
      }
    end
  end
  local h = nil
  for _, other in ipairs(near) do if other.key == own_key then h = other break end end
  if h == nil then error("creature: its hall is not among the halls near the window") end

  local meta = step.writes.creature_meta
  meta:field("hall"):set(0, own_key)
  meta:field("resident"):set(0, h.resident)
  meta:field("body_radius"):set(0, h.body_radius)
  local counts = step.writes.creature_counts

  -- Граф окна.
  local nodes = step.reads.nodes
  local n_key, n_centre, n_kind = nodes:field("key"), nodes:field("centre"), nodes:field("kind")
  local node_count = math.tointeger(step.reads.graph_meta:field("nodes"):get(0))
  local edge_count = math.tointeger(step.reads.graph_meta:field("edges"):get(0))
  local e_a, e_b = step.reads.edges:field("a"), step.reads.edges:field("b")
  local route = {}
  for i = 0, node_count - 1 do
    route[i] = { key = n_key:get(i), x = n_centre:get(i, 0), y = n_centre:get(i, 1), z = n_centre:get(i, 2),
                 kind = math.tointeger(n_kind:get(i)) }
  end
  local edges = {}
  for i = 0, edge_count - 1 do edges[i + 1] = { a = math.tointeger(e_a:get(i)), b = math.tointeger(e_b:get(i)) } end

  local function inside(hall, n) return W.inside_hall(hall, n.x, n.y, n.z) end
  local function in_halls(list, n)
    for _, hall in ipairs(list) do if inside(hall, n) then return true end end
    return false
  end
  -- Два зала сообщаются, если на отрезке между центрами есть точка внутри обоих.
  local function halls_touch(a, b)
    for i = 0, 64 do
      local t = i / 64.0
      local p = { x = a.cx + (b.cx - a.cx) * t, y = a.cy + (b.cy - a.cy) * t, z = a.cz + (b.cz - a.cz) * t }
      if inside(a, p) and inside(b, p) then return true end
    end
    return false
  end

  -- Тело проходит отрезок: поле отрицательно во всех пробах.
  local probes = step.writes.probes
  local p_pos, p_val = probes:field("position"), probes:field("value")
  local diagonal = 1.0 / math.sqrt(3.0)
  local directions = {
    { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 },
    { diagonal, diagonal, diagonal }, { -diagonal, diagonal, diagonal }, { diagonal, -diagonal, diagonal },
    { diagonal, diagonal, -diagonal }, { -diagonal, -diagonal, diagonal }, { -diagonal, diagonal, -diagonal },
    { diagonal, -diagonal, -diagonal }, { -diagonal, -diagonal, -diagonal },
  }
  local function body_fits(a, b, body)
    local probe = body + math.sqrt(3.0)
    local dx, dy, dz = b.x - a.x, b.y - a.y, b.z - a.z
    local span = math.sqrt(dx * dx + dy * dy + dz * dz)
    local steps = math.max(1, math.ceil(span / 2.0))
    local n = 0
    for s = 0, steps do
      local t = s / steps
      local cx, cy, cz = a.x + dx * t, a.y + dy * t, a.z + dz * t
      if n + 15 > probes:count() then error("creature: probe capacity " .. probes:count() .. " exceeded") end
      p_pos:set(n, cx, 0); p_pos:set(n, cy, 1); p_pos:set(n, cz, 2)
      n = n + 1
      for _, d in ipairs(directions) do
        p_pos:set(n, cx + d[1] * probe, 0); p_pos:set(n, cy + d[2] * probe, 1); p_pos:set(n, cz + d[3] * probe, 2)
        n = n + 1
      end
    end
    originator.field_sample{ inputs = { p_pos }, outputs = { p_val }, params = { field = "passage", count = n, half = 24 } }
    for i = 0, n - 1 do
      if p_val:get(i) >= 0.0 then return false end
    end
    return true
  end

  -- Залы домена: свой и ярусы, с которыми он сообщается; дальше — те, куда выведут проходы.
  local halls = { h }
  for _, other in ipairs(near) do
    if other.key ~= own_key and halls_touch(h, other) and #halls < 3 then halls[#halls + 1] = other end
  end
  local depth={}
  for i=0,node_count-1 do depth[i]=-1 end
  local around=originator.algorithms.adjacency(node_count,#edges,function(i)
    local e=edges[i+1];return e.a,e.b
  end)
  local function seed_halls(seed)
    for i=0,node_count-1 do
      if depth[i]<0 and not W.is_base(route[i].kind) and in_halls(halls,route[i]) then seed(i,0) end
    end
  end
  local passages={}
  originator.algorithms.propagate{
    adjacency=around,state=depth,max_visits=node_count*4,
    initial=seed_halls,
    transition=function(current,nxt,current_depth,old_depth,seed)
      if not W.is_base(route[nxt].kind) then
        local next_in_hall = in_halls(halls, route[nxt])
        local next_depth = next_in_hall and 0 or current_depth + 1
        if next_depth <= 4 and body_fits(route[current], route[nxt], h.body_radius) then
          passages[#passages + 1] = { math.min(current, nxt), math.max(current, nxt) }
          if old_depth < 0 then
            if not next_in_hall and #halls < 3 then
              for _, other in ipairs(near) do
                local listed = false
                for _, x in ipairs(halls) do if x == other then listed = true break end end
                if not listed and inside(other, route[nxt]) then
                  halls[#halls + 1] = other
                  seed_halls(seed)
                  break
                end
              end
            end
            return in_halls(halls, route[nxt]) and 0 or next_depth
          end
        end
      end
    end,
  }
  table.sort(passages, function(x, y) return x[1] < y[1] or (x[1] == y[1] and x[2] < y[2]) end)

  local function write_keys(buffer, keys, what)
    local field = buffer:field("key")
    if #keys > buffer:count() then error("creature: " .. what .. " capacity " .. buffer:count() .. " exceeded") end
    for i, k in ipairs(keys) do field:set(i - 1, k) end
  end
  local hall_keys = {}
  for _, x in ipairs(halls) do hall_keys[#hall_keys + 1] = x.key end
  write_keys(step.writes.creature_halls, hall_keys, "hall")
  counts:field("halls"):set(0, #hall_keys)
  local node_keys = {}
  for i = 0, node_count - 1 do if depth[i] >= 0 then node_keys[#node_keys + 1] = route[i].key end end
  write_keys(step.writes.creature_nodes, node_keys, "node")
  counts:field("nodes"):set(0, #node_keys)
  local pa, pb = step.writes.creature_passages:field("a"), step.writes.creature_passages:field("b")
  local written, last = 0, nil
  for _, p in ipairs(passages) do
    if not (last and last[1] == p[1] and last[2] == p[2]) and depth[p[1]] >= 0 and depth[p[2]] >= 0 then
      if written >= step.writes.creature_passages:count() then error("creature: passage capacity exceeded") end
      pa:set(written, route[p[1]].key); pb:set(written, route[p[2]].key)
      written = written + 1
    end
    last = p
  end
  counts:field("passages"):set(0, written)

  -- Убежища.
  local refuges = {}
  for _, e in ipairs(edges) do
    local in_a, in_b = in_halls(halls, route[e.a]), in_halls(halls, route[e.b])
    if in_a ~= in_b then
      local outside = in_a and e.b or e.a
      if depth[outside] < 0 then refuges[#refuges + 1] = route[outside].key end
    end
  end
  local component, components = {}, 0
  for start = 0, node_count - 1 do
    if component[start] == nil and depth[start] < 0 then
      local stack = { start }
      component[start] = components
      while #stack > 0 do
        local current = stack[#stack]
        stack[#stack] = nil
        for _, nxt in ipairs(around[current]) do
          if component[nxt] == nil and depth[nxt] < 0 then
            component[nxt] = components
            stack[#stack + 1] = nxt
          end
        end
      end
      components = components + 1
    end
  end
  local outside = {}
  for _, key in ipairs(refuges) do
    for i = 0, node_count - 1 do if route[i].key == key then outside[#outside + 1] = i end end
  end
  local votes = {}
  for c = 0, components - 1 do votes[c] = 0 end
  for _, id in ipairs(outside) do votes[component[id]] = votes[component[id]] + 1 end
  local best = 0
  for c = 1, components - 1 do if votes[c] > votes[best] then best = c end end
  local kept = {}
  for _, id in ipairs(outside) do
    if component[id] == best then kept[#kept + 1] = route[id].key end
  end
  table.sort(kept, function(x, y) return math.ult(x, y) end)
  local unique = {}
  for _, k in ipairs(kept) do if unique[#unique] ~= k then unique[#unique + 1] = k end end
  write_keys(step.writes.creature_refuges, unique, "refuge")
  counts:field("refuges"):set(0, #unique)
end
