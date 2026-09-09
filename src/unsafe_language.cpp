#include "unsafe_gdscript.h"
#include <c_codegen.h>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource_format_loader.hpp>
#include <godot_cpp/classes/resource_format_saver.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/core/class_db.hpp>

namespace godot {
template <typename... Args> PackedStringArray strings(Args... args) {
    return PackedStringArray(Array::make(args...));
}
class UnsafeGDScriptLanguage : public ScriptLanguageExtension {
    GDCLASS(UnsafeGDScriptLanguage, ScriptLanguageExtension)
  protected:
    static void _bind_methods() {}

  public:
    String _get_name() const override { return "UnsafeGDScript"; }
    String _get_type() const override { return "UnsafeGDScript"; }
    String _get_extension() const override { return "ugd"; }
    void _init() override {}
    void _finish() override {}
    PackedStringArray _get_reserved_words() const override {
        return String("and as assert break breakpoint class class_name const continue elif else enum extends false for "
                      "func if in is match not null or pass preload return self setget signal static super true var "
                      "void while")
            .split(" ");
    }
    bool _is_control_flow_keyword(const String &s) const override {
        return String("break continue elif else for if match return while").split(" ").has(s);
    }
    PackedStringArray _get_comment_delimiters() const override { return strings("#"); }
    PackedStringArray _get_doc_comment_delimiters() const override { return strings("##"); }
    PackedStringArray _get_string_delimiters() const override { return strings("\" \"", "' '", "\"\"\" \"\"\""); }
    Ref<Script> _make_template(const String &, const String &, const String &base) const override {
        Ref<UnsafeGDScript> s;
        s.instantiate();
        s->_set_source_code("extends " + base + "\n\nfunc _ready():\n\tpass\n");
        return s;
    }
    TypedArray<Dictionary> _get_built_in_templates(const StringName &) const override { return {}; }
    bool _is_using_templates() override { return false; }
    Dictionary _validate(const String &source, const String &path, bool functions, bool errors, bool,
                         bool) const override {
        gdscript::Compiler c;
        gdscript::CompilerOptions o;
        String error;
        std::optional<gdscript::IRProgram> ir;
        if (unsafe_compiler_options(source, path, o, error) == OK) {
            ir = c.compile_to_ir(source.utf8().get_data(), o);
            if (!ir)
                error = String::utf8(c.get_error().c_str());
            else
                try {
                    gdscript::CCodeGenerator().generate(*ir);
                } catch (const std::exception &e) {
                    error = String::utf8(e.what());
                    ir.reset();
                }
        }
        Dictionary d;
        d["valid"] = bool(ir);
        TypedArray<Dictionary> e;
        PackedStringArray f;
        if (!ir && errors) {
            const auto &info = c.get_error_info();
            Dictionary v;
            v["line"] = info.line;
            v["column"] = info.column;
            v["message"] = error;
            e.push_back(v);
        }
        if (ir && functions)
            for (auto &s : ir->signatures)
                if (!s.name.empty() && s.name[0] != '@')
                    f.push_back(String::utf8(s.name.c_str()) + ":" + String::num_int64(s.line));
        d["errors"] = e;
        d["functions"] = f;
        d["warnings"] = TypedArray<Dictionary>();
        d["safe_lines"] = PackedInt32Array();
        return d;
    }
    String _validate_path(const String &) const override { return {}; }
    Object *_create_script() const override { return memnew(UnsafeGDScript); }
    bool _has_named_classes() const override { return true; }
    bool _supports_builtin_mode() const override { return true; }
    bool _supports_documentation() const override { return false; }
    bool _can_inherit_from_file() const override { return true; }
    bool _can_make_function() const override { return true; }
    int32_t _find_function(const String &name, const String &code) const override {
        auto lines = code.split("\n");
        for (int i = 0; i < lines.size(); ++i)
            if (lines[i].strip_edges().begins_with("func " + name + "("))
                return i + 1;
        return -1;
    }
    String _make_function(const String &, const String &name, const PackedStringArray &args) const override {
        return "\nfunc " + name + "(" + String(", ").join(args) + "):\n\tpass\n";
    }
    Error _open_in_external_editor(const Ref<Script> &, int32_t, int32_t) override { return ERR_UNAVAILABLE; }
    bool _overrides_external_editor() override { return false; }
    Dictionary _complete_code(const String &, const String &, Object *) const override {
        Dictionary d;
        d["result"] = ERR_UNAVAILABLE;
        return d;
    }
    Dictionary _lookup_code(const String &, const String &, const String &, Object *) const override {
        Dictionary d;
        d["result"] = ERR_UNAVAILABLE;
        return d;
    }
    String _auto_indent_code(const String &code, int32_t, int32_t) const override { return code; }
    void _add_global_constant(const StringName &, const Variant &) override {}
    void _add_named_global_constant(const StringName &, const Variant &) override {}
    void _remove_named_global_constant(const StringName &) override {}
    void _thread_enter() override {}
    void _thread_exit() override {}
    String _debug_get_error() const override { return {}; }
    int32_t _debug_get_stack_level_count() const override { return 0; }
    int32_t _debug_get_stack_level_line(int32_t) const override { return 0; }
    String _debug_get_stack_level_function(int32_t) const override { return {}; }
    String _debug_get_stack_level_source(int32_t) const override { return {}; }
    Dictionary _debug_get_stack_level_locals(int32_t, int32_t, int32_t) override { return {}; }
    Dictionary _debug_get_stack_level_members(int32_t, int32_t, int32_t) override { return {}; }
    void *_debug_get_stack_level_instance(int32_t) override { return nullptr; }
    Dictionary _debug_get_globals(int32_t, int32_t) override { return {}; }
    String _debug_parse_stack_level_expression(int32_t, const String &, int32_t, int32_t) override { return {}; }
    TypedArray<Dictionary> _debug_get_current_stack_info() override { return {}; }
    void _reload_all_scripts() override {
        // Hold resources while reloading: releasing a dependency can remove it.
        auto scripts = UnsafeGDScript::live_scripts();
        for (const auto &s : scripts) {
            if (!s->nested_name.is_empty())
                continue;
            if (FileAccess::file_exists(s->get_path()))
                s->_set_source_code(FileAccess::get_file_as_string(s->get_path()));
            s->_reload(true);
        }
    }
    void _reload_scripts(const Array &scripts, bool soft) override {
        for (int i = 0; i < scripts.size(); ++i) {
            Ref<Script> s = scripts[i];
            if (s.is_valid())
                s->reload(soft);
        }
    }
    void _reload_tool_script(const Ref<Script> &s, bool soft) override {
        if (s.is_valid())
            s->reload(soft);
    }
    PackedStringArray _get_recognized_extensions() const override { return strings("ugd", "unsafegd"); }
    TypedArray<Dictionary> _get_public_functions() const override { return {}; }
    Dictionary _get_public_constants() const override { return {}; }
    TypedArray<Dictionary> _get_public_annotations() const override { return {}; }
    void _profiling_start() override {}
    void _profiling_stop() override {}
    void _profiling_set_save_native_calls(bool) override {}
    void _frame() override {}
    bool _handles_global_class_type(const String &type) const override { return type == "UnsafeGDScript"; }
    Dictionary _get_global_class_name(const String &path) const override {
        Dictionary d;
        String code = FileAccess::get_file_as_string(path);
        String name, base = "RefCounted";
        for (auto line : code.split("\n")) {
            line = line.strip_edges();
            if (line.begins_with("class_name "))
                name = line.substr(11).get_slice(" ", 0);
            if (line.begins_with("extends "))
                base = line.substr(8).strip_edges();
        }
        if (!name.is_empty()) {
            d["name"] = name;
            d["base_type"] = base;
            d["icon_path"] = "";
            d["is_abstract"] = false;
            d["is_tool"] = code.contains("@tool");
        }
        return d;
    }
};
class UnsafeGDScriptLoader : public ResourceFormatLoader {
    GDCLASS(UnsafeGDScriptLoader, ResourceFormatLoader)
  protected:
    static void _bind_methods() {}

  public:
    PackedStringArray _get_recognized_extensions() const override { return strings("ugd", "unsafegd"); }
    bool _handles_type(const StringName &t) const override {
        return t == StringName("Script") || t == StringName("UnsafeGDScript");
    }
    String _get_resource_type(const String &path) const override {
        return _get_recognized_extensions().has(path.get_extension().to_lower()) ? "UnsafeGDScript" : "";
    }
    Variant _load(const String &path, const String &, bool, int32_t) const override {
        auto file = FileAccess::open(path, FileAccess::READ);
        if (file.is_null())
            return ERR_FILE_CANT_OPEN;
        Ref<UnsafeGDScript> s;
        s.instantiate();
        s->set_path(path);
        s->_set_source_code(file->get_as_text());
        if (s->_reload(false) != OK) {
            ERR_PRINT(s->get_compile_error());
            return ERR_PARSE_ERROR;
        }
        return s;
    }
};
class UnsafeGDScriptSaver : public ResourceFormatSaver {
    GDCLASS(UnsafeGDScriptSaver, ResourceFormatSaver)
  protected:
    static void _bind_methods() {}

  public:
    bool _recognize(const Ref<Resource> &r) const override { return Object::cast_to<UnsafeGDScript>(r.ptr()); }
    PackedStringArray _get_recognized_extensions(const Ref<Resource> &r) const override {
        return _recognize(r) ? strings("ugd", "unsafegd") : PackedStringArray();
    }
    bool _recognize_path(const Ref<Resource> &r, const String &p) const override {
        return _get_recognized_extensions(r).has(p.get_extension().to_lower());
    }
    Error _save(const Ref<Resource> &r, const String &path, uint32_t) override {
        auto *s = Object::cast_to<UnsafeGDScript>(r.ptr());
        if (!s)
            return ERR_INVALID_PARAMETER;
        auto f = FileAccess::open(path, FileAccess::WRITE);
        if (f.is_null())
            return ERR_FILE_CANT_WRITE;
        f->store_string(s->source);
        return f->get_error();
    }
};
static UnsafeGDScriptLanguage *language = nullptr;
static Ref<UnsafeGDScriptLoader> loader;
static Ref<UnsafeGDScriptSaver> saver;
ScriptLanguageExtension *unsafe_language() {
    return language;
}
void initialize_unsafe_language() {
    ClassDB::register_class<UnsafeGDScript>();
    ClassDB::register_class<UnsafeGDScriptLanguage>();
    ClassDB::register_class<UnsafeGDScriptLoader>();
    ClassDB::register_class<UnsafeGDScriptSaver>();
    language = memnew(UnsafeGDScriptLanguage);
    Engine::get_singleton()->register_script_language(language);
    loader.instantiate();
    saver.instantiate();
    ResourceLoader::get_singleton()->add_resource_format_loader(loader, true);
    ResourceSaver::get_singleton()->add_resource_format_saver(saver, true);
}
void uninitialize_unsafe_language() {
    ResourceSaver::get_singleton()->remove_resource_format_saver(saver);
    saver.unref();
    ResourceLoader::get_singleton()->remove_resource_format_loader(loader);
    loader.unref();
    Engine::get_singleton()->unregister_script_language(language);
    memdelete(language);
    language = nullptr;
}
} // namespace godot
