#pragma once
#include "native_program.h"
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_jit/c_module.h>

namespace godot {

class GodotJIT : public RefCounted {
    GDCLASS(GodotJIT, RefCounted)

  protected:
    static void _bind_methods();

  public:
    bool compile_c(const String &source);
    bool compile_sgd(const String &source, Object *receiver = nullptr);
    Variant execute_function(const String &name, const Array &arguments = Array());
    String get_generated_c() const;
    int64_t execute(int64_t argument);
    bool is_compiled() const { return module_ != nullptr || native_ != nullptr; }
    String get_error() const { return String::utf8(error_.c_str()); }
    void clear();
    static int64_t get_instruction_count();

  private:
    std::shared_ptr<godot_jit::CModule> module_;
    std::shared_ptr<NativeState> native_;
    std::string error_;
};

} // namespace godot
