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
  src/Driver/BodyGraft.cpp
  src/Driver/CImport.cpp
  src/Driver/Compile.cpp
  src/Driver/ConfigConstants.cpp
  src/Driver/FrontEnd.cpp
  src/Driver/Link.cpp
  src/Driver/ModuleCompile.cpp
  src/Driver/Options.cpp
  src/Driver/Runtime.cpp
  src/convert.cpp
  src/lexer.cpp
  src/SymbolTable.cpp
  src/parser.cpp
  src/resolver.cpp
  src/Resolver/Walker.cpp
  src/Resolver/Lookup.cpp
  src/Resolver/InstanceTasks.cpp
  src/Resolver/AccessChecker.cpp
  src/Resolver/TypeOfAst.cpp
  src/Resolver/Indexer.cpp
  src/Resolver/GlobalScope.cpp
  src/Resolver/OverloadResolution.cpp
  src/Resolver/Instantiation.cpp
  src/Resolver/Interpreter.cpp
  src/TypeTable.cpp
  src/Codegen/CCallingConvention.cpp
  src/Codegen/Calls.cpp
  src/Codegen/ClassMetadata.cpp
  src/Codegen/CodegenSession.cpp
  src/Codegen/ConstantData.cpp
  src/Codegen/Constants.cpp
  src/Codegen/ControlFlow.cpp
  src/Codegen/DebugInfo.cpp
  src/Codegen/Diagnostics.cpp
  src/Codegen/EmitterNames.cpp
  src/Codegen/Exceptions.cpp
  src/Codegen/Expressions.cpp
  src/Codegen/FunctionDefinitions.cpp
  src/Codegen/Functions.cpp
  src/Codegen/ItaniumExceptions.cpp
  src/Codegen/Lambdas.cpp
  src/Codegen/Layout.cpp
  src/Codegen/Operators.cpp
  src/Codegen/ParameterPacks.cpp
  src/Codegen/Scopes.cpp
  src/Codegen/SseBuiltins.cpp
  src/Codegen/Statements.cpp
  src/Codegen/TypedUnions.cpp
  src/Codegen/Values.cpp
  src/consteval.cpp
  src/instance.cpp
  src/source.cpp
  src/config.cpp
  src/server.cpp
  src/lsp.cpp
  src/natvis.cpp
  src/run.cpp
  src/generate.cpp
  src/packages.cpp
  src/Project/Project.cpp
  src/Project/Describe.cpp
  src/Project/DescribeFile.cpp
  src/Project/Build.cpp
  src/Project/CLibraryBuild.cpp
  src/Project/IdeProjects.cpp
  src/Project/Measure.cpp
  src/Project/Menu.cpp
  src/tests.cpp
  src/ast_walk.cpp
  src/version.cpp
  src/backend.cpp
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

# LLVM's back end inside kppc (src/backend.cpp, docs/codegen-speed.md): KPP_LLVM_DIR names the
# official clang+llvm-19.1.7 release's lib and include, as compiler/llvm.ps1 unpacks them. Without
# it kppc runs clang per module as before.
set(KPP_LLVM_DIR "" CACHE PATH "LLVM (lib and include) for the in-process back end; empty: none")
if (KPP_LLVM_DIR)
  # The release's static libraries (not its DLLs' import libraries: LLVM-C, LTO, Remarks); the linker
  # takes what the back end uses. (Its CMake package names its tools in bin\, which kppc needs not.)
  file(GLOB kpp_llvm_libs "${KPP_LLVM_DIR}/lib/LLVM*.lib")
  list(FILTER kpp_llvm_libs EXCLUDE REGEX "LLVM-C[.]lib$")
  target_include_directories(kppc PRIVATE "${KPP_LLVM_DIR}/include")
  target_compile_definitions(kppc PRIVATE KPP_WITH_LLVM)
  # The release's libraries are built against the static C runtime (/MT): so is kppc with them.
  set_property(TARGET kppc PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
  target_link_libraries(kppc PRIVATE ${kpp_llvm_libs} psapi shell32 ole32 uuid advapi32 ws2_32 ntdll)
  if (MSVC)
    # (LLVM's headers are not warning-free at /W4.)
    set_source_files_properties(src/backend.cpp PROPERTIES COMPILE_OPTIONS "/W0;/WX-")
  endif()
endif()
