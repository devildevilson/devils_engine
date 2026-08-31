-- Тело шага climate: ветры, температура и перенос влаги.
--
-- Главная мысль шага: осадки нельзя посчитать «на месте». Влага — это состояние, которое ЕДЕТ:
-- испаряется над водой, летит по ветру, поднимается на горах и выпадает. Поэтому здесь не одна
-- формула, а цикл переносов, и каждый перенос — обычный параллельный gather: клетка тянет влагу из
-- наветренного соседа, отнимает свои осадки и копит их у себя.
--
-- Число переносов считается не «на глазок», а из расстояния: конфиг задаёт путь в радианах, а тело
-- шага делит его на шаг решётки. Иначе смена разрешения планеты меняла бы дальность переноса влаги, и
-- материк, который на одной решётке высыхает в центре, на другой оставался бы влажным.

return function(step)
  local cells = step.writes.cells
  local p = step.params

  local offsets = step.reads.cell_offsets:field("start")
  local arcs = step.reads.cell_arcs:field("cell")

  local position = cells:field("position")
  local lat_sin = cells:field("lat_sin")
  local height = cells:field("height")
  local land = cells:field("land")
  local wind = cells:field("wind")
  local temperature = cells:field("temperature")
  local moisture = cells:field("moisture")
  local moisture_next = cells:field("moisture_next")
  local rain = cells:field("rain")

  local count = cells:count()
  local spacing = math.sqrt(4.0 * math.pi / count)

  -- 1. Ветер. Число поясов приходит из конфига, а не из кода: у быстро вращающейся планеты одна
  -- ячейка Хэдли разрезается на несколько, и это одно из двух мест, где вращение влияет на климат.
  -- Второе — соотношение зональной и меридиональной составляющей: чем сильнее Кориолис, тем более
  -- широтным становится приземный ветер.
  originator.wind_field{
    inputs = { position, lat_sin },
    outputs = { wind },
    params = {
      axis = p.axis,
      bands = p.wind_bands,
      speed = p.wind_speed,
      meridional = p.wind_meridional,
      rotation = p.rotation,
    },
  }

  -- 2. Годовая температура. Нужна раньше влаги, потому что ёмкость воздуха зависит от температуры:
  -- полярная пустыня существует именно поэтому — океан рядом, а испарять нечем.
  originator.insolation{
    inputs = { lat_sin, height, cells:field("ocean_distance") },
    outputs = { temperature },
    params = {
      season = 0.0,
      tilt = p.axial_tilt,
      equator_temperature = p.equator_temperature,
      pole_temperature = p.pole_temperature,
      exponent = p.insolation_exponent,
      lapse_rate = p.lapse_rate,
      continental_gain = p.continental_gain,
      ocean_reference = p.ocean_reference / spacing,
    },
  }

  -- 3. Перенос влаги. Начинается с нуля: накопленный дождь обязан быть результатом переноса, а не
  -- остатком предыдущего прогона.
  originator.fill{ outputs = { moisture }, params = { value = 0.0 } }
  originator.fill{ outputs = { rain }, params = { value = 0.0 } }

  local pairs_count = math.floor(math.min(p.moisture_travel / spacing, p.moisture_max_iterations) / 2)
  if pairs_count < 1 then
    pairs_count = 1
  end

  local transport = {
    transport = p.moisture_transport,
    evaporation = p.moisture_evaporation,
    land_evaporation = p.moisture_land_evaporation,
    capacity_base = p.moisture_capacity_base,
    capacity_gain = p.moisture_capacity_gain,
    convective = p.rain_convective,
    convective_reference = p.rain_convective_reference,
    convective_exponent = p.rain_convective_exponent,
    -- Орографический подъём задан «на метр набора высоты», поэтому от разрешения не зависит: набор
    -- высоты между соседями и так становится больше на грубой решётке.
    orographic = p.rain_orographic,
    ocean_rain = p.rain_ocean,
  }

  -- Проходы идут парами, потому что gather требует разных полей источника и приёмника: влага течёт
  -- moisture -> moisture_next -> moisture, а дождь копится в одном и том же поле. Копить в своём
  -- элементе выходного поля можно: клетка читает и пишет только себя, и порядок обхода ни на что не
  -- влияет — проход остаётся параллельным.
  for _ = 1, pairs_count do
    originator.moisture_step{
      inputs = { offsets, arcs, position, wind, moisture, land, temperature, height },
      outputs = { moisture_next, rain },
      params = transport,
    }
    originator.moisture_step{
      inputs = { offsets, arcs, position, wind, moisture_next, land, temperature, height },
      outputs = { moisture, rain },
      params = transport,
    }
  end

  -- 4. Осадки в долях от СРЕДНЕГО по планете, а не в условных единицах накопленного дождя.
  --
  -- Так сделано после первого прогона, где средние осадки вышли 0.045 при пороге сухости 0.09, и
  -- 28% суши стало пустыней: пороги климата оказались в единицах, которых модель переноса не знает.
  -- Нормировка на среднее делает порог читаемым («суше трети средних осадков») и, главное, не
  -- требует перенастройки порогов после каждой правки модели влаги.
  --
  -- Самокалибровка по измеренному среднему здесь ЗАКОННА именно потому, что генератор планеты
  -- одноразовый и видит поверхность целиком. При чанкованной генерации это запрещено (GN01: каждый
  -- чанк измерил бы своё среднее, и соседние чанки получили бы разные климаты).
  -- Среднее берётся по СУШЕ, а не по планете, и это второй урок того же прогона: над океаном дождь
  -- идёт всегда, поэтому среднее по планете оказалось в три раза выше среднего по суше, и порог
  -- сухости срезал две трети материков в пустыню. Пороги климата описывают сушу — значит и единица
  -- измерения обязана быть сушей.
  originator.modulate{ inputs = { rain, land }, outputs = { moisture_next } }
  local land_cells = originator.reduce_sum{ inputs = { land } }
  local mean_rain = originator.reduce_sum{ inputs = { moisture_next } } / math.max(land_cells, 1.0)
  if mean_rain <= 0.0 then
    error("climate: перенос влаги не дал над сушей ни одного дождя, проверьте ветер и испарение")
  end

  originator.remap{
    inputs = { rain },
    outputs = { cells:field("precipitation") },
    params = { scale = 1.0 / mean_rain, min = 0.0 },
  }
end
