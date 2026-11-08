#include "devils_engine/originator/generator_chain.h"
#include "devils_engine/originator/primitive_field.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <bit>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>

#include <tavl/tavl.h>

#include "devils_engine/utils/core.h"
#include "devils_engine/utils/prng.h"
#include "devils_engine/utils/string_id.h"

// Цепочка генераторов: разбор документа, разрешение связей и кэш экземпляров.
//
// СБОРКА ЭКЗЕМПЛЯРА: сначала связи (рекурсивно, через тот же кэш), потом пайплайн с подключёнными
// буферами производителей или нативное тело, потом объявленные выходы уходят в результат — у
// пайплайна они ЗАБИРАЮТСЯ, а не копируются: пайплайн строится на каждую сборку и после неё не нужен.
//
// КЭШ: на генератор одна карта (зерно мира, ключ) -> shared_future под мьютексом генератора. Мьютекс
// не держится во время сборки, поэтому вложенные запросы (пещера просит старшие пещеры) берут его
// заново. Вытесняется давно не нужный ГОТОВЫЙ экземпляр; недостроенный не вытесняется никогда —
// его ждут.
//
// ЦИКЛ в одном потоке ловится стеком сборок потока. Между потоками цикл стал бы взаимным ожиданием,
// и проверить его здесь нечем — см. заголовок.

namespace devils_engine {
namespace originator {

namespace {

struct link_mirror {
  std::string name;
  std::string from;
  // `same` или `буфер.поле`.
  std::string key;
  std::vector<uint32_t> project;
  std::vector<std::string> keys;
  std::string count;
};

struct field_term_mirror {
  std::string kind;
  std::string link;
};

struct field_mirror {
  std::string name;
  std::vector<field_term_mirror> terms;
};

struct generator_mirror {
  std::string name;
  uint32_t key = 1;
  std::string entry;
  std::string native;
  std::string buffers;
  std::vector<std::string> outputs;
  std::vector<link_mirror> links;
  std::vector<field_mirror> fields;
  std::map<std::string, std::string> trim;
  size_t capacity = 0;
  bool keep_fields = false;
};

struct module_mirror {
  std::string name;
  std::string path;
};

struct shape_mirror {
  std::string name;
  std::string buffer;
  std::string count;
  std::string box_a;
  std::string box_b;
  std::string box;
  std::string reach;
  std::string tag;
  std::string value;
  std::string role;
  double blend = 0.0;
  struct part_mirror {
    std::string buffer;
    std::string first;
    std::string count;
    std::string op;
    std::string value;
    std::string box_a;
    std::string box_b;
  };
  std::vector<part_mirror> parts;
};

struct noise_slot_mirror {
  std::string name;
  uint64_t seed_add = 0;
  double period = 1.0;
};

struct background_term_mirror {
  std::string slot;
  double weight = 0.0;
};

struct background_mirror {
  bool enabled = false;
  double base = 0.0;
  double outside = 0.0;
  double y_min = -1e30;
  double y_max = 1e30;
  double upper = 0.0;
  double lower = 0.0;
  uint32_t tag = 0;
  std::vector<background_term_mirror> terms;
  std::string y_min_constant;
  std::string y_max_constant;
};

struct chain_mirror {
  std::string name;
  std::string constants;
  std::vector<module_mirror> modules;
  std::vector<noise_slot_mirror> noise_slots;
  background_mirror background;
  std::vector<shape_mirror> shapes;
  std::vector<generator_mirror> generators;
};

void report(const tavl::ct_context& ctx, const std::string_view& label) {
  for (const auto& d : ctx.diagnostics) {
    if (!d.error.is_critical()) {
      continue;
    }
    utils::error{}("originator: could not parse chain '{}': error '{}' at {}:{} field '{}'", label,
                   tavl::to_string(d.error.type), d.error.span.line, d.error.span.column, d.field);
  }
}

// `буфер.поле` -> пара имён. Точка обязательна: связь читает ДАННЫЕ, а данные называются полем.
std::pair<std::string_view, std::string_view> split_field(const std::string_view& reference,
                                                          const std::string_view& generator,
                                                          const std::string_view& link) {
  const auto dot = reference.find('.');
  if (dot == std::string_view::npos || dot == 0 || dot + 1 == reference.size()) {
    utils::error{}("originator chain generator '{}', link '{}': '{}' must name a field as buffer.field", generator,
                   link, reference);
  }
  return {reference.substr(0, dot), reference.substr(dot + 1)};
}

} // namespace

struct generator_chain::build_node {
  const void* at = nullptr;
  uint64_t seed = 0;
  instance_key key;
  std::shared_ptr<const build_node> parent;
};

struct generator_chain::pending_build {
  const slot* at = nullptr;
  uint64_t seed = 0;
  instance_key key;
  std::shared_ptr<const build_node> parent;
  std::promise<result_ptr> promise;
  std::atomic<bool> started{false};
};

void generation_context::release() {
  if (!building.empty() || step_depth != 0 || fields != nullptr) utils::error{}("originator: cannot release an active generation context");
  hosts.clear();
  decltype(building)().swap(building);
  current_node.reset();
  field_scratch.release();
}
generation_context generation_context::fork_task() const {
  generation_context child;
  child.building = building;
  child.step_depth = step_depth;
  child.current_node = current_node;
  child.fields = fields;
  return child;
}
struct generator_chain::context_slot {
  generation_context context;
  bool busy = false;
  std::thread::id owner;
};
struct generator_chain::context_lease {
  const generator_chain* chain;
  context_slot* slot;
  context_lease(const generator_chain* chain_, context_slot* slot_) : chain(chain_), slot(slot_) {}
  context_lease(const context_lease&) = delete;
  ~context_lease() {
    std::lock_guard lock(chain->contexts_lock_);
    slot->busy = false;
    slot->owner = {};
  }
};
generator_chain::~generator_chain() {
  // Lua finalizers and captured tools must die while the chain's other members still exist.
  release_contexts();
}
generator_chain::context_lease generator_chain::acquire_context() const {
  std::lock_guard lock(contexts_lock_);
  const auto caller = std::this_thread::get_id();
  context_slot* free = nullptr;
  for (const auto& slot : contexts_) {
    if (slot->busy && slot->owner == caller) utils::error{}("originator: nested chain call must pass run.context() explicitly");
    if (!slot->busy) free = slot.get();
  }
  if (!free) { contexts_.push_back(std::make_unique<context_slot>()); free = contexts_.back().get(); }
  free->busy = true;
  free->owner = caller;
  return context_lease(this, free);
}
void generator_chain::release_contexts() const {
  std::lock_guard lock(contexts_lock_);
  for (const auto& slot : contexts_) if (slot->busy) utils::error{}("originator: cannot release active chain contexts");
  contexts_.clear();
}
size_t generator_chain::context_count() const {
  std::lock_guard lock(contexts_lock_);
  return contexts_.size();
}
result_ptr generator_chain::get(const std::string_view& name, uint64_t seed, const instance_key& key) const {
  auto lease = acquire_context();
  return get(lease.slot->context, name, seed, key);
}
void generator_chain::prefetch(const std::string_view& name, uint64_t seed, const instance_key& key) const {
  auto lease = acquire_context();
  prefetch(lease.slot->context, name, seed, key);
}
std::string generator_chain::verify(const std::string_view& name, uint64_t seed, const instance_key& key) const {
  auto lease = acquire_context();
  return verify(lease.slot->context, name, seed, key);
}
std::shared_ptr<const primitive_field> generator_chain::field(const std::string_view& name, uint64_t seed,
                                                            const instance_key& key, const std::string_view& field_name) const {
  auto lease = acquire_context();
  return field(lease.slot->context, name, seed, key, field_name);
}

uint64_t read_key_word(const const_field_accessor& field, const size_t element, const uint32_t word) {
  if (!is_wide_integer(field.type().base) || field.type().components <= word) {
    utils::error{}("originator: a key word is one 'ul' component; word {} needs ul{} or wider, got {}{}", word,
                   word + 1, to_string(field.type().base), field.type().components);
  }
  return std::bit_cast<uint64_t>(field.get_integer(element, word));
}

void write_key_word(const field_accessor& field, const size_t element, const uint64_t value, const uint32_t word) {
  if (!is_wide_integer(field.type().base) || field.type().components <= word) {
    utils::error{}("originator: a key word is one 'ul' component; word {} needs ul{} or wider, got {}{}", word,
                   word + 1, to_string(field.type().base), field.type().components);
  }
  field.set_integer(element, std::bit_cast<int64_t>(value), word);
}

chain_description parse_chain(const std::string_view& text, const std::string_view& label) {
  tavl::parser p;
  p.add_default_operator();
  p.flush(std::string(text));
  p.finish();

  tavl::ct_context ctx;
  chain_mirror mirror{};
  tavl::deserialize_next(p, ctx, mirror);
  report(ctx, label);

  if (mirror.generators.empty()) {
    utils::error{}("originator chain '{}': 'generators' is empty", label);
  }

  chain_description out;
  out.name = mirror.name.empty() ? std::string(label) : mirror.name;
  out.constants = mirror.constants;
  for (auto& m : mirror.modules) {
    if (m.name.empty() || m.path.empty()) {
      utils::error{}("originator chain '{}': module {} needs both a name and a path", label, out.modules.size());
    }
    for (const auto& known : out.modules) {
      if (known.name == m.name) utils::error{}("originator chain '{}': module '{}' is declared twice", label, m.name);
    }
    out.modules.push_back({std::move(m.name), std::move(m.path)});
  }
  for (auto& slot : mirror.noise_slots) {
    if (slot.name.empty() || !std::isfinite(slot.period) || slot.period <= 0.0) {
      utils::error{}("originator chain '{}': invalid noise slot '{}' with period {}", label, slot.name, slot.period);
    }
    for (const auto& known : out.noise_slots) {
      if (known.name == slot.name) utils::error{}("originator chain '{}': noise slot '{}' declared twice", label, slot.name);
    }
    out.noise_slots.push_back({std::move(slot.name), slot.seed_add, slot.period});
  }
  if (mirror.background.enabled) {
    const auto& b = mirror.background;
    if (!std::isfinite(b.base) || !std::isfinite(b.outside) || !std::isfinite(b.y_min) ||
        !std::isfinite(b.y_max) || b.y_min > b.y_max || !std::isfinite(b.upper) || !std::isfinite(b.lower)) {
      utils::error{}("originator chain '{}': invalid background bounds", label);
    }
    out.has_background = true;
    out.background = {b.base, b.outside, b.y_min, b.y_max, b.upper, b.lower, b.tag, {},
                      b.y_min_constant, b.y_max_constant};
    for (const auto& term : b.terms) {
      if (term.slot.empty() || !std::isfinite(term.weight)) {
        utils::error{}("originator chain '{}': invalid background term", label);
      }
      out.background.terms.push_back({term.slot, term.weight});
    }
  }
  for (auto& m : mirror.shapes) {
    if (m.name.empty() || m.buffer.empty() || m.box_a.empty() || m.reach.empty() || m.value.empty()) {
      utils::error{}("originator chain '{}': shape {} needs name, buffer, box_a, reach and value", label,
                     out.shapes.size());
    }
    if (!m.role.empty() && m.role != "natural" && m.role != "fill" && m.role != "cut" && m.role != "insert") {
      utils::error{}("originator chain '{}': shape '{}' has role '{}', expected natural, fill, cut or insert", label, m.name,
                     m.role);
    }
    if (!m.box.empty() && m.box != "capsule" && m.box != "aabb") {
      utils::error{}("originator chain '{}': shape '{}' has box '{}', expected capsule or aabb", label, m.name, m.box);
    }
    if (m.box == "aabb" && m.box_b.empty()) {
      utils::error{}("originator chain '{}': shape '{}' needs box_b for an aabb", label, m.name);
    }
    out.shapes.push_back({std::move(m.name), std::move(m.buffer), std::move(m.count), std::move(m.box_a),
                          std::move(m.box_b), std::move(m.box), std::move(m.reach), std::move(m.tag), std::move(m.value), {},
                          std::move(m.role), m.blend, {}});
    for (auto& part : m.parts) {
      if (part.buffer.empty() || part.first.empty() || part.count.empty() || part.value.empty() ||
          (part.op != "max" && part.op != "min") || part.box_a.empty() != part.box_b.empty()) {
        utils::error{}("originator chain '{}': shape '{}' has an invalid part; expected buffer, first, count, op=max|min and value",
                       label, out.shapes.back().name);
      }
      out.shapes.back().parts.push_back({std::move(part.buffer), std::move(part.first), std::move(part.count),
                                         std::move(part.op), std::move(part.value), {},
                                         std::move(part.box_a), std::move(part.box_b)});
    }
  }
  for (auto& g : mirror.generators) {
    if (g.name.empty()) {
      utils::error{}("originator chain '{}': generator {} has no name", label, out.generators.size());
    }
    chain_generator generator;
    generator.name = std::move(g.name);
    generator.key_words = g.key;
    generator.entry = std::move(g.entry);
    generator.native = std::move(g.native);
    generator.buffers = std::move(g.buffers);
    generator.outputs = std::move(g.outputs);
    generator.capacity = g.capacity;
    generator.keep_fields = g.keep_fields;
    for (auto& l : g.links) {
      chain_link link;
      link.name = l.name.empty() ? l.from : std::move(l.name);
      link.from = std::move(l.from);
      const int forms = int(!l.key.empty()) + int(!l.project.empty()) + int(!l.keys.empty());
      if (forms != 1) {
        utils::error{}("originator chain generator '{}', link '{}': give exactly one of key (same or buffer.field), "
                       "project (words of the own key) or keys (fan-in), got {}",
                       generator.name, link.name, forms);
      }
      if (!l.key.empty()) {
        if (l.key == "same") {
          link.type = chain_link::kind::same;
        } else {
          link.type = chain_link::kind::from_data;
          link.fields.push_back(std::move(l.key));
        }
      } else if (!l.project.empty()) {
        link.type = chain_link::kind::project;
        link.project = std::move(l.project);
      } else {
        // `keys` с `count` — fan-in; без `count` — один ключ, слова которого лежат в нескольких полях
        // (у ключа в три слова двух компонент `ui4` не хватит).
        link.type = l.count.empty() ? chain_link::kind::from_data : chain_link::kind::fan_in;
        link.fields = std::move(l.keys);
      }
      link.count = std::move(l.count);
      generator.links.push_back(std::move(link));
    }
    for (auto& [buffer_name, count] : g.trim) generator.trim.emplace_back(buffer_name, count);
    for (auto& f : g.fields) {
      chain_field field;
      field.name = std::move(f.name);
      if (field.name.empty() || f.terms.empty()) {
        utils::error{}("originator chain generator '{}': a field needs a name and terms", generator.name);
      }
      for (auto& t : f.terms) field.terms.push_back({std::move(t.kind), std::move(t.link)});
      generator.fields.push_back(std::move(field));
    }
    out.generators.push_back(std::move(generator));
  }
  return out;
}

const std::string& generator_result::generator() const noexcept { return generator_; }
uint64_t generator_result::seed() const noexcept { return seed_; }
const instance_key& generator_result::key() const noexcept { return key_; }
size_t generator_result::buffer_count() const noexcept { return buffers_.size(); }
const buffer& generator_result::buffer_at(const size_t index) const noexcept { return *buffers_[index]; }
std::shared_ptr<const void> generator_result::payload_handle() const noexcept { return payload_; }

const buffer* generator_result::find(const std::string_view& buffer_name) const noexcept {
  for (const auto& candidate : buffers_) {
    if (candidate->name() == buffer_name) {
      return candidate.get();
    }
  }
  return nullptr;
}

const buffer& generator_result::at(const std::string_view& buffer_name) const {
  const auto* found = find(buffer_name);
  if (found == nullptr) {
    utils::error{}("originator generator '{}': has no output '{}'", generator_, buffer_name);
  }
  return *found;
}

const_field_accessor generator_result::field(const std::string_view& buffer_name,
                                             const std::string_view& field_name) const {
  const auto& source = at(buffer_name);
  const size_t index = source.find_field(field_name);
  if (index == buffer_layout::npos) {
    utils::error{}("originator generator '{}': output '{}' has no field '{}'", generator_, buffer_name, field_name);
  }
  return source.field(index);
}

std::string generator_result::compare(const generator_result& other) const {
  if (buffers_.size() != other.buffers_.size()) {
    return std::format("{} outputs against {}", buffers_.size(), other.buffers_.size());
  }
  for (size_t i = 0; i < buffers_.size(); ++i) {
    const auto& a = *buffers_[i];
    const auto& b = *other.buffers_[i];
    if (a.name() != b.name() || a.byte_size() != b.byte_size()) {
      return std::format("output {} is '{}' of {} bytes against '{}' of {}", i, a.name(), a.byte_size(), b.name(),
                         b.byte_size());
    }
    if (std::memcmp(a.base_pointer(), b.base_pointer(), a.byte_size()) != 0) {
      size_t at = 0;
      while (at < a.byte_size() && a.base_pointer()[at] == b.base_pointer()[at]) ++at;
      return std::format("output '{}' differs first at byte {} of {}", a.name(), at, a.byte_size());
    }
  }
  return {};
}

generator_run::generator_run() = default;
generator_run::~generator_run() = default;

const primitive_field& generator_run::field(const std::string_view& field_name) const {
  for (const auto& [name, field] : fields_) {
    if (name == field_name) return *field;
  }
  utils::error{}("originator generator '{}': declares no field '{}'", generator_->name, field_name);
  return *fields_.front().second;
}

const chain_generator& generator_run::generator() const noexcept { return *generator_; }
uint64_t generator_run::world_seed() const noexcept { return world_seed_; }
uint64_t generator_run::seed() const noexcept { return seed_; }
const instance_key& generator_run::key() const noexcept { return key_; }
const parameters& generator_run::values() const noexcept { return *values_; }
std::span<const resolved_link> generator_run::links() const noexcept { return links_; }
pipeline* generator_run::line() noexcept { return line_.get(); }

const resolved_link& generator_run::link(const std::string_view& link_name) const {
  for (const auto& candidate : links_) {
    if (candidate.name == link_name) {
      return candidate;
    }
  }
  utils::error{}("originator generator '{}': has no link '{}'", generator_->name, link_name);
  return links_.front();
}

const generator_result& generator_run::linked(const std::string_view& link_name) const {
  const auto& found = link(link_name);
  if (found.results.size() != 1) {
    utils::error{}("originator generator '{}': link '{}' holds {} instances, not one", generator_->name, link_name,
                   found.results.size());
  }
  return *found.results.front();
}

void generator_run::run_steps() {
  if (line_ == nullptr) {
    utils::error{}("originator generator '{}': is native and has no steps to run", generator_->name);
  }
  if (!chain_->invokers_) {
    utils::error{}("originator generator '{}': the chain has no invoker source for pipeline steps", generator_->name);
  }
  line_->run(chain_->invokers_(*generator_, *context_));
}

buffer& generator_run::output(const std::string_view& buffer_name) {
  if (line_ != nullptr) {
    auto* found = line_->find_buffer(buffer_name);
    if (found == nullptr) {
      utils::error{}("originator generator '{}': has no own buffer '{}'", generator_->name, buffer_name);
    }
    return *found;
  }
  for (auto& candidate : native_buffers_) {
    if (candidate->name() == buffer_name) {
      return *candidate;
    }
  }
  utils::error{}("originator generator '{}': has no buffer '{}'", generator_->name, buffer_name);
  return *native_buffers_.front();
}

generator_chain::generator_chain(chain_description description, chain_setup setup) :
  description_(std::move(description)), setup_(std::move(setup)) {
  for (const auto& g : description_.generators) {
    for (const auto& other : slots_) {
      if (other->description.name == g.name) {
        utils::error{}("originator chain '{}': generator '{}' is declared twice", description_.name, g.name);
      }
    }
    auto at = std::make_unique<slot>();
    at->description = g;
    at->name_hash = utils::string_hash(g.name);
    at->capacity = g.capacity != 0 ? g.capacity : setup_.default_capacity;
    if (g.key_words == 0 || g.key_words > max_key_words) {
      utils::error{}("originator chain generator '{}': key has {} words, 1..{} are allowed", g.name, g.key_words,
                     max_key_words);
    }
    if (g.entry.empty() == g.native.empty()) {
      utils::error{}("originator chain generator '{}': name exactly one of 'entry' (a pipeline) or 'native' (a host "
                     "body)", g.name);
    }
    if (g.outputs.empty()) {
      utils::error{}("originator chain generator '{}': declares no outputs, so nothing of it can be cached or linked",
                     g.name);
    }

    if (!g.entry.empty()) {
      const auto found = std::find_if(setup_.pipelines.begin(), setup_.pipelines.end(),
                                      [&](const auto& item) { return item.first == g.name; });
      if (found == setup_.pipelines.end()) {
        utils::error{}("originator chain generator '{}': its pipeline description was not supplied", g.name);
      }
      at->has_pipeline = true;
      at->merged = found->second;
      at->merged.values.overlay(setup_.constants);
      at->merged.values.overlay(setup_.values);
      at->values = at->merged.values;
      const auto scripts = std::find_if(setup_.scripts.begin(), setup_.scripts.end(),
                                        [&](const auto& item) { return item.first == g.name; });
      if (scripts != setup_.scripts.end()) {
        at->scripts = scripts->second;
      }
      for (const auto& output : g.outputs) {
        const bool declared = std::any_of(at->merged.buffers.begin(), at->merged.buffers.end(),
                                          [&](const auto& b) { return b.name == output; });
        if (!declared) {
          utils::error{}("originator chain generator '{}': output '{}' is not among its declared buffers", g.name,
                         output);
        }
      }
    } else {
      const auto found = std::find_if(setup_.native_buffers.begin(), setup_.native_buffers.end(),
                                      [&](const auto& item) { return item.first == g.name; });
      if (found == setup_.native_buffers.end()) {
        utils::error{}("originator chain generator '{}': native, but its buffer declarations were not supplied", g.name);
      }
      at->buffers = &found->second;
      at->values = setup_.constants;
      at->values.overlay(setup_.values);
      for (const auto& output : g.outputs) {
        const bool declared = std::any_of(found->second.begin(), found->second.end(),
                                          [&](const auto& b) { return b.name == output; });
        if (!declared) {
          utils::error{}("originator chain generator '{}': output '{}' is not among its declared buffers", g.name,
                         output);
        }
      }
    }
    slots_.push_back(std::move(at));
  }

  for (const auto& at : slots_) {
    const auto& g = at->description;
    for (const auto& [buffer_name, count] : g.trim) {
      const auto is_output = [&](const std::string_view name) {
        return std::find(g.outputs.begin(), g.outputs.end(), name) != g.outputs.end();
      };
      const auto dot = count.find('.');
      if (!is_output(buffer_name) || dot == std::string::npos || !is_output(std::string_view(count).substr(0, dot))) {
        utils::error{}("originator chain generator '{}': trim '{} = {}' must name an output and an output field "
                       "holding its count", g.name, buffer_name, count);
      }
    }
  }

  // Связи проверяются, когда известны все генераторы: связь может вести к объявленному позже.
  for (const auto& at : slots_) {
    const auto& g = at->description;
    for (const auto& link : g.links) {
      const slot& from = find_slot(link.from);
      const uint32_t words = from.description.key_words;
      if (link.type == chain_link::kind::same && words != g.key_words) {
        utils::error{}("originator chain generator '{}', link '{}': 'same' needs equal keys, {} words against {}",
                       g.name, link.name, g.key_words, words);
      }
      if (link.type == chain_link::kind::project) {
        if (link.project.size() != words) {
          utils::error{}("originator chain generator '{}', link '{}': projects {} words, '{}' has a key of {}", g.name,
                         link.name, link.project.size(), link.from, words);
        }
        for (const uint32_t w : link.project) {
          if (w >= g.key_words) {
            utils::error{}("originator chain generator '{}', link '{}': word {} is beyond its own key of {}", g.name,
                           link.name, w, g.key_words);
          }
        }
      }
      for (const auto& other : g.links) {
        if (&other != &link && other.name == link.name) {
          utils::error{}("originator chain generator '{}': link '{}' is declared twice", g.name, link.name);
        }
      }
    }
  }

  for (const auto& at : slots_) {
    for (const auto& field : at->description.fields) {
      if (setup_.vocabulary == nullptr) {
        utils::error{}("originator chain generator '{}': declares field '{}' but the chain has no shape vocabulary",
                       at->description.name, field.name);
      }
      for (const auto& term : field.terms) {
        setup_.vocabulary->kind(term.kind);
        const bool known = std::any_of(at->description.links.begin(), at->description.links.end(),
                                       [&](const chain_link& l) { return l.name == term.link; });
        if (!known) {
          utils::error{}("originator chain generator '{}', field '{}': no link '{}'", at->description.name, field.name,
                         term.link);
        }
      }
    }
  }

  for (auto& at : slots_) {
    std::vector<const slot*> visiting;
    at->fingerprint = fingerprint_of(*at, visiting);
  }
}

uint64_t generator_chain::fingerprint_of(const slot& at, std::vector<const slot*>& visiting) const {
  if (std::find(visiting.begin(), visiting.end(), &at) != visiting.end()) {
    return 0; // связь на себя (старшие пещеры) или цикл: свой вклад уже учтён выше по стеку
  }
  visiting.push_back(&at);
  uint64_t h = utils::mix(at.name_hash, at.description.key_words);
  // Общие модули видны каждому генератору-пайплайну, поэтому входят в отпечаток каждого.
  for (const auto& module : setup_.modules) h = utils::mix(h, utils::string_hash(module.id), utils::string_hash(module.text));
  for (const auto& [name, text] : setup_.fingerprints) {
    if (name == at.description.name) {
      h = utils::mix(h, utils::string_hash(text));
    }
  }
  for (const auto& output : at.description.outputs) {
    h = utils::mix(h, utils::string_hash(output));
  }
  for (size_t i = 0; i < setup_.values.size(); ++i) {
    h = utils::mix(h, utils::string_hash(setup_.values.name_at(i)),
                   setup_.values.is_string_at(i) ? utils::string_hash(setup_.values.string_at(i)) :
                                                   std::bit_cast<uint64_t>(setup_.values.number_at(i)));
  }
  for (size_t i = 0; i < setup_.constants.size(); ++i) {
    h = utils::mix(h, utils::string_hash(setup_.constants.name_at(i)),
                   setup_.constants.is_string_at(i) ? utils::string_hash(setup_.constants.string_at(i)) :
                     std::bit_cast<uint64_t>(setup_.constants.number_at(i)));
  }
  for (const auto& [buffer_name, count] : at.description.trim) {
    h = utils::mix(h, utils::string_hash(buffer_name), utils::string_hash(count));
  }
  for (const auto& field : at.description.fields) {
    h = utils::mix(h, utils::string_hash(field.name));
    for (const auto& term : field.terms) h = utils::mix(h, utils::string_hash(term.kind), utils::string_hash(term.link));
  }
  for (const auto& link : at.description.links) {
    h = utils::mix(h, utils::string_hash(link.name), uint64_t(link.type), fingerprint_of(find_slot(link.from), visiting));
    for (const auto& field : link.fields) h = utils::mix(h, utils::string_hash(field));
    for (const uint32_t w : link.project) h = utils::mix(h, w);
  }
  visiting.pop_back();
  return h;
}

const chain_description& generator_chain::description() const noexcept { return description_; }

const chain_generator& generator_chain::generator(const std::string_view& name) const {
  return find_slot(name).description;
}

generator_chain::slot& generator_chain::find_slot(const std::string_view& name) {
  for (auto& at : slots_) {
    if (at->description.name == name) {
      return *at;
    }
  }
  utils::error{}("originator chain '{}': has no generator '{}'", description_.name, name);
  return *slots_.front();
}

const generator_chain::slot& generator_chain::find_slot(const std::string_view& name) const {
  return const_cast<generator_chain*>(this)->find_slot(name);
}

void generator_chain::set_native(const std::string_view& name, generator_executor body) {
  auto& at = find_slot(name);
  if (at.has_pipeline) {
    utils::error{}("originator chain generator '{}': is a pipeline, a native body would never run", name);
  }
  at.native = std::move(body);
}

void generator_chain::set_executor(const std::string_view& name, generator_executor executor) {
  auto& at = find_slot(name);
  if (!at.has_pipeline) {
    utils::error{}("originator chain generator '{}': is native, set its body with set_native", name);
  }
  at.executor = std::move(executor);
}

void generator_chain::set_invoker_source(invoker_source source) {
  release_contexts();
  invokers_ = std::move(source);
}
void generator_chain::set_invoker_source(std::function<step_invoker(const chain_generator&)> source) {
  set_invoker_source(invoker_source([source = std::move(source)](const chain_generator& generator, generation_context&) { return source(generator); }));
}


uint64_t generator_chain::fingerprint(const std::string_view& name) const { return find_slot(name).fingerprint; }

std::span<const script_text> generator_chain::modules() const noexcept { return setup_.modules; }
const parameters& generator_chain::constants() const noexcept { return setup_.constants; }

std::span<const script_text> generator_chain::scripts(const std::string_view& name) const {
  return find_slot(name).scripts;
}

generator_chain::statistics generator_chain::stats(const std::string_view& name) const {
  const auto& at = find_slot(name);
  std::lock_guard guard(at.lock);
  return at.stats;
}

std::vector<std::pair<std::string, generator_chain::statistics>> generator_chain::all_stats() const {
  std::vector<std::pair<std::string, statistics>> out;
  for (const auto& at : slots_) {
    std::lock_guard guard(at->lock);
    out.emplace_back(at->description.name, at->stats);
  }
  return out;
}

std::vector<std::pair<std::string, generator_chain::footprint>> generator_chain::cache_footprint() const {
  std::vector<std::pair<std::string, footprint>> out;
  for (const auto& at : slots_) {
    footprint f;
    std::lock_guard guard(at->lock);
    for (const auto& [key, e] : at->cache) {
      if (e.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
      const result_ptr& r = e.future.get();
      if (r == nullptr) continue;
      ++f.instances;
      for (size_t i = 0; i < r->buffer_count(); ++i) f.buffer_bytes += r->buffer_at(i).byte_size();
    }
    out.emplace_back(at->description.name, f);
  }
  return out;
}

void generator_chain::reset_stats() const {
  for (const auto& at : slots_) {
    std::lock_guard guard(at->lock);
    at->stats = {};
  }
}

result_ptr generator_chain::get(generation_context& context, const std::string_view& name, const uint64_t world_seed, const instance_key& key) const {
  const slot& at = find_slot(name);
  for (uint32_t w = at.description.key_words; w < max_key_words; ++w) {
    if (key.words[w] != 0) {
      // Иначе у одного экземпляра было бы несколько имён в кэше и несколько сборок.
      utils::error{}("originator chain generator '{}': key has {} words, word {} must be zero", name,
                     at.description.key_words, w);
    }
  }
  for (const auto& [who, seed, k] : context.building) {
    if (who == &at && seed == world_seed && k == key) {
      utils::error{}("originator chain generator '{}': instance {:x}/{:x}/{:x} links back to itself while being built",
                     name, key.words[0], key.words[1], key.words[2]);
    }
  }

  const auto made = claim(context, at, world_seed, key);
  if (made.job) run_claim(context, *made.job);
  wait_helping(context, at, world_seed, key, made.future);
  return made.future.get();
}

generator_chain::claimed generator_chain::claim(generation_context& context, const slot& at, const uint64_t world_seed, const instance_key& key) const {
  claimed out;
  const auto cache_key = std::make_pair(world_seed, key);
  std::lock_guard guard(at.lock);
  const auto found = at.cache.find(cache_key);
  if (found != at.cache.end()) {
    out.future = found->second.future;
    found->second.used = ++at.tick;
    ++at.stats.hits;
    return out;
  }
  if (at.cache.size() >= at.capacity) {
    auto oldest = at.cache.end();
    for (auto it = at.cache.begin(); it != at.cache.end(); ++it) {
      if (it->second.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
      if (oldest == at.cache.end() || it->second.used < oldest->second.used) oldest = it;
    }
    if (oldest != at.cache.end()) {
      at.cache.erase(oldest);
      ++at.stats.evictions;
    }
  }
  out.job = std::make_shared<pending_build>();
  out.job->at = &at;
  out.job->seed = world_seed;
  out.job->key = key;
  out.job->parent = std::static_pointer_cast<const build_node>(context.current_node);
  out.future = out.job->promise.get_future().share();
  at.cache.emplace(cache_key, entry{out.future, ++at.tick});
  ++at.stats.builds;
  return out;
}

void generator_chain::run_claim(generation_context& context, pending_build& job) const {
  if (job.started.exchange(true)) return;
  const slot& at = *job.at;
  auto node = std::make_shared<build_node>();
  node->at = &at;
  node->seed = job.seed;
  node->key = job.key;
  node->parent = job.parent;
  struct node_scope {
    std::shared_ptr<const void> previous;
    ~node_scope() { context.current_node = std::move(previous); }
    generation_context& context;
  } scope{std::exchange(context.current_node, std::shared_ptr<const void>(node)), context};
  context.building.emplace_back(&at, job.seed, job.key);
  try {
    job.promise.set_value(build(context, at, job.seed, job.key));
  } catch (...) {
    context.building.pop_back();
    // Неудачная сборка не остаётся в кэше: ждущие получат исключение из своей копии будущего, а
    // следующий запрос попробует снова, а не получит то же исключение навсегда.
    {
      std::lock_guard guard(at.lock);
      at.cache.erase(std::make_pair(job.seed, job.key));
    }
    job.promise.set_exception(std::current_exception());
    return;
  }
  context.building.pop_back();
}

void generator_chain::wait_helping(generation_context& context, const slot& at, const uint64_t world_seed, const instance_key& key,
                                   const std::shared_future<result_ptr>& future) const {
  if (future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) return;
  // Пока экземпляр строит другой поток, этот берёт захваченные сборки из ЕГО поддерева: они нужны
  // тому, кого ждём, и не могут зависеть ни от чего на стеке этого потока (иначе в графе был бы цикл).
  // Брать можно: сам ожидаемый экземпляр, его поддерево и поддерево собственной текущей сборки этого
  // потока — всё это нужно тем, кто на стеке, и ни от чего на стеке зависеть не может.
  const auto own = std::static_pointer_cast<const build_node>(context.current_node);
  const auto under_awaited = [&](const pending_build& job) {
    if (job.at == &at && job.seed == world_seed && job.key == key) return true;
    for (auto node = job.parent; node != nullptr; node = node->parent) {
      if (node->at == &at && node->seed == world_seed && node->key == key) return true;
      if (own != nullptr && node.get() == own.get()) return true;
    }
    return false;
  };
  uint64_t idle_ns = 0;
  while (future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
    std::shared_ptr<pending_build> job;
    {
      // Внутри шага поток берёт только сам ожидаемый экземпляр: это ровно сборка «на месте», которую
      // делает и обычный get; чужие поддеревья внутри шага не берутся (lua-хост потока занят шагом).
      std::lock_guard guard(pending_lock_);
      std::erase_if(pending_, [](const auto& p) { return p->started.load(); });
      for (const auto& p : pending_) {
        const bool itself = p->at == &at && p->seed == world_seed && p->key == key;
        if (context.step_depth == 0 ? under_awaited(*p) : itself) {
          job = p;
          break;
        }
      }
    }
    if (job) {
      run_claim(context, *job);
      continue;
    }
    const auto started = std::chrono::steady_clock::now();
    future.wait_for(std::chrono::microseconds(200));
    idle_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
    if (idle_ns > 10'000'000'000ull && std::getenv("ORIGINATOR_HANG_DEBUG")) {
      static std::mutex print_lock;
      std::lock_guard print(print_lock);
      const auto name_of = [&](const void* slot_ptr) { return static_cast<const slot*>(slot_ptr)->description.name; };
      std::fprintf(stderr, "HANG thread %zu waits %s %llx/%llx/%llx step_depth %d; stack:",
                   std::hash<std::thread::id>{}(std::this_thread::get_id()) % 1000, at.description.name.c_str(),
                   (unsigned long long)key.words[0], (unsigned long long)key.words[1], (unsigned long long)key.words[2],
                   context.step_depth);
      for (const auto& [who, seed, k] : context.building) {
        std::fprintf(stderr, " %s:%llx", name_of(who).c_str(), (unsigned long long)k.words[2]);
      }
      std::fprintf(stderr, "\n");
      std::lock_guard guard(pending_lock_);
      for (const auto& p : pending_) {
        std::fprintf(stderr, "   pending %s:%llx started %d parents:", p->at->description.name.c_str(),
                     (unsigned long long)p->key.words[2], int(p->started.load()));
        for (auto node = p->parent; node != nullptr; node = node->parent) {
          std::fprintf(stderr, " %s:%llx", name_of(node->at).c_str(), (unsigned long long)node->key.words[2]);
        }
        std::fprintf(stderr, "\n");
      }
      idle_ns = 0;
    }
  }
  std::lock_guard guard(at.lock);
  at.stats.wait_ns += idle_ns;
  ++at.stats.waits;
}

void generator_chain::prefetch(generation_context& context, const std::string_view& name, const uint64_t world_seed, const instance_key& key) const {
  const slot& at = find_slot(name);
  {
    std::lock_guard guard(at.lock);
    if (at.cache.contains(std::make_pair(world_seed, key))) return;
  }
  (void)resolve(context, at, world_seed, key);
}

std::string generator_chain::verify(generation_context& context, const std::string_view& name, const uint64_t world_seed,
                                    const instance_key& key) const {
  const auto cached = get(context, name, world_seed, key);
  const slot& at = find_slot(name);
  context.building.emplace_back(&at, world_seed, key);
  result_ptr fresh;
  try {
    fresh = build(context, at, world_seed, key);
  } catch (...) {
    context.building.pop_back();
    throw;
  }
  context.building.pop_back();
  return cached->compare(*fresh);
}

std::vector<resolved_link> generator_chain::resolve(generation_context& context, const slot& at, const uint64_t world_seed,
                                                    const instance_key& key) const {
  std::vector<resolved_link> out;
  const auto& g = at.description;

  // Поле `буфер.поле` из буферов, привезённых ОДИНОЧНЫМИ связями раньше этой.
  const auto field_of = [&](const std::string_view& reference, const std::string_view& link_name) {
    const auto [buffer_name, field_name] = split_field(reference, g.name, link_name);
    for (const auto& earlier : out) {
      if (earlier.results.size() != 1) continue;
      const auto* source = earlier.results.front()->find(buffer_name);
      if (source == nullptr) continue;
      const size_t index = source->find_field(field_name);
      if (index == buffer_layout::npos) {
        utils::error{}("originator chain generator '{}', link '{}': buffer '{}' has no field '{}'", g.name, link_name,
                       buffer_name, field_name);
      }
      return source->field(index);
    }
    utils::error{}("originator chain generator '{}', link '{}': no earlier single link brings buffer '{}'", g.name,
                   link_name, buffer_name);
    return const_field_accessor{};
  };

  const auto key_at = [&](const chain_link& link, const size_t element, const uint32_t words) {
    instance_key produced{};
    uint32_t filled = 0;
    for (const auto& reference : link.fields) {
      const auto field = field_of(reference, link.name);
      if (element >= field.count()) {
        utils::error{}("originator chain generator '{}', link '{}': key {} is beyond the {} elements of '{}'", g.name,
                       link.name, element, field.count(), reference);
      }
      const uint32_t in_field = field.type().components;
      for (uint32_t w = 0; w < in_field; ++w) {
        if (filled >= words) {
          utils::error{}("originator chain generator '{}', link '{}': the key fields hold more than the {} words of "
                         "'{}'", g.name, link.name, words, link.from);
        }
        produced.words[filled++] = read_key_word(field, element, w);
      }
    }
    if (filled != words) {
      utils::error{}("originator chain generator '{}', link '{}': the key fields hold {} words, '{}' needs {}", g.name,
                     link.name, filled, link.from, words);
    }
    return produced;
  };

  for (const auto& link : g.links) {
    const slot& from = find_slot(link.from);
    resolved_link resolved;
    resolved.name = link.name;
    switch (link.type) {
      case chain_link::kind::same:
        resolved.results.push_back(get(context, link.from, world_seed, key));
        break;
      case chain_link::kind::project: {
        instance_key produced{};
        for (size_t i = 0; i < link.project.size(); ++i) produced.words[i] = key.words[link.project[i]];
        resolved.results.push_back(get(context, link.from, world_seed, produced));
        break;
      }
      case chain_link::kind::from_data:
        resolved.results.push_back(get(context, link.from, world_seed, key_at(link, 0, from.description.key_words)));
        break;
      case chain_link::kind::fan_in: {
        const auto count_field = field_of(link.count, link.name);
        const double count = count_field.get(0);
        if (count < 0.0 || count != std::floor(count)) {
          utils::error{}("originator chain generator '{}', link '{}': count {} is not a whole number", g.name,
                         link.name, count);
        }
        // Все недостающие экземпляры захватываются сразу и выставляются в очередь помощи: потоки, которые
        // ждут ЭТУ сборку, строят их параллельно, а сам поток берёт их по порядку. Порядок результатов —
        // порядок ключей, кто бы ни строил.
        std::vector<claimed> items;
        std::vector<instance_key> keys;
        items.reserve(size_t(count));
        keys.reserve(size_t(count));
        struct abandon {
          const generator_chain* chain;
          std::vector<claimed>* items;
          bool done = false;
          ~abandon() {
            if (done) return;
            for (auto& item : *items) {
              if (!item.job || item.job->started.exchange(true)) continue;
              {
                std::lock_guard guard(item.job->at->lock);
                item.job->at->cache.erase(std::make_pair(item.job->seed, item.job->key));
              }
              item.job->promise.set_exception(std::make_exception_ptr(
                std::runtime_error("originator chain: a sibling build of this fan-in failed")));
            }
          }
        } guard{this, &items};
        for (size_t e = 0; e < size_t(count); ++e) {
          const instance_key produced = key_at(link, e, from.description.key_words);
          for (const auto& [who, seed, k] : context.building) {
            if (who == &from && seed == world_seed && k == produced) {
              utils::error{}("originator chain generator '{}': instance {:x}/{:x}/{:x} links back to itself while "
                             "being built", link.from, produced.words[0], produced.words[1], produced.words[2]);
            }
          }
          items.push_back(claim(context, from, world_seed, produced));
          keys.push_back(produced);
        }
        {
          std::lock_guard lock(pending_lock_);
          for (const auto& item : items) if (item.job) pending_.push_back(item.job);
        }
        for (size_t e = 0; e < items.size(); ++e) {
          if (items[e].job) run_claim(context, *items[e].job);
          wait_helping(context, from, world_seed, keys[e], items[e].future);
          resolved.results.push_back(items[e].future.get());
        }
        guard.done = true;
        break;
      }
    }
    out.push_back(std::move(resolved));
  }
  return out;
}

void generator_chain::trim_outputs(const chain_generator& g, generator_result& result) {
  for (const auto& [buffer_name, count_reference] : g.trim) {
    const auto dot = count_reference.find('.');
    const double count = result.field(count_reference.substr(0, dot), count_reference.substr(dot + 1)).get(0);
    for (auto& target : result.buffers_) {
      if (target->name() != buffer_name) continue;
      if (count < 0.0 || count != std::floor(count) || count > double(target->count())) {
        utils::error{}("originator chain generator '{}': '{}' says {} elements of '{}' are used, it holds {}", g.name,
                       count_reference, count, buffer_name, target->count());
      }
      target = std::make_unique<buffer>(target->truncated(size_t(count)));
    }
  }
}

std::shared_ptr<const primitive_field> generator_chain::field(generation_context& context, const std::string_view& name, const uint64_t world_seed,
                                                             const instance_key& key,
                                                             const std::string_view& field_name) const {
  const slot& at = find_slot(name);
  for (const auto& declared : at.description.fields) {
    if (declared.name != field_name) continue;
    const std::vector<resolved_link> links = resolve(context, at, world_seed, key);
    std::vector<field_term_source> terms;
    for (const auto& term : declared.terms) {
      const auto found = std::find_if(links.begin(), links.end(), [&](const resolved_link& l) { return l.name == term.link; });
      if (found == links.end()) utils::error{}("originator chain generator '{}': field '{}' names unknown link '{}'", name, field_name, term.link);
      terms.push_back({term.kind, found->results});
    }
    return std::make_shared<const primitive_field>(*setup_.vocabulary, terms, world_seed);
  }
  utils::error{}("originator chain generator '{}' declares no field '{}'", name, field_name);
  return nullptr;
}

result_ptr generator_chain::build(generation_context& context, const slot& at, const uint64_t world_seed, const instance_key& key) const {
  generator_run run;
  run.context_ = &context;
  run.chain_ = this;
  run.generator_ = &at.description;
  run.world_seed_ = world_seed;
  run.key_ = key;
  run.seed_ = utils::mix(world_seed, at.name_hash, key.words[0], key.words[1], key.words[2]);
  run.values_ = &at.values;
  const auto started = std::chrono::steady_clock::now();
  run.links_ = resolve(context, at, world_seed, key);
  const auto linked_at = std::chrono::steady_clock::now();
  struct timing {
    const slot& at;
    std::chrono::steady_clock::time_point started, linked;
    ~timing() {
      const auto now = std::chrono::steady_clock::now();
      std::lock_guard guard(at.lock);
      at.stats.build_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now - started).count());
      at.stats.self_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now - linked).count());
    }
  } timed{at, started, linked_at};
  for (const auto& declared : at.description.fields) {
    std::vector<field_term_source> terms;
    for (const auto& term : declared.terms) terms.push_back({term.kind, run.link(term.link).results});
    run.fields_.emplace_back(declared.name, std::make_shared<const primitive_field>(*setup_.vocabulary, terms, world_seed));
  }
  // Поля видны инструментам шагов и нативному исполнителю на время сборки этого экземпляра; связи
  // уже разрешены, поэтому вложенных сборок дальше нет.
  struct fields_scope {
    const std::vector<std::pair<std::string, std::shared_ptr<const primitive_field>>>* previous;
    ~fields_scope() { context.fields = previous; }
    generation_context& context;
  } scope{std::exchange(context.fields, &run.fields_), context};

  run.result_ = std::make_shared<generator_result>();
  run.result_->generator_ = at.description.name;
  run.result_->seed_ = world_seed;
  run.result_->key_ = key;

  if (at.has_pipeline) {
    std::vector<const buffer*> linked;
    std::vector<step_link> step_links;
    for (size_t i = 0; i < run.links_.size(); ++i) {
      const auto& link = run.links_[i];
      step_link visible;
      visible.name = link.name;
      for (const auto& result : link.results) {
        auto& instance = visible.instances.emplace_back();
        for (size_t b = 0; b < result->buffer_count(); ++b) instance.push_back(&result->buffer_at(b));
      }
      // Буферы одиночной связи входят в пайплайн по именам; список fan-in виден только через step.links.
      if (at.description.links[i].type != chain_link::kind::fan_in) {
        for (const auto* source : visible.instances.front()) {
          const bool twice = std::any_of(linked.begin(), linked.end(),
                                         [&](const buffer* b) { return b->name() == source->name(); });
          if (twice) {
            utils::error{}("originator chain generator '{}': two links bring a buffer named '{}'", at.description.name,
                           source->name());
          }
          linked.push_back(source);
        }
      }
      step_links.push_back(std::move(visible));
    }
    run.line_ = std::make_unique<pipeline>(at.merged, setup_.sizes, run.seed_, linked);
    run.line_->set_context(&context);
    run.line_->set_links(std::move(step_links));
    run.line_->set_instance(key.words, world_seed);
    ++context.step_depth;
    struct depth_scope { generation_context& context; ~depth_scope() { --context.step_depth; } } depth{context};
    if (at.executor) {
      at.executor(run);
    } else {
      run.run_steps();
    }
    for (const auto& output : at.description.outputs) {
      run.result_->buffers_.push_back(run.line_->take(output));
    }
    trim_outputs(at.description, *run.result_);
  } else {
    if (!at.native) {
      utils::error{}("originator chain generator '{}': native, but the host registered no body for it",
                     at.description.name);
    }
    for (const auto& declaration : *at.buffers) {
      if (!declaration.extent_names.empty()) {
        buffer_extent extent;
        size_t* const axes[3] = {&extent.x, &extent.y, &extent.z};
        for (size_t axis = 0; axis < declaration.extent_names.size(); ++axis) {
          *axes[axis] = setup_.sizes.get(declaration.extent_names[axis], declaration.name);
        }
        run.native_buffers_.push_back(std::make_unique<buffer>(declaration.name, declaration.layout, extent));
      } else {
        run.native_buffers_.push_back(std::make_unique<buffer>(
          declaration.name, declaration.layout, setup_.sizes.get(declaration.size_name, declaration.name)));
      }
    }
    ++context.step_depth;
    struct depth_scope { generation_context& context; ~depth_scope() { --context.step_depth; } } depth{context};
    at.native(run);
    for (const auto& output : at.description.outputs) {
      for (auto& candidate : run.native_buffers_) {
        if (candidate != nullptr && candidate->name() == output) {
          run.result_->buffers_.push_back(std::move(candidate));
          break;
        }
      }
    }
    trim_outputs(at.description, *run.result_);
  }
  // Поля остаются с экземпляром, только если генератор объявил keep_fields = true: поле держит строки
  // всех своих связей, и кэш из сотен экземпляров держал бы их (а с ними вытесненные экземпляры связей).
  if (at.description.keep_fields) run.result_->fields_ = run.fields_;
  return run.result_;
}

const primitive_field* generator_result::shape_field(const std::string_view& field_name) const noexcept {
  for (const auto& [name, field] : fields_) {
    if (name == field_name) return field.get();
  }
  return nullptr;
}

std::shared_ptr<const primitive_field> generator_result::shape_field_handle(const std::string_view& field_name) const {
  for (const auto& [name, field] : fields_) {
    if (name == field_name) return field;
  }
  utils::error{}("originator chain generator '{}': the instance has no field '{}'", generator_, field_name);
  return nullptr;
}

shape_rows generator_result::decoded_rows(const shape_vocabulary& vocabulary, const size_t kind) const {
  // Разбор — без замка: вид формы вправе спросить у экземпляра его представление (view), а оно
  // берёт свой замок. Повторный разбор в двух потоках даёт те же строки; остаются первые.
  {
    std::lock_guard guard(decoded_lock_);
    for (const auto& entry : decoded_) {
      if (entry.vocabulary == &vocabulary && entry.kind == kind) return entry.rows;
    }
  }
  auto rows = vocabulary.decoder(kind)(*this);
  std::lock_guard guard(decoded_lock_);
  for (const auto& entry : decoded_) {
    if (entry.vocabulary == &vocabulary && entry.kind == kind) return entry.rows;
  }
  decoded_.push_back({&vocabulary, kind, rows});
  return rows;
}

} // namespace originator
} // namespace devils_engine
