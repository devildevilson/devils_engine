-- Тело шага roles: ЧТО ЭТО ЗА МЕСТА.
--
-- Виды раскладывает РЕШАТЕЛЬ ОГРАНИЧЕНИЙ по графу смежности зон — тот самый, что в GN05 раскладывал
-- тайлы по сфере. Ему всё равно, что вершина графа теперь комната, а не клетка: соседство приходит
-- данными (CSR), правила — симметричной матрицей.
--
-- ГЛАВНАЯ ОГОВОРКА, И ОНА НЕ ЛЕЧИТСЯ ПРАВИЛАМИ. Решатель покупает ЗАПРЕТ («кухня никогда не граничит
-- с покоями»), а не ТРЕБОВАНИЕ («кухня обязана быть рядом с залом»). Требование ставится УСЛОВИЕМ —
-- заранее занятой зоной, — и это ровно тот приём, которым в GN05 ставили вершины и впадины на сфере:
-- у правила «через ступень» нет возвращающей силы, и нужное приходится назначать с обоих концов.
--
-- Поэтому условий здесь четыре, и каждое отвечает на своё:
--
--   КОРИДОРЫ      их назначил раздел: циркуляция это структура, а не местное правило. Никакая
--                 матрица запретов не сделает коридор связным — связность глобальна, а запрет
--                 локален;
--   УЛИЦА И ДВОР  их положил тот же шаг раздела: что снаружи, решает участок, а не соседство;
--   ЗАЛ           самое большое место, СМЕЖНОЕ С УЛИЦЕЙ. Два требования сразу, и оба решателю
--                 невыразимы: площади он не видит вовсе, а «выходит на улицу» — это требование, а
--                 не запрет. Отсюда же берётся парадный вход: дверь улица-зал объявлена
--                 обязательной, и стык для неё есть по построению;
--   КУХНЯ         самое большое место, СМЕЖНОЕ СО ДВОРОМ (а из таких — смежное ещё и с залом, если
--                 такое есть). Служебный вход — то, что делает кухню кухней, и он тоже требование.
--
-- Остальное — работа решателя: рядом с кухней покои встать не могут, значит там окажутся кладовые, и
-- это видно на карте, хотя никто такого не писал.

return function(step)
  local p = step.params
  local zones = step.writes.zones
  local state = step.writes.state

  local rect = zones:field("rect")
  local kind = zones:field("kind")
  local given = zones:field("given")
  local count = math.floor(state:field("zones"):get(0))
  if count <= 0 then
    error("roles: the layout produced no zones")
  end

  local CORRIDOR = math.floor(p.corridor_kind)
  local HALL = math.floor(p.hall_kind)
  local KITCHEN = math.floor(p.kitchen_kind)
  local STREET = math.floor(p.street_kind)
  local YARD = math.floor(p.yard_kind)

  local function area(i)
    return rect:get(i, 2) * rect:get(i, 3)
  end

  -- Смежность берётся из ТОГО ЖЕ CSR, который будет читать решатель: второй способ узнать, кто с кем
  -- граничит, однажды разошёлся бы с первым, и кухня оказалась бы «у двора» только в скрипте.
  local offsets = step.reads.zone_offsets:field("start")
  local arcs = step.reads.zone_arcs:field("zone")
  local function adjacent(a, b)
    for k = math.floor(offsets:get(a)), math.floor(offsets:get(a + 1)) - 1 do
      if math.floor(arcs:get(k)) == b then return true end
    end
    return false
  end

  -- Ноль значит «зона свободна», поэтому в поле кладётся номер вида ПЛЮС ОДИН.
  local street, yard = -1, -1
  for i = 0, count - 1 do
    local k = kind:get(i)
    if k == CORRIDOR or k == STREET or k == YARD then
      given:set(i, k + 1)
    end
    if k == STREET then street = i end
    if k == YARD then yard = i end
  end
  if street < 0 or yard < 0 then
    error("roles: the layout left the plot without a street or a yard")
  end

  -- САМОЕ БОЛЬШОЕ МЕСТО, ВЫХОДЯЩЕЕ НА УЛИЦУ. Не просто самое большое: парадный зал, до которого
  -- нельзя дойти с улицы, залом быть перестаёт, а объявленная обязательной дверь улица-зал не нашла
  -- бы стыка.
  local function pick(is_free, near)
    local best, best_area = -1, -1
    for i = 0, count - 1 do
      if is_free(i) and adjacent(i, near) and area(i) > best_area then
        best, best_area = i, area(i)
      end
    end
    return best
  end

  local function free(i)
    local k = kind:get(i)
    return k ~= CORRIDOR and k ~= STREET and k ~= YARD and given:get(i) == 0
  end

  local hall = pick(free, street)
  if hall < 0 then
    error("roles: no place on this plot faces the street, so there is nowhere to put the hall")
  end
  given:set(hall, HALL + 1)

  -- КУХНЯ У ДВОРА, и по возможности рядом с залом: служебный вход — то, что делает кухню кухней, а
  -- выход в зал — то, ради чего она стоит рядом. Первое обязательно, второе предпочтительно, и
  -- порядок именно такой.
  local kitchen = pick(function(i) return free(i) and adjacent(i, hall) end, yard)
  if kitchen < 0 then
    kitchen = pick(free, yard)
  end
  if kitchen < 0 then
    error("roles: no place on this plot faces the yard, so the kitchen would have no service entrance")
  end
  given:set(kitchen, KITCHEN + 1)

  -- КРУПНОЕ МЕСТО, НЕ СТАВШЕЕ ЗАЛОМ, СТАНОВИТСЯ ДВОРОМ. Раздел оставил его целым нарочно, и
  -- превращать его в спальню размером с площадь значило бы выбросить решение раздела: крупное место
  -- существует затем, чтобы быть крупным.
  local COURT = math.floor(p.court_kind)
  local big = math.floor(p.big_area)
  for i = 0, count - 1 do
    if free(i) and rect:get(i, 2) * rect:get(i, 3) >= big then
      given:set(i, COURT + 1)
    end
  end

  originator.graph_collapse{
    inputs = { offsets, arcs, step.reads.kinds:field("weight"),
               step.reads.adjacent_rules:field("allowed"), given },
    outputs = { kind, state:field("attempts"), state:field("rollbacks") },
    params = { attempts = p.role_attempts, rollbacks = p.role_rollbacks, history = p.role_history },
    range = { count = state:field("zones") },
  }
end
