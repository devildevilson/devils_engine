-- Станция базы графа или аванпоста зала — три вложенных объёма, поле не спрашивается вовсе.
--
-- 1. МЕСТО — от мокрого дока к породе. Док — лодка у причала целиком в ЗАВЕДОМОЙ воде полости базы
--    (шар base_cavern_radius); рядом с ним, вне шара поворота, шлюз. Поза задаёт лишь уровень и люк:
--    стена — люк сбоку на высоте узла, дно — станция ниже узла и люк в крыше, свод — выше и люк в полу.
--    От шлюза по горизонтали (16 поворотов) луч по нижней оценке воды окна (modules.world.macro_water) до
--    первой заведомой породы на всю высоту этажа — там лицевая линия тела (u = 0). Между шлюзом и линией —
--    прямой коридор шлюза («хобот»), не длиннее trunk_max: ничего не вырезается под лодку, растягивается
--    коридор. Аванпост зала стоит на уступе (поза ledge), лодка над крышей.
-- 2. МАСКА — классы клеток плана 0.5 м. Lua считает запрет по точному расстоянию до
--    маршрутов, поворотов, пути лодки, коридоров залов; порода — за лицевой линией (на уступе — под
--    полом). Ленивое состояние маски живёт в буферах mask_*.
-- 3. ПЛАН — шлюз и коридор заданы местом, дальше рост графа помещений по программе рецепта: вход,
--    коридор, внутренние (охотнее в породе — тело), наружные (охотнее перед линией — выступают); иногда
--    техническое ответвление от коридора шлюза; петли дверей между соседями через одну стену.
-- 4. ПОРОДА — тонкий воротник вокруг помещений в скале, не ближе лицевой линии.
-- 5. ПОСТРОЙКА — modules.station_build: воксели, материал, коробка выреза; общая сетка четвертей.

local W = modules.world

local pi = math.pi
local berth_gap = 0.4
local door_centre = 0.95
local station_cell = constants.station_cell
local interior_layers = 6
local plinth_layers = 6
local rock_ring, rock_relief, rock_rounding = 2.0, 0.5, 1.0
local surface_guard = constants.surface_guard
-- Корпус лодки — константы хоста (значения цепочки), выставляются в начале шага.
local hull_radius, sub_turn_radius, sub_axis_half, sub_path_radius, keep_out
local function hull_from(p)
  hull_radius = constants.sub_radius
  sub_turn_radius = p.sub_turn_radius
  sub_axis_half = constants.sub_axis_half
  sub_path_radius = p.sub_path_radius
  keep_out = sub_turn_radius + 0.4
end
-- Маска плана: u от -36 до +36 м (шлюз на конце коридора далеко перед линией породы), v от -24 до +24.
local mask_i0, mask_k0, mask_width, mask_depth = -72, -48, 144, 96
-- Коридор шлюза не длиннее, м.
local trunk_max = 24.0

local function add(a, b) return { x = a.x + b.x, y = a.y + b.y, z = a.z + b.z } end
local function sub(a, b) return { x = a.x - b.x, y = a.y - b.y, z = a.z - b.z } end
local function scale(a, s) return { x = a.x * s, y = a.y * s, z = a.z * s } end
local function dot(a, b) return a.x * b.x + a.y * b.y + a.z * b.z end
local function length(a) return math.sqrt(dot(a, a)) end
local function cross(a, b) return { x = a.y * b.z - a.z * b.y, y = a.z * b.x - a.x * b.z,
                                    z = a.x * b.y - a.y * b.x } end
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

local function box_distance(p, low, high)
  local x = math.max(low.x - p.x, 0.0, p.x - high.x)
  local y = math.max(low.y - p.y, 0.0, p.y - high.y)
  local z = math.max(low.z - p.z, 0.0, p.z - high.z)
  return math.sqrt(x * x + y * y + z * z)
end

local function segment_box_distance(a, b, low, high, threshold)
  return originator.segment_box_distance(a, b, low, high, threshold)
end

local function station_to_world(site, u, y, v)
  local a, b = axis_u(site.yaw), axis_v(site.yaw)
  return { x = site.origin.x + a.x * u + b.x * v, y = site.origin.y + y, z = site.origin.z + a.z * u + b.z * v }
end

return function(step)
  local p = step.params
  hull_from(p)
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
  meta:field("cut_rows"):set(0, 0)
  local mask_rect = meta:field("mask")
  mask_rect:set(0, mask_i0, 0); mask_rect:set(0, mask_k0, 1); mask_rect:set(0, mask_width, 2); mask_rect:set(0, mask_depth, 3)
  local centre = anchor.centre

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
  local keep_lists
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
    keep_lists = { routes = routes, turns = turns, berth = berth, tubes = tubes }
    return { routes = put(keep.routes, routes, true), turns = put(keep.turns, turns, false),
             berth = put(keep.berth, berth, true), tubes = put(keep.tubes, tubes, true) }
  end

  -- ---------------------------------------------------------------- маска и программа
  local mask = { band = step.writes.mask_band:field("value"), held = step.writes.mask_held:field("value"),
                 margin = step.writes.mask_margin:field("value"), cells = step.writes.mask_cells:field("value") }
  local mask_cache = { band = {}, held = {}, margin = {}, cells = {} }
  local keep_counts = nil
  local mask_site, mask_berth, mask_exclusions, band_prefix
  local function reset_mask()
    mask_cache = { band = {}, held = {}, margin = {}, cells = {} }
    originator.fill{ outputs = { mask.band }, params = { value = 255 } }
    originator.fill{ outputs = { mask.cells }, params = { value = 255 } }
    originator.fill{ outputs = { mask.held }, params = { value = 0 } }
    originator.fill{ outputs = { mask.margin }, params = { value = 0 } }
  end
  local function flush_mask()
    for name, rows in pairs(mask_cache) do
      local field = mask[name]
      for index, value in pairs(rows) do field:set(index, value) end
    end
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
  local function roof_hatch(pose) return pose == 1 or pose == 3 end
  local function berth_field(u, y, v)
    local b = mask_berth
    -- За пределами AABB заметаемого корпуса значение заведомо положительно.
    if u < b.low.x or u > b.high.x or y < b.low.y or y > b.high.y or v < b.low.z or v > b.high.z then return 1.0 end
    local p = station_to_world(mask_site, u, y, v)
    local d = sub(p, b.origin)
    local distance = 1e30
    if b.nn > 1e-9 then
      local b1, b2 = dot(d, b.e1), dot(d, b.e2)
      local s = (b1 * b.a22 - b2 * b.a12) / b.det
      local t = (b2 * b.a11 - b1 * b.a12) / b.det
      if s >= 0 and s <= 1 and t >= 0 and t <= 1 then distance = math.abs(dot(d, b.normal)) / math.sqrt(b.nn) end
    end
    for i = 1, 4 do distance = math.min(distance, point_segment(p, b.corners[i], b.corners[i % 4 + 1])) end
    return distance - sub_path_radius
  end
  local function plan_field(u, y, v)
    local natural = mask_site.pose == 3 and y < 0.0 or mask_site.pose ~= 3 and u >= 0.0
    if not natural then return -1.0 end
    return math.min(1.0, berth_field(u, y, v))
  end
  local function forbidden(low, high)
    local s = mask_site
    if s.origin.y + low.y < W.world_y_min + 1.0 or s.origin.y + high.y > W.world_y_max - 1.0 then return true end
    return mask_exclusions:intersects_box(low, high)
  end
  local function mask_index(a, b) return b * mask_width + a end
  local band_y = { -0.25, 0.5, 1.5, 2.5, 3.25 }
  local function band_at(a, b)
    if a < 0 or b < 0 or a >= mask_width or b >= mask_depth then return 0 end
    local index = mask_index(a, b)
    local old = mask_cache.band[index] or 255
    if old ~= 255 then return old end
    local u, v = (mask_i0 + a + 0.5) * station_cell, (mask_k0 + b + 0.5) * station_cell
    -- Вне горизонтальной коробки причального выреза все высоты дают одну и ту же
    -- полуплоскость породы. Это основная часть маски: пять геометрических проб не нужны.
    local berth = mask_berth
    if u < berth.low.x or u > berth.high.x or v < berth.low.z or v > berth.high.z then
      local band = mask_site.pose == 3 and 1 or (u >= 0.0 and 2 or 0)
      mask_cache.band[index] = band
      if band == 2 then mask_cache.margin[index] = 1 end
      return band
    end
    local solid = 0
    for _, y in ipairs(band_y) do if plan_field(u, y, v) > 0.0 then solid = solid + 1 end end
    local band = solid == 0 and 0 or solid == #band_y and 2 or 1
    mask_cache.band[index] = band
    if band == 0 and mask_site.pose ~= 0 then
      local sign = roof_hatch(mask_site.pose) and -1.0 or 1.0
      local start = roof_hatch(mask_site.pose) and -station_cell or station_cell * (interior_layers + 1)
      for n = 1, plinth_layers do
        if plan_field(u, start + sign * station_cell * (n - 0.5), v) > 0.0 then
          mask_cache.held[index] = 1; break
        end
      end
    elseif band == 2 then
      if plan_field(u, -1.0, v) > 0.0 and plan_field(u, 4.0, v) > 0.0 then mask_cache.margin[index] = 1 end
    end
    return band
  end
  local function prepare_band_prefix()
    local stride = mask_width + 1
    local nonrock, nonwater = {}, {}
    for b = 0, mask_depth do nonrock[b * stride] = 0; nonwater[b * stride] = 0 end
    for a = 0, mask_width do nonrock[a] = 0; nonwater[a] = 0 end
    for b = 0, mask_depth - 1 do for a = 0, mask_width - 1 do
      local band = band_at(a, b)
      local at = (b + 1) * stride + a + 1
      nonrock[at] = nonrock[at - 1] + nonrock[at - stride] - nonrock[at - stride - 1] + (band ~= 2 and 1 or 0)
      nonwater[at] = nonwater[at - 1] + nonwater[at - stride] - nonwater[at - stride - 1] + (band ~= 0 and 1 or 0)
    end end
    band_prefix = { nonrock = nonrock, nonwater = nonwater, stride = stride }
  end
  local function ring(a, b, radius, same)
    local x0, y0, x1, y1 = a - radius, b - radius, a + radius, b + radius
    if same == 2 and (x0 < 0 or y0 < 0 or x1 >= mask_width or y1 >= mask_depth) then return true end
    x0, y0 = math.max(0, x0), math.max(0, y0)
    x1, y1 = math.min(mask_width - 1, x1), math.min(mask_depth - 1, y1)
    local p, stride = same == 2 and band_prefix.nonrock or band_prefix.nonwater, band_prefix.stride
    local sum = p[(y1 + 1) * stride + x1 + 1] - p[y0 * stride + x1 + 1] -
                p[(y1 + 1) * stride + x0] + p[y0 * stride + x0]
    return sum > 0
  end
  local function cell(i, k)
    local a, b = i - mask_i0, k - mask_k0
    if a < 0 or b < 0 or a >= mask_width or b >= mask_depth then return 0 end
    local index = mask_index(a, b)
    local old = mask_cache.cells[index] or 255
    if old ~= 255 then return old end
    local u, v = i * station_cell, k * station_cell
    local bottom = -station_cell - (roof_hatch(mask_site.pose) and station_cell * plinth_layers or 0.0)
    local top = station_cell * (interior_layers + 1) + (mask_site.pose == 2 and station_cell * plinth_layers or 0.0)
    local result
    if forbidden({ x = u, y = bottom, z = v }, { x = u + station_cell, y = top, z = v + station_cell }) then
      result = 0
    else
      local band = band_at(a, b)
      if band == 2 then result = mask_cache.margin[index] and not ring(a, b, 3, 2) and 5 or 4
      elseif band == 1 then result = 3
      else
        local supported = mask_cache.held[index] or ring(a, b, mask_site.pose == 0 and 12 or 8, 0)
        result = supported and 2 or 1
      end
    end
    mask_cache.cells[index] = result
    return result
  end
  -- Классы клеток под прямоугольником [u0-1, u1] x [v0-1, v1]: отказ или счётчики внутренности.
  local function room_cells(r)
    local total, solid, deep, water = 0, 0, 0, 0
    for i = r.u0 - 1, r.u1 do for k = r.v0 - 1, r.v1 do
      local c = cell(i, k)
      if c == 0 then return nil end
      if i >= r.u0 and i < r.u1 and k >= r.v0 and k < r.v1 then
        total = total + 1
        if c == 4 or c == 5 then solid = solid + 1 end
        if c == 5 then deep = deep + 1 end
        if c == 2 then water = water + 1 end
      end
    end end
    return total, solid, deep, water
  end
  local outer, passage, inside = 0, 1, 2
  local ledge = 3
  local function spec(purpose, role, a0, a1, b0, b1, chamfer, parent, optional, only_parent)
    return { purpose = purpose, role = role, a0 = a0, a1 = a1, b0 = b0, b1 = b1, chamfer = chamfer,
             parent = parent, optional = optional or false, only_parent = only_parent or false }
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
  -- Иногда у коридора шлюза техническое ответвление (насосная, щитовая) — только от самого коридора.
  if anchor.hall == 0 and W.random01(seed, anchor.key, 0x54454348) < 0.5 then
    specs[#specs + 1] = spec("service_bay", passage, 5, 8, 6, 8, 2, 0, true, true)
  end

  local function interiors_clear(r, rooms)
    for _, o in ipairs(rooms) do
      if r.u0 - 1 < o.u1 and o.u0 < r.u1 + 1 and r.v0 - 1 < o.v1 and o.v0 < r.v1 + 1 then return false end
    end
    return true
  end
  -- side: 0 -u, 1 +v, 2 +u, 3 -v; углы 0 (u0,v0), 1 (u1,v0), 2 (u1,v1), 3 (u0,v1).
  local corner = { [0] = { 0, 3 }, [1] = { 3, 2 }, [2] = { 1, 2 }, [3] = { 0, 1 } }
  local function corner_margin(r, side, low_end) return r.chamfer[corner[side][low_end and 1 or 2]] end
  -- Роль помещения — предпочтение, а не условие: внутренние охотнее за линией стены, наружные — перед
  -- ней. Отказ — только запретная клетка. Второе значение — доля клеток глубоко в породе (по ней метка
  -- «внутреннее», то есть в скале, а не выступающее).
  local function cells_score(r, role)
    local total, solid, deep = room_cells(r)
    if total == nil then return -1.0, 0.0 end
    local share = deep / total
    if role == outer then return 1.0 - solid / total, share end
    if role == inside then return share, share end
    return r.purpose == "corridor" and 0.25 + 0.75 * solid / total or 0.5, share
  end

  local function layout(site)
    local plan = { site = site, rooms = {}, doors = {} }
    local function room(purpose, u0, u1, v0, v1, parent)
      return { purpose = purpose, u0 = u0, u1 = u1, v0 = v0, v1 = v1, chamfer = { [0] = 0, 0, 0, 0 }, inner = false,
               parent = parent }
    end
    -- Шлюз и коридор шлюза заданы местом: шлюз у дока, коридор от него прямо до тела за линией породы.
    local spine = 0
    if site.pose == ledge then
      plan.rooms[1] = room("airlock", -3, 3, -3, 3, -1)
      plan.hatch = { -1, -2, 1, 2 }
      if cells_score(plan.rooms[1], passage) < 0.0 then return nil end
    else
      local a0 = site.airlock_u
      plan.rooms[1] = room("airlock", a0, a0 + 6, -3, 3, -1)
      plan.hatch = site.pose == 0 and { a0 - 1, -1, a0, 1 } or { a0 + 2, -2, a0 + 4, 2 }
      plan.rooms[2] = room("corridor", a0 + 7, 2, -2, 2, 0)
      plan.doors[1] = { a = 0, b = 1, u0 = a0 + 6, u1 = a0 + 7, v0 = -1, v1 = 1 }
      if cells_score(plan.rooms[1], passage) < 0.0 or cells_score(plan.rooms[2], passage) < 0.0 then return nil end
      spine = 1
    end
    local placed = {}
    for i = 0, #specs - 1 do placed[i] = spine end
    for index = 1, #specs - 1 do
      local s = specs[index + 1]
      local hash = base.mix(seed, anchor.key, index, 0x524f4f4d)
      local best_score, best = -1e30, nil
      for parent_index = 0, #plan.rooms - 1 do
        local parent = plan.rooms[parent_index + 1]
        if not ((s.role ~= inside and parent.inner) or (s.only_parent and parent_index ~= placed[s.parent])) then
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
              -- Вдоль длинной стены (коридор шлюза) сдвиги прорежены: не больше дюжины на сторону.
              local first_c, last_c = pa0 - across + 2, pa1 - 2
              for c0 = first_c, last_c, math.max(1, (last_c - first_c) // 12 + 1) do
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
                    local fit, share = cells_score(room, s.role)
                    room.inner = share >= 0.999
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
    mask_site = site
    local half = scale(site.berth_axis, sub_axis_half)
    local origin = sub(site.node_centre, half)
    local e1, e2 = sub(site.berth, site.node_centre), scale(half, 2.0)
    local normal = cross(e1, e2)
    local corners = { origin, add(origin, e1), add(add(origin, e1), e2), add(origin, e2) }
    local low = { x = math.huge, y = math.huge, z = math.huge }
    local high = { x = -math.huge, y = -math.huge, z = -math.huge }
    for _, corner in ipairs(corners) do
      local q = to_local(site, corner)
      low.x = math.min(low.x, q.x - sub_path_radius); low.y = math.min(low.y, q.y - sub_path_radius)
      low.z = math.min(low.z, q.z - sub_path_radius)
      high.x = math.max(high.x, q.x + sub_path_radius); high.y = math.max(high.y, q.y + sub_path_radius)
      high.z = math.max(high.z, q.z + sub_path_radius)
    end
    local a11, a12, a22 = dot(e1, e1), dot(e1, e2), dot(e2, e2)
    mask_berth = { origin = origin, e1 = e1, e2 = e2, normal = normal, nn = dot(normal, normal),
                   a11 = a11, a12 = a12, a22 = a22, det = a11 * a22 - a12 * a12,
                   corners = corners, low = low, high = high }
    keep_counts = write_keep_out(site)
    mask_exclusions = originator.exclusion_set()
    for _, edge in ipairs(keep_lists.routes) do mask_exclusions:add_segment(edge[1], edge[2], sub_path_radius) end
    for _, tube in ipairs(keep_lists.tubes) do mask_exclusions:add_segment(tube[1], tube[2], tube[3]) end
    for _, edge in ipairs(keep_lists.berth) do mask_exclusions:add_segment(edge[1], edge[2], hull_radius + 0.15) end
    for _, turn in ipairs(keep_lists.turns) do mask_exclusions:add_point(turn, sub_turn_radius) end
    reset_mask()
    prepare_band_prefix()
    return layout(site)
  end

  -- ---------------------------------------------------------------- место
  -- От дока к породе (см. шапку). Аванпост зала — на уступе (якорь в 15 м над ним): уступ знает зал, а не
  -- оценка окна. Первой пробуется поза рецепта.
  local route, edge_list = {}, {}
  local n_rank = nodes:field("rank")
  for i = 0, node_count - 1 do
    route[i] = { key = n_key:get(i), centre = node_centre(i), kind = math.tointeger(n_kind:get(i)),
                 rank = math.tointeger(n_rank:get(i)) }
  end
  for i = 0, edge_count - 1 do edge_list[#edge_list + 1] = { a = math.tointeger(e_a:get(i)), b = math.tointeger(e_b:get(i)) } end
  local water = W.macro_water(seed, p, route, edge_list, centre, 140.0)
  local yaw0 = 2.0 * pi * W.random01(seed, anchor.key, 0x594157)
  local first = anchor.preferred == 0 and 0 or (anchor.preferred > 0 and 2 or 1)
  local plan = nil
  local layers_top = station_cell * (interior_layers + 1)
  -- Док: вся лодка в шаре полости базы с запасом в метр — там воды не меньше при любой неровности.
  local dock_radius = p.base_cavern_radius - 1.0
  local function new_site(pose, yaw)
    return { pose = pose, yaw = yaw, outer_cells = 0, tilt_deg = 0.0, residual = 0.0, reach = 0.0,
             rock_distance = 0.0, node_centre = centre, berth_axis = axis_v(yaw) }
  end
  local function base_site(pose, k)
    local yaw = yaw0 + 2.0 * pi * k / 16.0
    local site = new_site(pose, yaw)
    local r = W.random01(seed, anchor.key, 0x444f434b + 16 * pose + k)
    -- Центр лодки у причала относительно узла: вдоль u на h, вверх на dy. Шлюз — вне шара поворота.
    local dy, h = 0.0, 13.0 + 1.5 * r
    if pose ~= 0 then dy, h = (pose == 1 and -12.0 or 12.0), 8.0 + 4.0 * r end
    local floor_y
    if pose == 0 then floor_y = centre.y - door_centre
    elseif pose == 1 then floor_y = centre.y + dy - hull_radius - berth_gap - layers_top
    else floor_y = centre.y + dy + hull_radius + berth_gap + station_cell end
    -- Лицевая линия тела: первая заведомая порода на всю высоту этажа. Луч по середине этажа ищет её от
    -- шлюза, у плиты пола и крыши достаточно продолжить от найденного: нужна линия, за которой порода на
    -- всех трёх высотах.
    local dir = axis_u(yaw)
    local function certain(t, y)
      return water({ x = centre.x + dir.x * t, y = floor_y + y, z = centre.z + dir.z * t }, 1.0, 0.5) > 0.5
    end
    local limit = h + 4.0 + trunk_max
    local face = nil
    for t = h + 2.0, limit, 1.0 do
      if certain(t, 1.75) then face = t break end
    end
    if face == nil then return nil end
    for _, y in ipairs({ -0.5, layers_top + 0.5 }) do
      while not certain(face, y) do
        face = face + 1.0
        if face > limit then return nil end
      end
    end
    site.origin = { x = centre.x + dir.x * face, y = floor_y, z = centre.z + dir.z * face }
    -- Шлюз в клетках плана: боковой люк лицом к лодке, вертикальный — лодка над или под шлюзом.
    local a0
    if pose == 0 then
      a0 = math.ceil((h + hull_radius + berth_gap - face) / station_cell) + 1
      site.berth = station_to_world(site, (a0 - 1) * station_cell - berth_gap - hull_radius, door_centre, 0.0)
    else
      a0 = math.floor((h - face) / station_cell + 0.5) - 3
      local y = pose == 1 and layers_top + berth_gap + hull_radius or -station_cell - berth_gap - hull_radius
      site.berth = station_to_world(site, (a0 + 3) * station_cell, y, 0.0)
    end
    local d = length(sub(site.berth, centre))
    if math.sqrt(d * d + sub_axis_half * sub_axis_half) + hull_radius > dock_radius then return nil end
    site.airlock_u = a0
    site.rock_distance = -(a0 + 6) * station_cell
    if site.rock_distance > trunk_max or a0 + 7 >= 2 then return nil end
    site.reach = face
    return site
  end
  local function ledge_site(yaw, dx, dz)
    local site = new_site(ledge, yaw)
    local floor_y = centre.y - 15.0 + station_cell
    site.origin = { x = centre.x + dx, y = floor_y, z = centre.z + dz }
    local near_face = floor_y + layers_top
    site.reach = math.abs(near_face - centre.y)
    site.berth = { x = site.origin.x, y = near_face + berth_gap + hull_radius, z = site.origin.z }
    return site
  end
  local rock = step.writes.station_rock
  -- Воротник своей породы вокруг заглублённых помещений: пустой, если таких нет или
  -- он задел бы запретную геометрию. Путь лодки к причалу не препятствие: вырез станции идёт поверх заливки.
  local function collar(candidate)
    local site = candidate.site
    local function read3(f, i) return { x = f:get(i, 0), y = f:get(i, 1), z = f:get(i, 2) } end
    local function forbidden(low, high)
      if site.origin.y + low.y < W.world_y_min + 1.0 or site.origin.y + high.y > W.world_y_max - 1.0 then return true end
      local route_a, route_b = keep.routes:field("a"), keep.routes:field("b")
      for i = 0, keep_counts.routes - 1 do
        if segment_box_distance(read3(route_a, i), read3(route_b, i), low, high, sub_path_radius) < sub_path_radius then return true end
      end
      local turn = keep.turns:field("centre")
      for i = 0, keep_counts.turns - 1 do
        if box_distance(read3(turn, i), low, high) < sub_turn_radius then return true end
      end
      local tube_a, tube_b, tube_r = keep.tubes:field("a"), keep.tubes:field("b"), keep.tubes:field("radius")
      for i = 0, keep_counts.tubes - 1 do
        local radius = tube_r:get(i)
        if segment_box_distance(read3(tube_a, i), read3(tube_b, i), low, high, radius) < radius then return true end
      end
      return false
    end
    local u0, v0, u1, v1 = math.huge, math.huge, -math.huge, -math.huge
    for _, room in ipairs(candidate.rooms) do
      if room.inner then
        u0 = math.min(u0, room.u0); v0 = math.min(v0, room.v0)
        u1 = math.max(u1, room.u1); v1 = math.max(v1, room.v1)
      end
    end
    local made = u0 < u1
    local low, high = { x = 1.0, y = 1.0, z = 1.0 }, { x = 0.0, y = 0.0, z = 0.0 }
    if made then
      local bottom = -station_cell - ((site.pose == 1 or site.pose == 3) and station_cell * plinth_layers or 0.0)
      local top = station_cell * (interior_layers + 1) + (site.pose == 2 and station_cell * plinth_layers or 0.0)
      low = { x = u0 * station_cell - rock_ring, y = bottom - rock_ring, z = v0 * station_cell - rock_ring }
      high = { x = u1 * station_cell + rock_ring, y = top + rock_ring, z = v1 * station_cell + rock_ring }
      if site.pose ~= 3 then low.x = math.max(low.x, 0.0) end
      local reach = rock_relief + 1.0
      if forbidden({ x = low.x - reach, y = low.y - reach, z = low.z - reach },
                   { x = high.x + reach, y = high.y + reach, z = high.z + reach }) then
        made = false
        low, high = { x = 1.0, y = 1.0, z = 1.0 }, { x = 0.0, y = 0.0, z = 0.0 }
      end
    end
    local mid = scale(add(low, high), 0.5)
    set3(rock:field("centre"), 0, station_to_world(site, mid.x, mid.y, mid.z))
    set3(rock:field("axis_u"), 0, axis_u(site.yaw))
    set3(rock:field("axis_v"), 0, axis_v(site.yaw))
    set3(rock:field("half"), 0, scale(sub(high, low), 0.5))
    rock:field("relief"):set(0, rock_relief)
    rock:field("rounding"):set(0, rock_rounding)
    set3(rock:field("local_low"), 0, low)
    set3(rock:field("local_high"), 0, high)
    meta:field("rock_rows"):set(0, made and 1 or 0)
  end
  local function try(site)
    if site == nil then return false end
    plan = accept(site)
    return plan ~= nil
  end
  if anchor.hall ~= 0 then
    -- Уступ: под якорем и со сдвигом (его окружают ход и шары поворота зала).
    for _, shift in ipairs({ { 0, 0 }, { 6, 0 }, { -6, 0 }, { 0, 6 }, { 0, -6 } }) do
      if plan ~= nil then break end
      for k = 0, 3 do
        if try(ledge_site(yaw0 + 0.5 * pi * k, shift[1], shift[2])) then break end
      end
    end
  else
    for s = 0, 2 do
      if plan ~= nil then break end
      for k = 0, 15 do
        if try(base_site((first + s) % 3, k)) then break end
      end
    end
  end
  if plan ~= nil then
    collar(plan)
    -- Без воротника «в скале» не обещано никому: метка снимается.
    if meta:field("rock_rows"):get(0) == 0 then
      for _, r in ipairs(plan.rooms) do r.inner = false end
    end
  end
  flush_mask()
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
  local function read3(field)
    return { x = field:get(0, 0), y = field:get(0, 1), z = field:get(0, 2) }
  end
  modules.station_build.build(plan, step, p, read3(rock:field("local_low")), read3(rock:field("local_high")),
                              sub_axis_half, sub_path_radius)
  if meta:field("built"):get(0) == 0 then return end

  -- Вырезы поля — строки, а не объект C++: каждый прямоугольник знает локальную рамку, границы влияния
  -- и высоты. Нулевой срез угла выключен отрицательным пределом, остальные точно повторяют план.
  meta:field("sub_axis_half"):set(0, sub_axis_half)
  meta:field("sub_path_radius"):set(0, sub_path_radius)
  local cuts = step.writes.station_cuts
  local count = 0
  local function put_cut(u0, v0, u1, v1, y0, y1, chamfer)
    if count >= cuts:count() then error("station: cut capacity exceeded") end
    local ax, av = axis_u(plan.site.yaw), axis_v(plan.site.yaw)
    local lo = { x = math.huge, y = math.huge, z = math.huge }
    local hi = { x = -math.huge, y = -math.huge, z = -math.huge }
    for _, u in ipairs({ u0, u1 }) do for _, v in ipairs({ v0, v1 }) do
      for _, y in ipairs({ y0, y1 }) do
        local w = station_to_world(plan.site, u, y, v)
        lo.x = math.min(lo.x, w.x); lo.y = math.min(lo.y, w.y); lo.z = math.min(lo.z, w.z)
        hi.x = math.max(hi.x, w.x); hi.y = math.max(hi.y, w.y); hi.z = math.max(hi.z, w.z)
      end
    end end
    set3(cuts:field("origin"), count, plan.site.origin)
    set3(cuts:field("axis_u"), count, ax); set3(cuts:field("axis_v"), count, av)
    set3(cuts:field("low"), count, lo); set3(cuts:field("high"), count, hi)
    local rect, heights, angles = cuts:field("rect"), cuts:field("heights"), cuts:field("chamfer")
    rect:set(count, u0, 0); rect:set(count, v0, 1); rect:set(count, u1, 2); rect:set(count, v1, 3)
    heights:set(count, y0, 0); heights:set(count, y1, 1)
    for c = 0, 3 do angles:set(count, chamfer and chamfer[c] and chamfer[c] > 0 and chamfer[c] * station_cell or -1e6, c) end
    count = count + 1
  end
  local ceiling = station_cell * interior_layers
  for _, r in ipairs(plan.rooms) do
    put_cut(r.u0 * station_cell, r.v0 * station_cell, r.u1 * station_cell, r.v1 * station_cell,
            0.0, ceiling, r.chamfer)
  end
  for _, d in ipairs(plan.doors) do
    put_cut(d.u0 * station_cell, d.v0 * station_cell, d.u1 * station_cell, d.v1 * station_cell, 0.0, ceiling)
  end
  local h = plan.hatch
  if plan.site.pose == 0 then
    put_cut((h[1] - 2) * station_cell, h[2] * station_cell, h[3] * station_cell, h[4] * station_cell, 0.0, 2.0)
  elseif plan.site.pose == 1 or plan.site.pose == 3 then
    put_cut(h[1] * station_cell, h[2] * station_cell, h[3] * station_cell, h[4] * station_cell, 0.0, ceiling + 1.5)
  else
    put_cut(h[1] * station_cell, h[2] * station_cell, h[3] * station_cell, h[4] * station_cell, -1.5, ceiling)
  end
  meta:field("cut_rows"):set(0, count)
end
