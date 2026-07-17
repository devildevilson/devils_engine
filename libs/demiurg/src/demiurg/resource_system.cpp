#include <algorithm>
#include <cassert>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <limits>

#include "catalogue_domain.h"
#include "devils_engine/catalogue/logging.h"
#include "devils_engine/thread/atomic.h"
#include "devils_engine/utils/fileio.h"
#include "devils_engine/utils/named_serializer.h"
#include "devils_engine/utils/sha256.h"
#include "devils_engine/utils/string-utils.hpp"
#include "devils_engine/utils/time-utils.hpp"
#include "folder_module.h"
#include "module_interface.h"
#include "module_system.h"
#include "resource_system.h"

namespace fs = std::filesystem;

namespace devils_engine {
namespace demiurg {
module_interface::module_interface(std::string path) noexcept : _path(std::move(path)) {}

std::string_view module_interface::path() const noexcept {
  return _path;
}

std::vector<uint8_t> module_interface::load_binary(const std::string& path) const {
  std::vector<uint8_t> memory;
  load_binary(path, memory);
  return memory;
}

std::string module_interface::load_text(const std::string& path) const {
  std::string memory;
  load_text(path, memory);
  return memory;
}

resource_interface::resource_interface() noexcept
  : type_id(0),
    loading_type_id(0),
    module(nullptr),
    raw_size(0),
    list_index(invalid_list_index),
    list_start_line(0),
    list_offset(SIZE_MAX),
    list_size(0),
    _state(0) {}

void resource_interface::add_dependency(resource_interface* dep) {
  if (dep != nullptr && dep != this) {
    dependencies.push_back(dep);
  }
}

bool resource_interface::is_list_entry() const noexcept {
  return list_index != invalid_list_index;
}

bool resource_interface::is_type(const size_t requested_type_id) const noexcept {
  return type_id == requested_type_id || loading_type_id == requested_type_id;
}

resource_interface* resource_handle::get() const noexcept {
  return system != nullptr ? system->get(hash) : nullptr;
}

resource_handle::operator bool() const noexcept {
  return get() != nullptr;
}

namespace {
resource_interface* instantiate_resource(resource_system& sys, const resource_candidate& candidate) {
  auto* res = sys.create(candidate.id, candidate.ext);
  if (res == nullptr) {
    return nullptr;
  }

  res->set(candidate.path, candidate.module_name, candidate.id, candidate.ext);
  res->module = candidate.module;
  res->raw_size = candidate.raw_size;
  res->list_index = candidate.list_index;
  res->list_start_line = candidate.list_start_line;
  res->list_offset = candidate.list_offset;
  res->list_size = candidate.list_size;
  res->list_name = candidate.list_name;
  res->list_section = candidate.list_section;
  return res;
}

uint32_t parse_index_alias(const std::string_view id, const std::string_view alias) {
  const size_t colon = id.rfind(':');
  if (colon == std::string_view::npos) {
    return invalid_list_index;
  }
  const std::string_view prefix = id.substr(0, colon + 1);
  if (alias.substr(0, prefix.size()) != prefix) {
    return invalid_list_index;
  }

  const std::string_view value = alias.substr(prefix.size());
  if (value.empty()) {
    return invalid_list_index;
  }

  uint32_t index = invalid_list_index;
  const auto* begin = value.data();
  const auto* end = value.data() + value.size();
  const auto res = std::from_chars(begin, end, index);
  if (res.ec != std::errc{} || res.ptr != end) {
    return invalid_list_index;
  }
  return index;
}
} // namespace

resource_system::type::type(
  std::string name,
  std::string ext,
  const size_t allocator_size,
  const size_t block_size,
  const size_t allocator_align,
  resource_producer create) noexcept : name(std::move(name)),
                                       ext(std::move(ext)),
                                       type_list(nullptr),
                                       allocator(allocator_size, block_size, allocator_align),
                                       createf(std::move(create)) {
  auto sp = std::span(exts.data(), exts.size());
  const size_t count = utils::string::split(this->ext, ",", sp);
  if (count == SIZE_MAX) {
    utils::error{}("Found to many extensions in str '{}' when creating demiurg resource type '{}'", this->ext, this->name);
  }
}

resource_interface* resource_system::type::create() {
  auto ptr = createf(allocator);
  ptr->type = name;
  if (type_list == nullptr) {
    type_list = ptr;
  } else {
    type_list->exemplary_radd(ptr);
  }
  return ptr;
}

void resource_system::type::destroy(resource_interface* ptr) {
  if (type_list == ptr) {
    type_list = type_list->exemplary_next(type_list);
  }
  allocator.destroy(ptr);
}

size_t resource_system::type::find_ext(const std::string_view& str) const {
  size_t i = 0;
  for (; i < exts.size() && exts[i] != str; ++i) {
  }
  return i >= exts.size() ? SIZE_MAX : i;
}

resource_system::resource_system() noexcept {}

resource_system::~resource_system() noexcept {
  clear();
  for (auto& [name, ptr] : types) {
    types_pool.destroy(ptr);
  }
}

resource_interface* resource_system::create(const std::string_view& id, const std::string_view& extension) {
  auto t = find_proper_type(id, extension);
  if (t == nullptr) {
    return nullptr;
  }
  auto ptr = t->create();
  all_resources.push_back(ptr);
  return ptr;
}

static bool get_f(const resource_interface* const res, const std::string_view& value) {
  std::less<std::string_view> l;
  return l(res->id, value);
}

resource_interface* resource_system::get(const std::string_view& id) const {
  if (id == "") {
    return nullptr;
  }

  const auto itr = std::lower_bound(resources.begin(), resources.end(), id, &get_f);
  if (itr != resources.end() && (*itr)->id == id) {
    return (*itr);
  }

  const auto alias_itr = aliases.find(id);
  if (alias_itr == aliases.end()) {
    return nullptr;
  }
  return alias_itr->second;
}

resource_interface* resource_system::get(const utils::id hash) const noexcept {
  if (hash == utils::invalid_id) {
    return nullptr;
  }
  const auto itr = resources_by_hash.find(hash);
  if (itr == resources_by_hash.end()) {
    return nullptr;
  }
  return itr->second.res;
}

utils::id resource_system::resource_hash(const std::string_view& id) noexcept {
  if (id.empty()) {
    return utils::invalid_id;
  }
  return utils::string_hash(id);
}

resource_handle resource_system::handle(const std::string_view& id) const noexcept {
  return resource_handle{this, resource_hash(id)};
}

resource_handle resource_system::handle(const utils::id hash) const noexcept {
  return resource_handle{this, hash};
}

static bool lazy_compare(const std::string_view& a, const std::string_view& b) {
  return a.substr(0, b.size()) == b;
}

view<> resource_system::find(const std::string_view& filter) const {
  const auto span = raw_find(filter);
  return view<>(span.begin(), span.end());
}

std::span<resource_interface* const> resource_system::raw_find(const std::string_view& filter) const {
  if (filter == "") {
    return std::span(resources);
  }

  const auto itr = std::lower_bound(resources.begin(), resources.end(), filter, [](const resource_interface* const res, const std::string_view& value) {
    std::less<std::string_view> l;
    return l(res->id.substr(0, value.size()), value);
  });

  if (itr == resources.end()) {
    return std::span<resource_interface* const>();
  }

  //auto start = resources.begin();
  auto start = itr;
  auto prev = itr;
  auto end = itr;
  // (*start)->id.substr(0, filter.size()) != filter
  // (*end)->id.substr(0, filter.size()) == filter
  for (; start != resources.begin() && lazy_compare((*start)->id, filter); prev = start, --start) {
  }
  if (lazy_compare((*start)->id, filter)) {
    prev = start;
  }

  for (; end != resources.end() && lazy_compare((*end)->id, filter); ++end) {
  }
  return std::span(prev, end);
}

std::vector<resource_system::manifest_entry> resource_system::resolve_manifest(
  const std::vector<resource_candidate>& candidates) const {
  std::vector<typed_candidate> typed;
  typed.reserve(candidates.size());

  for (size_t i = 0; i < candidates.size(); ++i) {
    const auto& candidate = candidates[i];
    auto* type = find_proper_type(candidate.id, candidate.ext);
    if (type == nullptr) {
      utils::warn("Could not find proper type for resource '{}' extension '{}'. Skip", candidate.id, candidate.ext);
      continue;
    }

    typed.push_back(typed_candidate{
      &candidate,
      type,
      type->find_ext(candidate.ext),
      i});
  }

  std::sort(typed.begin(), typed.end(), [](const typed_candidate& a, const typed_candidate& b) {
    std::less<std::string_view> less;
    if (less(a.candidate->id, b.candidate->id)) {
      return true;
    }
    if (less(b.candidate->id, a.candidate->id)) {
      return false;
    }
    if (a.candidate->module_priority != b.candidate->module_priority) {
      return a.candidate->module_priority < b.candidate->module_priority;
    }
    if (a.ext_index != b.ext_index) {
      return a.ext_index < b.ext_index;
    }
    return a.order < b.order;
  });

  std::vector<manifest_entry> manifest;
  for (size_t i = 0; i < typed.size();) {
    size_t end = i + 1;
    while (end < typed.size() && typed[end].candidate->id == typed[i].candidate->id) {
      end += 1;
    }

    uint32_t winner_priority = std::numeric_limits<uint32_t>::max();
    for (size_t j = i; j < end; ++j) {
      winner_priority = std::min(winner_priority, typed[j].candidate->module_priority);
    }

    size_t primary_index = SIZE_MAX;
    for (size_t j = i; j < end; ++j) {
      if (typed[j].candidate->module_priority != winner_priority) {
        continue;
      }
      if (primary_index == SIZE_MAX || typed[j].ext_index < typed[primary_index].ext_index) {
        primary_index = j;
      }
    }

    if (primary_index == SIZE_MAX) {
      i = end;
      continue;
    }

    manifest_entry entry{typed[primary_index], {}, {}, invalid_list_index};
    bool inherited_aliases = false;
    for (size_t j = i; j < end; ++j) {
      if (typed[j].candidate->module_priority == winner_priority) {
        continue;
      }
      for (const auto& alias : typed[j].candidate->aliases) {
        entry.aliases.push_back(alias);
        inherited_aliases = true;
        if (entry.list_index_override == invalid_list_index) {
          entry.list_index_override = parse_index_alias(typed[primary_index].candidate->id, alias);
        }
      }
    }

    if (!inherited_aliases) {
      for (const auto& alias : typed[primary_index].candidate->aliases) {
        entry.aliases.push_back(alias);
      }
    }

    for (size_t j = i; j < end; ++j) {
      if (j == primary_index) {
        continue;
      }
      if (typed[j].candidate->module_priority != winner_priority) {
        continue;
      }
      if (typed[j].resource_type != entry.primary.resource_type) {
        utils::warn(
          "Resource '{}' file '{}' has type '{}' but primary file '{}' has type '{}'. Skip supplementary",
          typed[j].candidate->id,
          typed[j].candidate->path,
          typed[j].resource_type->name,
          entry.primary.candidate->path,
          entry.primary.resource_type->name);
        continue;
      }

      entry.supplementary.push_back(typed[j]);
    }

    manifest.push_back(std::move(entry));
    i = end;
  }

  return manifest;
}

void resource_system::sort_active_resources(std::vector<resource_interface*>& resources) {
  std::sort(resources.begin(), resources.end(), [](auto a, auto b) {
    std::less<std::string_view> l;
    return l(a->id, b->id);
  });
}

void resource_system::instantiate_manifest(
  const std::vector<manifest_entry>& manifest,
  std::vector<resource_interface*>* pending) {
  for (const auto& entry : manifest) {
    const auto& primary_candidate = *entry.primary.candidate;
    auto* primary = instantiate_resource(*this, primary_candidate);
    if (primary == nullptr) {
      utils::warn("Could not create resource '{}' extension '{}'. Skip", primary_candidate.id, primary_candidate.ext);
      continue;
    }
    if (entry.list_index_override != invalid_list_index) {
      primary->list_index = entry.list_index_override;
    }

    if (pending != nullptr) {
      pending->push_back(primary);
    } else {
      resources.push_back(primary);
    }

    for (const auto& alias : entry.aliases) {
      register_alias(alias, primary);
    }

    for (const auto& supplementary : entry.supplementary) {
      const auto& candidate = *supplementary.candidate;
      auto* res = instantiate_resource(*this, candidate);
      if (res == nullptr) {
        utils::warn("Could not create supplementary resource '{}' extension '{}'. Skip", candidate.id, candidate.ext);
        continue;
      }

      primary->supplementary_radd(res);
    }
  }
}

void resource_system::register_alias(std::string alias, resource_interface* res) {
  if (alias.empty() || res == nullptr || alias == res->id) {
    return;
  }
  const auto active_itr = std::find_if(resources.begin(), resources.end(), [&](const resource_interface* cur) {
    return cur != nullptr && cur->id == alias;
  });
  if (active_itr != resources.end() || aliases.find(alias) != aliases.end()) {
    utils::warn("demiurg: resource alias '{}' for '{}' conflicts with an existing resource id/alias; skip", alias, res->id);
    return;
  }

  alias_storage.push_back(std::move(alias));
  aliases[std::string_view(alias_storage.back())] = res;
}

void resource_system::register_hash_key(const std::string_view id, resource_interface* res) {
  if (id.empty() || res == nullptr) {
    return;
  }
  const auto hash = resource_hash(id);
  if (hash == utils::invalid_id) {
    utils::error{}("demiurg: resource id '{}' produced invalid hash value", id);
  }

  const auto [itr, inserted] = resources_by_hash.emplace(hash, hashed_resource{id, res});
  if (inserted) {
    return;
  }

  if (itr->second.id != id) {
    utils::error{}(
      "demiurg: resource id hash collision: '{}' and '{}' both hash to {}",
      itr->second.id,
      id,
      hash);
  }

  if (itr->second.res != res) {
    utils::warn("demiurg: duplicate resource hash key '{}' points to both '{}' and '{}'; keeping first", id, itr->second.res->id, res->id);
  }
}

void resource_system::rebuild_hash_index() {
  resources_by_hash.clear();
  resources_by_hash.reserve(resources.size() + aliases.size());

  for (auto* res : resources) {
    if (res == nullptr) {
      continue;
    }
    register_hash_key(res->id, res);
  }

  for (const auto& [alias, res] : aliases) {
    register_hash_key(alias, res);
  }
}

void resource_system::parse_resources(module_system* sys) {
  install_catalogue_introspection();
  using parse_t = catalogue_domain::fn_traits<&resource_system::parse_resources_impl, "resource_system.parse_resources", "self", "modules">;
  parse_t::loc_fn_t{}(*this, sys);
}

void resource_system::parse_resources_impl(module_system* sys) {
  clear();

  {
    std::vector<resource_candidate> candidates;
    sys->open_modules();
    sys->discover_resources(candidates);
    sys->close_modules();

    auto manifest = resolve_manifest(candidates);
    instantiate_manifest(manifest);

    manifest.clear();
    candidates.clear();
  }

  sort_active_resources(resources);
  rebuild_hash_index();
  DE_LOG(catalogue::log_domain::demiurg, flow, "resource_system: parsed {} active resources ({} instantiated)", resources.size(), all_resources.size());
}

void resource_system::append_resources(module_system* sys) {
  install_catalogue_introspection();
  using append_t = catalogue_domain::fn_traits<&resource_system::append_resources_impl, "resource_system.append_resources", "self", "modules">;
  append_t::loc_fn_t{}(*this, sys);
}

void resource_system::append_resources_impl(module_system* sys) {
  std::vector<resource_interface*> pending;

  {
    std::vector<resource_candidate> candidates;
    sys->open_modules();
    sys->discover_resources(candidates);
    sys->close_modules();

    auto manifest = resolve_manifest(candidates);
    for (auto itr = manifest.begin(); itr != manifest.end();) {
      const auto& id = itr->primary.candidate->id;
      if (get(id) != nullptr) {
        utils::warn("append_resources: resource id '{}' is already registered; append does not support overrides, skipping", id);
        itr = manifest.erase(itr);
      } else {
        ++itr;
      }
    }

    instantiate_manifest(manifest, &pending);

    manifest.clear();
    candidates.clear();
  }

  resources.insert(resources.end(), pending.begin(), pending.end());
  sort_active_resources(resources);
  rebuild_hash_index();
  DE_LOG(catalogue::log_domain::demiurg, flow, "resource_system: appended {} active resources ({} instantiated total)", pending.size(), all_resources.size());
}

void resource_system::clear() {
  for (auto ptr : all_resources) {
    const auto itr = types.find(ptr->type);
    assert(itr != types.end());
    itr->second->destroy(ptr);
  }
  resources.clear();
  all_resources.clear();
  aliases.clear();
  alias_storage.clear();
  resources_by_hash.clear();
}

size_t resource_system::resources_count() const noexcept {
  return resources.size();
}
size_t resource_system::all_resources_count() const noexcept {
  return all_resources.size();
}

// if type is models/monster1
// then something/somesome/abc/models/monster1 is prior over models/monster1/something/somesome/abc
resource_system::type* resource_system::find_proper_type(const std::string_view& id, const std::string_view& extension) const {
  type* t = nullptr;
  std::string_view current_full_str = id;
  while (current_full_str.size() > 0 && t == nullptr) {
    size_t end = current_full_str.size();
    while (end != std::string_view::npos && t == nullptr) {
      const auto cur_id = current_full_str.substr(0, end);
      const auto itr = types.find(cur_id);
      if (itr != types.end()) {
        auto found_t = itr->second;
        const size_t ext_index = found_t->find_ext(extension);
        if (ext_index != SIZE_MAX) {
          t = itr->second;
        }
      }

      end = current_full_str.rfind('/', end - 1);
    }

    const size_t slash_index = current_full_str.find('/');
    current_full_str = slash_index == std::string_view::npos ? "" : current_full_str.substr(slash_index + 1);
  }

  return t;
}

static std::tuple<std::string_view, std::string_view, std::string_view> parse_path(const std::string_view& path) {
  const size_t last_slash = path.rfind('/');
  const size_t last_dot = path.rfind('.');
  const auto ext = last_dot != std::string_view::npos ? path.substr(last_dot + 1) : std::string_view();
  const auto id = path.substr(0, last_dot);
  const auto name = path.substr(last_slash + 1).substr(0, last_dot);
  return std::make_tuple(id, name, ext);
}

void resource_interface::set(std::string path, const std::string_view& module_name, const std::string_view& id, const std::string_view& ext) {
  this->path = std::move(path);
  this->module_name = module_name;
  const auto [local_id, local_name, local_ext] = parse_path(this->path);
  id_storage = !id.empty() ? std::string(id) : std::string(local_id);
  ext_storage = !ext.empty() ? std::string(ext) : std::string(local_ext);
  this->id = id_storage;
  this->ext = ext_storage;
}

uint32_t resource_interface::source_line(const uint32_t local_line) const noexcept {
  if (local_line == 0) {
    return 0;
  }
  if (!is_list_entry() || list_start_line == 0) {
    return local_line;
  }
  return list_start_line + local_line - 1;
}

resource_interface* resource_interface::replacement_next(const resource_interface* ptr) const {
  return utils::ring::list_next<list_type::replacement>(this, ptr);
}

resource_interface* resource_interface::supplementary_next(const resource_interface* ptr) const {
  return utils::ring::list_next<list_type::supplementary>(this, ptr);
}

resource_interface* resource_interface::exemplary_next(const resource_interface* ptr) const {
  return utils::ring::list_next<list_type::exemplary>(this, ptr);
}

void resource_interface::replacement_add(resource_interface* ptr) {
  utils::ring::list_add<list_type::replacement>(this, ptr);
}

void resource_interface::supplementary_add(resource_interface* ptr) {
  utils::ring::list_add<list_type::supplementary>(this, ptr);
}

void resource_interface::exemplary_add(resource_interface* ptr) {
  utils::ring::list_add<list_type::exemplary>(this, ptr);
}

void resource_interface::replacement_radd(resource_interface* ptr) {
  utils::ring::list_radd<list_type::replacement>(this, ptr);
}

void resource_interface::supplementary_radd(resource_interface* ptr) {
  utils::ring::list_radd<list_type::supplementary>(this, ptr);
}

void resource_interface::exemplary_radd(resource_interface* ptr) {
  utils::ring::list_radd<list_type::exemplary>(this, ptr);
}

void resource_interface::replacement_remove() {
  utils::ring::list_remove<list_type::replacement>(this);
}

void resource_interface::supplementary_remove() {
  utils::ring::list_remove<list_type::supplementary>(this);
}

void resource_interface::exemplary_remove() {
  utils::ring::list_remove<list_type::exemplary>(this);
}

int32_t resource_interface::top_state() const {
  return static_cast<int32_t>(state::hot);
}

void resource_interface::load_step(const int32_t from, const utils::safe_handle_t& handle) {
  // дефолтная 3-state лестница: 0->1 load_cold, 1->2 load_warm
  switch (from) {
    case state::cold: load_cold(handle); break;
    case state::warm: load_warm(handle); break;
    default: break; // многошаговые ресурсы переопределяют load_step для доп. уровней
  }
}

void resource_interface::unload_step(const int32_t from, const utils::safe_handle_t& handle) {
  // дефолтная 3-state лестница вниз: 2->1 unload_hot, 1->0 unload_warm
  switch (from) {
    case state::hot: unload_hot(handle); break;
    case state::warm: unload_warm(handle); break;
    default: break;
  }
}

bool resource_interface::is_external_step(const int32_t from) const {
  // прежняя эвристика loader'а: warm->hot внешний (GPU/рендер), если не CPU-only
  return from == static_cast<int32_t>(state::warm) && !flag(resource_flags::warm_and_hot_same);
}

int32_t resource_interface::final_state() const {
  // warm_and_hot_same => ресурс "готов" уже на warm (hot-переход не выполняется)
  return flag(resource_flags::warm_and_hot_same) ? static_cast<int32_t>(state::warm) : top_state();
}

void resource_interface::load(const utils::safe_handle_t& handle) {
  const int32_t cur = _state.load(std::memory_order_relaxed);
  if (cur >= final_state()) {
    return; // уже готов
  }
  load_step(cur, handle);
  _state.fetch_add(1, std::memory_order_relaxed);
  thread::atomic_min(_state, final_state());
  const int32_t next = _state.load(std::memory_order_relaxed);
  DE_LOG(catalogue::log_domain::demiurg, flow, "resource loaded '{}' module '{}' type '{}' level {}->{}", id, module_name, type, cur, next);
}

void resource_interface::unload(const utils::safe_handle_t& handle) {
  const int32_t cur = _state.load(std::memory_order_relaxed);
  if (cur <= static_cast<int32_t>(state::cold)) {
    return;
  }
  const bool direct_hot_to_cold = cur == static_cast<int32_t>(state::hot) && flag(resource_flags::hot_unload_to_cold);
  // force_unload_warm (сейчас нигде не ставится) — исторически пропускал выгрузку с warm
  if (!(cur == static_cast<int32_t>(state::warm) && flag(resource_flags::force_unload_warm))) {
    unload_step(cur, handle);
  }
  if (direct_hot_to_cold) {
    if (!flag(resource_flags::force_unload_warm)) {
      unload_step(static_cast<int32_t>(state::warm), handle);
    }
    _state.store(static_cast<int32_t>(state::cold), std::memory_order_relaxed);
  } else {
    _state.fetch_add(-1, std::memory_order_relaxed);
  }
  thread::atomic_max(_state, 0);
  const int32_t next = _state.load(std::memory_order_relaxed);
  DE_LOG(catalogue::log_domain::demiurg, flow, "resource unloaded '{}' module '{}' type '{}' level {}->{}", id, module_name, type, cur, next);
}

void resource_interface::force_unload(const utils::safe_handle_t& handle) {
  const int32_t cur = _state.load(std::memory_order_relaxed);
  if (cur <= static_cast<int32_t>(state::cold)) {
    return;
  }
  const bool direct_hot_to_cold = cur == static_cast<int32_t>(state::hot) && flag(resource_flags::hot_unload_to_cold);
  unload_step(cur, handle);
  if (direct_hot_to_cold) {
    unload_step(static_cast<int32_t>(state::warm), handle);
    _state.store(static_cast<int32_t>(state::cold), std::memory_order_relaxed);
  } else {
    _state.fetch_add(-1, std::memory_order_relaxed);
  }
  thread::atomic_max(_state, 0);
  const int32_t next = _state.load(std::memory_order_relaxed);
  DE_LOG(catalogue::log_domain::demiurg, flow, "resource force-unloaded '{}' module '{}' type '{}' level {}->{}", id, module_name, type, cur, next);
}

int32_t resource_interface::state() const {
  return _state.load(std::memory_order_relaxed);
}

bool resource_interface::usable() const {
  return _state.load(std::memory_order_relaxed) >= final_state();
}
} // namespace demiurg
} // namespace devils_engine
