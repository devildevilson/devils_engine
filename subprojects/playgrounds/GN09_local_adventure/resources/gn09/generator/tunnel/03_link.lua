-- Взаимность пары: ближайший к партнёру — снова этот якорь. Первый — меньший ключ (беззнаково).

local W = modules.world

return function(step)
  local link = step.writes.tunnel_link
  link:field("mutual"):set(0, 0)
  link:field("pairs"):set(0, 0)
  local partner = step.reads.tunnel_partner
  if partner:field("found"):get(0) == 0 then return end
  local back = step.links.back[1].tunnel_partner
  local self = step.reads.tunnel_self
  if back:field("found"):get(0) == 0 or back:field("anchor"):get(0) ~= self:field("anchor"):get(0) then return end
  local own = { buffer = self }
  local other = { buffer = partner }
  local own_key, other_key = self:field("anchor"):get(0), partner:field("anchor"):get(0)
  local first, second = own, other
  if not math.ult(own_key, other_key) then first, second = other, own end
  local pair = step.writes.tunnel_pair
  local centres = {}
  for i, entry in ipairs({ first, second }) do
    for w = 0, 2 do pair:field("station"):set(i - 1, entry.buffer:field("station"):get(0, w), w) end
    pair:field("anchor"):set(i - 1, entry.buffer:field("anchor"):get(0))
    local c = entry.buffer:field("centre")
    centres[i] = { x = c:get(0, 0), y = c:get(0, 1), z = c:get(0, 2) }
    for k = 0, 2 do pair:field("centre"):set(i - 1, c:get(0, k), k) end
  end
  link:field("mutual"):set(0, 1)
  link:field("pairs"):set(0, 2)
  for w = 0, 2 do link:field("first_station"):set(0, first.buffer:field("station"):get(0, w), w) end
  link:field("first"):set(0, first.buffer:field("anchor"):get(0))
  link:field("second"):set(0, second.buffer:field("anchor"):get(0))
  local mx = (centres[1].x + centres[2].x) * 0.5
  local mz = (centres[1].z + centres[2].z) * 0.5
  local corner = link:field("corner")
  corner:set(0, math.floor(mx / W.sector_size) - 1, 0)
  corner:set(0, math.floor(mz / W.sector_size) - 1, 1)
  link:field("size"):set(0, 3 | (3 << 32))
end
