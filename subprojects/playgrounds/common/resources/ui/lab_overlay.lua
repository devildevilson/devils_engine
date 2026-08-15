local styled = false

return function()
  if not styled then
    nk.style_from_table({
      window = {0.055, 0.065, 0.085, 0.88},
      header = {0.09, 0.11, 0.15, 0.94},
      text = {0.90, 0.93, 0.98, 1.0},
      border = {0.24, 0.30, 0.40, 0.95},
    })
    styled = true
  end

  local flags = nk.panel_flags.border | nk.panel_flags.title |
                nk.panel_flags.no_scrollbar | nk.panel_flags.no_input
  if nk.begin_titled("playground_overlay", playground_title or "playground", {16, 16, 430, 112}, flags) then
    nk.push_font(14)
    nk.layout.row_dynamic(19, 1)
    nk.label(playground_scene or "", nil, nk.text_align.left)
    nk.label(playground_controls or "", nil, nk.text_align.left)
    nk.label(string.format("%.1f FPS   %.2f ms", playground_fps or 0, playground_frame_ms or 0), nil, nk.text_align.left)
    nk.pop_font()
  end
  nk.fin()
end
