#ifndef DEVILS_ENGINE_SOUND_SYSTEM_H
#define DEVILS_ENGINE_SOUND_SYSTEM_H

#include <string>
#include <vector>
#include <queue>
#include <unordered_map>
#include <memory>

#include <glm/vec3.hpp>

#include "utils/memory_pool.h"
#include "resource.h"
#include "basic_sources.h"

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


typedef struct ALCdevice ALCdevice;
typedef struct ALCcontext ALCcontext;

// как бы аккуратно сделать поиск?
#define SOUND_SYSTEM_EXTENSION_LIST \
  X(mp3)  \
  X(flac) \
  X(wav)  \
  X(ogg)  \
  X(pcm)  \

namespace devils_engine {
  namespace sound {
    struct settings {
      uint32_t type;
      float speed;
      float volume;
      float rnd_pitch;
      //float rnd_rate; // не будет
      bool is_loop;
      bool is_mono;
      bool is_needed; // если все источники сейчас заняты, то дропаем звук - подойдет для мелких звуков
      uint32_t force_source;
      // координаты
      glm::vec3 pos;
      glm::vec3 dir;
      glm::vec3 vel;

      settings() noexcept;

      settings(
        const uint32_t type,
        const float volume = 1.0f,
        const float speed = 1.0f,
        const float rnd_pitch = 0.0f,
        const bool is_mono = true
      ) noexcept;

      settings(
        const uint32_t type,
        const uint32_t force_source,
        const bool is_mono = false
      ) noexcept;
    };

    class system {
    public:
      using handle_t = uint32_t;

      /*struct source {
        handle_t handle;
        handle_t buffers[2];

        inline source() : handle(0), buffers{0, 0} {}
      };*/

//      struct resource {
//        enum class type {
//#define X(name) name,
//          SOUND_SYSTEM_EXTENSION_LIST
//#undef X
//
//          undefined
//        };
//
//        static std::string_view type_to_string(const size_t index);
//        
//        // теперь не нужно
//        std::string id;
//        enum type type;
//        std::unique_ptr<sound::decoder> sound;
//        std::vector<char> buffer;
//
//        resource();
//        // некоторые ресурсы нужно перевести сразу в pcm формат
//        resource(std::string id, enum type type, std::vector<char> buffer);
//        ~resource() noexcept;
//
//        double duration() const; // s
//      };

      system(const size_t queue_size = 3);
      ~system();

      system(const system &) noexcept = delete;
      system(system &&) noexcept = default;
      system & operator=(const system &) noexcept = delete;
      system & operator=(system &&) noexcept = default;

      //size_t setup_sound(const resource *res, const settings &info = settings());
      //bool remove_sound(const size_t source_id);
      //bool play_sound(const size_t source_id); // мы не хотим этим пользоваться нигде кроме музыки
      //bool stop_sound(const size_t source_id); // мы не хотим этим пользоваться нигде кроме музыки (наверное хотим приостановить все звуки в меню)
      //double stat_sound(const size_t source_id) const; // [0,1]
      //bool set_sound(const size_t source_id, const double place); // [0,1]
      //bool set_sound(const size_t source_id, const glm::vec3 &pos, const glm::vec3 &dir, const glm::vec3 &vel);

      bool set_listener_pos(const glm::vec3 &pos);
      bool set_listener_ori(const glm::vec3 &look_at, const glm::vec3 &up);
      bool set_listener_vel(const glm::vec3 &vel);

      // 1 - очень громко, наверное по умолчанию нужно ставить 0.2
      // хотя может быть вообще замаппить [0, 0.2]
      void set_master_volume(const float val);
      void set_source_volume(const uint32_t type, const float val);

      void update(const size_t time);

      //void load_resource(std::string id, const enum resource::type type, std::vector<char> buffer);

      template <typename T, typename... Args>
      T* create(Args&&... args) {
        auto ptr = std::make_unique<T>(std::forward<Args>(args)...);
        auto p = ptr.get();
        vsources.push_back(std::move(ptr));

        /*if constexpr (std::is_base_of_v<background_source, T>) {
          if (background != nullptr) utils::error("More background sounds?");
          background = p;
        } else if constexpr (std::is_base_of_v<menu_source, T>) {
          menu_sources.push_back(p);
        } else if constexpr (std::is_base_of_v<special_source, T>) {
          special_sources.push_back(p);
        } else if constexpr (std::is_base_of_v<game_source, T>) {
          game_sources.push_back(p);
        }*/

        return p;
      }

      background_source *create_background_source();
      menu_source *create_menu_source();
      special_source *create_special_source();
      game_source *create_game_source();

      size_t available_sources_count() const;
    private:
      struct volume_set {
        static const size_t sound_types_count = 8;
        float master;
        float source[sound_types_count];

        volume_set() noexcept;
      };

      //struct sound_processing_data {
      //  struct settings info; // возможно не все настройки нам отсюда нужны
      //  const resource *res;
      //  size_t time;
      //  size_t loaded_frames;
      //  size_t processed_frames;
      //  size_t id;

      //  sound_processing_data() noexcept;
      //  void init(const size_t id, const resource *res, const struct settings &info) noexcept;
      //  void reset() noexcept;
      //  size_t load_next(const uint32_t buffer, const size_t frames_count, const uint16_t channels);
      //};

      //struct source_data {
      //  struct source source;
      //  sound_processing_data *queue;

      //  inline source_data() noexcept : queue(nullptr) {}
      //  source_data(const struct source &source, sound_processing_data *queue) noexcept;
      //  void init(const float volume, const size_t frames_count);
      //  void update(const float volume, const size_t frames_count);
      //};

      //// теперь тут вместо queue будет virtual_source
      //// но при этом сюда все равно нужно передать source
      //struct source_data2 {
      //  struct source source;
      //  virtual_source *vsource;
      //  size_t time;
      //  size_t loaded_frames;
      //  size_t processed_frames;

      //  source_data2() noexcept;
      //  void init(virtual_source *vsource) noexcept;
      //  void reset() noexcept;
      //  size_t load_next(const uint32_t buffer, const size_t frames_count, const uint16_t channels);
      //};

      ALCdevice* device;
      ALCcontext* ctx;
      size_t counter;
      size_t queue_size;
      size_t sources_offset;

      volume_set volume;

      //std::vector<source_data> sources;
      std::vector<sound::source> sources;
      //std::unique_ptr<sound_processing_data[]> proc_array;
      std::vector<std::unique_ptr<virtual_source>> vsources;
      std::vector<source_processing*> static_processors;
      std::vector<source_processing*> processors;

      /*background_source *background;
      std::vector<menu_source*> menu_sources;
      std::vector<special_source*> special_sources;
      std::vector<game_source*> game_sources;*/
      // тут у нас сорсы поделятся скорее на эксклюзивные и виртуальные
      // где эксклюзивным выдадут сорс с самого начала
      // а виртуальные будут его ожидать по ходу дела

      //size_t get_new_id();
      //std::tuple<size_t, size_t> find_source_id(const size_t source_id) const;
      //void remove_from_queue(sound_processing_data *queue, const size_t index);
    };
  }
}

#endif
