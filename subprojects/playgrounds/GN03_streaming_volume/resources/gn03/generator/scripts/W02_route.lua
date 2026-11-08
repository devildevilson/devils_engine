-- Тело шага route: маршрут через узлы, распрямленный в ломаную.
--
-- Две вещи здесь стоит знать.
--
-- ОБХОД — ближайший сосед от нулевого узла. Не потому, что это лучший маршрут (он не лучший), а
-- потому, что он ДЕТЕРМИНИРОВАН и не зависит ни от порядка, ни от числа потоков: на каждом шаге
-- выбирается минимальное расстояние, а при равенстве — меньший индекс. Настоящая задача маршрута
-- (минимальная стоимость по рельефу, ветвления, узлы-развязки) принадлежит проекту, а не площадке.
--
-- Обход и вычисление кривой — общая механика originator.algorithms. Стиль, плотность
-- распрямления и группировка звеньев остаются правилами этого мира.

return function(step)
  local nodes = step.reads.nodes
  local points = step.writes.route_points
  local offsets = step.writes.route_offsets
  local state = step.writes.skeleton_state
  local p = step.params

  local node_position = nodes:field("position")
  local out = points:field("position")
  local out_offset = offsets:field("offset")
  local out_style = step.writes.route_styles:field("style")
  local capacity = points:count()
  local chain_capacity = offsets:count()

  local count = math.tointeger(state:field("node_count"):get(0))
  if count < 2 then
    out_offset:set(0, 0)
    out_offset:set(1, 0)
    state:field("point_count"):set(0, 0)
    state:field("chain_count"):set(0, 0)
    return
  end

  local function node_at(index)
    return node_position:get(index), node_position:get(index, 1), node_position:get(index, 2)
  end

  local algorithms=originator.algorithms
  local function point_at(index)
    local x,y,z=node_at(index)
    return {x=x,y=y,z=z}
  end
  local order=algorithms.nearest_order(count,point_at)

  -- Катмулл-Ром через узлы обхода: кривая проходит ЧЕРЕЗ точки, а не мимо них, и это важно — узел
  -- геймплейный, маршрут обязан прийти именно в него, а не рядом.
  local function control(i)
    local index = i
    if index < 1 then index = 1 end
    if index > #order then index = #order end
    return node_at(order[index])
  end

  local flatten = math.tointeger(p.route_flatten) or math.floor(p.route_flatten)
  local written = 0
  local function emit(x, y, z)
    if written >= capacity then
      error(string.format("route needs more than %d points: raise route_capacity or lower route_flatten", capacity))
    end
    out:set(written, x)
    out:set(written, y, 1)
    out:set(written, z, 2)
    written = written + 1
  end

  -- СТИЛЬ ВЫБИРАЕТСЯ НА ЗВЕНО, а подряд идущие звенья одного стиля становятся ОДНОЙ цепочкой. Так у
  -- мира получаются участки — несколько звеньев естественной пещеры, потом рукотворный тоннель, — а
  -- не чередование через шаг, которое читалось бы как дефект, а не как замысел.
  --
  -- Стиль — свойство ЦЕПОЧКИ, а не точки: он меняется на узлах маршрута, иначе сечение перескакивало
  -- бы с круглого на угловатое посреди хода.
  local styles = {}
  for i = 1, #order - 1 do
    local roll = base.prng64_normalize(base.prng64_2(step.seed, 0x51D + i))
    styles[i] = roll < p.bunker_share and 1 or 0
  end

  local chains = 0
  local function open_chain(style)
    if chains + 1 >= chain_capacity then
      error(string.format("route needs more than %d chains: raise route_chain_capacity", chain_capacity))
    end
    out_offset:set(chains, written)
    out_style:set(chains, style)
    chains = chains + 1
  end

  local link = 1
  while link <= #order - 1 do
    local style = styles[link]
    local last = link
    while last + 1 <= #order - 1 and styles[last + 1] == style do
      last = last + 1
    end

    open_chain(style)
    if style == 0 then
      -- ЕСТЕСТВЕННАЯ ПЕЩЕРА: Катмулл-Ром через узлы, распрямленный в ломаную. Кривая проходит ЧЕРЕЗ
      -- узлы, а не мимо: узел геймплейный, маршрут обязан прийти именно в него.
      for i = link, last do
        local x0, y0, z0 = control(i - 1)
        local x1, y1, z1 = control(i)
        local x2, y2, z2 = control(i + 1)
        local x3, y3, z3 = control(i + 2)
        for k = 0, flatten - 1 do
          local t = k / flatten
          local point=algorithms.catmull_rom(
            {x=x0,y=y0,z=z0},{x=x1,y=y1,z=z1},
            {x=x2,y=y2,z=z2},{x=x3,y=y3,z=z3},t,'weights')
          emit(point.x,point.y,point.z)
        end
      end
      local ex, ey, ez = control(last + 1)
      emit(ex, ey, ez)
    else
      -- ТОННЕЛЬ БУНКЕРА: узлы соединяются ПРЯМО, без сглаживания. Рукотворный ход не изгибается по
      -- Катмулл-Рому — он идёт отрезками и поворачивает в узле, и это видно так же ясно, как
      -- угловатое сечение.
      for i = link, last do
        local sx, sy, sz = control(i)
        emit(sx, sy, sz)
      end
      local ex, ey, ez = control(last + 1)
      emit(ex, ey, ez)
    end

    link = last + 1
  end

  -- Замыкающее смещение CSR: у последней цепочки должен быть конец.
  out_offset:set(chains, written)
  state:field("point_count"):set(0, written)
  state:field("chain_count"):set(0, chains)
end
