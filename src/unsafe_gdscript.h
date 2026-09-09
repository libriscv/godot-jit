#pragma once
#include "native_program.h"
#include "script_instance.h"
#include <godot_cpp/classes/script_extension.hpp>
#include <godot_cpp/classes/script_language_extension.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <unordered_set>

namespace godot {
class UnsafeGDScriptInstance;
class UnsafeGDScript : public ScriptExtension {
    GDCLASS(UnsafeGDScript, ScriptExtension)
  protected:
    static void _bind_methods();
    bool _set(const StringName &, const Variant &);
    bool _get(const StringName &, Variant &) const;
    void _get_property_list(List<PropertyInfo> *) const;

  public:
    String source, compile_error;
    String nested_name;
    Dictionary pending_fields;
    std::shared_ptr<NativeProgram> program;
    std::shared_ptr<NativeState> static_state;
    mutable std::unordered_set<UnsafeGDScriptInstance *> instances;
    Ref<Script> base_script;
    const Variant **pending_args = nullptr;
    int pending_count = 0;
    GDExtensionCallError *pending_error = nullptr;
    bool _editor_can_reload_from_file() override { return true; }
    void _placeholder_erased(void *) override {}
    bool _can_instantiate() const override;
    Ref<Script> _get_base_script() const override { return base_script; }
    StringName _get_global_name() const override;
    bool _inherits_script(const Ref<Script> &) const override;
    StringName _get_instance_base_type() const override;
    void *_instance_create(Object *) const override;
    void *_placeholder_instance_create(Object *) const override;
    bool _instance_has(Object *) const override;
    bool _has_source_code() const override { return !source.is_empty(); }
    String _get_source_code() const override { return source; }
    void _set_source_code(const String &s) override { source = s; }
    Error _reload(bool) override;
    StringName _get_doc_class_name() const override { return _get_global_name(); }
    TypedArray<Dictionary> _get_documentation() const override { return {}; }
    String _get_class_icon_path() const override { return {}; }
    bool _has_method(const StringName &) const override;
    bool _has_static_method(const StringName &) const override;
    Variant _get_script_method_argument_count(const StringName &) const override;
    Dictionary _get_method_info(const StringName &) const override;
    bool _is_tool() const override { return program && program->ir.is_tool; }
    bool _is_valid() const override { return bool(program); }
    bool _is_abstract() const override { return false; }
    ScriptLanguage *_get_language() const override;
    bool _has_script_signal(const StringName &) const override;
    TypedArray<Dictionary> _get_script_signal_list() const override;
    bool _has_property_default_value(const StringName &) const override;
    Variant _get_property_default_value(const StringName &) const override;
    void _update_exports() override {}
    TypedArray<Dictionary> _get_script_method_list() const override;
    TypedArray<Dictionary> _get_script_property_list() const override;
    int32_t _get_member_line(const StringName &) const override;
    Dictionary _get_constants() const override;
    TypedArray<StringName> _get_members() const override;
    bool _is_placeholder_fallback_enabled() const override { return true; }
    Variant _get_rpc_config() const override;
    Variant new_instance(const Variant **, GDExtensionInt, GDExtensionCallError &);
    String get_compile_error() const { return compile_error; }
    String get_generated_c() const { return program ? String::utf8(program->generated.c_str()) : String(); }
    int method_index(const StringName &) const;
    int property_index(const StringName &) const;
    std::vector<MethodInfo> methods() const;
    std::vector<PropertyInfo> properties() const;
    UnsafeGDScript();
    ~UnsafeGDScript() override;
    static std::vector<Ref<UnsafeGDScript>> live_scripts();
};

class UnsafeGDScriptInstance : public ScriptInstanceExtension {
  public:
    Object *owner;
    Ref<UnsafeGDScript> script;
    UnsafeGDScript *resource;
    std::shared_ptr<NativeState> state;
    bool placeholder = false;
    bool static_dispatch = false;
    Dictionary placeholder_values;
    Dictionary fields;
    void *native_instance = nullptr;
    void *create_native() {
        native_instance = ScriptInstanceExtension::create_native_instance(this);
        return native_instance;
    }
    UnsafeGDScriptInstance(Object *, UnsafeGDScript *, bool = false, bool = false);
    ~UnsafeGDScriptInstance() override;
    bool set(const StringName &, const Variant &) override;
    bool get(const StringName &, Variant &) const override;
    const GDExtensionPropertyInfo *get_property_list(uint32_t *) const override;
    void free_property_list(const GDExtensionPropertyInfo *, uint32_t) const override;
    Variant::Type get_property_type(const StringName &, bool *) const override;
    bool validate_property(GDExtensionPropertyInfo &) const override { return false; }
    bool property_can_revert(const StringName &) const override;
    bool property_get_revert(const StringName &, Variant &) const override;
    Object *get_owner() override { return owner; }
    void get_property_state(GDExtensionScriptInstancePropertyStateAdd, void *) override;
    const GDExtensionMethodInfo *get_method_list(uint32_t *) const override;
    void free_method_list(const GDExtensionMethodInfo *, uint32_t) const override;
    bool has_method(const StringName &) const override;
    GDExtensionInt get_method_argument_count(const StringName &, bool &) const override;
    void callp(const StringName &, const Variant **, int, Variant &, GDExtensionCallError &) override;
    void notification(int, bool) override;
    String to_string(bool *) override;
    void refcount_incremented() override {}
    bool refcount_decremented() override { return true; }
    Ref<Script> get_script() const override { return Ref<Script>(resource); }
    bool is_placeholder() const override { return placeholder; }
    void property_set_fallback(const StringName &n, const Variant &v, bool *ok) override { *ok = set(n, v); }
    Variant property_get_fallback(const StringName &n, bool *ok) override {
        Variant v;
        *ok = get(n, v);
        return v;
    }
    ScriptLanguage *_get_language() override { return resource->_get_language(); }
    std::vector<PropertyInfo> properties() const;
    bool call_hook(const StringName &, const Variant **, int, Variant &) const;
};
Error unsafe_compiler_options(const String &, const String &, gdscript::CompilerOptions &, String &);
Dictionary unsafe_constants(const gdscript::IRProgram &);
ScriptLanguageExtension *unsafe_language();
void initialize_unsafe_language();
void uninitialize_unsafe_language();
void initialize_unsafe_editor();
void uninitialize_unsafe_editor();
Variant bind_native_class(GJContext *, const String &, const Variant &);
} // namespace godot
