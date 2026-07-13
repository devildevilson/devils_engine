#ifndef DEVILS_ENGINE_VISAGE_SYSTEM_H
#define DEVILS_ENGINE_VISAGE_SYSTEM_H

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <span>
#include <vector>
#include <utility>
#include <tuple>
#include <string>
#include <string_view>
#include <memory>
#include "devils_engine/bindings/lua_header.h"
#include "devils_engine/utils/stack_allocator.h"
#include "render_output.h"

struct nk_context;
struct nk_buffer;
struct nk_user_font;

namespace devils_engine {
namespace visage {

struct font_t;

// Нейтральный снапшот ввода за кадр: главный поток собирает его из своей системы ввода
// (мышь/колесо/текст/клавиши-модификаторы), а system::input раздаёт это в nk_input_*.
// Так Nuklear остаётся целиком внутри visage, а наружу торчит только этот POD.
struct input_snapshot_t {
  float mouse_x = 0.0f, mouse_y = 0.0f;
  bool mouse_left = false, mouse_middle = false, mouse_right = false;
  float scroll_x = 0.0f, scroll_y = 0.0f;

  // utf32-кодпоинты, введённые в этом кадре (текстовый ввод)
  const uint32_t* text = nullptr;
  size_t text_count = 0;

  // состояние клавиш-модификаторов/редактирования в этом кадре (down)
  bool key_shift = false, key_ctrl = false;
  bool key_backspace = false, key_delete = false, key_enter = false, key_tab = false;
  bool key_left = false, key_right = false, key_up = false, key_down = false;
};

// где то тут наверное будет храниться отдельный луа стейт чисто под интерфейс
// + энвайронмент
// + nk_context
// и тут мы зададим собственно все биндинги для интерфейса
// и каждый кадр будем обновлять функцией update
// + будет шаг в котором нужно будет скинуть все данные по контексту в буфер

// для каких то других систем которые используют луа будет другой хук и другое время
// больше всего меня волнует генератор, как бы не попасть в просак с ним

// со шрифтами нужно будет повозиться

class system {
public:
  static constexpr int hook_after_instructions_count = 100000;
  static constexpr size_t seconds10 = 10ull * 1000ull * 1000ull;
  using clock_t = std::chrono::steady_clock;
  static clock_t::time_point start_tp;
  static size_t instruction_counter;

  system(const font_t* default_font);
  ~system() noexcept;
  void set_entry_point(const sol::object& value);

  // Порядок за кадр: input() -> update() -> convert(). input раздаёт ввод в nk,
  // update гоняет lua entry (строит UI), convert гонит nk_convert в host-буферы ниже.
  // update передаёт в lua-entry: (time, timestamp, rng_state). time — длительность кадра;
  // timestamp — монотонная метка времени (для фиксации начала UI-анимаций); rng_state — сид
  // псевдослучайности (заворачивается в bindings::rng_state; хост продвигает его свой prng каждый кадр).
  void input(const input_snapshot_t& in);
  void set_env_number(const std::string& name, double value);
  void update(const size_t time, const size_t timestamp, const uint64_t rng_state);
  void convert();

  // Доступ к lua-стейту/песочнице UI для ХОСТА: visage не знает про геймплейные системы
  // (звук/мир и т.п.), но хост может зарегистрировать свои usertype'ы в стейте и функции
  // в env (напр. "сыграй звук" → message в звуковой тред). Так visage остаётся чисто UI.
  sol::state& script_state() noexcept { return lua; }
  sol::environment& script_env() noexcept { return env; }

  // Результат последнего convert(): сырые байты вершин, 16-бит индексы и список draw-команд.
  std::span<const uint8_t> vertices() const noexcept { return vertices_; }
  std::span<const uint8_t> indices() const noexcept { return indices_; }
  std::span<const gui_draw_command_t> commands() const noexcept { return commands_; }

  nk_context* ctx_native() const noexcept;
  nk_buffer* cmds_native() const noexcept;
private:
  // nk_user_font под (базовый шрифт, размер в пикселях) для nk_style_push_font. Кэшируется по
  // паре (base, height): MSDF масштабируется в шейдере, поэтому варианты одного шрифта делят
  // атлас/глифы (query/width/userdata) базового nkfont и отличаются только height. texture.id
  // освежается из nkfont при каждом вызове (атлас мог дойти до HOT уже после кэширования варианта).
  nk_user_font* sized_font(const font_t* base, float height);

  sol::state lua;
  sol::environment env;
  // дефолтный базовый шрифт. ДОПОЛНИТЕЛЬНЫЕ шрифты visage не хранит: lua выбирает их
  // demiurg-хендлом в push_font{ font = handle } (метрики живут в font_resource у реестра).
  const font_t* default_font;
  std::unique_ptr<nk_context> ctx;
  // отдельный буфер draw-команд для nk_convert (НЕ ctx->memory: convert читает UI-команды
  // из ctx и пишет draw-команды сюда — это должны быть разные буферы)
  std::unique_ptr<nk_buffer> cmds;

  sol::function entry;

  // host-буферы вывода: заполняются в convert(), читаются через аксессоры выше
  std::vector<uint8_t> vertices_;
  std::vector<uint8_t> indices_;
  std::vector<gui_draw_command_t> commands_;

  // CPU-скретч под SDF-эффекты текста (push_font пишет сюда struct, nk_handle.id = offset).
  // Очищается в начале каждого update; offset 0 = дефолтный эффект (без жирности/контура).
  utils::stack_allocator effect_arena;
  // стек userdata для баланса push_font/pop_font (у nuklear нет стека userdata, как у font)
  std::vector<int> userdata_stack;
  // кэш nk_user_font по (базовый шрифт, размер) (см. sized_font). Живёт всё время жизни system; nk
  // держит указатели на варианты в стеке стиля и draw-командах, поэтому хранилище стабильно.
  std::vector<std::tuple<const font_t*, float, std::unique_ptr<nk_user_font>>> sized_fonts_;
};
}
}

#endif
