#include "devils_engine/originator/execution_profile.h"

#include <algorithm>
#include <format>

#include "devils_engine/originator/computation_queue.h"
#include "devils_engine/originator/device_form.h"
#include "devils_engine/utils/process_memory.h"

// Реализация учёта: накопление записей и сводка по ним.
//
// СВОДКА НИЧЕГО НЕ ИЗМЕРЯЕТ — она только складывает уже измеренное. Это разделение существует затем,
// чтобы вопрос «какая доля уедет» можно было переспросить другим способом, не трогая замер: сами
// записи остаются доступными, и всякий новый разрез считается по ним, а не новым прогоном.
//
// ПРОГОНЫ СЧИТАЮТСЯ ПО ПОРЯДКУ ЗАПИСЕЙ, и порядок здесь настоящий: тело шага исполняется
// последовательно, а вызов возвращается только посчитавшись. Поэтому «подряд идущие» — это
// действительно соседи в потоке работы, а не просто соседи в списке.

namespace devils_engine {
namespace originator {

std::string_view to_string(const device_fitness::values value) noexcept {
  switch (value) {
    case device_fitness::ready: return "ready";
    case device_fitness::no_body: return "no_body";
    case device_fitness::narrow: return "narrow";
    case device_fitness::refused: return "refused";
    default: return "unknown";
  }
}

void execution_profile::clear() noexcept {
  records_.clear();
  steps_.clear();
}

void execution_profile::begin_step(const std::string_view& name) {
  profile_step step;
  step.name.assign(name);
  step.first_record = records_.size();
  steps_.push_back(std::move(step));
}

void execution_profile::end_step(const uint64_t microseconds) {
  if (steps_.empty()) {
    return;
  }
  auto& step = steps_.back();
  step.microseconds = microseconds;
  step.record_count = records_.size() - step.first_record;
}

void execution_profile::add(profile_record record) {
  records_.push_back(std::move(record));
}

const std::vector<profile_record>& execution_profile::records() const noexcept {
  return records_;
}

const std::vector<profile_step>& execution_profile::steps() const noexcept {
  return steps_;
}

bool device_representable(const std::span<const field_ref>& inputs,
                          const std::span<const field_ref>& outputs) noexcept {
  // Узкий род представление ИМЕЕТ: на устройстве поле живёт расширенным (`device_storage_base`).
  // Не представляется только то, у чего нет даже расширения.
  const auto representable = [](const std::span<const field_ref>& list) {
    for (const auto& binding : list) {
      if (!binding.valid()) continue;
      if (device_storage_base(binding.type().base) == field_base::count) return false;
    }
    return true;
  };
  return representable(inputs) && representable(outputs);
}

device_fitness::values fitness_of(const tool_description& tool,
                                  const std::span<const field_ref>& inputs,
                                  const std::span<const field_ref>& outputs) noexcept {
  // Порядок проверок значим и отвечает на вопрос «во что упирается ПЕРВЫМ»: апертура — свойство
  // алгоритма, её не переписать; род поля — решение конфига; тело — работа, которую можно сделать.
  if (!fits_in_queue(tool.shape) && !(tool.shape == aperture::scatter && tool.order_free_writes)) {
    return device_fitness::refused;
  }
  if (!device_representable(inputs, outputs)) {
    return device_fitness::narrow;
  }
  if (tool.device_body.empty()) {
    return device_fitness::no_body;
  }
  // ТЕЛО ЕСТЬ, НО НЕ ДЛЯ ЭТОГО РОДА: написанное против `float`, оно годится над узким целым (оно
  // точно во `float32`) и не годится над широким, пока инструмент не объявил себя кинд-агностичным.
  // Упирается такой вызов опять в РОД, поэтому и класс тот же.
  if (!tool.device_integer_ready) {
    const auto wide_integer = [](const std::span<const field_ref>& list) {
      for (const auto& binding : list) {
        if (!binding.valid()) continue;
        const auto base = binding.type().base;
        if (base != field_base::v && !exact_in_float(base)) return true;
      }
      return false;
    };
    if (wide_integer(inputs) || wide_integer(outputs)) {
      return device_fitness::narrow;
    }
  }
  return device_fitness::ready;
}

std::vector<std::pair<std::string, size_t>> touched_fields(const std::span<const field_ref>& inputs,
                                                           const std::span<const field_ref>& outputs) {
  std::vector<field_ref> counted;
  std::vector<std::pair<std::string, size_t>> result;

  const auto account = [&](const field_ref& binding) {
    if (!binding.valid()) return;
    for (const auto& known : counted) {
      if (known.same_field_as(binding)) return;
    }
    counted.push_back(binding);
    result.emplace_back(std::format("{}.{}", binding.buffer_name(), binding.field_name()),
                        binding.count() * binding.type().byte_size());
  };

  for (const auto& binding : inputs) account(binding);
  for (const auto& binding : outputs) account(binding);
  return result;
}

size_t touched_bytes(const std::span<const field_ref>& inputs, const std::span<const field_ref>& outputs) {
  size_t total = 0;
  for (const auto& [name, bytes] : touched_fields(inputs, outputs)) {
    total += bytes;
  }
  return total;
}

memory_summary summarize_memory(const execution_profile& profile, const size_t declared_buffers) {
  memory_summary summary;
  summary.declared_buffers = declared_buffers;
  // ПИК ПРОЦЕССА, а не текущее значение: временная таблица живёт миллисекунды и к концу шага уже
  // возвращена аллокатору, но пока она была, машина её держала.
  summary.peak_resident = utils::peak_resident_bytes();

  for (const auto& record : profile.records()) {
    if (!record.declared_footprint) {
      summary.undeclared_calls += 1;
      summary.undeclared_microseconds += record.microseconds;
      continue;
    }
    if (record.footprint <= summary.largest_temporary) continue;
    summary.largest_temporary = record.footprint;
    summary.largest_temporary_call = record.label;
    summary.largest_temporary_step = record.step;
  }

  return summary;
}

profile_summary summarize(const execution_profile& profile) {
  profile_summary summary;

  for (const auto& step : profile.steps()) {
    summary.total_microseconds += step.microseconds;
  }

  for (const auto& record : profile.records()) {
    summary.calls_microseconds += record.microseconds;
    summary.translation_microseconds += record.translation_microseconds;
    summary.by_fitness[record.fitness] += record.microseconds;
    summary.by_aperture[record.shape] += record.microseconds;
    summary.on_device_microseconds += record.on_device ? record.microseconds : 0;
    summary.calls += 1;
    summary.queues += size_t(record.queue_size != 0);
  }

  summary.script_microseconds = summary.total_microseconds > summary.calls_microseconds
                                  ? summary.total_microseconds - summary.calls_microseconds
                                  : 0;

  // Прогоны обрываются и сменой шага: между шагами лежит вся композиция тела, поэтому соседями по
  // работе такие вызовы не являются.
  const auto& records = profile.records();
  const auto collect = [&records](const device_fitness::values limit) {
    std::vector<profile_run> found;
    for (size_t i = 0; i < records.size();) {
      if (records[i].fitness > limit) {
        ++i;
        continue;
      }

      profile_run run;
      run.step = records[i].step;
      run.first_record = i;
      std::vector<std::string> resident;
      while (i < records.size() && records[i].fitness <= limit && records[i].step == run.step) {
        run.passes += records[i].queue_size != 0 ? records[i].queue_size : 1;
        run.records += 1;
        run.microseconds += records[i].microseconds;
        run.bytes = std::max(run.bytes, records[i].bytes);
        run.transfer_per_call += records[i].bytes;
        for (const auto& [name, bytes] : records[i].fields) {
          if (std::find(resident.begin(), resident.end(), name) != resident.end()) continue;
          resident.push_back(name);
          run.transfer_shared += bytes;
        }
        ++i;
      }
      found.push_back(std::move(run));
    }
    return found;
  };

  summary.potential_runs = collect(device_fitness::no_body);
  for (const auto& run : summary.potential_runs) {
    summary.potential_longest_run = std::max(summary.potential_longest_run, run.passes);
    if (run.passes < 2) continue;
    summary.potential_microseconds += run.microseconds;
    summary.potential_runs_count += 1;
  }

  for (size_t i = 0; i < records.size();) {
    if (records[i].fitness != device_fitness::ready) {
      ++i;
      continue;
    }

    profile_run run;
    run.step = records[i].step;
    run.first_record = i;
    std::vector<std::string> resident;
    while (i < records.size() && records[i].fitness == device_fitness::ready && records[i].step == run.step) {
      // Очередь считается СВОИМИ проходами: она уже цепочка, и засчитывать её одним проходом значило
      // бы занизить ровно то, ради чего цепочки объявляют.
      run.passes += records[i].queue_size != 0 ? records[i].queue_size : 1;
      run.records += 1;
      run.microseconds += records[i].microseconds;
      run.bytes = std::max(run.bytes, records[i].bytes);
      run.transfer_per_call += records[i].bytes;
      // Общая резидентность: поле, которое трогают несколько вызовов прогона, едет ОДИН раз.
      for (const auto& [name, bytes] : records[i].fields) {
        if (std::find(resident.begin(), resident.end(), name) != resident.end()) continue;
        resident.push_back(name);
        run.transfer_shared += bytes;
      }
      ++i;
    }

    summary.longest_run = std::max(summary.longest_run, run.passes);
    if (run.passes >= 2) {
      summary.payable_microseconds += run.microseconds;
      summary.payable_bytes += run.bytes;
      summary.payable_transfer_per_call += run.transfer_per_call;
      summary.payable_transfer_shared += run.transfer_shared;
      summary.payable_runs += 1;
      summary.split_runs += size_t(run.records >= 2);
    }
    summary.runs.push_back(std::move(run));
  }

  return summary;
}

namespace {
// Во что упирается очередь: классы её элементов перечисляются поимённо, потому что «годных 0» не
// говорит, дописывать тела или переобъявлять поля.
std::string format_queue_shape(const profile_record& record) {
  std::string text = std::format(", очередь из {}:", record.queue_size);
  for (size_t i = 0; i < device_fitness::count; ++i) {
    if (record.queue_fitness[i] == 0) continue;
    text.append(std::format(" {} {}", record.queue_fitness[i], to_string(device_fitness::values(i))));
  }
  return text;
}

double share(const uint64_t part, const uint64_t whole) noexcept {
  return whole == 0 ? 0.0 : 100.0 * double(part) / double(whole);
}

double milliseconds(const uint64_t value) noexcept {
  return double(value) / 1000.0;
}
} // namespace

std::string format_profile(const execution_profile& profile, const size_t top_calls,
                           const size_t declared_buffers) {
  const auto summary = summarize(profile);

  std::string text;
  text.append(std::format("генератор: {:.2f} мс всего, {} вызовов ({} очередей) в {} шагах\n",
                          milliseconds(summary.total_microseconds), summary.calls, summary.queues,
                          profile.steps().size()));
  text.append(std::format("  вызовы     {:9.2f} мс  {:5.1f}%\n", milliseconds(summary.calls_microseconds),
                          share(summary.calls_microseconds, summary.total_microseconds)));
  text.append(std::format("  композиция {:9.2f} мс  {:5.1f}%   (lua вне вызовов)\n",
                          milliseconds(summary.script_microseconds),
                          share(summary.script_microseconds, summary.total_microseconds)));

  text.append("\nГОДНОСТЬ К УСТРОЙСТВУ (доля стенных часов ГЕНЕРАТОРА):\n");
  static constexpr std::string_view fitness_notes[device_fitness::count] = {
    "апертура пускается, тело есть",
    "апертура пускается, тела нет — это работа «охват»",
    "род поля не подходит вызову: широкое целое под телом против float — это решение конфига",
    "апертура не переносится по построению",
  };
  for (size_t i = 0; i < device_fitness::count; ++i) {
    text.append(std::format("  {:8s} {:9.2f} мс  {:5.1f}%   {}\n", to_string(device_fitness::values(i)),
                            milliseconds(summary.by_fitness[i]), share(summary.by_fitness[i], summary.total_microseconds),
                            fitness_notes[i]));
  }
  if (summary.on_device_microseconds != 0) {
    text.append(std::format("  из них уже НА УСТРОЙСТВЕ: {:.2f} мс\n", milliseconds(summary.on_device_microseconds)));
  }

  text.append("\nПО АПЕРТУРАМ:\n");
  for (size_t i = 0; i < aperture::count; ++i) {
    if (summary.by_aperture[i] == 0) continue;
    text.append(std::format("  {:11s} {:9.2f} мс  {:5.1f}%\n", to_string(aperture::values(i)),
                            milliseconds(summary.by_aperture[i]),
                            share(summary.by_aperture[i], summary.total_microseconds)));
  }

  // ПРОГОНЫ ДЛИНОЙ ОТ ДВУХ — единственные кандидаты на перенос: порог измерен и равен двум-трём
  // проходам, поэтому одиночный годный вызов сюда не входит, даже если он дорогой.
  text.append(std::format("\nПРОГОНЫ ГОДНЫХ ВЫЗОВОВ: {} всего, самый длинный {}, от двух вызовов — {}\n",
                          summary.runs.size(), summary.longest_run, summary.payable_runs));
  text.append(std::format("  в прогонах >= 2: {:.2f} мс ({:.1f}% часов генератора)\n",
                          milliseconds(summary.payable_microseconds),
                          share(summary.payable_microseconds, summary.total_microseconds)));
  // ДВЕ ПЕРЕДАЧИ — это и есть цена того, что резидентность живёт ровно одну очередь: сегодня соседние
  // вызовы платят за одно и то же поле по разу каждый, при общей сессии поле уехало бы однажды.
  text.append(std::format("  передача: {:.2f} МБ по вызову (как сегодня) против {:.2f} МБ при общей "
                          "резидентности — экономия {:.1f}%\n",
                          double(summary.payable_transfer_per_call) / (1024.0 * 1024.0),
                          double(summary.payable_transfer_shared) / (1024.0 * 1024.0),
                          summary.payable_transfer_per_call == 0
                            ? 0.0
                            : 100.0 * double(summary.payable_transfer_per_call - summary.payable_transfer_shared) /
                                double(summary.payable_transfer_per_call)));
  if (summary.split_runs != 0) {
    // РЕЗИДЕНТНОСТЬ ВНУТРИ ОЧЕРЕДИ УЖЕ ЕСТЬ: поле, которое пишет один элемент очереди и читает
    // следующий, границы не пересекает. Прогон, объявленный несколькими вызовами, платит за передачу
    // зря — и это правка КОНФИГА (объявить их одной очередью), а не работа библиотеки.
    text.append(std::format("  из них {} объявлены НЕСКОЛЬКИМИ вызовами там, где могли быть одной "
                            "очередью — это и есть разница выше\n",
                            summary.split_runs));
  }
  // ЧТО ДАДУТ ТЕЛА, в правильной валюте: не своя доля, а СКЛЕЙКА соседей. Отсутствующее тело рвёт
  // цепочку, и годный вызов по обе стороны от него остаётся одиночкой, которая кругу передачи
  // проигрывает.
  text.append(std::format("  если дописать недостающие тела: {} прогонов >= 2, самый длинный {}, "
                          "{:.2f} мс ({:.1f}% часов)\n",
                          summary.potential_runs_count, summary.potential_longest_run,
                          milliseconds(summary.potential_microseconds),
                          share(summary.potential_microseconds, summary.total_microseconds)));

  auto ranked = summary.runs;
  std::sort(ranked.begin(), ranked.end(), [](const profile_run& a, const profile_run& b) {
    return a.microseconds > b.microseconds;
  });
  for (size_t i = 0; i < ranked.size() && i < 5; ++i) {
    if (ranked[i].passes < 2) continue;
    const auto& run = ranked[i];
    text.append(std::format("  шаг '{}': {} проходов в {} вызовах, {:.2f} мс, передача {:.2f} -> {:.2f} МБ\n",
                            run.step, run.passes, run.records, milliseconds(run.microseconds),
                            double(run.transfer_per_call) / (1024.0 * 1024.0),
                            double(run.transfer_shared) / (1024.0 * 1024.0)));
    // ИЗ ЧЕГО ПРОГОН СОСТОИТ. Без этого не отличить «несколько очередей подряд, между которыми ничего
    // нет» (тогда их надо просто объявить одной, и это правка конфига) от «между ними стоит вызов,
    // который в очередь не пускается» (тогда нужна общая резидентность, и это работа библиотеки).
    text.append("   ");
    for (size_t k = 0; k < run.records; ++k) {
      const auto& part = profile.records()[run.first_record + k];
      text.append(std::format(" {}{}", part.label,
                              part.queue_size != 0 ? std::format("[{}]", part.queue_size) : ""));
    }
    text.push_back('\n');
  }

  if (summary.translation_microseconds != 0) {
    text.append(std::format("\nПЕРЕВОД ds -> GLSL: {:.2f} мс за прогон (цена ПЕРВОГО чанка, дальше кэш)\n",
                            milliseconds(summary.translation_microseconds)));
  }

  // РАБОЧИЙ СПИСОК ОХВАТА: инструменты без тела, сложенные по имени. Порядок здесь и есть порядок
  // работы — тело стоит писать тому, чья доля больше, а не тому, чьё имя первым пришло в голову.
  {
    std::vector<std::pair<std::string, uint64_t>> missing;
    for (const auto& record : profile.records()) {
      if (record.fitness != device_fitness::no_body) continue;
      const auto found = std::find_if(missing.begin(), missing.end(),
                                      [&](const auto& entry) { return entry.first == record.label; });
      if (found != missing.end()) {
        found->second += record.microseconds;
        continue;
      }
      missing.emplace_back(record.label, record.microseconds);
    }
    if (!missing.empty()) {
      std::sort(missing.begin(), missing.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
      text.append("\nЧЕГО НЕ ХВАТАЕТ — ИНСТРУМЕНТЫ БЕЗ УСТРОЙСТВЕННОГО ТЕЛА:\n");
      for (size_t i = 0; i < missing.size() && i < top_calls; ++i) {
        text.append(std::format("  {:9.2f} мс  {:5.1f}%  {}\n", milliseconds(missing[i].second),
                                share(missing[i].second, summary.total_microseconds), missing[i].first));
      }
    }
  }

  text.append(std::format("\nСАМЫЕ ДОРОГИЕ ВЫЗОВЫ (топ {}):\n", top_calls));
  auto costly = profile.records();
  std::sort(costly.begin(), costly.end(), [](const profile_record& a, const profile_record& b) {
    return a.microseconds > b.microseconds;
  });
  for (size_t i = 0; i < costly.size() && i < top_calls; ++i) {
    const auto& record = costly[i];
    text.append(std::format("  {:9.2f} мс  {:5.1f}%  {:22s} {:11s} {:8s} шаг '{}'{}\n", milliseconds(record.microseconds),
                            share(record.microseconds, summary.total_microseconds), record.label,
                            to_string(record.shape), to_string(record.fitness), record.step,
                            record.queue_size != 0 ? format_queue_shape(record) : ""));
  }

  // ПАМЯТЬ: объявленное против занятого. Раздел стоит последним не по важности, а потому, что он
  // единственный говорит про величину, которую генератор обещает знать ЗАРАНЕЕ, а не про то, во что
  // обошёлся этот прогон.
  {
    const auto memory = summarize_memory(profile, declared_buffers);
    const auto megabytes = [](const size_t bytes) { return double(bytes) / (1024.0 * 1024.0); };

    text.append("\nПАМЯТЬ:\n");
    if (memory.declared_buffers != 0) {
      text.append(std::format("  буферы объявлены     {:9.1f} МиБ\n", megabytes(memory.declared_buffers)));
    }
    if (memory.largest_temporary != 0) {
      text.append(std::format("  наибольшая временная {:9.1f} МиБ  ({} в шаге '{}')\n",
                              megabytes(memory.largest_temporary), memory.largest_temporary_call,
                              memory.largest_temporary_step));
    }
    // ПРОБЕЛ ИЗМЕРЯЕТСЯ ТОЙ ЖЕ ВЕЛИЧИНОЙ, что и всё остальное: вызов, не объявивший стоимость, — это
    // не ноль, а НЕИЗВЕСТНО, и знать, сколько работы стоит за этим «неизвестно», важнее самого числа.
    if (memory.undeclared_calls != 0) {
      text.append(std::format("  НЕ ОБЪЯВИЛИ         {:5} вызовов ({:.1f}% часов) — их стоимость неизвестна, а не нулевая\n",
                              memory.undeclared_calls,
                              share(memory.undeclared_microseconds, summary.total_microseconds)));
      // ИМЕНА, а не только число: пробел закрывается по одному инструменту, и порядок работы задаёт
      // доля, как и у недостающих тел.
      std::vector<std::pair<std::string, uint64_t>> silent;
      for (const auto& record : profile.records()) {
        if (record.declared_footprint) continue;
        const auto found = std::find_if(silent.begin(), silent.end(),
                                        [&](const auto& entry) { return entry.first == record.label; });
        if (found != silent.end()) {
          found->second += record.microseconds;
          continue;
        }
        silent.emplace_back(record.label, record.microseconds);
      }
      std::sort(silent.begin(), silent.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
      text.append("                      ");
      for (size_t i = 0; i < silent.size() && i < 8; ++i) {
        text.append(std::format("{}{}", i == 0 ? "" : ", ", silent[i].first));
      }
      if (silent.size() > 8) {
        text.append(std::format(" и ещё {}", silent.size() - 8));
      }
      text.push_back('\n');
    }
    if (memory.peak_resident != 0) {
      text.append(std::format("  пик процесса         {:9.1f} МиБ\n", megabytes(memory.peak_resident)));
      // ОТНОШЕНИЕ ОБЪЯВЛЕННОГО К ЗАНЯТОМУ ГОВОРИТСЯ В ОБЕ СТОРОНЫ, и это не симметрия ради красоты:
      // молчать в одну из них значило бы прятать ровно то, ради чего величина и считается.
      //
      //   объявлено МЕНЬШЕ пика — часть стоимости не названа никем, и обещание «назвать стоимость до
      //   запуска» выполняется только на объявленную часть;
      //   объявлено БОЛЬШЕ пика — оценки верхние (число дуг у соседства, длина очереди у заливки), и
      //   запас говорит, насколько они грубы. Это законно: занижать стоимость памяти нельзя, а
      //   завышать — можно, если сказать вслух.
      const size_t named = memory.declared_buffers + memory.largest_temporary;
      if (memory.peak_resident > named) {
        text.append(std::format("  НЕ РАЗЛОЖЕНО         {:9.1f} МиБ  (образ процесса, lua, ресурсы, чужие "
                                "библиотеки — и всё, что ещё не объявлено)\n",
                                megabytes(memory.peak_resident - named)));
      } else {
        text.append(std::format("  объявлено с запасом  {:9.1f} МиБ  (оценки временных таблиц ВЕРХНИЕ: занижать "
                                "стоимость памяти нельзя)\n",
                                megabytes(named - memory.peak_resident)));
      }
    } else {
      text.append("  пик процесса         не измеряется на этой платформе\n");
    }
  }

  text.append("\nПО ШАГАМ:\n");
  auto steps = profile.steps();
  std::sort(steps.begin(), steps.end(), [](const profile_step& a, const profile_step& b) {
    return a.microseconds > b.microseconds;
  });
  for (const auto& step : steps) {
    uint64_t ready = 0;
    for (const auto& record : profile.records()) {
      if (record.step != step.name || record.fitness != device_fitness::ready) continue;
      ready += record.microseconds;
    }
    text.append(std::format("  {:9.2f} мс  {:5.1f}%  {:22s} годных {:5.1f}%\n", milliseconds(step.microseconds),
                            share(step.microseconds, summary.total_microseconds), step.name,
                            share(ready, step.microseconds)));
  }

  return text;
}

} // namespace originator
} // namespace devils_engine
