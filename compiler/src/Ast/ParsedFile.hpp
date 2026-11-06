#pragma once
// A file's parse (Ast.hpp includes it, after the nodes it holds).

struct ParsedFile
{
  struct NamespaceDecl* libraryNamespace = nullptr;

  Arena arena;
  Span<Decl*> decls;
  const std::vector<Token>* tokens = nullptr; // Set by parseFile; the driver keeps the vector alive.
  std::string_view text;
  std::string path; // Set by the driver, for diagnostics.
  bool isImported = false; // Preprocessed C declarations, not user code.
  // The namespace-scope declarations a false `when` dropped (design.md 19), each with its parent: not
  // declarations of the program, but names a lookup stops at (the resolver's index, Scope::absent).
  std::vector<Decl*> absentDecls;
  bool droppedWhen = false; // a false `when` dropped a declaration in it, at namespace or class scope
  // The tokens [from, to) the parser dropped for this configuration: a false `when` at namespace or
  // class scope, `when` included, and a declaration-scope `if constexpr` branch not taken, braces
  // included (the language server shows them as inactive: docs/editors.md, "Inactive code"). In the
  // order the parser finished them, not sorted, and they may nest (a class's false member inside a
  // false namespace `when` comes first): sort and merge before searching them. Not in them: a local's
  // `when` (the walk decides it: Decl::whenKnown), a class template's member decided per instance, a
  // `when` whose declaration did not parse, and the `if constexpr (...)` and `else` tokens. A compile
  // server's graft assigns the new text's ranges (they index its tokens).
  std::vector<std::pair<uint32_t, uint32_t>> droppedTokens;
  // Each [[deprecated]] declaration of the file with the token of its reason's string literal
  // (UINT32_MAX: none written), which a call's warning quotes (Walker::noteDeprecatedCall).
  std::vector<std::pair<const struct Decl*, uint32_t>> deprecations;
  bool isLibrary = false;  // The standard library's sources (set by the driver): the top-level names they claim are reserved.
  Scope* privateScope = nullptr; // Its `private` top-level names (design.md 15), set by the resolver's index.
  // The library it is a file of (design.md 16; set by the driver): its name, empty for the program's
  // files and the standard library's; for a scoped library also the name's symbol, and the namespace
  // of that name its top-level declarations are in (the resolver's index makes it, their parent).
  std::string library;
  uint32_t libraryScope = UINT32_MAX;
  uint32_t index = 0;      // Position in the driver's file list (the library first): declaration order across files.
  // What names its lambdas' closures apart from other files' (`__lambda_<tag>_<n>`, the parser's), and
  // its private entities (filePrivatePrefix, `inflate.kpp@<tag>::helper`): a hash of the file's name
  // under its root, so that those names (in the IR, the PDB, a module's hash) are the same wherever
  // the sources are and whatever files come before it (main.cpp, lambdaTags). 0, unset: the index
  // names the closures, the path's hash the private entities.
  uint32_t lambdaTag = 0;
  std::vector<Diagnostic> diagnostics;
  // The struct rule's breaks in it (design.md "struct and class": a struct is a public value type),
  // which the resolver reports as errors under kppc --struct-rule.
  std::vector<Diagnostic> structRule;
  // `(HANDLE) -1`, `(T) *p`, parsed as `(name) op x`, with the declaration it is in: a cast when the
  // name is a type, rewritten as one by the resolver's finishIndex.
  std::vector<std::pair<BinaryExpr*, Decl*>> castCandidates;
  // Declarations that hold only some of a function's default arguments: a default before a parameter
  // without one, or any on an out-of-line definition (GlobalScope::mergeDefaultArguments).
  std::vector<FunctionDecl*> splitDefaults;
  // `template struct Registry<int>;`: the class an explicit instantiation names, with the
  // declaration it is written in. It declares nothing; the class's static data members are built
  // at startup (GlobalScope::explicitInstantiationsOf).
  std::vector<std::pair<NamedType*, Decl*>> explicitInstantiations;
  // Its test suites (design.md 8.7; parsed only in a --tests build), for the checks across files
  // (driver::checkTestSuites): two suites of one name, two tests of one name in a suite, a depends
  // naming no suite, a cycle. Each with its name's token, its dependencies' and its tests' names
  // with their tokens.
  struct TestSuite
  {
    uint32_t name = UINT32_MAX, token = 0;
    std::vector<std::pair<uint32_t, uint32_t>> depends, tests;
  };
  std::vector<TestSuite> testSuites;
  uint32_t declCount = 0; // All declaration nodes created.
  uint32_t functionBodies = 0;
  uint32_t nodeCount = 0;
  // A compile server's kept file whose bodies were replaced (step 3b): the arena holds the old
  // bodies too, so the parse statistics report what a parse of the file as it is now allocates.
  size_t arenaBytesReported = 0;
};
