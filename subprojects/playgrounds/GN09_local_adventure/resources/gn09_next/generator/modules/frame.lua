-- Outpost frame: free yaw around Y, decks stay horizontal.
-- Local x = right, y = up, z = forward; world = origin + x*(c,0,s) + y*(0,1,0) + z*(-s,0,c).
local F = {}

function F.new(origin, c, s)
  local frame = {origin = origin, c = c, s = s}
  function frame.dir(p) return {x = p.x * c - p.z * s, y = p.y, z = p.x * s + p.z * c} end
  function frame.point(p)
    local d = frame.dir(p)
    return {x = origin.x + d.x, y = origin.y + d.y, z = origin.z + d.z}
  end
  function frame.to_local(p)
    local dx, dy, dz = p.x - origin.x, p.y - origin.y, p.z - origin.z
    return {x = dx * c + dz * s, y = dy, z = -dx * s + dz * c}
  end
  -- World half extents of a local box, for conservative bounds.
  function frame.extent(h)
    return {x = math.abs(c) * h.x + math.abs(s) * h.z, y = h.y, z = math.abs(s) * h.x + math.abs(c) * h.z}
  end
  return frame
end

-- Frame whose forward axis is the horizontal direction d.
function F.facing(origin, d)
  local length = math.sqrt(d.x * d.x + d.z * d.z)
  assert(length > 1e-6, 'outpost frame needs a horizontal forward direction')
  return F.new(origin, d.z / length, -d.x / length)
end

function F.from_meta(meta)
  local o = meta:field('origin')
  return F.new({x = o:get(0, 0), y = o:get(0, 1), z = o:get(0, 2)}, meta:field('yaw_c'):get(0),
    meta:field('yaw_s'):get(0))
end

-- A cut box: world centre for the field, local centre for grammar steps.
function F.box(frame, buffer, i, centre, half, tag)
  local world = frame.point(centre)
  for axis, name in ipairs({'x', 'y', 'z'}) do
    buffer:field('centre'):set(i, world[name], axis - 1)
    buffer:field('local_centre'):set(i, centre[name], axis - 1)
    buffer:field('half'):set(i, half[name], axis - 1)
  end
  buffer:field('yaw_c'):set(i, frame.c)
  buffer:field('yaw_s'):set(i, frame.s)
  buffer:field('tag'):set(i, tag)
end

return F
