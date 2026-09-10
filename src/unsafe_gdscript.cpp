#include "unsafe_gdscript.h"
#include "script_dicts.h"
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/engine_debugger.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/signal.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <lexer.h>
#include <mutex>
#include <parser.h>

namespace godot {
namespace {
std::unordered_set<UnsafeGDScript *> scripts;
std::mutex scripts_mutex;
String str(const std::string &s) {
    return String::utf8(s.c_str());
}
PropertyInfo type_info(int type, const String &name = String(), const String &class_name = String()) {
    PropertyInfo p(type < 0 ? Variant::NIL : Variant::Type(type), name);
    p.class_name = class_name;
    if (type < 0)
        p.usage |= PROPERTY_USAGE_NIL_IS_VARIANT;
    return p;
}
MethodInfo method_info(const gdscript::FunctionSignature &s) {
    MethodInfo m;
    m.name = str(s.name);
    m.return_val = type_info(s.is_coroutine ? -1 : s.return_type, "", s.is_coroutine ? String() : str(s.return_class_name));
    m.flags = METHOD_FLAGS_DEFAULT | (s.is_static ? METHOD_FLAG_STATIC : 0);
    for (const auto &p : s.parameters) {
        m.arguments.push_back(type_info(p.type, str(p.name), str(p.class_name)));
        if (p.optional())
            m.default_arguments.push_back(parameter_default(p));
    }
    return m;
}
Variant property_default(const gdscript::PropertySignature &p) {
    using K = gdscript::PropertyDefaultKind;
    switch (p.default_kind) {
    case K::INT:
        return std::get<int64_t>(p.default_value);
    case K::FLOAT:
        return std::get<double>(p.default_value);
    case K::BOOL:
        return std::get<bool>(p.default_value);
    case K::STRING:
        return str(std::get<std::string>(p.default_value));
    case K::EMPTY_ARRAY:
        return Array();
    case K::EMPTY_DICTIONARY:
        return Dictionary();
    default:
        return {};
    }
}
GDExtensionPropertyInfo native_property(const PropertyInfo &p) {
    return {GDExtensionVariantType(p.type),   memnew(StringName(p.name)),
            memnew(StringName(p.class_name)), p.hint,
            memnew(String(p.hint_string)),    p.usage};
}
void free_property(const GDExtensionPropertyInfo &p) {
    memdelete(static_cast<StringName *>(p.name));
    memdelete(static_cast<StringName *>(p.class_name));
    memdelete(static_cast<String *>(p.hint_string));
}
} // namespace
void UnsafeGDScript::_bind_methods() {
    ClassDB::bind_vararg_method(METHOD_FLAGS_DEFAULT, "new", &UnsafeGDScript::new_instance, MethodInfo("new"));
    ClassDB::bind_method(D_METHOD("get_compile_error"), &UnsafeGDScript::get_compile_error);
    ClassDB::bind_method(D_METHOD("get_generated_c"), &UnsafeGDScript::get_generated_c);
}
bool UnsafeGDScript::_set(const StringName &n, const Variant &v) {
    if (n != StringName("script/source"))
        return false;
    _set_source_code(v);
    _reload(false);
    return true;
}
bool UnsafeGDScript::_get(const StringName &n, Variant &v) const {
    if (n != StringName("script/source"))
        return false;
    v = source;
    return true;
}
void UnsafeGDScript::_get_property_list(List<PropertyInfo> *p) const {
    p->push_back(PropertyInfo(Variant::STRING, "script/source", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE));
}
bool UnsafeGDScript::_can_instantiate() const {
    return program && (_is_tool() || !Engine::get_singleton()->is_editor_hint());
}
StringName UnsafeGDScript::_get_global_name() const {
    return program ? StringName(str(program->ir.class_name)) : StringName();
}
StringName UnsafeGDScript::_get_instance_base_type() const {
    if (program && !nested_name.is_empty())
        for (const auto &c : program->ir.class_signatures)
            if (str(c.name) == nested_name)
                return StringName(str(c.native_base));
    return program && !program->ir.native_base_class.empty() ? StringName(str(program->ir.native_base_class))
                                                             : StringName("RefCounted");
}
bool UnsafeGDScript::_inherits_script(const Ref<Script> &s) const {
    for (auto b = base_script; b.is_valid(); b = b->get_base_script())
        if (b == s)
            return true;
    return false;
}
ScriptLanguage *UnsafeGDScript::_get_language() const {
    return unsafe_language();
}
int UnsafeGDScript::method_index(const StringName &n) const {
    if (!program)
        return -1;
    if (!nested_name.is_empty()) {
        String current = nested_name;
        while (!current.is_empty()) {
            auto found = program->functions.find(("@" + current + "." + String(n)).utf8().get_data());
            if (found != program->functions.end())
                return found->second;
            String base;
            for (const auto &c : program->ir.class_signatures)
                if (str(c.name) == current)
                    base = str(c.base_name);
            current = base;
        }
        return -1;
    }
    const int *index = program->methods.getptr(n);
    return index ? *index : -1;
}
int UnsafeGDScript::property_index(const StringName &n) const {
    if (program)
        for (size_t i = 0; i < program->ir.globals.size(); ++i)
            if (str(program->ir.globals[i].name) == String(n))
                return i;
    return -1;
}
bool UnsafeGDScript::_has_method(const StringName &n) const {
    return method_index(n) >= 0;
}
bool UnsafeGDScript::_has_static_method(const StringName &n) const {
    int i = method_index(n);
    return i >= 0 && program->ir.signatures[i].is_static;
}
Variant UnsafeGDScript::_get_script_method_argument_count(const StringName &n) const {
    int i = method_index(n);
    return i < 0 ? Variant() : Variant(int64_t(program->ir.signatures[i].parameters.size()));
}
Dictionary UnsafeGDScript::_get_method_info(const StringName &n) const {
    int i = method_index(n);
    return i < 0 ? Dictionary() : method_dict(method_info(program->ir.signatures[i]));
}
std::vector<MethodInfo> UnsafeGDScript::methods() const {
    std::vector<MethodInfo> m;
    if (program && !nested_name.is_empty()) {
        std::unordered_set<std::string> seen;
        String current = nested_name;
        while (!current.is_empty()) {
            String base;
            for (const auto &c : program->ir.class_signatures)
                if (str(c.name) == current) {
                    base = str(c.base_name);
                    for (const auto &method : c.methods)
                        if (seen.insert(method.name).second) {
                            auto info = method_info(program->ir.signatures[method_index(str(method.name))]);
                            info.name = str(method.name);
                            m.push_back(info);
                        }
                }
            current = base;
        }
        return m;
    }
    if (program)
        for (const auto &s : program->ir.signatures)
            if (!s.name.empty() && s.name[0] != '@')
                m.push_back(method_info(s));
    return m;
}
std::vector<PropertyInfo> UnsafeGDScript::properties() const {
    std::vector<PropertyInfo> out;
    if (program && !nested_name.is_empty()) {
        for (const auto &c : program->ir.class_signatures)
            if (str(c.name) == nested_name)
                for (const auto &field : c.fields)
                    out.push_back(type_info(field.type, str(field.name), str(field.class_name)));
        return out;
    }
    if (program)
        for (const auto &p : program->ir.properties) {
            auto info = type_info(p.type, str(p.name), str(p.class_name));
            info.hint = PropertyHint(p.hint);
            info.hint_string = str(p.hint_string);
            info.usage = p.usage | (p.type < 0 ? PROPERTY_USAGE_NIL_IS_VARIANT : 0);
            out.push_back(info);
        }
    return out;
}
TypedArray<Dictionary> UnsafeGDScript::_get_script_method_list() const {
    TypedArray<Dictionary> a;
    for (auto &m : methods())
        a.push_back(method_dict(m));
    return a;
}
TypedArray<Dictionary> UnsafeGDScript::_get_script_property_list() const {
    TypedArray<Dictionary> a;
    for (auto &p : properties())
        a.push_back(property_dict(p));
    return a;
}
bool UnsafeGDScript::_has_script_signal(const StringName &n) const {
    if (program)
        for (auto &s : program->ir.signals)
            if (str(s.name) == String(n))
                return true;
    return false;
}
TypedArray<Dictionary> UnsafeGDScript::_get_script_signal_list() const {
    TypedArray<Dictionary> a;
    if (program)
        for (auto &s : program->ir.signals)
            a.push_back(method_dict(method_info(s), ""));
    return a;
}
bool UnsafeGDScript::_has_property_default_value(const StringName &n) const {
    if (program && program->property_defaults.has(n)) return true;
    if (program)
        for (auto &p : program->ir.properties)
            if (str(p.name) == String(n))
                return p.default_kind != gdscript::PropertyDefaultKind::NONE;
    return false;
}
Variant UnsafeGDScript::_get_property_default_value(const StringName &n) const {
    if (program && program->property_defaults.has(n))
        return Variant(program->property_defaults[n]).duplicate(true);
    if (program)
        for (auto &p : program->ir.properties)
            if (str(p.name) == String(n))
                return property_default(p);
    return {};
}
int32_t UnsafeGDScript::_get_member_line(const StringName &n) const {
    int i = method_index(n);
    if (i >= 0)
        return program->ir.signatures[i].line;
    i = property_index(n);
    return i >= 0 ? program->ir.globals[i].declaration_line : -1;
}
Dictionary UnsafeGDScript::_get_constants() const {
    return program ? unsafe_constants(program->ir) : Dictionary();
}
Dictionary unsafe_constants(const gdscript::IRProgram &ir) {
    Dictionary d;
    for (auto &c : ir.constants) {
        Variant v;
        using K = gdscript::ScriptConstant::Kind;
        switch (c.kind) {
        case K::INT:
            v = std::get<int64_t>(c.value);
            break;
        case K::FLOAT:
            v = std::get<double>(c.value);
            break;
        case K::BOOL:
            v = std::get<bool>(c.value);
            break;
        case K::STRING:
            v = str(std::get<std::string>(c.value));
            break;
        case K::ENUM: {
            Dictionary e;
            for (auto &m : c.members)
                e[str(m.name)] = m.value;
            v = e;
            break;
        }
        }
        d[str(c.name)] = v;
    }
    return d;
}
TypedArray<StringName> UnsafeGDScript::_get_members() const {
    TypedArray<StringName> a;
    for (auto &p : properties())
        a.push_back(p.name);
    return a;
}
Variant UnsafeGDScript::_get_rpc_config() const {
    Dictionary d;
    if (program)
        for (auto &r : program->ir.rpc_configs) {
            Dictionary c;
            c["rpc_mode"] = r.rpc_mode;
            c["transfer_mode"] = r.transfer_mode;
            c["call_local"] = r.call_local;
            c["channel"] = r.channel;
            d[str(r.name)] = c;
        }
    return d;
}
Error unsafe_compiler_options(const String &source, const String &source_path, gdscript::CompilerOptions &options,
                              String &error) {
    options.native_classes = true;
    options.debug_info = EngineDebugger::get_singleton()->is_active();
    options.optimize = false;
    options.batch_iteration = false;
    options.source_path = source_path.utf8().get_data();
    auto settings = ProjectSettings::get_singleton();
    auto settings_properties = settings->get_property_list();
    for (int i = 0; i < settings_properties.size(); ++i) {
        Dictionary p = settings_properties[i];
        String n = p["name"];
        if (n.begins_with("autoload/"))
            options.autoloads.push_back(n.substr(9).utf8().get_data());
    }
    auto classes = settings->get_global_class_list();
    for (int i = 0; i < classes.size(); ++i) {
        Dictionary c = classes[i];
        options.global_script_classes.emplace_back(String(c["class"]).utf8().get_data(),
                                                   String(c["path"]).utf8().get_data());
    }
    // Read inheritance headers with the lexer so comments, strings and nested
    // declarations cannot be mistaken for a top-level extends statement.
    String current = source, current_path = source_path;
    std::unordered_set<std::string> visited;
    if (!current_path.is_empty())
        visited.insert(current_path.utf8().get_data());
    for (int depth = 0; depth < 64; ++depth) {
        String path, base_name;
        try {
            gdscript::Lexer lexer(current.utf8().get_data());
            gdscript::Parser parser(lexer.tokenize());
            auto parsed = parser.parse();
            if (parsed.base_is_path)
                path = str(parsed.base_class);
            else {
                base_name = str(parsed.base_class);
                for (const auto &c : options.global_script_classes)
                    if (str(c.first) == base_name)
                        path = str(c.second);
            }
        } catch (const std::exception &e) {
            error = str(e.what());
            return ERR_PARSE_ERROR;
        }
        if (path.is_empty())
            break;
        if (depth == 63) {
            error = "Script inheritance is too deep";
            return ERR_PARSE_ERROR;
        }
        if (!path.begins_with("res://") && !path.begins_with("user://"))
            path = current_path.get_base_dir().path_join(path).simplify_path();
        if (!visited.insert(path.utf8().get_data()).second) {
            error = "Cyclic script inheritance";
            return ERR_PARSE_ERROR;
        }
        if (!FileAccess::file_exists(path)) {
            error = "Base script not found: " + path;
            return ERR_FILE_NOT_FOUND;
        }
        current = FileAccess::get_file_as_string(path);
        current_path = path;
        options.base_sources.push_back(
            {base_name.utf8().get_data(), path.utf8().get_data(), current.utf8().get_data(), false});
    }
    return OK;
}
Error UnsafeGDScript::_reload(bool keep) {
    if (!nested_name.is_empty())
        return ERR_UNAVAILABLE;
    if (!keep && !instances.empty()) {
        compile_error = "Cannot discard state while script instances exist";
        return ERR_ALREADY_IN_USE;
    }
    gdscript::CompilerOptions options;
    Ref<Script> candidate_base;
    Error preparation = unsafe_compiler_options(source, get_path(), options, compile_error);
    if (preparation != OK)
        return preparation;
    std::string error;
    auto candidate = NativeProgram::compile(source, options, error);
    if (!candidate) {
        compile_error = str(error);
        return ERR_PARSE_ERROR;
    }
    auto shared = std::make_shared<NativeState>(candidate);
    Variant result;
    if (!shared->invoke(-2, nullptr, 0, result)) {
        compile_error = str(shared->error);
        return ERR_COMPILATION_FAILED;
    }
    auto static_init = candidate->functions.find("_static_init");
    if (static_init != candidate->functions.end() && !shared->invoke(static_init->second, nullptr, 0, result)) {
        compile_error = str(shared->error);
        return ERR_COMPILATION_FAILED;
    }
    if (!options.base_sources.empty())
        candidate_base = ResourceLoader::get_singleton()->load(str(options.base_sources[0].path));
    // Snapshot old values before publishing new metadata (indices may change).
    std::vector<std::pair<UnsafeGDScriptInstance *, Dictionary>> snapshots;
    for (auto *instance : instances) {
        Dictionary saved;
        if (keep && program)
            for (const auto &g : program->ir.globals) {
                if (g.is_member()) {
                    Variant value;
                    if (instance->get(str(g.name), value))
                        saved[str(g.name)] = value;
                }
            }
        snapshots.emplace_back(instance, saved);
    }
    if (keep && program) {
        for (size_t i = 0; i < candidate->ir.globals.size(); ++i) {
            const auto &g = candidate->ir.globals[i];
            if (g.is_member())
                continue;
            int old_index = property_index(str(g.name));
            if (old_index >= 0 && !program->ir.globals[old_index].is_member())
                candidate->statics[i] = program->statics[old_index];
        }
    }
    const auto previous_program = program;
    const auto previous_static_state = static_state;
    const auto previous_base = base_script;
    std::vector<std::pair<UnsafeGDScriptInstance *, std::shared_ptr<NativeState>>> previous_states;
    for (const auto &snapshot : snapshots)
        previous_states.emplace_back(snapshot.first, snapshot.first->state);
    program = candidate;
    static_state = shared;
    base_script = candidate_base;
    compile_error = String();
    for (auto &snapshot : snapshots) {
        auto *instance = snapshot.first;
        instance->state = std::make_shared<NativeState>(program, instance->owner);
        if (!instance->placeholder && !instance->state->invoke(-3, nullptr, 0, result)) {
            compile_error = str(instance->state->error);
            program = previous_program;
            static_state = previous_static_state;
            base_script = previous_base;
            for (const auto &previous : previous_states)
                previous.first->state = previous.second;
            return ERR_COMPILATION_FAILED;
        }
        const auto keys = snapshot.second.keys();
        for (int i = 0; i < keys.size(); ++i)
            instance->set(keys[i], snapshot.second[keys[i]]);
    }
    if (previous_static_state) previous_static_state->cancel_coroutines();
    for (const auto &previous : previous_states) previous.second->cancel_coroutines();
    // Script resources themselves dispatch static methods, as GDScript does.
    gdextension_interface::object_set_script_instance(_owner,
        memnew(UnsafeGDScriptInstance(this, this, false, true))->create_native());
    return OK;
}
void *UnsafeGDScript::_instance_create(Object *owner) const {
    if (!program || !owner || !owner->is_class(_get_instance_base_type()))
        return nullptr;
    auto *instance = memnew(UnsafeGDScriptInstance(owner, const_cast<UnsafeGDScript *>(this)));
    void *native = instance->create_native();
    gdextension_interface::object_set_script_instance(owner->_owner, native);
    Variant result;
    GDExtensionCallError error{};
    if (!nested_name.is_empty())
        return native;
    if (!instance->state->invoke(-3, nullptr, 0, result)) {
        ERR_PRINT(str(instance->state->error));
        error.error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
    } else if (_has_method("_init")) {
        instance->callp("_init", pending_args, pending_count, result, error);
        if (error.error == GDEXTENSION_CALL_OK && !instance->state->error.empty())
            error.error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
    }
    if (pending_error)
        *pending_error = error;
    return native;
}
void *UnsafeGDScript::_placeholder_instance_create(Object *owner) const {
    return memnew(UnsafeGDScriptInstance(owner, const_cast<UnsafeGDScript *>(this), true))->create_native();
}
bool UnsafeGDScript::_instance_has(Object *owner) const {
    for (auto *i : instances)
        if (i->owner == owner)
            return true;
    return false;
}
Variant UnsafeGDScript::new_instance(const Variant **args, GDExtensionInt count, GDExtensionCallError &error) {
    error = {};
    if (!program) {
        error.error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
        return {};
    }
    if (!_has_method("_init") && count) {
        error.error = GDEXTENSION_CALL_ERROR_TOO_MANY_ARGUMENTS;
        return {};
    }
    Variant result = ClassDB::instantiate(_get_instance_base_type());
    Object *object = result;
    if (!object) {
        error.error = GDEXTENSION_CALL_ERROR_INSTANCE_IS_NULL;
        return {};
    }
    auto previous_args = pending_args;
    auto previous_count = pending_count;
    auto previous_error = pending_error;
    pending_args = args;
    pending_count = count;
    pending_error = &error;
    object->set_script(Ref<Script>(this));
    pending_args = previous_args;
    pending_count = previous_count;
    pending_error = previous_error;
    if (error.error != GDEXTENSION_CALL_OK) {
        if (!Object::cast_to<RefCounted>(object))
            object->call("free");
        return {};
    }
    return result;
}
UnsafeGDScript::UnsafeGDScript() {
    std::lock_guard<std::mutex> lock(scripts_mutex);
    scripts.insert(this);
}
UnsafeGDScript::~UnsafeGDScript() {
    {
        std::lock_guard<std::mutex> lock(scripts_mutex);
        scripts.erase(this);
    }
    gdextension_interface::object_set_script_instance(_owner, nullptr);
}
std::vector<Ref<UnsafeGDScript>> UnsafeGDScript::live_scripts() {
    std::lock_guard<std::mutex> lock(scripts_mutex);
    std::vector<Ref<UnsafeGDScript>> result;
    for (auto *script : scripts) {
        Ref<UnsafeGDScript> ref(script);
        if (ref.is_valid())
            result.push_back(ref);
    }
    return result;
}

UnsafeGDScriptInstance::UnsafeGDScriptInstance(Object *o, UnsafeGDScript *s, bool p, bool sd)
    : owner(o), resource(s), placeholder(p), static_dispatch(sd), nested_dispatch(!s->nested_name.is_empty()) {
    fields = s->pending_fields;
    if (sd)
        state = s->static_state;
    else {
        script = Ref<UnsafeGDScript>(s);
        state = s->program ? std::make_shared<NativeState>(s->program, o) : nullptr;
        s->instances.insert(this);
    }
}
UnsafeGDScriptInstance::~UnsafeGDScriptInstance() {
    if (state && !static_dispatch) state->cancel_coroutines(false);
    if (!static_dispatch)
        resource->instances.erase(this);
}
bool UnsafeGDScriptInstance::set(const StringName &n, const Variant &v) {
    if (!resource->nested_name.is_empty()) {
        if (String(n).begins_with("@")) return false;
        if (fields.has(n)) {
            fields[n] = v;
            return true;
        }
    }
    int i = resource->nested_name.is_empty() ? resource->property_index(n) : -1;
    if (i < 0) {
        if (!static_dispatch && has_method("_set")) {
            Variant name = n, result;
            const Variant *args[] = {&name, &v};
            return call_hook("_set", args, 2, result) && result.booleanize();
        }
        return false;
    }
    const auto &g = state->program->ir.globals[i];
    if (g.is_const || (static_dispatch && !g.is_static))
        return false;
    if (placeholder) {
        placeholder_values[n] = v;
        return true;
    }
    if (!g.setter_function.empty()) {
        const Variant *a[] = {&v};
        Variant r;
        GDExtensionCallError e{};
        return state->call(str(g.setter_function), a, 1, r, e);
    }
    auto &slot = g.is_member() ? state->members[i] : state->program->statics[i];
    int type = g.value_type;
    if (type >= 0 && type != v.get_type() && !(type == Variant::OBJECT && v.get_type() == Variant::NIL)) {
        if ((type == Variant::INT && v.get_type() == Variant::FLOAT) ||
            (type == Variant::FLOAT && v.get_type() == Variant::INT))
            slot = UtilityFunctions::type_convert(v, type);
        else
            return false;
    } else
        slot = v;
    return true;
}
bool UnsafeGDScriptInstance::get(const StringName &n, Variant &v) const {
    if (!resource->nested_name.is_empty()) {
        if (n == StringName("@base")) {
            v = owner;
            return true;
        }
        if (fields.has(n)) {
            v = fields[n];
            return true;
        }
    }
    auto constants = resource->_get_constants();
    if (constants.has(n)) {
        v = constants[n];
        return true;
    }
    int i = resource->nested_name.is_empty() ? resource->property_index(n) : -1;
    if (i >= 0) {
        const auto &g = state->program->ir.globals[i];
        if (static_dispatch && !g.is_static)
            return false;
        if (placeholder) {
            if (!placeholder_values.has(n))
                placeholder_values[n] = resource->_get_property_default_value(n);
            v = placeholder_values[n];
            return true;
        }
        if (!g.getter_function.empty()) {
            GDExtensionCallError e{};
            return state->call(str(g.getter_function), nullptr, 0, v, e);
        }
        v = g.is_member() ? state->members[i] : state->program->statics[i];
        return true;
    }
    if (resource->_has_script_signal(n) && !static_dispatch) {
        v = Signal(owner, n);
        return true;
    }
    if (has_method(n)) {
        v = Callable(owner, n);
        return true;
    }
    if (!static_dispatch && has_method("_get")) {
        Variant name = n;
        const Variant *args[] = {&name};
        return call_hook("_get", args, 1, v) && v.get_type() != Variant::NIL;
    }
    return false;
}
std::vector<PropertyInfo> UnsafeGDScriptInstance::properties() const {
    auto result = resource->properties();
    if (!static_dispatch && has_method("_get_property_list")) {
        Variant extra;
        if (call_hook("_get_property_list", nullptr, 0, extra) && extra.get_type() == Variant::ARRAY) {
            for (const Variant &entry : Array(extra)) {
                if (entry.get_type() != Variant::DICTIONARY) continue;
                Dictionary property = entry;
                const int type = property.get("type", Variant::NIL);
                if (!property.has("name") || type < 0 || type >= Variant::VARIANT_MAX) continue;
                auto info = type_info(type, property["name"], property.get("class_name", String()));
                info.hint = PropertyHint(int(property.get("hint", PROPERTY_HINT_NONE)));
                info.hint_string = property.get("hint_string", String());
                info.usage = property.get("usage", PROPERTY_USAGE_DEFAULT);
                result.push_back(info);
            }
        }
    }
    return result;
}
bool UnsafeGDScriptInstance::property_can_revert(const StringName &name) const {
    if (!static_dispatch && has_method("_property_can_revert")) {
        Variant argument = name, result;
        const Variant *args[] = {&argument};
        if (call_hook("_property_can_revert", args, 1, result) && result.booleanize()) return true;
    }
    return resource->_has_property_default_value(name);
}
bool UnsafeGDScriptInstance::property_get_revert(const StringName &name, Variant &result) const {
    if (!static_dispatch && has_method("_property_get_revert")) {
        Variant argument = name;
        const Variant *args[] = {&argument};
        if (call_hook("_property_get_revert", args, 1, result) && result.get_type() != Variant::NIL)
            return true;
    }
    result = resource->_get_property_default_value(name);
    return resource->_has_property_default_value(name);
}
bool UnsafeGDScriptInstance::call_hook(const StringName &name, const Variant **args, int count, Variant &result) const {
    GDExtensionCallError error{};
    const_cast<UnsafeGDScriptInstance *>(this)->callp(name, args, count, result, error);
    return error.error == GDEXTENSION_CALL_OK && state->error.empty();
}
const GDExtensionPropertyInfo *UnsafeGDScriptInstance::get_property_list(uint32_t *count) const {
    auto list_properties = properties();
    *count = list_properties.size();
    auto *list = memnew_arr(GDExtensionPropertyInfo, *count);
    for (uint32_t i = 0; i < *count; ++i)
        list[i] = native_property(list_properties[i]);
    return list;
}
void UnsafeGDScriptInstance::free_property_list(const GDExtensionPropertyInfo *list, uint32_t count) const {
    for (uint32_t i = 0; i < count; ++i)
        free_property(list[i]);
    if (list)
        memdelete_arr(list);
}
Variant::Type UnsafeGDScriptInstance::get_property_type(const StringName &n, bool *valid) const {
    for (auto &p : properties())
        if (p.name == n) {
            *valid = true;
            return p.type;
        }
    *valid = false;
    return Variant::NIL;
}
void UnsafeGDScriptInstance::get_property_state(GDExtensionScriptInstancePropertyStateAdd add, void *data) {
    for (auto &p : properties()) {
        Variant v;
        if ((p.usage & PROPERTY_USAGE_STORAGE) && get(p.name, v))
            add(&p.name, &v, data);
    }
}
const GDExtensionMethodInfo *UnsafeGDScriptInstance::get_method_list(uint32_t *count) const {
    auto methods = resource->methods();
    *count = methods.size();
    auto *list = memnew_arr(GDExtensionMethodInfo, *count);
    for (uint32_t i = 0; i < *count; ++i) {
        const auto &m = methods[i];
        auto &n = list[i];
        n = {};
        n.name = memnew(StringName(m.name));
        n.return_value = native_property(m.return_val);
        n.flags = m.flags;
        n.id = m.id;
        n.argument_count = m.arguments.size();
        n.arguments = memnew_arr(GDExtensionPropertyInfo, n.argument_count);
        for (uint32_t j = 0; j < n.argument_count; ++j)
            n.arguments[j] = native_property(m.arguments[j]);
        n.default_argument_count = m.default_arguments.size();
        // ScriptInstance's MethodInfo constructor reads contiguous Variants,
        // despite GDExtensionMethodInfo declaring this as a pointer array.
        auto *defaults = memnew_arr(Variant, n.default_argument_count);
        for (uint32_t j = 0; j < n.default_argument_count; ++j)
            defaults[j] = m.default_arguments[j];
        n.default_arguments = reinterpret_cast<GDExtensionVariantPtr *>(defaults);
    }
    return list;
}
void UnsafeGDScriptInstance::free_method_list(const GDExtensionMethodInfo *list, uint32_t count) const {
    for (uint32_t i = 0; i < count; ++i) {
        auto &m = list[i];
        memdelete(static_cast<StringName *>(m.name));
        free_property(m.return_value);
        for (uint32_t j = 0; j < m.argument_count; ++j)
            free_property(m.arguments[j]);
        if (m.arguments)
            memdelete_arr(m.arguments);
        if (m.default_arguments)
            memdelete_arr(reinterpret_cast<Variant *>(m.default_arguments));
    }
    if (list)
        memdelete_arr(list);
}
bool UnsafeGDScriptInstance::has_method(const StringName &n) const {
    return !placeholder && (static_dispatch ? resource->_has_static_method(n) : resource->_has_method(n));
}
GDExtensionInt UnsafeGDScriptInstance::get_method_argument_count(const StringName &n, bool &valid) const {
    valid = has_method(n);
    return valid ? int64_t(resource->_get_script_method_argument_count(n)) : 0;
}
void UnsafeGDScriptInstance::callp(const StringName &n, const Variant **a, int count, Variant &r,
                                   GDExtensionCallError &e) {
    e = {};
    // Ordinary entry resolves and validates the method in NativeState::call.
    // Nested classes still need their inherited public-to-internal name mapping.
    if (placeholder || (nested_dispatch && !has_method(n))) {
        e.error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
        return;
    }
    auto active = state;
    if (!nested_dispatch)
        active->call(n, a, count, r, e, static_dispatch);
    else {
        int index = resource->method_index(n);
        Variant self = owner;
        std::vector<const Variant *> args;
        if (!active->program->ir.signatures[index].is_static)
            args.push_back(&self);
        for (int i = 0; i < count; ++i)
            args.push_back(a[i]);
        active->call(str(active->program->ir.functions[index].name), args.data(), args.size(), r, e);
    }
    if (!active->error.empty()) {
        ERR_PRINT(str(active->error));
        // A runtime fault was already reported at its native frame. The method
        // exists: avoid a misleading "nonexistent function" break in its caller.
        e.error = GDEXTENSION_CALL_OK;
        r = Variant();
    }
}
void UnsafeGDScriptInstance::notification(int what, bool) {
    if (has_method("_notification")) {
        Variant v = what, r;
        const Variant *a[] = {&v};
        GDExtensionCallError e{};
        callp("_notification", a, 1, r, e);
    }
}
String UnsafeGDScriptInstance::to_string(bool *valid) {
    *valid = has_method("_to_string");
    if (!*valid)
        return {};
    Variant r;
    GDExtensionCallError e{};
    callp("_to_string", nullptr, 0, r, e);
    *valid = e.error == GDEXTENSION_CALL_OK && r.get_type() == Variant::STRING;
    return *valid ? String(r) : String();
}
Variant bind_native_class(GJContext *ctx, const String &name, const Variant &value) {
    if (!ctx->runtime || value.get_type() != Variant::DICTIONARY)
        throw std::runtime_error("Native class binding requires runtime and fields");
    auto *state = static_cast<NativeState *>(ctx->runtime);
    Dictionary fields = value;
    Variant object = fields["@base"];
    Object *owner = object;
    if (!owner)
        throw std::runtime_error("Native class has no owner");
    Ref<UnsafeGDScript> script;
    script.instantiate();
    script->nested_name = name;
    script->program = state->program;
    script->pending_fields = fields.duplicate();
    script->pending_fields.erase("@base");
    owner->set_script(script);
    script->pending_fields = Dictionary();
    return object;
}

} // namespace godot
