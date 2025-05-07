#ifndef DEVILS_ENGINE_DEMIURG_RESOURCE_BASE_H
#define DEVILS_ENGINE_DEMIURG_RESOURCE_BASE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <bitset>
#include "utils/list.h"
#include "utils/safe_handle.h"
//#include <boost/sml.hpp>
//namespace sml = boost::sml;

#define DEMIURG_STATES_LIST2 \
  X(unload) \
  X(memory_load) \

// должны быть еще промежуточные состояния, когда ресурс догружается из состояния в состояние асинхронно
// возможно сами состояния можно сделать атомарными
#define DEMIURG_STATES_LIST \
  X(cold)                  \
  X(warm)                  \
  X(hot)                   \

#define DEMIURG_ACTIONS_LIST2 \
  X(unload) \
  X(load_to_memory) \

// по сути действия будет только 2, загрузить/выгрузить
// загрузка переводит на стейт ниже в DEMIURG_STATES_LIST2, выгрузка на стейт выше
// при этом должны ли ресурсы следить за свои состоянием?
// или состояние можно сделать тут?
// в основном что мне не нравится - я могу встретить неподготовленную картинку
// что делать? вылетать по ошибке? не хотелось бы, но при этом в продакшене иного выбора как будто нет
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
  protected:
    // по любому будет много флагов у нас для файла, нужно битовое поле
    std::bitset<64> flags;
    int32_t _state;
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