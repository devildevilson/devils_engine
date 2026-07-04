#ifndef TILE_FRONTIER_CORE_CONFIG_H
#define TILE_FRONTIER_CORE_CONFIG_H

#include <cstdint>
#include <string>

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

struct app_config {
  window_config window;
  simulation_config simulation;
  render_config render;
  metrics_config metrics;
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
