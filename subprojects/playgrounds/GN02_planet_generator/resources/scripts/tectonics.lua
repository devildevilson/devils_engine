-- Тело шага tectonics: плиты, их движение, стыки и рельеф.
--
-- Роль lua здесь видна целиком. Плит десятки, поэтому их параметры скрипт расставляет ПОЭЛЕМЕНТНО —
-- это ровно тот случай, который правилом разрешён: обходится множество, которое сам же перечислил.
-- Всё, что касается десятков и сотен тысяч клеток, уходит в нативные инструменты и в правила на
-- devils_script; ни одного цикла по клеткам здесь нет, кроме одного сбора затравок.

-- Детерминированный хеш вместо math.random: генератора случайности в окружении нет намеренно, а
-- зерно приходит от шага. Одна и та же пара (зерно, номер) обязана давать одно и то же значение при
-- любом порядке вызовов, иначе перестановка шагов сдвинула бы весь мир.
local function mix(a, b)
  -- Приведение к целому обязательно: поля буфера читаются как числа lua, то есть double, а битовые
  -- операции требуют целого. Метка «3» из поля — это 3.0, и без tointeger умножение уехало бы во
  -- float, где старшие биты хеша теряются, а сам сдвиг вообще не определён.
  local ia = math.tointeger(a) or 0
  local ib = math.tointeger(b) or 0
  local x = (ia * 0x9E3779B97F4A7C15) ~ (ib * 0xBF58476D1CE4E5B9)
  x = x ~ (x >> 30)
  x = x * 0xBF58476D1CE4E5B9
  x = x ~ (x >> 27)
  x = x * 0x94D049BB133111EB
  return x ~ (x >> 31)
end

-- Значение в [0, 1) из хеша: старшие 53 бита, потому что мантисса double именно такая.
local function unit(a, b)
  return ((mix(a, b) >> 11) & 0x1FFFFFFFFFFFFF) / 9007199254740992.0
end

return function(step)
  local cells = step.writes.cells
  local plates = step.writes.plates
  local p = step.params

  local offsets = step.reads.cell_offsets:field("start")
  local arcs = step.reads.cell_arcs:field("cell")

  local position = cells:field("position")
  local noise = cells:field("relief_noise")
  local one = cells:field("one")
  local plate = cells:field("plate")
  local plate_seed = cells:field("plate_seed")

  -- 1. Крупный шум. Он же становится стоимостью роста плиты: с постоянной стоимостью плиты выходят
  -- одинаковыми сотами, чего на планете не бывает — у Земли одна плита больше нескольких остальных
  -- вместе. Тот же шум служит счётом при выборе затравок, чтобы их порядок не совпадал с порядком
  -- индексов решётки.
  originator.noise_at{
    inputs = { position },
    outputs = { noise },
    params = { tree = p.tree, frequency = p.plate_cost_frequency, amplitude = 0.5, offset = 0.5 },
  }

  originator.remap{
    inputs = { noise },
    outputs = { cells:field("flood_cost") },
    params = { scale = p.plate_cost_variation, offset = 1.0, min = 0.05 },
  }

  -- 2. Затравки плит, разнесённые по поверхности. «Каждая k-я клетка» здесь не работает: под маской
  -- индексы идут неравномерно, и затравки собрались бы в кучи.
  originator.poisson_seeds{
    inputs = { position, one, noise },
    outputs = { plate_seed },
    params = { target_count = p.plate_count, limit = p.plate_count },
  }

  -- 3. Параметры плит. Плита описана ПОЛЮСОМ ЭЙЛЕРА и угловой скоростью: на замкнутой поверхности
  -- «едет в сторону» не имеет смысла, а «поворачивается вокруг оси» имеет, и из этого сама собой
  -- получается разная скорость вдоль одного стыка.
  local plate_axis = plates:field("axis")
  local plate_rate = plates:field("rate")
  local plate_crust = plates:field("crust")
  local plate_seed_cell = plates:field("seed_cell")

  local count = cells:count()
  local found = 0
  local seed = step.seed

  -- Шаг решётки в радианах: площадь клетки 4π/N, значит шаг порядка корня из неё. Через него ширины
  -- из конфига переводятся из радианов в шаги графа. Без этого перевода смена разрешения планеты
  -- молча меняла бы мир: «2.6 клетки» — это 230 км на одной решётке и 115 км на другой.
  local spacing = math.sqrt(4.0 * math.pi / count)

  for i = 0, count - 1 do
    local label = math.tointeger(plate_seed:get(i))
    if label ~= 0 then
      -- Индекс обязан быть ЦЕЛЫМ подтипом lua, а не целым значением double: привязка индекса поля
      -- принимает size_t, и 2.0 в неё не проходит, потому что lua различает 2 и 2.0 по типу.
      local index = label - 1
      local key = label * 4
      local u1 = unit(seed, key + 1)
      local u2 = unit(seed, key + 2)
      local u3 = unit(seed, key + 3)
      local u4 = unit(seed, key + 4)

      -- Равномерное направление на сфере: сначала высота, потом угол.
      local along = 2.0 * u1 - 1.0
      local ring = math.sqrt(math.max(0.0, 1.0 - along * along))
      local angle = 2.0 * math.pi * u2
      local ax, ay, az = ring * math.cos(angle), along, ring * math.sin(angle)

      -- Смещение полюса в экваториальную плоскость. Плита, вращающаяся вокруг оси планеты, движется
      -- чисто по широте и материки не перемешивает; полюс на экваторе гоняет кору ПОПЕРЁК широт.
      -- Отсюда и связь дрейфа с вращением — но только связь: куда именно уедет суша, показывает
      -- измерение, а не это число.
      ay = ay * (1.0 - p.polar_axis_bias)
      local length = math.sqrt(ax * ax + ay * ay + az * az)
      if length < 1e-6 then
        ax, ay, az, length = 1.0, 0.0, 0.0, 1.0
      end

      plate_axis:set(index, ax / length, 0)
      plate_axis:set(index, ay / length, 1)
      plate_axis:set(index, az / length, 2)
      plate_rate:set(index, p.plate_rate_min + u3 * (p.plate_rate_max - p.plate_rate_min))
      -- Кора: континентальная легче, поэтому она не тонет, а сминается. Доля берётся из конфига.
      plate_crust:set(index, u4 < p.continental_share and 1.0 or 0.0)
      plate_seed_cell:set(index, i)

      found = found + 1
    end
  end

  if found < 4 then
    error("tectonics: получилось всего " .. found .. " плит, планета из них не собирается")
  end

  -- 4. Плиты растут заливкой: «кто ближе по стоимости», а не «кто ближе по прямой». Метка 0 значит
  -- «нет плиты», поэтому недостигнутая клетка обязана остаться нулём и упасть на проверке.
  originator.graph_flood{
    inputs = { offsets, arcs, plate_seed, cells:field("flood_cost"), one },
    outputs = { plate, cells:field("scratch_distance") },
    params = { unreached = 0 },
  }

  -- 5. Свойства плиты разносятся по её клеткам выборкой по индексу. Смещение -1 потому, что метки
  -- считаются с единицы, а элементы буфера плит — с нуля.
  originator.lookup{
    inputs = { plate, plate_axis },
    outputs = { cells:field("plate_axis") },
    params = { offset = -1 },
  }
  originator.lookup{
    inputs = { plate, plate_rate },
    outputs = { cells:field("plate_rate") },
    params = { offset = -1 },
  }
  originator.lookup{
    inputs = { plate, plate_crust },
    outputs = { cells:field("crust") },
    params = { offset = -1 },
  }

  -- 6. Скорость поверхности и разложение относительной скорости на стыке. Тип границы не
  -- объявляется списком, а считается: сближение, расхождение и сдвиг — это три проекции одного
  -- вектора, поэтому один стык может быть сходящимся в одном месте и трансформным в другом.
  originator.plate_velocity{
    inputs = { position, cells:field("plate_axis"), cells:field("plate_rate") },
    outputs = { cells:field("velocity") },
  }

  originator.plate_interaction{
    inputs = { offsets, arcs, plate, cells:field("velocity"), position, cells:field("crust") },
    outputs = { cells:field("convergence"), cells:field("shear"), cells:field("subduction") },
  }

  -- 7. Затравки границ: правило в скрипте, порог в конфиге.
  originator.run_script{
    program = step.programs.convergent,
    predicate = true,
    inputs = { cells:field("convergence") },
    outputs = { cells:field("convergent_seed") },
    params = { boundary_threshold = p.boundary_threshold },
  }
  originator.run_script{
    program = step.programs.divergent,
    predicate = true,
    inputs = { cells:field("convergence") },
    outputs = { cells:field("divergent_seed") },
    params = { boundary_threshold = p.boundary_threshold },
  }

  -- 8. Расстояния до границ каждого типа. unreached большое, а не -1: «границы такого типа нет»
  -- должно ГАСНУТЬ в экспоненте формулы рельефа, а отрицательное расстояние там выросло бы.
  originator.graph_flood{
    inputs = { offsets, arcs, cells:field("convergent_seed"), one, one },
    outputs = { cells:field("scratch_label"), cells:field("convergent_distance") },
    params = { unreached = 999 },
  }
  originator.graph_flood{
    inputs = { offsets, arcs, cells:field("divergent_seed"), one, one },
    outputs = { cells:field("scratch_label"), cells:field("divergent_distance") },
    params = { unreached = 999 },
  }

  -- 9. Мелкий шум для изломанности рельефа. Буфер шума переиспользуется: время жизни буферов —
  -- решение скрипта, автоматического наложения нет.
  originator.noise_at{
    inputs = { position },
    outputs = { noise },
    params = { tree = p.tree, frequency = p.relief_frequency, amplitude = 1.0, offset = 0.0 },
  }

  -- 10. Рельеф. Это ПРАВИЛО, а не алгоритм, и живёт оно в конфиге, потому что автор мира правит его
  -- чаще всего. Но правило складывает готовые слагаемые, а не считает их: в этой версии
  -- devils_script вызов функции не является операндом выражения, а у контекста восемь слотов
  -- аргументов. Поэтому спады и ограничения считает движок — decay и remap, — а конфиг остаётся
  -- местом, где рельеф СОБИРАЕТСЯ из слагаемых. Заодно из вызовов ниже видно, из чего он состоит.
  local height = cells:field("height")
  local part = cells:field("height_smooth")
  local drive = cells:field("drive")
  local open_drive = cells:field("open_drive")
  local age = cells:field("age")
  local decay_a = cells:field("decay_a")
  local decay_b = cells:field("decay_b")

  -- Скорость сближения и расхождения, ограниченная сверху: при встречных плитах на полном ходу
  -- поднятие иначе выросло бы втрое против расчётного.
  originator.remap{
    inputs = { cells:field("convergence") },
    outputs = { drive },
    params = { scale = 1.0 / p.uplift_reference, min = 0.0, max = 1.5 },
  }
  originator.remap{
    inputs = { cells:field("convergence") },
    outputs = { open_drive },
    params = { scale = -1.0 / p.uplift_reference, min = 0.0, max = 1.5 },
  }

  -- Спад поднятия и глубины жёлоба от сходящейся границы.
  originator.decay{
    inputs = { cells:field("convergent_distance") },
    outputs = { decay_a },
    params = { width = p.uplift_width / spacing },
  }
  originator.decay{
    inputs = { cells:field("convergent_distance") },
    outputs = { decay_b },
    params = { width = p.trench_width / spacing },
  }

  originator.run_script{
    program = step.programs.relief_convergent,
    inputs = { cells:field("crust"), cells:field("subduction"), drive, decay_a, decay_b },
    outputs = { height },
    params = {
      continental_base = p.continental_base,
      oceanic_base = p.oceanic_base,
      uplift = p.uplift,
      trench = p.trench,
    },
  }

  -- Те же два поля спада переиспользуются для расходящейся границы: время жизни буфера — решение
  -- скрипта, и здесь оно очевидное.
  originator.decay{
    inputs = { cells:field("divergent_distance") },
    outputs = { decay_a },
    params = { width = p.ridge_width / spacing },
  }
  originator.decay{
    inputs = { cells:field("divergent_distance") },
    outputs = { decay_b },
    params = { width = p.rift_width / spacing },
  }
  -- Возраст дна: расстояние от хребта, ограниченное сверху.
  originator.remap{
    inputs = { cells:field("divergent_distance") },
    outputs = { age },
    params = { scale = 1.0, min = 0.0, max = p.abyss_limit / spacing },
  }

  originator.run_script{
    program = step.programs.relief_divergent,
    inputs = {
      cells:field("crust"),
      open_drive,
      decay_a,
      decay_b,
      age,
      cells:field("relief_noise"),
      cells:field("shear"),
    },
    outputs = { part },
    params = {
      ridge = p.ridge,
      rift = p.rift,
      abyss_slope = p.abyss_slope,
      relief_detail = p.relief_detail,
      shear_roughness = p.shear_roughness,
    },
  }

  originator.blend{
    inputs = { height, part },
    outputs = { height },
  }

  -- 11. Раскладка клеток по плитам и сводка. Диапазон scatter относится ко ВХОДАМ: число корзин
  -- задаёт буфер смещений, а не число обрабатываемых клеток.
  originator.group_by{
    inputs = { plate },
    outputs = { step.writes.plate_offsets:field("start"), step.writes.plate_order:field("cell") },
    key_support = "global",
  }

  local plate_starts = step.writes.plate_offsets:field("start")
  local plate_cells = plates:field("cells")
  for index = 0, found - 1 do
    local label = index + 1
    plate_cells:set(index, plate_starts:get(label + 1) - plate_starts:get(label))
  end

  step.writes.state:field("plate_count"):set(0, found)
end
