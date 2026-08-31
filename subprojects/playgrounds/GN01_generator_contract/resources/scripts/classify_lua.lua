-- Тот же расчёт, что делает нативный classify, но поэлементно из lua.
--
-- Этот файл существует РАДИ ЗАМЕРА, а не ради использования: он показывает, во что обходится обход
-- плотного буфера из скрипта. Правило библиотеки — lua обходит только те множества, которые сама
-- перечислила (список областей, отобранные кандидаты), а не плотный буфер целиком.
--
-- Написан настолько быстро, насколько это возможно в lua: поиск полей и методов вынесен из цикла.

return function(step)
  local cells = step.writes.cells

  local smoothed = cells:field("smoothed")
  local moisture = cells:field("moisture")
  local biome = cells:field("biome_lua")

  local get = smoothed.get
  local set = biome.set

  local sea_level = step.params.sea_level
  local dry = step.params.dry
  local wet = step.params.wet

  local count = smoothed:count()

  for i = 0, count - 1 do
    local h = get(smoothed, i)
    if h < sea_level then
      set(biome, i, 0)
    else
      local m = get(moisture, i)
      if m < dry then
        set(biome, i, 1)
      elseif m < wet then
        set(biome, i, 2)
      else
        set(biome, i, 3)
      end
    end
  end
end
