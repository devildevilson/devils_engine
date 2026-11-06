return function(step)
  local p=modules.network.main(step.world_seed,step.key[1],step.key[2])
  step.writes.network_request:field('position'):set(0,math.floor(p.x),0)
  step.writes.network_request:field('position'):set(0,math.floor(p.z),1)
end
