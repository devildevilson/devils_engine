local W=modules.world
return function(step)
  local meta=step.writes.outpost_meta
  local values=step.reads.outpost_probes:field('value')
  local proposals=step.reads.outpost_proposals
  local clearance=step.reads.outpost_probes:field('clearance')
  local best,best_score,occupancy,fallback=-1,-math.huge,0,false
  for i=0,meta:field('proposals'):get(0)-1 do
    local first=i*39
    local rock=0
    local protected=false
    for j=0,26 do if values:get(first+j)>0 then rock=rock+1 end end
    for j=0,26 do protected=protected or clearance:get(first+j)<2.9 end
    local clear,fallback_clear=true,true
    for j=27,32 do clear=clear and values:get(first+j)<-0.4 end
    for j=33,38 do fallback_clear=fallback_clear and values:get(first+j)<-0.4 end
    if rock>=17 and not protected and (clear or fallback_clear) then
      local depth=proposals:field('depth'):get(i)
      local distance=proposals:field('distance'):get(i)
      local score=-math.abs(rock/27-0.78)*100-depth*0.3-(clear and 0 or distance*0.08)
      score=score+base.prng64_normalize(base.mix(step.world_seed,step.key[1],step.key[2],i,920))*6
      if score>best_score then best,best_score,occupancy,fallback=i,score,rock/27,not clear end
    end
  end
  meta:field('chosen'):set(0,best);meta:field('occupancy'):set(0,occupancy)
  meta:field('fallback_dock'):set(0,fallback and 1 or 0)
  if best<0 then error('main outpost has no fitting rock candidate') end
  local body=W.get(proposals:field('body'),best)
  local direction=W.get(proposals:field('direction'),best)
  local dock=W.get(proposals:field('dock'),best)
  if fallback then dock=modules.network.main(step.world_seed,step.key[1],step.key[2]) end
  local mouth=W.sub(body,W.scale(direction,math.abs(direction.y)>0.5 and 4 or 17))
  local ports=step.writes.ports
  ports:field('key'):set(0,step.key[1],0);ports:field('key'):set(0,step.key[2],1)
  W.put(ports:field('mouth'),0,mouth);W.put(ports:field('direction'),0,direction);W.put(ports:field('dock'),0,dock)
  ports:field('kind'):set(0,2)
end
