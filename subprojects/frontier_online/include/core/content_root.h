#ifndef FRONTIER_ONLINE_CORE_CONTENT_ROOT_H
#define FRONTIER_ONLINE_CORE_CONTENT_ROOT_H

#include <cstddef>
#include <string>
#include <string_view>

#include <devils_engine/utils/sha256cpp.h>

// ОТПЕЧАТОК ПРИЧИННОГО СОДЕРЖИМОГО.
//
// Две стороны считают ОДИН мир только если у них одни и те же документы, определяющие поведение:
// предикаты, FSM, GOAP, префабы и генератор земли. Совпадение версий сборки этого не гарантирует —
// правка порога в values.tavl меняет мир, не трогая ни строчки C++.
//
// Поэтому корень считается по САМИМ ФАЙЛАМ: `session.h` описывает ровно такой манифест (продукт,
// версия, затем метаданные и полные байты каждого файла в каноническом порядке), и здесь он
// строится для причинного поддерева.
//
// СПИСОК КАТАЛОГОВ ОБЪЯВЛЕН ЯВНО, и это осознанный компромисс с двумя следствиями.
//
// Хорошее: клиент и авторитет держат РАЗНЫЕ деревья ресурсов — у клиента в рабочем каталоге лежат
// ещё и движковые дефолты, шейдеры, ui. Хеш всего дерева отказал бы в присоединении из-за шрифта.
// Хешируется только то, от чего зависит СОСТОЯНИЕ.
//
// Плохое: новый причинный каталог, не вписанный сюда, тихо выпадет из проверки. Ровно поэтому
// построитель ОТКАЗЫВАЕТ, если объявленного каталога нет на диске или он пуст: молчаливо пустой
// манифест — это отпечаток, совпадающий у двух разных миров.

namespace frontier_online::core {

struct causal_content_manifest {
  devils_engine::utils::digest root{};
  std::size_t files = 0;
  std::size_t bytes = 0;
};

// Причинные каталоги внутри модуля. Порядок здесь не важен — манифест сортируется по пути.
inline constexpr std::string_view causal_content_directories[] = {
  "fsm", "generator", "goap", "prefab", "scripts",
};

// resource_root — тот же путь, что уходит в demiurg::module_system (каталог, внутри которого
// лежит `core/`). При отказе `detail` объясняет, ЧТО именно не сошлось; корень не меняется.
[[nodiscard]] bool build_causal_content_root(std::string_view resource_root,
                                             std::string_view module_name,
                                             causal_content_manifest& output,
                                             std::string& detail);

// Короткое читаемое представление: первые восемь байт корня. Для печати в отчёт, не для сравнения.
[[nodiscard]] std::string short_digest(const devils_engine::utils::digest& value);

} // namespace frontier_online::core

#endif
