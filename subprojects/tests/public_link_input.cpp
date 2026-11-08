#include <devils_engine/input/key_names.h>
int main() { return devils_engine::input::get_glfw_key_from_canonical("key_a") >= 0 ? 0 : 1; }
