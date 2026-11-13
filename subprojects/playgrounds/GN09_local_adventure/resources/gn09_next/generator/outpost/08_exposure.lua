-- Exposure of the outer shell, its openings and the beacon point (stage 5 of NEXT_OUTPOST_PLAN.md).
-- Each outer wall piece is probed by a grid 0.4 m outside it, about 2 m apart and 0.5 m past its
-- edges. The share of water decides whether the piece faces open water; openings are placed only
-- where the probes around them are water. The details step reuses the same samples.
local W,F,O=modules.world,modules.frame,modules.outpost
local names={'x','y','z'}
local window_step,margin=3.5,0.5

return function(step)
  local walls,rooms=step.writes.station_walls,step.reads.station_rooms
  local meta=step.writes.station_meta
  local frame=F.from_meta(meta)
  local probes,openings=step.writes.outpost_probes,step.writes.station_openings
  local function random(salt) return base.prng64_normalize(base.mix(step.world_seed,step.key[1],step.key[2],salt)) end
  local function room(i)
    return {c=W.get(rooms:field('local_centre'),i),h=W.get(rooms:field('half'),i),label=rooms:field('label'):get(i)}
  end

  -- Probe grids.
  local pieces,np={},0
  for w=0,meta:field('walls'):get(0)-1 do
    if walls:field('other'):get(w)<0 then
      local axis=names[walls:field('axis'):get(w)+1]
      local pa,pb=O.plane_axes[axis][1],O.plane_axes[axis][2]
      local c,h=W.get(walls:field('local_centre'),w),W.get(walls:field('half'),w)
      local sign=walls:field('sign'):get(w)
      local span={}
      span[pa],span[pb]=h[pa]+margin,h[pb]+margin
      local na=math.min(17,math.max(2,math.ceil(span[pa])+1))
      local nb=math.min(17,math.max(2,math.ceil(span[pb])+1))
      local piece={w=w,axis=axis,pa=pa,pb=pb,c=c,h=h,sign=sign,span=span,na=na,nb=nb,first=np,owner=room(walls:field('room'):get(w))}
      for b=0,nb-1 do for a=0,na-1 do
        assert(np<probes:count(),'outpost probe capacity exceeded by the shell exposure grid')
        local p={x=c.x,y=c.y,z=c.z}
        p[pa]=p[pa]+(2*a/(na-1)-1)*span[pa]
        p[pb]=p[pb]+(2*b/(nb-1)-1)*span[pb]
        p[axis]=p[axis]+sign*0.4
        W.put(probes:field('position'),np,frame.point(p));np=np+1
      end end
      walls:field('first_probe'):set(w,np-na*nb)
      walls:field('probes_a'):set(w,na);walls:field('probes_b'):set(w,nb)
      pieces[#pieces+1]=piece
    end
  end
  if np>0 then
    originator.field_sample{inputs={probes:field('position')},outputs={probes:field('value')},params={field='natural',count=np,half=24}}
  end
  -- Grid point coordinates in the face plane and their sample.
  local function each(piece,fn)
    for b=0,piece.nb-1 do for a=0,piece.na-1 do
      local u=piece.c[piece.pa]+(2*a/(piece.na-1)-1)*piece.span[piece.pa]
      local v=piece.c[piece.pb]+(2*b/(piece.nb-1)-1)*piece.span[piece.pb]
      fn(u,v,probes:field('value'):get(piece.first+b*piece.na+a))
    end end
  end
  -- Every probe within a face rectangle widened by 0.5 m is water; with none inside, the nearest one.
  local function clear(piece,u0,u1,v0,v1)
    local seen,dry,nearest,best=0,false,nil,math.huge
    local cu,cv=(u0+u1)/2,(v0+v1)/2
    each(piece,function(u,v,value)
      if u>=u0-0.5 and u<=u1+0.5 and v>=v0-0.5 and v<=v1+0.5 then
        seen=seen+1;dry=dry or value>=0
      end
      local d=(u-cu)^2+(v-cv)^2
      if d<best then nearest,best=value,d end
    end)
    if seen==0 then return nearest~=nil and nearest<0 end
    return not dry
  end
  for _,piece in ipairs(pieces) do
    local water,total=0,0
    each(piece,function(u,v,value)
      if math.abs(u-piece.c[piece.pa])<=piece.h[piece.pa]+0.01 and math.abs(v-piece.c[piece.pb])<=piece.h[piece.pb]+0.01 then
        total=total+1
        if value<0 then water=water+1 end
      end
    end)
    piece.exposure=total>0 and water/total or 0
    walls:field('exposure'):set(piece.w,piece.exposure)
    walls:field('outside'):set(piece.w,piece.exposure>=0.5 and 1 or 0)
  end

  -- Openings on vertical outer pieces: slots every 3.5 m across the piece, on the owner's lowest deck.
  local count,windows,hatches=0,0,0
  local function opening(piece,u,y0,y1,half_u,kind)
    assert(count<openings:count(),'outpost opening capacity exceeded')
    local centre,half={x=piece.c.x,y=(y0+y1)/2,z=piece.c.z},{x=0,y=(y1-y0)/2,z=0}
    local across=piece.axis=='x' and 'z' or 'x'
    centre[across],half[across]=u,half_u
    W.put(openings:field('local_centre'),count,centre)
    W.put(openings:field('centre'),count,frame.point(centre))
    W.put(openings:field('half'),count,half)
    openings:field('wall'):set(count,piece.w)
    openings:field('kind'):set(count,kind)
    count=count+1
  end
  local function slots(piece)
    local across=piece.axis=='x' and 'z' or 'x'
    local width=2*piece.h[across]
    local out={}
    if width<2.6 then return out,across end
    local n=math.floor((width-2.6)/window_step)+1
    for k=0,n-1 do out[#out+1]=piece.c[across]+(k-(n-1)/2)*window_step end
    return out,across
  end
  local function dwelling(label) return label~=O.lock and label~=O.passage end
  local function floor_of(piece) return piece.owner.c.y-piece.owner.h.y end
  -- Emergency hatches: a door to open water on the floor of a core or technical room; a few per
  -- outpost, one per room. The docking bay and the guard post keep their walls for windows.
  local candidates={}
  for i,piece in ipairs(pieces) do
    local label=piece.owner.label
    if piece.axis~='y' and piece.exposure>=0.5 and (label==O.core or label==O.technical) then
      local floor=floor_of(piece)
      if piece.c.y-piece.h.y<=floor+0.01 and piece.c.y+piece.h.y>=floor+2.75 then
        local list=slots(piece)
        for k,u in ipairs(list) do
          if clear(piece,u-0.9,u+0.9,floor+0.25,floor+2.5) then
            candidates[#candidates+1]={piece=piece,u=u,key=random(5000+i*32+k),room=walls:field('room'):get(piece.w)}
          end
        end
      end
    end
  end
  table.sort(candidates,function(a,b) return a.key<b.key end)
  local limit=1+meta:field('rooms'):get(0)//25
  local used_room,used={},{}
  for _,candidate in ipairs(candidates) do
    if hatches<limit and not used_room[candidate.room] then
      used_room[candidate.room]=true
      local floor=floor_of(candidate.piece)
      opening(candidate.piece,candidate.u,floor+0.25,floor+2.5,0.9,2)
      used[candidate.piece.w..':'..candidate.u]=true
      hatches=hatches+1
    end
  end
  -- Windows: the docking bay and the guard post look at the dock; other rooms have fewer windows.
  local dock=frame.to_local(W.get(meta:field('dock'),0))
  for i,piece in ipairs(pieces) do
    local label=piece.owner.label
    if piece.axis~='y' and piece.exposure>=0.25 and dwelling(label) then
      local floor=floor_of(piece)
      if piece.c.y-piece.h.y<=floor+1.1 and piece.c.y+piece.h.y>=floor+2.3 then
        local normal={x=0,y=0,z=0};normal[piece.axis]=piece.sign
        local towards=W.sub(dock,piece.c)
        local facing=normal.x*towards.x+normal.z*towards.z>0
        local chance=(label==O.dock or label==O.guard) and (facing and 1 or 0.6) or (label==O.technical and 0.3 or 0.45)
        for k,u in ipairs(slots(piece)) do
          if not used[piece.w..':'..u] and random(6000+i*32+k)<chance and clear(piece,u-0.9,u+0.9,floor+1.1,floor+2.3) then
            opening(piece,u,floor+1.1,floor+2.3,0.9,3)
            windows=windows+1
          end
        end
      end
    end
  end

  -- Beacon: a data point on the most visible exposed part near the dock, else at the lock mouth.
  local best,best_score=nil,-math.huge
  for _,piece in ipairs(pieces) do
    if piece.exposure>=0.75 then
      local area=4*piece.h[piece.pa]*piece.h[piece.pb]
      local score=math.sqrt(area)+((piece.axis=='y' and piece.sign>0) and 4 or 0)-W.length(W.sub(piece.c,dock))*0.15
      if score>best_score then best,best_score=piece,score end
    end
  end
  if best then
    -- The water probe of that piece nearest to its centre: a sampled point in open water.
    local index,distance=nil,math.huge
    for b=0,best.nb-1 do for a=0,best.na-1 do
      local j=best.first+b*best.na+a
      local d=(2*a/(best.na-1)-1)^2*best.span[best.pa]^2+(2*b/(best.nb-1)-1)^2*best.span[best.pb]^2
      if probes:field('value'):get(j)<0 and d<distance then index,distance=j,d end
    end end
    W.put(meta:field('beacon'),0,W.get(probes:field('position'),index))
    meta:field('beacon_kind'):set(0,(best.axis=='y' and best.sign>0) and 0 or 1)
  else
    W.put(meta:field('beacon'),0,W.get(meta:field('hatch'),0))
    meta:field('beacon_kind'):set(0,2)
  end
  meta:field('openings'):set(0,count)
  meta:field('windows'):set(0,windows)
  meta:field('hatches'):set(0,hatches)
end
