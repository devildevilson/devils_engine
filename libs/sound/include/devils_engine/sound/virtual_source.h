#ifndef DEVILS_ENGINE_SOUND_VIRTUAL_SOURCE_H
#define DEVILS_ENGINE_SOUND_VIRTUAL_SOURCE_H

#include <cstddef>
#include <cstdint>
#include "glm/vec3.hpp"
#include "resource.h"

namespace devils_engine {
namespace sound {

// 1 секунда, имеет ли смысл делать больше?
#define DEVILS_ENGINE_SOUND_LOADING_COEFFICIENT 1.0f

struct source {
  constexpr static size_t buffer_size = 2;
  uint32_t handle;
  uint32_t buffers[buffer_size];
  inline source() noexcept : handle(0), buffers{0,0} {}
  inline bool valid() const noexcept { return handle != 0; }
};

// может быть переназвать неймспейс?

// нужно ли приставить к виртуальному сорсу собственные буферы?
// типа отрабатывать обычный сценарий со звуком при его появлении
// скорее нет чем да, лучше немного потрудиться и написать старт по времени
// желательно добиться того чтобы звук не прерывался никак
// + возможно нужно интерполировать положение звука?
// если 30фпс даже берем то вряд ли
class virtual_source {
public:
  struct settings {
    uint32_t type; // тип наверное укажем заранее, так уйдет и is_mono
    float speed;
    float volume;
    float rnd_pitch;
    //float rnd_rate; // не будет
    bool is_loop;
    bool is_mono;
    bool is_needed; // если все источники сейчас заняты, то дропаем звук - подойдет для мелких звуков
    uint32_t force_source; // не уверен что это теперь нужно вообще
    // координаты
    //glm::vec3 pos;
    //glm::vec3 dir;
    //glm::vec3 vel;

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


  virtual ~virtual_source() noexcept = default;

  // тут нужно задать все настроечки

  // стартует звук, потом обнуляет указатель
  // либо его нужно будет обнулить если это залупленный звук
  virtual void set_resource(const resource* res) = 0;
  virtual bool is_valid() const = 0;
  virtual const resource* currently_playing() const = 0;

  //virtual void set(const settings &s) = 0;
  // наверное чаще нам придется задавать скорость нежели чем громкость звука
  // громкость определяется типом звука и настраивается через настроечки
  // мы дополнительно можем ее уменьшить
  virtual void set(float speed, float volume = 1.0f, float rnd_pitch = 0.0f, bool is_loop = false) = 0;
  // dir здесь это не НАПРАВЛЕНИЕ ДВИЖЕНИЯ это НАПРАВЛЕНИЕ ЗВУКА !!!
  // направление движения берется из скорости, направленный звук нужен для эффектов
  // надо чутка переработать это дело
  virtual void set_transform(const glm::vec3 &pos, const glm::vec3 &dir, const glm::vec3 &vel) = 0;
  virtual void set_relative(const bool relative) = 0;

  virtual bool play() = 0;
  virtual bool pause() = 0;
  virtual bool is_playing() const = 0;
  virtual bool is_paused() const = 0;

  // тут мы легко можем возвращать текущее место в звуке
  // и устанавливать положение с которого звук стартанет
  virtual double stat() const = 0;
  virtual bool set_stat(const double place) = 0;
};

enum class processing_state { waiting_source, waiting_resource, processing, paused, finished };

// мы должны как то понять что отсюда нужно забрать сорс? processing_state::finished
// как положить и как обратно добавить?
// сорс наверное нужно проинициализировать 
// так у нас тут еще может быть состояние когда есть сорс, но нет ресурса
class source_processing {
public:
  virtual ~source_processing() noexcept = default;
  virtual processing_state state() const = 0;
  virtual float distance(const glm::vec3 &listener_pos) const = 0;
  virtual void update(const size_t time) = 0;
  virtual void setup_source(const struct source &source) = 0;
  virtual struct source release_source() = 0;
  virtual bool has_source() const = 0;
  virtual void invalidate() = 0;
};



// имеет смысл еще поверх этого дела написать класс который положит в очередь 
// звуки и предоставит какую то обратную связь, например
// нам имеет смысл проверить когда закачивается тот или иной специальный звук
// его приостановить и начать какой нибудь другой
// как эти сорсы сюда передать?
class queue {
public:
  void set_resource(const resource* res);
private:
  size_t counter;
  std::vector<virtual_source*> sources;
};
}
}

#endif