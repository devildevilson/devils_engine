# flow

`libs/flow` - заготовка будущей системы анимаций. Сейчас в библиотеке почти нет
реализации: фактически есть только `flow::state_t` в `system.h`. Поэтому этот
README фиксирует не столько текущий API, сколько предполагаемую границу
подпроекта и направление дизайна.

Будущая роль `flow` - интерпретировать gameplay state как визуальное состояние,
растянутое во времени: 2D анимации, 2.5D sprite states, 3D skeletal clips,
материальные/UV эффекты и animation callbacks.

## Текущий Код

Сейчас есть один базовый тип:

```cpp
struct state_t {
  using action_f = std::function<int32_t(void*)>;

  std::string name;
  action_f action;
  size_t time;
  const state_t* next;
};
```

Смысл текущего наброска:

- `name` - имя состояния;
- `action` - callback, который можно вызвать в этом состоянии;
- `time` - длительность состояния;
- `next` - следующее состояние в sequence.

`flow::system` пока пустой. CMake target для `libs/flow` также отсутствует в
самой папке: библиотека еще не оформлена как полноценный build target.

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

На данный момент `libs/flow` умеет только на уровне наброска:

- описать named state;
- указать длительность state;
- указать callback function object;
- связать state с `next`.

Готовой системы обновления, resource loading, clip sampling, sprite selection,
interpolation, blending или callbacks через `act` пока нет.

## Что Еще Не Сделано

Основной техдолг:

- оформить `libs/flow` как полноценный CMake target;
- решить базовую модель времени: gameplay tick, render time, local clip time;
- описать 2D animation data model;
- описать 2.5D directional sprite model;
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
