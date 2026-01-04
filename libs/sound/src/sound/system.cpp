#include "system.h"

#include <iostream>
#include <cassert>
#include <cstring>
#include <glm/glm.hpp>

#include "AL/al.h"
#include "AL/alc.h"
#include "AL/alext.h"
#include "al_helper.h"

#include "devils_engine/utils/core.h"

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

static void completely_stop_source(source &s) {
  al_call(alSourceStop, s.handle);
  ALint count = 0;
  al_call(alGetSourcei, s.handle, AL_BUFFERS_PROCESSED, &count);
  uint32_t buffers[2] = {0,0};
  al_call(alSourceUnqueueBuffers, s.handle, count, buffers);
}

    system::system(const size_t queue_size) : 
      device(nullptr), ctx(nullptr), counter(1), queue_size(queue_size), sources_offset(1)//, background(nullptr)
    {
      ALCenum error = AL_NO_ERROR;

      char *devices = (char *)alcGetString(NULL, ALC_DEVICE_SPECIFIER);
      while (devices && *devices != NULL) {
        //ALCdevice *device = alcOpenDevice(devices);
        //utils::println("device", devices);
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
        sound::source s;

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

    settings::settings() noexcept : settings(0,0, false) {}

    settings::settings(
      const uint32_t type,
      const float volume,
      const float speed,
      const float rnd_pitch,
      const bool is_mono
    ) noexcept : 
      type(type), speed(speed), volume(volume), rnd_pitch(rnd_pitch), 
      is_loop(false), is_mono(is_mono), is_needed(false), force_source(UINT32_MAX),
      pos(0.0f, 0.0f, 0.0f),
      dir(0.0f, 0.0f, 0.0f),
      vel(0.0f, 0.0f, 0.0f)
    {}

    settings::settings(
      const uint32_t type,
      const uint32_t force_source,
      const bool is_mono
    ) noexcept : 
      type(type), speed(1.0f), volume(1.0f), rnd_pitch(0.0f), 
      is_loop(false), is_mono(is_mono), is_needed(false), force_source(force_source),
      pos(0.0f, 0.0f, 0.0f),
      dir(0.0f, 0.0f, 0.0f),
      vel(0.0f, 0.0f, 0.0f)
    {}

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

    bool system::set_listener_pos(const glm::vec3 &pos) {
      al_call(alListener3f, AL_POSITION, pos.x, pos.y, pos.z);
      return true;
    }

    bool system::set_listener_ori(const glm::vec3 &look_at, const glm::vec3 &up) {
      const ALfloat listener_ori[6] = { look_at.x, look_at.y, look_at.z, up.x, up.y, up.z };
      al_call(alListenerfv, AL_ORIENTATION, listener_ori);
      return true;
    }

    bool system::set_listener_vel(const glm::vec3 &vel) {
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
    void system::update(const size_t time) {
      al_call(alListenerf, AL_GAIN, volume.master);

      glm::vec3 lpos;
      al_call(alGetListenerfv, AL_POSITION, (float*)&lpos.x);

      // в чем заключается update? нужно отсортировать по дальности все вирутальные источники
      std::sort(processors.begin(), processors.end(), [&lpos](auto a, auto b){
        if (a->state() == processing_state::waiting_resource) return false;
        if (b->state() == processing_state::waiting_resource) return true;

        // может быть всегда задавать относительные координаты?
        const float d1 = a->distance(lpos);
        const float d2 = b->distance(lpos);
        return d1 < d2;
      });

      for (auto p : static_processors) {
        if (p->state() == processing_state::waiting_resource) continue;
        p->update(time);

        if (p->state() == processing_state::finished) {
          p->invalidate();
        }
      }

      for (auto p : processors) {
        // continue?
        if (p->state() == processing_state::paused || p->state() == processing_state::waiting_resource || p->distance(lpos) >= 100.0f) {
          if (p->has_source()) {
            auto s = p->release_source();
            completely_stop_source(s);
            sources.push_back(s);
          }

          //p->invalidate();

          continue;
        }

        if (p->state() == processing_state::waiting_source) {
          if (sources.empty()) continue;
          auto s = sources.back();
          sources.pop_back();
          p->setup_source(s);
        }

        p->update(time);

        if (p->state() == processing_state::finished) {
          auto s = p->release_source();
          completely_stop_source(s);
          sources.push_back(s);
          p->invalidate();
        }
      }

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

    background_source *system::create_background_source() {
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
    }

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
  }
}
