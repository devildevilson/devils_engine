#ifndef DEVILS_ENGINE_PAINTER_COMPUTE_CONTEXT_H
#define DEVILS_ENGINE_PAINTER_COMPUTE_CONTEXT_H

#include <cstddef>
#include <cstdint>
#include <functional>
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
  // компиляция шейдера платится один раз за прогон.
  std::string pipeline_cache_path;
  // Гонять ли оптимизатор SPIR-V. Вынесено наружу потому, что цена его ИЗМЕРЕНА и она велика: у
  // сгенерированного шейдера почти вся стоимость компиляции лежит здесь, а выигрыш от оптимизации
  // выражения, которое и так тривиально, ещё надо доказать. Значение по умолчанию оставлено тем же,
  // с которым движок компилирует шейдеры материалов, — чтобы разница была видна как РЕШЕНИЕ, а не
  // как случайное расхождение с остальным движком.
  bool optimize_shaders = true;
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

  // КАРТИНКА, а не буфер, и различает их не форма данных, а КАК АДРЕСУЮТ И КАК ЧИТАЮТ: у буфера
  // линейный индекс элемента, у картинки координата, аппаратный clamp/wrap, фильтр МЕЖДУ элементами
  // и свёрнутая раскладка ради 2D-локальности. Поэтому картинка выгодна ровно тогда, когда читают по
  // НЕЦЕЛОЙ координате; если читают только свой элемент — буфер лучше по всем пунктам.
  //
  // И там же граница, которую надо знать вслух: ФИЛЬТРОВАННАЯ ВЫБОРКА НЕ ДЕТЕРМИНИРОВАНА — точность
  // фильтра в Vulkan implementation-defined, то есть хуже обычного float. Значит картинка с фильтром
  // это строго класс ПРЕДСТАВЛЕНИЯ и в чанковую генерацию не пускается вовсе.
  enum class image_format {
    r32f,   // одно поле float32: поле высоты, маска, промежуточный результат
    rgba8,  // видимая текстура: четыре нормализованных байта
  };

  using image_id = uint32_t;

  // Картинка живёт в layout GENERAL всё время своей жизни. Это упрощение ЛАБОРАТОРИИ, и оно названо:
  // GENERAL законен и для записи storage-образом, и для выборки, но стоит дороже, чем
  // SHADER_READ_ONLY_OPTIMAL. Настоящий путь — вывод переходов из привязок, и он у painter уже есть в
  // render-графе; заводить второй здесь значило бы завести второго планировщика.
  image_id create_image(const uint32_t width, const uint32_t height, const image_format format);
  uint32_t image_width(const image_id id) const;
  uint32_t image_height(const image_id id) const;

  // Забрать картинку на хост. Нужно ровно двум вещам: сводке и КАРТИНКЕ ДЛЯ ГЛАЗА. Ни то, ни другое
  // не является частью конвейера — результат остаётся на устройстве, а сюда приезжает то, что человек
  // или отчёт обязаны увидеть.
  void read_image(const image_id id, void* data, const size_t byte_size);

  // Что программа привязывает. Род объявляется, потому что дескриптор типизирован: storage-образ,
  // выборка с сэмплером и storage-буфер — три разных дескриптора, и перепутать их молча нельзя.
  enum class binding_kind {
    storage_buffer,
    storage_image,
    sampled_image,
  };

  // Ссылка на привязываемый ресурс. Один из двух идентификаторов, второй остаётся invalid_id: род
  // ресурса обязан совпасть с родом биндинга, и расхождение — громкая ошибка.
  struct bound_resource {
    buffer_id buffer = invalid_id;
    image_id image = invalid_id;

    static bound_resource of_buffer(const buffer_id id) noexcept { return bound_resource{id, invalid_id}; }
    static bound_resource of_image(const image_id id) noexcept { return bound_resource{invalid_id, id}; }
  };

  // Программа: GLSL-текст компилируется в SPIR-V и становится вычислительным пайплайном на
  // `storage_count` storage-буферов (биндинги 0..n-1) и push-константы.
  //
  // Текст, а не файл: шейдер лаборатории написан руками и лежит рядом с кодом, который его
  // проверяет. Компиляция идёт тем же `shader_crafter`, которым движок компилирует шейдеры
  // материалов, поэтому второго компилятора GLSL здесь не появляется.
  //
  // РЕЗУЛЬТАТ КЭШИРУЕТСЯ по самому тексту вместе с формой привязок. Ключом служит текст, потому что он
  // и есть производная от всего остального: перевод — чистая функция от (текст ds, имена входов, роды
  // полей, версия транслятора), поэтому другой ключ означает другой текст, а тот же текст — ту же
  // программу. Нужно это не ради красоты: компиляция GLSL стоит миллисекунды-десятки, а у
  // стримингового генератора чанков тысячи, и один и тот же перевод зовётся в каждом.
  program_id create_program(const std::string& name,
                            const std::string& source,
                            const uint32_t storage_count,
                            const uint32_t push_byte_size);

  // Та же программа, но привязки объявлены ПО РОДАМ: storage-буферы, storage-образы и выборка с
  // сэмплером в порядке биндингов 0..n-1.
  program_id create_program(const std::string& name,
                            const std::string& source,
                            const std::span<const binding_kind>& bindings,
                            const uint32_t push_byte_size);

  using binding_set_id = uint32_t;

  // НАБОР ДЕСКРИПТОРОВ НА ВЫЗОВ, а не на программу, и это не удобство.
  //
  // Дескрипторы вступают в силу при ОТПРАВКЕ, а не при записи команды. Значит два вызова одной
  // программы с разными привязками, записанные в одну отправку, при общем наборе прочитали бы оба
  // ПОСЛЕДНИЕ привязки — и это молча другие числа, а не отказ. А очередь ровно этим и занимается:
  // два `remap` подряд — та же программа над разными полями. Поэтому набор принадлежит вызову.
  binding_set_id create_binding_set(const program_id program);
  void update_binding_set(const binding_set_id set, const std::span<const bound_resource>& resources);

  // Сколько КОМПИЛЯЦИЙ действительно было. Отдаётся наружу затем, чтобы попадание в кэш можно было
  // проверить, а не предположить: программа, скомпилированная дважды, работает так же, и по
  // результату этого не видно.
  size_t compiled_programs() const noexcept;

  // Один проход. Число ЭЛЕМЕНТОВ, а не групп: размер группы объявлен программой, а свёртка числа
  // групп в оси и проверка по пределам — забота контекста, потому что предел у устройства, а не у
  // автора шейдера.
  void dispatch(const program_id program,
                const std::span<const buffer_id>& buffers,
                const void* push,
                const size_t push_byte_size,
                const size_t element_count,
                const uint32_t group_size = 64);

  // Диспатч по ДВУМ осям: число элементов по x и по y. Для картинки это её собственная форма, и
  // считать по ней прямо честнее, чем сворачивать линейный индекс обратно в координату.
  //
  // Барьеров между отправками здесь нет и не нужно: каждая отправка ждёт свой fence, то есть запись
  // предыдущего прохода завершена до начала следующего. У НАСТОЯЩЕЙ очереди, которая уходит на
  // устройство одной отправкой, барьеры понадобятся — и они выводятся из привязок тем же вопросом,
  // которым выводится проверка мёртвой работы (§6.1).
  void dispatch_2d(const program_id program,
                   const std::span<const bound_resource>& resources,
                   const void* push,
                   const size_t push_byte_size,
                   const uint32_t width,
                   const uint32_t height,
                   const uint32_t group_x = 8,
                   const uint32_t group_y = 8);

  // ЗАПИСЬ НЕСКОЛЬКИХ ОПЕРАЦИЙ В ОДНУ ОТПРАВКУ.
  //
  // Существует потому, что цена отправки — не цена работы: замер §5 п.3 в `ORIGINATOR_GPGPU.md`
  // раскладывал круг передачи на три отправки НАМЕРЕННО, чтобы приписать цену по фазам, и получил
  // 70% на передачу как оценку СВЕРХУ. Настоящая очередь уходит на устройство одной отправкой — это
  // и есть смысл «исполняется целиком, без возврата в оркестратор», верный и на уровне Vulkan.
  class recorder {
  public:
    void copy(const buffer_id from, const buffer_id to, const size_t byte_size);
    void dispatch(const program_id program,
                  const binding_set_id set,
                  const void* push,
                  const size_t push_byte_size,
                  const size_t element_count,
                  const uint32_t group_size = 64);
    // БАРЬЕР между проходами: запись предыдущего видна чтению следующего. Ставится там, где
    // зависимость ЕСТЬ, и не ставится там, где её нет — а знает это тот, кто составил очередь.
    void barrier();

  private:
    friend class compute_context;
    recorder(compute_context& owner, VkCommandBuffer buffer) noexcept;

    compute_context* owner_ = nullptr;
    VkCommandBuffer buffer_ = nullptr;
  };

  // Отправляет ОДИН командный буфер и ждёт его. Всё, что записал `record`, уходит вместе.
  void submit(const std::function<void(recorder&)>& record);

private:
  struct buffer_entry;
  struct image_entry;
  struct program_entry;
  struct binding_set_entry;

  const buffer_entry& buffer_at(const buffer_id id) const;
  const image_entry& image_at(const image_id id) const;
  const binding_set_entry& set_at(const binding_set_id id) const;
  program_id make_program(const std::string& name,
                          const std::string& source,
                          const std::span<const binding_kind>& bindings,
                          const uint32_t push_byte_size);

  compute_context_config config_;
  created_instance instance_{};
  created_device device_{};
  std::unique_ptr<graphics_base> base_;
  VkCommandPool command_pool_ = nullptr;
  VkFence fence_ = nullptr;
  std::string device_name_;
  device_limits limits_{};
  VkSampler linear_sampler_ = nullptr;
  std::vector<buffer_entry> buffers_;
  std::vector<image_entry> images_;
  std::vector<program_entry> programs_;
  std::vector<binding_set_entry> sets_;
  size_t compiled_ = 0;
};

} // namespace painter
} // namespace devils_engine

#endif
