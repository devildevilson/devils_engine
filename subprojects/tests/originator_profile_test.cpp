#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "devils_engine/originator/execution_profile.h"

using namespace devils_engine;

// УЧЁТ ИСПОЛНЕНИЯ: проверяется СВОДКА, а не замер. Замер — это две отметки времени, у него нет
// поведения; поведение есть у правил, по которым из записей получаются доли и прогоны, и каждое из
// этих правил меняет вывод о том, что переносить.
//
// Записи здесь собираются руками, без буферов и без пайплайна: сводка обязана быть функцией ТОЛЬКО
// от записей, иначе её нельзя ни перепроверить, ни пересчитать другим разрезом.

namespace {
originator::profile_record made(const std::string& step,
                                const std::string& label,
                                const uint64_t microseconds,
                                const originator::device_fitness::values fitness,
                                const std::vector<std::pair<std::string, size_t>>& fields = {},
                                const size_t queue_size = 0) {
  originator::profile_record record;
  record.step = step;
  record.label = label;
  record.microseconds = microseconds;
  record.fitness = fitness;
  record.fields = fields;
  for (const auto& [name, bytes] : fields) {
    record.bytes += bytes;
  }
  record.queue_size = queue_size;
  return record;
}
} // namespace

TEST_CASE("originator profile splits the wall clock and names why work stays on the cpu") {
  originator::execution_profile profile;

  profile.begin_step("terrain");
  profile.add(made("terrain", "remap", 100, originator::device_fitness::ready));
  profile.add(made("terrain", "noise_grid", 200, originator::device_fitness::no_body));
  profile.end_step(400);

  profile.begin_step("regions");
  profile.add(made("regions", "graph_flood", 700, originator::device_fitness::refused));
  profile.add(made("regions", "biome_rule", 300, originator::device_fitness::narrow));
  profile.end_step(1100);

  const auto summary = originator::summarize(profile);

  CHECK(summary.total_microseconds == 1500);
  CHECK(summary.calls_microseconds == 1300);
  // Композиция — это РАЗНИЦА: время шага, не попавшее ни в один вызов. Считать её суммой чего-либо
  // нельзя, потому что она и есть то, что ни один вызов на себя не записал.
  CHECK(summary.script_microseconds == 200);

  CHECK(summary.by_fitness[originator::device_fitness::ready] == 100);
  CHECK(summary.by_fitness[originator::device_fitness::no_body] == 200);
  CHECK(summary.by_fitness[originator::device_fitness::narrow] == 300);
  CHECK(summary.by_fitness[originator::device_fitness::refused] == 700);
}

TEST_CASE("originator profile counts runs of adjacent ready calls, not ready calls") {
  originator::execution_profile profile;

  // Годная работа, РАЗОРВАННАЯ вызовом без тела: соседями два годных вызова здесь не являются, и
  // переносить их поодиночке нельзя — измеренный порог переноса это два-три прохода.
  profile.begin_step("field");
  profile.add(made("field", "remap", 100, originator::device_fitness::ready));
  profile.add(made("field", "noise_at", 100, originator::device_fitness::no_body));
  profile.add(made("field", "blend", 100, originator::device_fitness::ready));
  profile.end_step(300);

  const auto summary = originator::summarize(profile);
  CHECK(summary.runs.size() == 2);
  CHECK(summary.longest_run == 1);
  CHECK(summary.payable_runs == 0);
  CHECK(summary.payable_microseconds == 0);

  // А ТЕПЕРЬ ГЛАВНОЕ ЧИСЛО ОХВАТА: дописанное тело не просто добавляет свою долю, оно СКЛЕИВАЕТ
  // соседей. Три вызова становятся одним прогоном из трёх проходов.
  CHECK(summary.potential_runs_count == 1);
  CHECK(summary.potential_longest_run == 3);
  CHECK(summary.potential_microseconds == 300);
}

TEST_CASE("originator profile breaks a run on a step boundary") {
  originator::execution_profile profile;

  profile.begin_step("first");
  profile.add(made("first", "remap", 100, originator::device_fitness::ready));
  profile.end_step(100);

  profile.begin_step("second");
  profile.add(made("second", "blend", 100, originator::device_fitness::ready));
  profile.end_step(100);

  // Между шагами лежит вся композиция тела и возврат в дирижёра, поэтому соседями по РАБОТЕ такие
  // вызовы не являются, даже когда стоят рядом в записях.
  const auto summary = originator::summarize(profile);
  CHECK(summary.runs.size() == 2);
  CHECK(summary.payable_runs == 0);
}

TEST_CASE("originator profile counts a queue by its own passes") {
  originator::execution_profile profile;

  // Объявленная очередь — УЖЕ цепочка: засчитывать её одним проходом значило бы занизить ровно то,
  // ради чего цепочки и объявляют.
  profile.begin_step("climate");
  profile.add(made("climate", "queue", 500, originator::device_fitness::ready, {}, 17));
  profile.end_step(500);

  const auto summary = originator::summarize(profile);
  REQUIRE(summary.runs.size() == 1);
  CHECK(summary.runs.front().passes == 17);
  CHECK(summary.runs.front().records == 1);
  CHECK(summary.payable_runs == 1);
}

TEST_CASE("originator profile measures what residency between calls would save") {
  originator::execution_profile profile;

  // Два соседних годных вызова над ОДНИМ полем плюс своё у каждого. Сегодня каждый вызов заливает и
  // скачивает своё, поэтому общее поле платится дважды; при общей сессии оно уехало бы один раз.
  const std::vector<std::pair<std::string, size_t>> first = {{"cells.height", 1000}, {"cells.a", 100}};
  const std::vector<std::pair<std::string, size_t>> second = {{"cells.height", 1000}, {"cells.b", 100}};

  profile.begin_step("field");
  profile.add(made("field", "remap", 100, originator::device_fitness::ready, first));
  profile.add(made("field", "blend", 100, originator::device_fitness::ready, second));
  profile.end_step(200);

  const auto summary = originator::summarize(profile);
  REQUIRE(summary.payable_runs == 1);
  CHECK(summary.payable_transfer_per_call == 2200);
  CHECK(summary.payable_transfer_shared == 1200);
}
