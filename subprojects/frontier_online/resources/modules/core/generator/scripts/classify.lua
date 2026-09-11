-- Тело шага classify: код рельефа из высоты и влажности.
--
-- Два прохода подряд, между которыми дирижёр ничего не читает и ничего не решает, — это ОЧЕРЕДЬ.
-- Объявленная очередь проверяется целиком до первого вызова и сливает подряд идущие pointwise
-- проходы в один обход данных.
--
-- box_blur имеет апертуру gather: он читает окно вокруг клетки, поэтому приёмник обязан быть
-- ОТДЕЛЬНЫМ полем — иначе соседи читались бы в неопределённом состоянии. Движок отклоняет такой
-- вызов до исполнения.
--
-- ОГОВОРКА ПРО ШОВ: сглаживание — операция с окном, а окно на краю чанка упирается в его границу и
-- не видит соседа. При radius = 1 это один ряд клеток по периметру; на глаз незаметно, но это
-- НЕ «почти правильно», а другое значение. Честная починка — считать чанк с полями (apron) шире
-- нужного и отдавать середину; она приедет, когда швы начнут мешать.
--
-- `output` — это ГРАНИЦА ПЕРЕДАЧИ, а не список результатов: наружу шага уходит только terrain,
-- остальное было промежуточным.

return function(step)
  local tiles = step.writes.tiles

  local height = tiles:field("height")
  local smoothed = tiles:field("smoothed")
  local moisture = tiles:field("moisture")
  local terrain = tiles:field("terrain")

  originator.queue{
    originator.queue.box_blur{
      inputs = { height },
      outputs = { smoothed },
      params = { radius = step.params.radius },
    },

    -- Классы: 0 вода, 1 сухая суша, 2 обычная, 3 влажная. Пороги приходят из значений генератора,
    -- поэтому их читает и этот проход, и всё, что позже будет судить о проходимости.
    originator.queue.classify{
      inputs = { smoothed, moisture },
      outputs = { terrain },
      params = {
        sea_level = step.params.sea_level,
        dry = step.params.dry,
        wet = step.params.wet,
      },
    },

    output = { terrain },
  }
end
