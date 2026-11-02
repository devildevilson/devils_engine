-- Якоря канонической окрестности середины пары не дальше tunnel_partner_reach + station_reach от середины:
-- их станции туннель обходит.

local W = modules.world

return function(step)
  local pair = step.reads.tunnel_pair:field("centre")
  local mx = (pair:get(0, 0) + pair:get(1, 0)) * 0.5
  local my = (pair:get(0, 1) + pair:get(1, 1)) * 0.5
  local mz = (pair:get(0, 2) + pair:get(1, 2)) * 0.5
  local reach = step.params.tunnel_partner_reach + step.params.station_reach
  local out, made = step.writes.tunnel_around, 0
  local station = out:field("station")
  for _, a in ipairs(W.station_anchors(step.world_seed, step.reads.nodes, step.reads.graph_meta, step.links.regions)) do
    local dx, dy, dz = a.centre.x - mx, a.centre.y - my, a.centre.z - mz
    if math.sqrt(dx * dx + dy * dy + dz * dz) <= reach then
      if made >= out:count() then error("tunnel_scope: capacity exceeded") end
      local sx, sz, key = W.station_key(a)
      station:set(made, sx, 0); station:set(made, sz, 1); station:set(made, key, 2)
      made = made + 1
    end
  end
  step.writes.around_counts:field("stations"):set(0, made)
end
