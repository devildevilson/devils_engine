-- Horizontal parts of the outer shell carry the same exposure metadata as walls.
local W=modules.world
return function(step)
  local rooms=step.reads.station_rooms
  local walls,meta=step.writes.station_walls,step.writes.station_meta
  local first=meta:field('walls'):get(0)
  local count=first
  for i=0,meta:field('rooms'):get(0)-1 do
    local floor=rooms:field('floor'):get(i)
    for _,sign in ipairs({-1,1}) do
      if (sign<0 and floor==0) or (sign>0 and floor==meta:field('floors'):get(0)-1) then
        local p=W.get(rooms:field('centre'),i)
        p.y=p.y+sign*3
        W.put(walls:field('centre'),count,p)
        p.y=p.y+sign*0.75
        W.put(walls:field('probe_low'),count,p)
        W.put(walls:field('probe_high'),count,p)
        walls:field('axis'):set(count,1);walls:field('sign'):set(count,sign)
        walls:field('room'):set(count,i);walls:field('opening'):set(count,0)
        count=count+1
      end
    end
  end
  originator.field_sample{inputs={walls:field('centre')},outputs={walls:field('value')},params={field='natural',count=count}}
  for i=first,count-1 do walls:field('outside'):set(i,walls:field('value'):get(i)<0 and 1 or 0) end
  meta:field('walls'):set(0,count)
end
