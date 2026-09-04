-- Тело шага climate.
--
-- box_blur имеет апертуру gather: он читает окно вокруг элемента. Поэтому приёмник обязан быть
-- ОТДЕЛЬНЫМ полем — иначе соседи читались бы в неопределённом состоянии. Движок отклоняет такой
-- вызов до исполнения, так что ошибку невозможно не заметить.
--
-- Эти два вызова — ОЧЕРЕДЬ, и она здесь не украшение. Между размытием и классификацией нет ни
-- одного промежуточного значения в lua: дирижёр ничего не читает и ничего не решает, он просто
-- назвал два прохода подряд. Значит подграф существовал и раньше — у него не было имени, и потому
-- его нельзя было ни померить как одну вещь, ни перенести целиком.
--
-- Отличие от `originator.box_blur{...}` ровно одно: `originator.queue.box_blur{...}` работу
-- ОБЪЯВЛЯЕТ, а исполняет её `originator.queue{ ... }` — целиком, одним отказом до первого вызова.
--
-- `output` — это ГРАНИЦА ПЕРЕДАЧИ, а не список результатов. Что читают следующие шаги, очередь
-- знать не может, поэтому автор называет это вслух; здесь и `smoothed`, и `biome`, потому что
-- правило на devils_script в шаге semantics читает именно `smoothed`. Из той же границы следует
-- проверка: проход, чей выход никто дальше не читает и в `output` не назван, — мёртвый, и об этом
-- скажут до исполнения.

return function(step)
  local cells = step.writes.cells

  local height = cells:field("height")
  local smoothed = cells:field("smoothed")
  local moisture = cells:field("moisture")
  local biome = cells:field("biome")

  originator.queue{
    originator.queue.box_blur{
      inputs = { height },
      outputs = { smoothed },
      params = { width = step.params.width, radius = step.params.radius },
    },

    -- Классификация — семантический проход. Здесь он нативный; тот же расчёт повторяют
    -- classify_lua.lua и правило на devils_script, чтобы сравнение шло по одной задаче.
    originator.queue.classify{
      inputs = { smoothed, moisture },
      outputs = { biome },
      params = {
        sea_level = step.params.sea_level,
        dry = step.params.dry,
        wet = step.params.wet,
      },
    },

    output = { smoothed, biome },
  }
end
