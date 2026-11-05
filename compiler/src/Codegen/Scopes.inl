// struct Emitter's members: temporaries and scopes. Part of codegen.cpp, included in the struct's
// body in Codegen/Emitter.hpp (a file of its own since 2026-11-05, moved without a change).
    // --- statements and scopes ---

    // (Each object leaves the list as it is destroyed: a destructor that throws unwinds through the
    // objects built before it alone.)
    void destroyTemps()
    {
      for (size_t i = statementTemps.size(); i-- > 0;)
      {
        Object o = statementTemps[i];
        statementTemps.resize(i);
        destroyOnExit(o);
      }
      statementTemps.clear();
    }

    // Destroys the temporaries made since mark: those of one branch of `?:` or of the right
    // operand of `&&`/`||`, which exist only when that branch ran, so they die inside it.
    void destroyTempsFrom(size_t mark)
    {
      for (size_t i = statementTemps.size(); i-- > mark;)
      {
        Object o = statementTemps[i];
        statementTemps.resize(i);
        destroyOnExit(o);
      }
      statementTemps.resize(mark);
    }

    void destroyObject(const Object& o)
    {
      if (o.exception)
        fn += "  call void @kpp_eh_end(ptr " + o.addr + ")" + bundle() + "\n";
      else if (o.freeMemory)
      {
        ensureMalloc();
        fn += "  call void @free(ptr " + o.addr + ")" + bundle() + "\n";
      }
      else
        destroy(o.addr, o.ty, nullptr);
    }

    // What a normal exit (the end of a scope or a statement, a return, a break) destroys: not what
    // only an exception leaving does (Object::unwindOnly).
    void destroyOnExit(const Object& o)
    {
      if (!o.unwindOnly && !(!keptOnReturn.empty() && o.addr == keptOnReturn && !o.exception && !o.freeMemory))
        destroyObject(o);
    }

    // The body of an if, else or loop: in a block of its own when it is not one (C++ [stmt.pre]), so
    // that a declaration as the whole body (TargeterBase::save's `if (entity) TagWriter _(...); else ...`)
    // is built and destroyed in its branch or iteration, not at the end of the enclosing block.
    void subStatement(Stmt* s)
    {
      if (!s || s->kind == NodeKind::BlockStmt)
      {
        statement(s);
        return;
      }
      pushScope();
      statement(s);
      popScope();
    }

    void pushScope() { scopes.emplace_back(); }

    void popScope()
    {
      if (!blockTerminated)
        for (size_t i = scopes.back().size(); i-- > 0;)
        {
          Object o = scopes.back()[i];
          scopes.back().resize(i); // (destroyed: a destructor that throws unwinds through those before it)
          destroyOnExit(o);
        }
      scopes.pop_back();
    }

    // Destroys the objects of the innermost scopes down to (not including) depth.
    void unwindTo(size_t depth) { unwindRange(depth, scopes.size()); }

    // Destroys the objects of scopes [low, high), innermost first; the scopes stay open (each object
    // leaves them while it is destroyed, and they are as they were after).
    void unwindRange(size_t low, size_t high)
    {
      std::vector<std::vector<Object>> open(scopes.begin() + low, scopes.begin() + high);
      for (size_t s = high; s-- > low;)
        for (size_t i = scopes[s].size(); i-- > 0;)
        {
          Object o = scopes[s][i];
          scopes[s].resize(i);
          destroyOnExit(o);
        }
      std::move(open.begin(), open.end(), scopes.begin() + low);
    }

    // Destroys the scopes above depth for a `return`, `break` or `continue`, leaving the catch
    // handlers in between through catchret: a funclet cannot branch out directly.
    void leaveScopes(size_t depth)
    {
      std::vector<EHFrame> saved = ehFrames;
      size_t top = scopes.size();
      for (size_t i = ehFrames.size(); i-- > 0;)
      {
        if (!ehFrames[i].isHandler)
          continue;
        if (ehFrames[i].scopeDepth < depth)
          break; // the target is inside this handler
        unwindRange(ehFrames[i].scopeDepth, top);
        top = ehFrames[i].scopeDepth;
        if (!ehFrames[i].funclet.empty()) // (off Windows a handler is plain code: nothing to leave)
        {
          std::string next = newLabel("catchret");
          fn += "  catchret from " + ehFrames[i].funclet + " to label %" + next + "\n";
          startBlock(next);
        }
        ehFrames.resize(i);
      }
      unwindRange(depth, top);
      ehFrames = saved;
    }

