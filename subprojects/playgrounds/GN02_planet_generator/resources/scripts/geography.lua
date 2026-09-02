-- Тело шага geography: иерархия названных мест.
--
-- ЗАЧЕМ ЭТОТ ШАГ ВООБЩЕ ЕСТЬ. У готовой планеты есть провинции и морские зоны, но у неё нет
-- НАЗВАННЫХ МЕСТ: нельзя сказать «этот берег» или «эта область», потому что «область» — это не
-- значение поля, а множество провинций. Игровой слой без такой иерархии не собирается: и надпись на
-- поверхности, и выделение мышью, и политический титул спрашивают одно и то же — «какое место я
-- сейчас имею в виду», а место бывает разного масштаба.
--
-- Иерархия по воде:  океан -> океаническая область (морская зона).
-- Иерархия по суше:  земляной массив -> материк -> историческая область -> провинция.
--
-- ГЛАВНОЕ СВОЙСТВО: ВСЕ ГРАНИЦЫ СОВПАДАЮТ С ГРАНИЦАМИ ПРОВИНЦИЙ, и совпадают они ПО ПОСТРОЕНИЮ, а
-- не по счастливой случайности. Достигается это тем, что уровень растится не по клеткам, а по ГРАФУ
-- СОСЕДСТВА ПРОВИНЦИЙ: узел графа — провинция целиком, поэтому граница уровня физически не может
-- пройти внутри провинции. Если бы уровень заливался по клеткам, его граница резала бы провинции на
-- части, и тогда надпись «Северная Европа» и выделение провинции начали бы показывать разное.
--
-- Отсюда же порядок: шаг идёт ПОСЛЕ regions, потому что он их аггрегирует, а не нарезает заново.
--
-- Про имена. Буферы генератора числовые, строк в них нет и быть не должно, поэтому название лежит
-- ЗАТРАВКОЙ (name_seed) и наименующей культурой. Синтез названия из затравки детерминирован, значит
-- название в пакете есть — просто в свёрнутом виде, как и всё остальное в этом генераторе.

-- Детерминированный хеш вместо math.random: генератора случайности в окружении нет намеренно, а
-- зерно приходит от шага. Тот же, что в tectonics: одна и та же пара (зерно, номер) обязана давать
-- одно и то же значение при любом порядке вызовов.
local function mix(a, b)
  local ia = math.tointeger(a) or 0
  local ib = math.tointeger(b) or 0
  local x = (ia * 0x9E3779B97F4A7C15) ~ (ib * 0xBF58476D1CE4E5B9)
  x = x ~ (x >> 30)
  x = x * 0xBF58476D1CE4E5B9
  x = x ~ (x >> 27)
  x = x * 0x94D049BB133111EB
  return x ~ (x >> 31)
end

return function(step)
  local cells = step.writes.cells
  local provinces = step.writes.provinces
  local sea_zones = step.writes.sea_zones
  local state = step.writes.state
  local p = step.params

  local offsets = step.reads.cell_offsets:field("start")
  local arcs = step.reads.cell_arcs:field("cell")
  local province_starts = step.reads.province_offsets:field("start")
  local province_order = step.reads.province_order:field("cell")
  local sea_starts = step.reads.sea_offsets:field("start")
  local sea_order = step.reads.sea_order:field("cell")

  local land = cells:field("land")
  local province = cells:field("province")
  local sea_zone = cells:field("sea_zone")

  local seed = step.seed
  local count = cells:count()

  local province_count = math.tointeger(state:field("province_count"):get(0)) or 0
  local sea_zone_count = math.tointeger(state:field("sea_zone_count"):get(0)) or 0

  -- ------------------------------------------------------------------ графы соседства областей
  --
  -- Ровно то, чего в движке не было и без чего задача не решается. Строка метки лежит по индексу,
  -- равному метке; строка 0 пуста, потому что метка 0 означает «нет области».
  local pn_offsets = step.writes.province_neighbour_offsets:field("start")
  local pn_arcs = step.writes.province_neighbour_arcs:field("province")
  local sn_offsets = step.writes.sea_neighbour_offsets:field("start")
  local sn_arcs = step.writes.sea_neighbour_arcs:field("zone")

  originator.label_adjacency{ inputs = { province, offsets, arcs }, outputs = { pn_offsets, pn_arcs } }
  originator.label_adjacency{ inputs = { sea_zone, offsets, arcs }, outputs = { sn_offsets, sn_arcs } }

  -- ------------------------------------------------------------------ земляные массивы
  --
  -- Земляной массив — это СВЯЗНЫЙ КУСОК суши, а не разбиение: Евразия и Америка это два разных
  -- куска, и узнать это можно только обходом графа. Поэтому здесь связные компоненты, а не заливка
  -- от затравок: у верхнего уровня иерархии нет свободы, он определён связностью.
  local cell_land_mass = cells:field("land_mass")
  originator.connected_components{
    inputs = { offsets, arcs, land },
    outputs = { cell_land_mass },
    params = { min_size = 1 },
  }

  -- ------------------------------------------------------------------ рост уровня по графу областей
  local function node_first_cell(starts, order, label)
    local first = math.tointeger(starts:get(label))
    local last = math.tointeger(starts:get(label + 1))
    if last <= first then
      return nil
    end
    return math.tointeger(order:get(first))
  end

  -- Добор: каждая непокрытая связная группа получает свою область. Такие группы существуют не от
  -- ошибки, а по построению — группу провинций, отрезанную водой от всех затравок, ни заливка, ни
  -- голосование не достанут, и правильный ответ «дать ей свою область», а не тянуть к ней границу
  -- через море. Отдельный остров и есть отдельная область, а держава на нём — независимая держава.
  --
  -- Обход идёт в lua по тому же CSR, а не отдельным инструментом: узлов здесь тысячи, а не сотни
  -- тысяч, и заводить ради этого проход нативным инструментом незачем.
  local function label_leftover_groups(csr_offsets, csr_arcs, label_field, mask_field, members, placed)
    local stack = {}
    for _, node in ipairs(members) do
      if mask_field:get(node) > 0.5 and (math.tointeger(label_field:get(node)) or 0) == 0 then
        placed = placed + 1
        label_field:set(node, placed)
        stack[1] = node
        local top = 1
        while top > 0 do
          local current = stack[top]
          top = top - 1
          local first = math.tointeger(csr_offsets:get(current))
          local last = math.tointeger(csr_offsets:get(current + 1))
          for k = first, last - 1 do
            local other = math.tointeger(csr_arcs:get(k))
            if mask_field:get(other) > 0.5 and (math.tointeger(label_field:get(other)) or 0) == 0 then
              label_field:set(other, placed)
              top = top + 1
              stack[top] = other
            end
          end
        end
      end
    end
    return placed
  end

  -- Рост уровня: затравки по площади под маской, заливка по графу областей, добор непокрытых групп.
  --
  -- Расстояние между затравками считается ЗДЕСЬ, а не выводится инструментом, и по той же причине,
  -- что у провинций: инструмент выводит его из доли КАНДИДАТОВ во всём диапазоне, а диапазон здесь
  -- это ЁМКОСТЬ буфера областей, а не их число. Доля вышла бы вчетверо меньше настоящей, и затравок
  -- стало бы вдвое больше, чем просили.
  --
  -- Добор идёт в lua и обходом того же CSR, а не отдельным инструментом: непокрытая группа — это
  -- связный кусок графа областей, узлов в нём тысячи, а не сотни тысяч, и заводить ради этого ещё
  -- один проход нативным инструментом незачем.
  local function grow_level(csr_offsets, csr_arcs, centers, sizes, seed_field, label_field, distance_field,
                            mask_field, members, wanted, node_limit)
    -- Обход идёт по СПИСКУ УЗЛОВ уровня, а не по всему буферу: список приходит готовым, а буфер
    -- областей — это ёмкость, и она вчетверо больше числа областей. Пока обход шёл по ёмкости на
    -- каждого родителя, шаг был квадратичным: 8.7 с из 33 на планету, то есть самый дорогой шаг
    -- генератора при том, что настоящей работы в нём меньше всех.
    local covered = 0.0
    for _, node in ipairs(members) do
      covered = covered + sizes:get(node)
    end
    if #members == 0 then
      return 0
    end

    local share = covered / count
    local distance = 0.92 * math.sqrt(4.0 * math.pi * share / math.max(1, wanted))

    originator.poisson_seeds{
      inputs = { centers, mask_field, sizes },
      outputs = { seed_field },
      params = { min_distance = distance },
    }

    -- Диапазон задан явно: у буфера записей областей на один элемент больше, чем строк у CSR
    -- соседства, и заливка обязана идти по строкам CSR, иначе последняя строка прочитала бы
    -- смещение за концом буфера.
    originator.graph_flood{
      inputs = { csr_offsets, csr_arcs, seed_field, sizes, mask_field },
      outputs = { label_field, distance_field },
      params = { unreached = -1 },
      range = { 0, node_limit },
    }

    -- Нумерация добора продолжает нумерацию затравок, а не начинается заново: иначе две разных
    -- области получили бы один номер.
    local placed = 0
    for _, node in ipairs(members) do
      local label = math.tointeger(label_field:get(node)) or 0
      if label > placed then
        placed = label
      end
    end
    return label_leftover_groups(csr_offsets, csr_arcs, label_field, mask_field, members, placed)
  end

  -- Уровень растится ВНУТРИ КАЖДОГО РОДИТЕЛЯ ОТДЕЛЬНО, а не по всему множеству сразу, и это не
  -- оптимизация, а исправление настоящей ошибки. Заливка по графу областей не переходит через воду,
  -- поэтому при росте по всему множеству сразу материк, до которого не дошла ни одна затравка,
  -- доставался бы добору целиком — и число материков определялось бы не размером материка, а тем,
  -- сколько кусков суши есть на планете. Измерено: 3 затравки океанов накрыли 57 морских зон из 94,
  -- остаток дал 35 «океанов», каждый в один замкнутый водоём.
  --
  -- Второе следствие того же: область уровня обязана лежать в ОДНОМ родителе, а внутри родителя дуги
  -- есть везде, поэтому без маски историческая область перелезла бы в соседний материк.
  local function grow_by_parent(csr_offsets, csr_arcs, centers, sizes, seed_field, label_field,
                                distance_field, mask_field, node_count, node_limit, parent_of,
                                parent_count, per_area, out_field)
    -- Раскладка узлов по родителям считается ОДИН раз. Родителей бывают сотни, а буфер областей —
    -- это ёмкость: обход ёмкости на каждого родителя делал шаг квадратичным.
    local members_of = {}
    for node = 1, node_count do
      if sizes:get(node) > 0 then
        local parent = parent_of(node)
        if parent >= 1 then
          local list = members_of[parent]
          if list == nil then
            list = {}
            members_of[parent] = list
          end
          list[#list + 1] = node
        end
      end
    end

    -- Маска обнуляется один раз на весь буфер, а дальше сбрасываются только те узлы, которые её
    -- ставили: иначе обнуление и было бы тем самым квадратом.
    for node = 0, node_limit do
      mask_field:set(node, 0)
    end

    local total = 0
    for parent = 1, parent_count do
      local members = members_of[parent]
      if members ~= nil and #members > 0 then
        for _, node in ipairs(members) do
          mask_field:set(node, 1)
        end
        local wanted = math.max(1, math.floor(#members / math.max(1, per_area) + 0.5))
        local grown = grow_level(csr_offsets, csr_arcs, centers, sizes, seed_field, label_field,
                                 distance_field, mask_field, members, wanted, node_limit)
        for _, node in ipairs(members) do
          out_field:set(node, total + (math.tointeger(label_field:get(node)) or 0))
          mask_field:set(node, 0)
        end
        total = total + grown
      end
    end
    return total
  end

  -- ------------------------------------------------------------------ уровни суши
  local pr_center = provinces:field("center")
  local pr_cells = provinces:field("cells")
  local pr_culture = provinces:field("culture")
  local pr_land_mass = provinces:field("land_mass")
  local pr_continent = provinces:field("continent")
  local pr_region = provinces:field("historical_region")
  local pr_seed = provinces:field("region_seed")
  local pr_label = provinces:field("region_label")
  local pr_distance = provinces:field("region_distance")
  local pr_mask = provinces:field("region_mask")

  local province_limit = provinces:count() - 1
  local sea_limit = sea_zones:count() - 1

  -- Массив провинции — это массив любой её клетки: провинция связна по суше, поэтому все её клетки
  -- лежат в одном куске. Берётся первая клетка из раскладки group_by — обхода по клеткам не нужно.
  local land_mass_count = 0
  local mass_provinces = {}
  for label = 1, province_count do
    local cell = node_first_cell(province_starts, province_order, label)
    local mass = cell and (math.tointeger(cell_land_mass:get(cell)) or 0) or 0
    pr_land_mass:set(label, mass)
    if mass > land_mass_count then
      land_mass_count = mass
    end
    if mass >= 1 then
      mass_provinces[mass] = (mass_provinces[mass] or 0) + 1
    end
  end

  -- МЕЛКИЕ МАССИВЫ НЕ ПОЛУЧАЮТ СВОЙ МАТЕРИК, а прикрепляются к ближайшему, и это география, а не
  -- удобство: Япония — это остров, но материк у неё Азия. Иначе на планете с сотней островов вышло
  -- бы сто с лишним «материков», из которых сто — по одному острову, и уровень перестал бы что-либо
  -- означать. Порог задан в провинциях, потому что в них задан и размер материка.
  local major = {}
  local major_count = 0
  for mass = 1, land_mass_count do
    if (mass_provinces[mass] or 0) >= p.continent_min_provinces then
      major[mass] = true
      major_count = major_count + 1
    end
  end
  -- Если крупных массивов не нашлось вовсе (крошечная планета), крупным считается самый большой:
  -- уровень обязан существовать, иначе прикреплять будет некуда.
  if major_count == 0 then
    local best, best_size = 0, -1
    for mass = 1, land_mass_count do
      if (mass_provinces[mass] or 0) > best_size then
        best_size = mass_provinces[mass] or 0
        best = mass
      end
    end
    if best ~= 0 then
      major[best] = true
      major_count = 1
    end
  end

  local continent_count = grow_by_parent(pn_offsets, pn_arcs, pr_center, pr_cells, pr_seed, pr_label,
                                         pr_distance, pr_mask, province_count, province_limit,
                                         function(node)
                                           local mass = math.tointeger(pr_land_mass:get(node)) or 0
                                           return major[mass] and mass or -1
                                         end,
                                         land_mass_count, p.continent_provinces, pr_continent)

  -- Прикрепление: провинции, оставшиеся без области уровня, уходят в ту, чей центр к ним ближе
  -- всего. Ближе по УГЛУ, а не по прямой в пространстве: на замкнутой поверхности это одно и то же с
  -- точностью до монотонности, а косинус считается втрое дешевле.
  --
  -- Нужно двум уровням сразу — материку и империи, — и по одной причине: и тот и другой обязаны
  -- ЗАБРАТЬ остров, а не отдать ему свой ранг. Империи это касается даже сильнее: измерено 77 империй
  -- на 862 графства при заказанной сотне графств на империю, и вся разница — острова, каждый из
  -- которых объявил себя империей.
  local function attach_leftovers(out_field, level_count)
    local center = {}
    for node = 1, province_count do
      local index = math.tointeger(out_field:get(node)) or 0
      if index >= 1 then
        local slot = center[index] or { x = 0.0, y = 0.0, z = 0.0 }
        local weight = pr_cells:get(node)
        slot.x = slot.x + pr_center:get(node, 0) * weight
        slot.y = slot.y + pr_center:get(node, 1) * weight
        slot.z = slot.z + pr_center:get(node, 2) * weight
        center[index] = slot
      end
    end
    for index = 1, level_count do
      local slot = center[index]
      if slot ~= nil then
        local length = math.sqrt(slot.x * slot.x + slot.y * slot.y + slot.z * slot.z)
        if length > 0.0 then
          slot.x, slot.y, slot.z = slot.x / length, slot.y / length, slot.z / length
        else
          center[index] = nil
        end
      end
    end

    local joined = 0
    for node = 1, province_count do
      if (math.tointeger(out_field:get(node)) or 0) == 0 and pr_cells:get(node) > 0 then
        local x, y, z = pr_center:get(node, 0), pr_center:get(node, 1), pr_center:get(node, 2)
        local best, best_dot = 0, -2.0
        for index = 1, level_count do
          local slot = center[index]
          if slot ~= nil then
            local dot = x * slot.x + y * slot.y + z * slot.z
            if dot > best_dot then
              best_dot = dot
              best = index
            end
          end
        end
        if best ~= 0 then
          out_field:set(node, best)
          joined = joined + 1
        end
      end
    end
    return joined
  end

  local attached = attach_leftovers(pr_continent, continent_count)

  -- Исторические области: внутри материка. Прикреплённый остров графом с материком не связан, поэтому
  -- заливка до него не дойдёт и добор отдаст ему СВОЮ область внутри того же материка — что и
  -- требуется: Япония это отдельная историческая область внутри Восточной Азии.
  local historical_count = grow_by_parent(pn_offsets, pn_arcs, pr_center, pr_cells, pr_seed, pr_label,
                                          pr_distance, pr_mask, province_count, province_limit,
                                          function(node) return math.tointeger(pr_continent:get(node)) or 0 end,
                                          continent_count, p.historical_provinces, pr_region)

  -- ------------------------------------------------------------------ океаны
  --
  -- Водный массив — то же, что земляной: связный кусок воды. Замкнутое море и озеро это ОТДЕЛЬНЫЕ
  -- массивы, и океан внутри них ровно один, сколько бы океанических областей в них ни оказалось.
  local sz_center = sea_zones:field("center")
  local sz_cells = sea_zones:field("cells")
  local sz_ocean = sea_zones:field("ocean")
  local sz_body = sea_zones:field("water_body")
  local sz_seed = sea_zones:field("region_seed")
  local sz_label = sea_zones:field("region_label")
  local sz_distance = sea_zones:field("region_distance")
  local sz_mask = sea_zones:field("region_mask")

  local cell_ocean = cells:field("ocean")
  originator.connected_components{
    inputs = { offsets, arcs, cells:field("ocean_seed") },
    outputs = { cell_ocean },
    params = { min_size = 1 },
  }

  local water_body_count = 0
  for zone = 1, sea_zone_count do
    local cell = node_first_cell(sea_starts, sea_order, zone)
    local body = cell and (math.tointeger(cell_ocean:get(cell)) or 0) or 0
    sz_body:set(zone, body)
    if body > water_body_count then
      water_body_count = body
    end
  end

  -- ОКЕАН — ЭТО ОТКРЫТАЯ ВОДА, А ОЗЕРО — НЕ ОКЕАН. Порог по числу областей в массиве, и без него
  -- уровень вырождается: измерено 184 замкнутых водоёма из 640 морских областей, то есть 210
  -- «океанов», из которых 184 — по одному озеру. Озеро при этом никуда не пропадает: у него есть
  -- своя океаническая область, просто океана у неё нет, и это верно по смыслу — у озера родитель на
  -- суше, а не в океане.
  local body_zones = {}
  for zone = 1, sea_zone_count do
    local body = math.tointeger(sz_body:get(zone)) or 0
    if body >= 1 and sz_cells:get(zone) > 0 then
      body_zones[body] = (body_zones[body] or 0) + 1
    end
  end
  local lakes = 0
  for body = 1, water_body_count do
    if (body_zones[body] or 0) > 0 and (body_zones[body] or 0) < p.ocean_min_zones then
      lakes = lakes + 1
    end
  end

  local ocean_count = grow_by_parent(sn_offsets, sn_arcs, sz_center, sz_cells, sz_seed, sz_label,
                                     sz_distance, sz_mask, sea_zone_count, sea_limit,
                                     function(node)
                                       local body = math.tointeger(sz_body:get(node)) or 0
                                       return (body_zones[body] or 0) >= p.ocean_min_zones and body or -1
                                     end,
                                     water_body_count, p.ocean_zones, sz_ocean)

  -- ------------------------------------------------------------------ титулы де-юре
  --
  -- ПОЧЕМУ ЭТО ЗДЕСЬ, А НЕ ОТДЕЛЬНЫМ ШАГОМ. Политическая иерархия де-юре — это НЕ второе разбиение
  -- поверх географического, а то же самое множество провинций, нарезанное в других масштабах: де-юре
  -- королевство и есть исторический край. Ровно это и означает «из географии вытекает политика».
  -- Машинерия та же (рост по графу провинций внутри родителя), граф тот же, и заводить ради другого
  -- названия шага второй проход по тому же графу было бы платой ни за что.
  --
  -- Королевство отдельным уровнем не растится вовсе: это историческая область, и своё разбиение того
  -- же масштаба было бы её копией с другими границами.
  local pr_duchy = provinces:field("duchy")
  local pr_empire = provinces:field("empire")

  local duchy_count = grow_by_parent(pn_offsets, pn_arcs, pr_center, pr_cells, pr_seed, pr_label,
                                     pr_distance, pr_mask, province_count, province_limit,
                                     function(node) return math.tointeger(pr_region:get(node)) or 0 end,
                                     historical_count, p.duchy_provinces, pr_duchy)

  -- Империя, как и материк, растится только по КРУПНЫМ массивам, а острова к ней прикрепляются.
  -- Иначе каждый прикреплённый остров, не связанный с материком по графу, достаётся добору и
  -- объявляет себя отдельной империей.
  local empire_count = grow_by_parent(pn_offsets, pn_arcs, pr_center, pr_cells, pr_seed, pr_label,
                                      pr_distance, pr_mask, province_count, province_limit,
                                      function(node)
                                        local mass = math.tointeger(pr_land_mass:get(node)) or 0
                                        if not major[mass] then
                                          return -1
                                        end
                                        return math.tointeger(pr_continent:get(node)) or 0
                                      end,
                                      continent_count, p.empire_provinces, pr_empire)
  attach_leftovers(pr_empire, empire_count)

  -- ------------------------------------------------------------------ державы де-факто
  --
  -- Другой процесс, а не другой масштаб, и поэтому другой инструмент. Титул — это ПРАВО, и площадь
  -- под ним делится ровно; держава — это СИЛА, и границы её рисует то, кто кого пересилил. Поэтому
  -- здесь голосование по населению, а не заливка по площади: у заливки все области выходят одного
  -- размера, а у державы размер и есть главное, что о ней известно.
  local pr_realm = provinces:field("realm")
  local pr_realm_next = provinces:field("realm_next")
  local pr_population = provinces:field("population_sum")

  for node = 0, province_limit do
    pr_mask:set(node, node >= 1 and node <= province_count and pr_cells:get(node) > 0 and 1 or 0)
  end

  local wanted_realms = math.max(1, math.floor(province_count / math.max(1, p.realm_provinces) + 0.5))
  local realm_share = 0.0
  for node = 1, province_count do
    if pr_mask:get(node) > 0.5 then
      realm_share = realm_share + pr_cells:get(node)
    end
  end
  originator.poisson_seeds{
    inputs = { pr_center, pr_mask, pr_population },
    outputs = { pr_realm },
    params = { min_distance = 0.92 * math.sqrt(4.0 * math.pi * (realm_share / count) / wanted_realms) },
  }

  -- Проходов столько, сколько нужно, а не сколько назначено: рост останавливается сам, когда
  -- незанятых провинций не осталось. Предел стоит на случай изолированной провинции, до которой не
  -- дотянется никто — тогда цикл обязан кончиться, а не крутиться до конца времён.
  for _ = 1, 512 do
    originator.graph_vote{
      inputs = { pn_offsets, pn_arcs, pr_realm, pr_population, pr_mask },
      outputs = { pr_realm_next },
      range = { 0, province_limit },
    }
    local unclaimed, changed = 0, 0
    for node = 1, province_count do
      local before = math.tointeger(pr_realm:get(node)) or 0
      local after = math.tointeger(pr_realm_next:get(node)) or 0
      if after ~= before then
        changed = changed + 1
      end
      if pr_mask:get(node) > 0.5 and after == 0 then
        unclaimed = unclaimed + 1
      end
    end
    originator.blend{
      inputs = { pr_realm_next, pr_realm_next },
      outputs = { pr_realm },
      params = { first = 1.0, second = 0.0 },
    }
    if changed == 0 or unclaimed == 0 then
      break
    end
  end
  -- Провинции, до которых голосование не дотянулось: остров без соседей по графу переголосовать
  -- некому. Измерено 43 таких графства из 450 — каждое получает свою державу, и это верно по смыслу:
  -- остров, который никто не завоевал, независим.
  local realm_members = {}
  for node = 1, province_count do
    if pr_mask:get(node) > 0.5 then
      realm_members[#realm_members + 1] = node
    end
  end
  local realm_count = label_leftover_groups(pn_offsets, pn_arcs, pr_realm, pr_mask, realm_members,
                                            math.tointeger(originator.reduce_max{ inputs = { pr_realm } }) or 0)

  -- ------------------------------------------------------------------ баронства
  --
  -- Баронство — это ТОЧКА, а не область: владение стоит в клетке. Поэтому здесь нет ни роста, ни
  -- заливки — есть выбор клеток внутри графства по пригодности, и вид владения по тому, что за
  -- клетка. Число зависит от пригодности земли: на голом камне поселений меньше, чем в долине.
  local baronies = step.writes.baronies
  baronies:clear()
  local barony_cell = baronies:field("cell")
  local barony_province = baronies:field("province")
  local barony_name = baronies:field("name_seed")
  local barony_kind = baronies:field("kind")
  local pr_baronies = provinces:field("baronies")
  local pr_habitability = provinces:field("habitability_sum")

  local habitability = cells:field("habitability")
  local ocean_distance = cells:field("ocean_distance")
  local barony_capacity = baronies:count()
  local barony_count = 0

  for label = 1, province_count do
    local first = math.tointeger(province_starts:get(label))
    local last = math.tointeger(province_starts:get(label + 1))
    local size = last - first
    if size > 0 then
      -- Клетки графства по убыванию пригодности: владение ставится там, где живут. Сортировка идёт
      -- по КОПИИ списка, потому что раскладка group_by — общие данные, а не черновик шага.
      local members = {}
      for k = first, last - 1 do
        members[#members + 1] = math.tointeger(province_order:get(k))
      end
      table.sort(members, function(a, b)
        local ha, hb = habitability:get(a), habitability:get(b)
        if ha ~= hb then
          return ha > hb
        end
        return a < b
      end)

      -- Сколько владений: среднее из конфига, помноженное на пригодность графства относительно
      -- средней клетки. Не меньше одного — графство без владения не держит никто.
      local mean_habitability = size == 0 and 0.0 or pr_habitability:get(label) / size
      local wanted = math.max(1, math.min(size, math.floor(p.baronies_per_province * (0.35 + 1.30 * mean_habitability) + 0.5)))

      local placed = 0
      for index = 1, #members do
        if placed >= wanted or barony_count >= barony_capacity then
          break
        end
        local cell = members[index]
        placed = placed + 1
        barony_count = barony_count + 1
        local slot = barony_count
        barony_cell:set(slot, cell)
        barony_province:set(slot, label)
        barony_name:set(slot, mix(seed, 5 * 1048576 + slot) & 0x7FFFFFFF)
        -- Первое владение графства — замок (это его столица), одно — храм, остальные города; город
        -- на берегу остаётся городом, а храм в глубине не мешает никому.
        local kind = 1
        if placed == 1 then
          kind = 0
        elseif placed == 2 then
          kind = 2
        elseif ocean_distance:get(cell) <= 1.5 then
          kind = 1
        end
        barony_kind:set(slot, kind)
      end
      pr_baronies:set(label, placed)
    end
  end

  -- ------------------------------------------------------------------ клетка узнаёт свои уровни
  --
  -- Через выборку по номеру области, а не заливкой: именно здесь и обеспечивается совпадение границ.
  -- Клетка воды получает ноль на суше и наоборот, потому что строка 0 таблицы областей пуста.
  originator.lookup{ inputs = { province, pr_land_mass }, outputs = { cell_land_mass } }
  originator.lookup{ inputs = { province, pr_continent }, outputs = { cells:field("continent") } }
  originator.lookup{ inputs = { province, pr_region }, outputs = { cells:field("historical_region") } }
  originator.lookup{ inputs = { province, pr_duchy }, outputs = { cells:field("duchy") } }
  originator.lookup{ inputs = { province, pr_empire }, outputs = { cells:field("empire") } }
  originator.lookup{ inputs = { province, pr_realm }, outputs = { cells:field("realm") } }
  originator.lookup{ inputs = { sea_zone, sz_ocean }, outputs = { cells:field("ocean") } }

  -- ------------------------------------------------------------------ записи уровней
  --
  -- Центр области — средневзвешенное НАПРАВЛЕНИЕ её провинций, а не среднее координат: на замкнутой
  -- поверхности среднее координат уезжает внутрь шара, и у области, лежащей по обе стороны от
  -- меридиана, оказывается в центре планеты. Взвешивание по числу клеток, потому что подпись
  -- обязана стоять там, где области больше, а не там, где больше мелких провинций.
  local function summarise(records, member_count, member_of, member_cells, member_centers, member_culture,
                           members_name, seed_name, level_tag, level_count)
    local accumulated = {}
    for index = 1, level_count do
      accumulated[index] = { x = 0.0, y = 0.0, z = 0.0, cells = 0, members = 0, first = 0 }
    end

    for node = 1, member_count do
      local index = math.tointeger(member_of:get(node)) or 0
      if index >= 1 and index <= level_count then
        local slot = accumulated[index]
        local weight = member_cells:get(node)
        slot.x = slot.x + member_centers:get(node, 0) * weight
        slot.y = slot.y + member_centers:get(node, 1) * weight
        slot.z = slot.z + member_centers:get(node, 2) * weight
        slot.cells = slot.cells + weight
        slot.members = slot.members + 1
        if slot.first == 0 then
          slot.first = node
        end
      end
    end

    local center = records:field("center")
    local cells_field = records:field("cells")
    local members_field = records:field(members_name)
    local seed_field = records:field(seed_name)
    local name_field = records:field("name_seed")
    local culture_field = records:field("culture")

    for index = 1, level_count do
      local slot = accumulated[index]
      local length = math.sqrt(slot.x * slot.x + slot.y * slot.y + slot.z * slot.z)
      if length > 0.0 then
        center:set(index, slot.x / length, 0)
        center:set(index, slot.y / length, 1)
        center:set(index, slot.z / length, 2)
      end
      cells_field:set(index, slot.cells)
      members_field:set(index, slot.members)
      seed_field:set(index, slot.first)
      -- Затравка имени зависит от зерна мира, вида уровня и номера области. Вид уровня входит в
      -- ключ обязательно: иначе первый материк и первый океан получили бы одно и то же имя.
      name_field:set(index, mix(seed, level_tag * 1048576 + index) & 0x7FFFFFFF)
      -- Наименующая культура — культура самой крупной провинции области: название места даёт тот,
      -- кто в нём живёт, а не тот, кто первым попал в обход.
      if member_culture ~= nil then
        local best, best_size = 0, -1
        for node = 1, member_count do
          if (math.tointeger(member_of:get(node)) or 0) == index and member_cells:get(node) > best_size then
            best_size = member_cells:get(node)
            best = node
          end
        end
        if best ~= 0 then
          culture_field:set(index, member_culture:get(best))
        end
      end
    end
  end

  local land_masses = step.writes.land_masses
  local continents = step.writes.continents
  local historical_regions = step.writes.historical_regions
  local oceans = step.writes.oceans

  land_masses:clear()
  continents:clear()
  historical_regions:clear()
  oceans:clear()

  summarise(land_masses, province_count, pr_land_mass, pr_cells, pr_center, pr_culture,
            "provinces", "seed_province", 1, land_mass_count)
  summarise(continents, province_count, pr_continent, pr_cells, pr_center, pr_culture,
            "provinces", "seed_province", 2, continent_count)
  summarise(historical_regions, province_count, pr_region, pr_cells, pr_center, pr_culture,
            "provinces", "seed_province", 3, historical_count)
  summarise(oceans, sea_zone_count, sz_ocean, sz_cells, sz_center, nil,
            "zones", "seed_zone", 4, ocean_count)

  local duchies = step.writes.duchies
  local empires = step.writes.empires
  local realms = step.writes.realms
  duchies:clear()
  empires:clear()
  realms:clear()

  summarise(duchies, province_count, pr_duchy, pr_cells, pr_center, pr_culture,
            "provinces", "seed_province", 5, duchy_count)
  summarise(empires, province_count, pr_empire, pr_cells, pr_center, pr_culture,
            "provinces", "seed_province", 6, empire_count)
  summarise(realms, province_count, pr_realm, pr_cells, pr_center, pr_culture,
            "provinces", "seed_province", 7, realm_count)

  -- Своё имя есть и у графства: иерархия названий кончается провинцией, а без имени провинция
  -- остаётся номером, и подписать её нечем.
  local pr_name = provinces:field("name_seed")
  for node = 1, province_count do
    pr_name:set(node, mix(seed, 8 * 1048576 + node) & 0x7FFFFFFF)
  end

  -- Население державы и сколько её провинций лежит в её СОБСТВЕННОЙ культуре. Второе число — не
  -- украшение: держава из своих и держава из чужих играются по-разному, а границы им обеим рисует
  -- одно и то же голосование, поэтому отличить их можно только измерением.
  local realm_population = realms:field("population")
  local realm_native = realms:field("native_provinces")
  local realm_culture = realms:field("culture")
  for node = 1, province_count do
    local realm = math.tointeger(pr_realm:get(node)) or 0
    if realm >= 1 and realm <= realm_count then
      realm_population:set(realm, realm_population:get(realm) + pr_population:get(node))
      if pr_culture:get(node) == realm_culture:get(realm) then
        realm_native:set(realm, realm_native:get(realm) + 1)
      end
    end
  end

  -- Родители титулов. У герцогства родитель однозначен — оно росло внутри исторической области; у
  -- империи тоже — внутри материка.
  local duchy_parent = duchies:field("historical_region")
  local empire_parent = empires:field("continent")
  for node = 1, province_count do
    local duchy = math.tointeger(pr_duchy:get(node)) or 0
    local empire = math.tointeger(pr_empire:get(node)) or 0
    if duchy >= 1 then
      duchy_parent:set(duchy, pr_region:get(node))
    end
    if empire >= 1 then
      empire_parent:set(empire, pr_continent:get(node))
    end
  end

  -- Родители.
  --
  -- У исторической области родитель однозначен: она растится внутри материка и выйти за него не
  -- может. У материка — НЕТ, и это следствие прикрепления: провинции мелких массивов лежат в чужом
  -- материке намеренно. Поэтому массив материка — тот, в котором лежит БОЛЬШЕ ВСЕГО его клеток, а не
  -- тот, чья провинция попалась последней. Иначе материк уезжает в массив прикреплённого острова, и
  -- «крупнейший массив» в отчёте оказывается без материков вовсе.
  local continent_parent = continents:field("land_mass")
  local region_parent = historical_regions:field("continent")
  local mass_weight = {}
  for node = 1, province_count do
    local continent = math.tointeger(pr_continent:get(node)) or 0
    local region = math.tointeger(pr_region:get(node)) or 0
    if continent >= 1 then
      local mass = math.tointeger(pr_land_mass:get(node)) or 0
      local per_continent = mass_weight[continent]
      if per_continent == nil then
        per_continent = {}
        mass_weight[continent] = per_continent
      end
      per_continent[mass] = (per_continent[mass] or 0) + pr_cells:get(node)
    end
    if region >= 1 then
      region_parent:set(region, continent)
    end
  end
  for continent = 1, continent_count do
    local per_continent = mass_weight[continent]
    if per_continent ~= nil then
      local best, best_weight = 0, -1
      for mass, weight in pairs(per_continent) do
        if weight > best_weight then
          best_weight = weight
          best = mass
        end
      end
      continent_parent:set(continent, best)
    end
  end

  state:field("duchy_count"):set(0, duchy_count)
  state:field("empire_count"):set(0, empire_count)
  state:field("realm_count"):set(0, realm_count)
  state:field("barony_count"):set(0, barony_count)
  state:field("water_body_count"):set(0, water_body_count)
  state:field("lake_count"):set(0, lakes)
  state:field("attached_provinces"):set(0, attached)
  state:field("land_mass_count"):set(0, land_mass_count)
  state:field("continent_count"):set(0, continent_count)
  state:field("historical_count"):set(0, historical_count)
  state:field("ocean_count"):set(0, ocean_count)
end
