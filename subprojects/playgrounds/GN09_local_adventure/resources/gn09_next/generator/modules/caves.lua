-- Stable cave-system addresses and the geometric envelope used by streaming queries.
local C={}
function C.random(seed,x,z,layer,salt)
  return base.prng64_normalize(base.mix(seed,x,z,layer,salt))
end
function C.locate(seed,x,z,layer)
  local r=function(salt) return C.random(seed,x,z,layer,salt) end
  local span=constants.cave_system_spacing
  local px=(x+0.5+(r(1100)-0.5)*0.45)*span
  local pz=(z+0.5+(r(1101)-0.5)*0.45)*span
  return {x=px,y=modules.world.height(px,pz)+layer*constants.cave_layer_spacing+(r(1102)-0.5)*48,z=pz}
end
function C.bounds(p)
  local W=modules.world
  return W.add(p,{x=-constants.cave_system_reach,y=-constants.cave_system_height,z=-constants.cave_system_reach}),
         W.add(p,{x=constants.cave_system_reach,y=constants.cave_system_height,z=constants.cave_system_reach})
end
function C.frame(delta)
  return originator.algorithms.frame(delta)
end
return C
