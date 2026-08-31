-- Тело шага terrain.
--
-- Скрипт возвращает функцию: она и есть шаг. Lua тут дирижёр одного куплета генерации — выбирает
-- инструмент, поля и параметры, но сама по элементам не ходит. Индексы элементов везде с НУЛЯ.

return function(step)
  local cells = step.writes.cells

  -- Имя поля разрешается ОДИН раз, дальше в работе участвует уже ссылка.
  local height = cells:field("height")
  local moisture = cells:field("moisture")

  originator.value_noise{
    outputs = { height },
    params = {
      width = step.params.width,
      frequency = step.params.frequency,
      octaves = step.params.octaves,
      salt = 1,
    },
  }

  -- Второй слой берёт другую соль, поэтому это независимое поле, а не сдвинутая копия первого.
  originator.value_noise{
    outputs = { moisture },
    params = {
      width = step.params.width,
      frequency = step.params.frequency * 2.0,
      octaves = 3,
      salt = 2,
    },
  }
end
