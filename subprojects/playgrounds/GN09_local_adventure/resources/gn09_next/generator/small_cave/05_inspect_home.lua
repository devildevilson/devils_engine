return function(step)
  local p=modules.caves.locate(step.world_seed,step.key[1],step.key[2],step.key[3])
  for axis,name in ipairs({'x','y','z'}) do step.writes.cave_inspect_request:field('chunk'):set(0,math.floor(p[name]/step.params.chunk_cells),axis-1) end
end
