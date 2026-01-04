#include "resource.h"

#include "al_helper.h"

#include "mp3_decoder.h"
#include "ogg_decoder.h"
#include "wav_decoder.h"
#include "flac_decoder.h"
#include "pcm_decoder.h"
#include "devils_engine/utils/core.h"

#define SMALL_SOUND_COEFFICIENT 5.0f

namespace devils_engine {
namespace sound {
const std::string_view type_names[] = {
#define X(name) #name ,
  SOUND_SYSTEM_EXTENSION_LIST
#undef X

  "undefined"
};
std::string_view resource::type_to_string(const size_t index) {
  return type_names[index];
}

resource::resource() : type(type::undefined) {}
resource::resource(std::string id, enum type type, std::vector<char> buffer) :
  id(std::move(id)),
  type(type),
  buffer(std::move(buffer))
{
  if (type == type::mp3) {
    sound.reset(new  mp3_decoder(this->id, this->buffer.data(), this->buffer.size()));
  } else if (type == type::wav) {
    sound.reset(new  wav_decoder(this->id, this->buffer.data(), this->buffer.size()));
  } else if (type == type::ogg) {
    sound.reset(new  ogg_decoder(this->id, this->buffer.data(), this->buffer.size()));
  } else if (type == type::flac) {
    sound.reset(new flac_decoder(this->id, this->buffer.data(), this->buffer.size()));
  } else {
    utils::error{}("Invalid sound resource type {}", size_t(type));
  }

  const size_t frames_treshold = second_to_pcm_frames(SMALL_SOUND_COEFFICIENT, sound->sample_rate()); // , sound->channels()
  if (sound->frames_count() < frames_treshold) {
    auto dec = new pcm_decoder(sound.get());
    sound.reset(dec);
    this->type = type::pcm;
  }
}

resource::~resource() noexcept {
  //sound->~decoder();
}

double resource::duration() const {
  return pcm_frames_to_seconds(sound->frames_count(), sound->sample_rate());
}
}
}