// struct Emitter's members: instantiation requests, the functions a call uses, inlining and calls:
// the declarations, part of the struct's body, included in it in Codegen/Emitter.hpp; the
// definitions in Calls.cpp (codegen.cpp split, stage 2, 2026-11-05, moved without a change).
    // An instantiation requested through a longer chain of instantiations is an error: a template
    // that instantiates itself with ever larger arguments would otherwise never stop, its names
    // growing with every step. The whole Factorio source needs 12. See docs/design.md.
    static constexpr unsigned maxInstantiationDepth = 64;

    // The instantiations this emitter asked for while the current level was emitted, each with
    // its first requester by name: the driver takes the first over all emitters as the
    // instantiation's requester (which the diagnostics name, and whose depth it inherits), the same
    // in every run, where the first to ask depends on timing. Every request counts, cached or not.
    std::unordered_map<std::string, Request> levelRequests;
    void noteRequest(const std::string& name);
    bool lastUseInstantiates = false; // the last useFunctionOnce asked for an instantiation

    // Asks for the instantiation `name`: true for the first request, which queues it.
    bool claimInstantiation(const std::string& name);

    // An instantiation whose chain of requests is longer than the limit is an error, reported
    // instead of emitting it (with its requester as the level settled it, the same in every run).
    void reportTooDeep(const Instantiation& item);

    // The declaration of a function queued now and defined at a later level, as emitFunction will
    // write its header (the same signatureOf, under the same arguments and class, emitFunction's
    // state set for it and restored), for a debug build's program pieces (Shared::requestDeclarations).
    // Only until level 0's barrier: what level 0 names is all a program piece declares. Nothing is
    // recorded when the signature fails here; the definition reports that, and the check at the end
    // finds a name without a declaration.
    void noteRequestDeclaration(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy, const std::string& name);

    void queueInstantiation(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy, const std::string& name);

    // Makes a function usable from the current function: declares external ones, queues
    // instantiations. Returns its IR name.
    struct UsedFunction
    {
      std::string name;
      bool instantiation; // asked for as an instantiation: each use is a request (noteRequest)
    };
    std::unordered_map<std::string, UsedFunction> usedFunctions; // what useFunction answered this emitter (its shared part done)
    // Why f under args (and classTy) is no instantiation a symbol can name: one of its own template
    // parameters, or of the class template it is a member of, is unbound or bound to a type not known
    // or still dependent (`@Bindable::debugChildren<?>(int32)`, undefined at the link). Empty when it is one.
    std::string notAnInstance(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy);

    std::string useFunction(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy, Node* at = nullptr);


    // The arguments of f's instantiation as its definition def names them: a definition separate
    // from the declaration has its own parameter declarations (`template<typename Policy>
    // ScrollBar<Policy>::ScrollBar(...)` names the class's parameters first, then the member's own).
    TemplateArgs definitionArguments(FunctionDecl* f, FunctionDecl* def, const TemplateArgs& args);

    std::string useFunctionOnce(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy);

    static size_t enumeratorCount(Decl* e);

    // The library's enum intrinsics (resolver: intrinsicOf, design.md 14's enum appendTo): the
    // name of the enumerator a value is, through a switch emitted once per enumeration (null for a
    // value that is none), and the enumeration's name.
    Value enumIntrinsic(int which, CallExpr* c);

    // The generated `const char* str() const` of a kpp enum: a switch over the enumerators.
    void emitEnumStr(FunctionDecl* f, const std::string& name);

    // The class a friend function defined in a class (a hidden friend, a static member to the
    // compiler) is called through: for one of a class template, the instance among the arguments'
    // classes whose function it is (its arguments are the function's); null for any other function.
    const Ty* friendClass(FunctionDecl* f, const std::vector<const Ty*>& argTypes, Node* at);

    // Template arguments for calling f on an object of classTy, with the call's deduced arguments.
    TemplateArgs calleeArgs(FunctionDecl* f, const Ty* classTy, CallExpr* call, Node* at);

    // Emits a call. thisArg/sret may be empty. For class returns the result is the sret address.
    // `kpp_atomic_add32(p, v)` and friends (stdlib/core/Thread.kpp): atomic instructions, not calls.
    std::optional<Value> atomicBuiltin(FunctionDecl* f, Span<Expr*> callArgs, Node* at);

    // The library's `move` and `forward` called with their one argument: identity casts, a reference
    // in and the same reference out, so the call is the argument's address as the parameter would
    // receive it (the same conversions and temporaries) without the call (docs/compile-speed.md,
    // "Design: inlining trivial functions in kppc", step 1). Null for any other call.
    std::optional<Value> inlinedIdentity(FunctionDecl* f, const TemplateArgs& args, Span<Expr*> callArgs, Node* at);

    std::unordered_map<FunctionDecl*, FunctionDecl*> trivialDefinitions; // a function's typed definition -> it, when trivial (or null)
    std::unordered_set<FunctionDecl*> inlining;                            // the definitions being emitted in a caller now

    // Whether a value of type t is a scalar, a pointer or a reference: what a trivial function
    // takes and gives (a class by value needs a copy and a destruction; a class result a slot).
    bool inlineScalar(const Ty* t);

    // Whether e may stand in a trivial function's body: its parameters, `this` and fields reached
    // from them, constants and enumerators, combined by built-in operators on scalars and pointers,
    // built-in subscripts and casts. No call, no overloaded operator, nothing that makes a temporary
    // to destroy or can throw; a parameter is only read.
    bool trivialExpression(Expr* e, FunctionDecl* def, int depth = 0);

    // The typed definition of the call's callee when it is trivial: one statement, `return E;` or
    // `E;`, with E a trivialExpression, the parameters and the result scalars, pointers or
    // references. Decided once per definition (it depends on the definition alone). Null otherwise.
    // written: set to the definition the body is written in (an instance's template's).
    FunctionDecl* trivialDefinition(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy, FunctionDecl** written = nullptr);

    // The typed definition of a constructor call's callee when it is trivial: its body empty, its
    // parameters scalars, pointers or references, and each of its member initializers giving a
    // data member of the class at most one value, a trivialExpression; the class's data members
    // scalars or pointers, each that it does not initialize with a trivial default member
    // initializer or none, and no base, virtual function or union (NamedBool's, ID's). Decided
    // once per definition. Null otherwise.
    // written: as trivialDefinition's.
    FunctionDecl* trivialConstructor(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy, Node* at, FunctionDecl** written = nullptr);

    // A slot of this function for what an inlined call binds (inlineCall): one for each inlining
    // depth, binding and type, shared by every call inlined at that depth.
    std::string inlineSlot(const std::string& key, const std::string& type);

    // Runs emit as the body of a call of def, the typed definition of a trivial callee whose
    // signature is s: the arguments as the call passes them, in order, bound to the definition's
    // parameters as its own prologue binds them (in inlineSlots), `this` likewise, and emit run
    // under the callee's arguments, class and file (an error in it is the callee's line). The
    // bindings and the state are restored however emit ends. written: the definition the body is
    // written in (an instance's template's), which the function being emitted depends on from here.
    template<typename F>
    void inlineCall(FunctionDecl* def, const FunctionDecl* written, const TemplateArgs& args, const Ty* classTy, const Signature& s,
                    const std::string& thisArg, Span<Expr*> callArgs, Node* at, F&& emit)
    {
      // A callee whose result is returned in memory is never inlined (inlinedBody and
      // trivialDefinition refuse one before here): its named return variable (design.md 23) is
      // built in the caller's destination, which these bindings of parameters do not make.
      if (s.hasSret)
        unsupported(at, "an inlined call whose result is returned in memory");
      noteInlined(written);
      size_t first = s.hasThis ? 1 : 0;
      std::vector<std::string> texts;
      for (size_t i = 0; i < callArgs.size(); ++i)
        texts.push_back(argumentText(s.paramTys[first + i], callArgs[i], at));
      struct Restore
      {
        ~Restore()
        {
          e.inlining.erase(def);
          for (auto it = saved.rbegin(); it != saved.rend(); ++it)
            if (it->second)
              e.locals[it->first] = *it->second;
            else
              e.locals.erase(it->first);
          e.currentClassTy = classTy;
          e.initializerOf = initializerOf;
          e.lambdaContext = lambdaContext;
          e.currentFile = file;
        }

        Emitter& e;
        FunctionDecl* def;
        const Ty* classTy;
        Decl* initializerOf;
        Decl* lambdaContext;
        ParsedFile* file;
        std::vector<std::pair<Decl*, std::optional<Local>>> saved;
      } restore{*this, def, currentClassTy, initializerOf, lambdaContext, currentFile, {}};
      auto bind = [&](Decl* d, Local local) {
        auto it = locals.find(d);
        restore.saved.push_back({d, it == locals.end() ? std::nullopt : std::optional<Local>(it->second)});
        locals[d] = local;
      };
      size_t depth = inlining.size();
      if (s.hasThis)
      {
        std::string slot = inlineSlot(std::to_string(depth) + ".this", "ptr");
        fn += "  store ptr " + thisArg + ", ptr " + slot + "\n";
        bind(nullptr, Local{slot, nullptr, false});
      }
      for (size_t i = 0; i < callArgs.size(); ++i)
      {
        const Ty* pt = s.paramTys[first + i];
        bool isRef = pt->isReference();
        std::string type = isRef ? std::string("ptr") : llType(pt, at);
        size_t space = texts[i].find(' ');
        std::string slot = inlineSlot(std::to_string(depth) + "." + std::to_string(i), type);
        fn += "  store " + type + " " + texts[i].substr(space + 1) + ", ptr " + slot + "\n";
        bind(def->params[i], Local{slot, pt, isRef});
      }
      inlining.insert(def);
      ArgsScope argsScope(*this, &args);
      currentClassTy = classTy;
      initializerOf = def;
      lambdaContext = nullptr;
      currentFile = def->file;
      emit();
    }

    // A call of a trivial function (trivialDefinition) emitted as its body (inlineCall), its result
    // as the callee's return statement makes it (docs/compile-speed.md, step 2). Null for any other
    // call.
    std::optional<Value> inlinedBody(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy, const std::string& thisArg, const std::string& sret, Span<Expr*> callArgs, Node* at);

    // A call of a trivial constructor (trivialConstructor) emitted as its member initializers on the
    // object at thisArg (inlineCall), as the constructor initializes them before its empty body
    // (docs/compile-speed.md, step 3). Null for any other call.
    std::optional<Value> inlinedConstructor(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy, const std::string& thisArg, const std::string& sret, Span<Expr*> callArgs, Node* at);

    Value callFunction(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy, const std::string& thisArg, const std::string& sret, Span<Expr*> callArgs, Node* at, const std::string& indirect = "");

    // A member declared `= default`: the memberwise constructor, destructor, assignment or
    // comparison the compiler writes for it, on the object at thisArg.
    Value defaultedCall(FunctionDecl* f, const Ty* classTy, const std::string& thisArg, Span<Expr*> args, Node* at);

    // The memberwise body of a member declared `= default`, on the object at thisArg (and the one
    // at src: the other operand of a copy, move, assignment or comparison).
    Value defaultedOperation(FunctionDecl* f, const Ty* t, const std::string& thisArg, const std::string& src, Node* at);

    // a == b for the objects at those addresses, base by base and member by member.
    Value memberwiseEqual(const std::string& a, const std::string& b, const Ty* t, Node* at);

    // a == b of two parts of objects (members, elements) at those addresses.
    Value partsEqual(const std::string& a, const std::string& b, const Ty* t, Node* at);

    // a == b of two arrays at those addresses, element by element (the first that differs decides).
    Value arraysEqual(const std::string& a, const std::string& b, const Ty* t, Node* at);

    // `x OP y` of the objects at a and b, emitted as that expression would be: the class's operator,
    // a free one, or a built-in one through a conversion (a Direction's to its Enum).
    Value comparedAt(P op, const std::string& a, const std::string& b, const Ty* t, Node* at);

    // a == b for class objects: the class's own operator== unless it is defaulted, else memberwise.
    Value objectsEqual(const std::string& a, const std::string& b, const Ty* t, Node* at);

    // A brace list as a value of a type that is no class (`x = {}`, `return {};`, an argument `{}`):
    // empty, the type's zero (value initialization); one value, that value converted.
    Value braceScalar(BraceInitExpr* list, const Ty* t, Node* at);

    // Whether e is the literal 0, a null pointer constant.
    static bool nullPointerConstant(Expr* e);

    // One call argument as "<type> <operand>", converted to the parameter type (null: variadic).
    std::string argumentText(const Ty* pt, Expr* arg, Node* at);

    // What a local reference to a scalar binds to: the value itself, or, for a const or rvalue
    // reference given a value of another type, a temporary of the referenced type holding the
    // converted value, as C++ binds it and as argumentText binds a parameter (`const double& d =
    // count;`, a `const B* const&` given a `C*`): the value's own storage holds another
    // representation, and a double read through an int's address is 4 bytes of garbage.
    // Whether e names a bit-field (`data.progress` with `uint8_t progress : 4`), to which a reference
    // binds a temporary holding its value.
    static bool namesBitField(Expr* e);
    // The member expression (or member's name) whose bit-field an operand e designates, through
    // parentheses, an assignment, a prefix ++ or -- and a comma's right operand (built-in ones), as
    // GlobalScope's designatedBitField finds it; null for any other operand.
    static Expr* bitFieldDesignator(Expr* e);
    // The name those operators designate when it is no member's (a structured binding's, which
    // bindingBitfield maps to its field), or null.
    static Expr* bindingDesignator(Expr* e);


    std::string scalarReferenceTarget(const Ty* refTy, Expr* value, Node* at);

    // A temporary of class type cls built from a non-class value through a converting
    // constructor, alive until the end of the full expression: what a reference parameter binds to.
    std::string convertedTemporary(const Ty* cls, Expr* arg, Node* at);

