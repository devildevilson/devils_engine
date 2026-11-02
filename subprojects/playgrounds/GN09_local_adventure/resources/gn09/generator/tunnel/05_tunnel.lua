-- Трасса туннеля пары. Перенос C++ (tunnel.cpp: door_choices, build_tunnel, tunnel_in_rock) бит в бит.
--
-- Выход — из внутреннего помещения сквозь прямой отрезок стены (проём 2 м), нога и тамбур 3x3 м не
-- задевают соседних помещений. Трасса — тамбур A, необязательные площадки, тамбур B: напрямую или прочь
-- от стены на 10/20 м и через уровень (середина, на 10/20 м ниже, на 10 м выше), пролёты не круче 25°.
-- Принимается первая по длине трасса, чей объём с кольцом породы 1.5 м лежит в природной породе и не
-- задевает выреза и пути лодки станций рядом (поле `carves` больше 0.4).
-- Плитки сетки и коробка готового туннеля также собираются здесь; C++ читает итоговые буферы.

local half_height, half_width = 1.25, 1.0
local leg_inset, leg_out = 0.25, 3.5
local chamber_half, chamber_offset = 1.5, 4.5
local door_zone, rock_ring = 1.25, 1.5
local station_cell = constants.station_cell
local up = { x = 0.0, y = 1.0, z = 0.0 }
local pi = math.pi

local function add(a, b) return { x = a.x + b.x, y = a.y + b.y, z = a.z + b.z } end
local function sub(a, b) return { x = a.x - b.x, y = a.y - b.y, z = a.z - b.z } end
local function scale(a, s) return { x = a.x * s, y = a.y * s, z = a.z * s } end
local function dot(a, b) return a.x * b.x + a.y * b.y + a.z * b.z end
local function length(a) return math.sqrt(dot(a, a)) end
local function cross(a, b) return { x = a.y * b.z - a.z * b.y, y = a.z * b.x - a.x * b.z, z = a.x * b.y - a.y * b.x } end
local function normalized(a) return scale(a, 1.0 / length(a)) end
local function axis_u(yaw) return { x = math.cos(yaw), y = 0.0, z = math.sin(yaw) } end
local function axis_v(yaw) return { x = -math.sin(yaw), y = 0.0, z = math.cos(yaw) } end
local function idiv(a, b)
  local q = a // b
  if q < 0 and q * b ~= a then q = q + 1 end
  return q
end
local function copy(piece)
  local out = {}
  for k, v in pairs(piece) do out[k] = v end
  return out
end
local function piece_world(piece, lx, ly, lz)
  return add(piece.centre, add(add(scale(piece.x, lx), scale(piece.y, ly)), scale(piece.z, lz)))
end

-- Станция из буферов её экземпляра: место и помещения, нужные выходу туннеля.
local function read_station(instance)
  local meta = instance.station_meta
  local function at3(field) return { x = field:get(0, 0), y = field:get(0, 1), z = field:get(0, 2) } end
  local station = { key = meta:field("anchor_key"):get(0), origin = at3(meta:field("origin")),
                    yaw = meta:field("yaw"):get(0), kind = meta:field("kind"):get(0), rooms = {} }
  local rooms = instance.station_rooms
  local rect, chamfer, inner = rooms:field("rect"), rooms:field("chamfer"), rooms:field("inner")
  for i = 0, rooms:count() - 1 do
    station.rooms[#station.rooms + 1] = {
      u0 = math.tointeger(rect:get(i, 0)), v0 = math.tointeger(rect:get(i, 1)),
      u1 = math.tointeger(rect:get(i, 2)), v1 = math.tointeger(rect:get(i, 3)),
      chamfer = { [0] = math.tointeger(chamfer:get(i, 0)), math.tointeger(chamfer:get(i, 1)),
                  math.tointeger(chamfer:get(i, 2)), math.tointeger(chamfer:get(i, 3)) },
      inner = inner:get(i) ~= 0 }
  end
  return station
end

local function station_to_world(station, u, y, v)
  local a, b = axis_u(station.yaw), axis_v(station.yaw)
  return { x = station.origin.x + a.x * u + b.x * v, y = station.origin.y + y, z = station.origin.z + a.z * u + b.z * v }
end

-- Выходы из внутренних помещений к цели, ближние первыми, не больше шести.
local function door_choices(station, target)
  local du, dv = axis_u(station.yaw), axis_v(station.yaw)
  local out = {}
  for index, r in ipairs(station.rooms) do
    if r.inner then
      for side = 0, 3 do
        local along_v = side < 2
        local sign = side % 2 == 0 and -1.0 or 1.0
        local lo, hi = 0, 0
        if side == 0 then lo = r.v0 + r.chamfer[0]; hi = r.v1 - r.chamfer[3] end
        if side == 1 then lo = r.v0 + r.chamfer[1]; hi = r.v1 - r.chamfer[2] end
        if side == 2 then lo = r.u0 + r.chamfer[0]; hi = r.u1 - r.chamfer[1] end
        if side == 3 then lo = r.u0 + r.chamfer[3]; hi = r.u1 - r.chamfer[2] end
        if hi - lo >= 6 then
          local lateral = idiv(lo + hi, 2) * station_cell
          local face
          if along_v then face = (side == 0 and r.u0 or r.u1) * station_cell
          else face = (side == 2 and r.v0 or r.v1) * station_cell end
          local near_end, far_end = face, face + sign * (chamber_offset + chamber_half + 0.5)
          local a0, a1 = math.min(near_end, far_end), math.max(near_end, far_end)
          local b0, b1 = lateral - chamber_half - 0.25, lateral + chamber_half + 0.25
          local blocked = false
          for other, o in ipairs(station.rooms) do
            if other ~= index then
              local ou0, ou1 = (o.u0 - 2) * station_cell, (o.u1 + 2) * station_cell
              local ov0, ov1 = (o.v0 - 2) * station_cell, (o.v1 + 2) * station_cell
              local pu0, pu1 = along_v and a0 or b0, along_v and a1 or b1
              local pv0, pv1 = along_v and b0 or a0, along_v and b1 or a1
              if pu0 < ou1 and ou0 < pu1 and pv0 < ov1 and ov0 < pv1 then blocked = true break end
            end
          end
          if not blocked then
            local normal = along_v and scale(du, sign) or scale(dv, sign)
            local at_face = along_v and station_to_world(station, face, half_height, lateral)
                                     or station_to_world(station, lateral, half_height, face)
            local z = normalized(cross(normal, up))
            local leg = { x = normal, y = up, z = z, hx = (leg_inset + leg_out) / 2.0, hy = half_height, hz = half_width,
                          centre = add(at_face, scale(normal, (leg_out - leg_inset) / 2.0)), station = station.key }
            local chamber = copy(leg)
            chamber.station = 0
            chamber.hx, chamber.hz = chamber_half, chamber_half
            chamber.centre = add(at_face, scale(normal, chamber_offset))
            local toward = sub(target, chamber.centre)
            if dot(toward, normal) > 0.0 then
              out[#out + 1] = { leg = leg, chamber = chamber, normal = normal, score = length(toward) }
            end
          end
        end
      end
    end
  end
  table.sort(out, function(a, b) return a.score < b.score end)
  while #out > 6 do out[#out] = nil end
  return out
end

local function room_contains(r, u, v)
  local u0, u1, v0, v1 = r.u0 * station_cell, r.u1 * station_cell, r.v0 * station_cell, r.v1 * station_cell
  local d = math.max(u0 - u, u - u1, v0 - v, v - v1)
  local cu, cv = { u0, u1, u1, u0 }, { v0, v0, v1, v1 }
  local su, sv = { 1, -1, -1, 1 }, { 1, 1, -1, -1 }
  for c = 0, 3 do
    if r.chamfer[c] ~= 0 then
      local cut = r.chamfer[c] * station_cell
      d = math.max(d, (cut - (su[c + 1] * (u - cu[c + 1]) + sv[c + 1] * (v - cv[c + 1]))) / math.sqrt(2.0))
    end
  end
  return d < 0.0
end

local function station_interior_contains(station, p)
  local d = sub(p, station.origin)
  if d.y <= 0.0 or d.y >= 3.0 then return false end
  local u, v = dot(d, axis_u(station.yaw)), dot(d, axis_v(station.yaw))
  for _, r in ipairs(station.rooms) do if room_contains(r, u, v) then return true end end
  return false
end

local function piece_contains(piece, p)
  local d = sub(p, piece.centre)
  return math.abs(dot(d, piece.x)) < piece.hx - 1e-4 and
         math.abs(dot(d, piece.y)) < piece.hy - 1e-4 and
         math.abs(dot(d, piece.z)) < piece.hz - 1e-4
end

local function breaks(lo, hi, first)
  local out = { lo }
  local at = lo
  if first > 0.0 then at = lo + first; out[#out + 1] = at end
  local count = math.max(1, math.ceil((hi - at) / 0.5 - 1e-9))
  for i = 1, count do out[#out + 1] = at + (hi - at) * i / count end
  return out
end

local function build_mesh(step, pieces, first, second)
  local meta, mesh = step.writes.tunnel_meta, step.writes.tunnel_mesh
  local ma, mb, mc, mn, ms = mesh:field("a"), mesh:field("b"), mesh:field("c"),
                            mesh:field("normal"), mesh:field("shade")
  local derelict = first.kind == 2 or second.kind == 2
  local wall_shade, slab_shade = derelict and 115 or 123, derelict and 112 or 119
  local made = 0
  local function write3(field, row, p)
    field:set(row, p.x, 0); field:set(row, p.y, 1); field:set(row, p.z, 2)
  end
  local function emit(a, b, c, normal, shade)
    if dot(cross(sub(b, a), sub(c, a)), normal) < 0.0 then b, c = c, b end
    if made >= mesh:count() then error("tunnel: mesh capacity exceeded") end
    write3(ma, made, a); write3(mb, made, b); write3(mc, made, c); write3(mn, made, normal)
    ms:set(made, shade)
    made = made + 1
  end
  for index, piece in ipairs(pieces) do
    local half = { piece.hx, piece.hy, piece.hz }
    local axes = { piece.x, piece.y, piece.z }
    for a = 0, 2 do for _, side in ipairs({ -1.0, 1.0 }) do
      local b, c = (a + 1) % 3, (a + 2) % 3
      local first_b = piece.station ~= 0 and b == 0 and leg_inset or 0.0
      local first_c = piece.station ~= 0 and c == 0 and leg_inset or 0.0
      local bs, cs = breaks(-half[b + 1], half[b + 1], first_b), breaks(-half[c + 1], half[c + 1], first_c)
      local normal = scale(axes[a + 1], -side)
      local shade = a == 1 and slab_shade or wall_shade
      for i = 1, #bs - 1 do for j = 1, #cs - 1 do
        local function corner(vb, vc)
          local l = { [a + 1] = side * half[a + 1], [b + 1] = vb, [c + 1] = vc }
          return piece_world(piece, l[1], l[2], l[3])
        end
        local p0, p1 = corner(bs[i], cs[j]), corner(bs[i + 1], cs[j])
        local p2, p3 = corner(bs[i + 1], cs[j + 1]), corner(bs[i], cs[j + 1])
        local middle = scale(add(add(p0, p1), add(p2, p3)), 0.25)
        local hidden = station_interior_contains(first, middle) or station_interior_contains(second, middle)
        for other, item in ipairs(pieces) do
          if hidden then break end
          if other ~= index and piece_contains(item, middle) then hidden = true end
        end
        if not hidden then emit(p0, p1, p2, normal, shade); emit(p0, p2, p3, normal, shade) end
      end end
    end end
  end
  local low = { x = math.huge, y = math.huge, z = math.huge }
  local high = { x = -math.huge, y = -math.huge, z = -math.huge }
  for _, piece in ipairs(pieces) do
    for _, sx in ipairs({ -1.0, 1.0 }) do for _, sy in ipairs({ -1.0, 1.0 }) do
      for _, sz in ipairs({ -1.0, 1.0 }) do
        local p = piece_world(piece, sx * piece.hx, sy * piece.hy, sz * piece.hz)
        low.x = math.min(low.x, p.x); low.y = math.min(low.y, p.y); low.z = math.min(low.z, p.z)
        high.x = math.max(high.x, p.x); high.y = math.max(high.y, p.y); high.z = math.max(high.z, p.z)
      end
    end end
  end
  low.x = low.x - 5.0; low.y = low.y - 5.0; low.z = low.z - 5.0
  high.x = high.x + 5.0; high.y = high.y + 5.0; high.z = high.z + 5.0
  write3(meta:field("low"), 0, low); write3(meta:field("high"), 0, high)
  meta:field("triangles"):set(0, made)
  meta:field("built"):set(0, 1)
end

return function(step)
  local meta = step.writes.tunnel_meta
  meta:field("built"):set(0, 0)
  meta:field("seed"):set(0, step.world_seed)
  local link = step.reads.tunnel_link
  if link:field("mutual"):get(0) == 0 then return end
  local first_key, second_key = link:field("first"):get(0), link:field("second"):get(0)
  meta:field("first"):set(0, first_key)
  meta:field("second"):set(0, second_key)
  local a = step.links.pair[1].station_meta:field("built"):get(0) ~= 0 and read_station(step.links.pair[1]) or nil
  local b = step.links.pair[2].station_meta:field("built"):get(0) ~= 0 and read_station(step.links.pair[2]) or nil
  if a == nil or b == nil then return end
  local centres = step.reads.tunnel_pair:field("centre")
  local first_centre = { x = centres:get(0, 0), y = centres:get(0, 1), z = centres:get(0, 2) }
  local second_centre = { x = centres:get(1, 0), y = centres:get(1, 1), z = centres:get(1, 2) }
  local from_a = door_choices(a, second_centre)
  local from_b = door_choices(b, first_centre)

  local grade = math.tan(constants.tunnel_max_slope_deg * pi / 180.0)
  local routes = {}
  for i = 1, math.min(#from_a, 3) do
    for j = 1, math.min(#from_b, 3) do
      local pa, pb = from_a[i].chamber.centre, from_b[j].chamber.centre
      local variants = { {} }
      for _, out in ipairs({ 10.0, 20.0 }) do
        for _, level in ipairs({ 0.5 * (pa.y + pb.y), math.min(pa.y, pb.y) - 10.0, math.min(pa.y, pb.y) - 20.0,
                                 math.max(pa.y, pb.y) + 10.0 }) do
          local w1, w2 = add(pa, scale(from_a[i].normal, out)), add(pb, scale(from_b[j].normal, out))
          w1.y, w2.y = level, level
          variants[#variants + 1] = { w1, w2 }
        end
      end
      for _, via in ipairs(variants) do
        local r = { i = i, j = j, via = via, length = 0.0, slope = 0.0 }
        local path = { pa }
        for _, w in ipairs(via) do path[#path + 1] = w end
        path[#path + 1] = pb
        local fits = true
        local k = 1
        while k + 1 <= #path and fits do
          local d = sub(path[k + 1], path[k])
          local run = base.hypot(d.x, d.z)
          fits = run >= 4.0 and math.abs(d.y) <= grade * run + 1e-9
          r.length = r.length + length(d)
          r.slope = math.max(r.slope, math.atan(math.abs(d.y), run) * 180.0 / pi)
          k = k + 1
        end
        if fits and not (r.length > 2.5 * step.params.tunnel_partner_reach) then routes[#routes + 1] = r end
      end
    end
  end
  -- Устойчивая сортировка по длине (как std::stable_sort), не больше сорока.
  for i = 2, #routes do
    local item, j = routes[i], i - 1
    while j >= 1 and item.length < routes[j].length do routes[j + 1] = routes[j]; j = j - 1 end
    routes[j + 1] = item
  end
  while #routes > 40 do routes[#routes] = nil end

  local function segment(p, q)
    local d = sub(q, p)
    local distance = length(d)
    local piece = { station = 0, hy = half_height, hz = half_width }
    piece.x = scale(d, 1.0 / distance)
    piece.z = normalized(cross(piece.x, up))
    piece.y = cross(piece.z, piece.x)
    piece.centre = scale(add(p, q), 0.5)
    piece.hx = distance / 2.0
    return piece
  end
  local function landing(at, from)
    local piece = { station = 0, y = up, centre = at, hx = chamber_half, hz = chamber_half, hy = half_height }
    piece.x = normalized({ x = at.x - from.x, y = 0.0, z = at.z - from.z })
    piece.z = normalized(cross(piece.x, up))
    return piece
  end

  local samples = step.writes.samples
  local s_position, s_value = samples:field("position"), samples:field("value")
  local function all_above(points, field, threshold)
    local done = 0
    while done < #points do
      local n = math.min(#points - done, samples:count())
      for i = 1, n do
        local p = points[done + i]
        s_position:set(i - 1, p.x, 0); s_position:set(i - 1, p.y, 1); s_position:set(i - 1, p.z, 2)
      end
      originator.field_sample{ inputs = { s_position }, outputs = { s_value }, params = { field = field, half = 24, count = n } }
      for i = 0, n - 1 do if s_value:get(i) <= threshold then return false end end
      done = done + n
    end
    return true
  end
  -- Порода вокруг кусков: природная и вне выреза и пути лодки станций рядом.
  local function in_rock(pieces)
    local points = {}
    for _, piece in ipairs(pieces) do
      local steps = math.ceil(2.0 * piece.hx / 0.5)
      for s = 0, steps do
        local lx = -piece.hx + 2.0 * piece.hx * s / steps
        local ry, rz = piece.hy + rock_ring, piece.hz + rock_ring
        for _, c in ipairs({ { 0, 0 }, { -ry, -rz }, { -ry, 0 }, { -ry, rz }, { ry, -rz }, { ry, 0 }, { ry, rz },
                             { -piece.hy, -rz }, { piece.hy, -rz }, { -piece.hy, rz }, { piece.hy, rz } }) do
          local p = piece_world(piece, lx, c[1], c[2])
          local skip = false
          if piece.station ~= 0 then
            skip = dot(sub(p, piece.centre), piece.x) + piece.hx - leg_inset < door_zone
          end
          if not skip then points[#points + 1] = p end
        end
      end
    end
    return all_above(points, "natural_sites", 0.0) and all_above(points, "carves", 0.4)
  end

  for _, r in ipairs(routes) do
    local da, db = from_a[r.i], from_b[r.j]
    local path = { da.chamber.centre }
    for _, w in ipairs(r.via) do path[#path + 1] = w end
    path[#path + 1] = db.chamber.centre
    local first_step = normalized(sub(path[2], path[1]))
    local last_step = normalized(sub(path[#path], path[#path - 1]))
    if not (dot(first_step, da.normal) < 0.09 or dot(last_step, db.normal) > -0.09) then
      local pieces = { da.leg, da.chamber }
      for k = 1, #path - 1 do
        pieces[#pieces + 1] = segment(path[k], path[k + 1])
        if k + 1 < #path then pieces[#pieces + 1] = landing(path[k + 1], path[k]) end
      end
      pieces[#pieces + 1] = db.chamber
      pieces[#pieces + 1] = db.leg
      if in_rock(pieces) then
        local out = step.writes.tunnel_pieces
        if #pieces > out:count() then error("tunnel: piece capacity exceeded") end
        local function put3(field, i, v) field:set(i, v.x, 0); field:set(i, v.y, 1); field:set(i, v.z, 2) end
        for i, piece in ipairs(pieces) do
          put3(out:field("centre"), i - 1, piece.centre)
          put3(out:field("x"), i - 1, piece.x)
          put3(out:field("y"), i - 1, piece.y)
          put3(out:field("z"), i - 1, piece.z)
          put3(out:field("half"), i - 1, { x = piece.hx, y = piece.hy, z = piece.hz })
          out:field("station"):set(i - 1, piece.station)
        end
        meta:field("pieces"):set(0, #pieces)
        meta:field("key"):set(0, base.mix(first_key, second_key, 0x54554e4e))
        meta:field("span"):set(0, r.length)
        meta:field("slope"):set(0, r.slope)
        build_mesh(step, pieces, a, b)
        return
      end
    end
  end
end
