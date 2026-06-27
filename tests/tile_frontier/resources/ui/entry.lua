-- Минимальный entry-point интерфейса visage.
-- visage::system грузит этот файл в песочнице env и вызывает возвращённую функцию каждый кадр.
-- На вход приходит time (игровое время кадра). Доступен только биндинг nk (Nuklear) + базовые функции.

return function(time)
  local flags = nk.panel_flags.border | nk.panel_flags.title | nk.panel_flags.movable
  if nk.begin("hello", {50, 50, 340, 280}, flags) then
    -- label — обычная функция; flags = выравнивание текста (nk.text_align)
    nk.layout.row_dynamic(28, 1)
    nk.label("tile_frontier UI", nil, nk.text_align.left)

    -- размер + SDF-эффекты: push_font({size=, bold=, outline={color={r,g,b,a}, width=}, softness=})
    nk.push_font({size = 34, bold = 0.1, outline = {color = {0.0, 0.0, 0.0, 1.0}, width = 0.15}})
    nk.layout.row_dynamic(42, 1)
    nk.label("BIG outline", nil, nk.text_align.left)
    nk.pop_font()

    nk.push_font({size = 22, bold = 0.12})
    nk.layout.row_dynamic(28, 1)
    nk.label("bold 22px", nil, nk.text_align.left)
    nk.pop_font()

    nk.push_font(12)
    nk.layout.row_dynamic(18, 1)
    nk.label("small 12px text", nil, nk.text_align.left)
    nk.pop_font()

    nk.layout.row_dynamic(30, 1)
    if nk.button("Click me") then
      print("visage: button clicked")
    end

    nk.layout.row_dynamic(30, 1)
    if nk.button("And me") then
      print("visage: second button clicked")
    end
  end
  -- nk.end должен вызываться всегда, даже если begin вернул false (требование Nuklear)
  nk.endf()
end
