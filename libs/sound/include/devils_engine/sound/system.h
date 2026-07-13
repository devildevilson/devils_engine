#ifndef DEVILS_ENGINE_SOUND_SYSTEM_H
#define DEVILS_ENGINE_SOUND_SYSTEM_H

#include <string>
#include <string_view>
#include <vector>
#include <queue>
#include <unordered_map>
#include <memory>
#include <array>

//#include <glm/vec3.hpp>

#include "common.h"
#include "devils_engine/utils/memory_pool.h"
#include "resource.h"
// надо бы добавить сюда поддержку opus формата наверное

// возвращаясь к звуку, виртуальные сорсы это откровенный бред какой то
// если так подумать то все таки настройки звук (settings) это тема
// его нужно просто добавить в какой то кью который подберет в начале своего кадра система
// дополнительно нужно проверить в каком состоянии находится конкретный звук
// в настройках укажем какое действие мы хотим сделать (запустить, остановить и тд)
// текущий воспроизводимый звук поди добавим в отдельный массив
// что делаем с координатами? должна быть интерполяция
// мы можем добавить координату1 и координату2 с точками во времени
// но точка во времени должна быть в будущем для координаты2

// да нужно на самом деле просто использовать абстрактный task_id для каждого звука
// + указать поле next для того чтобы плавно перейти к следующему звуку (loop)
// отслеживать состояние звука через task_id

typedef struct ALCdevice ALCdevice;
typedef struct ALCcontext ALCcontext;

typedef struct ma_device ma_device;
typedef struct ma_engine ma_engine;
typedef struct ma_sound ma_sound;
typedef struct ma_context ma_context;

// как бы аккуратно сделать поиск?
#define SOUND_SYSTEM_EXTENSION_LIST \
  X(mp3)  \
  X(flac) \
  X(wav)  \
  X(ogg)  \
  X(opus) \
  X(pcm)  \

namespace devils_engine {
  namespace sound {
    constexpr size_t default_mono_sounds_count = 256;
    constexpr size_t default_stereo_sounds_count = 64;
    //constexpr size_t default_mono_sounds_count = 0;
    //constexpr size_t default_stereo_sounds_count = 1;
    constexpr float default_sound_max_distance = 100.0f;
    constexpr uint32_t default_sound_sample_rate = 48000;

    // тип звука? музыка, высказывание, бекграунд, эффект
    // сильно зависит от приоритета и от того относительно чего звук воспроизводится 
    struct task {
      // update only for pitch and volume?
      enum class command { play, stop, update };

      size_t id;
      resource2 res;
      enum command command;
      enum type type;
      float pitch;
      float volume;
      double start;
      size_t after;

      vec3 pos;
      vec3 dir;
      vec3 vel;

      task() noexcept;
      task(const size_t id, const resource2 &res) noexcept;
    };

    // dedicated update for sound position
    struct task_update {
      size_t id;
      vec3 pos;
      vec3 dir;
      vec3 vel;
    };

    enum class task_state {
      queued,
      playing,
      stopped,
      finished
    };

    struct task_status {
      size_t id;
      std::string_view resource_id;
      enum type type;
      enum task_state state;
      double progress;
      size_t after;
      size_t frames_decoded;
      size_t frames_total;
      size_t underruns;
      vec3 pos;
      vec3 dir;
      vec3 vel;
    };

    class system {
    public:
      using handle_t = uint32_t;

      struct source {
        handle_t handle;
        handle_t buffers[2];

        inline source() : handle(0), buffers{0, 0} {}
      };

      system(const size_t queue_size = 3);
      ~system();

      system(const system &) noexcept = delete;
      system(system &&) noexcept = default;
      system & operator=(const system &) noexcept = delete;
      system & operator=(system &&) noexcept = default;

      bool setup_sound(const struct task &task);
      bool remove_sound(const size_t task_id);
      bool play_sound(const size_t task_id); // мы не хотим этим пользоваться нигде кроме музыки
      bool stop_sound(const size_t task_id); // мы не хотим этим пользоваться нигде кроме музыки (наверное хотим приостановить все звуки в меню)
      double stat_sound(const size_t task_id) const; // [0,1]
      bool set_sound(const size_t task_id, const double place); // [0,1]
      bool update_sound(const struct task_update &task);

      bool set_listener_pos(const vec3 &pos);
      bool set_listener_ori(const vec3 &look_at, const vec3 &up);
      bool set_listener_vel(const vec3 &vel);

      // 1 - очень громко, наверное по умолчанию нужно ставить 0.2
      // хотя может быть вообще замаппить [0, 0.2]
      void set_master_volume(const float val);
      void set_source_volume(const uint32_t type, const float val);

      void update(const size_t time);

      //void load_resource(std::string id, const enum resource::type type, std::vector<char> buffer);

      size_t available_sources_count() const;
    private:
      struct volume_set {
        inline static constexpr size_t sound_types_count = 8;
        float master;
        float source[sound_types_count];

        volume_set() noexcept;
      };

      struct sound_task {
        struct task task;
        size_t timestamp;
        size_t loaded_frames;
        size_t processed_frames;

        sound_task() noexcept;
      };

      struct sound_task_queue {
        std::array<sound_task, 8> queue;
        struct source source;
        double priority;

        sound_task_queue() noexcept;
        bool has_source() const noexcept;
      };

      ALCdevice* device;
      ALCcontext* ctx;
      size_t counter;
      size_t queue_size;
      size_t sources_offset;

      volume_set volume;

      std::vector<sound_task_queue> tasks;
      std::vector<source> sources;
      //std::unique_ptr<sound_processing_data[]> proc_array;
      //std::vector<std::unique_ptr<virtual_source>> vsources;
      //std::vector<source_processing*> static_processors;
      //std::vector<source_processing*> processors;

      /*background_source *background;
      std::vector<menu_source*> menu_sources;
      std::vector<special_source*> special_sources;
      std::vector<game_source*> game_sources;*/
      // тут у нас сорсы поделятся скорее на эксклюзивные и виртуальные
      // где эксклюзивным выдадут сорс с самого начала
      // а виртуальные будут его ожидать по ходу дела

      //size_t get_new_id();
      std::tuple<size_t, size_t> find_task_id(const size_t task_id) const;
      //void remove_from_queue(sound_processing_data *queue, const size_t index);
    };

    // получить список устройств

    class system2 {
    public:
      struct sound_instance;
      struct miniaudio_data_converter;

      system2(const std::string_view device_name = {}, const double stream_buffer_seconds = 1.0, const size_t decode_frames_per_update = 0); // сюда передать устройство, пересоздадим всю структуру чтобы поменять устройство
      ~system2() noexcept;

      system2(const system2& copy) noexcept = delete;
      system2(system2&& move) noexcept = delete;
      system2 & operator=(const system2& copy) noexcept = delete;
      system2 & operator=(system2& move) noexcept = delete;

      bool setup_sound(const struct task &task);
      bool remove_sound(const size_t task_id);
      bool play_sound(const size_t task_id); // мы не хотим этим пользоваться нигде кроме музыки
      bool stop_sound(const size_t task_id); // мы не хотим этим пользоваться нигде кроме музыки (наверное хотим приостановить все звуки в меню)
      double stat_sound(const size_t task_id) const; // [0,1]
      bool stat_sound(const size_t task_id, struct task_status &out) const;
      void snapshot(std::vector<struct task_status> &out) const;
      bool set_sound(const size_t task_id, const double place); // [0,1]
      bool update_sound(const struct task_update &task);
      void set_decode_budget_frames(const size_t frames) noexcept;
      static bool playback_devices(std::vector<std::string> &out);

      bool set_listener_pos(const vec3 &pos);
      bool set_listener_ori(const vec3 &look_at, const vec3 &up);
      bool set_listener_vel(const vec3 &vel);

      // 1 - очень громко, наверное по умолчанию нужно ставить 0.2
      // хотя может быть вообще замаппить [0, 0.2]
      void set_master_volume(const float val);
      void set_source_volume(const uint32_t type, const float val);

      void update(const size_t time);
    private:
      // Явная FSM жизненного цикла задачи. Гарантирует терминацию из ЛЮБОГО состояния
      // (таймаут ожидания голоса + дедлайн жизни голоса) и единую точку очистки → нет утечек
      // задач/голосов/декодеров. Тяжёлые ресурсы (decoder/converter) создаются на входе в active.
      enum class task_phase : uint8_t {
        waiting_voice, // голоса нет: ждём свободный (или голос prev для after); дедлайн ожидания → dropped
        active,        // голос есть, декодер создан, декодируем сегмент в кольцо
        draining,      // декод закончен, ждём вычитки кольца; дедлайн жизни → finished (страховка)
        finished       // терминал: единая точка очистки (release_task) — вернуть голос, освободить ресурсы
      };

      struct sound_task {
        sound_instance* inst;
        struct task task;
        size_t timestamp;        // cur_time создания — база дедлайна ОЖИДАНИЯ голоса
        size_t active_time;      // cur_time входа в active — база дедлайна ЖИЗНИ голоса
        size_t lifetime_budget;  // макс. µs в active+draining (длительность сегмента + запас)
        size_t frames_decoded;
        size_t stream_begin_frame;
        size_t stream_frames_count;
        size_t source_frames_count;
        task_phase phase;
        bool started;
        std::unique_ptr<sound::decoder> decoder;
        std::unique_ptr<miniaudio_data_converter> converter;
      };

      size_t cur_time;
      uint32_t playback_channels;
      uint32_t playback_sample_rate;
      double stream_buffer_seconds;
      size_t decode_frames_per_update;
      std::unique_ptr<ma_context> m_context;
      std::unique_ptr<ma_engine> m_engine;
      std::unique_ptr<ma_device> m_device;
      std::vector<std::unique_ptr<sound_instance>> m_instances_mono;
      std::vector<sound_instance*> m_instances_mono_stack;
      std::vector<std::unique_ptr<sound_instance>> m_instances_stereo;
      std::vector<sound_instance*> m_instances_stereo_stack;
      std::vector<std::unique_ptr<sound_instance>> m_instances;
      std::vector<sound_task> m_tasks;

      std::vector<uint32_t> cache1;
      std::vector<uint32_t> cache2;

      size_t find_task_id(const size_t task_id) const;
      struct task_status make_status(const sound_task &task) const;
      uint32_t instance_init(sound_instance* inst, const uint32_t sample_rate, const uint32_t channels, const enum format format) const;

      // FSM-хелперы жизненного цикла задачи (см. task_phase):
      bool acquire_voice(sound_task &task);  // выдать/разделить голос; false если пул пуст (idempotent при уже выданном)
      void init_decode(sound_task &task);    // создать decoder+converter, посчитать frames (idempotent) — на входе в active
      void release_task(sound_task &task);   // терминал: вернуть/выгрузить голос (если не разделён), освободить ресурсы
    };
  }
}

#endif
