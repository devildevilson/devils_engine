#include <devils_engine/sound/system.h>
int main() { return devils_engine::sound::normalize({0, 2, 0}).y == 1 ? 0 : 1; }
