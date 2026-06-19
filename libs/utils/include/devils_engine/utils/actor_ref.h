#ifndef DEVILS_ENGINE_UTILS_ACTOR_REF_H
#define DEVILS_ENGINE_UTILS_ACTOR_REF_H

#include <cstdint>
#include <cstddef>
#include <vector>
#include <type_traits>
#include "type_traits.h"

namespace devils_engine {
namespace utils {
// не уверен что задействую все...
enum class send_status { ok, mailbox_full, no_receiver, dropped, backpressure };

class dtor { public: virtual ~dtor() noexcept = default; };

template <typename MSG_T>
class message_reciever : public dtor {
public:
  ~message_reciever() noexcept = default;
  virtual send_status send(MSG_T msg) = 0;
  virtual send_status send(std::vector<MSG_T>& msg) = 0;
};

template <size_t ID>
class actor_ref {
public:
  template <typename MSG_T, typename T>
  requires (std::is_base_of_v<message_reciever<MSG_T>, T>)
  void add_receiver(T* ptr) {
    const size_t tid = utils::sequential_type_id<ID, MSG_T>();
    if (tid >= receivers.size()) receivers.resize(tid+1, nullptr);
    receivers[tid] = ptr;
  }

  template <typename MSG_T>
  send_status send(MSG_T msg) {
    const size_t tid = utils::sequential_type_id<ID, MSG_T>();
    if (tid >= receivers.size()) return send_status::no_receiver;
    if (receivers[tid] == nullptr) return send_status::no_receiver;
    auto ptr = static_cast<message_reciever<MSG_T>*>(receivers[tid]);
    return ptr->send(std::move(msg));
  }

  template <typename MSG_T>
  send_status send(std::vector<MSG_T>& msgs) {
    const size_t tid = utils::sequential_type_id<ID, MSG_T>();
    if (tid >= receivers.size()) return send_status::no_receiver;
    if (receivers[tid] == nullptr) return send_status::no_receiver;
    auto ptr = static_cast<message_reciever<MSG_T>*>(receivers[tid]);
    return ptr->send(msgs);
  }
private:
  std::vector<dtor*> receivers;
};

}
}

#endif