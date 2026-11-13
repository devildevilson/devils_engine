local B = modules.biomes
return function(step)
  -- Any point of this cell moves by at most biome_warp along either axis.
  local radius = 2 + math.ceil(constants.biome_warp / B.span())
  B.write_sources(step, step.key[1], step.key[2], radius)
end
