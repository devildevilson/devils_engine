#ifndef DEVILS_ENGINE_INPUT_CORE_H
#define DEVILS_ENGINE_INPUT_CORE_H

#include <cstdint>
#include <cstddef>
#include <tuple>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
  // On Windows, Vulkan commands use the stdcall convention
  #define VKAPI_ATTR
  #define VKAPI_CALL __stdcall
  #define VKAPI_PTR  VKAPI_CALL
#elif defined(__ANDROID__) && defined(__ARM_ARCH) && __ARM_ARCH < 7
  #error "Vulkan is not supported for the 'armeabi' NDK ABI"
#elif defined(__ANDROID__) && defined(__ARM_ARCH) && __ARM_ARCH >= 7 && defined(__ARM_32BIT_STATE)
  // On Android 32-bit ARM targets, Vulkan functions use the "hardfloat"
  // calling convention, i.e. float parameters are passed in registers. This
  // is true even if the rest of the application passes floats on the stack,
  // as it does by default when compiling for the armeabi-v7a NDK ABI.
  #define VKAPI_ATTR __attribute__((pcs("aapcs-vfp")))
  #define VKAPI_CALL
  #define VKAPI_PTR  VKAPI_ATTR
#else
  // On other platforms, use the default calling convention
  #define VKAPI_ATTR
  #define VKAPI_CALL
  #define VKAPI_PTR
#endif

#define VK_DEFINE_HANDLE(object) typedef struct object##_T* object;
#define VK_DEFINE_NON_DISPATCHABLE_HANDLE(object) typedef struct object##_T *object;
VK_DEFINE_HANDLE(VkInstance)
VK_DEFINE_HANDLE(VkPhysicalDevice)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkSurfaceKHR)

typedef void (VKAPI_PTR* PFN_vkVoidFunction)(void);
typedef PFN_vkVoidFunction(VKAPI_PTR* PFN_vkGetInstanceProcAddr)(VkInstance instance, const char* pName);

// здесь отстутствует создание поверхности окна
// от рендера к нам что должно придти?

typedef struct GLFWmonitor GLFWmonitor;
typedef struct GLFWwindow GLFWwindow;
typedef struct GLFWcursor GLFWcursor;

// GLFW defines
#define DEVILS_ENGINE_INPUT_DONT_CARE (-1)
#define DEVILS_ENGINE_INPUT_KEY_UNKNOWN (-1)
#define DEVILS_ENGINE_INPUT_CURSOR_NORMAL 0x00034001
#define DEVILS_ENGINE_INPUT_CURSOR_HIDDEN 0x00034002
#define DEVILS_ENGINE_INPUT_CURSOR_DISABLED 0x00034003
#define DEVILS_ENGINE_INPUT_CURSOR_CAPTURED 0x00034004

namespace devils_engine {
namespace input {
typedef void (*error_callback)(int, const char*) noexcept;
typedef void (*window_size_callback)(GLFWwindow*, int, int) noexcept;
typedef void (*window_content_scale_callback)(GLFWwindow*, float, float) noexcept;
typedef void (*window_refresh_callback)(GLFWwindow*) noexcept;
typedef void (*key_callback)(GLFWwindow*, int, int, int, int) noexcept;
typedef void (*character_callback)(GLFWwindow*, unsigned int) noexcept;
typedef void (*cursor_position_callback)(GLFWwindow*, double, double) noexcept;
typedef void (*cursor_enter_callback)(GLFWwindow*, int) noexcept;
typedef void (*mouse_button_callback)(GLFWwindow*, int, int, int) noexcept;
typedef void (*scroll_callback)(GLFWwindow*, double, double) noexcept;
typedef void (*drop_callback)(GLFWwindow*, int, const char**) noexcept;

struct init {
  init(error_callback callback);
  ~init() noexcept;
};

// 32бит РГБА
struct icon_t {
  uint32_t width;
  uint32_t height;
  uint8_t* pixels;
};

GLFWmonitor* primary_monitor() noexcept;
std::vector<GLFWmonitor*> monitors() noexcept;
std::tuple<uint32_t, uint32_t, uint32_t> primary_video_mode(GLFWmonitor* m) noexcept;
std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> video_modes(GLFWmonitor* m) noexcept;
std::tuple<uint32_t, uint32_t> monitor_physical_size(GLFWmonitor* m) noexcept;
std::tuple<float, float> monitor_content_scale(GLFWmonitor* m) noexcept;
std::tuple<int32_t, int32_t> monitor_pos(GLFWmonitor* m) noexcept;
std::tuple<int32_t, int32_t, int32_t, int32_t> monitor_workarea(GLFWmonitor* m) noexcept;
std::string_view monitor_name(GLFWmonitor* m) noexcept;
// гамма, но возможно лучше задать ее в программном виде

bool vulkan_supported() noexcept;
const char** get_required_instance_extensions(uint32_t* count) noexcept;
void init_vulkan_loader(PFN_vkGetInstanceProcAddr fn) noexcept;
uint32_t get_physical_device_presentation_support(VkInstance i, VkPhysicalDevice p, uint32_t index) noexcept;
uint32_t create_window_surface(VkInstance i, GLFWwindow* w, const void* ptr, VkSurfaceKHR* s) noexcept;

GLFWwindow* create_window(const uint32_t width, const uint32_t height, const std::string &name, GLFWmonitor* m = nullptr, GLFWwindow* share = nullptr);
void destroy(GLFWwindow* w);
// glfwSetWindowMonitor
void hide(GLFWwindow* w);
void show(GLFWwindow* w);
bool should_close(GLFWwindow* w) noexcept;
std::tuple<uint32_t, uint32_t> window_size(GLFWwindow* m) noexcept;
std::tuple<float, float> window_content_scale(GLFWwindow* m) noexcept;
std::string_view window_title(GLFWwindow* m) noexcept;
GLFWmonitor* window_monitor(GLFWwindow* m) noexcept;
void set_icon(GLFWwindow* m, const size_t count, const icon_t* icons);

void set_window_callback(GLFWwindow* w, window_size_callback callback);
void set_window_callback(GLFWwindow* w, window_content_scale_callback callback);
void set_window_callback(GLFWwindow* w, window_refresh_callback callback);
void set_window_callback(GLFWwindow* w, key_callback callback);
void set_window_callback(GLFWwindow* w, character_callback callback);
void set_window_cursor_pos_callback(GLFWwindow* w, cursor_position_callback callback);
void set_window_callback(GLFWwindow* w, cursor_enter_callback callback);
void set_window_callback(GLFWwindow* w, mouse_button_callback callback);
void set_window_callback(GLFWwindow* w, scroll_callback callback);
void set_window_callback(GLFWwindow* w, drop_callback callback);

void poll_events();
int32_t key_scancode(const int32_t key);
std::string_view key_name(const int32_t key, const int32_t scancode);
std::string key_name_native(const int32_t key, const int32_t scancode);
std::tuple<double, double> cursor_pos(GLFWwindow* m);
void set_cursor_input_mode(GLFWwindow* m, const int32_t mode);
void set_raw_mouse_motion(GLFWwindow* m);

// настройки курсора, их может быть несколько, например это вполне нормально менять курсор во время игры в зависимости от ситуации
GLFWcursor* create_cursor(const icon_t &icon, const int32_t xhot, const int32_t yhot);
// посмотреть id курсора тут https://www.glfw.org/docs/latest/group__shapes.html
GLFWcursor* create_default_cursor(const int32_t id);

void destroy_cursor(GLFWcursor* cursor);
void set_cursor(GLFWwindow* m, GLFWcursor* cursor = nullptr);

// поддержка джойстиков, пока не знаю стоит ли вкладываться
// поддержка геймпадов, пока не знаю стоит ли вкладываться

std::string_view clipboard_string(GLFWwindow* w) noexcept;
void set_clipboard_string(GLFWwindow* w, const std::string& str) noexcept;

void open_internet_URL(const std::string &str);
}
}

#endif
