-- Тело шага seasons: лето, зима и климатические зоны.
--
-- Сезон считается дважды — при наклонении +tilt и -tilt, — а лето и зима выбираются из этой пары как
-- тёплый и холодный. Так и получается ГРАДИЕНТ от лета к зиме, причём местный: при наклонении +tilt
-- в северном полушарии лето, а в южном зима, и «сезон с положительным наклонением» летом назвать
-- нельзя. Заодно сезонность (размах) выходит величиной клетки: у берега мала, в глубине материка
-- велика, потому что континентальность усиливает именно ОТКЛОНЕНИЕ от годового значения.

return function(step)
  local cells = step.writes.cells
  local p = step.params
  local count = cells:count()
  local spacing = math.sqrt(4.0 * math.pi / count)

  local lat_sin = cells:field("lat_sin")
  local height = cells:field("height")
  local ocean_distance = cells:field("ocean_distance")
  local season_a = cells:field("season_a")
  local season_b = cells:field("season_b")

  local insolation = {
    tilt = p.axial_tilt,
    equator_temperature = p.equator_temperature,
    pole_temperature = p.pole_temperature,
    exponent = p.insolation_exponent,
    lapse_rate = p.lapse_rate,
    continental_gain = p.continental_gain,
    ocean_reference = p.ocean_reference / spacing,
  }

  insolation.season = 1.0
  originator.insolation{
    inputs = { lat_sin, height, ocean_distance },
    outputs = { season_a },
    params = insolation,
  }

  insolation.season = -1.0
  originator.insolation{
    inputs = { lat_sin, height, ocean_distance },
    outputs = { season_b },
    params = insolation,
  }

  originator.run_script{
    program = step.programs.summer,
    inputs = { season_a, season_b },
    outputs = { cells:field("temperature_summer") },
  }
  originator.run_script{
    program = step.programs.winter,
    inputs = { season_a, season_b },
    outputs = { cells:field("temperature_winter") },
  }
  originator.run_script{
    program = step.programs.seasonality,
    inputs = { cells:field("temperature_summer"), cells:field("temperature_winter") },
    outputs = { cells:field("seasonality") },
  }

  -- Климатическая зона — семантика мира, поэтому она правило в конфиге, а не switch в C++. Пороги
  -- лежат рядом в values.tavl, и правку «где начинается тайга» автор делает не открывая компилятор.
  originator.run_script{
    program = step.programs.climate_zone,
    inputs = {
      cells:field("land"),
      cells:field("temperature_summer"),
      cells:field("temperature_winter"),
      cells:field("precipitation"),
    },
    outputs = { cells:field("climate") },
    params = {
      ice_summer = p.ice_summer,
      tundra_summer = p.tundra_summer,
      boreal_summer = p.boreal_summer,
      tropical_winter = p.tropical_winter,
      arid_precipitation = p.arid_precipitation,
      steppe_precipitation = p.steppe_precipitation,
      wet_precipitation = p.wet_precipitation,
    },
  }

  -- Высокогорье переопределяет любую зону: снежная вершина посреди джунглей остаётся вершиной.
  originator.run_script{
    program = step.programs.alpine,
    inputs = { cells:field("climate"), height },
    outputs = { cells:field("climate") },
    params = { alpine_height = p.alpine_height },
  }

  local mean = originator.reduce_sum{ inputs = { cells:field("temperature") } } / count
  step.writes.state:field("mean_temperature"):set(0, mean)
end
