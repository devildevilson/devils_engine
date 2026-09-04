#ifndef DEVILS_ENGINE_PAINTER_COMPUTE_CONTEXT_H
#define DEVILS_ENGINE_PAINTER_COMPUTE_CONTEXT_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "auxiliary.h"
#include "vulkan_minimal.h"

// ВЫЧИСЛИТЕЛЬНЫЙ КОНТЕКСТ: Vulkan без окна и БЕЗ RENDER-ГРАФА.
//
// Существует не потому, что headless-режима у движка не было — он был. Существует потому, что он был
// собран НЕ ТАМ: путь без окна жил в шаблонах `simul::render_runtime`, то есть в прикладном рантайме,
// а `standard_render_create_base_resources` вдобавок ТРЕБУЕТ реестр с render-конфигом, в котором есть
// графы, иначе падает громко. Чистому счёту нужен меньший набор: устройство, аллокатор, командный пул,
// пул дескрипторов и кэш пайплайнов — и ни одного графа.
//
// Собран он из УЖЕ СУЩЕСТВУЮЩИХ кусков painter, а не из своих: `create_instance`/`create_device`
// (общие с рантаймом, см. auxiliary.h), `system_info::choose_physical_device_headless`,
// `graphics_base` для аллокатора и пулов, `shader_crafter` для GLSL, makers для пайплайна и
// дескрипторов, `do_command` для отправки. Второго способа поднять устройство в движке не появляется.
//
// Командный пул при этом СВОЙ и на COMPUTE-очереди: `graphics_base::create_command_pool` берёт
// семейство графики, а ради отдельной вычислительной очереди (`device_queues::compute`) весь план
// очередей и существует.
//
// Чего здесь НЕТ намеренно: поверхности, свопчейна, кадров в полёте, render-графа и составления
// пайплайна из конфига. Последнее — задача originator (он СОСТАВЛЯЕТ pipeline, а не рисует), и
// делать её здесь значило бы завести второго планировщика.

namespace devils_engine {
namespace painter {

struct graphics_base;

struct compute_context_config {
  std::string app_name = "devils_compute";
  bool validation = false;
  // Файл кэша пайплайнов. Пусто => кэш живёт только в памяти: лаборатории он не нужен, а
  // компиляция шейдера стоит миллисекунды-десятки и платится один раз за прогон.
  std::string pipeline_cache_path;
};

// ЕСТЬ ЛИ УСТРОЙСТВО ВООБЩЕ. Отдельно от конструктора потому, что отсутствие устройства — не ошибка:
// CI, сервер без GPU и машина без драйвера законны, и потребитель обязан уметь пойти на CPU
// (`ORIGINATOR_GPGPU.md` §4.6). Проверка поднимает инстанс и опускает его обратно, поэтому звать её
// в горячем пути незачем — она отвечает один раз за процесс.
bool compute_device_available();

class compute_context {
public:
  using buffer_id = uint32_t;
  using program_id = uint32_t;
  static constexpr uint32_t invalid_id = UINT32_MAX;

  // Пределы, по которым обязан зажиматься диспатч. Держатся здесь, а не спрашиваются у устройства в
  // месте вызова, потому что молча превышенный предел — это отказ драйвера в рантайме у игрока, а не
  // у автора: у Intel `maxComputeWorkGroupCount[0]` это 2^31-1, а ГАРАНТИРОВАННЫЙ Vulkan'ом минимум
  // 65535, то есть 4.2 млн элементов при группе 64. Работает у автора, ломается у игрока — тот же
  // класс, что расхождение FastNoise2 между наборами SIMD.
  struct device_limits {
    uint32_t max_group_count[3] = {0, 0, 0};
    uint32_t max_group_size[3] = {0, 0, 0};
    uint32_t max_group_invocations = 0;
    uint32_t max_shared_memory_bytes = 0;
    uint64_t max_storage_buffer_range = 0;
  };

  explicit compute_context(compute_context_config config);
  ~compute_context() noexcept;

  compute_context(const compute_context&) = delete;
  compute_context& operator=(const compute_context&) = delete;

  VkDevice device() const noexcept;
  const std::string& device_name() const noexcept;
  const device_limits& limits() const noexcept;

  // Буфер. `host_visible` означает «виден хосту на запись и чтение»: такой служит перевалочным, а
  // счёт идёт по буферу устройства. На интегрированном GPU память одна, поэтому разница между ними
  // там мала — и именно поэтому замер обязан идти по РАЗДЕЛЬНОМУ пути, иначе он измерит удачу.
  buffer_id create_buffer(const size_t byte_size, const bool host_visible);
  size_t buffer_byte_size(const buffer_id id) const;

  // Запись и чтение хостом. Только для host_visible: у буфера устройства отображения нет, и попытка
  // — громкая ошибка, а не молчаливый нуль.
  void write(const buffer_id id, const void* data, const size_t byte_size);
  void read(const buffer_id id, void* data, const size_t byte_size) const;

  // Копирование между буферами через вычислительную очередь. Это и есть ПЕРЕДАЧА, цена которой
  // решает, окупается ли устройство на короткой очереди (§4.5).
  void copy(const buffer_id from, const buffer_id to, const size_t byte_size);

  // Программа: GLSL-текст компилируется в SPIR-V и становится вычислительным пайплайном на
  // `storage_count` storage-буферов (биндинги 0..n-1) и push-константы.
  //
  // Текст, а не файл: шейдер лаборатории написан руками и лежит рядом с кодом, который его
  // проверяет. Компиляция идёт тем же `shader_crafter`, которым движок компилирует шейдеры
  // материалов, поэтому второго компилятора GLSL здесь не появляется.
  program_id create_program(const std::string& name,
                            const std::string& source,
                            const uint32_t storage_count,
                            const uint32_t push_byte_size);

  // Один проход. Число ЭЛЕМЕНТОВ, а не групп: размер группы объявлен программой, а свёртка числа
  // групп в оси и проверка по пределам — забота контекста, потому что предел у устройства, а не у
  // автора шейдера.
  void dispatch(const program_id program,
                const std::span<const buffer_id>& buffers,
                const void* push,
                const size_t push_byte_size,
                const size_t element_count,
                const uint32_t group_size = 64);

private:
  struct buffer_entry;
  struct program_entry;

  const buffer_entry& buffer_at(const buffer_id id) const;

  compute_context_config config_;
  created_instance instance_{};
  created_device device_{};
  std::unique_ptr<graphics_base> base_;
  VkCommandPool command_pool_ = nullptr;
  VkFence fence_ = nullptr;
  std::string device_name_;
  device_limits limits_{};
  std::vector<buffer_entry> buffers_;
  std::vector<program_entry> programs_;
};

} // namespace painter
} // namespace devils_engine

#endif
