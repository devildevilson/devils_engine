return function(step)
  local probes=step.writes.outpost_probes
  originator.field_sample{inputs={probes:field('position')},outputs={probes:field('value')},
    params={field='natural',count=step.reads.outpost_meta:field('probes'):get(0),half=24}}
  originator.field_sample{inputs={probes:field('position')},outputs={probes:field('clearance')},
    params={field='clearance',count=step.reads.outpost_meta:field('probes'):get(0),half=24}}
end
