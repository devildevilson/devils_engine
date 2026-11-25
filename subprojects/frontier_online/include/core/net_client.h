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
// Обрыв, тишина авторитета, срок фазы и отставание за историю ведут к ПРОДОЛЖЕНИЮ (решения автора
// 2026-11-23): клиент возвращается тем же участником с ключом `reconnect_grant`, пока авторитет
// держит его слот, и авторитет догоняет его тиками из истории или снимком кольца; мир клиента и
// неподтверждённые предложения переживают соединение. Рассинхрон лечится тем же продолжением (чужой
// корень авторитет догонит снимком). Окно истекло, догнать не из чего или «авторитет полон» —
// новый вход новым участником со свежим checkpoint. Паузы, сроки и предел попыток задаёт
// `network::reconnect_coordinator`. Несовместимая сборка, другой мир при присоединении, нарушения
// протокола и конец прогона авторитета завершают СЕССИЮ, а не процесс: окно остаётся с последним
// проверенным миром, причина уходит в лог. Критические ошибки (свои ресурсы, свои инварианты) —
// `utils::error`, их процесс не переживает. Без сетевого бэкенда `connect` сообщает об этом и
// возвращает nullptr.

namespace frontier_online::net {
class net_runtime;
}

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


// Мир сессии глазами клиента: земля из объявления мира авторитета (зерно, чанк, генератор
// проверены на рукопожатии), бокс — из его checkpoint. Свой конфиг сцены клиент для этого не
// читает: с другим зерном или размером он нарисовал бы не ту землю под чужими акторами.
struct session_world {
  std::string generator;
  uint64_t world_seed = 0;
  float min_x = 0.0f, min_y = 0.0f, max_x = 0.0f, max_y = 0.0f;
  uint32_t chunk_size = 0;

  bool operator==(const session_world&) const = default;
};

[[nodiscard]] bool network_client_available() noexcept;

// Сеть глазами интерфейса: что показать игроку о соединении.
struct net_status {
  // joining — вход идёт (соединение, рукопожатие, мир); live — свой игрок в мире; resuming — обрыв,
  // возвращаемся тем же участником; ended — сессия кончилась.
  const char* phase = "joining";
  uint64_t peer = 0;          // logical peer текущего входа; 0 — не вошёл
  uint64_t tick = 0;          // последний проверенный тик
  uint64_t catchup_tick = 0;  // тик, до которого авторитет догоняет при входе; 0 — не догоняет
  uint64_t silence_ms = 0;    // сколько проверенный тик не двигался (задержка/тишина авторитета)
  uint32_t players = 0;       // участники с игроком в мире
  uint32_t sessions = 0;      // входы новым участником
  uint32_t resumes = 0;       // возвраты тем же участником
  uint32_t attempts = 0;      // попытки текущего входа или продолжения
  uint32_t own_inputs = 0;    // свои клики, записанные авторитетом
  uint32_t rejected_inputs = 0;
};

class net_client {
public:
  // resource_root — каталог модулей клиента (внутри папки проекта): из него собирается причинное
  // содержимое, отпечаток которого сверяется с авторитетом на рукопожатии.
  // runtime — общий сетевой рантайм процесса; nullptr — свой. Общий нужен, когда в процессе уже
  // есть другой конец (тест с авторитетом): диспетчер GNS раздаёт события соединений только
  // транспортам того рантайма, который его качает, и второй рантайм их терял бы.
  [[nodiscard]] static std::unique_ptr<net_client> connect(const join_request_t& request,
                                                           const std::string& resource_root,
                                                           devils_engine::thread::atomic_pool& pool,
                                                           net::net_runtime* runtime = nullptr);
  // Уходит словом `leave` (если в мире) и коротко ждёт закрытия авторитетом.
  ~net_client();
  net_client(const net_client&) = delete;
  net_client& operator=(const net_client&) = delete;

  // Сетевой круг и исполнение всех полученных bundles. Только из потока кадра.
  void service();
  // Клик игрока. false — сессия не готова (в том числе между попытками входа), кончилась или
  // очередь неподтверждённых полна.
  bool propose_food(double x, double y);

  // Сколько раз клиент вошёл в мир новым участником (первый вход и каждый новый после истёкшего
  // окна удержания).
  [[nodiscard]] uint32_t sessions() const noexcept;
  // Сколько раз клиент вернулся тем же участником (продолжение после обрыва).
  [[nodiscard]] uint32_t resumes() const noexcept;
  // Logical peer текущего входа; 0 — не вошёл.
  [[nodiscard]] uint64_t peer() const noexcept;
  // Мир сессии; false, пока checkpoint не загружен.
  [[nodiscard]] bool layout(session_world& out) const;
  // Мир проверенного тика; nullptr, пока checkpoint не загружен и между новыми входами (при
  // продолжении показывается мир последнего проверенного тика).
  [[nodiscard]] const actor_world_slice* world() const noexcept;
  // Последний проверенный тик любой из попыток.
  [[nodiscard]] uint64_t tick() const noexcept;
  // Сессия кончилась окончательно: повторного входа не будет.
  [[nodiscard]] bool ended() const noexcept;
  // Сводка для интерфейса.
  [[nodiscard]] net_status status() const;

private:
  struct state;
  explicit net_client(std::unique_ptr<state> value) noexcept;
  std::unique_ptr<state> state_;
};

} // namespace frontier_online::core

#endif
