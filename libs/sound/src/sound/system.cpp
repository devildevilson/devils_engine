#include "system.h"

#include <algorithm>
#include <iostream>
#include <cassert>
#include <cstring>
//#include <glm/glm.hpp>

#include "AL/al.h"
#include "AL/alc.h"
#include "AL/alext.h"
#include "al_helper.h"

#include "devils_engine/utils/core.h"

#include "mp3_decoder.h"
#include "wav_decoder.h"
#include "flac_decoder.h"
#include "ogg_decoder.h"
#include "pcm_decoder.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

//#define SOUND_LOADING_COEFFICIENT 1.0f

namespace devils_engine {
  namespace sound {
    /*static size_t compute_buffer_frames(const uint32_t buffer, const system::resource *res, const bool is_mono) {
      ALint buffer_bytes = 0;
      al_call(alGetBufferi, buffer, AL_SIZE, &buffer_bytes);
      const uint32_t channels = is_mono ? 1 : res->sound->channels();
      const uint32_t bits = adjust_bits_per_channel(res->sound->bits_per_channel());
      const size_t frames = bytes_to_pcm_frames(buffer_bytes, channels, bits);
      return frames;
    }*/

struct system2::miniaudio_data_converter : public ma_data_converter {};

static void completely_stop_source(system::source &s) {
  al_call(alSourceStop, s.handle);
  ALint count = 0;
  al_call(alGetSourcei, s.handle, AL_BUFFERS_PROCESSED, &count);
  uint32_t buffers[2] = {0,0};
  al_call(alSourceUnqueueBuffers, s.handle, count, buffers);
}

    static_assert(static_cast<uint32_t>(format::u8)  == ma_format_u8);
    static_assert(static_cast<uint32_t>(format::s16) == ma_format_s16);
    static_assert(static_cast<uint32_t>(format::s24) == ma_format_s24);
    static_assert(static_cast<uint32_t>(format::s32) == ma_format_s32);
    static_assert(static_cast<uint32_t>(format::f32) == ma_format_f32);

    vec3::vec3() noexcept : vec3(0.0f, 0.0f, 0.0f) {}
    vec3::vec3(const float x, const float y, const float z) noexcept : x(x), y(y), z(z) {}

    float distance2(const vec3 &a, const vec3 &b) noexcept {
      const auto diff = a - b;
      return dot2(diff, diff);
      //return (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z);
    }

    vec3 operator-(const vec3 &a, const vec3 &b) noexcept {
      return vec3(a.x - b.x, a.y - b.y, a.z - b.z);
    }

    float dot2(const vec3 &a, const vec3 &b) noexcept {
      return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    vec3 normalize(const vec3 &a) noexcept {
      const float k = std::sqrt(dot2(a, a));
      return vec3(a.x / k, a.y / k, a.z / k);
    }

    double compute_base_priority(const enum type type) noexcept {
      return double(static_cast<size_t>(type)) / double(static_cast<size_t>(type::count));
    }

    std::unique_ptr<decoder> make_decoder(const data_type type, const std::string_view &name, const std::span<const char> &data) {
      switch (type) {
        case data_type::mp3:  return std::make_unique<mp3_decoder>(name, data.data(), data.size());
        case data_type::wav:  return std::make_unique<wav_decoder>(name, data.data(), data.size());
        case data_type::flac: return std::make_unique<flac_decoder>(name, data.data(), data.size());
        case data_type::ogg:  return std::make_unique<ogg_decoder>(name, data.data(), data.size());
        //case data_type::pcm:  return std::make_unique<pcm_decoder>(name, data.data(), data.size());
        case data_type::pcm:  return std::unique_ptr<decoder>();
        case data_type::undefined: return std::unique_ptr<decoder>();
      }

      return std::unique_ptr<decoder>();
    }

    task::task() noexcept : task(SIZE_MAX, {}) {}
    task::task(const size_t id, const resource2 &res) noexcept :
      id(id),
      res(res),
      command(command::play),
      type(type::sfx),
      pitch(1.0f),
      volume(1.0f),
      start(0.0),
      after(SIZE_MAX)
    {}

    system::system(const size_t queue_size) : 
      device(nullptr), ctx(nullptr), counter(1), queue_size(queue_size), sources_offset(1)//, background(nullptr)
    {
      ALCenum error = AL_NO_ERROR;

      // нужно добавить поддержку переключения источника звука
      char *devices = (char *)alcGetString(NULL, ALC_DEVICE_SPECIFIER);
      while (devices && *devices != '\0') {
        std::string_view str(devices);
        if (str == "OpenAL Soft") device = alc_call(alcOpenDevice, nullptr, devices);
        devices += strlen(devices) + 1;  // next device
      }

      if (device == nullptr) device = alc_call(alcOpenDevice, nullptr, nullptr);
      assert(device != nullptr);

      const auto actual_device_name = alc_call(alcGetString, device, ALC_DEVICE_SPECIFIER);
      utils::info("sound::system: Using sound output device {}", actual_device_name);

      ctx = alc_call(alcCreateContext, device, nullptr);

      if (!alc_call(alcMakeContextCurrent, device, ctx)) {
        utils::error{}("sound::system: OpenAL: Could not make context current");
      }

      al_call_info(alDistanceModel, AL_LINEAR_DISTANCE_CLAMPED);

      // создадим сорсы + 1 для музыки

      while (error == AL_NO_ERROR) {
        system::source s;

        alGenBuffers(2, s.buffers);
        error = alGetError();

        alGenSources(1, &s.handle);
        error = error != AL_NO_ERROR ? error : alGetError();

        //sources.push_back(system::source_data(s, nullptr));
        sources.push_back(s);
      }

      if (sources.size() == 0 || (sources.size() == 1 && sources[0].handle == 0)) {
        check_al_error(error);
      }

      alDeleteBuffers(2, sources.back().buffers);
      sources.pop_back();

      //proc_array.reset(new sound_processing_data[sources.size() * queue_size]);

      /*for (size_t i = 0; i < sources.size(); ++i) {
        sources[i].queue = &proc_array[i*queue_size];
      }*/

      utils::info("sound::system: Created {} sound sources", sources.size());
    }

    system::~system() {
      for (auto &data : sources) {
        completely_stop_source(data);
        alDeleteBuffers(2, data.buffers);
        alDeleteSources(1, &data.handle);
      }

      alc_call_info(alcMakeContextCurrent, device, nullptr);
      alc_call(alcDestroyContext, device, ctx);
      alc_call(alcCloseDevice, device);

      /*for (const auto& [ name, res ] : resources) {
        resource_pool.destroy(res);
      }*/
    }

    //size_t system::setup_sound(const system::resource *res, const settings &info) {
    //  if (info.type >= volume_set::sound_types_count) utils::error{}("Could not set volume to sound type {} max is {}", info.type, volume_set::sound_types_count);

    //  if (info.force_source < sources.size()) {
    //    for (size_t i = 0; i < queue_size; ++i) {
    //      auto &data = sources[info.force_source];
    //      if (data.queue[i].id != 0) continue;

    //      const size_t sound_id = get_new_id();
    //      data.queue[i].init(sound_id, res, info);

    //      return sound_id;
    //    }

    //    return 0;
    //  }

    //  for (size_t i = 0; i < queue_size; ++i) {
    //    for (size_t j = sources_offset; j < sources.size(); ++j) {
    //      auto &data = sources[j];
    //      if (data.queue[i].id != 0) continue;

    //      const size_t sound_id = get_new_id();
    //      data.queue[i].init(sound_id, res, info);

    //      return sound_id;
    //    }
    //  }

    //  return 0;
    //}

    //bool system::remove_sound(const size_t source_id) {
    //  const auto [source_index, queue_index] = find_source_id(source_id);
    //  if (source_index == SIZE_MAX) return false;
    //  if (queue_index == 0) al_call(alSourceStop, sources[source_index].source.handle);

    //  // оставим всю основную работу апдейту?
    //  remove_from_queue(sources[source_index].queue, queue_index);

    //  return true;
    //}

    //bool system::play_sound(const size_t source_id) {
    //  const auto [source_index, queue_index] = find_source_id(source_id);
    //  if (source_index == SIZE_MAX) return false;

    //  ALint ret; 
    //  al_call(alGetSourcei, sources[source_index].source.handle, AL_SOURCE_STATE, &ret);
    //  if (ret == AL_PLAYING) return true;
    //  al_call(alSourcePlay, sources[source_index].source.handle);
    //  return true;
    //}

    //bool system::stop_sound(const size_t source_id) {
    //  const auto [source_index, queue_index] = find_source_id(source_id);
    //  if (source_index == SIZE_MAX) return false;
    //  if (queue_index != 0) return false; // паузим если только это первый звук в очереди

    //  al_call(alSourcePause, sources[source_index].source.handle);
    //  return true;
    //}

    //double system::stat_sound(const size_t source_id) const {
    //  const auto [source_index, queue_index] = find_source_id(source_id);
    //  if (source_index == SIZE_MAX) return -1.0;
    //  if (queue_index != 0) return 0.0;

    //  const auto cur = sources[source_index].queue;

    //  ALint samples_offset;
    //  al_call(alGetSourcei, sources[source_index].source.handle, AL_SAMPLE_OFFSET, &samples_offset);

    //  const size_t samples_count = cur->res->sound->frames_count();
    //  const size_t processed_samples = cur->processed_frames % samples_count + samples_offset;
    //  return double(processed_samples) / double(samples_count);
    //}

    //bool system::set_sound(const size_t source_id, const double place) {
    //  const auto [source_index, queue_index] = find_source_id(source_id);
    //  if (source_index == SIZE_MAX) return -1.0;

    //  const auto cur = &sources[source_index].queue[queue_index];
    //  if (queue_index == 0) {
    //    al_call(alSourceStop, sources[source_index].source.handle);
    //    uint32_t buffers[2] = {0,0};
    //    al_call(alSourceUnqueueBuffers, sources[source_index].source.handle, 2, buffers);
    //  }

    //  cur->loaded_frames = place * cur->res->sound->frames_count();
    //  cur->time = 0;

    //  return false;
    //}

    //bool system::set_sound(const size_t source_id, const glm::vec3 &pos, const glm::vec3 &dir, const glm::vec3 &vel) {
    //  const auto [source_index, queue_index] = find_source_id(source_id);
    //  if (source_index == SIZE_MAX) return false;

    //  auto &info = sources[source_index].queue[queue_index].info;
    //  info.pos = pos;
    //  info.dir = dir;
    //  info.vel = vel;

    //  if (queue_index == 0) {
    //    al_call(alSource3f, sources[source_index].source.handle, AL_POSITION, pos.x, pos.y, pos.z);
    //    al_call(alSource3f, sources[source_index].source.handle, AL_DIRECTION, dir.x, dir.y, dir.z);
    //    al_call(alSource3f, sources[source_index].source.handle, AL_VELOCITY, vel.x, vel.y, vel.z);
    //  }

    //  return true;
    //}

    bool system::set_listener_pos(const vec3 &pos) {
      al_call(alListener3f, AL_POSITION, pos.x, pos.y, pos.z);
      return true;
    }

    bool system::set_listener_ori(const vec3 &look_at, const vec3 &up) {
      const ALfloat listener_ori[6] = { look_at.x, look_at.y, look_at.z, up.x, up.y, up.z };
      al_call(alListenerfv, AL_ORIENTATION, listener_ori);
      return true;
    }

    bool system::set_listener_vel(const vec3 &vel) {
      al_call(alListener3f, AL_VELOCITY, vel.x, vel.y, vel.z);
      return true;
    }

    void system::set_master_volume(const float val) {
      volume.master = std::clamp(val, 0.0f, 1.0f);
    }

    void system::set_source_volume(const uint32_t type, const float val) {
      if (type >= volume_set::sound_types_count) utils::error{}("Could not set volume to sound type {} max is {}", type, volume_set::sound_types_count);
      volume.source[type] = std::clamp(val, 0.0f, 1.0f);
    }

    // пройдемся по всем источникам, если что то закончило играть ставим следующий звук в очереди
    void system::update(const size_t) {
      al_call(alListenerf, AL_GAIN, volume.master);

      vec3 lpos;
      al_call(alGetListenerfv, AL_POSITION, (float*)&lpos.x);

      // в чем заключается update? нужно отсортировать по дальности все вирутальные источники
      //std::sort(processors.begin(), processors.end(), [&lpos](auto a, auto b){
      //  if (a->state() == processing_state::waiting_resource) return false;
      //  if (b->state() == processing_state::waiting_resource) return true;

      //  // может быть всегда задавать относительные координаты?
      //  const float d1 = a->distance(lpos);
      //  const float d2 = b->distance(lpos);
      //  return d1 < d2;
      //});

      //for (auto p : static_processors) {
      //  if (p->state() == processing_state::waiting_resource) continue;
      //  p->update(time);

      //  if (p->state() == processing_state::finished) {
      //    p->invalidate();
      //  }
      //}

      //for (auto p : processors) {
      //  // continue?
      //  if (p->state() == processing_state::paused || p->state() == processing_state::waiting_resource || p->distance(lpos) >= 100.0f) {
      //    if (p->has_source()) {
      //      auto s = p->release_source();
      //      completely_stop_source(s);
      //      sources.push_back(s);
      //    }

      //    //p->invalidate();

      //    continue;
      //  }

      //  if (p->state() == processing_state::waiting_source) {
      //    if (sources.empty()) continue;
      //    auto s = sources.back();
      //    sources.pop_back();
      //    p->setup_source(s);
      //  }

      //  p->update(time);

      //  if (p->state() == processing_state::finished) {
      //    auto s = p->release_source();
      //    completely_stop_source(s);
      //    sources.push_back(s);
      //    p->invalidate();
      //  }
      //}

      // надо раскидать сорсы 

      //for (auto m : menu_sources) {
      //  // нет стоп
      //  // нам нужно составить несколько source_data2, но по типам
      //  // и вот их пройти
      //  // так чет вместо того чтобы быть проще стало сложнее
      //  // нам тип по большому счету нужен при инициализации
      //}

      //for (auto &data : sources) {
      //  if (data.queue->id == 0) continue;

      //  const float source_volume = volume.source[data.queue[0].info.type];
      //  const auto &snd_res = data.queue[0].res->sound;
      //  //const uint16_t channels_count = data.queue->info.is_mono ? 1 : snd_res->channels();
      //  const size_t frames_to_load = second_to_pcm_frames(SOUND_LOADING_COEFFICIENT, snd_res->sample_rate());
      //  //utils::println("pcm frames", frames_to_load, SOUND_LOADING_COEFFICIENT, snd_res->sample_rate(), channels_count);
      //  data.init(source_volume, frames_to_load); // по идее во всех новых звуках time должен быть 0
      //  data.update(source_volume, frames_to_load);
      //  data.queue->time += time;

      //  // громкость зависит от типа звука, тип звука указываем в настройках

      //  ALint state = AL_PLAYING;
      //  al_call(alGetSourcei, data.source.handle, AL_SOURCE_STATE, &state);

      //  if (state == AL_PLAYING || state == AL_PAUSED) continue;

      //  utils::info("Stop playing {}", data.queue->res->id);
      //  uint32_t buffers[2] = {0,0};
      //  al_call(alSourceUnqueueBuffers, data.source.handle, 2, buffers);

      //  remove_from_queue(data.queue, 0);
      //}
    }

    /*void system::load_resource(std::string id, const enum resource::type type, std::vector<char> buffer) {
      const auto itr = resources.find(id);
      if (itr != resources.end()) utils::error{}("Resource {} is already created", std::string_view(id));

      auto res = resource_pool.create(std::move(id), type, std::move(buffer));
      resources.emplace(res->id, res);
    }*/

    /*background_source *system::create_background_source() {
      auto s = create<background_source>();
      s->type_volume = &volume.source[0];
      
      auto sorc = sources.back();
      sources.pop_back();
      s->setup_source(sorc);
      static_processors.push_back(s);

      return s;
    }

    menu_source *system::create_menu_source() {
      auto s = create<menu_source>();
      s->type_volume = &volume.source[1];

      auto sorc = sources.back();
      sources.pop_back();
      s->setup_source(sorc);
      static_processors.push_back(s);

      return s;
    }

    special_source *system::create_special_source() {
      auto s = create<special_source>();
      s->type_volume = &volume.source[2];
      processors.push_back(s);
      return s;
    }

    game_source *system::create_game_source() {
      auto s = create<game_source>();
      s->type_volume = &volume.source[3];
      processors.push_back(s);
      return s;
    }*/

    size_t system::available_sources_count() const {
      return sources.size();
    }

    //size_t system::get_new_id() { 
    //  const size_t id = counter;
    //  counter += 1;
    //  counter += size_t(counter == 0);
    //  return id;
    //}

    //std::tuple<size_t, size_t> system::find_source_id(const size_t source_id) const {
    //  if (source_id == 0) return std::make_tuple(SIZE_MAX, SIZE_MAX);

    //  // вообще необязательно использовать sources
    //  for (size_t j = 0; j < sources.size(); ++j) {
    //    for (size_t i = 0; i < queue_size; ++i) {
    //      if (proc_array[j * queue_size + i].id != source_id) continue;

    //      return std::make_tuple(j, i);
    //    }
    //  }

    //  return std::make_tuple(SIZE_MAX, SIZE_MAX);
    //}

    //void system::remove_from_queue(system::sound_processing_data *queue, const size_t index) {
    //  if (index >= queue_size) return;

    //  for (size_t i = 0; i < queue_size-1; ++i) {
    //    queue[i] = queue[i + 1];
    //  }
    //  queue[queue_size - 1] = sound_processing_data();
    //}

    system::volume_set::volume_set() noexcept
      : master(1.0f), source{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f}
    {}

    //const size_t system::volume_set::sound_types_count;

    //system::sound_processing_data::sound_processing_data() noexcept
    //  : res(nullptr), time(0), loaded_frames(0), processed_frames(0), id(0) {}

    //void system::sound_processing_data::init(
    //    const size_t id, const resource *res,
    //    const struct settings &info
    //) noexcept {
    //  this->id = id;
    //  this->res = res;
    //  this->info = info;
    //  this->time = 0;
    //  this->loaded_frames = 0;
    //}

    //void system::sound_processing_data::reset() noexcept { 
    //  init(0, nullptr, settings());
    //}

    //size_t system::sound_processing_data::load_next(
    //  const uint32_t buffer,
    //  const size_t frames_count,
    //  const uint16_t channels
    //) {
    //  if (!res->sound->seek(loaded_frames))
    //    utils::error{}("seek to pcm frame {} failed in resource '{}'", loaded_frames, res->id);

    //  const uint16_t final_channels = std::min(channels, res->sound->channels());
    //  const size_t frames = res->sound->get_frames(buffer, frames_count, final_channels);

    //  return frames;
    //}

    //system::source_data::source_data(const struct source &source, sound_processing_data *queue) noexcept 
    //  : source(source), queue(queue)
    //{}

    //void system::source_data::init(const float volume, size_t frames_count) {
    //  if (queue->time != 0) return;

    //  if (frames_count >= queue->res->sound->frames_count())
    //    frames_count = queue->res->sound->frames_count() / 2 + 1;

    //  const uint16_t channels_count = queue->info.is_mono ? 1 : 0;
    //  queue->loaded_frames += queue->load_next(source.buffers[0], frames_count, channels_count);
    //  queue->loaded_frames += queue->load_next(source.buffers[1], frames_count, channels_count);
    //  al_call(alSourceQueueBuffers, source.handle, 2, source.buffers);
    //  al_call(alSourcef, source.handle, AL_PITCH, queue->info.speed);

    //  const auto pos = queue->info.pos, dir = queue->info.dir, vel = queue->info.vel;
    //  al_call(alSource3f, source.handle, AL_POSITION, pos.x, pos.y, pos.z);
    //  al_call(alSource3f, source.handle, AL_DIRECTION, dir.x, dir.y, dir.z);
    //  al_call(alSource3f, source.handle, AL_VELOCITY, vel.x, vel.y, vel.z);

    //  const float gain = volume * std::clamp(queue->info.volume, 0.0f, 1.0f);
    //  al_call(alSourcef, source.handle, AL_GAIN, gain);

    //  al_call(alSourcePlay, source.handle);
    //}

    //void system::source_data::update(const float volume, const size_t frames_count) {
    //  // надо перенести в
    //  const float gain = volume * std::clamp(queue->info.volume, 0.0f, 1.0f);
    //  al_call(alSourcef, source.handle, AL_GAIN, gain);

    //  int32_t processed_buffers_count = 0;
    //  al_call(alGetSourcei, source.handle, AL_BUFFERS_PROCESSED, &processed_buffers_count);
    //  //spdlog::info("frames_count {} processed_buffers_count {} time {}", frames_count, processed_buffers_count, time);
    //  if (processed_buffers_count == 0) return;
    //  if (queue->loaded_frames >= queue->res->sound->frames_count() && !queue->info.is_loop) return;

    //  queue->loaded_frames = queue->loaded_frames >= queue->res->sound->frames_count() ? 0 : queue->loaded_frames;

    //  uint32_t buffer = 0;
    //  al_call(alSourceUnqueueBuffers, source.handle, 1, &buffer);
    //  const size_t frames = compute_buffer_frames(buffer, queue->res, queue->info.is_mono);

    //  queue->processed_frames += frames;

    //  const uint16_t channels_count = queue->info.is_mono ? 1 : 0;
    //  queue->loaded_frames += queue->load_next(buffer, frames_count, channels_count); // как залупить звук? + как залупить мелкий звук?

    //  al_call(alSourceQueueBuffers, source.handle, 1, &buffer);
    //}

    

    // как то вот так...
    struct devils_engine_sound_data_source {
      ma_data_source_base base;

      std::vector<float> buffer;
      std::atomic<size_t> write_pos;
      std::atomic<size_t> read_pos;
      std::atomic<size_t> cursor_pos;
      std::atomic<size_t> frames_read_total;
      std::atomic<size_t> frames_written_total;
      std::atomic<size_t> underrun_count;

      size_t current_cursor_pos;

      // never change once initialized 
      uint32_t sample_rate;
      uint32_t channels;
      enum format format;

      devils_engine_sound_data_source() noexcept :
        base{},
        write_pos(0),
        read_pos(0),
        cursor_pos(0),
        frames_read_total(0),
        frames_written_total(0),
        underrun_count(0),
        current_cursor_pos(0),
        sample_rate(0),
        channels(0),
        format(format::unknown)
      {}

      void reset_stream() noexcept {
        read_pos.store(0, std::memory_order_release);
        write_pos.store(0, std::memory_order_release);
        cursor_pos.store(0, std::memory_order_release);
        frames_read_total.store(0, std::memory_order_release);
        frames_written_total.store(0, std::memory_order_release);
        underrun_count.store(0, std::memory_order_release);
        current_cursor_pos = 0;
      }

      bool is_standart_mono() const noexcept {
        return sample_rate == 48000 && channels == 1 && format == format::f32;
      }

      bool is_standart_stereo() const noexcept {
        return sample_rate == 48000 && channels == 2 && format == format::f32;
      }

      size_t buffer_size() const noexcept {
        //return bytes_to_pcm_frames(this->buffer.size() * sizeof(this->buffer[0]), this->channels, this->format);
        return buffer.size() * sizeof(buffer[0]);
      }

      size_t available_frames_to_write() const noexcept {
        const size_t write_pos = this->write_pos.load(std::memory_order_relaxed);
        const size_t read_pos  = this->read_pos.load(std::memory_order_acquire);
        const size_t free_bytes = buffer_size() - (write_pos - read_pos);
        return bytes_to_pcm_frames(free_bytes, channels, format);
      }

      devils_engine_sound_data_source(const devils_engine_sound_data_source& copy) noexcept = delete;
      devils_engine_sound_data_source(devils_engine_sound_data_source&& move) noexcept = delete;
      devils_engine_sound_data_source & operator=(const devils_engine_sound_data_source& copy) noexcept = delete;
      devils_engine_sound_data_source & operator=(devils_engine_sound_data_source&& move) noexcept = delete;
    };

    static_assert(offsetof(devils_engine_sound_data_source, base) == 0);

    // надо все переделать на фреймы ...........
    
    static ma_result my_data_source_read(ma_data_source* pDataSource, void* pFramesOut, ma_uint64 frameCount, ma_uint64* pFramesRead) {
      auto ptr = reinterpret_cast<devils_engine_sound_data_source*>(pDataSource);
      auto out_buffer = reinterpret_cast<uint8_t*>(pFramesOut);
      auto in_buffer = reinterpret_cast<const uint8_t*>(ptr->buffer.data());
      //const size_t buffer_size = bytes_to_pcm_frames(ptr->buffer.size() * sizeof(ptr->buffer[0]), ptr->channels, ptr->format);
      const size_t buffer_size = ptr->buffer_size();

      const size_t bytesCount = pcm_samples_to_bytes(frameCount, ptr->channels, ptr->format);
      //utils::info("Data source read {} frames ({} samples, {} bytes)", frameCount, samplesCount, bytesCount);

      // что тут мы делаем:
      // получаем позицию кольцевого буфера (сначала read потом write)
      //const size_t samples_needed = frameCount * ptr->channels;
      const size_t read_pos  = ptr->read_pos.load(std::memory_order_relaxed);
      const size_t write_pos = ptr->write_pos.load(std::memory_order_acquire);

      // позиция записи - позиция чтения = сколько всего фреймов доступно (наверное должно быть в байтах)
      // нужно прочитать только samples_needed
      const size_t available = write_pos - read_pos;
      const size_t to_read = std::min(bytesCount, available);
      const size_t buffer_read_pos = read_pos % buffer_size;

      // первая часть от read_pos до buffer.end()
      const size_t first_part = std::min(to_read, buffer_size - buffer_read_pos);

      //utils::info("Can read {}, first part {}, read pos {}, write pos {}", to_read, first_part, read_pos, write_pos);
      memcpy(out_buffer, in_buffer + buffer_read_pos, first_part);

      // вторая часть от buffer.begin() до (to_read - first_part)
      const size_t second_part = to_read - first_part;
      if (second_part > 0) {
        memcpy(out_buffer + first_part, in_buffer, second_part);
      }

      // если не хватает сэмплов заполним тишиной
      if (to_read < bytesCount) {
        memset(out_buffer + to_read, 0, bytesCount - to_read);
        ptr->underrun_count.fetch_add(1, std::memory_order_relaxed);
      }

      // двигаем указатель чтения
      ptr->read_pos.store(read_pos + to_read, std::memory_order_release);

      const size_t final_frames_read = bytes_to_pcm_frames(to_read, ptr->channels, ptr->format);
      ptr->current_cursor_pos += final_frames_read;
      ptr->cursor_pos.store(ptr->current_cursor_pos, std::memory_order_release);
      ptr->frames_read_total.fetch_add(final_frames_read, std::memory_order_release);

      // может быть стриминг... вообще для стриминга генерировать бы пакеты вне my_data_source_read
      // мы бы хотели добавить шумы
      if (to_read == 0) {
        if (pFramesRead) *pFramesRead = 0;
        return MA_AT_END;
      }

      if (pFramesRead) *pFramesRead = final_frames_read;

      return MA_SUCCESS;
    }

    static size_t write_decoded_pcm_frames(devils_engine_sound_data_source* src, const void* data, const size_t frame_count) {
      auto in = reinterpret_cast<const uint8_t*>(data);
      auto out = reinterpret_cast<uint8_t*>(src->buffer.data());
      const size_t buffer_size = src->buffer.size() * sizeof(src->buffer[0]);

      const size_t bytesCount = pcm_samples_to_bytes(frame_count, src->channels, src->format);

      // получаем позицию кольцевого буфера (сначала write потом read)
      const size_t write_pos = src->write_pos.load(std::memory_order_relaxed);
      const size_t read_pos  = src->read_pos.load(std::memory_order_acquire);

      const size_t free = buffer_size - (write_pos - read_pos);
      const size_t to_write = std::min(bytesCount, free);
      const size_t buffer_write_pos = write_pos % buffer_size;

      // memcpy (with wrap)
      const size_t first_part = std::min(to_write, buffer_size - buffer_write_pos);
      memcpy(out + buffer_write_pos, in, first_part);

      // вторая часть от buffer.begin() до (to_read - first_part)
      const size_t second_part = to_write - first_part;
      if (second_part > 0) {
        memcpy(out, in + first_part, second_part);
      }

      src->write_pos.store(write_pos + to_write, std::memory_order_release);

      const size_t written_frames = bytes_to_pcm_frames(to_write, src->channels, src->format);
      src->frames_written_total.fetch_add(written_frames, std::memory_order_release);
      return written_frames;
    }

    // поиск в фреймах (сэмплы * каналы)
    static ma_result my_data_source_seek(ma_data_source* pDataSource, ma_uint64 frameIndex) {
      auto ptr = (devils_engine_sound_data_source*)pDataSource;
      const size_t frame_bytes = pcm_frame_to_bytes(ptr->channels, ptr->format);
      const size_t byte_pos = frameIndex * frame_bytes;
      ptr->read_pos.store(byte_pos, std::memory_order_release);
      ptr->write_pos.store(byte_pos, std::memory_order_release);
      ptr->cursor_pos.store(frameIndex, std::memory_order_release);
      ptr->frames_read_total.store(frameIndex, std::memory_order_release);
      ptr->frames_written_total.store(frameIndex, std::memory_order_release);
      ptr->current_cursor_pos = frameIndex;

      utils::info("Data source seek");

      return MA_SUCCESS;
    }

    static ma_result my_data_source_get_data_format(ma_data_source* pDataSource, ma_format* pFormat, ma_uint32* pChannels, ma_uint32* pSampleRate, ma_channel* pChannelMap, size_t channelMapCap) {
      auto ptr = (devils_engine_sound_data_source*)pDataSource;
      if (pFormat) *pFormat = static_cast<ma_format>(ptr->format);
      if (pChannels) *pChannels = ptr->channels;
      if (pSampleRate) *pSampleRate = ptr->sample_rate;

      if (pChannelMap != nullptr && channelMapCap >= ptr->channels) {
        if (ptr->channels == 1) {
          pChannelMap[0] = MA_CHANNEL_MONO;
        } else if (ptr->channels == 2) {
          pChannelMap[0] = MA_CHANNEL_FRONT_LEFT;
          pChannelMap[1] = MA_CHANNEL_FRONT_RIGHT;
        } else if (ptr->channels == 6) {
          pChannelMap[0] = MA_CHANNEL_FRONT_LEFT;
          pChannelMap[1] = MA_CHANNEL_FRONT_RIGHT;
          pChannelMap[2] = MA_CHANNEL_FRONT_CENTER;
          pChannelMap[3] = MA_CHANNEL_LFE;
          pChannelMap[4] = MA_CHANNEL_BACK_LEFT;
          pChannelMap[5] = MA_CHANNEL_BACK_RIGHT;
        } else if (ptr->channels == 8) {
          pChannelMap[0] = MA_CHANNEL_FRONT_LEFT;
          pChannelMap[1] = MA_CHANNEL_FRONT_RIGHT;
          pChannelMap[2] = MA_CHANNEL_FRONT_CENTER;
          pChannelMap[3] = MA_CHANNEL_LFE;
          pChannelMap[4] = MA_CHANNEL_BACK_LEFT;
          pChannelMap[5] = MA_CHANNEL_BACK_RIGHT;
          pChannelMap[6] = MA_CHANNEL_SIDE_LEFT;
          pChannelMap[7] = MA_CHANNEL_SIDE_RIGHT;
        } else {
          for (ma_uint32 i = 0; i < ptr->channels; ++i) {
            pChannelMap[i] = MA_CHANNEL_NONE;
          }
        }
      }

      //utils::info("Data source get_data_format: {} {} {}", static_cast<size_t>(ptr->format), ptr->channels, ptr->sample_rate);

      return MA_SUCCESS;
    }

    // зацикленный звук надо бы вычитать из курсора
    // то есть для каждого прочитанного файла увеличивать дополнительный счетчик и тут его вычитать
    static ma_result my_data_source_get_cursor(ma_data_source* pDataSource, ma_uint64* pCursor) {
      auto ptr = (devils_engine_sound_data_source*)pDataSource;

      *pCursor = ptr->cursor_pos.load(std::memory_order_acquire);

      utils::info("Data source get_cursor");

      return MA_SUCCESS;
    }

    // возвращаю именно фреймы (сэмплы * каналы)
    static ma_result my_data_source_get_length(ma_data_source* pDataSource, ma_uint64* pLength) {
      auto ptr = (devils_engine_sound_data_source*)pDataSource;
      // тут мы должны понять какой именно источник данных у нас скрыт в ресурсе
      // (например если VOIP то мы будем заполнять потихоньку буфер данных в ресурсе)
      // пока что такого нет

      *pLength = ptr->frames_written_total.load(std::memory_order_acquire);

      utils::info("Data source get_length");

      return MA_SUCCESS;
    }

    static ma_data_source_vtable g_data_source_vtable = {
      my_data_source_read,
      my_data_source_seek,
      my_data_source_get_data_format,
      my_data_source_get_cursor,
      my_data_source_get_length,
      nullptr,
      0
    };


    static ma_result standart_source_init(devils_engine_sound_data_source* pMyDataSource, const uint32_t sample_rate, const uint32_t channels, const enum format format, const double buffer_seconds) {
      auto baseConfig = ma_data_source_config_init();
      baseConfig.vtable = &g_data_source_vtable;

      const auto result = ma_data_source_init(&baseConfig, &pMyDataSource->base);
      if (result != MA_SUCCESS) {
        return result;
      }

      pMyDataSource->format = format;
      pMyDataSource->channels = channels;
      pMyDataSource->sample_rate = sample_rate;

      const size_t buffer_size = seconds_to_bytes(buffer_seconds, pMyDataSource->sample_rate, pMyDataSource->channels, pMyDataSource->format);
      pMyDataSource->buffer.resize(buffer_size, 0.0f);
      pMyDataSource->read_pos = 0;
      pMyDataSource->write_pos = 0;

      return MA_SUCCESS;
    }

    static ma_result standart_mono_source_init(devils_engine_sound_data_source* pMyDataSource, const double buffer_seconds) {
      return standart_source_init(pMyDataSource, 48000, 1, format::f32, buffer_seconds);
    }

    static ma_result standart_stereo_source_init(devils_engine_sound_data_source* pMyDataSource, const double buffer_seconds) {
      return standart_source_init(pMyDataSource, 48000, 2, format::f32, buffer_seconds);
    }

//    static ma_result sound_instance_init(system2::sound_instance* ptr, const uint32_t sample_rate, const uint32_t channels, const enum format format) {
//      
//    }

    static void data_source_uninit(devils_engine_sound_data_source* pMyDataSource) {
      // ... do the uninitialization of your custom data source here ...

      ma_data_source_uninit(&pMyDataSource->base);
    }

    struct system2::sound_instance {
      ma_sound sound;
      devils_engine_sound_data_source data_source;

      sound_instance() noexcept = default;
      sound_instance(const sound_instance& copy) noexcept = delete;
      sound_instance(sound_instance&& move) noexcept = delete;
      sound_instance & operator=(const sound_instance& copy) noexcept = delete;
      sound_instance & operator=(sound_instance&& move) noexcept = delete;
    };

    static void playback_data_callback(ma_device* pDevice, void* pOutput, const void*, ma_uint32 frameCount) {
      const auto ret = ma_engine_read_pcm_frames(reinterpret_cast<ma_engine*>(pDevice->pUserData), pOutput, frameCount, nullptr);
      if (ret != MA_SUCCESS) {
        utils::error{}("'ma_engine_read_pcm_frames' error");
      }
    }

    static bool is_positioned_type(const enum type sound_type) noexcept {
      return sound_type == type::sfx || sound_type == type::talk_pos;
    }

    static bool is_too_far(const ma_engine* engine, const struct task &task) noexcept {
      if (!is_positioned_type(task.type)) return false;
      const auto pos = ma_engine_listener_get_position(engine, 0);
      const auto dist2 = distance2(task.pos, vec3(pos.x, pos.y, pos.z));
      return dist2 >= default_sound_max_distance * default_sound_max_distance;
    }

    bool system2::playback_devices(std::vector<std::string> &out) {
      out.clear();

      ma_context context;
      auto result = ma_context_init(nullptr, 0, nullptr, &context);
      if (result != MA_SUCCESS) return false;

      ma_device_info* playback_infos = nullptr;
      ma_uint32 playback_count = 0;
      result = ma_context_get_devices(&context, &playback_infos, &playback_count, nullptr, nullptr);
      if (result == MA_SUCCESS) {
        out.reserve(playback_count);
        for (ma_uint32 i = 0; i < playback_count; ++i) {
          out.emplace_back(playback_infos[i].name);
        }
      }

      ma_context_uninit(&context);
      return result == MA_SUCCESS;
    }

    system2::system2(const std::string_view device_name, const double stream_buffer_seconds, const size_t decode_frames_per_update) :
      cur_time(0),
      playback_channels(0),
      playback_sample_rate(0),
      stream_buffer_seconds(std::max(stream_buffer_seconds, 0.05)),
      decode_frames_per_update(decode_frames_per_update)
    {
      ma_result result = MA_SUCCESS;

      {
        auto cfg = ma_engine_config_init();
        //cfg.pDevice = m_device.get();
        cfg.listenerCount = 1;
        cfg.noAutoStart = MA_TRUE; // ОЧЕНЬ ВАЖНО НЕ ДАВАТЬ ДВИЖКУ САМОСТОЯТЕЛЬНО ЗАПУСКАТЬСЯ!!!!!

        m_engine.reset(new ma_engine);
        result = ma_engine_init(&cfg, m_engine.get());
        if (result != MA_SUCCESS) {
          utils::error{}("Could not initialize engine");
        }
      }

      // возможно потом придется зафорсить количество каналов
      {
        m_context.reset(new ma_context);
        result = ma_context_init(nullptr, 0, nullptr, m_context.get());
        if (result != MA_SUCCESS) {
          utils::error{}("Could not initialize sound context");
        }

        const ma_device_id* device_id = nullptr;
        std::string_view default_device_name;
        if (!device_name.empty()) {
          ma_device_info* playback_infos = nullptr;
          ma_uint32 playback_count = 0;
          result = ma_context_get_devices(m_context.get(), &playback_infos, &playback_count, nullptr, nullptr);
          if (result != MA_SUCCESS) {
            utils::error{}("Could not enumerate sound devices");
          }

          for (ma_uint32 i = 0; i < playback_count; ++i) {
            if (playback_infos[i].isDefault) {
              default_device_name = playback_infos[i].name;
            }

            if (std::string_view(playback_infos[i].name) != device_name) continue;
            device_id = &playback_infos[i].id;
            break;
          }

          if (device_id == nullptr) {
            const std::string_view fallback_name = default_device_name.empty() ? std::string_view("<unknown>") : default_device_name;
            utils::warn("Could not find requested sound device '{}'; creating default playback device '{}'", device_name, fallback_name);
          }
        }

        auto cfg = ma_device_config_init(ma_device_type_playback);
        cfg.playback.pDeviceID = device_id;
        cfg.playback.format = ma_format_f32;
        cfg.playback.channels = 0;
        cfg.sampleRate = 0;
        cfg.dataCallback = &playback_data_callback;
        cfg.pUserData = m_engine.get();

        m_device.reset(new ma_device);
        result = ma_device_init(m_context.get(), &cfg, m_device.get());
        if (result != MA_SUCCESS) {
          utils::error{}("Could not initialize device");
        }

        utils::info("Using sound device '{}'", std::string_view(m_device->playback.name));

        playback_channels = m_device->playback.channels;
        playback_sample_rate = m_device->sampleRate;
      }
      
      {
        for (size_t i = 0; i < default_mono_sounds_count; ++i) {
          m_instances_mono.emplace_back(new sound_instance);
          m_instances_mono_stack.push_back(m_instances_mono.back().get());

          auto s = &m_instances_mono.back()->sound;
          auto ds = &m_instances_mono.back()->data_source;

          result = standart_mono_source_init(ds, this->stream_buffer_seconds);
          if (result != MA_SUCCESS) {
            utils::error{}("Could not initialize data source");
          }
          
          // нужно заранее продумать звуковые группы
          result = ma_sound_init_from_data_source(m_engine.get(), ds, 0, nullptr, s);
          if (result != MA_SUCCESS) {
            utils::error{}("Could not initialize sound");
          }

          ma_sound_set_attenuation_model(s, ma_attenuation_model_linear);
          ma_sound_set_rolloff(s, 1.0f);
          ma_sound_set_min_gain(s, 0.0f);
          ma_sound_set_max_gain(s, 1.0f);
          ma_sound_set_volume(s, 1.0f);
          ma_sound_set_min_distance(s, 0.0f);
          ma_sound_set_max_distance(s, default_sound_max_distance);
        }
      }

      {
        for (size_t i = 0; i < default_stereo_sounds_count; ++i) {
          m_instances_stereo.emplace_back(new sound_instance);
          m_instances_stereo_stack.push_back(m_instances_stereo.back().get());

          auto s = &m_instances_stereo.back()->sound;
          auto ds = &m_instances_stereo.back()->data_source;

          result = standart_stereo_source_init(ds, this->stream_buffer_seconds);
          if (result != MA_SUCCESS) {
            utils::error{}("Could not initialize data source");
          }
          
          // нужно заранее продумать звуковые группы
          result = ma_sound_init_from_data_source(m_engine.get(), ds, MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr, s);
          if (result != MA_SUCCESS) {
            utils::error{}("Could not initialize sound");
          }

          ma_sound_set_attenuation_model(s, ma_attenuation_model_linear);
          ma_sound_set_rolloff(s, 1.0f);
          ma_sound_set_min_gain(s, 0.0f);
          ma_sound_set_max_gain(s, 1.0f);
          ma_sound_set_volume(s, 1.0f);
          ma_sound_set_min_distance(s, 0.0f);
          ma_sound_set_max_distance(s, default_sound_max_distance);
        }
      }

      if (this->decode_frames_per_update == 0) {
        this->decode_frames_per_update = playback_sample_rate;
      }

      constexpr size_t cache_size = pcm_samples_to_bytes(48000, 8, format::f32) / sizeof(uint32_t);
      cache1.resize(cache_size, 0);
      cache2.resize(cache_size, 0);

      result = ma_device_start(m_device.get());
      if (result != MA_SUCCESS) {
        utils::error{}("Could not start device");
      }
    }

    system2::~system2() noexcept {
      ma_device_stop(m_device.get());

      for (auto& ptr : m_instances) {
        ma_sound_stop(&ptr->sound);
        ma_sound_uninit(&ptr->sound);
        data_source_uninit(&ptr->data_source);
      }

      for (auto& ptr : m_instances_mono) {
        ma_sound_stop(&ptr->sound);
        ma_sound_uninit(&ptr->sound);
        data_source_uninit(&ptr->data_source);
      }

      for (auto& ptr : m_instances_stereo) {
        ma_sound_stop(&ptr->sound);
        ma_sound_uninit(&ptr->sound);
        data_source_uninit(&ptr->data_source);
      }

      ma_device_uninit(m_device.get());
      ma_context_uninit(m_context.get());
      ma_engine_uninit(m_engine.get());
    }

    bool system2::setup_sound(const struct task &task) {
      // еще нужно поглядеть будем ли мы вообще воспроизводить этот звук
      if (task.id == SIZE_MAX || task.res.data.empty() || task.res.type == data_type::undefined) return false;
      if (is_too_far(m_engine.get(), task)) return false;
      if (find_task_id(task.id) != SIZE_MAX) return false;
      m_tasks.push_back({nullptr, task, cur_time, 0, 0, 0, 0, false, false, false, nullptr, nullptr});
      return true;
    }

    bool system2::remove_sound(const size_t task_id) {
      const size_t index = find_task_id(task_id);
      if (index == SIZE_MAX) return false;
      auto& cur_task = m_tasks[index];

      if (cur_task.inst != nullptr) {
        auto* inst = cur_task.inst;
        ma_sound_stop(&inst->sound);
        inst->data_source.reset_stream();

        if (inst->data_source.is_standart_mono()) {
          m_instances_mono_stack.push_back(inst);
        } else if (inst->data_source.is_standart_stereo()) {
          m_instances_stereo_stack.push_back(inst);
        } else {
          ma_sound_uninit(&inst->sound);
          data_source_uninit(&inst->data_source);
          auto itr = std::find_if(m_instances.begin(), m_instances.end(), [ptr = inst] (const auto &a) { return a.get() == ptr; });
          if (itr != m_instances.end()) m_instances.erase(itr);
        }

        std::erase_if(m_tasks, [inst] (const auto &t) { return t.inst == inst; });
        return true;
      }

      m_tasks.erase(m_tasks.begin() + index);
      return true;
    }

    bool system2::play_sound(const size_t task_id) {
      const size_t index = find_task_id(task_id);
      if (index == SIZE_MAX) return false;

      auto& cur_task = m_tasks[index];
      if (cur_task.inst == nullptr) return false;
      const auto res = ma_sound_start(&cur_task.inst->sound);
      if (res == MA_SUCCESS) cur_task.started = true;
      return true;
    }

    bool system2::stop_sound(const size_t task_id) {
      const size_t index = find_task_id(task_id);
      if (index == SIZE_MAX) return false;

      auto& cur_task = m_tasks[index];
      if (cur_task.inst == nullptr) return false;
      ma_sound_stop(&cur_task.inst->sound);
      cur_task.started = false;
      return true;
    }

    double system2::stat_sound(const size_t task_id) const {
      const size_t index = find_task_id(task_id);
      if (index == SIZE_MAX) return 0.0;

      auto& cur_task = m_tasks[index];
      if (cur_task.inst == nullptr || cur_task.stream_frames_count == 0 || cur_task.source_frames_count == 0)
        return cur_task.task.start; // ещё не играет → абсолютная стартовая позиция

      // АБСОЛЮТНАЯ позиция в источнике (с учётом старта не с нуля), как и в make_status
      const size_t start_frame = cur_task.source_frames_count - cur_task.stream_frames_count;
      const size_t frames_read = cur_task.inst->data_source.frames_read_total.load(std::memory_order_acquire);
      if (frames_read <= cur_task.stream_begin_frame) return cur_task.task.start;
      const size_t local_frames = std::min(frames_read - cur_task.stream_begin_frame, cur_task.stream_frames_count);
      return double(start_frame + local_frames) / double(cur_task.source_frames_count);
    }

    task_status system2::make_status(const system2::sound_task &task) const {
      task_status status;
      status.id = task.task.id;
      status.resource_id = task.task.res.id;
      status.type = task.task.type;
      status.state = task_state::queued;
      // progress — АБСОЛЮТНАЯ позиция в ИСТОЧНИКЕ [0,1], а не доля проигранного сегмента.
      // При старте не с нуля сегмент = source - start_frame, поэтому без учёта start_frame
      // прогресс шёл бы 0→1 по куску и «вдвое быстрее». До старта показываем сам старт.
      status.progress = task.task.start;
      status.after = task.task.after;
      status.frames_decoded = task.frames_decoded;
      status.frames_total = task.stream_frames_count;
      status.underruns = 0;
      status.pos = task.task.pos;
      status.dir = task.task.dir;
      status.vel = task.task.vel;

      if (task.inst == nullptr || task.stream_frames_count == 0 || task.source_frames_count == 0) return status;

      const size_t start_frame = task.source_frames_count - task.stream_frames_count; // кадры до сегмента
      const size_t frames_read = task.inst->data_source.frames_read_total.load(std::memory_order_acquire);
      if (frames_read > task.stream_begin_frame) {
        const size_t local_frames = std::min(frames_read - task.stream_begin_frame, task.stream_frames_count);
        status.progress = double(start_frame + local_frames) / double(task.source_frames_count);
      }

      status.state = status.progress >= 1.0 ? task_state::finished : task_state::playing;
      status.underruns = task.inst->data_source.underrun_count.load(std::memory_order_acquire);
      return status;
    }

    bool system2::stat_sound(const size_t task_id, struct task_status &out) const {
      const size_t index = find_task_id(task_id);
      if (index == SIZE_MAX) return false;
      out = make_status(m_tasks[index]);
      return true;
    }

    void system2::snapshot(std::vector<struct task_status> &out) const {
      out.clear();
      out.reserve(m_tasks.size());
      for (const auto& task : m_tasks) {
        out.push_back(make_status(task));
      }
    }

    bool system2::set_sound(const size_t task_id, const double place) {
      const size_t index = find_task_id(task_id);
      if (index == SIZE_MAX) return false;

      auto& cur_task = m_tasks[index];
      cur_task.task.start = std::clamp(place, 0.0, 1.0);
      if (cur_task.inst == nullptr) return true;

      const auto shared_inst = cur_task.inst;
      const auto shared_count = std::count_if(m_tasks.begin(), m_tasks.end(), [shared_inst] (const auto &t) {
        return t.inst == shared_inst;
      });
      if (shared_count != 1) return false;

      ma_sound_stop(&shared_inst->sound);
      shared_inst->data_source.reset_stream();
      cur_task.frames_decoded = 0;
      cur_task.stream_begin_frame = 0;
      cur_task.stream_frames_count = 0;
      cur_task.source_frames_count = 0;
      cur_task.initialized = false;
      cur_task.segment_registered = false;
      cur_task.started = false;
      cur_task.decoder.reset();
      cur_task.converter.reset();
      cur_task.timestamp = cur_time;
      return true;
    }

    bool system2::update_sound(const struct task_update &task) {
      const size_t index = find_task_id(task.id);
      if (index == SIZE_MAX) return false;

      auto& cur_task = m_tasks[index];
      if (cur_task.inst == nullptr) return false;

      cur_task.task.pos = task.pos;
      cur_task.task.dir = task.dir;
      cur_task.task.vel = task.vel;

      if (is_too_far(m_engine.get(), cur_task.task)) {
        return remove_sound(task.id);
      }

      ma_sound_set_position(&cur_task.inst->sound, task.pos.x, task.pos.y, task.pos.z);
      ma_sound_set_direction(&cur_task.inst->sound, task.dir.x, task.dir.y, task.dir.z);
      ma_sound_set_velocity(&cur_task.inst->sound, task.vel.x, task.vel.y, task.vel.z);
      return true;
    }

    void system2::set_decode_budget_frames(const size_t frames) noexcept {
      decode_frames_per_update = frames == 0 ? playback_sample_rate : frames;
    }

    bool system2::set_listener_pos(const vec3 &pos) {
      ma_engine_listener_set_position(m_engine.get(), 0, pos.x, pos.y, pos.z);
      return true;
    }

    bool system2::set_listener_ori(const vec3 &look_at, const vec3 &up) {
      const auto pos = ma_engine_listener_get_position(m_engine.get(), 0);
      const auto norm = normalize(look_at - vec3(pos.x, pos.y, pos.z));
      ma_engine_listener_set_direction(m_engine.get(), 0, norm.x, norm.y, norm.z);
      ma_engine_listener_set_world_up(m_engine.get(), 0, up.x, up.y, up.z);
      return true;
    }

    bool system2::set_listener_vel(const vec3 &vel) {
      ma_engine_listener_set_velocity(m_engine.get(), 0, vel.x, vel.y, vel.z);
      return true;
    }

    // 1 - очень громко, наверное по умолчанию нужно ставить 0.2
    // хотя может быть вообще замаппить [0, 0.2]
    void system2::set_master_volume(const float val) {
      ma_engine_set_volume(m_engine.get(), val);
    }

    void system2::set_source_volume(const uint32_t, const float) {
      // ...
      utils::error{}("Not implemented");
    }

    void system2::update(const size_t time) {
      this->cur_time += time;

      for (auto& t : m_tasks) {
        if (!t.initialized) {
          // PCM-ветка: данные уже декодированы (короткий звук, sound_resource), метаданные в resource2 →
          // pcm_decoder-passthrough. Остальные типы декодятся из сжатых байт через make_decoder.
          if (t.task.res.type == data_type::pcm) {
            const auto& r = t.task.res;
            t.decoder = std::make_unique<pcm_decoder>(
              r.data.data(), r.data.size(), r.sample_format, r.channels, r.sample_rate, r.frames_count);
          } else {
            t.decoder = make_decoder(t.task.res.type, t.task.res.id, t.task.res.data);
          }
          if (!t.decoder) {
            utils::error{}("Could not create decoder for sound task '{}'", t.task.res.id);
          }

          t.source_frames_count = t.decoder->frames_count();
          const size_t start_frame = std::min<size_t>(t.task.start * double(t.source_frames_count), t.source_frames_count);
          if (start_frame != 0 && !t.decoder->seek(start_frame)) {
            utils::error{}("Could not seek sound task '{}' to frame {}", t.task.res.id, start_frame);
          }
          t.stream_frames_count = t.source_frames_count - start_frame;

          const uint32_t out_channels = 
            (t.task.type == type::sfx || t.task.type == type::talk_pos ? 1 : 
              (t.task.type == type::ui_effect ? 
                2 : t.decoder->channels()
              )
            );

          auto cfg = ma_data_converter_config_init(
            static_cast<ma_format>(t.decoder->format()),
            ma_format_f32,
            t.decoder->channels(),
            out_channels,
            t.decoder->sample_rate(),
            playback_sample_rate
          );

          t.converter.reset(new miniaudio_data_converter);
          auto res = ma_data_converter_init(&cfg, nullptr, t.converter.get());
          if (res != MA_SUCCESS) {
            utils::error{}("Could not initialize data converter for sound task '{}'", t.task.res.id);
          }

          t.initialized = true;
        }

        if (t.inst == nullptr) {
          if (t.task.after != SIZE_MAX) {
            const size_t prev_index = find_task_id(t.task.after);
            if (prev_index == SIZE_MAX || m_tasks[prev_index].inst == nullptr) continue;
            t.inst = m_tasks[prev_index].inst;
          } else if (t.task.type == type::sfx || t.task.type == type::talk_pos) {
            if (!m_instances_mono_stack.empty()) {
              t.inst = m_instances_mono_stack.back();
              m_instances_mono_stack.pop_back();
              t.inst->data_source.reset_stream();
            }
          } else if (t.task.type == type::ui_effect) {
            if (!m_instances_stereo_stack.empty()) {
              t.inst = m_instances_stereo_stack.back();
              m_instances_stereo_stack.pop_back();
              t.inst->data_source.reset_stream();
            }
          } else if (t.decoder->channels() == 1) {
            if (!m_instances_mono_stack.empty()) {
              t.inst = m_instances_mono_stack.back();
              m_instances_mono_stack.pop_back();
              t.inst->data_source.reset_stream();
            }
          } else if (t.decoder->channels() == 2) {
            if (!m_instances_stereo_stack.empty()) {
              t.inst = m_instances_stereo_stack.back();
              m_instances_stereo_stack.pop_back();
              t.inst->data_source.reset_stream();
            }
          } else {
            m_instances.emplace_back(new sound_instance);
            instance_init(m_instances.back().get(), playback_sample_rate, t.decoder->channels(), format::f32);
            t.inst = m_instances.back().get();
            t.inst->data_source.reset_stream();
          }
        }

        if (t.inst == nullptr) continue;

        if (!t.segment_registered) {
          ma_sound_set_volume(&t.inst->sound, t.task.volume);
          ma_sound_set_pitch(&t.inst->sound, t.task.pitch);
          ma_sound_set_position(&t.inst->sound, t.task.pos.x, t.task.pos.y, t.task.pos.z);
          ma_sound_set_direction(&t.inst->sound, t.task.dir.x, t.task.dir.y, t.task.dir.z);
          ma_sound_set_velocity(&t.inst->sound, t.task.vel.x, t.task.vel.y, t.task.vel.z);
        }

        if (!t.segment_registered) {
          if (t.task.after != SIZE_MAX) {
            const size_t prev_index = find_task_id(t.task.after);
            if (prev_index != SIZE_MAX) {
              const auto& prev = m_tasks[prev_index];
              if (!prev.segment_registered || prev.frames_decoded < prev.stream_frames_count) continue;
            }
          }

          t.stream_begin_frame = t.inst->data_source.frames_written_total.load(std::memory_order_acquire);
          t.segment_registered = true;
        }

        const bool was_start = t.frames_decoded == 0;
        const size_t free_frames = t.inst->data_source.available_frames_to_write();
        // free_frames может стать чуть больше за это время
        if (free_frames > 0 && t.frames_decoded < t.stream_frames_count) {
          const size_t input_cache_frames = bytes_to_pcm_frames(cache1.size() * sizeof(cache1[0]), t.decoder->channels(), t.decoder->format());
          const size_t output_cache_frames = bytes_to_pcm_frames(cache2.size() * sizeof(cache2[0]), t.inst->data_source.channels, t.inst->data_source.format);
          // для каждой таски декодируем очередной кусок данных
          const size_t frames_to_read = std::min({
            free_frames,
            decode_frames_per_update,
            input_cache_frames,
            output_cache_frames,
            t.stream_frames_count - t.frames_decoded
          });
          if (frames_to_read == 0) continue;

          ma_uint64 frames = t.decoder->get_frames(cache1.data(), frames_to_read);
          ma_uint64 frames_out = frames;
          t.frames_decoded += frames;

          // возможно его преобразуем
          auto res = ma_data_converter_process_pcm_frames(t.converter.get(), cache1.data(), &frames, cache2.data(), &frames_out);
          if (res != MA_SUCCESS) {
            utils::error{}("Could not convert {} frames data for resorce '{}'", frames, t.task.res.id);
          }

          // и кладем получившееся в буфер
          const size_t writed_frames = write_decoded_pcm_frames(&t.inst->data_source, cache2.data(), frames_out);
          assert(writed_frames == frames_out);
        }

        if (was_start && !t.started) {
          const auto res = ma_sound_start(&t.inst->sound);
          assert(res == MA_SUCCESS);
          t.started = true;
        }
      }

      for (auto it = m_tasks.begin(); it != m_tasks.end();) {
        auto& t = *it;
        if (t.inst == nullptr || !t.segment_registered) {
          ++it;
          continue;
        }

        const size_t frames_read = t.inst->data_source.frames_read_total.load(std::memory_order_acquire);
        const bool fully_decoded = t.frames_decoded >= t.stream_frames_count;
        const bool fully_read = frames_read >= t.stream_begin_frame + t.stream_frames_count;
        if (fully_decoded && fully_read) {
          auto* inst = t.inst;
          it = m_tasks.erase(it);
          const bool has_more_tasks = std::any_of(m_tasks.begin(), m_tasks.end(), [inst] (const auto &other) {
            return other.inst == inst;
          });
          if (!has_more_tasks) {
            ma_sound_stop(&inst->sound);
            inst->data_source.reset_stream();
            if (inst->data_source.is_standart_mono()) {
              m_instances_mono_stack.push_back(inst);
            } else if (inst->data_source.is_standart_stereo()) {
              m_instances_stereo_stack.push_back(inst);
            } else {
              ma_sound_uninit(&inst->sound);
              data_source_uninit(&inst->data_source);
              auto custom_it = std::find_if(m_instances.begin(), m_instances.end(), [inst] (const auto &ptr) {
                return ptr.get() == inst;
              });
              if (custom_it != m_instances.end()) m_instances.erase(custom_it);
            }
          }
          continue;
        }

        ++it;
      }
    }

    size_t system2::find_task_id(const size_t task_id) const {
      if (task_id == SIZE_MAX) return SIZE_MAX;

      const auto itr = std::find_if(m_tasks.begin(), m_tasks.end(), [task_id] (const auto &inst) { return inst.task.id == task_id; });
      if (itr != m_tasks.end() && itr->task.id == task_id) return itr - m_tasks.begin();
      return SIZE_MAX;
    }

    // останавливать ma_device когда создаем/удаляем инстанс? 
    // похоже что это пока что костыль, но что то подобное делать придется
    uint32_t system2::instance_init(sound_instance* inst, const uint32_t sample_rate, const uint32_t channels, const enum format format) const {
      auto s = &inst->sound;
      auto ds = &inst->data_source;

      auto result = standart_source_init(ds, sample_rate, channels, format, stream_buffer_seconds);
      if (result != MA_SUCCESS) {
        utils::error{}("Could not initialize data source");
      }

      uint32_t flags = 0;
      if (channels > 1) {
        flags = MA_SOUND_FLAG_NO_SPATIALIZATION;
      }
          
      // нужно заранее продумать звуковые группы
      result = ma_sound_init_from_data_source(m_engine.get(), ds, flags, nullptr, s);
      if (result != MA_SUCCESS) {
        utils::error{}("Could not initialize sound");
      }

      ma_sound_set_attenuation_model(s, ma_attenuation_model_linear);
      ma_sound_set_rolloff(s, 1.0f);
      ma_sound_set_min_gain(s, 0.0f);
      ma_sound_set_max_gain(s, 1.0f);
      ma_sound_set_min_distance(s, 0.0f);
      ma_sound_set_max_distance(s, default_sound_max_distance);

      return MA_SUCCESS;
    }
  }
}
