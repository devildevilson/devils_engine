-- Тело шага layout: раскладка тайлов по ОБЪЯВЛЕННЫМ запретам соседства.
--
-- Здесь объявляется то, чего в библиотеке до сих пор не выражалось ничем: «этот тайл НИКОГДА не
-- граничит с тем». Шум даёт плавность, вороной даёт области, заливка по графу даёт достижимость — но
-- ни один из них не умеет сказать «вода не касается травы». Умеет решатель ограничений.
--
-- ЛЕСЕНКА объявлена в S00_tiles.lua; здесь используется только её ПОРЯДОК: сосед разрешён через
-- соседнюю ступень. Отсюда и вид карты — между водой и травой ВСЕГДА песок, между травой и камнем
-- всегда лес — не потому, что так подобраны веса, а потому, что иначе просто запрещено.

return function(step)
  local p = step.params
  local rules = step.writes.rules
  local cells = step.writes.cells

  local count = math.tointeger(p.tile_count) or math.floor(p.tile_count)

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
  local inputs = { step.reads.tiles:field("weight"), allowed }
  if p.frame ~= 0 then
    inputs[#inputs + 1] = cells:field("given")
  end

  originator.collapse{
    inputs = inputs,
    outputs = { cells:field("tile"), step.writes.state:field("attempts"),
                step.writes.state:field("rollbacks") },
    params = { attempts = p.attempts, wrap = p.wrap, rollbacks = p.rollbacks, history = p.history },
  }
end
