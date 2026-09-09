#include <godot_jit/c_module.h>
#include <compiler.h>
#include <ir_interpreter.h>
#include <ir_verifier.h>
#include <iostream>
#include <stdexcept>

static void check(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

int main() try {
    std::string error;
    std::string logged;
    GodotJitHost host{GODOT_JIT_ABI_VERSION, sizeof(GodotJitHost), &logged,
        [](void *context, const char *message) { *static_cast<std::string *>(context) = message; }};
    auto module = godot_jit::CModule::compile(R"(
GodotJitInt godot_jit_entry(const GodotJitHost *host, GodotJitInt argument) {
    if (host->abi_version != GODOT_JIT_ABI_VERSION) return -1;
    host->log(host->userdata, "native callback");
    return argument * 2 + 2;
})", error);
    check(module != nullptr, error);
    check(module->invoke(host, 20) == 42, "Native entry returned the wrong value");
    check(logged == "native callback", "Host callback failed");
    check(!godot_jit::CModule::compile("broken C source", error) && !error.empty(), "Missing compiler diagnostic");
    check(!godot_jit::CModule::compile("int other(void) { return 1; }", error), "Accepted a missing entry point");
    check(!godot_jit::CModule::compile(R"(
extern int missing_symbol(void);
GodotJitInt godot_jit_entry(const GodotJitHost *host, GodotJitInt argument) {
    return missing_symbol();
})", error), "Accepted an unresolved symbol");
    check(module->invoke(host, 20) == 42, "Failed compilation invalidated an existing module");
    auto incompatible = host;
    incompatible.abi_version++;
    bool rejected = false;
    try { module->invoke(incompatible, 0); } catch (const std::invalid_argument &) { rejected = true; }
    check(rejected, "Accepted an incompatible ABI");

    gdscript::Compiler compiler;
    auto ir = compiler.compile_to_ir("func answer(value: int) -> int:\n\treturn value * 2 + 2\n");
    check(ir.has_value(), compiler.get_error());
    gdscript::ir_verify(*ir);
    gdscript::IRInterpreter interpreter(*ir);
    check(std::get<int64_t>(interpreter.call("answer", {int64_t(20)})) == 42, "Frontend IR execution failed");
    std::cout << "Native C execution, host ABI, diagnostics and SafeGDScript frontend passed\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
