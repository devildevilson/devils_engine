-- Soft choice among dock/body pairs whose batched probes passed.
local W=modules.world
local body_probes,dock_probes=27,27
return function(step)
  local meta=step.writes.outpost_meta
  local values=step.reads.outpost_probes:field('value')
  local clearance=step.reads.outpost_probes:field('clearance')
  local docks,bodies,rays=step.writes.outpost_docks,step.writes.outpost_bodies,step.reads.outpost_rays
  local approach={}
  for r=0,meta:field('rays'):get(0)-1 do
    local first,count=rays:field('first_probe'):get(r),rays:field('sweep_probes'):get(r)
    local clear=true
    for j=first,first+count-1 do clear=clear and values:get(j)<-0.4 end
    approach[r]=clear
  end
  -- 1 approach blocked, 2 dock blocked, 3 body mostly in water, 4 body cuts the protected boat route.
  local refused={0,0,0,0}
  local good_docks={}
  for i=0,meta:field('docks'):get(0)-1 do
    local first,clear=docks:field('first_probe'):get(i),true
    for j=first,first+dock_probes-1 do clear=clear and values:get(j)<-0.4 end
    local refusal=not approach[docks:field('ray'):get(i)] and 1 or (clear and 0 or 2)
    docks:field('refusal'):set(i,refusal)
    if refusal>0 then refused[refusal]=refused[refusal]+1
    else
      local slot=docks:field('ray'):get(i)*16+docks:field('rotation'):get(i)
      good_docks[slot]=good_docks[slot] or {}
      table.insert(good_docks[slot],i)
    end
  end
  -- The tower roll is made here: a wished tower prefers docks with water room between boat and rock.
  local tower_wish=base.prng64_normalize(base.mix(step.world_seed,step.key[1],step.key[2],940))<constants.outpost_tower_chance
  local best_dock,best_body,best_score,occupancy,pairs=-1,-1,-math.huge,0,0
  for i=0,meta:field('bodies'):get(0)-1 do
    local first=bodies:field('first_probe'):get(i)
    local rock,protected=0,false
    for j=first,first+body_probes-1 do
      if values:get(j)>0 then rock=rock+1 end
      protected=protected or clearance:get(j)<2.9
    end
    local refusal=rock<17 and 3 or (protected and 4 or 0)
    bodies:field('refusal'):set(i,refusal)
    if refusal>0 then refused[refusal]=refused[refusal]+1
    else
      local depth=bodies:field('depth'):get(i)
      local distance=bodies:field('distance'):get(i)
      for _,d in ipairs(good_docks[bodies:field('ray'):get(i)*16+bodies:field('rotation'):get(i)] or {}) do
        pairs=pairs+1
        local standoff=docks:field('standoff'):get(d)
        local score=-math.abs(rock/27-0.78)*100-depth*0.3-distance*0.04-(tower_wish and math.abs(standoff-14)*0.5 or standoff*0.5)
        score=score+base.prng64_normalize(base.mix(step.world_seed,step.key[1],step.key[2],i*64+d,920))*6
        if score>best_score then best_dock,best_body,best_score,occupancy=d,i,score,rock/27 end
      end
    end
  end
  meta:field('proposals'):set(0,pairs)
  meta:field('chosen_dock'):set(0,best_dock);meta:field('chosen_body'):set(0,best_body);meta:field('occupancy'):set(0,occupancy)
  for k,name in ipairs({'refused_approach','refused_dock','refused_water','refused_protected'}) do meta:field(name):set(0,refused[k]) end
  if best_body<0 then
    error(string.format('main outpost %d,%d has no fitting dock/body pair: %d docks, %d bodies, refused docks: approach %d, blocked %d; refused bodies: open water %d, protected route %d',
      step.key[1],step.key[2],meta:field('docks'):get(0),meta:field('bodies'):get(0),refused[1],refused[2],refused[3],refused[4]))
  end
  meta:field('standoff'):set(0,docks:field('standoff'):get(best_dock))
  local frame=step.writes.outpost_frame
  W.put(frame:field('dock'),0,W.get(docks:field('dock'),best_dock))
  W.put(frame:field('hatch'),0,W.get(docks:field('hatch'),best_dock))
  W.put(frame:field('origin'),0,W.get(bodies:field('origin'),best_body))
  W.put(frame:field('front'),0,W.get(bodies:field('front'),best_body))
  frame:field('yaw_c'):set(0,bodies:field('yaw_c'):get(best_body))
  frame:field('yaw_s'):set(0,bodies:field('yaw_s'):get(best_body))
  frame:field('pose'):set(0,bodies:field('pose'):get(best_body))
end
