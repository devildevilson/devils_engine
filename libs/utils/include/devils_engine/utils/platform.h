#pragma once

#include <string>

namespace devils_engine::utils {
// windows, unix, macos
std::string app_path() noexcept;
std::string project_folder() noexcept;
std::string cache_folder() noexcept;
// имеет смысл добавить получение каких то цифр к этому
std::string get_cpu_name() noexcept;

} // namespace devils_engine::utils
