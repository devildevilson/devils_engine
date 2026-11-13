-- Outposts a chunk links: only main cells whose node lies within the asserted outpost influence bound
-- (outpost_search_reach+80 in x and z, outpost/10_bounds.lua) of the chunk. Each linked outpost pulls
-- its own terrain and features, so the wide terrain halo would build outposts far from the chunk.
return function(step)
  local side=step.params.chunk_cells
  local x0,z0=step.key[1]*side,step.key[3]*side
  -- The surface step takes refinement boxes up to max(2, 2*terrain_normal_step) outside the chunk.
  local span,reach=constants.main_spacing,constants.outpost_search_reach+80+math.max(2,constants.terrain_normal_step*2)
  local radius=math.max(1,math.ceil(reach/span+0.5+constants.main_jitter))
  local cx,cz=math.floor((x0+side/2)/span),math.floor((z0+side/2)/span)
  local key=step.writes.station_keys:field('key');local n=0
  for dz=-radius,radius do for dx=-radius,radius do
    local nx,nz=modules.network.main_xz(step.world_seed,cx+dx,cz+dz)
    if nx+reach>=x0 and nx-reach<=x0+side and nz+reach>=z0 and nz-reach<=z0+side then
      assert(n<key:count(),'station scope exceeds host capacity')
      key:set(n,cx+dx,0);key:set(n,cz+dz,1);n=n+1
    end
  end end
  step.writes.station_count:field('value'):set(0,n)
end
