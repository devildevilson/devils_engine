#include <devils_engine/demiurg/module_profile.h>
int main() {
  return devils_engine::demiurg::module_profile_path("modules", "default").extension() == ".tavl" ? 0 : 1;
}
