// struct Emitter's members: exceptions, Windows' model: the declarations, part of the struct's
// body, included in it in Codegen/Emitter.hpp; the definitions in Exceptions.cpp (codegen.cpp
// split, stage 2, 2026-11-05, moved without a change).
    // --- exceptions ---

    // The catchpad token of the handler the code being emitted belongs to, or "" outside handlers.
    std::string currentFunclet();

    // The operand bundle a call inside a funclet carries.
    std::string bundle();

    // The personality of a function with exception handling: SEH's on Windows, kpp's own (main.cpp's
    // itaniumRuntime) on the other targets.
    std::string personality() const { return sh.target.windows() ? "@__C_specific_handler" : "@__kpp_personality_v0"; }

    bool handlerDeclared = false; // this emitter saw the personality declared
    void markEH();

    // Emits `call` or, when an exception leaving the call has to run destructors or reach a handler
    // of this function, `invoke`. prefix is "" or "%r = "; callee is the text after `call`.
    void emitCall(const std::string& prefix, const std::string& callee, bool mayThrow = true);

    // Where an exception thrown at this point unwinds to: a cleanup pad destroying the objects
    // constructed since the innermost try or handler began (shared by all calls with the same
    // objects), the try's dispatch, or "" when nothing in this function has to happen.
    std::string unwindDestination();

    // Windows: a noexcept function's outermost catchswitch, made on first use. Its one catchpad's
    // filter, kpp.eh.filter.noexcept, ends the program in the search when a kpp exception reaches it
    // (finishExceptionRuntime): every call of the body that may throw unwinds here, through the
    // cleanups and the handlers of its trys, so no exception leaves the function and nothing between
    // the throw and it is destroyed first. The handler is never entered.
    std::string noexceptDispatch();

    // The pad destroying objects[from..] (the newest first) and then unwinding to next ("": out of
    // the function or funclet): it destroys objects[from] and continues into the pad of the rest,
    // which every call whose live objects end the same way shares. A constructor's members so cost
    // one destructor call each, not one per member built before them: ControlSettings' constructor
    // was 875 k lines of IR with 482 k calls of ~String, a quarter of clang's critical path.
    std::string cleanupChain(const std::vector<Object>& objects, size_t from, const std::string& next, const std::string& parent);

