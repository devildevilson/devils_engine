return function(step)
  local p=modules.network.main(step.world_seed,step.key[1],step.key[2])
  for axis,name in ipairs({'x','y','z'}) do
    step.writes.outpost_context:field('chunk'):set(0,math.floor(p[name]/step.params.chunk_cells),axis-1)
  end
end
