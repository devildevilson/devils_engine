#ifndef GN07_RESOURCES_H
#define GN07_RESOURCES_H

#include <string>
#include <string_view>

#include "location_model.h"
#include "node_scope.h"

// Чтение архетипа, фрагмента мира и задания из текста. Неизвестное поле — отказ, а не пропуск:
// опечатка в имени поля иначе молча оставила бы значение по умолчанию.
// Ошибки ДАННЫХ (ссылка на несуществующий узел или место, цикл родителей) — исключение здесь,
// до генерации. Невозможность ЗАДАНИЯ (архетип не умеет порт, бюджет не набирается) — отказ
// решателя с причиной: это разные ответы, и автору нужно знать, какой из них он получил.

namespace gn07 {

std::string read_text_file(const std::string& path);

archetype read_archetype(std::string_view text, std::string source_name, const script_env& env);
world_fragment read_world_fragment(std::string_view text, std::string source_name);
brief read_brief(std::string_view text, std::string source_name);

// Ссылки задания на фрагмент и архетип: локация существует и это локация, места объявлены,
// лорные связи называют существующие размещения.
void validate_brief(const brief& b, const world_fragment& world, const archetype& arch);

// Вход локации: решётка из границ узла, факты с наследованием, порты из физических связей
// узла — сторона и клетка, обращённые к соседу.
location_input derive_location_input(const world_fragment& world, const brief& b, const archetype& arch);

} // namespace gn07

#endif
