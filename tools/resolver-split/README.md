# The resolver split (2026-11-05)

kovarex asked for resolver.cpp (28.5k lines) to be split, one class per file as Factorio has it.
Stage 1 (452414f0) moved GlobalScope::Impl's body into compiler/src/Resolver/*.inl, included where
the code stood. Stage 2 makes each part a translation unit of its own, mechanically:

- `make_header.py` (run once, with Walker, 2026-11-05) made Resolver/GlobalScopeImpl.hpp from
  resolver.cpp's prologue and the class: file-scope statics became `inline` (one object for every
  translation unit), the anonymous namespaces `resolver_detail`. resolver.cpp keeps what resolver.hpp
  declares (ResolveStats::add, stringLiteralArray, whyReport) and GlobalScope.inl.
- `inl2cpp.py IN.inl CLASS_LINE QUALIFIER INDENT OUT.hpp OUT.cpp PRELUDE` turns a class's inline
  member functions into declarations in OUT.hpp and `auto QUALIFIER::f(params) const -> R { ... }`
  definitions in OUT.cpp (default arguments only in the declaration; `static`, `virtual`, `override`
  dropped from the definition). Kept inline, untouched: templates, constexpr functions, deduced `auto`
  returns, one-line bodies, data members, nested types.

To convert a part: run inl2cpp.py on it, change its `#include "X.inl"` in GlobalScopeImpl.hpp to
`X.hpp`, add `src/Resolver/X.cpp` to compiler/CMakeLists.txt, build, and check that the game's IR is
byte-identical with the kppc before (both with one `--std`: the standard library's path is in the
program's strings) and that compiler\tests\gate.cmd passes.

Impl's own parts (Indexer, Lookup, Interpreter and the others) are pieces of Impl's body, their members
at two spaces: `inl2cpp.py Resolver/Lookup.inl - GlobalScope::Impl "  " Resolver/Lookup.hpp Resolver/Lookup.cpp PRELUDE`.
