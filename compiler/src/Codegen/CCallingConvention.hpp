// struct Emitter's members: C calling conventions: the declarations, part of the struct's body,
// included in it in Codegen/Emitter.hpp; the definitions in CCallingConvention.cpp (codegen.cpp
// split, stage 2, 2026-11-05, moved without a change).
    // --- C calling conventions (docs/design.md, "Targets and hosts") ---
    // A kpp function passes a class as a pointer to a copy and returns one through a result slot,
    // on every target. A C function (extern "C", or an imported header's) passes a struct by value
    // as the target's C does: MS x64 a struct of 1, 2, 4 or 8 bytes as one integer and any other
    // as a pointer to a copy, a result of 1, 2, 4 or 8 bytes in RAX and any other through the slot;
    // SysV x86-64 (Linux, FreeBSD) one of up to 16 bytes in one or two registers by its eightbytes'
    // classes (INTEGER where an integer or a pointer lies in it, SSE where only floats do), when
    // enough of those registers are left, and any other on the stack (`byval`) or through the slot.
    static bool isCFunction(const FunctionDecl* f) { return (f->flags & DF_ExternC) || (f->file && f->file->isImported); }

    // AAPCS64 (Linux arm64, Apple's arm64): a homogeneous floating-point aggregate (1 to 4 floats or
    // doubles of one type) in that many vector registers, as `[N x float]`; any other struct of up to
    // 16 bytes in one or two general registers, as `i64`, `[2 x i64]` (or `i128` when 16-aligned),
    // its bytes past the struct's end undefined; a larger one through a pointer to a copy, a larger
    // result through the slot in x8 (`sret`). LLVM keeps an array argument's registers together (all of
    // them, or all on the stack, as AAPCS64 says), so no registers are counted.
    struct CPassing
    {
      bool inMemory = true;
      std::vector<std::string> parts; // in registers: each register's LLVM type, in order
      std::vector<uint64_t> offsets;  // and where it starts in the struct
      unsigned ints = 0, sses = 0;    // SysV: the registers it takes
      uint64_t size = 0;              // the struct's (a part may be wider: AAPCS64's i64 for 3 bytes)
    };

    // A part of a struct passed in registers, read from (or written to) its place at addr + offset,
    // where bytes of the struct are left: a part wider than that (AAPCS64's `i64` or `[2 x i64]`) is
    // read as the bytes there and widened, written narrowed, never past the struct.
    // (align: the access's alignment, written when not 0: the register convention's parts)
    std::string loadCPart(const std::string& addr, uint64_t offset, const std::string& type, uint64_t bytes, uint64_t align = 0);
    void storeCPart(const std::string& addr, uint64_t offset, const std::string& type, const std::string& value, uint64_t bytes, bool gepAtZero = false,
                    uint64_t align = 0);

    // --- kpp's register convention (a prototype, docs/register-abi.md; KPP_REGISTER_ABI) ---
    // Off (KPP_REGISTER_ABI=off; the default except on the prototype's branch, where CI runs everything with
    // it on): kpp passes every class by value as a pointer to a copy and returns one through a result slot.
    // With KPP_REGISTER_ABI=vectorcall (or =sysv, =c), a kpp function (not a C one, not variadic, not a
    // closure's call operator) that takes or returns a small trivially copyable class (no destructor,
    // no copy or move constructor of its own, no virtual function; up to 32 bytes as a parameter, 16 as
    // a result) passes it as its eightbytes in registers (SysV's classes: an integer part, a float, a
    // `<2 x float>`, a double, a 16-byte vector), and returns it in registers. On Windows x64 such a
    // function also takes LLVM's x86_vectorcallcc with =vectorcall (MS x64's callee-saved registers, floats
    // and vectors in XMM0-5, results in RAX:RDX or XMM0-3), or x86_64_sysvcc with =sysv (6 integer and 8
    // vector registers; unsafe where the function has a catch or cleanup funclet, withRegisters), MS x64's 4
    // positional registers being too few; =c keeps the target's C convention. A function that passes
    // no such class keeps the target's C convention: C may call it through a pointer (a function type
    // that passes no class by value is one type for both conventions).
    enum class RegisterAbi : uint8_t { Off, Vectorcall, Sysv, C };
    static RegisterAbi registerAbi();
    // How the register convention passes (result: returns) an object of type t: in registers (its
    // parts), or not (a pointer to a copy, a result slot).
    RegisterParts registerPassing(const Ty* t, bool result, Node* at);
    std::unordered_map<const Ty*, RegisterParts> registerPassingCache[2]; // this emitter's answers, [result]
    RegisterParts registerPassingOf(const Ty* t, bool result, Node* at); // (registerPassing's, uncached)
    // s (a kpp function's, ret its result type) with the register convention's lowering, when it is on
    // and s passes or returns a class in registers.
    void withRegisters(Signature& s, const Ty* ret, Node* at);
    // A call of a kpp function of signature s: args one per s.paramTypes entry as kpp passes them
    // ("ptr THIS", "ptr SLOT", a class as "ptr COPY", "i32 5"), lowered where s.registers says (a class
    // in registers loaded from its copy, a result in registers stored into the slot after the call).
    // typed: the call names the function's type (a call through a pointer, a variadic one). Returns the
    // result's value, or "" (void, or the result in the slot).
    std::string emitKppCall(const Signature& s, const std::string& fnText, const std::vector<std::string>& args, bool mayThrow, bool typed);
    // The signature of a kpp function of function type ft (no `this`), as a call through a pointer of
    // that type passes its arguments (with the register convention's lowering when it has it).
    Signature functionTypeSignature(const Ty* ft, Node* at);
    // The alignment of part k of p in its object (p.align, less where the part's offset says so).
    static uint64_t partAlign(const RegisterParts& p, size_t k)
    {
      uint64_t offset = p.offsets[k];
      return offset ? std::min<uint64_t>(p.align, offset & (~offset + 1)) : p.align;
    }

    // The scalars a C struct holds, with their offsets: the members of its members, the elements of
    // its arrays, the storage of its bit fields, every member of a union at its start.
    void cScalars(const Ty* t, uint64_t base, std::vector<std::pair<uint64_t, const Ty*>>& out, Node* at);

    // How the target's C passes (or returns) a struct of type t by value.
    CPassing cPassing(const Ty* t, Node* at);

    // SysV's registers left for a C call's arguments as they are passed.
    struct CRegisters
    {
      unsigned ints = 6, sses = 8;
    };

    // The LLVM type a C call's struct argument of type t is passed as, given the registers left
    // (taken when it goes in registers): one text per register ("i64", "double"; with its parameter
    // attributes: Linux arm64's `[2 x float] alignstack(8)`), or one `byval` text for a copy on the
    // stack, or "ptr" (Windows' and AAPCS64's pointer to a copy).
    std::vector<std::string> cArgumentTypes(const Ty* t, CRegisters& left, CPassing& p, Node* at);

    // A scalar argument of a C call takes one of SysV's registers.
    void cScalarArgument(const Ty* t, CRegisters& left);

    // A C call's arguments as the target's C passes them: each struct by value (its argument text a
    // pointer to the call's copy) loaded into its registers, or passed `byval` from the copy, or (on
    // Windows, other than 1, 2, 4 or 8 bytes) the pointer itself. sretSlot: the call passes a result
    // slot first (it takes a register).
    void lowerCArguments(std::vector<std::pair<const Ty*, std::string>>& args, bool sretSlot, Node* at);

    // The parameter (or result) attribute of a char, short or bool (an enumeration of one too) where the
    // target's C reads it extended to 32 bits (Target::extendsNarrowIntegers): " signext" or " zeroext"
    // by its sign, else "". A C function's declaration takes it on its narrow parameters and result, a
    // call through a pointer on its narrow arguments (the caller extends: what a C callee reads, what
    // a kpp callee ignores), and every definition on its narrow result (the callee extends: what a C
    // caller of a kpp function, through a pointer too, reads).
    const char* narrowExtension(const Ty* t);
    // An argument's text ("i16 %v") with t's narrowExtension after its type ("i16 signext %v").
    std::string withNarrowExtension(const Ty* t, const std::string& text);
    // The argument text before a C call's result slot: `ptr `, and off Windows `ptr sret(T) ` for a C
    // function's (ret: its result type; null for a kpp function's slot), which AAPCS64 passes in x8,
    // not x0, as the C-callable definition's `sret` parameter has it.
    std::string cSlotArgument(const Ty* ret, Node* at);
    // A C function's parameter list without its parameter attributes (`byval`, `sret`, `signext`, ...), for a
    // call's function type, which takes none: they go on the call's arguments.
    static std::string withoutParameterAttributes(std::string params);

    // How a C function returns its result of class type ret (a struct by value): in registers (the
    // LLVM return type, its parts at their offsets), or through the caller's slot (inMemory).
    std::string cReturnType(const CPassing& p);

    // The `declare` line's return type and parameters of C function f under its target's C calling
    // convention (signatureOf's, with its structs by value lowered).
    std::pair<std::string, std::string> cDeclaration(FunctionDecl* f, const Signature& s);
    // cDeclaration's, for a function of result type ret and signature s (its kpp convention's).
    std::pair<std::string, std::string> cDeclarationOf(const Ty* ret, const Signature& s, Node* at);

    // A call through a pointer to a function of C's calling convention (Ty::isCConvention: a C
    // function's type, or a C header's function pointer type, that takes or returns a class by
    // value): its structs by value passed and its struct result taken as the target's C does, as a
    // direct call to a C function passes them (callIndirect's for kpp's convention). C functions
    // throw no kpp exception.
    Value callIndirectC(const std::string& fnText, const Ty* ft, Span<Expr*> callArgs, const std::string& sret, Node* c);

    // Whether a C function of signature s (its result of type ret) takes or returns a struct by
    // value other than as kpp passes one (a pointer to a copy, a result slot): SysV's every one,
    // MS x64's of 1, 2, 4 or 8 bytes. Its definition is then lowered as cDeclaration says.
    bool cStructsByValue(const Signature& s, const Ty* ret, Node* at);

    // The `ret` of a function whose result is void or in a slot: `ret void`, or for a C-callable
    // function's struct result in registers (cReturnParts) its parts loaded from the slot, returned.
    void emitReturnVoid();

    // The address of a C function the program declares (extern "C": defined in kpp, or in a C
    // library) that takes or returns a struct by value other than as kpp passes one: refused until
    // 2026-11-05, since a call through a function pointer passed the struct as kpp does. Its type is
    // now of C's convention (GlobalScope::Impl::withCConvention), and such a call is callIndirectC's;
    // nothing to refuse.
    void refuseCAddress(FunctionDecl* f, Node* at);

    // A function (in every module) that moves `this` by the offset `at` (a pointer constant) and calls
    // callee, or the function in slot `slot` of the vtable `this` then has, with its other arguments.
    std::string sharedThunk(const std::string& name, const Signature& s, const std::string& at, const std::string& callee, int slot);

    // The calling convention of a member function of type ft, as signatureOf gives it for a
    // declaration: `this`, the result's slot for a class result, then the parameters.
    Signature methodSignature(const Ty* ft, Node* at);

    // Signature under the arguments currently in effect.
    Signature signatureOf(FunctionDecl* f);

    std::string signatureText(const Signature& s);

