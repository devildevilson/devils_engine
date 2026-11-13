-- Core in rock (stage 4 of NEXT_OUTPOST_PLAN.md): an entry hall, a spine of corridor segments with
-- rooms along both sides, short branches from the hall sides, a shaft to a second spine one deck up
-- or down and shifted sideways.
-- The plan is fixed by the seed; the rooms step probes every option and keeps those that fit the rock.
-- Candidates come in acceptance order; each names its parent and lists options from best to fallback.
local C = {}

function C.plan(pose, b, random)
  local O = modules.outpost
  local u, deck = O.cell()/2, O.deck()
  local seg = 2*u
  local list = {}
  local function add(id, parent, label, min_rock, options, required)
    list[#list+1] = {id = id, parent = parent, label = label, min_rock = min_rock, options = options, required = required}
  end
  -- Shape of this core, all from the seed: hall size, spine lengths, shaft places, room habits.
  local function pick(salt, low, high) return low+math.floor(random(salt)*(high-low+1)) end
  local shape = {
    hall_x = pick(990, 3, 5)*u/2, hall_z = pick(991, 3, 4)*u/2,
    a = pick(992, 3, 5), b = pick(993, 2, 4), e = pick(994, 1, 3), c = pick(995, 2, 3),
    d = pick(996, 1, 3), f = pick(997, 1, 3), g = pick(998, 2, 3),
    skip = 0.1+random(999)*0.2, long = 0.15+random(1001)*0.3, tall = 0.1+random(1002)*0.3,
    third = random(1003)<0.4,
  }
  shape.k = pick(1004, 0, shape.a-2)
  -- Entry hall: one deck, entered through its near face (side) or its floor/roof (vertical).
  local hall_h = {x = shape.hall_x, y = deck/2, z = shape.hall_z}
  local hall_c, s, ss
  if pose == O.side then
    hall_c, s, ss = {x = b.x, y = b.y, z = b.z+hall_h.z}, 'z', 1
  else
    hall_c = {x = b.x, y = b.y+(pose == O.above and 1 or -1)*deck/2, z = b.z}
    local k = math.floor(random(980)*4)
    s, ss = k<2 and 'z' or 'x', k%2 == 0 and 1 or -1
  end
  local l = s == 'z' and 'x' or 'z'
  local floor = hall_c.y-deck/2
  -- Box by distance along a spine axis from a start face, lateral offset from the hall centre, deck floors.
  local function box(sa, la, start, dir, a0, a1, l0, l1, d0, d1)
    local c, h = {}, {}
    c[sa], h[sa] = start+dir*(a0+a1)/2, (a1-a0)/2
    c[la], h[la] = hall_c[la]+(l0+l1)/2, math.abs(l1-l0)/2
    c.y, h.y = floor+(d0+d1)/2*deck, (d1-d0)/2*deck
    return {c = c, h = h}
  end
  add('hall', 'tail', O.core, 0, {{c = hall_c, h = hall_h}}, true)

  -- Second deck: towards the rock the placement search found (up for side and above, down for below).
  local up = pose == O.below and -1 or 1
  local shaft_side = random(981)<0.5 and -1 or 1
  local spines = {}
  local function spine(prefix, parent, start, dir, count, lateral, d0, sa, la)
    sa, la = sa or s, la or l
    local ids = {}
    for i = 0, count-1 do
      local id = prefix..i
      add(id, i == 0 and parent or prefix..(i-1), O.core, 9, {box(sa, la, start, dir, i*seg, (i+1)*seg, lateral-u/2, lateral+u/2, d0, d0+1)})
      ids[#ids+1] = {id = id, a0 = i*seg, start = start, dir = dir, lateral = lateral, d0 = d0, sa = sa, la = la}
    end
    spines[#spines+1] = ids
    return ids
  end
  local front = hall_c[s]+ss*hall_h[s]
  local a = spine('a', 'hall', front, ss, shape.a, 0, 0)
  if pose ~= O.side then spine('c', 'hall', hall_c[s]-ss*hall_h[s], -ss, shape.c, 0, 0) end
  -- Branches from the side faces of the hall: the core can go round a pocket of water behind it.
  spine('d', 'hall', hall_c[l]+hall_h[l], 1, shape.d, 0, 0, l, s)
  spine('f', 'hall', hall_c[l]-hall_h[l], -1, shape.f, 0, 0, l, s)
  -- Shaft beside segment k of spine a, spanning two decks; spine b starts beyond it, spine e runs
  -- back from the same face (its first segment hangs off b0).
  local k = shape.k
  local function shaft(id, parent, at, lateral, d0, d1)
    add(id, parent, O.passage, 9, {box(s, l, front, ss, at*seg+seg/2-u/2, at*seg+seg/2+u/2, lateral-u/2, lateral+u/2, d0, d1)})
  end
  shaft('shaft', a[k+1].id, k, shaft_side*u, math.min(0, up), math.max(1, up+1))
  local b_lateral = shaft_side*2*u
  local forward = spine('b', 'shaft', front+ss*k*seg, ss, shape.b, b_lateral, up)
  spine('e', 'b0', front+ss*k*seg, -ss, shape.e, b_lateral, up)
  -- Sometimes a third deck: another shaft on the outer side of spine b and spine g beyond it.
  local k2
  if shape.third and shape.b>=2 then
    k2 = 1
    shaft('shaft2', forward[k2+1].id, k+k2, shaft_side*3*u, math.min(up, 2*up), math.max(up, 2*up)+1)
    spine('g', 'shaft2', front+ss*(k+k2)*seg, ss, shape.g, shaft_side*4*u, 2*up)
  end
  -- Rooms on both sides of every segment: some skipped, some two segments long, some two decks tall.
  local salt = 2000
  local function rnd() salt = salt+1 return random(salt) end
  local taken = {}
  for _, ids in ipairs(spines) do
    for i, segment in ipairs(ids) do
      for _, side in ipairs({-1, 1}) do
        local slot = segment.id..':'..side
        local blocked = taken[slot] or (segment.id == 'a'..k and side == shaft_side) or (segment.id == 'b0' and side == -shaft_side)
          or (k2 and segment.id == 'b'..k2 and side == shaft_side) or (segment.id == 'g0' and side == -shaft_side)
        if not blocked and rnd()>shape.skip then
          local long = i<#ids and rnd()<shape.long
          local length = long and 2*seg or seg
          if long then taken[ids[i+1].id..':'..side] = true end
          local tall = rnd()<shape.tall
          local options = {}
          local function option(depth, decks)
            local d0 = segment.d0
            local d1 = d0+decks
            if decks == 2 and up<0 then d0, d1 = d0-1, d0+1 end
            local near, far = segment.lateral+side*u/2, segment.lateral+side*(u/2+depth)
            options[#options+1] = box(segment.sa, segment.la, segment.start, segment.dir, segment.a0, segment.a0+length, near, far, d0, d1)
          end
          if tall then option(3*u, 2) end
          option(3*u, 1)
          option(2*u, 1)
          if long then
            length = seg
            option(2*u, 1)
          end
          add(segment.id..':'..side, segment.id, O.core, 12, options)
        end
      end
    end
  end
  return list
end

return C
