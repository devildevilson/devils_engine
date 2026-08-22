# Render settings: независимые настройки выбранного renderer

## Что является основной сущностью

Большой `performance/balanced/quality` profile не должен быть основным контрактом. В пользовательском меню
обычно есть несколько независимых выборов:

```text
resolution = native
shadows = high
ao = low
reflections = off
dof = gameplay
bloom = high
```

Кнопка `High` — лишь макрос, который выставляет сразу несколько таких значений. После ручного изменения одной
оси UI может показать `Custom`, но отдельного профиля `Custom` в данных нет: он выводится из несовпадения
текущей таблицы ни с одним известным preset.

При этом список настроек не должен быть универсальным для всего движка. Production сначала выбирает renderer
или recipe графа для сцены и платформы; уже он объявляет поддерживаемые оси, их варианты, default и
ограничения совместимости. Forward/MSAA, deferred indoor и cinematic renderer могут иметь разные схемы
настроек. Это согласуется с тем, что проект определяет меню только после выбора технологий.

Удобно различать четыре слоя:

1. **Renderer/graph recipe** — крупный набор технологий и контрактов ресурсов, выбранный проектом или сценой.
2. **Independent settings** — пользовательские `shadows/ao/reflections/...`, разрешаемые в конкретную сборку.
3. **Preset** — необязательная именованная таблица, которая массово записывает independent settings.
4. **Runtime scene state** — focus distance, текущая экспозиционная компенсация, цветовой look и другие
   прикладные UBO/SSBO-данные, которые сцена или camera volume может менять поверх выбранного качества.

Concrete render variant тогда является **результатом resolver**, например хешем
`indoor_post + {ao=low,dof=off,...}`, а не документом, который автор обязан написать целиком.

## Две цены применения

Пользователю и проекту достаточно видеть две цены:

| класс | примеры | применение |
|---|---|---|
| runtime data | focus distance, aperture, bloom intensity, fog density, цветовой look | запись UBO/SSBO на следующий кадр |
| graph build data | specialization constants, shader defines, resource sizes/formats/mips/samples, material pipeline state, topology | полная сборка нового поколения графа |

Внутри Painter сборка состоит из нескольких Vulkan-операций, но наружу не нужно выставлять отдельные цены
pipeline/resource/topology recreation: это одна транзакция. Если готовое поколение уже резидентно, проект
может переключить его на границе кадра; это оптимизация хранения, а не третий семантический вид настройки.

Одна пользовательская ось может содержать варианты с разной ценой. Например:

- `dof: gameplay ↔ cinematic` меняет только project-owned lens data и применяется в runtime;
- `dof: off ↔ gameplay` добавляет или убирает цепочку и собирает новое поколение graph;
- `dof: 12 ↔ 24 samples` через specialization constant тоже требует полной сборки, хотя topology та же.

UI может пометить настройку значком «применится после загрузки/пересборки», но цена выводится из targets её
варианта, а не дублируется вручную отдельным `cost = ...`.

## Как может выглядеть пользовательское состояние

Текущая конфигурация хранит независимые выборы:

```tavl
{
  graph = indoor_post
  settings = [
    resolution = native,
    shadows = high,
    ao = low,
    reflections = off,
    dof = gameplay,
    bloom = high
  ]
}
```

Preset — это обычная частичная или полная запись в эту таблицу:

```tavl
{
  name = high
  settings = [
    resolution = native,
    shadows = high,
    ao = high,
    reflections = high,
    dof = gameplay,
    bloom = high
  ]
}
```

Применение preset не создаёт особой связи с ним: после записи значений каждая ось снова независима. Для
повторяемого benchmark полезно сохранять итоговую развёрнутую таблицу, а не только имя preset.

Разумный precedence верхнего уровня:

```text
renderer defaults
  < platform/device defaults
  < selected preset
  < saved per-setting user choices
  < temporary project/camera runtime overrides
```

Последний слой не должен тайно менять build targets. Если cinematic camera требует другую topology, проект
явно запрашивает другую settings table или готовое graph generation.

## Определение одной оси

Каждая ось принадлежит выбранному graph recipe и содержит именованные choices. Choice — это delta, а не
полная копия config:

```tavl
{
  name = ao
  for_graph = indoor_post
  default = low

  choices = [
    {
      name = off
      fragments = [ ambient_occlusion = none ]
    },
    {
      name = low
      fragments = [ ambient_occlusion = ssao ]
      values = [ { name = ao_samples, value = [8, 0, 0] } ]
    },
    {
      name = high
      fragments = [ ambient_occlusion = ssao ]
      values = [ { name = ao_samples, value = [16, 0, 0] } ]
    }
  ]
}
```

`fragment` здесь — будущий structural механизм, не существующий синтаксис Painter. Важен контракт: recipe
объявляет именованный slot `ambient_occlusion` с известными inputs/outputs, а choice выбирает `none` или
совместимую реализацию. Это безопаснее, чем разрешить профилю произвольно удалить pass и надеяться, что
consumers сами починятся.

Runtime-варианты могут жить в том же проектном описании оси, но не обязаны проходить через Painter:

```tavl
{
  name = dof
  choices = [
    { name = off,       fragments = [ depth_of_field = none ] },
    { name = gameplay, fragments = [ depth_of_field = gather_dof ], runtime = { aperture = 4.0 } },
    { name = cinematic, fragments = [ depth_of_field = gather_dof ], runtime = { aperture = 1.4 } }
  ]
}
```

Переход `gameplay → cinematic` не требует graph build, если resolver видит, что structural/build-части
choices одинаковы и различается только runtime delta. `off → gameplay` меняет fragment и требует build.

Painter не должен знать семантику `aperture`: верхний project layer раскладывает `runtime` по своим
UBO/SSBO. Painter получает только structural/build delta.

## Почему это не требует вручную писать 2^N графов

Если восемь независимых эффектов имеют `off/on`, существует 256 итоговых комбинаций. Хранить 256 полных
TAVL-графов нельзя: они разойдутся при первом общем изменении.

Нужна композиция на уровне **ограниченных именованных extension points**:

1. base recipe объявляет slots и contracts ресурсов;
2. choice каждой оси выбирает fragment для своего slot и добавляет typed patches;
3. resolver объединяет choices, проверяет зависимости и конфликты;
4. результат проходит обычную полную graph validation;
5. только затем транзакционно строится новое поколение;
6. каноническая таблица choices может служить cache key уже собранного поколения.

То есть комбинаций исполнения по-прежнему много, но автор описывает каждый вариант эффекта один раз. Сборка
конкретного сочетания происходит по требованию. Огромная тестовая матрица от этого не исчезает, поэтому наружу
следует выставлять только полезные проекту группы, ограничивать несовместимые сочетания и объединять сильно
связанные параметры в одну ось.

Не всё обязано быть fragment. Переход `ssao → gtao` хорошо ложится в AO slot; переход `forward MSAA → deferred
TAA` меняет слишком много базовых контрактов и должен выбирать другой renderer/graph recipe. Если две
«независимые» оси постоянно конфликтуют, они либо принадлежат разным recipes, либо на самом деле являются
одной составной осью.

## Минимальный недостающий build-механизм

PF03 уже вручную делает то, что должен делать общий resolver: после разбора TAVL и до commit ищет
`viewport_render`, `compute_ao`, `gather_dof` и заменяет значения. Независимо от будущих fragments полезны три
базовых примитива:

1. `declare_values.tavl` можно использовать ссылкой в `step.shader_constants`;
2. те же значения можно использовать в material `definitions` (это текущее имя поля; по смыслу shader
   defines);
3. choice настройки может содержать typed partial patches именованных values/materials/steps до полной
   валидации и commit.

Это build-time механизм. Он не обязан менять живой `VkPipeline` или отдельный ресурс на месте.

### Ссылки на declare values

Базовый config хранит связь один раз:

```tavl
// declare_values.tavl
{
  name = ao_samples
  type = fixed
  value = [16, 0, 0]
}
{
  name = ssr_hierarchical
  type = fixed
  value = [0, 0, 0]
}

// steps/steps.tavl
{
  name = compute_ao
  material = ao_material
  shader_constants = [ ao_samples = "$ao_samples" ]
  sets = [ frame_data, gbuffer_inputs, ao_write ]
  command = "dispatch constant half_render_dispatch"
}

// materials/materials.tavl
{
  name = ssr_material
  definitions = [ PF03_SSR_HIERARCHICAL = "$ssr_hierarchical" ]
  shaders = { compute = "ssr.comp.glsl" }
}
```

`$name` означает первую компоненту declared value; `$name.x/.y/.z` — явный выбор компоненты. В MVP ссылка
разрешена только на `type = fixed`: нынешний `screensize` хранит scale, а реальный extent выводится позже из
swapchain, поэтому его `current_value.x` не является шириной. Для specialization constant конечный тип
по-прежнему приходит из SPIR-V reflection; для definition значение форматируется как GLSL token. Неизвестное
имя или неподходящий тип — громкая ошибка до Vulkan.

Для будущих дробных compile-time значений лучше добавить declared scalar с явным типом
`u32/i32/f32/bool`, а не угадывать тип по строке.

### Typed partial patches

Не нужен универсальный путь вроде `materials.scene.raster.cull`: это создаст второй язык поверх TAVL. Choice
содержит те же именованные типы объектов, но в patch-форме, где отсутствующее поле означает «оставить базовое»:

```tavl
{
  name = high
  values = [
    { name = viewport_render, scale = [1.0, 1.0, 1.0] },
    { name = ssr_hierarchical, value = [1, 0, 0] }
  ]
  materials = [
    { name = ssr_material, definitions = [ PF03_SSR_QUALITY = "2" ] }
  ]
  steps = [
    { name = trace_ssr, shader_constants = [ ssr_steps = "128" ] }
  ]
}
```

Material уже является описанием Vulkan pipeline, поэтому partial material patch покрывает defines и pipeline
state без отдельной абстракции. Step patch в MVP стоит ограничить material и shader constants; изменение
sets/command слишком близко к topology и должно идти через graph fragment.

## Merge, ownership и совместимость

При одном большом variant достаточно было бы порядка patch'ей. При независимых настройках такой порядок
опасен: `ao=high` и `reflections=low` не должны молча соревноваться за один leaf. Правила resolver:

- object target ищется по `(kind, name)` и обязан быть достижим из выбранного graph recipe;
- неизвестный или неиспользуемый target — ошибка, а не настройка «на будущее»;
- отсутствующий leaf наследует base, указанный leaf заменяет его, включая `false`, `0` и пустое значение;
- именованные maps `definitions`, `shader_constants` и `blending` сливаются по ключу;
- structural ordered arrays не патчатся; topology меняется только выбором объявленного fragment slot;
- два settings choices не могут менять один build leaf, если recipe не объявил явное правило композиции;
- fragment обязан удовлетворить slot contract, а requirements/incompatibilities проверяются до сборки;
- после merge разрешаются `$value`, затем запускается обычная parse/semantic/reflection/graph validation;
- commit происходит только целиком; при ошибке старое поколение остаётся активным.

Для partial objects нужны отдельные typed patch-структуры с `optional`-полями. Нынешние mirror defaults
(`false`, `0`, пустая строка) не различают «поле отсутствовало» и «явно записано false/0», поэтому повторно
прогнать существующий `convert()` недостаточно.

## Порядок реализации

1. **Value references (S):** `$name[.component]` в `shader_constants` и material `definitions`, с тестами
   resolution/type/error paths.
2. **Typed build patches (S–M):** `values/materials/steps`, merge в копию parsed config, ownership/conflict
   diagnostics и полный validation до commit.
3. **Independent settings resolver (M):** graph-specific schema осей и choices, defaults, preset как массовая
   запись, канонический resolved state и diff `runtime/build`. Сначала работает только с неизменной topology.
4. **Named graph slots/fragments (L):** `none` и несколько совместимых реализаций с явными resource contracts;
   этим закрываются честные `AO/SSR/DoF = off` без ручных 2^N графов.
5. **Transactional generation build/cache (M–L):** сборка целого поколения, сброс его temporal history,
   optional cache/residency и безопасное освобождение старого поколения после fences.

Минимально полезный результат — пункты 1–3: проект получает настоящие независимые quality controls и
перестаёт вручную патчить PF03 `main.cpp`. До пункта 4 structural `off/on` либо недоступен в данном recipe,
либо выбирается через несколько явно поддержанных graph recipes; подменять выключение нулевой intensity нельзя,
потому что GPU-проход всё равно будет стоить времени.
