-- Shared outpost layout: the placement search and the building grammar must agree on it.
local O = {}
-- Nominal body: 2x2 rooms on 2-3 floors. The placement search only asks whether this much rock lies
-- behind the wall (body_offsets); the real core grows room by room (modules/core.lua).
O.room = {x = 6, y = 3, z = 5}
-- Pose codes keep the report order: 0 side, 1 below the dock, 2 above the dock.
O.side, O.below, O.above = 0, 1, 2
-- Room labels in station_rooms; the order dock -> lock -> guard -> technical -> core follows the graph.
O.lock, O.dock, O.guard, O.technical, O.core, O.passage = 0, 1, 2, 3, 4, 5
O.trunk, O.tower = 0, 1
-- Half width of a horizontal tube (its height is one deck) and of a vertical shaft.
O.tube, O.shaft = 1.5, 1.4

function O.deck() return constants.outpost_deck end
function O.cell() return constants.outpost_cell end

function O.floors(seed, x, z) return 2 + base.umod(base.mix(seed, x, z, 501), 2) end
function O.body_half(floors) return {x = 2 * O.room.x, y = floors * O.room.y, z = 2 * O.room.z} end

-- Local point where the transition meets the core; the origin is placed so that this point lies on the hatch axis.
function O.entry_local(floors, pose)
  local h = O.body_half(floors)
  if pose == O.side then return {x = O.room.x, y = -h.y + O.deck() / 2, z = -h.z} end
  if pose == O.above then return {x = O.room.x, y = -h.y, z = -O.room.z} end
  return {x = O.room.x, y = h.y, z = -O.room.z}
end

-- Transition axis in the outpost frame: the side lock runs along +z, vertical ones along +-y.
function O.along(pose)
  if pose == O.side then return 'z', 1 end
  return 'y', pose == O.above and 1 or -1
end
-- Lateral axes of the transition; for a side lock y is lateral but stays on the deck.
function O.lateral(pose)
  if pose == O.side then return 'x', 'y' end
  return 'x', 'z'
end
-- Cross-section half of a tube on the hatch axis.
function O.tube_half(pose)
  if pose == O.side then return {x = O.tube, y = O.deck() / 2, z = 0} end
  return {x = O.shaft, y = 0, z = O.shaft}
end

-- Boat volume probes in the dock frame: long axis is local x; a 3x3x3 lattice over the hull box.
function O.dock_offsets()
  local L, B = constants.sub_length / 2, constants.sub_beam / 2
  local out = {}
  for iz = -1, 1 do
    for iy = -1, 1 do
      for ix = -1, 1 do
        out[#out + 1] = {x = ix * L, y = iy * B, z = iz * B}
      end
    end
  end
  return out
end

function O.body_offsets(floors)
  local h = O.body_half(floors)
  local out = {}
  for iz = -1, 1 do
    for iy = -1, 1 do
      for ix = -1, 1 do
        out[#out + 1] = {x = ix * (h.x - 2), y = iy * (h.y - 1), z = iz * (h.z - 2)}
      end
    end
  end
  return out
end

-- Tower shapes as cells {lateral u, lateral v or decks, along}. Side towers stand two cells deep;
-- vertical towers stack their decks along the hatch axis. The choice is made by the rooms step and
-- repeats the tower roll of the pair choice (salt 940).
function O.tower_shapes(pose)
  if pose == O.side then
    return {{2, 2, 2}, {2, 3, 2}, {3, 2, 2}, {2, 4, 2}, {3, 3, 2}, {2, 3, 1}, {3, 2, 1}, {3, 3, 1}}
  end
  return {{2, 2, 2}, {2, 2, 3}, {3, 2, 2}, {2, 2, 4}, {3, 2, 3}}
end

-- Trunk rooms: half extents across the axis and their length along it.
function O.trunk_room(pose, label)
  local deck = O.deck()
  local wide = label == O.technical
  if pose == O.side then return {x = wide and 5 or 3, y = deck / 2}, wide and 8 or 6 end
  return {x = wide and 5 or 3, z = wide and 4 or 3}, deck
end

-- Rectangles {l={a,b},h={a,b}} minus one rectangle; pieces are split along both plane axes.
function O.subtract(pieces, low, high)
  local eps = 0.01
  local out = {}
  for _, p in ipairs(pieces) do
    local hit = true
    for k = 1, 2 do hit = hit and p.l[k] < high[k] - eps and p.h[k] > low[k] + eps end
    if not hit then
      out[#out + 1] = p
    else
      local l, h = {p.l[1], p.l[2]}, {p.h[1], p.h[2]}
      for k = 1, 2 do
        if l[k] < low[k] - eps then
          local nh = {h[1], h[2]}
          nh[k] = low[k]
          out[#out + 1] = {l = {l[1], l[2]}, h = nh}
          l[k] = low[k]
        end
        if h[k] > high[k] + eps then
          local nl = {l[1], l[2]}
          nl[k] = high[k]
          out[#out + 1] = {l = nl, h = {h[1], h[2]}}
          h[k] = high[k]
        end
      end
    end
  end
  return out
end

-- In-plane axes of a face whose normal is the given axis.
O.plane_axes = {x = {'y', 'z'}, y = {'x', 'z'}, z = {'x', 'y'}}

return O
