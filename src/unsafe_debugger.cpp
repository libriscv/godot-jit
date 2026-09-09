#include "unsafe_debugger.h"
#include "unsafe_gdscript.h"
#include <godot_cpp/classes/engine_debugger.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot {
namespace {
struct Frame {
    GJContext *context;
    GJDebugFrame *view;
    int last_line = 0;
    int last_instruction = -1;
};
struct DebugState {
    std::vector<Frame> frames;
    String error;
};
DebugState &debug_state() {
    thread_local DebugState state;
    return state;
}
Frame *frame(int level) {
    auto &frames = debug_state().frames;
    return level >= 0 && size_t(level) < frames.size() ? &frames[frames.size() - 1 - level] : nullptr;
}
NativeState *runtime(const Frame &f) { return static_cast<NativeState *>(f.context->runtime); }
const gdscript::IRFunction *ir_function(const Frame &f) {
    const auto &ir = runtime(f)->program->ir;
    const size_t index = f.view->function;
    if (index < ir.functions.size()) return &ir.functions[index];
    if (index == ir.functions.size() && ir.has_global_init) return &ir.global_init;
    if (index == ir.functions.size() + 1 && ir.has_member_init) return &ir.member_init;
    return nullptr;
}
Object *owner(const Frame &f) {
    // Lifted nested methods pass their receiver as the first argument.
    if (f.view->self) {
        const auto &self = *reinterpret_cast<const Variant *>(f.view->self);
        if (self.get_type() == Variant::OBJECT) return self;
    }
    return ObjectDB::get_instance(runtime(f)->owner);
}
UnsafeGDScriptInstance *script_instance(const Frame &f) {
    Object *object = owner(f);
    return object ? static_cast<UnsafeGDScriptInstance *>(gdextension_interface::object_get_script_instance(
        object->_owner, unsafe_language()->_owner)) : nullptr;
}
Variant bounded(const Variant &value, int items, int depth) {
    if (value.get_type() != Variant::ARRAY && value.get_type() != Variant::DICTIONARY) return value;
    if (depth <= 0) return String("<max depth>");
    if (value.get_type() == Variant::ARRAY) {
        Array input = value, result;
        for (int i = 0; i < input.size() && i < items; ++i) result.push_back(bounded(input[i], items, depth - 1));
        return result;
    }
    Dictionary input = value, result;
    const Array keys = input.keys();
    for (int i = 0; i < keys.size() && i < items; ++i) result[keys[i]] = bounded(input[keys[i]], items, depth - 1);
    return result;
}
Dictionary values(int level, const String &kind) {
    Dictionary result;
    auto *f = frame(level);
    if (!f) return result;
    auto *state = runtime(*f);
    const auto *fn = ir_function(*f);
    if (kind == "locals") {
        if (fn) for (const auto &local : fn->debug_locals) {
            if (local.name.empty() || local.name[0] == '@' || local.register_num < 0 ||
                local.register_num >= f->view->local_count ||
                size_t(f->view->instruction) < local.begin_instruction ||
                size_t(f->view->instruction) >= local.end_instruction) continue;
            result[String::utf8(local.name.c_str())] =
                *reinterpret_cast<const Variant *>(f->view->locals[local.register_num]);
        }
    } else {
        const bool members = kind == "members";
        if (members && !owner(*f)) return result;
        if (members) {
            auto *instance = script_instance(*f);
            if (instance && !instance->resource->nested_name.is_empty()) {
                for (const Variant &key : instance->fields.keys())
                    if (!String(key).begins_with("@")) result[key] = instance->fields[key];
                return result;
            }
        }
        for (size_t i = 0; i < state->program->ir.globals.size(); ++i) {
            const auto &global = state->program->ir.globals[i];
            if (global.is_member() == members)
                result[String::utf8(global.name.c_str())] = members ? state->members[i] : state->program->statics[i];
        }
        if (!members) {
            result.merge(unsafe_constants(state->program->ir), false);
        }
    }
    return result;
}
} // namespace

void UnsafeDebugger::hook(GJContext *context, GJDebugFrame *view, int event) {
    auto &state = debug_state();
    auto *debugger = EngineDebugger::get_singleton();
    const bool active = debugger && debugger->is_active();
    if (event == GJ_DEBUG_ENTER) {
        state.frames.push_back({context, view});
        if (active && debugger->get_lines_left() > 0 && debugger->get_depth() >= 0)
            debugger->set_depth(debugger->get_depth() + 1);
        return;
    }
    if (event == GJ_DEBUG_EXIT) {
        if (active && debugger->get_lines_left() > 0 && debugger->get_depth() >= 0)
            debugger->set_depth(debugger->get_depth() - 1);
        if (!state.frames.empty())
            state.frames.pop_back();
        return;
    }
    if (!active || state.frames.empty()) return;
    if (event == GJ_DEBUG_ERROR) {
        state.error = context->error ? String::utf8(static_cast<std::string *>(context->error)->c_str()) : String("Runtime error");
        debugger->script_debug(unsafe_language(), false, true);
        state.error = String();
        return;
    }
    auto &current = state.frames.back();
    const bool new_line = view->line != current.last_line || view->instruction <= current.last_instruction;
    current.last_line = view->line;
    current.last_instruction = view->instruction;
    if (!new_line && event != GJ_DEBUG_BREAKPOINT) return;
    // Service debugger messages even while a native loop is running.
    debugger->line_poll();
    bool stop = !debugger->is_skipping_breakpoints() &&
        (event == GJ_DEBUG_BREAKPOINT || debugger->is_breakpoint(view->line, StringName(source(0))));
    if (debugger->get_lines_left() > 0 && debugger->get_depth() <= 0) {
        debugger->set_lines_left(debugger->get_lines_left() - 1);
        stop = stop || debugger->get_lines_left() == 0;
    }
    if (stop) {
        state.error = event == GJ_DEBUG_BREAKPOINT ? String("Breakpoint statement") : String("Breakpoint");
        debugger->script_debug(unsafe_language(), true, false);
        state.error = String();
    }
}
String UnsafeDebugger::error() { return debug_state().error; }
int UnsafeDebugger::count() { return debug_state().frames.size(); }
int UnsafeDebugger::line(int level) { auto *f = frame(level); return f ? f->view->line : 0; }
String UnsafeDebugger::function(int level) {
    auto *f = frame(level);
    const auto *fn = f ? ir_function(*f) : nullptr;
    return fn ? String::utf8(fn->name.c_str()) : String();
}
String UnsafeDebugger::source(int level) {
    auto *f = frame(level);
    if (!f) return {};
    const auto *fn = ir_function(*f);
    return fn && !fn->source_path.empty() ? String::utf8(fn->source_path.c_str()) : runtime(*f)->program->source_path;
}
Dictionary UnsafeDebugger::variables(int level, const char *kind, int max_items, int max_depth) {
    const Dictionary raw = values(level, kind);
    PackedStringArray names;
    Array contents;
    for (const Variant &key : raw.keys()) {
        names.push_back(key);
        contents.push_back(bounded(raw[key], max_items < 0 ? 1024 : max_items, max_depth < 0 ? 16 : max_depth));
    }
    Dictionary result;
    result[kind] = names;
    result["values"] = contents;
    return result;
}
void *UnsafeDebugger::instance(int level) {
    auto *f = frame(level);
    auto *instance = f ? script_instance(*f) : nullptr;
    return instance ? instance->native_instance : nullptr;
}
String UnsafeDebugger::expression(int level, const String &expression, int max_items, int max_depth) {
    if (!frame(level)) return "Invalid stack level";
    auto parts = expression.strip_edges().split(".");
    Variant value;
    bool found = false;
    if (parts[0] == "self") { value = owner(*frame(level)); found = true; }
    else for (const char *kind : {"locals", "members", "globals"}) {
        auto scope = values(level, kind);
        if (scope.has(parts[0])) { value = scope[parts[0]]; found = true; break; }
    }
    if (!found) return "Unknown identifier or unsupported expression";
    for (int i = 1; i < parts.size(); ++i) {
        bool valid = false;
        value = value.get_named(StringName(parts[i]), valid);
        if (!valid) return "Unknown member";
    }
    return UtilityFunctions::var_to_str(bounded(value, max_items < 0 ? 1024 : max_items, max_depth < 0 ? 16 : max_depth));
}
TypedArray<Dictionary> UnsafeDebugger::stack() {
    TypedArray<Dictionary> result;
    for (int i = 0; i < count(); ++i) {
        Dictionary item;
        item["file"] = source(i);
        item["func"] = function(i);
        item["line"] = line(i);
        result.push_back(item);
    }
    return result;
}
} // namespace godot
