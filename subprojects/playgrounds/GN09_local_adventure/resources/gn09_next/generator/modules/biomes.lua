-- Streaming warped Voronoi. Address cells are not visible biome boundaries.
local B = {}
function B.span() return constants.sector_size * constants.region_sectors end

function B.site(seed, x, z)
  local span = B.span()
  local jitter = constants.biome_jitter
  assert(jitter >= 0 and jitter <= 0.3, 'biome_jitter must be within [0,0.3]')
  local random = function(salt) return base.prng64_normalize(base.mix(seed,x,z,salt)) end
  local roll=base.umod(base.mix(seed,x,z,0x42494f4d),100)
  local kind=roll<40 and 0 or (roll<85 and 1 or (roll<97 and 2 or 3))
  return {x=(x+0.5+(random(701)*2-1)*jitter)*span,
          z=(z+0.5+(random(702)*2-1)*jitter)*span,
          key_x=x,key_z=z,kind=kind}
end

function B.warp(seed, x, z)
  local span = B.span()
  local phase = function(salt) return base.prng64_normalize(base.mix(seed,salt))*math.pi*2 end
  local u,v=x/(span*1.3),z/(span*1.3)
  local a,b=phase(703),phase(704)
  local reach=constants.biome_warp
  assert(reach >= 0, 'biome_warp must be nonnegative')
  return x+reach*(math.sin(u+a)*math.cos(v+b)+0.35*math.sin(2.1*v+a))/1.35,
         z+reach*(math.sin(v+b)*math.cos(u+a)+0.35*math.sin(1.7*u+b))/1.35
end

function B.read_sites(instances)
  local sites={}
  for _,instance in ipairs(instances) do
    local b=instance.biome
    sites[#sites+1]={x=b:field('centre'):get(0,0),z=b:field('centre'):get(0,2),
      key_x=b:field('region'):get(0,0),key_z=b:field('region'):get(0,1),kind=b:field('kind'):get(0)}
  end
  return sites
end

function B.nearest(sites,x,z)
  local first,second,d1,d2=nil,nil,math.huge,math.huge
  local function before(d,s,old_d,old)
    return d<old_d or (d==old_d and (not old or s.key_x<old.key_x or (s.key_x==old.key_x and s.key_z<old.key_z)))
  end
  for _,s in ipairs(sites) do
    local d=(x-s.x)^2+(z-s.z)^2
    if before(d,s,d1,first) then second,d2=first,d1;first,d1=s,d
    elseif before(d,s,d2,second) then second,d2=s,d end
  end
  assert(second,'Voronoi query needs at least two sites')
  local gap=math.sqrt(d2)-math.sqrt(d1)
  local blend=0.5*math.max(0,1-gap/constants.biome_transition)
  return first,second,blend,math.sqrt(d1)
end

function B.write_sources(step,cx,cz,radius)
  local keys=step.writes.biome_keys:field('key')
  assert((2*radius+1)^2<=keys:count(),'biome source capacity too small for the proven query halo')
  local n=0
  for dz=-radius,radius do for dx=-radius,radius do
    keys:set(n,cx+dx,0);keys:set(n,cz+dz,1);n=n+1
  end end
  step.writes.biome_source_count:field('value'):set(0,n)
end
function B.sample(seed,x,z)
  local qx,qz=B.warp(seed,x,z)
  local cx,cz=math.floor(qx/B.span()),math.floor(qz/B.span())
  local sites={}
  for dz=-2,2 do for dx=-2,2 do sites[#sites+1]=B.site(seed,cx+dx,cz+dz) end end
  return B.nearest(sites,qx,qz)
end
return B
