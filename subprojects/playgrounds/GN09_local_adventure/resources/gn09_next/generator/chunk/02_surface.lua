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
    for _,cave in ipairs(step.links.caves) do
      for i=0,cave.cave_details:count()-1 do
        local low,high=W.get(cave.cave_details:field('low'),i),W.get(cave.cave_details:field('high'),i)
        local hit=true
        for axis,name in ipairs({'x','y','z'}) do
          local p=step.key[axis]*cells
          if low[name]>p+cells+math.max(2,constants.terrain_normal_step*2) or high[name]<p-math.max(2,constants.terrain_normal_step*2) then hit=false;break end
        end
        if hit then
          assert(count<boxes:count(),'detail box capacity exceeded')
          W.put(boxes:field('low'),count,low);W.put(boxes:field('high'),count,high);count=count+1
        end
      end
    end
    originator.field_surface{
      inputs={boxes:field('low'),boxes:field('high')},
      outputs={surface:field('position'),surface:field('normal'),surface:field('shade'),counts:field('vertices')},
      params={field='solid',count=count,cells=cells,origin_x=step.key[1]*cells,origin_y=step.key[2]*cells,origin_z=step.key[3]*cells,
              refinement=constants.cave_surface_refinement,normal_step=constants.terrain_normal_step,detail_normal_step=constants.cave_detail_normal_step}}
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
