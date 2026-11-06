-- Rooms are a small regular floor graph. Excavation enters the thickness of the shell.
local W = modules.world
return function(step)
  local mouth, direction, dock, kind = W.port(step)
  if kind ~= 2 then return end
  local floors = 2+base.umod(base.mix(step.world_seed, step.key[1], step.key[2], 501), 2)
  local body = W.add(mouth, W.scale(direction, math.abs(direction.y)>0.5 and 4 or 17))
  local low = {x = body.x-12, y = body.y-floors*3, z = body.z-10}
  local rooms, cuts = step.writes.station_rooms, step.writes.station_cut
  local count = 0
  for floor = 0, floors-1 do
    for z = 0, 1 do for x = 0, 1 do
      local centre = W.add(low, {x = x*12+6, y = floor*6+3, z = z*10+5})
      W.put(rooms:field('centre'), count, centre)
      W.put(rooms:field('half'), count, {x = 6, y = 3, z = 5})
      rooms:field('floor'):set(count, floor)
      -- Spanning tree: neighbouring rooms and the stairwell in the first room of each floor.
      local parent = x>0 and count-1 or (z>0 and count-2 or (floor>0 and count-4 or -1))
      rooms:field('parent'):set(count, parent)
      W.ball(cuts, count, centre, {x = 6.1, y = 3.1, z = 5.1}, 6)
      count = count+1
    end end
  end
  -- Three axis-aligned trunk legs. Their union connects side, roof and floor approaches.
  local entry = {x = low.x, y = low.y+2, z = low.z+5}
  local support=math.abs(direction.x)*constants.sub_length/2+(math.abs(direction.y)+math.abs(direction.z))*constants.sub_beam/2
  local hatch=W.add(dock,W.scale(direction,support+3))
  local bend1 = {x = hatch.x, y = hatch.y, z = entry.z}
  local bend2 = {x = hatch.x, y = entry.y, z = entry.z}
  local points = {hatch, bend1, bend2, entry}
  local trunk_length = 0
  local ncuts = count
  for i = 1, 3 do
    local a, b = points[i], points[i+1]
    local length = W.length(W.sub(b, a))
    if length>0.01 then
      local centre = W.scale(W.add(a, b), 0.5)
      local half = {x = math.abs(b.x-a.x)/2+1.8, y = math.abs(b.y-a.y)/2+1.8, z = math.abs(b.z-a.z)/2+1.8}
      W.ball(cuts, ncuts, centre, half, 6)
      ncuts = ncuts+1
      trunk_length = trunk_length+length
    end
  end
  local meta = step.writes.station_meta
  W.put(meta:field('dock'), 0, dock)
  W.put(meta:field('hatch'),0,hatch)
  W.put(meta:field('entry'), 0, entry)
  meta:field('pose'):set(0, direction.y>0.5 and 2 or (direction.y< -0.5 and 1 or 0))
  meta:field('built'):set(0, 1)
  meta:field('rooms'):set(0, count)
  meta:field('floors'):set(0, floors)
  meta:field('cuts'):set(0, ncuts)
  meta:field('trunk_length'):set(0, trunk_length)
end
