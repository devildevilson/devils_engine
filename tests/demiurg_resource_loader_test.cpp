#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "devils_engine/demiurg/resource_base.h"
#include "devils_engine/demiurg/resource_loader.h"
#include "devils_engine/utils/safe_handle.h"

using namespace devils_engine;

namespace {

class staged_pipeline_resource final : public demiurg::resource_interface {
public:
  int read_config_count = 0;
  int compile_cpu_count = 0;
  int commit_gpu_count = 0;
  int release_gpu_count = 0;
  int unload_cpu_count = 0;

  staged_pipeline_resource() {
    set_flag(demiurg::resource_flags::warm_and_hot_same, false);
  }

  int32_t top_state() const override { return 3; }

  bool is_external_step(const int32_t from) const override {
    return from == 2;
  }

  void load_step(const int32_t from, const utils::safe_handle_t&) override {
    switch (from) {
      case 0: ++read_config_count; break;
      case 1: ++compile_cpu_count; break;
      case 2: ++commit_gpu_count; break;
      default: FAIL("unexpected load step");
    }
  }

  void unload_step(const int32_t from, const utils::safe_handle_t&) override {
    switch (from) {
      case 3: ++release_gpu_count; break;
      case 2: ++unload_cpu_count; break;
      default: break;
    }
  }

  void render_commit() {
    load(utils::safe_handle_t(this));
  }

  void render_release() {
    unload(utils::safe_handle_t(this));
  }

  void load_cold(const utils::safe_handle_t&) override {}
  void load_warm(const utils::safe_handle_t&) override {}
  void unload_warm(const utils::safe_handle_t&) override {}
  void unload_hot(const utils::safe_handle_t&) override {}
};

class cpu_ready_resource final : public demiurg::resource_interface {
public:
  int load_count = 0;
  int unload_count = 0;

  cpu_ready_resource() {
    set_flag(demiurg::resource_flags::warm_and_hot_same, true);
  }

  void load_cold(const utils::safe_handle_t&) override { ++load_count; }
  void load_warm(const utils::safe_handle_t&) override {}
  void unload_warm(const utils::safe_handle_t&) override { ++unload_count; }
  void unload_hot(const utils::safe_handle_t&) override {}
};

}

TEST_CASE("resource_loader keeps pipeline GPU commit as render-owned external step [demiurg]") {
  staged_pipeline_resource pipeline;
  demiurg::resource_loader loader;
  std::vector<demiurg::resource_loader::external_job> jobs;

  loader.request(&pipeline, pipeline.final_state());

  CHECK(loader.update(jobs) == 1);
  CHECK(jobs.empty());
  CHECK(pipeline.state() == 1);
  CHECK(pipeline.read_config_count == 1);
  CHECK(pipeline.compile_cpu_count == 0);
  CHECK(pipeline.commit_gpu_count == 0);

  CHECK(loader.update(jobs) == 1);
  CHECK(jobs.empty());
  CHECK(pipeline.state() == 2);
  CHECK(pipeline.compile_cpu_count == 1);
  CHECK(pipeline.commit_gpu_count == 0);

  CHECK(loader.update(jobs) == 1);
  REQUIRE(jobs.size() == 1);
  CHECK(jobs[0].res == &pipeline);
  CHECK(jobs[0].load);
  CHECK(pipeline.state() == 2);
  CHECK(pipeline.commit_gpu_count == 0);

  jobs.clear();
  CHECK(loader.update(jobs) == 1);
  CHECK(jobs.empty());
  CHECK(pipeline.state() == 2);

  pipeline.render_commit();
  CHECK(pipeline.state() == 3);
  CHECK(pipeline.commit_gpu_count == 1);
  loader.external_done(&pipeline);

  CHECK(loader.update(jobs) == 0);
  CHECK(jobs.empty());
  CHECK(loader.pending_count() == 0);
}

TEST_CASE("resource_loader waits for dependencies before CPU-heavy pipeline prepare [demiurg]") {
  cpu_ready_resource shader_source;
  staged_pipeline_resource pipeline;
  pipeline.add_dependency(&shader_source);

  demiurg::resource_loader loader;
  std::vector<demiurg::resource_loader::external_job> jobs;

  loader.request(&pipeline, pipeline.final_state());

  CHECK(loader.update(jobs) == 2);
  CHECK(jobs.empty());
  CHECK(shader_source.usable());
  CHECK(shader_source.load_count == 1);
  CHECK(pipeline.state() == 0);
  CHECK(pipeline.read_config_count == 0);
  CHECK(pipeline.compile_cpu_count == 0);

  CHECK(loader.update(jobs) == 1);
  CHECK(jobs.empty());
  CHECK(pipeline.state() == 1);
  CHECK(pipeline.read_config_count == 1);

  CHECK(loader.update(jobs) == 1);
  CHECK(jobs.empty());
  CHECK(pipeline.state() == 2);
  CHECK(pipeline.compile_cpu_count == 1);
}
