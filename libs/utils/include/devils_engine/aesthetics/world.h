#ifndef DEVILS_ENGINE_AESTHETICS_WORLD_H
#define DEVILS_ENGINE_AESTHETICS_WORLD_H

#include <cstddef>
#include <cstdint>
#include <vector>
#include <functional>
#include <atomic>
#include <memory>
#include <string_view>
#include "utils/type_traits.h"
#include "utils/block_allocator.h"
#include <gtl/phmap.hpp>

#include "common.h"

// нужно еще сделать константы с размерами по умолчанию

namespace devils_engine {
namespace aesthetics {
class world {
public:
  template <typename... Comp_T>
  using tuple_t = std::tuple<entityid_t, Comp_T*...>;
  using raw_itr = gtl::flat_hash_map<entityid_t, void*>::const_iterator;

  // вернет итераторы, будет ходить по каждому энтити у которого есть этот набор компонентов
  template <typename... Comp_T>
  class view_t {
  public:
    using view_tuple_t = tuple_t<Comp_T...>;

    class iterator {
    public:
      using underlying_itr = gtl::flat_hash_map<entityid_t, void*>::const_iterator;

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
    void forEach(const std::function<void(entityid_t, Comp_T*...)> &f) const;
  private:
    const class world* world;
  };

  // будет хранить указатели на все компоненты в массиве - крайне быстрый доступ ко всем похожим энтити
  template <typename... Comp_T>
  class query {
  public:
    using query_tuple_t = tuple_t<Comp_T...>;
    using iterator = std::vector<query_tuple_t>::iterator;

    template <typename T>
    class query_receiver : public basic_reciever<T> {
    public:
      using event_t = T;

      query_receiver(const class world* world, query<Comp_T...>* q) noexcept;
      void receive(const T& event) override;
    private:
      const class world* world;
      query<Comp_T...>* q;
    };

    template <typename T>
    class query_remover : public basic_reciever<T> {
    public:
      using event_t = T;

      query_remover(const class world* world, query<Comp_T...>* q) noexcept;
      void receive(const T& event) override;
    private:
      const class world* world;
      query<Comp_T...>* q;
    };

    query(class world* world) noexcept;
    ~query() noexcept;

    query(const query& copy) noexcept = delete;
    query(query&& move) noexcept = delete;
    query& operator=(const query& copy) noexcept = delete;
    query& operator=(query&& move) noexcept = delete;

    iterator begin() const;
    iterator end() const;
    void forEach(const std::function<void(entityid_t, Comp_T*...)> &f) const;
    std::vector<query_tuple_t>& array();
    const std::vector<query_tuple_t>& array() const;

    void add_entity(const query_tuple_t &t);
    void remove_entity(const query_tuple_t& t);
  private:
    class world* world;
    std::vector<query_tuple_t> container;
    std::tuple<query_receiver<create_component_event<Comp_T>>...> receivers;
    std::tuple<query_remover<remove_component_event<Comp_T>>...> removers;
  };

  class entity { // does not automatically find new component
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
    template <typename T, typename T2, typename... Comp_T>
    auto get() const -> tuple_t<T, T2, Comp_T...>;
    template <typename T>
    bool has() const;
    template <typename T, typename T2, typename... Comp_T>
    bool has() const;

    // наверное пригодилось бы еще упаковать void* в tuple по id'шникам типов

    void clear(); // remove all components
  private:
    entityid_t m_id;
    class world* m_world;
    components_arr m_components;
  };

  world() noexcept;
  world(const world& copy) noexcept = delete;
  world(world&& move) noexcept = default;
  world& operator=(const world& copy) noexcept = delete;
  world& operator=(world&& move) noexcept = default;
  entityid_t gen_entityid();
  entity::components_arr find_components(const entityid_t id) const;
  entity get_entity(const entityid_t id) const;
  bool exists(const entityid_t id) const; // has at least one component
  bool raw_has(const entityid_t id, const size_t type_id) const;
  void* raw_get(const entityid_t id, const size_t type_id) const;
  bool raw_remove(const entityid_t id, const size_t type_id);
  void clear(const entityid_t id);

  template <typename T>
  void create_allocator(const size_t block_size);
  template <typename T>
  bool is_allocators_exist() const;
  template <typename T, typename T2, typename... Comp_T>
  bool is_allocators_exist() const;

  template <typename T>
  T* create(const entityid_t id);
  template <typename T, typename... Args>
  T* create(const entityid_t id, Args&&... args);
  template <typename T>
  bool remove(const entityid_t id);
  template <typename T>
  T* get(const entityid_t id) const;
  template <typename T, typename T2, typename... Comp_T>
  auto get(const entityid_t id) const -> tuple_t<T, T2, Comp_T...>;
  template <typename T>
  bool has(const entityid_t id) const;
  template <typename T, typename T2, typename... Comp_T>
  bool has(const entityid_t id) const;
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

  template <typename T, typename... Args>
  T* create_unique(Args&&... args);
  template <typename T>
  bool remove_unique(T* ptr);

  template <typename... Comp_T>
  query<Comp_T...>* create_query();
  template <typename... Comp_T>
  bool remove_query(query<Comp_T...>* ptr);

  template <typename T, typename... Args>
    requires(std::derived_from<T, basic_system>)
  T* create_system(Args&&... args);
  template <typename T>
    requires(std::derived_from<T, basic_system>)
  bool remove_system(T* ptr);
private:
  class container_interface {
  public:
    std::string_view type_name;
    gtl::flat_hash_map<entityid_t, void*> components;

    inline container_interface(const std::string_view& type_name) noexcept : type_name(type_name) {}
    virtual ~container_interface() noexcept = default;
    virtual void* allocate(const entityid_t id) noexcept = 0;
    virtual bool remove(const entityid_t id) noexcept = 0;

    template <typename T>
    T* create(const entityid_t id);
    template <typename T, typename... Args>
    T* create(const entityid_t id, Args&&... args);
  };

  template <typename T>
  struct type_container : public container_interface {
    using destructor_f = std::function<void(void* ptr)>;

    utils::block_allocator allocator;

    type_container(const std::string_view &type_name, const size_t block_size) noexcept;
    type_container(const std::string_view &type_name, const size_t block_size, const size_t alloc_size, const size_t alignment) noexcept;
    ~type_container() noexcept;

    type_container(const type_container& copy) noexcept = delete;
    type_container(type_container&& move) noexcept = default;
    type_container& operator=(const type_container& copy) noexcept = delete;
    type_container& operator=(type_container&& move) noexcept = default;

    void* allocate(const entityid_t id) noexcept override;
    bool remove(const entityid_t id) noexcept override;
  };

  template <typename T>
  struct small_type_container : public container_interface { // obj size is no more than sizeof(void*)
    utils::block_allocator* allocator;

    small_type_container(const std::string_view& type_name, utils::block_allocator* allocator) noexcept;
    ~small_type_container() noexcept;

    small_type_container(const small_type_container& copy) noexcept = delete;
    small_type_container(small_type_container&& move) noexcept = default;
    small_type_container& operator=(const small_type_container& copy) noexcept = delete;
    small_type_container& operator=(small_type_container&& move) noexcept = default;

    void* allocate(const entityid_t id) noexcept override;
    bool remove(const entityid_t id) noexcept override;
  };

  utils::block_allocator small_allocator; // no more than sizeof(void*)
  std::atomic<entityid_t> cur_id;
  gtl::flat_hash_map<size_t, std::unique_ptr<container_interface>> containers;
  gtl::node_hash_map<size_t, std::vector<void*>> subscribers;
  std::vector<std::unique_ptr<basic_container>> arbitrary_container;

  template<typename T>
  gtl::flat_hash_map<size_t, std::unique_ptr<container_interface>>::iterator get_or_create_allocator(const size_t block_size, const size_t alloc_size = sizeof(T), const size_t alignment = alignof(T));
};

template <typename... Comp_T>
world::view_t<Comp_T...>::iterator::iterator(const class world* world, world::view_t<Comp_T...>::iterator::underlying_itr it, world::view_t<Comp_T...>::iterator::underlying_itr end) noexcept : world(world), it(it), end(end) {
  if (it == end) return;
  bool ret = world->has<Comp_T...>(it->first);
  for (; it != end && !ret; ++it) { ret = world->has<Comp_T...>(it->first); }
}
template <typename... Comp_T>
world::view_t<Comp_T...>::view_tuple_t world::view_t<Comp_T...>::iterator::operator*() const { return world->get<Comp_T...>(it->first); }
template <typename... Comp_T>
world::view_t<Comp_T...>::view_tuple_t world::view_t<Comp_T...>::iterator::get() const { return world->get<Comp_T...>(it->first); }
template <typename... Comp_T>
world::view_t<Comp_T...>::iterator& world::view_t<Comp_T...>::iterator::operator++() {
  bool ret = false;
  for (; it != end && !ret; ++it) { ret = world->has<Comp_T...>(it->first); }
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
void world::view_t<Comp_T...>::forEach(const std::function<void(entityid_t, Comp_T*...)>& f) const {
  for (const auto &t : *this) { std::apply(f, t); }
}

template <typename... Comp_T>
template <typename T>
world::query<Comp_T...>::query_receiver<T>::query_receiver(const class world* world, query<Comp_T...>* q) noexcept : world(world), q(q) {}
template <typename... Comp_T>
template <typename T>
void world::query<Comp_T...>::query_receiver<T>::receive(const T& event) {
  const auto t = world->get<Comp_T...>(event.id);
  if (std::apply(&all_of_not_null<entityid_t, Comp_T*...>, t)) {
    q->add_entity(t);
  }
}

template <typename... Comp_T>
template <typename T>
world::query<Comp_T...>::query_remover<T>::query_remover(const class world* world, query<Comp_T...>* q) noexcept : world(world), q(q) {}
template <typename... Comp_T>
template <typename T>
void world::query<Comp_T...>::query_remover<T>::receive(const T& event) {
  const auto t = world->get<Comp_T...>(event.id);
  if (std::apply(&all_of_not_null<entityid_t, Comp_T*...>, t)) {
    q->remove_entity(t);
  }
}

template <size_t N, typename... Ts>
void subscribe_all(world* w, std::tuple<Ts...> &t) {
  using tuple_t = std::tuple<Ts...>;
  if constexpr (N >= std::tuple_size_v<tuple_t>) return;
  else {
    using evt_t = std::tuple_element_t<N, tuple_t>::event_t;
    w->subscribe<evt_t>(std::addressof(std::get<N>(t)));
    subscribe_all<N + 1>(w, t);
  }
}

template <size_t N, typename... Ts>
void unsubscribe_all(world* w, std::tuple<Ts...>& t) {
  using tuple_t = std::tuple<Ts...>;
  if constexpr (N >= std::tuple_size_v<tuple_t>) return;
  else {
    using evt_t = std::tuple_element_t<N, tuple_t>::event_t;
    w->unsubscribe<evt_t>(std::addressof(std::get<N>(t)));
    unsubscribe_all<N + 1>(w, t);
  }
}

template <typename... Comp_T>
world::query<Comp_T...>::query(class world* world) noexcept : 
  world(world), receivers(std::make_tuple(query_receiver<create_component_event<Comp_T>>(world, this)...)), removers(std::make_tuple(query_remover<remove_component_event<Comp_T>>(world, this)...))
{
  subscribe_all<0>(world, receivers);
  subscribe_all<0>(world, removers);
  const auto view = world->view<Comp_T...>();
  for (const auto &t : view) {
    add_entity(t);
  }
}
template <typename... Comp_T>
world::query<Comp_T...>::~query() noexcept {
  unsubscribe_all<0>(world, receivers);
  unsubscribe_all<0>(world, removers);
}
template <typename... Comp_T>
world::query<Comp_T...>::iterator world::query<Comp_T...>::begin() const { return container.begin(); }
template <typename... Comp_T>
world::query<Comp_T...>::iterator world::query<Comp_T...>::end() const { return container.begin(); }
template <typename... Comp_T>
void world::query<Comp_T...>::forEach(const std::function<void(entityid_t, Comp_T*...)> &f) const {
  for (const auto& t : container) { std::apply(f, t); }
}
template <typename... Comp_T>
std::vector<typename world::query<Comp_T...>::query_tuple_t>& world::query<Comp_T...>::array() { return container; }
template <typename... Comp_T>
const std::vector<typename world::query<Comp_T...>::query_tuple_t>& world::query<Comp_T...>::array() const { return container; }

template <typename... Comp_T>
void world::query<Comp_T...>::add_entity(const query_tuple_t& t) {
  auto itr = std::lower_bound(container.begin(), container.end(), t, [] (const auto &a, const auto &b) {
    std::less<entityid_t> l;
    return l(std::get<0>(a), std::get<0>(b));
  });

  container.insert(itr, t);
}

template <typename... Comp_T>
void world::query<Comp_T...>::remove_entity(const query_tuple_t& t) {
  auto itr = std::lower_bound(container.begin(), container.end(), t, [](const auto& a, const auto& b) {
    std::less<entityid_t> l;
    return l(std::get<0>(a), std::get<0>(b));
  });

  container.erase(itr);
}

static bool entity_component_predicate(const std::pair<size_t, void*> &pair, const size_t &type_id) {
  std::less<size_t> l;
  return l(pair.first, type_id);
}

template <typename T, typename... Args>
T* world::entity::create(Args&&... args) {
  // если уже есть такой компонент? world сейчас вернет nullptr
  if (has<T>()) return nullptr;

  constexpr size_t type_id = utils::type_id<T>();

  auto ptr = m_world->create(m_id, std::forward<Args>(args)...);
  auto itr = std::lower_bound(m_components.begin(), m_components.end(), type_id, &entity_component_predicate);
  m_components.insert(itr, ptr);
  return ptr;
}

template <typename T>
bool world::entity::remove() {
  if (!has<T>()) return false;

  constexpr size_t type_id = utils::type_id<T>();

  auto itr = std::lower_bound(m_components.begin(), m_components.end(), type_id, &entity_component_predicate);
  if (itr == m_components.end()) return false;
  m_components.erase(itr);
  return m_world->remove<T>(m_id);
}

template <typename T>
T* world::entity::get() const {
  constexpr size_t type_id = utils::type_id<T>();
  auto itr = std::lower_bound(m_components.begin(), m_components.end(), type_id, &entity_component_predicate);
  if (itr == m_components.end()) return nullptr;
  auto ptr = reinterpret_cast<T*>(itr->second);
  return ptr;
}

template <typename T, typename T2, typename... Comp_T>
auto world::entity::get() const -> tuple_t<T, T2, Comp_T...> {
  return std::make_tuple(m_id, get<T>(), get<T2>(), get<Comp_T>()...);
}

template <typename T>
bool world::entity::has() const { return get<T>() != nullptr; }

template <typename T, typename T2, typename... Comp_T>
bool world::entity::has() const {
  return (get<T>() != nullptr) && has<T2, Comp_T...>();
}

template <typename T>
void world::create_allocator(const size_t block_size) {
  get_or_create_allocator<T>(block_size);
}

template <typename T>
bool world::is_allocators_exist() const {
  return containers.find(utils::type_id<T>()) != containers.end();
}

template <typename T, typename T2, typename... Comp_T>
bool world::is_allocators_exist() const {
  return is_allocators_exist<T>() && is_allocators_exist<T2, Comp_T...>();
}

template <typename T>
T* world::create(const entityid_t id) {
  constexpr size_t type_id = utils::type_id<T>();

  auto itr = get_or_create_allocator<T>(sizeof(T) * 250);
  auto ptr = itr->second->create<T>(id);
  if (ptr == nullptr) return nullptr;
  emit(create_component_event<T>{id, ptr});
  return ptr;
}

template <typename T, typename... Args>
T* world::create(const entityid_t id, Args&&... args) {
  constexpr size_t type_id = utils::type_id<T>();

  auto itr = get_or_create_allocator<T>(sizeof(T)*250);
  auto ptr = itr->second->create<T>(id, std::forward<Args>(args)...);
  if (ptr == nullptr) return nullptr;
  emit(create_component_event<T>{id, ptr});
  return ptr;
}

template <typename T>
bool world::remove(const entityid_t id) {
  constexpr size_t type_id = utils::type_id<T>();

  auto itr = containers.find(type_id);
  if (itr == containers.end()) return false;

  auto comp_itr = itr->second->components.find(id);
  if (comp_itr == itr->second->components.end()) return false;

  auto ptr = reinterpret_cast<T*>(comp_itr->second);
  emit(remove_component_event<T>{id, ptr});

  itr->second->remove(id);
  return true;
}

template <typename T>
T* world::get(const entityid_t id) const {
  constexpr size_t type_id = utils::type_id<T>();

  auto itr = containers.find(type_id);
  if (itr == containers.end()) return nullptr;

  auto comp_itr = itr->second->components.find(id);
  if (comp_itr == itr->second->components.end()) return nullptr;

  auto ptr = reinterpret_cast<T*>(comp_itr->second);
  return ptr;
}

template <typename T, typename T2, typename... Comp_T>
auto world::get(const entityid_t id) const -> tuple_t<T, T2, Comp_T...> {
  return std::make_tuple(id, get<T>(id), get<T2>(id), get<Comp_T>(id)...);
}

template <typename T>
bool world::has(const entityid_t id) const {
  return get<T>(id) != nullptr;
}

template <typename T, typename T2, typename... Comp_T>
bool world::has(const entityid_t id) const {
  return has<T>(id) && has<T2, Comp_T...>(id);
}

template <typename T>
size_t world::count() const {
  constexpr size_t type_id = utils::type_id<T>();

  auto itr = containers.find(type_id);
  if (itr == containers.end()) return 0;

  return itr->second->components.size();
}

template <typename T>
world::raw_itr world::raw_begin() const {
  auto itr = containers.find(utils::type_id<T>());
  return itr->second->components.begin();
}

template <typename T>
world::raw_itr world::raw_end() const {
  auto itr = containers.find(utils::type_id<T>());
  return itr->second->components.end();
}

template <typename Event_T, typename T>
  requires(std::derived_from<T, basic_reciever<Event_T>>)
void world::subscribe(T* ptr) {
  constexpr size_t type_id = utils::type_id<Event_T>();

  auto itr = subscribers.find(type_id);
  if (itr == subscribers.end()) {
    itr = subscribers.emplace(std::make_pair(type_id, std::vector<void*>{})).first;
  }

  auto better_ptr = static_cast<basic_reciever<Event_T>*>(ptr);
  // блен, мне нужно обязательно поддерживать порядок
  //auto subs_itr = std::lower_bound(itr->second.begin(), itr->second.end(), better_ptr);
  //itr->second.insert(subs_itr, better_ptr);
  itr->second.push_back(better_ptr);
}

template <typename Event_T, typename T>
  requires(std::derived_from<T, basic_reciever<Event_T>>)
bool world::unsubscribe(T* ptr) {
  auto itr = subscribers.find(utils::type_id<Event_T>());
  if (itr == subscribers.end()) return false;

  auto better_ptr = static_cast<basic_reciever<Event_T>*>(ptr);
  //auto subs_itr = std::lower_bound(itr->second.begin(), itr->second.end(), better_ptr);
  auto subs_itr = std::find(itr->second.begin(), itr->second.end(), better_ptr);
  itr->second.erase(subs_itr);
  return true;
}

template <typename Event_T>
void world::emit(const Event_T& event) const {
  auto itr = subscribers.find(utils::type_id<Event_T>());
  if (itr == subscribers.end()) return;

  for (auto ptr : itr->second) {
    reinterpret_cast<basic_reciever<Event_T>*>(ptr)->receive(event);
  }
}

template <typename... Comp_T>
world::view_t<Comp_T...> world::view() const {
  // желательно создать все аллокаторы для компонентов 
  // по идее это все должно быть конст
  // вылетать? единственный вменяемый сценарий честно говоря 
  if (!is_allocators_exist<Comp_T...>()) utils::error("Could not create view '{}', all allocators must be created beforehand", utils::type_name<view_t<Comp_T...>>());
  return view_t<Comp_T...>(this);
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

template <typename... Comp_T>
world::query<Comp_T...>* world::create_query() {
  return create_unique<query<Comp_T...>>(this);
}

template <typename... Comp_T>
bool world::remove_query(query<Comp_T...>* ptr) {
  return remove_unique(ptr);
}

template <typename T, typename... Args>
  requires(std::derived_from<T, basic_system>)
T* world::create_system(Args&&... args) {
  return create_unique<T>(std::forward<Args>(args)...);
}

template <typename T>
  requires(std::derived_from<T, basic_system>)
bool world::remove_system(T* ptr) {
  return remove_unique(ptr);
}

template<typename T>
gtl::flat_hash_map<size_t, std::unique_ptr<world::container_interface>>::iterator world::get_or_create_allocator(const size_t block_size, const size_t alloc_size, const size_t alignment) {
  constexpr size_t type_id = utils::type_id<T>();

  auto itr = containers.find(type_id);
  if (itr != containers.end()) return itr;

  if constexpr (sizeof(T) <= sizeof(void*)) {
    return containers.emplace(std::make_pair(type_id, std::make_unique<small_type_container<T>>(utils::type_name<T>(), &small_allocator))).first;
  } else {
    return containers.emplace(std::make_pair(type_id, std::make_unique<type_container<T>>(utils::type_name<T>(), block_size, alloc_size, alignment))).first;
  }
}

template <typename T>
T* world::container_interface::create(const entityid_t id) {
  auto ptr = allocate(id);
  if (ptr == nullptr) return nullptr;
  auto comp_ptr = new (ptr) T();
  return comp_ptr;
}

template <typename T, typename... Args>
T* world::container_interface::create(const entityid_t id, Args&&... args) {
  auto ptr = allocate(id);
  if (ptr == nullptr) return nullptr;
  auto comp_ptr = new (ptr) T(std::forward<Args>(args)...);
  return comp_ptr;
}

template<typename T>
world::type_container<T>::type_container(const std::string_view& type_name, const size_t block_size) noexcept : type_container(type_name, block_size, sizeof(T), alignof(T)) {}
template<typename T>
world::type_container<T>::type_container(const std::string_view& type_name, const size_t block_size, const size_t alloc_size, const size_t alignment) noexcept :
  container_interface(type_name), allocator(block_size, alloc_size, alignment)
{}
template<typename T>
world::type_container<T>::~type_container() noexcept {
  for (const auto &[ id, ptr ] : components) {
    reinterpret_cast<T*>(ptr)->~T();
  }
}

template<typename T>
void* world::type_container<T>::allocate(const entityid_t id) noexcept {
  auto itr = components.find(id);
  if (itr != components.end()) return nullptr; // ???

  auto ptr = allocator.allocate();
  components[id] = ptr;
  return ptr;
}

template<typename T>
bool world::type_container<T>::remove(const entityid_t id) noexcept {
  auto itr = components.find(id);
  if (itr == components.end()) return false;

  reinterpret_cast<T*>(itr->second)->~T();
  allocator.free(itr->second);
  components.erase(itr);
  return true;
}

template<typename T>
world::small_type_container<T>::small_type_container(const std::string_view& type_name, utils::block_allocator* allocator) noexcept : container_interface(type_name), allocator(allocator) {}
template<typename T>
world::small_type_container<T>::~small_type_container() noexcept {
  for (const auto& [id, ptr] : components) {
    reinterpret_cast<T*>(ptr)->~T();
    allocator->free(ptr);
  }
}

template<typename T>
void* world::small_type_container<T>::allocate(const entityid_t id) noexcept {
  auto itr = components.find(id);
  if (itr != components.end()) return nullptr; // ???

  auto ptr = allocator->allocate();
  components[id] = ptr;
  return ptr;
}

template<typename T>
bool world::small_type_container<T>::remove(const entityid_t id) noexcept {
  auto itr = components.find(id);
  if (itr == components.end()) return false;

  reinterpret_cast<T*>(itr->second)->~T();
  allocator->free(itr->second);
  components.erase(itr);
  return true;
}

}
}

#ifdef DEVILS_ENGINE_AESTHETICS_IMPLEMENTATION

namespace devils_engine {
namespace aesthetics {

world::entity::entity() noexcept : m_id(0), m_world(nullptr) {}
world::entity::entity(const entityid_t id, class world* world) noexcept : m_id(id), m_world(world), m_components(world->find_components(m_id)) {}
world::entity::~entity() noexcept {}

entityid_t world::entity::id() const noexcept { return m_id; }
class world* world::entity::world() const noexcept { return m_world; }
const world::entity::components_arr& world::entity::components() const noexcept { return m_components; }
bool world::entity::valid() const noexcept { return m_world == nullptr; }
bool world::entity::exists() const noexcept { return !m_components.empty(); }

void world::entity::clear() {
  m_world->clear(m_id);
  m_components.clear();
}

world::world() noexcept : small_allocator(sizeof(void*) * 1000, sizeof(void*), alignof(void*)), cur_id(1) {}
entityid_t world::gen_entityid() { return cur_id.fetch_add(1, std::memory_order_relaxed); }
world::entity::components_arr world::find_components(const entityid_t id) const {
  world::entity::components_arr arr;
  for (const auto& pair : containers) {
    auto itr = pair.second->components.find(id);
    if (itr != pair.second->components.end()) {
      arr.emplace_back(std::make_pair(pair.first, itr->second));
    }
  }

  return arr;
}

world::entity world::get_entity(const entityid_t id) const {
  return entity(id, const_cast<class world*>(this)); // конст каст?
}

bool world::exists(const entityid_t id) const {
  for (const auto& pair : containers) {
    auto itr = pair.second->components.find(id);
    if (itr != pair.second->components.end()) return true;
  }

  return false;
}

bool world::raw_has(const entityid_t id, const size_t type_id) const { return raw_get(id, type_id) != nullptr; }
void* world::raw_get(const entityid_t id, const size_t type_id) const {
  auto itr = containers.find(type_id);
  if (itr == containers.end()) return nullptr;

  auto comp_itr = itr->second->components.find(id);
  if (comp_itr == itr->second->components.end()) return nullptr;

  return comp_itr->second;
}

bool world::raw_remove(const entityid_t id, const size_t type_id) {
  auto itr = containers.find(type_id);
  if (itr == containers.end()) return false;

  return itr->second->remove(id);
}

void world::clear(const entityid_t id) {
  for (auto& pair : containers) { pair.second->remove(id); }
}

}
}

#endif

#endif