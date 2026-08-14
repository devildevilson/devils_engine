# Playground common

Общая минимальная оболочка лабораторий. Она должна сокращать механическое дублирование, но не
превращаться в ещё один renderer или универсальное приложение.

Уже есть:

- input-neutral `free_camera` и `camera_motion`;
- Vulkan reversed-Z infinite projection helper.

Планируемые обязанности:

- создание стандартного laboratory host;
- свободная и фиксированная camera, camera rail;
- pause, single-step, time scale и deterministic seed;
- runtime parameter inspector;
- просмотр render targets и debug overlays;
- CPU/GPU timings;
- screenshot/capture с config/resource/build fingerprints;
- reset и ограниченный headless frame run.

Первый consumer — `PF01_forward_plus`. Новая механика переносится сюда только когда она действительно
является лабораторной оболочкой, а не особенностью renderer feature. Feature manifests, shaders и
project look остаются у своих лабораторий.
