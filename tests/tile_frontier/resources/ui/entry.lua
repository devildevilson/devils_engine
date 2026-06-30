-- Минимальный entry-point интерфейса visage.
-- visage::system грузит этот файл в песочнице env и вызывает возвращённую функцию каждый кадр.
-- На вход приходит time (игровое время кадра). Доступен только биндинг nk (Nuklear) + базовые функции.

-- Состояние плеера живёт в upvalue (между кадрами): движок отдаёт только примитивы
-- play(+start)/state/stop, а очередь/повтор плеер держит сам на lua.
local player = { handle = nil, loop = false }

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

    -- UI-звук: presentation→sound напрямую. app.play_sound возвращает непрозрачный
    -- sound_handle (без арифметики); его можно передать вторым аргументом как after
    -- для секвенсинга (играть после другой задачи).
    nk.layout.row_dynamic(30, 1)
    if nk.button("Play sound") then
      app.play_sound("eating")
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

  -- Демо-плеер эмбиента: весь API звука (play+start, state, stop). Очередь/повтор — на lua.
  local pflags = nk.panel_flags.border | nk.panel_flags.title | nk.panel_flags.movable
  if nk.begin("sound player", {50, 360, 340, 150}, pflags) then
    nk.layout.row_dynamic(28, 3)
    if nk.button("Play") then
      if player.handle then app.stop_sound(player.handle) end
      player.handle = app.play_sound("ambient")
    end
    if nk.button("From 50%") then
      if player.handle then app.stop_sound(player.handle) end
      -- форма вызова одним аргументом-таблицей (в будущем name станет handle ресурса demiurg)
      player.handle = app.play_sound{ name = "ambient", start = 0.5 }
    end
    if nk.button("Stop") then
      if player.handle then app.stop_sound(player.handle) end
      player.handle = nil
    end

    nk.layout.row_dynamic(26, 1)
    if nk.button(player.loop and "loop: ON" or "loop: off") then
      player.loop = not player.loop
    end

    -- индикатор + очередь силами lua: app.sound_state → progress(0..1) или nil (задачи уже нет)
    nk.layout.row_dynamic(20, 1)
    if player.handle then
      local progress = app.sound_state(player.handle)
      if progress then
        nk.label(string.format("playing  %.0f%%", progress * 100), nil, nk.text_align.left)
      elseif player.loop then
        player.handle = app.play_sound("ambient") -- трек кончился → следующий (тут была бы очередь)
      else
        player.handle = nil
        nk.label("finished", nil, nk.text_align.left)
      end
    else
      nk.label("idle", nil, nk.text_align.left)
    end
  end
  nk.endf()
end
