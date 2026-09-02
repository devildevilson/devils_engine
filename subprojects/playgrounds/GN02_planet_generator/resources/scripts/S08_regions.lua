-- Тело шага regions: нарезка суши на провинции и воды на большие морские зоны.
--
-- Это две РАЗНЫЕ нарезки, а не одна с параметром. У суши единица владения мелкая: провинция — то, чем
-- владеют, через что проходят войска и с чего собирают. У воды единица крупная: акватория — то, через
-- что ходят, и нарезать её так же мелко незачем. Поэтому у них разное число областей, разные затравки
-- и разные суммы в сводке.
--
-- Обе нарезки — заливка по ПОДГРАФУ: вода для провинции непроходима, суша для морской зоны тоже.
-- Дорогая цена вместо запрета не работает: провинция, залитая через пролив, после отсечения воды
-- осталась бы двумя кусками на разных берегах, и заметно это стало бы только на отрисовке границы.
--
-- Отсюда же добор затравок. Раз вода непроходима, остров, на который не попала ни одна затравка,
-- остаётся без провинции вовсе — это нашла проверка «каждая клетка суши лежит в провинции».
-- Правильный ответ не «залить через море», а «дать острову свою область».
--
-- РАЗМЕР ПРОВИНЦИИ ОГРАНИЧЕН С ДВУХ СТОРОН, и это уступка играбельности, сделанная сознательно.
-- Сверху ограничение держит вместимость заливки: метка, набравшая предел, дальше не растёт, и её
-- остаток забирает добор — то есть крупная провинция делится, а не расползается на полматерика.
-- Снизу работает слияние: у слишком мелкой провинции снимается ЗАТРАВКА, и заливка сама отдаёт её
-- клетки соседям по стоимости. Остров меньше минимума так не исправить, потому что соседа по суше у
-- него нет, — и это верный ответ: остров и есть отдельная провинция.

return function(step)
  local cells = step.writes.cells
  local provinces = step.writes.provinces
  local sea_zones = step.writes.sea_zones
  local state = step.writes.state
  local p = step.params

  local offsets = step.reads.cell_offsets:field("start")
  local arcs = step.reads.cell_arcs:field("cell")

  local position = cells:field("position")
  local land = cells:field("land")
  local water = cells:field("ocean_seed") -- маска воды уже посчитана шагом поверхности
  local one = cells:field("one")
  local height = cells:field("height")
  local habitability = cells:field("habitability")
  local province = cells:field("province")
  local province_seed = cells:field("province_seed")
  local sea_zone = cells:field("sea_zone")
  local sea_seed = cells:field("sea_seed")
  local flood_cost = cells:field("flood_cost")
  local island_label = cells:field("island_label")
  local island_mask = cells:field("island_mask")
  local frontier = cells:field("frontier")
  local scratch_distance = cells:field("scratch_distance")

  local count = cells:count()
  local spacing = math.sqrt(4.0 * math.pi / count)

  -- Расстояние между затравками считается ЗДЕСЬ, а не выводится инструментом. Инструмент выводит его
  -- из площади под маской, и для первой волны это верно, а для добора — нет: там маска это несколько
  -- островов, и вывод по их площади дал бы затравку почти в каждой клетке.
  local land_cells = originator.reduce_sum{ inputs = { land } }
  local land_share = land_cells / count
  local province_distance = 0.92 * math.sqrt(4.0 * math.pi * land_share / p.province_count)
  local sea_distance = 0.92 * math.sqrt(4.0 * math.pi * (1.0 - land_share) / p.sea_zone_count)

  -- Добор ставит затравки НЕ на том же расстоянии, что первая волна, и это выяснилось измерением: при
  -- 30 морских зонах шаг первой волны равен 0.5 радиана (3200 км по Земле), и одна принятая затравка
  -- добора гасила все остальные непокрытые клетки в этом радиусе — включая замкнутые моря по другую
  -- сторону перешейка, до которых заливка не доходит. Гашение идёт по прямой, а проходимость — по
  -- графу, и совпадать они не обязаны. Поэтому у добора шаг соседский: его задача не «разложить
  -- ровно», а «накрыть каждый оставшийся кусок».
  local patch_distance = spacing * 1.5

  -- Границы размера провинции В КЛЕТКАХ, посчитанные от СРЕДНЕЙ провинции. Так они не зависят ни от
  -- разрешения планеты, ни от того, сколько провинций попросили в командной строке.
  local average_province = math.max(1.0, land_cells / p.province_count)
  local min_cells = math.max(2, math.floor(average_province * p.province_min_share))
  local max_cells = math.max(min_cells + 1, math.floor(average_province * p.province_max_share))

  local function place_seeds(seed_field, passable, score, distance)
    originator.poisson_seeds{
      inputs = { position, passable, score },
      outputs = { seed_field },
      params = { min_distance = distance },
    }
  end

  -- Заливка плюс добор непокрытого остатка. Цикл, а не один проход: гашение по прямой может оставить
  -- непокрытым кусок, лежащий рядом но не связанный, а при ограничении сверху добор ещё и подхватывает
  -- клетки, которые не влезли в набравшую предел метку.
  --
  -- Шаг добора УМЕНЬШАЕТСЯ вдвое от прохода к проходу, начиная с обычного расстояния между областями,
  -- и это исправление после замера. Когда добор сразу шёл с соседским шагом, он вёл себя правильно на
  -- островах и отвратительно внутри материка: после слияния мелких провинций освободившийся кусок
  -- оказывался непокрытым целиком, и добор засевал его затравками через полторы клетки — вместо одной
  -- области получались десятки провинций в одну клетку. Первый проход с обычным шагом отдаёт такой
  -- кусок одной-двум областям, а соседский шаг остаётся тем, чем и был: последним средством для
  -- крошечных островов, которые погасила затравка за проливом.
  local function grow(seed_field, label_field, passable, cost, capacity, score, unassigned_program,
                      primary_distance)
    local function flood()
      originator.graph_flood{
        inputs = { offsets, arcs, seed_field, cost, passable },
        outputs = { label_field, scratch_distance },
        params = { unreached = -1, capacity = capacity },
      }
    end

    flood()
    for pass = 1, 16 do
      originator.run_script{
        program = unassigned_program,
        predicate = true,
        inputs = { land, label_field },
        outputs = { island_mask },
      }
      if originator.reduce_sum{ inputs = { island_mask } } == 0 then
        break
      end

      local distance = math.max(patch_distance, primary_distance * 0.5 ^ (pass - 1))
      local placed = originator.reduce_max{ inputs = { seed_field } }
      originator.poisson_seeds{
        inputs = { position, island_mask, score },
        outputs = { island_label },
        params = { min_distance = distance },
      }
      -- Признак затравки добора и сдвиг её номера в свободную часть пространства метк: волна обязана
      -- продолжать нумерацию, а не начинать заново, иначе два разных острова получили бы одну область.
      originator.run_script{
        program = step.programs.island_seeded,
        predicate = true,
        inputs = { island_label },
        outputs = { island_mask },
      }
      originator.blend{
        inputs = { island_label, island_mask },
        outputs = { island_label },
        params = { first = 1.0, second = placed },
      }
      -- Множества затравок не пересекаются: добор стоит только там, где метки не было.
      originator.blend{
        inputs = { seed_field, island_label },
        outputs = { seed_field },
      }

      flood()
    end

    return originator.reduce_max{ inputs = { label_field } }
  end

  local province_starts = step.writes.province_offsets:field("start")
  local province_order = step.writes.province_order:field("cell")
  local province_frontier = provinces:field("frontier")

  -- Сводка по областям: раскладка клеток и длина рубежа. Рубеж нужен слиянию (у острова его нет), и он
  -- же уходит в пакет как данные: у области с длинным рубежом другая цена обороны.
  local function summarise_provinces()
    originator.group_by{
      inputs = { province },
      outputs = { province_starts, province_order },
      key_support = "global",
    }
    originator.graph_frontier{
      inputs = { offsets, arcs, province },
      outputs = { frontier },
      params = { ignore = 0 },
    }
    originator.accumulate{
      inputs = { province, frontier },
      outputs = { province_frontier },
      key_support = "global",
    }
  end

  -- Обратное отображение «метка -> клетка затравки». Строится ТЕМ ЖЕ `group_by`, только по полю
  -- ЗАТРАВКИ: у метки там ровно одна клетка, поэтому её корзина и есть ответ. Читается сразу в
  -- таблицу по меткам (их тысячи, а не сотни тысяч), чтобы служебные буферы можно было переиспользовать
  -- под второе разбиение.
  local seed_offsets = step.writes.seed_offsets:field("start")
  local seed_order = step.writes.seed_order:field("cell")
  local function seed_cells_of(seed_field, label_count)
    originator.group_by{
      inputs = { seed_field },
      outputs = { seed_offsets, seed_order },
      key_support = "global",
    }
    local result = {}
    for label = 1, label_count do
      local first = math.tointeger(seed_offsets:get(label))
      if math.tointeger(seed_offsets:get(label + 1)) > first then
        result[label] = math.tointeger(seed_order:get(first))
      end
    end
    return result
  end

  -- 1. Стоимость роста провинции: горы дороги. Провинции из-за этого ложатся по долинам и вдоль
  -- берега, а не режут хребет пополам — так же, как это делают настоящие границы.
  originator.remap{
    inputs = { height },
    outputs = { flood_cost },
    params = { scale = p.province_mountain_cost / 1000.0, offset = 1.0, min = 1.0, max = 8.0 },
  }

  -- 2. Провинции. Счёт при выборе затравок — пригодность: провинции получаются мельче там, где живут
  -- люди, и крупнее в пустом краю. Это не украшение, а то, как устроено административное деление.
  place_seeds(province_seed, land, habitability, province_distance)
  local province_count = grow(province_seed, province, land, flood_cost, max_cells, habitability,
                              step.programs.unassigned_land, province_distance)

  -- 3. Слияние мелких провинций.
  local merged = 0
  for _ = 1, math.floor(p.province_merge_passes) do
    summarise_provinces()

    local seed_of = seed_cells_of(province_seed, province_count)

    local removed = 0
    for label = 1, province_count do
      local size = province_starts:get(label + 1) - province_starts:get(label)
      -- Рубеж больше нуля означает «есть сосед по суше»: только тогда клетки есть кому отдать.
      if size > 0 and size < min_cells and province_frontier:get(label) > 0 and seed_of[label] then
        province_seed:set(seed_of[label], 0)
        removed = removed + 1
      end
    end

    if removed == 0 then
      break
    end
    merged = merged + removed
    province_count = grow(province_seed, province, land, flood_cost, max_cells, habitability,
                          step.programs.unassigned_land, province_distance)
  end

  -- Последняя заливка с запасом к пределу. Без запаса одиночные клетки, у которых все соседи набрали
  -- предел, получают свою затравку от добора — и появляется провинция в одну клетку. Такой артефакт
  -- хуже, чем провинция на четверть больше предела.
  local soft_capacity = math.floor(max_cells * p.province_capacity_slack)
  province_count = grow(province_seed, province, land, flood_cost, soft_capacity, habitability,
                        step.programs.unassigned_land, province_distance)

  -- 4. Морские зоны. Стоимость постоянная и предела нет: у воды нет рельефа, который стоило бы
  -- объезжать, а крупная акватория — это то, чего от неё и хотят.
  place_seeds(sea_seed, water, one, sea_distance)
  local sea_zone_count = grow(sea_seed, sea_zone, water, one, 0, one, step.programs.unassigned_water,
                              sea_distance)

  -- 5. Итоговые сводки. Суммы, а не средние: делить будет потребитель, зато сложение в scatter
  -- воспроизводится при любом числе потоков.
  --
  -- Сначала буферы областей ОБНУЛЯЮТСЯ, и это не гигиена, а исправление настоящего бага: метка,
  -- исчезнувшая при слиянии, оставляла в записи свои старые числа. В отчёте из-за этого висели
  -- «провинции в одну клетку», которых на карте нет, а в пакет уходила запись области, которой не
  -- существует. Ни одна проверка связности такого не ловит: буфер-то валиден.
  provinces:clear()
  sea_zones:clear()

  summarise_provinces()

  local province_sums = {
    { "height", provinces:field("height_sum") },
    { "temperature", provinces:field("temperature_sum") },
    { "precipitation", provinces:field("precipitation_sum") },
    { "population", provinces:field("population_sum") },
    { "habitability", provinces:field("habitability_sum") },
  }
  for _, entry in ipairs(province_sums) do
    originator.accumulate{
      inputs = { province, cells:field(entry[1]) },
      outputs = { entry[2] },
      key_support = "global",
    }
  end

  originator.group_by{
    inputs = { sea_zone },
    outputs = { step.writes.sea_offsets:field("start"), step.writes.sea_order:field("cell") },
    key_support = "global",
  }
  originator.accumulate{
    inputs = { sea_zone, height },
    outputs = { sea_zones:field("depth_sum") },
    key_support = "global",
  }
  originator.accumulate{
    inputs = { sea_zone, cells:field("temperature") },
    outputs = { sea_zones:field("temperature_sum") },
    key_support = "global",
  }

  -- 6. Что про область знает только скрипт: её затравка, центр и культура. Обход идёт ПО МЕТКАМ, а не
  -- по клеткам: обратное отображение «метка -> клетка затравки» даёт `group_by` по полю затравки.
  local province_center = provinces:field("center")
  local province_seed_cell = provinces:field("seed_cell")
  local province_cells = provinces:field("cells")
  local province_culture = provinces:field("culture")
  local sea_center = sea_zones:field("center")
  local sea_seed_cell = sea_zones:field("seed_cell")
  local sea_cells = sea_zones:field("cells")

  local sea_starts = step.writes.sea_offsets:field("start")
  local culture = cells:field("culture")

  local province_seed_cells = seed_cells_of(province_seed, province_count)
  for label = 1, province_count do
    local i = province_seed_cells[label]
    if i then
      -- Индекс равен МЕТКЕ (соглашение в buffers.tavl): корзина 0 это «метки нет».
      province_center:set(label, position:get(i, 0), 0)
      province_center:set(label, position:get(i, 1), 1)
      province_center:set(label, position:get(i, 2), 2)
      province_seed_cell:set(label, i)
      province_cells:set(label, province_starts:get(label + 1) - province_starts:get(label))
      province_culture:set(label, culture:get(i))
    end
  end

  local sea_seed_cells = seed_cells_of(sea_seed, sea_zone_count)
  for zone = 1, sea_zone_count do
    local i = sea_seed_cells[zone]
    if i then
      sea_center:set(zone, position:get(i, 0), 0)
      sea_center:set(zone, position:get(i, 1), 1)
      sea_center:set(zone, position:get(i, 2), 2)
      sea_seed_cell:set(zone, i)
      sea_cells:set(zone, sea_starts:get(zone + 1) - sea_starts:get(zone))
    end
  end

  state:field("province_count"):set(0, province_count)
  state:field("sea_zone_count"):set(0, sea_zone_count)
  state:field("province_merged"):set(0, merged)
  state:field("province_min_cells"):set(0, min_cells)
  -- В сводку уходит МЯГКИЙ предел: это тот, который держится по факту, а строгий остаётся целью.
  state:field("province_max_cells"):set(0, soft_capacity)
end
