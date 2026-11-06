#ifndef DEVILS_ENGINE_PLAYGROUND_GENERATOR_RESOURCES_H
#define DEVILS_ENGINE_PLAYGROUND_GENERATOR_RESOURCES_H

#include <filesystem>
#include <vector>

#include "devils_engine/demiurg/module_system.h"
#include "devils_engine/demiurg/resource_system.h"
#include "devils_engine/originator/generator_resource.h"

// Минимальное владение ресурсами headless-лаборатории. Модули живут дольше registry;
// имена генераторов, overrides, инструменты и исполнение остаются у потребителя.
namespace devils_engine::playground {

class generator_resources {
public:
  generator_resources(const std::filesystem::path& root,
                      const std::vector<demiurg::module_system::list_entry>& module_entries)
    : modules_(root.generic_string() + "/") {
    modules_.load_modules(module_entries);
    originator::register_generator_resources(resources_);
    resources_.parse_resources(&modules_);
  }

  generator_resources(const generator_resources&) = delete;
  generator_resources& operator=(const generator_resources&) = delete;
  generator_resources(generator_resources&&) = delete;
  generator_resources& operator=(generator_resources&&) = delete;

  demiurg::resource_system& resources() noexcept {
    return resources_;
  }

  const demiurg::resource_system& resources() const noexcept {
    return resources_;
  }

private:
  demiurg::module_system modules_;
  demiurg::resource_system resources_;
};

} // namespace devils_engine::playground

#endif
