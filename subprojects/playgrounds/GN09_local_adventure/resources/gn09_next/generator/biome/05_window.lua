local B, W = modules.biomes, modules.world
return function(step)
  local sites = B.read_sites(step.links.sites)
  local out = step.writes.biome_pixels
  local side = math.tointeger(step.params.biome_map_side)
  local pitch = B.span() / (side - 1)
  for iz = 0, side - 1 do
    for ix = 0, side - 1 do
      local x, z = step.key[1] * B.span() + ix * pitch, step.key[2] * B.span() + iz * pitch
      local qx, qz = B.warp(step.world_seed, x, z)
      local site, _, blend = B.nearest(sites, qx, qz)
      local at = iz * side + ix
      W.put(out:field('position'), at, {x = x, y = 0, z = z})
      out:field('kind'):set(at, site.kind)
      out:field('region'):set(at, site.key_x, 0)
      out:field('region'):set(at, site.key_z, 1)
      out:field('blend'):set(at, blend)
    end
  end
end
