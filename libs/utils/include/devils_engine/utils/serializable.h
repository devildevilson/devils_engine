#ifndef DEVILS_ENGINE_UTILS_SERIALIZABLE_H
#define DEVILS_ENGINE_UTILS_SERIALIZABLE_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <expected>
#include "fileio.h"
#include "named_serializer.h"

namespace devils_engine {
namespace utils {

//constexpr auto default_binary_options = alpaca::options::with_checksum | alpaca::options::with_version;

// в этом чет особо нет никакого смысла
// есть смысл - легко сделать настроечки присобачив этот интерфейс к любой структуре
// другое дело что тогда везде придется подключать довольно тяжелые хедеры
template <typename T>
struct serializable {
  using inner_t = T;

  std::expected<std::string, glz::error_ctx> to_string() const;
  glz::error_ctx from_string(const std::string& str);
  std::vector<uint8_t> to_binary() const;
  std::errc from_binary(const std::vector<uint8_t> &data);
  bool dump(const std::string &path); // .json поймем что нужно сделать по расширению
  bool load(const std::string &path);
};

// по умолчанию мы хотели бы видеть json в удобном виде
// и гораздо в менее часто в неудобном
template <typename T>
std::expected<std::string, glz::error_ctx> serializable<T>::to_string() const {
  const auto& obj = static_cast<const T&>(*this);
  std::string str;
  const auto err = utils::to_json<glz::opts{.prettify=true, .indentation_width=2}>(obj, str);
  return std::expected(str, err);
}

template <typename T>
glz::error_ctx serializable<T>::from_string(const std::string& str) {
  auto& obj = static_cast<T&>(*this);
  const auto err = utils::from_json(obj, str);
  return err;
}

template <typename T>
std::vector<uint8_t> serializable<T>::to_binary() const {
  const auto& obj = static_cast<const T&>(*this);
  //return utils::to_binary<default_binary_options>(obj); // тут наверное будут в целом стандартные опции
  return utils::to_binary(obj); // опции есть, нужно разобраться что к чему
}

template <typename T>
std::errc serializable<T>::from_binary(const std::vector<uint8_t>& data) {
  auto& obj = static_cast<T&>(*this);
  //return utils::from_binary<default_binary_options>(obj, data);
  return utils::from_binary(data, obj);
}

template <typename T>
bool serializable<T>::dump(const std::string& path) {
  const size_t index = path.rfind(".");
  if (index != std::string::npos && std::string_view(path).substr(index) == ".json") {
    const auto ret = to_string();
    if (ret.has_value()) {
      file_io::write(ret.value(), path);
      return true;
    } else {
      utils::warn("{}", ret.error().custom_error_message);
    }
    
    utils::warn("Could not write structure '{}' on disk. Path: {}", utils::type_name<T>(), path);
    return false;
  }

  const auto ret = to_binary();
  file_io::write(ret, path);
  return true;
}

template <typename T>
bool serializable<T>::load(const std::string& path) {
  const size_t index = path.rfind(".");
  const bool reading_json = index != std::string::npos && std::string_view(path).substr(index) == ".json";
  if (reading_json) {
    const auto content = file_io::read(path);
    const auto err = from_string(content);
    if (!err) return true;

    utils::warn("Could not read file '{}'. Error: {}", path, err.custom_error_message);
    return false;
  }

  const auto content = file_io::read<uint8_t>(path);
  const auto err = from_binary(content);
  if (!err) return true;

  utils::warn("Could not read file '{}'. Error: {}", path, err.message());
  return false;
}
}
}

#endif