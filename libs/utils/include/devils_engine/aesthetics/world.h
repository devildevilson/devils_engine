#ifndef DEVILS_ENGINE_AESTHETICS_WORLD_H
#define DEVILS_ENGINE_AESTHETICS_WORLD_H

#include <cstddef>
#include <cstdint>
#include <vector>
#include <functional>
#include <atomic>
#include <memory>
#include <string_view>
#include "devils_engine/utils/type_traits.h"
#include "devils_engine/utils/block_allocator.h"
#include <gtl/phmap.hpp>

#include "common.h"

// view_t как переделать?
/*
нужно переделать на использование последовательных id
теперь я могу их задать несколько разных для разных типов структур
почему меня может раздражать std::vector<std::vector<>> ?
лишнее выделение памяти когда у нас есть объект который гарантировано живет вне world
да но лист не может загрузить из памяти данные заранее, так что он проиграет по скорости в несколкьо 100 раз =(
*/

namespace devils_engine {
namespace aesthetics {

class world {
public:
  template <typename... Comp_T>
  using tuple_t = std::tuple<entityid_t, Comp_T*...>;
  //using raw_itr = gtl::flat_hash_map<entityid_t, void*>::const_iterator;
  using raw_itr = std::vector<entityid_t>::iterator;

  class container_interface {
  public:
    std::string_view type_name;
    size_t type_index;
    //size_t size;

    inline container_interface(const std::string_view& type_name, const size_t type_index) noexcept : type_name(type_name), type_index(type_index) {} //, size(0)
    virtual ~container_interface() noexcept = default;
    virtual bool remove(const entityid_t id) noexcept = 0; // придется делать виртуальной?
    virtual void* rawget(const entityid_t id) const noexcept = 0;

    template <typename T>
    T* create(const entityid_t id);
    template <typename T, typename... Args>
    T* create(const entityid_t id, Args&&... args);
    template <typename T>
    T* get(const entityid_t id) const;
  };

  template <typename T>
  class sparce_dence_set : public container_interface {
  public:
    std::vector<T> components;
    std::vector<entityid_t> sparce_set;
    //size_t offset;

    inline sparce_dence_set() noexcept : container_interface(utils::type_name<T>(), aesthetics::component_type_id<T>()) {} //, offset(0)
    bool remove(const entityid_t id) noexcept override;
    void* rawget(const entityid_t id) const noexcept override;

    T* create_comp(const entityid_t id);
    template <typename... Args>
    T* create_comp(const entityid_t id, Args&&... args);
    T* get_comp(const entityid_t id) const;
  };

  // вернет итераторы, будет ходить по каждому энтити у которого есть этот набор компонентов
  template <typename... Comp_T>
  class view_t {
  public:
    using view_tuple_t = tuple_t<Comp_T...>;

    class iterator {
    public:
      //using underlying_itr = gtl::flat_hash_map<entityid_t, void*>::const_iterator;
      using underlying_itr = std::vector<entityid_t>::iterator;
      // берем "случайные" итераторы у хранилища компонентов
      // тут это сработает

      iterator(const class world* world, underlying_itr it, underlying_itr end) noexcept;

      view_tuple_t operator*() const;
      view_tuple_t get() const;
      iterator& operator++();
      iterator operator++(int);

      friend bool operator==(const iterator& a, const iterator& b) noexcept { return a.it == b.it; }
      friend bool operator!=(const iterator& a, const iterator& b) noexcept { return !(a == b); }
    private:
      const class world* world;
      underlying_itr it;
      underlying_itr end;
    };

    view_t(const class world* world) noexcept;

    iterator begin() const;
    iterator end() const;
    template <typename F>
    void each(const F &f) const;
  private:
    const class world* world;
  };

  template <typename... Comp_T>
  class query_t {
  public:
    using container_t = std::vector<entityid_t>;
    using query_tuple_t = tuple_t<Comp_T...>;

    class iterator {
    public:
      using underlying_t = container_t::iterator;

      iterator(class world* world, underlying_t itr) noexcept;

      query_tuple_t operator*() const;
      query_tuple_t get() const;
      iterator& operator++();
      iterator operator++(int);

      friend bool operator==(const iterator& a, const iterator& b) noexcept { return a.world == b.world && a.itr == b.itr; }
      friend bool operator!=(const iterator& a, const iterator& b) noexcept { return !(a == b); }
    private:
      class world* world;
      underlying_t itr;
    };

    // придется добавить какую то такую штуку для того чтобы реагировать на добавление удаление энтити
    // если без этой штуки то как? скорее всего и так и сяк придется сабскрайбить на события
    // это универсальный способ доступа к таким вещам
    template <typename T>
    class query_receiver : public basic_reciever<T> {
    public:
      using event_t = T;

      query_receiver(const class world* world, query_t<Comp_T...>* q) noexcept;
      query_receiver(const query_receiver& copy) noexcept = delete;
      query_receiver(query_receiver&& move) noexcept = default;
      query_receiver& operator=(const query_receiver& copy) noexcept = delete;
      query_receiver& operator=(query_receiver&& move) noexcept = default;

      void receive(const T& event) override;
    private:
      const class world* world;
      query_t<Comp_T...>* q;
    };

    template <typename T>
    class query_remover : public basic_reciever<T> {
    public:
      using event_t = T;

      query_remover(const class world* world, query_t<Comp_T...>* q) noexcept;
      query_remover(const query_remover& copy) noexcept = delete;
      query_remover(query_remover&& move) noexcept = default;
      query_remover& operator=(const query_remover& copy) noexcept = delete;
      query_remover& operator=(query_remover&& move) noexcept = default;

      void receive(const T& event) override;
    private:
      const class world* world;
      query_t<Comp_T...>* q;
    };

    query_t(class world* world) noexcept;
    ~query_t() noexcept;

    query_t(const query_t& copy) noexcept = delete;
    query_t(query_t&& move) noexcept;
    query_t& operator=(const query_t& copy) noexcept = delete;
    query_t& operator=(query_t&& move) noexcept;

    iterator begin() const;
    iterator end() const;
    template <typename F>
    void each(const F &f) const;
    container_t& array();
    const container_t& array() const;

    void add_entity(const entityid_t id);
    void remove_entity(const entityid_t id);
  private:
    class world* world;
    container_t container;
    std::tuple<query_receiver<create_component_event<Comp_T>>...> receivers;
    std::tuple<query_remover<remove_component_event<Comp_T>>...> removers;
  };

  template <typename... Comp_T>
  class lazy_view_t {
  public:
    using lazy_view_tuple_t = tuple_t<Comp_T...>;
    using collection_t = gtl::flat_hash_set<entityid_t>;

    class iterator {
    public:
      using underlying_itr = collection_t::const_iterator;

      iterator(const class world* world, underlying_itr it) noexcept;

      lazy_view_tuple_t operator*() const;
      lazy_view_tuple_t get() const;
      iterator& operator++();
      iterator operator++(int);

      friend bool operator==(const iterator& a, const iterator& b) noexcept { return a.it == b.it; }
      friend bool operator!=(const iterator& a, const iterator& b) noexcept { return !(a == b); }
    private:
      const class world* world;
      underlying_itr it;
    };

    lazy_view_t(const class world* world) noexcept;

    iterator begin() const;
    iterator end() const;
    template <typename F>
    void each(const F& f) const;
  private:
    const class world* world;
    collection_t entities;
  };

  template <typename... Comp_T>
  class lazy_query_t {
  public:
    using lazy_query_tuple_t = tuple_t<Comp_T...>;
    using container_t = std::vector<entityid_t>;
    
    class iterator {
    public:
      using underlying_t = container_t::iterator;

      iterator(class world* world, underlying_t itr) noexcept;

      lazy_query_tuple_t operator*() const;
      lazy_query_tuple_t get() const;
      iterator& operator++();
      iterator operator++(int);

      friend bool operator==(const iterator& a, const iterator& b) noexcept { return a.world == b.world && a.itr == b.itr; }
      friend bool operator!=(const iterator& a, const iterator& b) noexcept { return !(a == b); }
    private:
      class world* world;
      underlying_t itr;
    };

    template <typename T>
    class query_receiver : public basic_reciever<T> {
    public:
      using event_t = T;

      query_receiver(const class world* world, lazy_query_t<Comp_T...>* q) noexcept;
      query_receiver(const query_receiver& copy) noexcept = delete;
      query_receiver(query_receiver&& move) noexcept = default;
      query_receiver& operator=(const query_receiver& copy) noexcept = delete;
      query_receiver& operator=(query_receiver&& move) noexcept = default;

      void receive(const T& event) override;
    private:
      const class world* world;
      lazy_query_t<Comp_T...>* q;
    };

    template <typename T>
    class query_remover : public basic_reciever<T> {
    public:
      using event_t = T;

      query_remover(const class world* world, lazy_query_t<Comp_T...>* q) noexcept;
      query_remover(const query_remover& copy) noexcept = delete;
      query_remover(query_remover&& move) noexcept = default;
      query_remover& operator=(const query_remover& copy) noexcept = delete;
      query_remover& operator=(query_remover&& move) noexcept = default;

      void receive(const T& event) override;
    private:
      const class world* world;
      lazy_query_t<Comp_T...>* q;
    };

    lazy_query_t(class world* world) noexcept;
    ~lazy_query_t() noexcept;

    lazy_query_t(const lazy_query_t& copy) noexcept = delete;
    lazy_query_t(lazy_query_t&& move) noexcept;
    lazy_query_t& operator=(const lazy_query_t& copy) noexcept = delete;
    lazy_query_t& operator=(lazy_query_t&& move) noexcept;

    iterator begin() const;
    iterator end() const;
    template <typename F>
    void each(const F& f) const;
    container_t& array();
    const container_t& array() const;

    void add_entity(const entityid_t id);
    void remove_entity(const entityid_t id);
  private:
    class world* world;
    container_t container;
    std::tuple<query_receiver<create_component_event<Comp_T>>...> receivers;
    std::tuple<query_remover<remove_component_event<Comp_T>>...> removers;
  };

  class entity {
  public:
    using components_arr = std::vector<std::pair<size_t, void*>>;

    entity() noexcept;
    entity(const entityid_t id, class world* world) noexcept;
    ~entity() noexcept; // DOES NOT REMOVE COMPONENTS, use 'clear'

    entity(const entity& copy) noexcept = default;
    entity(entity&& move) noexcept = default;
    entity& operator=(const entity& copy) noexcept = default;
    entity& operator=(entity&& move) noexcept = default;

    entityid_t id() const noexcept;
    class world* world() const noexcept;
    const components_arr& components() const noexcept;
    bool valid() const noexcept;
    bool exists() const noexcept; // at least one component

    template <typename T, typename... Args>
    T* create(Args&&... args);
    template <typename T>
    bool remove();
    template <typename T>
    T* get() const;
    template <typename T>
    auto get_tuple() const -> tuple_t<T>;
    template <typename T, typename... Comp_T>
    auto get_tuple() const -> tuple_t<T, Comp_T...>;
    template <typename T>
    bool has() const;
    template <typename T, typename T2, typename... Comp_T>
    bool has() const;
    template <typename T, typename T2, typename... Comp_T>
    bool has_any() const;

    void clear(); // remove all components
    void remove(); // remove entity and remove all components
  private:
    entityid_t m_id;
    class world* m_world;
  };

  world() noexcept;
  world(const world& copy) noexcept = delete;
  world(world&& move) noexcept = default;
  world& operator=(const world& copy) noexcept = delete;
  world& operator=(world&& move) noexcept = default;
  entityid_t gen_entityid();
  void remove_entity(const entityid_t id);
  entity::components_arr find_components(const entityid_t id) const;
  entity get_entity(const entityid_t id) const;
  bool exists(const entityid_t id) const; // has at least one component
  bool raw_has(const entityid_t id, const size_t type_id) const;
  void* raw_get(const entityid_t id, const size_t type_id) const;
  bool raw_remove(const entityid_t id, const size_t type_id);
  void clear(const entityid_t id);

  /*template <typename T>
  void create_allocator(const size_t block_size);*/
  template <typename T>
  bool is_allocators_exist() const;
  template <typename T, typename T2, typename... Comp_T>
  bool is_allocators_exist() const;

  template<typename T>
  sparce_dence_set<T>* get_or_create_allocator(const size_t block_size);
  template<typename T>
  sparce_dence_set<T>* get_allocator() const;

  template <typename T>
  T* create(const entityid_t id);
  template <typename T, typename... Args>
  T* create(const entityid_t id, Args&&... args);
  template <typename T>
  bool remove(const entityid_t id);
  template <typename T>
  T* get(const entityid_t id) const;
  template <typename T>
  auto get_tuple(const entityid_t id) const -> tuple_t<T>;
  template <typename T, typename... Comp_T>
  auto get_tuple(const entityid_t id) const -> tuple_t<T, Comp_T...>;
  template <typename T>
  bool has(const entityid_t id) const;
  template <typename T, typename T2, typename... Comp_T>
  bool has(const entityid_t id) const;
  template <typename T, typename T2, typename... Comp_T>
  bool has_any(const entityid_t id) const;
  template <typename T>
  size_t count() const;
  template <typename T>
  raw_itr raw_begin() const;
  template <typename T>
  raw_itr raw_end() const;

  template <typename Event_T, typename T>
    requires(std::derived_from<T, basic_reciever<Event_T>>)
  void subscribe(T* ptr);
  template <typename Event_T, typename T>
    requires(std::derived_from<T, basic_reciever<Event_T>>)
  bool unsubscribe(T* ptr);
  template <typename Event_T>
  void emit(const Event_T& event) const;

  template <typename... Comp_T>
  view_t<Comp_T...> view() const;
  template <typename... Comp_T>
  lazy_view_t<Comp_T...> lazy_view() const;

  template <typename... Comp_T>
  query_t<Comp_T...> query();
  template <typename... Comp_T>
  lazy_query_t<Comp_T...> lazy_query();

  template <typename T, typename... Args>
  T* create_unique(Args&&... args);
  template <typename T>
  bool remove_unique(T* ptr);
  //template <typename... Comp_T>
  //bool remove_query(query_t<Comp_T...>* ptr);

  template <typename T, typename... Args>
    requires(std::derived_from<T, basic_system>)
  T* create_system(Args&&... args);
  template <typename T>
    requires(std::derived_from<T, basic_system>)
  bool remove_system(T* ptr);
private:
  size_t cur_index;
  std::vector<entityid_t> removed_entities;
  std::vector<std::unique_ptr<container_interface>> containers;
  std::vector<std::vector<accurate_remover*>> subscribers;
  std::vector<std::unique_ptr<basic_container>> arbitrary_container;

  //template<typename T>
  //gtl::flat_hash_map<size_t, std::unique_ptr<container_interface>>::iterator get_or_create_allocator(const size_t block_size, const size_t alloc_size = sizeof(T), const size_t alignment = alignof(T));
  /*template<typename T>
  sparce_dence_set<T>* get_or_create_allocator(const size_t block_size);
  template<typename T>
  sparce_dence_set<T>* get_allocator() const;*/
};





template <typename... Comp_T>
world::view_t<Comp_T...>::iterator::iterator(const class world* world, world::view_t<Comp_T...>::iterator::underlying_itr it, world::view_t<Comp_T...>::iterator::underlying_itr end) noexcept : world(world), it(it), end(end) {
  if (it == end) return;
  bool ret = world->has<Comp_T...>(*it);
  for (; it != end && !ret; ++it) { ret = world->has<Comp_T...>(*it); }
}
template <typename... Comp_T>
world::view_t<Comp_T...>::view_tuple_t world::view_t<Comp_T...>::iterator::operator*() const { return world->get_tuple<Comp_T...>(*it); }
template <typename... Comp_T>
world::view_t<Comp_T...>::view_tuple_t world::view_t<Comp_T...>::iterator::get() const { return world->get_tuple<Comp_T...>(*it); }
template <typename... Comp_T>
world::view_t<Comp_T...>::iterator& world::view_t<Comp_T...>::iterator::operator++() {
  bool ret = false;
  for (; it != end && !ret; ++it) { ret = world->has<Comp_T...>(*it); }
  return *this;
}
template <typename... Comp_T>
world::view_t<Comp_T...>::iterator world::view_t<Comp_T...>::iterator::operator++(int) {
  auto i = *this;
  ++(*this);
  return i;
}

template <typename T>
std::tuple<size_t, world::raw_itr> get_minimum_raw_begin(const class world* world) {
  return std::make_tuple(world->count<T>(), world->raw_begin<T>());
}

template <typename T, typename T2, typename... Comp_T>
std::tuple<size_t, world::raw_itr> get_minimum_raw_begin(const class world* world) {
  const size_t count = world->count<T>();
  const auto t = get_minimum_raw_begin<T2, Comp_T...>(world);
  if (std::get<0>(t) < count) return t;
  return std::make_tuple(count, world->raw_begin<T>());
}

template <typename T>
std::tuple<size_t, world::raw_itr> get_minimum_raw_end(const class world* world) {
  return std::make_tuple(world->count<T>(), world->raw_end<T>());
}

template <typename T, typename T2, typename... Comp_T>
std::tuple<size_t, world::raw_itr> get_minimum_raw_end(const class world* world) {
  const size_t count = world->count<T>();
  const auto t = get_minimum_raw_end<T2, Comp_T...>(world);
  if (std::get<0>(t) < count) return t;
  return std::make_tuple(count, world->raw_end<T>());
}

template <typename... Comp_T>
world::view_t<Comp_T...>::view_t(const class world* world) noexcept : world(world) {}
template <typename... Comp_T>
world::view_t<Comp_T...>::iterator world::view_t<Comp_T...>::begin() const {
  const auto [count1, begin] = get_minimum_raw_begin<Comp_T...>(world);
  const auto [count2, end] = get_minimum_raw_end<Comp_T...>(world);
  return iterator(world, begin, end);
}
template <typename... Comp_T>
world::view_t<Comp_T...>::iterator world::view_t<Comp_T...>::end() const {
  const auto [count2, end] = get_minimum_raw_end<Comp_T...>(world);
  return iterator(world, end, end);
}
template <typename... Comp_T>
template <typename F>
void world::view_t<Comp_T...>::each(const F& f) const {
  for (const auto &t : *this) { std::apply(f, t); }
}

template <typename... Comp_T>
template <typename T>
world::query_t<Comp_T...>::query_receiver<T>::query_receiver(const class world* world, query_t<Comp_T...>* q) noexcept : world(world), q(q) {}
template <typename... Comp_T>
template <typename T>
void world::query_t<Comp_T...>::query_receiver<T>::receive(const T& event) {
  const auto& t = world->get_tuple<Comp_T...>(event.id);
  if (std::apply(&all_of_not_null<entityid_t, Comp_T*...>, t)) {
    q->add_entity(event.id);
  }
}

template <typename... Comp_T>
template <typename T>
world::query_t<Comp_T...>::query_remover<T>::query_remover(const class world* world, query_t<Comp_T...>* q) noexcept : world(world), q(q) {}
template <typename... Comp_T>
template <typename T>
void world::query_t<Comp_T...>::query_remover<T>::receive(const T& event) {
  const auto& t = world->get_tuple<Comp_T...>(event.id);
  if (std::apply(&all_of_not_null<entityid_t, Comp_T*...>, t)) {
    q->remove_entity(event.id);
  }
}

template <size_t N, typename... Ts>
static void subscribe_all(world* w, std::tuple<Ts...> &t) {
  using tuple_t = std::tuple<Ts...>;
  if constexpr (N < std::tuple_size_v<tuple_t>) {
    using evt_t = std::tuple_element_t<N, tuple_t>::event_t;
    w->subscribe<evt_t>(std::addressof(std::get<N>(t)));
    subscribe_all<N + 1>(w, t);
  }
}

template <size_t N, typename... Ts>
static void unsubscribe_all(world* w, std::tuple<Ts...>& t) {
  using tuple_t = std::tuple<Ts...>;
  if constexpr (N < std::tuple_size_v<tuple_t>) {
    using evt_t = std::tuple_element_t<N, tuple_t>::event_t;
    w->unsubscribe<evt_t>(std::addressof(std::get<N>(t)));
    unsubscribe_all<N + 1>(w, t);
  }
}

template <typename... Comp_T>
world::query_t<Comp_T...>::iterator::iterator(class world* world, underlying_t itr) noexcept : world(world), itr(itr) {}

template <typename... Comp_T>
world::query_t<Comp_T...>::query_tuple_t world::query_t<Comp_T...>::iterator::operator*() const {
  const auto ent_id = *itr;
  return world->get_tuple<Comp_T...>(ent_id);
}

template <typename... Comp_T>
world::query_t<Comp_T...>::query_tuple_t world::query_t<Comp_T...>::iterator::get() const {
  const auto ent_id = *itr;
  return world->get_tuple<Comp_T...>(ent_id);
}

template <typename... Comp_T>
world::query_t<Comp_T...>::iterator& world::query_t<Comp_T...>::iterator::operator++() {
  ++itr;
  return *this;
}

template <typename... Comp_T>
world::query_t<Comp_T...>::iterator world::query_t<Comp_T...>::iterator::operator++(int) {
  auto cur = *this;
  ++itr;
  return cur;
}

template <typename... Comp_T>
world::query_t<Comp_T...>::query_t(class world* world) noexcept :
  world(world), receivers(std::make_tuple(query_receiver<create_component_event<Comp_T>>(world, this)...)), removers(std::make_tuple(query_remover<remove_component_event<Comp_T>>(world, this)...))
{
  subscribe_all<0>(world, receivers);
  subscribe_all<0>(world, removers);
  const auto& view = world->view<Comp_T...>();
  for (const auto &t : view) {
    add_entity(t);
  }
}
template <typename... Comp_T>
world::query_t<Comp_T...>::~query_t() noexcept {
  if (world == nullptr) return;

  unsubscribe_all<0>(world, receivers);
  unsubscribe_all<0>(world, removers);
}

template <typename... Comp_T>
world::query_t<Comp_T...>::query_t(query_t&& move) noexcept :
  world(move.world),
  container(std::move(move.container)),
  receivers(std::move(move.receivers)),
  removers(std::move(move.removers))
{
  move.world = nullptr;
}

template <typename... Comp_T>
world::query_t<Comp_T...>& world::query_t<Comp_T...>::operator=(query_t&& move) noexcept {
  world = move.world;
  container = std::move(move.container);
  receivers = std::move(move.receivers);
  removers = std::move(move.removers);
  move.world = nullptr;
  return *this;
}

template <typename... Comp_T>
world::query_t<Comp_T...>::iterator world::query_t<Comp_T...>::begin() const { return iterator(world, container.begin()); }
template <typename... Comp_T>
world::query_t<Comp_T...>::iterator world::query_t<Comp_T...>::end() const { return iterator(world, container.end()); }
template <typename... Comp_T>
template <typename F>
void world::query_t<Comp_T...>::each(const F &f) const {
  for (const auto& t : container) { std::apply(f, world->get_tuple<Comp_T...>(t)); }
}

template <typename... Comp_T>
world::query_t<Comp_T...>::container_t& world::query_t<Comp_T...>::array() { return container; }
template <typename... Comp_T>
const world::query_t<Comp_T...>::container_t& world::query_t<Comp_T...>::array() const { return container; }

//template <typename... Comp_T>
//void world::query_t<Comp_T...>::add_entity(const query_tuple_t& t) {
//  auto itr = std::lower_bound(container.begin(), container.end(), t, [] (const auto &a, const auto &b) {
//    std::less<entityid_t> l;
//    return l(std::get<0>(a), std::get<0>(b));
//  });
//
//  container.insert(itr, t);
//}
//
//template <typename... Comp_T>
//void world::query_t<Comp_T...>::remove_entity(const query_tuple_t& t) {
//  auto itr = std::lower_bound(container.begin(), container.end(), t, [](const auto& a, const auto& b) {
//    std::less<entityid_t> l;
//    return l(std::get<0>(a), std::get<0>(b));
//  });
//
//  container.erase(itr);
//}

template <typename... Comp_T>
void world::query_t<Comp_T...>::add_entity(const entityid_t id) {
  auto itr = std::lower_bound(container.begin(), container.end(), id, [](const auto& a, const auto& b) {
    std::less<size_t> l;
    return l(get_entityid_index(a), get_entityid_index(b));
  });

  container.insert(itr, id);
}

template <typename... Comp_T>
void world::query_t<Comp_T...>::remove_entity(const entityid_t id) {
  auto itr = std::lower_bound(container.begin(), container.end(), id, [](const auto& a, const auto& b) {
    std::less<size_t> l;
    return l(get_entityid_index(a), get_entityid_index(b));
  });

  container.erase(itr);
}

template <typename... Comp_T>
world::lazy_view_t<Comp_T...>::iterator::iterator(const class world* world, underlying_itr it) noexcept : world(world), it(it) {}

template <typename... Comp_T>
world::lazy_view_t<Comp_T...>::lazy_view_tuple_t world::lazy_view_t<Comp_T...>::iterator::operator*() const { return world->get_tuple<Comp_T...>((*it)); }
template <typename... Comp_T>
world::lazy_view_t<Comp_T...>::lazy_view_tuple_t world::lazy_view_t<Comp_T...>::iterator::get() const { return world->get_tuple<Comp_T...>((*it)); }
template <typename... Comp_T>
world::lazy_view_t<Comp_T...>::iterator& world::lazy_view_t<Comp_T...>::iterator::operator++() { ++it; }
template <typename... Comp_T>
world::lazy_view_t<Comp_T...>::iterator world::lazy_view_t<Comp_T...>::iterator::operator++(int) {
  auto cur = it;
  ++it;
  return iterator(world, cur);
}

static void make_unique_entityids(const class world* world, gtl::flat_hash_set<entityid_t>& ids) {}

template <typename T, typename... Comp_T>
static void make_unique_entityids(const class world* world, gtl::flat_hash_set<entityid_t> &ids) {
  auto stor = world->get_allocator<T>();
  //auto begin = world->raw_begin<T>();
  //auto end = world->raw_end<T>();
  for (size_t i = 0; i < stor->sparce_set.size(); ++i) {
    const auto ent_id = make_entityid(i, get_entityid_version(stor->sparce_set[i]));
    ids.emplace(ent_id);
  }

  make_unique_entityids<Comp_T...>(world, ids);
}

// подписаться? не для вью
template <typename... Comp_T>
world::lazy_view_t<Comp_T...>::lazy_view_t(const class world* world) noexcept : world(world) {
  // собрать все id здесь? видимо
  make_unique_entityids<Comp_T...>(world, entities);
}

template <typename... Comp_T>
world::lazy_view_t<Comp_T...>::iterator world::lazy_view_t<Comp_T...>::begin() const { return iterator(world, entities.begin()); }
template <typename... Comp_T>
world::lazy_view_t<Comp_T...>::iterator world::lazy_view_t<Comp_T...>::end() const { return iterator(world, entities.end()); }
template <typename... Comp_T>
template <typename F>
void world::lazy_view_t<Comp_T...>::each(const F& f) const {
  for (auto it = begin(); it != end(); ++it) {
    std::apply(f, it.get());
  }
}

template <typename... Comp_T>
world::lazy_query_t<Comp_T...>::iterator::iterator(class world* world, underlying_t itr) noexcept : world(world), itr(itr) {}

template <typename... Comp_T>
world::lazy_query_t<Comp_T...>::lazy_query_tuple_t world::lazy_query_t<Comp_T...>::iterator::operator*() const {
  const auto ent_id = *itr;
  return world->get_tuple<Comp_T...>(ent_id);
}

template <typename... Comp_T>
world::lazy_query_t<Comp_T...>::lazy_query_tuple_t world::lazy_query_t<Comp_T...>::iterator::get() const {
  const auto ent_id = *itr;
  return world->get_tuple<Comp_T...>(ent_id);
}

template <typename... Comp_T>
world::lazy_query_t<Comp_T...>::iterator& world::lazy_query_t<Comp_T...>::iterator::operator++() {
  ++itr;
  return *this;
}

template <typename... Comp_T>
world::lazy_query_t<Comp_T...>::iterator world::lazy_query_t<Comp_T...>::iterator::operator++(int) {
  auto cur = *this;
  ++itr;
  return cur;
}

template <typename... Comp_T>
template <typename T>
world::lazy_query_t<Comp_T...>::query_receiver<T>::query_receiver(const class world* world, lazy_query_t<Comp_T...>* q) noexcept : world(world), q(q) {}

template <typename... Comp_T>
template <typename T>
void world::lazy_query_t<Comp_T...>::query_receiver<T>::receive(const T& event) {
  // есть хотя бы один компонент - добавляем
  q->add_entity(event.id);
}


template <typename... Comp_T>
template <typename T>
world::lazy_query_t<Comp_T...>::query_remover<T>::query_remover(const class world* world, lazy_query_t<Comp_T...>* q) noexcept : world(world), q(q) {}

template <typename... Comp_T>
template <typename T>
void world::lazy_query_t<Comp_T...>::query_remover<T>::receive(const T& event) {
  const bool any = world->has_any<Comp_T...>;
  if (!any) {
    q->remove_entity(event.id);
  }
}

template <typename... Comp_T>
world::lazy_query_t<Comp_T...>::lazy_query_t(class world* world) noexcept :
  world(world), receivers(std::make_tuple(query_receiver<create_component_event<Comp_T>>(world, this)...)), removers(std::make_tuple(query_remover<remove_component_event<Comp_T>>(world, this)...))
{
  subscribe_all<0>(world, receivers);
  subscribe_all<0>(world, removers);
  const auto view = world->lazy_view<Comp_T...>();
  for (const auto& t : view) {
    add_entity(t);
  }
}

template <typename... Comp_T>
world::lazy_query_t<Comp_T...>::~lazy_query_t() noexcept {
  if (world == nullptr) return;

  unsubscribe_all<0>(world, receivers);
  unsubscribe_all<0>(world, removers);
}

template <typename... Comp_T>
world::lazy_query_t<Comp_T...>::lazy_query_t(lazy_query_t&& move) noexcept :
  world(move.world),
  container(std::move(move.container)),
  receivers(std::move(move.receivers)),
  removers(std::move(move.removers))
{
  move.world = nullptr;
}

template <typename... Comp_T>
world::lazy_query_t<Comp_T...>& world::lazy_query_t<Comp_T...>::operator=(lazy_query_t&& move) noexcept {
  world = move.world;
  container = std::move(move.container);
  receivers = std::move(move.receivers);
  removers = std::move(move.removers);
  move.world = nullptr;
  return *this;
}

template <typename... Comp_T>
world::lazy_query_t<Comp_T...>::iterator world::lazy_query_t<Comp_T...>::begin() const { return iterator(world, container.begin()); }
template <typename... Comp_T>
world::lazy_query_t<Comp_T...>::iterator world::lazy_query_t<Comp_T...>::end() const { return iterator(world, container.end()); }

template <typename... Comp_T>
template <typename F>
void world::lazy_query_t<Comp_T...>::each(const F& f) const {
  for (const auto &t : container) {
    std::apply(f, world->get_tuple<Comp_T...>(t));
  }
}

template <typename... Comp_T>
world::lazy_query_t<Comp_T...>::container_t& world::lazy_query_t<Comp_T...>::array() { return container; }
template <typename... Comp_T>
const world::lazy_query_t<Comp_T...>::container_t& world::lazy_query_t<Comp_T...>::array() const { return container; }


template <typename... Comp_T>
void world::lazy_query_t<Comp_T...>::add_entity(const entityid_t id) {
  auto itr = std::lower_bound(container.begin(), container.end(), id, [](const auto& a, const auto& b) {
    std::less<size_t> l;
    return l(get_entityid_index(a), get_entityid_index(b));
  });

  container.insert(itr, id);
}

template <typename... Comp_T>
void world::lazy_query_t<Comp_T...>::remove_entity(const entityid_t id) {
  auto itr = std::lower_bound(container.begin(), container.end(), id, [](const auto& a, const auto& b) {
    std::less<size_t> l;
    return l(get_entityid_index(a), get_entityid_index(b));
  });

  container.erase(itr);
}

static bool entity_component_predicate(const std::pair<size_t, void*> &pair, const size_t &type_id) {
  std::less<size_t> l;
  return l(pair.first, type_id);
}

template <typename T, typename... Args>
T* world::entity::create(Args&&... args) {
  return m_world->create(m_id, std::forward<Args>(args)...);
}

template <typename T>
bool world::entity::remove() {
  return m_world->remove<T>(m_id);
}

template <typename T>
T* world::entity::get() const {
  return m_world->get<T>(m_id);
}

template <typename T>
auto world::entity::get_tuple() const -> tuple_t<T> {
  return m_world->get_tuple<T>(m_id);
}

template <typename T, typename... Comp_T>
auto world::entity::get_tuple() const -> tuple_t<T, Comp_T...> {
  return m_world->get_tuple<T, Comp_T...>(m_id);
}

template <typename T>
bool world::entity::has() const { return get<T>() != nullptr; }

template <typename T, typename T2, typename... Comp_T>
bool world::entity::has() const {
  return m_world->has<T, T2, Comp_T...>(m_id);
}

template <typename T, typename T2, typename... Comp_T>
bool world::entity::has_any() const {
  return m_world->has_any<T, T2, Comp_T...>(m_id);
}

//template <typename T>
//void world::create_allocator(const size_t block_size) {
//  get_or_create_allocator<T>(block_size);
//}

template <typename T>
bool world::is_allocators_exist() const {
  return get_allocator<T>() != nullptr;
}

template <typename T, typename T2, typename... Comp_T>
bool world::is_allocators_exist() const {
  return is_allocators_exist<T>() && is_allocators_exist<T2, Comp_T...>();
}

template <typename T>
T* world::create(const entityid_t id) {
  auto stor = get_or_create_allocator<T>(sizeof(T) * 250);
  auto ptr = stor->create(id);
  if (ptr == nullptr) return nullptr;
  emit(create_component_event<T>{id, ptr});
  return ptr;
}

template <typename T, typename... Args>
T* world::create(const entityid_t id, Args&&... args) {
  auto stor = get_or_create_allocator<T>(sizeof(T)*250);
  auto ptr = stor->create(id, std::forward<Args>(args)...);
  if (ptr == nullptr) return nullptr;
  emit(create_component_event<T>{id, ptr});
  return ptr;
}

template <typename T>
bool world::remove(const entityid_t id) {
  auto stor = get_allocator<T>();
  if (stor == nullptr) return false;

  auto ptr = stor->get(id);
  if (ptr == nullptr) return false;
  emit(remove_component_event<T>{id, ptr});

  stor->remove(id);
  return true;
}

template <typename T>
T* world::get(const entityid_t id) const {
  auto stor = get_allocator<T>();
  if (stor == nullptr) return nullptr;
  return stor->get(id);
}

template <typename T>
auto world::get_tuple(const entityid_t id) const -> tuple_t<T> {
  return std::make_tuple(id, get<T>(id));
}

template <typename T, typename... Comp_T>
auto world::get_tuple(const entityid_t id) const -> tuple_t<T, Comp_T...> {
  return std::make_tuple(id, get<T>(id), get<Comp_T>(id)...);
}

template <typename T>
bool world::has(const entityid_t id) const {
  return get<T>(id) != nullptr;
}

template <typename T, typename T2, typename... Comp_T>
bool world::has(const entityid_t id) const {
  return has<T>(id) && has<T2, Comp_T...>(id);
}

template <typename T, typename T2, typename... Comp_T>
bool world::has_any(const entityid_t id) const {
  return has<T>(id) || has_any<T2, Comp_T...>(id);
}

template <typename T>
size_t world::count() const {
  auto stor = get_allocator<T>();
  return stor != nullptr ? stor->size : 0;
}

template <typename T>
world::raw_itr world::raw_begin() const {
  //auto itr = containers.find(utils::type_id<T>());
  //return itr->second->components.begin();
  auto stor = get_or_create_allocator<T>(sizeof(T) * 250);
  return stor->sparce_set.begin();
}

template <typename T>
world::raw_itr world::raw_end() const {
  //auto itr = containers.find(utils::type_id<T>());
  //return itr->second->components.end();
  auto stor = get_or_create_allocator<T>(sizeof(T) * 250);
  return stor->sparce_set.end();
}

template <typename Event_T, typename T>
  requires(std::derived_from<T, basic_reciever<Event_T>>)
void world::subscribe(T* ptr) {
  const size_t event_id = aesthetics::event_type_id<Event_T>();
  if (subscribers.size() <= event_id) subscribers.resize(event_id+1);

  subscribers[event_id].push_back(static_cast<accurate_remover*>(ptr));

  //if constexpr (std::is_same_v<update_event, Event_T>) {
  //  auto better_ptr = static_cast<basic_reciever<Event_T>*>(ptr);
  //  //if (systems == nullptr) systems = better_ptr;
  //  //else systems->add(better_ptr);
  //  auto empty_el = systems.get<Event_T>();
  //  empty_el->add(better_ptr);
  //} else {
  //  constexpr size_t type_id = utils::type_id<Event_T>();

  //  auto itr = subscribers.find(type_id);
  //  if (itr == subscribers.end()) {
  //    //itr = subscribers.emplace(std::make_pair(type_id, std::vector<void*>{})).first;
  //    itr = subscribers.emplace(std::make_pair(type_id, basic_reciever_container<Event_T>())).first;
  //  }

  //  auto better_ptr = static_cast<basic_reciever<Event_T>*>(ptr);
  //  // блен, мне нужно обязательно поддерживать порядок... зачем?
  //  // увы забыл... да это сделано для того чтобы запихивать сюда же системы - они должны отрабатывать исключительно по порядку
  //  // реально вместо вектора можно тогда сделать лист
  //  // либо выделить функцию апдейт
  //  //auto subs_itr = std::lower_bound(itr->second.begin(), itr->second.end(), better_ptr);
  //  //itr->second.insert(subs_itr, better_ptr);
  //  //itr->second.push_back(better_ptr);

  //  auto empty_el = itr->second.get<Event_T>();
  //  empty_el->add(better_ptr);
  //}
}

template <typename Event_T, typename T>
  requires(std::derived_from<T, basic_reciever<Event_T>>)
bool world::unsubscribe(T* ptr) {
  const size_t event_id = aesthetics::event_type_id<Event_T>();
  if (event_id >= subscribers.size()) return false;

  auto itr = std::find(subscribers[event_id].begin(), subscribers[event_id].end(), static_cast<basic_reciever<Event_T>*>(ptr));
  if (itr == subscribers[event_id].end()) return false;

  subscribers[event_id].erase(itr);

  //if constexpr (std::is_same_v<update_event, Event_T>) {
  //  auto better_ptr = static_cast<basic_reciever<Event_T>*>(ptr);
  //  //if (systems == better_ptr) systems = better_ptr->next(better_ptr);
  //  better_ptr->unsubscribe();
  //} else {
  //  auto itr = subscribers.find(utils::type_id<Event_T>());
  //  if (itr == subscribers.end()) return false;

  //  auto better_ptr = static_cast<basic_reciever<Event_T>*>(ptr);
  //  //auto subs_itr = std::lower_bound(itr->second.begin(), itr->second.end(), better_ptr);
  //  //auto subs_itr = std::find(itr->second.begin(), itr->second.end(), better_ptr);
  //  //itr->second.erase(subs_itr);
  //  //if (itr->second == better_ptr) itr->second = better_ptr->next(better_ptr);
  //  better_ptr->unsubscribe(); // достаточно отписаться
  //  return true;
  //}
}

template <typename Event_T>
void world::emit(const Event_T& event) const {
  const size_t event_id = aesthetics::event_type_id<Event_T>();
  if (event_id >= subscribers.size()) return;

  for (auto ptr : subscribers[event_id]) {
    auto local_ptr = static_cast<basic_reciever<Event_T>*>(ptr);
    local_ptr->receive(event);
  }

  //if constexpr (std::is_same_v<update_event, Event_T>) {
  //  /*for (auto p = systems; p != nullptr; p = p->next(systems)) {
  //    p->receive(event);
  //  }*/
  //  auto empty_el = systems.get<Event_T>();
  //  for (auto p = empty_el->next(empty_el); p != nullptr; p = p->next(empty_el)) {
  //    p->receive(event);
  //  }
  //} else {
  //  auto itr = subscribers.find(utils::type_id<Event_T>());
  //  if (itr == subscribers.end()) return;

  //  /*for (auto ptr : itr->second) {
  //    reinterpret_cast<basic_reciever<Event_T>*>(ptr)->receive(event);
  //  }*/

  //  auto empty_el = itr->second.get<Event_T>(); // берем следующий после пустого ресивер
  //  for (auto p = empty_el->next(empty_el); p != nullptr; p = p->next(empty_el)) {
  //    p->receive(event);
  //  }
  //}
}

template <typename... Comp_T>
world::view_t<Comp_T...> world::view() const {
  // желательно создать все аллокаторы для компонентов 
  // по идее это все должно быть конст
  // вылетать? единственный вменяемый сценарий честно говоря 
  if (!is_allocators_exist<Comp_T...>()) utils::error{}("Could not create view '{}', all allocators must be created beforehand", utils::type_name<view_t<Comp_T...>>());
  return view_t<Comp_T...>(this);
}

template <typename... Comp_T>
world::lazy_view_t<Comp_T...> world::lazy_view() const {
  if (!is_allocators_exist<Comp_T...>()) utils::error{}("Could not create view '{}', all allocators must be created beforehand", utils::type_name<lazy_view_t<Comp_T...>>());
  return lazy_view_t<Comp_T...>(this);
}

template <typename T, typename... Args>
T* world::create_unique(Args&&... args) {
  auto u_ptr = std::make_unique<class_container<T>>(std::forward<Args>(args)...);
  auto ret = u_ptr.get();
  auto itr = std::lower_bound(arbitrary_container.begin(), arbitrary_container.end(), ret, [](const auto& a, const auto& b) {
    std::less<basic_container*> l;
    return l(a.get(), b);
  });

  arbitrary_container.insert(itr, std::move(u_ptr));
  assert(ret == make_container_ptr(ret->get_ptr()));
  return ret->get_ptr();
}

template <typename T>
bool world::remove_unique(T* ptr) {
  auto better_ptr = make_container_ptr(ptr);
  auto itr = std::lower_bound(arbitrary_container.begin(), arbitrary_container.end(), better_ptr, [](const auto& a, const auto& b) {
    std::less<basic_container*> l;
    return l(a.get(), b);
  });

  if (itr == arbitrary_container.end()) return false;
  arbitrary_container.erase(itr);
  return true;
}

// почему не конст? ощущение такое что и контейнеры будут конст
template <typename... Comp_T>
world::query_t<Comp_T...> world::query() {
  //return create_unique<query_t<Comp_T...>>(this);
  return query_t<Comp_T...>(this);
}

template <typename... Comp_T>
world::lazy_query_t<Comp_T...> world::lazy_query() {
  //return create_unique<query_t<Comp_T...>>(this);
  return lazy_query_t<Comp_T...>(this);
}


//template <typename... Comp_T>
//bool world::remove_query(query_t<Comp_T...>* ptr) {
//  return remove_unique(ptr);
//}

template <typename T, typename... Args>
  requires(std::derived_from<T, basic_system>)
T* world::create_system(Args&&... args) {
  auto ptr = create_unique<T>(std::forward<Args>(args)...);
  subscribe<update_event>(ptr);
  return ptr;
}

template <typename T>
  requires(std::derived_from<T, basic_system>)
bool world::remove_system(T* ptr) {
  unsubscribe<update_event>(ptr);
  return remove_unique(ptr);
}

template<typename T>
world::sparce_dence_set<T>* world::get_or_create_allocator(const size_t) {
  //const size_t index = global_index::get<T>();
  const size_t index = aesthetics::component_type_id<T>();

  if (index < containers.size()) {
    auto stor = static_cast<sparce_dence_set<T>*>(containers[index].get());
    if (stor != nullptr) return stor;

    containers[index] = std::make_unique<sparce_dence_set<T>>();
    return static_cast<sparce_dence_set<T>*>(containers[index].get());
  }

  containers.resize(index+1);
  containers[index] = std::make_unique<sparce_dence_set<T>>();
  return static_cast<sparce_dence_set<T>*>(containers[index].get());
}

template<typename T>
world::sparce_dence_set<T>* world::get_allocator() const {
  const size_t index = aesthetics::component_type_id<T>();
  if (index >= containers.size()) return nullptr;
  return static_cast<sparce_dence_set<T>*>(containers[index].get());
}

//template <typename T>
//T* world::container_interface::create(const entityid_t id) {
//  auto ptr = allocate(id);
//  if (ptr == nullptr) return nullptr;
//  auto comp_ptr = new (ptr) T();
//  return comp_ptr;
//}
//
//template <typename T, typename... Args>
//T* world::container_interface::create(const entityid_t id, Args&&... args) {
//  auto ptr = allocate(id);
//  if (ptr == nullptr) return nullptr;
//  auto comp_ptr = new (ptr) T(std::forward<Args>(args)...);
//  return comp_ptr;
//}

//template<typename T>
//world::type_container<T>::type_container(const std::string_view& type_name, const size_t block_size) noexcept : type_container(type_name, block_size, sizeof(T), alignof(T)) {}
//template<typename T>
//world::type_container<T>::type_container(const std::string_view& type_name, const size_t block_size, const size_t alloc_size, const size_t alignment) noexcept :
//  container_interface(type_name), allocator(block_size, alloc_size, alignment)
//{}
//template<typename T>
//world::type_container<T>::~type_container() noexcept {
//  for (const auto &[ id, ptr ] : components) {
//    reinterpret_cast<T*>(ptr)->~T();
//  }
//}
//
//template<typename T>
//void* world::type_container<T>::allocate(const entityid_t id) noexcept {
//  auto itr = components.find(id);
//  if (itr != components.end()) return nullptr; // ???
//
//  auto ptr = allocator.allocate();
//  components[id] = ptr;
//  return ptr;
//}
//
//template<typename T>
//bool world::type_container<T>::remove(const entityid_t id) noexcept {
//  auto itr = components.find(id);
//  if (itr == components.end()) return false;
//
//  reinterpret_cast<T*>(itr->second)->~T();
//  allocator.free(itr->second);
//  components.erase(itr);
//  return true;
//}
//
//template<typename T>
//world::small_type_container<T>::small_type_container(const std::string_view& type_name, utils::block_allocator* allocator) noexcept : container_interface(type_name), allocator(allocator) {}
//template<typename T>
//world::small_type_container<T>::~small_type_container() noexcept {
//  for (const auto& [id, ptr] : components) {
//    reinterpret_cast<T*>(ptr)->~T();
//    allocator->free(ptr);
//  }
//}
//
//template<typename T>
//void* world::small_type_container<T>::allocate(const entityid_t id) noexcept {
//  auto itr = components.find(id);
//  if (itr != components.end()) return nullptr; // ???
//
//  auto ptr = allocator->allocate();
//  components[id] = ptr;
//  return ptr;
//}
//
//template<typename T>
//bool world::small_type_container<T>::remove(const entityid_t id) noexcept {
//  auto itr = components.find(id);
//  if (itr == components.end()) return false;
//
//  reinterpret_cast<T*>(itr->second)->~T();
//  allocator->free(itr->second);
//  components.erase(itr);
//  return true;
//}

template <typename T>
T* world::container_interface::create(const entityid_t id) {
  auto ptr = static_cast<sparce_dence_set<T>*>(this);
  return ptr->create(id);
}
template <typename T, typename... Args>
T* world::container_interface::create(const entityid_t id, Args&&... args) {
  auto ptr = static_cast<sparce_dence_set<T>*>(this);
  return ptr->create(id, std::forward<Args>(args)...);
}

template <typename T>
T* world::container_interface::get(const entityid_t id) const {
  auto ptr = static_cast<const sparce_dence_set<T>*>(this);
  return ptr->get(id);
}

static size_t count_invalid_data_from_begin(const std::vector<entityid_t> &datas) {
  size_t i = 0;
  for (; i < datas.size() && is_invalid_entityid(datas[i]); ++i) {}
  return i;
}

static size_t move_data(std::vector<entityid_t>& datas) {
  const size_t count = count_invalid_data_from_begin(datas);
  if (count == 0) return 0;
  if (count == datas.size()) {
    datas.clear();
    return count;
  }

  memmove(datas.data(), datas.data()+count, (datas.size() - count) * sizeof(datas[0]));
  datas.resize(datas.size() - count);
  return count;
}

template <typename T>
bool world::sparce_dence_set<T>::remove(const entityid_t id) noexcept {
  const size_t ent_index = get_entityid_index(id);
  if (sparce_set.size() <= ent_index) return false;
  if (sparce_set[ent_index] == invalid_entityid) return false;
  if (get_entityid_version(id) != get_entityid_version(sparce_set[ent_index])) return false;

  const size_t container_index = get_entityid_index(sparce_set[ent_index]);

  auto itr = std::find_if(sparce_set.begin(), sparce_set.end(), [this](const auto data) {
    const size_t container_index = get_entityid_index(data);
    if (container_index == components.size() - 1) return true;
    return false;
  });

  std::swap(components[container_index], components.back());
  components.pop_back();
  *itr = make_entityid(container_index, get_entityid_version(*itr));
  sparce_set[ent_index] = invalid_entityid;

  return true;

  //const size_t index = get_entityid_index(id);
  //if (index < offset || index - offset >= sparce_set.size() || is_invalid_entityid(sparce_set[index - offset])) return false;

  //const size_t final_index = index - offset;
  //const auto data = sparce_set[final_index];
  //if (get_entityid_version(id) != get_entityid_version(data)) return false;

  //const size_t container_index = get_entityid_index(data);
  //if (container_index == components.size() - 1) {
  //  components.pop_back();
  //  sparce_set[final_index] = invalid_entityid;
  //  offset += move_data(sparce_set);
  //  offset = sparce_set.empty() ? 0 : offset;
  //  size -= 1;
  //  return true;
  //}

  //// как удаляем? нужно найти где лежит индекс последнего компонента
  //auto itr = std::find_if(sparce_set.begin(), sparce_set.end(), [this](const auto data) {
  //  const size_t container_index = get_entityid_index(data);
  //  if (container_index == components.size() - 1) return true;
  //  return false;
  //});

  //std::swap(components[container_index], components.back());
  //components.pop_back();
  //*itr = make_entityid(container_index, get_entityid_version(*itr));
  //offset += move_data(sparce_set);
  //offset = sparce_set.empty() ? 0 : offset;
  //size -= 1;
  //return true;
}

template <typename T>
void* world::sparce_dence_set<T>::rawget(const entityid_t id) const noexcept {
  const size_t ent_index = get_entityid_index(id);
  if (sparce_set.size() <= ent_index) return nullptr;
  if (sparce_set[ent_index] == invalid_entityid) return nullptr;
  if (get_entityid_version(id) != get_entityid_version(sparce_set[ent_index])) return nullptr;

  const size_t container_index = get_entityid_index(sparce_set[ent_index]);
  return &components[container_index];

  /*const size_t index = get_entityid_index(id);
  if (index < offset || index - offset >= sparce_set.size() || is_invalid_entityid(sparce_set[index - offset])) return nullptr;

  const size_t final_index = index - offset;
  const auto data = sparce_set[final_index];
  if (get_entityid_version(id) != get_entityid_version(data)) return nullptr;

  const size_t container_index = get_entityid_index(data);
  return &components[container_index];*/
}

template <typename T>
T* world::sparce_dence_set<T>::create_comp(const entityid_t id) {
  const size_t ent_index = get_entityid_index(id);
  if (sparce_set.size() <= ent_index) {
    sparce_set.resize(ent_index+1, invalid_entityid);
  }

  if (sparce_set[ent_index] != invalid_entityid) return nullptr;

  const size_t cont_index = components.size();
  components.emplace_back();
  sparce_set[ent_index] = make_entityid(cont_index, get_entityid_version(id));
  return &(components.back());

  //const size_t index = get_entityid_index(id);
  //if (sparce_set.empty()) {
  //  offset = index;
  //  sparce_set.push_back(make_entityid(components.size(), get_entityid_version(id)));
  //  components.emplace_back();
  //  size += 1;
  //  return &(components.back());
  //}

  //// здесь мы должны создать недостающий размер
  //if (index >= offset) {
  //  const size_t final_index = index - offset;
  //  if (final_index >= sparce_set.size()) {
  //    sparce_set.resize(final_index+1, invalid_entityid);
  //    sparce_set.back() = make_entityid(components.size(), get_entityid_version(id));
  //    components.emplace_back();
  //    size += 1;
  //    return &(components.back());
  //  }

  //  if (!is_invalid_entityid(sparce_set[final_index])) utils::error{}("Component '{}' is already exists for entity {}", utils::type_name<T>(), id);

  //  sparce_set[final_index] = make_entityid(components.size(), get_entityid_version(id));
  //  components.emplace_back();
  //  size += 1;
  //  return &(components.back());
  //}

  //// тут нужно передвинуть от начала все данные
  //const size_t move_size = offset - index;
  //const size_t prev_size = sparce_set.size();
  //sparce_set.resize(sparce_set.size()+move_size, invalid_entityid);
  //memmove(sparce_set.data()+move_size, sparce_set.data(), prev_size * sizeof(sparce_set[0]));
  //for (size_t i = 0; i < move_size; ++i) sparce_set[i] = invalid_entityid;
  //offset = index;
  //sparce_set[0] = make_entityid(components.size(), get_entityid_version(id));
  //components.emplace_back();
  //size += 1;
  //return &(components.back());
}

template <typename T>
template <typename... Args>
T* world::sparce_dence_set<T>::create_comp(const entityid_t id, Args&&... args) {
  const size_t ent_index = get_entityid_index(id);
  if (sparce_set.size() <= ent_index) {
    sparce_set.resize(ent_index + 1, invalid_entityid);
  }

  if (sparce_set[ent_index] != invalid_entityid) return nullptr;

  const size_t cont_index = components.size();
  components.emplace_back(std::forward<Args>(args)...);
  sparce_set[ent_index] = make_entityid(cont_index, get_entityid_version(id));
  return &(components.back());

  //const size_t index = get_entityid_index(id);
  //if (sparce_set.empty()) {
  //  offset = index;
  //  sparce_set.push_back(make_entityid(components.size(), get_entityid_version(id)));
  //  components.emplace_back(std::forward<Args>(args)...);
  //  size += 1;
  //  return &(components.back());
  //}

  //// здесь мы должны создать недостающий размер
  //if (index >= offset) {
  //  const size_t final_index = index - offset;
  //  if (final_index >= sparce_set.size()) {
  //    sparce_set.resize(final_index + 1, invalid_entityid);
  //    sparce_set.back() = make_entityid(components.size(), get_entityid_version(id));
  //    components.emplace_back(std::forward<Args>(args)...);
  //    size += 1;
  //    return &(components.back());
  //  }

  //  if (!is_invalid_entityid(sparce_set[final_index])) utils::error{}("Component '{}' is already exists for entity {}", utils::type_name<T>(), id);

  //  sparce_set[final_index] = make_entityid(components.size(), get_entityid_version(id));
  //  components.emplace_back(std::forward<Args>(args)...);
  //  size += 1;
  //  return &(components.back());
  //}

  //// тут нужно передвинуть от начала все данные
  //const size_t move_size = offset - index;
  //const size_t prev_size = sparce_set.size();
  //sparce_set.resize(sparce_set.size() + move_size, invalid_entityid);
  //memmove(sparce_set.data() + move_size, sparce_set.data(), prev_size * sizeof(sparce_set[0]));
  //for (size_t i = 0; i < move_size; ++i) sparce_set[i] = invalid_entityid;
  //offset = index;
  //sparce_set[0] = make_entityid(components.size(), get_entityid_version(id));
  //components.emplace_back(std::forward<Args>(args)...);
  //size += 1;
  //return &(components.back());
}

template <typename T>
T* world::sparce_dence_set<T>::get_comp(const entityid_t id) const {
  const size_t ent_index = get_entityid_index(id);
  if (sparce_set.size() <= ent_index) return nullptr;
  if (sparce_set[ent_index] == invalid_entityid) return nullptr;
  if (get_entityid_version(id) != get_entityid_version(sparce_set[ent_index])) return nullptr;

  const size_t container_index = get_entityid_index(sparce_set[ent_index]);
  return &components[container_index];

  /*const size_t index = get_entityid_index(id);
  if (index < offset || index - offset >= sparce_set.size() || is_invalid_entityid(sparce_set[index - offset])) return nullptr;

  const size_t final_index = index - offset;
  const auto data = sparce_set[final_index];
  if (get_entityid_version(id) != get_entityid_version(data)) return nullptr;

  const size_t container_index = get_entityid_index(data);
  return &components[container_index];*/
}
  


}
}

#ifdef DEVILS_ENGINE_AESTHETICS_IMPLEMENTATION

namespace devils_engine {
namespace aesthetics {

size_t global_index::index = 0;

world::entity::entity() noexcept : m_id(invalid_entityid), m_world(nullptr) {}
world::entity::entity(const entityid_t id, class world* world) noexcept : m_id(id), m_world(world) {}
world::entity::~entity() noexcept {}

entityid_t world::entity::id() const noexcept { return m_id; }
class world* world::entity::world() const noexcept { return m_world; }
const world::entity::components_arr& world::entity::components() const noexcept { return m_world->find_components(m_id); }
bool world::entity::valid() const noexcept { return m_world != nullptr; }
bool world::entity::exists() const noexcept { return m_world->exists(m_id); }

void world::entity::clear() {
  m_world->clear(m_id);
}

world::world() noexcept : cur_index(0) {}
entityid_t world::gen_entityid() {
  if (removed_entities.empty()) {
    const auto ent = make_entityid(cur_index, 0);
    cur_index += 1;
    return ent;
  }

  auto prev_ent = removed_entities.back();
  removed_entities.pop_back();
  return make_entityid(get_entityid_index(prev_ent), get_entityid_version(prev_ent)+1);
}

void world::remove_entity(const entityid_t id) {
  clear(id);
  removed_entities.push_back(id);
}

world::entity::components_arr world::find_components(const entityid_t id) const {
  world::entity::components_arr arr;
  for (const auto& up : containers) {
    auto ptr = up->rawget(id);
    if (ptr != nullptr) {
      arr.emplace_back(std::make_pair(up->type_index, ptr));
    }
  }

  return arr;
}

world::entity world::get_entity(const entityid_t id) const {
  return entity(id, const_cast<class world*>(this)); // конст каст?
}

bool world::exists(const entityid_t id) const {
  for (const auto& up : containers) {
    auto ptr = up->rawget(id);
    if (ptr != nullptr) return true;
  }

  return false;
}

bool world::raw_has(const entityid_t id, const size_t type_id) const { return raw_get(id, type_id) != nullptr; }
void* world::raw_get(const entityid_t id, const size_t type_id) const {
  if (type_id >= containers.size()) return nullptr;
  return containers[type_id]->rawget(id);
}

bool world::raw_remove(const entityid_t id, const size_t type_id) {
  if (type_id >= containers.size()) return false;
  return containers[type_id]->remove(id);
}

void world::clear(const entityid_t id) {
  for (auto& up : containers) { up->remove(id); }
}

}
}

#endif

#endif