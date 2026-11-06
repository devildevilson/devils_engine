local W, M = modules.world, modules.mesh
return function(step)
  local rooms, walls, cuts = step.reads.station_rooms, step.writes.station_walls, step.reads.station_cut
  local meta = step.writes.station_meta
  if meta:field('built'):get(0) == 0 then return end
  local mesh = M.writer(step.writes.station_mesh)
  local n = meta:field('rooms'):get(0)
  local wall_count = 0
  local function slab(c, h, shade)
    mesh.box({c.x-h.x, c.y-h.y, c.z-h.z}, {c.x+h.x, c.y+h.y, c.z+h.z}, shade)
  end
  -- Split a slab around overlapping cut boxes. This opens trunk bends without sealed cross-walls.
  local function trunk_slab(c, h, own)
    local pieces = {{{c.x-h.x, c.y-h.y, c.z-h.z}, {c.x+h.x, c.y+h.y, c.z+h.z}}}
    for other = 0, meta:field('cuts'):get(0)-1 do if other ~= own then
      local oc, oh = W.get(cuts:field('centre'), other), W.get(cuts:field('half'), other)
      local low, high = {oc.x-oh.x, oc.y-oh.y, oc.z-oh.z}, {oc.x+oh.x, oc.y+oh.y, oc.z+oh.z}
      local next_pieces = {}
      for _, piece in ipairs(pieces) do
        local a, b = piece[1], piece[2]
        local overlap = true
        for axis = 1, 3 do overlap = overlap and a[axis]<high[axis] and b[axis]>low[axis] end
        if not overlap then next_pieces[#next_pieces+1] = piece
        else
          local ca, cb = {a[1], a[2], a[3]}, {b[1], b[2], b[3]}
          for axis = 1, 3 do
            if ca[axis]<low[axis] then
              local endp = {cb[1], cb[2], cb[3]}
              endp[axis] = low[axis]
              next_pieces[#next_pieces+1] = {{ca[1], ca[2], ca[3]}, endp}
              ca[axis] = low[axis]
            end
            if cb[axis]>high[axis] then
              local start = {ca[1], ca[2], ca[3]}
              start[axis] = high[axis]
              next_pieces[#next_pieces+1] = {start, {cb[1], cb[2], cb[3]}}
              cb[axis] = high[axis]
            end
          end
        end
      end
      pieces = next_pieces
    end end
    for _, piece in ipairs(pieces) do mesh.box(piece[1], piece[2], 116) end
  end
  local emergency = 0
  for i = 0, n-1 do
    local x, z = i%2, (i//2)%2
    local c = W.get(rooms:field('centre'), i)
    for _, axis in ipairs({0, 2}) do for _, sign in ipairs({-1, 1}) do
      local coordinate = axis == 0 and x or z
      local interior = (coordinate == 0 and sign == 1) or (coordinate == 1 and sign == -1)
      if not interior or sign == 1 then
        local at = wall_count
        wall_count = wall_count+1
        local p = W.get(walls:field('centre'), at)
        local outside = not interior and walls:field('value'):get(at)<0
        local opening = walls:field('opening'):get(at)
        if outside and opening == 0 then
          if emergency<2 and walls:field('value_low'):get(at)<0 and walls:field('value_high'):get(at)<0 then opening = 2
          emergency = emergency+1
          elseif at%3 == 0 then opening = 3 end
        end
        -- 0 solid, 1 doorway, 2 emergency hatch, 3 window. Exposed walls are destructible metadata.
        walls:field('outside'):set(at, outside and 1 or 0)
        walls:field('opening'):set(at, opening)
        local span = axis == 0 and 5 or 6
        local function patch(u, y, hu, hy, shade)
          if hu <= 0 or hy <= 0 then return end
          local centre = {x = p.x, y = p.y+y, z = p.z}
          local half = {x = 0.25, y = hy, z = hu}
          if axis == 0 then centre.z = centre.z+u
          else centre.x = centre.x+u
          half.x = hu
          half.z = 0.25 end
          slab(centre, half, shade)
        end
        local shade = outside and 116 or 112
        if opening == 0 then patch(0, 0, span, 3, shade)
        else
          local hw = opening == 3 and 1.8 or 1.3
          local bottom = opening == 3 and -0.5 or -2.75
          local top = opening == 3 and 1.5 or 0.5
          patch(-(span+hw)/2, 0, (span-hw)/2, 3, shade)
          patch((span+hw)/2, 0, (span-hw)/2, 3, shade)
          patch(0, (-3+bottom)/2, hw, (bottom+3)/2, shade)
          patch(0, (3+top)/2, hw, (3-top)/2, shade)
          if opening == 3 then patch(0, (bottom+top)/2, hw, (top-bottom)/2, 113) end
        end
      end
    end end
    -- Floor slabs. The first room is the stairwell; a rectangular aperture joins adjacent floors.
    if i%4 == 0 and i >= 4 then
      slab({x = c.x-3.75, y = c.y-3, z = c.z}, {x = 2.25, y = 0.25, z = 5}, 112)
      slab({x = c.x+3.75, y = c.y-3, z = c.z}, {x = 2.25, y = 0.25, z = 5}, 112)
      slab({x = c.x, y = c.y-3, z = c.z-3.75}, {x = 1.5, y = 0.25, z = 1.25}, 112)
      slab({x = c.x, y = c.y-3, z = c.z+3.75}, {x = 1.5, y = 0.25, z = 1.25}, 112)
    else slab({x = c.x, y = c.y-3, z = c.z}, {x = 6, y = 0.25, z = 5}, 112) end
    if i >= n-4 then slab({x = c.x, y = c.y+3, z = c.z}, {x = 6, y = 0.25, z = 5}, 112) end
    if i%4 == 0 and i<n-4 then
      for j = 0, 11 do
        slab({x = c.x, y = c.y-2.75+j*0.5, z = c.z-2.75+j*0.5}, {x = 1.2, y = 0.25, z = 0.3}, 112)
      end
    end
  end
  -- A trunk shell surrounds the excavation. End caps remain open at bends and the wet dock.
  for i = n, meta:field('cuts'):get(0)-1 do
    local c, h = W.get(cuts:field('centre'), i), W.get(cuts:field('half'), i)
    local axis = h.x>h.y and (h.x>h.z and 0 or 2) or (h.y>h.z and 1 or 2)
    for perpendicular = 0, 2 do if perpendicular ~= axis then
      local names = {'x','y','z'}
      local name = names[perpendicular+1]
      for _, sign in ipairs({-1, 1}) do
        local centre = {x = c.x, y = c.y, z = c.z}
        local half = {x = h.x, y = h.y, z = h.z}
        centre[name] = centre[name]+sign*h[name]
        half[name] = 0.25
        trunk_slab(centre, half, i)
      end
    end end
  end
  meta:field('walls'):set(0, wall_count)
  meta:field('triangles'):set(0, mesh.count())
end
