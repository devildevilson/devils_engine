-- Станция базы графа или аванпоста зала. Перенос C++ (station.cpp: for_each_station_site,
-- keep_out_for, layout_station) бит в бит: те же хеши, порядок перебора и операций с double.
--
-- 1. МЕСТО — лучами по полю пещеры до монтажа (поле `natural_sites`: галереи, повороты, пещеры мест,
--    залы канонической окрестности): стена (16 поворотов, 8 лучей вбок) или дно/свод (9 лучей вниз или
--    вверх под узлом и в 8 точках вокруг), плоскость по наименьшим квадратам, три прохода допусков —
--    предпочтённая рецептом поза проходит все три раньше остальных. Позади — объём породы, путь лодки
--    к причалу — в воде.
-- 2. МАСКА — классы клеток плана 0.5 м. Считает блок station_room_cells (запрет — точное расстояние до
--    маршрутов, поворотов, пути лодки, коридоров залов; порода и опора — пробы поля); ленивое состояние
--    маски живёт в буферах mask_*, сброс — на каждое новое место.
-- 3. ПЛАН — рост графа помещений по программе рецепта: шлюз, вход, коридор, внутренние, наружные; петли
--    дверей между соседями через одну стену.
-- 4. ПОСТРОЙКА — блок station_build: воксели, сетка, коробка выреза.

local W = modules.world

local pi = math.pi
local degrees = 180.0 / pi
local max_floor_tilt = { 15.0, 25.0, 32.0 }
local max_wall_tilt = { 25.0, 35.0, 45.0 }
local max_residual = { 1.5, 2.5, 3.5 }
local inner_depth = { 14.0, 14.0, 8.0 }
local ray_reach = 70.0
local berth_gap = 0.4
local door_centre = 0.95
local hull_radius = 6.0 / 2.0
local station_cell = 0.5
local interior_layers = 6
local surface_guard = 2.0
local sub_turn_radius = 17.0 / 2.0 + surface_guard
local sub_axis_half = (17.0 - 6.0) / 2.0
local sub_path_radius = 6.0 / 2.0 + surface_guard
local keep_out = sub_turn_radius + 0.4
local mask_i0, mask_k0, mask_width, mask_depth = -40, -48, 112, 96

local function add(a, b) return { x = a.x + b.x, y = a.y + b.y, z = a.z + b.z } end
local function sub(a, b) return { x = a.x - b.x, y = a.y - b.y, z = a.z - b.z } end
local function scale(a, s) return { x = a.x * s, y = a.y * s, z = a.z * s } end
local function dot(a, b) return a.x * b.x + a.y * b.y + a.z * b.z end
local function length(a) return math.sqrt(dot(a, a)) end
local function cross(a, b) return { x = a.y * b.z - a.z * b.y, y = a.z * b.x - a.x * b.z, z = a.x * b.y - a.y * b.x } end
local function axis_u(yaw) return { x = math.cos(yaw), y = 0.0, z = math.sin(yaw) } end
local function axis_v(yaw) return { x = -math.sin(yaw), y = 0.0, z = math.cos(yaw) } end
-- Целое деление C++ (к нулю), а не `//` lua (вниз): у отрицательных они расходятся.
local function idiv(a, b)
  local q = a // b
  if q < 0 and q * b ~= a then q = q + 1 end
  return q
end

local function point_segment(p, a, b)
  local ab = sub(b, a)
  local len = dot(ab, ab)
  local t = len > 0.0 and W.clamp(dot(sub(p, a), ab) / len, 0.0, 1.0) or 0.0
  return length(sub(p, add(a, scale(ab, t))))
end

local function station_to_world(site, u, y, v)
  local a, b = axis_u(site.yaw), axis_v(site.yaw)
  return { x = site.origin.x + a.x * u + b.x * v, y = site.origin.y + y, z = site.origin.z + a.z * u + b.z * v }
end

-- Плоскость h = a*x + b*z + c по наименьшим квадратам: a, b, c, наибольший остаток.
local function fit_plane(points)
  local mx, mz, mh = 0.0, 0.0, 0.0
  for _, p in ipairs(points) do mx = mx + p[1]; mz = mz + p[2]; mh = mh + p[3] end
  local n = #points + 0.0
  mx = mx / n; mz = mz / n; mh = mh / n
  local xx, xz, zz, xh, zh = 0.0, 0.0, 0.0, 0.0, 0.0
  for _, p in ipairs(points) do
    local x, z, h = p[1] - mx, p[2] - mz, p[3] - mh
    xx = xx + x * x; xz = xz + x * z; zz = zz + z * z; xh = xh + x * h; zh = zh + z * h
  end
  local det = xx * zz - xz * xz
  local a, b = 0.0, 0.0
  if math.abs(det) > 1e-9 then a = (xh * zz - zh * xz) / det; b = (zh * xx - xh * xz) / det end
  local residual = 0.0
  for _, p in ipairs(points) do
    residual = math.max(residual, math.abs(p[3] - mh - a * (p[1] - mx) - b * (p[2] - mz)))
  end
  return a, b, mh - a * mx - b * mz, residual
end

return function(step)
  local p = step.params
  local seed = step.world_seed
  local key = step.key[3]
  local meta = step.writes.station_meta
  local function set3(field, i, v) field:set(i, v.x, 0); field:set(i, v.y, 1); field:set(i, v.z, 2) end

  -- ---------------------------------------------------------------- якорь
  local nodes = step.reads.nodes
  local n_key, n_centre, n_kind = nodes:field("key"), nodes:field("centre"), nodes:field("kind")
  local node_count = math.tointeger(step.reads.graph_meta:field("nodes"):get(0))
  local halls = {}
  for _, region in ipairs(step.links.regions) do
    local h_ = region.halls
    local count = math.tointeger(region.hall_counts:field("halls"):get(0))
    local hk, hc, hf, hh = h_:field("key"), h_:field("centre"), h_:field("forward"), h_:field("half")
    local hb, fl, nl, fn, nn = h_:field("body_radius"), h_:field("first_loop"), h_:field("loop_count"),
                               h_:field("first_niche"), h_:field("niche_count")
    local lp = region.hall_loop:field("point")
    local nm, ne, nd, nr = region.hall_niches:field("mouth"), region.hall_niches:field("elbow"),
                           region.hall_niches:field("end"), region.hall_niches:field("radius")
    local function at3(f, k) return { x = f:get(k, 0), y = f:get(k, 1), z = f:get(k, 2) } end
    for i = 0, count - 1 do
      local h = { key = hk:get(i), cx = hc:get(i, 0), cy = hc:get(i, 1), cz = hc:get(i, 2),
                  fx = hf:get(i, 0), fy = hf:get(i, 1), fz = hf:get(i, 2), hl = hh:get(i, 0),
                  body_radius = hb:get(i), loop = {}, niches = {} }
      local l0 = math.tointeger(fl:get(i))
      for k = l0, l0 + math.tointeger(nl:get(i)) - 1 do h.loop[#h.loop + 1] = at3(lp, k) end
      local n0 = math.tointeger(fn:get(i))
      for k = n0, n0 + math.tointeger(nn:get(i)) - 1 do
        h.niches[#h.niches + 1] = { mouth = at3(nm, k), elbow = at3(ne, k), ["end"] = at3(nd, k), radius = nr:get(k) }
      end
      halls[#halls + 1] = h
    end
  end
  local anchor = nil
  for _, candidate in ipairs(W.station_anchors(seed, nodes, step.reads.graph_meta, step.links.regions)) do
    if candidate.key == key then anchor = candidate break end
  end
  if anchor == nil then error("station: no base node or hall outpost with this key in the canonical window") end
  meta:field("anchor_key"):set(0, anchor.key)
  set3(meta:field("anchor_centre"), 0, anchor.centre)
  meta:field("kind"):set(0, anchor.kind)
  meta:field("recipe"):set(0, anchor.recipe)
  meta:field("preferred"):set(0, anchor.preferred)
  meta:field("hall_key"):set(0, anchor.hall)
  meta:field("built"):set(0, 0)
  local mask_rect = meta:field("mask")
  mask_rect:set(0, mask_i0, 0); mask_rect:set(0, mask_k0, 1); mask_rect:set(0, mask_width, 2); mask_rect:set(0, mask_depth, 3)
  local centre = anchor.centre

  -- ---------------------------------------------------------------- пробы поля
  local rays = step.writes.rays
  local r_origin, r_direction, r_hit = rays:field("origin"), rays:field("direction"), rays:field("hit")
  -- Первая порода вдоль каждого луча, уточнённая семью делениями пополам; -1 — промах.
  local function cast(list, reach)
    if #list > rays:count() then error("station: ray capacity exceeded") end
    for i, ray in ipairs(list) do set3(r_origin, i - 1, ray.o); set3(r_direction, i - 1, ray.d) end
    originator.field_march{ inputs = { r_origin, r_direction }, outputs = { r_hit },
                            params = { field = "natural_sites", half = 48, count = #list, from = 0, to = reach,
                                       step = 0.5, want = "rock", refine = 7 } }
    local out = {}
    for i = 1, #list do out[i] = r_hit:get(i - 1) end
    return out
  end
  local samples = step.writes.samples
  local s_position, s_value = samples:field("position"), samples:field("value")
  local function sample(points)
    if #points > samples:count() then error("station: sample capacity exceeded") end
    for i, q in ipairs(points) do set3(s_position, i - 1, q) end
    originator.field_sample{ inputs = { s_position }, outputs = { s_value },
                             params = { field = "natural_sites", half = 48, count = #points } }
    local out = {}
    for i = 1, #points do out[i] = s_value:get(i - 1) end
    return out
  end
  local function all_water(points)
    for _, v in ipairs(sample(points)) do if v >= 0.0 then return false end end
    return true
  end

  -- Капсула радиуса radius вокруг [a,b]: ось через метр, вокруг оси восемь направлений на полном и
  -- половинном радиусе, на концах — полусферы.
  local function capsule_points(a, b, radius, out)
    local axis = sub(b, a)
    local len = length(axis)
    axis = len > 1e-9 and scale(axis, 1.0 / len) or { x = 1.0, y = 0.0, z = 0.0 }
    local helper = math.abs(axis.y) < 0.9 and { x = 0.0, y = 1.0, z = 0.0 } or { x = 1.0, y = 0.0, z = 0.0 }
    local e1 = cross(axis, helper)
    e1 = scale(e1, 1.0 / length(e1))
    local e2 = cross(axis, e1)
    local steps = math.max(1, math.ceil(len))
    for i = 0, steps do
      local c = add(a, scale(axis, len * i / steps))
      out[#out + 1] = c
      for j = 0, 7 do
        local angle = pi * j / 4.0
        local d = add(scale(e1, math.cos(angle)), scale(e2, math.sin(angle)))
        out[#out + 1] = add(c, scale(d, radius))
        out[#out + 1] = add(c, scale(d, radius * 0.5))
      end
    end
    out[#out + 1] = add(a, scale(axis, -radius))
    out[#out + 1] = add(b, scale(axis, radius))
  end
  -- Путь лодки от поворотного узла до причала: параллельный перенос капсулы корпуса.
  local function berth_sweep_in_water(from, berth, berth_axis)
    local points = {}
    for i = 0, 5 do
      local c = add(from, scale(sub(berth, from), i / 5.0))
      capsule_points(add(c, scale(berth_axis, -sub_axis_half)), add(c, scale(berth_axis, sub_axis_half)), sub_path_radius, points)
    end
    return all_water(points)
  end
  -- Внутренняя часть: за точкой породы вдоль u лежит объём ~depth x 8 x 4 м скалы.
  local function inner_volume(site, from, depth)
    local points = {}
    for _, u in ipairs({ 2.0, 5.0, 8.0, 11.0, 14.0 }) do
      if u > depth then break end
      for _, v in ipairs({ -4.0, 0.0, 4.0 }) do
        for _, y in ipairs({ -0.75, 1.5, 3.75 }) do points[#points + 1] = station_to_world(site, from + u, y, v) end
      end
    end
    for _, value in ipairs(sample(points)) do if value <= 0.0 then return false end end
    return true
  end

  -- ---------------------------------------------------------------- запретная геометрия
  local function to_local(site, q)
    local d = sub(q, site.origin)
    return { x = dot(d, axis_u(site.yaw)), y = d.y, z = dot(d, axis_v(site.yaw)) }
  end
  local edge_count = math.tointeger(step.reads.graph_meta:field("edges"):get(0))
  local e_a, e_b = step.reads.edges:field("a"), step.reads.edges:field("b")
  local function node_centre(i)
    return { x = n_centre:get(i, 0), y = n_centre:get(i, 1), z = n_centre:get(i, 2) }
  end
  local keep = {
    routes = step.writes.keep_routes, turns = step.writes.keep_turns, berth = step.writes.keep_berth,
    tubes = step.writes.keep_tubes,
  }
  local function write_keep_out(site)
    local routes, turns, berth, tubes = {}, {}, {}, {}
    for i = 0, edge_count - 1 do
      local a, b = node_centre(math.tointeger(e_a:get(i))), node_centre(math.tointeger(e_b:get(i)))
      if point_segment(centre, a, b) <= 140.0 then routes[#routes + 1] = { to_local(site, a), to_local(site, b) } end
    end
    for i = 0, node_count - 1 do
      local c = node_centre(i)
      if length(sub(c, centre)) < 140.0 then turns[#turns + 1] = to_local(site, c) end
    end
    local half = scale(site.berth_axis, sub_axis_half)
    for i = 0, 4 do
      local c = add(centre, scale(sub(site.berth, centre), i / 4.0))
      berth[#berth + 1] = { to_local(site, sub(c, half)), to_local(site, add(c, half)) }
    end
    berth[#berth + 1] = { to_local(site, sub(centre, half)), to_local(site, sub(site.berth, half)) }
    berth[#berth + 1] = { to_local(site, add(centre, half)), to_local(site, add(site.berth, half)) }
    local function tube(h, a, b, radius)
      local wa = { W.hall_to_world(h, a.x, a.y, a.z) }
      local wb = { W.hall_to_world(h, b.x, b.y, b.z) }
      wa = { x = wa[1], y = wa[2], z = wa[3] }
      wb = { x = wb[1], y = wb[2], z = wb[3] }
      if point_segment(centre, wa, wb) > 140.0 + radius then return end
      tubes[#tubes + 1] = { to_local(site, wa), to_local(site, wb), radius }
    end
    for _, h in ipairs(halls) do
      if not (math.abs(h.cx - centre.x) > h.hl + 200.0 or math.abs(h.cz - centre.z) > h.hl + 200.0) then
        for i = 1, #h.loop do tube(h, h.loop[i], h.loop[i % #h.loop + 1], h.body_radius + surface_guard) end
        for _, n in ipairs(h.niches) do
          local radius = 1.35 * n.radius + surface_guard + 2.5
          tube(h, n.mouth, n.elbow, radius)
          tube(h, n.elbow, n["end"], radius)
        end
      end
    end
    local function put(buffer, list, pair)
      if #list > buffer:count() then error("station: keep-out capacity exceeded") end
      for i, item in ipairs(list) do
        if pair then
          set3(buffer:field("a"), i - 1, item[1]); set3(buffer:field("b"), i - 1, item[2])
          if item[3] then buffer:field("radius"):set(i - 1, item[3]) end
        else
          set3(buffer:field("centre"), i - 1, item)
        end
      end
      return #list
    end
    return { routes = put(keep.routes, routes, true), turns = put(keep.turns, turns, false),
             berth = put(keep.berth, berth, true), tubes = put(keep.tubes, tubes, true) }
  end

  -- ---------------------------------------------------------------- маска и программа
  local mask = { band = step.writes.mask_band:field("value"), held = step.writes.mask_held:field("value"),
                 margin = step.writes.mask_margin:field("value"), cells = step.writes.mask_cells:field("value") }
  local probe = step.writes.station_probe
  local keep_counts = nil
  local function reset_mask()
    originator.fill{ outputs = { mask.band }, params = { value = 255 } }
    originator.fill{ outputs = { mask.cells }, params = { value = 255 } }
    originator.fill{ outputs = { mask.held }, params = { value = 0 } }
    originator.fill{ outputs = { mask.margin }, params = { value = 0 } }
  end
  local function write_site(site)
    meta:field("pose"):set(0, site.pose)
    meta:field("outer_cells"):set(0, site.outer_cells)
    set3(meta:field("origin"), 0, site.origin)
    meta:field("yaw"):set(0, site.yaw)
    set3(meta:field("node_centre"), 0, site.node_centre)
    set3(meta:field("berth"), 0, site.berth)
    set3(meta:field("berth_axis"), 0, site.berth_axis)
    meta:field("tilt"):set(0, site.tilt_deg)
    meta:field("residual"):set(0, site.residual)
    meta:field("reach"):set(0, site.reach)
    meta:field("rock_distance"):set(0, site.rock_distance)
  end
  -- Классы клеток под прямоугольником [u0-1, u1] x [v0-1, v1]: отказ или счётчики внутренности.
  local function room_cells(r)
    originator.station_room_cells{
      inputs = { meta:field("pose"), meta:field("origin"), meta:field("yaw"), meta:field("node_centre"),
                 meta:field("berth"), meta:field("berth_axis"),
                 keep.routes:field("a"), keep.routes:field("b"), keep.turns:field("centre"), keep.berth:field("a"),
                 keep.berth:field("b"), keep.tubes:field("a"), keep.tubes:field("b"), keep.tubes:field("radius") },
      outputs = { mask.band, mask.held, mask.margin, mask.cells, probe:field("refused"), probe:field("total"),
                  probe:field("solid"), probe:field("deep"), probe:field("water") },
      params = { u0 = r.u0, v0 = r.v0, u1 = r.u1, v1 = r.v1, routes = keep_counts.routes, turns = keep_counts.turns,
                 berths = keep_counts.berth, tubes = keep_counts.tubes, i0 = mask_i0, k0 = mask_k0,
                 width = mask_width, depth = mask_depth, field = "natural_sites" } }
    if probe:field("refused"):get(0) ~= 0 then return nil end
    return math.tointeger(probe:field("total"):get(0)), math.tointeger(probe:field("solid"):get(0)),
           math.tointeger(probe:field("deep"):get(0)), math.tointeger(probe:field("water"):get(0))
  end
  local outer, passage, inside = 0, 1, 2
  local function spec(purpose, role, a0, a1, b0, b1, chamfer, parent, optional)
    return { purpose = purpose, role = role, a0 = a0, a1 = a1, b0 = b0, b1 = b1, chamfer = chamfer,
             parent = parent, optional = optional or false }
  end
  local programmes = {
    [4] = { spec("airlock", passage, 6, 6, 6, 6, 0, -1), spec("entry_hall", passage, 12, 16, 12, 16, 3, 0),
            spec("corridor", passage, 10, 28, 3, 3, 0, 1), spec("bunks", inside, 10, 14, 12, 16, 0, 2),
            spec("slums", inside, 14, 20, 14, 20, 2, 2), spec("bunks", inside, 10, 14, 12, 16, 0, 2, true),
            spec("storage", inside, 8, 12, 8, 12, 0, 2, true), spec("lounge", outer, 10, 16, 12, 18, 4, 1, true),
            spec("greenhouse", outer, 8, 12, 10, 14, 3, 1, true) },
    [3] = { spec("airlock", passage, 6, 6, 6, 6, 0, -1), spec("entry_hall", passage, 10, 14, 10, 14, 2, 0),
            spec("corridor", passage, 12, 32, 3, 3, 0, 1), spec("storage", inside, 8, 12, 10, 14, 0, 2),
            spec("bunks", inside, 10, 12, 12, 14, 0, 2), spec("mine_shaft", inside, 16, 24, 6, 6, 0, 2),
            spec("storage", inside, 8, 12, 10, 14, 0, 2, true), spec("equipment_bay", outer, 10, 16, 8, 12, 2, 1, true) },
    [5] = { spec("airlock", passage, 6, 6, 6, 6, 0, -1), spec("guard_post", passage, 10, 12, 10, 12, 2, 0),
            spec("corridor", passage, 10, 28, 3, 3, 0, 1), spec("bunks", inside, 10, 12, 12, 14, 0, 2),
            spec("armory", inside, 8, 10, 8, 10, 0, 2), spec("bunks", inside, 10, 12, 12, 14, 0, 2, true),
            spec("storage", inside, 8, 12, 8, 12, 0, 2, true), spec("watch_room", outer, 8, 10, 8, 10, 2, 1, true) },
  }
  local specs = programmes[anchor.recipe]
  if specs == nil then error("station: no programme for recipe " .. anchor.recipe) end

  local function interiors_clear(r, rooms)
    for _, o in ipairs(rooms) do
      if r.u0 - 1 < o.u1 and o.u0 < r.u1 + 1 and r.v0 - 1 < o.v1 and o.v0 < r.v1 + 1 then return false end
    end
    return true
  end
  -- side: 0 -u, 1 +v, 2 +u, 3 -v; углы 0 (u0,v0), 1 (u1,v0), 2 (u1,v1), 3 (u0,v1).
  local corner = { [0] = { 0, 3 }, [1] = { 3, 2 }, [2] = { 1, 2 }, [3] = { 0, 1 } }
  local function corner_margin(r, side, low_end) return r.chamfer[corner[side][low_end and 1 or 2]] end
  local function cells_score(r, role)
    local total, solid, deep, water = room_cells(r)
    if total == nil then return -1.0 end
    if role == outer then return solid * 5 <= total * 2 and water / total or -1.0 end
    if role == inside then return deep == total and 1.0 or -1.0 end
    return r.purpose == "corridor" and 0.25 + 0.75 * solid / total or 0.5
  end

  local function layout(site)
    local plan = { site = site, rooms = {}, doors = {} }
    local airlock = { purpose = "airlock", chamfer = { [0] = 0, 0, 0, 0 }, inner = false, parent = -1 }
    if site.pose == 0 then
      airlock.u0 = -site.outer_cells; airlock.u1 = airlock.u0 + 6
      airlock.v0 = -3; airlock.v1 = 3
      plan.hatch = { airlock.u0 - 1, -1, airlock.u0, 1 }
    else
      airlock.u0 = -3; airlock.u1 = 3; airlock.v0 = -3; airlock.v1 = 3
      plan.hatch = { -1, -2, 1, 2 }
    end
    if cells_score(airlock, passage) < 0.0 then return nil end
    plan.rooms[1] = airlock
    local placed = {}
    for i = 0, #specs - 1 do placed[i] = 0 end
    for index = 1, #specs - 1 do
      local s = specs[index + 1]
      local hash = base.mix(seed, anchor.key, index, 0x524f4f4d)
      local best_score, best = -1e30, nil
      for parent_index = 0, #plan.rooms - 1 do
        local parent = plan.rooms[parent_index + 1]
        if not (s.role ~= inside and parent.inner) then
          local kinship = parent_index == placed[s.parent] and 6.0 or 0.0
          local sides = s.role == outer and { 0, 1, 3, 2 } or { 2, 1, 3, 0 }
          if hash & 1 ~= 0 then sides[2], sides[3] = sides[3], sides[2] end
          for _, side in ipairs(sides) do
            for variant = 0, 2 do
              local h = base.mix(hash, side, variant)
              local along = s.a0 + base.umod(h, s.a1 - s.a0 + 1)
              local across = s.b0 + base.umod(h >> 8, s.b1 - s.b0 + 1)
              local u_side = side == 0 or side == 2
              local pa0 = u_side and parent.v0 or parent.u0
              local pa1 = u_side and parent.v1 or parent.u1
              for c0 = pa0 - across + 2, pa1 - 2 do
                local room = { purpose = s.purpose, inner = s.role == inside, parent = parent_index,
                               chamfer = { [0] = 0, 0, 0, 0 } }
                local c1 = c0 + across
                if side == 2 then room.u0 = parent.u1 + 1; room.u1 = room.u0 + along end
                if side == 0 then room.u1 = parent.u0 - 1; room.u0 = room.u1 - along end
                if side == 1 then room.v0 = parent.v1 + 1; room.v1 = room.v0 + along end
                if side == 3 then room.v1 = parent.v0 - 1; room.v0 = room.v1 - along end
                if u_side then room.v0 = c0; room.v1 = c1 else room.u0 = c0; room.u1 = c1 end
                if s.chamfer > 0 then
                  for c = 0, 3 do
                    if base.umod(h >> (16 + 4 * c), 3) == 0 then room.chamfer[c] = 0
                    else room.chamfer[c] = 2 + base.umod(h >> (17 + 4 * c), s.chamfer - 1) end
                  end
                end
                local room_side = (side + 2) % 4
                local o0 = math.max(u_side and room.v0 or room.u0, pa0) +
                  math.max(corner_margin(room, room_side, true), corner_margin(parent, side, true))
                local o1 = math.min(u_side and room.v1 or room.u1, pa1) -
                  math.max(corner_margin(room, room_side, false), corner_margin(parent, side, false))
                local width = (s.b0 == 3 or parent.purpose == "corridor") and 3 or 2
                if o1 - o0 >= width then
                  local d0 = idiv(o0 + o1 - width, 2)
                  local door = { a = parent_index, b = #plan.rooms }
                  if u_side then
                    door.u0 = side == 2 and parent.u1 or parent.u0 - 1; door.u1 = door.u0 + 1
                    door.v0 = d0; door.v1 = d0 + width
                  else
                    door.v0 = side == 1 and parent.v1 or parent.v0 - 1; door.v1 = door.v0 + 1
                    door.u0 = d0; door.u1 = d0 + width
                  end
                  if interiors_clear(room, plan.rooms) then
                    local fit = cells_score(room, s.role)
                    if fit >= 0.0 then
                      local cu, cv = (room.u0 + room.u1) / 2.0, (room.v0 + room.v1) / 2.0
                      local score = 20.0 * fit + kinship - 0.5 * base.hypot(cu, cv) + 3.0 * W.random01(h, c0 + 1000, 7)
                      if score > best_score then best_score = score; best = { room, door } end
                    end
                  end
                end
              end
            end
          end
        end
      end
      if best == nil and s.optional then
        placed[index] = placed[s.parent]
      elseif best == nil then
        return nil
      else
        placed[index] = #plan.rooms
        plan.rooms[#plan.rooms + 1] = best[1]
        plan.doors[#plan.doors + 1] = best[2]
      end
    end
    -- Петли: соседние через одну стену помещения иногда получают вторую дверь.
    local n = #plan.rooms
    for a = 1, n - 1 do
      for b = a + 1, n - 1 do
        local linked = false
        for _, d in ipairs(plan.doors) do
          if (d.a == a and d.b == b) or (d.a == b and d.b == a) then linked = true break end
        end
        if not (linked or W.random01(seed, anchor.key, a * 64 + b) > 0.4) then
          local ra, rb = plan.rooms[a + 1], plan.rooms[b + 1]
          local door = { a = a, b = b }
          local ok = false
          if ra.u1 + 1 == rb.u0 or rb.u1 + 1 == ra.u0 then
            local side_a = ra.u1 + 1 == rb.u0 and 2 or 0
            local o0 = math.max(ra.v0, rb.v0) + math.max(corner_margin(ra, side_a, true), corner_margin(rb, (side_a + 2) % 4, true))
            local o1 = math.min(ra.v1, rb.v1) - math.max(corner_margin(ra, side_a, false), corner_margin(rb, (side_a + 2) % 4, false))
            if o1 - o0 >= 2 then
              door.u0 = side_a == 2 and ra.u1 or ra.u0 - 1; door.u1 = door.u0 + 1
              door.v0 = idiv(o0 + o1, 2) - 1; door.v1 = door.v0 + 2
              ok = true
            end
          elseif ra.v1 + 1 == rb.v0 or rb.v1 + 1 == ra.v0 then
            local side_a = ra.v1 + 1 == rb.v0 and 1 or 3
            local o0 = math.max(ra.u0, rb.u0) + math.max(corner_margin(ra, side_a, true), corner_margin(rb, (side_a + 2) % 4, true))
            local o1 = math.min(ra.u1, rb.u1) - math.max(corner_margin(ra, side_a, false), corner_margin(rb, (side_a + 2) % 4, false))
            if o1 - o0 >= 2 then
              door.v0 = side_a == 1 and ra.v1 or ra.v0 - 1; door.v1 = door.v0 + 1
              door.u0 = idiv(o0 + o1, 2) - 1; door.u1 = door.u0 + 2
              ok = true
            end
          end
          if ok then plan.doors[#plan.doors + 1] = door end
        end
      end
    end
    return plan
  end

  local function accept(site)
    write_site(site)
    keep_counts = write_keep_out(site)
    reset_mask()
    return layout(site)
  end

  -- ---------------------------------------------------------------- место
  local yaw0 = 2.0 * pi * W.random01(seed, anchor.key, 0x594157)
  local first = anchor.preferred == 0 and 0 or (anchor.preferred > 0 and 2 or 1)
  local pass = 1
  local plan = nil

  local function try_wall()
    for k = 0, 15 do
      local yaw = yaw0 + 2.0 * pi * k / 16.0
      local u, v = axis_u(yaw), axis_v(yaw)
      local list = {}
      for _, side in ipairs({ -6.0, -2.0, 2.0, 6.0 }) do
        for _, dy in ipairs({ -1.0, 1.5 }) do
          list[#list + 1] = { o = add(add(centre, scale(v, side)), { x = 0.0, y = dy, z = 0.0 }), d = u, side = side, dy = dy }
        end
      end
      local t = cast(list, ray_reach)
      local hits = {}
      for i, ray in ipairs(list) do
        if t[i] < 0.0 then hits = nil break end
        hits[#hits + 1] = { ray.side, ray.dy, t[i] }
      end
      if hits ~= nil then
        local a, b, c, residual = fit_plane(hits)
        local site = { pose = 0, residual = residual, tilt_deg = math.atan(math.abs(b) / base.hypot(1.0, a)) * degrees }
        if not (site.tilt_deg > max_wall_tilt[pass] or residual > max_residual[pass]) then
          site.yaw = yaw - math.atan(a)
          local wall = c * math.cos(math.atan(a))
          site.reach = wall
          local floor_y = centre.y - door_centre
          local wish = 6 + 3 * base.umod(base.mix(seed, anchor.key, 0x4f5554), 3)
          local limit = math.floor((wall - keep_out - 0.5) / station_cell)
          site.outer_cells = math.min(wish, limit)
          if site.outer_cells >= 4 then
            local au = axis_u(site.yaw)
            site.origin = add(centre, scale(au, wall))
            site.origin.y = floor_y
            local face = -station_cell * (site.outer_cells + 1)
            site.berth = station_to_world(site, face - berth_gap - hull_radius, door_centre, 0.0)
            site.berth_axis = axis_v(site.yaw)
            site.node_centre = centre
            site.rock_distance = 0.0
            if inner_volume(site, 0.0, inner_depth[pass]) and berth_sweep_in_water(centre, site.berth, site.berth_axis) then
              plan = accept(site)
              if plan ~= nil then return true end
            end
          end
        end
      end
    end
    return false
  end

  local function try_vertical_at(on_floor, spot)
    local sign = on_floor and -1.0 or 1.0
    local list = {}
    for _, x in ipairs({ -5.0, 0.0, 5.0 }) do
      for _, z in ipairs({ -5.0, 0.0, 5.0 }) do
        list[#list + 1] = { o = add(spot, { x = x, y = 0.0, z = z }), d = { x = 0.0, y = sign, z = 0.0 }, x = x, z = z }
      end
    end
    local t = cast(list, ray_reach)
    local hits, reach = {}, 0.0
    for i, ray in ipairs(list) do
      if t[i] < 0.0 then return false end
      reach = math.max(reach, t[i])
      hits[#hits + 1] = { ray.x, ray.z, spot.y + sign * t[i] }
    end
    local a, b, c, residual = fit_plane(hits)
    local base_site = { pose = on_floor and 1 or 2, tilt_deg = math.atan(base.hypot(a, b)) * degrees,
                        residual = residual, reach = reach, outer_cells = 0 }
    if base_site.tilt_deg > max_floor_tilt[pass] or residual > max_residual[pass] then return false end
    local floor_y = on_floor and c + station_cell or c - station_cell * (interior_layers + 1)
    local near_face = on_floor and floor_y + station_cell * (interior_layers + 1) or floor_y - station_cell
    base_site.origin = { x = spot.x, y = floor_y, z = spot.z }
    if math.abs(near_face - centre.y) < keep_out and base.hypot(spot.x - centre.x, spot.z - centre.z) < keep_out then
      return false
    end
    local berth_y = on_floor and near_face + berth_gap + hull_radius or near_face - berth_gap - hull_radius
    local dirs = {}
    for k = 0, 15 do
      local yaw = yaw0 + 2.0 * pi * k / 16.0
      for _, y in ipairs({ 0.25, 1.75, 3.25 }) do
        dirs[#dirs + 1] = { o = add(base_site.origin, { x = 0.0, y = y, z = 0.0 }), d = axis_u(yaw) }
      end
    end
    local d_hit = cast(dirs, 22.0)
    local found = {}
    for k = 0, 15 do
      local yaw = yaw0 + 2.0 * pi * k / 16.0
      local tt = 0.0
      for j = 1, 3 do
        local hit = d_hit[k * 3 + j]
        if hit < 0.0 then tt = -1.0 elseif tt >= 0.0 then tt = math.max(tt, hit) end
      end
      if tt > 0.0 then found[#found + 1] = { yaw = yaw, distance = tt } end
    end
    -- Устойчивая сортировка по расстоянию (как std::stable_sort): вставками.
    for i = 2, #found do
      local item, j = found[i], i - 1
      while j >= 1 and item.distance < found[j].distance do found[j + 1] = found[j]; j = j - 1 end
      found[j + 1] = item
    end
    for _, d in ipairs(found) do
      local site = { pose = base_site.pose, tilt_deg = base_site.tilt_deg, residual = base_site.residual,
                     reach = base_site.reach, outer_cells = 0, origin = base_site.origin, yaw = d.yaw,
                     rock_distance = d.distance, berth = { x = spot.x, y = berth_y, z = spot.z },
                     berth_axis = axis_v(d.yaw), node_centre = centre }
      if inner_volume(site, d.distance, inner_depth[pass]) and berth_sweep_in_water(centre, site.berth, site.berth_axis) then
        plan = accept(site)
        if plan ~= nil then return true end
      end
    end
    return false
  end

  local function try_vertical(on_floor)
    if try_vertical_at(on_floor, centre) then return true end
    for k = 0, 7 do
      local angle = yaw0 + 2.0 * pi * k / 8.0
      if try_vertical_at(on_floor, add(centre, { x = 6.0 * math.cos(angle), y = 0.0, z = 6.0 * math.sin(angle) })) then
        return true
      end
    end
    return false
  end

  local done = false
  for s = 0, 2 do
    if done then break end
    local pose = (first + s) % 3
    for pass_index = 1, 3 do
      pass = pass_index
      if (pose == 0 and try_wall()) or (pose ~= 0 and try_vertical(pose == 1)) then done = true break end
    end
  end
  if plan == nil then return end

  -- ---------------------------------------------------------------- план в буферы и постройка
  local purpose_code = {}
  local index = 0
  for word in string.gmatch(p.purposes, "%S+") do purpose_code[word] = index; index = index + 1 end
  local rooms, doors = step.writes.station_rooms, step.writes.station_doors
  if #plan.rooms > rooms:count() or #plan.doors > doors:count() then error("station: room or door capacity exceeded") end
  for i, r in ipairs(plan.rooms) do
    local rect, chamfer = rooms:field("rect"), rooms:field("chamfer")
    rect:set(i - 1, r.u0, 0); rect:set(i - 1, r.v0, 1); rect:set(i - 1, r.u1, 2); rect:set(i - 1, r.v1, 3)
    for c = 0, 3 do chamfer:set(i - 1, r.chamfer[c], c) end
    local code = purpose_code[r.purpose]
    if code == nil then error("station: purpose '" .. r.purpose .. "' is not in the purposes list") end
    rooms:field("purpose"):set(i - 1, code)
    rooms:field("inner"):set(i - 1, r.inner and 1 or 0)
    rooms:field("parent"):set(i - 1, r.parent < 0 and 0xffffffff or r.parent)
  end
  for i, d in ipairs(plan.doors) do
    doors:field("a"):set(i - 1, d.a); doors:field("b"):set(i - 1, d.b)
    local rect = doors:field("rect")
    rect:set(i - 1, d.u0, 0); rect:set(i - 1, d.v0, 1); rect:set(i - 1, d.u1, 2); rect:set(i - 1, d.v1, 3)
  end
  local hatch = meta:field("hatch")
  for c = 1, 4 do hatch:set(0, plan.hatch[c], c - 1) end
  meta:field("rooms"):set(0, #plan.rooms)
  meta:field("doors"):set(0, #plan.doors)
  originator.station_build{
    inputs = { rooms:field("rect"), rooms:field("chamfer"), rooms:field("inner"), doors:field("a"), doors:field("b"),
               doors:field("rect") },
    outputs = { meta:field("built"), meta:field("pose"), meta:field("origin"), meta:field("yaw"), meta:field("node_centre"),
                meta:field("berth"), meta:field("berth_axis"), meta:field("hatch"), meta:field("kind"),
                meta:field("triangles"), meta:field("voxel_origin"), meta:field("voxel_size"), meta:field("low"),
                meta:field("high"), step.writes.station_mesh:field("a"), step.writes.station_mesh:field("b"),
                step.writes.station_mesh:field("c"), step.writes.station_mesh:field("normal"),
                step.writes.station_mesh:field("shade"), step.writes.station_voxels:field("solid"),
                step.writes.station_voxels:field("shade") },
    params = { rooms = #plan.rooms, doors = #plan.doors, field = "natural_sites" } }
  local size = meta:field("voxel_size")
  meta:field("voxels"):set(0, math.tointeger(size:get(0, 0) * size:get(0, 1) * size:get(0, 2)))
end
