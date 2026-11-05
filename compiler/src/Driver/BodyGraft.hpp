// The driver: a compile server's incremental builds: bodies and functions grafted into the kept
// build (steps 3b and 4); the declarations, in namespace driver (Driver.hpp includes this), the
// definitions in BodyGraft.cpp (main.cpp split, stage 2, 2026-11-05, moved without a change).
  // ---- Step 3b: a request whose files changed only inside function bodies (docs/compile-speed.md) ----


  // A token's identity for comparing two versions of a file: its kind, and its symbol or spelling.
  bool sameToken(const Token& a, std::string_view ta, const Token& b, std::string_view tb);


  // The bodies of the functions decls declare (members of namespaces, classes and enums included).
  // False when one cannot be told apart (a body that is no `{...}`).
  bool collectBodies(Span<Decl*> decls, const std::vector<Token>& tokens, bool inTemplate, std::vector<Body>& out);


  // One changed file of the kept build, checked to have changed inside eligible function bodies
  // only: its new text and tokens, and which bodies changed (indexes into the paired body lists).
  struct ChangedFile
  {
    size_t index = 0;
    std::string text;
    LexResult lexed;              // the new text's, as the lexer gave them
    std::vector<Token> tokens;    // as the parser left them (filtered, `>>` split)
    std::unique_ptr<ParsedFile> parsed;
    std::vector<Body> oldBodies, newBodies;
    std::vector<size_t> changed;
  };


  // Whether a file's parse errors are all inside function bodies (bodies, sorted by their opening
  // brace), each of which the parser ended at its closing brace: the parser recovers inside a block
  // up to its end, so the declarations around such a body are what the tokens around it say, as in
  // a file without the error. A body that does not parse (a line half typed) is grafted with what
  // the parser recovered: its errors are the build's (no code is generated), the rest of the file
  // stays the kept tree.
  bool errorsInBodies(const ParsedFile& parsed, const std::vector<Body>& bodies);


  bool bodiesOnly(Build& kept, size_t i, std::string text, ChangedFile& out, std::string& why);


  // Where each of a file's old tokens is in its new tokens (UINT32_MAX: gone), by a diff of the two
  // token streams (Myers', on the stretch between the common prefix and suffix). False when the
  // versions differ too much for a diff worth keeping.
  bool tokenMap(const std::vector<Token>& a, std::string_view ta, const std::vector<Token>& b, std::string_view tb, std::vector<uint32_t>& map);


  // A function a file declares outside any body, with the declaration it is a member of (null at
  // the file's top level) and whether a template encloses it.
  struct DeclaredFunction
  {
    FunctionDecl* f;
    Decl* container;
    bool templated;
  };


  // A class's data member (step 4's layout edits), with its class and whether a template encloses it.
  struct DeclaredMember
  {
    VariableDecl* v;
    ClassDecl* container;
    bool templated;
  };


  void collectFunctions(Span<Decl*> decls, Decl* container, bool inTemplate, std::vector<DeclaredFunction>& out,
                        std::vector<Decl*>& containers, std::vector<DeclaredMember>* members = nullptr);


  // Points every node of a kept file at its new tokens: its declarations' nodes, and the copies
  // instances made of them (they index the file's tokens too); skip: nodes dropped with the old
  // version, staleSources: the declarations whose instances are copied anew (their old copies
  // are dropped). False when a node's token has no place in the new version.
  bool remapKeptFile(Build& kept, ParsedFile& file, const std::vector<uint32_t>& map, const std::unordered_set<Node*>& skip,
                     const std::unordered_set<const Decl*>& staleSources, const ParsedFile& fresh);


  // Moves the new bodies into the kept declarations. The file takes its new tokens and text: every
  // node it keeps is pointed at the new tokens (through a diff of the two versions), and each
  // changed function takes its new body from the new parse, whose arena the kept build keeps. The
  // declaration objects, which the rest of the program points at, stay. False (the kept build is
  // then unusable) when a new body needs what only indexing the whole file gives (a `(name) op x`
  // cast to rewrite) or a kept node's token has no place in the new version.
  bool graftBodies(Build& kept, ChangedFile& c, std::string& why);


  // ---- Step 4, first kind: functions added, removed or changed (docs/compile-speed.md) ----

  // A file whose changes are whole functions (added, removed, or changed and so both): which, and
  // where each added one goes among the kept declarations.
  struct FunctionChange
  {
    size_t index = 0;
    std::string text;
    LexResult lexed;
    std::vector<Token> tokens;
    std::unique_ptr<ParsedFile> parsed;
    std::vector<uint32_t> map;                          // the kept tokens' places in the new ones
    std::vector<std::pair<FunctionDecl*, Decl*>> removed; // kept functions, with the kept declaration they are members of
    std::vector<std::pair<FunctionDecl*, Decl*>> added; // new functions, with the kept declaration they belong to
    std::vector<std::pair<VariableDecl*, ClassDecl*>> removedMembers; // kept data members, with their kept class
    std::vector<std::pair<VariableDecl*, ClassDecl*>> addedMembers;   // new data members, with the kept class they go into
  };


  // One past a data member's last token, its `;` included (the parser ends the declaration before it).
  uint32_t memberEnd(const VariableDecl* v, const std::vector<Token>& tokens);


  // Whether a data member can come, go or change with what reads it found (its name's readers, its
  // class's member list's, its declared type's): one of a plain class's (no template, no union, no
  // typed union, no lambda's closure), not static (a global), no alias or friend. (A declaration of
  // several declarators comes or goes whole: functionsOnly pairs them a declaration at a time.)
  bool plainDataMember(const DeclaredMember& d);


  // Whether a function can come or go without anything but its name's readers seeing it: no
  // template, no virtual or special member (layouts, vtables and traits read those without a
  // lookup), not constexpr, no deduced return type, no class or enumeration in its body.
  bool plainFunction(const DeclaredFunction& d, const std::vector<Token>& tokens);


  bool functionsOnly(Build& kept, size_t i, std::string text, FunctionChange& out, std::string& why);


  // The list of a declaration's members (or the file's top level) with the removed functions taken
  // out and the added ones put in, in the order of their tokens.
  Span<Decl*> withFunctions(Span<Decl*> list, const std::unordered_set<const Decl*>& removed, const std::vector<Decl*>& added,
                            Arena& arena);


  // Takes the removed functions out of the kept file and its index, points what it keeps at the
  // new tokens, and grafts the added functions in (indexed, attached as finishIndex attaches them).
  // False (the kept build is then unusable) when that needs what only a whole index gives.
  bool graftFunctions(Build& kept, FunctionChange& c, std::string& why);


  // The kept build again with the files' new texts, when they changed inside eligible function
  // bodies only: those bodies parsed and checked again and nothing else, then the reports and
  // codegen as a build from scratch runs them. False when the request is no such build: it is built
  // from scratch then (keptChanged: the kept build was changed on the way, and is dropped).
  // headersSame: the C import's headers are as the kept build read them (lastImportStamps).
  bool incrementalBuild(Build& kept, std::vector<SourceFile>& files, double collectTime, bool headersSame, bool& keptChanged);
