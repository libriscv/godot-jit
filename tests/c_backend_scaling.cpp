#include <compiler.h>
#include <chrono>
#include <iostream>
#include <sstream>
#include <string>

int main(int argc, char **argv) {
    if (argc > 2 || (argc == 2 && std::string(argv[1]) != "--stress")) {
        std::cerr << "Usage: " << argv[0] << " [--stress]\n";
        return 2;
    }
    const bool stress = argc == 2;
    // Regression for repeated propagation through every suffix of a large
    // branchy function. Keep routine runs affordable with an unoptimized Debug
    // compiler; --stress retains the original sizes for manual profiling.
    // CTest bounds runtime without a machine-specific timing ratio.
    for (int base_count : {50, 100, 200, 400}) {
        const int count = stress ? base_count * 2 : base_count;
        std::ostringstream source;
        source << "extends SceneTree\nvar total := 0.0\nfunc large() -> void:\n";
        for (int i = 0; i < count; ++i) {
            source << "    var v" << i << " := Vector3(" << i << ", " << i << ".5, " << i
                   << ") + Vector3(" << i << ", 0, 1)\n"
                   << "    if v" << i << ".x > 0.0: total += v" << i << ".length()\n";
        }
        gdscript::Compiler compiler;
        std::cout << "Compiling " << count << " vector branches..." << std::endl;
        const auto start = std::chrono::steady_clock::now();
        auto generated = compiler.compile_to_c(source.str());
        if (!generated || generated->empty()) {
            std::cerr << compiler.get_error() << '\n';
            return 1;
        }
        std::cout << count << " vector branches: "
                  << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()
                  << " seconds" << std::endl;
    }

    // Dictionary/callback-heavy control flow exercises verifier joins as well
    // as the C backend's type analysis, with the native script options.
    const int count = stress ? 600 : 200;
    std::ostringstream source;
    source << "extends SceneTree\nfunc large(target) -> void:\n";
    for (int i = 0; i < count; ++i) {
        source << "    var row" << i << " = {\"position\": Vector3(" << i
               << ", 0, 0), \"name\": \"entry\", \"enabled\": true}\n"
               << "    var callback" << i << " = func(value): return value == row" << i << ".name\n"
               << "    if target.valid and callback" << i << ".call(\"entry\"):\n"
               << "        target.accept(row" << i << ")\n"
               << "    else:\n        target.reject(row" << i << ")\n";
    }
    source << "    await process_frame\n";
    gdscript::Compiler compiler;
    gdscript::CompilerOptions options;
    options.native_classes = true;
    options.optimize = false;
    options.batch_iteration = false;
    std::cout << "Compiling " << count << " dictionary/callback branches..." << std::endl;
    const auto start = std::chrono::steady_clock::now();
    auto generated = compiler.compile_to_c(source.str(), options);
    if (!generated || generated->empty()) {
        std::cerr << compiler.get_error() << '\n';
        return 1;
    }
    std::cout << count << " dictionary/callback branches: "
              << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()
              << " seconds" << std::endl;
}
