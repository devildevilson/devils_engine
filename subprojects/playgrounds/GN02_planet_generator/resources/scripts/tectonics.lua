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

  -- Размеры узора шума объявлены в РАДИАНАХ, как и все прочие расстояния мира, а сюда переводятся в
  -- единицы frequency закодированного дерева. Пересчёт нужен потому, что в дерево зашита собственная
  -- частота: единица frequency даёт всего noise_scale цикла на радиан. Пока размеры задавались прямо
  -- в этих единицах, они были примерно в сто раз меньше задуманного — «мелкий шум рельефа» с
  -- частотой 11 имел узор в несколько радиан, то есть был плавным градиентом по всей планете, а не
  -- рельефом. Заметно это стало только по измерению: уклон суши не отзывался ни на амплитуду детали,
  -- ни на частоту, пока та не выросла в полсотни раз.
  local function noise_frequency(feature)
    return 1.0 / (math.max(feature, 1e-6) * p.noise_scale)
  end

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
    params = { tree = p.tree, frequency = noise_frequency(p.plate_cost_feature), amplitude = 0.5, offset = 0.5 },
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

  -- 2a. Мантийные плюмы. Выбираются тем же разнесённым выбором, что и плиты, но по СВОЕМУ счёту и в
  -- своей системе: плюм стоит в МАНТИИ, а не на плите, поэтому он не привязан ни к границам, ни к
  -- коре. Затравки собираются ниже в том же проходе, что и плиты: обход всех клеток из lua стоит
  -- заметно, и второй такой проход ради десятка точек был бы чистой потерей.
  originator.noise_at{
    inputs = { position },
    outputs = { cells:field("crust_work") },
    params = {
      tree = p.tree, frequency = noise_frequency(p.hotspot_feature),
      amplitude = 0.5, offset = 0.5, seed_offset = 911,
    },
  }
  originator.poisson_seeds{
    inputs = { position, one, cells:field("crust_work") },
    outputs = { cells:field("hotspot_seed") },
    params = { target_count = p.hotspot_count, limit = p.hotspot_count },
  }

  -- 3. Параметры плит. Плита описана ПОЛЮСОМ ЭЙЛЕРА и угловой скоростью: на замкнутой поверхности
  -- «едет в сторону» не имеет смысла, а «поворачивается вокруг оси» имеет, и из этого сама собой
  -- получается разная скорость вдоль одного стыка.
  local plate_axis = plates:field("axis")
  local plate_rate = plates:field("rate")
  local plate_crust = plates:field("crust")
  local plate_seed_cell = plates:field("seed_cell")

  local hotspots = step.writes.hotspots
  local hotspot_position = hotspots:field("position")
  local hotspot_strength = hotspots:field("strength")
  local hotspot_seed_cell = hotspots:field("seed_cell")
  local hotspot_seed = cells:field("hotspot_seed")

  local count = cells:count()
  local found = 0
  local plumes = 0
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
      --
      -- Индекс равен МЕТКЕ, а не метке минус один: group_by и accumulate раскладывают по корзине,
      -- равной сырому значению ключа, и корзина 0 означает «метки нет». Запись по метке минус один
      -- склеивала бы в одной строке центр одной области и суммы другой.
      local index = label
      local key = label * 4
      local u1 = unit(seed, key + 1)
      local u2 = unit(seed, key + 2)
      local u3 = unit(seed, key + 3)

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
      plate_seed_cell:set(index, i)

      found = found + 1
    end

    local plume = math.tointeger(hotspot_seed:get(i))
    if plume ~= 0 then
      local index = plume - 1
      hotspot_position:set(index, position:get(i, 0), 0)
      hotspot_position:set(index, position:get(i, 1), 1)
      hotspot_position:set(index, position:get(i, 2), 2)
      hotspot_seed_cell:set(index, i)
      -- Сила плюма разная: одинаковые плюмы дали бы одинаковые цепи, а на планете есть и Гавайи, и
      -- невзрачная подводная банка. Разброс берётся из того же детерминированного хеша, что и оси плит.
      local u = unit(seed, 9000 + plume)
      hotspot_strength:set(index, p.hotspot_height * (1.0 - p.hotspot_variation * u))
      plumes = plumes + 1
    end
  end

  if found < 4 then
    error("tectonics: only " .. found .. " plates were placed; a planet does not assemble from that")
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
  -- 5a. Континентальная масса. Это место, где играбельность перевешивает прямую физику, и перевес
  -- сознательный: континентальной объявлялась целая плита, и материки выходили как повезёт — то один
  -- сверхматерик, то вся суша в одном полушарии. Центры материков теперь разнесены пуассоновским
  -- выбором по всей сфере, а масса спадает от центра, поэтому суша распределена относительно ровно.
  -- Плиты при этом ничего не потеряли: они двигаются и строят горы, а «континентальность» стала
  -- свойством КЛЕТКИ — что и физичнее, потому что настоящая плита несёт и континент, и дно.
  local crust = cells:field("crust")
  local crust_work = cells:field("crust_work")
  local continent_distance = cells:field("continent_distance")

  -- Материки ставятся ГРУППАМИ, а не поодиночке, и это второе требование играбельности рядом с
  -- равномерностью. Пуассоновский выбор по всей сфере разносит центры на одинаковое расстояние, и
  -- получается восемь одинаковых одиночных материков: суша распределена ровно, но крупной связной
  -- суши нет вовсе — largest land mass 13% при 52% до правки. Играть на такой карте значит вечно
  -- плыть. Поэтому сначала выбираются якоря групп, а материки выбираются по счёту «близко к якорю» с
  -- ЯВНЫМ минимальным расстоянием: соседи по группе перекрываются вздутиями и срастаются в большой
  -- материк, а группы между собой остаются разделены океаном.
  originator.poisson_seeds{
    inputs = { position, one, noise },
    outputs = { cells:field("group_seed") },
    params = { target_count = p.continent_group_count, limit = p.continent_group_count },
  }
  originator.graph_flood{
    inputs = { offsets, arcs, cells:field("group_seed"), one, one },
    outputs = { cells:field("scratch_label"), crust_work },
    params = { unreached = 999 },
  }
  originator.decay{
    inputs = { crust_work },
    outputs = { cells:field("shelf_ramp") },
    params = { width = p.continent_group_reach / spacing },
  }
  originator.poisson_seeds{
    inputs = { position, one, cells:field("shelf_ramp") },
    outputs = { cells:field("continent_seed") },
    params = { min_distance = p.continent_spacing, limit = p.continent_count },
  }
  originator.graph_flood{
    inputs = { offsets, arcs, cells:field("continent_seed"), one, one },
    outputs = { cells:field("scratch_label"), continent_distance },
    params = { unreached = 999 },
  }

  -- Океанических островов здесь БОЛЬШЕ НЕТ, и это главная правка второго захода. Раньше они
  -- задавались числом: полторы сотни конусов, разнесённых пуассоновским выбором по сфере. Как игровое
  -- требование это работало («много островов среднего размера»), как мир — нет: россыпь одинаковых
  -- круглых пятен, никак не связанная ни с плитами, ни с их движением.
  --
  -- Теперь острова получаются ДВУМЯ механизмами, у каждого своя причина и своя форма:
  --   горячая точка  — плита едет над неподвижным мантийным плюмом и оставляет ЦЕПЬ (шаг 9a ниже);
  --   островная дуга — поднятие на надвигающейся стороне субдукции, полосой вдоль жёлоба.
  -- Оба дают архипелаги, то есть ровно то, чего просила игра, но из физики, а не из счёта.

  -- Шум сдвигает РАССТОЯНИЕ до центра, а не высоту: так ломается сама форма материка, и берег
  -- получается с заливами и полуостровами, а не окружностью с шумом поверх.
  -- Смещённое расстояние остаётся В СВОЁМ поле, а не в рабочем: из него считается и масса коры, и
  -- материковое вздутие ниже, и оба обязаны видеть один и тот же изломанный край. Разные поля здесь
  -- дали бы берег в одном месте, а подъём внутрь материка — в другом.
  originator.blend{
    inputs = { continent_distance, noise },
    outputs = { continent_distance },
    params = { first = 1.0, second = p.continent_edge_noise / spacing, offset = 0.0 },
  }
  -- Плато внутри и спад на ширину шельфа: масса равна единице до радиуса минус шельф и падает до
  -- нуля на самом радиусе. Спад «от центра» вместо плато давал сушу только в середине материка.
  originator.remap{
    inputs = { continent_distance },
    outputs = { crust },
    params = {
      scale = -spacing / p.continent_shelf,
      offset = p.continent_radius / p.continent_shelf,
      min = 0.0,
      max = 1.0,
    },
  }


  -- 5b. Следы горячих точек. Считаются после материковой массы, потому что нужны и свойства плиты
  -- (полюс, скорость), и сама масса. Результат в метрах и войдёт в рельеф отдельным слагаемым:
  -- остров горячей точки стоит на ОКЕАНИЧЕСКОЙ коре и материком не является.
  originator.hotspot_tracks{
    inputs = {
      position, cells:field("plate_axis"), cells:field("plate_rate"),
      hotspot_position, hotspot_strength,
    },
    outputs = { cells:field("hotspot") },
    params = {
      count = plumes,
      life = p.hotspot_life,
      track_width = p.hotspot_track_width,
      swell_width = p.hotspot_swell_width,
      swell_share = p.hotspot_swell_share,
    },
  }

  -- Цепь — это отдельные вулканы, а не сплошной вал: между Гавайями вода. Та же складчатая маска с
  -- нулевым полом, что режет островную дугу, режет и след горячей точки.
  originator.noise_at{
    inputs = { position },
    outputs = { cells:field("crust_work") },
    params = {
      tree = p.tree, frequency = noise_frequency(p.hotspot_spacing),
      amplitude = 1.0, offset = 0.0, seed_offset = 5501,
    },
  }
  originator.remap{
    inputs = { cells:field("crust_work") },
    outputs = { cells:field("crust_work") },
    params = { absolute = true, scale = -1.0 / 0.9, offset = 1.0, min = p.hotspot_gap, max = 1.0 },
  }
  originator.modulate{ inputs = { cells:field("hotspot"), cells:field("crust_work") }, outputs = { cells:field("hotspot") } }

  -- На континенте плюм не строит вулкан с нуля, а лишь ПОДНИМАЕТ уже существующую кору: Эфиопское
  -- нагорье, а не Гавайи. Без этого множителя плюм под материком добавлял к нему всю высоту
  -- океанической постройки — почти восемь километров, — и площадь выше двух километров вырастала
  -- вчетверо (6.1% поверхности против 1.6% без горячих точек вовсе).
  originator.remap{
    inputs = { crust },
    outputs = { crust_work },
    params = { scale = p.hotspot_continental - 1.0, offset = 1.0 },
  }
  originator.modulate{ inputs = { cells:field("hotspot"), crust_work }, outputs = { cells:field("hotspot") } }

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

  -- 8. Расстояние до ближайшей границы каждого типа И ЕЁ ВЕЛИЧИНЫ.
  --
  -- Заливка несёт НОМЕР клетки границы, а не признак «здесь граница», и это исправление настоящей
  -- ошибки модели, а не украшение. Скорость сближения ненулевая только у клеток, у которых сосед из
  -- другой плиты, то есть на ленте шириной в одну клетку. Поднятие считалось как
  -- uplift * drive * decay(расстояние), но drive обращался в ноль везде, кроме самой границы, где
  -- decay и так равен единице, — значит ВСЕ ширины из конфига (ороген, жёлоб, хребет, рифт) не влияли
  -- ни на что, а горная система была лентой в одну клетку, размазанной потом сглаживанием. Отсюда же
  -- задранное поднятие: его поднимали, чтобы компенсировать несуществующую ширину.
  --
  -- Теперь клетка получает величины СВОЕЙ границы — скорость сближения и сторону субдукции — и спад
  -- от расстояния наконец лепит из них пояс. Сторона важна: у клетки за надвигающейся плитой
  -- ближайшая граница со своей стороны, поэтому она получает горы, а за уходящей вниз — жёлоб.
  local boundary_seed = cells:field("boundary_seed")
  local scratch_label = cells:field("scratch_label")
  local belt_convergence = cells:field("belt_convergence")
  local belt_subduction = cells:field("belt_subduction")

  originator.index{ outputs = { boundary_seed }, params = { offset = 1.0 } }
  originator.modulate{
    inputs = { boundary_seed, cells:field("convergent_seed") },
    outputs = { boundary_seed },
  }
  originator.graph_flood{
    -- unreached большое, а не -1: «границы такого типа нет» должно ГАСНУТЬ в экспоненте спада, а
    -- отрицательное расстояние там выросло бы.
    inputs = { offsets, arcs, boundary_seed, one, one },
    outputs = { scratch_label, cells:field("convergent_distance") },
    params = { unreached = 999 },
  }
  originator.lookup{
    inputs = { scratch_label, cells:field("convergence") },
    outputs = { belt_convergence },
    params = { offset = -1 },
  }
  originator.lookup{
    inputs = { scratch_label, cells:field("subduction") },
    outputs = { belt_subduction },
    params = { offset = -1 },
  }

  -- 9. Изломанность рельефа: ФРАКТАЛЬНЫЙ шум. Одна частота даёт одинаковые бугры одного размера —
  -- у настоящего рельефа спектр близок к 1/f, то есть крупная форма несёт на себе мелкую, а та ещё
  -- более мелкую. Октавы складываются здесь, а не внутри дерева FastNoise2, по той же причине, по
  -- которой здесь вообще стоит lua: число октав зависит от РЕШЁТКИ, а дерево — данные конфига.
  --
  -- Отбрасывание мелких октав обязательно. Октава с длиной волны меньше двух шагов решётки не рельеф,
  -- а шум выборки: на грубой планете она даёт чересполосицу соседних клеток, на мелкой — настоящую
  -- деталь, то есть один и тот же конфиг описывал бы два разных мира.
  local octave_field = cells:field("relief_octave")
  do
    local octaves = math.max(1, math.floor(p.relief_octaves))
    -- Октава мельче двух шагов решётки не рельеф, а шум выборки: на грубой планете она даёт
    -- чересполосицу соседних клеток, на мелкой — настоящую деталь, то есть один и тот же конфиг
    -- описывал бы два разных мира.
    local limit = 2.0 * spacing
    local feature, amplitude, total = p.relief_feature, 1.0, 0.0
    local written = false
    for octave = 1, octaves do
      if written and feature < limit then
        break
      end
      originator.noise_at{
        inputs = { position },
        outputs = { written and octave_field or noise },
        params = {
          tree = p.tree, frequency = noise_frequency(feature), amplitude = amplitude, offset = 0.0,
          -- Своё смещение зерна на октаву: без него все октавы — один и тот же узор в разном
          -- масштабе, и сумма выходит самоподобной до узнаваемости.
          seed_offset = 101 * octave,
        },
      }
      if written then
        originator.blend{ inputs = { noise, octave_field }, outputs = { noise } }
      end
      written = true
      -- Складывается КВАДРАТ амплитуды, потому что нормировка идёт по мощности, а не по сумме.
      -- Сумма амплитуд гарантирует диапазон, но делает фрактал СЛАБЕЕ одной октавы: пики октав почти
      -- никогда не совпадают, поэтому сумма трёх октав имела размах ±0.72, а после деления на 1.85
      -- оставалось ±0.39 — вчетверо меньше объявленной амплитуды детали. Октавы независимы, значит
      -- складываются их дисперсии, и корень из суммы квадратов оставляет размах постоянным при любом
      -- числе октав — ровно то, что здесь и нужно, раз число октав зависит от решётки.
      total = total + amplitude * amplitude
      feature = feature / p.relief_lacunarity
      amplitude = amplitude * p.relief_gain
    end
    -- Нормировка на сумму амплитуд: диапазон шума не должен зависеть от того, сколько октав унесла
    -- решётка, иначе грубая планета оказалась бы ещё и выше мелкой.
    originator.remap{ inputs = { noise }, outputs = { noise }, params = { scale = 1.0 / math.sqrt(total) } }
  end

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
  local base_height = cells:field("base_height")
  local shelf_ramp = cells:field("shelf_ramp")
  local belt = cells:field("belt")
  local roughness = cells:field("roughness")

  -- 10a. Высота коры: материковая поверхность, шельф, континентальный склон и абиссальное дно.
  --
  -- Кривая устроена так, чтобы гипсография планеты вышла ДВУГОРБОЙ — абиссальная равнина и
  -- прибрежная суша часты, склон между ними редок. Два прежних захода этого не дали:
  --   прямая от океанической отметки к континентальной растягивала самый редкий рельеф, склон, на
  --   всю ширину шельфа;
  --   ломаная по доле континентальной массы давала столовую гору — плато массы ровное, и 89% суши
  --   оказывалось в одной высотной полосе при 41% у Земли.
  --
  -- Поэтому поверхность материка задаётся ВЗДУТИЕМ — подъёмом от края к середине, — а шельф и склон
  -- отсчитываются ОТ БЕРЕГА, который не назначается, а ищется: это тот уровень вздутия, выше которого
  -- лежит заданная доля поверхности. Отсюда главное свойство модели: шельф есть при любом размере
  -- материков и любой доле суши. Пока склон был привязан к доле массы, стоило материкам чуть не
  -- совпасть с долей суши, и уровень моря проваливался ниже излома — шельф исчезал целиком, а суша
  -- снова становилась столовой горой (измерено при радиусе 0.47: уровень моря -659 м, 16% поверхности
  -- в полосе 1000..2000 м и 1.0% шельфа).
  local swell = cells:field("swell")

  -- Вздутие материка: подъём от края к середине. Океанические острова в него НЕ входят — они не
  -- материки, а вулканы на океанической коре, и приходят отдельным слагаемым тектоники.
  --
  -- Конус продолжается НИЖЕ нуля за краем материка, и это не косметика. Шельф и склон
  -- строятся как участки этого же поля, поэтому под бровкой обязан оставаться запас: пока вздутие
  -- обрезалось нулём, весь океан оказывался ВЫШЕ подошвы склона, и высота коры вырождалась в само
  -- вздутие — дна у планеты не оставалось вовсе. Островной конус, наоборот, обрезан нулём: остров
  -- только ДОБАВЛЯЕТ высоту, а вычитать её посреди океана ему нечем.
  originator.remap{
    inputs = { continent_distance },
    outputs = { crust_work },
    params = { scale = -spacing / p.continent_radius, offset = 1.0, min = -1.0, max = 1.0 },
  }
  originator.remap{
    inputs = { crust_work },
    outputs = { swell },
    params = { scale = p.continent_swell, offset = p.continental_base },
  }

  -- Берег: уровень вздутия под заданную долю суши. Это та же бисекция, что ищет уровень моря в шаге
  -- surface, и по той же причине — доля суши требование автора мира, а отметка следствие. Здесь она
  -- обходится без маски: свёртка reduce_count_above считает клетки выше порога сама.
  local coast_low = p.continental_base - p.continent_swell - 1.0
  local coast_high = p.continental_base + p.continent_swell + 1.0
  local coast_target = p.land_target * count
  for _ = 1, math.floor(p.sea_level_iterations) do
    local level = 0.5 * (coast_low + coast_high)
    local above = originator.reduce_count_above{ inputs = { swell }, params = { threshold = level } }
    if above > coast_target then
      coast_low = level
    else
      coast_high = level
    end
  end
  local coast_level = 0.5 * (coast_low + coast_high)

  -- Ранняя проверка вместо неявной поломки: если материки покрывают меньше, чем просят суши, берег
  -- сходится к самому краю вздутия, и «сушей» становится вся поверхность разом.
  if coast_level <= p.continental_base + 1.0 then
    error(string.format(
      "tectonics: continents cover less than the requested land fraction %.2f - raise continent_count, "
      .. "continent_radius or continent_group_count, or lower land_target", p.land_target))
  end

  -- Шельф отсчитывается ОТ БЕРЕГА и в метрах: у планеты бровка шельфа лежит на 130-140 м, и это
  -- величина уровня моря, а не свойство коры. Ширина склона задана в радианах и переводится в метры
  -- вздутия через наклон материкового конуса — так склон остаётся узким при любом размере материка.
  local slope_span = math.max(1.0, p.slope_width * p.continent_swell / p.continent_radius)

  -- Дно океана входит в ту же кривую, а не отдельным слагаемым, и это исправление настоящей ошибки
  -- модели. Пока глубина возраста дна вычиталась в правиле расходящейся границы через (1 - crust),
  -- обрыв к абиссали делала ШИРИНА КОРЫ: перепад в три километра ложился на ширину шельфа коры, то
  -- есть на 0.12 рад, и был круче построенного склона. Два механизма спорили за одно и то же место,
  -- побеждал случайный, и берег каждый раз оказывался на обрыве: шельф занимал 1.7% поверхности при
  -- 5% у Земли, а сушей становилась ровно вся материковая площадка целиком.
  --
  -- Теперь кривая одна: дно (глубина по остыванию) → склон → бровка → материковая поверхность.
  local ocean_floor = cells:field("ocean_floor")
  local slope_inverse = cells:field("island_crust") -- поле осталось от прежней модели островов и свободно

  -- Поля затопленного нагорья объявлены ЗДЕСЬ, потому что читает их `build_base`, а заполняются они
  -- ниже, когда посчитаны расстояния до стыков. Пока они нулевые, замена отметки ничего не меняет:
  -- множитель маски равен нулю.
  local basin = cells:field("basin")
  local basin_shape = cells:field("basin_shape")
  originator.fill{ outputs = { basin }, params = { value = 0.0 } }

  local function build_base(shelf_top)
    -- Подъём от подошвы склона к бровке шельфа.
    originator.remap{
      inputs = { swell },
      outputs = { crust_work },
      params = {
        scale = 1.0 / slope_span,
        offset = -(shelf_top - slope_span) / slope_span,
        min = 0.0, max = 1.0,
      },
    }
    originator.remap{
      inputs = { crust_work },
      outputs = { slope_inverse },
      params = { scale = -1.0, offset = 1.0 },
    }
    -- Глубина дна по остыванию: у хребта мелко, на равнине глубоко.
    originator.remap{
      inputs = { age },
      outputs = { ocean_floor },
      params = { scale = -p.abyss_depth, offset = p.oceanic_base },
    }
    originator.modulate{ inputs = { ocean_floor, slope_inverse }, outputs = { base_height } }
    originator.blend{
      inputs = { base_height, crust_work },
      outputs = { base_height },
      params = { first = 1.0, second = shelf_top },
    }
    -- Всё, что выше бровки, идёт по самому вздутию: материковая поверхность — это и есть конус.
    originator.remap{
      inputs = { swell },
      outputs = { shelf_ramp },
      params = { scale = 1.0, offset = -shelf_top, min = 0.0 },
    }
    originator.blend{ inputs = { base_height, shelf_ramp }, outputs = { base_height } }

    -- ЗАТОПЛЕННОЕ НАГОРЬЕ. Отметка коры внутри бассейна ЗАМЕНЯЕТСЯ шельфовой, а не уменьшается на
    -- фиксированную величину, и это разница между работающим механизмом и неработающим. Фиксированная
    -- просадка не знает, где стояла кора: у центра материка отметка на километр выше, чем у края,
    -- поэтому одна и та же просадка в одном месте оставляла сушу нетронутой, а в другом утапливала её
    -- так, что вершины не всплывали. Измерено: 14 островов с НУЛЁМ соседей вместо скопления.
    --
    -- Замена привязана к НАЙДЕННОЙ бровке шельфа, а не к абсолютной отметке, и это тоже по смыслу:
    -- задуговой бассейн — шельфовое море, и глубина его отсчитывается от бровки, как и у всякого
    -- шельфа. Поэтому механизм и живёт внутри `build_base`: только здесь бровка уже известна.
    originator.remap{
      inputs = { basin_shape },
      outputs = { decay_a },
      params = { offset = shelf_top + p.basin_floor },
    }
    originator.blend{
      inputs = { decay_a, base_height },
      outputs = { decay_a },
      params = { first = 1.0, second = -1.0 },
    }
    originator.modulate{ inputs = { decay_a, basin }, outputs = { decay_a } }
    originator.blend{ inputs = { base_height, decay_a }, outputs = { base_height } }
  end

  -- 10b. Складчатая маска горного пояса. Спад от стыка сам по себе даёт гладкий вал; настоящий ороген
  -- состоит из гряд и перевалов. Модуль шума, свёрнутый обратно в единицу, даёт ИЗЛОМ там, где шум
  -- проходит ноль, то есть линию хребта, а не круглый гребень.
  originator.noise_at{
    inputs = { position },
    outputs = { belt },
    params = { tree = p.tree, frequency = noise_frequency(p.belt_feature), amplitude = 1.0, offset = 0.0, seed_offset = 7717 },
  }
  originator.remap{
    inputs = { belt },
    outputs = { belt },
    -- Делитель 0.9 — это наблюдаемый размах шума: без него маска не доходит до пола и пояс остаётся
    -- почти ровным (при поле 0.5 минимум маски был 0.54 вместо 0.5).
    params = { absolute = true, scale = -(1.0 - p.belt_floor) / 0.9, offset = 1.0,
               min = p.belt_floor, max = 1.0 },
  }

  -- 10c. Амплитуда изломанности по месту. Абиссальная равнина ровная, континент неровен, горный пояс
  -- неровен сильнее всего, трансформный стык ломает всё, к чему прикасается. Одинаковая деталь везде
  -- делала дно таким же бугристым, как сушу.
  --
  -- Здесь набираются слагаемые, зависящие от тектоники; слагаемое высоты добавляется ниже, когда
  -- высота коры уже посчитана.
  originator.remap{
    inputs = { cells:field("shear") },
    outputs = { roughness },
    params = { scale = p.shear_roughness, min = 0.0 },
  }

  -- Скорость сближения и расхождения, ограниченная сверху: при встречных плитах на полном ходу
  -- поднятие иначе выросло бы втрое против расчётного.
  originator.remap{
    inputs = { belt_convergence },
    outputs = { drive },
    params = { scale = 1.0 / p.uplift_reference, min = 0.0, max = 1.5 },
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

  -- Широкое нагорье поверх узкого хребта. Настоящий ороген — это не только гряда: за ней лежит
  -- плоскогорье шириной в тысячу километров (Тибет, Альтиплано, Эфиопское нагорье), и на Земле именно
  -- оно занимает бо́льшую часть площади выше двух километров. Один экспоненциальный спад такого не
  -- даёт: измерено 0.6% поверхности в полосе 2000..4000 м против 1.7% у Земли при том, что вершины
  -- уже доходили до 8.7 км. Поэтому спадов ДВА, узкий и широкий, и складываются они до умножения на
  -- складчатую маску: нагорье тоже сложено грядами.
  originator.decay{
    inputs = { cells:field("convergent_distance") },
    outputs = { shelf_ramp },
    params = { width = p.plateau_width / spacing },
  }
  originator.blend{
    inputs = { decay_a, shelf_ramp },
    outputs = { decay_a },
    params = { first = 1.0, second = p.plateau_share },
  }

  -- 10c-bis. ЗАТОПЛЕННОЕ НАГОРЬЕ: третий механизм островов, и он даёт то, чего не дают ни цепь, ни
  -- дуга — СКОПЛЕНИЕ островов вроде греческого архипелага.
  --
  -- Ни плюм, ни дуга такого дать не могут: у обоих острова стоят ЛИНИЕЙ, потому что и след плиты над
  -- плюмом, и вулканический фронт — это линии. Эгейское море устроено иначе и на континентальной, а не
  -- на океанической коре: за Эллинской дугой кора РАСТЯНУТА задуговым расширением почти вдвое, область
  -- от этого просела ниже уровня моря — но складчатый рельеф прежнего пояса остался, и над водой
  -- торчат его вершины. Отсюда и вид: не цепь, а россыпь по площади, все острова мелкие, все на
  -- шельфовой глубине, и все сгруппированы.
  --
  -- Поэтому механизм — не «поднять острова», а ОПУСТИТЬ ГОРНУЮ СТРАНУ. Задаётся тремя вещами: где
  -- растянуто (полоса за стыком на континентальной коре надвигающейся плиты), насколько просело, и
  -- какой рельеф там остался.
  -- Форма маски — ПОЛОСА за дугой, шириной в размер настоящего архипелага. Полоса, а не спад от
  -- границы: у самого стыка идёт сжатие, а не растяжение. И не широкая область: измерено, что спад с
  -- шириной 0.18 радиана накрывал почти всю сушу и превращал материки в решето — расстояние до
  -- сходящегося стыка мало почти везде, потому что стыков много.
  originator.remap{
    inputs = { cells:field("convergent_distance") },
    outputs = { basin },
    params = { scale = 1.0, offset = -p.basin_offset / spacing },
  }
  originator.remap{ inputs = { basin }, outputs = { basin }, params = { absolute = true } }
  originator.decay{ inputs = { basin }, outputs = { basin }, params = { width = p.basin_width / spacing } }
  -- Край полосы РЕЗКИЙ, а не плавный, и это не вкус. У плавного края остаётся полукруглая кайма, где
  -- отметка опущена наполовину: она остаётся сушей и СШИВАЕТ все вершины бассейна с материком — вместо
  -- скопления островов выходит полуостровная бахрома. Насыщение по половине делает полосу
  -- плоскодонной, и тогда единственная суша внутри неё — вершины.
  originator.remap{
    inputs = { basin },
    outputs = { basin },
    params = { scale = 1.0 / p.basin_edge, min = 0.0, max = 1.0 },
  }

  -- Условия задаются ПОРОГАМИ, а не сомножителями, и это второе исправление того же промаха.
  -- Произведение четырёх величин, каждая около половины, даёт четверть — а после насыщения по общему
  -- порогу единицу, то есть «полное растяжение» получалось везде, где хоть что-то из четырёх было.
  -- Порог отвечает на правильный вопрос по каждому условию отдельно: континентальная ли это кора,
  -- надвигающаяся ли это сторона, быстрая ли субдукция.
  --
  -- Сторона НАДВИГАЮЩАЯСЯ, а не тонущая: растягивается верхняя плита, у тонущей растягиваться нечему.
  originator.remap{
    inputs = { belt_subduction },
    outputs = { crust_work },
    params = { scale = -1.0, offset = 1.0 },
  }
  originator.modulate{ inputs = { basin, crust_work }, outputs = { basin } }

  -- Кора КОНТИНЕНТАЛЬНАЯ: на океанической коре за дугой получается не архипелаг, а глубокий задуговой
  -- бассейн, и он в модели уже есть — это платформа дуги.
  originator.remap{
    inputs = { cells:field("crust") },
    outputs = { crust_work },
    params = {
      scale = 1.0 / (1.0 - p.basin_crust_min),
      offset = -p.basin_crust_min / (1.0 - p.basin_crust_min),
      min = 0.0, max = 1.0,
    },
  }
  originator.modulate{ inputs = { basin, crust_work }, outputs = { basin } }

  -- И субдукция БЫСТРАЯ: медленная задугового растяжения не даёт.
  originator.remap{
    inputs = { drive },
    outputs = { crust_work },
    params = {
      scale = 1.0 / p.basin_drive_min,
      offset = 1.0 - 1.0 / p.basin_drive_min,
      min = 0.0, max = 1.0,
    },
  }
  originator.modulate{ inputs = { basin, crust_work }, outputs = { basin } }

  -- Оставшийся рельеф пояса. Шум ГЛАДКИЙ, а не складчатый, и выбран он замером: у складчатого
  -- `1 - |n|` над водой остаётся лента около нулей шума, и при шельфовой глубине эта лента выходит
  -- тоньше клетки — над водой не появлялось почти ничего, а что появлялось, смывала прибрежная
  -- эрозия. У гладкого шума над водой остаются ВЕРШИНЫ, то есть пятна размером в треть длины волны,
  -- и они переживают и решётку, и эрозию. Размер узора в радианах и есть расстояние между островами.
  originator.noise_at{
    inputs = { position },
    outputs = { basin_shape },
    params = {
      tree = p.tree, frequency = noise_frequency(p.basin_relief_scale),
      amplitude = p.basin_relief, offset = 0.0, seed_offset = 8171,
    },
  }

  -- Изломанность в бассейне выше: затопленная горная страна остаётся горной страной.
  originator.modulate{ inputs = { basin, one }, outputs = { crust_work }, params = { scale = p.basin_roughness } }
  originator.blend{ inputs = { roughness, crust_work }, outputs = { roughness } }

  -- ТАМ, ГДЕ РАСТЯГИВАЕТ, НЕ СЖИМАЕТ. Поднятие орогена гасится маской бассейна, и без этого механизм
  -- не работает вовсе: полоса бассейна лежит внутри пояса орогена, поэтому тектоническое поднятие
  -- поднимало опущенную кору обратно, бассейн не тонул, и внутри него вместо скопления островов
  -- получалась суша с прудами — измерено 459 замкнутых водоёмов при 13 островах.
  originator.remap{ inputs = { basin }, outputs = { crust_work }, params = { scale = -1.0, offset = 1.0 } }
  originator.modulate{ inputs = { decay_a, crust_work }, outputs = { decay_a } }

  -- Пояс режет спад поднятия на гряды. Множитель ложится на спад, а не на готовую высоту, потому что
  -- жёлоб грядами не режется: у него другая причина и другая форма.
  originator.modulate{ inputs = { decay_a, belt }, outputs = { decay_a } }

  -- Горная страна тем выше, чем континентальнее кора: Гималаи против Японской дуги. Множитель ложится
  -- на спад орогена, а не на скорость сближения, потому что скорость нужна ещё и островной дуге —
  -- а вот ей континентальность как раз мешает.
  originator.remap{
    inputs = { crust },
    outputs = { crust_work },
    params = { scale = 1.0 - p.arc_uplift, offset = p.arc_uplift },
  }
  originator.modulate{ inputs = { decay_a, crust_work }, outputs = { decay_a } }

  -- Полоса островной дуги: не спад от жёлоба, а ПОЛОСА на расстоянии от него. У настоящей дуги
  -- вулканы стоят там, где погружающаяся плита доходит до глубины плавления, то есть в сотне-двух
  -- километров за жёлобом, а не на самом жёлобе. Полоса собирается из модуля разности расстояний:
  -- сдвинуть, взять модуль, спасть — три вызова, зато форма ровно та.
  local arc_band = cells:field("arc_band")
  local arc_platform = cells:field("arc_platform")
  local arc_segment = cells:field("arc_segment")

  -- Фронт дуги ГУЛЯЕТ относительно жёлоба, а не стоит от него на постоянном расстоянии. У настоящей
  -- дуги угол погружения плиты меняется вдоль стыка, и вместе с ним меняется расстояние до глубины
  -- плавления: у Курил вулканы стоят ближе к жёлобу, чем у Марианской дуги. При постоянном смещении
  -- полоса шла точно вдоль стыка, а расстояние до стыка считается заливкой по графу и оттого гладкое
  -- — отсюда и «ровная форма» полосы, которая и была замечена глазом.
  originator.noise_at{
    inputs = { position },
    outputs = { crust_work },
    params = {
      tree = p.tree, frequency = noise_frequency(p.arc_wander_scale),
      amplitude = p.arc_wander * p.arc_offset / spacing,
      offset = -p.arc_offset / spacing,
      seed_offset = 5171,
    },
  }
  originator.blend{
    inputs = { cells:field("convergent_distance"), crust_work },
    outputs = { arc_band },
  }
  originator.remap{ inputs = { arc_band }, outputs = { arc_band }, params = { absolute = true } }

  -- Дуга РАЗБИТА НА СЕГМЕНТЫ поперечными проливами, и это не косметика. У Алеутской, Курильской и
  -- Идзу-Бонинской дуг сегменты отделены поперечными разломами, где кора не утолщена и цепь
  -- прерывается водой. Без этого утолщённая кора задугового бассейна непрерывна по всей длине дуги,
  -- поднимает её целиком выше уровня моря, и мельчащая вулканы маска уже ничего не решает: измерено
  -- 18 океанических островов, медианой в 254 клетки при вытянутости 4.9, и 11 из 18 длиннее
  -- четырёх — то есть ленты, а не архипелаги.
  --
  -- Маска обратна складчатой: не «оставить у нулей шума», а «убрать у нулей». Нули шума образуют
  -- поверхности, их пересечение с линией дуги — точки, и в этих точках сегмент и разрывается.
  originator.noise_at{
    inputs = { position },
    outputs = { arc_segment },
    params = {
      tree = p.tree, frequency = noise_frequency(p.arc_segment_scale),
      amplitude = 1.0, offset = 0.0, seed_offset = 7717,
    },
  }
  originator.remap{
    inputs = { arc_segment },
    outputs = { arc_segment },
    params = {
      absolute = true,
      scale = (1.0 - p.arc_segment_gap) / p.arc_segment_width,
      offset = p.arc_segment_gap,
      min = p.arc_segment_gap, max = 1.0,
    },
  }

  -- Дуга состоит из ДВУХ частей, и без обеих она не работает. Широкая и низкая — утолщённая кора
  -- задугового бассейна: она поднимает дно до пары километров, то есть делает мелкое море. Узкая и
  -- высокая — сами вулканы. Без платформы вулкану пришлось бы подниматься с шести километров, и
  -- остров получался бы либо горой в четыре километра, либо ничем: измерено 19 кусков суши из 36 и
  -- 5.1% поверхности выше двух километров, то есть дуга выходила подводной стеной, а не архипелагом.
  originator.decay{
    inputs = { arc_band },
    outputs = { arc_platform },
    params = { width = p.arc_platform_width / spacing, amplitude = p.arc_platform_share },
  }
  originator.decay{ inputs = { arc_band }, outputs = { arc_band }, params = { width = p.arc_width / spacing } }

  -- Дуга — это ЦЕПЬ вулканов, а не сплошная стена: между Курилами и Алеутами вода. Складчатая маска
  -- с нулевым полом режет полосу на отдельные постройки, и расстояние между ними задаётся в радианах,
  -- как и всё остальное. Платформу маска НЕ режет: кора утолщена по всей дуге.
  originator.noise_at{
    inputs = { position },
    outputs = { crust_work },
    params = {
      tree = p.tree, frequency = noise_frequency(p.arc_spacing),
      amplitude = 1.0, offset = 0.0, seed_offset = 4093,
    },
  }
  originator.remap{
    inputs = { crust_work },
    outputs = { crust_work },
    params = { absolute = true, scale = -1.0 / 0.9, offset = 1.0, min = 0.0, max = 1.0 },
  }
  originator.modulate{ inputs = { arc_band, crust_work }, outputs = { arc_band } }
  -- Маска сегментов ложится на ОБЕ части: в проливе нет ни утолщённой коры, ни вулкана. Только на
  -- платформу её мало — вулкан, попавший на пролив, замкнул бы его обратно в ленту.
  originator.modulate{ inputs = { arc_band, arc_segment }, outputs = { arc_band } }
  originator.modulate{ inputs = { arc_platform, arc_segment }, outputs = { arc_platform } }
  originator.blend{ inputs = { arc_band, arc_platform }, outputs = { arc_band } }

  -- Горы неровны там, где они растут: изломанность идёт от того же произведения, что и поднятие.
  originator.modulate{ inputs = { drive, decay_a }, outputs = { crust_work }, params = { scale = p.roughness_orogen } }
  originator.blend{ inputs = { roughness, crust_work }, outputs = { roughness } }

  originator.run_script{
    program = step.programs.relief_convergent,
    inputs = { belt_subduction, drive, decay_a, decay_b, crust, arc_band },
    outputs = { height },
    params = {
      uplift = p.uplift,
      trench = p.trench,
      arc = p.arc_height,
    },
  }

  -- Те же два поля спада переиспользуются для расходящейся границы: время жизни буфера — решение
  -- скрипта, и здесь оно очевидное.
  -- Расходящаяся граница считается так же: заливка от НОМЕРОВ её клеток, и хребет получает скорость
  -- расхождения своей границы, а не нулевую скорость собственной клетки.
  originator.index{ outputs = { boundary_seed }, params = { offset = 1.0 } }
  originator.modulate{
    inputs = { boundary_seed, cells:field("divergent_seed") },
    outputs = { boundary_seed },
  }
  originator.graph_flood{
    inputs = { offsets, arcs, boundary_seed, one, one },
    outputs = { scratch_label, cells:field("divergent_distance") },
    params = { unreached = 999 },
  }
  originator.lookup{
    inputs = { scratch_label, cells:field("convergence") },
    outputs = { belt_convergence },
    params = { offset = -1 },
  }
  originator.remap{
    inputs = { belt_convergence },
    outputs = { open_drive },
    params = { scale = -1.0 / p.uplift_reference, min = 0.0, max = 1.5 },
  }

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
  -- Возраст дна: насколько оно успело остыть и просесть, от нуля у хребта до единицы на равнине.
  --
  -- Кривая НАСЫЩАЮЩАЯСЯ, а не прямая, и это и физика, и измерение сразу. Модель остывания плиты даёт
  -- выход на равновесную глубину, а не бесконечное погружение; прямая же делала фланг хребта круче
  -- суши — средний перепад между соседями на «абиссальной равнине» выходил 198 м против 151 м на
  -- суше, то есть самая ровная поверхность планеты оказывалась самой неровной. Заодно исчезла
  -- зависимость от решётки: прямая задавалась метрами на ШАГ заливки и на мелкой планете топила
  -- океан вдвое глубже.
  originator.decay{
    inputs = { cells:field("divergent_distance") },
    outputs = { age },
    params = { width = p.abyss_width / spacing, amplitude = -1.0, offset = 1.0 },
  }

  -- Срединный хребет — самый изломанный рельеф океана: он молодой, его ещё не засыпало осадками.
  originator.modulate{ inputs = { open_drive, decay_a }, outputs = { crust_work }, params = { scale = p.roughness_ridge } }
  originator.blend{ inputs = { roughness, crust_work }, outputs = { roughness } }

  -- Сама изломанность НЕ складывается здесь: её накладывает шаг surface уже после сглаживания.
  -- Сглаживание убирает ступеньки заливки по графу, то есть артефакт решётки в тектоническом
  -- сигнале, а фрактальная деталь к этому сигналу не относится — два прохода размытия съедали почти
  -- половину её амплитуды.
  originator.run_script{
    program = step.programs.relief_divergent,
    inputs = {
      cells:field("crust"),
      open_drive,
      decay_a,
      decay_b,
      age,
    },
    outputs = { part },
    params = {
      ridge = p.ridge,
      rift = p.rift,
    },
  }

  -- 10d. Сборка. Сначала складываются ТЕКТОНИЧЕСКИЕ добавки, потом под них подкладывается высота коры.
  --
  -- Бровка шельфа ищется БИСЕКЦИЕЙ, и это не про точность, а про связность модели. Два условия
  -- обязаны выполняться одновременно: бровка лежит на shelf_drop ниже уровня моря, а уровень моря
  -- стоит там, где суши ровно столько, сколько заказано. Условия зацеплены — поднять бровку значит
  -- уронить полосу мелководья на дно, то есть убрать сушу и опустить уровень моря, — и поэтому
  -- пересчитывать их по очереди нельзя: измерено, что оценка расходилась (бровка 166, а настоящий
  -- уровень моря -618, то есть на 780 м ниже собственной бровки, и берег снова садился на обрыв).
  --
  -- Зацепление снимается тем, что оба условия сводятся к ОДНОМУ: клеток выше отметки
  -- «бровка + shelf_drop» должно быть ровно столько, сколько просит доля суши. Это одна свёртка на
  -- шаг бисекции, а не вложенный поиск уровня.
  originator.blend{ inputs = { height, part }, outputs = { part } }
  originator.blend{ inputs = { part, cells:field("hotspot") }, outputs = { part } }


  local shelf_low = p.continental_base - p.continent_swell - p.shelf_drop
  local shelf_high = p.continental_base + p.continent_swell
  local shelf_top = coast_level - p.shelf_drop
  for _ = 1, math.floor(p.sea_level_iterations) do
    shelf_top = 0.5 * (shelf_low + shelf_high)
    build_base(shelf_top)
    originator.blend{ inputs = { base_height, part }, outputs = { height } }

    local above = originator.reduce_count_above{
      inputs = { height },
      params = { threshold = shelf_top + p.shelf_drop },
    }
    -- Выше бровка — меньше суши, поэтому направление именно такое.
    if above > coast_target then
      shelf_low = shelf_top
    else
      shelf_high = shelf_top
    end
  end

  -- Последняя сборка идёт по СЕРЕДИНЕ найденного отрезка, а не по последней пробе: проба могла быть
  -- любым из концов, и мир не должен зависеть от того, каким именно.
  shelf_top = 0.5 * (shelf_low + shelf_high)
  build_base(shelf_top)
  originator.blend{ inputs = { base_height, part }, outputs = { height } }

  -- 10e. Слагаемое высоты в изломанности. Оно последнее, потому что читает готовую высоту коры.
  -- Изломанность растёт С ВЫСОТОЙ над бровкой шельфа, а не с долей континентальной массы: шельф и
  -- прибрежная равнина — засыпанные осадком поверхности, они ровные, а кора под ними такая же
  -- континентальная, как под нагорьем. По массе коры выходило иначе — деталь в 550 м накладывалась на
  -- шельф толщиной в 140 м и рвала его в клочья.
  originator.remap{
    inputs = { base_height },
    outputs = { crust_work },
    params = {
      scale = (p.roughness_land - p.roughness_abyss) / p.roughness_relief_span,
      offset = p.roughness_abyss - (p.roughness_land - p.roughness_abyss) * shelf_top / p.roughness_relief_span,
      min = p.roughness_abyss,
      max = p.roughness_land,
    },
  }
  originator.blend{ inputs = { roughness, crust_work }, outputs = { roughness } }

  -- 11. Раскладка клеток по плитам и сводка. Диапазон scatter относится ко ВХОДАМ: число корзин
  -- задаёт буфер смещений, а не число обрабатываемых клеток.
  originator.group_by{
    inputs = { plate },
    outputs = { step.writes.plate_offsets:field("start"), step.writes.plate_order:field("cell") },
    key_support = "global",
  }

  -- Кора плиты — теперь СВОДКА по её клеткам, а не жребий: континентальность стала свойством клетки,
  -- и запись плиты обязана говорить правду о том, что на ней лежит.
  originator.accumulate{
    inputs = { plate, cells:field("crust") },
    outputs = { plate_crust },
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
