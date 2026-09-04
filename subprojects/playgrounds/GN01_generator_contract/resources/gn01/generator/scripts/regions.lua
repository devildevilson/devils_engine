-- Тело шага regions.
--
-- Здесь видно распределение ролей целиком. Сайтов мало, поэтому их позиции lua расставляет
-- поэлементно — это ровно тот случай, который правилом разрешён: скрипт обходит множество, которое
-- сам же перечислил. Всё, что касается миллионов клеток, уходит в нативные инструменты.

return function(step)
  local sites = step.writes.sites
  local cells = step.writes.cells

  local position = sites:field("position")
  local site_count = sites:count()
  -- Мир областей занимает ровно растр карты, поэтому его ширина берётся у ФОРМЫ буфера. У диаграммы
  -- это остаётся ПАРАМЕТРОМ и дальше: `width`/`height` у соседства и контуров — границы МИРА в его
  -- единицах, а не растр приёмника, и числа совпадают здесь случайно.
  local width = cells:extent().x
  local spread = step.params.spread

  -- Детерминированная россыпь без math.random: его в окружении генератора нет намеренно.
  -- Аддитивная последовательность по золотому сечению даёт равномерное покрытие без решётки.
  local gx, gy = 0.7548776662, 0.5698402910
  local fx, fy = 0.5, 0.5
  for i = 0, site_count - 1 do
    fx = (fx + gx) % 1.0
    fy = (fy + gy) % 1.0
    -- Дрожание берётся ДВИЖКОВЫМ хешем (`base.*`), а не умножением на константу руками: одна
    -- функция хеша на весь движок, и у неё нет своего состояния — число выводится из номера сайта.
    local jitter = base.prng64_normalize(base.prng64(i)) - 0.5
    position:set(i, (fx + jitter * spread / width) % 1.0 * width, 0)
    position:set(i, fy * width, 1)
  end

  -- gather: читает сайты, пишет свою клетку. Подготовка строит kd-дерево один раз.
  originator.voronoi_label{
    inputs = { sites:field("position") },
    outputs = { cells:field("region") },
    -- Растр разметки — это форма ПРИЁМНИКА, и она уже объявлена буфером.
    params = { site_count = site_count },
  }

  -- scatter: точная топология из Делоне. Ребро между сайтами и есть соседство областей, поэтому
  -- тонкая область не теряется так, как потерялась бы при выводе соседства из растра.
  originator.voronoi_adjacency{
    inputs = { sites:field("position") },
    outputs = { step.writes.region_offsets:field("start"), step.writes.region_arcs:field("site") },
    params = { width = width, height = width },
    range = { 0, site_count },
    key_support = "global",
  }

  -- scatter: контуры областей. Это ответ на третий, отдельный вопрос — не «кому принадлежит клетка»
  -- и не «кто с кем граничит», а «какой у области контур». Вершины ОБЩИЕ: соседние области ссылаются
  -- на одну и ту же, поэтому сетка замощает карту без щелей, что бы потом с координатами ни делали.
  originator.voronoi_polygons{
    inputs = { sites:field("position") },
    outputs = {
      step.writes.polygon_offsets:field("start"),
      step.writes.polygon_corners:field("vertex"),
      step.writes.polygon_vertices:field("position"),
      step.writes.polygon_counts:field("vertices"),
    },
    params = { width = width, height = width },
    range = { 0, site_count },
    key_support = "global",
  }

  -- scatter: клетки, разложенные по областям. Диапазон относится ко ВХОДАМ, а выход — структура
  -- другого размера, поэтому число корзин задаёт буфер смещений, а не диапазон.
  --
  -- key_support объявляет автор, потому что из одного чанка это не проверяется: область простирается
  -- по всей карте, значит ни один чанк её не заканчивает. Объявление честное, и оно же означает, что
  -- этот шаг принадлежит грубому мировому проходу, а не чанковому. При чанкованной генерации вызов
  -- будет отклонён — это правильно, такую сводку из чанков не собирают.
  originator.group_by{
    inputs = { cells:field("region") },
    outputs = { step.writes.region_cell_offsets:field("start"), step.writes.region_cell_order:field("cell") },
    key_support = "global",
  }

  -- scatter: сумма высот по областям, воспроизводимая бит в бит при любом числе потоков.
  originator.accumulate{
    inputs = { cells:field("region"), cells:field("height") },
    outputs = { step.writes.region_stats:field("height_sum") },
    key_support = "global",
  }
end
