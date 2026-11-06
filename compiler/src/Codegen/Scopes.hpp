// struct Emitter's members: temporaries and scopes: the declarations, part of the struct's body,
// included in it in Codegen/Emitter.hpp; the definitions in Scopes.cpp (codegen.cpp split, stage 2,
// 2026-11-05, moved without a change).
    // --- statements and scopes ---

    // (Each object leaves the list as it is destroyed: a destructor that throws unwinds through the
    // objects built before it alone.)
    void destroyTemps();

    // Destroys the temporaries made since mark: a part's own (a member initializer's, a base's).
    void destroyTempsFrom(size_t mark);

    // The temporaries made since mark by one operand that may not run (a branch of `?:`, the right
    // operand of `&&`/`||`): they live to the end of the full-expression, as C++'s do ([class.temporary]
    // 4), each with a flag that says whether its operand ran, so that only those built are destroyed
    // there, or in unwinding (Object::builtFlag). They died with their operand until 2026-11-05: a
    // pointer to one read a destroyed object later in the expression.
    void keepOperandTemps(size_t mark);

    void destroyObject(const Object& o);

    // What a normal exit (the end of a scope or a statement, a return, a break) destroys: not what
    // only an exception leaving does (Object::unwindOnly).
    void destroyOnExit(const Object& o);

    // The body of an if, else or loop: in a block of its own when it is not one (C++ [stmt.pre]), so
    // that a declaration as the whole body (TargeterBase::save's `if (entity) TagWriter _(...); else ...`)
    // is built and destroyed in its branch or iteration, not at the end of the enclosing block.
    void subStatement(Stmt* s);

    void pushScope()
    {
      scopes.emplace_back();
      scopeIds.push_back(nextScopeId++);
      scopeInits.push_back(0);
      scopeEnds.push_back(0);
    }

    // Whether a jump from a point in the scopes fromIds (with fromInits) to one in toIds (with
    // toInits) enters the scope of a variable past its initialization, which C++ makes ill-formed
    // ([stmt.dcl] 3): one declared before the label in a scope the jump enters, or between the two
    // points in the innermost scope they share.
    static bool jumpBypassesInitialization(const std::vector<uint32_t>& fromIds, const std::vector<uint32_t>& fromInits,
                                           const std::vector<uint32_t>& toIds, const std::vector<uint32_t>& toInits)
    {
      size_t common = 0;
      while (common < fromIds.size() && common < toIds.size() && fromIds[common] == toIds[common])
        ++common;
      if (common > 0 && common - 1 < toInits.size() && common - 1 < fromInits.size() && toInits[common - 1] > fromInits[common - 1])
        return true;
      for (size_t d = common; d < toInits.size(); ++d)
        if (toInits[d] > 0)
          return true;
      return false;
    }

    void popScope();

    // --debug-info: what destroys the objects of the scope comes at its block's closing brace, as in
    // C++ debuggers, by whatever way the scope is left: a `return`'s too, and what follows it. Not
    // when a statement that leaves it has no instruction yet (`continue;`, `return;`): the first
    // destruction stays at its line, for a breakpoint there.
    void debugScopeEnd(size_t scope);

    // Destroys the objects of the innermost scopes down to (not including) depth.
    void unwindTo(size_t depth) { unwindRange(depth, scopes.size()); }

    // Destroys the objects of scopes [low, high), innermost first; the scopes stay open (each object
    // leaves them while it is destroyed, and they are as they were after).
    void unwindRange(size_t low, size_t high);

    // Destroys the scopes above depth for a `return`, `break` or `continue`, leaving the catch
    // handlers in between through catchret: a funclet cannot branch out directly.
    void leaveScopes(size_t depth);

