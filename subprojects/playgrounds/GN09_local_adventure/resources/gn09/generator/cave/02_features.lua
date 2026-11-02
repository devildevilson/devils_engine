-- Тело шага features: предметы пещеры. Залежь — в кармане (pocket:*), кладки — в гнезде (у гнездовой
-- пещеры три), монстры — в логове (у пещеры с залежами логово и гнездо одно), ледяная пробка — в горловине
-- с флагом пробки. Предмет лежит у дна своей камеры, внутри её
-- гарантированного ядра (форма с шумом может приподнять пол).

return function(step)
  local p = step.params
  local state = step.writes.state
  state:field("features"):set(0, 0)
  if state:field("built"):get(0) == 0 then return end
  local seed, key = step.world_seed, step.key[3]
  local recipe = math.tointeger(step.reads.site:field("recipe"):get(0))
  local core = p.cave_core
  local purposes, count = {}, 0
  for word in string.gmatch(p.purposes, "%S+") do purposes[count] = word; count = count + 1 end
  local kinds = {}
  local index = 0
  for word in string.gmatch(p.feature_kinds, "%S+") do kinds[word] = index; index = index + 1 end
  local rooms = step.reads.rooms
  local centre, radius, squash, purpose, flags = rooms:field("centre"), rooms:field("radius"), rooms:field("squash"),
                                                 rooms:field("purpose"), rooms:field("flags")
  local nest = math.tointeger(state:field("nest"):get(0))
  local lair = math.tointeger(state:field("lair"):get(0))
  local out = step.writes.features
  local made = 0
  local function add(room, kind, amount, x, y, z)
    if made >= out:count() then error("cave features: capacity " .. out:count() .. " exceeded") end
    out:field("room"):set(made, room)
    out:field("kind"):set(made, kinds[kind])
    out:field("amount"):set(made, amount)
    local c = out:field("centre")
    c:set(made, x, 0); c:set(made, y, 1); c:set(made, z, 2)
    made = made + 1
  end
  for i = 1, math.tointeger(state:field("rooms"):get(0)) - 1 do
    local cx, cy, cz = centre:get(i, 0), centre:get(i, 1), centre:get(i, 2)
    local hash = base.mix(seed, key, i, 0x524553)
    local function floor_spot(dx, dz)
      local drop = math.min(0.5 * radius:get(i) * squash:get(i), core - 0.8)
      local reach = math.min(1.0, core - 0.8 - drop)
      local len = base.hypot(dx, dz)
      local k = len > reach and reach / len or 1.0
      return cx + dx * k, cy - drop, cz + dz * k
    end
    local name = purposes[math.tointeger(purpose:get(i))]
    if string.sub(name, 1, 7) == "pocket:" then
      local kind = recipe == 2 and "thermal_crystal" or ((hash & 16) ~= 0 and "metal_ore" or "mineral_vein")
      add(i, kind, 1 + base.umod(hash >> 8, 3), floor_spot(0.0, 0.0))
    elseif i == nest then
      local clutches = recipe == 1 and 3 or 1
      for c = 0, clutches - 1 do
        local angle = 2.094395102393195 * c + base.umod(hash, 7)
        local spread = clutches > 1 and 0.45 * radius:get(i) or 0.0
        add(i, "egg_cluster", 2 + base.umod(hash >> (8 + 4 * c), 4), floor_spot(spread * math.cos(angle), spread * math.sin(angle)))
      end
      if i == lair then add(i, "monster_group", 2 + base.umod(hash >> 20, 3), floor_spot(0.0, 0.0)) end
    elseif name == "lair" then
      add(i, "monster_group", 2 + base.umod(hash >> 8, 3), floor_spot(0.0, 0.0))
    elseif math.tointeger(flags:get(i)) & 2 ~= 0 then
      add(i, "ice_plug", 1, cx, cy, cz)
    end
  end
  state:field("features"):set(0, made)
end
