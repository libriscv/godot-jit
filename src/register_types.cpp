#include "godot_jit.h"
#include "unsafe_gdscript.h"
#include <godot_cpp/godot.hpp>

namespace {
void initialize(godot::ModuleInitializationLevel level) {
    if (level == godot::MODULE_INITIALIZATION_LEVEL_SCENE) {
        godot::ClassDB::register_class<godot::GodotJIT>();
        godot::initialize_unsafe_language();
    }
    if (level == godot::MODULE_INITIALIZATION_LEVEL_EDITOR) godot::initialize_unsafe_editor();
}
void uninitialize(godot::ModuleInitializationLevel level) {
    if (level == godot::MODULE_INITIALIZATION_LEVEL_EDITOR) godot::uninitialize_unsafe_editor();
    if (level == godot::MODULE_INITIALIZATION_LEVEL_SCENE) godot::uninitialize_unsafe_language();
}
}

extern "C" GDExtensionBool GDE_EXPORT godot_jit_library_init(
    GDExtensionInterfaceGetProcAddress get_proc_address,
    GDExtensionClassLibraryPtr library, GDExtensionInitialization *initialization) {
    godot::GDExtensionBinding::InitObject init(get_proc_address, library, initialization);
    init.register_initializer(initialize);
    init.register_terminator(uninitialize);
    init.set_minimum_library_initialization_level(godot::MODULE_INITIALIZATION_LEVEL_SCENE);
    return init.init();
}
