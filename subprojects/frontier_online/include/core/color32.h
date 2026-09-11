#ifndef FRONTIER_ONLINE_CORE_COLOR32_H
#define FRONTIER_ONLINE_CORE_COLOR32_H

#include <cstdint>

// Цвет как ПРИЧИННАЯ величина: 8 бит на канал, RGBA в младших-к-старшим байтах.
//
// Он живёт здесь, а не в instance_layout, потому что цвет актора попадает в реплицируемое
// состояние (actor_visual — зарегистрированный компонент, он едет в checkpoint и по проводу),
// а instance_layout::rgba8_color — это ЛИСТ GPU-раскладки из libs/painter. Пока причинный
// компонент был объявлен через него, причинное ядро тянуло за собой презентационную библиотеку
// ради одного uint32 — и headless-авторитет не мог существовать без painter.
//
// Раскладка совпадает с instance_layout::rgba8_color побайтово специально: сборка инстансов
// перекладывает одно поле без преобразования, а не пересчитывает цвет.

namespace frontier_online {
namespace core {

struct color32 {
  uint32_t value = 0xffffffffu;
};

} // namespace core
} // namespace frontier_online

#endif
