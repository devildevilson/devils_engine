#include "draw_stage.h"

#include "painter/vulkan_header.h"
#include "draw_resource.h"

// наверное нужно придумать формат данных в сторадж буфере 
// в котором я смогу указать вообще все что мне нужно
// или буферов будет много на разные типы картинок ....
// чем меньше буферов - тем проще жить
// че бы я хотел сейчас реализовать:
// 1) кламп/клип/растяжение/якорь картинки (но это кажется можно все сделать с помощью стандартного nk_image)
// 2) градиент
// 3) заливка части изображения по переменной
// 4) геральдика (то есть несколько слоев картинок с расположением)
// 5) сложный шрифт (граница вокруг шрифта + цвет + остальное)
// 6) трафарет

// самый охуенный способ это конечно соединить индекс картинки и индекс юзердаты в одну кучу
// тогда в принципе вопрос решаемый
// иногда в крайних случаях может помочь дубликаты данных
// вообще тут индексы можно положить в инстанс
// да и матрицу туда запихать
// да и другие данные тоже как будто
// но к сожалению все равно не все

namespace devils_engine {
namespace visage {
// вся буферизация будет находиться за пределами interface_draw
// я где то в другом месте буду составлять буфер и отдавать сюда данные
// нужно ли мне париться на счет двойной буферизации?
// я так думаю если у меня будут разные гпу и цпу буферы для этого дела
// то наверное нет, собрал данные, положил, скопировал и снова сижу собираю

void interface_draw::begin() {}

void interface_draw::process(VkCommandBuffer buffer) {
  vk::CommandBuffer b(buffer);

  // положим UBO + текстурки заранее
  // UBO для интерфейса будет вместе со сторадж буфером

  // кажется наклир пользуется 16бит индексами
  b.bindIndexBuffer(in->index, 0, vk::IndexType::eUint16);
  b.bindVertexBuffers(0u, vk::Buffer(in->vertex), 0ull);
  b.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, vk::PipelineLayout(pipe->pipeline_layout), in->set_slot, vk::DescriptorSet(in->set), nullptr); 
  b.bindPipeline(vk::PipelineBindPoint::eGraphics, pipe->pipeline);

  uint32_t el_offset = 0;
  uint32_t inst_offset = 0;
  for (const auto &cmd : in->cmds) {
    const uint32_t inst = (uint32_t(cmd.userdata_id) << 16) | uint32_t(cmd.texture_id);
    // а вот так нельзя !!!! адлыупзшэжщцфтжилзэьжмкулцтлджмкофзмтлдж
    // надо попробовать
    b.drawIndexed(cmd.count, 1, el_offset, 0, inst);
    el_offset += cmd.count;
    inst_offset += 1;
  }
}

void interface_draw::clear() {}
}
}