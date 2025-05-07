#ifndef DEVILS_ENGINE_PAINTER_PIPELINES_CONFIG_STATIC_CONTAINER_H
#define DEVILS_ENGINE_PAINTER_PIPELINES_CONFIG_STATIC_CONTAINER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace devils_engine {
namespace painter {

struct graphics_pipeline_create_config;
struct compute_pipeline_create_config;
struct render_pass_data_t;
struct attachment_config_t;
struct sampler_config_t;

std::vector<std::string> available_default_graphics_pipeline_configs();
std::vector<std::string> available_default_compute_pipeline_configs();
std::vector<std::string> available_default_render_pass_configs();
std::vector<std::string> available_default_attachments_configs();
std::vector<std::string> available_default_samplers_configs();

const graphics_pipeline_create_config* get_default_graphics_pipeline_config(const std::string &name);
const compute_pipeline_create_config* get_default_compute_pipeline_config(const std::string &name);
const render_pass_data_t* get_default_render_pass_config(const std::string &name);
const std::vector<attachment_config_t> * get_default_attachments_config(const std::string &name);
const sampler_config_t* get_default_sampler_config(const std::string &name);
const void* get_default_descriptor_set_layout_configs();
const void* get_default_pipeline_layout_configs();

}
}

#endif