local styled = false

-- Срез 2 показывает небо и ничего не настраивает мышью: все рычаги пока идут через CLI и клавиши,
-- поэтому панель здесь ровно одна и она диагностическая. Слайдеры вернутся в срезе 3 вместе с
-- экспозицией, где крутить действительно есть что.

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
  if nk.begin_titled("pf07_diagnostics", playground_title or "PF07", {16, 16, 520, diagnostic_height}, diagnostic_flags) then
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
