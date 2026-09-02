-- Тело шага landforms: физические ОБЛАСТИ рельефа и их усиление.
--
-- Шаг отвечает на вопрос, которого прежде в генераторе не было: что это за место. До него рельеф был
-- полем высот, и всё, что его различало, — сама высота. Но игрок различает не высоту, а МЕСТА:
-- нагорье, низину, шельф, архипелаг, горную страну. Такие места у планеты действительно есть, и они
-- не выдумываются, а ВЫВОДЯТСЯ из уже посчитанной физики: глубины от уровня моря, местной
-- изрезанности, доли суши по соседству и типа коры.
--
-- Дальше идёт вторая половина шага и главная его мысль: область, будучи опознанной, УСИЛИВАЕТСЯ. У
-- каждого вида свой множитель отклонения от сглаженной высоты и своя поправка к ней:
--   нагорье и низина    множитель меньше единицы — они становятся ровнее, то есть больше похожи на
--                       себя; нагорье вдобавок приподнимается, низина оседает;
--   горы и хребты       множитель больше единицы — гребни выше, долины глубже;
--   архипелаг           множитель больше единицы — проливы глубже, банки выше, острова читаются
--                       островами, а не сплошной отмелью.
-- Это не украшение и не шум: одна и та же операция (отклонение от сглаженного, помноженное на
-- множитель) делает каждый вид более собой. Тем же приёмом фотограф поднимает локальный контраст.
--
-- Усиление меняет высоты, значит уровень моря надо искать ЗАНОВО: доля суши — требование автора мира,
-- и шаг не имеет права её сдвинуть.

return function(step)
  local cells = step.writes.cells
  local types = step.writes.landform_types
  local state = step.writes.state
  local p = step.params

  local offsets = step.reads.cell_offsets:field("start")
  local arcs = step.reads.cell_arcs:field("cell")

  local height = cells:field("height")
  local smooth = cells:field("height_smooth")
  local work = cells:field("crust_work")
  local land = cells:field("land")
  local one = cells:field("one")
  local relative = cells:field("height_relative")
  local relief_local = cells:field("relief_local")
  local land_support = cells:field("land_support")
  local landform = cells:field("landform")
  local gain = cells:field("landform_gain")
  local offset = cells:field("landform_offset")

  local sea_level = state:field("sea_level"):get(0)

  -- 1. Высота ОТ УРОВНЯ МОРЯ. Правила говорят про глубину и высоту над водой, а не про абсолютную
  -- отметку: сама отметка ищется бисекцией и от прогона к прогону разная.
  originator.remap{ inputs = { height }, outputs = { relative }, params = { scale = 1.0, offset = -sea_level } }

  -- 2. Местный УКЛОН: средний перепад высоты до соседей, приведённый к ста километрам. Именно он
  -- отличает нагорье от горной страны при одинаковой высоте и абиссальную равнину от фланга хребта
  -- при одинаковой глубине.
  --
  -- Уклон, а не отклонение от сглаженной высоты, и это исправление настоящей ошибки: наклонная
  -- ПЛОСКОСТЬ от своего размытия почти не отклоняется, поэтому ровное нагорье и ровный склон выходили
  -- неразличимы — у опознанного «нагорья» уклон получался тот же, что у холмов (122 против 121 м на
  -- сто километров, то есть признак не работал вовсе).
  --
  -- Приведение к ста километрам обязательно: сам инструмент отдаёт перепад НА ШАГ РЕШЁТКИ, и пороги
  -- в конфиге иначе означали бы разное на разных разрешениях.
  local count = cells:count()
  local spacing = math.sqrt(4.0 * math.pi / count)
  originator.graph_slope{ inputs = { offsets, arcs, height }, outputs = { relief_local } }
  originator.remap{ inputs = { relief_local }, outputs = { relief_local }, params = { scale = 0.0157 / spacing } }

  -- Уклон РАЗМЫВАЕТСЯ: вид области — свойство места, а не отдельной клетки, и одиночный обрыв посреди
  -- равнины не делает её горной страной.
  for _ = 1, math.floor(p.landform_smoothing) do
    originator.graph_blur{ inputs = { offsets, arcs, relief_local }, outputs = { work }, params = { self_weight = p.landform_self_weight } }
    originator.graph_blur{ inputs = { offsets, arcs, work }, outputs = { relief_local }, params = { self_weight = p.landform_self_weight } }
  end

  -- Сглаженная высота — опора усиления: усиливается ОТКЛОНЕНИЕ от неё.
  originator.remap{ inputs = { height }, outputs = { smooth }, params = { scale = 1.0 } }
  for _ = 1, math.floor(p.landform_smoothing) do
    originator.graph_blur{ inputs = { offsets, arcs, smooth }, outputs = { work }, params = { self_weight = p.landform_self_weight } }
    originator.graph_blur{ inputs = { offsets, arcs, work }, outputs = { smooth }, params = { self_weight = p.landform_self_weight } }
  end

  -- 3. Доля суши в окрестности: по ней опознаётся архипелаг — мелководье, вокруг которого есть земля,
  -- в отличие от такого же мелководья у открытого океана.
  originator.graph_blur{
    inputs = { offsets, arcs, land },
    outputs = { land_support },
    params = { self_weight = 1.0, neighbour_weight = 1.0 },
  }

  -- 4. Опознание. Правил ДВА, по воде и по суше, и это не деление по вкусу: у контекста devils_script
  -- восемь слотов аргументов, а признаков у воды и у суши разные наборы. Собираются они обратно
  -- умножением на маску суши — она ровно ноль или единица.
  originator.run_script{
    program = step.programs.landform_water,
    inputs = { relative, relief_local, land_support },
    outputs = { cells:field("landform_water") },
    params = {
      trench_depth = p.trench_depth,
      ridge_relief = p.ridge_relief,
      shallow_depth = p.shallow_depth,
      archipelago_support = p.archipelago_support,
    },
  }
  originator.run_script{
    program = step.programs.landform_land,
    inputs = { relative, relief_local, cells:field("crust") },
    outputs = { cells:field("landform_land") },
    params = {
      island_crust = p.island_crust,
      mountain_relief = p.mountain_relief,
      hill_relief = p.hill_relief,
      plateau_height = p.plateau_height,
      plain_height = p.plain_height,
    },
  }
  originator.modulate{ inputs = { cells:field("landform_land"), land }, outputs = { landform } }
  originator.remap{ inputs = { land }, outputs = { work }, params = { scale = -1.0, offset = 1.0 } }
  originator.modulate{ inputs = { cells:field("landform_water"), work }, outputs = { work } }
  originator.blend{ inputs = { landform, work }, outputs = { landform } }

  -- 5. Таблица усиления. Числа приходят из конфига по ИМЕНАМ вида, а не позицией в списке: список из
  -- двадцати двух чисел подряд однажды разъедется с порядком видов, и заметить это будет нечем.
  local names = {
    "abyss", "ridge", "shelf", "archipelago", "trench",
    "coast", "lowland", "plateau", "hills", "mountains", "volcanic",
  }
  local type_gain = types:field("gain")
  local type_offset = types:field("offset")
  local type_cells = types:field("cells")
  for index = 1, #names do
    local name = names[index]
    local gain_value = p["landform_gain_" .. name]
    local offset_value = p["landform_offset_" .. name]
    if gain_value == nil or offset_value == nil then
      error("landforms: values.tavl has no gain/offset for landform '" .. name .. "'")
    end
    type_gain:set(index - 1, gain_value)
    type_offset:set(index - 1, offset_value)
    type_cells:set(index - 1, 0)
  end

  originator.lookup{ inputs = { landform, type_gain }, outputs = { gain } }
  originator.lookup{ inputs = { landform, type_offset }, outputs = { offset } }

  -- Множитель и поправка РАЗМЫВАЮТСЯ по графу, и это не сглаживание ради красоты. Опознание даёт
  -- резкую границу между видами, а усиление с разными множителями по разные стороны такой границы
  -- создаёт СТУПЕНЬКУ там, где в рельефе ничего не происходит: измерено, что «выровненная» абиссальная
  -- равнина с множителем 0.30 получалась изрезаннее, чем без усиления вовсе (49 против 45 м на сто
  -- километров), — вся разница набегала на стыках с хребтом. Размытый множитель делает переход
  -- постепенным, а у природы он такой и есть: между низиной и холмами нет линии.
  for _ = 1, math.floor(p.landform_blend) do
    originator.graph_blur{ inputs = { offsets, arcs, gain }, outputs = { work }, params = { self_weight = 1.5 } }
    originator.graph_blur{ inputs = { offsets, arcs, work }, outputs = { gain }, params = { self_weight = 1.5 } }
    originator.graph_blur{ inputs = { offsets, arcs, offset }, outputs = { work }, params = { self_weight = 1.5 } }
    originator.graph_blur{ inputs = { offsets, arcs, work }, outputs = { offset }, params = { self_weight = 1.5 } }
  end

  -- 6. Усиление: отклонение от сглаженной высоты, помноженное на множитель вида, плюс поправка.
  originator.blend{ inputs = { height, smooth }, outputs = { work }, params = { first = 1.0, second = -1.0 } }
  originator.modulate{ inputs = { work, gain }, outputs = { work } }
  originator.blend{ inputs = { smooth, work }, outputs = { height } }
  originator.blend{ inputs = { height, offset }, outputs = { height } }

  -- 7. Уровень моря заново: усиление подняло нагорья и опустило низины, и доля суши уехала бы. Поиск
  -- тот же, что в шаге surface, вместе с прибрежной эрозией — иначе усиление архипелагов нарежет
  -- новых крошек, которые эрозия уже смывала.
  local target = p.land_target * count
  local low, high = -12000.0, 12000.0
  local level = sea_level
  local land_cells = 0
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
    for _ = 1, erosion_passes do
      -- Поддержка считается В СВОЁ поле, а не в рабочее: правило `coast` читает поля по ИМЕНИ
      -- привязки, и поле, названное иначе, для него просто не существует.
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
    if land_cells > target then
      low = level
    else
      high = level
    end
  end

  -- 8. Пересчёт того, что зависит от уровня моря и от маски суши. Расстояние до океана читает климат,
  -- и оно обязано относиться к ТОЙ ЖЕ суше, что и всё остальное.
  originator.remap{ inputs = { height }, outputs = { relative }, params = { scale = 1.0, offset = -level } }
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

  -- 9. Сводка по видам: сколько клеток каждого. Читается отчётом и проверками — без неё «области
  -- есть» остаётся утверждением, а не измерением.
  originator.group_by{
    inputs = { landform },
    outputs = { step.writes.landform_offsets:field("start"), step.writes.landform_order:field("cell") },
    key_support = "global",
  }
  local starts = step.writes.landform_offsets:field("start")
  for index = 0, #names - 1 do
    type_cells:set(index, starts:get(index + 1) - starts:get(index))
  end

  state:field("sea_level"):set(0, level)
  state:field("land_cells"):set(0, land_cells)
  state:field("landform_count"):set(0, #names)
end
