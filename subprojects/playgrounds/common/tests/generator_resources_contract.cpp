#include "devils_engine/playground/generator_resources.h"
#include <iostream>

using namespace devils_engine;
int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const std::filesystem::path root = argv[i];
    const auto module = root.parent_path().filename().string().substr(0, 4);
    std::vector<demiurg::module_system::list_entry> list{{module == "GN02" ? "gn02/" : module == "GN03" ? "gn03/" : module == "GN04" ? "gn04/" : "gn06/", "", ""}};
    demiurg::module_system old_modules(root.generic_string() + "/");
    old_modules.load_modules(list);
    demiurg::resource_system old_registry;
    originator::register_generator_resources(old_registry);
    old_registry.parse_resources(&old_modules);
    playground::generator_resources current(root, list);
    const auto before = old_registry.find("");
    const auto after = current.resources().find("");
    if (before.size() != after.size() || before.empty()) return 1;
    for (size_t j = 0; j < before.size(); ++j) {
      if (before[j]->id != after[j]->id || before[j]->path != after[j]->path ||
          before[j]->ext != after[j]->ext || before[j]->module_name != after[j]->module_name ||
          originator::read_generator_source(old_registry, before[j]->id) !=
          originator::read_generator_source(current.resources(), after[j]->id)) return 1;
    }
    std::cout << module << ": " << before.size() << " ordered resource records and source bytes equal\n";
  }
}
