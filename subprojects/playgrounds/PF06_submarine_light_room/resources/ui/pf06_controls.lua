local styled = false

local function slider_row(label, value, min_value, max_value, step)
  nk.layout.row_dynamic(18, 1)
  nk.label(string.format("%s   %.3f", label, value), nil, nk.text_align.left)
  nk.layout.row_dynamic(20, 1)
  return nk.slider(min_value, value, max_value, step)
end

return function()
  if not styled then
    nk.style_from_table({
      window = {0.045, 0.055, 0.072, 0.91},
      header = {0.085, 0.105, 0.14, 0.97},
      text = {0.90, 0.93, 0.98, 1.0},
      border = {0.24, 0.32, 0.43, 0.98},
      button = {0.11, 0.15, 0.20, 1.0},
      button_hover = {0.17, 0.24, 0.32, 1.0},
      button_active = {0.20, 0.34, 0.43, 1.0},
      slider = {0.07, 0.09, 0.12, 1.0},
      slider_cursor = {0.20, 0.48, 0.58, 1.0},
      slider_cursor_hover = {0.28, 0.63, 0.73, 1.0},
      slider_cursor_active = {0.38, 0.76, 0.84, 1.0},
    })
    styled = true
  end

  local diagnostic_flags = nk.panel_flags.border | nk.panel_flags.title |
                           nk.panel_flags.no_scrollbar | nk.panel_flags.no_input
  local detail_count = math.min(playground_detail_count or 0, 14)
  local detail_padding = detail_count > 0 and 20 or 0
  local diagnostic_height = 112 + detail_count * 23 + detail_padding
  if nk.begin_titled("pf06_diagnostics", playground_title or "PF06", {16, 16, 520, diagnostic_height}, diagnostic_flags) then
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

  local control_flags = nk.panel_flags.border | nk.panel_flags.title | nk.panel_flags.no_scrollbar
  if nk.begin_titled("pf06_tuning", "PF06 atmosphere tuning", {552, 16, 390, 286}, control_flags) then
    nk.push_font(14)
    pf06_gi = slider_row("Exploration GI", pf06_gi or 0.23, 0.0, 1.0, 0.005)
    pf06_left_source = slider_row("Left situational light", pf06_left_source or 1.0, 0.0, 3.0, 0.02)
    pf06_medium_density = slider_row("Atmosphere density", pf06_medium_density or 0.14, 0.0, 0.80, 0.005)
    pf06_pattern_contrast = slider_row("Volumetric pattern contrast", pf06_pattern_contrast or 1.55, 0.0, 2.0, 0.01)

    nk.layout.row_dynamic(27, 2)
    if nk.button("Reset defaults") then
      pf06_gi = 0.23
      pf06_left_source = 1.0
      pf06_medium_density = 0.14
      pf06_pattern_contrast = 1.55
    end
    if nk.button("Hide UI (U restores)") then
      pf06_hide_requested = true
    end
    nk.pop_font()
  end
  nk.fin()
end
