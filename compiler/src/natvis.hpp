#pragma once
// Visual Studio's view of the standard library's values (docs/debugging.md, "Values in Visual
// Studio"): a .natvis that shows a String as its text, a Vector as its elements, a HashMap as its
// entries. kpp writes it beside the executable of a --debug-info build and links it into the PDB
// (/NATVIS), so the debugger loads it with the program. The rules name the library's members; each
// is checked against the library's classes as they are, and one whose member is gone is left out
// with a problem (tests\natvis.ps1 fails on any).
#include <string>
#include <vector>

struct ParsedFile;
class GlobalScope;
class SymbolTable;

struct DebugClass;

// The .natvis for a program's files (the library's among them) and the classes its debug
// information defines, by the names it gives them (CodegenResult::debugClasses): the per-instance
// rules are of those classes, by those names. A class's debugValue() is its view (docs/debugging.md,
// "debugValue"), one rule per instance; errors gets each one the debugger cannot compute, a compile
// error FILE:LINE:COLUMN: error: ... (problems are warnings: a library rule left out).
std::string makeNatvis(const std::vector<ParsedFile*>& files, GlobalScope& global, const SymbolTable& symbols, const std::vector<DebugClass>& classes,
                       std::vector<std::string>& problems, std::vector<std::string>& errors);
