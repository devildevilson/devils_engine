-- Тело шага props: сущности чанка.
--
-- Здесь lua работает ровно в своей роли и на своём масштабе. Правило библиотеки не «lua не трогает
-- данные», а «lua обходит только те множества, которые сама перечислила»: сорок столбцов по
-- тридцать пять узлов — это чуть больше тысячи обращений к полю, то есть десятые доли миллисекунды
-- против шести миллисекунд самого чанка. Поэлементный обход миллионов узлов остался бы у ядра.
--
-- ДВА ЗЕРНА, И ЗДЕСЬ ВАЖНО ВЗЯТЬ ВТОРОЕ. Поле мира считается по `step.seed` (без ключа), потому что
-- оно обязано быть НЕПРЕРЫВНЫМ через шов чанка. Разброс сущностей — наоборот: он обязан быть
-- НЕЗАВИСИМЫМ в каждом чанке, иначе один и тот же хеш по мировой позиции дал бы соседним чанкам
-- согласованный узор, а при смене размера чанка всё разъехалось бы. Поэтому берётся
-- `step.chunk_seed` = hash(зерно мира, имя шага, ключ чанка).
--
-- Хеш — `base.prng64_2` из движка, а не свой: пока каждый скрипт держал свой splitmix, «то же
-- зерно» означало «то же зерно в этом файле».
--
-- ПОЗИЦИЯ ЛОКАЛЬНАЯ, от начала чанка: мировое место — это целый ключ чанка плюс это смещение.
-- Абсолютной позиции у сущности нет, как и у вершины.

return function(step)
  local samples = step.reads.samples
  local props = step.writes.props
  local state = step.writes.state
  local p = step.params

  local density = samples:field("density")
  local out_position = props:field("position")
  local out_normal = props:field("normal")
  local out_size = props:field("size")
  local out_kind = props:field("kind")
  local capacity = props:count()

  -- ЦЕЛЫЕ ЯВНО. Числа конфига приходят в lua как double, поэтому `cells + 3` это 35.0, а не 35, и
  -- индекс поля выходит дробным. Проверки sol2 включены только в отладочной сборке, поэтому Release
  -- такой индекс молча принимал, а Debug падал с «number maybe has significant decimals»: ошибка была
  -- латентной ровно до первой отладочной сборки.
  local cells = math.tointeger(p.chunk_cells) or math.floor(p.chunk_cells)
  local cell = p.cell_size
  local side = cells + 3
  local iso = p.iso
  local seed = step.chunk_seed

  -- Индекс узла решётки: x внутренний, затем y, затем z — та же раскладка, что у position_grid.
  local function node(x, y, z)
    return x + side * (y + side * z)
  end

  -- Локальная координата узла: узел с индексом 1 стоит ровно в нуле (первый — полоса перекрытия).
  local function coordinate(index)
    return (index - 1) * cell
  end

  local function value_at(x, y, z)
    return density:get(node(x, y, z))
  end

  -- Случайное число из хеша пары. Второй аргумент — номер попытки и роль, чтобы одна попытка не
  -- расходовала «то же» число дважды.
  local function random(attempt, role)
    return base.prng64_normalize(base.prng64_2(seed, attempt * 16 + role))
  end

  local written = 0

  for attempt = 1, p.prop_attempts do
    if written >= capacity then
      break
    end

    -- Столбец выбирается ХЕШЕМ, а не перебором сетки: сетка дала бы видимую регулярность на швах,
    -- потому что кандидаты в каждом чанке начинались бы от его начала. Крайние клетки исключены:
    -- веха на самой границе принадлежала бы двум чанкам сразу.
    local x = 2 + math.tointeger(math.floor(random(attempt, 1) * (cells - 2)))
    local z = 2 + math.tointeger(math.floor(random(attempt, 2) * (cells - 2)))

    -- Полы столбца: переход из вещества в пустоту при подъёме. Их может быть несколько — пол пещеры
    -- это тоже пол, и именно из этого объём отличается от карты высот.
    local floors = {}
    for y = 1, side - 3 do
      if value_at(x, y, z) >= iso and value_at(x, y + 1, z) < iso then
        floors[#floors + 1] = y
      end
    end

    if #floors > 0 and random(attempt, 3) < p.prop_chance then
      local chosen = floors[1 + math.tointeger(math.floor(random(attempt, 4) * #floors))]
      local below = value_at(x, chosen, z)
      local above = value_at(x, chosen + 1, z)

      -- Точная высота пола — там, где поле проходит порог. Та же линейная доля, что у marching
      -- cubes, поэтому веха стоит именно на поверхности, а не на ближайшем узле решётки.
      local span = above - below
      local t = 0.5
      if span ~= 0.0 then
        t = (iso - below) / span
        if t < 0.0 then t = 0.0 elseif t > 1.0 then t = 1.0 end
      end

      -- Нормаль пола: минус градиент плотности центральными разностями. Полоса перекрытия ровно для
      -- этого и нужна — у каждого узла есть сосед с обеих сторон.
      local nx = -(value_at(x + 1, chosen, z) - value_at(x - 1, chosen, z)) / (2.0 * cell)
      local ny = -(value_at(x, chosen + 1, z) - value_at(x, chosen - 1, z)) / (2.0 * cell)
      local nz = -(value_at(x, chosen, z + 1) - value_at(x, chosen, z - 1)) / (2.0 * cell)
      local length = math.sqrt(nx * nx + ny * ny + nz * nz)
      if length > 0.0 then
        nx, ny, nz = nx / length, ny / length, nz / length
      else
        nx, ny, nz = 0.0, 1.0, 0.0
      end

      -- Стена полом не считается: на ней веха выглядела бы приклеенной. Порог задан вертикальной
      -- компонентой нормали, потому что это и есть «насколько это площадка».
      if ny >= p.prop_slope_limit then
        -- Свод над головой: если выше по столбцу снова начинается вещество, это пещера или
        -- нависание. Дальность в метрах, а не в узлах: узлы зависят от размера клетки.
        local reach = math.tointeger(math.floor(p.prop_roof_reach / cell))
        local roofed = false
        for step_up = 2, reach do
          local y = chosen + step_up
          if y > side - 2 then
            break
          end
          if value_at(x, y, z) >= iso then
            roofed = true
            break
          end
        end

        local kind = 2
        if not roofed then
          -- На открытом месте род выбирает уклон: на площадке столб, на склоне валун.
          kind = ny > 0.85 and 0 or 1
        end

        local size = p.prop_size_min + random(attempt, 5) * (p.prop_size_max - p.prop_size_min)

        out_position:set(written, coordinate(x))
        out_position:set(written, coordinate(chosen) + t * cell, 1)
        out_position:set(written, coordinate(z), 2)
        out_normal:set(written, nx)
        out_normal:set(written, ny, 1)
        out_normal:set(written, nz, 2)
        out_size:set(written, size)
        out_kind:set(written, kind)
        written = written + 1
      end
    end
  end

  -- Сколько получилось — обычное состояние между шагами, то есть обычный буфер на один элемент.
  state:field("prop_count"):set(0, written)
end
