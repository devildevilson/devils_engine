#ifndef DEVILS_ENGINE_DEMIURG_RESOURCE_BASE_H
#define DEVILS_ENGINE_DEMIURG_RESOURCE_BASE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <bitset>
#include <atomic>
#include "utils/list.h"
#include "utils/safe_handle.h"
//#include <boost/sml.hpp>
//namespace sml = boost::sml;

// так итого мне сейчас нужно:
// 1. добавить к ресурсам состояние парсинга (не нужно по идее)
// 2. сделать ресурс (по крайней мере часть данных оттуда) потокобезопасным (сделал)
// 3. сделать систему загрузки ресурсов 

#define DEMIURG_STATES_LIST2 \
  X(unload) \
  X(memory_load) \

// должны быть еще промежуточные состояния, когда ресурс догружается из состояния в состояние асинхронно
// возможно сами состояния можно сделать атомарными
// скорее всего нам нужно добавить еще одно действие для ресурса: парсинг
// предположительно в нем мы парсим все конфиги и создаем сразу несколько дополнительных ресурсов
// то есть на диске лежит json с массивом конфигов например для монстров
// на этом этапе мы его прочитаем и создадим несколько ресурсов с названием типа
// level1/monsters/config:mon1, level1/monsters/config:mon2
#define DEMIURG_STATES_LIST \
  X(cold)                  \
  X(warm)                  \
  X(hot)                   \

#define DEMIURG_ACTIONS_LIST2 \
  X(unload) \
  X(load_to_memory) \

// по сути состояния у ресурса 3: ресурс на диске, ресурс в памяти, ресурс готов к использованию 
// для большинства ресурсов "ресурс в памяти" и "ресурс готов к использованию" это одно и тоже
// но например картинки мы бы хотели вгружать и выгружать в/из гпу
// вообще между каждым состоянием есть промежуточное переходное состояние
// любая загрузка состоит из: 
// 1. проверки что ресурс есть по заданному адресу (дебаг)
// 2. поиск свободного слота для ресурса
// 3. заполнение этого слота данными
// управлять ресурсами и их загрузкой выгрузкой скорее всего должен отдельный класс
// в нем мы зададим какой ресурс подгрузить сейчас, а также грузятся ли сейчас какие нибудь ресурсы
// так же у ресурсов есть явная иерархия, то есть одни ресурсы зависят от других
// и чтобы до конца загрузить ресурс нужно полностью по графу загрузить все ресурсы-зависмости текущего
// сам по себе ресурс ну никак не может понять от чего он зависит - тут как будто нужна помощь со стороны
// разработчика для каждого типа ресурса определить ресурсы зависимости
// по идее сам ресурс должен хранить информацию о текущем состоянии
#define DEMIURG_ACTIONS_LIST  \
  X(load_cold)                \
  X(load_warm)                \
  X(unload_warm)              \
  X(unload_hot)               \

#define DEMIURG_RESOURCE_FLAGS_LIST \
  X(underlying_owner_of_raw_memory) \
  X(binary)                         \
  X(warm_and_hot_same)              \
  X(force_unload_warm)              \

// для чего?

namespace devils_engine {
namespace demiurg {
  class module_interface;

  // events
  struct loading { utils::safe_handle_t handle; };
  struct unloading { utils::safe_handle_t handle; };

  namespace state {
    enum values {
#define X(name) name,
      DEMIURG_STATES_LIST
#undef X

      count
    };
  }

  namespace resource_flags {
    enum values {
#define X(name) name,
      DEMIURG_RESOURCE_FLAGS_LIST
#undef X

      count
    };
  }

  namespace list_type {
    enum values {
      replacement,
      supplementary,
      exemplary,
      count
    };
  }

  // как бы мы хотели передать ресурс в другое место?
  class resource_interface : 
    public utils::ring::list<resource_interface, list_type::replacement>,
    public utils::ring::list<resource_interface, list_type::supplementary>,
    public utils::ring::list<resource_interface, list_type::exemplary>
  {
  public:
    std::string path;
    std::string_view id;
    std::string_view ext;
    std::string_view module_name;
    std::string_view type;
    std::string_view loading_type;
    size_t loading_type_id;

    const module_interface* module;

    size_t replacing_order;
    size_t raw_size;

    inline resource_interface() noexcept : 
      loading_type_id(0), 
      module(nullptr),
      replacing_order(0),
      raw_size(0),
      _state(0)
    {}
    virtual ~resource_interface() noexcept = default;

    void set_path(std::string path, const std::string_view &root);
    void set(std::string path, const std::string_view &module_name, const std::string_view &id, const std::string_view &ext);

    resource_interface* replacement_next(const resource_interface* ptr) const;
    resource_interface* supplementary_next(const resource_interface* ptr) const;
    resource_interface* exemplary_next(const resource_interface* ptr) const;

    void replacement_add(resource_interface* ptr);
    void supplementary_add(resource_interface* ptr);
    void exemplary_add(resource_interface* ptr);

    void replacement_radd(resource_interface* ptr);
    void supplementary_radd(resource_interface* ptr);
    void exemplary_radd(resource_interface* ptr);

    void replacement_remove();
    void supplementary_remove();
    void exemplary_remove();

    template <typename T>
    bool flag(const T &index) const {
      return flags.test(static_cast<size_t>(index));
    }

    template <typename T>
    void set_flag(const T &index, const bool val) {
      flags.set(static_cast<size_t>(index), val);
    }

#define X(name) virtual void name(const utils::safe_handle_t &handle) = 0;
    DEMIURG_ACTIONS_LIST
#undef X

    void load(const utils::safe_handle_t& handle);
    void unload(const utils::safe_handle_t& handle);
    void force_unload(const utils::safe_handle_t& handle);

    enum state::values state() const;
    bool usable() const;
  protected:
    // по любому будет много флагов у нас для файла, нужно битовое поле
    std::bitset<64> flags;
    std::atomic<int32_t> _state; // атомик? имеет смысл
  };

  void parse_path(
    const std::string& path, 
    std::string_view& module_name,
    std::string_view& file_name,
    std::string_view& ext,
    std::string_view& id
  );
}
}

#endif