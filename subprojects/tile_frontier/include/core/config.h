#ifndef TILE_FRONTIER_CORE_CONFIG_H
#define TILE_FRONTIER_CORE_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>

namespace devils_engine {
namespace utils {
class calendar_clock;
}
} // namespace devils_engine

// Рантайм-конфиг приложения (resources/config/app.tavl).
// Структуры намеренно остаются агрегатами — их читает tavl через рефлексию.

namespace tile_frontier {
namespace core {

struct window_config {
  uint32_t width = 1280;
  uint32_t height = 720;
  bool create_on_start = true;
  bool fullscreen = false;
  std::string monitor;
};

struct simulation_config {
  uint32_t main_fps = 20;
  uint32_t sound_fps = 60;
  uint32_t render_fps = 60;
  uint32_t assets_fps = 60;
  // Тумблер звукового потока (топологическая настройка → требует перезапуска движка, как render.enabled).
  // Выключение освобождает зарезервированное ядро под worker-потоки (см. init).
  bool sound_enabled = true;
  // Ассетный поток является частью топологии runtime. tile_frontier требует его для обычного
  // gameplay boot, но app-shell корректно не создаёт объект/поток при выключенном флаге.
  bool assets_enabled = true;
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
  std::string file; // пусто = без файла
  // Startup/restart option: validation layer + VK_EXT_debug_utils + debug messenger.
  bool vulkan_debug = false;
  std::string main = "off";
  std::string assets = "off";
  std::string sound = "off";
  std::string render = "off";
  std::string ui = "off";
  std::string gameplay = "off";
  std::string resource = "off";
  std::string demiurg = "off";
};

// Пользовательские уровни громкости. Имена категорий совпадают с sound::type; итоговый gain
// голоса = master * category * volume конкретной задачи.
struct sound_config {
  std::string device;
  float master = 1.0f;
  float music = 1.0f;
  float talk = 1.0f;
  float talk_pos = 1.0f;
  float ui_effect = 1.0f;
  float sfx = 1.0f;
};

struct app_config {
  window_config window;
  simulation_config simulation;
  render_config render;
  time_config time;
  metrics_config metrics;
  logging_config logging;
  sound_config sound;
};

// Единственная схема, которую standard runtime пишет в пользовательский settings.tavl.
// Project/engine topology (simulation/render/time/create_on_start) остаётся в bundled app.tavl.
struct user_window_config {
  uint32_t width = 1280;
  uint32_t height = 720;
  bool fullscreen = false;
  std::string monitor;
};

struct user_settings {
  user_window_config window;
  sound_config sound;
  metrics_config metrics;
  logging_config logging;
};

// Конфиг грузится как demiurg-ресурс (app_config_resource) из движкового реестра —
// см. simulation::init. Прежний load_app_config(file_io) удалён (demiurg 1a, Q3).

// Резолв путей относительно project_folder(): абсолютные ('/...') оставляем как есть.
std::string make_project_path(std::string path);
// То же, но гарантирует завершающий '/' (нужно парсеру render-конфига).
std::string make_project_folder_path(std::string path);

} // namespace core
} // namespace tile_frontier

#endif
