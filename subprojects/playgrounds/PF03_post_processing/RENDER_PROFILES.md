# Render profiles: предложение после PF03

PF03 показал, что одного `low / mid / high` рядом с отдельными числами недостаточно. У рендера есть несколько
независимых осей качества, и они меняются с разной ценой. Нужен не глобальный enum в Painter, а именованный
документ композиции: базовая форма графа плюс выбранный вариант каждой оси и, при необходимости, точечные
переопределения.

## Что уже есть и почему этого мало

- `pf03_post`, `pf03_forward`, `pf03_forward_plain` правильно представляют разные **формы pipeline**.
- `--render-scale`, `--ao-samples`, `--dof-taps` и похожие ключи патчат распарсенный config до commit. Механизм
  работает, но знание о нём размазано по `main.cpp`.
- значения UBO меняются живьём, однако `--ao=0`, `--shutter=0` и `--dof=0` не экономят GPU: pass остаётся в
  графе. Для качества это ложное выключение, пригодное только для A/B.
- старые `presets` / `scale_presets` у `declare_value` парсятся, но нигде не применяются. Они умеют менять
  только одно значение и заранее зашиты в `low / mid / high`, поэтому всё равно не описывают профиль кадра.
- `--look` — полезный прикладной preset изображения, но это другая ось. Цветовой look не должен случайно
  менять число SSAO-проб или форму графа.

## Предлагаемая модель

Профиль — таблица выбранных **фрагментов по именованным осям**:

| ось | примеры вариантов | что меняет |
|---|---|---|
| `pipeline` | `post`, `forward_msaa`, `forward_plain` | форму графа целиком |
| `resolution` | `native`, `taa_u_75`, `taa_u_67`, `taa_u_50` | размеры render targets |
| `ao` | `off`, `low`, `high` | наличие pass + specialization constants |
| `ssr` | `off`, `linear_64`, `linear_128`, `hiz_128` | наличие pass + pipeline variant |
| `dof` | `off`, `gameplay`, `cinematic` | наличие цепочки + число проб |
| `motion_blur` | `off`, `gameplay`, `cinematic` | наличие цепочки + число проб |
| `bloom` | `off`, `three_mips`, `four_mips` | форму цепочки и число mip levels |
| `output` | `sdr`, позднее `hdr10` | формат/transfer function |
| `look` | `neutral`, `warm`, `bleach` | только прикладные runtime-параметры |

Один профиль выбирает по одному варианту нужных осей. Пользовательская точечная настройка заменяет только
одну ось, не копируя весь preset:

```text
--render-profile=balanced
--render.ao=high
--render.ssr=off
--look=warm
```

Здесь `balanced` остаётся базой, `ao` и `ssr` — две явные дельты, а `look` ортогонален качеству.

## Возможная форма TAVL

Это эскиз контракта, не предложение немедленно учить общий TAVL произвольным object paths:

```tavl
{
  name = balanced
  axes = [
    pipeline = post,
    resolution = taa_u_75,
    ao = low,
    ssr = linear_64,
    dof = off,
    motion_blur = off,
    bloom = four_mips,
    output = sdr
  ]
}

{
  name = ao.low
  group = ao
  graph_fragment = post_ao
  overrides = [
    { step = compute_ao, shader_constant = ao_samples, value = "8" }
  ]
}

{
  name = resolution.taa_u_75
  group = resolution
  overrides = [
    { declared_value = viewport_render, scale = [0.75, 0.75, 1.0] },
    { declared_value = viewport_render_half, scale = [0.375, 0.375, 1.0] }
  ]
}
```

У fragment должен быть один `group`. Два выбранных fragment одной группы — громкая ошибка, если второй не
объявлен явным CLI/user override. Это ловит случай `ao.low + ao.high`, вместо того чтобы молча зависеть от
порядка файлов.

## Где проходит граница Painter

Painter применяет только то, чем владеет:

- выбор/композицию графа;
- размеры, mip levels, sample count и форматы ресурсов;
- specialization constants шагов;
- material definitions и pipeline state.

Числа вроде `ao_radius`, `fog_density`, `bloom_intensity` и цветового look принадлежат проекту. Верхний слой
может хранить их рядом в одном пользовательском документе, но передаёт Painter только секцию `render`, а
секцию `settings` применяет сам. Иначе Painter пришлось бы знать смысл каждого эффекта.

## Цена изменения должна выводиться, а не писаться от руки

Тип target уже определяет необходимую операцию:

| target | цена применения |
|---|---|
| runtime setting / UBO | следующий кадр |
| уже резидентный graph | переключение на границе кадра |
| specialization constant / material definition | пересборка pipeline |
| extent / mips / samples / format | пересоздание ресурсов и зависимых views/descriptors |
| topology fragment | пересборка render graph; сначала можно оставить startup-only |

Resolver сначала полностью строит итоговый профиль, валидирует ссылки и конфликты, затем выдаёт diff и план
операций. Применять профиль по одному полю опасно: при ошибке посередине получится наполовину старый граф и
наполовину новый. Нужна транзакция «всё валидно — commit целиком».

## Как представлять выключенные эффекты

`enabled = false` внутри существующего pass не решает задачу, если pass всё равно dispatch'ится. Низкий tier
должен выбирать форму графа без SSAO/SSR/DoF/motion blur. На первом этапе разумнее держать несколько явных
графов или сгенерированных вариантов, чем сразу строить универсальный condition language.

Практичная первая ступень для PF03:

| профиль | resolution | AO | SSR | DoF | motion blur | назначение |
|---|---:|---:|---:|---:|---:|---|
| `performance` | 0.67 TAAU | 8 | off | off | off | слабая iGPU / 60 FPS |
| `balanced` | 0.75 TAAU | 8 | linear 64 | off | off | обычная игра |
| `quality` | 1.0 | 16 | linear 128 | off | off | native-resolution база |
| `cinematic` | 1.0 | 16 | linear 128 | 24 | 12 | всё включено для A/B/кадра |

Это именно стартовая таблица, а не утверждение об универсальных настройках: PF03 уже измерил, что стоимость
`compose`, SSR и post-TAA проходов зависит от конкретного железа, а TAAU ускоряет только часть до resolve.

## Порядок реализации

1. **Resolver без runtime hot reload (S–M):** документы profile/fragment, группы, наследование одного base,
   конфликтные override и итоговый diff. Чистые unit tests без Vulkan.
2. **Применение до graph commit (M):** declared values, resource fields, step specialization constants,
   material definitions и выбор существующего графа. Это заменяет ручные патчи в playground `main.cpp`.
3. **Форма графа (L):** либо явные варианты, либо ограниченные named fragments с проверкой зависимостей.
   Не начинать с произвольного удаления любого pass: автоматически чинить ресурсы и consumers слишком легко
   превратить в неявный второй render graph compiler.
4. **Hot switch (отдельная L-задача):** транзакционное пересоздание ресурсов/pipelines на границе кадра,
   отчёт о цене, сброс temporal history и безопасное освобождение старого поколения после fences.

Минимально полезный результат — пункты 1–2: `--render-profile` и частичная замена оси до старта. Этого уже
достаточно, чтобы перестать копировать по `main.cpp` ручной поиск `compute_ao`, `viewport_render` и похожих
полей. Runtime-переключение не должно блокировать этот первый контракт.
