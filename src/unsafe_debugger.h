#pragma once
#include "native_program.h"
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/typed_array.hpp>

namespace godot {
// Godot blocks in script_debug(); stack views remain live until it returns.
class UnsafeDebugger {
  public:
    static void hook(GJContext *, GJDebugFrame *, int);
    static String error();
    static int count();
    static int line(int);
    static String function(int);
    static String source(int);
    static Dictionary variables(int level, const char *kind, int max_items, int max_depth);
    static void *instance(int);
    static String expression(int, const String &, int, int);
    static TypedArray<Dictionary> stack();
};
} // namespace godot
