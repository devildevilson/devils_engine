-- Локатор чанка: окно секторов с сектором чанка в середине — не меньше 5x5 и такое, чтобы в него
-- попали все места, чьи пещеры дотягиваются до чанка (site_reach). Чанк целиком лежит в одном секторе
-- (16 делит 96), поэтому сектор — сектор его угла.

return function(step)
  local cells = math.tointeger(step.params.chunk_cells)
  local sector = math.tointeger(step.params.sector_size)
  local margin = math.max(2, math.ceil(step.params.site_reach / sector))
  local sx = (step.key[1] * cells) // sector
  local sz = (step.key[3] * cells) // sector
  local side = 2 * margin + 1
  local area = step.writes.home:field("area")
  area:set(0, sx - margin, 0)
  area:set(0, sz - margin, 1)
  area:set(0, side | (side << 32), 2)
end
