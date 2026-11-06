return function(step)
  local walls = step.writes.station_walls
  originator.field_sample{inputs = {walls:field('centre')}, outputs = {walls:field('value')}, params = {field = 'natural'}}
  originator.field_sample{inputs = {walls:field('probe_low')}, outputs = {walls:field('value_low')}, params = {field = 'natural'}}
  originator.field_sample{inputs = {walls:field('probe_high')}, outputs = {walls:field('value_high')}, params = {field = 'natural'}}
end
