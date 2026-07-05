# flow

`libs/flow` - первый слой будущей системы анимаций. Сейчас реализован
минимальный 2D/2.5D/UV presentation sampler: анимация задается цепочкой
неизменяемых timed state'ов, runtime `playback` двигается по этой цепочке,
выбирает картинку по направлению, накапливает UV-смещение и эмитит action id
наружу.

Роль `flow` - интерпретировать gameplay state как визуальное состояние,
растянутое во времени. 3D skeletal clips, blending и сложные animation
callbacks остаются будущими расширениями.

## Текущий Код / Первый Срез

Базовый state сейчас выглядит концептуально так:

```cpp
struct image_ref {
  const demiurg::resource_interface* image;
  uint8_t mirror_state;
};

struct state {
  uint64_t duration_mcs;
  uint32_t next;
  std::vector<image_ref> images;
  utils::id action;
  vec2 uv;
};
```

`flow::library` хранит общую таблицу state'ов. `next` в runtime - уже индекс в
этой таблице, а не строка. `animation_resource` парсит `.tavl`, добавляет
state'ы в library и резолвит строковые ссылки вида `anim/abc:2`.

`flow::playback` хранит текущий state, elapsed time, флаг уже отправленного
action и текущее UV. UV двигается к `current_uv + state.uv` в течение duration,
после чего целая часть отбрасывается через truncation, чтобы не наматывать
бесполезные большие значения.

## Назначение

`libs/mood` должен отвечать за gameplay FSM: какое состояние у актора с точки
зрения правил игры.

`libs/flow` должен отвечать за отображение этого gameplay state: какую
анимацию, кадр, clip, pose или визуальный time-sequence нужно показать.

Пример разделения:

```text
mood:
attack1 [button_press] -> attack2 [button_press] -> attack3
attack1 -> attack1_to_idle
attack2 -> attack2_to_idle
attack3 -> attack3_to_idle

flow:
attack1         -> конкретная анимация первого удара
attack2         -> конкретная анимация второго удара
attack3         -> конкретная анимация третьего удара
attack1_to_idle -> плавный возврат из первого удара
```

То есть `mood` решает "что происходит по gameplay", а `flow` решает "как это
показать во времени".

## 2D / 2.5D Анимации

Для 2D flow должен уметь описывать sequence состояний, где одно состояние - это
одна картинка или набор картинок.

Минимальная модель:

- animation state;
- duration;
- sprite/frame resource;
- optional next state;
- callbacks в заданные моменты.

Для 2.5D, в духе DOOM-style sprites, одно состояние может ссылаться не на одну
картинку, а на набор directional frames:

- front;
- side;
- back;
- mirrored variants;
- angle buckets.

В таком режиме `flow` должен выбирать картинку не только по времени, но и по
углу камеры/объекта.

## 3D Анимации

Для 3D flow должен описывать clip-based skeletal animation:

- animation clip;
- local time внутри clip;
- keyframes;
- interpolation;
- bone transform output;
- blending/crossfade;
- optional root motion;
- callbacks по времени.

Базовая задача 3D flow - по времени кадра получить pose: набор матриц/трансформ
костей для модели.

В будущем здесь возможен GPGPU path: пересчет bone matrices на GPU, batched
skinning data, animation texture или другие способы убрать CPU work с большого
количества акторов.

## Generic Timed State

Желательно, чтобы `flow` не был жестко привязан только к sprite или skeleton.
Библиотека должна быть достаточно шаблонной, чтобы описывать любое состояние,
растянутое во времени.

Потенциальные применения:

- смена sprite frames;
- skeletal pose;
- UV scrolling/rotation;
- shader/material parameters;
- billboard animation;
- animated texture atlas;
- timed visibility/effect states;
- gameplay-synced visual events.

Например эффект вращения UV у стены можно представить как flow track, который
по времени обновляет material parameter, а не как отдельную ad-hoc систему.

## Animation Callbacks

`flow` должен уметь вызывать функции из `libs/act` в заданные моменты
анимации.

Примеры:

- в момент удара вызвать effect `deal_damage`;
- на кадре шага вызвать `play_footstep`;
- при старте cast вызвать `spawn_charge_fx`;
- при завершении анимации вызвать FSM event или gameplay intent.

Текущий `state_t::action` - только ранний намек на это. В будущем callback
лучше связать с `act::registry`, `act::exec_context` и, вероятно, `act::intent`,
чтобы callback не мутировал мир напрямую в render thread.

## Thread Model

Есть высокая вероятность, что `flow` должен крутиться рядом с render thread.

Причина: визуальная анимация должна интерполироваться с частотой отрисовщика, а
не только с частотой gameplay tick. Gameplay может идти 30/60 Hz, а render -
выше или ниже. `flow` должен уметь получить промежуточное визуальное состояние
на конкретное render time.

Возможная модель:

- gameplay thread задает coarse state через `mood`/ECS;
- render/flow thread интерполирует presentation state;
- callbacks, которые меняют gameplay, не исполняются напрямую на render thread,
  а публикуются как intents/messages.

## Связь С Другими Библиотеками

`mood`:
задает gameplay state и переходы между состояниями.

`flow`:
маппит gameplay state на визуальную animation sequence.

`act`:
дает общий контракт для animation callbacks.

`demiurg`:
должен поставлять animation resources: sprite atlases, clips, skeletons,
material animation configs.

`painter`:
получает готовые instance/pose/material данные для отрисовки.

`aesthetics`:
может хранить компоненты текущего animation state, clip time, visual id,
playback speed и т.п.

## Что Уже Умеет

На данный момент `libs/flow` умеет:

- собираться как `devils_engine::flow`;
- хранить immutable state'ы в `flow::library`;
- резолвить имена state'ов вида `anim/abc:0` в индексы общей таблицы;
- парсить `.tavl` state list через `flow::animation_resource`;
- хранить картинки как `demiurg::resource_interface*` + mirror flags;
- разбирать image refs вида `tex/img`, `tex/img:u`, `tex/img2:3:uv`;
- выбирать directional image bucket с bucket 0, центрированным на угле 0;
- проигрывать state chain через `flow::playback`;
- эмитить `action_event` один раз при входе в state;
- поддерживать `duration_mcs = 0` для последовательности action state'ов с
  защитным лимитом от бесконечного цикла;
- накапливать UV delta и truncation итогового значения;
- отдавать `sprite_sample` без знания о GPU/render internals.

Покрытие: `tests/flow_test.cpp` проверяет directional buckets, action-on-entry,
переходы по `next`, UV accumulation/truncation, zero-duration chains и `.tavl`
парсинг.

## Что Еще Не Сделано

Основной техдолг:

- решить базовую модель времени: gameplay tick, render time, local clip time;
- расширить 2D/2.5D модель после подключения к реальным sprite/image ресурсам;
- описать 3D skeletal clip model;
- определить формат animation resources и связь с `demiurg`;
- определить ECS components для текущего animation state;
- сделать mapping из `mood` state в `flow` animation state;
- добавить interpolation/blending/crossfade;
- добавить animation callbacks через `act`;
- определить, какие callbacks можно исполнять на render thread, а какие должны
  публиковаться как intents/messages;
- спроектировать GPGPU path для bone matrices и batched animation;
- добавить тесты для sequence stepping, loops, callbacks и time sampling.

Граница библиотеки должна быть такой: `flow` отвечает за визуальную
интерпретацию состояний во времени. Gameplay state и правила переходов остаются
в `mood`/gameplay systems, загрузка данных - в `demiurg`, а финальная отрисовка
- в `painter`.
