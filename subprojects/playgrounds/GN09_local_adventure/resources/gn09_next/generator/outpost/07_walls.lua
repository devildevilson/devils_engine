local W = modules.world
return function(step)
  local rooms, walls = step.reads.station_rooms, step.writes.station_walls
  local count = 0
  local n = step.reads.station_meta:field('rooms'):get(0)
  for i = 0, n-1 do
    local centre = W.get(rooms:field('centre'), i)
    local half = W.get(rooms:field('half'), i)
    local x, z = i%2, (i//2)%2
    for _, axis in ipairs({0, 2}) do
      for _, sign in ipairs({-1, 1}) do
        local coordinate = axis == 0 and x or z
        local interior = (coordinate == 0 and sign == 1) or (coordinate == 1 and sign == -1)
        -- A shared partition belongs to the room on its negative side.
        if not interior or sign == 1 then
          local p = {x = centre.x, y = centre.y, z = centre.z}
          if axis == 0 then p.x = p.x+sign*half.x else p.z = p.z+sign*half.z end
          W.put(walls:field('centre'), count, p)
          local probe = {x = p.x, y = p.y-1.6, z = p.z}
          if axis == 0 then probe.x = probe.x+sign*0.75 else probe.z = probe.z+sign*0.75 end
          W.put(walls:field('probe_low'), count, probe)
          probe.y = p.y+0.5
          W.put(walls:field('probe_high'), count, probe)
          walls:field('room'):set(count, i)
          walls:field('axis'):set(count, axis)
          walls:field('sign'):set(count, sign)
          local entry = i == 0 and axis == 0 and sign == -1
          walls:field('opening'):set(count, (interior or entry) and 1 or 0)
          count = count+1
        end
      end
    end
  end
  -- Unused rows are harmless for the single batched field query and never become walls.
end
