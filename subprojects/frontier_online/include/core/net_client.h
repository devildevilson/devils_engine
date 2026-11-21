#ifndef FRONTIER_ONLINE_CORE_NET_CLIENT_H
#define FRONTIER_ONLINE_CORE_NET_CLIENT_H

#include <cstdint>
#include <memory>
#include <string>

// Графический клиент в живой сессии: ведомый (`net::live_follower`) внутри кадра клиента.
//
// Окно показывает мир ПОСЛЕДНЕГО ПРОВЕРЕННОГО тика (решение автора 2026-11-21: без прогноза).
// Свой клик уходит предложением и появляется в мире, когда авторитет запишет его в canonical
// bundle, то есть через задержку туда и обратно. Ведомый работает в потоке кадра и считает на
// пуле кадра: второго пула нет.
//
// Отказ присоединения, обрыв и расхождение корня завершают СЕССИЮ, а не процесс: окно остаётся с
// последним проверенным миром, причина уходит в лог. Без сетевого бэкенда сборки `connect`
// сообщает об этом и возвращает nullptr.

namespace devils_engine::thread {
class atomic_pool;
}

namespace frontier_online::core {

class actor_world_slice;

// Аргумент клиента `--join=HOST:PORT`; пустой endpoint — локальная симуляция.
struct join_request_t {
  std::string endpoint;
  // На сколько тиков вперёд от своего проверенного тика назначать предложение. Ведомый отстаёт от
  // авторитета на половину задержки, предложение летит ещё половину: цель должна оказаться позже
  // тика авторитета в момент приёма. Авторитет принимает цели не дальше 32 тиков вперёд.
  uint32_t input_lead_ticks = 12;
};

join_request_t& join_request();
[[nodiscard]] bool network_client_available() noexcept;

class net_client {
public:
  // resource_root — каталог модулей клиента (внутри папки проекта): из него собирается причинное
  // содержимое, отпечаток которого сверяется с авторитетом на рукопожатии.
  [[nodiscard]] static std::unique_ptr<net_client> connect(const join_request_t& request,
                                                           const std::string& resource_root,
                                                           devils_engine::thread::atomic_pool& pool);
  ~net_client();
  net_client(const net_client&) = delete;
  net_client& operator=(const net_client&) = delete;

  // Сетевой круг и исполнение всех полученных bundles. Только из потока кадра.
  void service();
  // Клик игрока. false — сессия не готова, кончилась или очередь неподтверждённых полна.
  bool propose_food(double x, double y);

  // Мир проверенного тика; nullptr, пока checkpoint не загружен.
  [[nodiscard]] const actor_world_slice* world() const noexcept;
  [[nodiscard]] uint64_t tick() const noexcept;
  // Сессия кончилась (отказ, обрыв, расхождение, конец прогона авторитета).
  [[nodiscard]] bool ended() const noexcept;

private:
  struct state;
  explicit net_client(std::unique_ptr<state> value) noexcept;
  std::unique_ptr<state> state_;
};

} // namespace frontier_online::core

#endif
