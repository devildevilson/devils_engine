#ifndef DEVILS_ENGINE_INPUT_BINDINGS_H
#define DEVILS_ENGINE_INPUT_BINDINGS_H

#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <gtl/btree.hpp>

// Key-mapping как данные настроек. bindings_config — tavl-сериализуемая карта «действие → кнопки»
// (в файле: `actions = { camera_up = [key_w, up] }`). Standard runtime хранит
// её отдельно от остальных settings в `key_bingings.tavl`, накатывает поверх стандартного
// словаря при создании окна/reload и записывает обратно эффективную живую карту.
//
// Имена кнопок: canonical-таблица клавиатуры (key_names.h, требует живого GLFW для сканкода),
// mouse_left/right/middle и mouse_4..mouse_8 для мыши (синтетические сканкоды, GLFW не нужен),
// «scancode_N» — lossless-fallback для кнопок вне canonical-таблицы: сканкоды платформенные,
// но settings-файл и так пер-машинный, зато экзотическая кнопка переживает save/load.

namespace devils_engine {
namespace input {

struct bindings_config {
  // действие → имена кнопок (слоты, максимум events::event_key_slots_count).
  // btree_map (сортированный), не hash: детерминированный порядок строк в сохранённом файле.
  gtl::btree_map<std::string, std::vector<std::string>> actions;
};

// имя кнопки конфига → (glfw_key, scancode); {-1, -1} = не распознано / раскладке неизвестно
std::tuple<int32_t, int32_t> binding_key_from_name(const std::string_view& name) noexcept;
// сканкод → имя для конфига (mouse_* / canonical / scancode_N-fallback); пусто для scancode < 0
std::string binding_name_from_scancode(const int32_t scancode);

// Валидирует весь снимок до его применения. Canonical keyboard names требуют уже
// инициализированный GLFW; поэтому standard runtime вызывает это после создания окна.
// Любая неизвестная клавиша или переполнение слотов делает весь файл невалидным.
bool validate_bindings(const bindings_config& config);

// Накатить конфиг поверх текущих биндингов: перечисленные действия перепривязываются ЦЕЛИКОМ
// (хвостовые слоты чистятся), неперечисленные не трогаются, пустой список = отвязать действие,
// неизвестное имя кнопки — warn и пропуск слота.
void apply_bindings(const bindings_config& config);
// Снимок эффективной живой карты (для выгрузки в settings). События, привязанные голым hash-ом
// (без имени), в текстовый конфиг не попадают.
bindings_config collect_bindings();

} // namespace input
} // namespace devils_engine

#endif
