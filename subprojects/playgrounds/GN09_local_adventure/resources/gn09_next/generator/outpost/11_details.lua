-- Refinement boxes where rock meets the rotated shell: the 1 m lattice would show its stair pattern
-- along a crease that is not aligned with the lattice. The exposure step sampled a grid just outside
-- every outer wall piece; every grid cell whose corners see both rock and water gets a thin box.
local W,F,O=modules.world,modules.frame,modules.outpost
local names={'x','y','z'}
local margin,thickness=0.5,0.75
return function(step)
  local walls,values=step.reads.station_walls,step.reads.outpost_probes:field('value')
  local meta=step.writes.station_meta
  local frame=F.from_meta(meta)
  local out=step.writes.station_details
  local count=0
  local detail_low,detail_high={x=math.huge,y=math.huge,z=math.huge},{x=-math.huge,y=-math.huge,z=-math.huge}
  -- outpost_crease_refinement = 0 leaves the stair pattern, for comparing the price of the boxes.
  local walls_count=constants.outpost_crease_refinement~=0 and meta:field('walls'):get(0) or 0
  for w=0,walls_count-1 do
    if walls:field('other'):get(w)<0 then
      local axis=names[walls:field('axis'):get(w)+1]
      local pa,pb=O.plane_axes[axis][1],O.plane_axes[axis][2]
      local c,h=W.get(walls:field('local_centre'),w),W.get(walls:field('half'),w)
      local first,na,nb=walls:field('first_probe'):get(w),walls:field('probes_a'):get(w),walls:field('probes_b'):get(w)
      local span={}
      span[pa],span[pb]=h[pa]+margin,h[pb]+margin
      local function rock(a,b) return values:get(first+b*na+a)>=0 end
      for b=0,nb-2 do for a=0,na-2 do
        local r=(rock(a,b) and 1 or 0)+(rock(a+1,b) and 1 or 0)+(rock(a,b+1) and 1 or 0)+(rock(a+1,b+1) and 1 or 0)
        if r>0 and r<4 then
          assert(count<out:count(),'outpost detail box capacity exceeded')
          local centre,half={x=c.x,y=c.y,z=c.z},{x=0,y=0,z=0}
          half[axis]=thickness
          half[pa],half[pb]=span[pa]/(na-1)+0.25,span[pb]/(nb-1)+0.25
          centre[pa]=centre[pa]+((2*a+1)/(na-1)-1)*span[pa]
          centre[pb]=centre[pb]+((2*b+1)/(nb-1)-1)*span[pb]
          local world,extent=frame.point(centre),frame.extent(half)
          local lo,hi=W.sub(world,extent),W.add(world,extent)
          W.put(out:field('low'),count,lo);W.put(out:field('high'),count,hi)
          for _,a in ipairs(names) do detail_low[a]=math.min(detail_low[a],lo[a]);detail_high[a]=math.max(detail_high[a],hi[a]) end
          count=count+1
        end
      end end
    end
  end
  meta:field('details'):set(0,count)
  -- Bounds of all refinement boxes: a chunk far from them skips the whole list.
  W.put(meta:field('detail_low'),0,detail_low);W.put(meta:field('detail_high'),0,detail_high)
end
