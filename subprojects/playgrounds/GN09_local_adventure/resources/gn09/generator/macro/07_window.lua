-- Окружение окна: галерея на каждое ребро графа (порты у мест, ширины и свод по биому, две смещённые
-- полости, иногда сужение, иногда декоративный карман) и шар поворота лодки в каждом узле. Правила — в
-- модуле `world` (тот же генератор галерей строит и ходы пещер мест, в меньшем масштабе); формулы поля по
-- этим строкам — shared/field/*.ds.

local W = modules.world

return function(step)
  local p = step.params
  local seed = step.world_seed
  local limits = step.writes.limits
  limits:field("core"):set(0, p.cave_core)
  limits:field("main_radius"):set(0, math.max(p.sub_turn_radius, p.cave_core + 0.5))
  limits:field("extent"):set(0, p.cave_extent)
  limits:field("clearance"):set(0, p.clearance_radius)

  local nodes = step.reads.nodes
  local n_key, n_centre, n_kind, n_rank = nodes:field("key"), nodes:field("centre"), nodes:field("kind"), nodes:field("rank")
  local meta = step.reads.graph_meta
  local node_count = math.tointeger(meta:field("nodes"):get(0))
  local edge_count = math.tointeger(meta:field("edges"):get(0))
  local route = {}
  for i = 0, node_count - 1 do
    route[i] = { key = n_key:get(i), centre = { x = n_centre:get(i, 0), y = n_centre:get(i, 1), z = n_centre:get(i, 2) },
                 kind = math.tointeger(n_kind:get(i)), rank = math.tointeger(n_rank:get(i)) }
  end
  local e_a, e_b = step.reads.edges:field("a"), step.reads.edges:field("b")

  local w = W.shape_writers(step)
  for i = 0, edge_count - 1 do
    local from, to = route[math.tointeger(e_a:get(i))], route[math.tointeger(e_b:get(i))]
    W.write_gallery(w, W.macro_gallery(seed, from, to, p.cave_core, p), from.centre, to.centre)
    local pocket = W.edge_pocket(seed, from, to)
    if pocket then W.write_pocket(w, pocket, p.cave_core) end
  end
  for i = 0, node_count - 1 do
    local n = route[i]
    W.write_turn(w, n.centre, W.turn_radius(seed, n, p), W.turn_rock(seed, n), W.turn_relief)
  end
  W.finish_shapes(w)
end
