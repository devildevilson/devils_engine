-- Тело шага surface: сглаживание рельефа, уровень моря и расстояние до океана.
--
-- Главное решение шага: уровень моря НЕ задан числом, а ищется под заданную долю суши. Задавать его
-- напрямую значит получать случайную долю суши при каждой правке формулы рельефа — а доля суши это
-- то, что автор мира держит в голове как требование, в отличие от абсолютной отметки в метрах.
--
-- Бисекция стоит 22 прохода правила по всем клеткам плюс столько же свёрток. Это заметная цена, и она
-- честнее, чем угаданная константа: искать приходится ровно потому, что рельеф считается формулой, а
-- не подгоняется.

return function(step)
  local cells = step.writes.cells
  local state = step.writes.state
  local p = step.params

  local offsets = step.reads.cell_offsets:field("start")
  local arcs = step.reads.cell_arcs:field("cell")

  local height = cells:field("height")
  local smooth = cells:field("height_smooth")
  local land = cells:field("land")
  local one = cells:field("one")

  -- 1. Сглаживание по графу. gather требует РАЗНЫХ полей источника и приёмника, иначе соседи
  -- читались бы в неопределённом состоянии, поэтому проходы идут парами: height -> smooth -> height.
  -- Нечётное число проходов оставило бы результат не в том поле, и это была бы тихая ошибка.
  for _ = 1, math.floor(p.relief_smoothing) do
    originator.graph_blur{
      inputs = { offsets, arcs, height },
      outputs = { smooth },
      params = { self_weight = p.relief_self_weight },
    }
    originator.graph_blur{
      inputs = { offsets, arcs, smooth },
      outputs = { height },
      params = { self_weight = p.relief_self_weight },
    }
  end

  -- 1a. Изломанность накладывается ПОСЛЕ сглаживания, и порядок здесь содержательный. Сглаживание
  -- убирает ступеньки заливки по графу — артефакт решётки в ТЕКТОНИЧЕСКОМ сигнале; фрактальная
  -- деталь к этому сигналу не относится и сглаживанию не подлежит. Пока она добавлялась внутри
  -- формулы рельефа, два прохода размытия съедали около половины её амплитуды, и деталь приходилось
  -- задирать вдвое, чтобы она была видна вообще.
  --
  -- Амплитуда приходит полем roughness: дно ровное, континент неровен, горный пояс неровен сильнее
  -- всего. Это произведение и сумма, то есть арифметика, поэтому её считает движок, а не правило.
  originator.modulate{
    inputs = { cells:field("relief_noise"), cells:field("roughness") },
    outputs = { smooth },
  }
  originator.blend{ inputs = { height, smooth }, outputs = { height } }

  -- 2. Уровень моря бисекцией под долю суши. Границы поиска взяты шире любого рельефа: формула
  -- рельефа может измениться, а поиск обязан сойтись всё равно.
  local count = cells:count()
  local target = p.land_target * count
  local low, high = -12000.0, 12000.0
  local level = 0.0
  local land_cells = 0

  local land_support = cells:field("land_support")
  local erosion_passes = math.floor(p.coast_erosion_passes)

  for _ = 1, math.floor(p.sea_level_iterations) do
    level = 0.5 * (low + high)
    originator.run_script{
      program = step.programs.land,
      predicate = true,
      inputs = { height, cells:field("lat_sin") },
      outputs = { land },
      params = { sea_level = level, bulge = p.bulge },
    }

    -- Прибрежная эрозия ВНУТРИ поиска уровня моря, а не после него. Если смывать крошки потом, доля
    -- суши уедет вниз вместе со смытым, а доля — это требование автора мира, а не что получится.
    -- Поддержка считается размытием по графу: доля суши среди клетки и её соседей.
    for _ = 1, erosion_passes do
      originator.graph_blur{
        inputs = { offsets, arcs, land },
        outputs = { land_support },
        params = { self_weight = 1.0, neighbour_weight = 1.0 },
      }
      originator.run_script{
        program = step.programs.coast,
        predicate = true,
        inputs = { land, land_support },
        outputs = { land },
        params = { coast_support = p.coast_support },
      }
    end

    land_cells = originator.reduce_sum{ inputs = { land } }
    -- Выше уровень — меньше суши, поэтому направление именно такое.
    if land_cells > target then
      low = level
    else
      high = level
    end
  end

  -- 3. Расстояние до океана. Из него берётся континентальность климата: вода греется и остывает
  -- медленно, поэтому берег живёт почти без сезонов, а центр материка — с двумя климатами в одном
  -- месте. Метка -1 у недостигнутых клеток значит «океана рядом нет вовсе», и инсоляция читает это
  -- как максимальную континентальность, а не как ноль.
  originator.run_script{
    program = step.programs.ocean,
    predicate = true,
    inputs = { land },
    outputs = { cells:field("ocean_seed") },
  }

  originator.graph_flood{
    inputs = { offsets, arcs, cells:field("ocean_seed"), one, one },
    outputs = { cells:field("scratch_label"), cells:field("ocean_distance") },
    params = { unreached = -1 },
  }

  state:field("sea_level"):set(0, level)
  state:field("land_target"):set(0, p.land_target)
  state:field("land_cells"):set(0, land_cells)
end
