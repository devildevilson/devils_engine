local F={}
function F.collect(step,padding)
  local count=0
  local function add(meta,n)
    if n==0 then return end
    local low,high=meta:field('feature_low'),meta:field('feature_high')
    for axis=0,2 do local p=step.key[axis+1]*step.params.chunk_cells;if low:get(0,axis)>p+step.params.chunk_cells+padding or high:get(0,axis)<p-padding then return end end
    assert(count<step.writes.feature_keys:count(),'feature key capacity exceeded')
    for axis=0,2 do step.writes.feature_keys:field('key'):set(count,meta:field('feature_key'):get(0,axis),axis) end
    count=count+1
  end
  for _,source in ipairs(step.links.terrain) do add(source.terrain_meta,source.terrain_meta:field('ribs'):get(0)) end
  for _,source in ipairs(step.links.biomes) do add(source.cavern_meta,source.cavern_meta:field('buttresses'):get(0)) end
  step.writes.feature_count:field('value'):set(0,count)
end
return F
