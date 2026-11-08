#ifndef DEVILS_ENGINE_ORIGINATOR_SKELETON_H
#define DEVILS_ENGINE_ORIGINATOR_SKELETON_H

#include <array>
#include <cstdint>
#include <span>
#include <vector>
#include "graph_walk.h"

// СКЕЛЕТ — граф из точек и рёбер, заданных ЯВНЫМИ СПИСКАМИ: узел — точка в пространстве, ребро —
// пара номеров, длина ребра — расстояние между его концами. Так описываются пещеры из ниш и ходов,
// дороги между поселениями, сети коридоров — всё, что строится «кусок за куском» и потом
// спрашивается «сколько идти отсюда туда».
//
// Отдельно от графа поверхности (`graph_tools`), потому что адресуется иначе: там соседство — CSR по
// клеткам, и у каждой клетки есть место в растре или на сфере; здесь узлов десятки, они растут по
// одному, и рёбра лежат списком, который скрипт дописывает и откатывает.
//
// Те же функции стоят за инструментами `skeleton_distance` и `skeleton_tour`: хост, которому нужно то
// же число вне генератора (проверка, отчёт), зовёт их напрямую, и второй реализации обхода нет.

namespace devils_engine::originator {

struct skeleton_graph {
  std::span<const std::array<double, 3>> nodes;
  std::span<const std::array<uint32_t, 2>> edges;
  // Непроходимый узел (0) не входит ни в один путь, и рёбра к нему не считаются. Пусто — проходимы все.
  std::span<const uint8_t> passable;
};

// Кратчайший путь по рёбрам от БЛИЖАЙШЕГО из источников до каждого узла; недостижимый узел — -1.
// Ребро с концом вне списка узлов — ошибка вызывающего (utils::error), а не тихо пропущенное ребро.
std::vector<double> skeleton_distance(const skeleton_graph& graph, std::span<const uint32_t> sources);
std::vector<double> skeleton_distance(const skeleton_graph& graph, std::span<const uint32_t> sources, walk_workspace& workspace);

// Длина замкнутого обхода: от start через все остановки и обратно, по кратчайшим путям между ними.
// Порядок остановок — ближайший сосед, затем 2-opt до неподвижной точки: это не оптимум задачи
// коммивояжёра, но детерминированная и честная оценка сверху. Недостижимая остановка — -1.
double skeleton_tour(const skeleton_graph& graph, uint32_t start, std::span<const uint32_t> stops);

} // namespace devils_engine::originator

#endif
