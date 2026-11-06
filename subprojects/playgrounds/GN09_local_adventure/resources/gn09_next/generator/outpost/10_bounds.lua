local W=modules.world
return function(step)
  local cut=step.reads.station_cut
  local meta=step.writes.station_meta
  local low,high={x=math.huge,y=math.huge,z=math.huge},{x=-math.huge,y=-math.huge,z=-math.huge}
  for i=0,meta:field('cuts'):get(0)-1 do
    local c,h=W.get(cut:field('centre'),i),W.get(cut:field('half'),i)
    for _,axis in ipairs({'x','y','z'}) do low[axis]=math.min(low[axis],c[axis]-h[axis]-1);high[axis]=math.max(high[axis],c[axis]+h[axis]+1) end
  end
  W.put(meta:field('low'),0,low);W.put(meta:field('high'),0,high)
end
