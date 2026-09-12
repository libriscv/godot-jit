#pragma once
#include "abi.h"
#include <memory>
#include <string>
#include <vector>
#include <utility>

struct TCCState;

namespace godot_jit {

class CModule {
public:
    static std::unique_ptr<CModule> compile(const std::string &source, std::string &error,
        const std::vector<std::pair<std::string, const void *>> &symbols = {},
        const char *entry_name = "godot_jit_entry");
    void *symbol(const char *name) const;
    // Linux diagnostics only; enabled by GODOT_JIT_PERF_MAP=1.
    void write_perf_map(const std::vector<std::pair<const void *, std::string>> &functions) const;
    ~CModule();
    CModule(const CModule &) = delete;
    CModule &operator=(const CModule &) = delete;

    GodotJitInt invoke(const GodotJitHost &host, GodotJitInt argument) const;

private:
    CModule(TCCState *state, GodotJitEntry entry) : state_(state), entry_(entry) {}
    static std::unique_ptr<CModule> compile_system(const std::string &, std::string &,
        const std::vector<std::pair<std::string, const void *>> &, const char *, const char *);
    TCCState *state_;
    GodotJitEntry entry_;
    void *library_ = nullptr;
};

}
