-- Тело шага terrain.
--
-- Скрипт возвращает функцию: она и есть шаг. Lua тут дирижёр одного куплета генерации — выбирает
-- инструмент, поля и параметры, но сама по элементам не ходит. Индексы элементов везде с НУЛЯ.
--
-- Здесь же видно, зачем дирижёру вообще читать данные: свёртка возвращает ДВА числа, и по ним
-- скрипт параметризует следующий вызов. Без этого fixture зависел бы от разрешения — одна и та же
-- частота на сетке 512 и 2048 покрывала бы разные участки поля шума, и доля суши уезжала бы.
--
-- И здесь же видна граница чанкования. Шум чанкуется: он зависит только от мировой позиции, поэтому
-- достаточно сдвинуть начало координат по ключу чанка. Нормализация по измеренному диапазону НЕ
-- чанкуется: чанк измерил бы свой собственный диапазон и получил бы своё отображение, не совпадающее
-- с соседями. Поэтому режим выбирает конфиг, а не скрипт угадывает.

local function generate(field, step, cells, features, salt)
  -- Ширина строки буфера читается У БУФЕРА, а не из параметров: инструмент возьмёт её оттуда же, и
  -- двух чисел, случайно равных, больше не существует. map_width — величина ДРУГАЯ (ширина всей
  -- карты), и она остаётся значением пайплайна.
  local width = cells:extent().x

  -- Частота задаётся в чертах на КАРТУ, а не на клетку и не на чанк: ни разрешение, ни разбиение на
  -- чанки не должны менять мир.
  local frequency = features / step.params.map_width

  originator.noise_grid{
    outputs = { field },
    params = {
      tree = step.params.tree,
      frequency = frequency,
      -- Мировое смещение чанка: поле в чанке (2,3) продолжает поле соседа, а не начинается заново.
      x_offset = step.chunk.x * width,
      y_offset = step.chunk.y * width,
      seed_offset = salt,
    },
  }
end

local function normalize(field)
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

local function rescale(field, step)
  originator.remap{
    inputs = { field },
    outputs = { field },
    params = { scale = step.params.fixed_scale, offset = step.params.fixed_offset },
  }
end

return function(step)
  local cells = step.writes.cells

  -- Имя поля разрешается ОДИН раз, дальше в работе участвует уже ссылка.
  local height = cells:field("height")
  local moisture = cells:field("moisture")

  generate(height, step, cells, step.params.features, 1)
  generate(moisture, step, cells, step.params.features * 2.5, 2)

  if step.params.normalize ~= 0 then
    normalize(height)
    normalize(moisture)
  else
    rescale(height, step)
    rescale(moisture, step)
  end
end
