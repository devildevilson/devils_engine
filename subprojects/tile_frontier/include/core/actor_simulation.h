#ifndef TILE_FRONTIER_CORE_ACTOR_SIMULATION_H
#define TILE_FRONTIER_CORE_ACTOR_SIMULATION_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <devils_engine/act/interaction.h> // act::interaction — дескриптор арбитража эффекта-взаимодействия
#include <devils_engine/act/registry.h>    // act::registry + function<RetT>
#include <devils_engine/catalogue/call_log.h> // catalogue::call_log — контейнер отложенных вызовов (record/dispatch)
#include <devils_engine/acumen/execution_scratch.h>
#include <devils_engine/acumen/registry.h>
#include <devils_engine/acumen/system.h>   // acumen::system — GOAP над act::registry
#include <devils_engine/aesthetics/sink.h>           // serial::sink_policy/seal/unseal — save/load слайса
#include <devils_engine/aesthetics/world.h>
#include <devils_engine/mood/registry.h>
#include <devils_engine/mood/system.h>            // mood::system — FSM-исполнитель (состояние/анимация/звук)
#include <devils_engine/prefab/prefab_registry.h> // prefab::prefab_registry — рецепт сборки энтити (spawn_at)
#include <devils_engine/utils/kd_tree.h>          // utils::kd_tree — пространственный акселератор sense
#include <glm/glm.hpp>

#include "draw_intent.h"
#include "spawn_scope.h" // spawn_sink — слайс = мутабельная способность спавна для ds-натива spawn_at
#include "tile_map.h"

namespace devils_engine {
namespace thread {
class atomic_pool;
}
} // namespace devils_engine
namespace devils_engine {
namespace catalogue {
class statistics_store;
}
} // namespace devils_engine
namespace devils_script {
struct container;
} // namespace devils_script

namespace tile_frontier {
namespace core {
struct goap_config;
}
} // namespace tile_frontier

namespace tile_frontier {
namespace core {

// Статистика времени фаз апдейта актора (catalogue). Актор-сим и UI (visage) живут в ОДНОМ
// потоке (оба зовутся из simulation::update), поэтому UI читает её напрямую — без broker.
const devils_engine::catalogue::statistics_store& actor_perf_statistics() noexcept;

// Actor gameplay slice: deterministic MT cognition records catalogue effects, then explicit
// collect/elect commit lanes mutate aesthetics components before integration.
struct actor_position {
  glm::vec2 value{0.0f, 0.0f};
};

// Аргументы спавна префаба (per-instance, НЕ из конфига): точка в мире + детерминированное зерно и
// индекс/число текстур. on_construct-хук префаба лепит из них DERIVED-компоненты (food — позиция+визуал;
// actor — seed-производные brain/visual/stats). Еда использует только pos (остальное дефолт).
struct spawn_args {
  glm::vec2 pos{0.0f, 0.0f};
  uint32_t seed = 0;      // зерно per-instance разброса (actor: brain/stats/size)
  uint32_t index = 0;     // порядковый номер (actor: палитра цвета + слот текстуры)
  uint32_t tex_count = 1; // число текстур (actor: (index+1) % tex_count)
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

// Характеристики/мотивации актора — generic-компонент (пункт (д) engine-usage-model): плоский POD
// чисел. ОБЪЕДИНЯЕТ бывшие drives (hunger/boredom) и характеристики (strength) — демонстрация
// шаблонного механизма на реальных полях. ds-аксессоры чтения (`stats.hunger`) и прибавления
// (`add_hunger`, логируется в catalogue) АВТОГЕНЕРИРУЮТСЯ рефлексией (register_stat_accessors), вместо
// ручных scope_*-функций. РЕПЛИЦИРУЕМОЕ (тикает само, плоский POD ⇒ идеально сериализуется): hunger
// растёт со временем; boredom растёт пока актор стоит, падает в движении. Метаинформация (что
// положительно, для UI) — позже.
struct stats {
  float hunger = 0.0f;
  float boredom = 0.0f;
  int64_t strength = 0;
};

// Тюнинг-параметры актора из конфига (prefab/actor.tavl → data-компонент). Задают РАЗБРОС per-instance
// величин, которые on_construct считает из зерна: скорость (base + u·var), стартовый голод (u·scale),
// сила (hash % mod). Дефолты = историческим хардкод-константам (фолбэк тестов/резюме). НЕ сериализуется
// (uniform, потребляется только в on_construct на спавне; load не спавнит — восстанавливает компоненты).
struct actor_tuning {
  float speed_base = 0.65f;
  float speed_var = 1.35f;
  float hunger_scale = 0.4f;
  int64_t strength_mod = 11;
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
  bool valid() const noexcept {
    return intent_.valid();
  }

  // tick нужен для анимации (синусоида масштаба по текущему состоянию FSM, выводимая, не хранимая).
  void build(const devils_engine::aesthetics::world& world, uint64_t tick);

  std::span<const actor_instance> instances() const noexcept {
    return instances_;
  }
  std::span<const uint32_t> ids() const noexcept {
    return ids_;
  }
  uint32_t count() const noexcept {
    return uint32_t(instances_.size());
  }
  static constexpr uint32_t stride() noexcept {
    return draw_intent<actor_instance>::stride();
  }

  std::size_t blit(const std::span<uint8_t>& dst) const {
    return intent_.blit(std::span<const actor_instance>(instances_), dst);
  }

private:
  draw_intent<actor_instance> intent_;
  std::vector<actor_instance> instances_;
  std::vector<uint32_t> ids_;
};

// Проектные описания «мозга» актора, загруженные из tavl (заимствуются — владелец реестр ассетов).
// Каждое поле опционально: nullptr ⇒ нативный/хардкод фолбэк (тесты/резюме без ассетов). Растёт по
// мере переезда gameplay-данных в конфиг (GOAP-метрики/действия — следующими).
// Определение префаба из конфига: логическое имя + сырой tavl-текст. Слайс регистрирует C++-специи
// компонентов и скармливает текст в prefab_registry.add_prefab. Потребляется ЦЕЛИКОМ в init (текст
// копируется в реестр), поэтому вектор может быть временным у вызывающего.
struct prefab_def {
  std::string name;
  std::string text;
};

struct brain_config {
  const devils_script::container* is_hungry_program = nullptr; // скрипт-предикат "actor.is_hungry"
  const std::vector<std::string>* fsm_transitions = nullptr;   // строки переходов mood FSM
  std::shared_ptr<const goap_config> goap;                     // flattened GOAP config; owns script containers
  const std::vector<prefab_def>* prefabs = nullptr;            // префабы из prefab/*.tavl (иначе хардкод food)
};

// Проектные map-фазы актора на общем примитиве aesthetics::template_system_mt (параллельный обход
// запроса, виртуальный process). Определения в .cpp — здесь только forward-decl под unique_ptr-члены.
struct integration_system; // движение позиции по скорости + расталкивание препятствиями
struct drives_system;      // пассивная динамика мотиваций (голод/скука)
struct cognition_system;   // think-фаза: worklist_system над «созревшими», record'ит effects
struct deferred_effects;   // catalogue collect/elect executors (определены рядом с gameplay functions)

class actor_world_slice : public spawn_sink {
  friend struct cognition_system; // worklist_system::process зовёт приватный decide_actor
public:
  // Плоский кэш препятствия для коллизии (публичен — читается integration_system при обходе).
  struct obstacle_disc {
    glm::vec2 pos;
    float radius;
  };

  actor_world_slice() noexcept;
  ~actor_world_slice(); // out-of-line: unique_ptr на неполные *_system (pimpl-идиома)

  void init(uint32_t count, glm::vec2 min_bound, glm::vec2 max_bound, uint32_t texture_count,
            const brain_config& brains = {});

  // spawn_sink: спавн префаба по имени в точке (для ds-натива spawn_at). Тот же путь, что spawn_food —
  // prefab_.spawn(name, world, {pos}). Возвращает id новой сущности.
  devils_engine::aesthetics::entityid_t spawn_prefab(std::string_view name, glm::vec2 pos) override;
  actor_metrics update(float dt_seconds, actor_batch& batch, devils_engine::thread::atomic_pool& pool);

  devils_engine::aesthetics::world& ecs() noexcept {
    return world_;
  }
  const devils_engine::aesthetics::world& ecs() const noexcept {
    return world_;
  }

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
  std::span<const sound_emit> sound_events() const noexcept {
    return sound_emits_;
  }

private:
  // Регистрирует нативные геймплейные функции (предикаты-метрики + эффекты-действия)
  // в act::registry и собирает GOAP-систему acumen (резолв по имени — одноразовый).
  void setup_brain_registry();
  // Собирает acumen::system из tavl-конфига: метрики (порядок=биты), действия/цели по ключам
  // (префикс !/not = инвертированный бит; символические биты типа "resolved" индексируются после метрик).
  void build_goap_from_config(const goap_config& cfg);
  // Строит kD-дерево восприятия над ВСЕМИ акторами (позиции меняются каждый тик).
  void build_sense_tree();
  // «Созрел» = ещё не думал (last_think==0) ИЛИ истёк commit_ticks_. Отбор в enumerate.
  bool matured(const uint64_t last_think, const uint64_t tick) const noexcept {
    return last_think == 0 || (tick - last_think) >= commit_ticks_;
  }
  // Планировщик когниции: выбирает «созревших» акторов в пределах бюджета (приоритет —
  // давность решения), и только им обновляет восприятие + гоняет GOAP → deferred effect. Остальные
  // коастят на прошлом решении. Тяжёлый перебор отобранных раскидан по потокам пула через
  // cognition_system (worklist_system над отобранными).
  void cognition(uint64_t tick, devils_engine::thread::atomic_pool& pool);
  // Восприятие (kD-запрос) + GOAP для ОДНОГО актора, в свой scratch/cache/буфер (на поток). Зовётся
  // из cognition_system::process на своей scratch-полосе (оттого friend).
  void decide_actor(devils_engine::aesthetics::entityid_t id, uint64_t tick,
                    devils_engine::acumen::execution_scratch& scratch);
  void apply(devils_engine::thread::atomic_pool& pool);
  // Интеграция позиций (движение по скорости + расталкивание препятствиями) — параллельный map-фаза
  // на integration_system. Ленивое создание системы на первом апдейте (пул известен только тут).
  void integrate(float dt_seconds, devils_engine::thread::atomic_pool& pool);
  // Пассивная динамика мотиваций (голод копится, скука от простоя) — map-фаза на drives_system.
  void update_drives(float dt_seconds, devils_engine::thread::atomic_pool& pool);
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
  devils_engine::act::registry registry_; // общий реестр геймплейных функций (см. libs/act)
  devils_engine::acumen::registry goap_registry_;
  devils_engine::mood::registry fsm_registry_;
  const devils_engine::acumen::system* goap_ = nullptr; // выбранный профиль слайса (per-entity ref позже)
  const devils_engine::mood::system* fsm_ = nullptr;    // FSM независима от GOAP; для actor-типа фиксирована
  // kD-дерево слоя восприятия: перестраивается раз за тик, отвечает на «ближайший
  // крупнее/мельче в радиусе» с прунингом. Арена реюзится. Читается воркерами конкурентно.
  devils_engine::utils::kd_tree<perception_target> sense_tree_;
  // Детерминированный журнал ВЫБРАННОГО action на source (для FSM/звука после effect commit).
  // Тела эффектов здесь больше не хранятся/не вызываются: cognition зовёт act::effect, чья
  // fn_deferred_ptr-обёртка пишет typed args в deferred_->local/eat.
  devils_engine::catalogue::call_log calls_;
  // Typed catalogue executors: local collect (parallel key-groups) + eat elect (ST structural).
  // Pimpl оставляет strategy-типы рядом с нативными функциями в .cpp.
  std::unique_ptr<deferred_effects> deferred_;
  std::vector<sound_emit> sound_emits_; // sim-звуки тика (вход в состояние FSM)

  // Политика think-фазы (были поля cognition_scheduler; сериализуются как commit_ticks/think_budget).
  // commit_ticks: сущность держится решения N тиков (спрос ≈ N_actors/commit_ticks). think_budget:
  // потолок обдумываний за тик (спайк-сейфти, count-budget ⇒ детерминированно).
  size_t commit_ticks_ = 3;
  size_t think_budget_ = 2048;
  // Map-фазы на общих примитивах aesthetics (владеет слайс; лениво создаются на 1-м апдейте, когда
  // известен пул). cognition_system = worklist_system над «созревшими» (per-thread scratch = A*+cache
  // +ds VM), append-ит typed calls в dense deferred journal. Неполные типы ⇒ out-of-line дтор слайса.
  std::unique_ptr<integration_system> integration_sys_;
  std::unique_ptr<drives_system> drives_sys_;
  std::unique_ptr<cognition_system> cognition_sys_;
  // build_sense_tree как лямбда-система (aesthetics::make_map_system): reduce query<pos,vis> → kd-дерево.
  // Базой basic_system (комп-типы скрыты); лениво создаётся, сбрасывается на init/load (держит query на world).
  std::unique_ptr<devils_engine::aesthetics::basic_system> sense_tree_sys_;
  // cognition-SELECT как лямбда-система: reduce view<actor_cognition> → кандидаты (созревшие, не занятые).
  struct think_candidate {
    uint64_t overdue;
    devils_engine::aesthetics::entityid_t entity;
  };
  std::unique_ptr<devils_engine::aesthetics::basic_system> select_sys_;
  std::vector<think_candidate> due_; // выход SELECT (переиспользуется/очищается за тик), вход budget_clamp
  // resolve_eating: скан-часть — лямбда-система (reduce view<actor_eating> в буферы); структурное удаление
  // (снять actor_eating, убить жертву) — ПОСЛЕ прохода в обёртке (нельзя мутировать пул при обходе).
  std::unique_ptr<devils_engine::aesthetics::basic_system> resolve_eating_sys_;
  std::vector<devils_engine::aesthetics::entityid_t> eat_finished_; // хищники с истёкшим поеданием (per-tick)
  std::vector<devils_engine::aesthetics::entityid_t> eat_kill_;     // съеденные жертвы на удаление (per-tick)
  // Проектные конфиги мозга (из tavl); поля nullptr ⇒ нативный/хардкод фолбэк. Задаются в init.
  brain_config brains_;

  // Реестр префабов слайса: рецепты сборки энтити из компонентов. Пока — «food» (data food_item +
  // derived visual/position через on_construct); spawn_at через prefab_.spawn(name, world, {pos}).
  // Регистрируется в setup_brain_registry (общая точка init/load). Растёт по мере переезда спавна в него.
  devils_engine::prefab::prefab_registry<spawn_args> prefab_;

  uint64_t tick_ = 0;

  // ── еда и препятствия (C2) ──
  glm::vec2 spawn_min_{0.0f, 0.0f}; // границы спавна (для респавна еды), заданы в init
  glm::vec2 spawn_max_{0.0f, 0.0f};
  uint32_t food_target_ = 0;    // целевое число еды на карте (поддерживается респавном)
  uint64_t food_spawn_seq_ = 0; // счётчик спавнов — детерминированный поток позиций еды
  uint32_t texture_count_ = 1;  // запомненное число текстур (для визуала еды/спавна)
  std::vector<obstacle_disc> obstacles_; // плоский кэш препятствий для коллизии (тип — публичный выше)
};

} // namespace core
} // namespace tile_frontier

#endif
