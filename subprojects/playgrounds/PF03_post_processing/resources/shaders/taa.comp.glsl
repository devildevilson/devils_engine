#version 450

// Алгоритм: общий temporal resolve для TAA и TAAU. История репроецируется motion vector'ом, выбирается
// Catmull-Rom, сжимается по яркости и ограничивается 3x3 окрестностью в YCoCg, чтобы яркие disocclusion не
// оставляли шлейф. При полном разрешении действует обычный фиксированный history weight. При upscale каждый
// jittered source sample вносится в соседние display-пиксели непрерывным tent-footprint: центр получает
// полный вес, к краю одного render-текселя вес плавно падает. Взвешенный per-pixel coverage count задаёт вес
// среднего, поэтому движение не переключает пиксель скачком между «чистой историей» и spatial fallback.

#include "pf03_frame.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform FrameBlock {
  PF03_FRAME_BLOCK_BODY
} frame;

layout(set = 1, binding = 0) uniform sampler2D depth_image;
layout(set = 1, binding = 1) uniform sampler2D normal_image;
layout(set = 1, binding = 2) uniform sampler2D motion_image;

layout(set = 2, binding = 0) uniform sampler2D current_image;

layout(set = 3, binding = 0, rgba16f) uniform writeonly image2D taa_output;
// Прошлый НАКОПЛЕННЫЙ кадр ('history = 1' у того же ресурса): копится результат накопления, а не сырой кадр
layout(set = 3, binding = 1) uniform sampler2D taa_history;
// Метаданные живут отдельно от цвета: alpha кадра сохраняет пропускание тумана.
// .r = накопленный эффективный вес TAAU-отсчётов, .g = доля отвергнутой истории.
layout(set = 3, binding = 2, rg16f) uniform writeonly image2D taa_meta_output;
layout(set = 3, binding = 3) uniform sampler2D taa_meta_history;

void main() {
  const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  const ivec2 size = imageSize(taa_output);
  if (pixel.x >= size.x || pixel.y >= size.y) {
    return;
  }

  const vec2 uv = (vec2(pixel) + 0.5) / vec2(size);

  // Вход может быть мельче цели. Catmull-Rom даёт spatial estimate между source samples, но temporal resolve
  // не считает её полным новым наблюдением: доверие задаёт расстояние до ближайшей фактической jittered
  // позиции, а история со временем собирает complementary phases.
  const ivec2 render_size = textureSize(current_image, 0);
  const bool upsampling = render_size.x != size.x || render_size.y != size.y;
  const float current_transmittance = texture(current_image, uv).a;

  // Выключенный TAA либо нулевой вес — честный passthrough и контрольный простой upscale. Coverage-ветка
  // не должна менять его даже в пикселях, куда в эту фазу попал настоящий source-отсчёт: иначе `--taa=0`
  // и `--taa-weight=0` перестают быть одной и той же базой A/B.
  if (frame.taa_params.z <= 0.5 || frame.taa_params.x <= 0.0) {
    const vec3 passthrough = upsampling
      ? pf03_sample_catmull_rom(current_image, uv, vec2(render_size))
      : texture(current_image, uv).rgb;
    imageStore(taa_output, pixel, vec4(passthrough, current_transmittance));
    imageStore(taa_meta_output, pixel, vec4(0.0));
    return;
  }

  // Позиция ближайшего настоящего отсчёта рендера в сетке дисплея. Проекция сдвигает геометрию на -jitter,
  // а измеренный знак покрытия здесь обратный: значение source-текселя принадлежит позиции center - jitter.
  // При другом знаке RMSE к full-resolution эталону вырос с 3.76 до 6.87 уровня из 255.
  //
  // Первая версия считала coverage БУЛЕВЫМ: один display-пиксель получал sample целиком, соседний не получал
  // ничего и держал историю с весом 1. При малейшем движении граница этой маски перескакивала через силуэт,
  // и кромка заметно «достраивалась». Здесь sample имеет tent-footprint размером в один render-тексель:
  // вклад меняется непрерывно вместе с его положением, а не прыгает между 0 и 1.
  const vec2 source_position = (uv + frame.taa_jitter.xy) * vec2(render_size) - 0.5;
  const ivec2 source_pixel = clamp(
    ivec2(floor(source_position + 0.5)), ivec2(0), render_size - 1);
  const vec2 sample_uv = (vec2(source_pixel) + 0.5) / vec2(render_size) - frame.taa_jitter.xy;
  const vec2 footprint_pixels = vec2(size) / vec2(render_size);
  const vec2 sample_offset_pixels = sample_uv * vec2(size) - (vec2(pixel) + 0.5);
  const vec2 coverage_axis = clamp(
    vec2(1.0) - abs(sample_offset_pixels) / max(footprint_pixels, vec2(1.0)), vec2(0.0), vec2(1.0));
  const float current_coverage = upsampling ? coverage_axis.x * coverage_axis.y : 1.0;

  // Spatial estimate адресуется с jitter-поправкой: нужен цвет в текущем display-пикселе, а source grid
  // физически сдвинут относительно него. В центре footprint берём настоящий texel без фильтра; к краям
  // плавно переходим к Catmull-Rom, чтобы footprint не превращался в увеличенный квадрат source-пикселя.
  const vec2 aligned_uv = clamp(uv + frame.taa_jitter.xy, vec2(0.0), vec2(1.0));
  const vec3 aligned_reconstructed = upsampling
    ? pf03_sample_catmull_rom(current_image, aligned_uv, vec2(render_size))
    : texture(current_image, uv).rgb;
  const vec3 source_sample = texelFetch(current_image, source_pixel, 0).rgb;
  const vec3 current = mix(aligned_reconstructed, source_sample, current_coverage);
  // Transmittance — свойство текущей сцены, не temporal confidence: её реконструируем пространственно и
  // никогда не смешиваем с историей, иначе движущийся силуэт оставляет шлейф уже в самом тумане.

  // TAA выключен либо истории ещё нет — отдаём текущий кадр как есть. Это и есть passthrough: без него
  // первые кадры после сброса подмешивали бы пустую историю и кадр выходил бы темнее.
  // taa_params.z: 0 выключен, 1 включён с bilinear-выборкой истории, 2 включён с Catmull-Rom
  const bool history_valid = frame.viewport_near.w > 1.5 && frame.taa_params.z > 0.5;
  if (!history_valid) {
    imageStore(taa_output, pixel, vec4(current, current_transmittance));
    imageStore(taa_meta_output, pixel, vec4(current_coverage, 0.0, 0.0, 0.0));
    return;
  }

  // Motion-вектор уже геометрический: джиттер из него вычтен на этапе G-buffer, иначе субпиксельное дрожание
  // проектора протекло бы в вектор и TAA боролся бы сам с собой.
  const vec2 motion = texture(motion_image, uv).rg;
  const vec2 history_uv = uv + motion;

  // Выход за кадр — это disocclusion: истории для этого пикселя физически не существует
  const bool inside = all(greaterThanEqual(history_uv, vec2(0.0))) && all(lessThanEqual(history_uv, vec2(1.0)));
  if (!inside) {
    imageStore(taa_output, pixel, vec4(current, current_transmittance));
    imageStore(taa_meta_output, pixel, vec4(current_coverage, 0.0, 0.0, 0.0));
    return;
  }

  const float history_count = texture(taa_meta_history, history_uv).r;
  // При полном разрешении сохраняется старый контракт TAA (глобально валидная история). При TAAU нулевой
  // счётчик означает, что этот display-пиксель ещё ни разу не получил настоящий source-отсчёт: использовать
  // его пространственный fallback как историю означало бы снова накапливать один и тот же upscale.
  if (upsampling && history_count <= 0.0) {
    imageStore(taa_output, pixel, vec4(current, current_transmittance));
    imageStore(taa_meta_output, pixel, vec4(current_coverage, 0.0, 0.0, 0.0));
    return;
  }

  const vec3 current_compressed = pf03_range_compress(current);
  // Фильтр выборки истории — не деталь: под движением она переинтерполируется каждый кадр, и bilinear
  // накапливает размытие. Тумблер оставлен данными, потому что разницу надо было измерить.
  const vec3 history_sample = frame.taa_params.z > 1.5
    ? pf03_sample_catmull_rom(taa_history, history_uv, vec2(size))
    : texture(taa_history, history_uv).rgb;
  const vec3 history_raw = pf03_range_compress(history_sample);

  const vec3 current_ycocg = pf03_rgb_to_ycocg(current_compressed);
  vec3 history_ycocg = pf03_rgb_to_ycocg(history_raw);

  // Окрестность текущего кадра собирается и как коробка min/max, и как первые два момента: жёсткая коробка
  // и клип по дисперсии — разные компромиссы, и площадка обязана уметь показать оба.
  vec3 neighbourhood_min = current_ycocg;
  vec3 neighbourhood_max = current_ycocg;
  vec3 moment1 = current_ycocg;
  vec3 moment2 = current_ycocg * current_ycocg;
  for (int y = -1; y <= 1; ++y) {
    for (int x = -1; x <= 1; ++x) {
      if (x == 0 && y == 0) {
        continue;
      }
      // Окрестность берётся в текселях ИСТОЧНИКА: при подъёме разрешения координата пикселя дисплея не
      // адресует низкое разрешение, и коробка отбраковки собиралась бы не по тем соседям.
      const ivec2 tap = clamp(ivec2(uv * vec2(render_size)) + ivec2(x, y), ivec2(0), render_size - 1);
      const vec3 neighbour = pf03_rgb_to_ycocg(pf03_range_compress(texelFetch(current_image, tap, 0).rgb));
      neighbourhood_min = min(neighbourhood_min, neighbour);
      neighbourhood_max = max(neighbourhood_max, neighbour);
      moment1 += neighbour;
      moment2 += neighbour * neighbour;
    }
  }

  const int reject_mode = int(frame.taa_params.y + 0.5);
  vec3 lower = neighbourhood_min;
  vec3 upper = neighbourhood_max;
  if (reject_mode == 3) {
    // Отбраковка, зависящая от СКОРОСТИ. Смысл: надёжность истории определяется тем, насколько далеко её
    // пришлось тащить. При почти стоящей камере репроекция попадает почти в тот же тексель, история верна
    // практически всегда — и жёсткая коробка там только возвращает ступеньку на далёких кромках, потому что
    // на тонкой кромке диапазон девяти соседей узкий. При быстром движении наоборот: история почти наверняка
    // с другой поверхности, и коробку надо сжимать.
    const float motion_pixels = length(motion * vec2(size));
    const float gamma = mix(4.0, 1.0, clamp(motion_pixels / 4.0, 0.0, 1.0));
    const vec3 mean = moment1 / 9.0;
    const vec3 sigma = sqrt(max(moment2 / 9.0 - mean * mean, vec3(0.0)));
    lower = mean - gamma * sigma;
    upper = mean + gamma * sigma;
  } else if (reject_mode == 2) {
    // Клип по дисперсии. Жёсткий min/max отбраковывает историю всякий раз, когда она вышла за диапазон
    // девяти соседей, а при движении камеры репроекция почти всегда попадает между текселями — и тогда на
    // далёкой тонкой кромке, где диапазон узок, выход схлопывается к неусреднённому кадру и ступенька
    // выступает наружу. Коробка mean +- gamma*sigma допускает правдоподобное отклонение и отбраковывает
    // только то, что действительно выпадает из распределения.
    const vec3 mean = moment1 / 9.0;
    const vec3 sigma = sqrt(max(moment2 / 9.0 - mean * mean, vec3(0.0)));
    const float gamma = 1.25;
    lower = max(neighbourhood_min, mean - gamma * sigma);
    upper = min(neighbourhood_max, mean + gamma * sigma);
  }

  // Клип ВДОЛЬ ЛУЧА от текущего значения к истории, а не поканальное защемление. Поканальное защемление
  // независимо ломает каждый канал и теряет истории больше, чем нужно: сдвигается и оттенок. Здесь ищется
  // максимальная доля пути к истории, при которой значение ещё внутри коробки, — цвет сохраняет направление.
  vec3 clipped = clamp(history_ycocg, lower, upper);
  if (reject_mode == 3) {
    const vec3 direction = history_ycocg - current_ycocg;
    vec3 limit = vec3(1.0);
    for (int c = 0; c < 3; ++c) {
      if (abs(direction[c]) > 1.0e-6) {
        const float to_upper = (upper[c] - current_ycocg[c]) / direction[c];
        const float to_lower = (lower[c] - current_ycocg[c]) / direction[c];
        limit[c] = max(min(to_upper, to_lower) > 0.0 ? min(to_upper, to_lower) : max(to_upper, to_lower), 0.0);
      }
    }
    const float travel = clamp(min(min(limit.x, limit.y), limit.z), 0.0, 1.0);
    clipped = current_ycocg + direction * travel;
  }
  // Насколько историю пришлось подтянуть — это и есть мера «истории здесь верить нельзя». Показывается
  // отдельным видом: на движущихся силуэтах она обязана вспыхивать, на статике быть нулём.
  const float rejection = length(clipped - history_ycocg) / max(length(history_ycocg), 1.0e-3);
  history_ycocg = reject_mode > 0 ? clipped : history_ycocg;
  const vec3 history_compressed = pf03_ycocg_to_rgb(history_ycocg);

  const float requested_weight = clamp(frame.taa_params.x, 0.0, 0.98);
  float weight = requested_weight;
  float next_count = 1.0;
  if (upsampling) {
    // Счётчик — орган управления реконструкцией: накопленный эффективный вес n получает долю n/(n+w),
    // новый footprint весом w — долю w/(n+w). Сумма ограничена значением, эквивалентным обычному history
    // weight, поэтому после сходимости TAAU не превращается в бесконечное среднее с весом 0.99+.
    const float confidence = 1.0 - clamp(rejection, 0.0, 1.0);
    const float trusted_count = history_count * confidence;
    const float count_limit = requested_weight / max(1.0 - requested_weight, 1.0e-4);
    if (current_coverage > 1.0e-3 && requested_weight > 0.0) {
      // Дробный coverage — дробный новый sample. По мере движения footprint вклад текущего кадра и вес
      // истории меняются непрерывно; count хранит сумму эффективных весов, а не число бинарных попаданий.
      weight = min(
        requested_weight, trusted_count / max(trusted_count + current_coverage, 1.0e-4));
      next_count = min(trusted_count + current_coverage, count_limit);
    } else if (trusted_count > 0.0 && requested_weight > 0.0) {
      // За пределом footprint переносим репроецированную историю. На штатных масштабах tent'ы соседних
      // source samples перекрывают экран, но ветка сохраняет корректность для экстремального downscale.
      weight = requested_weight;
      next_count = min(trusted_count, count_limit);
    } else {
      // История потеряна либо полностью отвергнута — временно возвращаемся к пространственному upscale.
      weight = 0.0;
      next_count = requested_weight > 0.0 ? current_coverage : 0.0;
    }
  }
  const vec3 blended = mix(current_compressed, history_compressed, weight);

  imageStore(taa_output, pixel, vec4(pf03_range_expand(blended), current_transmittance));
  imageStore(taa_meta_output, pixel, vec4(next_count, rejection, 0.0, 0.0));
}
