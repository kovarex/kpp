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
)
target_include_directories(kppc PRIVATE src)
if (MSVC)
  target_compile_options(kppc PRIVATE /W4 /EHsc)
  target_compile_definitions(kppc PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_options(kppc PRIVATE /STACK:8388608)  # 8 MB stack for deep ASTs
endif()
