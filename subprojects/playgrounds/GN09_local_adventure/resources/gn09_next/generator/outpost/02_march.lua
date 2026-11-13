return function(step)
  local rays = step.writes.outpost_rays
  originator.field_march{inputs = {rays:field('origin'), rays:field('direction')}, outputs = {rays:field('distance')},
    params =
    {field = 'natural', count = step.reads.outpost_meta:field('rays'):get(0), from = 0,
      to = constants.outpost_search_reach, step = 4, refine = 5, want = 'rock', half = 24}}
end
