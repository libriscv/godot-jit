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
#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <sstream>
#ifdef __linux__
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#endif

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
    if (const char *cc = std::getenv("GODOT_JIT_CC")) {
        if (*cc) return compile_system(source, error, symbols, entry_name, cc);
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
    if (state_) tcc_delete(state_);
#ifdef __linux__
    if (library_) dlclose(library_);
#endif
}

void *CModule::symbol(const char *name) const {
    std::lock_guard<std::mutex> lock(compiler_mutex);
#ifdef __linux__
    if (library_) return dlsym(library_, name);
#endif
    return tcc_get_symbol(state_, name);
}

std::unique_ptr<CModule> CModule::compile_system(const std::string &source, std::string &error,
    const std::vector<std::pair<std::string, const void *>> &symbols, const char *entry_name, const char *cc) {
#ifdef __linux__
    // Use a private directory and argv (never a shell). Keep diagnostics on
    // disk so a verbose compiler cannot deadlock a pipe while we wait for it.
    char directory[] = "/tmp/godot-jit-cc-XXXXXX";
    if (!mkdtemp(directory)) { error = "Cannot create system compiler directory"; return nullptr; }
    const std::string base(directory), input = base + "/module.c", output = base + "/module.so", log = base + "/errors";
    struct Cleanup {
        std::string input, output, log, base;
        ~Cleanup() { unlink(input.c_str()); unlink(output.c_str()); unlink(log.c_str()); rmdir(base.c_str()); }
    } cleanup{input, output, log, base};
    std::ofstream unit(input);
    // The ABI declarations also declare these private function pointers. Both
    // compilers call precisely the same host addresses, even with hidden host
    // visibility. No dependency on the extension's exported ELF symbols.
    for (const auto &s : symbols) unit << "#define " << s.first << " (*gj_host_" << s.first << ")\n";
    unit << godot_jit_abi_source << '\n';
    unit << "\n#line 1 \"jit_input.c\"\n" << source << '\n';
    for (const auto &s : symbols)
        unit << "__typeof__(gj_host_" << s.first << ") gj_host_" << s.first
             << " = (__typeof__(gj_host_" << s.first << "))0x" << std::hex
             << reinterpret_cast<uintptr_t>(s.second) << "ULL;\n" << std::dec;
    unit.close();
    if (!unit) { error = "Cannot write system compiler input"; return nullptr; }
    const pid_t child = fork();
    if (child == 0) {
        int fd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0 || dup2(fd, STDERR_FILENO) < 0 || dup2(fd, STDOUT_FILENO) < 0) _exit(126);
        close(fd);
        execlp(cc, cc, "-std=c99", "-O2", "-g", "-fPIC", "-shared", "-Wl,-z,defs",
               "-o", output.c_str(), input.c_str(), "-lm", static_cast<char *>(nullptr));
        _exit(127);
    }
    if (child < 0) { error = "Cannot start system compiler"; return nullptr; }
    int status = 0;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status)) {
        std::ifstream diagnostics(log);
        error = "System C compiler failed: " + std::string(cc) + "\n" +
            std::string(std::istreambuf_iterator<char>(diagnostics), {});
        return nullptr;
    }
    void *library = dlopen(output.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!library) { error = dlerror(); return nullptr; }
    auto entry = reinterpret_cast<GodotJitEntry>(dlsym(library, entry_name));
    if (!entry) {
        error = std::string("C module must define ") + entry_name;
        dlclose(library);
        return nullptr;
    }
    auto module = std::unique_ptr<CModule>(new CModule(nullptr, std::strcmp(entry_name, "godot_jit_entry") == 0 ? entry : nullptr));
    module->library_ = library;
    return module;
#else
    error = "GODOT_JIT_CC is supported only on Linux";
    return nullptr;
#endif
}

void CModule::write_perf_map(const std::vector<std::pair<const void *, std::string>> &functions) const {
#ifdef __linux__
    const char *enabled = std::getenv("GODOT_JIT_PERF_MAP");
    if (!enabled || std::strcmp(enabled, "1") || !state_) return;
    std::lock_guard<std::mutex> lock(compiler_mutex);
    std::vector<std::pair<uintptr_t, std::string>> entries;
    for (const auto &f : functions)
        if (f.first) entries.emplace_back(reinterpret_cast<uintptr_t>(f.first), f.second);
    // Emitted last in the text section, this marks the end of gj_entry too.
    if (auto end = tcc_get_symbol(state_, "gj_code_end"))
        entries.emplace_back(reinterpret_cast<uintptr_t>(end), "");
    std::sort(entries.begin(), entries.end());
    const std::string path = "/tmp/perf-" + std::to_string(getpid()) + ".map";
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    if (fd < 0) return;
    struct stat info{};
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_uid != getuid()) { close(fd); return; }
    static pid_t map_pid = 0;
    if (map_pid != getpid()) {
        if (ftruncate(fd, 0)) { close(fd); return; }
        map_pid = getpid();
    }
    std::ostringstream lines;
    for (size_t i = 0; i + 1 < entries.size(); ++i) {
        if (entries[i].second.empty() || entries[i].first == entries[i + 1].first) continue;
        auto name = entries[i].second;
        std::replace(name.begin(), name.end(), '\n', ' ');
        std::replace(name.begin(), name.end(), '\r', ' ');
        lines << std::hex << entries[i].first << ' ' << entries[i + 1].first - entries[i].first << ' ' << name << '\n';
    }
    const auto data = lines.str();
    size_t written = 0;
    while (written < data.size()) {
        auto n = write(fd, data.data() + written, data.size() - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        written += n;
    }
    close(fd);
#endif
}

GodotJitInt CModule::invoke(const GodotJitHost &host, GodotJitInt argument) const {
    if (!entry_) throw std::logic_error("This C module uses a different entry signature");
    if (host.abi_version != GODOT_JIT_ABI_VERSION || host.struct_size < sizeof(GodotJitHost)) {
        throw std::invalid_argument("Incompatible Godot JIT host ABI");
    }
    return entry_(&host, argument);
}

}
