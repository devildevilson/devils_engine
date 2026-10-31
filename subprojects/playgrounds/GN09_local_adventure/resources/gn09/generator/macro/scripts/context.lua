-- Локатор канонической окрестности: окно 3x3 секторов с сектором ключа в середине.

return function(step)
  local x, z = step.key[1], step.key[2]
  local canonical = step.writes.canonical
  local corner = canonical:field("corner")
  corner:set(0, x - 1, 0)
  corner:set(0, z - 1, 1)
  canonical:field("size"):set(0, 3 | (3 << 32))
end
