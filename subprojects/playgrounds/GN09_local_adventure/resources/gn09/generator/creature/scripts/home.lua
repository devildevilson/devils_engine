-- Окно вокруг зала: сектора, накрывающие область влияния зала (relevance — данные генератора halls), и
-- ещё два сектора во все стороны. Тот же расчёт, что у хоста window_around(h, 2).

local W = modules.world

return function(step)
  local halls = step.reads.halls
  local count = math.tointeger(step.reads.hall_counts:field("halls"):get(0))
  local key, centre, forward, relevance = halls:field("key"), halls:field("centre"), halls:field("forward"), halls:field("relevance")
  local index = nil
  for i = 0, count - 1 do
    if key:get(i) == step.key[3] then index = i break end
  end
  if index == nil then error("creature_home: no hall with this key in the region") end
  local h = { cx = centre:get(index, 0), cy = centre:get(index, 1), cz = centre:get(index, 2),
              fx = forward:get(index, 0), fy = forward:get(index, 1), fz = forward:get(index, 2) }
  local hx, hz = relevance:get(index, 0), relevance:get(index, 2)
  local min_x, max_x, min_z, max_z = 1e30, -1e30, 1e30, -1e30
  for _, u in ipairs({ -hx, hx }) do
    for _, v in ipairs({ -hz, hz }) do
      local x, _, z = W.hall_to_world(h, u, 0.0, v)
      min_x = math.min(min_x, x); max_x = math.max(max_x, x)
      min_z = math.min(min_z, z); max_z = math.max(max_z, z)
    end
  end
  local margin = 2
  local s = W.sector_size
  local x0, z0 = math.floor(min_x / s) - margin, math.floor(min_z / s) - margin
  local x1, z1 = math.floor(max_x / s) + 1 + margin, math.floor(max_z / s) + 1 + margin
  local home = step.writes.creature_home
  local area = home:field("area")
  area:set(0, x0, 0)
  area:set(0, z0, 1)
  area:set(0, (x1 - x0) | ((z1 - z0) << 32), 2)
  home:field("hall"):set(0, index)
end
