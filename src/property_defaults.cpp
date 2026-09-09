#include "unsafe_gdscript.h"
#include <lexer.h>
#include <parser.h>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/node_path.hpp>

namespace godot {
namespace {
// The frontend's portable property metadata only carries scalar/empty defaults.
// Recover compound values from its AST using value constructors exclusively:
// never execute an initializer, user method, resource load or engine call here.
std::optional<Variant> constant_value(const gdscript::Expr *expr, const Dictionary &constants) {
    using namespace gdscript;
    if (auto *literal = dynamic_cast<const LiteralExpr *>(expr)) {
        switch (literal->lit_type) {
        case LiteralExpr::Type::INTEGER: return Variant(std::get<int64_t>(literal->value));
        case LiteralExpr::Type::FLOAT: return Variant(std::get<double>(literal->value));
        case LiteralExpr::Type::BOOL: return Variant(std::get<bool>(literal->value));
        case LiteralExpr::Type::NULL_VAL: return Variant();
        case LiteralExpr::Type::STRING: {
            String value = String::utf8(std::get<std::string>(literal->value).c_str());
            if (literal->string_type == LiteralExpr::StringType::STRING_NAME) return Variant(StringName(value));
            if (literal->string_type == LiteralExpr::StringType::NODE_PATH) return Variant(NodePath(value));
            return Variant(value);
        }
        }
    }
    if (auto *variable = dynamic_cast<const VariableExpr *>(expr)) {
        String name = String::utf8(variable->name.c_str());
        if (constants.has(name)) return constants[name];
    }
    if (auto *array = dynamic_cast<const ArrayLiteralExpr *>(expr)) {
        Array result;
        for (const auto &element : array->elements) {
            auto value = constant_value(element.get(), constants);
            if (!value) return std::nullopt;
            result.push_back(*value);
        }
        return Variant(result);
    }
    if (auto *dict = dynamic_cast<const DictionaryLiteralExpr *>(expr)) {
        Dictionary result;
        for (const auto &element : dict->elements) {
            auto key = constant_value(element.first.get(), constants);
            auto value = constant_value(element.second.get(), constants);
            if (!key || !value) return std::nullopt;
            result[*key] = *value;
        }
        return Variant(result);
    }
    if (auto *unary = dynamic_cast<const UnaryExpr *>(expr)) {
        auto value = constant_value(unary->operand.get(), constants);
        if (!value) return std::nullopt;
        Variant result;
        bool valid = false;
        auto op = unary->op == UnaryExpr::Op::NEG ? Variant::OP_NEGATE :
                  unary->op == UnaryExpr::Op::NOT ? Variant::OP_NOT : Variant::OP_BIT_NEGATE;
        Variant::evaluate(op, *value, Variant(), result, valid);
        if (valid) return result;
    }
    if (auto *call = dynamic_cast<const CallExpr *>(expr)) {
        if (call->has_named_arguments()) return std::nullopt;
        // Only built-in value types. In particular, Object/Callable/Signal
        // construction and arbitrary functions are not constant evaluation.
        for (int type = Variant::BOOL; type < Variant::VARIANT_MAX; ++type) {
            if (type == Variant::OBJECT || type == Variant::CALLABLE || type == Variant::SIGNAL || type == Variant::RID)
                continue;
            if (Variant::get_type_name(Variant::Type(type)) != String::utf8(call->function_name.c_str()))
                continue;
            std::vector<Variant> args;
            for (const auto &argument : call->arguments) {
                auto value = constant_value(argument.get(), constants);
                if (!value) return std::nullopt;
                args.push_back(*value);
            }
            std::vector<GDExtensionConstVariantPtr> pointers;
            for (const auto &argument : args) pointers.push_back(&argument);
            alignas(Variant) unsigned char storage[sizeof(Variant)];
            GDExtensionCallError error{};
            gdextension_interface::variant_construct(GDExtensionVariantType(type), storage,
                pointers.data(), pointers.size(), &error);
            auto &built = *reinterpret_cast<Variant *>(storage);
            Variant result = built;
            built.~Variant();
            if (error.error == GDEXTENSION_CALL_OK) return result;
            return std::nullopt;
        }
    }
    return std::nullopt;
}
} // namespace

Dictionary native_property_defaults(const String &source, const gdscript::CompilerOptions &options,
                                    const gdscript::IRProgram &ir) {
    Dictionary defaults;
    const Dictionary constants = unsafe_constants(ir);
    auto collect = [&](const std::string &text) {
        gdscript::Lexer lexer(text);
        gdscript::Parser parser(lexer.tokenize());
        auto parsed = parser.parse();
        for (const auto &global : parsed.globals) {
            if (global.is_const || global.is_static || global.is_onready) continue;
            StringName name(String::utf8(global.name.c_str()));
            defaults.erase(name);
            auto value = constant_value(global.initializer.get(), constants);
            // Scalar defaults (including declared numeric conversions) already
            // come from the frontend metadata. Only supplement compound values.
            if (value && value->get_type() > Variant::STRING) defaults[name] = *value;
        }
    };
    // Base sources are nearest-first; let derived declarations override them.
    for (auto base = options.base_sources.rbegin(); base != options.base_sources.rend(); ++base)
        if (!base->trait_only) collect(base->source);
    collect(source.utf8().get_data());
    return defaults;
}
} // namespace godot
