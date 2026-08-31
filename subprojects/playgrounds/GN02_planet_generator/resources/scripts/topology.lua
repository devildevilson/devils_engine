-- Тело шага topology: форма планеты и соседство.
--
-- Здесь нет ни одного мирового понятия — ни суши, ни климата. Это намеренно: топология замкнутой
-- поверхности одинакова для планеты, для луны и для газового шара, а всё остальное лежит на ней
-- сверху. Если шаг начнёт знать про сушу, его нельзя будет переиспользовать.

return function(step)
  local cells = step.writes.cells
  local position = cells:field("position")

  -- Решётка Фибоначчи: площадь клетки почти одинакова по всей поверхности. Отсюда важное следствие
  -- для всего дальнейшего — суммы по клеткам можно считать без весов площади.
  originator.sphere_points{
    outputs = { position },
    params = { count = cells:count(), radius = 1.0, axis = step.params.axis },
  }

  -- Синус широты. Он нужен ветрам, инсоляции и правилу суши, а достать компоненту вектора правило
  -- на devils_script не может: его скоуп — скалярные поля элемента. Значит компонента становится
  -- полем, и стоит она один pointwise-проход.
  originator.axis_component{
    inputs = { position },
    outputs = { cells:field("lat_sin") },
    params = { axis = step.params.axis },
  }

  -- Поле единиц. Выглядит служебным, но это единственный способ сказать «стоимость шага равна
  -- единице» и «маска включает всё» инструментам, которые берут и то и другое ПОЛЕМ, а не числом:
  -- иначе у них появился бы второй способ получать те же данные.
  originator.fill{
    outputs = { cells:field("one") },
    params = { value = 1.0 },
  }

  -- Соседство: K ближайших, симметризованные в канонический CSR. Это ответ на вопрос, который на
  -- сфере нельзя решить растром: у планеты нет края, и любая развёртка даёт шов, где соседство
  -- считается неверно молча.
  originator.sphere_adjacency{
    inputs = { position },
    outputs = { step.writes.cell_offsets:field("start"), step.writes.cell_arcs:field("cell") },
    params = { neighbours = step.params.neighbours },
    range = { 0, cells:count() },
    key_support = "global",
  }
end
