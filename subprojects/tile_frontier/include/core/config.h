#ifndef TILE_FRONTIER_CORE_CONFIG_H
#define TILE_FRONTIER_CORE_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>

namespace devils_engine { namespace utils { class calendar_clock; } }

// Рантайм-конфиг приложения (resources/config/app.tavl).
// Структуры намеренно остаются агрегатами — их читает tavl через рефлексию.

namespace tile_frontier {
namespace core {

struct window_config {
  std::string title = "tile_frontier";
  uint32_t width = 1280;
  uint32_t height = 720;
  bool create_on_start = true;
};

struct simulation_config {
  uint32_t main_fps = 20;
  uint32_t sound_fps = 60;
  uint32_t render_fps = 60;
  uint32_t assets_fps = 60;
  // Тумблер звукового потока (топологическая настройка → требует перезапуска движка, как render.enabled).
  // Выключение освобождает зарезервированное ядро под worker-потоки (см. init).
  bool sound_enabled = true;
  uint32_t worker_threads_reserved = 4;
  uint32_t min_worker_threads = 1;
  uint32_t thread_start_gap_divisor = 4;
};

struct render_config {
  bool enabled = true;
  bool headless = false;
  std::string config_folder = "render_config";
  std::string shader_folder = "shaders"; // префикс шейдеров в движковом demiurg-реестре
  std::string cache_folder = "cache/render";
  std::string pipeline_cache = "cache/render/pipeline_cache.bin";
  std::string preferred_gpu;
  uint32_t preferred_gpu_index = 0;
  std::string graph = "graphics1";
  // п.2/п.3: второй resident-граф (его ресурсы попадают в общий used-set, своп на него мгновенный).
  // Пусто ⇒ резидентен только основной граф.
  std::string menu_graph = "menu1";
  // Демо интеграционного слоя: >0 ⇒ main периодически переключает активный граф graph<->menu_graph
  // каждые N мс (проверяет мгновенный своп без пересоздания ресурсов). 0 ⇒ выключено.
  uint32_t demo_graph_toggle_ms = 0;
};

struct metrics_config {
  bool enabled = true;
  uint32_t log_interval_ms = 1000;
};

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

// Единственная project-config → engine-calendar граница. Вызывается при старте; calendar source
// намеренно не является live setting.
devils_engine::utils::calendar_clock make_calendar_clock(const time_config& cfg);

// Политика логгирования. Базовый always-on слой (подсистемы/окно/устройства) идёт через
// utils::info всегда. Домены (catalogue) по умолчанию OFF; здесь задаётся глубина на домен
// (off/info/flow/trace) + файловый сток. Уровни можно менять и в рантайме (app.set_log_level).
struct logging_config {
  bool console = true;
  std::string file = "logs/tile_frontier.log"; // пусто = без файла
  std::string main = "off";
  std::string assets = "off";
  std::string sound = "off";
  std::string render = "off";
  std::string ui = "off";
  std::string gameplay = "off";
  std::string resource = "off";
  std::string demiurg = "off";
};

struct app_config {
  window_config window;
  simulation_config simulation;
  render_config render;
  time_config time;
  metrics_config metrics;
  logging_config logging;
};

// Конфиг грузится как demiurg-ресурс (app_config_resource) из движкового реестра —
// см. simulation::init. Прежний load_app_config(file_io) удалён (demiurg 1a, Q3).

// Резолв путей относительно project_folder(): абсолютные ('/...') оставляем как есть.
std::string make_project_path(std::string path);
// То же, но гарантирует завершающий '/' (нужно парсеру render-конфига).
std::string make_project_folder_path(std::string path);

}
}

#endif
