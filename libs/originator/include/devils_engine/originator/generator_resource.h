#ifndef DEVILS_ENGINE_ORIGINATOR_GENERATOR_RESOURCE_H
#define DEVILS_ENGINE_ORIGINATOR_GENERATOR_RESOURCE_H

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "devils_engine/demiurg/resource_base.h"

#include "pipeline.h"

// Генератор как РЕСУРС: точка входа, которую можно назвать одним id, и всё, что она за собой тянет.
//
// Ядро originator знает только текст: ему дают документ, он отдаёт описание. Этого достаточно для
// стенда с папкой на диске и совершенно недостаточно для мода — модуль игрока приезжает zip'ом, его
// файлы переопределяют движковые по логическому id, а расширения у ресурса нет вовсе. Всё это уже
// умеет demiurg, поэтому здесь не появляется второй способ искать файлы: появляется ШОВ.
//
// Отсюда и деление целей сборки. `devils_engine::originator` не знает про demiurg, как не знает про
// lua; `devils_engine::originator_config` знает про оба, но только чтобы превратить id в текст.
//
// Проекту, у которого генераторов больше одного (планета, подземелье, чанк объёма), точка входа даёт
// ровно то, чего не хватало: генератор называется ОДНИМ именем, а как он внутри разложен по файлам —
// его собственное дело.

namespace devils_engine {
namespace demiurg {
class resource_system;
}

namespace originator {

// Один документ генератора: точка входа, буферы, значения (`tavl`), тело шага (`lua`) или программа
// devils_script (`ds`). Ресурс CPU-only и текстовый — ровно как `painter::render_config_source`,
// потому что вопрос у них один и тот же: дать текст, пришедший из модуля, а не с диска.
class generator_source : public demiurg::resource_interface {
public:
  std::string text;

  generator_source();

  void ensure_text_loaded();
  void drop_text();

  void load_cold(const utils::safe_handle_t& handle) override;
  void load_warm(const utils::safe_handle_t& handle) override;
  void unload_hot(const utils::safe_handle_t& handle) override;
  void unload_warm(const utils::safe_handle_t& handle) override;
};

// Собранный генератор: описание пайплайна и тексты всех его скриптов, уже разрешённые по id.
//
// Тексты лежат здесь, а не берутся из реестра по требованию, по двум причинам. Первая: шаг знает
// СВОЙ id тела, и держать этот id живым до конца генерации значило бы держать открытым реестр.
// Вторая: `script_host` принимает текст, а не ресурс, и это правильно — хост скриптов не должен
// уметь ходить за файлами.
struct generator_config {
  // id точки входа, уже абсолютный. Относительные ссылки внутри генератора считаются от него.
  std::string entry_id;
  pipeline_description description;
  // Диапазоны настройки из документа значений: границы и шаг тех чисел, которые автор мира разрешил
  // крутить. Пусто, если документа значений нет или диапазоны в нём не объявлены.
  std::vector<value_range> ranges;
  // id -> текст. Ключ — тот же id, который лежит в `step.body` и в `step.programs`.
  std::vector<std::pair<std::string, std::string>> sources;

  // Текст по id. Отсутствие текста — ошибка сборки пакета, а не пустая строка: молчаливое пустое
  // тело шага выглядело бы как шаг, который ничего не делает.
  const std::string& source(const std::string_view& id) const;
};

// Регистрирует тип ресурса генератора. Имя типа обязано быть СЕГМЕНТОМ пути (контракт demiurg),
// поэтому файлы генератора лежат под `<type_name>/...` внутри модуля.
//
// Все три расширения — один тип, потому что тип ресурса здесь отвечает на вопрос «чей это файл», а
// не «что в нём написано»: что документ tavl, а что тело шага, говорит точка входа.
void register_generator_resources(demiurg::resource_system& resources, const std::string& type_name = "generator");

// Собирает генератор по id точки входа: читает документ, разрешает ссылки на значения, буферы, тела
// шагов и программы, и отдаёт всё разом.
//
// Ссылки внутри точки входа — обычные demiurg-пути: без расширения, от корня модуля; путь, начатый с
// точки, считается от папки самой точки входа.
generator_config load_generator(const demiurg::resource_system& resources, const std::string_view& entry_id);

// Текст одного ресурса генератора по абсолютному id. Нужен хосту, которому понадобился скрипт ВНЕ
// пайплайна — например перф-стенду, сравнивающему два уровня исполнения на одном и том же правиле:
// такой скрипт не является шагом, поэтому точка входа его и не называет.
std::string read_generator_source(const demiurg::resource_system& resources, const std::string_view& id);

} // namespace originator
} // namespace devils_engine

#endif
