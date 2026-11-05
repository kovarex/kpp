// struct Emitter's members: temporaries and scopes: the declarations, part of the struct's body,
// included in it in Codegen/Emitter.hpp; the definitions in Scopes.cpp (codegen.cpp split, stage 2,
// 2026-11-05, moved without a change).
    // --- statements and scopes ---

    // (Each object leaves the list as it is destroyed: a destructor that throws unwinds through the
    // objects built before it alone.)
    void destroyTemps();

    // Destroys the temporaries made since mark: those of one branch of `?:` or of the right
    // operand of `&&`/`||`, which exist only when that branch ran, so they die inside it.
    void destroyTempsFrom(size_t mark);

    void destroyObject(const Object& o);

    // What a normal exit (the end of a scope or a statement, a return, a break) destroys: not what
    // only an exception leaving does (Object::unwindOnly).
    void destroyOnExit(const Object& o);

    // The body of an if, else or loop: in a block of its own when it is not one (C++ [stmt.pre]), so
    // that a declaration as the whole body (TargeterBase::save's `if (entity) TagWriter _(...); else ...`)
    // is built and destroyed in its branch or iteration, not at the end of the enclosing block.
    void subStatement(Stmt* s);

    void pushScope() { scopes.emplace_back(); }

    void popScope();

    // Destroys the objects of the innermost scopes down to (not including) depth.
    void unwindTo(size_t depth) { unwindRange(depth, scopes.size()); }

    // Destroys the objects of scopes [low, high), innermost first; the scopes stay open (each object
    // leaves them while it is destroyed, and they are as they were after).
    void unwindRange(size_t low, size_t high);

    // Destroys the scopes above depth for a `return`, `break` or `continue`, leaving the catch
    // handlers in between through catchret: a funclet cannot branch out directly.
    void leaveScopes(size_t depth);

