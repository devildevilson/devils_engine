#include "font_atlas_packer.h"

#include "devils_engine/utils/fileio.h"
#include "devils_engine/utils/core.h"

#include "header.h"

#define MSDFGEN_PUBLIC
#include "msdfgen.h"
#include "msdf-atlas-gen/msdf-atlas-gen.h"

namespace devils_engine {
namespace visage {
struct freetype_raii {
  msdfgen::FreetypeHandle* ft;

  freetype_raii() : ft(msdfgen::initializeFreetype()) {
    if (ft == nullptr) utils::error{}("Could not init freetype");
  }
  ~freetype_raii() noexcept {
    msdfgen::deinitializeFreetype(ft);
  }
};

struct font_raii {
  msdfgen::FontHandle* fh;

  font_raii(msdfgen::FreetypeHandle* ft, const msdfgen::byte* data, const size_t length, const std::string_view& hint) :
    fh(msdfgen::loadFontData(ft, data, length))
  {
    if (fh == nullptr) utils::error{}("Could not load font '{}'", hint);
  }

  ~font_raii() {
    msdfgen::destroyFont(fh);
  }
};

void font_atlas_packer::setup_font(std::string path) {
  auto data = file_io::read<uint8_t>(path);
  fonts_data.push_back(std::make_pair(std::move(data), std::move(path)));
}

void font_atlas_packer::setup_font(std::vector<uint8_t> data, std::string hint) {
  fonts_data.push_back(std::make_pair(std::move(data), std::move(hint)));
}

static float local_text_width(nk_handle h, float height, const char* text, const int32_t len) {
  auto f = reinterpret_cast<const font_t*>(h.ptr);
  return f->text_width(height, std::string_view(text, len));
}

static void local_query_font_glyph(nk_handle h, float font_height, struct nk_user_font_glyph* glyph, nk_rune codepoint, nk_rune next_codepoint) {
  auto f = reinterpret_cast<const font_t*>(h.ptr);
  f->query_font_glyph(font_height, glyph, codepoint, next_codepoint);
}

std::tuple<std::vector<std::unique_ptr<font_t>>, font_atlas_packer::font_image_t> font_atlas_packer::load_fonts(const config& cfg) {
  std::vector<msdf_atlas::GlyphGeometry> glyphs;
  //msdf_atlas::FontGeometry fontGeometry(&glyphs);
  std::vector<msdf_atlas::FontGeometry> geometries(fonts_data.size(), msdf_atlas::FontGeometry(&glyphs));
  std::vector<size_t> load_sizes(fonts_data.size(), 0);

  // кажется указатель нигде не сохраняется на будущее и мы можем так сделать
  // вот тут как раз символы локализации загружаются в память
  // скорее всего FontGeometry нужно сделать уникальной для шрифта
  {
    freetype_raii ftraii;
    std::vector<font_raii> fraiis;
    for (size_t i = 0; i < fonts_data.size(); ++i) {
      const auto& data = fonts_data[i];
      font_raii fraii(ftraii.ft, data.first.data(), data.first.size(), data.second);

      msdf_atlas::Charset set;
      for (const auto &pair : cfg.charsets) {
        if (pair.first > pair.second) utils::error{}("Bad charset ({}, {})", pair.first, pair.second);
        for (uint32_t i = pair.first; i < pair.second; ++i) {
          set.add(i);
        }
      }

      // чарсеты локализации ?
      
      //fontGeometry.loadCharset(fraii.fh, 1.0, msdf_atlas::Charset::ASCII);
      //fontGeometry.loadCharset(fraii.fh, 1.0, set);
      load_sizes[i] += geometries[i].loadCharset(fraii.fh, 1.0, msdf_atlas::Charset::ASCII);
      load_sizes[i] += geometries[i].loadCharset(fraii.fh, 1.0, set);

      // loadCharset возвращает количество загруженных символов
      // вполне возможно что не во всех шрифтах есть все интересующие нас символы
      // блин было бы неплохо это все дело как то заранее еще проверить
      // но только что мы можем сделать? по идее надо бы хотя бы сообщить о том 
      // что мы не можем загрузить все символы из заданного чарсета
    }
  }

  const double max_corner_angle = cfg.max_corner_angle;
  for (auto& glyph : glyphs) {
    glyph.edgeColoring(&msdfgen::edgeColoringInkTrap, max_corner_angle, 0);
  }

  msdf_atlas::TightAtlasPacker packer;
  packer.setDimensionsConstraint(msdf_atlas::DimensionsConstraint::MULTIPLE_OF_FOUR_SQUARE); // SQUARE
  packer.setMinimumScale(cfg.minimum_scale);
  packer.setPixelRange(cfg.pixel_range);
  packer.setMiterLimit(cfg.mitter_limit);
  packer.pack(glyphs.data(), glyphs.size());
  int width = 0, height = 0;
  packer.getDimensions(width, height);

  font_image_t img;
  img.width = 0;
  img.height = 0;
  img.channels = 0;

  if (cfg.color_channels == 3) {
    const size_t color_channels = 3;

    msdf_atlas::ImmediateAtlasGenerator<
      float, // pixel type of buffer for individual glyphs depends on generator function
      color_channels,     // number of atlas color channels
      msdf_atlas::msdfGenerator, // function to generate bitmaps for individual glyphs
      msdf_atlas::BitmapAtlasStorage<msdf_atlas::byte, color_channels> // storage
    > generator(width, height);

    msdf_atlas::GeneratorAttributes attributes; // ???
    generator.setAttributes(attributes);
    generator.setThreadCount(cfg.thread_count);
    generator.generate(glyphs.data(), glyphs.size());
    const msdfgen::BitmapConstRef<msdf_atlas::byte, color_channels>& atlas_storage = generator.atlasStorage();
    if (cfg.save_png) msdfgen::savePng(atlas_storage, "font.png");
    const size_t size = size_t(width) * size_t(height) * color_channels * sizeof(msdf_atlas::byte);

    img.bytes.resize(size, 0);
    img.width = width;
    img.height = height;
    img.channels = color_channels;
    memcpy(img.bytes.data(), atlas_storage.pixels, size);
  } else if (cfg.color_channels == 4) {
    const size_t color_channels = 4;

    msdf_atlas::ImmediateAtlasGenerator<
      float, // pixel type of buffer for individual glyphs depends on generator function
      color_channels,     // number of atlas color channels
      msdf_atlas::mtsdfGenerator, // function to generate bitmaps for individual glyphs
      msdf_atlas::BitmapAtlasStorage<msdf_atlas::byte, color_channels> // storage
    > generator(width, height);

    msdf_atlas::GeneratorAttributes attributes; // ???
    generator.setAttributes(attributes);
    generator.setThreadCount(cfg.thread_count);
    generator.generate(glyphs.data(), glyphs.size());
    const msdfgen::BitmapConstRef<msdf_atlas::byte, color_channels>& atlas_storage = generator.atlasStorage();
    if (cfg.save_png) msdfgen::savePng(atlas_storage, "font.png");
    const size_t size = size_t(width) * size_t(height) * color_channels * sizeof(msdf_atlas::byte);

    img.bytes.resize(size, 0);
    img.width = width;
    img.height = height;
    img.channels = color_channels;
    memcpy(img.bytes.data(), atlas_storage.pixels, size);
  } else utils::error{}("Unsupported color channels count {}", cfg.color_channels);
  
  size_t offset = 0;
  std::vector<std::unique_ptr<font_t>> fonts;
  for (size_t i = 0; i < fonts_data.size(); ++i) {
    auto f = std::make_unique<font_t>();
    f->width = width;
    f->height = height;
    f->scale = packer.getScale();
    /*f->metrics.em_size = fontGeometry.getMetrics().emSize;
    f->metrics.ascender_y = fontGeometry.getMetrics().ascenderY;
    f->metrics.descender_y = fontGeometry.getMetrics().descenderY;
    f->metrics.line_height = fontGeometry.getMetrics().lineHeight;
    f->metrics.underline_y = fontGeometry.getMetrics().underlineY;
    f->metrics.underline_thickness = fontGeometry.getMetrics().underlineThickness;*/
    f->metrics.em_size = geometries[i].getMetrics().emSize;
    f->metrics.ascender_y = geometries[i].getMetrics().ascenderY;
    f->metrics.descender_y = geometries[i].getMetrics().descenderY;
    f->metrics.line_height = geometries[i].getMetrics().lineHeight;
    f->metrics.underline_y = geometries[i].getMetrics().underlineY;
    f->metrics.underline_thickness = geometries[i].getMetrics().underlineThickness;
    f->glyphs.reserve(glyphs.size());
    for (size_t j = offset; j < offset + load_sizes[i]; ++j) {
      const auto& glyph = glyphs[j];
      f->glyphs.emplace_back();
      auto& g = f->glyphs.back();
      g.advance = glyph.getAdvance();
      glyph.getBoxRect(g.x, g.y, g.w, g.h);
      g.scale = glyph.getBoxScale();
      g.codepoint = glyph.getCodepoint();
      g.gscale = glyph.getGeometryScale();
      g.index = glyph.getIndex();
      glyph.getQuadAtlasBounds(g.al, g.ab, g.ar, g.at);
      glyph.getQuadPlaneBounds(g.pl, g.pb, g.pr, g.pt);
    }

    std::sort(f->glyphs.begin(), f->glyphs.end(), [](const auto& a, const auto& b) {
      return a.codepoint < b.codepoint;
    });

    // переводим данные в удобный для нас вид
    for (auto& g : f->glyphs) {
      const int ny = f->height - (g.y + g.h);
      const double nab = double(f->height) - g.at;
      const double nat = double(ny + g.h) - 0.5;
      const double npb = 1.0 - g.pt; // все еще немного сомневаюсь на счет этого
      const double npt = 1.0 - g.pb;

      g.y = ny;
      g.ab = nab;
      g.at = nat;
      g.pb = npb;
      g.pt = npt;
    }

    f->nkfont.reset(new nk_user_font);
    memset(f->nkfont.get(), 0, sizeof(nk_user_font));
    f->nkfont->height = 1000.0f;
    f->nkfont->query = local_query_font_glyph;
    f->nkfont->width = local_text_width;
    f->nkfont->userdata.ptr = f.get();

    fonts.push_back(std::move(f));
    offset += load_sizes[i];
  }
  
  return std::make_tuple(std::move(fonts), std::move(img));
}
}
}