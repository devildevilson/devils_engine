-- Ближайший кандидат с построенной станцией; при равном расстоянии — меньший ключ. Тот же перебор, что у
-- хоста nearest_partner: порядок кандидатов значим только для равных расстояний.

return function(step)
  local out = step.writes.tunnel_partner
  out:field("found"):set(0, 0)
  if step.links.own[1].station_meta:field("built"):get(0) == 0 then return end
  local self = step.reads.tunnel_self:field("centre")
  local cx, cy, cz = self:get(0, 0), self:get(0, 1), self:get(0, 2)
  local cand = step.reads.tunnel_candidates
  local station, anchor, centre = cand:field("station"), cand:field("anchor"), cand:field("centre")
  local best, best_distance, best_key = nil, step.params.tunnel_partner_reach, nil
  for i = 0, math.tointeger(step.reads.candidate_counts:field("candidates"):get(0)) - 1 do
    local dx, dy, dz = centre:get(i, 0) - cx, centre:get(i, 1) - cy, centre:get(i, 2) - cz
    local distance = math.sqrt(dx * dx + dy * dy + dz * dz)
    local key = anchor:get(i)
    if not (distance > best_distance or (best ~= nil and distance == best_distance and math.ult(best_key, key))) then
      if step.links.others[i + 1].station_meta:field("built"):get(0) ~= 0 then
        best, best_distance, best_key = i, distance, key
      end
    end
  end
  if best == nil then return end
  out:field("found"):set(0, 1)
  for w = 0, 2 do out:field("station"):set(0, station:get(best, w), w) end
  out:field("anchor"):set(0, best_key)
  for c = 0, 2 do out:field("centre"):set(0, centre:get(best, c), c) end
end
