#pragma once
#include <c_abi.h>
#include <compiler.h>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_jit/c_module.h>
#include <memory>
#include <unordered_map>
#include <string_view>

namespace godot {
struct NativeProgram {
    std::unique_ptr<godot_jit::CModule> module;
    gdscript::IRProgram ir;
    std::string generated;
    std::unordered_map<std::string, int> functions;
    HashMap<StringName, int> methods;
    std::unordered_map<std::string_view, StringName> names;
    std::vector<Variant> statics;
    GJEntry entry = nullptr;
    static std::shared_ptr<NativeProgram> compile(const String &, const gdscript::CompilerOptions &, std::string &);
};
struct NativeState : std::enable_shared_from_this<NativeState> {
    std::shared_ptr<NativeProgram> program;
    std::vector<Variant> members;
    ObjectID owner;
    std::string error;
    explicit NativeState(std::shared_ptr<NativeProgram> p, Object *object = nullptr);
    bool invoke(int index, const Variant **args, int count, Variant &result);
    bool call(const StringName &, const Variant **, int, Variant &, GDExtensionCallError &);
};
Variant parameter_default(const gdscript::FunctionParameter &);
const std::vector<std::pair<std::string, const void *>> &native_symbols();
Variant native_callable(GJContext *, int, const Variant &);
} // namespace godot
