// struct Emitter's members: constant data and globals: the declarations, part of the struct's body,
// included in it in Codegen/Emitter.hpp; the definitions in ConstantData.cpp (codegen.cpp split,
// stage 2, 2026-11-05, moved without a change).
    // --- constant data (design.md, "Compile-time evaluation: one interpreter", step 4b) ---

    static bool zeroText(const std::string& s) { return s == "0" || s == "false" || s == "null" || s == "zeroinitializer" || s == "0x0000000000000000"; }

    // The data members of class c as the interpreter's Object parts list them (after the bases).
    static std::vector<Decl*> valueMembers(ClassDecl* c);

    // The LLVM constant value v of type t is (a global's initializer, ConstValue's contract in
    // consteval.hpp), or nothing when a part is none the IR can hold here: a polymorphic class, a
    // union, a class with an anonymous member or a reference member, a member pointer, a part of a
    // kind its type cannot take. Then the global is built at startup, as before.
    std::optional<std::string> constantText(const ConstValue& v, const Ty* t, Node* at, int depth = 0);

    std::optional<std::string> classConstant(const ConstValue& v, const Ty* t, Node* at, int depth);

    // A Pointer value: into an array the evaluation made (a string literal's chars, a brace list's
    // elements), which becomes a constant (one object per contents in the program, as a string
    // literal is: constantName), and the element's address.
    std::optional<std::string> evaluatedArrayElement(const ConstValue& v, const Ty* pointerTy, Node* at, int depth);

    // An Address value: a function, or a global (a part of its value along the path).
    std::optional<std::string> addressConstant(const ConstTarget& target, Node* at);

    // A class (or an array of them) with a mutable member: no constant memory for a const global.
    bool hasMutableMember(const Ty* t, Node* at, int depth = 0);

    // A global's initializer as constant data: the value the interpreter computes for it as type t,
    // lowered (constantText), or nothing (it is built at startup). evaluated: whether the
    // interpreter computed the value (nothing then means no LLVM constant expresses it yet).
    std::optional<std::string> constantData(const Ty* t, Expr* value, VariableDecl* def, const TemplateArgs& args, Node* at, bool* evaluated = nullptr);
    // Why constantData's evaluation gave no value (the interpreter's reason, maybe none): said by
    // the error for a constexpr variable that is no constant.
    std::string notConstantWhy;

    std::map<std::pair<Decl*, const Ty*>, std::string> globalCache; // globalFor's answers this emitter had
    // A global's initializer evaluated before globalFor takes the shared lock (preEvaluateGlobal): the
    // values globalForOnce would compute, so that the evaluator, nearly all of a global's definition,
    // runs while the other emitters go on (it held the lock 6 of codegen's 9 s). Taken only for the
    // same initializer and type; unused, what the evaluation noted is taken back.
    struct PreEvaluated
    {
      Expr* value = nullptr; // the initializer
      const Ty* type = nullptr;
      bool folded = false; // foldConst's value, as globalForOnce takes it
      std::optional<ConstValue> scalar;
      bool evaluated = false; // the object's value (constantData)
      std::optional<ConstValue> data;
      std::string why; // the interpreter's reason when data is none
      bool used = false;
      // The errors the evaluation recorded (a layout it asked for failed: the evaluator's hook
      // catches the throw, the error stays): reported when globalForOnce takes a value, as if it
      // had evaluated it there, else dropped with the values.
      std::vector<std::string> errors;
    };
    PreEvaluated* preEvaluated = nullptr;
    // globalForOnce takes a pre-evaluated value: the evaluation's errors are its now.
    void takePreEvaluated(PreEvaluated& pre);

    // The values globalForOnce computes from v's initializer, computed as it does (its arguments,
    // class, file and declaration context), without the shared lock.
    void preEvaluateGlobal(VariableDecl* v, Node* at, const Ty* classTy, PreEvaluated& pre);

    // An emitter's claim on a global it evaluates before the lock (sh.evaluating), given up once the
    // global is defined (or failed).
    struct GlobalClaim
    {
      Shared& sh;
      std::pair<Decl*, const Ty*> key;
      GlobalClaim(Shared& sh, std::pair<Decl*, const Ty*> key) : sh(sh), key(key) {}
      ~GlobalClaim()
      {
        {
          std::lock_guard claims(sh.claimsMutex);
          sh.evaluating.erase(key);
        }
        sh.claimsChanged.notify_all();
      }
    };

    std::string globalFor(VariableDecl* v, Node* at, const Ty* classTy = nullptr);

    // A thread_local global built per thread: its function, called before the use being emitted
    // (it returns at once once this thread has built it). Known for good once the global is defined
    // (globalFor asks after that): remembered by this emitter, as published answers are.
    std::unordered_map<std::string, std::string> threadLocalCache;
    void buildThreadLocal(const std::string& name);

    std::string globalForOnce(VariableDecl* v, Node* at, const Ty* classTy);

