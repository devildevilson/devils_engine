# PF05 — submarine light room

Первая художественно направленная сцена painter stack для `submarine_coop`: тесная тёмная комната,
в которой свет является читаемым пространственным препятствием.

Лаборатория выборочно наследует доказанные возможности `PF01`–`PF03`, но владеет собственными
resources, shaders, parameter presets и executable. Расширение общей post gallery не меняет её без
явного переноса.

## Первый наблюдаемый результат

- две тесные комнаты и коридор;
- normal/emergency/damaged lights и ручной spot light;
- плотные spot/directional shadows;
- emissive fixtures и flicker;
- ограниченная exposure policy, сохраняющая настоящую темноту;
- локальный пар/туман;
- wet highlights и project color grading;
- debug view визуальной и дешёвой gameplay-light модели.

## Куда смотреть

Пока executable отсутствует. Project look живёт в локальных `resources/`, shaders и presets; `src/`
содержит только scene interaction/debug glue. Нужные lighting/shadow/post capabilities берутся как
зафиксированное подмножество, а не через зависимость от targets `PF01`–`PF03`.

## Definition of Done

Комнату можно обесточить и осветить локальным источником; тёмные области не вытягиваются auto exposure
в серый день, движущийся объект отбрасывает стабильную тень, а presentation и gameplay-light state
коррелируют без чтения framebuffer симуляцией.
