-- Тело шага layout: раскладка пещеры природного места.
--
-- Узкий небольшой проход ведёт в систему пещер: крупная ниша, из неё ходы в разные стороны — к
-- другим нишам или в тупики. Ниши связаны петлями, иногда есть второй вход. Залежи лежат в тупиках,
-- самых далёких друг от друга по пути; монстр — в нише, в боковом кармане ниши или в пустом тупике.
--
-- Пещера растёт ПО ЧАСТЯМ: часть дописывается в буферы скелета, инструмент cave_fits отвечает,
-- уместилась ли она, и неудачная часть откатывается уменьшением счётчиков. Жёсткие обещания —
-- порода вокруг, граница места, не сливаться с собой и с соседями — держит инструмент, а не этот
-- скрипт: что бы ни решил скрипт, хост их не потеряет. Мягкие обещания (глубина целей, скрытость
-- гнезда) скрипт добивается сам и может уступить, если пещера иначе не укладывается.
--
-- Случайность — функция (зерно места, поток попытки, номер вытягивания), а не состояние: одна и та
-- же попытка всегда раскладывается одинаково, сколько бы попыток ни было до неё.

return function(step)
  local p = step.params
  -- Счётчики циклов из конфига приезжают числами с плавающей точкой; номер потока случайности обязан
  -- быть целым, поэтому все такие границы приводятся здесь, один раз.
  local function whole(name)
    local v = math.tointeger(p[name])
    if v == nil then error("cave layout: value '" .. name .. "' must be a whole number") end
    return v
  end
  local site = step.reads.site
  local rooms = step.writes.rooms
  local links = step.writes.links
  local probes = step.writes.probes
  local state = step.writes.state

  local f_centre = rooms:field("centre")
  local f_radius = rooms:field("radius")
  local f_squash = rooms:field("squash")
  local f_stretch = rooms:field("stretch")
  local f_yaw = rooms:field("yaw")
  local f_purpose = rooms:field("purpose")
  local f_flags = rooms:field("flags")
  local f_distance = rooms:field("distance")
  local f_source = rooms:field("source")
  local f_passable = rooms:field("passable")
  local l_a = links:field("a")
  local l_b = links:field("b")
  local l_width = links:field("width")
  local l_roof = links:field("roof")
  local l_floor = links:field("floor")
  local l_bulge = links:field("bulge")
  local l_bulge_at = links:field("bulge_at")
  local verdict = state:field("verdict")
  local probe_origin = probes:field("origin")
  local probe_direction = probes:field("direction")
  local probe_hit = probes:field("hit")

  local meta = step.writes.layout_meta
  for w = 1, 3 do meta:field("key"):set(0, step.key[w], w - 1) end
  meta:field("seed"):set(0, step.world_seed)

  local site_centre = site:field("centre")
  local centre = { x = site_centre:get(0, 0), y = site_centre:get(0, 1), z = site_centre:get(0, 2) }
  local core = site:field("core"):get(0)
  local main_radius = site:field("main_radius"):get(0)
  local biome = math.tointeger(site:field("biome"):get(0))
  local recipe = math.tointeger(site:field("recipe"):get(0))
  local thermal = recipe == 2
  local nest_cave = recipe == 1
  local resource = not nest_cave

  -- ---------------------------------------------------------------- словарь назначений
  local function words(text)
    local out = {}
    for word in string.gmatch(text, "%S+") do out[#out + 1] = word end
    return out
  end
  local purpose_code = {}
  for index, name in ipairs(words(p.purposes)) do purpose_code[name] = index - 1 end
  local function code(name)
    local c = purpose_code[name]
    if c == nil then error("cave layout: purpose '" .. name .. "' is not in the purposes list") end
    return c
  end
  local uses = nest_cave and words(p.shelter_uses) or (thermal and words(p.thermal_uses) or words(p.ore_uses))

  -- ---------------------------------------------------------------- векторы
  local pi = math.pi
  local function add(a, b) return { x = a.x + b.x, y = a.y + b.y, z = a.z + b.z } end
  local function sub(a, b) return { x = a.x - b.x, y = a.y - b.y, z = a.z - b.z } end
  local function mul(a, s) return { x = a.x * s, y = a.y * s, z = a.z * s } end
  local function len(a) return math.sqrt(a.x * a.x + a.y * a.y + a.z * a.z) end
  local function heading(yaw, pitch)
    return { x = math.cos(pitch) * math.cos(yaw), y = math.sin(pitch), z = math.cos(pitch) * math.sin(yaw) }
  end
  local function clamp(v, low, high) return math.max(low, math.min(high, v)) end

  -- ---------------------------------------------------------------- случайность
  local seed = step.seed
  local stream, draw = 0, 0
  local function next()
    draw = draw + 1
    return base.prng64_normalize(base.prng64_2(base.prng64_2(seed, stream), draw))
  end
  local function between(low, high) return low + (high - low) * next() end
  local function pick(list) return list[math.floor(next() * #list) % #list + 1] end

  -- Облик пещеры — свой поток, он не зависит от попытки.
  stream = -1
  local style = {
    scale = between(p.style_scale_min, p.style_scale_max),
    lair = 0, radio = 0, chambers = 0, resources = 0,
  }
  if next() < 0.5 then
    style.turn_low, style.turn_high = p.turn_gentle_min, p.turn_gentle_max
  else
    style.turn_low, style.turn_high = p.turn_sharp_min, p.turn_sharp_max
  end
  style.lair = math.floor(next() * 2.999)
  style.radio = between(p.radio_min, p.radio_max)
  style.chambers = p.chambers_min + math.floor(next() * (p.chambers_extra + 0.99)) + (nest_cave and 1 or 0)
  style.resources = p.resources_min + math.floor(next() * (p.resources_extra + 0.99))
  style.vestibule = next() < p.vestibule_chance
  style.second_entrance = next() < p.second_entrance_chance

  -- ---------------------------------------------------------------- скелет
  -- Зеркало буферов в lua: комнаты и связи с номерами от нуля, как в буферах. Каждое изменение
  -- сразу пишется в буфер — инструменты читают буферы, а не это зеркало.
  local R, L = {}, {}
  local nr, nl = 0, 0

  local function put_room(i)
    local r = R[i]
    f_centre:set(i, r.c.x, 0); f_centre:set(i, r.c.y, 1); f_centre:set(i, r.c.z, 2)
    f_radius:set(i, r.radius); f_squash:set(i, r.squash); f_stretch:set(i, r.stretch); f_yaw:set(i, r.yaw)
    f_purpose:set(i, code(r.purpose)); f_flags:set(i, r.flags); f_passable:set(i, i == 0 and 0 or 1)
  end
  local function put_link(j)
    local l = L[j]
    l_a:set(j, l.a); l_b:set(j, l.b)
    l_width:set(j, l.width); l_roof:set(j, l.roof); l_floor:set(j, l.floor)
    l_bulge:set(j, l.bulge); l_bulge_at:set(j, l.bulge_at)
  end
  local function room_capacity_check()
    if nr >= rooms:count() then error("cave layout: room capacity " .. rooms:count() .. " exceeded") end
    if nl >= links:count() then error("cave layout: link capacity " .. links:count() .. " exceeded") end
  end
  local function add_room(c, radius, purpose, squash, stretch, yaw)
    room_capacity_check()
    local i = nr
    R[i] = { c = c, radius = radius, purpose = purpose, squash = squash or 1.0, stretch = stretch or 1.0,
             yaw = yaw or 0.0, flags = 0 }
    nr = nr + 1
    put_room(i)
    return i
  end
  local function set_purpose(i, purpose) R[i].purpose = purpose; put_room(i) end
  local function set_flag(i, bit) R[i].flags = R[i].flags | bit; put_room(i) end
  local function is_chamber(r) return r.radius > core + 0.01 end

  local passage_narrow = 1.0
  -- Связь с галереей в стиле биома. Ядро всегда внутри: полуоси не меньше ядра.
  local function link(a, b, narrow)
    room_capacity_check()
    local s = style.scale * (narrow or 1.0)
    local l = { a = a, b = b, width = 0, roof = 0, floor = 0, bulge = 0, bulge_at = 0.5 }
    if biome == 0 then
      l.width = core + between(0.5, 2.6) * s; l.roof = core + between(0.2, 2.0) * s; l.floor = core + between(0.0, 0.8) * s
    elseif biome == 1 then
      l.width = core + between(0.0, 1.0) * s; l.roof = core + between(2.5, 6.5) * s; l.floor = core + between(0.0, 1.2) * s
    else
      l.width = core + between(0.0, 0.8) * s; l.roof = core + between(3.0, 7.0) * s; l.floor = core + between(1.0, 3.5) * s
    end
    if next() < 0.5 then
      l.bulge = between(1.2, 3.8) * s
      l.bulge_at = between(0.2, 0.8)
    end
    L[nl] = l
    put_link(nl)
    nl = nl + 1
  end
  local function plain_link(a, b)
    room_capacity_check()
    L[nl] = { a = a, b = b, width = 0, roof = 0, floor = 0, bulge = 0, bulge_at = 0.5 }
    put_link(nl)
    nl = nl + 1
  end

  local function save() return { r = nr, l = nl } end
  local function restore(m) nr, nl = m.r, m.l end

  -- ---------------------------------------------------------------- блоки
  local skeleton = { f_centre, f_radius, f_squash, f_stretch, f_yaw, f_flags,
                     l_a, l_b, l_width, l_roof, l_floor, l_bulge, l_bulge_at,
                     meta:field("key"), meta:field("seed") }
  local function fits(m)
    originator.cave_fits{ inputs = skeleton, outputs = { verdict },
                          params = { rooms = nr, links = nl, from_rooms = m.r, from_links = m.l,
                                    clearance = step.params.clearance_radius } }
    return verdict:get(0) ~= 0
  end
  local function hidden(target)
    originator.cave_hidden{ inputs = skeleton, outputs = { verdict },
                            params = { rooms = nr, links = nl, target = target, clearance = step.params.clearance_radius } }
    return verdict:get(0) ~= 0
  end
  -- Путь по скелету от ближайшего источника; недостижимое — очень далеко.
  local function distances(sources)
    for i = 0, nr - 1 do f_source:set(i, 0) end
    for _, s in ipairs(sources) do f_source:set(s, 1) end
    originator.skeleton_distance{ inputs = { f_centre, l_a, l_b, f_source, f_passable },
                                  outputs = { f_distance }, params = { nodes = nr, edges = nl } }
    local out = {}
    for i = 0, nr - 1 do
      local d = f_distance:get(i)
      out[i] = d < 0 and 1e30 or d
    end
    return out
  end
  local function entrances()
    local out = {}
    for i = 0, nr - 1 do if R[i].flags & 1 ~= 0 then out[#out + 1] = i end end
    return out
  end
  -- Где вдоль каждого луча впервые порода (want = "rock") или вода ("water"): -1, если нигде.
  local function march(rays, from, to, step_length, want)
    if #rays > probes:count() then error("cave layout: probe capacity " .. probes:count() .. " exceeded") end
    for k, ray in ipairs(rays) do
      local i = k - 1
      probe_origin:set(i, ray.o.x, 0); probe_origin:set(i, ray.o.y, 1); probe_origin:set(i, ray.o.z, 2)
      probe_direction:set(i, ray.d.x, 0); probe_direction:set(i, ray.d.y, 1); probe_direction:set(i, ray.d.z, 2)
    end
    originator.field_march{ inputs = { probe_origin, probe_direction }, outputs = { probe_hit },
                            params = { field = "natural", half = 24, count = #rays, from = from, to = to,
                                       step = step_length, want = want } }
    local out = {}
    for k = 1, #rays do out[k] = probe_hit:get(k - 1) end
    return out
  end

  -- ---------------------------------------------------------------- грамматика
  -- Ход по шагам: каждый отрезок пробует несколько поворотов и принимается, только если уложился.
  -- Идёт, пока путь не станет длиннее target. Первый отрезок из устья — прямо в стену.
  local function walk(at, target, size, straight_first)
    local low, high = thermal and -0.8 or -0.45, thermal and 0.6 or 0.35
    local travelled = 0.0
    local first = straight_first
    while travelled < target do
      local placed = false
      for _ = 1, whole("segment_variants") do
        if placed then break end
        local before = save()
        local turn = between(style.turn_low, style.turn_high) * pi / 180.0
        local y = first and at.yaw or at.yaw + (next() < 0.5 and -1.0 or 1.0) * turn
        local pt = first and at.pitch or clamp(at.pitch + (next() - 0.55) * (thermal and 0.9 or 0.6), low, high)
        local from = R[at.tail]
        local span = size * style.scale * core * between(p.segment_min, p.segment_max)
        local start = is_chamber(from) and add(from.c, mul(heading(y, pt), 0.7 * from.radius)) or from.c
        local bend = add_room(add(start, mul(heading(y, pt), span)), core, "bend")
        link(at.tail, bend, passage_narrow)
        if fits(before) then
          travelled = travelled + len(sub(R[bend].c, from.c))
          at.tail, at.yaw, at.pitch = bend, y, pt
          placed = true
        else
          restore(before)
        end
        first = false
      end
      if not placed then return false end
    end
    return true
  end

  -- Ниша по направлению хода: вытянутая вдоль него, сплюснутая, с неровными стенами.
  local function chamber(at, radius, purpose, squash)
    for _ = 1, 3 do
      local before = save()
      local y = at.yaw + (next() - 0.5) * 0.8
      local stretch = between(1.0, 1.7)
      local index = add_room(add(R[at.tail].c, mul(heading(y, at.pitch * 0.5), 0.8 * radius * stretch)), radius,
                             purpose, squash, stretch, y)
      link(at.tail, index)
      if fits(before) then return index end
      restore(before)
    end
    return nil
  end

  -- Крупная ниша: главный объём и до двух слитых с ним — из одной части другую не видно,
  -- противоположной стены с фонарём не видно тоже.
  local function big_chamber(at, radius, purpose)
    for _ = 1, 3 do
      local before = save()
      local y = at.yaw + (next() - 0.5) * 0.8
      local stretch, squash = between(1.0, 1.5), between(0.5, 0.75)
      local index = add_room(add(R[at.tail].c, mul(heading(y, at.pitch * 0.5), 0.85 * radius * stretch)), radius,
                             purpose, squash, stretch, y)
      link(at.tail, index, passage_narrow)
      local parts
      if radius >= core + p.hall_part_large then
        parts = 1 + (next() < p.hall_part_chance_large and 1 or 0)
      else
        parts = next() < p.hall_part_chance_small and 1 or 0
      end
      for _ = 1, parts do
        local a = y + (next() < 0.5 and -1.0 or 1.0) * between(0.6, 2.6)
        local offset = radius * between(0.55, 0.9)
        local part_radius = math.max(core + 1.0, radius * between(0.5, 0.8))
        local c = R[index].c
        local at_part = add(c, { x = offset * math.cos(a), y = between(-0.25, 0.25) * radius * squash, z = offset * math.sin(a) })
        local part = add_room(at_part, part_radius, "hall_part", between(0.5, 0.8), between(1.0, 1.5), a + between(-0.5, 0.5))
        plain_link(index, part)
      end
      if fits(before) then at.tail = index; return index end
      restore(before)
    end
    return nil
  end

  -- Ветка из ниши: горловина, изогнутые отрезки, на конце камера.
  local function branch(hub, yaw, pitch, segments, length_scale, end_radius, purpose, squash)
    local hall = R[hub]
    local head = add(hall.c, mul(heading(yaw, pitch * 0.5), 0.8 * hall.radius * hall.stretch))
    local neck = add_room(head, core, "neck")
    link(hub, neck, 0.7)
    local tail = neck
    for _ = 1, segments do
      yaw = yaw + (next() < 0.5 and -1.0 or 1.0) * (25.0 + 35.0 * next()) * pi / 180.0
      pitch = clamp(pitch + (next() - 0.5) * (thermal and 1.2 or 0.9), thermal and -1.0 or -0.75, thermal and 0.9 or 0.75)
      head = add(head, mul(heading(yaw, pitch), core * length_scale * (1.2 + 0.7 * next())))
      local bend = add_room(head, core, "bend")
      link(tail, bend, 0.7)
      tail = bend
    end
    local stretch = between(1.0, 1.5)
    local finish = add(head, mul(heading(yaw, pitch * 0.5), 0.6 * end_radius * stretch))
    local index = add_room(finish, end_radius, purpose, squash, stretch, yaw)
    link(tail, index, 0.7)
    return index
  end

  -- Петли: ниши, близкие по прямой, но далёкие по пути, иногда связаны прямо — в системе с
  -- петлями легче потерять направление.
  local function loops(chambers)
    local made = 0
    for i = 1, #chambers do
      for j = i + 1, #chambers do
        if made >= p.loops_max then return end
        local d = distances({ chambers[i] })
        local a, b = R[chambers[i]].c, R[chambers[j]].c
        local straight = len(sub(b, a))
        if not (straight < p.loop_straight_min or straight > p.loop_straight_max or
                d[chambers[j]] < p.loop_detour * straight or next() < p.loop_skip_chance) then
          local before = save()
          local mid = add(mul(add(a, b), 0.5), { x = between(-6.0, 6.0), y = between(-3.0, 3.0), z = between(-6.0, 6.0) })
          local bend = add_room(mid, core, "bend")
          link(chambers[i], bend, passage_narrow)
          link(bend, chambers[j], passage_narrow)
          if fits(before) then made = made + 1 else restore(before) end
        end
      end
    end
  end

  -- Второй вход: из комнаты первой части хода короткий отрезок к воде соседней галереи или
  -- полости, не ближе заданного к другим входам.
  local function second_entrance(before_depth)
    local depth = distances(entrances())
    for i = 1, nr - 1 do
      if depth[i] >= p.second_entrance_min_depth and depth[i] <= before_depth and R[i].purpose == "bend" then
        local yaw0 = 2.0 * pi * next()
        local rays = {}
        for k = 0, 11 do
          rays[#rays + 1] = { o = R[i].c, d = heading(yaw0 + 2.0 * pi * k / 12.0, (k % 3 - 1) * 0.35) }
        end
        local hits = march(rays, p.second_entrance_reach_min, p.second_entrance_reach_max, 0.5, "water")
        for k, ray in ipairs(rays) do
          local s = hits[k]
          if s >= 0 then
            local w = add(ray.o, mul(ray.d, s))
            local far_enough = true
            for _, e in ipairs(entrances()) do
              far_enough = far_enough and len(sub(w, R[e].c)) > p.second_entrance_spacing
            end
            if far_enough then
              local before = save()
              local door = add_room(add(ray.o, mul(ray.d, math.max(s - 0.5, 4.0))), core, "entrance")
              set_flag(door, 1)
              link(i, door, 0.8)
              if fits(before) then return end
              restore(before)
            end
          end
        end
        if next() < 0.5 then return end -- пробуем немногие комнаты
      end
    end
  end

  -- Полная попытка от одного устья.
  local function grow(mouth_at, direction, size, strict)
    nr, nl = 0, 0
    add_room(centre, main_radius, "main_chamber")
    local radio_target = style.radio * size
    local mouth = add_room(mouth_at, core, "mouth")
    set_flag(mouth, 1)
    plain_link(0, mouth)
    local at = { tail = mouth, yaw = math.atan(direction.z, direction.x),
                 pitch = clamp(math.asin(clamp(direction.y, -1.0, 1.0)), thermal and -0.5 or -0.26, thermal and 0.17 or 0.26) }
    if style.vestibule then
      local hall = chamber(at, core + between(p.vestibule_min, p.vestibule_max) * style.scale, "vestibule", 0.7)
      if hall == nil then return nil end
      at.tail = hall
    end
    -- Узкий небольшой проход в систему.
    passage_narrow = p.entry_narrow
    if not walk(at, size * between(p.entry_passage_min, p.entry_passage_max), size, not style.vestibule) then return nil end
    passage_narrow = p.first_chamber_narrow
    local first = big_chamber(at, core + between(p.first_chamber_min, p.first_chamber_max) * style.scale * size, "chamber")
    if first == nil then return nil end
    local chambers, dead_ends = { first }, {}
    local frontier = { { room = first, yaw = at.yaw, pitch = at.pitch } }
    -- Из каждой ниши — ходы в разные стороны: к новой нише или в тупик.
    local f = 1
    while f <= #frontier do
      local from = frontier[f]
      local exits = f == 1 and p.first_exits + (next() < 0.5 and 1 or 0) or p.exits_min + math.floor(next() * 2.99)
      local fan = between(-0.4, 0.4)
      for e = 0, exits - 1 do
        local y = from.yaw + fan + (-2.2 + 4.4 * (e + 0.5) / exits) + between(-0.3, 0.3)
        local way = { tail = from.room, yaw = y,
                      pitch = clamp(from.pitch + between(-0.5, 0.5), thermal and -0.9 or -0.45, thermal and 0.7 or 0.4) }
        local before = save()
        passage_narrow = between(p.passage_narrow_min, p.passage_narrow_max)
        if walk(way, size * style.scale * between(p.passage_min, p.passage_max), size, false) then
          local made = false
          if #chambers < style.chambers and next() < p.chamber_chance then
            local large = next() < p.large_chamber_chance
            local radius = core + (large and between(p.large_chamber_min, p.large_chamber_max)
                                          or between(p.small_chamber_min, p.small_chamber_max)) * style.scale * size
            local index = big_chamber(way, radius, "chamber")
            if index ~= nil then
              chambers[#chambers + 1] = index
              frontier[#frontier + 1] = { room = index, yaw = way.yaw, pitch = way.pitch }
              made = true
            end
          end
          if not made then
            local index = chamber(way, core + between(p.dead_end_min, p.dead_end_max), "dead_end", 0.8)
            if index ~= nil then dead_ends[#dead_ends + 1] = index else restore(before) end
          end
        else
          restore(before)
        end
      end
      f = f + 1
    end
    if #chambers < 2 or #dead_ends < (resource and 2 or 1) then return nil end
    loops(chambers)
    if style.second_entrance then second_entrance(radio_target) end

    -- Залежи (укрытия) — в тупиках, самых далёких друг от друга по пути: обойти все значит пройти
    -- систему из конца в конец.
    local chosen, spare = {}, {}
    for _, d in ipairs(dead_ends) do spare[#spare + 1] = d end
    local want = math.min(style.resources, #dead_ends)
    while #chosen < want do
      local d = distances(#chosen == 0 and { mouth } or chosen)
      local best, best_score = 1, -1.0
      for c, candidate in ipairs(spare) do
        if d[candidate] > best_score then best_score = d[candidate]; best = c end
      end
      chosen[#chosen + 1] = spare[best]
      table.remove(spare, best)
    end
    for k, index in ipairs(chosen) do
      set_purpose(index, (resource and "pocket:" or "shelter:") .. uses[(k - 1) % #uses + 1])
      -- Горловина тупика — будущий разрушаемый лёд.
      if resource and next() < p.plug_chance then
        for j = 0, nl - 1 do
          if L[j].b == index and R[L[j].a].purpose == "bend" then
            set_purpose(L[j].a, "neck")
            set_flag(L[j].a, 2)
            break
          end
        end
      end
    end

    -- Логово: ниша (не первая), боковой карман ниши или пустой тупик — монстр не там, где его ждут.
    -- У гнездовой крупное гнездо — самая дальняя ниша, логово — раньше.
    local from_mouth = distances({ mouth })
    local later = {}
    for k = 2, #chambers do later[#later + 1] = chambers[k] end
    local nest = nil
    if nest_cave then
      local far = chambers[#chambers]
      for _, c in ipairs(chambers) do if from_mouth[c] > from_mouth[far] then far = c end end
      nest = far
      set_purpose(far, "nest")
      for k = #later, 1, -1 do if later[k] == far then table.remove(later, k) end end
    end
    for attempt = 0, whole("lair_attempts") - 1 do
      local before = save()
      local purposes = {}
      for i = 0, nr - 1 do purposes[i] = R[i].purpose end
      local kind = (style.lair + attempt) % 3
      local name = nest_cave and "lair" or "nest"
      local lair = nil
      if kind == 0 and #later > 0 then
        lair = pick(later)
        set_purpose(lair, name)
      elseif kind == 1 and #chambers > 0 then
        local host = pick(chambers)
        if host ~= nest then
          local pocket = branch(host, 2.0 * pi * next(), between(-0.6, 0.6), 1, 1.0,
                                core + between(p.lair_pocket_min, p.lair_pocket_max) * style.scale, name, 0.8)
          if fits(before) then lair = pocket end
        end
      elseif kind == 2 and #spare > 0 then
        lair = pick(spare)
        set_purpose(lair, name)
      end
      if lair ~= nil then
        local target = nest_cave and nest or lair
        if not strict or hidden(target) then
          return { mouth = mouth, nest = target, lair = lair, lair_kind = kind, radio = radio_target }
        end
      end
      restore(before)
      for i = 0, nr - 1 do if purposes[i] ~= nil and R[i].purpose ~= purposes[i] then set_purpose(i, purposes[i]) end end
    end
    return nil
  end

  -- ---------------------------------------------------------------- устья и попытки
  -- Устья — там, где лучи из центра узла впервые входят в породу; ближние стены первыми: от
  -- далёкого устья пещера не уложится в границу места.
  stream = -2
  local yaw0 = 2.0 * pi * next()
  local pitches = thermal and { -25.0, 0.0, 20.0, -40.0 } or { 0.0, -12.0, 12.0, -30.0 }
  local rays = {}
  local mouth_rays = whole("mouth_rays")
  for k = 0, mouth_rays - 1 do
    for j, pitch in ipairs(pitches) do
      rays[#rays + 1] = { o = centre, d = heading(yaw0 + 2.0 * pi * k / mouth_rays, pitch * pi / 180.0),
                          index = k * #pitches + (j - 1) }
    end
  end
  local hits = march(rays, 1.0, p.mouth_search, p.mouth_step, "rock")
  local openings = {}
  for k, ray in ipairs(rays) do
    if hits[k] >= 0 then
      openings[#openings + 1] = { mouth = add(centre, mul(ray.d, math.max(0.5, hits[k] - 0.5))), direction = ray.d,
                                  distance = hits[k], index = ray.index }
    end
  end
  -- Устойчивая сортировка по расстоянию: при равенстве — по номеру луча.
  table.sort(openings, function(l, r)
    if l.distance ~= r.distance then return l.distance < r.distance end
    return l.index < r.index
  end)

  -- Полный размер, затем компактный, затем без требования скрытого гнезда: обещания мягкие,
  -- нарушения допустимы, пока их меньшинство.
  local passes = { { 1.0, true }, { p.compact_size, true }, { p.compact_size, false } }
  local tries = 0
  local result = nil
  for _, pass in ipairs(passes) do
    local size, strict = pass[1], pass[2]
    for _, o in ipairs(openings) do
      for attempt = 0, whole("attempts") - 1 do
        if result ~= nil then break end
        tries = tries + 1
        stream = o.index * 8 + attempt + (size < 1.0 and 4 or 0) + (strict and 0 or 2048)
        draw = 0
        result = grow(o.mouth, o.direction, size, strict)
      end
      if result ~= nil then break end
    end
    if result ~= nil then break end
  end

  state:field("tries"):set(0, tries)
  state:field("scale"):set(0, style.scale)
  if result == nil then
    state:field("built"):set(0, 0)
    state:field("rooms"):set(0, 0)
    state:field("links"):set(0, 0)
    return
  end
  state:field("built"):set(0, 1)
  state:field("rooms"):set(0, nr)
  state:field("links"):set(0, nl)
  state:field("mouth"):set(0, result.mouth)
  state:field("nest"):set(0, result.nest)
  state:field("lair"):set(0, result.lair)
  state:field("lair_kind"):set(0, result.lair_kind)
  state:field("radio"):set(0, result.radio)
  state:field("gap"):set(0, 0)
end
