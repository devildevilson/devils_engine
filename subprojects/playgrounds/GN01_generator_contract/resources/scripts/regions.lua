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
  local width = step.params.width
  local spread = step.params.spread

  -- Детерминированная россыпь без math.random: его в окружении генератора нет намеренно.
  -- Аддитивная последовательность по золотому сечению даёт равномерное покрытие без решётки.
  local gx, gy = 0.7548776662, 0.5698402910
  local fx, fy = 0.5, 0.5
  for i = 0, site_count - 1 do
    fx = (fx + gx) % 1.0
    fy = (fy + gy) % 1.0
    local jitter = ((i * 2654435761) % 1024) / 1024.0 - 0.5
    position:set(i, (fx + jitter * spread / width) % 1.0 * width, 0)
    position:set(i, fy * width, 1)
  end

  -- gather: читает сайты, пишет свою клетку. Подготовка строит kd-дерево один раз.
  originator.voronoi_label{
    inputs = { sites:field("position") },
    outputs = { cells:field("region") },
    params = { width = width, height = width, site_count = site_count },
  }

  -- scatter: точная топология из Делоне. Ребро между сайтами и есть соседство областей, поэтому
  -- тонкая область не теряется так, как потерялась бы при выводе соседства из растра.
  originator.voronoi_adjacency{
    inputs = { sites:field("position") },
    outputs = { step.writes.region_offsets:field("start"), step.writes.region_arcs:field("site") },
    params = { width = width, height = width },
    range = { 0, site_count },
  }

  -- scatter: клетки, разложенные по областям. Диапазон относится ко ВХОДАМ, а выход — структура
  -- другого размера, поэтому число корзин задаёт буфер смещений, а не диапазон.
  originator.group_by{
    inputs = { cells:field("region") },
    outputs = { step.writes.region_cell_offsets:field("start"), step.writes.region_cell_order:field("cell") },
  }

  -- scatter: сумма высот по областям, воспроизводимая бит в бит при любом числе потоков.
  originator.accumulate{
    inputs = { cells:field("region"), cells:field("height") },
    outputs = { step.writes.region_stats:field("height_sum") },
  }
end
