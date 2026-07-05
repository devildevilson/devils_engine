#include "sound_resource.h"

#include <string_view>

#include <devils_engine/utils/core.h>
#include <devils_engine/utils/safe_handle.h>
#include <devils_engine/demiurg/module_interface.h>

namespace devils_engine {
namespace sound {

static data_type type_from_ext(const std::string_view ext) noexcept {
  if (ext == "mp3")  return data_type::mp3;
  if (ext == "flac") return data_type::flac;
  if (ext == "wav")  return data_type::wav;
  if (ext == "ogg")  return data_type::ogg;
  // pcm НЕ поддержан в resource2/system2 пути: make_decoder(pcm) отдаёт null → play был бы фатальным.
  // Не заявляем '.pcm' как загружаемый формат, пока PCM-ветка не готова (см. system.cpp make_decoder).
  return data_type::undefined;
}

sound_resource::sound_resource() {
  set_flag(demiurg::resource_flags::warm_and_hot_same, true); // CPU-only: warm == hot
  set_flag(demiurg::resource_flags::binary, true);
}

void sound_resource::load_cold(const utils::safe_handle_t&) {
  module->load_binary(path, data);
  type = type_from_ext(ext);
  if (type == data_type::undefined) utils::warn("sound_resource '{}': unknown extension '{}'", id, ext);
  utils::info("sound_resource '{}': loaded {} bytes (type {})", id, data.size(), size_t(type));
}

void sound_resource::load_warm(const utils::safe_handle_t&) {}
void sound_resource::unload_hot(const utils::safe_handle_t&) {}
void sound_resource::unload_warm(const utils::safe_handle_t&) { data.clear(); data.shrink_to_fit(); }

}
}
