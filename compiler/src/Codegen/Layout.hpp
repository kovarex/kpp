// struct Emitter's members: sizes, alignment, class layouts, vtables and fields: the declarations,
// part of the struct's body, included in it in Codegen/Emitter.hpp; the definitions in Layout.cpp
// (codegen.cpp split, stage 2, 2026-11-05, moved without a change).
    uint64_t sizeOf(const Ty* t, Node* at = nullptr);

    // A class's size and alignment, which its layout has once complete (buildStruct's last step).
    // Read without the shared lock: structFor lays the class out first, and a complete layout's size
    // does not change; a layout this emitter is building is its own.
    std::unordered_map<const Ty*, std::pair<uint64_t, uint64_t>> sizeCache; // this emitter's view of the published sizes
    std::pair<uint64_t, uint64_t> classSizeAndAlign(const Ty* t, Node* at);

    // A class's size and alignment from its fields and its alignas: its layout's last step, and the
    // size of a class still being laid out. A class that comes back here while this emitter computes
    // its size contains itself.
    std::unordered_set<const StructInfo*> sizingNow; // the layouts whose size this emitter is computing
    std::pair<uint64_t, uint64_t> fieldsSize(const StructInfo& info, const Ty* t, Node* at);

    uint64_t alignOf(const Ty* t, Node* at = nullptr);

    // The alignment a class declares with alignas(N), or 1; N under the instantiation's arguments
    // (`template<size_t Align> struct alignas(Align) AlignedBytes`).
    uint64_t declaredAlignment(const Ty* t, Node* at);

    // An alignas(N) written in owner (a class, a function), N under args: a power of two.
    uint64_t alignmentValue(Expr* alignment, Decl* owner, const TemplateArgs& args, const std::string& what, Node* at);

    // The alignment LLVM gives a value of type t from the type alone (its ABI alignment): what a
    // class's alignas adds on top of its fields' it only gets from padding fields and `align`.
    uint64_t llvmAlignOf(const Ty* t, Node* at);

    // A pointer to a member function of class cls carries the bytes to add to `this` (`{ ptr, i32 }`,
    // MSVC's multiple inheritance model, 16 bytes) when some base subobject anywhere in cls's
    // hierarchy is at a non-zero offset, so that one of a base there converts to cls's and back,
    // read at run time too; else it is the function alone (`ptr`, the single model, 8 bytes).
    // Decided from the declarations as structFor lays classes out (its own vtable pointer, the
    // polymorphic bases, then the others; an empty base takes no bytes), never from a layout, which
    // may hold a pointer to a member function of the class itself (a base's `void (Derived::*)()`).
    bool hasBaseAtOffset(const Ty* cls, Node* at, int depth = 0);
    std::unordered_map<const Ty*, bool> baseAtOffsetCache;
    // Whether mp, a pointer to a member function, is of the multiple model (hasBaseAtOffset).
    bool memberFunctionPointerAdjusts(const Ty* mp, Node* at);
    // A member function pointer value of the multiple model: its function and adjustment (constants
    // read from a constant, else extracted), and one made of them (a constant from constants).
    std::pair<std::string, std::string> memberFunctionParts(const std::string& value);
    std::string memberFunctionValue(const std::string& function, const std::string& adjustment);

    // `, align N` for an object of type t (and an alignas(N) of the variable itself, or 0) whose
    // alignment is more than LLVM gives its type: an alloca's, a global's.
    std::string alignSuffix(const Ty* t, uint64_t declared, Node* at);

    // Whether an object of type t needs more alignment than LLVM gives its type (a class with
    // alignas, or holding a member that has it).
    bool overAlignedType(const Ty* t, Node* at);

    // The alignment of a class (info, laid out but for its LLVM text) and of each field: LLVM aligns
    // a field to its type's ABI alignment only, so where alignas asks for more (on the class, on a
    // member, or a member's class has it) the bytes C++ puts before a field become a `[n x i8]` field
    // of their own, and the class gets the size and alignment sizeof and alignof give. Returns the
    // padding to write at the end of the struct (after its byte of its own, when empty).
    uint64_t alignPadding(StructInfo& info, ClassDecl* c, const Ty* key, const TemplateArgs& args, Node* at);

    // A local's or a global's own alignas(N), or 0: N in the context it is written in (the function
    // being emitted for a local, the variable itself for a global), under the current arguments.
    uint64_t variableAlignment(VariableDecl* v, Decl* context, Node* at);

    // A class type whose class is declared but has no definition in the program.
    bool declaredOnly(const Ty* t);

    ClassDecl* classDef(Decl* decl, Node* at);
    // A class's bases as written, their types under the class's arguments: a pack of bases
    // (`struct overloaded : Ts...`) one per element of its pack, in order (null for a type unknown).
    std::vector<const Ty*> basesOf(ClassDecl* c, const TemplateArgs& args, Node* at);

    // The instantiation type with the definition as its decl (forward declarations map here).
    const Ty* canonicalClass(const Ty* t, Node* at);

    // Type t with every class in it named by its definition, a forward declaration's decl and the
    // definition's being one class: the key of a class's layout (and IR name) is then the same however
    // its arguments were reached (`ReverseIterator<const agui::Widget**>` with Widget declared ahead in
    // one file and defined in another was two layouts, the second named `.2` in whatever order the
    // threads met them, so the IR differed from run to run).
    std::unordered_map<const Ty*, const Ty*> definitionsCache;
    const Ty* definitionsIn(const Ty* t, int depth = 0);

    // The layouts this emitter has used: published ones, complete (or failed), which do not change
    // any more, so they are read without a lock.
    std::unordered_map<const Ty*, StructInfo*> layoutCache;
    // The layouts this emitter is building: its own copies, by key, until each is published (a class
    // that comes back while it is being built, a member or base of its own type or its vtables
    // asking for it, finds its copy here, as built so far). The copies another emitter published
    // first are kept, so that nothing this emitter took from one points at freed memory.
    std::unordered_map<const Ty*, StructInfo> privateLayouts;
    std::vector<std::unordered_map<const Ty*, StructInfo>::node_type> droppedLayouts;

    // A class's layout, built the first time any emitter asks for it. Without the shared lock: the
    // emitter builds its own copy, its vtables included (buildStruct), and publishes it whole; when
    // two build one class at once, the copy published first is the class's, the same either way,
    // and the other is dropped. So nobody ever waits for a layout another emitter builds (no lock
    // order to keep), and the struct's type line is added once, by the copy published.
    StructInfo& structFor(const Ty* classTy, Node* at);

    // Builds class key's layout as this emitter's copy and publishes it, or takes the copy another
    // emitter published first (dropping this one). A layout that fails is published failed: every
    // use reports what made it fail, at the use, the first use like the others (its own report came
    // from inside the layout, wherever that was), so that the lines do not depend on which use comes
    // first or which emitter built it.
    StructInfo& buildAndPublish(const Ty* key, const Ty* classTy, Node* at);

    // A failed layout gives every use the error its first use got (the same lines whichever use
    // comes first, so that they do not depend on the order functions are emitted in).
    StructInfo& checkedLayout(StructInfo& info, const Ty* classTy, Node* at);

    // The placeholder class of a type name declared nowhere (a draft) that a value of type t is or
    // holds by value (through arrays), or null.
    static const Ty* draftHeld(const Ty* t);

    // Lays out a class into info (this emitter's copy, structFor's): bases first, then its data
    // members, its size, and for a polymorphic class its vtables; every member and base must be known
    // (a guess would be a wrong layout that compiles). typeLine: the struct's type definition, which
    // the copy published adds to the declarations.
    void buildStruct(const Ty* key, Node* at, StructInfo& info, std::string& typeLine);

    // The vtables of class key (design.md section 9): the primary one extends the primary base's
    // (or starts empty, the class's own); then every vtable of the other polymorphic bases, and those
    // of the primary base after its first, as the bases have them, at the bases' places. The class's
    // virtual functions take the slots they override in all of them; one that overrides no slot of
    // the primary vtable also gets a slot there, so that a call through the class needs no thunk.
    std::vector<Vtable> collectVtables(const Ty* key, ClassDecl* c, const TemplateArgs& args, const std::vector<const Ty*>& baseKeys, const std::vector<uint32_t>& baseIndex, int primary, Node* at);

    // The address an object's subobject at path (base fields down from class cls) has in an object
    // placed at address 0, which is its offset: a constant expression, "null" for the object itself.
    std::string subobjectAtNull(const Ty* cls, const std::vector<uint32_t>& path, Node* at);

    // Whether two subobjects at these paths start at the same address: a field 0 is at its
    // object's start, so trailing zeros go (the primary base's subobject, and its primary base's).
    static bool samePlace(std::vector<uint32_t> a, std::vector<uint32_t> b);

    // The definitions of class key's vtables, with a thunk for each slot whose function belongs to a
    // class at another place than the subobject whose vtable pointer leads to the slot. A vtable
    // pointer points at the first function's slot, the functions following in declaration order
    // (MSVC's layout and COM's: a C object's table of functions is called as a kpp class's, and the
    // reverse). Slot -1 is the class's descriptor (typeid, dynamic_cast), slot -2 the offset of the
    // subobject holding the pointer within the whole object (delete and dynamic_cast find the whole
    // object with it), slot -3 the function that gives the whole object's block back when delete
    // goes through a virtual destructor: the class's own operator delete's (deleterFor), or null for
    // free ([expr.delete] 10: the operator delete of the object's own class).
    // The name a class's vtables, thunks and typeinfo are called by: its printed name, and for a class
    // whose printed name another class shares, the number its IR type got (irTypeName's ".N"). A
    // local class of a function template has one name for all instances
    // (`LambdaIDConnector::make::InnerResolver@73` for every Callback): their vtables were one
    // linkonce_odr vtable, which the linker kept for all, so every InnerResolver ran one callback.
    std::string classIrName(const Ty* key, const StructInfo& info);
    // A class's name as MSVC mangles it (`Square@game@@`), for its vtables' symbols; empty for one with
    // template arguments, a local or anonymous one, or one in a class template or anonymous namespace.
    std::string msvcClassName(const Ty* cls);
    std::set<std::string> msvcTableNames; // (a class's tables' MSVC names, each once)

    void emitVtables(const Ty* key, StructInfo& info, Node* at);

    // How far a vtable entry moves the result of its function: a covariant override returns a
    // pointer or reference to its own result class, of which the callers through that vtable
    // expect a base (the function as the classes sharing the table declare or inherit it: the class
    // at the table's path and its primary bases), at bytes into it; a pointer may be null, which
    // stays null. Callers that expect results at different places are an error (one slot cannot
    // give both).
    struct ResultMove
    {
      uint64_t bytes = 0;
      bool pointer = false;
    };
    ResultMove covariantMove(const Ty* key, const Vtable& table, const VtableSlot& slot, Node* at);

    // The class whose object f, a member of owner, returns a pointer or a reference to (pointer
    // says which), or null for any other result.
    const Ty* resultClassOf(FunctionDecl* f, const Ty* owner, bool& pointer);

    // Where base's subobject starts in an object of class derived (0 for the class itself), or
    // nothing when base is no base of it.
    std::optional<uint64_t> baseOffsetIn(const Ty* derived, const Ty* base, Node* at);

    // A function that moves `this` from the subobject at `from` to the one at `to` (offsets as
    // subobjectAtNull gives them), calls target with the arguments it was given, and moves its
    // result as covariantMove says.
    std::string adjustingThunk(const std::string& name, const std::string& target, const VtableSlot& slot, const std::string& from, const std::string& to, Node* at,
                               ResultMove move);

    // What a pointer to a member function of class cls holds when calling f with the object is not
    // enough: a virtual f calls the object's override (through the vtable pointer of owner, the class
    // declaring f), and an f of a base at another place in cls gets `this` moved to that base (path,
    // from cls down to owner). One function per class and member, in every module.
    std::string memberFunctionThunk(const Ty* cls, const Ty* owner, const std::vector<std::pair<std::string, uint32_t>>& path, FunctionDecl* f, int slot, Node* at);

    // The offset of the subobject a path of base fields leads to, as a pointer constant (see
    // subobjectAtNull).
    static std::string pathAtNull(const std::vector<std::pair<std::string, uint32_t>>& path);

    // The descriptor of a class (what slot -1 of its vtables points at, what dynamic_cast searches):
    // { name, number of direct bases, [{ base's descriptor, base's offset }] }, for any class (a
    // polymorphic class's bases need not be polymorphic). Built once, used per module (useDescriptor).
    // Made without the shared lock: a published layout's descriptor under sh.descriptorsMutex
    // (another emitter may make the same text at once: the first one set stays), the layout this
    // emitter is building (its vtables' descriptor) as its own copy's.
    std::string descriptorFor(const Ty* cls, Node* at);

    // A class's descriptor and its bases' in the module being emitted (each once).
    void useDescriptor(const StructInfo& info);

    // Identity of a method for override matching: name, parameter types and constness.
    std::string methodKey(FunctionDecl* f);

    // Slot of f in the primary vtable of classTy, or -1 if f is not virtual there. A call goes
    // through the vtable pointer of the class that declares f (every virtual function of a class has
    // a slot in its primary vtable), with `this` moved to that class's subobject.
    int slotOf(const Ty* classTy, FunctionDecl* f, Node* at);
    static int slotKeyed(const StructInfo& info, const std::string& key);

    // Loads a vtable slot of the object at addr: a function's (from 0), or the class descriptor (-1).
    std::string loadSlot(const std::string& addr, int slot);

    std::string gep(const std::string& structName, const std::string& addr, uint32_t index);

    // The type a data member has in the object's class as laid out (under the class's arguments:
    // Holder<int&>'s `A value` is an int&), found in the class, its bases or its anonymous members;
    // null when it is none of them.
    const Ty* fieldTypeIn(const Ty* classTy, Decl* member, Node* at, int depth = 0);

    // The address of member in the object at objectAddr (of class classTy): its field's, found in the
    // class's layout, an anonymous member's or a base's. bits, when given, gets the member's bit-field
    // place in the layout that holds it (null for a member that is no bit field).
    bool fieldAddress(const Ty* classTy, Decl* member, const std::string& objectAddr, std::string& out, Node* at, const BitfieldInfo** bits = nullptr);

    // Look up a field by symbol name in a class's own members and its bases (used when the
    // resolver left MemberExpr::resolved null).  Searches the AST directly, without building
    // struct layouts, so it's safe for types that structFor would choke on.
    Decl* findFieldByName(const Ty* classTy, uint32_t nameSymbol, Node* at, int depth = 0);

    // Whether data member m of class c exists in the instance key (args its arguments): a `when`
    // member on a class template's parameters (design.md 19) is decided per instance, its condition
    // folded under the instance's arguments as a bit field's width is. The parser decided every
    // other `when` member (one it found false is no member of c).
    bool whenMemberHolds(Decl* m, ClassDecl* c, const TemplateArgs& args, const Ty* key, Node* at);

