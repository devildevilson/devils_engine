return function(step)
  local p = step.writes.probes
  local count = step.reads.features_meta:field('probes'):get(0)
  originator.field_sample{inputs = {p:field('position')}, outputs = {p:field('value')},
    params = {field = 'support', count = count, half = 32}}
  originator.field_sample{inputs = {p:field('position')}, outputs = {p:field('raw')},
    params = {field = 'raw', count = count, half = 32}}
end
