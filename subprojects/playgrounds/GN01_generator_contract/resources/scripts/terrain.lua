-- Тело шага terrain.
--
-- Скрипт возвращает функцию: она и есть шаг. Lua тут дирижёр одного куплета генерации — выбирает
-- инструмент, поля и параметры, но сама по элементам не ходит. Индексы элементов везде с НУЛЯ.
--
-- Здесь же видно, зачем дирижёру вообще читать данные: свёртка возвращает ДВА числа, и по ним
-- скрипт параметризует следующий вызов. Без этого fixture зависел бы от разрешения — одна и та же
-- частота на сетке 512 и 2048 покрывает разные участки поля шума, и доля суши уезжала бы с 55% до
-- 37% при том же зерне.

local function generate_normalized(field, step, features, salt)
  -- Частота задаётся в ЧЕРТАХ НА КАРТУ, а не на клетку: разрешение не должно менять мир.
  originator.noise_grid{
    outputs = { field },
    params = {
      tree = step.params.tree,
      width = step.params.width,
      frequency = features / step.params.width,
      seed_offset = salt,
    },
  }

  -- Диапазон выхода закодированного дерева узлов заранее не известен, поэтому он не угадывается
  -- параметрами, а измеряется. Две свёртки — это два числа, читать их из lua дешево.
  local lowest = originator.reduce_min{ inputs = { field } }
  local highest = originator.reduce_max{ inputs = { field } }

  local span = highest - lowest
  if span <= 0.0 then
    error("terrain: noise field is constant, check the node tree")
  end

  -- remap имеет апертуру pointwise, поэтому источник и приёмник МОГУТ совпадать: элемент трогает
  -- только себя, промежуточное поле не нужно.
  originator.remap{
    inputs = { field },
    outputs = { field },
    params = { scale = 1.0 / span, offset = -lowest / span },
  }
end

return function(step)
  local cells = step.writes.cells

  -- Имя поля разрешается ОДИН раз, дальше в работе участвует уже ссылка.
  generate_normalized(cells:field("height"), step, step.params.features, 1)
  generate_normalized(cells:field("moisture"), step, step.params.features * 2.5, 2)
end
