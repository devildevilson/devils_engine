local B=modules.biomes
return function(step)
  local x,z=B.warp(step.world_seed,step.key[1],step.key[2])
  -- jitter <= .3: one site's distance <= sqrt(2)*.8*span; two sites <=
  -- sqrt(1.8^2+.8^2)*span < 2.2*span, the nearest possible omitted site.
  -- Hence radius 2 contains both nearest sites, even on address-cell boundaries.
  B.write_sources(step,math.floor(x/B.span()),math.floor(z/B.span()),2)
end
