#ifndef FRONTIER_ONLINE_CORE_WORLD_RULES_H
#define FRONTIER_ONLINE_CORE_WORLD_RULES_H

#include <cstdint>
#include <string>
#include <vector>

#include <devils_engine/utils/timeline.h>

// Фундаментальные правила мира frontier_online, вшитые в проект: как игровое время соотносится с
// реальным и как устроен календарь. Это не настройка и не ресурс — модуль, поменявший их, испортил
// бы слишком многое (сроки акторов, сохранения, сеть), поэтому они живут в коде причинного ядра и
// одинаковы у клиента, у сервера и в одиночной игре. Правила будут добавляться, но не подменяться.

namespace frontier_online::core {

struct calendar_config {
  // game_time: дата следует за масштабированной непрерывной шкалой; turn: меняется только с ходом.
  // Это проектная/topological настройка, а не runtime option.
  std::string source = "game_time";
  uint32_t hours_per_day = 24;
  std::vector<uint32_t> days_in_month;

  // Если days_in_month пуст, используется start_absolute_day; иначе calendar epoch ниже.
  uint64_t start_absolute_day = 0;
  uint64_t start_year = 0;
  uint32_t start_month = 1;
  uint32_t start_day = 1;
  uint32_t start_hour = 0;
  uint32_t start_minute = 0;
  uint32_t start_second = 0;

  // Вклад одного хода в дату при source=turn. Можно выразить, например, сутки или месяц.
  uint64_t seconds_per_turn = 0;
  uint64_t days_per_turn = 1;
  uint64_t months_per_turn = 0;
  uint64_t years_per_turn = 0;
};

// Масштаб gameplay timeline: game_seconds игровых секунд проходят за real_seconds
// номинальных реальных секунд. Неквалифицированные длительности в конфигах считаются
// реальными; игровые/календарные/turn-длительности должны быть названы явно.
struct time_config {
  uint32_t game_seconds = 1;
  uint32_t real_seconds = 1;
  calendar_config calendar;
};

// Правила времени мира: игровая секунда = реальной, сутки 24 часа, обычный год без високосов,
// отсчёт с года 0. Масштаб — причинное правило (по нему идут игровые сроки акторов), календарь —
// толкование игрового времени для людей.
const time_config& world_time_rules() noexcept;

// Масштаб игровой шкалы по правилам мира — то, что ставят часы авторитета и одиночной игры.
inline devils_engine::utils::game_time_scale world_time_scale() {
  const auto& rules = world_time_rules();
  return devils_engine::utils::game_time_scale::from_seconds(rules.game_seconds, rules.real_seconds);
}

} // namespace frontier_online::core

#endif
