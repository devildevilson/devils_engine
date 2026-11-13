-- Small mesh writer. All dimensions and openings are chosen by the station grammar.
-- With a frame (modules.frame), boxes are given in its local coordinates.
local M = {}
function M.writer(buffer, frame)
  local count = 0
  local function world(p, direction)
    if not frame then return p end
    local q = {x = p[1], y = p[2], z = p[3]}
    q = direction and frame.dir(q) or frame.point(q)
    return {q.x, q.y, q.z}
  end
  local function triangle(a, b, c, n, shade)
    assert(count < buffer:count(), 'station triangle capacity exceeded')
    a, b, c, n = world(a), world(b), world(c), world(n, true)
    for name, p in pairs({a = a, b = b, c = c, normal = n}) do
      local f = buffer:field(name)
      for axis = 1, 3 do f:set(count, p[axis], axis - 1) end
    end
    buffer:field('shade'):set(count, shade)
    count = count + 1
  end
  local function quad(a, b, c, d, n, shade)
    triangle(a, b, c, n, shade)
    triangle(a, c, d, n, shade)
  end
  local function box(low, high, shade)
    for axis = 1, 3 do
      local u, v = axis % 3 + 1, (axis + 1) % 3 + 1
      for _, sign in ipairs({-1, 1}) do
        local points = {}
        for i, pair in ipairs({{0, 0}, {1, 0}, {1, 1}, {0, 1}}) do
          local p = {0, 0, 0}
          p[axis] = sign < 0 and low[axis] or high[axis]
          p[u] = pair[1] == 0 and low[u] or high[u]
          p[v] = pair[2] == 0 and low[v] or high[v]
          points[i] = p
        end
        local n = {0, 0, 0}
        n[axis] = sign
        if sign < 0 then
          quad(points[4], points[3], points[2], points[1], n, shade)
        else
          quad(points[1], points[2], points[3], points[4], n, shade)
        end
      end
    end
  end
  return {box = box, count = function() return count end}
end
return M
