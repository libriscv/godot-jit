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
#ifdef GJ_TEST_MATH
#include <math.h>
double gj_math_sin(double x) { return 100 + x; }
double gj_math_log(double x) { return 20 + x; }
GJReal gj_sqrt_real(GJReal x) { return (GJReal)sqrt(x); }
static void normalized(int n, const GJReal *v, GJReal *out) {
    GJReal squared = 0;
    int j;
    for (j = 0; j < n; ++j) squared += v[j] * v[j];
    GJReal length = gj_sqrt_real(squared);
    for (j = 0; j < n; ++j) out[j] = length ? v[j] / length : 0;
}
#define GJ_TEST_NORMALIZE(n, type, hash) \
    void gj_vector##n##_normalized(void *self, const void **args, void *out, int count) { normalized(n, self, out); }
GJ_VECTOR_NORMALIZE(GJ_TEST_NORMALIZE)
#undef GJ_TEST_NORMALIZE
int main(void) {
    GJContext ctx = {0};
    GJVariant left = {0}, right = {0}, result = {0};
    const GJVariant *args[] = {&left, &right};
    left.type = 3; left.data.f = -3;
    right.type = 2; right.data.i = 2;
    if (!gj_entry(&ctx, 0, &result, args, 2) || result.type != 3 || result.data.f != 3) return 1;
    left.type = 2; left.data.i = 9007199254740993LL;
    right.data.i = 9007199254740994LL;
    if (!gj_entry(&ctx, 1, &result, args, 2) || result.type != 2 || result.data.i != left.data.i) return 2;
    left.type = 3; left.data.f = 3;
    if (!gj_entry(&ctx, 2, &result, args, 1) || result.type != 3 || result.data.f != 125) return 3;
    left.type = 5; left.data.real[0] = 3; left.data.real[1] = 4;
    if (!gj_entry(&ctx, 3, &result, args, 1) || result.type != 5 ||
        result.data.real[0] != (GJReal)0.6 || result.data.real[1] != (GJReal)0.8) return 4;
    if (!gj_entry(&ctx, 4, &result, args, 1) || result.type != 3 || result.data.f != 5) return 5;
    return 0;
}
#else
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

#endif
