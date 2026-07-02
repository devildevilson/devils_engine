#ifndef TILE_FRONTIER_CORE_ACTOR_SIMULATION_H
#define TILE_FRONTIER_CORE_ACTOR_SIMULATION_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include <devils_engine/aesthetics/world.h>
#include <devils_engine/aesthetics/sink.h>   // serial::sink_policy/seal/unseal — save/load слайса
#include <devils_engine/act/registry.h>     // act::registry + function<RetT>
#include <devils_engine/act/intent.h>       // act::intent — обобщённый буфер интентов
#include <devils_engine/acumen/system.h>    // acumen::system — GOAP над act::registry
#include <devils_engine/mood/system.h>      // mood::system — FSM-исполнитель (состояние/анимация/звук)
#include <devils_engine/utils/kd_tree.h>    // utils::kd_tree — пространственный акселератор sense

#include "draw_intent.h"
#include "tile_map.h"

namespace devils_engine { namespace thread { class atomic_pool; } } // MT-пул для cognition

namespace tile_frontier {
namespace core {

// First lightweight actor slice: tiny deterministic brains write move intents,
// then a stable apply phase mutates aesthetics components.
struct actor_position {
  glm::vec2 value{0.0f, 0.0f};
};

struct actor_velocity {
  glm::vec2 value{0.0f, 0.0f};
};

struct actor_brain {
  uint32_t seed = 0;
  uint32_t phase = 0;
  float speed = 1.0f;
};

struct actor_visual {
  uint32_t texture = 0;
  instance_layout::rgba8_color color{};
  float size = 1.0f;
};

// Payload точки в kD-дереве восприятия: размер (для фильтра крупнее/мельче) и ПОЛНЫЙ
// entityid цели (нужен, чтобы схватить добычу при поедании; самоисключение — по индексу
// из id). Позиция лежит в самом узле дерева (node.pos).
struct perception_target {
  float size = 0.0f;
  devils_engine::aesthetics::entityid_t id = devils_engine::aesthetics::invalid_entityid;
};

// Восприятие: результат слоя поиска цели (sense-фаза). Хранит позицию ближайшего
// БО́ЛЬШЕГО актора (угроза, от него бежим) и ближайшего МЕНЬШЕГО (добыча, за ней
// гонимся) + ПОЛНЫЙ id добычи (для хвата при поедании). Предикаты/эффекты читают O(1).
struct actor_perception {
  glm::vec2 threat_pos{0.0f, 0.0f}; // ближайший актор крупнее нас
  glm::vec2 prey_pos{0.0f, 0.0f};   // ближайший актор мельче нас
  devils_engine::aesthetics::entityid_t prey_id = devils_engine::aesthetics::invalid_entityid;
  bool has_threat = false;
  bool has_prey = false;
};

// Состояние планировщика ИИ на акторе: тик последнего решения. «Давность» (tick -
// last_think) = приоритет на повторное обдумывание (дольше ждал — раньше выберут). Между
// решениями актор коастит на последней скорости. Сюда же позже ляжет LOD/таймаут/dirty.
struct actor_cognition {
  uint64_t last_think = 0;
};

// Мотивации актора (drives) — РЕПЛИЦИРУЕМОЕ состояние (тикает само, не выводимо из других
// компонентов за тик ⇒ кандидат на запись в save, в отличие от perception/плана). 0..1.
// hunger растёт со временем; boredom растёт пока актор стоит (думает), падает в движении.
// Предикаты GOAP (is_hungry/is_bored) читают пороги, эффекты их двигают на apply-фазе.
struct actor_drives {
  float hunger = 0.0f;
  float boredom = 0.0f;
};

// Текущее состояние FSM-исполнителя (mood) — хеш имени состояния (think/wander/seek_food/
// chase/flee/…). РЕПЛИЦИРУЕМОЕ состояние. Выбирает анимацию в рендере (state → freq/amp
// синусоиды) и служит точкой входа для on_entry-эффектов (звук — фаза D, поедание — фаза C).
// Хранится как uint64_t (= utils::id), чтобы не тянуть string_id в этот заголовок.
struct actor_state {
  uint64_t state = 0;
};

// Хищник в процессе поедания: КОГО ест (полный id жертвы) и ДО какого тика. Пока компонент
// есть — актор «закоммичен» (cognition его пропускает, скорость 0) → это НАСТОЯЩИЙ источник
// коммита (ретайрит commit_ticks для едящих). Снимается в resolve_eating по истечении.
struct actor_eating {
  devils_engine::aesthetics::entityid_t target = devils_engine::aesthetics::invalid_entityid;
  uint64_t until_tick = 0;
};

// Жертва, которую схватили: КТО ест. Пока есть — актор заморожен и пропущен cognition'ом,
// из дерева восприятия исключён (никто другой её не таргетит), и будет удалён по завершении.
struct actor_grabbed {
  devils_engine::aesthetics::entityid_t by = devils_engine::aesthetics::invalid_entityid;
};

// Еда-предмет: статичная маленькая сущность. Тег. В дереве восприятия она — «добыча» (меньше
// любого актора) и потребляется ТЕМ ЖЕ хэндшейком поедания (надёжно — еда не убегает).
// Респавнится до целевого числа (см. maintain_food) → не даёт популяции выесть саму себя.
struct food_item {
  float nutrition = 1.0f; // задел: сейчас поедание просто обнуляет голод
};

// Препятствие: статичный диск. Из восприятия ИСКЛЮЧЕНО (не добыча/не угроза); столкновение
// разрешается позиционно в фазе интеграции. Рисуется (есть position+visual).
struct obstacle {
  float radius = 1.0f;
};

// GPU instance for actor draw group. Layout: "v2ui1c4v1".
struct actor_instance {
  glm::vec2 pos;
  uint32_t texture;
  instance_layout::rgba8_color color;
  float size;
};

struct actor_metrics {
  uint32_t actors = 0;
  uint32_t intents = 0;
  uint32_t instances = 0;
  uint64_t ticks = 0;
};

// Запрос на звук, порождённый симуляцией (вход в состояние FSM). ЭФЕМЕРНЫЙ side-output: не
// реплицируемое состояние (sim-звуки переэмитятся при реплее интентов). name — хеш имени
// предзагруженного звука в звуковом акторе; pos — мировая позиция (для куллинга по слушателю).
// Собирается в apply, отдаётся наружу через sound_events(); презентационный мост (simulation.cpp)
// решает, что реально проиграть (куллинг по близости к камере + кап) и шлёт command_sound_play.
struct sound_emit {
  uint64_t name = 0;
  glm::vec2 pos{0.0f, 0.0f};
};

class actor_batch {
public:
  instance_layout::match_result bind(const std::string_view& layout = "v2ui1c4v1") {
    return intent_.bind(layout);
  }
  bool valid() const noexcept { return intent_.valid(); }

  // tick нужен для анимации (синусоида масштаба по текущему состоянию FSM, выводимая, не хранимая).
  void build(const devils_engine::aesthetics::world& world, uint64_t tick);

  std::span<const actor_instance> instances() const noexcept { return instances_; }
  std::span<const uint32_t> ids() const noexcept { return ids_; }
  uint32_t count() const noexcept { return uint32_t(instances_.size()); }
  static constexpr uint32_t stride() noexcept { return draw_intent<actor_instance>::stride(); }

  std::size_t blit(const std::span<uint8_t>& dst) const {
    return intent_.blit(std::span<const actor_instance>(instances_), dst);
  }

private:
  draw_intent<actor_instance> intent_;
  std::vector<actor_instance> instances_;
  std::vector<uint32_t> ids_;
};

class actor_world_slice {
public:
  void init(uint32_t count, glm::vec2 min_bound, glm::vec2 max_bound, uint32_t texture_count);
  actor_metrics update(float dt_seconds, actor_batch& batch, devils_engine::thread::atomic_pool& pool);

  devils_engine::aesthetics::world& ecs() noexcept { return world_; }
  const devils_engine::aesthetics::world& ecs() const noexcept { return world_; }

  // ── save/load полного состояния слайса (мир + не-ECS скаляры) ──
  // Слоистая схема сериализатора: dump_world кладёт компоненты, следом свой дампер кладёт
  // реплицируемые скаляры (tick/seq/config), seal заворачивает готовый payload в пакет.
  // save: слайс -> пакет (пригоден для диска/сети — политика решает компрессию/скриншот).
  std::vector<uint8_t> save(const devils_engine::aesthetics::serial::sink_policy& policy =
                              devils_engine::aesthetics::serial::disk_policy) const;
  // load: пакет -> ЧИСТЫЙ слайс (пересобирает registry/GOAP/FSM, грузит мир и скаляры,
  // перестраивает кэш препятствий). false при битом пакете/несовпадении схемы. Кэши/скретч
  // MT перестраиваются лениво в update; obstacles_ восстанавливается здесь из компонентов.
  bool load(std::span<const uint8_t> packet);

  // sim-звуки этого тика (вход в состояние FSM). Презентационный мост дренажит после update().
  std::span<const sound_emit> sound_events() const noexcept { return sound_emits_; }

private:
  // Регистрирует нативные геймплейные функции (предикаты-метрики + эффекты-действия)
  // в act::registry и собирает GOAP-систему acumen (резолв по имени — одноразовый).
  void setup_brain_registry();
  // Строит kD-дерево восприятия над ВСЕМИ акторами (позиции меняются каждый тик).
  void build_sense_tree();
  // Планировщик когниции: выбирает «созревших» акторов в пределах бюджета (приоритет —
  // давность решения), и только им обновляет восприятие + гоняет GOAP → intent. Остальные
  // коастят на прошлом решении. Тяжёлый перебор отобранных раскидан по потокам пула.
  void cognition(uint64_t tick, devils_engine::thread::atomic_pool& pool);
  // Восприятие (kD-запрос) + GOAP для ОДНОГО актора, в свой scratch/cache/буфер (на поток).
  void decide_actor(devils_engine::aesthetics::entityid_t id, uint64_t tick,
                    devils_engine::astar<devils_engine::acumen::astar_data>::container& scratch,
                    devils_engine::acumen::solution_cache& cache,
                    std::vector<devils_engine::act::intent>& out);
  void apply(float dt_seconds);
  // Завершает поедание у хищников, чей срок истёк: сбрасывает голод, снимает actor_eating,
  // удаляет съеденную жертву из мира (kill-list, удаление ПОСЛЕ обхода). Зовётся после apply.
  void resolve_eating(uint64_t tick);
  // Допополняет еду до food_target_ (детерминированно по тику+счётчику). Зовётся раз за тик.
  void maintain_food();
  // Спавнит одну еду-сущность в случайной (детерминированной) точке в пределах bounds.
  void spawn_food();
  // Перестраивает плоский кэш obstacles_ из компонентов мира (obstacle+position). Зовётся
  // после load: кэш выводим из мира, поэтому в снапшот не пишется. Порядок = dense-порядок
  // (= порядок id), совпадает с исходным спавном ⇒ детерминизм коллизии сохраняется.
  void rebuild_obstacle_cache();

  devils_engine::aesthetics::world world_;
  devils_engine::act::registry registry_;        // общий реестр геймплейных функций (см. libs/act)
  std::optional<devils_engine::acumen::system> goap_; // GOAP-арбитр: выбирает действие по приоритету
  std::optional<devils_engine::mood::system> fsm_;    // mood-исполнитель: действие→состояние→анимация/звук
  // kD-дерево слоя восприятия: перестраивается раз за тик, отвечает на «ближайший
  // крупнее/мельче в радиусе» с прунингом. Арена реюзится. Читается воркерами конкурентно.
  devils_engine::utils::kd_tree<perception_target> sense_tree_;
  std::vector<devils_engine::act::intent> intents_;   // обобщённый буфер интентов (sort by actor id)
  std::vector<sound_emit> sound_emits_;               // sim-звуки тика (вход в состояние FSM)

  // ── per-thread scratch для MT-cognition (индекс = pool.thread_index: 0=вызывающий, 1..=воркеры) ──
  // A*-контейнер на поток: find_solution чистит узлы решения в пул → переиспользуем без аллокаций.
  std::vector<devils_engine::astar<devils_engine::acumen::astar_data>::container> plan_containers_;
  // мемоизация GOAP на поток (без шаринга — крошечное пространство состояний, дупликация копеечная).
  std::vector<devils_engine::acumen::solution_cache> plan_caches_;
  // выходные буферы интентов на поток → конкатенируются в intents_ и сортируются по id.
  std::vector<std::vector<devils_engine::act::intent>> intent_buffers_;

  // ── планировщик когниции ──
  // окно коммита: актор держится своего решения K тиков (стенд-ин для длительности
  // действия/анимации) и переобдумывает не чаще. Спрос ≈ N/commit, лаг ≈ commit (~60мс при 3).
  uint32_t commit_ticks_ = 3;
  // бюджет: потолок обдумываний/тик (спайк-сейфти). ДОЛЖЕН быть ≥ спроса (N/commit), иначе
  // узким местом станет бюджет, а не коммит, и лаг вернётся к N/budget. count-budget → детерм.
  uint32_t think_budget_ = 2048;
  struct think_request { uint64_t overdue; devils_engine::aesthetics::entityid_t actor; };
  std::vector<think_request> due_; // переиспользуемый скретч отбора

  uint64_t tick_ = 0;

  // ── еда и препятствия (C2) ──
  glm::vec2 spawn_min_{0.0f, 0.0f};   // границы спавна (для респавна еды), заданы в init
  glm::vec2 spawn_max_{0.0f, 0.0f};
  uint32_t  food_target_ = 0;         // целевое число еды на карте (поддерживается респавном)
  uint64_t  food_spawn_seq_ = 0;      // счётчик спавнов — детерминированный поток позиций еды
  uint32_t  texture_count_ = 1;       // запомненное число текстур (для визуала еды/спавна)
  struct obstacle_disc { glm::vec2 pos; float radius; }; // плоский кэш препятствий для коллизии
  std::vector<obstacle_disc> obstacles_;
};

} // namespace core
} // namespace tile_frontier

#endif
