-- Тело шага planet: раскладка по ГРАФУ СОСЕДСТВА замкнутой поверхности.
--
-- У растра сосед задан формой буфера, и осей две. У сферы плоскости нет: любая развёртка даёт шов, а
-- шов — место, где соседство считается неверно и молча. Поэтому здесь соседство ДАННЫЕ (CSR), и
-- решатель не знает ни про «правее», ни про «ниже».
--
-- Отсюда единственное настоящее отличие: у дуги НЕТ НАПРАВЛЕНИЯ, значит матрица ОДНА и она обязана
-- быть симметричной. Несимметричную инструмент отклоняет вслух и называет пару — тихая симметризация
-- дала бы правила, которых никто не писал.

return function(step)
  local p = step.params
  local cells = step.writes.cells
  local rules = step.writes.rules

  -- Решётка Фибоначчи: площадь клетки почти одинакова по всей поверхности, поэтому «сколько вышло
  -- воды» считается счётом клеток без весов площади.
  originator.sphere_points{
    outputs = { cells:field("position") },
    params = { count = cells:count(), radius = 1.0, axis = 1 },
  }

  originator.sphere_adjacency{
    inputs = { cells:field("position") },
    outputs = { step.writes.cell_offsets:field("start"), step.writes.cell_arcs:field("cell") },
    params = { neighbours = p.neighbours },
    range = { 0, cells:count() },
    key_support = "global",
  }

  local count = math.tointeger(p.tile_count) or math.floor(p.tile_count)
  local allowed = rules:field("allowed")
  local function link(a, b)
    -- ОДНА клетка матрицы на пару, обе стороны: «a рядом с b» и «b рядом с a» на графе — одно
    -- утверждение.
    allowed:set(a * count + b, 1)
    allowed:set(b * count + a, 1)
  end

  for i = 0, count - 1 do
    link(i, i)
  end
  for i = 0, count - 2 do
    link(i, i + 1)
  end

  -- ЗАРАНЕЕ ЗАДАННЫЕ ВЕРШИНЫ И ВПАДИНЫ. У той же лесенки на графе степени 6 НЕТ РАВНОВЕСИЯ:
  -- свободная сфера уходит целиком в воду, а сфера с двумя десятками снежных вершин — целиком в
  -- горы. У правила «сосед через ступень» нет возвращающей силы, и весь шар едет туда, куда его
  -- толкнуло первое условие. Поэтому условий ДВА, с разных концов лесенки, и между ними появляется
  -- склон. Расставлены они равномерным шагом по НОМЕРУ клетки: решётка Фибоначчи выложена по
  -- спирали, поэтому равный шаг по номеру это почти равный шаг по поверхности.
  local given = cells:field("given")
  local function pin(how_many, tile, offset)
    if how_many <= 0 then return end
    local stride = math.floor(cells:count() / how_many)
    for i = 0, how_many - 1 do
      -- Ноль значит «клетка свободна», поэтому в поле кладётся номер тайла ПЛЮС ОДИН.
      given:set((i * stride + offset) % cells:count(), tile + 1)
    end
  end

  local peaks = math.tointeger(p.peaks) or math.floor(p.peaks)
  local basins = math.tointeger(p.basins) or math.floor(p.basins)
  pin(peaks, count - 1, 0)
  pin(basins, 0, math.floor(cells:count() / (2 * math.max(basins, 1))))

  originator.graph_collapse{
    inputs = { step.writes.cell_offsets:field("start"), step.writes.cell_arcs:field("cell"),
               step.reads.tiles:field("weight"), allowed, given },
    outputs = { cells:field("tile"), step.writes.state:field("attempts"),
                step.writes.state:field("rollbacks") },
    params = { attempts = p.attempts, rollbacks = p.rollbacks, history = p.history },
    range = { 0, cells:count() },
  }
end
