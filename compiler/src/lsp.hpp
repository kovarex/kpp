#pragma once
#include "server.hpp"
#include <functional>
#include <vector>

struct ParsedFile;
class SymbolTable;
class GlobalScope;

// The language server (docs/ide.md, "The language server as built"): `kppc --lsp [OPTIONS
// SOURCES]`, started by an editor, speaks the Language Server Protocol as JSON-RPC on stdin and
// stdout. It builds the program's front end in this process as a compile server does (kept between
// builds, incremental where it can be), with the editor's unsaved buffers standing for their files,
// and answers from what that build resolved. Without sources it checks the project of the editor's
// workspace (build.kpp, found from the workspace folder up), or the workspace folder itself.
// --check-each-change: a check after every change (tests: the transcript does not depend on timing).
int runLanguageServer(int argc, char** argv, CompileFunction compile);

// The program the last build kept (main.cpp), for queries: f runs with it locked, so no build
// changes it meanwhile. False when no build was kept.
struct KeptProgram
{
  std::vector<ParsedFile*> files; // in the build's order: the standard library, the program, the configuration, the C import
  SymbolTable* symbols = nullptr;
  GlobalScope* global = nullptr;
};
bool withKeptProgram(const std::function<void(const KeptProgram&)>& f);
