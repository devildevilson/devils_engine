-- Тело шага elevation: высота и влажность из шума.
--
-- Скрипт возвращает функцию — она и есть шаг. Lua здесь дирижёр одного куплета: выбирает
-- инструмент, поля и параметры, но по элементам сама не ходит.
--
-- ГРАНИЦА ЧАНКОВАНИЯ. Шум чанкуется: он зависит только от мировой позиции, поэтому достаточно
-- сдвинуть начало координат по ключу чанка. А вот нормализация по ИЗМЕРЕННОМУ диапазону НЕ
-- чанкуется: каждый чанк измерил бы свой собственный и получил бы своё отображение, не совпадающее
-- с соседями. Поэтому поле уходит дальше КАК ЕСТЬ, а пороги классификации откалиброваны против
-- него напрямую (см. values.tavl) — приводящего преобразования здесь нет намеренно, потому что
-- тождественное преобразование было бы просто ложью о том, что где-то есть нормализация.

local function field_from_noise(field, step, side, feature_size, salt)
  -- Частота задаётся размером черты В ТАЙЛАХ, а не «на чанк»: иначе смена размера чанка меняла бы
  -- мир, хотя ключ чанка тот же.
  local frequency = 1.0 / feature_size

  originator.noise_grid{
    outputs = { field },
    params = {
      tree = step.params.tree,
      frequency = frequency,
      -- Мировое смещение чанка: поле в чанке (2,3) продолжает поле соседа.
      x_offset = step.chunk.x * side,
      y_offset = step.chunk.y * side,
      seed_offset = salt,
    },
  }

end

return function(step)
  local tiles = step.writes.tiles
  -- Сторону растра читаем У БУФЕРА: он объявил свою форму, и второго источника этого числа нет.
  local side = tiles:extent().x

  field_from_noise(tiles:field("height"), step, side, step.params.feature_size, 1)
  field_from_noise(tiles:field("moisture"), step, side, step.params.moisture_feature_size, 2)
end
