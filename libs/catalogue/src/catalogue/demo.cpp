#include "demo.h"

#include "channel_data.h"

namespace devils_engine {
namespace catalogue {
demo::input_consumer::input_consumer(demo* cur) noexcept : cur(cur) {}
void demo::input_consumer::consume(const buffer& b) {
  cur->storage.input_buffer.push_back(b);
}

demo::mutator_consumer::mutator_consumer(demo* cur) noexcept : cur(cur) {}
void demo::mutator_consumer::consume(const buffer& b) {
  cur->storage.mutator_buffer.push_back(b);
}

demo::demo(const consume_type t) noexcept : c1(this), c2(this) {
  if (t == consume_type::input) {
    input_channed_data::add_consumer(&c1);
  }

  if (t == consume_type::mutator) {
    mutator_channed_data::add_consumer(&c2);
  }

  if (t == consume_type::all) {
    input_channed_data::add_consumer(&c1);
    mutator_channed_data::add_consumer(&c2);
  }
}

bool demo::write_to_disk(const std::string& path) const {
  return false;
}

bool demo::load_from_disk(const std::string& path) {
  return false;
}
}
}