-- Тело шага carve: КАК ЭТО ВЫГЛЯДИТ НА РАСТРЕ.
--
-- Два вызова, и оба `scatter`: они пишут по чужим индексам — комната знает свои клетки, клетка про
-- комнату не знает ничего. Диапазон у обоих приходит СЧЁТЧИКОМ: ёмкость это желание, занятая длина
-- это факт, и читать список до ёмкости значит читать мусор прошлого прогона.
--
-- Коридор режется между ДВУМЯ ТОЧКАМИ и про комнаты не знает ничего: концы вывел предыдущий шаг.
-- Тем же вызовом режется дорога между городами — инструмент назван по делу, а не по подземелью.
--
-- Комнаты и коридоры пишутся в РАЗНЫЕ поля. Одно поле «пол» было бы короче, но тогда пропал бы
-- ответ на вопрос «какой комнате принадлежит клетка», а без него не проверить достижимость по
-- комнатам и не поставить дверь на стыке.

return function(step)
  local p = step.params
  local rooms = step.reads.rooms
  local links = step.reads.links
  local state = step.reads.state
  local cells = step.writes.cells

  -- ОДНО ЧИСЛО, ДВА ОБЪЯВЛЕНИЯ. Сторона растра объявлена формой буфера, а границы плана — значением
  -- пайплайна; проверяется это здесь, потому что раньше растра просто нет. Разошедшиеся числа
  -- означают план, посчитанный в одних границах и вырезанный в других, и по карте это выглядит как
  -- «комнаты почему-то жмутся к одному углу».
  local extent = cells:extent()
  if extent.x ~= p.side or extent.y ~= p.side then
    error(string.format("carve: the plan was made for a %dx%d raster and the grid is %dx%d",
                        p.side, p.side, extent.x, extent.y))
  end

  originator.paint_rects{
    inputs = { rooms:field("rect") },
    outputs = { cells:field("room") },
    range = { count = state:field("rooms") },
  }

  originator.carve_corridors{
    inputs = { links:field("path") },
    outputs = { cells:field("corridor") },
    range = { count = state:field("links") },
    params = { corridor_width = p.corridor_width },
  }
end
