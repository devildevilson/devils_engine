#ifndef DEVILS_ENGINE_ORIGINATOR_SCRIPT_SHAPE_H
#define DEVILS_ENGINE_ORIGINATOR_SCRIPT_SHAPE_H

#include "generator_chain.h"
#include "primitive_field.h"

// ВИД ФОРМЫ НА DS: формула формы — программа `devils_script`, объявленная данными (`shapes` в документе
// цепочки), а не C++ проекта. Мод заменяет её файлом с тем же id.
//
// Строки вида — элементы буфера `buffer` экземпляра; каждое поле строки видно программе СКАЛЯРОМ
// (многокомпонентное — `имя_x`, `имя_y`, `имя_z`, `имя_w`). Столбцы — координаты точки `x`, `y`, `z` и
// слоты словаря (шумы точки) по их именам. Программа возвращает значение формы; метка — поле `tag` или
// число. Коробка влияния — `box_a` (и `box_b` у капсулы) ± `reach`; это ОБЕЩАНИЕ формулы: вне коробки
// значение не ниже верхней оценки фона.
//
// Считается пакетом (`column_program`): одна строка — столбец точек, скаляры строки подняты из цикла.
// Программа компилируется при первом разборе строк, когда известна раскладка буфера.

namespace devils_engine {
namespace originator {

void add_script_kind(shape_vocabulary& words, const chain_shape& shape);
void configure_script_field(shape_vocabulary& words, const chain_description& chain,
                            const parameters* constants = nullptr);

} // namespace originator
} // namespace devils_engine

#endif
