#include "basic_sources.h"

#include "AL/al.h"
#include "AL/alc.h"
#include "AL/alext.h"
#include "al_helper.h"

#include <glm/glm.hpp>

namespace devils_engine {
namespace sound {

void background_source::set_resource(const resource* res) {
  basic_sound_data::res = res;
  set_stat(0.0);
}

bool background_source::is_valid() const {
  return basic_sound_data::res != nullptr;
}

const resource* background_source::currently_playing() const {
  return basic_sound_data::res;
}

// зачем я тут все это обновляю, когда есть отдельный АПДЕЙТ
void background_source::set(float , float volume, float , bool ) {
  basic_sound_data::volume = volume;

  //if (!sound_processing_data::source.valid()) return;
  //const float final_vol = *basic_sound_data::type_volume * volume;
  //al_call(alSourcef, sound_processing_data::source.handle, AL_GAIN, final_vol);
}

void background_source::set_transform(const glm::vec3 &, const glm::vec3 &, const glm::vec3 &) {}

void background_source::set_relative(const bool relative) {
  //if (sound_processing_data::source.valid()) {
  //  al_call(alSourcei, sound_processing_data::source.handle, AL_SOURCE_RELATIVE, int(relative));
  //}
}

bool background_source::play() {
  if (!is_valid()) return false;
  if (is_playing()) return false;
  sound_processing_data::time = 0;
  return true;
}

bool background_source::pause() {
  if (!is_valid()) return false;
  sound_processing_data::time = SIZE_MAX;

  // нужно запомнить на каком именно месте мы остановились
  if (sound_processing_data::source.valid()) {
    ALint samples_offset = 0;
    al_call(alGetSourcei, sound_processing_data::source.handle, AL_SAMPLE_OFFSET, &samples_offset);

    const size_t samples_count = basic_sound_data::res->sound->frames_count();
    const size_t processed_samples = (sound_processing_data::processed_frames + samples_offset) % samples_count;
    sound_processing_data::processed_frames = processed_samples;
    sound_processing_data::loaded_frames = sound_processing_data::processed_frames;

    al_call(alSourcePause, sound_processing_data::source.handle);
  }

  return true;
}

bool background_source::is_playing() const {
  return is_valid() && sound_processing_data::time != SIZE_MAX;
}

bool background_source::is_paused() const {
  return sound_processing_data::time == SIZE_MAX;
}

double background_source::stat() const {
  //if (sound_processing_data::fast_source == 0) return 0.0;
  if (!is_valid()) return 0.0;

  // может быть вариант что мы приостановим звук
  // и все равно захотим стат сделать
  // после приостановки уберем fast_source

  ALint samples_offset = 0;
  if (sound_processing_data::source.valid()) {
    al_call(alGetSourcei, sound_processing_data::source.handle, AL_SAMPLE_OFFSET, &samples_offset);
  }

  const size_t samples_count = basic_sound_data::res->sound->frames_count();
  const size_t processed_samples = (sound_processing_data::processed_frames + samples_offset) % samples_count;
  return double(processed_samples) / double(samples_count);
}

bool background_source::set_stat(const double place) {
  if (!is_valid()) return false;

  // тут надо дать понять что меняем место, как?
  // да просто поди выставим время в 0 и при обработке будет понятно че делать

  sound_processing_data::loaded_frames = place * currently_playing()->sound->frames_count();
  sound_processing_data::time = 0;
  return true;
}

static bool is_finished(const source &s) {
  ALint state = AL_PLAYING;
  al_call(alGetSourcei, s.handle, AL_SOURCE_STATE, &state);
  return state == AL_STOPPED;
}

processing_state background_source::state() const {
  if (!is_valid()) return processing_state::waiting_resource;
  if (is_paused()) return processing_state::paused;
  if (!sound_processing_data::source.valid()) return processing_state::waiting_source;

  if (is_finished(sound_processing_data::source)) return processing_state::finished;

  return processing_state::processing;
}

float background_source::distance(const glm::vec3 &listener_pos) const {
  return 0.0f;
}

static void completely_stop_source(source &s) {
  al_call(alSourceStop, s.handle);
  ALint count = 0;
  al_call(alGetSourcei, s.handle, AL_BUFFERS_PROCESSED, &count);
  uint32_t buffers[2] = {0,0};
  al_call(alSourceUnqueueBuffers, s.handle, count, buffers);
}

// update придется переделывать для всех типов? =(
// здесь тупо нужно понять сколько каналов + функцию обновления данных передать
void background_source::update(const size_t _time) {
  _update(_time, 0);
}

void background_source::setup_source(const struct source &source) {
  // ставим сорс, но при этом скорее всего не трогаем другие настройки
  sound_processing_data::source = source;
}

// должны ли мы остановить прежде этого?
// имеет смысл сделать проверки чтобы мы были либо в паузе либо в финише
// это по идее логическая ошибка которую надо поправить в коде
struct source background_source::release_source() {
  if (!(state() == processing_state::finished || state() == processing_state::paused)) utils::error{}("Trying to remove source from playing sound resource!");

  const auto s = sound_processing_data::source;
  sound_processing_data::source = sound::source();
  return s;
}

bool background_source::has_source() const {
  return sound_processing_data::source.valid();
}

void background_source::invalidate() {
  set_resource(nullptr);
}

size_t background_source::compute_frames_to_load(const size_t expected) {
  size_t frames_count = expected;
  // начинать мы можем не с нуля, а например с паузы
  const size_t sound_frames_count = basic_sound_data::res->sound->frames_count();
  if (sound_processing_data::loaded_frames + expected > sound_frames_count) {
    const size_t amount1 = (sound_processing_data::loaded_frames + expected) - sound_frames_count;
    frames_count = std::ceil(double(amount1) / 2.0);
  }

  return frames_count;
}

size_t background_source::load_next(const uint32_t buffer_handle, const size_t offset, const size_t amount, const uint16_t channels) {
  if (!basic_sound_data::res->sound->seek(offset)) {
    utils::error{}("seek to pcm frame {} failed in resource '{}'", offset, basic_sound_data::res->id);
  }

  const uint16_t final_channels = std::min(channels, basic_sound_data::res->sound->channels());
  const size_t frames = res->sound->get_frames(buffer_handle, amount, final_channels);

  return frames;
}

void background_source::setup_source_stats() {
  const float final_vol = *basic_sound_data::type_volume * volume;
  al_call(alSourcef, sound_processing_data::source.handle, AL_GAIN, final_vol);
  al_call(alSourcei, sound_processing_data::source.handle, AL_SOURCE_RELATIVE, AL_TRUE);
  al_call(alSource3f, sound_processing_data::source.handle, AL_POSITION, 0.0f, 0.0f, 0.0f);
  al_call(alSource3f, sound_processing_data::source.handle, AL_DIRECTION, 0.0f, 0.0f, 0.0f);
  al_call(alSource3f, sound_processing_data::source.handle, AL_VELOCITY, 0.0f, 0.0f, 0.0f);
}

void background_source::_update(const size_t _time, const uint16_t channels) {
  if (sound_processing_data::time == SIZE_MAX) return;
  if (!sound_processing_data::source.valid()) return;

  // только только поставили звук или поменяли положение в проигрывателе
  if (sound_processing_data::time == 0) {
    // сорс к нам должен приходить чистым, но на всякий случай сделаем стоп
    completely_stop_source(sound_processing_data::source);

    const size_t frames_to_load = second_to_pcm_frames(DEVILS_ENGINE_SOUND_LOADING_COEFFICIENT, basic_sound_data::res->sound->sample_rate());
    const size_t frames_count = compute_frames_to_load(frames_to_load);

    sound_processing_data::loaded_frames += load_next(sound_processing_data::source.buffers[0], sound_processing_data::loaded_frames, frames_count, channels);
    sound_processing_data::loaded_frames += load_next(sound_processing_data::source.buffers[1], sound_processing_data::loaded_frames, frames_count, channels);
    al_call(alSourceQueueBuffers, sound_processing_data::source.handle, 2, source.buffers);

    setup_source_stats();
    //std::invoke(f, this);

    al_call(alSourcePlay, sound_processing_data::source.handle);
  }

  sound_processing_data::time += _time;

  if (is_finished(sound_processing_data::source)) return;

  // обновить громкость звуков? да вообще бы все обновить
  setup_source_stats();
  //std::invoke(f, this);

  ALint processed_buffers_count = 0;
  al_call(alGetSourcei, source.handle, AL_BUFFERS_PROCESSED, &processed_buffers_count);

  if (processed_buffers_count == 0) return;
  if (sound_processing_data::loaded_frames >= basic_sound_data::res->sound->frames_count()) return; //  && !queue->info.is_loop

  sound_processing_data::loaded_frames = sound_processing_data::loaded_frames >= basic_sound_data::res->sound->frames_count() ? 0 : sound_processing_data::loaded_frames;

  uint32_t buffer = 0;
  al_call(alSourceUnqueueBuffers, source.handle, 1, &buffer);
  // почему я так сделал? типа смотрим у предыдущего буфера размер
  // и пытаемся загрузить по этому размеру
  //const size_t frames = compute_buffer_frames(buffer, queue->res, queue->info.is_mono);
  // почему не вот так?
  const size_t frames_to_load = second_to_pcm_frames(DEVILS_ENGINE_SOUND_LOADING_COEFFICIENT, basic_sound_data::res->sound->sample_rate());
  const size_t frames = compute_frames_to_load(frames_to_load);

  const size_t samples_count = basic_sound_data::res->sound->frames_count();
  sound_processing_data::processed_frames = (sound_processing_data::processed_frames + frames) % samples_count;

  //const uint16_t channels_count = queue->info.is_mono ? 1 : 0;
  sound_processing_data::loaded_frames += load_next(buffer, sound_processing_data::loaded_frames, frames, 0);

  al_call(alSourceQueueBuffers, source.handle, 1, &buffer);
}

void menu_source::set(float speed, float volume, float rnd_pitch, bool is_loop) {
  basic_sound_data::volume = volume;
  advanced_sound_data::speed = speed;
  advanced_sound_data::rnd_pitch = rnd_pitch;
  advanced_sound_data::is_loop = is_loop;

  //if (!sound_processing_data::source.valid()) return;
  //const float final_vol = *basic_sound_data::type_volume * volume;
  //al_call(alSourcef, sound_processing_data::source.handle, AL_GAIN, final_vol);

  // разобраться бы вот в этой штуке
  // AL_CONE_INNER_ANGLE
}

void menu_source::set_relative(const bool relative) {
  advanced_sound_data::is_rel = relative;
  /*if (sound_processing_data::source.valid()) {
    al_call(alSourcei, sound_processing_data::source.handle, AL_SOURCE_RELATIVE, int(relative));
  }*/
}

void menu_source::update(const size_t _time) {
  const uint16_t chans = advanced_sound_data::is_mono ? 1 : 0;
  _update(_time, chans);
}

void menu_source::setup_source_stats() {
  const float final_vol = *basic_sound_data::type_volume * volume;
  al_call(alSourcef, sound_processing_data::source.handle, AL_GAIN, final_vol);
  al_call(alSourcei, sound_processing_data::source.handle, AL_SOURCE_RELATIVE, advanced_sound_data::is_rel);
  al_call(alSourcef, source.handle, AL_PITCH, advanced_sound_data::speed);
  // случайный питч как? нужно где то тут держать стейт рандомизации
  al_call(alSource3f, sound_processing_data::source.handle, AL_POSITION, 0.0f, 0.0f, 0.0f);
  al_call(alSource3f, sound_processing_data::source.handle, AL_DIRECTION, 0.0f, 0.0f, 0.0f);
  al_call(alSource3f, sound_processing_data::source.handle, AL_VELOCITY, 0.0f, 0.0f, 0.0f);
}

float special_source::distance(const glm::vec3 &listener_pos) const {
  return glm::distance(full_sound_data::pos, listener_pos);
}

void special_source::set_transform(const glm::vec3 &pos, const glm::vec3 &dir, const glm::vec3 &vel) {
  full_sound_data::pos = pos;
  full_sound_data::dir = dir;
  full_sound_data::vel = vel;

  //if (!sound_processing_data::source.valid()) return;
  //al_call(alSourcefv, sound_processing_data::source.handle, AL_POSITION, (float*)&pos);
  //al_call(alSourcefv, sound_processing_data::source.handle, AL_DIRECTION, (float*)&dir);
  //al_call(alSourcefv, sound_processing_data::source.handle, AL_VELOCITY, (float*)&vel);
}

void special_source::setup_source_stats() {
  const float final_vol = *basic_sound_data::type_volume * volume;
  al_call(alSourcef, sound_processing_data::source.handle, AL_GAIN, final_vol);
  al_call(alSourcei, sound_processing_data::source.handle, AL_SOURCE_RELATIVE, advanced_sound_data::is_rel);
  al_call(alSourcef, source.handle, AL_PITCH, advanced_sound_data::speed);
  // случайный питч как? нужно где то тут держать стейт рандомизации
  al_call(alSourcefv, sound_processing_data::source.handle, AL_POSITION, (float*)&full_sound_data::pos.x);
  //al_call(alSourcefv, sound_processing_data::source.handle, AL_DIRECTION, (float*)&full_sound_data::dir.x);
  al_call(alSourcefv, sound_processing_data::source.handle, AL_VELOCITY, (float*)&full_sound_data::vel.x);
}
}
}