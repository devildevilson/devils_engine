#ifndef DEVILS_ENGINE_VISAGE_SYSTEM_H
#define DEVILS_ENGINE_VISAGE_SYSTEM_H

#include <cstddef>
#include <cstdint>
#include <chrono>
#include "bindings/lua_header.h"

struct nk_context;
struct nk_buffer;

namespace devils_engine {
namespace visage {

struct font_t;

// где то тут наверное будет храниться отдельный луа стейт чисто под интерфейс
// + энвайронмент 
// + nk_context
// и тут мы зададим собственно все биндинги для интерфейса
// и каждый кадр будем обновлять функцией update
// + будет шаг в котором нужно будет скинуть все данные по контексту в буфер

// для каких то других систем которые используют луа будет другой хук и другое время 
// больше всего меня волнует генератор, как бы не попасть в просак с ним

class system {
public:
  static constexpr int hook_after_instructions_count = 100000;
  static constexpr size_t seconds10 = 10ull * 1000ull * 1000ull;
  using clock_t = std::chrono::steady_clock;
  static clock_t::time_point start_tp;
  static size_t instruction_counter;

  system(const font_t* default_font);
  ~system() noexcept;
  void load_entry_point(const std::string &path); // потом будет текст скрипта
  void update(const size_t time);

  nk_context* ctx_native() const noexcept;
  nk_buffer* cmds_native() const noexcept;
private:
  sol::state lua;
  sol::environment env;
  const font_t* default_font;
  std::unique_ptr<nk_context> ctx;
  //std::unique_ptr<nk_buffer> cmds;
  nk_buffer* cmds;

  sol::function entry;
};
}
}

#endif