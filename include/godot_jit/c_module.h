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
    ~CModule();
    CModule(const CModule &) = delete;
    CModule &operator=(const CModule &) = delete;

    GodotJitInt invoke(const GodotJitHost &host, GodotJitInt argument) const;

private:
    CModule(TCCState *state, GodotJitEntry entry) : state_(state), entry_(entry) {}
    TCCState *state_;
    GodotJitEntry entry_;
};

}
