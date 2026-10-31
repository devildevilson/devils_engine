-- Залы региона (768 м, ключ — регион x, z): эллипсоид, высота по рельефу слоя или над/под первым залом
-- (стопка), обитатель и петля его патруля, колонны, уступы, ниши-укрытия с навесами.
--
-- Перенос C++ (hall.cpp, shape_hall) бит в бит: те же хеши (modules.world.random01 = utils::mix), тот же
-- порядок вытягиваний и операций с double. Один нативный блок — hall_niche_fits: вход ниши в открытой
-- воде зала, а дно не видно ни с одной станции петли при ЛЮБОМ рельефе. Это обещание хоста, его же
-- независимо проверяет verify_hall по настоящему полю.

local W = modules.world

-- Рельеф стен и запасы — те же числа, что у формы зала в словаре форм (adventure.cpp, hall_field).
local relief_bound = 5.0 + 1.5
local core_relief = 4.0
local niche_relief = 2.5
local niche_stretch = 1.35
local rock_background = 3.46
local surface_guard = 2.0
local pillar_flare = W.pillar_flare
local pi = math.pi

local function add(a, b) return { x = a.x + b.x, y = a.y + b.y, z = a.z + b.z } end
local function sub(a, b) return { x = a.x - b.x, y = a.y - b.y, z = a.z - b.z } end
local function scale(a, s) return { x = a.x * s, y = a.y * s, z = a.z * s } end
local function dot(a, b) return a.x * b.x + a.y * b.y + a.z * b.z end
local function length(a) return math.sqrt(dot(a, a)) end
local function normalize(a) return scale(a, 1.0 / length(a)) end

local function segment_distance(p, a, b)
  local ab = sub(b, a)
  local span = dot(ab, ab)
  local t = span > 0.0 and W.clamp(dot(sub(p, a), ab) / span, 0.0, 1.0) or 0.0
  return length(sub(p, add(a, scale(ab, t))))
end

local function box_distance(p, centre, half)
  local qx = math.abs(p.x - centre.x) - half.x
  local qy = math.abs(p.y - centre.y) - half.y
  local qz = math.abs(p.z - centre.z) - half.z
  local outside = length({ x = math.max(qx, 0.0), y = math.max(qy, 0.0), z = math.max(qz, 0.0) })
  return outside + math.min(math.max(qx, qy, qz), 0.0)
end

local function smallest_radius(h) return math.min(h.half_length, h.half_width, h.half_height) end
local function loop_clearance(h) return h.body_radius + surface_guard end
local function ellipse_level(h, u, v)
  return 1.0 - (u / h.half_length) * (u / h.half_length) - (v / h.half_width) * (v / h.half_width)
end

return function(step)
  local seed = step.world_seed
  local rx, rz = step.key[1], step.key[2]
  local out_halls, out_pillars = step.writes.halls, step.writes.hall_pillars
  local out_ledges, out_niches, out_loop = step.writes.hall_ledges, step.writes.hall_niches, step.writes.hall_loop
  local f = {
    key = out_halls:field("key"), centre = out_halls:field("centre"), forward = out_halls:field("forward"),
    half = out_halls:field("half"), floor = out_halls:field("floor"), kind = out_halls:field("kind"),
    resident = out_halls:field("resident"), body = out_halls:field("body_radius"),
    first_pillar = out_halls:field("first_pillar"), pillar_count = out_halls:field("pillar_count"),
    first_ledge = out_halls:field("first_ledge"), ledge_count = out_halls:field("ledge_count"),
    first_niche = out_halls:field("first_niche"), niche_count = out_halls:field("niche_count"),
    first_loop = out_halls:field("first_loop"), loop_count = out_halls:field("loop_count"),
    pu = out_pillars:field("u"), pv = out_pillars:field("v"), pr = out_pillars:field("radius"),
    lc = out_ledges:field("centre"), lh = out_ledges:field("half"),
    nm = out_niches:field("mouth"), ne = out_niches:field("elbow"), nd = out_niches:field("end"),
    nr = out_niches:field("radius"), no = out_niches:field("overhang"),
    lp = out_loop:field("point"),
  }
  local verdict = step.writes.hall_probe:field("verdict")
  local counts = { halls = 0, pillars = 0, ledges = 0, niches = 0, loop = 0 }

  local function put3(field, i, p) field:set(i, p.x, 0); field:set(i, p.y, 1); field:set(i, p.z, 2) end
  local function check(buffer, used, what)
    if used >= buffer:count() then error("halls: " .. what .. " capacity " .. buffer:count() .. " exceeded") end
  end

  -- Строки зала пишутся в буферы сразу, как в C++ меняются векторы: блок ниш читает буферы.
  local function write_ledges(h)
    for k, ledge in ipairs(h.ledges) do
      local i = h.first_ledge + k - 1
      check(out_ledges, i, "ledge")
      put3(f.lc, i, ledge.centre); put3(f.lh, i, ledge.half)
    end
  end
  local function write_niches(h)
    for k, niche in ipairs(h.niches) do
      local i = h.first_niche + k - 1
      check(out_niches, i, "niche")
      put3(f.nm, i, niche.mouth); put3(f.ne, i, niche.elbow); put3(f.nd, i, niche["end"])
      f.nr:set(i, niche.radius); f.no:set(i, niche.overhang and 1 or 0)
    end
  end
  local function write_hall(index, h)
    check(out_halls, index, "hall")
    f.key:set(index, h.key)
    put3(f.centre, index, h.centre); put3(f.forward, index, h.forward)
    f.half:set(index, h.half_length, 0); f.half:set(index, h.half_width, 1); f.half:set(index, h.half_height, 2)
    f.floor:set(index, h.floor); f.kind:set(index, h.kind); f.resident:set(index, h.resident)
    f.body:set(index, h.body_radius)
    f.first_pillar:set(index, h.first_pillar); f.pillar_count:set(index, #h.pillars)
    f.first_ledge:set(index, h.first_ledge); f.ledge_count:set(index, #h.ledges)
    f.first_niche:set(index, h.first_niche); f.niche_count:set(index, #h.niches)
    f.first_loop:set(index, h.first_loop); f.loop_count:set(index, #h.loop)
    for k, p in ipairs(h.pillars) do
      local i = h.first_pillar + k - 1
      check(out_pillars, i, "pillar")
      f.pu:set(i, p.u); f.pv:set(i, p.v); f.pr:set(i, p.radius)
    end
    for k, p in ipairs(h.loop) do
      local i = h.first_loop + k - 1
      check(out_loop, i, "loop point")
      put3(f.lp, i, p)
    end
    write_ledges(h)
    write_niches(h)
  end

  -- Блок: 0 — вход ниши не в открытой воде зала, 1 — ниша скрыта, 2 — видна с петли.
  local function niche_fits(index, h, target, mouth)
    write_hall(index, h)
    originator.hall_niche_fits{
      inputs = { f.half, f.floor, f.body, f.pu, f.pv, f.pr, f.lc, f.lh, f.nm, f.ne, f.nd, f.nr, f.lp },
      outputs = { verdict },
      params = { hall = index, first_pillar = h.first_pillar, pillars = #h.pillars, first_ledge = h.first_ledge,
                 ledges = #h.ledges, first_niche = h.first_niche, niches = #h.niches, first_loop = h.first_loop,
                 loop = #h.loop, target = target - 1, mouth = mouth and 1 or 0 } }
    return math.tointeger(verdict:get(0))
  end

  local function shape_hall(index, key, previous)
    local stacked = false
    local h = { key = key, pillars = {}, ledges = {}, niches = {}, loop = {}, body_radius = 0.0,
                first_pillar = counts.pillars, first_ledge = counts.ledges, first_niche = counts.niches,
                first_loop = counts.loop }
    local function r(n) return W.random01(seed, key, n) end
    h.kind = r(2) < 0.4 and 1 or 0 -- 1 lair, 0 pillared
    if h.kind == 0 then
      h.half_length = 160.0 + 160.0 * r(3)
      h.half_width = 70.0 + 60.0 * r(4)
      h.half_height = 40.0 + 40.0 * r(5)
      h.floor = -0.72 * h.half_height
    else
      h.half_length = 200.0 + 180.0 * r(3)
      h.half_width = 110.0 + 70.0 * r(4)
      h.half_height = 70.0 + 50.0 * r(5)
      h.floor = -0.82 * h.half_height
    end
    local yaw = 2.0 * pi * r(6)
    h.forward = { x = math.cos(yaw), y = 0.0, z = math.sin(yaw) }

    local region_x = (rx + 0.5) * W.region_size
    local region_z = (rz + 0.5) * W.region_size
    local vertical = h.half_height * (1.0 + (rock_background + relief_bound) / smallest_radius(h)) + 60.0
    h.centre = {}
    if previous ~= nil and r(8) < 0.6 then
      stacked = true
      h.centre.x = previous.centre.x + (r(9) - 0.5) * 120.0
      h.centre.z = previous.centre.z + (r(10) - 0.5) * 120.0
      local gap = (previous.half_height + h.half_height) * (0.6 + 0.15 * r(11))
      local y = previous.centre.y + (r(7) < 0.5 and gap or -gap)
      if y + vertical > W.world_y_max or y - vertical < W.world_y_min then y = 2.0 * previous.centre.y - y end
      h.centre.y = y
    else
      h.centre.x = region_x + (r(9) - 0.5) * 240.0
      h.centre.z = region_z + (r(10) - 0.5) * 240.0
      h.centre.y = W.layer_height(seed, h.centre.x, h.centre.z) + 0.35 * h.half_height
    end

    -- Обитатель: 0 none, 1 beast, 2 lord. Логово чаще у повелителя, зал с колоннами — у зверя.
    local who = r(40)
    if h.kind == 1 then h.resident = who < 0.7 and 2 or 1
    else h.resident = who < 0.2 and 2 or (who < 0.7 and 1 or 0) end
    local phase = 2.0 * pi * r(41)
    local tier = 0
    while tier < 3 and #h.loop == 0 do
      if h.resident == 2 then h.body_radius = 16.0 + 6.0 * r(42)
      elseif h.resident == 1 then h.body_radius = 4.5 + 1.5 * r(42)
      else h.body_radius = 12.0 end
      local room = loop_clearance(h) + 2.0 * core_relief + 6.0
      local inner_u, inner_v = h.half_length - room, h.half_width - room
      local inner_y = h.half_height - room
      local loop = {}
      local ring = 0.62
      local shrink = 0
      while shrink < 12 and #loop == 0 and inner_u > 10.0 and inner_v > 10.0 and inner_y > 5.0 do
        for i = 0, 7 do
          local angle = phase + 2.0 * pi * (i + 0.3 * (r(50 + i) - 0.5)) / 8.0
          local u = ring * h.half_length * math.cos(angle)
          local v = ring * 0.9 * h.half_width * math.sin(angle)
          local level = 1.0 - (u / inner_u) * (u / inner_u) - (v / inner_v) * (v / inner_v)
          if level <= 0.0 then loop = {} break end
          local high = inner_y * math.sqrt(level)
          local low = math.max(-high, h.floor + room)
          if low >= high then loop = {} break end
          loop[#loop + 1] = { x = u, y = low + (high - low) * (0.25 + 0.5 * r(60 + i)), z = v }
        end
        shrink = shrink + 1
        ring = ring * 0.88
      end
      if #loop > 0 then h.loop = loop
      else h.resident = h.resident == 2 and 1 or 0 end
      tier = tier + 1
    end

    if h.kind == 0 then
      local wanted = 4 + math.floor(7.0 * r(20))
      local attempt = 0
      while attempt < 48 and #h.pillars < wanted do
        local a = 100 + 4 * attempt
        local pillar = { u = (r(a) - 0.5) * 1.5 * h.half_length, v = (r(a + 1) - 0.5) * 1.6 * h.half_width,
                         radius = 5.0 + 7.0 * r(a + 2) }
        local ok = ellipse_level(h, pillar.u, pillar.v) >= 0.3
        if ok then
          for i = 1, #h.loop do
            local p, q = h.loop[i], h.loop[i % #h.loop + 1]
            if segment_distance({ x = pillar.u, y = 0.0, z = pillar.v }, { x = p.x, y = 0.0, z = p.z },
                                { x = q.x, y = 0.0, z = q.z }) <
               pillar.radius * (1.0 + pillar_flare) + loop_clearance(h) + 2.0 * core_relief + 6.0 then
              ok = false
              break
            end
          end
        end
        if ok then
          for _, other in ipairs(h.pillars) do
            if base.hypot(pillar.u - other.u, pillar.v - other.v) < pillar.radius + other.radius + 18.0 then
              ok = false
              break
            end
          end
        end
        if ok then h.pillars[#h.pillars + 1] = pillar end
        attempt = attempt + 1
      end
    end

    -- Уступ не заходит в петлю: точки петли через 2 м.
    local function ledge_blocks_core(ledge)
      for i = 1, #h.loop do
        local p, q = h.loop[i], h.loop[i % #h.loop + 1]
        local span = length(sub(q, p))
        local t = 0.0
        while t <= span do
          if box_distance(add(p, scale(sub(q, p), t / span)), ledge.centre, ledge.half) <
             loop_clearance(h) + 2.0 * core_relief + 3.0 then return true end
          t = t + 2.0
        end
      end
      return false
    end

    local ledges = h.kind == 0 and 3 + math.floor(3.0 * r(21)) or 1 + math.floor(2.0 * r(21))
    local attempt = 0
    while attempt < 24 and #h.ledges < ledges do
      local a = 400 + 5 * attempt
      local side = r(a) < 0.5 and -1.0 or 1.0
      local u = (r(a + 1) - 0.5) * 1.4 * h.half_length
      local y_level = h.floor + 12.0 + (0.5 * h.half_height - h.floor - 12.0) * r(a + 2)
      local inner = 1.0 - (u / h.half_length) * (u / h.half_length) - (y_level / h.half_height) * (y_level / h.half_height)
      if inner > 0.0 then
        local wall = h.half_width * math.sqrt(inner)
        if wall >= 25.0 then
          local protrude = 12.0 + 14.0 * r(a + 3)
          local ledge = { centre = { x = u, y = y_level, z = side * (wall - protrude / 2.0 + 3.0) },
                          half = { x = 12.0 + 20.0 * r(a + 4), y = 1.5 + 2.0 * r(a + 4), z = protrude / 2.0 + 3.0 } }
          if not ledge_blocks_core(ledge) then h.ledges[#h.ledges + 1] = ledge end
        end
      end
      attempt = attempt + 1
    end

    -- Укрытия: ниша уходит в стену под углом к нормали, поэтому с оси коридора её дно закрыто скалой.
    local niches = h.kind == 1 and 4 + math.floor(3.0 * r(22)) or 3 + math.floor(3.0 * r(22))
    attempt = 0
    while attempt < 96 and #h.niches < niches do
      local a = 700 + 8 * attempt
      attempt = attempt + 1
      local side = r(a) < 0.5 and -1.0 or 1.0
      local u = (r(a + 1) - 0.5) * 1.3 * h.half_length
      local radius = 7.0 + 1.5 * r(a + 2)
      local y_level = h.floor + radius * niche_stretch + 6.0 + 0.3 * h.half_height * r(a + 3)
      local inner = 1.0 - (u / h.half_length) * (u / h.half_length) - (y_level / h.half_height) * (y_level / h.half_height)
      if inner > 0.1 then
        local wall = { x = u, y = y_level, z = side * h.half_width * math.sqrt(inner) }
        local normal = normalize({ x = wall.x / (h.half_length * h.half_length), y = wall.y / (h.half_height * h.half_height),
                                   z = wall.z / (h.half_width * h.half_width) })
        local tangent = { x = normal.z, y = 0.0, z = -normal.x }
        if length(tangent) >= 1e-3 then
          tangent = normalize(tangent)
          if r(a + 4) < 0.5 then tangent = scale(tangent, -1.0) end
          local niche = { radius = radius, overhang = false }
          niche.mouth = sub(wall, scale(normal, radius + 3.0))
          niche.elbow = add(wall, scale(normal, 32.0 + 4.0 * r(a + 5)))
          niche["end"] = add(niche.elbow, scale(tangent, 30.0 + 10.0 * r(a + 6)))
          local crowded = false
          for _, other in ipairs(h.niches) do
            if length(sub(other["end"], niche["end"])) < other.radius + radius + 14.0 or
               length(sub(other.mouth, niche.mouth)) < other.radius + radius + 10.0 then
              crowded = true
              break
            end
          end
          if not crowded then
            h.niches[#h.niches + 1] = niche
            if niche_fits(index, h, #h.niches, true) ~= 1 then
              h.niches[#h.niches] = nil
            elseif r(a + 7) < 0.5 then
              -- Навес над частью входов: уступ над нишей закрывает её и сверху.
              local protrude = 14.0
              local roof = { centre = { x = wall.x, y = wall.y + radius * niche_stretch + 2.0 * niche_relief + 3.0,
                                        z = side * (math.abs(wall.z) - protrude / 2.0 + 3.0) },
                             half = { x = 12.0, y = 2.0, z = protrude / 2.0 + 3.0 } }
              if not ledge_blocks_core(roof) then
                h.ledges[#h.ledges + 1] = roof
                niche.overhang = true
              end
            end
          end
        end
      end
    end
    -- Каждая следующая ниша вырезает воду и могла открыть луч к уже поставленной: снимаем ставшие
    -- видимыми, с конца. Навес снятой ниши остаётся простым уступом.
    for i = #h.niches, 1, -1 do
      if niche_fits(index, h, i, false) ~= 1 then table.remove(h.niches, i) end
    end
    return h, stacked
  end

  local key = base.mix(seed, rx, rz, 0x48414c4c)
  local count = W.random01(seed, key, 1) < 0.35 and 2 or 1
  local made = {}
  for i = 0, count - 1 do
    local h, stacked = shape_hall(i, base.mix(key, i), made[1])
    made[#made + 1] = h
    counts.pillars = counts.pillars + #h.pillars
    counts.ledges = counts.ledges + #h.ledges
    counts.niches = counts.niches + #h.niches
    counts.loop = counts.loop + #h.loop
    if stacked then
      -- У верхнего яруса нет плоского дна: его низ и есть пролом в нижний ярус.
      local upper = made[#made].centre.y > made[1].centre.y and made[#made] or made[1]
      upper.floor = -upper.half_height
    end
  end
  -- Аванпосты залов: на некоторых широких уступах (вдоль стены >= 18 м, от стены >= 10 м, над уступом
  -- 30 м воды до свода). Первый подходящий уступ получает аванпост с вероятностью 0.75, второй — 0.35.
  -- Якорь станции — данные: генератор станции читает его отсюда.
  local outposts = step.writes.hall_outposts
  local o = { key = outposts:field("key"), centre = outposts:field("centre"), kind = outposts:field("kind"),
              recipe = outposts:field("recipe"), preferred = outposts:field("preferred"), hall = outposts:field("hall") }
  local made_outposts = 0
  for _, h in ipairs(made) do
    local found = 0
    for i, ledge in ipairs(h.ledges) do
      if found >= 2 then break end
      if not (ledge.half.x < 9.0 or ledge.half.z < 8.0) then
        local top = ledge.centre.y + ledge.half.y
        local side = ledge.centre.z < 0.0 and -1.0 or 1.0
        local rim = ledge.centre.z - side * ledge.half.z
        local lx, ly, lz = ledge.centre.x, top + 15.0, rim + side * 0.45 * (ledge.half.z - 3.0)
        local a, c = lx / h.half_length, lz / h.half_width
        local room = 1.0 - a * a - c * c
        if not (room <= 0.0 or h.half_height * math.sqrt(room) < ly + 15.0) then
          local key = base.mix(h.key, i - 1, 0x48414c4c535441)
          if not (W.random01(seed, key, 1) > (found == 0 and 0.75 or 0.35)) then
            check(outposts, made_outposts, "outpost")
            local f_ = h.forward
            o.key:set(made_outposts, key)
            o.centre:set(made_outposts, h.centre.x + ((f_.x * lx + -f_.z * lz) + 0.0), 0)
            o.centre:set(made_outposts, h.centre.y + ((f_.y * lx + 0.0 * lz) + ly), 1)
            o.centre:set(made_outposts, h.centre.z + ((f_.z * lx + f_.x * lz) + 0.0), 2)
            -- theme: 1 outpost, 2 derelict; recipe: 3 mining, 4 residential, 5 military; поза -1 дно, 0 стена.
            o.kind:set(made_outposts, W.random01(seed, key, 2) < 0.25 and 2 or 1)
            o.recipe:set(made_outposts, 3 + base.umod(base.mix(seed, key, 3), 3))
            o.preferred:set(made_outposts, W.random01(seed, key, 4) < 0.7 and -1 or 0)
            o.hall:set(made_outposts, h.key)
            made_outposts = made_outposts + 1
            found = found + 1
          end
        end
      end
    end
  end
  step.writes.hall_counts:field("outposts"):set(0, made_outposts)

  for i, h in ipairs(made) do
    write_hall(i - 1, h)
    originator.hall_relevance{
      inputs = { f.half, f.floor, f.body, f.pu, f.pv, f.pr, f.lc, f.lh, f.nm, f.ne, f.nd, f.nr, f.lp },
      outputs = { out_halls:field("relevance") },
      params = { hall = i - 1, first_pillar = h.first_pillar, pillars = #h.pillars, first_ledge = h.first_ledge,
                 ledges = #h.ledges, first_niche = h.first_niche, niches = #h.niches, first_loop = h.first_loop,
                 loop = #h.loop } }
  end
  local c = step.writes.hall_counts
  c:field("halls"):set(0, #made)
  c:field("pillars"):set(0, counts.pillars)
  c:field("ledges"):set(0, counts.ledges)
  c:field("niches"):set(0, counts.niches)
  c:field("loop"):set(0, counts.loop)
end
