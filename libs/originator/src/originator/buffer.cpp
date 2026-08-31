#include "devils_engine/originator/buffer.h"

#include <algorithm>
#include <cstring>

#include "devils_engine/utils/core.h"

namespace devils_engine {
namespace originator {

namespace {
// Планы soa выравниваются на строку кэша: соседние поля не должны делить одну строку, иначе
// параллельная запись в разные поля даёт ложное разделение.
constexpr size_t plane_alignment = 64;

constexpr std::string_view storage_names[storage_kind::count] = {"aos", "soa"};

size_t align_up(const size_t value, const size_t alignment) noexcept {
  if (alignment <= 1) {
    return value;
  }
  return (value + alignment - 1) / alignment * alignment;
}
} // namespace

std::string_view to_string(const storage_kind::values value) noexcept {
  return value < storage_kind::count ? storage_names[value] : std::string_view("invalid");
}

storage_kind::values parse_storage_kind(const std::string_view& str) noexcept {
  for (size_t i = 0; i < storage_kind::count; ++i) {
    if (storage_names[i] == str) {
      return storage_kind::values(i);
    }
  }
  return storage_kind::count;
}

bool buffer_layout::valid() const noexcept {
  if (fields.empty() || storage >= storage_kind::count) {
    return false;
  }
  return std::all_of(fields.begin(), fields.end(), [](const field_declaration& f) {
    return !f.name.empty() && f.type.valid();
  });
}

size_t buffer_layout::find_field(const std::string_view& name) const noexcept {
  for (size_t i = 0; i < fields.size(); ++i) {
    if (fields[i].name == name) {
      return i;
    }
  }
  return npos;
}

size_t buffer_layout::element_byte_size() const noexcept {
  size_t offset = 0;
  size_t max_alignment = 1;
  for (const auto& field : fields) {
    const size_t alignment = field.type.alignment();
    max_alignment = std::max(max_alignment, alignment);
    offset = align_up(offset, alignment) + field.type.byte_size();
  }
  return align_up(offset, max_alignment);
}

size_t buffer_layout::byte_size(const size_t count) const noexcept {
  if (storage == storage_kind::aos) {
    return element_byte_size() * count;
  }

  size_t total = 0;
  for (const auto& field : fields) {
    total = align_up(total, plane_alignment) + field.type.byte_size() * count;
  }
  return total;
}

buffer_layout make_buffer_layout(const storage_kind::values storage,
                                 const std::span<const std::pair<std::string_view, std::string_view>>& fields,
                                 const std::string_view& buffer_name) {
  if (storage >= storage_kind::count) {
    utils::error{}("originator buffer '{}': unknown storage kind", buffer_name);
  }
  if (fields.empty()) {
    utils::error{}("originator buffer '{}': format must declare at least one field", buffer_name);
  }

  buffer_layout layout;
  layout.storage = storage;
  layout.fields.reserve(fields.size());

  for (const auto& [name, spelling] : fields) {
    if (name.empty()) {
      utils::error{}("originator buffer '{}': field {} has an empty name", buffer_name, layout.fields.size());
    }
    if (layout.find_field(name) != buffer_layout::npos) {
      utils::error{}("originator buffer '{}': duplicate field name '{}'", buffer_name, name);
    }

    const auto type = parse_field_type(spelling);
    if (!type.valid()) {
      utils::error{}("originator buffer '{}': could not parse type '{}' of field '{}'", buffer_name, spelling, name);
    }

    layout.fields.push_back(field_declaration{std::string(name), type});
  }

  return layout;
}

buffer::buffer(std::string name, buffer_layout layout, const size_t count) :
  name_(std::move(name)), layout_(std::move(layout)), count_(count) {
  if (!layout_.valid()) {
    utils::error{}("originator buffer '{}': invalid layout", name_);
  }
  if (count_ == 0) {
    utils::error{}("originator buffer '{}': element count must be greater than zero", name_);
  }

  placement_.resize(layout_.fields.size());

  if (layout_.storage == storage_kind::aos) {
    const size_t stride = layout_.element_byte_size();
    size_t offset = 0;
    for (size_t i = 0; i < layout_.fields.size(); ++i) {
      const auto& field = layout_.fields[i];
      offset = align_up(offset, field.type.alignment());
      placement_[i] = field_placement{offset, stride};
      offset += field.type.byte_size();
    }
  } else {
    size_t offset = 0;
    for (size_t i = 0; i < layout_.fields.size(); ++i) {
      const auto& field = layout_.fields[i];
      offset = align_up(offset, plane_alignment);
      placement_[i] = field_placement{offset, field.type.byte_size()};
      offset += field.type.byte_size() * count_;
    }
  }

  const size_t total = layout_.byte_size(count_);
  memory_.assign(total + plane_alignment, std::byte{0});
  auto* raw = memory_.data();
  const auto address = reinterpret_cast<uintptr_t>(raw);
  const auto aligned = align_up(size_t(address), plane_alignment);
  data_ = raw + (aligned - size_t(address));
}

const std::string& buffer::name() const noexcept {
  return name_;
}

const buffer_layout& buffer::layout() const noexcept {
  return layout_;
}

size_t buffer::count() const noexcept {
  return count_;
}

size_t buffer::byte_size() const noexcept {
  return layout_.byte_size(count_);
}

size_t buffer::find_field(const std::string_view& name) const noexcept {
  return layout_.find_field(name);
}

const field_placement& buffer::placement(const size_t field_index) const noexcept {
  static const field_placement invalid{};
  return field_index < placement_.size() ? placement_[field_index] : invalid;
}

field_accessor buffer::field(const size_t field_index) noexcept {
  if (field_index >= layout_.fields.size()) {
    return field_accessor{};
  }
  return field_accessor(data_, placement_[field_index], layout_.fields[field_index].type, count_);
}

const_field_accessor buffer::field(const size_t field_index) const noexcept {
  if (field_index >= layout_.fields.size()) {
    return const_field_accessor{};
  }
  return const_field_accessor(data_, placement_[field_index], layout_.fields[field_index].type, count_);
}

void buffer::clear() noexcept {
  if (data_ != nullptr) {
    std::memset(data_, 0, byte_size());
  }
}

const std::byte* buffer::base_pointer() const noexcept {
  return data_;
}

} // namespace originator
} // namespace devils_engine
