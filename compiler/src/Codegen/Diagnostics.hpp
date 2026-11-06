// struct Emitter's members: errors, failures and their places: the declarations, part of the
// struct's body, included in it in Codegen/Emitter.hpp; the definitions in Diagnostics.cpp
// (codegen.cpp split, stage 2, 2026-11-05, moved without a change).
    // --- diagnostics ---

    std::unordered_map<const ParsedFile*, LineStarts> lineStarts; // per file: where its lines start

    // shown: false for a failure that follows from an error the checker reported (fail): the
    // checker's error is the build's, and the function fails as it would with this one.
    void error(Node* at, const std::string& message, bool shown = true);

    // "file:line" of a node in a file ("file:line:column" with --diagnostic-columns when column),
    // "?" when unknown.
    std::string locationOf(Node* at, ParsedFile* file, bool column = false);

    // "file:line" of a node, written into the program itself (what --stub-missing's stub says): the
    // same in every build, whatever form its diagnostics take (--msvc-diagnostics, a compile server's
    // marked places, source.hpp).
    std::string programLocationOf(Node* at, ParsedFile* file);

    // The line a node starts on in its file (from 1), 0 when unknown.
    size_t lineOf(Node* at, ParsedFile* file);

    struct Unsupported
    {
    };
    std::string lastFailure; // the message of the last fail(), without its place (a failed layout or global repeats it at every use)
    bool lastFailureShown = true; // and whether it was shown (fail's shown)
    // An error that abandons what is being emitted (the function, layout or global it is in).
    // shown: false when the failure follows from a checker error: a name or a type the checker
    // reported as not declared (followsUndeclared), which the build fails with already.
    [[noreturn]] void fail(Node* at, const std::string& message, bool shown = true);
    // Whether node at is an expression that holds an error the checker reported: one at a token from
    // its first to its last (its subtree's), in the file being emitted. Only an expression (a call,
    // an argument, a conversion): a failure on a declaration (a local array whose length the checker
    // found no constant) is the code generator's own consequence, and stays shown.
    // tokensAfter: tokens past its last that are its own too (a member's name, after the `.`).
    bool holdsCheckerError(Node* at, uint32_t tokensAfter = 0);
    // A construct the code generator cannot translate yet.
    [[noreturn]] void unsupported(Node* at, const std::string& what, bool shown = true) { fail(at, "codegen does not support " + what + " yet", shown); }
    // Whether t (through references and pointers) is an undeclared type's placeholder class the
    // checker reported where it is written: what fails on it follows from that error.
    bool followsUndeclared(const Ty* t);

