-- Тело шага climate.
--
-- box_blur имеет апертуру gather: он читает окно вокруг элемента. Поэтому приёмник обязан быть
-- ОТДЕЛЬНЫМ полем — иначе соседи читались бы в неопределённом состоянии. Движок отклоняет такой
-- вызов до исполнения, так что ошибку невозможно не заметить.

return function(step)
  local cells = step.writes.cells

  local height = cells:field("height")
  local smoothed = cells:field("smoothed")
  local moisture = cells:field("moisture")
  local biome = cells:field("biome")

  originator.box_blur{
    inputs = { height },
    outputs = { smoothed },
    params = { width = step.params.width, radius = step.params.radius },
  }

  -- Классификация — семантический проход. Здесь он нативный; тот же расчёт повторяют
  -- classify_lua.lua и (следующим срезом) devils_script, чтобы сравнение шло по одной задаче.
  originator.classify{
    inputs = { smoothed, moisture },
    outputs = { biome },
    params = {
      sea_level = step.params.sea_level,
      dry = step.params.dry,
      wet = step.params.wet,
    },
  }
end
