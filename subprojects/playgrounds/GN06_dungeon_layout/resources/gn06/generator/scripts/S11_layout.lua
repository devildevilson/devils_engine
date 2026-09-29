-- Тело шага layout: ГДЕ ЧТО СТОИТ.
--
-- ЦИРКУЛЯЦИЯ КЛАДЁТСЯ ПЕРВОЙ. Это переворот по сравнению с норой, где коридор был СЛЕДСТВИЕМ связей:
-- в здании сначала известно, как по нему ходят, и уже под это нарезаны места. Отсюда и вид
-- настоящего этажа — длинная лента, вдоль которой рядами стоят места одной глубины.
--
-- Сколько лент положить, инструмент считает сам из глубины пятна и объявленных границ полосы:
-- «сколько лент» — это ответ, а не вопрос. Хребет (поперечная лента) появляется ровно тогда, когда
-- лент больше одной: одна связна сама по себе, а две без хребта пришлось бы связывать дверьми через
-- чужие комнаты.

return function(step)
  local p = step.params
  local zones = step.writes.zones
  local columns_list = step.writes.columns
  local state = step.writes.state

  -- УЧАСТОК ОБЪЯВЛЕН В МЕРЕ, А НЕ В КЛЕТКАХ, и растра здесь нет вовсе: ни одна строчка этого шага не
  -- знает, какой будет картинка и будет ли она. Участок — полоса улицы, пятно застройки, полоса
  -- двора; раздел знает, как делить ПЯТНО, и ничего не знает про то, что снаружи, поэтому ему
  -- сообщают начало пятна, а улицу и двор кладёт площадка.
  local plot_w = math.floor(p.plot_width)
  local plot_h = math.floor(p.plot_height)
  local street_depth = math.floor(p.street_depth)
  local yard_depth = math.floor(p.yard_depth)

  originator.slice_zones{
    outputs = { zones:field("rect"), zones:field("kind"), state:field("zones") },
    params = {
      origin_y = street_depth,
      width = plot_w, height = plot_h - street_depth - yard_depth,
      wall = p.wall, corridor = p.corridor,
      min_band = p.min_band, max_band = p.max_band,
      min_room = p.min_room, max_room = p.max_room,
      jitter = p.jitter, reserve_percent = p.reserve_percent,
      -- Номера видов, а не имена: инструмент про кухни ничего не знает и знать не должен. Вид,
      -- которым помечены места раздела, — обычные покои: решатель заменит его везде, кроме условий.
      corridor_kind = p.corridor_kind, room_kind = p.room_kind,
    },
  }

  -- УЛИЦА И ДВОР — ОБЫЧНЫЕ ЗОНЫ, и кладутся они в тот же список мест. Две записи это не «тяжёлые
  -- данные», а композиция шага — ровно то, для чего в теле есть lua. Наружная дверь после этого не
  -- требует ни строчки нового механизма: у неё с одной стороны улица, и правила видов решают
  -- остальное.
  local count = math.floor(state:field("zones"):get(0))
  local rect = zones:field("rect")
  local kind = zones:field("kind")
  local function place(x, y, w, h, zone_kind)
    rect:set(count, x, 0)
    rect:set(count, y, 1)
    rect:set(count, w, 2)
    rect:set(count, h, 3)
    kind:set(count, zone_kind)
    count = count + 1
  end
  place(0, 0, plot_w, street_depth, p.street_kind)
  place(0, plot_h - yard_depth, plot_w, yard_depth, p.yard_kind)
  state:field("zones"):set(0, count)

  -- ФОРМА ВЫБИРАЕТСЯ ПО РАЗМЕРУ, А НЕ ПО РОЛИ, и это не обход трудности: роль назначает решатель
  -- ПОСЛЕ того, как выведены стыки, а стык считается уже с учётом формы. Порядок «геометрия, потом
  -- программа» тут не выбран — он вынужден, и он же верный: округлым зал делает размер, а залом его
  -- делает соседство.
  --
  -- Циркуляцию не скругляют: по ней ходят, и прямая кромка коридора — это то, вдоль чего стоят
  -- двери. Улицу и двор тоже: у них форма участка, а не замысел.
  local shape = zones:field("shape")
  local big = math.floor(p.big_area)
  local round_min = math.floor(p.round_min)
  local corner_cut = math.floor(p.corner_cut)
  local min_span = math.floor(p.min_span)
  local columns = zones:field("columns")

  -- СРЕЗ НЕ ДОЛЖЕН СЪЕДАТЬ ДВЕРНОЙ ПРОЁМ, и это не осторожность, а найденный отказ: комната шириной
  -- в четыре меры со срезом в одну оставляет прямой кромки две, дверь в неё не ставится, и у
  -- комнаты не остаётся ни одного стыка. Генератор отказал и назвал зону — поэтому правило написано
  -- здесь, а не подобрано числом: кромки обязано хватить на проём.
  local function allowed_cut(w, h, wanted)
    local cut = math.max(wanted, 0)
    while cut > 0 and math.min(w, h) - 2 * cut < min_span do
      cut = cut - 1
    end
    return cut
  end

  -- УГОЛ ЗДАНИЯ СРЕЗАЮТ СИЛЬНЕЕ, чем угол комнаты, и это видно снаружи: угловое место, выходящее
  -- сразу на две наружные стены, получает большой срез. Мотив старый и узнаваемый — на этом срезе
  -- ставят башенку, эркер или ту же колонну.
  local wall_size = math.floor(p.wall)
  local inner_left = wall_size
  local inner_right = plot_w - wall_size
  local inner_top = street_depth + wall_size
  local inner_bottom = plot_h - yard_depth - wall_size

  for i = 0, count - 1 do
    local k = kind:get(i)
    local x, y = rect:get(i, 0), rect:get(i, 1)
    local w, h = rect:get(i, 2), rect:get(i, 3)
    local area = w * h
    local plain = k == p.corridor_kind or k == p.street_kind or k == p.yard_kind
    local on_side = (x == inner_left) or (x + w == inner_right)
    local on_end = (y == inner_top) or (y + h == inner_bottom)
    local corner_of_house = on_side and on_end

    if plain then
      shape:set(i, 0, 0)
    elseif area >= big and math.min(w, h) >= round_min then
      -- ОКРУГЛЫЙ ЗАЛ. Прямой кромки у эллипса остаётся немного, и вся она посередине стороны —
      -- поэтому нижняя граница стороны объявлена: на кромке стоят двери.
      shape:set(i, 3, 0)
    elseif area >= big then
      shape:set(i, 1, 0)
      shape:set(i, allowed_cut(w, h, math.max(math.floor(math.min(w, h) / 4), 1)), 1)
    else
      -- Срез в одну меру поодиночке незаметен, а на перекрестьях, где сходятся четыре комнаты,
      -- четыре среза оставляют стенной столб — ту самую колонну, в которую упираются стены.
      shape:set(i, 1, 0)
      shape:set(i, allowed_cut(w, h, corner_cut), 1)
    end

    -- Колоннада — только крупным местам: в каморке колонна не замысел, а препятствие.
    columns:set(i, (not plain and area >= big) and 1 or 0)
  end

  -- КОЛОННА — ЭТО КУСОК СТЕНЫ, стоящий посреди зала, поэтому она не зона: по ней не ходят, связей у
  -- неё нет и вид ей не нужен. Считается она в мере и отдельным списком прямоугольников — прежде
  -- колоннада писалась прямо в растр, и это была последняя ниточка, которой план держался за
  -- картинку: посчитанная на картинке, она поехала бы при смене масштаба.
  originator.place_columns{
    inputs = { zones:field("rect"), state:field("zones"), shape, columns },
    outputs = { columns_list:field("rect"), state:field("columns") },
    params = { spacing = p.column_spacing, size = p.column_size, margin = p.column_margin },
  }
end
