// struct Emitter's members: functions' names and signatures: the declarations, part of the struct's
// body, included in it in Codegen/Emitter.hpp; the definitions in Functions.cpp (codegen.cpp split,
// stage 2, 2026-11-05, moved without a change).
    // --- functions ---

    // IR name of a function (for an instantiation: with the class and template arguments),
    // computed once per function, class and arguments.
    struct NameKey
    {
      FunctionDecl* f;
      const Ty* classTy;
      std::vector<std::pair<Decl*, const Ty*>> args;
      bool operator==(const NameKey&) const = default;
    };
    struct NameKeyHash
    {
      size_t operator()(const NameKey& k) const
      {
        size_t h = std::hash<const void*>()(k.f) * 31 + std::hash<const void*>()(k.classTy);
        for (auto& [d, t] : k.args)
          h = (h * 1000003) ^ (std::hash<const void*>()(d) * 31 + std::hash<const void*>()(t));
        return h;
      }
    };
    std::unordered_map<NameKey, std::string, NameKeyHash> functionNames;

    const std::string& functionName(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy);

    // q, the qualified name of a member d, with the arguments written for the full specialisation
    // owning d (`template<> struct Limits<int>`) before the member's own name, so that the members
    // of two specialisations of one template differ (`Flag<true>::value`, `Flag<false>::value`).
    // Unchanged for a member of any other class.
    std::string withSpecialisationArgs(std::string q, Decl* d);

    std::string computeFunctionName(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy);

    // The function as an error's `(in ...)` names it (currentContext): written as errors write types
    // (types.toString: `Item^`, `const Vector<int32>&`), its class with the instance's arguments, a
    // function template's arguments, its parameters and a const method's `const`, so that two
    // instances or overloads stay two texts (the checker's errors are a set). computeFunctionName's
    // structure, with the program's own spelling instead of the IR's.
    std::string displayFunctionName(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy);

    struct Signature
    {
      std::string returnType;
      std::vector<std::string> paramTypes;
      std::vector<const Ty*> paramTys; // null for this/sret
      bool hasThis = false, hasSret = false, variadic = false;
      const char* resultExtension = ""; // " signext" or " zeroext" for a narrow result (narrowExtension)
    };

    // A lambda's call operator's signature, as emitLambda writes its header and a caller declares it
    // (callClosure): the closure, the result's slot when it returns in memory, then the call type's
    // parameters; an unknown result is a pointer, as emitLambda emits its body.
    Signature lambdaSignatureOf(const Ty* callTy, Node* at);

    // `declare RET NAME(TYPES)`, as declareFromHeader reads it from the header of the definition.
    static std::string declarationText(const Signature& s, const std::string& name);

    // Whether the declarations of requests are recorded (Shared::requestDeclarations): a debug
    // build's modules, until level 0's barrier.
    bool recordingRequests() const { return !sh.barrierTaken && sh.moduleCount > 1 && sh.importableModules == 0; }

    void recordRequestDeclaration(const std::string& name, std::string declaration);

    // Runs f, a computation for a request's declaration, not part of what is being emitted: when it
    // fails (Unsupported), what fail() recorded is taken back (the error, the erroring mark of the
    // function being emitted, the last failure), and false is returned. The definition then fails
    // the same way where it is emitted, and reports it there.
    template<typename F>
    bool withoutFailing(F&& f)
    {
      size_t errorsBefore = errors.size(), erroringBefore = erroring.size();
      bool unattributedBefore = unattributedErrors, shownBefore = lastFailureShown;
      std::string failureBefore = lastFailure;
      try
      {
        f();
        return true;
      }
      catch (Unsupported&)
      {
        errors.resize(errorsBefore);
        if (erroring.size() != erroringBefore)
          erroring.erase(owner);
        unattributedErrors = unattributedBefore;
        lastFailure = std::move(failureBefore);
        lastFailureShown = shownBefore;
        return false;
      }
    }

