-- Тело шага stats.
--
-- «Сколько всего получилось» — это обычные данные, просто их мало: буфер state объявлен с size = 1.
-- Отдельного механизма передачи состояния между шагами не нужно, и оно так же видно в конфиге.

return function(step)
  local cells = step.reads.cells
  local state = step.writes.state

  local peak = originator.reduce_max{ inputs = { cells:field("height") } }
  local land = originator.reduce_count_above{
    inputs = { cells:field("smoothed") },
    params = { threshold = step.params.sea_level },
  }

  state:field("peak"):set(0, peak)
  state:field("land_cells"):set(0, land)
end
