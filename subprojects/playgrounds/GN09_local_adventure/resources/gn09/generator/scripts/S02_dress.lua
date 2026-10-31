-- Тело шага dress: колонны в крупных нишах.
--
-- Колонна — порода внутри объёма ниши, за которой не видно дальше: в нише с колоннами фонарь не
-- достаёт до противоположной стены, и направление теряется легче. Колонна вычитается только из
-- красивой формы ниши, но не из гарантированного ядра прохода, — поэтому ставится в стороне от
-- всех ходов и комнат (по горизонтали), и проход она не режет при любых числах этого шага.

return function(step)
  local p = step.params
  local site = step.reads.site
  local links = step.reads.links
  local rooms = step.writes.rooms
  local pillars = step.writes.pillars
  local state = step.writes.state

  local count = state:field("pillars")
  count:set(0, 0)
  if state:field("built"):get(0) == 0 then return end

  local nr = math.tointeger(state:field("rooms"):get(0))
  local nl = math.tointeger(state:field("links"):get(0))
  local core = site:field("core"):get(0)
  local relief = site:field("relief"):get(0)

  local f_centre = rooms:field("centre")
  local f_radius = rooms:field("radius")
  local f_squash = rooms:field("squash")
  local f_stretch = rooms:field("stretch")
  local f_yaw = rooms:field("yaw")
  local f_flags = rooms:field("flags")
  local l_a = links:field("a")
  local l_b = links:field("b")
  local c_base = pillars:field("base")
  local c_radius = pillars:field("radius")
  local c_height = pillars:field("height")

  local seed = step.seed
  local draw = 0
  local function next()
    draw = draw + 1
    return base.prng64_normalize(base.prng64_2(seed, draw))
  end
  local function between(low, high) return low + (high - low) * next() end

  -- Масштаб облика пещеры раскладка оставила в state: шаги делят только буферы.
  local scale = state:field("scale"):get(0)
  local function xz(i) return f_centre:get(i, 0), f_centre:get(i, 2) end
  local function horizontal(px, pz, ax, az, bx, bz)
    local dx, dz = bx - ax, bz - az
    local d2 = dx * dx + dz * dz
    if d2 < 1e-12 then return math.sqrt((px - ax) ^ 2 + (pz - az) ^ 2) end
    local t = math.max(0.0, math.min(1.0, ((px - ax) * dx + (pz - az) * dz) / d2))
    return math.sqrt((px - ax - t * dx) ^ 2 + (pz - az - t * dz) ^ 2)
  end

  local made = 0
  local attempts = math.tointeger(p.pillar_attempts)
  for i = 1, nr - 1 do
    local radius = f_radius:get(i)
    local entrance = (math.tointeger(f_flags:get(i)) & 1) ~= 0
    if radius >= core + p.pillar_room and not entrance then
      local want
      if radius >= core + p.pillar_room_double then
        want = 1 + (next() < p.pillar_chance_double and 1 or 0)
      else
        want = next() < p.pillar_chance_single and 1 or 0
      end
      local cx, cy, cz = f_centre:get(i, 0), f_centre:get(i, 1), f_centre:get(i, 2)
      local stretch, squash, yaw = f_stretch:get(i), f_squash:get(i), f_yaw:get(i)
      for _ = 1, want do
        for _ = 1, attempts do
          local a, d = 2.0 * math.pi * next(), between(0.4, 0.8)
          local lx, lz = math.cos(a) * d * radius * stretch, math.sin(a) * d * radius
          local c, s = math.cos(yaw), math.sin(yaw)
          local x, z = cx + lx * c - lz * s, cz + lx * s + lz * c
          local pillar_radius = between(p.pillar_radius_min, p.pillar_radius_max) * scale
          local clear = core + relief + 1.3 * pillar_radius + 0.3
          local free = true
          for j = 0, nl - 1 do
            if free then
              local ax, az = xz(math.tointeger(l_a:get(j)))
              local bx, bz = xz(math.tointeger(l_b:get(j)))
              free = horizontal(x, z, ax, az, bx, bz) > clear
            end
          end
          for k = 0, nr - 1 do
            if free then
              local ox, oz = xz(k)
              free = horizontal(x, z, ox, oz, ox, oz) > clear
            end
          end
          if free then
            if made >= pillars:count() then error("cave dress: pillar capacity " .. pillars:count() .. " exceeded") end
            local half = radius * squash + 2.0
            c_base:set(made, x, 0); c_base:set(made, cy - half, 1); c_base:set(made, z, 2)
            c_radius:set(made, pillar_radius)
            c_height:set(made, 2.0 * half)
            made = made + 1
            break
          end
        end
      end
    end
  end
  count:set(0, made)
end
