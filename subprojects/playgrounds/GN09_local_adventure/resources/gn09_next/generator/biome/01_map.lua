local W, B = modules.world, modules.biomes
return function(step)
  local x, z = step.key[1], step.key[2]
  local site = B.site(step.world_seed, x, z)
  local b = step.writes.biome
  W.put(b:field('centre'), 0, {x = site.x, y = W.height(site.x, site.z), z = site.z})
  -- Half extent describes the address cell only, never the visible Voronoi polygon.
  W.put(b:field('half'), 0, {x = B.span() / 2, y = 180, z = B.span() / 2})
  b:field('kind'):set(0, site.kind)
  b:field('region'):set(0, x, 0)
  b:field('region'):set(0, z, 1)
end
