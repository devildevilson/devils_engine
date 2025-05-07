#ifndef DEVILS_ENGINE_PAINTER_SYSTEM_H
#define DEVILS_ENGINE_PAINTER_SYSTEM_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include "gtl/phmap.hpp"
#include "vulkan_minimal.h"
#include "primitives.h"
#include "pipeline_create_config.h"
#include "swapchain_resources.h"

namespace devils_engine {
namespace painter {

struct container;
struct attachments_container;
struct layouting;
class arbitrary_image_container; // sampler2D[]
class hierarchical_image_container; // sampler2D[]
class image_pool; // texture2DArray[]
// + этому надо сам сэмплер добавить
// или может быть все таки отказаться от семлеров в контейнерах вообще?
// мы можем отказаться от семплеров в контейнерах вообще и оставить только texture2D[] и texture2DArray[]
// так нам нужно будет определить откуда мы получаем картинку...
// хотя опять же у нас есть только четкое разделение на эррей и не эррей
// ВСЕ! теперь все контейнеры возвращают текстурки вида sampler2D
// осталось только примерно понять можно ли сделать в этих же контейнерах сторадж имейджы?

class simple_swapchain;

// тут что по идее создаем все ресурсы и даем несколкьо полезных функций наружу
// например зададим ряд интерфейсов и здесь их положим все в свои массивы
// выкинем наружу несколько функций которые пройдутся по интерфейсам и запустят функцию

// этот класс оставим чисто для хранения разных штук и возьни с конфигом?
// этот класс должен мне помочь настроить зависимости между разными частями рендера
// здесь будем все хранить + займемся конфигом + через него наверное будем запускать рендер 
// чтобы учесть разные штуки в том числе vmaSetCurrentFrameIndex
// да, здесь же можно сделать обновление экрана
// + перекомпиляцию шейдеров (пересборку пиплина)

// осталось практически вот что: у меня явно в кадре есть 
// какая то работа которая должна быть синхронизирована с цпу
// это например стриминг ресурсов на видеокарту
// например мне нужно перекопировать данные с хоста на гпу
// это дело должно контроллироваться за счет vkevent
// мне явно нужен какой то стейдж который

// вот еще что: тяжелый стриминг ресурсов должен быть вообще в отдельном кью
// или даже в отдельной кью фемели? (хотя вряд ли)
// но точно отдельным сабмитом
// таким образом этот стриминг может происходить несколько кадров
// пока мы показываем то что уже загружено игроку
// для этого нужно заранее понять какие ресурсы нуждаются в загрузке
// как? это видимо явно будут ресурсы которые расположены на карте 
// в некотором удалении от игрока, и так мы примерно поймем что нужно догрузить

// обязательно где то надо еще делать вот это vmaSetCurrentFrameIndex
class system {
public:
  struct system_create_info {
    // тут бы указать разные размеры всяких штук
  };

  system();
  ~system() noexcept;

  // как тут быть с конфигом? я бы так сказал: у нас должна быть возможность остановить эту систему 
  // удалить ее полностью и пересоздать и запустить вновь
  // должна ли загрузка происходить только в основном потоке? не обязательно
  // загрузка тащемта это набор функций которые выполняются одна за другой + синхронизация во время выполнения

  template <typename... Args>
  container* create_main_container(Args&&... args) {
    main_container.reset(new container(std::forward<Args>(args)...));
    return main_container.get();
  }

  template <typename T, typename... Args>
  T* create_image_container(Args&&... args) {
    auto std_ptr = std::make_unique<T>(std::forward<Args>(args)...);
    auto ptr = std_ptr.get();
    image_containers.emplace_back(std::move(std_ptr));
    return ptr;
  }

  template <typename T, typename... Args>
  T *create(Args&&... args) {
    auto ptr = std::make_unique<T>(std::forward<Args>(args)...);
    auto p = ptr.get();
    objects.push_back(std::move(ptr));

    if constexpr (std::derived_from<T, recreate_target>) {
      if (recreating == nullptr) recreating = p;
      else recreating->set_next(p);
    } 
    
    /*if constexpr (std::derived_from<T, submit_target>) {
      if (submiting == nullptr) submiting = p;
      else submiting->add(p);
    } */
    
    if constexpr (std::derived_from<T, recompile_shaders_target>) {
      if (recompiling_shaders == nullptr) recompiling_shaders = p;
      else recompiling_shaders->set_next(p);
    }

    if constexpr (std::is_same_v<T, surface_container>) {
      surface = p->surface;
    }
    
    return p;
  }

  // количество нужно указать
  /*template <typename... Args>
  layouting* create_layouting(Args&&... args) {
    graphics_layout.reset(new layouting(std::forward<Args>(args)...));
    return graphics_layout.get();
  }

  template <typename... Args>
  attachments_container* create_attachment_container(Args&&... args) {
    attachments.reset(new attachments_container(std::forward<Args>(args)...));
    return attachments.get();
  }*/

  template <typename T, typename... Args>
  T* create_frame_presenter(Args&&... args) {
    auto std_ptr = std::make_unique<T>(std::forward<Args>(args)...);
    auto ptr = std_ptr.get();
    frames_presenters.emplace_back(std::move(std_ptr));
    return ptr;
  }

  uint32_t recompile_shaders();
  void recreate(const uint32_t width, const uint32_t height);

  // блин функция сильно зависит от того существует ли сюрфейс, 
  // было бы неплохо откуда то из вне получать размер окна
  uint32_t wait_frame(const size_t timeout) const;
  uint32_t compute_frame();

  VkInstance get_instance() const;
  VkDevice get_device() const;
  container* get_main_container() const;
  void set_window_surface(VkSurfaceKHR surface);

  const graphics_pipeline_create_config* get_graphics_pipeline_config(const std::string &name) const;
  const compute_pipeline_create_config* get_compute_pipeline_config(const std::string &name) const;
  const render_pass_data_t* get_render_pass_config(const std::string &name) const;
  const std::vector<attachment_config_t> & get_attachments_config(const std::string &name) const;
  const sampler_config_t* get_sampler_config(const std::string &name) const;
  const descriptor_set_layouts_config_t* get_set_layouts_configs() const;
  const pipeline_layouts_t* get_pipeline_layouts_configs() const;
  void dump_configs_to_disk() const;
  void reload_configs();

private:
  std::unique_ptr<container> main_container;

  // было бы неплохо по аналогии с изображениями сделать буферы
  // с буферами попроще - у них только 3 параметра: сам буфер, отступ и размер
  // другое дело что потребуется наверное опять заводить несколько способов аллокации
  std::vector<std::unique_ptr<image_container>> image_containers;

  //std::unique_ptr<layouting> graphics_layout;
  //std::unique_ptr<arbitrary_image_container> any_images;
  //std::unique_ptr<hierarchical_image_container> block_images;
  //std::unique_ptr<image_pool> array32_images;
  //std::unique_ptr<image_pool> array512_images; // ?

  //std::unique_ptr<attachments_container> attachments;
  // надо также выкинуть функции создания изображения
  
  // сюда нужно передать окно
  VkSurfaceKHR surface;
  //std::unique_ptr<simple_swapchain> swapchain;

  std::vector<std::unique_ptr<arbitrary_data>> objects;
  recreate_target* recreating;
  //submit_target* submiting; // так указать это дело НЕЛЬЗЯ, желательно убрать из аттачмент контейнера swapchain
  recompile_shaders_target* recompiling_shaders; 

  size_t submiter_counter;
  std::vector<std::unique_ptr<present_target>> frames_presenters;

  // система должна подгрузить все конфиги всех вулкан штук и положить их где то здесь
  // + система подгрузит конфиги из кода + подгрузит конфиги с диска (диск по приоритету)
  gtl::node_hash_map<std::string, graphics_pipeline_create_config> graphics_pipeline_configs;
  gtl::node_hash_map<std::string, compute_pipeline_create_config> compute_pipeline_configs;
  gtl::node_hash_map<std::string, render_pass_data_t> render_pass_configs;
  gtl::node_hash_map<std::string, std::vector<attachment_config_t>> attachments_configs;
  gtl::node_hash_map<std::string, sampler_config_t> sampler_configs;
  descriptor_set_layouts_config_t set_layouts_configs;
  pipeline_layouts_t pipeline_layouts_configs;
};
}
}

#endif