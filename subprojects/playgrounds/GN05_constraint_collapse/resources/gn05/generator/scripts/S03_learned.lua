-- Тело шага layout: ПРАВИЛА СНИМАЮТСЯ С ОБРАЗЦА, и по ним раскладывается карта.
--
-- Разница с объявленными правилами не в удобстве. Объявить можно только то, что автор умеет
-- СФОРМУЛИРОВАТЬ, а «берег выглядит вот так» словами не формулируется — зато рисуется. Окно
-- `window x window` пробегает образец, разные окна становятся алфавитом узоров, а встреченные
-- соседства окон — таблицей.
--
-- Правило допуска одно: пара допущена тогда и только тогда, когда она ВСТРЕТИЛАСЬ. Отсюда обещание,
-- которое площадка и проверяет: ни одного окна в результате, которого не было в образце. Цена
-- названа: такие правила ТЕСНЕЕ объявленных, противоречия чаще — и ровно за этим решателю откаты.
--
-- Решатель работает в алфавите УЗОРОВ, а карта нужна в тайлах. Перевод делает обычный `lookup` по
-- представителям узоров: второго механизма для этого не понадобилось.

return function(step)
  local p = step.params
  local alphabet = step.writes.alphabet
  local rules = step.writes.learned_rules
  local cells = step.writes.cells
  local state = step.writes.state

  originator.learn_rules{
    inputs = { step.reads.sample:field("tile") },
    outputs = { alphabet:field("weight"), rules:field("allowed"), alphabet:field("representative"),
                state:field("patterns") },
    params = { window = p.window, wrap = 0 },
  }

  originator.collapse{
    inputs = { alphabet:field("weight"), rules:field("allowed") },
    outputs = { cells:field("pattern"), state:field("attempts"), state:field("rollbacks") },
    params = { attempts = p.attempts, wrap = p.wrap, rollbacks = p.rollbacks, history = p.history },
  }

  originator.lookup{
    inputs = { cells:field("pattern"), alphabet:field("representative") },
    outputs = { cells:field("tile") },
  }
end
