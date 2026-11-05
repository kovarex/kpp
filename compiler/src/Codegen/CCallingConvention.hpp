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

    struct CPassing
    {
      bool inMemory = true;
      std::vector<std::string> parts; // in registers: each register's LLVM type, in order
      std::vector<uint64_t> offsets;  // and where it starts in the struct
      unsigned ints = 0, sses = 0;    // SysV: the registers it takes
    };

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
    // (taken when it goes in registers): one text per register ("i64", "double"), or one `byval`
    // text for a copy on the stack, or "ptr" (Windows' pointer to a copy).
    std::vector<std::string> cArgumentTypes(const Ty* t, CRegisters& left, CPassing& p, Node* at);

    // A scalar argument of a C call takes one of SysV's registers.
    void cScalarArgument(const Ty* t, CRegisters& left);

    // A C call's arguments as the target's C passes them: each struct by value (its argument text a
    // pointer to the call's copy) loaded into its registers, or passed `byval` from the copy, or (on
    // Windows, other than 1, 2, 4 or 8 bytes) the pointer itself. sretSlot: the call passes a result
    // slot first (it takes a register).
    void lowerCArguments(std::vector<std::pair<const Ty*, std::string>>& args, bool sretSlot, Node* at);

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

