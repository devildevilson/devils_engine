#ifndef DEVILS_ENGINE_CATALOGUE_CHANNEL_DATA_H
#define DEVILS_ENGINE_CATALOGUE_CHANNEL_DATA_H

#include <cstdint>
#include <cstddef>
#include <array>
#include "common.h"
#include "registry.h"
#include "devils_engine/utils/core.h"
#include "devils_engine/utils/type_traits.h"

namespace devils_engine {
namespace catalogue {
enum cnsts {
  INPUT_CHANNEL_ID,
  MUTATOR_CHANNEL_ID,
  META_CHANNEL_ID,
  SERVICE_CHANNEL_ID,
};

template <size_t cid>
struct channel_data {
  static constexpr size_t max_consumer_count = 8;
  static constexpr size_t id = cid;

  static struct registry* registry;
  static struct buffer buffer;
  static std::array<consumer*, max_consumer_count> consumers;

  static void init(struct registry* registry);
  static void add_consumer(consumer* c);
  static void consume();
  static void clear_buffer();
};

using input_channed_data = channel_data<INPUT_CHANNEL_ID>;
using mutator_channed_data = channel_data<MUTATOR_CHANNEL_ID>;
using meta_channed_data = channel_data<META_CHANNEL_ID>;
using service_channed_data = channel_data<SERVICE_CHANNEL_ID>;





template <size_t id>
struct registry* channel_data<id>::registry = nullptr;
template <size_t id>
struct buffer channel_data<id>::buffer;
template <size_t id>
std::array<consumer*, channel_data<id>::max_consumer_count> channel_data<id>::consumers = { nullptr };

template <size_t id>
void channel_data<id>::init(struct registry* registry) {
  channel_data<id>::registry = registry;
  memset(consumers.data(), 0, consumers.size() * sizeof(consumers[0]));
}

template <size_t id>
void channel_data<id>::add_consumer(consumer* c) {
  size_t index = 0;
  while (index < channel_data<id>::max_consumer_count && consumers[index] != nullptr) { index += 1; }
  if (index >= channel_data<id>::max_consumer_count) utils::error("Could not add new consumer to '{}'", utils::type_name<decltype(channel_data<id>)>());
  consumers[index] = c;
}

template <size_t id>
void channel_data<id>::consume() {
  for (size_t i = 0; i < consumers.size() && consumers[i] != nullptr; ++i) {
    consumers[i]->consume(buffer);
  }
}

template <size_t id>
void channel_data<id>::clear_buffer() {
  buffer.headers.clear();
  buffer.payload.clear();
}

}
}

#endif
