#ifndef DEVILS_ENGINE_ORIGINATOR_DEVICE_QUEUE_H
#define DEVILS_ENGINE_ORIGINATOR_DEVICE_QUEUE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "computation_queue.h"

#include "devils_engine/painter/compute_context.h"

// Originator строит план вычислений для painter из очереди, объявленной конфигом.
//
// Второго планировщика при этом не появляется. Всё, что здесь есть, ВЫВОДИТСЯ из той же очереди,
// которую проверяет `check_queue`:
//
//   ПЕРЕДАЧА выводится из границ очереди: загружаются поля, которые вызов читает и которых до
//   него никто не писал, выгружается ровно `output`. Объявлять план передачи не надо — он уже
//   вычислен, и вычислен тем же обходом;
//
//   БАРЬЕРЫ выводятся тем же вопросом, что проверка мёртвой работы: «читает ли следующий проход то,
//   что записал предыдущий». Это буквально один вопрос, заданный дважды, поэтому и ответ здесь один,
//   а не второй набор правил. Где зависимости нет — барьера нет, и это не оптимизация, а следствие;
//
//   ФОРМА вызова на устройстве объявляется тем, кто его собрал: нативный инструмент объявляет её
//   сам, программа `devils_script` — переводом. Ядро очереди про GLSL не знает.
//
//   РОД РЕСУРСА ВЫВОДИТСЯ. Поле, которое хоть один вызов читает фильтром между элементами, становится
//   на устройстве картинкой. Остальные поля остаются буферами. Инструмент объявляет способ чтения.
//   Привязки для выбранных ресурсов собирает `build_device_shader`.
//
// План записывает загрузку, проходы и выгрузку в одну отправку.

namespace devils_engine {
namespace originator {

// Можно ли эту очередь исполнить на устройстве. Пустое сообщение => можно.
//
// Отказ сообщает о непригодности плана для устройства. Выбор CPU-конфигурации принадлежит
// вызывающему. Запрошенный через `device = true` запуск требует подходящего плана.
struct device_check {
  bool allowed = true;
  std::string message;
};

device_check check_device_queue(const computation_queue& queue);

struct device_report {
  size_t calls = 0;
  // Сколько барьеров реально поставлено. Величина существует затем, чтобы «барьеры выводятся» было
  // проверяемо: у цепочки, где каждый проход читает предыдущий, их на один меньше числа проходов, а
  // у независимых проходов — ни одного.
  size_t barriers = 0;
  size_t upload_bytes = 0;
  size_t download_bytes = 0;
  // Сколько полей стало КАРТИНКАМИ. Величина существует затем, чтобы вывод рода был проверяем: план,
  // разложивший в картинки всё подряд, считает то же самое и стоит дороже, а по результату этого не
  // видно.
  size_t images = 0;
  // Времена одной отправки: запись команд и ожидание устройства.
  double record_ms = 0.0;
  double submit_ms = 0.0;
};

// План, составленный ОДИН раз и исполняемый много: у стримингового генератора один и тот же перевод
// зовётся в каждом чанке, и компилировать шейдеры заново на каждый было бы той же ошибкой, что
// поднимать компилятор на каждый шейдер.
class device_queue {
public:
  device_queue(painter::compute_context& context, const computation_queue& queue);
  ~device_queue() noexcept;

  device_queue(const device_queue&) = delete;
  device_queue& operator=(const device_queue&) = delete;

  // Исполняет план: загрузка входов, все проходы с выведенными барьерами, выгрузка выхода — одной
  // отправкой. Читает из хостовых буферов очереди и пишет в них же, поэтому результат оказывается
  // там, где его ждёт остальной шаг.
  device_report run();

  // Тот же план, но с параметрами ИЗ ЭТОЙ очереди. Существует ради переиспользования: у стримингового
  // генератора один и тот же шаг зовётся в каждом чанке, и строить план заново значило бы заново
  // заводить буферы устройства и наборы дескрипторов. Структура при этом обязана совпадать — числа в
  // push-константе меняются, привязки и порядок нет, — и несовпадение это громкая ошибка.
  device_report run(const computation_queue& current);

  // Отдельная отправка каждого шага для замера по фазам и сравнения с общей отправкой.
  device_report run_step_by_step();

  // Что план из себя представляет. Отдаётся наружу затем, чтобы вывод передачи и барьеров можно было
  // ПРОВЕРИТЬ, а не предположить: план, загрузивший лишнее поле, работает так же.
  const std::vector<std::string>& uploaded_fields() const noexcept;
  const std::vector<std::string>& downloaded_fields() const noexcept;
  size_t barrier_count() const noexcept;
  size_t image_count() const noexcept;
  // Сколько байт план ПЕРЕНЕСЁТ. Отдаётся до исполнения потому, что и вычислено оно до исполнения:
  // передача выведена при составлении, и сравнивать два плана можно, не запуская ни одного.
  size_t upload_byte_count() const noexcept;
  size_t download_byte_count() const noexcept;
  // Стало ли поле картинкой. Имя пишется так же, как в списках выше: `буфер.поле`.
  bool is_image(const std::string_view& field) const noexcept;

private:
  struct field_slot;
  struct call_plan;

  device_report execute(const bool single_submission);
  // Преобразование узкого поля в расширенную копию устройства и обратно. Идёт теми же аксессорами,
  // что и весь CPU-путь, поэтому зажим и округление у двух путей одни и те же.
  std::vector<std::byte> widen(const field_slot& slot) const;
  void narrow_back(const field_slot& slot) const;
  void write_push(const queue_call& call, call_plan& plan) const;

  painter::compute_context* context_ = nullptr;
  computation_queue queue_;
  std::vector<field_slot> slots_;
  std::vector<call_plan> plans_;
  std::vector<std::string> uploaded_;
  std::vector<std::string> downloaded_;
  std::vector<size_t> upload_slots_;
  std::vector<size_t> download_slots_;
  size_t barriers_ = 0;
  size_t upload_bytes_ = 0;
  size_t download_bytes_ = 0;
};

} // namespace originator
} // namespace devils_engine

#endif
