#include <compiler.h>
#include <c_codegen.h>
#include <c_abi.h>
#include <godot_jit/c_module.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <cstring>

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
extern "C" int scalar_op(GJContext *ctx, int op, GJVariant *d, GJVariant *, const char *, int detail,
                         const GJVariant *const *a, int count) {
    if (op == GJ_CONSTRUCT && count == 1 && detail == 2 && a[0]->type == 2) {
        *d = *a[0];
        return 1;
    }
    return scalar_fail(ctx, "Unexpected scalar test host operation");
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
    check(!compiler.compile_to_c("func suspended(value):\n    return await value\n") &&
          compiler.get_error().find("AWAIT") != std::string::npos, "Missing unsupported async diagnostic");
    check(!compiler.compile_to_c("func broken("), "Accepted invalid frontend input");
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
