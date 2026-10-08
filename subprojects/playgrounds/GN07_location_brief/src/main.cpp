#include <algorithm>
#include <charconv>
#include <cstdint>
#include <exception>
#include <fstream>
#include <format>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "location_model.h"
#include "node_scope.h"
#include "resources.h"
#include "solver.h"

// GN07 — локация из задания: обязательства сводятся к графу с проверяемыми свойствами, граф
// воплощается в план участка, и свойства перепроверяются уже по плану. См. README.md площадки.

namespace {

using namespace gn07;

struct options {
  std::string brief_path = std::string(GN07_RESOURCE_ROOT) + "/gn07/briefs/first_mine.tavl";
  std::string world_path = std::string(GN07_RESOURCE_ROOT) + "/gn07/world/fragment.tavl";
  uint64_t seed = 0;
  uint32_t attempts = 64;
  uint32_t seeds = 128;
  bool verify = false;
  bool map = true;
  std::string svg_path;
};

bool read_number(const std::string_view text, uint64_t& out) {
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
  return error == std::errc() && end == text.data() + text.size();
}

void print_help() {
  std::cout <<
    "GN07_location_brief [options]\n"
    "  --brief=PATH     brief file (default: resources/gn07/briefs/first_mine.tavl)\n"
    "  --world=PATH     world fragment (default: resources/gn07/world/fragment.tavl)\n"
    "  --seed=N         location seed (default 0)\n"
    "  --attempts=N     graphs tried per seed before refusing (default 64)\n"
    "  --verify         solve seeds 0..N-1, check every result independently and repeat each\n"
    "  --seeds=N        seed count for --verify (default 128)\n"
    "  --no-map         do not print the lattice map\n"
    "  --svg=PATH       write the plan of the location as SVG\n";
}

bool parse_options(const int argc, char** argv, options& opt) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto value = [&](const std::string_view prefix) -> std::string_view {
      return arg.starts_with(prefix) ? arg.substr(prefix.size()) : std::string_view{};
    };
    uint64_t number = 0;
    if (arg == "--help") {
      print_help();
      return false;
    } else if (arg == "--verify") {
      opt.verify = true;
    } else if (arg == "--no-map") {
      opt.map = false;
    } else if (arg.starts_with("--brief=")) {
      opt.brief_path = std::string(value("--brief="));
    } else if (arg.starts_with("--svg=")) {
      opt.svg_path = std::string(value("--svg="));
    } else if (arg.starts_with("--world=")) {
      opt.world_path = std::string(value("--world="));
    } else if (arg.starts_with("--seed=") && read_number(value("--seed="), number)) {
      opt.seed = number;
    } else if (arg.starts_with("--attempts=") && read_number(value("--attempts="), number) && number > 0) {
      opt.attempts = uint32_t(number);
    } else if (arg.starts_with("--seeds=") && read_number(value("--seeds="), number) && number > 0) {
      opt.seeds = uint32_t(number);
    } else {
      std::cerr << "unknown or malformed option: " << arg << "\n";
      print_help();
      return false;
    }
  }
  return true;
}

// Всё, что решатель получает для одной локации.
struct setup {
  world_fragment world;
  brief task;
  archetype arch;
  location_input input;
};

// Вход локации из фрагмента, в котором сосед по обычной связи отражён относительно локации:
// так проверка видит, что порт обязан смотреть туда, где стоит сосед.
location_input with_mirrored_ordinary_neighbour(const setup& s) {
  world_fragment moved = s.world;
  const world_node* loc = moved.find(s.task.location);
  for (const auto& port : s.input.ports) {
    if (port.kind != link_kind::ordinary) {
      continue;
    }
    for (auto& n : moved.nodes) {
      if (n.name == port.neighbour) {
        n.at_x = 2 * loc->at_x - n.at_x;
        n.at_y = 2 * loc->at_y - n.at_y;
      }
    }
  }
  location_input in = derive_location_input(moved, s.task, s.arch);
  in.node = s.input.node; // узел из временной копии фрагмента пережил бы её; ссылаемся на исходный
  return in;
}

int run_verify(const options& opt, const setup& st, const script_env& env) {
  const brief& task = st.task;
  const archetype& arch = st.arch;
  const location_input& input = st.input;
  uint32_t failures = 0;
  uint32_t seed_failures = 0;
  uint64_t attempts_used = 0;
  uint32_t worst_attempt = 0;
  for (uint64_t seed = 0; seed < opt.seeds; ++seed) {
    const solve_result first = solve(task, input, arch, env, seed, opt.attempts);
    std::vector<std::string> problems = verify_result(first, task, input, arch, env);
    // Воспроизводимость: тот же seed и те же данные дают тот же отчёт и ту же карту байт в байт.
    const solve_result again = solve(task, input, arch, env, seed, opt.attempts);
    if (format_report(first, task, input, arch) + format_map(first, arch) + format_frame_svg(first, task, arch) !=
        format_report(again, task, input, arch) + format_map(again, arch) + format_frame_svg(again, task, arch)) {
      problems.push_back("second solve with the same seed differs");
    }
    if (!problems.empty()) {
      ++failures;
      ++seed_failures;
      std::cout << std::format("seed {}: FAILED\n", seed);
      for (const auto& p : problems) {
        std::cout << "  " << p << "\n";
      }
      continue;
    }
    attempts_used += first.attempt + 1;
    worst_attempt = std::max(worst_attempt, first.attempt + 1);
  }
  // Место, которое принимает все узлы или ни одного, ничего не различает. Так выглядела бы,
  // например, пустая программа ds после молча проваленного разбора.
  for (const auto& p : arch.places) {
    bool accepted = false;
    bool rejected = false;
    for (uint64_t seed = 0; seed < opt.seeds && !(accepted && rejected); ++seed) {
      const solve_result r = solve(task, input, arch, env, seed, opt.attempts);
      if (!r.ok) {
        continue;
      }
      const location_view view{&r.graph, &arch, &input.facts};
      for (uint32_t n = 0; n < r.graph.nodes.size(); ++n) {
        (env.evaluate(p.where, view, n) ? accepted : rejected) = true;
      }
    }
    if (!accepted || !rejected) {
      ++failures;
      std::cout << std::format("place '{}' {} every node it was shown\n", p.name, accepted ? "accepts" : "rejects");
    }
  }

  // Словарь скоупа против независимого C++ на реальном графе. Остальные проверки вызывают
  // тот же предикат, что и решатель, и не заметили бы, что ds понимает выражение иначе.
  const solve_result sample = solve(task, input, arch, env, 0, opt.attempts);
  if (sample.ok) {
    const location_graph& g = sample.graph;
    const auto share = [&](const uint32_t n) { return g.max_depth > 0 ? double(g.depth[n]) / double(g.max_depth) : 0.0; };
    const auto dead_end = [&](const uint32_t n) {
      return n != 0 && n != g.secret_entrance && g.neighbours[n].size() == 1;
    };
    const auto tagged = [&](const uint32_t n, const std::string_view tag) {
      const auto& tags = arch.kinds[g.nodes[n].kind].tags;
      return std::find(tags.begin(), tags.end(), tag) != tags.end();
    };
    // Как проба обязана отвечать: различать узлы, либо (для свойства локации) одинаково для всех.
    enum class expect { discriminate, all, none };
    struct probe {
      std::string text;
      std::function<bool(uint32_t)> truth;
      bool needs_secret = false;   // без тайного хода такое свойство ничего не различает
      expect shape = expect::discriminate;
    };
    std::vector<probe> probes = {
      {"dead_end", dead_end},
      {"NOR = { on_secret_way }", [&](uint32_t n) { return g.on_secret_way[n] == 0; }, true},
      {"depth_share >= 0.6", [&](uint32_t n) { return share(n) >= 0.6; }},
      {"depth >= 5, depth <= 9", [&](uint32_t n) { return g.depth[n] >= 5 && g.depth[n] <= 9; }},
      {"has_tag = safe", [&](uint32_t n) { return tagged(n, "safe"); }},
      {"kind_is = face", [&](uint32_t n) { return arch.kinds[g.nodes[n].kind].name == "face"; }},
      {std::format("at_side = {}", to_string(g.boundary[0])), [&](uint32_t n) { return g.boundary[n] == g.boundary[0]; }},
      {"hidden_entry", [&](uint32_t n) { return g.nodes[n].entry == passage::hidden; }, true},
      {"subtree >= 3", [&](uint32_t n) { return g.subtree[n] >= 3; }},
      {"secret_depth >= 4", [&](uint32_t n) { return g.secret_depth[n] >= 4; }, true},
      {"OR = { dead_end, has_tag = safe }", [&](uint32_t n) { return dead_end(n) || tagged(n, "safe"); }},
    };
    // Факт — свойство локации: одинаковый ответ для всех узлов, и неверное значение — «нет».
    if (!input.facts.empty()) {
      const fact& f = input.facts.front();
      probes.push_back({std::format("has_fact = {{ {}, {} }}", f.name, f.value), [](uint32_t) { return true; },
        false, expect::all});
      probes.push_back({std::format("has_fact = {{ {}, not_{} }}", f.name, f.value), [](uint32_t) { return false; },
        false, expect::none});
    }
    const location_view view{&g, &arch, &input.facts};
    size_t probed = 0;
    for (const auto& pr : probes) {
      if (pr.needs_secret && g.secret_entrance == no_node) {
        continue;
      }
      ++probed;
      const auto program = env.compile_text("probe", pr.text);
      size_t mismatches = 0, accepted = 0;
      for (uint32_t n = 0; n < g.nodes.size(); ++n) {
        const bool got = env.evaluate(program, view, n);
        accepted += got;
        mismatches += got != pr.truth(n);
      }
      const bool shape_ok = pr.shape == expect::all ? accepted == g.nodes.size() :
        pr.shape == expect::none ? accepted == 0 : (accepted != 0 && accepted != g.nodes.size());
      if (mismatches != 0 || !shape_ok) {
        ++failures;
        std::cout << std::format("probe '{}': {} mismatches, accepts {}/{}\n", pr.text, mismatches, accepted,
          g.nodes.size());
      }
    }
    // Опечатка в имени тега обязана быть ошибкой, а не тихим «нет»: иначе место молча пустеет.
    bool typo_caught = false;
    try {
      env.evaluate(env.compile_text("typo", "has_tag = saef"), view, 0);
    } catch (const std::exception&) {
      typo_caught = true;
    }
    if (!typo_caught) {
      ++failures;
      std::cout << "an undeclared tag in a predicate was not reported\n";
    }
    std::cout << std::format("vocabulary: {} probes checked against C++, typo reported\n", probed);
  }

  // Проверка, которая ни разу не срабатывала, ничего не обещает: результат портится нарочно,
  // и каждая порча обязана быть поймана.
  const solve_result base = solve(task, input, arch, env, 0, opt.attempts);
  if (base.ok) {
    struct mutation {
      std::string name;
      std::function<void(solve_result&, brief&, location_input&)> apply;
      bool needs_secret = false;
    };
    const std::vector<mutation> mutations = {
      {"binding moved to the ordinary entrance", [](solve_result& r, brief&, location_input&) {
         for (auto& b : r.bindings) b.node = 0;
       }},
      {"stale stored depth", [](solve_result& r, brief&, location_input&) {
         r.graph.depth[r.bindings.front().node] += 1;
       }},
      {"secret way made obvious", [](solve_result& r, brief&, location_input&) {
         for (auto& n : r.graph.nodes) n.entry = passage::open;
       }, true},
      {"road moved to the other side of the location", [&](solve_result&, brief&, location_input& in) {
         in = with_mirrored_ordinary_neighbour(st);
       }},
      {"budget tightened below the binding", [](solve_result& r, brief& b, location_input&) {
         for (const auto& bind : r.bindings) {
           b.placements[bind.placement].budget_max = r.graph.depth[bind.node] - 1;
         }
       }},
      {"allowance tightened below the total", [](solve_result& r, brief& b, location_input&) {
         b.has_allowance = true;
         b.content_max = r.graph.total_content - 1;
         b.content_min = std::min(b.content_min, b.content_max);
       }},
    };
    size_t checked = 0;
    for (const auto& m : mutations) {
      // Без тайного хода портить нечего: такая порча не может быть поймана и ничего не говорит.
      if (m.needs_secret && base.graph.secret_entrance == no_node) {
        continue;
      }
      ++checked;
      solve_result broken = base;
      brief changed = task;
      location_input moved = input;
      m.apply(broken, changed, moved);
      if (verify_result(broken, changed, moved, arch, env).empty()) {
        ++failures;
        std::cout << std::format("mutation '{}' was NOT caught\n", m.name);
      }
    }
    std::cout << std::format("mutations: {} checked\n", checked);

    // Порча каркаса: проверка плана выводит стыки из прямоугольников, и каждая из этих правок
    // обязана изменить то, что она видит.
    const location_graph& g = base.graph;
    const frame_rules& rules = arch.frame;
    const auto mouth_of = [&](location_frame& f, const uint32_t node, const uint32_t toward) -> frame_part* {
      for (auto& p : f.zones[node].parts) {
        if (p.role == part_role::mouth && p.toward == toward) return &p;
      }
      return nullptr;
    };
    // Сузить сечение части поперёк ребра node—toward до width вокруг её середины.
    const auto set_width = [&](plan_rect& b, const uint32_t node, const uint32_t toward, const int32_t width) {
      const bool horizontal = g.nodes[node].x != g.nodes[toward].x;
      int32_t& lo = horizontal ? b.y0 : b.x0;
      int32_t& hi = horizontal ? b.y1 : b.x1;
      const int32_t mid = lo + (hi - lo) / 2;
      lo = mid - width / 2;
      hi = lo + width;
    };
    // Первое открытое ребро от коридора к коридору вглубь шахты.
    uint32_t open_child = no_node;
    for (uint32_t i = 2; i < g.nodes.size() && open_child == no_node; ++i) {
      if (g.nodes[i].entry == passage::open && i != g.secret_entrance) open_child = i;
    }
    uint32_t hidden_child = no_node;
    for (uint32_t i = 0; i < g.nodes.size(); ++i) {
      if (g.nodes[i].entry == passage::hidden) hidden_child = i;
    }
    const auto portal_of = [](location_frame& f, const uint32_t node) -> frame_part* {
      for (auto& p : f.zones[node].parts) {
        if (p.role == part_role::portal) return &p;
      }
      return nullptr;
    };
    const auto push_portal = [&](location_frame& f, const int32_t delta) {
      plan_rect& b = portal_of(f, 0)->box;
      switch (g.boundary[0]) {
        case side::west: b.x0 -= delta; break;
        case side::east: b.x1 += delta; break;
        case side::north: b.y0 -= delta; break;
        default: b.y1 += delta; break;
      }
    };
    struct frame_mutation {
      std::string name;
      std::function<void(location_frame&)> apply;
      bool needs_secret = false;
    };
    const std::vector<frame_mutation> frame_mutations = {
      {"opening to a parent removed", [&](location_frame& f) {
         auto& parts = f.zones[open_child].parts;
         std::erase_if(parts, [&](const frame_part& p) {
           return p.role == part_role::mouth && p.toward == g.nodes[open_child].parent;
         });
       }},
      {"open passage narrowed below the passage", [&](location_frame& f) {
         const uint32_t p = g.nodes[open_child].parent;
         set_width(mouth_of(f, open_child, p)->box, open_child, p, rules.passage - 1);
         set_width(mouth_of(f, p, open_child)->box, p, open_child, rules.passage - 1);
       }},
      {"hidden passage widened into an obvious one", [&](location_frame& f) {
         const uint32_t p = g.nodes[hidden_child].parent;
         set_width(mouth_of(f, hidden_child, p)->box, hidden_child, p, rules.hidden_max + 60);
         set_width(mouth_of(f, p, hidden_child)->box, p, hidden_child, rules.hidden_max + 60);
       }, true},
      {"rock thinned between unrelated zones", [&](location_frame& f) {
         const frame_analysis fa = analyse_frame(f);
         frame_part* best = nullptr;
         const frame_part* other = nullptr;
         int64_t best_d = -1;
         for (auto& pa : f.zones[fa.wall_a].parts) {
           for (const auto& pb : f.zones[fa.wall_b].parts) {
             const int64_t gx = std::max<int64_t>({0, pb.box.x0 - pa.box.x1, pa.box.x0 - pb.box.x1});
             const int64_t gy = std::max<int64_t>({0, pb.box.y0 - pa.box.y1, pa.box.y0 - pb.box.y1});
             if (best_d < 0 || gx * gx + gy * gy < best_d) {
               best_d = gx * gx + gy * gy;
               best = &pa;
               other = &pb;
             }
           }
         }
         // Сдвинуть часть к соседу так, чтобы зазор по каждой оси стал четвертью стены.
         plan_rect& b = best->box;
         const plan_rect& o = other->box;
         int32_t dx = 0, dy = 0;
         if (o.x0 > b.x1) dx = o.x0 - b.x1 - rules.wall / 4;
         if (b.x0 > o.x1) dx = -(b.x0 - o.x1 - rules.wall / 4);
         if (o.y0 > b.y1) dy = o.y0 - b.y1 - rules.wall / 4;
         if (b.y0 > o.y1) dy = -(b.y0 - o.y1 - rules.wall / 4);
         b = {b.x0 + dx, b.y0 + dy, b.x1 + dx, b.y1 + dy};
       }},
      {"stray tunnel between unrelated zones", [&](location_frame& f) {
         const frame_analysis fa = analyse_frame(f);
         const plan_rect a = f.zones[fa.wall_a].parts.front().box;
         const plan_rect b = f.zones[fa.wall_b].parts.front().box;
         f.zones[fa.wall_a].parts.push_back({{std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1),
           std::max(a.y1, b.y1)}, part_role::lobe, no_node});
       }},
      {"entrance portal cut short of the edge", [&](location_frame& f) { push_portal(f, -1); }},
      {"entrance portal pushed out of the plot", [&](location_frame& f) { push_portal(f, 1); }},
    };
    size_t frame_checked = 0;
    for (const auto& m : frame_mutations) {
      if ((m.needs_secret && hidden_child == no_node) || open_child == no_node) {
        continue;
      }
      ++frame_checked;
      solve_result broken = base;
      m.apply(broken.frame);
      if (verify_result(broken, task, input, arch, env).empty()) {
        ++failures;
        std::cout << std::format("frame mutation '{}' was NOT caught\n", m.name);
      }
    }
    std::cout << std::format("frame mutations: {} checked\n", frame_checked);
  } else {
    ++failures;
    std::cout << "seed 0 refused, mutations not checked: " << base.refusal << "\n";
  }

  const uint32_t passed = opt.seeds - seed_failures;
  std::cout << std::format("verify: {}/{} seeds passed; attempts per seed avg={:.2f} max={}\n", passed, opt.seeds,
    passed == 0 ? 0.0 : double(attempts_used) / double(passed), worst_attempt);
  return failures == 0 ? 0 : 1;
}

} // namespace

int main(const int argc, char** argv) {
  options opt;
  if (!parse_options(argc, argv, opt)) {
    return 2;
  }
  try {
    const script_env env;
    setup st;
    const std::string world_text = read_text_file(opt.world_path);
    st.world = read_world_fragment(world_text, opt.world_path);
    const std::string brief_text = read_text_file(opt.brief_path);
    st.task = read_brief(brief_text, opt.brief_path);
    const world_node* node = st.world.find(st.task.location);
    if (node == nullptr || node->archetype.empty()) {
      throw std::runtime_error(std::format("brief '{}': location '{}' is not a location in the world fragment",
        st.task.name, st.task.location));
    }
    const std::string archetype_path = std::format("{}/gn07/archetypes/{}.tavl", GN07_RESOURCE_ROOT, node->archetype);
    const std::string archetype_text = read_text_file(archetype_path);
    st.arch = read_archetype(archetype_text, archetype_path, env);
    validate_brief(st.task, st.world, st.arch);
    st.input = derive_location_input(st.world, st.task, st.arch);

    if (opt.verify) {
      return run_verify(opt, st, env);
    }

    const solve_result result = solve(st.task, st.input, st.arch, env, opt.seed, opt.attempts);
    std::cout << std::format("seed={}\n", opt.seed) << format_report(result, st.task, st.input, st.arch);
    if (!result.ok) {
      return 1;
    }
    if (opt.map) {
      std::cout << format_map(result, st.arch);
    }
    if (!opt.svg_path.empty()) {
      std::ofstream svg(opt.svg_path, std::ios::binary);
      svg << format_frame_svg(result, st.task, st.arch);
      if (!svg) {
        throw std::runtime_error(std::format("cannot write '{}'", opt.svg_path));
      }
    }
    const auto problems = verify_result(result, st.task, st.input, st.arch, env);
    for (const auto& p : problems) {
      std::cout << "problem: " << p << "\n";
    }
    return problems.empty() ? 0 : 1;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 3;
  }
}
