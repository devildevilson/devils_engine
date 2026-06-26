#ifndef DEVILS_ENGINE_INPUT_KEY_NAMES_H
#define DEVILS_ENGINE_INPUT_KEY_NAMES_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <tuple>

namespace devils_engine {
namespace input {
using key_name_buffer = char[256];

// возможно в будущем сюда нужно будет передать окно
size_t get_key_name(const int32_t scancode, char* buffer, const size_t max_size);
size_t get_key_name(const int32_t scancode, key_name_buffer& buffer);
std::string get_key_name(const int32_t scancode);

std::string_view get_key_name_canonical(const int32_t scancode) noexcept;
std::string_view get_key_name_us_layout(const int32_t scancode) noexcept;
std::string get_key_name_local(const int32_t scancode);

std::string_view get_glfw_key_name_canonical(const int32_t key) noexcept;
std::string_view get_glfw_key_name_us_layout(const int32_t key) noexcept;
std::string get_glfw_key_name_local(const int32_t key);

int32_t get_glfw_key_from_canonical(const std::string_view &name) noexcept;
std::tuple<int32_t, int32_t> get_key_from_canonical(const std::string_view &name) noexcept;
}
}

#endif
