#include <devils_engine/visage/font_atlas_packer.h>
int main() {
  devils_engine::visage::font_atlas_packer packer;
  packer.setup_font(std::vector<uint8_t>{}, "owned data");
}
