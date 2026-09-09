#ifndef GODOT_JIT_ABI_H
#define GODOT_JIT_ABI_H

#define GODOT_JIT_ABI_VERSION 1u

/* Freestanding C: no Godot, C++ or system headers in generated translation units. */
typedef signed long long GodotJitInt;
typedef unsigned int GodotJitUInt;

typedef struct GodotJitHost {
    GodotJitUInt abi_version;
    GodotJitUInt struct_size;
    void *userdata;
    void (*log)(void *userdata, const char *message);
} GodotJitHost;

typedef GodotJitInt (*GodotJitEntry)(const GodotJitHost *host, GodotJitInt argument);

#ifdef __cplusplus
extern "C" {
#endif
GodotJitInt godot_jit_entry(const GodotJitHost *host, GodotJitInt argument);
#ifdef __cplusplus
}
static_assert(sizeof(GodotJitInt) == 8 && sizeof(GodotJitUInt) == 4);
#endif

#endif
