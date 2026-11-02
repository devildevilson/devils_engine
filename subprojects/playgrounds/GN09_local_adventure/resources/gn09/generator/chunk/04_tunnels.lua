-- Туннели якорей чанка без повторов: у обоих якорей пары один и тот же первый якорь.

return function(step)
  local out, made = step.writes.chunk_tunnels, 0
  local field = out:field("tunnel")
  local seen = {}
  for _, link in ipairs(step.links.links) do
    local l = link.tunnel_link
    if l:field("mutual"):get(0) ~= 0 then
      local first = l:field("first_station")
      local words = { first:get(0, 0), first:get(0, 1), first:get(0, 2) }
      local id = words[1] .. ":" .. words[2] .. ":" .. words[3]
      if not seen[id] then
        seen[id] = true
        if made >= out:count() then error("chunk_tunnels: capacity exceeded") end
        for w = 1, 3 do field:set(made, words[w], w - 1) end
        made = made + 1
      end
    end
  end
  step.writes.tunnel_counts:field("tunnels"):set(0, made)
end
