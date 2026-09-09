#include "unsafe_highlighter.h"
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_plugin.hpp>
#include <godot_cpp/classes/script_editor.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/editor_plugin_registration.hpp>

namespace godot {
class UnsafeGDScriptEditorPlugin : public EditorPlugin {
    GDCLASS(UnsafeGDScriptEditorPlugin, EditorPlugin)
    Ref<UnsafeGDScriptSyntaxHighlighter> highlighter;
  protected:
    static void _bind_methods() {}
  public:
    void _enter_tree() override {
        auto *editor = EditorInterface::get_singleton()->get_script_editor();
        if (editor) {
            highlighter.instantiate();
            editor->register_syntax_highlighter(highlighter);
        }
    }
    void _exit_tree() override {
        if (highlighter.is_valid()) {
            auto *editor = EditorInterface::get_singleton()->get_script_editor();
            if (editor) editor->unregister_syntax_highlighter(highlighter);
            highlighter.unref();
        }
    }
    String _get_plugin_name() const override { return "UnsafeGDScript"; }
};
void initialize_unsafe_editor() {
    ClassDB::register_class<UnsafeGDScriptSyntaxHighlighter>();
    ClassDB::register_internal_class<UnsafeGDScriptEditorPlugin>();
    EditorPlugins::add_by_type<UnsafeGDScriptEditorPlugin>();
}
void uninitialize_unsafe_editor() {
    EditorPlugins::remove_by_type<UnsafeGDScriptEditorPlugin>();
}
} // namespace godot
