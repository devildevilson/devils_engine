#define NK_IMPLEMENTATION
#include "header.h"

nk_uint *nk_add_value_g(struct nk_context *ctx, struct nk_window *win, nk_hash name, nk_uint value) {
  return nk_add_value(ctx, win, name, value);
}

nk_uint *nk_find_value_g(struct nk_window *win, nk_hash name) {
  return nk_find_value(win, name);
}

// наклир появится по идее в двух местах:
// в контейнере чтобы создать контекст и принять инпут
// и в биндингах
// в биндинги надо как то удачно передать контекст
// желательно не называть контейнером
// 