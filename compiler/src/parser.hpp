#pragma once
#include "Ast/Ast.hpp"
#include <cstdint>
#include <map>
#include <string>
#include "lexer.hpp"
#include <string_view>

// Parses one lexed file into out. Tokens may be modified (`>>` is split into two `>` when it
// closes nested template argument lists).
// Build configuration constants (`config::name`). The parser evaluates conditions made of them
// for `if constexpr` at declaration scope and `requires` at the top of a file.
struct ConfigValue
{
  bool isNumber = true; // false: an enumerator or string, compared by its spelling
  int64_t number = 0;
  std::string text;
};
using ConfigValues = std::map<std::string, ConfigValue>;

// What a file is written in, which the parser is told (never guesses):
// - KppSource: a .kpp file: no C macro idioms; `config` declarations and `test name { ... }` (`T^`
//   and `when` are taken in C++ too).
// - Cpp: C++ as the converter reads it, and a C++ code base parsed as a corpus (the front end's
//   benchmark): its macro idioms are recognised.
// - C: preprocessed C (an imported header): every declaration has C linkage, GNU attributes and C99
//   qualifiers are dropped, `(Name)` is a cast to a typedef name.
enum class Dialect : uint8_t { KppSource, Cpp, C };

// A source file's dialect by its name, for the driver: .kpp files are kpp, the rest C++.
inline Dialect dialectOfPath(std::string_view path) { return path.ends_with(".kpp") ? Dialect::KppSource : Dialect::Cpp; }

void parseFile(std::vector<Token>& tokens, std::string_view text, SymbolTable& symbols, ParsedFile& out, Dialect dialect,
               const ConfigValues* config = nullptr);
