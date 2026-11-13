-- Dock first: docks stand off the rock hit by each ray; bodies sit behind that rock.
-- A dock and a body on the same ray and rotation form one proposal.
local W, F, O = modules.world, modules.frame, modules.outpost
local standoffs = {3, 9, 18, 30}
local depths = {4, 10, 18}
return function(step)
  local rays = step.writes.outpost_rays
  local docks, bodies, probes = step.writes.outpost_docks, step.writes.outpost_bodies, step.writes.outpost_probes
  local meta = step.writes.outpost_meta
  local floors = O.floors(step.world_seed, step.key[1], step.key[2])
  local B = constants.sub_beam / 2
  local nd, nb, nv = 0, 0, 0
  local function probe(p)
    assert(nv < probes:count(), 'outpost probe capacity exceeded')
    W.put(probes:field('position'), nv, p)
    nv = nv + 1
  end
  local yaw_offset = base.prng64_normalize(base.mix(step.world_seed, step.key[1], step.key[2], 931))
  for i = 0, meta:field('rays'):get(0) - 1 do
    local distance = rays:field('distance'):get(i)
    local p, d = W.get(rays:field('origin'), i), W.get(rays:field('direction'), i)
    local nearest = distance - B - standoffs[1]
    rays:field('first_probe'):set(i, nv)
    local usable = distance > 0 and nearest > 0
    if usable then
      -- Approach sweep: the boat cross-section from the node centre to the nearest dock.
      local u = math.abs(d.y) > 0.5 and {x = 1, y = 0, z = 0} or {x = 0, y = 1, z = 0}
      local v = {x = d.y * u.z - d.z * u.y, y = d.z * u.x - d.x * u.z, z = d.x * u.y - d.y * u.x}
      for t = 0, nearest, constants.outpost_approach_step do
        local c = W.add(p, W.scale(d, t))
        for _, side in ipairs({u, v}) do
          for _, sign in ipairs({-1, 1}) do probe(W.add(c, W.scale(side, sign * B))) end
        end
      end
    end
    rays:field('sweep_probes'):set(i, nv - rays:field('first_probe'):get(i))
    if usable then
      local pose = d.y > 0.5 and O.above or (d.y < -0.5 and O.below or O.side)
      local hit = W.add(p, W.scale(d, distance))
      local rotations = {}
      if pose == O.side then
        rotations[1] = F.facing({x = 0, y = 0, z = 0}, d)
      else
        for k = 0, constants.outpost_vertical_yaws - 1 do
          local angle = (k + yaw_offset) * math.pi / constants.outpost_vertical_yaws
          rotations[#rotations + 1] = F.new({x = 0, y = 0, z = 0}, math.cos(angle), math.sin(angle))
        end
      end
      for r, rotation in ipairs(rotations) do
        local hatch_line = W.add(p, W.scale(d, nearest + B + 1))
        for _, standoff in ipairs(standoffs) do
          local reach = distance - B - standoff
          if reach >= 0 then
            assert(nd < docks:count(), 'outpost dock capacity exceeded')
            local dock = W.add(p, W.scale(d, reach))
            W.put(docks:field('dock'), nd, dock)
            W.put(docks:field('hatch'), nd, W.add(dock, W.scale(d, B + 1)))
            docks:field('yaw_c'):set(nd, rotation.c)
            docks:field('yaw_s'):set(nd, rotation.s)
            docks:field('standoff'):set(nd, standoff)
            docks:field('ray'):set(nd, i)
            docks:field('rotation'):set(nd, r)
            docks:field('first_probe'):set(nd, nv)
            local boat = F.new(dock, rotation.c, rotation.s)
            for _, q in ipairs(O.dock_offsets()) do probe(boat.point(q)) end
            nd = nd + 1
          end
        end
        for _, depth in ipairs(depths) do
          assert(nb < bodies:count(), 'outpost body capacity exceeded')
          -- Side: the lock stays at hatch height. Vertical: the shaft stays on the hatch axis.
          local front = pose == O.side and W.add(hit, W.scale(d, depth))
              or {x = hatch_line.x, y = hit.y + d.y * depth, z = hatch_line.z}
          if pose == O.side then front.y = hatch_line.y end
          local origin = W.sub(front, rotation.dir(O.entry_local(floors, pose)))
          local frame = F.new(origin, rotation.c, rotation.s)
          W.put(bodies:field('origin'), nb, origin)
          W.put(bodies:field('front'), nb, front)
          bodies:field('yaw_c'):set(nb, frame.c)
          bodies:field('yaw_s'):set(nb, frame.s)
          bodies:field('depth'):set(nb, depth)
          bodies:field('distance'):set(nb, distance)
          bodies:field('ray'):set(nb, i)
          bodies:field('rotation'):set(nb, r)
          bodies:field('pose'):set(nb, pose)
          bodies:field('first_probe'):set(nb, nv)
          for _, q in ipairs(O.body_offsets(floors)) do probe(frame.point(q)) end
          nb = nb + 1
        end
      end
    end
  end
  meta:field('docks'):set(0, nd)
  meta:field('bodies'):set(0, nb)
  meta:field('probes'):set(0, nv)
  meta:field('floors'):set(0, floors)
end
