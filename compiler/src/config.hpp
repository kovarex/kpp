#pragma once
#include "lexer.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// A config:: constant declared in source with its default (design.md, "config:: defaults in source",
// 2026-11-04): `config bool finalRelease = false;` at a file's top level, `config i32 level;` with
// none (required). kpp takes the defaults before it parses a file (the parser's declaration-scope
// if constexpr, when and requires read config:: values), and kpp takes a library's as its options.
struct ConfigDeclaration
{
  std::string name;        // as declared; a scoped library's caller makes it LIB::NAME
  std::string type;        // as written: bool, unsigned int, i32, StringView, Platform, zlib::Mode
  std::string kind;        // "bool", "int", "string" or "enum"
  std::string value;       // the default as --config spells it (true, -3, fast, Fast); empty when required
  bool required = false;   // no default: something must set it
  std::string description; // the // comment lines right above it, joined by blanks
  uint32_t line = 0, column = 0;
};

struct ConfigError
{
  uint32_t line = 0, column = 0;
  std::string message;
};

// Whether a file's text may hold a config declaration: a whole word `config` whose next non-blank
// characters are not `::`. It never misses one (a comment or a string only adds candidates); the
// tokens decide (collectConfigDeclarations).
bool mayDeclareConfig(std::string_view text);

// The config declarations among a .kpp file's tokens: `config` starting a declaration at the file's
// top level (outside every brace, first in the file or after a `;` or `}`), not followed by `::`. A
// malformed one is an error, and so is one that is not the first token on its line.
void collectConfigDeclarations(const std::vector<Token>& tokens, std::string_view text, std::vector<ConfigDeclaration>& out,
                               std::vector<ConfigError>& errors);

// mayDeclareConfig, then lexing (a table of its own) and collectConfigDeclarations: what kpp reads a
// file's declarations with, a project's build and a compile alike.
void findConfigDeclarations(std::string_view text, std::vector<ConfigDeclaration>& out, std::vector<ConfigError>& errors);

// Takes the well-formed config declarations out of a .kpp file's tokens before it is parsed: they are
// read by collectConfigDeclarations, and the parser never sees one (a `config` it meets is an error).
void dropConfigDeclarations(std::vector<Token>& tokens, std::string_view text);
