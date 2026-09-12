#include "c_abi.h"
#include "native_program.h"
#include "unsafe_gdscript.h"
#include <godot_cpp/classes/class_db_singleton.hpp>
#include <globals.h>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/templates/hashfuncs.hpp>
#include <new>
#include <stdexcept>
#include <string>
#include <cstdlib>

extern "C" GJReal gj_sqrt_real(GJReal value) { return godot::Math::sqrt(value); }

namespace {
using V = godot::Variant;
using godot::String;
using godot::StringName;
using godot::Array;
using godot::Dictionary;
using U = godot::UtilityFunctions;
static_assert(sizeof(GJVariant) == sizeof(V), "C ABI must match Godot Variant precision");
static_assert(alignof(GJVariant) == alignof(V));
static_assert(offsetof(GJVariant, data) == 8);
V &value(GJVariant *v) { return *reinterpret_cast<V *>(v); }
const V &value(const GJVariant *v) { return *reinterpret_cast<const V *>(v); }

void *array_storage(GJVariant *array) {
    static const auto get = godot::gdextension_interface::variant_get_ptr_internal_getter(GDEXTENSION_VARIANT_TYPE_ARRAY);
    return get(array);
}
GJInt array_size(void *array) {
    static const StringName name("size");
    static const auto size = godot::gdextension_interface::variant_get_ptr_builtin_method(
        GDEXTENSION_VARIANT_TYPE_ARRAY, name._native_ptr(), 3173160232);
    GJInt length;
    size(array, nullptr, &length, 0);
    return length;
}
void *builtin_payload(GJVariant *value, int type) {
    using Getter = decltype(godot::gdextension_interface::variant_get_ptr_internal_getter(GDEXTENSION_VARIANT_TYPE_BOOL));
    static const auto getters = [] {
        std::array<Getter, V::VARIANT_MAX> result{};
        for (int i = 1; i < V::VARIANT_MAX; ++i)
            result[i] = godot::gdextension_interface::variant_get_ptr_internal_getter(GDExtensionVariantType(i));
        return result;
    }();
    return type <= 0 ? value : getters[type](value);
}
void initialize_builtin_result(GJVariant &result, int type) {
    if (type <= 0) return;
    if (gj_trivial(type)) result.type = type;
    else {
        GDExtensionCallError error{};
        godot::gdextension_interface::variant_construct(GDExtensionVariantType(type), &result, nullptr, 0, &error);
        if (error.error != GDEXTENSION_CALL_OK) throw std::runtime_error("Cannot initialize builtin result");
    }
}

static uint32_t method_compatibility_hash(const godot::MethodInfo &method) {
	const bool has_return = method.return_val.type != V::NIL ||
			(method.return_val.usage & godot::PROPERTY_USAGE_NIL_IS_VARIANT);
	uint32_t hash = godot::hash_murmur3_one_32(has_return);
	hash = godot::hash_murmur3_one_32(uint32_t(method.arguments.size()), hash);
	if (has_return) {
		hash = godot::hash_murmur3_one_32(method.return_val.type, hash);
		if (method.return_val.class_name != StringName())
			hash = godot::hash_murmur3_one_32(method.return_val.class_name.hash(), hash);
	}
	for (const godot::PropertyInfo &arg : method.arguments) {
		hash = godot::hash_murmur3_one_32(arg.type, hash);
		if (arg.class_name != StringName())
			hash = godot::hash_murmur3_one_32(arg.class_name.hash(), hash);
	}
	hash = godot::hash_murmur3_one_32(uint32_t(method.default_arguments.size()), hash);
	for (const V &value : method.default_arguments)
		hash = godot::hash_murmur3_one_32(value.hash(), hash);
	hash = godot::hash_murmur3_one_32(method.flags & GDEXTENSION_METHOD_FLAG_CONST ? 1 : 0, hash);
	hash = godot::hash_murmur3_one_32(method.flags & GDEXTENSION_METHOD_FLAG_VARARG ? 1 : 0, hash);
	return godot::hash_fmix32(hash);
}

// Borrow the ABI pointer array. Helpers never retain it and publish their
// result only after reading inputs, preserving destination/input aliasing.
struct Arguments {
    const V *const *pointers;
    size_t length;
    const V *operator[](size_t i) const { return pointers[i]; }
    const V *at(size_t i) const {
        if (i >= length) throw std::runtime_error("Missing native operation argument");
        return pointers[i];
    }
    size_t size() const { return length; }
    bool empty() const { return length == 0; }
    const V *const *data() const { return pointers; }
    const V *const *begin() const { return pointers; }
    const V *const *end() const { return length ? pointers + length : pointers; }
};
V construct(int type, const Arguments &args) {
    if (type < 0 || type >= V::VARIANT_MAX) throw std::runtime_error("Invalid Variant constructor type");
    // variant_construct takes uninitialized storage, unlike the output ABI.
    alignas(V) unsigned char storage[sizeof(V)];
    GDExtensionCallError error{};
    godot::gdextension_interface::variant_construct(static_cast<GDExtensionVariantType>(type), storage,
        reinterpret_cast<const GDExtensionConstVariantPtr *>(args.data()), args.size(), &error);
    V &built = *reinterpret_cast<V *>(storage);
    V result = built;
    built.~Variant();
    if (error.error != GDEXTENSION_CALL_OK) throw std::runtime_error("Invalid Variant constructor arguments");
    return result;
}
V utility(gdscript::GlobalFn fn, const Arguments &a) {
    using namespace gdscript;
    const auto &info = global_function(fn);
    if (a.size() < info.min_args || a.size() > info.max_args) throw std::runtime_error("Wrong utility argument count");
    if (info.kind == GlobalKind::NUMERIC) {
        if (fn == GlobalFn::CLAMP) return U::clamp(*a[0], *a[1], *a[2]);
        if (fn == GlobalFn::MIN || fn == GlobalFn::MAX) {
            V result = *a[0];
            for (size_t i = 1; i < a.size(); ++i)
                result = fn == GlobalFn::MIN ? U::min(result, *a[i]) : U::max(result, *a[i]);
            return result;
        }
        bool ints = true;
        bool scalars = true;
        for (auto *v : a) {
            ints &= v->get_type() == V::INT;
            scalars &= v->get_type() == V::INT || v->get_type() == V::FLOAT;
        }
        if (!scalars) {
            // Generic GDScript math also accepts vectors. Scalar coercion here
            // silently loses their components (e.g. floor(Vector2) became 0.0).
            switch (fn) {
            case GlobalFn::ABS: return U::abs(*a[0]);
            case GlobalFn::SIGN: return U::sign(*a[0]);
            case GlobalFn::FLOOR: return U::floor(*a[0]);
            case GlobalFn::CEIL: return U::ceil(*a[0]);
            case GlobalFn::ROUND: return U::round(*a[0]);
            case GlobalFn::SNAPPED: return U::snapped(*a[0], *a[1]);
            case GlobalFn::WRAP: return U::wrap(*a[0], *a[1], *a[2]);
            default: throw std::runtime_error("Unsupported generic numeric utility");
            }
        }
        return utility(resolve_numeric_form(info, ints), a);
    }
    if (info.kind == GlobalKind::INT_OP) {
        int64_t args[UTILITY_MAX_FLOAT_ARGS]{};
        for (size_t j = 0; j < a.size(); ++j) args[j] = int64_t(*a[j]);
        return eval_global_int(fn, args, a.size());
    }
    if (info.kind == GlobalKind::FLOAT_OP || (info.kind == GlobalKind::SYSCALL && !info.impure)) {
        double args[UTILITY_MAX_FLOAT_ARGS]{};
        for (size_t j = 0; j < a.size(); ++j) args[j] = double(*a[j]);
        // Match the running engine, including transcendental implementations.
#define GJ_MATH_VALUES_1 args[0]
#define GJ_MATH_VALUES_2 args[0], args[1]
#define GJ_MATH_VALUES_3 args[0], args[1], args[2]
#define GJ_MATH_VALUES_5 args[0], args[1], args[2], args[3], args[4]
#define GJ_MATH_VALUES_8 args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]
#define GJ_CALL_MATH(id, name, result, count) case GlobalFn::id: return U::name(GJ_MATH_VALUES_##count);
        switch (fn) {
            GJ_ENGINE_MATH(GJ_CALL_MATH)
            case GlobalFn::LERP: return U::lerpf(args[0], args[1], args[2]);
            case GlobalFn::SNAPPEDI: return U::snappedi(args[0], int64_t(*a[1]));
            default: break;
        }
#undef GJ_CALL_MATH
#undef GJ_MATH_VALUES_1
#undef GJ_MATH_VALUES_2
#undef GJ_MATH_VALUES_3
#undef GJ_MATH_VALUES_5
#undef GJ_MATH_VALUES_8
        const double result = eval_global_float(fn, args, a.size());
        if (info.result == GlobalResult::INT) return int64_t(result);
        if (info.result == GlobalResult::BOOL) return bool(result);
        return result;
    }
    const V nil;
    const V &x = a.empty() ? nil : *a[0];
    const V &y = a.size() < 2 ? nil : *a[1];
    switch (fn) {
    case GlobalFn::TO_INT: return int64_t(x);
    case GlobalFn::TO_FLOAT: case GlobalFn::FLOAT_IDENTITY: return double(x);
    case GlobalFn::TO_BOOL: case GlobalFn::BOOLEANIZE: return x.booleanize();
    case GlobalFn::TO_STRING: return String(x);
    case GlobalFn::STR: { String s; for (auto *v : a) s += String(*v); return s; }
    case GlobalFn::LEN:
        if (x.get_type() == V::STRING || x.get_type() == V::STRING_NAME) return String(x).length();
        else { V obj = x, result; GDExtensionCallError error{}; obj.callp("size", nullptr, 0, result, error);
            if (error.error != GDEXTENSION_CALL_OK) throw std::runtime_error("Value has no length"); return result; }
    case GlobalFn::HASH: return U::hash(x);
    case GlobalFn::VAR_TO_STR: return U::var_to_str(x);
    case GlobalFn::STR_TO_VAR: return U::str_to_var(String(x));
    case GlobalFn::VAR_TO_BYTES: return U::var_to_bytes(x);
    case GlobalFn::BYTES_TO_VAR: return U::bytes_to_var(x);
    case GlobalFn::TYPE_STRING: return U::type_string(int64_t(x));
    case GlobalFn::TYPE_CONVERT: return U::type_convert(x, int64_t(y));
    case GlobalFn::ERROR_STRING: return U::error_string(int64_t(x));
    case GlobalFn::IS_SAME: return U::is_same(x, y);
    case GlobalFn::IS_INSTANCE_VALID: return x.get_type() == V::OBJECT && static_cast<godot::Object *>(x) != nullptr;
    case GlobalFn::IS_INSTANCE_OF: {
        if (y.get_type() == V::INT) return int64_t(x.get_type()) == int64_t(y);
        if (y.get_type() != V::OBJECT) throw std::runtime_error("Invalid is_instance_of type");
        auto *instance = x.get_type() == V::OBJECT ? static_cast<godot::Object *>(x) : nullptr;
        V script = instance ? instance->get_script() : V();
        for (int depth = 0; depth < 64 && script.get_type() == V::OBJECT && static_cast<godot::Object *>(script); ++depth) {
            if (script == y) return true;
            script = script.call("get_base_script");
        }
        return false;
    }
    case GlobalFn::CHAR: return String::chr(int64_t(x));
    case GlobalFn::ORD: return String(x).is_empty() ? 0 : int64_t(String(x).unicode_at(0));
    case GlobalFn::RAND_FROM_SEED: return U::rand_from_seed(int64_t(x));
    case GlobalFn::RANDOMIZE: U::randomize(); return {};
    case GlobalFn::SEED: U::seed(int64_t(x)); return {};
    case GlobalFn::RANDF: return U::randf();
    case GlobalFn::RANDF_RANGE: return U::randf_range(double(x), double(y));
    case GlobalFn::RANDFN: return U::randfn(double(x), double(y));
    case GlobalFn::RANDI: return U::randi();
    case GlobalFn::RANDI_RANGE: return U::randi_range(int64_t(x), int64_t(y));
    case GlobalFn::NEAREST_PO2: return U::nearest_po2(int64_t(x));
    default: throw std::runtime_error(std::string("Unsupported utility: ") + info.name);
    }
}
}

extern "C" void gj_copy(GJVariant *dst, const GJVariant *src) {
    if (dst == src) return;
    if (gj_trivial(dst->type)) {
        // No owned payload to destroy. Avoid Variant::operator='s extension
        // calls to discover the tag and clear an already-trivial destination.
        godot::gdextension_interface::variant_new_copy(dst, src);
        return;
    }
    // Native copy constructors maintain String/container/RefCounted ownership.
    value(dst) = value(src);
}
extern "C" void gj_destroy(GJVariant *v) {
    godot::gdextension_interface::variant_destroy(v);
    // Reinitialize the native ABI's nil representation without a second
    // extension call. Do not end the surrounding godot-cpp wrapper's lifetime.
    v->type = V::NIL;
    v->data.i = 0;
}
extern "C" int gj_truth(const GJVariant *v) { return value(v).booleanize(); }
extern "C" int gj_fail(GJContext *ctx, const char *message) {
    const bool first = !ctx->failed;
    if (first && ctx->error) *static_cast<std::string *>(ctx->error) = message;
    ctx->failed = 1;
    if (first && ctx->debug) ctx->debug(ctx, nullptr, GJ_DEBUG_ERROR);
    return 0;
}
extern "C" int gj_array_next(GJContext *ctx, GJVariant *item, GJVariant *array, GJInt index) {
    if (ctx->failed) return -1;
    try {
        if (array->type == V::ARRAY && index >= 0) {
            void *storage = array_storage(array);
            if (index >= array_size(storage)) return 0;
            const auto *element = reinterpret_cast<const GJVariant *>(
                godot::gdextension_interface::array_operator_index_const(storage, index));
            // Snapshot before clearing item, including when it owns the Array.
            GJVariant result{};
            gj_move(&result, element);
            gj_clear(item);
            *item = result;
            return 1;
        }
        // Preserve the ordinary size/compare/get behavior for other receivers
        // and negative indices. Normal Array loops never take this branch.
        GJVariant length{}, position{}, test{};
        gj_int(&position, index);
        const GJVariant *comparison[] = {&position, &length};
        bool ok = gj_op(ctx, GJ_ARRAY_SIZE, &length, array, -1, -1, nullptr, 0) &&
            gj_op(ctx, GJ_EVALUATE, &test, nullptr, -1, V::OP_LESS, comparison, 2);
        const bool more = ok && gj_boolean(&test);
        gj_clear(&length);
        gj_clear(&test);
        if (more) {
            const GJVariant *args[] = {&position};
            ok = gj_op(ctx, GJ_GET, item, array, -1, -1, args, 1);
        }
        return !ok ? -1 : more ? 1 : 0;
    } catch (const std::exception &e) {
        gj_fail(ctx, e.what());
        return -1;
    } catch (...) {
        gj_fail(ctx, "Native C array iteration failed");
        return -1;
    }
}
extern "C" int gj_array_next_vector(GJContext *ctx, GJReal *item, GJVariant *array, GJInt index, int type) {
    if (ctx->failed) return -1;
    try {
        void *storage = array_storage(array);
        const GJInt length = array_size(storage);
        if (index >= length) return 0;
        if (index < 0) index += length;
        if (index < 0) throw std::runtime_error("Invalid Array index");
        const auto *element = reinterpret_cast<const GJVariant *>(godot::gdextension_interface::array_operator_index_const(storage, index));
        const int width = type == V::VECTOR2 ? 2 : type == V::VECTOR3 ? 3 : 4;
        if (int(element->type) == type) {
            for (int j = 0; j < width; ++j) item[j] = element->data.real[j];
        } else {
            if (!V::can_convert_strict(V::Type(element->type), V::Type(type)))
                throw std::runtime_error("Invalid typed Array element");
            const V *args[] = {reinterpret_cast<const V *>(element)};
            V converted = construct(type, {args, 1});
            const auto *result = reinterpret_cast<const GJVariant *>(&converted);
            for (int j = 0; j < width; ++j) item[j] = result->data.real[j];
        }
        return 1;
    } catch (const std::exception &error) {
        gj_fail(ctx, error.what());
        return -1;
    } catch (...) {
        gj_fail(ctx, "Native typed Array iteration failed");
        return -1;
    }
}
extern "C" int gj_op(GJContext *ctx, int operation, GJVariant *dst, GJVariant *self,
    int name_id, int detail, const GJVariant *const *args, int count) {
    if (ctx->failed) return 0;
    try {
        if (count < 0) throw std::runtime_error("Invalid native operation argument count");
        const godot::NativeProgram::Name *cached = nullptr;
        if (name_id >= 0) {
            if (!ctx->runtime) throw std::runtime_error("Named operation requires a module runtime");
            const auto &names = static_cast<godot::NativeState *>(ctx->runtime)->program->names;
            if (size_t(name_id) >= names.size()) throw std::runtime_error("Invalid native name id");
            cached = &names[name_id];
        }
        const char *name = cached ? cached->text : nullptr;
        if (operation == GJ_EVALUATE && detail >= 0 && detail < 25 && detail != 9 && detail != 12 && count >= 1 && count <= 2 &&
            args[0]->type < V::VARIANT_MAX && (count == 1 || args[1]->type < V::VARIANT_MAX)) {
            static const auto *operators = godot::native_operators();
            const int left = args[0]->type, right = count == 1 ? V::NIL : args[1]->type;
            const auto &entry = operators[(detail * V::VARIANT_MAX + left) * V::VARIANT_MAX + right];
            if (entry.call) {
                GJVariant result{};
                initialize_builtin_result(result, entry.result);
                entry.call(builtin_payload(const_cast<GJVariant *>(args[0]), left),
                    count == 1 ? nullptr : builtin_payload(const_cast<GJVariant *>(args[1]), right),
                    builtin_payload(&result, entry.result));
                gj_take(dst, &result);
                return 1;
            }
        }
        // Resolve these fixed built-ins once. Borrow the engine's internal Array
        // through its public extension interface; no retain/release or name lookup.
        if (operation == GJ_ARRAY_SIZE) {
            if (self && self->type == V::ARRAY && count == 0) {
                const GJInt length = array_size(array_storage(self));
                gj_int(dst, length);
                return 1;
            }
            operation = GJ_CALL;
            name = "size";
            if (cached && std::strcmp(cached->text, name)) cached = nullptr;
        } else if (operation == GJ_VECTOR2_NORMALIZED) {
            if (self && self->type == V::VECTOR2 && count == 0) {
                static const StringName normalized_name("normalized");
                static const auto normalize = godot::gdextension_interface::variant_get_ptr_builtin_method(
                    GDEXTENSION_VARIANT_TYPE_VECTOR2, normalized_name._native_ptr(), 2428350749);
                GJVariant result{};
                result.type = V::VECTOR2;
                normalize(self->data.real, nullptr, result.data.real, 0);
                gj_move(dst, &result);
                return 1;
            }
            operation = GJ_CALL;
            name = "normalized";
            if (cached && std::strcmp(cached->text, name)) cached = nullptr;
        }
        if (operation == GJ_GET && self && self->type == V::ARRAY && count == 1 && args[0]->type == V::INT) {
            // The checked indexed API preserves negative indexes and bounds errors.
            // It constructs an uninitialized result, which must outlive input reads
            // when dst aliases the Array or the index.
            GJVariant result;
            GDExtensionBool valid, out_of_bounds;
            godot::gdextension_interface::variant_get_indexed(self, args[0]->data.i, &result, &valid, &out_of_bounds);
            if (!valid || out_of_bounds) {
                gj_clear(&result);
                return gj_fail(ctx, "Invalid Variant operation or property/index access");
            }
            // Transfer the freshly constructed result's ownership to dst.
            gj_clear(dst);
            *dst = result;
            return 1;
        }
        const bool named = operation == GJ_CALL || operation == GJ_SUPER_CALL ||
            operation == GJ_GET_NAMED || operation == GJ_SET_NAMED ||
            operation == GJ_DICTIONARY_HAS || operation == GJ_GET_OBJECT ||
            operation == GJ_NEW_OBJECT || operation == GJ_CALLABLE;
        if (operation == GJ_CALL && cached && self && self->type == V::CALLABLE && cached->callable_op) {
            using Name = godot::NativeProgram::Name;
            static const auto storage = godot::gdextension_interface::variant_get_ptr_internal_getter(GDEXTENSION_VARIANT_TYPE_CALLABLE);
            static const auto call = godot::gdextension_interface::variant_get_ptr_builtin_method(
                GDEXTENSION_VARIANT_TYPE_CALLABLE, StringName("call")._native_ptr(), 3643564216);
            static const auto callv = godot::gdextension_interface::variant_get_ptr_builtin_method(
                GDEXTENSION_VARIANT_TYPE_CALLABLE, StringName("callv")._native_ptr(), 413578926);
            static const auto bind = godot::gdextension_interface::variant_get_ptr_builtin_method(
                GDEXTENSION_VARIANT_TYPE_CALLABLE, StringName("bind")._native_ptr(), 3224143119);
            // The variadic ptrcall adapter measured more instructions on Godot
            // 4.6. Keep it measurable; the default retains the cheaper path.
            static const bool variadic_ptrcall = [] {
                const char *value = std::getenv("GODOT_JIT_CALLABLE_PTRCALL");
                return value && std::strcmp(value, "1") == 0;
            }();
            if ((cached->callable_op == Name::CALL && variadic_ptrcall) || cached->callable_op == Name::BIND ||
                (cached->callable_op == Name::CALLV && count == 1 && args[0]->type == V::ARRAY)) {
                GJVariant snapshot{};
                if (cached->callable_op == Name::CALL)
                    call(storage(self), reinterpret_cast<const void **>(const_cast<const GJVariant **>(args)), &snapshot, count);
                else if (cached->callable_op == Name::BIND) {
                    initialize_builtin_result(snapshot, V::CALLABLE);
                    bind(storage(self), reinterpret_cast<const void **>(const_cast<const GJVariant **>(args)), storage(&snapshot), count);
                }
                else {
                    const void *arguments[] = {array_storage(const_cast<GJVariant *>(args[0]))};
                    callv(storage(self), arguments, &snapshot, 1);
                }
                gj_take(dst, &snapshot);
                return 1;
            }
        }
        if (cached && self && self->type > V::NIL && self->type < V::VARIANT_MAX && self->type != V::OBJECT) {
            const auto &method = cached->methods[self->type];
            if (operation == GJ_CALL && method.call && count == method.info->count) {
                const auto &info = *method.info;
                const void *arguments[16];
                bool exact = true;
                for (int j = 0; j < count; ++j) {
                    const int type = info.arguments[j];
                    if (type >= 0 && int(args[j]->type) != type) { exact = false; break; }
                    arguments[j] = builtin_payload(const_cast<GJVariant *>(args[j]), type);
                }
                if (exact) {
                    GJVariant result{};
                    initialize_builtin_result(result, info.result);
                    method.call(builtin_payload(self, self->type), arguments,
                        info.result == -2 ? nullptr : builtin_payload(&result, info.result), count);
                    gj_take(dst, &result);
                    return 1;
                }
            }
            const auto &member = cached->members[self->type];
            if (operation == GJ_GET_NAMED && member.get && count == 0) {
                GJVariant result{};
                initialize_builtin_result(result, member.type);
                member.get(builtin_payload(self, self->type), builtin_payload(&result, member.type));
                gj_take(dst, &result);
                return 1;
            }
            if (operation == GJ_SET_NAMED && member.set && count == 1 && int(args[0]->type) == member.type) {
                member.set(builtin_payload(self, self->type), builtin_payload(const_cast<GJVariant *>(args[0]), member.type));
                gj_clear(dst);
                return 1;
            }
        }
        if (cached && operation == GJ_CALL && self) {
            GJVariant snapshot;
            GDExtensionCallError error{};
            static constexpr const char *static_prefix = "__safegdscript_static__:";
            const bool builtin_static = self->type == V::STRING &&
                String(value(self)).begins_with(static_prefix);
            if (builtin_static) {
                const int type = String(value(self)).trim_prefix(static_prefix).to_int();
                if (type < V::NIL || type >= V::VARIANT_MAX)
                    throw std::runtime_error("Invalid built-in static Variant type");
                godot::gdextension_interface::variant_call_static(
                    static_cast<GDExtensionVariantType>(type), cached->name._native_ptr(),
                    reinterpret_cast<const GDExtensionConstVariantPtr *>(args), count, &snapshot, &error);
            } else {
                godot::gdextension_interface::variant_call(self, cached->name._native_ptr(),
                    reinterpret_cast<const GDExtensionConstVariantPtr *>(args), count, &snapshot, &error);
            }
            if (error.error != GDEXTENSION_CALL_OK) {
                gj_clear(&snapshot);
                return gj_fail(ctx, (std::string("Method call failed: ") + name).c_str());
            }
            gj_clear(dst);
            *dst = snapshot;
            return 1;
        }
        if (cached && self && self->type == V::DICTIONARY) {
            if (operation == GJ_GET_NAMED) {
                // The public Dictionary index pointer API cannot report missing
                // keys. The checked Variant getter preserves that error and
                // constructs an owned snapshot, including when dst aliases self.
                GJVariant snapshot;
                GDExtensionBool ok;
                godot::gdextension_interface::variant_get_keyed(self, cached->key._native_ptr(), &snapshot, &ok);
                if (!ok) {
                    gj_clear(&snapshot);
                    return gj_fail(ctx, "Invalid Variant operation or property/index access");
                }
                gj_clear(dst);
                *dst = snapshot;
                return 1;
            }
            if (operation == GJ_SET_NAMED && count == 1) {
                GDExtensionBool ok;
                // Use the checked setter so typed/read-only Dictionaries retain
                // validation. Pooled keys avoid temporary StringName ownership.
                godot::gdextension_interface::variant_set_keyed(self,
                    (detail == 4 ? cached->string_key : cached->key)._native_ptr(), args[0], &ok);
                if (!ok) return gj_fail(ctx, "Invalid Variant operation or property/index access");
                gj_clear(dst);
                return 1;
            }
        }
        const Arguments a{reinterpret_cast<const V *const *>(args), size_t(count)};
        V result;
        bool valid = true;
        StringName uncached;
        if (!cached && named && name) uncached = StringName(name);
        const StringName &member = cached ? cached->name : uncached;
        auto object = [&]() -> V & {
            if (!self) throw std::runtime_error("This operation requires a receiver");
            return value(self);
        };
        switch (operation) {
        case GJ_EVALUATE: {
            V nil;
            V::evaluate(static_cast<V::Operator>(detail), *a.at(0), count > 1 ? *a[1] : nil, result, valid);
            break;
        }
        case GJ_CONSTRUCT: result = construct(detail, a); break;
        case GJ_COERCE:
            if (count != 1 || detail < 0 || detail >= V::VARIANT_MAX ||
                !V::can_convert_strict(a.at(0)->get_type(), V::Type(detail)))
                throw std::runtime_error("Invalid typed argument or assignment");
            result = construct(detail, a);
            break;
        case GJ_STRING: result = String::utf8(name, detail); break;
        case GJ_CALL: {
            GDExtensionCallError error{};
            static constexpr const char *static_prefix = "__safegdscript_static__:";
            const bool builtin_static = object().get_type() == V::STRING &&
                String(object()).begins_with(static_prefix);
            if (builtin_static) {
                const String receiver = object();
                const int type = receiver.trim_prefix(static_prefix).to_int();
                if (type < V::NIL || type >= V::VARIANT_MAX)
                    throw std::runtime_error("Invalid built-in static Variant type");
                godot::gdextension_interface::variant_call_static(
                    static_cast<GDExtensionVariantType>(type), member._native_ptr(),
                    reinterpret_cast<const GDExtensionConstVariantPtr *>(a.data()), count,
                    result._native_ptr(), &error);
            } else {
                object().callp(member, const_cast<const V **>(a.data()), count, result, error);
            }
            if (error.error != GDEXTENSION_CALL_OK) throw std::runtime_error(std::string("Method call failed: ") + name);
            break;
        }
        case GJ_CLASS_BIND: result = godot::bind_native_class(ctx, String(name), object()); break;
        case GJ_SUPER_CALL: {
            auto *receiver = static_cast<godot::Object *>(object());
            if (!receiver) throw std::runtime_error("super requires an Object");
            // MethodBind bypasses script overrides, preventing recursive dispatch.
            auto methods = godot::ClassDBSingleton::get_singleton()->class_get_method_list(receiver->get_class());
            GDExtensionMethodBindPtr bind = nullptr;
            const StringName receiver_class = receiver->get_class();
            for (int i = 0; i < methods.size(); ++i) {
                Dictionary m = methods[i];
                if (StringName(m["name"]) == member) {
                    bind = godot::gdextension_interface::classdb_get_method_bind(&receiver_class, &member, method_compatibility_hash(godot::MethodInfo::from_dict(m)));
                    break;
                }
            }
            if (!bind) throw std::runtime_error("Native super method not found");
            GDExtensionCallError error{};
            godot::gdextension_interface::object_method_bind_call(bind, receiver->_owner,
                reinterpret_cast<const GDExtensionConstVariantPtr *>(a.data()), count, result._native_ptr(), &error);
            if (error.error != GDEXTENSION_CALL_OK) throw std::runtime_error("Native super call failed");
            break;
        }
        case GJ_TRAIT_TEST: {
            if (!ctx->runtime) throw std::runtime_error("Trait tests require a native runtime");
            auto *state = static_cast<godot::NativeState *>(ctx->runtime);
            const auto &trait = state->program->ir.trait_signatures.at(detail);
            auto *receiver = object().get_type() == V::OBJECT ? static_cast<godot::Object *>(object()) : nullptr;
            bool matches = false;
            if (receiver) {
                godot::Ref<godot::UnsafeGDScript> script = receiver->get_script();
                if (script.is_valid()) for (const auto &use : script->program->ir.script_uses) matches |= use == trait.name;
                if (!matches && state->program->ir.trait_structural_fallback) {
                    matches = true;
                    for (const auto &method : trait.trait_methods) if (!method.is_static) matches &= receiver->has_method(StringName(method.name.c_str()));
                }
            }
            result = matches; break;
        }
        case GJ_GET: {
            GDExtensionBool ok;
            godot::gdextension_interface::variant_get(object()._native_ptr(), a.at(0)->_native_ptr(), result._native_ptr(), &ok);
            valid = ok;
            break;
        }
        case GJ_SET: object().set(*a.at(0), *a.at(1), &valid); break;
        case GJ_GET_NAMED: {
            GDExtensionBool ok;
            godot::gdextension_interface::variant_get_named(object()._native_ptr(), member._native_ptr(), result._native_ptr(), &ok);
            valid = ok;
            break;
        }
        case GJ_SET_NAMED:
            if (object().get_type() == V::DICTIONARY) object().set(detail == 4 ? V(String(name)) : V(member), *a.at(0), &valid);
            else object().set_named(member, *a.at(0), valid);
            break;
        case GJ_DICTIONARY_HAS: result = object().get_type() == V::DICTIONARY && Dictionary(object()).has(member); break;
        case GJ_STRUCT_CHECK: result = object().get_type() == V::DICTIONARY && Dictionary(object()).size() == detail; break;
        case GJ_PACKED_ARRAY: result = construct(detail, a); break;
        case GJ_ARRAY: {
            Array array;
            array.resize(count);
            for (int j = 0; j < count; ++j) array[j] = *a[j];
            result = array;
            break;
        }
        case GJ_DICTIONARY: {
            Dictionary dictionary;
            for (int j = 0; j < count; j += 2) dictionary[*a[j]] = *a.at(j+1);
            result = dictionary;
            break;
        }
        case GJ_UTILITY: result = utility(static_cast<gdscript::GlobalFn>(detail), a); break;
        case GJ_PRINT: {
            String message;
            for (int j = 0; j < count; ++j) {
                if (j && detail == 1) message += " ";
                if (j && detail == 2) message += "\t";
                message += String(*a[j]);
            }
            switch (detail) {
            case 0: case 1: case 2: U::print(message); break;
            case 3: U::printraw(message); break;
            case 4: U::print_rich(message); break;
            case 5: U::printerr(message); break;
            case 6: U::print_verbose(message); break;
            case 7: U::push_error(message); break;
            case 8: U::push_warning(message); break;
            default: throw std::runtime_error("Unknown print channel");
            }
            break;
        }
        case GJ_LOAD: result = godot::ResourceLoader::get_singleton()->load(name ? String(name) : String(*a.at(0))); break;
        case GJ_GET_OBJECT: {
            if (std::string(name) == "self") result = object();
            else if (godot::Engine::get_singleton()->has_singleton(member)) {
                // Dynamic singleton lookup must not create an untracked
                // godot-cpp instance binding that survives extension unload.
                auto singleton = godot::gdextension_interface::global_get_singleton(member._native_ptr());
                static const auto from_object = godot::gdextension_interface::get_variant_from_type_constructor(GDEXTENSION_VARIANT_TYPE_OBJECT);
                from_object(result._native_ptr(), &singleton);
            }
            else {
                auto *tree = godot::Object::cast_to<godot::SceneTree>(godot::Engine::get_singleton()->get_main_loop());
                auto *autoload = tree && tree->get_root() ? tree->get_root()->get_node_or_null(godot::NodePath(name)) : nullptr;
                if (!autoload) throw std::runtime_error(std::string("Unknown engine singleton or autoload: ") + name);
                result = autoload;
            }
            break;
        }
        case GJ_NEW_OBJECT:
            result = godot::ClassDB::instantiate(member);
            if (result.get_type() == V::NIL) throw std::runtime_error(std::string("Cannot construct engine class: ") + name);
            break;
        case GJ_GET_NODE: {
            if (String(name) == ".") { result = object(); break; }
            auto *node = godot::Object::cast_to<godot::Node>(static_cast<godot::Object *>(object()));
            if (!node) throw std::runtime_error("get_node requires a Node receiver");
            result = node->get_node_or_null(godot::NodePath(name));
            break;
        }
        case GJ_CALLABLE:
            if (detail >= 0) { result = godot::native_callable(ctx, detail, object()); break; }
            if (!name || !*name) { result = godot::Callable(); break; }
            if (object().get_type() != V::OBJECT) throw std::runtime_error("Callable requires a native Object receiver");
            result = godot::Callable(static_cast<godot::Object *>(object()), member);
            break;
        default: throw std::runtime_error("Unknown C syscall operation");
        }
        if (!valid) {
            std::string message = "Invalid Variant operation or property/index access";
            if (name && *name) message += std::string(": ") + name;
            if (self) message += std::string(" on ") + V::get_type_name(value(self).get_type()).utf8().get_data();
            if (operation == GJ_EVALUATE) message += " (operator " + std::to_string(detail) + ")";
            throw std::runtime_error(message);
        }
        gj_move(dst, reinterpret_cast<const GJVariant *>(&result));
        return 1;
    } catch (const std::exception &error) { return gj_fail(ctx, error.what()); }
    catch (...) { return gj_fail(ctx, "Native C syscall failed"); }
}
