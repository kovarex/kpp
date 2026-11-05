// struct Emitter's members: exceptions off Windows (the Itanium model); throw, try and catch; the
// entry: the declarations, part of the struct's body, included in it in Codegen/Emitter.hpp; the
// definitions in ItaniumExceptions.cpp (codegen.cpp split, stage 2, 2026-11-05, moved without a
// change).
    // --- exceptions off Windows: the Itanium model (docs/design.md, "Targets and hosts", stage 3) ---
    // A call that may throw is an invoke whose unwind edge is a landing pad: `landingpad cleanup` with
    // the catch clauses of every try around the call in this function, innermost first, and in a
    // noexcept function the empty filter. kpp's personality (main.cpp's itaniumRuntime) matches the
    // clauses in the search, phase 1. The pad keeps the exception and the selector (the index of the
    // clause matched in this frame, 0 for its cleanups alone) in two slots of the function, and goes
    // on to a chain of plain blocks that destroy what the call leaves, newest first, shared as
    // cleanupChain's pads are. The chain ends at the innermost try's dispatch or at `resume`.

    // The function's two slots, made on first use: %eh.exc, the exception (the unwinder's header),
    // and %eh.sel, the selector.
    void itaniumSlots();

    // The block that goes on unwinding to the caller with the exception in the slots.
    std::string itaniumResume();

    // The landing pad of the calls a cleanup block makes: a destructor that throws while an exception
    // unwinds ends the program (C++'s std::terminate), found in the search at the empty filter.
    std::string itaniumTerminatePad();

    // Whether the body being emitted is a noexcept function's own (`noexcept`, or `noexcept(cond)` with
    // cond true): no exception leaves it. Not a lambda's written in it, nor a helper emitted while it is
    // (currentFunction stays the writer for those: B's review): only the body of currentFunction itself.
    bool emittingNoexcept();

    // The chain destroying objects[from..] (the newest first), then going on to next (a try's dispatch
    // or the chain of an enclosing scope's objects), or to resume when next is "".
    std::string itaniumChain(const std::vector<Object>& objects, size_t from, const std::string& next);

    // The landing pad of a call whose exception goes on to head (a chain block or a try's dispatch).
    std::string itaniumLandingPad(const std::string& head);

    // Where the exception of a call at this point goes: the objects constructed since the innermost
    // try or handler began, then that try's dispatch (or, out of a handler, where its try's dispatch
    // goes when no handler takes the exception). "" when nothing in this function has to happen.
    std::vector<Object> itaniumObjects();

    std::string itaniumUnwindDestination();

    std::string sharedComdat(const std::string& name) { return "$" + name.substr(1) + " = comdat any\n"; }

    // The type descriptor matched against catch clauses, as a class descriptor is (descriptorFor):
    // { name, number of direct bases, [{ base's descriptor, base's offset }] }, so that a handler
    // takes any base of the thrown class, with that base's offset. A pointer's lists what it
    // converts to instead ([except.handle] 3.3), in the order searched: its pointee const, void*,
    // a pointer to each base of a class it points to, at the base's offset.
    std::string typeinfoFor(const Ty* t, Node* at);

    // What a handler of type t is matched against: a class's or a scalar's own descriptor
    // (typeinfoFor), or for a pointer a marker that kpp_eh_offset reads (baseSearchFunction): by
    // value or by const reference, any thrown pointer that converts to it and nullptr ([except.handle]
    // 3.3, 3.4); by a non-const reference, its own type alone (3.1).
    std::string handlerTypeinfo(const Ty* t, Node* at);

    // The destructor slot of a class that declares no destructor: a function destroying its members
    // and bases (destructorThunk), or one doing nothing when none of them needs it.
    const Ty* destructorHelperFor = nullptr; // the class whose destructor helper is being emitted (destructorThunk)

    std::string implicitDestructor(const Ty* t, Node* at);

    // A function destroying an object of type t through a pointer (for the exception runtime), or null.
    std::string destructorThunk(const Ty* t, Node* at);

    // `throw expr`: the object is built on the heap and raised with its descriptor and destructor.
    // `throw;` raises the innermost handler's exception again, which then no longer owns it. The
    // runtime's raise functions are declared as returning (they never do), see below.
    void emitThrow(ThrowExpr* t);

    // try { body } catch (T1 a) {...} catch (...) {...}: the body's calls unwind to a catchswitch
    // whose filter function (run by the OS during the search phase) matches the thrown type against
    // the handlers in order and records the exception for the one that takes it. The handler code
    // is a catchpad funclet reading that record; leaving it normally ends the exception.
    // Off Windows the Itanium model's landing pads and a dispatch block (itaniumTry).
    void tryStatement(TryStmt* t);

    // The first handlerLabels.size() handlers of t, each at its label, with the caught exception's
    // record in slot (the object at offset 32): each binds its parameter, runs, ends the exception
    // (the record, a scope object) and leaves for endL, out of its catchpad on Windows (pad), by a
    // branch elsewhere (pad "").
    void catchHandlers(TryStmt* t, const std::string& slot, const std::vector<std::string>& handlerLabels, const std::string& pad, const std::string& endL);

    // try off Windows (the Itanium model, above unwindDestination): the body's landing pads go on to
    // the dispatch, a plain block that compares the selector with each handler's descriptor's index
    // (llvm.eh.typeid.for), in order; the handler it finds takes the exception into its record
    // (kpp_eh_enter). With none (a cleanup of this frame's on the way to a handler further out), the
    // exception goes on through the objects outside the try to the enclosing try, or to the caller.
    void itaniumTry(TryStmt* t);

    // The lines of the report of an exception nobody caught that print its message (format: ": %s")
    // when it is an Exception: from %object (its address) and %typeinfo, on to %end. Exception's
    // message is a String (core/Utility.kpp): its characters are in the object when its last byte
    // (markIndex, exceptionMessageMark: 31 with 64-bit sizes) is a small string's length, at the
    // pointer in its first word when it is largeMark (0x80; core/String.kpp). Read as a pointer, a
    // small one's characters crashed.
    std::string exceptionMessage(const std::string& format);

    // The lines that call the terminate handler setTerminate installed (the stdlib's `extern "C"
    // kpp_terminate_handler`, platform/), if one is, after the report of an exception that ends the
    // program; the caller's abort follows. A program without the stdlib (--no-std) has none: the name
    // is claimed only by its definition.
    std::string terminateHandlerCall();
    // The runtime's change of the uncaught exception count (the standard library's `extern "C"
    // kpp_uncaught_change`, core/Error.kpp), if one is: +1 in kpp_throw (a rethrow's too), -1 where a
    // handler starts (kpp_eh_begin, kpp_eh_enter), as C++'s std::uncaught_exceptions counts.
    std::string uncaughtChange(int delta);

    // The exception runtime (linkonce_odr, in every module) and the real @main, which calls the
    // program's main and reports an exception nobody caught.
    void finishExceptionRuntime();

    void emitEntry(const std::string& entry, const std::string& mainName);

