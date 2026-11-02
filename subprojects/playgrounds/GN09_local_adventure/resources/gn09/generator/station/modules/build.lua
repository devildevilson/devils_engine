-- Постройка станции из плана: четверти клеток, материал и предел выреза. Общий вывод
-- граней четвертных вокселей выполняет originator.quarter_voxel_mesh.

local B = {}
local cell, layers, plinth = constants.station_cell, 6, 6

local function quarters(u0, v0, u1, v1, chamfer, i, k)
  if i < u0 or i >= u1 or k < v0 or k >= v1 then return 0 end
  local mask = 15
  local a = { i - u0, u1 - 1 - i, u1 - 1 - i, i - u0 }
  local b = { k - v0, k - v0, v1 - 1 - k, v1 - 1 - k }
  local kept = { 6, 3, 9, 12 }
  for c = 0, 3 do
    local cut = chamfer[c] or 0
    if cut ~= 0 then
      if a[c + 1] + b[c + 1] + 2 <= cut then return 0 end
      if a[c + 1] + b[c + 1] + 1 == cut then mask = mask & kept[c + 1] end
    end
  end
  return mask
end

local function chamfer_wall(r, i, k)
  local a = { i - r.u0, r.u1 - 1 - i, r.u1 - 1 - i, i - r.u0 }
  local b = { k - r.v0, k - r.v0, r.v1 - 1 - k, r.v1 - 1 - k }
  for c = 0, 3 do
    local cut = r.chamfer[c] or 0
    if cut ~= 0 and a[c + 1] >= -1 and b[c + 1] >= -1 and
       (a[c + 1] + b[c + 1] == cut - 2 or a[c + 1] + b[c + 1] == cut - 1) then return true end
  end
  return false
end

local function roof_hatch(pose) return pose == 1 or pose == 3 end

function B.build(plan, step, p, rock_low, rock_high, sub_axis_half, sub_path_radius)
  local meta, site = step.writes.station_meta, plan.site
  local u0, v0, u1, v1 = math.huge, math.huge, -math.huge, -math.huge
  for _, r in ipairs(plan.rooms) do
    u0 = math.min(u0, r.u0 - 1); v0 = math.min(v0, r.v0 - 1)
    u1 = math.max(u1, r.u1 + 1); v1 = math.max(v1, r.v1 + 1)
  end
  local width, depth, y0, height = u1 - u0, v1 - v0, -1 - plinth, layers + 2 + 2 * plinth
  local plane, volume = width * depth, width * depth * height
  local voxels = step.writes.station_voxels
  if volume > voxels:count() then error("station: voxel capacity exceeded") end
  local solid, shade = voxels:field("solid"), voxels:field("shade")
  local function cell_index(i, k) return (k - v0) * width + (i - u0) + 1 end
  local air, outline, owners, bevel = {}, {}, {}, {}
  for _, r in ipairs(plan.rooms) do
    local chamfer = {}
    for c = 0, 3 do chamfer[c] = r.chamfer[c] ~= 0 and r.chamfer[c] + 1 or 0 end
    for i = r.u0 - 1, r.u1 do for k = r.v0 - 1, r.v1 do
      local index = cell_index(i, k)
      local contour = quarters(r.u0 - 1, r.v0 - 1, r.u1 + 1, r.v1 + 1, chamfer, i, k)
      air[index] = (air[index] or 0) | quarters(r.u0, r.v0, r.u1, r.v1, r.chamfer, i, k)
      outline[index] = (outline[index] or 0) | contour
      if contour ~= 0 then owners[index] = math.min((owners[index] or 0) + 1, 255) end
      if contour ~= 0 and not r.inner and chamfer_wall(r, i, k) then bevel[index] = true end
    end end
  end
  for _, d in ipairs(plan.doors) do
    for i = d.u0, d.u1 - 1 do for k = d.v0, d.v1 - 1 do air[cell_index(i, k)] = 15 end end
  end
  local kind = math.tointeger(meta:field("kind"):get(0))
  local wall_shade, slab_shade = kind == 2 and 115 or 123, kind == 2 and 112 or 119
  local wall_hatch = site.pose == 0
  local h = plan.hatch
  local function in_hatch(i, k) return i >= h[1] and i < h[3] and k >= h[2] and k < h[4] end
  local function rock_at(u, y, v)
    local natural = site.pose == 3 and y < 0.0 or site.pose ~= 3 and u >= 0.0
    return natural or (u >= rock_low.x and u <= rock_high.x and y >= rock_low.y and
                       y <= rock_high.y and v >= rock_low.z and v <= rock_high.z)
  end
  local function voxel_index(i, y, k) return (y - y0) * plane + cell_index(i, k) - 1 end
  for k = v0, v1 - 1 do for i = u0, u1 - 1 do
    local index = cell_index(i, k)
    local footprint = outline[index] or 0
    if footprint ~= 0 then
      local wall = footprint & ~(air[index] or 0)
      local hatch = in_hatch(i, k)
      for y = y0, y0 + height - 1 do
        local q, material = 0, wall_shade
        if y == -1 then
          q = (wall_hatch or not hatch or site.pose ~= 2) and footprint or 0
          material = slab_shade
        elseif y == layers then
          q = (wall_hatch or not hatch or not roof_hatch(site.pose)) and footprint or 0
          material = slab_shade
        elseif y >= 0 and y < layers then
          q = hatch and wall_hatch and y < 4 and 0 or wall
          if wall_hatch and (hatch or in_hatch(i, k - 1) or in_hatch(i, k + 1)) then material = 126 end
          if q ~= 0 and bevel[index] and owners[index] == 1 and y >= 1 and y <= 4 and
             not rock_at((i + 0.5) * cell, (y + 0.5) * cell, (k + 0.5) * cell) then material = 113 end
        end
        local at = voxel_index(i, y, k)
        solid:set(at, q); shade:set(at, material)
      end
      if not wall_hatch and hatch then
        shade:set(voxel_index(i, -1, k), 126)
        shade:set(voxel_index(i, layers, k), 126)
      end
      local u, v = (i + 0.5) * cell, (k + 0.5) * cell
      local function extend(start, sign)
        local reaches = false
        for n = 0, plinth - 1 do
          if rock_at(u, (start + sign * n + 0.5) * cell, v) then reaches = true break end
        end
        if not reaches then return end
        for n = 0, plinth - 1 do
          local y = start + sign * n
          local rock = rock_at(u, (y + 0.5) * cell, v)
          local at = voxel_index(i, y, k)
          solid:set(at, footprint); shade:set(at, slab_shade)
          if rock then return end
        end
      end
      if roof_hatch(site.pose) then extend(-2, -1) end
      if site.pose == 2 then extend(layers + 1, 1) end
    end
  end end

  local origin_field, size_field = meta:field("voxel_origin"), meta:field("voxel_size")
  origin_field:set(0, u0, 0); origin_field:set(0, v0, 1); origin_field:set(0, y0, 2)
  size_field:set(0, width, 0); size_field:set(0, depth, 1); size_field:set(0, height, 2)
  meta:field("voxels"):set(0, volume)
  local mesh = step.writes.station_mesh
  -- Входы общего инструмента — заполненный растр, поза и геометрия ячейки.
  local axes = step.writes.station_rock
  originator.quarter_voxel_mesh{
    inputs = { solid, shade, meta:field("origin"), axes:field("axis_u"), axes:field("axis_v"),
               origin_field, size_field },
    outputs = { mesh:field("a"), mesh:field("b"), mesh:field("c"), mesh:field("normal"),
                mesh:field("shade"), meta:field("triangles") },
    params = { cell = cell } }

  local low = { x = math.huge, y = math.huge, z = math.huge }
  local high = { x = -math.huge, y = -math.huge, z = -math.huge }
  local au, av = math.cos(site.yaw), math.sin(site.yaw)
  local function include(x, y, z, radius)
    low.x = math.min(low.x, x - radius); low.y = math.min(low.y, y - radius); low.z = math.min(low.z, z - radius)
    high.x = math.max(high.x, x + radius); high.y = math.max(high.y, y + radius); high.z = math.max(high.z, z + radius)
  end
  for _, i in ipairs({ u0, u0 + width }) do for _, k in ipairs({ v0, v0 + depth }) do
    for _, y in ipairs({ y0, y0 + height }) do
      local u, v = i * cell, k * cell
      include(site.origin.x + au * u - av * v, site.origin.y + y * cell,
              site.origin.z + av * u + au * v, 0.0)
    end
  end end
  for _, centre in ipairs({ site.node_centre, site.berth }) do
    for _, sign in ipairs({ -1.0, 1.0 }) do
      include(centre.x + sign * site.berth_axis.x * sub_axis_half,
              centre.y + sign * site.berth_axis.y * sub_axis_half,
              centre.z + sign * site.berth_axis.z * sub_axis_half, sub_path_radius)
    end
  end
  local margin = 5.0
  low.x = low.x - margin; low.y = low.y - margin; low.z = low.z - margin
  high.x = high.x + margin; high.y = high.y + margin; high.z = high.z + margin
  local function set3(field, v)
    field:set(0, v.x, 0); field:set(0, v.y, 1); field:set(0, v.z, 2)
  end
  set3(meta:field("low"), low); set3(meta:field("high"), high)
  local c, reach = site.node_centre, p.station_reach
  local fits = not (low.x < c.x - reach or high.x > c.x + reach or
                    low.y < c.y - reach or high.y > c.y + reach or
                    low.z < c.z - reach or high.z > c.z + reach)
  meta:field("built"):set(0, fits and 1 or 0)
end

return B
