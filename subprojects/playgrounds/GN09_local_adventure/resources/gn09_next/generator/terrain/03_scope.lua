return function(step)
  local side=step.params.chunk_cells
  local x,z=(step.key[1]+0.5)*side,(step.key[3]+0.5)*side
  local function enumerate(buffer,span,reach,jitter)
    local cx,cz=math.floor(x/span),math.floor(z/span)
    local radius=math.max(1,math.ceil(reach/span-0.5+jitter))
    local key=buffer:field('key');local n=0
    assert((radius*2+1)^2<=key:count(),'terrain query halo exceeds host capacity')
    for dz=-radius,radius do for dx=-radius,radius do
      key:set(n,cx+dx,0);key:set(n,cz+dz,1);n=n+1
    end end
    return n
  end
  local terrain=enumerate(step.writes.terrain_keys,constants.main_spacing,math.max(constants.main_neighbour_radius+constants.route_bend+constants.route_branch_length+constants.terrain_max_reach,constants.outpost_search_reach+80),constants.main_jitter)
  local biomes=enumerate(step.writes.biome_terrain_keys,modules.biomes.span(),constants.biome_max_reach,constants.biome_jitter)
  step.writes.terrain_count:field('terrain'):set(0,terrain)
  step.writes.terrain_count:field('biomes'):set(0,biomes)
end
