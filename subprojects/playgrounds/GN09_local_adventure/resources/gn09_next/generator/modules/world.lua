-- Coordinate rules and buffer writers shared by the independent text generators.
local W = {}

function W.random(seed, x, z, salt)
  return base.prng64_normalize(base.mix(seed, x, z, salt))
end

function W.height(x, z)
  return -160 + 18 * math.sin(x / 570) + 12 * math.sin(z / 430)
end

function W.add(a, b) return {x = a.x + b.x, y = a.y + b.y, z = a.z + b.z} end
function W.sub(a, b) return {x = a.x - b.x, y = a.y - b.y, z = a.z - b.z} end
function W.scale(a, s) return {x = a.x * s, y = a.y * s, z = a.z * s} end
function W.length(a) return math.sqrt(a.x * a.x + a.y * a.y + a.z * a.z) end
function W.normal(a) return W.scale(a, 1 / W.length(a)) end

function W.put(field, i, p)
  field:set(i, p.x, 0)
  field:set(i, p.y, 1)
  field:set(i, p.z, 2)
end

function W.get(field, i)
  return {x = field:get(i, 0), y = field:get(i, 1), z = field:get(i, 2)}
end

function W.capsule(buffer, i, a, b, radius, tag)
  W.put(buffer:field('a'), i, a)
  W.put(buffer:field('b'), i, b)
  buffer:field('radius'):set(i, radius)
  buffer:field('tag'):set(i, tag)
end

function W.ball(buffer, i, centre, half, tag)
  W.put(buffer:field('centre'), i, centre)
  W.put(buffer:field('half'), i, half)
  buffer:field('tag'):set(i, tag)
end

return W
