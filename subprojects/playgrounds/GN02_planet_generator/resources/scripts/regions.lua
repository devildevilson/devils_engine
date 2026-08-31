-- Тело шага regions: нарезка суши на провинции и воды на большие морские зоны.
--
-- Это две РАЗНЫЕ нарезки, а не одна с параметром. У суши единица владения мелкая: провинция — то, чем
-- владеют, через что проходят войска и с чего собирают. У воды единица крупная: акватория — то, через
-- что ходят, и нарезать её так же мелко незачем. Поэтому у них разное число областей, разные затравки
-- и разные суммы в сводке.
--
-- Обе нарезки — заливка по ПОДГРАФУ: вода для провинции непроходима, суша для морской зоны тоже.
-- Дорогая цена вместо запрета здесь не работает: провинция, залитая через пролив, после отсечения
-- воды осталась бы двумя кусками на разных берегах, и заметно это стало бы только на отрисовке
-- границы.
--
-- Отсюда же добор затравок. Раз вода непроходима, остров, на который не попала ни одна затравка,
-- остаётся без провинции вовсе — это нашла проверка «каждая клетка суши лежит в провинции».
-- Правильный ответ не «залить через море», а «дать острову свою область»: добор ставит затравки в
-- каждый непокрытый кусок, продолжая нумерацию с того места, где кончилась предыдущая волна.

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
  local scratch_distance = cells:field("scratch_distance")

  local count = cells:count()
  local spacing = math.sqrt(4.0 * math.pi / count)

  -- Расстояние между затравками считается ЗДЕСЬ, а не выводится инструментом. Инструмент выводит его
  -- из площади под маской, и для первой волны это верно, а для добора — нет: там маска это несколько
  -- островов, и вывод по их площади дал бы затравку почти в каждой клетке.
  local land_share = originator.reduce_sum{ inputs = { land } } / count
  local province_distance = 0.92 * math.sqrt(4.0 * math.pi * land_share / p.province_count)
  local sea_distance = 0.92 * math.sqrt(4.0 * math.pi * (1.0 - land_share) / p.sea_zone_count)

  -- Вторая волна ставит затравки НЕ на том же расстоянии, что первая, и это выяснилось измерением:
  -- при 30 морских зонах шаг первой волны равен 0.5 радиана (3200 км по Земле), и одна принятая
  -- затравка второй волны гасила все остальные непокрытые клетки в этом радиусе — включая замкнутые
  -- моря по другую сторону перешейка, до которых заливка не доходит. Гашение идёт по прямой, а
  -- проходимость — по графу, и совпадать они не обязаны. Поэтому у второй волны шаг соседский: её
  -- задача не «разложить ровно», а «накрыть каждый оставшийся кусок».
  local patch_distance = spacing * 1.5

  -- Общая часть двух нарезок: заливка от затравок плюс добор непокрытого остатка.
  local function carve(seed_field, label_field, passable, cost, distance, score, unassigned_program)
    originator.poisson_seeds{
      inputs = { position, passable, score },
      outputs = { seed_field },
      params = { min_distance = distance },
    }
    originator.graph_flood{
      inputs = { offsets, arcs, seed_field, cost, passable },
      outputs = { label_field, scratch_distance },
      params = { unreached = -1 },
    }

    -- Добор идёт в цикле, а не одним проходом: гашение по прямой может оставить непокрытым кусок,
    -- лежащий рядом, но не связанный. Проходов немного — остаток каждый раз резко убывает, — а
    -- нижняя граница «ни одной непокрытой клетки» проверяется явно.
    for _ = 1, 8 do
      originator.run_script{
        program = unassigned_program,
        predicate = true,
        inputs = { land, label_field },
        outputs = { island_mask },
      }
      if originator.reduce_sum{ inputs = { island_mask } } == 0 then
        break
      end

      local placed = originator.reduce_max{ inputs = { seed_field } }
      originator.poisson_seeds{
        inputs = { position, island_mask, score },
        outputs = { island_label },
        params = { min_distance = patch_distance },
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

      originator.graph_flood{
        inputs = { offsets, arcs, seed_field, cost, passable },
        outputs = { label_field, scratch_distance },
        params = { unreached = -1 },
      }
    end

    return originator.reduce_max{ inputs = { label_field } }
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
  local province_count = carve(province_seed, province, land, flood_cost, province_distance,
                               habitability, step.programs.unassigned_land)

  -- 3. Морские зоны. Стоимость постоянная: у воды нет рельефа, который стоило бы объезжать.
  local sea_zone_count = carve(sea_seed, sea_zone, water, one, sea_distance, one,
                               step.programs.unassigned_water)

  -- 4. Раскладка клеток по областям и суммы. Суммы, а не средние: делить будет потребитель, зато
  -- сложение в scatter воспроизводится при любом числе потоков.
  originator.group_by{
    inputs = { province },
    outputs = { step.writes.province_offsets:field("start"), step.writes.province_order:field("cell") },
    key_support = "global",
  }
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

  -- 5. Что про область знает только скрипт: её затравка, центр и культура. Обход идёт по всем
  -- клеткам ОДИН раз, и это честная цена: узнать, в какой клетке стоит затравка области, больше
  -- ниоткуда нельзя — обратного отображения «метка -> клетка» в данных нет.
  local province_center = provinces:field("center")
  local province_seed_cell = provinces:field("seed_cell")
  local province_cells = provinces:field("cells")
  local province_culture = provinces:field("culture")
  local sea_center = sea_zones:field("center")
  local sea_seed_cell = sea_zones:field("seed_cell")
  local sea_cells = sea_zones:field("cells")

  local province_starts = step.writes.province_offsets:field("start")
  local sea_starts = step.writes.sea_offsets:field("start")
  local culture = cells:field("culture")

  for i = 0, count - 1 do
    local label = math.tointeger(province_seed:get(i))
    if label ~= 0 then
      local index = label - 1
      province_center:set(index, position:get(i, 0), 0)
      province_center:set(index, position:get(i, 1), 1)
      province_center:set(index, position:get(i, 2), 2)
      province_seed_cell:set(index, i)
      province_cells:set(index, province_starts:get(label + 1) - province_starts:get(label))
      province_culture:set(index, culture:get(i))
    end

    local zone = math.tointeger(sea_seed:get(i))
    if zone ~= 0 then
      local index = zone - 1
      sea_center:set(index, position:get(i, 0), 0)
      sea_center:set(index, position:get(i, 1), 1)
      sea_center:set(index, position:get(i, 2), 2)
      sea_seed_cell:set(index, i)
      sea_cells:set(index, sea_starts:get(zone + 1) - sea_starts:get(zone))
    end
  end

  state:field("province_count"):set(0, province_count)
  state:field("sea_zone_count"):set(0, sea_zone_count)
end
