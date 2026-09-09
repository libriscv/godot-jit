include(FetchContent)
FetchContent_Declare(godot_jit_tinycc
    GIT_REPOSITORY https://github.com/fwsGonzo/tinycc.git
    GIT_TAG 0a074d73ed8d9bc1378bb09f4ca526ae3b175b3c)
FetchContent_MakeAvailable(godot_jit_tinycc)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The initial Godot JIT ABI requires a 64-bit target")
endif()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(jit_tcc_arch TCC_TARGET_X86_64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(jit_tcc_arch TCC_TARGET_ARM64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^riscv64$")
    set(jit_tcc_arch TCC_TARGET_RISCV64)
else()
    message(FATAL_ERROR "Unsupported TinyCC native target: ${CMAKE_SYSTEM_PROCESSOR}")
endif()

# libtcc.c includes its compiler sources and a sibling config.h. Keep a private
# build copy so a FETCHCONTENT_SOURCE_DIR_GODOT_JIT_TINYCC checkout stays untouched.
set(jit_tcc_dir "${CMAKE_CURRENT_BINARY_DIR}/tinycc")
file(GLOB jit_tcc_sources CONFIGURE_DEPENDS
    "${godot_jit_tinycc_SOURCE_DIR}/*.c" "${godot_jit_tinycc_SOURCE_DIR}/*.h"
    "${godot_jit_tinycc_SOURCE_DIR}/*.def")
foreach(source IN LISTS jit_tcc_sources)
    get_filename_component(name "${source}" NAME)
    if(NOT name STREQUAL "config.h")
        configure_file("${source}" "${jit_tcc_dir}/${name}" COPYONLY)
    endif()
endforeach()
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/tcc_config.h.in" "${jit_tcc_dir}/config.h" @ONLY)

# Same one-source build and embedded runtime helpers as libriscv's RISCV_LIBTCC.
add_library(godot_jit_tcc STATIC "${jit_tcc_dir}/libtcc.c")
target_include_directories(godot_jit_tcc PUBLIC "${jit_tcc_dir}")
target_compile_definitions(godot_jit_tcc PRIVATE ${jit_tcc_arch}=1
    CONFIG_TCC_BACKTRACE=0 CONFIG_TCC_BCHECK=0)
if(ANDROID)
    target_compile_definitions(godot_jit_tcc PRIVATE TARGETOS_ANDROID=1)
elseif(APPLE)
    target_compile_definitions(godot_jit_tcc PRIVATE TARGETOS_Darwin=1 TCC_TARGET_MACHO=1)
elseif(WIN32)
    target_compile_definitions(godot_jit_tcc PRIVATE TARGETOS_Windows=1 TCC_TARGET_PE=1)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(godot_jit_tcc PRIVATE TARGETOS_Linux=1)
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    target_compile_definitions(godot_jit_tcc PRIVATE TARGETOS_FreeBSD=1)
else()
    message(FATAL_ERROR "Unsupported TinyCC system: ${CMAKE_SYSTEM_NAME}")
endif()
target_link_libraries(godot_jit_tcc PUBLIC ${CMAKE_DL_LIBS})
