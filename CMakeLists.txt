cmake_minimum_required(VERSION 3.24)
project(godot_jit LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

option(GODOT_JIT_BUILD_TESTS "Build addon tests" ON)
option(GODOT_JIT_BENCHMARK_GATE "Run the local typical performance regression gate (noisy on shared CI)" OFF)
set(GODOTCPP_DISABLE_EXCEPTIONS OFF CACHE BOOL "Enable C++ exceptions")
set(GODOTCPP_API_VERSION "4.6" CACHE STRING "Godot API version")
set(GODOTCPP_BUILD_PROFILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_profile.json"
    CACHE FILEPATH "Godot bindings used by the addon")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GODOTCPP_BUILD_PROFILE}")
add_subdirectory(godot-cpp EXCLUDE_FROM_ALL)
include(cmake/tinycc.cmake)

# Reuse the host compiler without the sandbox, VM, tools or upstream tests.
function(add_safegdscript_compiler)
    set(BUILD_TESTING OFF)
    set(GDSCRIPT_BUILD_RISCV OFF CACHE BOOL "Build the upstream RISC-V backend")
    add_subdirectory(godot-sandbox/src/gdscript/compiler safegdscript EXCLUDE_FROM_ALL)
    if(ANDROID)
        # NDK r24's libc++ has an explicit ostringstream default constructor.
        # Fix aggregate initialization in a build copy until upstream supports it.
        set(codegen_source "${CMAKE_CURRENT_SOURCE_DIR}/godot-sandbox/src/gdscript/compiler/c_codegen.cpp")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${codegen_source}")
        file(READ "${codegen_source}" codegen)
        string(REPLACE "Emitter{program, {}," "Emitter{program, std::ostringstream{}," codegen "${codegen}")
        string(REPLACE "Emitter{optimized, {}," "Emitter{optimized, std::ostringstream{}," codegen "${codegen}")
        set(codegen_copy "${CMAKE_CURRENT_BINARY_DIR}/safegdscript/c_codegen.cpp")
        file(CONFIGURE OUTPUT "${codegen_copy}" CONTENT "${codegen}" @ONLY)
        set_source_files_properties("${codegen_source}" TARGET_DIRECTORY gdscript_frontend PROPERTIES HEADER_FILE_ONLY TRUE)
        target_sources(gdscript_frontend PRIVATE "${codegen_copy}")
    endif()
endfunction()
add_safegdscript_compiler()
add_library(godot_jit::safegdscript ALIAS gdscript_frontend)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/include/godot_jit/abi.h" GODOT_JIT_ABI_SOURCE)
set(jit_c_abi "${CMAKE_CURRENT_SOURCE_DIR}/godot-sandbox/src/gdscript/compiler/c_abi.h")
file(READ "${jit_c_abi}" GODOT_JIT_C_ABI_SOURCE)
string(APPEND GODOT_JIT_ABI_SOURCE "\n${GODOT_JIT_C_ABI_SOURCE}")
if (DOUBLE_PRECISION OR GODOTCPP_PRECISION STREQUAL "double")
    string(PREPEND GODOT_JIT_ABI_SOURCE "#define DOUBLE_PRECISION_REAL_T 1\n")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS include/godot_jit/abi.h "${jit_c_abi}")
configure_file(cmake/abi_source.h.in generated/abi_source.h @ONLY)

find_package(Threads REQUIRED)
add_library(godot_jit_core STATIC src/c_module.cpp)
target_include_directories(godot_jit_core PUBLIC include PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(godot_jit_core PRIVATE godot_jit_tcc Threads::Threads ${CMAKE_DL_LIBS})

add_library(godot-jit SHARED src/godot_jit.cpp src/c_abi.cpp src/register_types.cpp src/native_program.cpp src/function_state.cpp src/property_defaults.cpp src/unsafe_gdscript.cpp src/unsafe_language.cpp src/script_instance.cpp src/unsafe_debugger.cpp src/unsafe_highlighter.cpp src/unsafe_editor.cpp)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/builtin_methods.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_builtin_cache.py"
        "${CMAKE_CURRENT_BINARY_DIR}/godot-cpp/extension_api.json" "${CMAKE_CURRENT_BINARY_DIR}/generated/builtin_methods.inc"
    DEPENDS scripts/generate_builtin_cache.py "${CMAKE_CURRENT_BINARY_DIR}/godot-cpp/extension_api.json"
    VERBATIM)
target_sources(godot-jit PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/builtin_methods.inc")
target_include_directories(godot-jit PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(godot-jit PRIVATE godot-cpp godot_jit_core godot_jit::safegdscript)
get_target_property(jit_platform godot-cpp GODOTCPP_PLATFORM)
get_target_property(jit_arch godot-cpp GODOTCPP_ARCH)
set(jit_output_dir "${CMAKE_CURRENT_SOURCE_DIR}/bin/addons/godot_jit/bin")
if(GODOTCPP_TARGET STREQUAL "template_release")
    set(jit_variant release)
else()
    set(jit_variant debug)
endif()
set_target_properties(godot-jit PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "godot-jit.${jit_platform}.template_${jit_variant}.${jit_arch}"
    LIBRARY_OUTPUT_DIRECTORY "$<1:${jit_output_dir}>"
    RUNTIME_OUTPUT_DIRECTORY "$<1:${jit_output_dir}>")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(godot-jit PRIVATE "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/exports.map")
    set_property(TARGET godot-jit APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/exports.map")
endif()
if(ANDROID)
    # Keep libraries built with older NDKs aligned for 16 KB Android pages too.
    target_link_options(godot-jit PRIVATE "LINKER:-z,max-page-size=16384" "LINKER:-z,common-page-size=16384")
endif()

if(GODOT_JIT_BUILD_TESTS)
    enable_testing()
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME godot_jit_demo_compat COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/demo_compat.py")
        add_test(NAME godot_jit_benchmark_report COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmark_report_test.py")
    endif()
    add_executable(godot_jit_basic tests/basic.cpp)
    target_link_libraries(godot_jit_basic PRIVATE godot_jit_core godot_jit::safegdscript)
    add_test(NAME godot_jit_basic COMMAND godot_jit_basic)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_test(NAME godot_jit_basic_cc COMMAND godot_jit_basic)
        set_tests_properties(godot_jit_basic_cc PROPERTIES ENVIRONMENT "GODOT_JIT_CC=${CMAKE_C_COMPILER}")
    endif()
    add_executable(godot_jit_c_backend tests/c_backend.cpp)
    target_link_libraries(godot_jit_c_backend PRIVATE godot_jit_core godot_jit::safegdscript)
    add_test(NAME godot_jit_c_backend COMMAND godot_jit_c_backend "${CMAKE_CURRENT_BINARY_DIR}/test_backend.c")
    set_tests_properties(godot_jit_c_backend PROPERTIES FIXTURES_SETUP emitted_c)
    if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        add_test(NAME godot_jit_system_c COMMAND "${CMAKE_C_COMPILER}" -std=c99 -Wall -Wextra
            -Werror -Wno-unused-parameter -Wno-unused-label -O2
            -I "${CMAKE_CURRENT_SOURCE_DIR}/godot-sandbox/src/gdscript/compiler"
            "${CMAKE_CURRENT_BINARY_DIR}/test_backend.c" "${CMAKE_CURRENT_SOURCE_DIR}/tests/system_c_host.c"
            -o "${CMAKE_CURRENT_BINARY_DIR}/test_backend${CMAKE_EXECUTABLE_SUFFIX}")
        set_tests_properties(godot_jit_system_c PROPERTIES FIXTURES_REQUIRED emitted_c FIXTURES_SETUP system_c)
        add_test(NAME godot_jit_system_c_run COMMAND "${CMAKE_CURRENT_BINARY_DIR}/test_backend${CMAKE_EXECUTABLE_SUFFIX}")
        set_tests_properties(godot_jit_system_c_run PROPERTIES FIXTURES_REQUIRED system_c)
        if(UNIX)
            set(jit_test_math_library -lm)
        endif()
        add_test(NAME godot_jit_system_math COMMAND "${CMAKE_C_COMPILER}" -std=c99 -Wall -Wextra
            -Werror -Wno-unused-parameter -Wno-unused-label -O2 -DGJ_TEST_MATH
            -I "${CMAKE_CURRENT_SOURCE_DIR}/godot-sandbox/src/gdscript/compiler"
            "${CMAKE_CURRENT_BINARY_DIR}/test_backend.c.math.c" "${CMAKE_CURRENT_SOURCE_DIR}/tests/system_c_host.c"
            -o "${CMAKE_CURRENT_BINARY_DIR}/test_math${CMAKE_EXECUTABLE_SUFFIX}" ${jit_test_math_library})
        set_tests_properties(godot_jit_system_math PROPERTIES FIXTURES_REQUIRED emitted_c FIXTURES_SETUP system_math)
        add_test(NAME godot_jit_system_math_run COMMAND "${CMAKE_CURRENT_BINARY_DIR}/test_math${CMAKE_EXECUTABLE_SUFFIX}")
        set_tests_properties(godot_jit_system_math_run PROPERTIES FIXTURES_REQUIRED system_math)
    endif()
    find_program(GODOT_JIT_GODOT_EXECUTABLE NAMES godot godot4)
    if(GODOT_JIT_GODOT_EXECUTABLE)
        # Exercise this build's binary, including Release under a debug editor.
        set(jit_test_project "${CMAKE_CURRENT_BINARY_DIR}/test-project/$<CONFIG>")
        file(GENERATE OUTPUT "${jit_test_project}/project.godot" INPUT "${CMAKE_CURRENT_SOURCE_DIR}/bin/project.godot")
        file(GENERATE OUTPUT "${jit_test_project}/tests/basic.gd" INPUT "${CMAKE_CURRENT_SOURCE_DIR}/bin/tests/basic.gd")
        file(GENERATE OUTPUT "${jit_test_project}/godot_jit.gdextension" CONTENT
            "[configuration]\nentry_symbol = \"godot_jit_library_init\"\ncompatibility_minimum = \"4.6\"\n\n[libraries]\n${jit_platform}.${jit_arch} = \"$<TARGET_FILE:godot-jit>\"\n")
        file(GENERATE OUTPUT "${jit_test_project}/tests/async.gd" INPUT "${CMAKE_CURRENT_SOURCE_DIR}/bin/tests/async.gd" NEWLINE_STYLE UNIX)
        file(GENERATE OUTPUT "${jit_test_project}/tests/math.gd" INPUT "${CMAKE_CURRENT_SOURCE_DIR}/bin/tests/math.gd")
        file(GENERATE OUTPUT "${jit_test_project}/tests/backend.gd" INPUT "${CMAKE_CURRENT_SOURCE_DIR}/bin/tests/backend.gd")
        file(GENERATE OUTPUT "${jit_test_project}/tests/baseline.gd" INPUT "${CMAKE_CURRENT_SOURCE_DIR}/bin/tests/baseline.gd")
        file(GENERATE OUTPUT "${jit_test_project}/tests/editor.gd" INPUT "${CMAKE_CURRENT_SOURCE_DIR}/bin/tests/editor.gd")
        file(GENERATE OUTPUT "${jit_test_project}/tests/compatibility.gd" INPUT "${CMAKE_CURRENT_SOURCE_DIR}/bin/tests/compatibility.gd")
        file(GENERATE OUTPUT "${jit_test_project}/tests/debugger_base.ugd" INPUT "${CMAKE_CURRENT_SOURCE_DIR}/tests/ugd/debugger_base.ugd" NEWLINE_STYLE UNIX)
        file(GENERATE OUTPUT "${jit_test_project}/tests/debugger_inherited.ugd" INPUT "${CMAKE_CURRENT_SOURCE_DIR}/tests/ugd/debugger_inherited.ugd" NEWLINE_STYLE UNIX)
        file(GENERATE OUTPUT "${jit_test_project}/tests/debugger.gd" INPUT "${CMAKE_CURRENT_SOURCE_DIR}/bin/tests/debugger.gd")
        file(GENERATE OUTPUT "${jit_test_project}/tests/debugger.ugd" INPUT "${CMAKE_CURRENT_SOURCE_DIR}/tests/ugd/debugger.ugd" NEWLINE_STYLE UNIX)
        file(GENERATE OUTPUT "${jit_test_project}/tests/debugger_remote.gd" INPUT "${CMAKE_CURRENT_SOURCE_DIR}/bin/tests/debugger_remote.gd")
        file(GENERATE OUTPUT "${jit_test_project}/tests/highlighter.gd" INPUT "${CMAKE_CURRENT_SOURCE_DIR}/bin/tests/highlighter.gd")
        # The reload fixture edits embedded source using LF-delimited matches.
        # file(GENERATE) otherwise writes native CRLF on Windows, even with LF input.
        file(GENERATE OUTPUT "${jit_test_project}/tests/language.gd" INPUT "${CMAKE_CURRENT_SOURCE_DIR}/bin/tests/language.gd" NEWLINE_STYLE UNIX)
        file(GENERATE OUTPUT "${jit_test_project}/tests/call_benchmark.gd" INPUT "${CMAKE_CURRENT_SOURCE_DIR}/bin/tests/call_benchmark.gd")
        file(GENERATE OUTPUT "${jit_test_project}/tests/typical_benchmark.gd" INPUT "${CMAKE_CURRENT_SOURCE_DIR}/bin/tests/typical_benchmark.gd")
        file(GENERATE OUTPUT "${jit_test_project}/tests/typical.ugd" INPUT "${CMAKE_CURRENT_SOURCE_DIR}/tests/ugd/typical.ugd")
        file(GENERATE OUTPUT "${jit_test_project}/tests/ugd.gd" INPUT "${CMAKE_CURRENT_SOURCE_DIR}/bin/tests/ugd.gd" NEWLINE_STYLE UNIX)
        # Copy helpers as well as tests, preserving relative resource paths.
        file(GLOB_RECURSE jit_ugd_fixtures CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/tests/ugd"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/ugd/*.ugd")
        foreach(fixture IN LISTS jit_ugd_fixtures)
            file(GENERATE OUTPUT "${jit_test_project}/tests/ugd/${fixture}"
                INPUT "${CMAKE_CURRENT_SOURCE_DIR}/tests/ugd/${fixture}" NEWLINE_STYLE UNIX)
            if(fixture MATCHES "(^|/)test_[^/]+\\.ugd$")
                string(REGEX REPLACE "\\.ugd$" "" fixture_name "${fixture}")
                add_test(NAME "godot_jit_ugd/${fixture_name}" COMMAND "${GODOT_JIT_GODOT_EXECUTABLE}"
                    --headless --path "${jit_test_project}" --script res://tests/ugd.gd -- "res://tests/ugd/${fixture}")
                set_tests_properties("godot_jit_ugd/${fixture_name}" PROPERTIES
                    FIXTURES_REQUIRED godot_jit_import TIMEOUT 30
                    ENVIRONMENT "XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-data;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-config;XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-cache")
            endif()
        endforeach()
        file(GLOB_RECURSE jit_ugd_scenes CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/tests/ugd"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/ugd/*.tscn")
        foreach(scene IN LISTS jit_ugd_scenes)
            file(GENERATE OUTPUT "${jit_test_project}/tests/ugd/${scene}"
                INPUT "${CMAKE_CURRENT_SOURCE_DIR}/tests/ugd/${scene}" NEWLINE_STYLE UNIX)
        endforeach()
        # Load at startup. Discovering a new extension mid-import restarts the
        # editor, whose cleanup can race deferred docs generation in Godot 4.6/4.7.
        file(GENERATE OUTPUT "${jit_test_project}/.godot/extension_list.cfg"
            CONTENT "res://godot_jit.gdextension\n")
        add_test(NAME godot_jit_import COMMAND "${GODOT_JIT_GODOT_EXECUTABLE}"
            --headless --path "${jit_test_project}" --import)
        set_tests_properties(godot_jit_import PROPERTIES FIXTURES_SETUP godot_jit_import TIMEOUT 60)
        add_test(NAME godot_jit_extension COMMAND "${GODOT_JIT_GODOT_EXECUTABLE}"
            --headless --path "${jit_test_project}" --script res://tests/basic.gd)
        set_tests_properties(godot_jit_extension PROPERTIES FIXTURES_REQUIRED godot_jit_import TIMEOUT 30)
        add_test(NAME godot_jit_async COMMAND "${GODOT_JIT_GODOT_EXECUTABLE}"
            --headless --path "${jit_test_project}" --script res://tests/async.gd)
        set_tests_properties(godot_jit_async PROPERTIES FIXTURES_REQUIRED godot_jit_import TIMEOUT 30)
        add_test(NAME godot_jit_math COMMAND "${GODOT_JIT_GODOT_EXECUTABLE}"
            --headless --path "${jit_test_project}" --script res://tests/math.gd)
        set_tests_properties(godot_jit_math PROPERTIES FIXTURES_REQUIRED godot_jit_import TIMEOUT 30)
        add_test(NAME godot_jit_backend COMMAND "${GODOT_JIT_GODOT_EXECUTABLE}"
            --headless --path "${jit_test_project}" --script res://tests/backend.gd)
        set_tests_properties(godot_jit_backend PROPERTIES FIXTURES_REQUIRED godot_jit_import TIMEOUT 30)
        add_test(NAME godot_jit_baseline COMMAND "${GODOT_JIT_GODOT_EXECUTABLE}"
            --headless --path "${jit_test_project}" --script res://tests/baseline.gd)
        set_tests_properties(godot_jit_baseline PROPERTIES FIXTURES_REQUIRED godot_jit_import TIMEOUT 30
            FAIL_REGULAR_EXPRESSION "SCRIPT ERROR:;ERROR:" ENVIRONMENT
            "XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-data;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-config;XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-cache")
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_test(NAME godot_jit_baseline_cc COMMAND "${GODOT_JIT_GODOT_EXECUTABLE}"
                --headless --path "${jit_test_project}" --script res://tests/baseline.gd)
            set_tests_properties(godot_jit_baseline_cc PROPERTIES FIXTURES_REQUIRED godot_jit_import TIMEOUT 60
                FAIL_REGULAR_EXPRESSION "SCRIPT ERROR:;ERROR:" ENVIRONMENT
                "GODOT_JIT_CC=${CMAKE_C_COMPILER};XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-data;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-config;XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-cache")
            add_test(NAME godot_jit_perf_map COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/perf_map.py" "${GODOT_JIT_GODOT_EXECUTABLE}" "${jit_test_project}")
            set_tests_properties(godot_jit_perf_map PROPERTIES FIXTURES_REQUIRED godot_jit_import TIMEOUT 40 ENVIRONMENT
                "XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-data;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-config;XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-cache")
        endif()
        add_test(NAME godot_jit_language COMMAND "${GODOT_JIT_GODOT_EXECUTABLE}"
            --headless --path "${jit_test_project}" --script res://tests/language.gd)
        set_tests_properties(godot_jit_language PROPERTIES FIXTURES_REQUIRED godot_jit_import TIMEOUT 60)
        add_test(NAME godot_jit_compatibility COMMAND "${GODOT_JIT_GODOT_EXECUTABLE}"
            --headless --path "${jit_test_project}" --script res://tests/compatibility.gd)
        set_tests_properties(godot_jit_compatibility PROPERTIES FIXTURES_REQUIRED godot_jit_import TIMEOUT 60
            FAIL_REGULAR_EXPRESSION "ERROR:;leaked;resources still in use"
            ENVIRONMENT "XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-data;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-config;XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-cache")
        if(Python3_Interpreter_FOUND)
            add_test(NAME godot_jit_debugger COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/debugger.py"
                "${GODOT_JIT_GODOT_EXECUTABLE}" "${jit_test_project}")
            set_tests_properties(godot_jit_debugger PROPERTIES FIXTURES_REQUIRED godot_jit_import TIMEOUT 60)
            set_tests_properties(godot_jit_debugger PROPERTIES ENVIRONMENT
                "XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-data;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-config;XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-cache")
        endif()
        add_test(NAME godot_jit_debugger_remote COMMAND "${GODOT_JIT_GODOT_EXECUTABLE}"
            --headless --path "${jit_test_project}" --script res://tests/debugger_remote.gd)
        set_tests_properties(godot_jit_debugger_remote PROPERTIES FIXTURES_REQUIRED godot_jit_import TIMEOUT 30)
        add_test(NAME godot_jit_editor COMMAND "${GODOT_JIT_GODOT_EXECUTABLE}"
            --headless --editor --path "${jit_test_project}" --script res://tests/editor.gd)
        set_tests_properties(godot_jit_editor PROPERTIES FIXTURES_REQUIRED godot_jit_import TIMEOUT 30)
        add_test(NAME godot_jit_highlighter COMMAND "${GODOT_JIT_GODOT_EXECUTABLE}"
            --headless --path "${jit_test_project}" --script res://tests/highlighter.gd)
        set_tests_properties(godot_jit_highlighter PROPERTIES FIXTURES_REQUIRED godot_jit_import TIMEOUT 30)
        add_test(NAME godot_jit_typical COMMAND "${GODOT_JIT_GODOT_EXECUTABLE}"
            --headless --path "${jit_test_project}" --script res://tests/typical_benchmark.gd -- --check-only)
        set_tests_properties(godot_jit_typical PROPERTIES FIXTURES_REQUIRED godot_jit_import TIMEOUT 30)
        if(GODOT_JIT_BENCHMARK_GATE AND Python3_Interpreter_FOUND)
            add_test(NAME godot_jit_typical_gate COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmark_report.py" --gate "${GODOT_JIT_GODOT_EXECUTABLE}" "${jit_test_project}")
            set_tests_properties(godot_jit_typical_gate PROPERTIES FIXTURES_REQUIRED godot_jit_import
                RUN_SERIAL TRUE TIMEOUT 150 LABELS performance
                ENVIRONMENT "XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-data;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-config;XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-cache")
        endif()
        set_tests_properties(godot_jit_math godot_jit_async godot_jit_import godot_jit_extension godot_jit_backend godot_jit_language godot_jit_highlighter godot_jit_editor godot_jit_debugger_remote godot_jit_typical PROPERTIES ENVIRONMENT
            "XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-data;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-config;XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-cache")
    endif()
endif()
