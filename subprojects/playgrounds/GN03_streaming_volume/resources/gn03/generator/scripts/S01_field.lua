-- Тело шага field: поле плотности одного чанка.
--
-- Всё, что здесь вызывается, — апертура pointwise, и это не совпадение, а условие чанкования: узел
-- считается по своей МИРОВОЙ позиции и не смотрит на соседей, поэтому чанк даёт ровно те же числа,
-- что и проход по всему миру (у GN01 это измерено: расхождение ровно ноль на 262144 клетках).
--
-- Ключ чанка превращается в мировое смещение ЗДЕСЬ, и только здесь: инструмент про чанки не знает,
-- ему приходит начало координат параметром. Начало берётся на один узел РАНЬШЕ начала чанка — это и
-- есть полоса перекрытия, из-за которой у каждого угла клетки есть сосед с обеих сторон, а значит
-- нормаль на шве считается по одним и тем же числам у обоих чанков.
--
-- ДВЕ РЕШЁТКИ ПОЗИЦИЙ, и это не дублирование:
--   `position` — мировая, от неё считается ПОЛЕ. Мир не имеет права зависеть от наблюдателя;
--   `local`    — та же решётка от начала чанка, из неё строится ГЕОМЕТРИЯ. Арена хранит вершины в
--                координатах своего чанка, а смещение чанка относительно чанка камеры лежит в
--                маленькой таблице на кадр.
--
-- БИОМЫ: СМЕШИВАЮТСЯ ПРАВИЛА, А НЕ КАРТИНКА.
--
-- Биом здесь — набор ЧИСЕЛ (амплитуды, градиент, пещеры, оттенок) плюс место в климатической
-- плоскости. Плотность считается по правилу каждого биома, ПРИСУТСТВУЮЩЕГО в этом чанке, и
-- складывается со своим весом; вес — гладкая функция климата, поэтому переход между биомами
-- непрерывен по построению, и шва между ними не бывает, как не бывает шва между чанками.
--
-- Два решения, без которых это не работает:
--
--   ВЕС С КОМПАКТНЫМ НОСИТЕЛЕМ. Вес обращается в НОЛЬ за своим радиусом, а не «почти в ноль», как у
--   экспоненты. Только поэтому «считать лишь присутствующие биомы» — не приближение, а точное
--   равенство: отброшенный биом ничего не весил бы и в полном расчёте. Это проверяется побитово.
--
--   ЦЕНА — ЧИСЛО БИОМОВ В ЧАНКЕ, А НЕ В МИРЕ. Присутствие определяется по диапазону климата в чанке
--   (две свёртки на поле), и в него обычно попадает один-три биома из пяти. Мир с сотней биомов стоил
--   бы столько же, пока в чанке их по-прежнему три.
--
-- Слои шума считаются ОДИН раз на все биомы: биомы отличаются амплитудами и порогами, а не размерами
-- формы. Как только биому понадобится ДРУГОЙ размер формы, шум придётся пересчитывать на биом — и
-- это будет настоящая цена такого биома, а не эта.

return function(step)
  local samples = step.writes.samples
  local p = step.params

  local position = samples:field("position")
  local local_position = samples:field("local")
  local relief = samples:field("relief")
  local detail = samples:field("detail")
  local cave = samples:field("cave")
  local warmth = samples:field("warmth")
  local wetness = samples:field("wetness")
  local climate_a = samples:field("climate_a")
  local climate_b = samples:field("climate_b")
  local weight = samples:field("weight")
  local biome_density = samples:field("biome_density")
  local blend_num = samples:field("blend_num")
  local blend_den = samples:field("blend_den")
  local shade_num = samples:field("shade_num")
  local biome_shade = samples:field("biome_shade")
  local island = samples:field("island")
  local gate = samples:field("gate")
  local lift = samples:field("lift")
  local route_distance = samples:field("route_distance")
  local bunker_distance = samples:field("bunker_distance")
  local density = samples:field("density")
  local island_density = samples:field("island_density")

  local cells = p.chunk_cells
  local cell = p.cell_size
  local side = cells + 3

  -- Первый узел решётки: начало чанка минус один узел перекрытия.
  local origin_x = (step.chunk.x * cells - 1) * cell
  local origin_y = (step.chunk.y * cells - 1) * cell
  local origin_z = (step.chunk.z * cells - 1) * cell

  originator.position_grid{
    outputs = { position },
    params = {
      size_x = side, size_y = side, cell_size = cell,
      origin_x = origin_x, origin_y = origin_y, origin_z = origin_z,
    },
  }

  -- Та же решётка, но от начала чанка: узел с индексом 1 попадает ровно в ноль.
  originator.position_grid{
    outputs = { local_position },
    params = {
      size_x = side, size_y = side, cell_size = cell,
      origin_x = -cell, origin_y = -cell, origin_z = -cell,
    },
  }

  -- РАЗМЕР ФОРМЫ В МЕТРАХ, а не частотой, и переводит одно в другое ДВИЖОК: масштаб дерева шума он
  -- измеряет сам. Предполагать масштаб чужих данных нельзя — это уже стоило ошибки в девяносто раз.
  --
  -- Амплитуды здесь ЕДИНИЧНЫЕ: масштабирует слои каждый биом по-своему, поэтому сам слой обязан быть
  -- «сырым», иначе биому досталась бы чужая амплитуда.
  originator.noise_at{
    inputs = { position }, outputs = { relief },
    params = { tree = p.tree, feature = p.relief_feature, amplitude = 1.0 },
  }
  originator.noise_at{
    inputs = { position }, outputs = { detail },
    params = { tree = p.tree, feature = p.detail_feature, amplitude = 1.0, seed_offset = 1721 },
  }
  originator.noise_at{
    inputs = { position }, outputs = { cave },
    params = { tree = p.tree, feature = p.cave_feature, amplitude = 1.0, seed_offset = 90113 },
  }

  -- КЛИМАТ: две крупные величины, по которым биом узнаёт своё место. Размер формы у них НАМНОГО
  -- больше рельефа — биом должен занимать область, а не пятно, иначе мир превращается в мозаику, где
  -- ни один биом не успевает себя показать.
  originator.noise_at{
    inputs = { position }, outputs = { warmth },
    params = { tree = p.tree, feature = p.climate_feature, amplitude = 1.0, seed_offset = 31337 },
  }
  originator.noise_at{
    inputs = { position }, outputs = { wetness },
    -- Второй климат считается на ДРУГОМ размере формы, а не только с другим зерном: одинаковый
    -- размер сделал бы две величины похожими по рисунку, и биомы легли бы полосами.
    params = { tree = p.tree, feature = p.climate_feature * 0.72, amplitude = 1.0, seed_offset = 6151 },
  }

  -- ПРИСУТСТВИЕ БИОМОВ. Диапазон климата в чанке — две свёртки на поле, и по нему сразу видно, каких
  -- биомов здесь быть не может. Проверка КОНСЕРВАТИВНА: биом отбрасывается только если его радиус не
  -- достаёт до диапазона, а за радиусом вес ровно нулевой — поэтому это не приближение.
  local warm_low = originator.reduce_min{ inputs = { warmth } }
  local warm_high = originator.reduce_max{ inputs = { warmth } }
  local wet_low = originator.reduce_min{ inputs = { wetness } }
  local wet_high = originator.reduce_max{ inputs = { wetness } }

  local function distance_to_range(value, low, high)
    if value < low then return low - value end
    if value > high then return value - high end
    return 0.0
  end

  local biome_count = math.tointeger(p.biome_count) or math.floor(p.biome_count)
  local force_all = (p.force_all_biomes or 0) > 0.5
  local present = {}
  for i = 1, biome_count do
    local reach = p["biome_" .. i .. "_reach"]
    local dw = distance_to_range(p["biome_" .. i .. "_warmth"], warm_low, warm_high)
    local dh = distance_to_range(p["biome_" .. i .. "_wetness"], wet_low, wet_high)
    if force_all or dw * dw + dh * dh <= reach * reach then
      present[#present + 1] = i
    end
  end

  -- Сколько биомов участвовало — в состояние: цена биомов равна их числу В ЧАНКЕ, и это число должно
  -- быть видно, а не подразумеваться.
  step.writes.state:field("biomes_used"):set(0, #present)

  originator.fill{ outputs = { blend_num }, params = { value = 0.0 } }
  originator.fill{ outputs = { blend_den }, params = { value = 0.0 } }
  originator.fill{ outputs = { shade_num }, params = { value = 0.0 } }

  for _, i in ipairs(present) do
    local reach = p["biome_" .. i .. "_reach"]

    -- ВЕС: (1 - r²/reach²)², гладкий бугор с КОМПАКТНЫМ носителем. За радиусом ровно ноль — именно
    -- это делает отбрасывание далёких биомов точным, а не приблизительным.
    originator.remap{ inputs = { warmth }, outputs = { climate_a },
                      params = { offset = -p["biome_" .. i .. "_warmth"] } }
    originator.modulate{ inputs = { climate_a, climate_a }, outputs = { weight } }
    originator.remap{ inputs = { wetness }, outputs = { climate_a },
                      params = { offset = -p["biome_" .. i .. "_wetness"] } }
    originator.modulate{ inputs = { climate_a, climate_a }, outputs = { climate_b } }
    originator.blend{ inputs = { weight, climate_b }, outputs = { weight } }
    originator.remap{ inputs = { weight }, outputs = { weight },
                      params = { scale = 1.0 / (reach * reach), max = 1.0 } }
    originator.remap{ inputs = { weight }, outputs = { weight },
                      params = { scale = -1.0, offset = 1.0, min = 0.0 } }
    originator.modulate{ inputs = { weight, weight }, outputs = { weight } }

    -- ПЛОТНОСТЬ ПО ПРАВИЛУ ЭТОГО БИОМА: запас по высоте со своим градиентом, рельеф и детали со
    -- своими амплитудами, пещеры со своей шириной и силой.
    originator.remap{
      inputs = { position }, outputs = { biome_density },
      params = {
        component = 1, scale = -p["biome_" .. i .. "_gradient"],
        offset = p.surface_level * p["biome_" .. i .. "_gradient"],
      },
    }
    originator.blend{ inputs = { biome_density, relief }, outputs = { biome_density },
                      params = { first = 1.0, second = p["biome_" .. i .. "_relief"] } }
    originator.blend{ inputs = { biome_density, detail }, outputs = { biome_density },
                      params = { first = 1.0, second = p["biome_" .. i .. "_detail"] } }

    local cave_width = p["biome_" .. i .. "_cave_width"]
    if cave_width > 0.0 then
      -- Ход идёт там, где слой шума проходит через нуль: нулевая изолиния — поверхность, её
      -- окрестность — труба, поэтому получаются ходы, а не шары.
      originator.remap{ inputs = { cave }, outputs = { climate_b },
                        params = { absolute = true, scale = -1.0 / cave_width, offset = 1.0, min = 0.0 } }
      originator.blend{ inputs = { biome_density, climate_b }, outputs = { biome_density },
                        params = { first = 1.0, second = -p["biome_" .. i .. "_cave_strength"] } }
    end

    -- Накопление взвешенной суммы и суммы весов. Оттенок накапливается ТЕМ ЖЕ весом, поэтому цвет
    -- переходит между биомами так же гладко, как и форма — и по картинке видно ровно то, что
    -- посчитано, а не отдельная раскраска.
    originator.modulate{ inputs = { biome_density, weight }, outputs = { climate_b } }
    originator.blend{ inputs = { blend_num, climate_b }, outputs = { blend_num } }
    originator.blend{ inputs = { blend_den, weight }, outputs = { blend_den } }
    originator.blend{ inputs = { shade_num, weight }, outputs = { shade_num },
                      params = { first = 1.0, second = p["biome_" .. i .. "_shade"] } }
  end

  -- Нормировка. Сумма весов не бывает нулевой по построению: базовый биом накрывает всю
  -- климатическую плоскость, и его радиус объявлен именно для этого.
  originator.ratio{ inputs = { blend_num, blend_den }, outputs = { density },
                    params = { minimum_divisor = 1.0e-6 } }
  originator.ratio{ inputs = { shade_num, blend_den }, outputs = { biome_shade },
                    params = { minimum_divisor = 1.0e-6 } }

  -- ВОЗДУШНЫЕ ОСТРОВА. Выше пола островов мир не кончается: там висят отдельные глыбы. Собираются они
  -- НАЛОЖЕНИЕМ, а не суммой: остров поднимается над пустотой, а не прибавляется к запасу плотности —
  -- с суммой он выходил бы тем толще, чем ниже висит.
  if p.island_amplitude > 0.0 then
    originator.noise_at{
      inputs = { position }, outputs = { island },
      params = { tree = p.tree, feature = p.island_feature, amplitude = 1.0, seed_offset = 5311 },
    }
    -- Ворота по высоте: ниже пола островов нет вовсе, выше они разгораются на island_soft метрах.
    -- Резкий пол дал бы всем островам ровное плоское днище на одной высоте.
    originator.remap{
      inputs = { position }, outputs = { gate },
      params = {
        component = 1, scale = 1.0 / p.island_soft,
        offset = -p.island_floor / p.island_soft, min = 0.0, max = 1.0,
      },
    }
    originator.remap{
      inputs = { position }, outputs = { lift },
      params = { component = 1, scale = p.island_fade, offset = -p.island_floor * p.island_fade, min = 0.0 },
    }
    originator.modulate{ inputs = { island, gate }, outputs = { island_density },
                         params = { scale = p.island_amplitude } }
    originator.blend{ inputs = { island_density, lift }, outputs = { island_density },
                      params = { first = 1.0, second = -1.0, offset = -p.island_threshold } }
    originator.maximum{ inputs = { density, island_density }, outputs = { density } }
  else
    originator.fill{ outputs = { island }, params = { value = 0.0 } }
    originator.fill{ outputs = { gate }, params = { value = 0.0 } }
    originator.fill{ outputs = { lift }, params = { value = 0.0 } }
    originator.fill{ outputs = { island_density }, params = { value = 0.0 } }
  end

  -- КОРИДОРЫ КАРКАСА. Здесь двухмасштабный генератор сходится: грубый проход проложил маршруты через
  -- геймплейные узлы, а чанк превращает их в поле — расстояние до ломаной, из которого вырезается
  -- проходимая труба. Маршрут приходит ВХОДОМ пайплайна: его заполняет хост запросом по области.
  --
  -- ДВА СТИЛЯ, И РАЗЛИЧАЕТ ИХ НЕ ЧИСЛО, А ГЕОМЕТРИЯ. Естественная пещера — круглое сечение
  -- (евклидова метрика) со стенами, разбитыми шумом; тоннель бункера — угловатое сечение (метрика
  -- Чебышёва, поверхность уровня там куб) и ровные стены. Поэтому это ДВА ВЫЗОВА с разными метриками,
  -- а не один вызов с коэффициентом: форму сечения коэффициентом не задать.
  --
  -- Режутся коридоры ПОСЛЕДНИМИ: они проходимые, значит ничто не имеет права их засыпать. Пока это
  -- стояло до островов, остров, попавший на маршрут, честно наполнял тоннель обратно.
  if p.corridor_radius > 0.0 then
    originator.polyline_distance{
      inputs = { position, step.reads.route_points:field("position"), step.reads.route_offsets:field("offset") },
      outputs = { route_distance },
      params = { max_distance = p.corridor_radius + p.corridor_falloff },
    }
    -- Стены естественной пещеры разбиваются тем же слоем деталей: радиус перестаёт быть постоянным, и
    -- труба читается как промытая водой, а не как просверлённая.
    originator.blend{ inputs = { route_distance, detail }, outputs = { route_distance },
                      params = { first = 1.0, second = p.corridor_roughness } }
    originator.remap{
      inputs = { route_distance }, outputs = { route_distance },
      params = { scale = p.corridor_wall, offset = -p.corridor_radius * p.corridor_wall },
    }
    originator.minimum{ inputs = { density, route_distance }, outputs = { density } }

    originator.polyline_distance{
      inputs = { position, step.reads.bunker_points:field("position"), step.reads.bunker_offsets:field("offset") },
      outputs = { bunker_distance },
      params = { max_distance = p.bunker_radius + p.corridor_falloff, metric = "chebyshev" },
    }
    originator.remap{
      inputs = { bunker_distance }, outputs = { bunker_distance },
      -- Стена бункера круче: у рукотворного тоннеля кромка резкая, а не размытая.
      params = { scale = p.bunker_wall, offset = -p.bunker_radius * p.bunker_wall },
    }
    originator.minimum{ inputs = { density, bunker_distance }, outputs = { density } }
  else
    originator.fill{ outputs = { route_distance }, params = { value = 0.0 } }
    originator.fill{ outputs = { bunker_distance }, params = { value = 0.0 } }
  end
end
