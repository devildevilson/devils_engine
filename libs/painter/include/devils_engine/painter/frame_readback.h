#ifndef DEVILS_ENGINE_PAINTER_FRAME_READBACK_H
#define DEVILS_ENGINE_PAINTER_FRAME_READBACK_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// СЪЁМ КАДРА ШАГОМ ГРАФА. Копию делает сам граф: условный пасс (RND-50) на счётчике съёма копирует
// цель кадра в буфер `readback` — слой и барьеры ведёт граф, как у любого другого шага. Хост только
// двигает счётчик и забирает байты, когда fence кадра-писателя уже пройден: никакой отдельной
// отправки и никакого ожидания GPU на потоке рендера.
//
// Объявление в рендер-конфиге (имена по умолчанию — `frame_readback::names`):
//
//   declare_counters: frame_capture
//   resources: { name = frame_readback, format = c4, role = readback, size = viewport,
//                swap = frame_capture, type = doublebuffer }
//   steps:     { name = copy_frame_to_readback, command = "copy albedo_res frame_readback" }
//   graph:     { name = capture, counter = frame_capture,
//                subpasses = [ { albedo_res = (transfer_src, store), frame_readback = (transfer_dst, store) } ],
//                steps = [ copy_frame_to_readback ] }
//
// Пасс ставится там, где цель уже лежит в transfer_src (после отрисовки, до блита в свопчейн). В
// кадрах без сдвига счётчика он не пишет ни одной команды.
//
// Один съём за раз: следующий запрос принимается, когда байты предыдущего забраны. Отсюда же
// следует, что счётчик кэша сдвигается не чаще раза в frames_in_flight кадров, — ровно то, чего
// требует проверка условных пассов.
//
// Владелец — поток рендера; объект не потокобезопасен.

namespace devils_engine {
namespace painter {

struct graphics_base;

// Байты кадра: width*height*4, строки сверху вниз, 8 бит на канал.
struct captured_frame {
  uint32_t width = 0;
  uint32_t height = 0;
  bool bgra = false; // порядок каналов цели (B8G8R8A8), иначе R8G8B8A8
  std::vector<uint8_t> pixels;
};

class frame_readback {
public:
  struct names {
    std::string_view source = "albedo_res";
    std::string_view readback = "frame_readback";
    std::string_view counter = "frame_capture";
  };

  enum class status { idle, pending, ready, lost };

  // Привязка к графу. false — граф съёма не объявляет (нет счётчика, буфера или цели): это выбор
  // конфига, а не ошибка. Объявление есть, но неверное (роль, счётчик буфера, формат цели, пасс
  // без этого счётчика) — utils::error. Вызывать после создания графа и после его смены.
  bool bind(const graphics_base& base, const names& n);
  bool bind(const graphics_base& base) { return bind(base, names{}); }
  void unbind() noexcept;
  bool bound() const noexcept { return counter_ != invalid; }
  // Запрос принят, а байты ещё не забраны.
  bool busy() const noexcept { return state_ != state::idle; }

  // Снять ближайший нарисованный кадр. false — не привязан или съём уже идёт.
  bool request() noexcept;

  // До prepare_frame: сдвигает счётчик, чтобы пасс копирования исполнился в этом кадре.
  void before_frame(graphics_base& base);
  // После submit_frame: кадр с копией ушёл на GPU (условный пасс зафиксирован успешной отправкой).
  void after_submit(const graphics_base& base);
  // После prepare_frame следующих кадров. ready — `out` заполнен: fence кадра-писателя пройден, копия
  // завершена. lost — размер цели сменился между отправкой и забором (пересоздание по окну),
  // байтов нет; съём закончен. pending — ждём; idle — съёма нет.
  status collect(graphics_base& base, captured_frame& out);

private:
  static constexpr uint32_t invalid = UINT32_MAX;
  enum class state { idle, requested, armed, submitted, lost };

  uint32_t source_ = invalid;
  uint32_t readback_ = invalid;
  uint32_t counter_ = invalid;
  uint32_t pass_ = invalid;
  state state_ = state::idle;
  uint32_t frames_waited_ = 0;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
};

} // namespace painter
} // namespace devils_engine

#endif
