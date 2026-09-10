#include "function_state.h"
#include "native_program.h"
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <algorithm>
#include <stdexcept>

namespace godot {
void UnsafeFunctionState::_bind_methods() {
    ClassDB::bind_method(D_METHOD("is_valid"), &UnsafeFunctionState::is_valid);
    ClassDB::bind_method(D_METHOD("has_failed"), &UnsafeFunctionState::has_failed);
    ClassDB::bind_method(D_METHOD("was_cancelled"), &UnsafeFunctionState::was_cancelled);
    ClassDB::bind_method(D_METHOD("get_failure_message"), &UnsafeFunctionState::get_failure_message);
    ClassDB::bind_method(D_METHOD("cancel"), &UnsafeFunctionState::cancel);
    ClassDB::bind_vararg_method(METHOD_FLAGS_DEFAULT, "resume_from_signal",
        &UnsafeFunctionState::resume_from_signal, MethodInfo("resume_from_signal"));
    ADD_SIGNAL(MethodInfo("completed", PropertyInfo(Variant::NIL, "result",
        PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NIL_IS_VARIANT)));
}
void UnsafeFunctionState::disconnect() {
    Object *target = awaited.get_object();
    Callable callback(this, "resume_from_signal");
    if (target && target->is_connected(awaited.get_name(), callback))
        target->disconnect(awaited.get_name(), callback);
    awaited = Signal();
}
void UnsafeFunctionState::cancel() {
    Ref<UnsafeFunctionState> keep_alive(this);
    if (auto state = instance.lock()) {
        cancelled = true;
        state->retire(keep_alive);
        emit_signal("completed", Variant());
    }
}
Variant UnsafeFunctionState::resume_from_signal(const Variant **args, GDExtensionInt count,
                                               GDExtensionCallError &error) {
    error = {};
    Ref<UnsafeFunctionState> keep_alive(this);
    auto state = instance.lock();
    if (!state || running) return {};
    try {
        Variant sent;
        if (count == 1) sent = *args[0];
        else if (count > 1) {
            Array values;
            for (int i = 0; i < count; ++i) values.push_back(*args[i]);
            sent = values;
        }
        state->resume(keep_alive, sent);
    } catch (const std::exception &e) {
        failed = true;
        failure_message = String::utf8(e.what());
        cancel();
        error.error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
    }
    return {};
}
NativeState::~NativeState() { cancel_coroutines(false); }
void NativeState::retire(const Ref<UnsafeFunctionState> &co) {
    co->disconnect();
    co->instance.reset();
    co->frame.clear();
    coroutines.erase(std::remove(coroutines.begin(), coroutines.end(), co), coroutines.end());
}
void NativeState::cancel_coroutines(bool notify) {
    const bool previous = cancelling;
    cancelling = true;
    auto dying = std::move(coroutines);
    coroutines.clear();
    // Detach every callback before a completion handler can reenter the runtime.
    for (const auto &co : dying) {
        co->disconnect();
        co->instance.reset();
        co->cancelled = true;
        co->frame.clear();
    }
    if (notify) for (const auto &co : dying) co->emit_signal("completed", Variant());
    cancelling = previous;
}
int NativeState::suspend(GJContext *, UnsafeFunctionState *resuming, int function, int instruction,
                        Variant &result, const Variant &operand, GJVariant *const *slots,
                        int count, int destination) {
    if (count <= 0 || destination < 0 || destination >= count)
        throw std::runtime_error("await: invalid coroutine frame");
    if (operand.get_type() != Variant::SIGNAL) {
        *reinterpret_cast<Variant *>(slots[destination]) = operand;
        return 0;
    }
    if (cancelling || (resuming && resuming->instance.lock().get() != this))
        throw std::runtime_error("await: coroutine was cancelled");
    Signal signal = operand;
    Object *target = signal.get_object();
    if (!target || !target->has_signal(signal.get_name()))
        throw std::runtime_error("await: the awaited Signal does not exist");
    Ref<UnsafeFunctionState> co(resuming);
    if (co.is_null()) {
        co.instantiate();
        co->instance = shared_from_this();
        coroutines.push_back(co);
    }
    try {
        co->disconnect();
        std::vector<Variant> saved;
        saved.reserve(count);
        for (int i = 0; i < count; ++i) saved.push_back(*reinterpret_cast<Variant *>(slots[i]));
        co->frame = std::move(saved);
        co->function = function;
        co->instruction = instruction;
        co->destination = destination;
        co->awaited = signal;
        if (target->connect(signal.get_name(), Callable(co.ptr(), "resume_from_signal"), Object::CONNECT_ONE_SHOT) != OK)
            throw std::runtime_error("await: could not connect to the awaited Signal");
        result = Signal(co.ptr(), "completed");
        return 1;
    } catch (...) {
        if (!resuming) retire(co);
        throw;
    }
}
int NativeState::restore(UnsafeFunctionState *co, GJVariant *const *slots, int count) {
    if (!co || co->instance.lock().get() != this || count != int(co->frame.size()))
        throw std::runtime_error("await: invalid coroutine restore");
    for (int i = 0; i < count; ++i)
        *reinterpret_cast<Variant *>(slots[i]) = co->frame[i];
    return co->instruction;
}
void NativeState::resume(const Ref<UnsafeFunctionState> &co, const Variant &sent) {
    Variant self = owner.is_valid() ? Variant(ObjectDB::get_instance(owner)) : Variant();
    if (owner.is_valid() && !ObjectDB::get_instance(owner)) {
        co->cancel();
        return;
    }
    if (auto *child = Object::cast_to<UnsafeFunctionState>(co->awaited.get_object())) {
        co->failed = co->failed || child->has_failed();
        if (child->has_failed()) co->failure_message = child->get_failure_message();
    }
    co->disconnect();
    co->running = true;
    co->frame.at(co->destination) = sent;
    Variant result;
    const bool ok = invoke(co->function, nullptr, -1, result, co.ptr());
    co->running = false;
    if (!ok) {
        co->failed = true;
        co->failure_message = String::utf8(error.c_str());
        ERR_PRINT(co->failure_message);
    }
    if (co->instance.lock().get() != this) return;
    if (ok && result.get_type() == Variant::SIGNAL && Signal(result) == Signal(co.ptr(), "completed")) return;
    retire(co);
    co->emit_signal("completed", ok ? result : Variant());
}
}

extern "C" int gj_await(GJContext *ctx, void *resuming, int function, int instruction,
                         GJVariant *result, const GJVariant *operand, GJVariant *const *slots,
                         int count, int destination) {
    try {
        if (!ctx->runtime) throw std::runtime_error("await requires a native runtime");
        return static_cast<godot::NativeState *>(ctx->runtime)->suspend(ctx,
            static_cast<godot::UnsafeFunctionState *>(resuming), function, instruction,
            *reinterpret_cast<godot::Variant *>(result), *reinterpret_cast<const godot::Variant *>(operand),
            slots, count, destination);
    } catch (const std::exception &e) { gj_fail(ctx, e.what()); return -1; }
    catch (...) { gj_fail(ctx, "await failed"); return -1; }
}
extern "C" int gj_await_restore(GJContext *ctx, void *resuming, GJVariant *const *slots, int count) {
    try {
        if (!ctx->runtime) throw std::runtime_error("await requires a native runtime");
        return static_cast<godot::NativeState *>(ctx->runtime)->restore(
            static_cast<godot::UnsafeFunctionState *>(resuming), slots, count);
    } catch (const std::exception &e) { gj_fail(ctx, e.what()); return -1; }
    catch (...) { gj_fail(ctx, "await restore failed"); return -1; }
}
