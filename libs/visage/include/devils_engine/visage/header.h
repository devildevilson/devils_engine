#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_COMMAND_USERDATA
#include <nuklear.h>

nk_uint *nk_add_value_g(struct nk_context *ctx, struct nk_window *win, nk_hash name, nk_uint value);
nk_uint *nk_find_value_g(struct nk_window *win, nk_hash name);

// итак интейрфейс
// у нас по большому счету будет 3 типа того
// что называется интерфейсом:
// 1) непосредственно надписи поверх аттачмента
// 2) надписи где то в мире в качестве билборда или 3х мерной надписи
// 3) набор буков на произвольной кривой
// первые 2 точно будут проходить через интерфейс в том или ином виде
// 3й возможно
// интерфейс вызывает функцию обновления
// в этой функции луа обращается к биндингам наклира
// после чего наклир спихивает инфу в буфер
// по мимо этого - функция должна знать о том в каком состоянии находится игра
// да и вообще все об игре