#include "native_program.h"
#include "unsafe_gdscript.h"
#include "unsafe_debugger.h"
#include <c_codegen.h>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/callable_custom.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot {
const std::vector<std::pair<std::string, const void *>> &native_symbols() {
    static const std::vector<std::pair<std::string, const void *>> symbols = {
        {"gj_await", reinterpret_cast<const void *>(&gj_await)},
        {"gj_await_restore", reinterpret_cast<const void *>(&gj_await_restore)},
        {"gj_copy", reinterpret_cast<const void *>(&gj_copy)},
        {"gj_destroy", reinterpret_cast<const void *>(&gj_destroy)},
        {"gj_truth", reinterpret_cast<const void *>(&gj_truth)},
        {"gj_op", reinterpret_cast<const void *>(&gj_op)},
        {"gj_array_next", reinterpret_cast<const void *>(&gj_array_next)},
        {"gj_fail", reinterpret_cast<const void *>(&gj_fail)},
    };
    return symbols;
}
std::shared_ptr<NativeProgram> NativeProgram::compile(const String &source, const gdscript::CompilerOptions &options,
                                                      std::string &error) {
    try {
        gdscript::Compiler compiler;
        auto utf8 = source.utf8();
        auto native_options = options;
        native_options.native_classes = true;
        auto ir = compiler.compile_to_ir(std::string(utf8.get_data(), utf8.length()), native_options);
        if (!ir) {
            error = compiler.get_error();
            return {};
        }
        auto p = std::make_shared<NativeProgram>();
        p->source_path = String::utf8(options.source_path.c_str());
        p->debug_info = options.debug_info || options.debug_step_points || !options.breakpoint_lines.empty();
        p->ir = std::move(*ir);
        p->property_defaults = native_property_defaults(source, options, p->ir);
        // Hidden accessors/lambdas publish only their name upstream. Their IR
        // still carries the complete ABI, including synthetic capture/self slots.
        for (size_t i = 0; i < p->ir.functions.size(); ++i) {
            auto &signature = p->ir.signatures[i];
            const auto &parameters = p->ir.functions[i].parameters;
            if (!signature.has_declaration) {
                signature.parameters.resize(parameters.size());
                signature.required_arguments = parameters.size();
                for (size_t j = 0; j < parameters.size(); ++j)
                    signature.parameters[j].name = parameters[j];
            }
        }
        p->generated = gdscript::CCodeGenerator().generate(p->ir, p->debug_info);
        p->module = godot_jit::CModule::compile(p->generated, error, native_symbols(), "gj_entry");
        if (!p->module)
            return {};
        p->entry = reinterpret_cast<GJEntry>(p->module->symbol("gj_entry"));
        p->function_entries = reinterpret_cast<const FunctionEntry *>(p->module->symbol("gj_functions"));
        for (size_t i = 0; i < p->ir.functions.size(); ++i) {
            p->functions.emplace(p->ir.functions[i].name, i);
            p->methods.insert(StringName(p->ir.functions[i].name.c_str()), i);
        }
        // Keys borrow immutable IR strings (or static literals). Keep names
        // interned for the module lifetime and avoid reconstructing them per op.
        for (size_t i = 0; i < p->ir.strings.size(); ++i)
            p->names.emplace(p->ir.strings[i], StringName(p->ir.strings[i].c_str()));
        for (const auto &name : p->ir.string_constants)
            p->names.emplace(name, StringName(name.c_str()));
        for (const char *name : {"call", "append", "size", "length", "get", "set", "erase", "has",
                                "keys", "values", "clear", "merge", "get_or_add"})
            p->names.emplace(name, StringName(name));
        p->statics.resize(p->ir.globals.size());
        return p;
    } catch (const std::exception &e) {
        error = e.what();
        return {};
    }
}
NativeState::NativeState(std::shared_ptr<NativeProgram> p, Object *object)
    : program(std::move(p)), owner(object ? object->get_instance_id() : ObjectID()) {
    members.resize(program->ir.globals.size());
}
bool NativeState::invoke(int index, const Variant **args, int count, Variant &result, UnsafeFunctionState *resuming) {
    auto active = program;
    // A transient reference protects RefCounted receivers during reentrant calls.
    // Construct from the engine object directly. Resolving a godot-cpp wrapper
    // first adds an instance-binding lookup to every script entry.
    GJVariant self{};
    if (owner.is_valid()) {
        auto object = gdextension_interface::object_get_instance_from_id(owner);
        static const auto from_object = gdextension_interface::get_variant_from_type_constructor(GDEXTENSION_VARIANT_TYPE_OBJECT);
        from_object(&self, &object);
    }
    GJContext context{reinterpret_cast<GJVariant *>(members.data()),
                      reinterpret_cast<GJVariant *>(&self),
                      &error,
                      0,
                      reinterpret_cast<GJVariant *>(active->statics.data()),
                      this,
                      active->debug_info ? &UnsafeDebugger::hook : nullptr, resuming};
    error.clear();
    const auto native_args = reinterpret_cast<const GJVariant *const *>(args);
    const bool ok = index >= 0 && size_t(index) < active->ir.functions.size() && active->function_entries
        ? active->function_entries[index](&context, reinterpret_cast<GJVariant *>(&result), native_args, count)
        : active->entry(&context, index, reinterpret_cast<GJVariant *>(&result), native_args, count);
    gj_clear(&self);
    return ok;
}
Variant parameter_default(const gdscript::FunctionParameter &p) {
    using K = gdscript::FunctionParameter::DefaultKind;
    switch (p.default_kind) {
    case K::INT:
        return std::get<int64_t>(p.default_value);
    case K::FLOAT:
        return std::get<double>(p.default_value);
    case K::BOOL:
        return std::get<bool>(p.default_value);
    case K::STRING:
        return String::utf8(std::get<std::string>(p.default_value).c_str());
    case K::EMPTY_ARRAY:
        return Array();
    case K::EMPTY_DICT:
        return Dictionary();
    default:
        return {};
    }
}
bool NativeState::call(const StringName &name, const Variant **args, int count, Variant &result,
                       GDExtensionCallError &error_out, bool static_only) {
    error_out = {};
    error.clear();
    gj_clear(reinterpret_cast<GJVariant *>(&result));
    const int *method = program->methods.getptr(name);
    if (!method) {
        error_out.error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
        return false;
    }
    const int index = *method;
    const auto &signature = program->ir.signatures.at(index);
    if (static_only && !signature.is_static) {
        error_out.error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
        return false;
    }
    const int implicit = program->ir.functions[index].parameters.size() - signature.parameters.size();
    const int total = signature.parameters.size() + implicit;
    if (count < int(signature.required_arguments) + implicit || count > total) {
        error_out.error =
            count > total ? GDEXTENSION_CALL_ERROR_TOO_MANY_ARGUMENTS : GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS;
        error_out.expected = count > total ? total : signature.required_arguments + implicit;
        return false;
    }
    // Most calls already have their declared types and need neither owned
    // argument copies nor heap buffers. Keep full conversion/class validation
    // below for defaults, coercions and named object types.
    bool exact = count == total;
    for (int i = implicit; exact && i < total; ++i) {
        const auto &parameter = signature.parameters[i - implicit];
        exact = parameter.class_name.empty() &&
                (parameter.type < 0 || parameter.type == int(reinterpret_cast<const GJVariant *>(args[i])->type));
    }
    if (exact) {
        if (invoke(index, args, count, result)) return true;
        error_out.error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
        return false;
    }
    std::vector<Variant> values(total);
    std::vector<const Variant *> pointers(total);
    for (int i = 0; i < total; ++i) {
        values[i] = i < count ? *args[i] : parameter_default(signature.parameters[i - implicit]);
        int type = i < implicit ? -1 : signature.parameters[i - implicit].type;
        if (type == Variant::DICTIONARY && !signature.parameters[i - implicit].class_name.empty()) {
            for (const auto &c : program->ir.class_signatures)
                if (!c.is_struct && c.name == signature.parameters[i - implicit].class_name)
                    type = Variant::OBJECT;
        }
        if (type >= 0 && type != values[i].get_type()) {
            if ((type == Variant::FLOAT && values[i].get_type() == Variant::INT) ||
                (type == Variant::INT && values[i].get_type() == Variant::FLOAT))
                values[i] = UtilityFunctions::type_convert(values[i], type);
            else if (!(type == Variant::OBJECT && values[i].get_type() == Variant::NIL)) {
                error_out.error = GDEXTENSION_CALL_ERROR_INVALID_ARGUMENT;
                error_out.argument = i;
                error_out.expected = type;
                return false;
            }
        }
        if (type == Variant::OBJECT && values[i].get_type() == Variant::OBJECT &&
            !signature.parameters[i - implicit].class_name.empty()) {
            Object *object = values[i];
            const String expected = String::utf8(signature.parameters[i - implicit].class_name.c_str());
            bool matches = !object || object->is_class(expected);
            if (!matches && object) {
                Ref<Script> script = object->get_script();
                while (script.is_valid() && !matches) {
                    matches = script->get_global_name() == StringName(expected);
                    auto *unsafe = Object::cast_to<UnsafeGDScript>(script.ptr());
                    if (unsafe && !unsafe->nested_name.is_empty()) {
                        String current = unsafe->nested_name;
                        while (!current.is_empty() && !matches) {
                            matches = current == expected;
                            String base;
                            for (const auto &c : unsafe->program->ir.class_signatures)
                                if (String::utf8(c.name.c_str()) == current)
                                    base = String::utf8(c.base_name.c_str());
                            current = base;
                        }
                    }
                    script = script->get_base_script();
                }
            }
            if (!matches) {
                error_out.error = GDEXTENSION_CALL_ERROR_INVALID_ARGUMENT;
                error_out.argument = i;
                error_out.expected = type;
                return false;
            }
        }
        pointers[i] = &values[i];
    }
    if (!invoke(index, pointers.data(), total, result)) {
        error_out.error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
        return false;
    }
    return true;
}
class NativeCallable final : public CallableCustom {
    std::weak_ptr<NativeState> state;
    std::weak_ptr<NativeProgram> program;
    int index;
    Variant bound;
    uint32_t stable_hash;
    bool lambda;

  public:
    NativeCallable(NativeState *s, int i, const Variant &b)
        : state(s->shared_from_this()), program(s->program), index(i), bound(b) {
        lambda = s->program->ir.functions[i].name.rfind("@lambda_", 0) == 0;
        stable_hash = lambda ? uint32_t(reinterpret_cast<uintptr_t>(this))
                             : uint32_t(reinterpret_cast<uintptr_t>(s)) ^ uint32_t(index) ^ bound.hash();
    }
    ObjectID get_object() const override {
        auto s = state.lock();
        return s ? s->owner : ObjectID();
    }
    bool is_valid() const override {
        auto s = state.lock();
        return s && s->program == program.lock() && (!s->owner.is_valid() || ObjectDB::get_instance(s->owner));
    }
    uint32_t hash() const override { return stable_hash; }
    String get_as_text() const override { return "UnsafeGDScript callable"; }
    static bool equal(const CallableCustom *a, const CallableCustom *b) {
        const auto *x = static_cast<const NativeCallable *>(a), *y = static_cast<const NativeCallable *>(b);
        if (x->lambda || y->lambda)
            return x == y;
        return !x->state.owner_before(y->state) && !y->state.owner_before(x->state) &&
               x->program.lock() == y->program.lock() && x->index == y->index && x->bound == y->bound;
    }
    CompareEqualFunc get_compare_equal_func() const override { return &equal; }
    CompareLessFunc get_compare_less_func() const override { return nullptr; }
    int get_argument_count(bool &valid) const override {
        auto s = state.lock();
        valid = is_valid();
        return valid ? int(s->program->ir.functions[index].parameters.size()) - (bound.get_type() != Variant::NIL) : 0;
    }
    void call(const Variant **args, int count, Variant &result, GDExtensionCallError &error) const override {
        auto s = state.lock();
        if (!is_valid()) {
            error = {};
            error.error = GDEXTENSION_CALL_ERROR_INSTANCE_IS_NULL;
            return;
        }
        std::vector<const Variant *> full;
        if (bound.get_type() != Variant::NIL)
            full.push_back(&bound);
        for (int i = 0; i < count; ++i)
            full.push_back(args[i]);
        s->call(StringName(s->program->ir.functions[index].name.c_str()), full.data(), full.size(), result, error);
    }
};
Variant native_callable(GJContext *ctx, int index, const Variant &bound) {
    if (!ctx->runtime)
        throw std::runtime_error("Generated Callable requires a native runtime");
    auto *state = static_cast<NativeState *>(ctx->runtime);
    // Native-backed nested method references use Godot's weak Object Callable.
    // Keeping their synthetic self Variant would create a RefCounted cycle.
    if (bound.get_type() == Variant::OBJECT) {
        Object *object = bound;
        if (object) {
            const auto &name = state->program->ir.functions[index].name;
            const auto dot = name.find('.');
            if (!name.empty() && name[0] == '@' && dot != std::string::npos)
                return Callable(object, StringName(name.substr(dot + 1).c_str()));
        }
    }
    return Callable(memnew(NativeCallable(state, index, bound)));
}
} // namespace godot
