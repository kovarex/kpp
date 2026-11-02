cmake_minimum_required(VERSION 3.25)
project(kpp C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(kppc
  src/main.cpp
  src/convert.cpp
  src/lexer.cpp
  src/parser.cpp
  src/resolver.cpp
  src/types.cpp
  src/codegen.cpp
  src/consteval.cpp
  src/instance.cpp
  src/source.cpp
  src/server.cpp
  src/lsp.cpp
  src/run.cpp
  src/generate.cpp
  src/project.cpp
  src/tests.cpp
  src/ast_walk.cpp
  src/version.cpp
)
target_include_directories(kppc PRIVATE src)

# kpp's version (docs/build.md, "Versions"): version.cmake runs at every build and rewrites
# generated/version.hpp only when the version or the commit changed, so only version.cpp recompiles.
add_custom_target(kpp_version
  COMMAND "${CMAKE_COMMAND}" "-DROOT=${CMAKE_CURRENT_SOURCE_DIR}/.." "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/generated/version.hpp"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/version.cmake"
  BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/generated/version.hpp"
  COMMENT "kpp's version")
add_dependencies(kppc kpp_version)
target_include_directories(kppc PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
if (MSVC)
  # Warnings are errors, so the build stays at none. Two are off: `T{a, b}` leaving the later
  # fields to their defaults is the code's aggregate style, and `a || b && c` reads as `a + b * c`.
  target_compile_options(kppc PRIVATE /W4 /WX /EHsc)
  if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(kppc PRIVATE -Wno-missing-field-initializers -Wno-logical-op-parentheses)
  endif()
  target_compile_definitions(kppc PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_options(kppc PRIVATE /STACK:8388608)  # 8 MB stack for deep ASTs
endif()
