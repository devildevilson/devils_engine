-- Поверхность чанка: марширующие тетраэдры по решётке с полосой; порода клетки — метка поля `solid` у
-- середины поверхности клетки, она же у каждой вершины клетки.
return function(step)
  local cells = math.tointeger(step.params.chunk_cells)
  local surface, middles, counts = step.writes.surface, step.writes.middles, step.writes.surface_counts
  if counts:field("sampled"):get(0) == 0 then
    counts:field("vertices"):set(0, 0)
    counts:field("middles"):set(0, 0)
    return
  end
  if step.links.caves then
    local W=modules.world
    local boxes=step.writes.detail_boxes
    local count=0
    -- Cave passages and the creases of rotated outpost shells share one refinement list. A source
    -- whose boxes all lie away from the chunk is skipped by the bounds of its list.
    local margin=math.max(2,constants.terrain_normal_step*2)
    local function touches(low,high)
      for axis,name in ipairs({'x','y','z'}) do
        local p=step.key[axis]*cells
        if low[name]>p+cells+margin or high[name]<p-margin then return false end
      end
      return true
    end
    local sources={}
    for _,cave in ipairs(step.links.caves) do
      local meta=cave.cave_detail_meta
      if touches(W.get(meta:field('low'),0),W.get(meta:field('high'),0)) then sources[#sources+1]=cave.cave_details end
    end
    for _,station in ipairs(step.links.stations or {}) do
      local meta=station.station_meta
      if meta:field('built'):get(0)~=0 and touches(W.get(meta:field('detail_low'),0),W.get(meta:field('detail_high'),0)) then sources[#sources+1]=station.station_details end
    end
    for _,details in ipairs(sources) do
      for i=0,details:count()-1 do
        local low,high=W.get(details:field('low'),i),W.get(details:field('high'),i)
        if touches(low,high) then
          assert(count<boxes:count(),'detail box capacity exceeded')
          W.put(boxes:field('low'),count,low);W.put(boxes:field('high'),count,high);count=count+1
        end
      end
    end
    originator.field_surface{
      inputs={boxes:field('low'),boxes:field('high')},
      outputs={surface:field('position'),surface:field('normal'),surface:field('shade'),counts:field('vertices')},
      params={field='solid',count=count,cells=cells,origin_x=step.key[1]*cells,origin_y=step.key[2]*cells,origin_z=step.key[3]*cells,
              refinement=constants.cave_surface_refinement,normal_step=constants.terrain_normal_step,detail_normal_step=constants.cave_detail_normal_step,
              -- Cells far from refinement: 1 = six Kuhn tetrahedra on the lattice corners, 0 = centre and face centres.
              plain=constants.surface_kuhn~=0 and 'kuhn' or 'centred',
              -- Kuhn cells only: skip blocks of surface_skip cells that cannot hold surface by the declared
              -- shape slopes, and take normals from the lattice instead of six more samples per vertex.
              skip=constants.surface_kuhn~=0 and constants.surface_skip or 0,
              -- Boxes cut a shape only beyond +-3: reach leaves fills below -3.5 and cuts above 3.5, and a
              -- cut blend of 0.35 moves that by at most 0.35+0.35/4.
              band=constants.surface_band,
              normals=(constants.surface_kuhn~=0 and constants.surface_lattice_normals~=0) and 'lattice' or 'sampled'}}
    counts:field('middles'):set(0,0)
    return
  end
  originator.marching_tetrahedra{
    inputs = { step.reads.samples:field("value"), step.reads.lattice:field("position") },
    outputs = { surface:field("position"), surface:field("normal"), surface:field("cell"), middles:field("centre"),
                counts:field("vertices"), counts:field("middles") },
    params = { border = 1 } }
  local made = math.tointeger(counts:field("middles"):get(0))
  if made == 0 then return end
  originator.field_sample{ inputs = { middles:field("centre") }, outputs = { middles:field("value"), middles:field("tag") },
                           params = { field = "solid", half = cells + 2, count = made } }
  originator.lookup{ inputs = { surface:field("cell"), middles:field("tag") }, outputs = { surface:field("shade") },
                     range = { count = counts:field("vertices") } }
end
