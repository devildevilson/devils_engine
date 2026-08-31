-- Тело шага semantics.
--
-- Lua остаётся дирижёром и здесь: она выбирает программу, поля и диапазон, но сама по элементам не
-- ходит. Имена входов не передаются отдельно — они берутся у привязок, поэтому в biome_rule.ds поле
-- зовётся ровно так же, как в buffers.tavl.

return function(step)
  local cells = step.writes.cells

  originator.run_script{
    program = step.programs.biome_rule,
    inputs = { cells:field("smoothed"), cells:field("moisture") },
    outputs = { cells:field("biome_ds") },
  }
end
