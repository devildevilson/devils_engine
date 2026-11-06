local B,W=modules.biomes,modules.world
return function(step)
  local x,z=step.key[1],step.key[2]
  local qx,qz=B.warp(step.world_seed,x,z)
  local a,b,blend,distance=B.nearest(B.read_sites(step.links.sites),qx,qz)
  local out=step.writes.biome_sample
  W.put(out:field('centre'),0,{x=x,y=W.height(x,z),z=z})
  out:field('kind'):set(0,a.kind);out:field('other_kind'):set(0,b.kind)
  out:field('region'):set(0,a.key_x,0);out:field('region'):set(0,a.key_z,1)
  out:field('other'):set(0,b.key_x,0);out:field('other'):set(0,b.key_z,1)
  out:field('blend'):set(0,blend);out:field('distance'):set(0,distance)
end
