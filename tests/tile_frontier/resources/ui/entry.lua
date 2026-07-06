-- Минимальный entry-point интерфейса visage.
-- visage::system грузит этот файл в песочнице env и вызывает возвращённую функцию каждый кадр.
-- На вход приходит time (игровое время кадра). Доступен биндинг nk (Nuklear) + хост-namespace app.
--
-- Экраны выбираются по app.state() (FSM движка): boot → loading → game.

-- Состояние плеера живёт в upvalue (между кадрами): движок отдаёт только примитивы
-- play(+start)/state/stop, а очередь/повтор плеер держит сам на lua.
local player = { handle = nil, loop = false }
local styled = false
local resources = {
  grass = request("textures/grass"),
  grad2 = request("textures/grad2"),
  quad = request("textures/quad"),
  eating = request("sounds/eating/freesound_community-chomp-chew-bite-102031"),
  ambient = request("sounds/ambient/soundreality-ambient-spring-forest-323801"),
}

-- Тема Nuklear (шаг 2c). Применяем один раз: nk.style_from_table задаёт цвета во все под-стили.
local function apply_theme()
  nk.style_from_table({
    window        = {0.10, 0.11, 0.14, 1.0},
    header        = {0.16, 0.18, 0.24, 1.0},
    text          = {0.86, 0.88, 0.92, 1.0},
    border        = {0.30, 0.34, 0.44, 1.0},
    button        = {0.20, 0.24, 0.34, 1.0},
    button_hover  = {0.28, 0.34, 0.48, 1.0},
    button_active = {0.16, 0.20, 0.30, 1.0},
    slider_cursor = {0.40, 0.60, 0.90, 1.0},
  })
end

-- boot: движок только стартует. Шрифт ещё мог не доехать на GPU, поэтому крупный текст даём
-- вторым (italic) шрифтом лишь как «лого»; главное — прикрыть экран панелью.
local function splash_screen()
  local flags = nk.panel_flags.border | nk.panel_flags.title
  if nk.begin("splash", {440, 280, 400, 150}, flags) then
    nk.push_font({font = "italic", size = 40, bold = 0.08})
    nk.layout.row_dynamic(52, 1)
    nk.label("devils_engine", nil, nk.text_align.centered)
    nk.pop_font()
    nk.layout.row_dynamic(22, 1)
    nk.label("starting up...", nil, nk.text_align.centered)
  end
  nk.fin()
end

-- loading: ассеты тянут стартовый набор. Прогресс — из app.loading_progress() [0,1].
local function loading_screen()
  local flags = nk.panel_flags.border | nk.panel_flags.title
  if nk.begin("loading", {420, 300, 440, 160}, flags) then
    nk.push_font({font = "italic", size = 30})
    nk.layout.row_dynamic(40, 1)
    nk.label("Loading resources", nil, nk.text_align.centered)
    nk.pop_font()

    local p = app.loading_progress()
    nk.layout.row_dynamic(24, 1)
    nk.progress(math.floor(p * 100 + 0.5), 100, false)
    nk.layout.row_dynamic(20, 1)
    nk.label(string.format("%.0f%%", p * 100), nil, nk.text_align.centered)
  end
  nk.fin()
end

-- game: обычный игровой UI (демо шрифтов/эффектов + звук + статистика + управление игрой).
-- rng — непрозрачный rng_state (сид кадра от хоста), timestamp — монотонная метка времени.
local function game_ui(time, timestamp, rng)
  local flags = nk.panel_flags.border | nk.panel_flags.title | nk.panel_flags.movable
  if nk.begin("hello", {50, 50, 340, 372}, flags) then
    nk.layout.row_dynamic(28, 1)
    nk.label("tile_frontier UI", nil, nk.text_align.left)

    -- демо prng: value(rng)->[0,1]; следующее состояние prng64(rng); value(s,n,m)->[n,m];
    -- микс двух состояний через '+' (rng + base.rng(42)). Случайность отвязана от математики.
    local a = base.value(rng)
    local roll = base.value(base.prng64(rng), 1, 6)
    local mixed = base.value(rng + base.rng(42))
    nk.layout.row_dynamic(18, 1)
    nk.label(string.format("rng %.3f  d6=%d  mix %.3f  t=%d", a, roll, mixed, timestamp), nil, nk.text_align.left)

    -- размер + SDF-эффекты: push_font({size=, bold=, outline={color={r,g,b,a}, width=}, softness=})
    nk.push_font({size = 34, bold = 0.1, outline = {color = {0.0, 0.0, 0.0, 1.0}, width = 0.15}})
    nk.layout.row_dynamic(42, 1)
    nk.label("BIG outline", nil, nk.text_align.left)
    nk.pop_font()

    -- второй шрифт (шаг 2b): выбираем базовый шрифт по имени "italic"
    nk.push_font({font = "italic", size = 26})
    nk.layout.row_dynamic(32, 1)
    nk.label("italic font", nil, nk.text_align.left)
    nk.pop_font()

    nk.push_font(12)
    nk.layout.row_dynamic(18, 1)
    nk.label("small 12px text", nil, nk.text_align.left)
    nk.pop_font()

    -- картинка: вписать с сохранением пропорций и отцентровать (шаг image + placement)
    -- + вторая копия с зеркальным флипом по u (mirror закодирован в id текстуры)
    local img = app.image(resources.grass)
    if img then
      nk.layout.row_dynamic(64, 2)
      nk.image(img, nk.placement.scale_ratio | nk.placement.center)
      nk.image(img, nk.placement.scale_ratio | nk.placement.center | nk.placement.mirror_u)
    end

    -- Стадия 2: cooldown (заливка по градиент-маске) + 4-blend (смешение по каналам quad-маски)
    local grad = app.image(resources.grad2)
    local quad = app.image(resources.quad)
    if img and grad then
      nk.layout.row_dynamic(64, 1)
      nk.image_gradient{ img = img, mask = grad, fill = 0.5 }
    end
    if quad then
      nk.layout.row_dynamic(64, 1)
      nk.image_mix{ comps = { {1,0,0,1}, {0,1,0,1}, {0,0,1,1}, {1,1,0,1} }, mask = quad }
    end

    nk.layout.row_dynamic(30, 1)
    if nk.button("Play sound") then
      app.play_sound(resources.eating)
    end

    -- управление игрой (шаг 2a/1f): полноэкранный режим + выход
    nk.layout.row_dynamic(30, 2)
    if nk.button(app.is_fullscreen() and "Windowed" or "Fullscreen") then
      app.set_fullscreen(not app.is_fullscreen())
    end
    if nk.button("Quit") then
      app.quit_game()
    end
  end
  nk.fin()

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
  nk.fin()

  -- perf-график фаз апдейта актора (catalogue::statistics_introspection). app.perf_stats() отдаёт
  -- { name, avg, min, max, last, count, samples={...} }; samples — последние замеры для nk.plot.
  local perf = app.perf_stats()
  table.sort(perf, function(a, b) return a.name < b.name end) -- стабильный порядок (hash_map иначе прыгает)
  local perf_flags = nk.panel_flags.border | nk.panel_flags.title | nk.panel_flags.movable | nk.panel_flags.scalable
  if nk.begin("perf (us)", {980, 236, 300, 360}, perf_flags) then
    nk.push_font(12)
    if #perf == 0 then
      nk.layout.row_dynamic(18, 1)
      nk.label("no samples yet", nil, nk.text_align.left)
    else
      local prev_markers = nk.chart.show_markers(false) -- чистые линии без точек-маркеров
      for _, e in ipairs(perf) do
        nk.layout.row_dynamic(16, 1)
        nk.label(string.format("%s  avg %.1f / max %.0f", e.name, e.avg, e.max), nil, nk.text_align.left)
        if #e.samples > 0 then
          nk.layout.row_dynamic(38, 1)
          nk.plot(nk.chart_type.lines, e.samples, #e.samples, 0) -- nk.plot сам масштабирует по значениям
        end
      end
      nk.chart.show_markers(prev_markers) -- восстановить для остального UI
    end
    nk.pop_font()
  end
  nk.fin()

  -- Демо-плеер эмбиента: весь API звука (play+start, state, stop). Очередь/повтор — на lua.
  local pflags = nk.panel_flags.border | nk.panel_flags.title | nk.panel_flags.movable
  if nk.begin("sound player", {50, 410, 340, 150}, pflags) then
    nk.layout.row_dynamic(28, 3)
    if nk.button("Play") then
      if player.handle then app.stop_sound(player.handle) end
      player.handle = app.play_sound(resources.ambient)
    end
    if nk.button("From 50%") then
      if player.handle then app.stop_sound(player.handle) end
      player.handle = app.play_sound{ resource = resources.ambient, start = 0.5 }
    end
    if nk.button("Stop") then
      if player.handle then app.stop_sound(player.handle) end
      player.handle = nil
    end

    nk.layout.row_dynamic(26, 1)
    if nk.button(player.loop and "loop: ON" or "loop: off") then
      player.loop = not player.loop
    end

    nk.layout.row_dynamic(20, 1)
    if player.handle then
      local progress = app.sound_state(player.handle)
      if progress then
        nk.label(string.format("playing  %.0f%%", progress * 100), nil, nk.text_align.left)
      elseif player.loop then
        player.handle = app.play_sound(resources.ambient)
      else
        player.handle = nil
        nk.label("finished", nil, nk.text_align.left)
      end
    else
      nk.label("idle", nil, nk.text_align.left)
    end
  end
  nk.fin()
end

-- entry получает (time, timestamp, rng_state): time — длительность кадра; timestamp — монотонная
-- метка времени (для фиксации начала UI-анимаций); rng — сид псевдослучайности этого кадра.
return function(time, timestamp, rng)
  if not styled then apply_theme(); styled = true end

  -- Esc → выход (именованное действие "quit", шаг 2d). Клик = завершённое нажатие в этом кадре.
  if app.action_clicked("quit") then app.quit_game() end

  local state = app.state()
  if state == "boot" then
    splash_screen()
  elseif state == "loading" then
    loading_screen()
  else
    game_ui(time, timestamp, rng)
  end
end
