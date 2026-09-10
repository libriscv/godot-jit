#pragma once
#include <c_abi.h>
#include "function_state.h"
#include <compiler.h>
#include <ir.h>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_jit/c_module.h>
#include <memory>
#include <unordered_map>
#include <string_view>

namespace godot {
struct NativeProgram {
    std::unique_ptr<godot_jit::CModule> module;
    gdscript::IRProgram ir;
    std::string generated;
    String source_path;
    bool debug_info = false;
    std::unordered_map<std::string, int> functions;
    HashMap<StringName, int> methods;
    std::unordered_map<std::string_view, StringName> names;
    std::vector<Variant> statics;
    Dictionary property_defaults;
    GJEntry entry = nullptr;
    static std::shared_ptr<NativeProgram> compile(const String &, const gdscript::CompilerOptions &, std::string &);
};
struct NativeState : std::enable_shared_from_this<NativeState> {
    std::shared_ptr<NativeProgram> program;
    std::vector<Variant> members;
    ObjectID owner;
    std::string error;
    std::vector<Ref<UnsafeFunctionState>> coroutines;
    bool cancelling = false;
    ~NativeState();
    void cancel_coroutines(bool notify = true);
    void retire(const Ref<UnsafeFunctionState> &);
    int suspend(GJContext *, UnsafeFunctionState *, int function, int instruction,
                Variant &result, const Variant &operand, GJVariant *const *slots, int count, int destination);
    int restore(UnsafeFunctionState *, GJVariant *const *slots, int count);
    void resume(const Ref<UnsafeFunctionState> &, const Variant &);
    explicit NativeState(std::shared_ptr<NativeProgram> p, Object *object = nullptr);
    bool invoke(int index, const Variant **args, int count, Variant &result, UnsafeFunctionState *resuming = nullptr);
    bool call(const StringName &, const Variant **, int, Variant &, GDExtensionCallError &);
};
Variant parameter_default(const gdscript::FunctionParameter &);
Dictionary native_property_defaults(const String &, const gdscript::CompilerOptions &, const gdscript::IRProgram &);
const std::vector<std::pair<std::string, const void *>> &native_symbols();
Variant native_callable(GJContext *, int, const Variant &);
} // namespace godot
