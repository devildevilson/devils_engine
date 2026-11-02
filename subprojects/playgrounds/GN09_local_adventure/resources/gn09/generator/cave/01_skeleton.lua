-- Тело шага skeleton: горло, дерево ходов с нишами, залежи, гнездо и логово — и всё это строками форм.
--
-- Поле не спрашивается вовсе. Чужая вода — галереи, выступы, карманы, ходы лодки и шары поворота окна у
-- этого узла — пересчитывается из графа теми же правилами модуля `world`, и для каждой формы есть нижняя
-- оценка её значения (отрицательно — возможно вода). Скелет держится от неё на ядро своей части плюс
-- запас породы; от своих прежних частей — так же. Не уложилось — меньше ходов, а не отказ.
--
-- Горло: прямой отрезок из полости узла в сторону, где порода начинается ближе всего (направление по
-- сфере — стена, пол или свод). Ядро горла меньше радиуса корпуса лодки, сечение точное (без вздутий и
-- выступов, стенка не дальше ядра), поэтому лодка в пещеру не входит, а ныряльщик проходит.
--
-- Время: K залежей, сбор одной N, весь заплыв T. Длина ходов подбирается так, чтобы обход залежей и обратно
-- со скоростью ныряльщика занял T - K N; лишние ходы (тупики, ветки без залежей) — сверху. Залежи
-- выбираются дальними друг от друга по пути (не обязательно в тупиках). Оценка по готовому скелету
-- пишется в `state` рядом с бюджетом.

local W = modules.world
local add, sub, scale, dot, length = W.v_add, W.v_sub, W.v_scale, W.v_dot, W.v_length
local pi = math.pi

local function normalized(a) return scale(a, 1.0 / length(a)) end
local function lerp(a, b, t) return a + (b - a) * t end
local function point_segment(p, a, b)
  local ab = sub(b, a)
  local t = W.clamp(dot(sub(p, a), ab) / dot(ab, ab), 0.0, 1.0)
  return length(sub(p, add(a, scale(ab, t))))
end
local function heading(d) return math.atan(d.z, d.x), math.asin(W.clamp(d.y, -1.0, 1.0)) end
local function direction(yaw, pitch)
  return { x = math.cos(pitch) * math.cos(yaw), y = math.sin(pitch), z = math.cos(pitch) * math.sin(yaw) }
end

return function(step)
  local p = step.params
  local seed = step.world_seed
  local key = base.mix(step.key[1], step.key[2], step.key[3], 0x534b454c)
  local counter = 0
  local function rnd() counter = counter + 1; return W.random01(seed, key, counter) end
  local function pick(lo, hi) return lerp(lo, hi, rnd()) end

  local site = step.reads.site
  local centre = { x = site:field("centre"):get(0, 0), y = site:field("centre"):get(0, 1), z = site:field("centre"):get(0, 2) }
  local core = site:field("core"):get(0)
  local main_radius = site:field("main_radius"):get(0)
  local biome = math.tointeger(site:field("biome"):get(0))
  local recipe = math.tointeger(site:field("recipe"):get(0))
  local extent = site:field("extent"):get(0) - p.shape_margin
  local rock = 6 + recipe -- rock_kind хоста: ore_cave, nest_cave, thermal_cave
  local margin = p.rock_margin

  local state = step.writes.state
  local function set_state(name, v) state:field(name):set(0, v) end

  -- Граф канонической окрестности.
  local nodes = step.reads.nodes
  local n_key, n_centre, n_kind, n_rank = nodes:field("key"), nodes:field("centre"), nodes:field("kind"), nodes:field("rank")
  local meta = step.reads.graph_meta
  local node_count = math.tointeger(meta:field("nodes"):get(0))
  local route, own = {}, nil
  for i = 0, node_count - 1 do
    route[i] = { key = n_key:get(i), centre = { x = n_centre:get(i, 0), y = n_centre:get(i, 1), z = n_centre:get(i, 2) },
                 kind = math.tointeger(n_kind:get(i)), rank = math.tointeger(n_rank:get(i)) }
    if route[i].key == step.key[3] then own = route[i] end
  end
  if own == nil then error(string.format("cave: node %x is not in its graph", step.key[3])) end
  local edges = {}
  local e_a, e_b = step.reads.edges:field("a"), step.reads.edges:field("b")
  for i = 0, math.tointeger(meta:field("edges"):get(0)) - 1 do
    edges[#edges + 1] = { a = math.tointeger(e_a:get(i)), b = math.tointeger(e_b:get(i)) }
  end
  -- Чужая вода: оценка снизу ближайшей формы окна (больше — дальше в породе), modules.world.
  local water = W.macro_water(seed, p, route, edges, centre, extent + 40.0)

  -- Своя пещера: отрезки и шары с радиусом основного сечения (без вздутий и шума: ходы, сошедшиеся
  -- вздутиями, — петля, а не беда). Части, сходящиеся в узле, откуда растёт ход (incident), не
  -- проверяются у самого узла (в зоне zone): там ходы и ниша и должны сливаться.
  local parts = {}
  local function own_clear(q, need, junction, zone, incident)
    local near_junction = length(sub(q, junction)) <= zone
    for i, part in ipairs(parts) do
      if not (incident[i] and near_junction) then
        local d = part.b and point_segment(q, part.a, part.b) or length(sub(q, part.a))
        if d < need + part.radius then return false end
      end
    end
    return true
  end

  local w = W.shape_writers(step)
  local rooms, links = {}, {}
  local s = pick(p.passage_scale_min, p.passage_scale_max)
  set_state("scale", s)
  set_state("radio", pick(p.radio_min, p.radio_max))

  -- ---------------------------------------------------------------------------------------- горло
  local turn = W.turn_radius(seed, own, p)
  -- Горло — обычный ход пещеры без выступов, уже корпуса лодки: круг, вписанный в сечение (меньшая из
  -- наибольших полуосей с неровностью стенки, или ядро), меньше радиуса корпуса. Если ход этого масштаба
  -- шире — масштаб горла уменьшается.
  local throat_core = core
  local throat_key = base.mix(key, 0x544852)
  local throat_scale = s
  local function throat_shape(scale_now)
    local row = W.gallery(seed, throat_key, { x = 0, y = 0, z = 0 }, { x = 1, y = 0, z = 0 },
                          { scale = scale_now, core = throat_core, biome = biome, rock = rock, lobes = false })
    local wide = (row.base_width + math.max(row.first_width, row.second_width)) * row.biome_width
    local tall = row.base_roof + 2.5 * row.pulse + row.vertical_gain
    return math.max(throat_core, math.min(wide, tall) + 1.2), math.max(wide, tall) + 1.2
  end
  local inscribed, throat_reach = throat_shape(throat_scale)
  for _ = 1, 8 do
    if inscribed < p.sub_beam / 2.0 then break end
    throat_scale = throat_scale * 0.8
    inscribed, throat_reach = throat_shape(throat_scale)
  end
  local choices = {}
  local tries = math.tointeger(p.mouth_directions)
  local spin = 2.0 * pi * rnd()
  for k = 0, tries - 1 do
    -- Сфера Фибоначчи, повёрнутая на случайный угол: направление может быть любым.
    local y = 1.0 - 2.0 * (k + 0.5) / tries
    local r = math.sqrt(1.0 - y * y)
    local a = spin + k * 2.399963229728653
    local dir = { x = r * math.cos(a), y = y, z = r * math.sin(a) }
    local exit
    for t = turn, p.throat_search, 1.0 do
      if water(add(centre, scale(dir, t)), throat_reach + margin, throat_reach + margin) > throat_reach + margin then exit = t break end
    end
    if exit then
      local narrow = pick(p.throat_narrow_min, p.throat_narrow_max)
      local hub = pick(p.hub_min, p.hub_max)
      local hub_relief = pick(p.chamber_relief_min, p.chamber_relief_max)
      -- Предзал: шар у устья, открытый в полость узла (лодка заходит в него носом); узкая часть горла
      -- начинается за его огибающей.
      local vest, vest_relief = 0.0, 0.0
      if rnd() < p.vestibule_chance then
        vest, vest_relief = pick(p.vestibule_min, p.vestibule_max), pick(p.chamber_relief_min, p.chamber_relief_max)
      end
      -- Центр предзала — на полрадиуса ближе к узлу, чем граница заведомой породы: он вдаётся в стену.
      local start = vest > 0.0 and exit + 0.5 * vest + 2.0 * vest_relief + 1.0 or exit
      local reach = start + narrow + hub + 2.0 * hub_relief + 1.0
      local h = add(centre, scale(dir, reach))
      local fits = length(sub(h, centre)) <= extent and
                   water(h, hub + 2.0 * hub_relief + margin, hub + 2.0 * hub_relief + margin) > hub + 2.0 * hub_relief + margin
      for t = start, start + narrow, 1.0 do
        fits = fits and water(add(centre, scale(dir, t)), throat_reach + margin, throat_reach + margin) > throat_reach + margin
      end
      if fits then
        local depth = water(h, hub + 2.0 * hub_relief + 20.0) - (hub + 2.0 * hub_relief)
        choices[#choices + 1] = { dir = dir, exit = exit, narrow = narrow, hub = hub, relief = hub_relief, h = h, depth = depth,
                                  vest = vest, vest_relief = vest_relief, start = start }
      end
    end
  end
  set_state("tries", tries)
  set_state("pillars", 0)
  set_state("built", 0)
  set_state("rooms", 0)
  set_state("links", 0)
  if #choices == 0 then
    W.finish_shapes(w)
    return
  end
  -- ---------------------------------------------------------------------------------------- время
  local deposits = 0
  if recipe ~= 1 then deposits = math.tointeger(p.deposits_min + math.floor(rnd() * (p.deposits_extra + 1))) end
  local collect = pick(p.collect_min, p.collect_max)
  local budget = pick(p.time_min, p.time_max)
  local tour_budget = math.max(p.tour_min, (budget - deposits * collect) * p.swim_speed)
  -- Обход дерева туда и обратно — дважды длина ходов к целям; сверху тупики и ветки без целей.
  local target_share = pick(p.dead_end_share_min, p.dead_end_share_max)
  set_state("deposits", deposits)
  set_state("collect", collect)
  set_state("budget", budget)

  -- Пещере нужна порода: среди устьев, за которыми она толще всего (верхняя треть), — любое.
  table.sort(choices, function(a, b) return a.depth > b.depth end)
  local keep = math.max(1, math.ceil(#choices / 3))
  local function room(at, radius, purpose, flags)
    rooms[#rooms + 1] = { centre = at, radius = radius, purpose = purpose, flags = flags or 0 }
    return #rooms - 1
  end
  -- Попытка от одного устья: горло, первая ниша, дерево ходов. Всё, что она пишет, обнуляется в начале,
  -- поэтому неудачная попытка (скелет меньше трёх узлов) просто переписывается следующей.
  -- Сечение хода без вздутий (своя порода держится от него) и огибающая со всем рельефом (чужая вода).
  local function body(row)
    return math.max(core, row.base_width * row.biome_width, row.base_roof)
  end
  local function envelope(row)
    local wide = (row.base_width + math.max(row.first_width, row.second_width)) * row.biome_width
    return math.max(math.max(wide, row.base_roof + 2.5 * row.pulse + row.vertical_gain) + 1.2, core)
  end
  local mouth, dir, mouth_room, throat_a, throat_b, tree
  local function attempt(chosen)
  mouth, dir = chosen, chosen.dir
  rooms, links, parts = {}, {}, {}
  w.made = { galleries = 0, lobes = 0, turns = 0, pockets = 0 }
  room(centre, main_radius, "main_chamber")
  local mouth_point = add(centre, scale(dir, mouth.exit - 1.0))
  mouth_room = room(mouth_point, throat_core, "mouth", 1)
  links[#links + 1] = { a = 0, b = mouth_room, width = 0.0, roof = 0.0 }
  throat_a = add(centre, scale(dir, mouth.start))
  throat_b = add(centre, scale(dir, mouth.start + mouth.narrow))
  local entry_room = mouth_room
  if mouth.vest > 0.0 then
    local at = add(centre, scale(dir, mouth.exit - 0.5 * mouth.vest))
    entry_room = room(at, mouth.vest, "vestibule")
    links[#links + 1] = { a = mouth_room, b = entry_room, width = mouth.vest, roof = mouth.vest }
    W.write_turn(w, at, mouth.vest, rock, mouth.vest_relief)
    parts[#parts + 1] = { a = at, radius = mouth.vest + mouth.vest_relief }
  end
  local throat_row = W.gallery(seed, throat_key, add(centre, scale(dir, 0.7 * turn)), mouth.h,
                               { scale = throat_scale, core = throat_core, biome = biome, rock = rock, lobes = false })
  W.write_gallery(w, throat_row, throat_row.port_a, throat_row.port_b)
  parts[#parts + 1] = { a = throat_row.port_a, b = throat_row.port_b, radius = throat_reach }

  -- ---------------------------------------------------------------------------------------- дерево
  -- Узел скелета: комната, вход хода, глубина по пути от устья.
  tree = {}
  local hub_room = room(mouth.h, mouth.hub, "chamber")
  links[#links + 1] = { a = entry_room, b = hub_room, width = throat_row.base_width, roof = throat_row.base_roof }
  W.write_turn(w, mouth.h, mouth.hub, rock, mouth.relief)
  parts[#parts + 1] = { a = mouth.h, radius = mouth.hub + mouth.relief }
  tree[1] = { room = hub_room, at = mouth.h, radius = mouth.hub, relief = mouth.relief, chamber = true, dir = dir,
              depth = length(sub(mouth.h, mouth_point)), parent = nil, incident = { [1] = true, [#parts] = true },
              children = {} }
  -- Путь от устья до первой ниши обход проходит дважды сам по себе.
  local target = math.max(10.0, (0.5 * tour_budget - tree[1].depth) * target_share)
  local total = 0.0
  local queue = { 1 }
  local visits = {}
  while total < target do
    if #queue == 0 then
      -- Ходы кончились раньше бюджета: ещё по разу от узлов, где выходов было меньше всего.
      for i = 1, #tree do
        if (visits[i] or 0) < 3 then queue[#queue + 1] = i end
      end
      if #queue == 0 then break end
    end
    -- Чаще вширь (от ниш ближе к устью), иногда вглубь: пещеры разной формы.
    local at = rnd() < 0.6 and 1 or #queue
    local index = table.remove(queue, at)
    visits[index] = (visits[index] or 0) + 1
    local from = tree[index]
    local exits
    if index == 1 then exits = math.tointeger(p.hub_exits) + (rnd() < 0.5 and 1 or 0)
    elseif from.chamber then exits = math.tointeger(p.chamber_exits) + math.floor(rnd() * 3)
    else
      local r = rnd()
      exits = r < p.dead_end_chance and 0 or (r < p.dead_end_chance + p.branch_chance and 2 or 1)
    end
    local yaw0, pitch0 = heading(from.dir)
    for k = 0, exits - 1 do
      if total >= target then break end
      -- Варианты поворота: из прошедших проверки берётся тот, где порода вокруг толще всего — скелет
      -- сам уходит вглубь камня между галереями окна.
      local best
      for variant = 1, math.tointeger(p.segment_variants) do
        local spread = (k - 0.5 * (exits - 1)) * p.exit_spread * pi / 180.0
        local yaw = yaw0 + spread + (rnd() - 0.5) * p.turn_max * pi / 180.0
        local pitch = W.clamp(0.5 * pitch0 + (rnd() - 0.5) * 2.0 * p.pitch_max * pi / 180.0, -0.45 * pi, 0.45 * pi)
        local d = direction(yaw, pitch)
        local span = pick(p.segment_min, p.segment_max)
        local chamber = rnd() < p.chamber_chance
        local radius, relief = core, 0.0
        if chamber then
          if rnd() < p.big_chamber_chance then radius = pick(p.big_chamber_min, p.big_chamber_max)
          else radius = pick(p.chamber_min, p.chamber_max) end
          radius = math.max(radius, core + 0.5)
          relief = pick(p.chamber_relief_min, p.chamber_relief_max)
        end
        local passage = s * pick(1.0 - p.passage_scale_spread, 1.0 + p.passage_scale_spread)
        local lobes = index ~= 1 and rnd() < p.lobe_chance
        -- Из первой ниши — не назад в горло.
        local q = add(from.at, scale(d, from.radius + span))
        if (index ~= 1 or dot(d, dir) > -0.4) and length(sub(q, centre)) <= extent then
          -- У ходов первой ниши выступов нет: выступ у начала хода мог бы дотянуться до горла.
          local row = W.gallery(seed, base.mix(key, 0x4c4e4b, #links), from.at, q,
                                { scale = passage, core = core, biome = biome, rock = rock, lobes = lobes })
          local env = envelope(row)
          -- Чужая вода: вдоль хода через 2 м (кроме начала внутри ниши, откуда он растёт) и у новой ниши.
          local steps = math.max(1, math.ceil(length(sub(q, from.at)) / 2.0))
          local inside = from.radius + 2.0 * from.relief
          local score = 1e30
          for t = 1, steps do
            local x = add(from.at, scale(sub(q, from.at), t / steps))
            if length(sub(x, from.at)) > inside then score = math.min(score, water(x, env + 12.0, env + margin) - env) end
            if score <= margin then break end
          end
          if score > margin and chamber then
            local room_score = water(q, radius + 2.0 * relief + 12.0, radius + 2.0 * relief + margin) - (radius + 2.0 * relief)
            -- Нише нет места — конец хода остаётся коленом.
            if room_score <= margin then chamber, radius, relief = false, core, 0.0
            else score = math.min(score, room_score) end
          end
          local ball = radius + 2.0 * relief
          -- Своя пещера: у узла, откуда ход растёт, сходящиеся в нём части не мешают.
          if score > margin and (best == nil or score > best.score) then
            local own_r = body(row)
            local zone = from.radius + from.relief + 2.0 * own_r + margin
            local ok = true
            for t = 1, steps do
              if not ok then break end
              ok = own_clear(add(from.at, scale(sub(q, from.at), t / steps)), own_r + margin, from.at, zone, from.incident)
            end
            ok = ok and own_clear(q, math.max(radius + relief, own_r) + margin, from.at, zone, from.incident)
            if ok then
              best = { score = score, row = row, own_r = own_r, q = q, d = d, chamber = chamber, radius = radius, relief = relief }
            end
          end
        end
      end
      if best then
        local q = best.q
        W.write_gallery(w, best.row, best.row.port_a, best.row.port_b)
        if best.chamber then W.write_turn(w, q, best.radius, rock, best.relief) end
        parts[#parts + 1] = { a = from.at, b = q, radius = best.own_r }
        local segment = #parts
        from.incident[segment] = true
        local incident = { [segment] = true }
        if best.chamber then parts[#parts + 1] = { a = q, radius = best.radius + best.relief }; incident[#parts] = true end
        local r = room(q, best.radius, best.chamber and "chamber" or "bend")
        links[#links + 1] = { a = from.room, b = r, width = best.row.base_width, roof = best.row.base_roof }
        tree[#tree + 1] = { room = r, at = q, radius = best.chamber and best.radius or 0.0, relief = best.relief,
                            chamber = best.chamber, dir = best.d, depth = from.depth + length(sub(q, from.at)),
                            parent = index, incident = incident, children = {}, span = length(sub(q, from.at)) }
        from.children[#from.children + 1] = #tree
        queue[#queue + 1] = #tree
        total = total + length(sub(q, from.at))
      end
    end
  end

  end
  -- Первое устье — любое из верхней трети по толщине породы, дальше — лучшие по порядку.
  local first = 1 + math.floor(rnd() * keep) % keep
  local order = { first }
  for i = 1, #choices do if i ~= first then order[#order + 1] = i end end
  for k = 1, math.min(#order, 4) do
    attempt(choices[order[k]])
    if #tree >= 3 then break end
  end

  -- ---------------------------------------------------------------------------------------- цели
  local function path(i, j)
    -- Длина пути по дереву между узлами i и j.
    local up = {}
    local x, d = i, 0.0
    while x do up[x] = d; d = d + (tree[x].span or 0.0); x = tree[x].parent end
    x, d = j, 0.0
    while x and up[x] == nil do d = d + (tree[x].span or 0.0); x = tree[x].parent end
    return d + up[x]
  end

  -- ---------------------------------------------------------------------------------------- петли
  -- Ход между узлами дерева, близкими по прямой и далёкими по пути: обход сокращается, а ныряльщик
  -- теряет ориентир. Порода — как у обычных ходов (чужая вода, своя пещера вне ниш на концах).
  local loops = 0
  local function segment_clear(a, b, row, skip)
    local env, own_r = envelope(row), body(row)
    local steps = math.max(1, math.ceil(length(sub(b, a)) / 2.0))
    for t = 0, steps do
      local x = add(a, scale(sub(b, a), t / steps))
      if not skip(x, own_r) then
        if water(x, env + 12.0, env + margin) - env <= margin then return false end
        if not own_clear(x, own_r + margin, x, -1.0, {}) then return false end
      end
    end
    return true
  end
  local function near_niche(n, x, own_r)
    return length(sub(x, n.at)) <= n.radius + n.relief + 2.0 * own_r + margin
  end
  if rnd() < p.loop_chance then
    local candidates = {}
    for i = 2, #tree do
      for j = i + 1, #tree do
        local a, b = tree[i], tree[j]
        if a.parent ~= j and b.parent ~= i then
          local d = length(sub(a.at, b.at))
          if d >= p.loop_span_min and d <= p.loop_span_max then
            local along = path(i, j)
            if along >= p.loop_path_ratio * d and along >= p.loop_path_min then
              candidates[#candidates + 1] = { i = i, j = j, gain = along / d }
            end
          end
        end
      end
    end
    table.sort(candidates, function(x, y) return x.gain > y.gain end)
    for _, c in ipairs(candidates) do
      if loops >= p.loops_max then break end
      local a, b = tree[c.i], tree[c.j]
      local row = W.gallery(seed, base.mix(key, 0x4c4f4f50, c.i, c.j), a.at, b.at,
                            { scale = s, core = core, biome = biome, rock = rock, lobes = false })
      if segment_clear(a.at, b.at, row, function(x, own_r) return near_niche(a, x, own_r) or near_niche(b, x, own_r) end) then
        W.write_gallery(w, row, row.port_a, row.port_b)
        parts[#parts + 1] = { a = a.at, b = b.at, radius = body(row) }
        a.incident[#parts] = true
        b.incident[#parts] = true
        links[#links + 1] = { a = a.room, b = b.room, width = row.base_width, roof = row.base_roof }
        loops = loops + 1
      end
    end
  end

  -- ---------------------------------------------------------------------------------------- второй вход
  -- Узкий ход (как горло — лодка в него не входит) из узла дерева поглубже в ЗАВЕДОМУЮ воду шара поворота
  -- ближнего узла маршрута, в стороне от устья: ныряльщик может выйти не там, где вошёл.
  local extra_entrances = 0
  if rnd() < p.second_entrance_chance then
    local entrance_key = base.mix(key, 0x454e5452)
    local function entrance_shape(scale_now)
      local row = W.gallery(seed, entrance_key, { x = 0, y = 0, z = 0 }, { x = 1, y = 0, z = 0 },
                            { scale = scale_now, core = throat_core, biome = biome, rock = rock, lobes = false })
      local wide = (row.base_width + math.max(row.first_width, row.second_width)) * row.biome_width
      local tall = row.base_roof + 2.5 * row.pulse + row.vertical_gain
      return math.max(throat_core, math.min(wide, tall) + 1.2), math.max(wide, tall) + 1.2
    end
    local entrance_scale = s
    local e_inscribed, e_reach = entrance_shape(entrance_scale)
    for _ = 1, 8 do
      if e_inscribed < p.sub_beam / 2.0 then break end
      entrance_scale = entrance_scale * 0.8
      e_inscribed, e_reach = entrance_shape(entrance_scale)
    end
    local mouth_at0 = rooms[mouth_room + 1].centre
    -- Заведомая вода окна: шар поворота узла (его радиус) и капсула хода лодки вдоль ребра (sub_path_radius).
    -- Цель — на 2 м внутри; у цели вода окна своя, порода вдоль хода проверяется до её окрестности.
    local targets = {}
    for g = 0, node_count - 1 do
      local node = route[g]
      targets[#targets + 1] = { a = node.centre, radius = W.turn_radius(seed, node, p), own = node == own,
                                zone = W.turn_radius(seed, node, p) + 2.0 * W.turn_relief + e_reach + margin }
    end
    for _, e in ipairs(edges) do
      targets[#targets + 1] = { a = route[e.a].centre, b = route[e.b].centre, radius = p.sub_path_radius, own = false,
                                zone = p.sub_path_radius + 22.0 }
    end
    local best
    for i, n in ipairs(tree) do
      if i ~= 1 and n.depth >= p.second_entrance_depth then
        for _, goal in ipairs(targets) do
          local foot = goal.a
          if goal.b then
            local ab = sub(goal.b, goal.a)
            local t = W.clamp(dot(sub(n.at, goal.a), ab) / dot(ab, ab), 0.0, 1.0)
            foot = add(goal.a, scale(ab, t))
          end
          local away = sub(n.at, foot)
          local distance = length(away)
          if distance > goal.radius and distance - goal.radius <= p.second_entrance_max then
            local target = add(foot, scale(away, (goal.radius - 2.0) / distance))
            local span = length(sub(target, n.at))
            local separate = length(sub(target, mouth_at0)) >= 15.0 and
                             (not goal.own or dot(normalized(sub(target, centre)), dir) < math.cos(p.second_entrance_angle * pi / 180.0))
            if separate and span <= p.second_entrance_max and (best == nil or span < best.span) then
              local row = W.gallery(seed, entrance_key, n.at, target,
                                    { scale = entrance_scale, core = throat_core, biome = biome, rock = rock, lobes = false })
              local skip = function(x, own_r)
                local to_goal = goal.b and point_segment(x, goal.a, goal.b) or length(sub(x, goal.a))
                return near_niche(n, x, own_r) or to_goal <= goal.zone
              end
              if segment_clear(n.at, target, row, skip) then best = { i = i, target = target, row = row, span = span } end
            end
          end
        end
      end
    end
    if best then
      local n = tree[best.i]
      W.write_gallery(w, best.row, best.row.port_a, best.row.port_b)
      parts[#parts + 1] = { a = n.at, b = best.target, radius = body(best.row) }
      n.incident[#parts] = true
      local r = room(best.target, throat_core, "entrance", 1)
      links[#links + 1] = { a = n.room, b = r, width = best.row.base_width, roof = best.row.base_roof }
      extra_entrances = 1
    end
  end
  local purposes = {}
  local chosen = {}
  local function words(text)
    local out = {}
    for word in string.gmatch(text, "%S+") do out[#out + 1] = word end
    return out
  end
  local deepest = 1
  for i, n in ipairs(tree) do if n.depth > tree[deepest].depth then deepest = i end end
  -- Прикидка «гнездо не на виду»: глубже nest_depth по пути и дальше nest_distance от устья по прямой.
  local mouth_at = rooms[mouth_room + 1].centre
  local function hidden(n)
    return n.depth >= p.nest_depth and length(sub(n.at, mouth_at)) >= p.nest_distance
  end
  local function lair_candidate(lo, hi, want_hidden)
    local found, fallback
    for i, n in ipairs(tree) do
      if i ~= 1 and not purposes[i] and (not want_hidden or hidden(n)) then
        local f = n.depth / math.max(tree[deepest].depth, 1.0)
        if f >= lo and f <= hi then
          if n.chamber then found = found or i else fallback = fallback or i end
        end
      end
    end
    return found or fallback
  end
  local resource_nest
  local goals = {}
  -- Сколько залежей уложилось на самом деле (скелет мог выйти меньше задуманного).
  local placed = 0
  if recipe == 1 then
    -- Гнездо — самая глубокая ниша (если там колено — оно расширяется в нишу, места под неё хватает
    -- внутри огибающей хода не всегда, поэтому радиус ограничен ядром + 2).
    local nest = tree[deepest]
    purposes[deepest] = "nest"
    goals[#goals + 1] = deepest
    if not nest.chamber then
      nest.radius = core + 2.0
      nest.relief = 0.5
      nest.chamber = true
      W.write_turn(w, nest.at, nest.radius, rock, nest.relief)
      rooms[nest.room + 1].radius = nest.radius
    end
    -- Укрытия — в тупиках.
    local shelter_uses = words(p.shelter_uses)
    local shelters = math.tointeger(p.shelters_min + math.floor(rnd() * (p.shelters_extra + 1)))
    for i, n in ipairs(tree) do
      if shelters == 0 then break end
      if i ~= deepest and #n.children == 0 and i ~= 1 then
        purposes[i] = "shelter:" .. shelter_uses[1 + math.floor(rnd() * #shelter_uses) % #shelter_uses]
        shelters = shelters - 1
      end
    end
  else
    -- Гнездо (оно же логово) выбирается РАНЬШЕ залежей: иначе дальние узлы уходят под залежи, а гнездо
    -- остаётся у устья.
    resource_nest = lair_candidate(0.4, 0.85, true) or lair_candidate(0.0, 1.0, true)
    if resource_nest then purposes[resource_nest] = "nest" end
    -- Залежи — дальние друг от друга по пути (первая — самая глубокая), не обязательно в тупиках.
    local uses = words(recipe == 2 and p.thermal_uses or p.ore_uses)
    for _ = 1, deposits do
      local best, best_score = nil, -1.0
      for i, n in ipairs(tree) do
        if i ~= 1 and not purposes[i] then
          local score = n.depth
          for _, g in ipairs(goals) do score = math.min(score, path(i, g)) end
          if score > best_score then best, best_score = i, score end
        end
      end
      if best == nil then break end
      purposes[best] = "pocket:" .. uses[1 + math.floor(rnd() * #uses) % #uses]
      placed = placed + 1
      goals[#goals + 1] = best
      local n = tree[best]
      if not n.chamber then
        -- Залежь в колене: маленькая ниша, внутри огибающей хода.
        n.radius = core + pick(0.5, 1.5)
        n.relief = 0.4
        n.chamber = true
        W.write_turn(w, n.at, n.radius, rock, n.relief)
        rooms[n.room + 1].radius = n.radius
      end
      -- Будущая ледяная пробка — в колене перед залежью.
      local parent = n.parent
      if parent and parent ~= 1 and not tree[parent].chamber and not purposes[parent] and rnd() < p.plug_chance then
        purposes[parent] = "neck"
        rooms[tree[parent].room + 1].flags = 2
      end
    end
  end
  -- Логово: у гнездовой пещеры — ниша на 0.35..0.75 глубины гнезда (первая встреча); у остальных гнездо
  -- и логово одно — ниша за 0.4 глубины, не залежь.
  local nest_index, lair_index
  if recipe == 1 then
    nest_index = deepest
    lair_index = lair_candidate(0.35, 0.75) or lair_candidate(0.0, 1.0)
    if lair_index then purposes[lair_index] = "lair" end
  else
    nest_index = resource_nest or lair_candidate(0.4, 1.0) or lair_candidate(0.0, 1.0)
    if nest_index then purposes[nest_index] = "nest" end
    lair_index = nest_index
  end
  if nest_index == nil or lair_index == nil then
    -- Скелет из одной ниши: гнездо и логово в ней.
    nest_index = nest_index or 1
    lair_index = lair_index or nest_index
    purposes[nest_index] = purposes[nest_index] or "nest"
  end
  for i, n in ipairs(tree) do
    if not purposes[i] and i ~= 1 and #n.children == 0 then purposes[i] = "dead_end" end
    if purposes[i] then rooms[n.room + 1].purpose = purposes[i] end
  end

  -- ---------------------------------------------------------------------------------------- колонны
  -- В крупных нишах без назначения — вертикальные колонны (вид cave_pillar, роль вставки): в стороне от
  -- осей всех ходов (с раструбом у пола и свода, неровностью и запасом на ныряльщика) и друг от друга.
  local pillar_out = step.writes.pillars
  local pillar_count = 0
  for i, n in ipairs(tree) do
    if n.chamber and n.radius >= p.pillar_room_min and not purposes[i] and rnd() < p.pillar_chance then
      local want = 1 + math.floor(rnd() * p.pillar_max)
      local here = {}
      for _ = 1, 16 do
        if #here >= want or pillar_count >= pillar_out:count() then break end
        local angle = 2.0 * pi * rnd()
        local rho = pick(0.55, 0.85) * n.radius
        local radius = math.min(pick(p.pillar_radius_min, p.pillar_radius_max), 0.16 * n.radius)
        local c = { x = n.at.x + rho * math.cos(angle), y = n.at.y, z = n.at.z + rho * math.sin(angle) }
        local ok = true
        for _, other in ipairs(here) do
          ok = ok and math.sqrt((c.x - other.x) ^ 2 + (c.z - other.z) ^ 2) >= 1.9 * (radius + other.radius) + 2.0
        end
        for _, l in ipairs(links) do
          if not ok then break end
          local a, b = rooms[l.a + 1].centre, rooms[l.b + 1].centre
          -- Ствол на высоте dy (раструб к полу и своду, как в cave/field/01_pillar.ds) плюс неровность — не ближе
          -- ядра хода с запасом.
          for dy = -n.radius, n.radius, 1.0 do
            local t = math.min(math.abs(dy) / n.radius, 1.0)
            local need = radius * (1.0 + 0.9 * t * t) + 0.45 + core + 0.5
            if point_segment({ x = c.x, y = c.y + dy, z = c.z }, a, b) < need then ok = false break end
          end
        end
        if ok then
          here[#here + 1] = { x = c.x, z = c.z, radius = radius }
          W.set3(pillar_out:field("centre"), pillar_count, c)
          pillar_out:field("radius"):set(pillar_count, radius)
          pillar_out:field("span"):set(pillar_count, n.radius)
          pillar_out:field("half"):set(pillar_count, n.radius + 2.0 * n.relief + 1.5)
          pillar_out:field("rock"):set(pillar_count, rock)
          pillar_count = pillar_count + 1
        end
      end
    end
  end
  set_state("pillars", pillar_count)

  -- Обход целей от устья и обратно: дважды длина объединения путей к ним (дерево).
  local used = {}
  for _, g in ipairs(goals) do
    local x = g
    while x and not used[x] do used[x] = true; x = tree[x].parent end
  end
  local tour = 2.0 * tree[1].depth
  for x in pairs(used) do tour = tour + 2.0 * (tree[x].span or 0.0) end
  set_state("tour", tour)
  set_state("deposits", placed)
  set_state("estimate", placed * collect + tour / p.swim_speed)

  -- ---------------------------------------------------------------------------------------- выход
  local word_index, wi = {}, 0
  for word in string.gmatch(p.purposes, "%S+") do word_index[word] = wi; wi = wi + 1 end
  local r_out = step.writes.rooms
  if #rooms > r_out:count() then error("cave: room capacity " .. r_out:count() .. " exceeded") end
  local rf = {}
  for _, name in ipairs({ "centre", "radius", "squash", "stretch", "yaw", "purpose", "flags" }) do rf[name] = r_out:field(name) end
  for i, r in ipairs(rooms) do
    local k = i - 1
    W.set3(rf.centre, k, r.centre)
    rf.radius:set(k, r.radius); rf.squash:set(k, 1.0); rf.stretch:set(k, 1.0); rf.yaw:set(k, 0.0)
    local code = word_index[r.purpose]
    if code == nil then error("cave: purpose '" .. r.purpose .. "' is not in the vocabulary") end
    rf.purpose:set(k, code); rf.flags:set(k, r.flags)
  end
  local l_out = step.writes.links
  if #links > l_out:count() then error("cave: link capacity " .. l_out:count() .. " exceeded") end
  for i, l in ipairs(links) do
    local k = i - 1
    l_out:field("a"):set(k, l.a); l_out:field("b"):set(k, l.b)
    l_out:field("width"):set(k, l.width); l_out:field("roof"):set(k, l.roof)
  end
  W.finish_shapes(w)
  set_state("built", 1)
  set_state("rooms", #rooms)
  set_state("links", #links)
  set_state("mouth", mouth_room)
  set_state("nest", tree[nest_index].room)
  set_state("lair", tree[lair_index].room)
  set_state("lair_kind", tree[lair_index].chamber and 0 or 2)
  W.set3(state:field("throat_a"), 0, throat_a)
  W.set3(state:field("throat_b"), 0, throat_b)
  set_state("throat_core", throat_core)
  set_state("blocks_sub", inscribed < p.sub_beam / 2.0 and 1 or 0)
end
