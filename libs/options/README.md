# options

`libs/options` - utility CMake-слой проекта. Он не содержит C++ кода; вместо
этого он создает `INTERFACE` target `devils_engine::options`, через который
распространяются общие требования сборки: стандарт языка, compiler flags,
link-time optimization, compile definitions и include directories для некоторых
header-only зависимостей.

Идея простая: вместо копирования флагов по всем подпроектам библиотеки линкуют
`devils_engine::options` и наследуют общий build contract.

## Target

В `libs/options/CMakeLists.txt` создается:

```cmake
add_library(devils_engine_options INTERFACE)
add_library(devils_engine::options ALIAS devils_engine_options)
```

Это `INTERFACE` библиотека: она ничего не компилирует сама, а только передает
usage requirements тем target'ам, которые ее линкуют.

Root umbrella target `devils_engine` линкует `devils_engine::options` с
`INTERFACE` visibility, поэтому внешние проекты, которые линкуют весь движок,
тоже получают эти требования.

## C++ Standard

Сейчас target требует:

```cmake
target_compile_features(devils_engine_options INTERFACE cxx_std_23)
```

То есть все потребители должны компилироваться как минимум с C++23.

## Architecture Flags

Архитектурные флаги вынесены в CMake cache variables:

```cmake
DEVILS_ENGINE_ARCH_FLAGS_MSVC
DEVILS_ENGINE_ARCH_FLAGS_GNU
```

Значения по умолчанию:

- MSVC: `/arch:AVX`;
- GCC/Clang: `-mavx`.

Внешний проект может переопределить их до подключения `libs/options` или через
`-D...=...`, например:

```cmake
set(DEVILS_ENGINE_ARCH_FLAGS_GNU -march=native)
set(DEVILS_ENGINE_ARCH_FLAGS_MSVC /arch:AVX2)
```

Если нужно несколько флагов, переменная должна быть CMake list.

## MSVC Runtime

Для MSVC, если `CMAKE_MSVC_RUNTIME_LIBRARY` еще не задан, `options` выставляет
динамический multithreaded runtime:

```cmake
MultiThreaded$<$<CONFIG:Debug>:Debug>DLL
```

То есть:

- Debug -> `/MDd`;
- остальные конфиги -> `/MD`.

Это делается через стандартный CMake-механизм CMP0091 и может быть переопределено
toolchain'ом или внешним проектом.

## Compile Flags

### MSVC

Debug:

```text
${DEVILS_ENGINE_ARCH_FLAGS_MSVC}
/Zi
/Wall
/wd5045
/wd4820
/wd4514
/GR-
/permissive-
/Zc:preprocessor
```

Release:

```text
${DEVILS_ENGINE_ARCH_FLAGS_MSVC}
/Wall
/wd5045
/wd4820
/O2
/GR-
/permissive-
/Zc:preprocessor
```

MinSizeRel:

```text
${DEVILS_ENGINE_ARCH_FLAGS_MSVC}
/Wall
/wd5045
/wd4820
/O2
/GR-
/permissive-
/Zc:preprocessor
```

RelWithDebInfo:

```text
${DEVILS_ENGINE_ARCH_FLAGS_MSVC}
/ZI
/Wall
/wd5045
/wd4820
/O2
/GR-
/permissive-
/Zc:preprocessor
```

### GCC / Clang

Debug:

```text
${DEVILS_ENGINE_ARCH_FLAGS_GNU}
-g
-Wall
-Wextra
-fno-rtti
-fno-inline
```

Release:

```text
${DEVILS_ENGINE_ARCH_FLAGS_GNU}
-O3
-Wall
-Wextra
-fno-rtti
-pipe
```

MinSizeRel:

```text
${DEVILS_ENGINE_ARCH_FLAGS_GNU}
-O3
-Wall
-Wextra
-fno-rtti
-pipe
-s
```

RelWithDebInfo:

```text
${DEVILS_ENGINE_ARCH_FLAGS_GNU}
-O3
-g
-Wall
-Wextra
-fno-rtti
-pipe
```

Общий заметный выбор сейчас: RTTI отключен (`/GR-` или `-fno-rtti`), а Debug
для GCC/Clang дополнительно отключает inline через `-fno-inline`.

## Compile Definitions

Для optimized конфигов:

```text
NDEBUG
_NDEBUG
```

Для всех потребителей:

```text
DEVILS_ENGINE_PROJECT_NAME="${PROJECT_NAME}"
DEVILS_ENGINE_ENGINE_NAME="${DEVILS_ENGINE_NAME}"
```

## LTO

Link-time optimization включается опцией:

```cmake
option(DEVILS_ENGINE_ENABLE_LTO "Enable link-time optimization for optimized builds" ON)
```

LTO применяется только для:

- Release;
- MinSizeRel;
- RelWithDebInfo.

Флаги:

- GCC: `-flto=auto`;
- Clang: `-flto=thin`;
- MSVC compile: `/GL`;
- MSVC link: `/LTCG`.

Флаг передается и на compile, и на link stage, потому что flavor LTO должен
совпадать.

## Header-Only Includes

`devils_engine::options` также добавляет include directories для header-only
зависимостей, которые раньше были глобальными:

- `dr_libs`;
- `stb`;
- `zpp_bits`;
- `rapidhash`;
- `reflect`.

Теперь их получают только target'ы, которые реально линкуют options.

## Что Уже Умеет

На данный момент `libs/options` умеет:

- задавать общий C++23 requirement;
- централизованно распространять warning/optimization флаги;
- отключать RTTI для C++ target'ов;
- задавать архитектурные flags через CMake cache variables;
- включать LTO для optimized конфигов;
- задавать release compile definitions;
- выставлять MSVC runtime policy;
- прокидывать header-only include directories;
- служить единым build contract target'ом для всех подпроектов.

## Что Еще Не Сделано

Открытые направления:

- разделить флаги детерминизма на отдельные interface targets: не всем
  библиотекам нужны одинаковые ограничения float/math/optimization;
- определить набор флагов для deterministic simulation: floating point model,
  fast-math policy, denormals, FMA, precise/strict modes, platform differences;
- отдельно описать release/min-size policy: что реально нужно для компактного и
  быстрого бинарника;
- проверить, нужен ли `-Os`/`-Oz` или дополнительные linker flags для
  MinSizeRel вместо текущего `-O3 -s`;
- добавить явные presets/options для instruction sets: baseline, SSE4, AVX,
  AVX2, AVX512, native;
- сделать понятный API для внешних проектов, которые хотят выбрать target CPU
  без ручного знания переменных;
- рассмотреть sanitizer/debug targets отдельно от обычных Debug flags;
- разделить warnings и hard errors: сейчас warnings включены, но политика
  `-Werror`/`/WX` не оформлена;
- документировать поддерживаемые compiler/config combinations;
- проверить, какие флаги должны быть PUBLIC/INTERFACE, а какие лучше держать
  PRIVATE на внутренних target'ах.

Граница библиотеки сейчас такая: `options` отвечает только за CMake usage
requirements и build policy. Она не должна содержать runtime-логику движка.
