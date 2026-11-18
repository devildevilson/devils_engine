#ifndef DEVILS_ENGINE_ACT_SCRIPT_MATH_H
#define DEVILS_ENGINE_ACT_SCRIPT_MATH_H

namespace devils_script {
class system;
}

namespace devils_engine::act {
// ЗАМЕНА devils_script::system::init_math() для всего, от чего зависит мир или симуляция.
// Регистрирует тот же набор операторов и функций под теми же именами, с той же семантикой, но:
// - sin cos tan asin acos atan exp — utils::deterministic (порт musl): у init_math это std::sin и
//   прочие, а libm меняет последние биты между версиями glibc и у MSVC своя;
// - fma, mix, smoothstep собраны без FP contraction (devils_engine::options): ds собирается своими
//   флагами, и с FMA-архитектурой `a * b + c` мог бы слиться в одну инструкцию на одной машине и
//   не слиться на другой.
// Вызывать ВМЕСТО init_math, не вместе: повторная регистрация того же имени и сигнатуры — ошибка ds.
// Отличие от init_math, которое видно: эти регистрации не помечены как builtin, поэтому ds не
// сворачивает константные выражения (`2 * 3`) при компиляции — результат тот же, считается в рантайме.
// Сверка набора с ds — act_script_math_test; при обновлении devils_script сверить init_math заново.
void init_deterministic_math(devils_script::system& system);
} // namespace devils_engine::act

#endif
