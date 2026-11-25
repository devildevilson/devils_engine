#ifndef DEVILS_ENGINE_SIMUL_LAUNCH_ARGUMENTS_H
#define DEVILS_ENGINE_SIMUL_LAUNCH_ARGUMENTS_H

#include <span>
#include <string>
#include <string_view>
#include <vector>

// Аргументы запуска — канал от main к системам. main разбирает argv один раз, до потоков, и
// передаёт в app_runtime::run; дальше они лежат в bootstrap (`standard_runtime_bootstrap::arguments`)
// и только читаются: проект переводит их в свои типизированные опции и раздаёт системам при их
// создании. Глобальных синглтонов «запрос этого процесса» не нужно.
//
// Движок не знает смысла аргументов. Форма — `--name=value` или `--name`; всё остальное (позиционный
// аргумент, одиночный дефис) сохраняется с пустым именем, чтобы проект мог отвергнуть его, а не
// потерять. Проверка «неизвестный аргумент — отказ» — дело проекта.

namespace devils_engine {
namespace simul {

class launch_arguments {
public:
  struct argument {
    std::string name;  // без «--»; пусто — аргумент не в форме --name
    std::string value; // после '='; для позиционного — весь текст
    bool has_value = false;
    std::string text; // как был передан
  };

  launch_arguments() = default;

  launch_arguments(const int argc, const char* const* argv) {
    for (int i = 1; i < argc; ++i) {
      add(argv[i]);
    }
  }

  void add(const std::string_view text) {
    argument out;
    out.text = std::string(text);
    if (text.size() > 2 && text.starts_with("--")) {
      const auto body = text.substr(2);
      const auto eq = body.find('=');
      out.name = std::string(body.substr(0, eq));
      if (eq != std::string_view::npos) {
        out.value = std::string(body.substr(eq + 1));
        out.has_value = true;
      }
    } else {
      out.value = std::string(text);
      out.has_value = true;
    }
    arguments_.push_back(std::move(out));
  }

  std::span<const argument> all() const noexcept {
    return arguments_;
  }

  bool empty() const noexcept {
    return arguments_.empty();
  }

  // Последний аргумент с этим именем (поздний перекрывает ранний) или nullptr.
  const argument* find(const std::string_view name) const noexcept {
    for (auto it = arguments_.rbegin(); it != arguments_.rend(); ++it) {
      if (it->name == name) {
        return &*it;
      }
    }
    return nullptr;
  }

private:
  std::vector<argument> arguments_;
};

} // namespace simul
} // namespace devils_engine

#endif
