// The driver: what one build made, kept by a compile server; the declarations, in namespace driver
// (Driver.hpp includes this), the definitions in Build.cpp (main.cpp split, stage 2, 2026-11-05,
// moved without a change).
  double seconds(std::chrono::steady_clock::duration d);
  inline int codegenExit = 0;
  inline std::string lastImportStamps;
 // the C import's header stamps (importCHeaders), for a server's next build
  inline std::string lastImportsLine;
  // the imports report of the build running (importCHeaders)

  // A function's body in a file's tokens: its `{` and the matching `}`; eligible when what depends
  // on it outside it can be found: not when a return type is deduced from it (callers elsewhere are
  // typed by it).
  struct Body
  {
    FunctionDecl* f;
    uint32_t open, close;
    bool eligible;
    bool templated; // a template's or a class template's: its instances are checked again (step 4)
    bool constant;  // constexpr or consteval: what the evaluator computed by running it is found (step 4)
  };

  // A config:: constant declared in source (config.cpp): its declaration and its file.
  struct ConfigDeclared
  {
    ConfigDeclaration declaration;
    std::string path;
  };


  // Everything one build made, from the files read to the resolved program. A compile server keeps
  // it for the next request: a request that changes only function bodies is built from it, the
  // changed bodies checked again and nothing else (docs/compile-speed.md, step 3b).
  struct Build
  {
    Options options;
    std::vector<SourceFile> files;
    std::vector<LexResult> lexed;
    std::unique_ptr<SymbolTable> symbols;
    std::unique_ptr<SourceConfiguration> configuration;
    std::vector<std::unique_ptr<ParsedFile>> parsed;
    std::unique_ptr<GlobalScope> global;
    std::vector<ResolveStats> stats;
    std::vector<int> fileLibrary; // (files[i]'s: its options.libraryRoots entry, -1 for the program's; the files the driver adds are the program's)
    std::vector<uint32_t> lambdaTags; // (files[i]'s: what names its lambdas' closures, ParsedFile::lambdaTag; lambdaTags)
    std::vector<PackageOf> packages; // --packages: files[i]'s code package (packages.hpp)
    // What the reports print that a later build of the kept program cannot recompute.
    std::string importsLine, indexLine;
    std::map<std::string, ConfigDeclared> configDeclared; // the config:: constants its sources declare (configDeclarations)
    double indexTime = 0, finishTime = 0; // (the index line again, when a later build changed the entries)
    size_t indexScopes = 0, indexEntries = 0; // as the index left them (the resolve adds instances' after)
    std::vector<std::string> clashes;
    size_t parseErrors = 0; // in the program's files (not the C import's): the build stops before codegen
    double resolveTime = 0;
    GlobalScope::InstantiationStats instantiation{}; // at the end of the resolve (codegen instantiates more)
    ResolveStats instanceStats;
    bool keepable = false; // the whole pipeline ran as one build of the files (not --repeat, --convert, --dump)
    // The parses later versions of files were taken from (step 3b): their arenas hold the nodes
    // grafted into the kept files.
    std::vector<std::unique_ptr<ParsedFile>> grafts;
    // Codegen's kept state (step 5b) and what the next build changes for it; each module's size in
    // bytes as last built (the IR report counts the modules rebuildLLVM did not assemble again).
    // Declared last: destroyed first, before the program it points into.
    std::vector<size_t> moduleBytes;
    std::vector<FunctionDecl*> changedBodies, addedFunctions, removedFunctions;
    // The classes whose data members an incremental build changed (step 4's layout edits): codegen
    // lays them out again (rebuildLLVM).
    std::vector<const Decl*> changedLayouts;
    // The templates of the instances an incremental build checked again or took out (they read a
    // change: recheckInstancesOf): codegen emits their instances again.
    std::vector<const Decl*> instancesOf;
    // The globals whose initializers read a changed or edited name: codegen emits their constructors
    // again (rebuildLLVM).
    std::vector<const Decl*> changedGlobals;
    // The class instances an incremental build took out (no one asks for them now): codegen drops
    // their layouts (rebuildLLVM).
    std::vector<const Ty*> droppedClasses;
    bool rebuildCodegen = false;
    // The build generated no code (parse errors, a clash; reportResolve): codegen's kept session has
    // not seen what it changed, which the next build that generates code emits too (incrementalBuild).
    bool codegenBehind = false;
    // The program's files' stamps (as listed) and their texts' hashes, by the files' order: a compile
    // server's next build does not read a file whose stamp is the same again (compile).
    std::vector<FileStamp> stamps;
    std::vector<uint64_t> textHashes;
    // An incremental build renumbered a kept lambda's closure or a kept class or enumeration (one added or removed above it in
    // its file): the names in the code carry the numbers (`f::Local@6c1f0e2a_3`), so codegen runs in full (its session keeps
    // the types' names). See remapKeptFile.
    bool positionalCodegenInFull = false;
    // --debug-info: the kept functions whose lines an edit moved (emitted again for their line tables, not for their code).
    std::vector<FunctionDecl*> movedLines;
    // The functions whose bodies this build replaced (changedBodies holds their readers too, emitted again with the bodies they
    // had): their lambdas' closures are new types, what was made for the old ones goes (rebuildLLVM).
    std::vector<FunctionDecl*> replacedBodies;
    CodegenSessionPtr codegen;
    // The session a codegen in full replaced: freed once the build has answered its client
    // (incrementalBuild), not on the build's own time (about 1 s on the game).
    CodegenSessionPtr retiredCodegen;
  };
  inline std::mutex keptMutex;
              // held by a server's build until it has kept its result
  inline std::unique_ptr<Build> keptBuild;
  // a server's last build (under keptMutex)
