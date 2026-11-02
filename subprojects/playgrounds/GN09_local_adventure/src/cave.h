#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "adventure.h"
#include "devils_engine/originator/generator_chain.h"

namespace gn09 {

// Пещера природного места — небольшая система ходов для заплыва нескольких ныряльщиков.
// Генератор целиком в данных (generator/cave/cave.tavl, cave/01_skeleton.lua): тот же
// генератор галерей, что у маршрута лодки, в меньшем масштабе — узкое горло из полости
// узла, за ним дерево ходов с нишами. Скелет строится без запросов к полю, чужая вода
// обходится по оценкам, пещера вправе прорезать что угодно.
//
// Жёстко: ныряльщик проходит каждый ход (ядро капсулы), лодка не проходит в горло (ядро
// горла меньше радиуса корпуса, стенка не дальше ядра) — если ядро вообще может быть меньше
// корпуса при этом радиусе прохода. Прикидки (мягко): число залежей и время сбора, длина
// обхода под бюджет заплыва, гнездо глубоко, логово по пути.

// Скелет пещеры не дальше cave_extent - 20 м от узла (порода у узла тонкая — между галереями
// окна, — и пещера уходит за ней далеко); вся пещера с формами — в кубе полуразмера cave_reach
// (по нему чанк отбирает пещеры мест).
// Радиус корпуса лодки: горло уже него.

// Ядро хода: проходит сфера R, и сетка 1 м не ставит в неё треугольник.
double cave_core(double clearance_radius);

// Пещера места: экземпляр генератора `cave` цепочки generator/world, ключ — (сектор центра, ключ
// узла). Выбирается один раз в канонической окрестности узла, кэш общий для всех окон.
std::shared_ptr<const site> cave_for(const adventure& world, uint32_t node_id);
// site из экземпляра генератора cave (представление выходов; живёт с экземпляром).
std::shared_ptr<const site> site_view(const devils_engine::originator::generator_result& instance,
                                      double clearance_radius);
// Экземпляр генератора cave места (пещера, предметы).
std::shared_ptr<const devils_engine::originator::generator_result> cave_instance(const adventure& world, uint32_t node_id);
// Пересобирает тот же экземпляр мимо кэша и сравнивает выходы побайтно, затем сверяет природное поле
// из примитивов с cave_density в точках вокруг места. Пусто — совпали.
std::string verify_cave_instance(const adventure& world, uint32_t node_id);
// Длина пути по скелету от устья до гнезда / до комнаты.
double cave_depth(const site& local);
double cave_room_depth(const site& local, uint32_t room);
// Длина обхода: от устья через все залежи (у гнездовой — до гнезда) и обратно по
// кратчайшим путям пещеры.
double cave_tour_length(const site& local);
// Независимая проверка по полю. Жёсткое — устройство пещеры, проход ныряльщика по каждому ходу и
// то, что лодка не проходит горло; мягкое — прикидки, которые могут нарушаться у меньшинства пещер.
struct cave_check {
  enum : uint32_t { lair_shallow = 1, time_off = 2, nest_exposed = 4, other_water = 8 };
  std::string refusal;
  uint32_t soft = 0;
};
cave_check verify_cave(const adventure& world, const site& local);

} // namespace gn09
