-- Observes intersections with the existing environment; does not fit or reject a system.
local W=modules.world
return function(step)
  local passages,probe=step.reads.passages,step.writes.cave_probe
  local n=step.reads.cave_meta:field('passages'):get(0)
  assert(n*2<=probe:count(),'cave inspection capacity exceeded')
  for i=0,n-1 do W.put(probe:field('position'),i*2,W.get(passages:field('a'),i));W.put(probe:field('position'),i*2+1,W.get(passages:field('b'),i)) end
  originator.field_sample{inputs={probe:field('position')},outputs={probe:field('value')},params={field='natural',count=n*2,half=16}}
  local portals={}
  if step.reads.cave_meta:field('has_entry'):get(0)~=0 then
    local p=W.get(step.reads.cave_meta:field('entry'),0)
    W.put(step.writes.cave_portals:field('position'),0,p)
    W.put(step.writes.cave_portals:field('direction'),0,W.get(step.reads.cave_meta:field('direction'),0))
    portals[1]=p
  end
  for i=0,n-1 do
    local va,vb=probe:field('value'):get(i*2),probe:field('value'):get(i*2+1)
    if (va<0)~=(vb<0) then
      local a,b=W.get(passages:field('a'),i),W.get(passages:field('b'),i)
      if vb<0 then a,b=b,a;va,vb=vb,va end
      local t=-va/(vb-va)
      local p=W.add(a,W.scale(W.sub(b,a),t));local unique=true
      for _,old in ipairs(portals) do if W.length(W.sub(old,p))<2 then unique=false;break end end
      if unique and #portals<step.writes.cave_portals:count() then
        W.put(step.writes.cave_portals:field('position'),#portals,p)
        W.put(step.writes.cave_portals:field('direction'),#portals,W.normal(W.sub(b,a)))
        portals[#portals+1]=p
      end
    end
  end
  step.writes.cave_inspection:field('portals'):set(0,#portals)
end
