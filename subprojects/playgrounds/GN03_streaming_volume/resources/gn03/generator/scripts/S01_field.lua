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
--                маленькой таблице на кадр. Поэтому уход камеры из чанка стоит обновления таблицы,
--                а не переписывания арены, и точность не зависит от того, как далеко улетел
--                наблюдатель.

return function(step)
  local samples = step.writes.samples
  local p = step.params

  local position = samples:field("position")
  local local_position = samples:field("local")
  local relief = samples:field("relief")
  local detail = samples:field("detail")
  local cave = samples:field("cave")
  local depth = samples:field("depth")
  local island = samples:field("island")
  local gate = samples:field("gate")
  local lift = samples:field("lift")
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
  -- измеряет сам (период при частоте 1, зерно ноль, кэш по тексту дерева). Раньше этот перевод жил
  -- здесь, множителем `noise_period` из конфига, и именно он однажды соврал в девяносто раз: «190
  -- метров» означали 17 километров, чанк видел поле постоянным, и мир выходил пустым. Предполагать
  -- масштаб чужих данных нельзя — его надо измерить, и теперь это делает тот, кто их читает.
  originator.noise_at{
    inputs = { position },
    outputs = { relief },
    params = { tree = p.tree, feature = p.relief_feature, amplitude = p.relief_amplitude },
  }

  originator.noise_at{
    inputs = { position },
    outputs = { detail },
    -- Смещение зерна делает слой независимым, не заводя второго зерна шага: зерно приходит от
    -- имени шага и от мирового, а смещение отличает слои внутри одного шага.
    params = {
      tree = p.tree, feature = p.detail_feature, amplitude = p.detail_amplitude,
      seed_offset = 1721,
    },
  }

  -- Запас плотности по высоте: (опорная высота - y) * градиент. Компонента вектора достаётся
  -- инструментом, а не правилом: правило на devils_script компоненту достать не может.
  originator.remap{
    inputs = { position },
    outputs = { depth },
    params = { component = 1, scale = -p.gradient, offset = p.surface_level * p.gradient },
  }

  -- Складывает слои КОНФИГ, а не движок: инструмент считает арифметику, а из каких слагаемых
  -- состоит мир — это и есть смысл, и он остаётся здесь.
  originator.blend{ inputs = { depth, relief }, outputs = { density }, params = { first = 1.0, second = 1.0 } }
  originator.blend{ inputs = { density, detail }, outputs = { density }, params = { first = 1.0, second = 1.0 } }

  -- Пещеры. Ход идёт там, где ТРЕТИЙ слой шума проходит через нуль: нулевая изолиния шума — это
  -- поверхность, а её окрестность — труба, поэтому получаются ходы, а не шары. Полоса |шум| <
  -- cave_width и есть толщина хода, а модуль до умножения даёт из шума именно расстояние до нуля.
  if p.cave_width > 0.0 and p.cave_strength > 0.0 then
    originator.noise_at{
      inputs = { position },
      outputs = { cave },
      params = { tree = p.tree, feature = p.cave_feature, amplitude = 1.0, seed_offset = 90113 },
    }
    originator.remap{
      inputs = { cave },
      outputs = { cave },
      params = { absolute = true, scale = -1.0 / p.cave_width, offset = 1.0, min = 0.0 },
    }
    originator.blend{
      inputs = { density, cave },
      outputs = { density },
      params = { first = 1.0, second = -p.cave_strength },
    }
  else
    -- Ноль ширины означает «пещер нет»: делить на неё нельзя, а тихо оставить прошлый слой в буфере
    -- было бы худшим из вариантов — чанк начал бы зависеть от того, что считалось до него.
    originator.fill{ outputs = { cave }, params = { value = 0.0 } }
  end

  -- ВОЗДУШНЫЕ ОСТРОВА. Выше пола островов мир не кончается: там висят отдельные глыбы.
  --
  -- Собираются они НАЛОЖЕНИЕМ, а не суммой, и это тот же довод, что у планеты про остров над дном:
  -- остров поднимается над пустотой, а не прибавляется к запасу плотности. С суммой он выходил бы
  -- тем толще, чем ниже висит, и на уровне земли превратился бы во второй слой рельефа.
  if p.island_amplitude > 0.0 then
    originator.noise_at{
      inputs = { position },
      outputs = { island },
      params = { tree = p.tree, feature = p.island_feature, amplitude = 1.0, seed_offset = 5311 },
    }

    -- Ворота по высоте: ниже пола островов нет вовсе, выше они разгораются на island_soft метрах.
    -- Мягкость нужна не для красоты — с резким полом у всех островов оказывается ровное плоское
    -- днище на одной высоте, и это видно с первого взгляда.
    originator.remap{
      inputs = { position },
      outputs = { gate },
      params = {
        component = 1, scale = 1.0 / p.island_soft,
        offset = -p.island_floor / p.island_soft, min = 0.0, max = 1.0,
      },
    }

    -- Спад с высотой: сколько плотности остров теряет на метр подъёма. Отсюда и высота, на которой
    -- острова кончаются сами: (амплитуда - порог) / island_fade метров над полом.
    originator.remap{
      inputs = { position },
      outputs = { lift },
      params = { component = 1, scale = p.island_fade, offset = -p.island_floor * p.island_fade, min = 0.0 },
    }

    originator.modulate{
      inputs = { island, gate },
      outputs = { island_density },
      params = { scale = p.island_amplitude },
    }
    originator.blend{
      inputs = { island_density, lift },
      outputs = { island_density },
      params = { first = 1.0, second = -1.0, offset = -p.island_threshold },
    }

    originator.maximum{ inputs = { density, island_density }, outputs = { density } }
  else
    originator.fill{ outputs = { island }, params = { value = 0.0 } }
    originator.fill{ outputs = { gate }, params = { value = 0.0 } }
    originator.fill{ outputs = { lift }, params = { value = 0.0 } }
    originator.fill{ outputs = { island_density }, params = { value = 0.0 } }
  end
end
