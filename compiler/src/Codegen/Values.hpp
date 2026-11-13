// struct Emitter's members: values and conversions: the declarations, part of the struct's body,
// included in it in Codegen/Emitter.hpp; the definitions in Values.cpp (codegen.cpp split, stage 2,
// 2026-11-05, moved without a change).
    // --- values ---

    // The class's `operator T()` converting to `to` (exactly, or any scalar one when none is exact),
    // its own or inherited (`operator bool` of a BoolConstant base); explicit ones for bool, where
    // conditions allow them (context), and in an explicit cast when they yield the type itself
    // (`double(b)`, `static_cast<double>(b)` with `explicit operator const double&()`: direct
    // initialisation's rule). foundIn: the class (or base) declaring it.
    // opArgs: the arguments to call it with (the class's, and for an operator template the one its
    // result is deduced to: `template<class T> operator T()` converting to double has T = double).
    FunctionDecl* conversionOperator(const Ty* from, const Ty* to, Node* at, const Ty** foundIn = nullptr, bool explicitCast = false, TemplateArgs* opArgs = nullptr,
                                     ConversionContext context = ConversionContext::Unknown);

    // A class value as conversionOperator takes it: T& for an lvalue, T&& for an rvalue, T when its
    // value category is not known (Value::category; chooseConversion).
    const Ty* conversionObject(const Value& v);

    // A class value through its (non-explicit) conversion operator to a pointer, own or inherited,
    // or nothing when it has none: what `*` and `delete` of such an object work on.
    std::optional<Value> toPointer(const Value& object, Node* at);

    // A class's (non-explicit) conversion operator to an enum or an integer, own or inherited, or
    // null: what a switch on the object switches on.
    FunctionDecl* integralConversionOperator(const Ty* from, Node* at, const Ty** foundIn = nullptr);
    // A kpp enum's conversion operator to `to` (kovarex, 2026-11-11: as a class's): an explicit one (kpp's default) where a
    // condition, an operand of ! && || ?:, a direct-initialization or a cast converts (contextual), an implicit one anywhere;
    // null when the enum declares none.
    FunctionDecl* enumConversionOperator(const Ty* from, const Ty* to, bool contextual);

    // The conversion operator the one rule chooses (GlobalScope::chooseConversion, design.md 24), the
    // class declaring it canonical.
    FunctionDecl* conversionOf(const Ty* from, const Ty* to, GlobalScope::ConversionKind kind, bool explicitCast, Node* at, const Ty** foundIn,
                               TemplateArgs* opArgs, ConversionContext context = ConversionContext::Unknown);

    // Calls a conversion operator found in `owner` (the object's class or one of its bases) on
    // the object at addr.
    // sret: where a class result is built (an object the conversion initializes, C++17's guaranteed
    // elision), "" for a temporary.
    Value callConversion(FunctionDecl* op, const Ty* objectTy, const Ty* owner, const std::string& addr, Node* at, const TemplateArgs* opArgs = nullptr, bool keepReference = false,
                         const std::string& sret = "");

    // explicitCast: the conversion an explicit cast asks for, which may use explicit conversion
    // operators.
    // kpp requires nullptr (kovarex, 2026-11-04; design.md 4): an integer 0 of kpp's own sources, the
    // literal or a constant, made a null pointer or member pointer here without a cast is an error,
    // at the literal's own place where it is one (the walk reports the ones it sees, an initializer,
    // an argument, a comparison; this, what only codegen converts: a template's return, a member
    // pointer). Imported C keeps C's rule. True when it failed.
    bool zeroIsNoPointer(Node* at);

    // An enumeration declared `enum class` (or `enum struct`).
    bool scopedEnum(const Ty* t) const
    {
      t = t ? types.unqualified(t) : nullptr;
      return t && t->kind == TyKind::Enum && t->decl && t->decl->kind == NodeKind::EnumDecl && static_cast<EnumDecl*>(t->decl)->isClass;
    }
    // context: a class's conversion to bool, a condition's or a copy's (conversionOperator).
    Value convert(Value v, const Ty* to, Node* at, bool explicitCast = false, ConversionContext context = ConversionContext::Unknown);

    // Address of the `base` subobject inside the `derived` object at addr (false if not a base).
    // An explicit cast: what convert does, plus the downcast to a derived class (pointer or
    // reference) that an implicit conversion never makes.
    Value explicitConvert(Value v, const Ty* to, Node* at);

    // A pointer moved to a base or derived subobject: null stays null (C++'s rule for pointer conversions).
    std::string keepNull(const std::string& original, const std::string& adjusted);

    // Whether C++ converts from (a pointer, array, function or nullptr) to the pointer `to` without a
    // cast ([conv.ptr], [conv.qual], [conv.array], [conv.func]): to the same type with added
    // qualifiers, to `void*` from an object pointer, from a derived class to its base (the caller
    // moves the address); a function or an array to a pointer to it, or to its element. Imported C
    // converts any object pointer to and from `void*` (C11 6.3.2.3).
    bool impliedPointerConversion(const Ty* from, const Ty* to, Node* at);

    // C++'s qualification conversion of a pointer to pointers ([conv.qual]): every level of `to` keeps
    // the qualifiers of `from`'s, and a level that adds one has const at every level above it but
    // the outermost (`T**` is no `const T**`, `T* const*` is a `const T* const*`).
    bool qualificationConverts(const Ty* from, const Ty* to);

    // The chain of base-class fields from derived down to base: (struct name, field index) pairs.
    // A base reached along two paths (`struct Both : Left, Right`, both of them Bases) is two
    // subobjects, and the conversion to it is ambiguous: an error, as in C++ ([conv.ptr]/3).
    bool basePath(const Ty* derived, const Ty* base, std::vector<std::pair<std::string, uint32_t>>& path, Node* at);

    // The derived object's address from its base subobject's, back along the path of base fields.
    std::string derivedAddress(const std::vector<std::pair<std::string, uint32_t>>& path, const std::string& baseAddr);

    // The type of the object a conditional's operand x names when the conditional, of type t, can
    // be that object (C++'s lvalue conditional): x an lvalue or a reference of type t or, for a
    // class, of a class derived from it (whose t part it is). Null when x converts to t, so that
    // the conditional is a new value (`p ? p->name : Mod::CORE_MOD`, a String and a `const char*`).
    const Ty* conditionalObject(Expr* x, const Ty* t, Node* at);

    // The address of the object the operand x names (of type xt, conditionalObject's), as an
    // object of t: its t part for a class derived from t.
    std::string conditionalAddress(Expr* x, const Ty* xt, const Ty* t, Node* at);

    bool baseAddress(const Ty* derived, const Ty* base, const std::string& addr, std::string& out, Node* at);

    // An arithmetic operand's type after the integral promotions.
    const Ty* promoted(const Ty* t);

    const Ty* commonArithmetic(const Ty* a, const Ty* b);

    // The left operand of `a ?: b` and `a?->b` (ParenExpr::holds), kept for its HeldExprs: its value, and
    // its address when it is a scalar lvalue.
    struct HeldValue
    {
      Value value;
      std::string address;
    };
    std::unordered_map<const ParenExpr*, HeldValue> heldValues;
    Value holdValue(ParenExpr* p);
    std::string heldAddress(ParenExpr* p, Node* at);

    Value load(const std::string& addr, const Ty* ty, Node* at);

    void store(Value v, const std::string& addr, const Ty* ty, Node* at);

    std::string thisPointer(Node* at);

    std::string operatorName(P op);

    // The address of the object a call's result names: a returned reference's referent, a class
    // result's object (its slot), and a scalar returned by value (a prvalue) in a temporary of its
    // own. Never the scalar itself: Targeter's `T* operator*() const` bound to Map's
    // `operator[](const K&)` passed the rail pointer as the key's address.
    std::string resultAddress(const Value& v, Node* at);

    // A variable template's object for the arguments of its use n (`&hasBegin<Vector<int>>`, a
    // reference bound to it): a constant global per argument list, holding the value the use folds
    // to (its rvalue). Never one global named by the template alone: that would be every
    // instance's, its initializer evaluated without the arguments (`hasBegin<Vector<int>>` read
    // false). Only a const one: a mutable instance would need its own storage, built at startup.
    bool isConstValue(VariableDecl* v);
    std::string variableTemplateObject(NameExpr* n, VariableDecl* v, Node* at);
    std::optional<Value> variableTemplateConstant(Expr* e, const Ty* t);

    std::string lvalue(Expr* e);
    std::string prefixTarget; // The address the last built-in `++`/`--` wrote: lvalue's for a prefix one.

    // A global built at startup (a class with a constructor, or an initializer that is not a
    // constant): a function constructing it, run from llvm.global_ctors in definition order (see
    // ctorsText); destructors at exit do not run yet.
    // init: the global's initializer (for a static member of a class template, its instantiation's
    // copy), or null.
    // perThread: a thread_local global, built by each thread at its first use, not at startup: the
    // function checks a thread_local guard, and globalFor calls it before every use (C++'s
    // thread_local wrapper). Destroyed at no thread's exit yet.
    void emitGlobalConstructor(VariableDecl* v, const std::string& name, const Ty* t, Expr* init, Node* at, bool perThread = false, VariableDecl* definition = nullptr,
                               Shared::GlobalBuild build = Shared::GlobalBuild::BuildAndExit);

    // The llvm.global_ctors array, all of it in module 0: the globals built at startup in
    // definition order within a file (design.md section 2: a global's initializer may read the ones
    // defined before it in its file), the files in the driver's order, the library first. Not by
    // module: which module a startup function lands in says nothing about the order.
    std::string ctorsText(unsigned k);

    // The declaration carrying a global's initializer: the out-of-line definition of a static data
    // member (`double Gui::scale = 1.0;`), or the declaration itself.
    VariableDecl* variableDefinition(VariableDecl* v);

    // The definition of static member v in the class instance classTy: the explicit specialisation
    // written for that instance (`template<> const uint32_t ElectricNetworkStatistics::smoothingTicks
    // = 0;`), or the template's definition; v's own definition without an instance.
    VariableDecl* variableDefinition(VariableDecl* v, const Ty* classTy);

    // The type the qualifier of a qualified name names, under the current arguments: `Counted<int>`
    // of `Counted<int>::instances`, the class `decltype(check<T>(nullptr))` names of
    // `decltype(check<T>(nullptr))::value`; null for an unqualified name.
    const Ty* qualifierOf(NameExpr* n);

    // A template parameter written as a qualifier (`T::loadExplicit(input)` in a generic lambda
    // `[&]<class T>(T) { ... }`): its argument in the instance being emitted, by name. The lambda's
    // own parameters are in no scope that a re-typing from the enclosing function looks in.
    const Ty* boundParameter(const QualifiedName& q);

    // The class a member's name is qualified with (`Hook<Robot, TagB>` of `r.Hook<Robot, TagB>::getId()`),
    // canonical; null for an unqualified member.
    // The implicit assignment of class target (objectTy itself or a base of it), member by member,
    // into the object of class objectTy at addr from arg (`this->Base::operator=(other)` and
    // `super::operator=(other)` of a class declaring none); the target object as an lvalue.
    Value implicitAssignment(const std::string& addr, const Ty* objectTy, const Ty* target, Expr* arg, Node* at);

    const Ty* memberQualifier(MemberExpr* m, Node* at);
    // The class a member named with no object is qualified with (`Right::pad` in a member
    // function), if it names one of its own.
    const Ty* implicitThisQualifier(NameExpr* n, Node* at);
    // Moves `object`, an objectTy, to the instance of member's class that qualifier is or derives
    // from, through the qualifier's own subobject: `b.Right::id` with Base under both Left and
    // Right is the id of Right's Base, and `this->Hook<Robot, TagB>::id` of that instance of Hook,
    // not of the first base holding a field of that declaration.
    auto moveToQualifier(const Ty*& objectTy, std::string& object, Decl* member, const Ty* qualifier, Node* at) -> void;

    // `Counted<int>::instances`: the instantiation a qualified static member belongs to (null for
    // an unqualified name or a class that is not a template).
    const Ty* qualifierClass(Node* e, Node* at);

    // The instantiation a static member of a class template is used with: the qualifier written,
    // or the class of the method being emitted, or the base of either that is an instantiation of
    // the member's class (`MyLimits<int>::digits` with digits declared in the base
    // IntegerLimits<int, true, 32>).
    const Ty* staticMemberClass(VariableDecl* v, Node* e, Node* at, const Ty* classTy);

    // The class a qualified name's qualifier names (`MyLimits<int>` of `MyLimits<int>::digits`),
    // any class; null for an unqualified name.
    const Ty* qualifierType(Node* e, Node* at);

    // The instantiation of the class template owner that cls is or derives from; null if none.
    const Ty* instantiationIn(const Ty* cls, Decl* owner, Node* at, int depth = 0);
    // The same through cls's enclosing classes (`Box<char>::Inner`, whose arguments are Box's, naming
    // Box's static member bare): null if none encloses an instantiation of owner.
    const Ty* enclosingInstantiationIn(const Ty* cls, Decl* owner, Node* at);

    // `using EmptyWidget::EmptyWidget;` in a class without constructors of its own: the base whose
    // constructors are inherited (its name, as a constructor initializer would spell it); null if none.
    std::optional<QualifiedName> inheritedConstructorBase(ClassDecl* c);

    // The address of the object a global names: the global itself, or, for a reference global,
    // what it was bound to. classTy: the instantiation, for a static member of a class template.
    std::string globalAddress(VariableDecl* v, Node* at, const Ty* classTy = nullptr);
    // The address of the thread_local global name in the current thread, for the code emitted now: through
    // llvm.threadlocal.address, as clang takes it (the global's own name as an operand is a constant to LLVM, which on
    // macOS arm64 an unoptimized build computed once and used after calls that had clobbered it: thread O, 2026-11-13).
    std::string threadLocalAddress(const std::string& global);
    // Whether v, a global or a static local, is thread_local (its definition's word counts too).
    bool isThreadLocalVariable(VariableDecl* v);
    // A local's address: a thread_local static local's in the current thread (threadLocalAddress).
    std::string localAddress(const Local& local) { return local.threadLocal ? threadLocalAddress(local.addr) : local.addr; }

