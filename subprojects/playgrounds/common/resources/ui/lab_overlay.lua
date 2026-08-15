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
  local detail_count = math.min(playground_detail_count or 0, 10)
  local detail_padding = detail_count > 0 and 20 or 0
  local panel_height = 112 + detail_count * 23 + detail_padding
  if nk.begin_titled("playground_overlay", playground_title or "playground", {16, 16, 520, panel_height}, flags) then
    nk.push_font(14)
    nk.layout.row_dynamic(19, 1)
    nk.label(playground_scene or "", nil, nk.text_align.left)
    nk.label(playground_controls or "", nil, nk.text_align.left)
    nk.label(string.format("%.1f FPS   %.2f ms", playground_fps or 0, playground_frame_ms or 0), nil, nk.text_align.left)
    for i = 1, detail_count do
      nk.label((playground_details and playground_details[i]) or "", nil, nk.text_align.left)
    end
    nk.pop_font()
  end
  nk.fin()
end
