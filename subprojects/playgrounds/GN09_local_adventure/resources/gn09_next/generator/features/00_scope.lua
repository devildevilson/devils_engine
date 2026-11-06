local W=modules.world
return function(step)
  local meta=step.key[3]==0 and step.reads.terrain_meta or step.reads.cavern_meta
  local low,high=W.get(meta:field('feature_low'),0),W.get(meta:field('feature_high'),0)
  local count=meta:field(step.key[3]==0 and 'ribs' or 'buttresses'):get(0)
  if count==0 then
    local p=step.key[3]==0 and modules.network.main(step.world_seed,step.key[1],step.key[2]) or modules.biomes.site(step.world_seed,step.key[1],step.key[2])
    low,high=p,p
  end
  local function enumerate(buffer,span,reach,jitter)
    -- An owner at (i+.5)*span can move by jitter*span. Include every owner
    -- whose declared shape reach intersects the actual anchor search box.
    local x0=math.ceil((low.x-reach)/span-0.5-jitter)
    local x1=math.floor((high.x+reach)/span-0.5+jitter)
    local z0=math.ceil((low.z-reach)/span-0.5-jitter)
    local z1=math.floor((high.z+reach)/span-0.5+jitter)
    local f=buffer:field('key');local n=0
    assert((x1-x0+1)*(z1-z0+1)<=f:count(),'feature query halo exceeds host capacity')
    for z=z0,z1 do for x=x0,x1 do f:set(n,x,0);f:set(n,z,1);n=n+1 end end
    return n
  end
  local reach=constants.main_neighbour_radius+constants.route_bend+constants.route_branch_length+constants.terrain_max_reach
  step.writes.terrain_count:field('terrain'):set(0,enumerate(step.writes.terrain_keys,constants.main_spacing,reach,constants.main_jitter))
  step.writes.terrain_count:field('biomes'):set(0,enumerate(step.writes.biome_terrain_keys,modules.biomes.span(),constants.biome_max_reach,constants.biome_jitter))
end
