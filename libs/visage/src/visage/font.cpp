#include "font.h" 

#include <algorithm>

#include "devils_engine/utils/core.h"
#include "devils_engine/utils/fileio.h"
#include "header.h"
#include "devils_engine/utf/utf.hpp"

//#include "devils_engine/painter/arbitrary_image_container.h"

#define MSDFGEN_PUBLIC
#include "msdfgen.h"
#include "msdf-atlas-gen/msdf-atlas-gen.h"

namespace devils_engine {
namespace visage {
const font_t::glyph_t *font_t::find_glyph(const uint32_t codepoint) const {
  auto itr = std::upper_bound(glyphs.begin(), glyphs.end(), codepoint, [] (const uint32_t codepoint, const auto &g) { return g.codepoint == codepoint; });
  if (itr == glyphs.end()) return fallback;
  return &(*itr);
}

void font_t::query_font_glyph(float font_height, struct nk_user_font_glyph *glyph, nk_rune codepoint, nk_rune next_codepoint) const {
  auto g = find_glyph(codepoint);

  const double local_scale = font_height / scale;

  glyph->width = double(g->w) * local_scale; // local_scale ?
  glyph->height = double(g->h) * local_scale;
  glyph->xadvance = g->advance * scale * local_scale;
  glyph->uv[0].x = g->al / double(width);
  glyph->uv[0].y = g->ab / double(height);
  glyph->uv[1].x = g->ar / double(width);
  glyph->uv[1].y = g->at / double(height);
  glyph->offset.x = g->pl * scale * local_scale;
  glyph->offset.y = g->pb * scale * local_scale;
}

double font_t::text_width(double height, const std::string_view &txt) const {
  uint32_t rune = 0;
  size_t size = 0;
  size_t gsize = 0;
  double text_width = 0.0;
  const double local_scale = height / scale;

  gsize = size = nk_utf_decode(txt.data(), &rune, txt.size());
  if (gsize == 0) return 0.0;

  while (size <= txt.size() && gsize != 0 && rune != NK_UTF_INVALID) {
    const auto g = find_glyph(rune);
    text_width += g->advance * scale * local_scale;
    gsize = nk_utf_decode(txt.data() + size, &rune, txt.size() - size);
    size += gsize;
  }

  return text_width;
}

static float local_text_width(nk_handle h, float height, const char* text, const int32_t len) {
  auto f = reinterpret_cast<const font_t*>(h.ptr);
  return f->text_width(height, std::string_view(text, len));
}

static void local_query_font_glyph(nk_handle h, float font_height, struct nk_user_font_glyph *glyph, nk_rune codepoint, nk_rune next_codepoint) {
  auto f = reinterpret_cast<const font_t*>(h.ptr);
  f->query_font_glyph(font_height, glyph, codepoint, next_codepoint);
}

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

  font_raii(msdfgen::FreetypeHandle* ft, const msdfgen::byte* data, const size_t length, const std::string_view &hint) : 
    fh(msdfgen::loadFontData(ft, data, length))
  {
    if (fh == nullptr) utils::error{}("Could not load font '{}'", hint);
  }

  ~font_raii() {
    msdfgen::destroyFont(fh);
  }
};

// вообще у нас будет несколько шрифтов + локализация, то есть желательно 
// написать отдельный класс под загрузку шрифтов - этот класс
// как раз должен будет выполнить вообще все шаги по созданию 
// одной картинки для всех задействованых шрифтов
// самих шрифтов поди будет штуки 4-5 в игре
// причем наверное один из них будет чисто английский
// предположим ситуацию где нам нужно создать 
// англ + кириллица: аскии таблица состоит из 90 (?) символов
// + к этому кириллическая талица 255 символов
// 345 символов, примерно 19 символов в строке, 19 * 32 = 608 размер картинки
// (в худшем случае), чуть больше 3к текстурка на 5 шрифтов
std::tuple<std::unique_ptr<font_t>, uint32_t> load_font(painter::host_image_container* imgs, const std::string &path) {
  std::vector<msdf_atlas::GlyphGeometry> glyphs;
  msdf_atlas::FontGeometry fontGeometry(&glyphs);

  // кажется указатель нигде не сохраняется на будущее и мы можем так сделать
  // вот тут как раз символы локализации загружаются в память
  {
    const auto file = file_io::read<uint8_t>(path);
    freetype_raii ftraii;
    font_raii fraii(ftraii.ft, file.data(), file.size(), path);

    msdf_atlas::Charset set; // ???

    // мы можем наверное составить чарсеты сразу из нескольких фонтов
    // после этого нужно будет вернуть font_t для каждого фонта отдельно
    fontGeometry.loadCharset(fraii.fh, 1.0, msdf_atlas::Charset::ASCII);
  }

  const double max_corner_angle = 3.0; // ???
  for (auto &glyph : glyphs) {
    glyph.edgeColoring(&msdfgen::edgeColoringInkTrap, max_corner_angle, 0);
  }

  msdf_atlas::TightAtlasPacker packer;
  packer.setDimensionsConstraint(msdf_atlas::DimensionsConstraint::SQUARE);
  packer.setMinimumScale(24.0); // ??? (размеры бокса для глифа?)
  packer.setPixelRange(2.0); // ??? (похоже что делает переходы более плавными)
  packer.setMiterLimit(1.0); // ???
  packer.pack(glyphs.data(), glyphs.size());
  int width = 0, height = 0;
  packer.getDimensions(width, height);

  const size_t color_channels = 3;

  msdf_atlas::ImmediateAtlasGenerator<
    float, // pixel type of buffer for individual glyphs depends on generator function
    color_channels,     // number of atlas color channels
    msdf_atlas::msdfGenerator, // function to generate bitmaps for individual glyphs
    msdf_atlas::BitmapAtlasStorage<msdf_atlas::byte, color_channels> // storage
  > generator(width, height);

  msdf_atlas::GeneratorAttributes attributes; // ???
  generator.setAttributes(attributes);
  generator.setThreadCount(4);
  generator.generate(glyphs.data(), glyphs.size());
  msdfgen::BitmapConstRef<msdf_atlas::byte, color_channels> atlas_storage = generator.atlasStorage();
  
  //const uint32_t host_image_id = imgs->create("font_storage", {uint32_t(width), uint32_t(height)}, painter::rgb24_format, VK_NULL_HANDLE);
  //auto mem = imgs->mapped_memory(host_image_id);
  //const size_t size = size_t(width) * size_t(height) * color_channels * sizeof(msdf_atlas::byte);
  //memcpy(mem, atlas_storage.pixels, size);

  msdfgen::savePng(atlas_storage, "font.png");

  auto f = std::make_unique<font_t>();
  f->width = width;
  f->height = height;
  f->scale = packer.getScale();
  f->metrics.em_size = fontGeometry.getMetrics().emSize;
  f->metrics.ascender_y = fontGeometry.getMetrics().ascenderY;
  f->metrics.descender_y = fontGeometry.getMetrics().descenderY;
  f->metrics.line_height = fontGeometry.getMetrics().lineHeight;
  f->metrics.underline_y = fontGeometry.getMetrics().underlineY;
  f->metrics.underline_thickness = fontGeometry.getMetrics().underlineThickness;
  f->glyphs.reserve(glyphs.size());
  for (const auto &glyph : glyphs) {
    f->glyphs.emplace_back();
    auto &g = f->glyphs.back();
    g.advance = glyph.getAdvance();
    glyph.getBoxRect(g.x,g.y,g.w,g.h);
    g.scale = glyph.getBoxScale();
    g.codepoint = glyph.getCodepoint();
    g.gscale = glyph.getGeometryScale();
    g.index = glyph.getIndex();
    glyph.getQuadAtlasBounds(g.al,g.ab,g.ar,g.at);
    glyph.getQuadPlaneBounds(g.pl,g.pb,g.pr,g.pt); // все еще не доконца понятно что делать с этим
  }

  std::sort(f->glyphs.begin(), f->glyphs.end(), [] (const auto &a, const auto &b) {
    return a.codepoint < b.codepoint;
  });

  // переводим данные в удобный для нас вид
  for (auto &g : f->glyphs) {
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
  f->nkfont->userdata.ptr = f.get();
  f->nkfont->height = 1000.0f;
  f->nkfont->width = &local_text_width;
  f->nkfont->query = &local_query_font_glyph;
  //f->nkfont->texture.id = host_image_id; // будет другим
  f->nkfont->texture.id = UINT32_MAX;

  return std::make_tuple(std::move(f), UINT32_MAX);
}
}
}