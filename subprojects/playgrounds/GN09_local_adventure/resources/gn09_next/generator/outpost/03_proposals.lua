local W=modules.world
return function(step)
  local rays=step.reads.outpost_rays
  local proposals,probes=step.writes.outpost_proposals,step.writes.outpost_probes
  local np,nv=0,0
  local floors=2+base.umod(base.mix(step.world_seed,step.key[1],step.key[2],501),2)
  local function probe(p) W.put(probes:field('position'),nv,p);nv=nv+1 end
  for i=0,step.writes.outpost_meta:field('rays'):get(0)-1 do
    local distance=rays:field('distance'):get(i)
    if distance>0 then
      local p,d=W.get(rays:field('origin'),i),W.get(rays:field('direction'),i)
      for _,depth in ipairs({4,10,18}) do
        local body=W.add(p,W.scale(d,distance+depth))
        local dock=W.add(p,W.scale(d,math.max(0,distance-28)))
        W.put(proposals:field('body'),np,body);W.put(proposals:field('dock'),np,dock)
        W.put(proposals:field('direction'),np,d)
        proposals:field('depth'):set(np,depth);proposals:field('distance'):set(np,distance)
        for iz=-1,1 do for iy=-1,1 do for ix=-1,1 do
          probe(W.add(body,{x=ix*10,y=iy*(floors*3-1),z=iz*8}))
        end end end
        for _,centre in ipairs({dock,p}) do
          for axis,name in ipairs({'x','y','z'}) do for _,sign in ipairs({-1,1}) do
            local q={x=centre.x,y=centre.y,z=centre.z}
            q[name]=q[name]+sign*(axis==1 and constants.sub_length/2 or constants.sub_beam/2)
            probe(q)
          end end
        end
        np=np+1
      end
    end
  end
  step.writes.outpost_meta:field('proposals'):set(0,np)
  step.writes.outpost_meta:field('probes'):set(0,nv)
end
