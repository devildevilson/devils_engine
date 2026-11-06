# options — карта правки

Compile/link policy, а не разбор аргументов executable. Единственный файл реализации —
`CMakeLists.txt`; интерфейсная цель `devils_engine::options` задаёт C++23, ISA, definitions и LTO.

Общие правила — [корневой AGENTS.md](../../AGENTS.md). Подробное устройство —
[README.md](README.md).

## Границы и контракты

- ISA выбирается корневым `DEVILS_ENGINE_ARCH=OFF|AVX|AVX2|NATIVE`; не добавлять скрытый `-march=native`.
- Сохранять согласованность compiler/linker LTO и MSVC runtime. Не включать fast-math ради скорости:
  генератор, serialization/replay и сравнения Debug/Release зависят от арифметики.
- Header-only include dirs здесь — существующий compatibility contract. Новую зависимость
  подключать к её реальному потребителю, а не ко всем целям через options.
- CLI11 подключается отдельно через `devils_engine::playground_cli` у площадок.

## Проверка

Сверить configure и compile/link commands затронутого toolchain; собрать реальную цель-потребитель.
Изменение ISA требует проверить `OFF` и выбранный baseline. Новый cross-platform результат не
приписывать одному Linux-прогону.

Собирать из корня: `cmake --build build-release --target <test_target> -j 8`, затем
`ctest --test-dir build-release -R '<test_target>' --output-on-failure`.
Выбирать проверки по изменению; полный прогон всей библиотеки ради правки текста не нужен.

## Память и задачи

Открытые работы и сверка прежних списков — [TASKS.md](../../TASKS.md), аудит —
[PROJECT_AUDIT.md](../../PROJECT_AUDIT.md). Исторические подробности доступны через
[указатель архива](../../docs/history/README.md). Новые контракты записывать здесь,
хронологию экспериментов — в отдельном audit/history.
