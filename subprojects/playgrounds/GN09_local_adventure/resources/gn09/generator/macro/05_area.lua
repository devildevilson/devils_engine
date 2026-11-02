-- Локатор окна: сектора окна по строкам и регионы залов, до которых окно может дотянуться.
-- Ключ окна — (min_x, min_z, ширина | высота << 32).

local W = modules.world
local sector_size = W.sector_size
local region_size = W.region_size
local reach = W.hall_reach

local function region(metres) return math.floor(metres / region_size) end

local function put(field, i, a, b)
  field:set(i, a, 0)
  field:set(i, b, 1)
end

return function(step)
  local min_x, min_z = step.key[1], step.key[2]
  local columns, rows = step.key[3] & 0xffffffff, (step.key[3] >> 32) & 0xffffffff
  if columns < 2 or rows < 2 then error("area: graph window must contain at least 2x2 sectors") end
  local max_x, max_z = min_x + columns, min_z + rows

  local cells = step.writes.cells:field("key")
  local n = 0
  for z = min_z, max_z - 1 do
    for x = min_x, max_x - 1 do
      if n >= cells:count() then error("area: cell capacity " .. cells:count() .. " exceeded") end
      put(cells, n, x, z)
      n = n + 1
    end
  end

  -- Как у хоста halls_near: регионы, покрывающие окно с запасом длины зала.
  local regions = step.writes.area_regions:field("key")
  local r = 0
  for rz = region(min_z * sector_size - reach), region(max_z * sector_size + reach) do
    for rx = region(min_x * sector_size - reach), region(max_x * sector_size + reach) do
      if r >= regions:count() then error("area: region capacity " .. regions:count() .. " exceeded") end
      put(regions, r, rx, rz)
      r = r + 1
    end
  end

  local counts = step.writes.area_counts
  counts:field("cells"):set(0, n)
  counts:field("regions"):set(0, r)
  counts:field("min_x"):set(0, min_x)
  counts:field("min_z"):set(0, min_z)
  counts:field("max_x"):set(0, max_x)
  counts:field("max_z"):set(0, max_z)
end
