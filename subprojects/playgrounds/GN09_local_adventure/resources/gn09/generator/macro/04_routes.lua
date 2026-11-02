-- Маршруты сектора: на восток (направление 1, к x + 1) и на юг (2, к z + 1).
--
-- У маршрута два промежуточных узла с изгибами в разные стороны, вертикальный профиль (ровный,
-- нырок, арка, зигзаг; отвесная шахта стоит в середине маршрута, не у баз — док подходит к станции
-- сбоку), место-ориентир на втором узле и тупик уступами от первого — в ту сторону, где дальше от баз.
--
-- Формулы повторяют мир хоста бит в бит (base.mix, base.umod, base.hypot, порядок операций).

local W = modules.world
local world_y_min, world_y_max = W.world_y_min, W.world_y_max
local clamp, node_key, random01, is_base = W.clamp, W.node_key, W.random01, W.is_base
local read_halls, inside_hall = W.read_halls, W.inside_hall

local function read_base(instance)
  local b = instance.base
  local centre, raw = b:field("centre"), b:field("raw")
  return {
    x = centre:get(0, 0), y = centre:get(0, 1), z = centre:get(0, 2),
    rx = raw:get(0, 0), ry = raw:get(0, 1), rz = raw:get(0, 2),
    kind = math.tointeger(b:field("kind"):get(0)),
  }
end

local function length(x, y, z) return math.sqrt(x * x + y * y + z * z) end

return function(step)
  local seed = step.world_seed
  local x, z = step.key[1], step.key[2]
  local halls = read_halls(step.links.halls)
  local function in_any_hall(px, py, pz)
    for _, h in ipairs(halls) do
      if inside_hall(h, px, py, pz) then return true end
    end
    return false
  end
  local ends = { read_base(step.links.ends[1]), read_base(step.links.ends[2]), read_base(step.links.ends[3]) }
  local around = {}
  for i, instance in ipairs(step.links.around) do around[i] = read_base(instance) end

  local nodes = step.writes.route_nodes
  local n_key, n_centre, n_kind, n_rank, n_dir =
    nodes:field("key"), nodes:field("centre"), nodes:field("kind"), nodes:field("rank"), nodes:field("direction")
  local edges = step.writes.route_edges
  local e_a, e_b, e_dir = edges:field("a"), edges:field("b"), edges:field("direction")
  local node_count, edge_count = 0, 0

  local function lay(direction)
    local from, to = ends[1], ends[direction + 1]
    local local_index = 0
    local function append(px, py, pz, kind, rank, key)
      if node_count >= nodes:count() then error("routes: node capacity " .. nodes:count() .. " exceeded") end
      n_key:set(node_count, key)
      n_centre:set(node_count, px, 0); n_centre:set(node_count, py, 1); n_centre:set(node_count, pz, 2)
      n_kind:set(node_count, kind)
      n_rank:set(node_count, rank)
      n_dir:set(node_count, direction)
      node_count = node_count + 1
      local_index = local_index + 1
      return local_index - 1
    end
    local function connect(a, b)
      if edge_count >= edges:count() then error("routes: edge capacity " .. edges:count() .. " exceeded") end
      e_a:set(edge_count, a)
      e_b:set(edge_count, b)
      e_dir:set(edge_count, direction)
      edge_count = edge_count + 1
    end
    local FROM, TO = -1, -2

    local dx, dy, dz = to.x - from.x, to.y - from.y, to.z - from.z
    local horizontal = base.hypot(dx, dz)
    local sx, sz = -dz / horizontal, dx / horizontal
    local key = node_key(x, z, direction, 0)
    local sign = base.umod(base.mix(seed, key, 7), 2) ~= 0 and 1.0 or -1.0
    local first_bend = sign * (12.0 + 7.0 * random01(seed, key, 101))
    local second_bend = -sign * (11.0 + 7.0 * random01(seed, key, 102))
    local first_t = 0.27 + 0.06 * random01(seed, key, 103)
    local second_t = 0.68 + 0.06 * random01(seed, key, 104)
    -- 0 ровный, 1 нырок, 2 арка, 3 зигзаг
    local shape = base.umod(base.mix(seed, key, 109), 4)
    local depth = 40.0 + 70.0 * random01(seed, key, 110)
    local function inside_world(y) return clamp(y, world_y_min + 40.0, world_y_max - 40.0) end

    local first_y
    if direction == 1 then
      first_y = from.y + (to.y - from.y) * first_t + (random01(seed, key, 105) - 0.5) * 8.0
    else
      first_y = from.y + (to.y - from.y) * first_t + sign * (6.0 + 4.0 * random01(seed, key, 105))
    end
    local fx = (from.x + dx * first_t) + sx * first_bend
    local fz = (from.z + dz * first_t) + sz * first_bend
    local fy = first_y
    local previous = FROM
    local first_id = append(fx, fy, fz, 0, 3, node_key(x, z, direction, 1))
    connect(previous, first_id)
    previous = first_id
    local s2x = (from.x + dx * second_t) + sx * second_bend
    local s2z = (from.z + dz * second_t) + sz * second_bend
    local s2y = to.y + (from.y - to.y) * (1.0 - second_t) - sign * (6.0 + 4.0 * random01(seed, key, 106))

    if shape == 0 then
      -- Восточное соединение ровного маршрута содержит короткое вертикальное ребро.
      if direction == 1 then
        local shaft = append(fx, fy + 12.0, fz, 0, 2, node_key(x, z, direction, 2))
        connect(previous, shaft)
        previous = shaft
      end
    else
      local down = shape == 2 and 1.0 or -1.0
      local by = inside_world(fy + down * (shape == 3 and 0.6 or 1.0) * depth)
      local shaft = append(fx, by, fz, 0, 2, node_key(x, z, direction, 4))
      connect(previous, shaft)
      previous = shaft
      if shape == 3 then
        local cx = fx + (s2x - fx) * 0.5
        local cz = fz + (s2z - fz) * 0.5
        local cy = inside_world(0.5 * (fy + s2y) + 0.6 * depth)
        local crest = append(cx, cy, cz, 0, 2, node_key(x, z, direction, 5))
        connect(previous, crest)
        previous = crest
      end
    end

    local landmark_step = direction == 1 and 3 or 2
    -- Малое природное место внутри зала не имело бы стен, а на маршруте к узлу зала оно не лежало бы
    -- между двумя базами — там узел пустой.
    local hall_route = not is_base(from.kind) or not is_base(to.kind)
    local landmark
    if hall_route or in_any_hall(s2x, s2y, s2z) then
      landmark = 0
    else
      landmark = base.umod(base.mix(seed, key, 107), 2) == 0 and 3 or 4
    end
    local second_id = append(s2x, s2y, s2z, landmark, 2, node_key(x, z, direction, landmark_step))
    connect(previous, second_id)
    connect(second_id, TO)

    local branch_length = 1 + base.umod(base.mix(seed, key, 108), 3)
    local function branch_point(branch_sign, s)
      local offset = branch_sign * (12.0 + 13.0 * s)
      local along = (s % 2 == 1 and 1.0 or -1.0) * 0.05
      return (fx + sx * offset) + dx * along, (fy + 0.0 * offset) + dy * along, (fz + sz * offset) + dz * along
    end
    local function branch_clearance(branch_sign)
      local nearest = 1e30
      for s = 1, branch_length do
        local px, py, pz = branch_point(branch_sign, s)
        for _, b in ipairs(around) do
          local l = length(px - b.rx, py - b.ry, pz - b.rz)
          if l < nearest then nearest = l end
        end
      end
      return nearest
    end
    local branch_sign = branch_clearance(-sign) > branch_clearance(sign) and -sign or sign
    previous = first_id
    -- Тупик уступами: часть ответвлений уходит вверх или вниз.
    local branch_rise = (base.umod(base.mix(seed, key, 113), 3) - 1.0) * (8.0 + 14.0 * random01(seed, key, 114))
    for s = 1, branch_length do
      local px, _, pz = branch_point(branch_sign, s)
      -- Чередование по высоте растёт вместе с подъёмом уступа: иначе подъём съедает зигзаг.
      local wiggle = 5.0 + 0.5 * math.abs(branch_rise)
      local py = clamp(fy + (s % 2 == 1 and wiggle or -wiggle + 1.0) + branch_rise * s,
                       world_y_min + 20.0, world_y_max - 20.0)
      local id = append(px, py, pz, 0, s == branch_length and 1 or 2, node_key(x, z, direction, 10, s))
      connect(previous, id)
      previous = id
    end
  end

  lay(1)
  lay(2)
  local counts = step.writes.route_counts
  counts:field("nodes"):set(0, node_count)
  counts:field("edges"):set(0, edge_count)
end
