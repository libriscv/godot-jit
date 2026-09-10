#pragma once
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/signal.hpp>
#include <memory>
#include <vector>

namespace godot {
struct NativeState;
class UnsafeFunctionState : public RefCounted {
    GDCLASS(UnsafeFunctionState, RefCounted);
    friend struct NativeState;
    std::weak_ptr<NativeState> instance;
    std::vector<Variant> frame;
    int function = 0, instruction = 0, destination = 0;
    Signal awaited;
    bool running = false;
    bool failed = false, cancelled = false;
    String failure_message;
    void disconnect();
protected:
    static void _bind_methods();
public:
    bool is_valid() const { return !instance.expired(); }
    bool has_failed() const { return failed; }
    bool was_cancelled() const { return cancelled; }
    String get_failure_message() const { return failure_message; }
    void cancel();
    Variant resume_from_signal(const Variant **args, GDExtensionInt count, GDExtensionCallError &error);
};
}
