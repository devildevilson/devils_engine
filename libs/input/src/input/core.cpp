#include "core.h"

#include <cstdlib>

#if defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__)
#define system_open_url_command(url_str) ("start "+(url_str))
#elif __APPLE__
#define system_open_url_command(url_str) ("open "+(url_str))
#elif __linux__
#define system_open_url_command(url_str) ("xdg-open "+(url_str))
#else
#error "Unknown compiler"
#endif

#include "utils/core.h"

#define GLFW_INCLUDE_VULKAN
//#include "painter/vulkan_header.h"
#include "GLFW/glfw3.h"

#include "key_names.h"

namespace devils_engine {
namespace input {
static_assert(sizeof(icon_t) == sizeof(GLFWimage));
static_assert(alignof(icon_t) == alignof(GLFWimage));

init::init(error_callback callback) {
  if (!glfwInit()) utils::error("Could not init GLFW");
  glfwSetErrorCallback(callback);
  //if (!glfwVulkanSupported()) utils::error("Vulkan is not supported by this system");
}
init::~init() noexcept {
  glfwTerminate();
}

GLFWmonitor* primary_monitor() noexcept { return glfwGetPrimaryMonitor(); }

std::vector<GLFWmonitor*> monitors() noexcept {
  int count = 0;
  auto monitors = glfwGetMonitors(&count);
  std::vector<GLFWmonitor*> v(count);
  for (int i = 0; i < count; ++i) {
    v.push_back(monitors[i]);
  }

  return v;
}

std::tuple<uint32_t, uint32_t, uint32_t> primary_video_mode(GLFWmonitor* m) noexcept {
  const auto mode = glfwGetVideoMode(m);
  return std::make_tuple(mode->width, mode->height, mode->refreshRate);
}

std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> video_modes(GLFWmonitor *m) noexcept {
  int count = 0;
  const auto modes = glfwGetVideoModes(m, &count);
  std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> v(count);
  for (int i = 0; i < count; ++i) {
  const auto &mode = modes[i];
    v.push_back(std::make_tuple(mode.width, mode.height, mode.refreshRate));
  }

  return v;
}

std::tuple<uint32_t, uint32_t> monitor_physical_size(GLFWmonitor* m) noexcept {
  int w = 0, h = 0;
  glfwGetMonitorPhysicalSize(m, &w, &h);
  return std::make_tuple(w, h);
}

std::tuple<float, float> monitor_content_scale(GLFWmonitor* m) noexcept {
  float w = 0.0f, h = 0.0f;
  glfwGetMonitorContentScale(m, &w, &h);
  return std::make_tuple(w, h);
}

std::tuple<int32_t, int32_t> monitor_pos(GLFWmonitor *m) noexcept {
  int x = 0, y = 0;
  glfwGetMonitorPos(m, &x, &y);
  return std::make_tuple(x, y);
}

std::tuple<int32_t, int32_t, int32_t, int32_t> monitor_workarea(GLFWmonitor* m) noexcept {
  int w = 0, h = 0, x = 0, y = 0;
  glfwGetMonitorWorkarea(m, &x, &y, &w, &h);
  return std::make_tuple(x, y, w, h);
}

std::string_view monitor_name(GLFWmonitor *m) noexcept {
  return std::string_view(glfwGetMonitorName(m));
}

bool vulkan_supported() noexcept { return glfwVulkanSupported(); }
const char** get_required_instance_extensions(uint32_t* count) noexcept { return glfwGetRequiredInstanceExtensions(count); }
void init_vulkan_loader(PFN_vkGetInstanceProcAddr fn) noexcept { glfwInitVulkanLoader(fn); }
uint32_t get_physical_device_presentation_support(VkInstance i, VkPhysicalDevice p, uint32_t index) noexcept { return glfwGetPhysicalDevicePresentationSupport(i, p, index); }
uint32_t create_window_surface(VkInstance i, GLFWwindow* w, const void* ptr, VkSurfaceKHR* s) noexcept { return glfwCreateWindowSurface(i, w, (const VkAllocationCallbacks*)ptr, s); }

GLFWwindow* create_window(const uint32_t width, const uint32_t height, const std::string& name, GLFWmonitor* m, GLFWwindow* share) {
  if (m != nullptr) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
  glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_TRUE);
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  return glfwCreateWindow(width, height, name.c_str(), m, share);
}

void destroy(GLFWwindow* w) { glfwDestroyWindow(w); }
// glfwSetWindowMonitor
void hide(GLFWwindow* w) { glfwHideWindow(w); }
void show(GLFWwindow* w) { glfwShowWindow(w); }
bool should_close(GLFWwindow* w) noexcept { return glfwWindowShouldClose(w); }
std::tuple<uint32_t, uint32_t> window_size(GLFWwindow* m) noexcept {
  int w=0,h=0;
  glfwGetWindowSize(m, &w, &h);
  return std::make_tuple(w,h);
}

std::tuple<float, float> window_content_scale(GLFWwindow* m) noexcept {
  float w = 0.0f, h = 0.0f;
  glfwGetWindowContentScale(m, &w, &h);
  return std::make_tuple(w, h);
}

std::string_view window_title(GLFWwindow *m) noexcept {
  return std::string_view(glfwGetWindowTitle(m));
}

GLFWmonitor* window_monitor(GLFWwindow* m) noexcept {
  return glfwGetWindowMonitor(m);
}

void set_icon(GLFWwindow *m, const size_t count, const icon_t *icons) {
  glfwSetWindowIcon(m, count, reinterpret_cast<const GLFWimage*>(icons));
}

void set_window_callback(GLFWwindow* w, window_size_callback callback) { 
  glfwSetWindowSizeCallback(w, callback); 
}

void set_window_callback(GLFWwindow* w, window_content_scale_callback callback) {
  glfwSetWindowContentScaleCallback(w, callback);
}

void set_window_callback(GLFWwindow* w, window_refresh_callback callback) {
  glfwSetWindowRefreshCallback(w, callback);
}

void set_window_callback(GLFWwindow* w, key_callback callback) {
  glfwSetKeyCallback(w, callback);
}

void set_window_callback(GLFWwindow* w, character_callback callback) {
  glfwSetCharCallback(w, callback);
}

void set_window_cursor_pos_callback(GLFWwindow* w, cursor_position_callback callback) {
  glfwSetCursorPosCallback(w, callback);
}

void set_window_callback(GLFWwindow* w, cursor_enter_callback callback) {
  glfwSetCursorEnterCallback(w, callback);
}

void set_window_callback(GLFWwindow* w, mouse_button_callback callback) {
  glfwSetMouseButtonCallback(w, callback);
}

void set_window_callback(GLFWwindow* w, scroll_callback callback) {
  glfwSetScrollCallback(w, callback);
}

void set_window_callback(GLFWwindow *w, drop_callback callback) {
  glfwSetDropCallback(w, callback);
}

void poll_events() { glfwPollEvents(); }
int32_t key_scancode(const int32_t key) { return glfwGetKeyScancode(key); }
std::string_view key_name(const int32_t key, const int32_t scancode) {
  auto str = glfwGetKeyName(key, scancode);
  return std::string_view(str == nullptr ? "" : str);
}

std::string key_name_native(const int32_t, const int32_t scancode) {
  return get_key_name(scancode);
}

std::tuple<double, double> cursor_pos(GLFWwindow* m) {
  double x = 0.0, y = 0.0;
  glfwGetCursorPos(m, &x, &y);
  return std::make_tuple(x, y);
}

void set_cursor_input_mode(GLFWwindow* m, const int32_t mode) {
  glfwSetInputMode(m, GLFW_CURSOR, mode);
}

void set_raw_mouse_motion(GLFWwindow* m) {
  if (glfwRawMouseMotionSupported()) {
    glfwSetInputMode(m, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
  }
}

// настройки курсора, их может быть несколько, например это вполне нормально
// менять курсор во время игры в зависимости от ситуации
GLFWcursor *create_cursor(const icon_t &icon, const int32_t xhot, const int32_t yhot) {
  return glfwCreateCursor(reinterpret_cast<const GLFWimage*>(&icon), xhot, yhot);
}

GLFWcursor* create_default_cursor(const int32_t id) {
  return glfwCreateStandardCursor(id);
}

void destroy_cursor(GLFWcursor* cursor) {
  glfwDestroyCursor(cursor);
}

void set_cursor(GLFWwindow *m, GLFWcursor *cursor) {
  glfwSetCursor(m, cursor);
}

// поддержка джойстиков, пока не знаю стоит ли вкладываться
// поддержка геймпадов, пока не знаю стоит ли вкладываться

std::string_view clipboard_string(GLFWwindow* w) noexcept {
  return std::string_view(glfwGetClipboardString(w));
}
void set_clipboard_string(GLFWwindow* w, const std::string& str) noexcept {
  glfwSetClipboardString(w, str.c_str());
}

void open_internet_URL(const std::string &str) {
  const auto final_str = system_open_url_command(str);
  std::system(final_str.c_str());
}
}
}