-- Тело шага layout: раскладка тайлов по ЖЁСТКИМ ЗАПРЕТАМ соседства.
--
-- Здесь объявляется то, чего в библиотеке до сих пор не выражалось ничем: «этот тайл НИКОГДА не
-- граничит с тем». Шум даёт плавность, вороной даёт области, заливка по графу даёт достижимость — но
-- ни один из них не умеет сказать «вода не касается травы». Умеет решатель ограничений.
--
-- ЛЕСЕНКА. Тайлы выстроены в ряд от глубины к снегу, и сосед разрешён только через соседнюю ступень.
-- Отсюда и вид карты: между водой и травой ВСЕГДА есть песок, между травой и камнем всегда лес — не
-- потому, что так подобраны веса, а потому, что иначе просто запрещено.
--
-- Таблица тайлов лежит здесь, а не в документе значений, потому что это СТРУКТУРА, а `values`
-- держит числа. Сколько их — объявлено в конфиге (`tile_count`), потому что по этому числу считаются
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
  local tile_table = step.writes.tiles
  local rules = step.writes.rules
  local cells = step.writes.cells

  local count = math.tointeger(p.tile_count) or math.floor(p.tile_count)
  if #tiles ~= count then
    error(string.format("layout: the config declares %d tiles and the table holds %d -- the size of every rule " ..
                        "buffer is computed from the declared number, so a mismatch is not a detail", count, #tiles))
  end

  local weight = tile_table:field("weight")
  local colour = tile_table:field("colour")
  for i, tile in ipairs(tiles) do
    weight:set(i - 1, tile.weight)
    colour:set(i - 1, tile.colour)
  end

  -- МАТРИЦА СОСЕДСТВА. Индекс считается арифметикой, и больше ничем: как решатель упакует её внутри,
  -- скрипту знать незачем.
  local allowed = rules:field("allowed")
  local function permit(axis, from, to)
    allowed:set((axis * count + from) * count + to, 1)
  end
  local function link(a, b)
    -- Соседство здесь СИММЕТРИЧНО, поэтому разрешается в обе стороны по обеим осям. Инструмент этого
    -- не навязывает: направленные правила («слева может стоять только это») он тоже принимает.
    for axis = 0, 1 do
      permit(axis, a, b)
      permit(axis, b, a)
    end
  end

  for i = 0, count - 1 do
    link(i, i)
  end
  for i = 0, count - 2 do
    link(i, i + 1)
  end

  -- ЗАРАНЕЕ ЗАНЯТЫЕ КЛЕТКИ: рамка из глубокой воды. Решатель получает их условием и обязан его
  -- соблюсти — а заодно рамка показывает, что ограничение расходится внутрь: у самой кромки не
  -- бывает ничего, кроме глубины и воды.
  local side = cells:extent().x
  if p.frame ~= 0 then
    local given = cells:field("given")
    for x = 0, side - 1 do
      given:set(x, 1)
      given:set((side - 1) * side + x, 1)
    end
    for y = 0, side - 1 do
      given:set(y * side, 1)
      given:set(y * side + side - 1, 1)
    end
  end

  -- Апертура у решателя `sequential`, и это не деталь реализации: какую клетку наблюдать следующей,
  -- решает поле, оставшееся после предыдущего распространения. Поэтому в очередь он не попадает и на
  -- устройство не поедет — и отказ очереди говорит ровно это.
  local inputs = { weight, allowed }
  if p.frame ~= 0 then
    inputs[#inputs + 1] = cells:field("given")
  end

  originator.collapse{
    inputs = inputs,
    outputs = { cells:field("tile"), step.writes.state:field("attempts") },
    params = { attempts = p.attempts, wrap = p.wrap },
  }
end
