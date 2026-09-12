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
#include <array>

namespace godot {
struct BuiltinMethodInfo {
    int type;
    const char *name;
    uint32_t hash;
    int result;
    int count;
    int arguments[16];
};
struct BuiltinMemberInfo { int type; const char *name; int result; };
struct BuiltinOperatorInfo { int operation; int left; int right; int result; };
struct BuiltinOperatorCache { GDExtensionPtrOperatorEvaluator call = nullptr; int result = -1; };
const BuiltinOperatorCache *native_operators();
struct NativeProgram {
    struct Name {
        StringName name;
        Variant key;
        Variant string_key;
        const char *text;
        enum CallableOp { NONE, CALL, CALLV, BIND } callable_op = NONE;
        struct Method { const BuiltinMethodInfo *info = nullptr; GDExtensionPtrBuiltInMethod call = nullptr; };
        struct Member { int type = -1; GDExtensionPtrGetter get = nullptr; GDExtensionPtrSetter set = nullptr; };
        std::array<Method, Variant::VARIANT_MAX> methods{};
        std::array<Member, Variant::VARIANT_MAX> members{};
        explicit Name(const char *n);
    };
    std::unique_ptr<godot_jit::CModule> module;
    gdscript::IRProgram ir;
    std::string generated;
    String source_path;
    bool debug_info = false;
    std::vector<bool> uses_self;
    std::unordered_map<std::string, int> functions;
    HashMap<StringName, int> methods;
    std::vector<Name> names;
    std::vector<Variant> statics;
    Dictionary property_defaults;
    GJEntry entry = nullptr;
    using FunctionEntry = int (*)(GJContext *, GJVariant *, const GJVariant *const *, int);
    const FunctionEntry *function_entries = nullptr;
    static std::shared_ptr<NativeProgram> compile(const String &, const gdscript::CompilerOptions &, std::string &);
};
struct NativeState : std::enable_shared_from_this<NativeState> {
    // Reload creates a replacement NativeState. callp/Callable hold this state
    // through reentry, so its immutable program needs no second owning copy.
    const std::shared_ptr<NativeProgram> program;
    std::vector<Variant> members;
    ObjectID owner;
    GDExtensionObjectPtr attached_owner = nullptr;
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
    explicit NativeState(std::shared_ptr<NativeProgram> p, Object *object = nullptr, bool attached = false);
    bool invoke(int index, const Variant **args, int count, Variant &result, UnsafeFunctionState *resuming = nullptr);
    bool call(const StringName &, const Variant **, int, Variant &, GDExtensionCallError &, bool static_only = false);
};
Variant parameter_default(const gdscript::FunctionParameter &);
Dictionary native_property_defaults(const String &, const gdscript::CompilerOptions &, const gdscript::IRProgram &);
const std::vector<std::pair<std::string, const void *>> &native_symbols();
Variant native_callable(GJContext *, int, const Variant &);
} // namespace godot
