#pragma once
// The syntax tree: every node kind, and a file's parse (ParsedFile). Nodes live in a per-file arena
// and are never freed individually.
#include "Node.hpp"
#include "Types.hpp"
#include "Declarations.hpp"
#include "Statements.hpp"
#include "Expressions.hpp"

struct Diagnostic
{
  uint32_t token;
  const char* message;
};

#include "ParsedFile.hpp"

// What tells apart the entities two files declare private under one name (design.md 15), put
// before a qualified name (a symbol's, a type's in the IR): "<file name>@<its file's tag>::" for a
// private top-level declaration and what is declared in it, else nothing. The tag is the one that
// names the file's lambdas (ParsedFile::lambdaTag: a hash of its name under its root, unique in the
// program), so the names are the same wherever the sources are (in the IR, the PDB, the modules'
// hashes); a file without one (the converter's parses) takes the hash of its path.
inline std::string filePrivatePrefix(const Decl* d)
{
  const Decl* top = d;
  while (top && top->parent && !(top->file && top->parent == (const Decl*)top->file->libraryNamespace))
    top = top->parent;
  if (!top || !top->filePrivate || !top->file)
    return std::string();
  const std::string& path = top->file->path;
  uint32_t hash = top->file->lambdaTag;
  if (hash == 0)
  {
    hash = 2166136261u;
    for (unsigned char c : path)
      hash = (hash ^ c) * 16777619u;
  }
  static const char digits[] = "0123456789abcdef";
  std::string tag = path.substr(path.find_last_of("/\\") + 1) + "@";
  for (int shift = 28; shift >= 0; shift -= 4)
    tag += digits[(hash >> shift) & 15];
  return tag + "::";
}

// A numbered class's or enumeration's number with its file's tag, as a name carries it (`f::Local@6c1f0e2a_3`, `Flag<int>/spec6c1f0e2a_7`;
// ClassDecl::number): the same wherever the declaration is written in its file, and unique in the program (the tag is the
// one that names the file's lambdas). A declaration the parser gave no number: its token.
inline std::string numberInFile(const Decl* d)
{
  uint32_t number = numberOf(d);
  if (number == UINT32_MAX)
    return std::to_string(d->token);
  uint32_t tag = d->file ? (d->file->lambdaTag ? d->file->lambdaTag : d->file->index) : 0;
  static const char digits[] = "0123456789abcdef";
  std::string text;
  for (int shift = 28; shift >= 0; shift -= 4)
    text += digits[(tag >> shift) & 15];
  return text + "_" + std::to_string(number);
}
