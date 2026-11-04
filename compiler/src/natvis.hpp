#pragma once
// Visual Studio's view of the standard library's values (docs/debugging.md, "Values in Visual
// Studio"): a .natvis that shows a String as its text, a Vector as its elements, a HashMap as its
// entries. kppc writes it beside the executable of a --debug-info build and links it into the PDB
// (/NATVIS), so the debugger loads it with the program. The rules name the library's members; each
// is checked against the library's classes as they are, and one whose member is gone is left out
// with a problem (tests\natvis.ps1 fails on any).
#include <string>
#include <vector>

struct ParsedFile;
class GlobalScope;
class SymbolTable;

// The .natvis for a program's files (the library's among them) and its class instances.
std::string makeNatvis(const std::vector<ParsedFile*>& files, GlobalScope& global, const SymbolTable& symbols, std::vector<std::string>& problems);

// A type's name in the debug information and the natvis: kppc's (TypeTable::toString, qualified)
// with closing angle brackets kept apart, `RbNode<Pair<String, int32> >`, as MSVC's PDBs write
// them. Visual Studio's expression evaluator reads `>>` as a shift, so it could not name, cast to or
// match a natvis rule to a type whose name ends two template argument lists at once.
std::string debuggerTypeName(std::string name);

// A class's name in the debug information and the natvis: debuggerTypeName of its qualified name,
// or `Item^` for the library's UniquePtr<Item> with its default deleter (design.md 20), as the
// program writes it and errors print it (TypeTable::toString's unqualified form). The PDB's type
// name is what Visual Studio's Watch and Locals show as the type.
class TypeTable;
struct Ty;
std::string debuggerClassName(const TypeTable& types, const Ty* t, const SymbolTable& symbols);
