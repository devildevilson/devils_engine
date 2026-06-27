-- Минимальный entry-point интерфейса visage.
-- visage::system грузит этот файл в песочнице env и вызывает возвращённую функцию каждый кадр.
-- На вход приходит time (игровое время кадра). Доступен только биндинг nk (Nuklear) + базовые функции.

return function(time)
  local flags = nk.panel_flags.border | nk.panel_flags.title | nk.panel_flags.movable
  if nk.begin("hello", {50, 50, 300, 200}, flags) then
    -- label — обычная функция, гоняет путь измерения/раскладки текста (шрифт)
    -- label — обычная функция; flags = выравнивание текста (nk.text_align)
    nk.layout.row_dynamic(28, 1)
    nk.label("tile_frontier UI", nil, nk.text_align.left)

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
