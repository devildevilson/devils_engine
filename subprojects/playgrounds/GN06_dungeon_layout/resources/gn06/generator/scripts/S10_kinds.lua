-- Тело шага kinds: ЧТО БЫВАЕТ В ЗДАНИИ.
--
-- Единственное место, где объявлены виды зон. Вид — это не украшение и не подпись на карте: он
-- несёт ПРОХОДИМОСТЬ, ПРЕДЕЛ ЧИСЛА СВЯЗЕЙ и обе матрицы — с кем вправе граничить и с кем вправе
-- соединяться дверью. Всё, чем раньше отличались «комната» и «коридор», лежит теперь здесь.
--
-- МАТРИЦЫ ДВЕ, И ЭТО ГЛАВНОЕ РЕШЕНИЕ. Смежность и связь — разные утверждения: у кухни с покоями
-- общая стена бывает сплошь и рядом, а двери между ними быть не должно. Слив их в одну таблицу,
-- пришлось бы выбрать, какое из двух утверждений потерять.
--
-- ВЕС НОЛЬ значит «этот вид никогда не выбирается решателем». Стена и дверь появляются не по выбору
-- (стена — это дополнение, дверь — решение шага doors), а коридор и зал ставятся УСЛОВИЕМ: заранее
-- занятая зона это условие, а не выбор, и её вид ставится независимо от веса.

-- `glyph` — буква для карты в терминале. Она здесь, а не в коде площадки, по той же причине, по
-- которой здесь цвет: имя вида объявлено ОДИН раз, и площадка его читает, а не повторяет.
local kinds = {
  { name = "wall",     glyph = "#", passable = 0, max_links = 0, weight = 0.0, colour = 0x1a1c22 },
  { name = "corridor", glyph = "+", passable = 1, max_links = 0, weight = 0.0, colour = 0xb9a67e },
  { name = "hall",     glyph = "H", passable = 1, max_links = 4, weight = 0.0, colour = 0x9d6b4f },
  { name = "kitchen",  glyph = "K", passable = 1, max_links = 3, weight = 0.0, colour = 0x7d9b5a },
  { name = "chamber",  glyph = ".", passable = 1, max_links = 2, weight = 2.0, colour = 0x5b7fa6 },
  { name = "store",    glyph = "s", passable = 1, max_links = 1, weight = 0.9, colour = 0x6f6a60 },
  { name = "door",     glyph = "o", passable = 1, max_links = 2, weight = 0.0, colour = 0xe4d9b0 },
  -- УЛИЦА И ДВОР — ТАКИЕ ЖЕ МЕСТА. Ровно поэтому наружная дверь не требует отдельного механизма: это
  -- обычная дверь, у которой с одной стороны улица. Предела связей у них нет (дом не один), а вес
  -- нулевой: их ставит площадка условием, выбирать их решателю незачем.
  { name = "street",   glyph = ",", passable = 1, max_links = 0, weight = 0.0, colour = 0x55565a },
  { name = "yard",     glyph = ":", passable = 1, max_links = 0, weight = 0.0, colour = 0x4d4128 },
  -- ДВОР (атриум) — крупное место, которое не стали резать. Оно внутри дома, и в этом вся разница с
  -- улицей: на него выходят двери комнат, а не наружные.
  { name = "court",    glyph = "\"", passable = 1, max_links = 0, weight = 0.0, colour = 0x6b7a52 },
}

-- Номера видов по имени: ниже они называются словами, а в буферах живут числами.
local id = {}
for i, kind in ipairs(kinds) do id[kind.name] = i - 1 end

-- КТО С КЕМ ВПРАВЕ ГРАНИЧИТЬ. Читает решатель, когда раскладывает виды по зонам.
local adjacent = {
  corridor = { "corridor", "hall", "kitchen", "chamber", "store", "street", "yard", "court" },
  hall     = { "hall", "corridor", "kitchen", "chamber", "street", "yard", "court" },
  kitchen  = { "kitchen", "corridor", "hall", "store", "street", "yard", "court" },
  chamber  = { "chamber", "corridor", "hall", "store", "street", "yard", "court" },
  store    = { "store", "corridor", "kitchen", "chamber", "street", "yard", "court" },
  court    = { "court", "corridor", "hall", "kitchen", "chamber", "store", "street", "yard" },
}

-- МЕЖДУ КЕМ ДОПУСТИМА ДВЕРЬ. Подмножество смежности, и разница — содержательная: кухня граничит с
-- покоями стеной, но выходит дверью только в зал, в коридор и в свою кладовую.
local doors = {
  corridor = { "corridor", "hall", "kitchen", "chamber", "store", "court" },
  hall     = { "corridor", "kitchen", "chamber", "street", "court" },
  kitchen  = { "corridor", "hall", "store", "yard", "court" },
  chamber  = { "corridor", "hall", "court" },
  store    = { "corridor", "kitchen", "yard" },
  court    = { "corridor", "hall", "kitchen", "chamber" },
}

-- ОБЯЗАТЕЛЬНЫЕ ДВЕРИ. Третья таблица, и она говорит то, чего запретом не сказать: «парадный вход
-- ведёт в зал», «служебный — в кухню». Запрет отвечает на вопрос «чего НЕ бывает», а это требование,
-- и без него оба входа зависели бы от того, какой стык окажется длиннее.
--
-- Читается так: КАЖДЫЙ стык между этими видами становится дверью. Поэтому пара обязана быть
-- подмножеством допустимого, и инструмент проверяет это до работы.
local required = {
  street = { "hall" },
  yard   = { "kitchen" },
}

return function(step)
  local p = step.params
  local table_buffer = step.writes.kinds

  -- НОМЕРА ВИДОВ ОБЪЯВЛЕНЫ В КОНФИГЕ: по ним инструменты получают параметры, по ним же площадка
  -- читает результат. Сверка по ИМЕНИ, а не по числу строк: перестановка строки в таблице иначе
  -- тихо превратила бы кухню в кладовую, и заметить это можно было бы только глазами на карте.
  for name, index in pairs({ wall = p.wall_kind, corridor = p.corridor_kind, hall = p.hall_kind,
                             kitchen = p.kitchen_kind, chamber = p.room_kind, store = p.store_kind,
                             door = p.door_kind, street = p.street_kind, yard = p.yard_kind,
                             court = p.court_kind }) do
    local declared = math.floor(index)
    if kinds[declared + 1] == nil or kinds[declared + 1].name ~= name then
      error(string.format("kinds: the config declares '%s' as kind %d and the table holds '%s' there",
                          name, declared, kinds[declared + 1] and kinds[declared + 1].name or "nothing"))
    end
  end

  local count = math.tointeger(p.kind_count) or math.floor(p.kind_count)
  if #kinds ~= count then
    error(string.format("kinds: the config declares %d kinds and the table holds %d -- both rule matrices are " ..
                        "sized from the declared number, so a mismatch is not a detail", count, #kinds))
  end

  local glyph = table_buffer:field("glyph")
  local passable = table_buffer:field("passable")
  local max_links = table_buffer:field("max_links")
  local weight = table_buffer:field("weight")
  local colour = table_buffer:field("colour")
  for i, kind in ipairs(kinds) do
    glyph:set(i - 1, string.byte(kind.glyph))
    passable:set(i - 1, kind.passable)
    max_links:set(i - 1, kind.max_links)
    weight:set(i - 1, kind.weight)
    colour:set(i - 1, kind.colour)
  end

  -- Обе матрицы СИММЕТРИЧНЫ: ни у смежности, ни у двери нет направления. Симметрию здесь и
  -- устанавливают — записью в обе клетки, а не надеждой на аккуратность списка.
  local function fill(field, table_of_lists)
    for from, list in pairs(table_of_lists) do
      for _, to in ipairs(list) do
        if id[from] == nil or id[to] == nil then
          error(string.format("kinds: unknown kind in the rules: '%s' or '%s'", from, to))
        end
        field:set(id[from] * count + id[to], 1)
        field:set(id[to] * count + id[from], 1)
      end
    end
  end

  fill(step.writes.adjacent_rules:field("allowed"), adjacent)
  fill(step.writes.door_rules:field("allowed"), doors)
  fill(step.writes.required_doors:field("allowed"), required)
end
