local W,B=modules.world,modules.biomes
local profiles={
  {width=43,roof=33,floor=24,relief=7},
  {width=15,roof=28,floor=15,relief=6},
  {width=78,roof=17,floor=14,relief=7},
  {width=55,roof=38,floor=22,relief=5}}
local function profile(seed,p)
  local a,b,blend=B.sample(seed,p.x,p.z)
  local function values(site)
    local v={}
    for k,n in pairs(profiles[site.kind+1]) do v[k]=n end
    if site.kind==1 and base.umod(base.mix(seed,site.key_x,site.key_z,950),2)==0 then
      v.width=35;v.roof=12;v.floor=11
    end
    return v
  end
  local va,vb=values(a),values(b)
  local out={kind=a.kind}
  for key,value in pairs(va) do out[key]=value*(1-blend)+vb[key]*blend end
  return out
end
local function curve(a,b,c,d,t)
  -- Catmull-Rom through logical waypoints; endpoints are repeated for end tangents.
  local out={}
  for _,axis in ipairs({'x','y','z'}) do
    out[axis]=0.5*((2*b[axis])+(-a[axis]+c[axis])*t+(2*a[axis]-5*b[axis]+4*c[axis]-d[axis])*t*t+(-a[axis]+3*b[axis]-3*c[axis]+d[axis])*t*t*t)
  end
  return out
end
return function(step)
  local seed=step.world_seed
  local owner=W.get(step.reads.network_node:field("centre"),0)
  local paths=step.reads.network_paths
  local groups,order={},{}
  for i=0,paths:count()-1 do
    local key=paths:field('route'):get(i)
    if not groups[key] then groups[key]={};order[#order+1]=key end
    groups[key][#groups[key]+1]={a=W.get(paths:field('a'),i),b=W.get(paths:field('b'),i)}
  end
  local np,ns,nr,nc=0,0,0,0
  local passages,shelters,ribs=step.writes.passages,step.writes.shelters,step.writes.ribs
  local function protect(a,b,r)
    assert(ns<shelters:count(),'shelter capacity exceeded')
    W.capsule(shelters,ns,a,b,r,1);ns=ns+1
  end
  for _,route in ipairs(order) do
    local segments=groups[route]
    -- The final optional branch is not part of the main spline.
    local arc=0
    for i,segment in ipairs(segments) do
      local previous=i>1 and segments[i-1].a or segment.a
      local following=i<#segments and segments[i+1].b or segment.b
      local joins_next=i<#segments and W.length(W.sub(segment.b,segments[i+1].a))<0.001
      if not joins_next then following=segment.b end
      if i>1 and W.length(W.sub(segments[i-1].b,segment.a))>0.001 then previous=segment.a end
      local last=segment.a
      local steps=math.max(2,math.ceil(W.length(W.sub(segment.b,segment.a))/constants.terrain_segment_length))
      for j=1,steps do
        local point=curve(previous,segment.a,segment.b,following,j/steps)
        local pa,pb=profile(seed,last),profile(seed,point)
        local phase=base.prng64_normalize(base.mix(route,901))*math.pi*2
        local function pulse(p,values)
          local gain=1+0.18*math.sin(p.x/73+p.z/91+phase)
          values.width=values.width*gain;values.roof=values.roof*(1+0.17*math.cos(p.z/87+phase))
          return values
        end
        pa,pb=pulse(last,pa),pulse(point,pb)
        local d=W.sub(point,last);local length=W.length(d)
        if length>0.01 then
          assert(np<passages:count(),'terrain passage capacity exceeded')
          local horizontal=math.sqrt(d.x*d.x+d.z*d.z)
          local right=horizontal<0.001 and {x=1,y=0,z=0} or {x=-d.z/horizontal,y=0,z=d.x/horizontal}
          local up=W.normal({x=-d.y*right.z,y=d.x*right.z-d.z*right.x,z=d.y*right.x})
          -- Cross(right,tangent) points up.
          if up.y<0 then up=W.scale(up,-1) end
          W.put(passages:field('a'),np,last);W.put(passages:field('b'),np,point)
          W.put(passages:field('right'),np,right);W.put(passages:field('up'),np,up)
          passages:field('length2'):set(np,length*length)
          for _,item in ipairs({{'width_a',pa.width},{'width_b',pb.width},{'roof_a',pa.roof},{'roof_b',pb.roof},{'floor_a',pa.floor},{'floor_b',pb.floor}}) do passages:field(item[1]):set(np,item[2]) end
          passages:field('relief'):set(np,math.max(pa.relief,pb.relief))
          passages:field('phase'):set(np,phase)
          local max_radius=math.max(pa.width,pb.width,pa.roof,pb.roof,pa.floor,pb.floor)
          -- Ratios of positive linearly interpolated radii stay between their endpoint
          -- ratios. Do not combine the wide endpoint with the other endpoint's small floor.
          local ratio_a=math.max(pa.width,pa.roof,pa.floor)/math.min(pa.width,pa.roof,pa.floor)
          local ratio_b=math.max(pb.width,pb.roof,pb.floor)/math.min(pb.width,pb.roof,pb.floor)
          local reach=max_radius+(math.max(pa.relief,pb.relief)+3.5)*math.max(ratio_a,ratio_b)+1
          assert(reach<=constants.terrain_max_reach,'gallery profile exceeds declared terrain_max_reach')
          local route_bound=constants.main_neighbour_radius+constants.route_bend+constants.route_branch_length
          for _,endpoint in ipairs({last,point}) do
            assert(math.sqrt((endpoint.x-owner.x)^2+(endpoint.z-owner.z)^2)<=route_bound,'spline exceeds its declared owner reach')
          end
          passages:field('reach'):set(np,reach)
          passages:field('tag'):set(np,pb.kind+1);np=np+1
          protect(last,point,constants.route_radius)
          local middle=W.scale(W.add(last,point),0.5)
          local function tooth(axis,outer_distance,tip_distance,radius,strand)
            assert(nr<ribs:count(),'rib capacity exceeded')
            W.capsule(ribs,nr,W.add(middle,W.scale(axis,outer_distance)),W.add(middle,W.scale(axis,tip_distance)),radius,pb.kind+1)
            ribs:field('start'):set(nr,1);ribs:field('finish'):set(nr,0.08)
            ribs:field('chain'):set(nr,base.mix(route,strand,1001));ribs:field('class'):set(nr,0)
            nr=nr+1
          end
          local spacing=pb.kind==1 and 36 or 90
          local next_arc=arc+length
          -- A coherent, slowly weaving band rather than independent random teeth.
          middle=W.add(middle,W.scale(right,math.sin(next_arc/95+phase)*pb.width*0.3))
          if pb.kind==1 and math.floor(next_arc/spacing)~=math.floor(arc/spacing) then
            -- Opposing, offset teeth create a low or narrow winding slot.
            tooth(up,pb.roof+22,constants.route_radius+2,9,1)
            tooth(W.scale(up,-1),pb.floor+22,constants.route_radius+2.5,8,2)
            if j%4==0 then
              local sign=base.umod(base.mix(route,i,j),2)==0 and 1 or -1
              tooth(W.scale(right,sign),pb.width+22,constants.route_radius+2,10,3)
            end
          elseif pb.kind~=3 and math.floor(next_arc/spacing)~=math.floor(arc/spacing) then
            tooth(up,pb.roof+28,pb.roof*0.55,7,1)
            if j%8==0 then tooth(W.scale(up,-1),pb.floor+25,pb.floor*0.55,6,2) end
          end
          if pb.kind~=3 and math.abs(d.y/length)<0.45 and math.floor(next_arc/240)~=math.floor(arc/240) then
            -- Later anchors see the damaged ceiling, so a tooth cannot root in removed rock.
            assert(nc<step.writes.collapse_voids:count() and nr<ribs:count(),'collapse capacity exceeded')
            local scar={x=middle.x,y=middle.y+pb.roof+8,z=middle.z}
            W.ball(step.writes.collapse_voids,nc,scar,{x=24,y=18,z=20},pb.kind+1)
            step.writes.collapse_voids:field('relief'):set(nc,3)
            step.writes.collapse_voids:field('phase'):set(nc,phase);nc=nc+1
            for lobe=1,2 do
              assert(nc<step.writes.collapse_voids:count(),'collapse lobe capacity exceeded')
              local offset=W.add(W.scale(right,(lobe==1 and -1 or 1)*13),{x=0,y=4+lobe*3,z=0})
              W.ball(step.writes.collapse_voids,nc,W.add(scar,offset),{x=14+lobe*2,y=10+lobe,z=13},pb.kind+1)
              step.writes.collapse_voids:field('relief'):set(nc,3)
              step.writes.collapse_voids:field('phase'):set(nc,phase+lobe);nc=nc+1
            end
            local a={x=middle.x,y=middle.y-pb.floor-8,z=middle.z}
            local b=W.add(a,{x=0,y=26,z=0})
            W.capsule(ribs,nr,a,b,22,pb.kind+1)
            ribs:field('class'):set(nr,1);ribs:field('chain'):set(nr,base.mix(route,nc,1002))
            W.put(ribs:field('scar'),nr,W.add(scar,{x=0,y=12,z=0}));nr=nr+1
          end
          arc=next_arc
        end
        last=point
      end
    end
  end
  local nodes=step.reads.network_nodes
  local edges=step.reads.network_edges
  local adjacency={}
  local positions={}
  for i=0,nodes:count()-1 do positions[nodes:field('key'):get(i)]=W.get(nodes:field('centre'),i) end
  for i=0,edges:count()-1 do
    local a,b=edges:field('a'):get(i),edges:field('b'):get(i)
    adjacency[a]=adjacency[a] or {};adjacency[b]=adjacency[b] or {}
    adjacency[a][#adjacency[a]+1]=b;adjacency[b][#adjacency[b]+1]=a
  end
  for i=0,nodes:count()-1 do
    local key=nodes:field('key'):get(i)
    local p=positions[key]
    local neighbours=adjacency[key] or {}
    local turn=nodes:field('main'):get(i)~=0 or #neighbours>=3
    if #neighbours==2 then
      local a=W.normal(W.sub(positions[neighbours[1]],p))
      local b=W.normal(W.sub(positions[neighbours[2]],p))
      turn=turn or (a.x*b.x+a.y*b.y+a.z*b.z)>-0.5
    end
    if turn then protect(p,p,constants.sub_length/2+2) end
  end
  local meta=step.writes.terrain_meta
  meta:field('passages'):set(0,np);meta:field('shelters'):set(0,ns);meta:field('ribs'):set(0,nr);meta:field('collapses'):set(0,nc)
  modules.feature_bounds.write(step,ribs,meta,nr,0)
end
