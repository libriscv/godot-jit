#include <godot_jit/c_module.h>
#include "abi_source.h"
#include <libtcc.h>
#if defined(__aarch64__) || defined(__riscv)
#include <lib-arm64.h>
#else
#include <libtcc1.h>
#endif
#include <cstring>
#include <mutex>
#include <stdexcept>

namespace godot_jit {
namespace {
// TinyCC has process-global compiler state, including across separate modules.
std::mutex compiler_mutex;

void collect_error(void *context, const char *message) {
    auto &error = *static_cast<std::string *>(context);
    if (!error.empty()) error += '\n';
    error += message;
}
}

std::unique_ptr<CModule> CModule::compile(const std::string &source, std::string &error,
    const std::vector<std::pair<std::string, const void *>> &symbols, const char *entry_name) {
    std::lock_guard<std::mutex> lock(compiler_mutex);
    error.clear();
    if (source.find('\0') != std::string::npos) {
        error = "C source contains an embedded NUL";
        return nullptr;
    }
    std::unique_ptr<TCCState, decltype(&tcc_delete)> state(tcc_new(), tcc_delete);
    if (!state) {
        error = "Could not create TinyCC compiler state";
        return nullptr;
    }
    tcc_set_error_func(state.get(), &error, collect_error);
    tcc_set_options(state.get(), "-std=c99 -nostdlib -nostdinc");
    if (tcc_set_output_type(state.get(), TCC_OUTPUT_MEMORY) < 0) return nullptr;

    for (const auto &symbol : symbols)
        if (tcc_add_symbol(state.get(), symbol.first.c_str(), symbol.second) < 0) return nullptr;

    // Runtime lowering can introduce these calls even for freestanding C.
    if (tcc_add_symbol(state.get(), "memcpy", reinterpret_cast<const void *>(&memcpy)) < 0 ||
        tcc_add_symbol(state.get(), "memmove", reinterpret_cast<const void *>(&memmove)) < 0 ||
        tcc_add_symbol(state.get(), "memset", reinterpret_cast<const void *>(&memset)) < 0 ||
        tcc_add_symbol(state.get(), "memcmp", reinterpret_cast<const void *>(&memcmp)) < 0) return nullptr;

#if defined(__aarch64__) || defined(__riscv)
    std::string unit(reinterpret_cast<const char *>(lib_lib_arm64_c), lib_lib_arm64_c_len);
#else
    std::string unit(reinterpret_cast<const char *>(lib_libtcc1_c), lib_libtcc1_c_len);
#endif
    unit += godot_jit_abi_source;
    unit += "\n#line 1 \"jit_input.c\"\n";
    unit += source;
    if (tcc_compile_string(state.get(), unit.c_str()) < 0) return nullptr;
#ifdef TCC_RELOCATE_AUTO
    if (tcc_relocate(state.get(), TCC_RELOCATE_AUTO) < 0) return nullptr;
#else
    if (tcc_relocate(state.get()) < 0) return nullptr;
#endif
    auto entry = reinterpret_cast<GodotJitEntry>(tcc_get_symbol(state.get(), entry_name));
    if (!entry) {
        error = std::string("C module must define ") + entry_name;
        return nullptr;
    }
    // The diagnostics buffer belongs to the caller, not the executable module.
    tcc_set_error_func(state.get(), nullptr, nullptr);
    auto module = std::unique_ptr<CModule>(new CModule(state.get(), std::strcmp(entry_name, "godot_jit_entry") == 0 ? entry : nullptr));
    state.release();
    return module;
}

CModule::~CModule() {
    std::lock_guard<std::mutex> lock(compiler_mutex);
    tcc_delete(state_);
}

void *CModule::symbol(const char *name) const {
    std::lock_guard<std::mutex> lock(compiler_mutex);
    return tcc_get_symbol(state_, name);
}

GodotJitInt CModule::invoke(const GodotJitHost &host, GodotJitInt argument) const {
    if (!entry_) throw std::logic_error("This C module uses a different entry signature");
    if (host.abi_version != GODOT_JIT_ABI_VERSION || host.struct_size < sizeof(GodotJitHost)) {
        throw std::invalid_argument("Incompatible Godot JIT host ABI");
    }
    return entry_(&host, argument);
}

}
