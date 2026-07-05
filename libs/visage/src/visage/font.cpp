#include "font.h"

#include <algorithm>
#include <cstring>

#include "devils_engine/utils/core.h"
#include "devils_engine/utils/fileio.h"
#include "header.h"
#include "render_output.h" // tex_id::pack — упаковать тип msdf + слот в texture.id
#include "devils_engine/utf/utf.hpp"


namespace devils_engine {
namespace visage {
font_t::~font_t() = default;

void font_t::set_texture_id(uint32_t id) {
  // только базовый nkfont; sized-варианты живут в visage::system и берут актуальный texture.id
  // отсюда при каждом push_font, так что гонки «push до HOT -> stale id» больше нет.
  // Пакуем ТИП msdf + слот: текст-команды nuklear понесут упакованный id, шейдер декодит type=msdf
  // (id — это gpu_index/слот атласа; см. tex_id в render_output.h).
  if (nkfont) nkfont->texture.id = int(tex_id::pack(gui_draw_mode::msdf, id));
}

const font_t::glyph_t *font_t::find_glyph(const uint32_t codepoint) const {
  // glyphs отсортированы по codepoint (см. font_atlas_packer::load_fonts) -> бинарный поиск.
  // ВАЖНО: компаратор lower_bound — это упорядочивающий предикат comp(element, value),
  // а не равенство (прежний код использовал '==' в upper_bound -> UB и возврат мусора).
  auto itr = std::lower_bound(glyphs.begin(), glyphs.end(), codepoint,
    [] (const glyph_t &g, const uint32_t cp) { return g.codepoint < cp; });
  if (itr != glyphs.end() && itr->codepoint == codepoint) return &(*itr);
  return fallback; // может быть nullptr — вызывающие обязаны это проверять
}

void font_t::query_font_glyph(float font_height, struct nk_user_font_glyph *glyph, nk_rune codepoint, nk_rune) const {
  auto g = find_glyph(codepoint);
  if (g == nullptr) { memset(glyph, 0, sizeof(*glyph)); return; }

  const double local_scale = font_height / scale;

  glyph->width = double(g->w) * local_scale; // local_scale ?
  glyph->height = double(g->h) * local_scale;
  glyph->xadvance = g->advance * scale * local_scale;
  glyph->uv[0].x = g->al / double(width);
  glyph->uv[1].x = g->ar / double(width);
  // v меняем местами: GPU-текстура = сырой bottom-up массив атласа (вертикально зеркальна
  // относительно «нормального» вида), поэтому верх квада (uv[0]) сэмплит верх глифа = at/H
  // (больший v), низ (uv[1]) = ab/H. Иначе глифы выходят вверх ногами.
  glyph->uv[0].y = g->at / double(height);
  glyph->uv[1].y = g->ab / double(height);
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
    if (g != nullptr) text_width += g->advance * scale * local_scale;
    gsize = nk_utf_decode(txt.data() + size, &rune, txt.size() - size);
    size += gsize;
  }

  return text_width;
}

// nk_user_font callbacks (local_text_width / local_query_font_glyph) и вся генерация атласа
// живут в font_atlas_packer.cpp — прежняя дублирующая пара здесь удалена вместе с легаси load_font.

}
}
