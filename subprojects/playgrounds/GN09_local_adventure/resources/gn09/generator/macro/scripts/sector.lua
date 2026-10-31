-- Локатор сектора: ключи регионов залов, концов маршрутов и окрестности 4x4.
--
-- Регионы — те, чьи залы могут дотянуться до базы сектора или до его маршрутов на восток и юг:
-- самый длинный зал с запасом на рельеф и ниши (reach) не дальше этого. Лишний регион ничего не
-- меняет (зал далеко), недостающий поменял бы мир, поэтому полоса взята с запасом в сектор.

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
  local x, z = step.key[1], step.key[2]
  local regions = step.writes.regions:field("key")
  local ends = step.writes.ends:field("key")
  local around = step.writes.around:field("key")
  local counts = step.writes.sector_counts

  local n = 0
  for rz = region((z - 1) * sector_size - reach), region((z + 3) * sector_size + reach) do
    for rx = region((x - 1) * sector_size - reach), region((x + 3) * sector_size + reach) do
      if n >= regions:count() then error("sector: region capacity " .. regions:count() .. " exceeded") end
      put(regions, n, rx, rz)
      n = n + 1
    end
  end
  counts:field("regions"):set(0, n)

  put(ends, 0, x, z)
  put(ends, 1, x + 1, z)
  put(ends, 2, x, z + 1)
  counts:field("ends"):set(0, 3)

  local k = 0
  for bz = z - 1, z + 2 do
    for bx = x - 1, x + 2 do
      put(around, k, bx, bz)
      k = k + 1
    end
  end
  counts:field("around"):set(0, k)
end
