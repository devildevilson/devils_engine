#ifndef DEVILS_ENGINE_UTILS_ISA_GUARD_H
#define DEVILS_ENGINE_UTILS_ISA_GUARD_H

#include <cstdint>
#include <string>

// ПРОВЕРКА НАБОРА ИНСТРУКЦИЙ ДО ПЕРВОЙ ИНСТРУКЦИИ ДВИЖКА. Код движка собран под DEVILS_ENGINE_ARCH
// (пакеты avx и baseline), и процессор без нужного расширения падает на первой же такой инструкции —
// где угодно, иногда ещё в статических инициализаторах. Проверка исполняется раньше них: конструктор
// с наивысшим приоритетом (GCC/Clang) или секция .CRT$XCT (MSVC) в библиотеке devils_isa_guard,
// которая сама собрана БЕЗ флагов архитектуры. Компоновщик тянет её в каждый исполняемый файл
// через неопределённый символ devils_isa_guard_anchor (опции линковки devils_utils).
//
// Требуемый набор выводит сам компилятор: isa_required.cpp собран с флагами движка и записывает
// предопределённые макросы (__AVX__, __AVX2__, …) в константу — без единой инструкции.
//
// DEVILS_ENGINE_ISA_GUARD_TEST=missing — считать, что процессор не умеет ничего (тест отказа).

namespace devils_engine::utils::isa {

enum feature : uint32_t {
  sse4_2  = 1u << 0,
  popcnt  = 1u << 1,
  avx     = 1u << 2,
  avx2    = 1u << 3,
  fma     = 1u << 4,
  f16c    = 1u << 5,
  bmi     = 1u << 6,
  bmi2    = 1u << 7,
  lzcnt   = 1u << 8,
  avx512f = 1u << 9,
};

// Под что собран код движка.
uint32_t required() noexcept;
// Что умеет этот процессор (с учётом поддержки ОС для AVX-состояния); 0 вне x86.
uint32_t available() noexcept;
// "avx avx2 fma"; пустая строка — пустой набор.
std::string names(uint32_t features);

} // namespace devils_engine::utils::isa

#endif
