local W=modules.world
return function(step)
  local p=W.get(step.reads.network_node:field('centre'),0)
  local rays=step.writes.outpost_rays
  local directions={{x=0,y=1,z=0},{x=0,y=-1,z=0}}
  for i=0,7 do local angle=i*math.pi/4;directions[#directions+1]={x=math.cos(angle),y=0,z=math.sin(angle)} end
  for i,d in ipairs(directions) do W.put(rays:field('origin'),i-1,p);W.put(rays:field('direction'),i-1,d) end
  step.writes.outpost_meta:field('rays'):set(0,#directions)
end
