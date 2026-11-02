-- Кандидаты в пару: якоря канонической окрестности (кроме себя) не дальше tunnel_partner_reach. Порядок
-- — порядок якорей окна (базы графа, затем аванпосты залов), как у хоста.

local W = modules.world

return function(step)
  local anchors = W.station_anchors(step.world_seed, step.reads.nodes, step.reads.graph_meta, step.links.regions)
  local own = nil
  for _, a in ipairs(anchors) do if a.key == step.key[3] then own = a break end end
  if own == nil then error("tunnel_candidates: no anchor with this key in the canonical window") end
  local function put(buffer, i, a)
    local sx, sz, key = W.station_key(a)
    local station = buffer:field("station")
    station:set(i, sx, 0); station:set(i, sz, 1); station:set(i, key, 2)
    buffer:field("anchor"):set(i, a.key)
    local c = buffer:field("centre")
    c:set(i, a.centre.x, 0); c:set(i, a.centre.y, 1); c:set(i, a.centre.z, 2)
  end
  put(step.writes.tunnel_self, 0, own)
  local reach = step.params.tunnel_partner_reach
  local out, made = step.writes.tunnel_candidates, 0
  for _, a in ipairs(anchors) do
    if a.key ~= own.key then
      local dx, dy, dz = a.centre.x - own.centre.x, a.centre.y - own.centre.y, a.centre.z - own.centre.z
      if math.sqrt(dx * dx + dy * dy + dz * dz) <= reach then
        if made >= out:count() then error("tunnel_candidates: capacity exceeded") end
        put(out, made, a)
        made = made + 1
      end
    end
  end
  step.writes.candidate_counts:field("candidates"):set(0, made)
end
