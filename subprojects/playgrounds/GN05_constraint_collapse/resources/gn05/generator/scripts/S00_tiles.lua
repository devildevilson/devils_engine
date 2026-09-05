-- Тело шага table: ЛЕСЕНКА ТАЙЛОВ.
--
-- Единственное место, где она объявлена. На неё смотрят три генератора площадки — растровый по
-- объявленным правилам, обученный по образцу и графовый на сфере, — и все три ссылаются на ЭТОТ файл,
-- а не переписывают таблицу к себе. Второй экземпляр разъехался бы с первым молча: цвета и веса
-- продолжали бы выглядеть правдоподобно, а карта показывала бы не то, что задумано.
--
-- Порядок тайлов ЗНАЧАЩИЙ: сосед разрешён только через соседнюю ступень, и правила строятся из этого
-- порядка арифметикой, а не перечислением пар.
--
-- Таблица лежит в скрипте, а не в документе значений, потому что это СТРУКТУРА, а `values` держит
-- числа. Сколько их — объявлено в конфиге (`tile_count`), потому что по этому числу считаются
-- размеры буферов ещё до запуска скрипта; расхождение проверяется вслух.

local tiles = {
  { name = "deep",   weight = 0.7, colour = 0x24406e },
  { name = "water",  weight = 1.0, colour = 0x3a6ea8 },
  { name = "sand",   weight = 0.8, colour = 0xd6c489 },
  { name = "grass",  weight = 2.4, colour = 0x5c9247 },
  { name = "forest", weight = 1.8, colour = 0x2f5f34 },
  { name = "rock",   weight = 0.9, colour = 0x7d7a72 },
  { name = "snow",   weight = 0.5, colour = 0xe8eef2 },
}

return function(step)
  local p = step.params
  local table_buffer = step.writes.tiles

  local count = math.tointeger(p.tile_count) or math.floor(p.tile_count)
  if #tiles ~= count then
    error(string.format("tiles: the config declares %d tiles and the table holds %d -- the size of every rule " ..
                        "buffer is computed from the declared number, so a mismatch is not a detail", count, #tiles))
  end

  local weight = table_buffer:field("weight")
  local colour = table_buffer:field("colour")
  for i, tile in ipairs(tiles) do
    weight:set(i - 1, tile.weight)
    colour:set(i - 1, tile.colour)
  end
end
