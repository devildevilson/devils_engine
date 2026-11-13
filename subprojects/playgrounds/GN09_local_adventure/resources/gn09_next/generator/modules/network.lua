-- Main outpost network, independent of the camera window. Rank means degree.
local M={}
local function random(seed,x,z,salt) return base.prng64_normalize(base.mix(seed,x,z,salt)) end
function M.ocean_weight(seed,x,z)
  local a,b,w=modules.biomes.sample(seed,x,z)
  return (a.kind==3 and 1-w or 0)+(b.kind==3 and w or 0)
end
-- Horizontal position of a main node: no biome sampling, for scopes that only need x and z.
function M.main_xz(seed,x,z)
  local span=constants.main_spacing
  local jitter=constants.main_jitter
  assert(jitter>=0 and jitter<=0.3,'main_jitter outside proven local bound')
  return (x+0.5+(random(seed,x,z,801)*2-1)*jitter)*span,(z+0.5+(random(seed,x,z,802)*2-1)*jitter)*span
end
function M.main(seed,x,z)
  local px,pz=M.main_xz(seed,x,z)
  local sea=M.ocean_weight(seed,px,pz)
  local offset=(random(seed,x,z,803)-0.5)*210
  offset=offset*(1-sea)+(-8+(random(seed,x,z,803)-0.5)*18)*sea
  return {x=px,y=modules.world.height(px,pz)+offset,z=pz,
    cx=x,cz=z,key=base.mix(seed,x,z,0x4d41494e)}
end
local function distance2(a,b) return (a.x-b.x)^2+(a.z-b.z)^2 end
function M.neighbours(seed,x,z)
  local a=M.main(seed,x,z)
  local radius=constants.main_neighbour_radius
  local span,jitter=constants.main_spacing,constants.main_jitter
  -- Mandatory comb backbone: every column is connected vertically, rows at a
  -- bounded interval connect all columns. Every point can reach such a row.
  -- Expanded Gabriel neighbours (at most one intervening centre) add cross-links
  -- without weakening connectivity.
  assert(radius>=span*math.sqrt((1+2*jitter)^2+(2*jitter)^2),'radius cannot contain mandatory neighbours')
  local halo=math.ceil(radius/span+2*jitter)
  local candidates={}
  for dz=-halo,halo do for dx=-halo,halo do
    if dx~=0 or dz~=0 then
      local b=M.main(seed,x+dx,z+dz)
      if distance2(a,b)<=radius^2 then candidates[#candidates+1]=b end
    end
  end end
  local stride=math.tointeger(constants.main_backbone_stride)
  assert(stride>=1,'main backbone stride must be positive')
  local phase=base.umod(base.mix(seed,804),stride)
  local neighbours={}
  for _,b in ipairs(candidates) do
    local mandatory=(b.cx==x and math.abs(b.cz-z)==1) or
      (b.cz==z and math.abs(b.cx-x)==1 and z%stride==phase)
    local keep=mandatory
    if not keep then
      keep=true
      local blockers=0
      local mx,mz=(a.x+b.x)/2,(a.z+b.z)/2
      local reach2=distance2(a,b)/4
      for _,c in ipairs(candidates) do
        if c.key~=b.key and (c.x-mx)^2+(c.z-mz)^2<reach2-1e-8 then blockers=blockers+1 end
        if blockers>1 then keep=false;break end
      end
    end
    if keep then neighbours[#neighbours+1]={point=b,mandatory=mandatory} end
  end
  return a,neighbours
end
return M
