/* Hand-written scalar host: execute the same generated functions with a
 * system compiler, independently of TinyCC and the Godot runtime. */
#include "c_abi.h"
#include <stdio.h>
#include <string.h>
extern int gj_entry(GJContext *, int, GJVariant *, const GJVariant *const *, int);
void gj_copy(GJVariant *d, const GJVariant *s) { *d = *s; }
void gj_destroy(GJVariant *v) { memset(v, 0, sizeof(*v)); }
int gj_truth(const GJVariant *v) { return v->data.i != 0; }
int gj_fail(GJContext *ctx, const char *message) {
    fprintf(stderr, "%s\n", message);
    ctx->failed = 1;
    return 0;
}
int gj_op(GJContext *ctx, int op, GJVariant *d, GJVariant *self, const char *name,
          int detail, const GJVariant *const *args, int count) {
    if (op == GJ_CONSTRUCT && detail == 2 && count == 1 && args[0]->type == 2) {
        *d = *args[0];
        return 1;
    }
    return gj_fail(ctx, "Unexpected system-compiler test syscall");
}
int main(void) {
    GJContext ctx = {0};
    GJVariant input = {0}, result = {0};
    const GJVariant *args[] = {&input};
    input.type = 2;
    input.data.i = 100;
    if (!gj_entry(&ctx, 0, &result, args, 1) || result.type != 2 || result.data.i != 4950) return 1;
    input.data.i = 10;
    if (!gj_entry(&ctx, 1, &result, args, 1) || result.type != 2 || result.data.i != 3628800) return 2;
    if (!gj_entry(&ctx, 2, &result, args, 1) || result.type != 3 || result.data.f != 16.00439453125) return 3;
    input.data.i = 9223372036854775807LL;
    if (!gj_entry(&ctx, 3, &result, args, 1) || result.type != 2 || result.data.i != 0) return 4;
    input.data.i = 0;
    if (gj_entry(&ctx, 4, &result, args, 1) || !ctx.failed) return 5;
    return 0;
}
