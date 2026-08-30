#ifndef DEVILS_ENGINE_PF09_VIEWER_H
#define DEVILS_ENGINE_PF09_VIEWER_H

// Интерактивный просмотр зональности: смотреть, наводиться, выделять, читать метаинформацию и гонять
// персонажей. Всё, что видно на экране, читается ИЗ ТОГО ЖЕ хранилища, что и проверки, — иначе картинка
// стала бы отдельным источником истины и разошлась бы с ним незаметно.

#include <cstdint>
#include <filesystem>
#include <string>

#include "locality.h"
#include "territory.h"

namespace devils_engine::pf09 {

struct viewer_options {
  std::filesystem::path world_root;
  double stream_radius_m = 12000.0;
  uint32_t stream_budget = 32;
  uint32_t width = 1280;
  uint32_t height = 720;
  double start_span_m = 900.0;   // ширина обзора при запуске
  uint32_t agent_count = 24;
  uint32_t frames = 0;           // ноль — до закрытия окна
  int32_t start_floor = 0;       // с какого этажа начинать
  bool start_cutaway = false;    // срезать передний план сразу
  bool start_tactics = false;    // сразу показывать тактическую картину наведённого места
  std::string dump_path;         // сохранить последний кадр
  bool validation = false;
};

int run_viewer(const territory& map, const locality_config& local, const viewer_options& options);

} // namespace devils_engine::pf09

#endif
