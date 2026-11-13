-- Shell mesh from the wall records: slabs around their holes (passages from the walls step, windows
-- and emergency hatches from the exposure step), glass in windows, ladder rungs under floor hatches
-- and along vertical shafts.
local W, M, F, O = modules.world, modules.mesh, modules.frame, modules.outpost
local names = {'x', 'y', 'z'}
return function(step)
  local rooms, walls, openings = step.reads.station_rooms, step.writes.station_walls, step.reads.station_openings
  local meta = step.writes.station_meta
  if meta:field('built'):get(0) == 0 then return end
  local frame = F.from_meta(meta)
  local mesh = M.writer(step.writes.station_mesh, frame)
  local function slab(c, h, shade)
    mesh.box({c.x-h.x, c.y-h.y, c.z-h.z}, {c.x+h.x, c.y+h.y, c.z+h.z}, shade)
  end
  local function room(i)
    return {c = W.get(rooms:field('local_centre'), i), h = W.get(rooms:field('half'), i), label = rooms:field('label'):get(i)}
  end
  local function shaft(q)
    return (q.label == O.lock or q.label == O.passage) and q.h.y>q.h.x and q.h.y>q.h.z
  end
  local function rungs(x, z, from, to)
    for y = from, to, 0.7 do slab({x = x, y = y, z = z}, {x = 0.45, y = 0.04, z = 0.04}, 112) end
  end
  local holes = {}
  for i = 0, meta:field('openings'):get(0)-1 do
    local w = openings:field('wall'):get(i)
    holes[w] = holes[w] or {}
    table.insert(holes[w], {c = W.get(openings:field('local_centre'), i), h = W.get(openings:field('half'), i), kind = openings:field('kind'):get(i)})
  end
  for w = 0, meta:field('walls'):get(0)-1 do
    local axis = names[walls:field('axis'):get(w)+1]
    local pa, pb = O.plane_axes[axis][1], O.plane_axes[axis][2]
    local c, h = W.get(walls:field('local_centre'), w), W.get(walls:field('half'), w)
    local other, owner = walls:field('other'):get(w), room(walls:field('room'):get(w))
    local list = holes[w] or {}
    if walls:field('opening'):get(w) == 1 then
      list[#list+1] = {c = W.get(walls:field('hole_centre'), w), h = W.get(walls:field('hole_half'), w), kind = 1}
    end
    local pieces = {{l = {c[pa]-h[pa], c[pb]-h[pb]}, h = {c[pa]+h[pa], c[pb]+h[pb]}}}
    for _, hole in ipairs(list) do
      pieces = O.subtract(pieces, {hole.c[pa]-hole.h[pa], hole.c[pb]-hole.h[pb]}, {hole.c[pa]+hole.h[pa], hole.c[pb]+hole.h[pb]})
    end
    local shade = (other<0 and walls:field('outside'):get(w) ~= 0) and 116 or 112
    for _, p in ipairs(pieces) do
      local centre, half = {x = c.x, y = c.y, z = c.z}, {x = 0.25, y = 0.25, z = 0.25}
      centre[pa], centre[pb] = (p.l[1]+p.h[1])/2, (p.l[2]+p.h[2])/2
      half[pa], half[pb] = (p.h[1]-p.l[1])/2, (p.h[2]-p.l[2])/2
      slab(centre, half, shade)
    end
    for _, hole in ipairs(list) do
      if hole.kind == 3 then
        local glass = {x = hole.h.x, y = hole.h.y, z = hole.h.z}
        glass[axis] = 0.08
        slab(hole.c, glass, 113)
      end
    end
    -- A ladder under every floor hatch, unless the lower room is a shaft with its own rungs.
    if walls:field('opening'):get(w) == 1 and axis == 'y' then
      local lower = walls:field('sign'):get(w)>0 and owner or room(other)
      if not shaft(lower) then
        local hc, hh = W.get(walls:field('hole_centre'), w), W.get(walls:field('hole_half'), w)
        rungs(hc.x, hc.z-hh.z+0.15, lower.c.y-lower.h.y+0.5, c.y-0.3)
      end
    end
  end
  for i = 0, meta:field('rooms'):get(0)-1 do
    local q = room(i)
    if shaft(q) then rungs(q.c.x, q.c.z-q.h.z+0.25, q.c.y-q.h.y+0.4, q.c.y+q.h.y-0.2) end
  end
  meta:field('triangles'):set(0, mesh.count())
end
