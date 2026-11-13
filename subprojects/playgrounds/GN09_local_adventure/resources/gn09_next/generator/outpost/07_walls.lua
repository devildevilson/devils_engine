-- Wall pieces of the room boxes. Each face loses the parts it shares with touching rooms; what is left
-- is outer shell. A shared part becomes one partition, owned by the lower room; it gets a passage
-- (door in a wall, hatch in a floor) only when the two rooms are linked by the spanning tree.
local W,F,O=modules.world,modules.frame,modules.outpost
local names={'x','y','z'}
local plane_axes={x={'y','z'},y={'x','z'},z={'x','y'}}
local eps=0.01

return function(step)
  local rooms,walls=step.reads.station_rooms,step.writes.station_walls
  local meta=step.writes.station_meta
  local frame=F.from_meta(meta)
  local n=meta:field('rooms'):get(0)
  local box={}
  for i=0,n-1 do
    box[i]={c=W.get(rooms:field('local_centre'),i),h=W.get(rooms:field('half'),i),parent=rooms:field('parent'):get(i),
      open_axis=rooms:field('open_axis'):get(i),open_sign=rooms:field('open_sign'):get(i)}
  end
  local function linked(i,j) return box[i].parent==j or box[j].parent==i end
  local count=0
  local function record(i,other,axis,sign,centre,half,opening,hole_c,hole_h)
    assert(count<walls:count(),'outpost wall capacity exceeded')
    local normal={x=0,y=0,z=0};normal[axis]=sign
    local hn=names[1]==axis and 1 or (names[2]==axis and 2 or 3)
    W.put(walls:field('local_centre'),count,centre)
    W.put(walls:field('centre'),count,frame.point(centre))
    W.put(walls:field('normal'),count,frame.dir(normal))
    W.put(walls:field('half'),count,half)
    W.put(walls:field('hole_centre'),count,hole_c or centre)
    W.put(walls:field('hole_half'),count,hole_h or {x=0,y=0,z=0})
    walls:field('room'):set(count,i)
    walls:field('other'):set(count,other)
    walls:field('axis'):set(count,hn-1)
    walls:field('sign'):set(count,sign)
    walls:field('opening'):set(count,opening)
    walls:field('outside'):set(count,0)
    walls:field('exposure'):set(count,0)
    walls:field('first_probe'):set(count,0);walls:field('probes_a'):set(count,0);walls:field('probes_b'):set(count,0)
    count=count+1
  end
  local passages=0
  for i=0,n-1 do
    local c,h=box[i].c,box[i].h
    for k,axis in ipairs(names) do for _,sign in ipairs({-1,1}) do
      local open=box[i].open_axis==k and box[i].open_sign==sign
      local pa,pb=plane_axes[axis][1],plane_axes[axis][2]
      local plane=c[axis]+sign*h[axis]
      local pieces={{l={c[pa]-h[pa],c[pb]-h[pb]},h={c[pa]+h[pa],c[pb]+h[pb]}}}
      for j=0,n-1 do if j~=i then
        local o=box[j]
        if math.abs(o.c[axis]-sign*o.h[axis]-plane)<eps then
          local low={math.max(c[pa]-h[pa],o.c[pa]-o.h[pa]),math.max(c[pb]-h[pb],o.c[pb]-o.h[pb])}
          local high={math.min(c[pa]+h[pa],o.c[pa]+o.h[pa]),math.min(c[pb]+h[pb],o.c[pb]+o.h[pb])}
          if high[1]-low[1]>eps and high[2]-low[2]>eps then
            pieces=O.subtract(pieces,low,high)
            if i<j and not open then
              local centre={};centre[axis]=plane
              local half={x=0,y=0,z=0}
              centre[pa],centre[pb]=(low[1]+high[1])/2,(low[2]+high[2])/2
              half[pa],half[pb]=(high[1]-low[1])/2,(high[2]-low[2])/2
              local opening,hole_c,hole_h=0,nil,nil
              if linked(i,j) then
                opening=1;passages=passages+1
                hole_c={x=centre.x,y=centre.y,z=centre.z};hole_h={x=0,y=0,z=0}
                if axis=='y' then
                  -- Floor hatch in the middle of the shared rectangle.
                  for _,name in ipairs({pa,pb}) do hole_h[name]=math.min(1.2,half[name]-0.25) end
                else
                  -- Door on the floor of the shared rectangle.
                  local across=pa=='y' and pb or pa
                  hole_h[across]=math.min(1.3,half[across]-0.25)
                  local bottom=centre.y-half.y+0.25
                  local top=math.min(bottom+2.5,centre.y+half.y-0.25)
                  hole_c.y,hole_h.y=(bottom+top)/2,(top-bottom)/2
                end
                for _,name in ipairs({pa,pb}) do
                  if hole_h[name]<=0.2 then error(string.format('outpost %d,%d passage between rooms %d and %d is too narrow',step.key[1],step.key[2],i,j)) end
                end
              end
              record(i,j,axis,sign,centre,half,opening,hole_c,hole_h)
            end
          end
        end
      end end
      if not open then
        for _,p in ipairs(pieces) do
          if p.h[1]-p.l[1]>0.05 and p.h[2]-p.l[2]>0.05 then
            local centre={};centre[axis]=plane
            local half={x=0,y=0,z=0}
            centre[pa],centre[pb]=(p.l[1]+p.h[1])/2,(p.l[2]+p.h[2])/2
            half[pa],half[pb]=(p.h[1]-p.l[1])/2,(p.h[2]-p.l[2])/2
            record(i,-1,axis,sign,centre,half,0)
          end
        end
      end
    end end
  end
  -- Every tree edge must have become a passage between touching rooms.
  if passages~=n-1 then
    error(string.format('outpost %d,%d has %d passages for %d rooms: a linked pair does not share a face',step.key[1],step.key[2],passages,n))
  end
  meta:field('walls'):set(0,count)
end
