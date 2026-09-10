#include "godot_jit.h"
#include <algorithm>
#include <c_abi.h>
#include <c_codegen.h>
#include <compiler.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot {
void GodotJIT::_bind_methods() {
    ClassDB::bind_method(D_METHOD("compile_c", "source"), &GodotJIT::compile_c);
    ClassDB::bind_method(D_METHOD("compile_sgd", "source", "receiver"), &GodotJIT::compile_sgd, DEFVAL(Variant()));
    ClassDB::bind_method(D_METHOD("execute_function", "name", "arguments"), &GodotJIT::execute_function,
                         DEFVAL(Array()));
    ClassDB::bind_method(D_METHOD("get_generated_c"), &GodotJIT::get_generated_c);
    ClassDB::bind_method(D_METHOD("execute", "argument"), &GodotJIT::execute, DEFVAL(0));
    ClassDB::bind_method(D_METHOD("is_compiled"), &GodotJIT::is_compiled);
    ClassDB::bind_method(D_METHOD("get_error"), &GodotJIT::get_error);
    ClassDB::bind_method(D_METHOD("clear"), &GodotJIT::clear);
}

bool GodotJIT::compile_c(const String &source) {
    const CharString utf8 = source.utf8();
    auto candidate = godot_jit::CModule::compile(std::string(utf8.get_data(), utf8.length()), error_, native_symbols());
    if (!candidate)
        return false;
    auto previous = std::move(native_);
    module_ = std::move(candidate);
    if (previous) previous->cancel_coroutines();
    return true;
}

int64_t GodotJIT::execute(int64_t argument) {
    ERR_FAIL_COND_V_MSG(!module_, 0, "Compile a C module before executing it");
    const GodotJitHost host{GODOT_JIT_ABI_VERSION, sizeof(GodotJitHost), nullptr,
                            [](void *, const char *message) { UtilityFunctions::print(String::utf8(message)); }};
    auto active = module_;
    return active->invoke(host, argument);
}

void GodotJIT::clear() {
    auto previous = std::move(native_);
    module_.reset();
    error_.clear();
    // Completion callbacks may clear or replace this runtime reentrantly.
    if (previous) previous->cancel_coroutines();
}

} // namespace godot

namespace godot {
bool GodotJIT::compile_sgd(const String &source, Object *receiver) {
    error_.clear();
    gdscript::CompilerOptions options;
    options.optimize = false;
    options.batch_iteration = false;
    auto candidate = NativeProgram::compile(source, options, error_);
    if (!candidate)
        return false;
    auto state = std::make_shared<NativeState>(candidate, receiver);
    Variant result;
    if (!state->invoke(-1, nullptr, 0, result)) {
        error_ = state->error;
        return false;
    }
    auto previous = std::move(native_);
    native_ = std::move(state);
    module_.reset();
    if (previous) previous->cancel_coroutines();
    return true;
}
Variant GodotJIT::execute_function(const String &name, const Array &arguments) {
    auto active = native_;
    error_.clear();
    if (!active) {
        error_ = "Compile SGD before calling a function";
        return {};
    }
    std::vector<Variant> values(arguments.size());
    std::vector<const Variant *> pointers(arguments.size());
    for (int i = 0; i < arguments.size(); ++i) {
        values[i] = arguments[i];
        pointers[i] = &values[i];
    }
    GDExtensionCallError error{};
    Variant result;
    if (!active->call(name, pointers.data(), pointers.size(), result, error)) {
        error_ = active->error.empty() ? "Invalid function or arguments" : active->error;
        return {};
    }
    return result;
}
String GodotJIT::get_generated_c() const {
    return native_ ? String::utf8(native_->program->generated.c_str()) : String();
}
} // namespace godot
