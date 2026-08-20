#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "devils_engine/painter/common.h"
#include "devils_engine/painter/structures.h"

using namespace devils_engine;

namespace {

// Модель ротации копий, ровно как её видит update_descriptors: индекс копии, отстоящей на frames_back
// кадров назад, при часах clock и общем числе копий.
uint32_t copy_at(const uint32_t clock, const uint32_t frames_back, const uint32_t copies) {
  return painter::history_copy_index(clock, frames_back, copies);
}

// Есть ли гонка при данном числе копий: кадр N пишет копию, которую кто-то ещё читает или пишет.
// Кадры N-1 … N-frames_in_flight+1 считаются «в полёте» — их команды могут выполняться параллельно.
bool has_collision(const uint32_t frames_in_flight, const uint32_t history, const uint32_t copies) {
  constexpr uint32_t simulated_frames = 64;
  for (uint32_t frame = frames_in_flight + history; frame < simulated_frames; ++frame) {
    const uint32_t target = copy_at(frame, 0, copies);

    for (uint32_t back = 1; back < frames_in_flight; ++back) {
      const uint32_t in_flight = frame - back;
      // писать в копию, которую ещё пишет кадр в полёте, нельзя
      if (copy_at(in_flight, 0, copies) == target) {
        return true;
      }
      // и в ту, которую он читает как историю, тоже
      for (uint32_t j = 1; j <= history; ++j) {
        if (copy_at(in_flight, j, copies) == target) {
          return true;
        }
      }
    }

    // история обязана дожить до чтения: копию, прочитанную как «j кадров назад», не должен был
    // перезаписать никто из кадров после её записи
    for (uint32_t j = 1; j <= history; ++j) {
      const uint32_t source = copy_at(frame, j, copies);
      for (uint32_t writer = frame - j + 1; writer <= frame; ++writer) {
        if (copy_at(writer, 0, copies) == source) {
          return true;
        }
      }
    }
  }
  return false;
}

} // namespace

TEST_CASE("history binding shows a window of history, never the copy being written [painter]") {
  // Без истории binding — одиночный дескриптор текущей копии: никакого массива в шейдере
  const painter::descriptor::binding current(0, painter::usage::sampled, painter::invalid_resource_slot, 0, 0);
  CHECK(current.array_size() == 1);
  CHECK(current.frames_back(0) == 0);

  // С историей — окно из ровно N копий, и текущая в него НЕ входит: она лежит в writable layout, а копии
  // истории зафиксированы в read-only, и один binding не может обещать оба layout сразу.
  const painter::descriptor::binding window(0, painter::usage::sampled, painter::invalid_resource_slot, 0, 2);
  CHECK(window.array_size() == 2);
  CHECK(window.frames_back(0) == 1);
  CHECK(window.frames_back(1) == 2);
}

TEST_CASE("history window indices address exactly the previous frames [painter]") {
  constexpr uint32_t copies = 4; // frames_in_flight 3 + history 1
  const painter::descriptor::binding window(0, painter::usage::sampled, painter::invalid_resource_slot, 0, 1);

  for (uint32_t clock = 0; clock < 12; ++clock) {
    const uint32_t current = copy_at(clock, 0, copies);
    const uint32_t previous = copy_at(clock, window.frames_back(0), copies);
    CHECK(previous != current);
    // копия, которую читает кадр clock, — это та, в которую писал кадр clock-1
    CHECK(previous == copy_at(clock - 1, 0, copies));
  }
}

TEST_CASE("copies = counter period + history is the minimum that has no races [painter]") {
  for (uint32_t frames = 1; frames <= 4; ++frames) {
    for (uint32_t history = 1; history <= 3; ++history) {
      const uint32_t derived = frames + history;
      CHECK_FALSE(has_collision(frames, history, derived));
      // и это именно МИНИМУМ: одной копией меньше — уже гонка, поэтому число копий выводится, а не
      // назначается «с запасом»
      CHECK(has_collision(frames, history, derived - 1));
    }
  }
}

TEST_CASE("resource copy count comes from the period plus reader-declared history [painter]") {
  painter::resource res;
  res.type = painter::type::values::frames_in_flight;

  constexpr uint32_t frames_in_flight = 3;
  constexpr uint32_t swapchain_images = 3;

  res.history_depth = 0;
  CHECK(res.compute_buffering(frames_in_flight, swapchain_images) == frames_in_flight);

  res.history_depth = 1;
  CHECK(res.compute_buffering(frames_in_flight, swapchain_images) == frames_in_flight + 1);

  res.history_depth = 2;
  CHECK(res.compute_buffering(frames_in_flight, swapchain_images) == frames_in_flight + 2);

  // период — свойство счётчика, а не глубины конвейера: ресурс на swapchain-счётчике крутится по числу
  // образов и историю прибавляет к нему же
  res.type = painter::type::values::swapchain;
  res.history_depth = 1;
  CHECK(res.compute_buffering(frames_in_flight, swapchain_images) == swapchain_images + 1);
}

TEST_CASE("constant memory offsets are byte offsets into a word array [painter]") {
  // Регресс на реальный баг: constant::offset задаётся в БАЙТАХ, а память констант — массив uint32_t.
  // get/write_constant_data прибавляли смещение к uint32_t*, то есть уходили вчетверо дальше. Первая
  // константа (offset 0) при этом работала, поэтому ошибка жила до первого конфига с двумя константами.
  std::vector<uint32_t> memory(16, 0);
  const size_t byte_offset = 12; // вторая dispatch3-константа

  const auto write_at = [&](const size_t offset, const uint32_t value) {
    *(memory.data() + offset / sizeof(uint32_t)) = value;
  };

  write_at(byte_offset, 7u);
  CHECK(memory[3] == 7u);  // 12 байт = слово 3
  CHECK(memory[12] == 0u); // а не слово 12
}

TEST_CASE("mip chain length is derived from the level 0 extent [painter]") {
  painter::resource res;
  res.role = painter::role::hdr_color;

  // Явное число уровней уважается как есть: это решение автора про память и качество
  res.mips = 4;
  CHECK(res.compute_mip_levels({1280, 720, 1}) == 4);
  CHECK(res.compute_mip_levels({64, 64, 1}) == 4);

  // 'auto' (mips == 0) — полная цепочка до 1x1 по БОЛЬШЕЙ стороне, с потолком max_mip_levels
  res.mips = 0;
  CHECK(res.compute_mip_levels({1, 1, 1}) == 1);
  CHECK(res.compute_mip_levels({2, 1, 1}) == 2);
  CHECK(res.compute_mip_levels({8, 8, 1}) == 4);   // 8 -> 4 -> 2 -> 1
  CHECK(res.compute_mip_levels({640, 360, 1}) == 10); // 640 -> 320 -> ... -> 2 -> 1, девять делений
  CHECK(res.compute_mip_levels({65536, 1, 1}) == painter::max_mip_levels);

  // У объёмной картинки уровень делит и глубину, поэтому она входит в максимум наравне с шириной и высотой:
  // цепочка, посчитанная только по двум осям, оставила бы уровни, у которых z уже равен единице.
  CHECK(res.compute_mip_levels({8, 8, 32}) == 6); // 32 -> 16 -> 8 -> 4 -> 2 -> 1
  CHECK(res.compute_mip_levels({32, 32, 32}) == 6);
}

TEST_CASE("depth above one means a volume image, and a volume image cannot use layers [painter]") {
  // «Глубина больше единицы» и «трёхмерная картинка» — одно утверждение, поэтому тип образа выводится из
  // размера, а не объявляется отдельно: два места для одного факта разошлись бы.
  CHECK_FALSE(painter::extent{32, 32, 1}.is_volume());
  CHECK_FALSE(painter::extent{1024, 32, 1}.is_volume());
  CHECK(painter::extent{32, 32, 32}.is_volume());
  CHECK(painter::extent{1, 1, 2}.is_volume());

  // Следствие, из которого и вырос RND-49: буферизация картинок здесь сделана СЛОЯМИ одного образа, а Vulkan
  // запрещает слои у трёхмерного образа (VUID-VkImageCreateInfo-imageType-00961). Значит копии объёмного
  // ресурса обязаны лежать в разных образах — это не оптимизация, а единственный законный вариант.
  painter::resource_container flat;
  flat.extent = {1024, 32, 1};
  flat.layers = 3;
  CHECK_FALSE(flat.extent.is_volume()); // три копии слоями — законно

  painter::resource_container volume;
  volume.extent = {32, 32, 32};
  volume.layers = 1;
  CHECK(volume.extent.is_volume()); // копия = отдельный контейнер, поэтому слой ровно один
}

TEST_CASE("a conditional counter may advance no faster than the cache has copies [painter]") {
  // Условный пасс пишет копию, выбранную значением счётчика. Живых поколений не может быть больше, чем копий:
  // иначе кадр в полёте читает копию, которую уже перезаписали. Отсюда минимальный разрыв между сдвигами —
  // он ВЫВОДИТСЯ из числа копий, а не объявляется автором отдельным полем.
  // Разрыв считается от ПЕРИОДА, а не от полного числа копий: копии истории заняты прошлыми поколениями и
  // свободными для записи не являются, поэтому история не имеет права ослаблять ограничение. А если кэш ещё и
  // читается как история, разрыв поднимается до frames_in_flight — тогда прошлое поколение записано за
  // горизонтом фенса кадра, его запись гарантированно завершена, и кросс-кадровый семафор не нужен (вывести
  // его и невозможно: писало поколение не обязательно предыдущим кадром).
  const auto min_gap = [](const uint32_t frames_in_flight, const uint32_t period, const uint32_t history = 0) {
    const uint32_t window = frames_in_flight > 1 ? frames_in_flight - 1 : 1;
    const uint32_t gap = (window + period - 2) / (period - 1);
    return history > 0 ? std::max(gap, frames_in_flight) : gap;
  };

  // Копий столько же, сколько кадров в полёте — двигать можно каждый кадр, то есть ограничения нет
  CHECK(min_gap(3, 3) == 1);
  CHECK(min_gap(2, 2) == 1);
  CHECK(min_gap(4, 4) == 1);

  // Две копии при трёх кадрах в полёте: не чаще раза в два кадра. Это тот самый doublebuffer, и он не
  // «оптимизация», а нижняя граница — одной копии не хватает никогда.
  CHECK(min_gap(3, 2) == 2);
  CHECK(min_gap(4, 2) == 3);
  CHECK(min_gap(4, 3) == 2);

  // История поколений стоит разрыва во весь конвейер: иначе прошлое поколение может быть ещё в полёте
  CHECK(min_gap(3, 3, 1) == 3);
  CHECK(min_gap(3, 2, 1) == 3);
  CHECK(min_gap(4, 2, 1) == 4);
  // и она не имеет права ослаблять ограничение: считать разрыв от периода+истории было бы ошибкой
  CHECK(min_gap(3, 2, 1) > min_gap(3, 3));

  // Модель прямой проверкой: при разрыве gap среди кадров в полёте живо не больше copies поколений
  for (uint32_t frames = 2; frames <= 4; ++frames) {
    for (uint32_t copies = 2; copies <= frames; ++copies) {
      const uint32_t gap = min_gap(frames, copies);
      uint32_t alive = 1; // поколение, которое пишет текущий кадр
      for (uint32_t back = 1; back < frames; ++back) {
        if (back % gap == 0) {
          alive += 1; // на этом кадре был сдвиг, значит кадры до него читают другое поколение
        }
      }
      CHECK(alive <= copies);
    }
  }
}

TEST_CASE("a storage binding must name its mip level [painter]") {
  // Требование Vulkan, а не соглашение: у imageLoad/imageStore нет параметра LOD, поэтому storage-вид
  // покрывает ровно один уровень. Биндинг это и отражает: 'mip' обязателен для пишущих юсаджей на цепочке.
  const painter::descriptor::binding chain_read(0, painter::usage::sampled, painter::invalid_resource_slot, 0);
  CHECK(chain_read.mip == painter::invalid_resource_slot);

  const painter::descriptor::binding level_write(0, painter::usage::texel_write, painter::invalid_resource_slot, 0, 0, 2);
  CHECK(level_write.mip == 2);
}
