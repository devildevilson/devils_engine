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

  local stats_flags = nk.panel_flags.border | nk.panel_flags.title | nk.panel_flags.movable
  if nk.begin("actor stats", {980, 48, 250, 176}, stats_flags) then
    nk.push_font(14)
    nk.layout.row_dynamic(20, 1)
    nk.label(string.format("actors: %.0f", tf_actor_count or 0), nil, nk.text_align.left)
    nk.label(string.format("instances: %.0f", tf_actor_instances or 0), nil, nk.text_align.left)
    nk.label(string.format("intents: %.0f", tf_actor_intents or 0), nil, nk.text_align.left)
    nk.label(string.format("main fps: %.1f", tf_main_fps or 0), nil, nk.text_align.left)
    nk.label(string.format("intent/s: %.0f", tf_intents_per_sec or 0), nil, nk.text_align.left)
    nk.label(string.format("actor update: %.1f us", tf_actor_update_avg_us or 0), nil, nk.text_align.left)
    nk.pop_font()
  end
  nk.endf()
end
