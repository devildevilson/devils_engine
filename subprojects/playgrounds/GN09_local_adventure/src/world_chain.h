#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "adventure.h"
#include "devils_engine/originator/generator_chain.h"
#include "devils_engine/originator/primitive_field.h"

// Цепочка генераторов мира GN09 (resources/gn09/generator/world.tavl): макрограф, места, пещеры.
// Правила живут в скриптах генераторов; здесь — загрузка цепочки через demiurg (с модами), нативные
// тела (залы региона и C++-мир окна) и перевод буферов графа в graph.
namespace gn09 {

// Цепочка на радиус прохода: он задаёт ядро и потому весь мир мест, а кэш цепочки различает
// экземпляры только зерном и ключом. Потокобезопасно; цепочки живут до конца процесса.
devils_engine::originator::generator_chain& world_chain(double clearance_radius);

// Словарь назначений комнат пещеры (значения генератора cave).
const std::vector<std::string>& cave_purposes();
// Словарь предметов пещеры (features.kind, значения генератора cave).
const std::vector<std::string>& cave_feature_kinds();
// Виды форм, объявленные данными (`shapes` документа цепочки): формулы на ds.
const std::vector<devils_engine::originator::chain_shape>& world_shape_kinds();
const devils_engine::originator::chain_description& world_chain_description();
const devils_engine::originator::parameters& world_constants();
// Словарь назначений комнат места базы (значения генератора base_site).
const std::vector<std::string>& base_purposes();

// Ключ окна макрографа: (min_x, min_z, ширина | высота << 32).
devils_engine::originator::instance_key window_key(graph::window area);

// Залы из выходов генератора halls (регион).
std::vector<hall> halls_from(const devils_engine::originator::generator_result& instance);

// C++-мир окна для нативных потребителей: представление экземпляра генератора window (граф и залы его
// окна из буферов); живёт с экземпляром.
std::shared_ptr<const adventure> world_view(const devils_engine::originator::generator_result& window);

// Граф из выходов генератора graph.
graph graph_from(const devils_engine::originator::generator_result& instance);

// Моды — папки внутри resources с файлами тех же id, кладутся поверх gn09 в порядке списка (позже —
// сильнее). Задаётся ДО первого запроса к цепочке.
void set_world_mods(std::string_view comma_separated_folders);

// Словарь форм мира (adventure.cpp) на радиус прохода: слоты шумов точки, фоновая скала, виды gallery,
// turn, hall (буферы окна), site (объект пещеры места), station и tunnel (экземпляры генераторов
// station и tunnel). Одни формулы и для полей цепочки, и для полей, собранных хостом (prepare_field).
const devils_engine::originator::shape_vocabulary& world_shapes(double clearance_radius);

// Инструменты lua-хостов цепочки: скелет и нативные блоки пещеры (cave.cpp).
void add_world_tools(devils_engine::originator::tool_registry& tools);

} // namespace gn09
