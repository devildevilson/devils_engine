#ifndef DEVILS_ENGINE_INPUT_KEY_NAMES_H
#define DEVILS_ENGINE_INPUT_KEY_NAMES_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>

namespace devils_engine {
namespace input {
using key_name_buffer = char[256];

// возможно в будущем сюда нужно будет передать окно
size_t get_key_name(const int32_t scancode, char* buffer, const size_t max_size);
size_t get_key_name(const int32_t scancode, key_name_buffer& buffer);
std::string get_key_name(const int32_t scancode);
}
}

#endif