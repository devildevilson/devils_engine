#ifndef DEVILS_ENGINE_MOOD_RUNTIME_H
#define DEVILS_ENGINE_MOOD_RUNTIME_H

#include <cstdint>
#include <span>

#include "devils_engine/utils/string_id.h"   // utils::id / string_hash
#include "devils_engine/act/exec_context.h"  // act::exec_context
#include "system.h"

// runtime — СЛОЙ КОНВЕНЦИЙ над тупым хранилищем mood::system. Именно здесь живут стандартные
// имена (any_state — общее имя для любого состояния; idle — стандартное "нет события";
// on_entry/on_exit — стандартные события входа/выхода) и вся работа: поиск перехода с
// fallback'ом на any_state, проверка гвардов top-down, классификация результата, валидация
// графа. system про это НЕ знает — он только быстро отдаёт группу (state, event).
//
// Эффекты (process/on_exit/on_entry) тут НЕ исполняются: step лишь РЕШАЕТ переход и возвращает
// его. Выполнение эффектов — дело вызывающего на детерминированной apply-фазе (через intent,
// sort by entity_id), чтобы "думать в любом порядке, применять в фиксированном".

namespace devils_engine {
namespace mood {

// стандартные имена-конвенции как готовые хеши (считаются один раз при инициализации модуля).
namespace conv {
inline const utils::id any_state = utils::string_hash("any_state");
inline const utils::id idle      = utils::string_hash("idle");
inline const utils::id on_entry  = utils::string_hash("on_entry");
inline const utils::id on_exit   = utils::string_hash("on_exit");
}

enum class step_result : uint8_t {
  transitioned,  // нашёлся переход, его гварды прошли — out.next_state/out.taken валидны
  blocked,       // переходы по событию ЕСТЬ, но все отсеяны гвардами (нормальный геймплей)
  no_transition  // перехода по (state, event) нет даже через any_state (скорее опечатка/баг)
};

// поля отсортированы по убыванию размера/выравнивания, чтобы минимизировать паддинг
// (правило проекта). Иначе step_result (1 байт) первым давал бы 7 байт дыры + 4 хвостовых
// => 32 байта; так — 24 без внутренних дыр.
struct step_outcome {
  utils::id    next_state = utils::invalid_id;          // 8: валиден при transitioned (invalid_id => остаёмся)
  const system::transition* taken = nullptr;            // 8: какая строка сработала (для process/диагностики)
  uint16_t     candidates = 0;                          // 2: сколько кандидатов было у (state, event)
  uint16_t     blocked    = 0;                          // 2: сколько из них отсеяли гварды
  step_result  result = step_result::no_transition;     // 1: исход шага (см. step_result)
};
static_assert(sizeof(step_outcome) == 24, "step_outcome: проверь порядок полей (минимизация паддинга)");

// поиск группы переходов с fallback'ом: сначала конкретное состояние, потом any_state.
std::span<const system::transition> find_with_fallback(const system& sys, const utils::id state, const utils::id event);

// решить переход: top-down первый, чьи гварды все прошли. Эффекты НЕ исполняет.
step_outcome step(const system& sys, const utils::id state, const utils::id event, const act::exec_context& ctx);

// удобная обёртка по именам (хеширует).
step_outcome step(const system& sys, const std::string_view& state, const std::string_view& event, const act::exec_context& ctx);

// выполнить выбранный переход ЦЕЛИКОМ (apply-фаза, ctx.sink боевой, НЕ dry-run):
//   если переход меняет состояние (есть '= next'): on_exit старого → эффекты перехода → on_entry нового;
//   внутренний переход (без '='): только эффекты перехода (состояние не покидаем — exit/entry не зовём).
// on_exit/on_entry — это группы (state, on_exit|on_entry); из каждой берётся ПЕРВЫЙ с прошедшими
// гвардами (как обычная top-down проверка). Возвращает хеш нового состояния, или invalid_id если
// переход внутренний (вызывающий тогда оставляет cur_state как есть).
//
// Делитель ответственности: step() РЕШАЕТ (чистая проверка гвардов), apply_transition() МУТИРУЕТ.
// Типичный кадр в apply-фазе: обработать пришедшее событие, затем досчитать idle (completion-
// transitions) до стабильного состояния — гвард мог стать истинным уже в этом кадре — с капом
// итераций от зацикливания (idle A→B→A с вечно-истинными гвардами).
utils::id apply_transition(const system& sys, const utils::id cur_state, const system::transition& taken, const act::exec_context& ctx);

// Готовый «кадр apply-фазы» с КАПОМ (раньше цикл приходилось писать вручную у каждого вызывающего):
// обработать пришедшее событие (step→apply_transition при transitioned), затем досчитать idle
// (completion-transitions) до стабильного состояния, но не более max_idle_iters раз (защита от
// зацикливания idle A→B→A с вечно-истинными гвардами; останов также на внутреннем переходе и
// само-петле). ctx.sink должен быть боевым (не dry-run). Возвращает финальное состояние (хеш).
utils::id settle(const system& sys, const utils::id cur_state, const utils::id event, const act::exec_context& ctx, const uint32_t max_idle_iters = 8);

// валидация графа на загрузке: предупреждает (utils::warn) о тупиковых (next_state без исходящих
// переходов) и недостижимых (current_state, который никогда не является чьим-то next_state)
// состояниях, с fuzzy-подсказкой "did you mean" (мини-левенштейн). Конвенция any_state как
// wildcard учтена (он не считается недостижимым). Однократная диагностика, звать в debug/на загрузке.
void validate(const system& sys);

}
}

#endif
