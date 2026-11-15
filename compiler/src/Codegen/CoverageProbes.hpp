// struct Emitter's members: test coverage (--coverage, docs/design.md 28): the declarations, part of
// the struct's body, included in it in Codegen/Emitter.hpp; the definitions in CoverageProbes.cpp.
    // --- coverage ---

    // Each function of the program's own files (not the standard library's, not a C header's, not a
    // file kpp adds) gets a byte array, @"kpp.cov.<function>", and probes that set its bytes to 1:
    // - the function's entry (probe 0);
    // - a region: a run of statements control enters only at its first and leaves only after its last
    //   (a new one after a call, which may throw, and at every block: a branch, a loop body, a case);
    // - a condition: two probes, it was true and it was false; the condition of an if, a loop and a ?:
    //   (so an if's missing else is its false), and each operand of && and || (an operand never
    //   evaluated has neither);
    // - a switch's missing default.
    // Its region map, @"kpp.covmap.<function>", is text: the file (as the program names it), the
    // hash of the file's text, the function's name and line, then a line for each probe in order
    // (`F l c l c`, `R`, `C l c l c` for two, `D l c l c`) and one for each statement (`S probe l c l
    // c`): a place is its first line and column and its last line and the column after its last
    // character, from 1. Module 0's kpp_coverage_entries lists every function's array, size and map
    // for the runner (<coverage>.kpp, compiler/src/coverage.cpp).
    std::unordered_map<const ParsedFile*, std::string> coverHashes; // a file's text hash, as the map says it

    // Whether the current function's code gets probes (sh.coverage, a file of the program's own):
    // sets coverActive and emits the entry probe. at: where the function is.
    void coverBegin(Node* at, const std::string& name = std::string());
    // An && or || (not a user's operator): its operands are conditions, itself is not one more.
    static bool coverageIsLogical(Node* n);
    // Before a statement's code: a probe when it starts a region, and its place in the map.
    void coverStatement(Stmt* s);
    // After a statement's code, which began at fn's size `from`: a call in it ends the region.
    void coverAfter(size_t from);
    // A condition's value (i1) just computed: its true or its false probe set.
    void coverCondition(Node* e, const std::string& value);
    // A switch with no default: its probe, at the block the switch goes to for every other value.
    void coverMissingDefault(Node* sw);
    // The function's array and map, defined once (decls) and listed for the table: at its end.
    void coverFinish();
    // Module 0's kpp_coverage_entries(count): the table of every function's array.
    std::string coverageTableText();

    std::string coverArrayName() const;
    std::string coverProbeAt(size_t probe) const;
    size_t coverProbe(char kind, const std::string& place);
    // Sets the byte at address to 1 when it is 0 (a branch around the store, weighted as rarely taken).
    void coverSet(const std::string& address);
    static constexpr int coverWeightsId = 999999980; // the branch weights' metadata (coverageTableText defines it)
    // `l c l c` of tokens first..last of the current file (the end: the column after last's text).
    std::string coverPlace(uint32_t first, uint32_t last);
    // The tokens a node's text spans: its first and its last (the closing brackets of what it opens
    // included).
    std::pair<uint32_t, uint32_t> coverTokens(Node* n);
    // A statement's: a simple one to its `;`, a compound one's head (`if (c)`, `while (c)`).
    std::pair<uint32_t, uint32_t> coverStatementTokens(Stmt* s);
