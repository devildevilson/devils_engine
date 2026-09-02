-- Тело шага peoples: пригодность земли, расселение, культуры и короткая история.
--
-- Разделение обязанностей здесь видно яснее всего:
--   движок  приводит величины к отрезку [0,1] по порогам из конфига — это арифметика;
--   ds      комбинирует условия в пригодность и считает логистический рост — это правила мира;
--   lua     ведёт время (тики), заполняет маленькие буферы культур и пишет журнал событий.
--
-- Расселение — процесс, а не картинка: население мигрирует размытием по графу и растёт логистически
-- на ёмкости, равной пригодности, а культура занимает клетку ГОЛОСОВАНИЕМ соседей, где вес голоса —
-- население. Поэтому граница культур едет туда, где людей больше, а не туда, где ближе центр.

return function(step)
  local cells = step.writes.cells
  local cultures = step.writes.cultures
  local history = step.writes.history
  local state = step.writes.state
  local p = step.params

  local offsets = step.reads.cell_offsets:field("start")
  local arcs = step.reads.cell_arcs:field("cell")

  local position = cells:field("position")
  local land = cells:field("land")
  local summer = cells:field("temperature_summer")
  local winter = cells:field("temperature_winter")
  local precipitation = cells:field("precipitation")
  local height = cells:field("height")
  local habitability = cells:field("habitability")
  local population = cells:field("population")
  local population_next = cells:field("population_next")
  local culture = cells:field("culture")
  local culture_next = cells:field("culture_next")
  local culture_mask = cells:field("culture_mask")
  local frontier = cells:field("frontier")

  local count = cells:count()

  -- 1. Нормировка условий. Каждое приведение — «величина между двумя порогами», то есть арифметика,
  -- поэтому её делает движок: правило на devils_script не может вызвать функцию внутри выражения, а
  -- размазывать пороги по нескольким однострочным программам значило бы потерять само правило.
  local warm_span = p.habitable_optimum - p.habitable_cold
  originator.remap{
    inputs = { summer },
    outputs = { cells:field("warmth") },
    params = { scale = 1.0 / warm_span, offset = -p.habitable_cold / warm_span, min = 0.0, max = 1.0 },
  }

  local heat_span = 8.0
  originator.remap{
    inputs = { summer },
    outputs = { cells:field("heat_penalty") },
    params = { scale = 1.0 / heat_span, offset = -p.habitable_warm / heat_span, min = 0.0, max = 1.0 },
  }

  local wet_span = p.habitable_wet - p.habitable_dry
  originator.remap{
    inputs = { precipitation },
    outputs = { cells:field("wetness") },
    params = { scale = 1.0 / wet_span, offset = -p.habitable_dry / wet_span, min = 0.0, max = 1.0 },
  }

  local winter_span = 20.0
  originator.remap{
    inputs = { winter },
    outputs = { cells:field("winter_penalty") },
    params = { scale = -1.0 / winter_span, offset = p.habitable_cold / winter_span, min = 0.0, max = 1.0 },
  }

  originator.remap{
    inputs = { height },
    outputs = { cells:field("altitude") },
    params = { scale = -1.0 / p.habitable_height, offset = 1.0, min = 0.0, max = 1.0 },
  }

  -- 2. Пригодность: произведение независимых условий. Недостаток любого не лечится избытком другого.
  originator.run_script{
    program = step.programs.habitability,
    inputs = {
      land,
      cells:field("warmth"),
      cells:field("wetness"),
      cells:field("altitude"),
      cells:field("heat_penalty"),
      cells:field("winter_penalty"),
    },
    outputs = { habitability },
  }

  -- 3. Начальное население пропорционально пригодности: люди появляются там, где можно жить, а не
  -- в случайной точке. Дальше их перемешает миграция.
  originator.remap{
    inputs = { habitability },
    outputs = { population },
    params = { scale = p.population_seed, min = 0.0 },
  }

  -- 4. Расселение. Один тик — это миграция плюс рост; порядок именно такой, потому что пришедшие
  -- люди должны успеть упереться в ёмкость нового места, а не наоборот.
  for _ = 1, math.floor(p.population_ticks) do
    originator.graph_blur{
      inputs = { offsets, arcs, population },
      outputs = { population_next },
      params = { self_weight = p.migration_self_weight },
    }
    originator.run_script{
      program = step.programs.population_step,
      inputs = { population_next, habitability },
      outputs = { population },
      params = { population_rate = p.population_rate, population_capacity = p.population_capacity },
    }
  end

  -- 5. Прародины культур. Маска — правило в конфиге, счёт — пригодность, поэтому культуры зарождаются
  -- в хороших местах, а расстояние между прародинами держит инструмент затравок.
  originator.run_script{
    program = step.programs.culture_mask,
    predicate = true,
    inputs = { land, habitability },
    outputs = { culture_mask },
    params = { culture_min_habitability = p.culture_min_habitability },
  }

  originator.poisson_seeds{
    inputs = { position, culture_mask, habitability },
    outputs = { cells:field("culture_seed") },
    params = { target_count = p.culture_count, limit = p.culture_count },
  }

  originator.remap{
    inputs = { cells:field("culture_seed") },
    outputs = { culture },
    params = { scale = 1.0 },
  }

  -- Буфер культур заполняет lua: их десятки, и это ровно тот случай, который правилом разрешён.
  local culture_origin = cultures:field("origin")
  local culture_origin_cell = cultures:field("origin_cell")
  local culture_birth = cultures:field("birth_tick")
  local culture_habitability = cultures:field("habitability")
  local culture_cells = cultures:field("cells")
  local culture_frontier = cultures:field("frontier")
  local culture_contacts = cultures:field("contacts")

  -- Журнал событий. Вид события — число, потому что событие это ЗАПИСЬ, а не сущность:
  --   1 основание   2 рост   3 первая встреча с другой культурой   4 господство
  local event_value = history:field("value")
  local event_cell = history:field("cell")
  local event_tick = history:field("tick")
  local event_subject = history:field("subject")
  local event_object = history:field("object")
  local event_kind = history:field("kind")
  local event_capacity = history:count()
  local events = 0

  local function record(tick, kind, subject, object, cell, value)
    if events >= event_capacity then
      return
    end
    event_tick:set(events, tick)
    event_kind:set(events, kind)
    event_subject:set(events, subject)
    event_object:set(events, object)
    event_cell:set(events, cell)
    event_value:set(events, value)
    events = events + 1
  end

  local culture_count = 0
  local known_size = {}
  local met = {}

  for i = 0, count - 1 do
    local label = math.tointeger(cells:field("culture_seed"):get(i))
    if label ~= 0 then
      -- Индекс равен МЕТКЕ (см. соглашение в buffers.tavl): корзина 0 у group_by и accumulate это
      -- «метки нет», поэтому строка записи и номер области совпадают.
      local index = label
      culture_origin:set(index, position:get(i, 0), 0)
      culture_origin:set(index, position:get(i, 1), 1)
      culture_origin:set(index, position:get(i, 2), 2)
      culture_origin_cell:set(index, i)
      culture_birth:set(index, 0)
      culture_habitability:set(index, habitability:get(i))
      known_size[label] = 1
      met[label] = false
      culture_count = culture_count + 1
      record(0, 1, label, 0, i, habitability:get(i))
    end
  end

  -- 6. Расселение культур голосованием. Проходы идут парами по той же причине, что и всюду: gather
  -- читает соседей из входного поля и пишет своё, поэтому поля меняются местами.
  --
  -- Число тиков считается из ДАЛЬНОСТИ в радианах, а не берётся числом: один тик — это один шаг
  -- графа, поэтому на решётке из 262 тысяч клеток те же 28 тиков уносят культуру вдвое ближе, чем на
  -- решётке из 65 тысяч. Первый же кадр просмотрщика показал это как пятна культур на пустой планете.
  local spacing = math.sqrt(4.0 * math.pi / count)
  local ticks = math.floor(math.min(p.culture_reach / spacing, p.culture_max_ticks))
  if ticks < 2 then
    ticks = 2
  end
  for tick = 1, math.floor(ticks / 2) do
    originator.graph_vote{
      inputs = { offsets, arcs, culture, population, culture_mask },
      outputs = { culture_next },
    }
    originator.graph_vote{
      inputs = { offsets, arcs, culture_next, population, culture_mask },
      outputs = { culture },
    }

    -- Сводка тика: сколько клеток у культуры и есть ли у неё рубеж с другой. Обе величины считаются
    -- инструментами по всем клеткам, а lua смотрит только на десятки культур.
    originator.group_by{
      inputs = { culture },
      outputs = { step.writes.culture_offsets:field("start"), step.writes.culture_order:field("cell") },
      key_support = "global",
    }
    originator.graph_frontier{
      inputs = { offsets, arcs, culture },
      outputs = { frontier },
      params = { ignore = 0 },
    }
    originator.accumulate{
      inputs = { culture, frontier },
      outputs = { culture_frontier },
      key_support = "global",
    }

    local starts = step.writes.culture_offsets:field("start")
    for label = 1, culture_count do
      local size = starts:get(label + 1) - starts:get(label)
      culture_cells:set(label, size)

      -- Рост становится событием не каждый тик, а когда культура вырастает в разы: журнал должен
      -- отвечать на вопрос «что было важным», а не «что было».
      if size >= known_size[label] * p.history_growth_step then
        known_size[label] = size
        record(tick * 2, 2, label, 0, math.tointeger(culture_origin_cell:get(label)), size)
      end

      if not met[label] and culture_frontier:get(label) > 0 then
        met[label] = true
        culture_contacts:set(label, 1)
        record(tick * 2, 3, label, 0, math.tointeger(culture_origin_cell:get(label)),
               culture_frontier:get(label))
      end
    end
  end

  -- 7. Итоги: население по культурам, крупнейшая культура и сводка мира.
  originator.accumulate{
    inputs = { culture, population },
    outputs = { cultures:field("population") },
    key_support = "global",
  }

  local largest, largest_size = 0, -1
  for label = 1, culture_count do
    local size = culture_cells:get(label)
    if size > largest_size then
      largest, largest_size = label, size
    end
  end
  if largest ~= 0 then
    record(ticks, 4, largest, 0, math.tointeger(culture_origin_cell:get(largest - 1)), largest_size)
  end

  state:field("culture_count"):set(0, culture_count)
  state:field("event_count"):set(0, events)
  state:field("population"):set(0, originator.reduce_sum{ inputs = { population } })
end
