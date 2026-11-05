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

    // A function (in every module) that moves `this` by the offset `at` (a pointer constant) and calls
    // callee, or the function in slot `slot` of the vtable `this` then has, with its other arguments.
    std::string sharedThunk(const std::string& name, const Signature& s, const std::string& at, const std::string& callee, int slot);

    // The calling convention of a member function of type ft, as signatureOf gives it for a
    // declaration: `this`, the result's slot for a class result, then the parameters.
    Signature methodSignature(const Ty* ft, Node* at);

    // Signature under the arguments currently in effect.
    Signature signatureOf(FunctionDecl* f);

    std::string signatureText(const Signature& s);

