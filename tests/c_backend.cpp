#include <compiler.h>
#include <c_codegen.h>
#include <c_abi.h>
#include <godot_jit/c_module.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <cmath>

// Hand-written scalar host for an engine-independent backend execution test.
// The headless Godot test exercises the actual C++ Variant ABI.
extern "C" void scalar_copy(GJVariant *d, const GJVariant *s) { *d = *s; }
extern "C" void scalar_destroy(GJVariant *v) { *v = {}; }
extern "C" int scalar_truth(const GJVariant *v) { return v->data.i != 0; }
extern "C" int scalar_fail(GJContext *ctx, const char *s) {
    *static_cast<std::string *>(ctx->error) = s;
    ctx->failed = 1;
    return 0;
}
extern "C" int scalar_op(GJContext *ctx, int op, GJVariant *d, GJVariant *, int, int detail,
                         const GJVariant *const *a, int count) {
    if ((op == GJ_CONSTRUCT || op == GJ_COERCE) && count == 1 && detail == 2 && a[0]->type == 2) {
        *d = *a[0];
        return 1;
    }
    return scalar_fail(ctx, "Unexpected scalar test host operation");
}
// Distinctive host answers prove calls (including constant arguments) reach
// the registered engine ABI instead of being folded or replaced with libm.
static int math_calls = 0;
extern "C" double engine_sin(double x) { ++math_calls; return 100 + x; }
extern "C" double engine_log(double x) { ++math_calls; return 20 + x; }
extern "C" GJReal test_sqrt_real(GJReal x) { return std::sqrt(x); }
template<int N>
void engine_normalized(void *self, const void **, void *result, int) {
    const auto *v = static_cast<const GJReal *>(self);
    auto *out = static_cast<GJReal *>(result);
    GJReal squared = 0;
    for (int j = 0; j < N; ++j) squared += v[j] * v[j];
    GJReal length = std::sqrt(squared);
    for (int j = 0; j < N; ++j) out[j] = length ? v[j] / length : 0;
}
void check(bool condition, const std::string &error) { if (!condition) throw std::runtime_error(error); }
struct DebugCheck {
    const gdscript::IRProgram *ir;
    int depth = 0;
    int stops = 0;
    bool local_visible = false;
};
void debug_hook(GJContext *ctx, GJDebugFrame *frame, int event) {
    auto &test = *static_cast<DebugCheck *>(ctx->runtime);
    if (event == GJ_DEBUG_ENTER) ++test.depth;
    if (event == GJ_DEBUG_EXIT) --test.depth;
    if (event == GJ_DEBUG_BREAKPOINT) {
        ++test.stops;
        for (const auto &local : test.ir->functions[frame->function].debug_locals) {
            if (local.name == "copied" && size_t(frame->instruction) >= local.begin_instruction &&
                size_t(frame->instruction) < local.end_instruction) {
                const auto *value = frame->locals[local.register_num];
                test.local_visible = test.depth == 2 && value->type == 2 && value->data.i == 42;
            }
        }
    }
}
int main(int argc, char **argv) try {
    const std::string script = R"(func sum(n: int) -> int:
    var total: int = 0
    for i in range(n):
        total += i
    return total
func recursive(n: int) -> int:
    if n <= 1:
        return 1
    return n * recursive(n - 1)
func float_loop(n: int) -> float:
    var value: float = 0.5
    for i in range(n):
        value = value * 0.5 + float(i)
    return value
func wrap(n: int) -> int:
    return ((n + 1) * 3) << 65
func divide(n: int) -> int:
    var unused = 7 / n
    return 1
)";
    gdscript::Compiler compiler;
    auto source = compiler.compile_to_c(script);
    check(source.has_value(), compiler.get_error());
    check(source->find("ecall") == std::string::npos, "C output contains a VM syscall");
    check(source->find("GJVariant r0") != std::string::npos, "Expected local C values");
    if (argc == 2) { std::ofstream file(argv[1]); file << *source; check(bool(file), "Cannot write emitted C"); }
    const std::vector<std::pair<std::string, const void *>> symbols = {
        {"gj_vector2_normalized", reinterpret_cast<const void *>(&engine_normalized<2>)},
        {"gj_vector3_normalized", reinterpret_cast<const void *>(&engine_normalized<3>)},
        {"gj_vector4_normalized", reinterpret_cast<const void *>(&engine_normalized<4>)},
        {"gj_math_sin", reinterpret_cast<const void *>(&engine_sin)},
        {"gj_math_log", reinterpret_cast<const void *>(&engine_log)},
        {"gj_sqrt_real", reinterpret_cast<const void *>(&test_sqrt_real)},
        {"gj_copy", reinterpret_cast<const void *>(&scalar_copy)},
        {"gj_destroy", reinterpret_cast<const void *>(&scalar_destroy)},
        {"gj_truth", reinterpret_cast<const void *>(&scalar_truth)},
        {"gj_fail", reinterpret_cast<const void *>(&scalar_fail)},
        {"gj_op", reinterpret_cast<const void *>(&scalar_op)},
    };
    std::string error;
    auto module = godot_jit::CModule::compile(*source, error, symbols, "gj_entry");
    check(bool(module), error);
    auto entry = reinterpret_cast<GJEntry>(module->symbol("gj_entry"));
    GJContext context{nullptr, nullptr, &error, 0};
    GJVariant input{}, result{};
    input.type = 2;
    input.data.i = 100;
    const GJVariant *args[] = {&input};
    check(entry(&context, 0, &result, args, 1) && result.data.i == 4950, "C loop failed: " + error);
    input.data.i = 10;
    check(entry(&context, 1, &result, args, 1) && result.data.i == 3628800, "C recursion failed: " + error);
    check(entry(&context, 2, &result, args, 1) && result.type == 3 && result.data.f == 16.00439453125,
          "Promoted float loop failed: " + error);
    input.data.i = 9223372036854775807LL;
    check(entry(&context, 3, &result, args, 1) && result.type == 2 && result.data.i == 0,
          "Promoted wrapping arithmetic failed: " + error);
    input.data.i = 0;
    check(!entry(&context, 4, &result, args, 1) && !error.empty(), "Eliminated an unused division diagnostic");
    context.failed = 0;
    error.clear();
    check(!entry(&context, 1, &result, args, 0) && !error.empty(), "Missing arity diagnostic");
    check(compiler.compile_to_c("func suspended(value):\n    return await value\n").has_value(),
          "Coroutine compilation failed: " + compiler.get_error());
    check(!compiler.compile_to_c("func broken("), "Accepted invalid frontend input");
    const std::string math_script = R"(func primitives(a, b):
    var x = absf(a)
    return clampf(maxf(x, b), 1.0, 5.0)
func select(a, b):
    return min(a, b)
func engine_calls(x):
    return sin(x) + log(2.0)
func normalize(v):
    v = v.normalized()
    return v
func length(v):
    return v.length()
)";
    for (bool debug_math : {false, true}) {
        gdscript::CompilerOptions math_options;
        math_options.debug_info = debug_math;
        math_calls = 0;
        auto math_source = compiler.compile_to_c(math_script, math_options);
        check(bool(math_source), compiler.get_error());
        if (argc == 2 && !debug_math) {
            std::ofstream file(std::string(argv[1]) + ".math.c");
            file << *math_source;
            check(bool(file), "Cannot write emitted math C");
        }
        auto math_module = godot_jit::CModule::compile(*math_source, error, symbols, "gj_entry");
        check(bool(math_module), error);
        auto math_entry = reinterpret_cast<GJEntry>(math_module->symbol("gj_entry"));
        context = {}; context.error = &error;
        GJVariant left{}, right{};
        const GJVariant *math_args[] = {&left, &right};
        left.type = 3; left.data.f = -3.0;
        right.type = 2; right.data.i = 2;
        check(math_entry(&context, 0, &result, math_args, 2) && result.type == 3 && result.data.f == 3,
              "Inline scalar math fell back to Variant host: " + error);
        left.type = 2; left.data.i = 9007199254740993LL;
        right.data.i = 9007199254740994LL;
        check(math_entry(&context, 1, &result, math_args, 2) && result.type == 2 && result.data.i == left.data.i,
              "Generic selection lost integer precision: " + error);
        left.type = 3; left.data.f = 3;
        check(math_entry(&context, 2, &result, math_args, 1) && result.type == 3 && result.data.f == 125 && math_calls == 2,
              "Transcendentals did not call the engine ABI: " + error);
        left.type = 5; left.data.real[0] = 3; left.data.real[1] = 4;
        check(math_entry(&context, 3, &result, math_args, 1) && result.type == 5 &&
              result.data.real[0] == GJReal(0.6) && result.data.real[1] == GJReal(0.8),
              "Direct normalization/aliasing failed: " + error);
        check(math_entry(&context, 4, &result, math_args, 1) && result.type == 3 && result.data.f == 5,
              "Vector length primitive failed: " + error);
    }
    const std::string debug_script = "func outer(n: int):\n    return inner(n)\nfunc inner(n: int):\n    var copied = n\n    breakpoint\n    return copied\n";
    auto plain = compiler.compile_to_c(debug_script);
    check(plain && plain->find("debug_frame") == std::string::npos, "Non-debug output gained instrumentation");
    gdscript::CompilerOptions debug_options;
    debug_options.optimize = false;
    debug_options.batch_iteration = false;
    debug_options.debug_info = true;
    auto debug_ir = compiler.compile_to_ir(debug_script, debug_options);
    auto debug_source = compiler.compile_to_c(debug_script, debug_options);
    check(debug_ir && debug_source && debug_source->find("GJ_DEBUG_BREAKPOINT") != std::string::npos,
          "Requested C debug info was omitted");
    auto debug_module = godot_jit::CModule::compile(*debug_source, error, symbols, "gj_entry");
    check(bool(debug_module), error);
    auto debug_entry = reinterpret_cast<GJEntry>(debug_module->symbol("gj_entry"));
    DebugCheck debug{&*debug_ir};
    context = {};
    context.error = &error;
    context.runtime = &debug;
    context.debug = &debug_hook;
    input.data.i = 42;
    check(debug_entry(&context, 0, &result, args, 1) && result.data.i == 42, "Debug C execution failed");
    check(debug.stops == 1 && debug.local_visible && debug.depth == 0, "Debug locals/stack lifetime failed");
    check(!debug_entry(&context, 0, &result, args, 0) && debug.depth == 0, "Failed call leaked a debug frame");
    std::cout << "C backend scalar execution and diagnostics passed\n";
    return 0;
} catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
