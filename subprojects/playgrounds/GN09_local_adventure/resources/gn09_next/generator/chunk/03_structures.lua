-- Clip independent station meshes into the requested chunk. No host structure builder.
return function(step)
  local out = step.writes.structure_surface
  local count = step.writes.surface_counts:field('structure')
  count:set(0, 0)
  local function reaches(station)
    local low, high = station.station_meta:field('low'), station.station_meta:field('high')
    local side = step.params.chunk_cells
    for axis = 0, 2 do
      local origin = step.key[axis + 1] * side
      if low:get(0, axis) > origin + side or high:get(0, axis) < origin then return false end
    end
    return true
  end
  for _, station in ipairs(step.links.stations or {}) do
    if reaches(station) and station.station_meta:field('built'):get(0) ~= 0 then
      local mesh, boxes = station.station_mesh, station.station_cut
      originator.clip_triangle_mesh{
        inputs = {mesh:field('a'), mesh:field('b'), mesh:field('c'), mesh:field('normal'), mesh:field('shade'),
          boxes:field('centre'), boxes:field('centre'), boxes:field('centre'), boxes:field('centre'),
          boxes:field('half'), station.station_meta:field('built')},
        outputs = {out:field('position'), out:field('normal'), out:field('shade'), count},
        params =
        {x = step.key[1], y = step.key[2], z = step.key[3], side = step.params.chunk_cells, exclusions = 0, tag = 0}}
    end
  end
end
