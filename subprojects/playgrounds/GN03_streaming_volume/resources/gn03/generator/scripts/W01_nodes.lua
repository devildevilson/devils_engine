-- Тело шага nodes: геймплейные узлы каркаса.
--
-- Узлы ПЕРВИЧНЫ, маршрут выводится из них. Это и есть смысл каркаса: сначала появляются места, ради
-- которых мир существует, и только потом то, что их соединяет.
--
-- Разнесены они по решётке С ДРОЖАНИЕМ, а не пуассоновским выбором: у решётки с дрожанием
-- минимальное расстояние гарантировано ПО ПОСТРОЕНИЮ (клетка одна, узел в ней один), и на сорока
-- узлах это ровно тот же результат за одну строку вместо инструмента.
--
-- Зерно здесь ОБЫЧНОЕ (`step.seed`), а не чанковое: у каркаса чанков нет вовсе, он считается один раз
-- на весь мир.

return function(step)
  local nodes = step.writes.nodes
  local state = step.writes.skeleton_state
  local p = step.params

  local position = nodes:field("position")
  local kind = nodes:field("kind")
  local capacity = nodes:count()

  local wanted = math.tointeger(p.node_count) or math.floor(p.node_count)
  if wanted > capacity then
    -- Громкий отказ, а не тихая обрезка: мир, у которого пропала половина узлов, дальше молча
    -- построит маршрут не туда.
    error(string.format("skeleton wants %d nodes but the buffer holds %d", wanted, capacity))
  end

  local span = p.world_span
  local side = math.tointeger(math.ceil(math.sqrt(wanted)))
  local cell = span / side

  local written = 0
  for index = 0, side * side - 1 do
    if written >= wanted then
      break
    end
    local gx = index % side
    local gz = math.tointeger(math.floor(index / side))

    -- Дрожание внутри своей клетки: 0.15..0.85 от её размера, чтобы узлы не липли к границам клеток
    -- и минимальное расстояние между ними осталось порядка 0.3 клетки.
    local jitter_x = 0.15 + 0.70 * base.prng64_normalize(base.prng64_2(step.seed, index * 4 + 1))
    local jitter_z = 0.15 + 0.70 * base.prng64_normalize(base.prng64_2(step.seed, index * 4 + 2))
    local height = base.prng64_normalize(base.prng64_2(step.seed, index * 4 + 3))

    -- Мир центрирован на нуле: игрок начинает у начала координат, и каркас обязан быть вокруг него.
    local x = (gx + jitter_x) * cell - span * 0.5
    local z = (gz + jitter_z) * cell - span * 0.5
    -- Высота узла — полоса вокруг опорной, а не поверхность земли. Каркас НЕ знает рельефа, и это
    -- решение: он говорит, где маршрут проходит, а что там делает камень — знает чанк. Отсюда и
    -- получается «коридор внутри горы»: там, где земля выше маршрута, чанк вырежет тоннель.
    local y = p.route_altitude + (height - 0.5) * p.route_wander

    position:set(written, x)
    position:set(written, y, 1)
    position:set(written, z, 2)
    kind:set(written, math.tointeger(math.floor(base.prng64_normalize(base.prng64_2(step.seed, index * 4 + 4)) * 3)))
    written = written + 1
  end

  state:field("node_count"):set(0, written)
end
