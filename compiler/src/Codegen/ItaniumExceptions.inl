// struct Emitter's members: exceptions off Windows (the Itanium model); throw, try and catch; the
// entry. Part of codegen.cpp, included in the struct's body in Codegen/Emitter.hpp (a file of its
// own since 2026-11-05, moved without a change).
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
    void itaniumSlots()
    {
      if (cleanupPads.emplace("itanium.slots", "").second)
        allocas += "  %eh.exc = alloca ptr\n  %eh.sel = alloca i32\n";
    }

    // The block that goes on unwinding to the caller with the exception in the slots.
    std::string itaniumResume()
    {
      auto found = cleanupPads.find("itanium.resume");
      if (found != cleanupPads.end())
        return found->second;
      itaniumSlots();
      std::string block = newLabel("eh.resume"), exc = newTemp(), sel = newTemp(), pair = newTemp(), full = newTemp();
      cleanupPads["itanium.resume"] = block;
      ehBlocks += block + ":\n  " + exc + " = load ptr, ptr %eh.exc\n  " + sel + " = load i32, ptr %eh.sel\n  " + pair +
                  " = insertvalue { ptr, i32 } poison, ptr " + exc + ", 0\n  " + full + " = insertvalue { ptr, i32 } " + pair + ", i32 " + sel +
                  ", 1\n  resume { ptr, i32 } " + full + "\n";
      return block;
    }

    // The landing pad of the calls a cleanup block makes: a destructor that throws while an exception
    // unwinds ends the program (C++'s std::terminate), found in the search at the empty filter.
    std::string itaniumTerminatePad()
    {
      auto found = cleanupPads.find("itanium.terminate");
      if (found != cleanupPads.end())
        return found->second;
      markEH();
      std::string block = newLabel("eh.terminate"), lp = newTemp();
      cleanupPads["itanium.terminate"] = block;
      ehBlocks += block + ":\n  " + lp + " = landingpad { ptr, i32 } filter [0 x ptr] zeroinitializer\n  unreachable\n";
      return block;
    }

    // Whether the body being emitted is a noexcept function's own (`noexcept`, or `noexcept(cond)` with
    // cond true): no exception leaves it. Not a lambda's written in it, nor a helper emitted while it is
    // (currentFunction stays the writer for those: B's review): only the body of currentFunction itself.
    bool emittingNoexcept()
    {
      static const TemplateArgs none;
      return currentFunction && bodyNow && bodyNow == currentFunction->body && (currentFunction->flags & DF_Noexcept) &&
             global.isNoexcept(currentFunction, currentArgs ? *currentArgs : none);
    }

    // The chain destroying objects[from..] (the newest first), then going on to next (a try's dispatch
    // or the chain of an enclosing scope's objects), or to resume when next is "".
    std::string itaniumChain(const std::vector<Object>& objects, size_t from, const std::string& next)
    {
      if (from == objects.size())
        return next.empty() ? itaniumResume() : next;
      std::string key = "itanium.chain|" + next;
      for (size_t i = from; i < objects.size(); ++i)
        key += "|" + objects[i].addr + (objects[i].freeMemory ? "~" : "") + (objects[i].exception ? "!" : "");
      auto found = cleanupPads.find(key);
      if (found != cleanupPads.end())
        return found->second;
      std::string rest = itaniumChain(objects, from + 1, next);
      std::string block = newLabel("eh.cleanup");
      cleanupPads[key] = block;
      // The block is emitted aside, after the function body.
      std::string savedFn = std::move(fn), savedBlock = currentBlock;
      bool savedTerminated = blockTerminated, savedCleanup = itaniumCleanup;
      fn.clear();
      startBlock(block);
      itaniumCleanup = true;
      destroyObject(objects[from]);
      itaniumCleanup = savedCleanup;
      emitBranch(rest);
      ehBlocks += fn;
      fn = std::move(savedFn);
      currentBlock = savedBlock;
      blockTerminated = savedTerminated;
      return block;
    }

    // The landing pad of a call whose exception goes on to head (a chain block or a try's dispatch).
    std::string itaniumLandingPad(const std::string& head)
    {
      std::string clauses;
      for (size_t i = ehFrames.size(); i-- > 0;)
        if (!ehFrames[i].isHandler)
          for (const std::string& type : ehFrames[i].catchTypes)
            clauses += " catch ptr " + type;
      if (emittingNoexcept())
        clauses += " filter [0 x ptr] zeroinitializer";
      std::string key = "itanium.pad|" + head + "|" + clauses;
      auto found = cleanupPads.find(key);
      if (found != cleanupPads.end())
        return found->second;
      markEH();
      itaniumSlots();
      std::string block = newLabel("eh.pad"), lp = newTemp(), exc = newTemp(), sel = newTemp();
      cleanupPads[key] = block;
      ehBlocks += block + ":\n  " + lp + " = landingpad { ptr, i32 } cleanup" + clauses + "\n  " + exc + " = extractvalue { ptr, i32 } " + lp +
                  ", 0\n  store ptr " + exc + ", ptr %eh.exc\n  " + sel + " = extractvalue { ptr, i32 } " + lp + ", 1\n  store i32 " + sel +
                  ", ptr %eh.sel\n  br label %" + head + "\n";
      return block;
    }

    // Where the exception of a call at this point goes: the objects constructed since the innermost
    // try or handler began, then that try's dispatch (or, out of a handler, where its try's dispatch
    // goes when no handler takes the exception). "" when nothing in this function has to happen.
    std::vector<Object> itaniumObjects()
    {
      size_t limit = ehFrames.empty() ? 0 : ehFrames.back().scopeDepth;
      std::vector<Object> objects;
      for (size_t i = statementTemps.size(); i-- > 0;)
        objects.push_back(statementTemps[i]);
      for (size_t s = scopes.size(); s-- > limit;)
        for (size_t i = scopes[s].size(); i-- > 0;)
          objects.push_back(scopes[s][i]);
      return objects;
    }

    std::string itaniumUnwindDestination()
    {
      if (itaniumCleanup)
        return itaniumTerminatePad();
      std::vector<Object> objects = itaniumObjects();
      std::string next;
      if (!ehFrames.empty())
      {
        next = ehFrames.back().dispatchLabel;
        if (!ehFrames.back().isHandler)
          ehFrames.back().used = true;
      }
      if (objects.empty() && next.empty() && !emittingNoexcept())
        return "";
      return itaniumLandingPad(itaniumChain(objects, 0, next));
    }

    std::string sharedComdat(const std::string& name) { return "$" + name.substr(1) + " = comdat any\n"; }

    // The type descriptor matched against catch clauses, as a class descriptor is (descriptorFor):
    // { name, number of direct bases, [{ base's descriptor, base's offset }] }, so that a handler
    // takes any base of the thrown class, with that base's offset.
    std::string typeinfoFor(const Ty* t, Node* at)
    {
      t = types.unqualified(types.stripRef(t));
      auto lock = sharedLock(); // the descriptors emitted, the library's Exception's
      std::string name, display, list;
      size_t count = 0;
      if (isClass(t))
      {
        const StructInfo& info = structFor(t, at);
        name = "@kpp.ti." + info.name.substr(8);
        display = types.toString(t, &symbols);
        std::string structName = info.name;
        std::vector<std::pair<Decl*, uint32_t>> bases = info.bases;
        std::vector<const Ty*> fieldTypes = info.fieldTypes;
        for (auto& [baseDef, index] : bases)
          list += (count++ ? ", " : "") + std::string("{ ptr, i64 } { ptr ") + typeinfoFor(fieldTypes[index], at) +
                  ", i64 ptrtoint (ptr getelementptr (" + structName + ", ptr null, i32 0, i32 " + std::to_string(index) + ") to i64) }";
        if (display == "Exception" && exceptionTypeinfo.empty())
        {
          exceptionTypeinfo = name;
          // Its message, its first field, is a String: the last of its bytes is the length of a small
          // one or the large mark (String.kpp's markIndex), wherever String's sizes put it.
          if (!fieldTypes.empty() && bases.empty() && types.toString(fieldTypes[0], &symbols) == "String")
            exceptionMessageMark = sizeOf(fieldTypes[0], at) - 1;
        }
      }
      else
      {
        display = types.toString(t, &symbols);
        name = "@\"kpp.ti." + display + "\""; // one descriptor per scalar type, pointers by pointee
      }
      if (emittedShared[name])
        return name;
      emittedShared[name] = true;
      bool was = emittingShared;
      emittingShared = true;
      std::string str = cStringConstant(display);
      emittingShared = was;
      std::string arrayType = "[" + std::to_string(count) + " x { ptr, i64 }]";
      shared += sharedComdat(name) + name + " = linkonce_odr constant { ptr, i64, " + arrayType + " } { ptr " + str + ", i64 " +
                std::to_string(count) + ", " + arrayType + " " + (count ? "[" + list + "]" : std::string("zeroinitializer")) + " }, comdat\n";
      return name;
    }

    // The destructor slot of a class that declares no destructor: a function destroying its members
    // and bases (destructorThunk), or one doing nothing when none of them needs it.
    const Ty* destructorHelperFor = nullptr; // the class whose destructor helper is being emitted (destructorThunk)

    std::string implicitDestructor(const Ty* t, Node* at)
    {
      std::string thunk = destructorThunk(t, at);
      if (thunk != "null")
        return thunk;
      auto lock = sharedLock();
      if (!emittedShared["@kpp.nodtor"])
      {
        emittedShared["@kpp.nodtor"] = true;
        shared += sharedComdat("@kpp.nodtor") + "define linkonce_odr void @kpp.nodtor(ptr %o) comdat {\nentry:\n  ret void\n}\n";
      }
      return "@kpp.nodtor";
    }

    // A function destroying an object of type t through a pointer (for the exception runtime), or null.
    std::string destructorThunk(const Ty* t, Node* at)
    {
      if (!needsDestruction(t, at))
        return "null";
      std::string name = "@kpp.dtor." + structFor(t, at).name.substr(8);
      auto lock = sharedLock(); // held while the thunk is emitted: one emitter writes it
      if (emittedShared[name])
        return name;
      emittedShared[name] = true;
      FunctionState saved = saveState();
      std::string savedOverride = funcletOverride;
      const Ty* savedHelperFor = destructorHelperFor;
      destructorHelperFor = types.stripRef(t); // (its own body spells its destruction out)
      funcletOverride.clear();
      resetFunctionState();
      try
      {
        destroy("%o", t, at);
        fn += "  ret void\n";
        shared += sharedComdat(name) + "define linkonce_odr void " + name + "(ptr %o) comdat " + (usesEH ? "personality ptr " + personality() + " " : std::string()) +
                  "{\nentry:\n" + allocas + fn + ehBlocks + "}\n";
      }
      catch (Unsupported&)
      {
        restoreState(saved);
        funcletOverride = savedOverride;
        destructorHelperFor = savedHelperFor;
        ++sh.failuresRemembered; // (marked emitted: its later uses do not fail)
        throw;
      }
      restoreState(saved);
      funcletOverride = savedOverride;
      destructorHelperFor = savedHelperFor;
      return name;
    }

    // `throw expr`: the object is built on the heap and raised with its descriptor and destructor.
    // `throw;` raises the innermost handler's exception again, which then no longer owns it. The
    // runtime's raise functions are declared as returning (they never do), see below.
    void emitThrow(ThrowExpr* t)
    {
      usesExceptions = true;
      if (!t->operand)
      {
        const EHFrame* handler = nullptr;
        for (size_t i = ehFrames.size(); i-- > 0 && !handler;)
          if (ehFrames[i].isHandler)
            handler = &ehFrames[i];
        std::string slot;
        if (handler)
          slot = handler->exceptionSlot;
        else
        {
          // In a function a handler calls (an exception dispatcher's `try { throw; } catch ...`):
          // the exception that handler holds, kpp.eh.current; with none, a wrong program, stopped.
          slot = newTemp();
          fn += "  " + slot + " = load ptr, ptr @kpp.eh.current\n";
          if (declaredNames.claim("@llvm.trap"))
            decls += "declare void @llvm.trap()\n";
          std::string none = newTemp(), okL = newLabel("rethrow"), noneL = newLabel("rethrow.none");
          fn += "  " + none + " = icmp eq ptr " + slot + ", null\n";
          fn += "  br i1 " + none + ", label %" + noneL + ", label %" + okL + "\n";
          startBlock(noneL);
          fn += "  call void @llvm.trap()\n  unreachable\n";
          blockTerminated = true;
          startBlock(okL);
        }
        destroyTemps();
        emitCall("", "void @kpp_eh_rethrow(ptr " + slot + ")");
      }
      else
      {
        const Ty* ty = t->operand->ty ? types.unqualified(types.stripRef(t->operand->ty)) : nullptr;
        if (!ty || !ty->isKnown() || types.isDependent(ty))
          unsupported(t, "throwing a value of unknown type");
        if (ty->kind == TyKind::Array)
          ty = types.pointer(ty->inner);
        ensureMalloc();
        std::string obj = newTemp();
        emitCall(obj + " = ", "ptr @malloc(i64 " + std::to_string(sizeOf(ty, t)) + ")", false);
        if (isClass(ty))
          initializeFrom(obj, ty, t->operand, t);
        else
          store(convert(rvalue(t->operand), ty, t), obj, ty, t);
        std::string ti = typeinfoFor(ty, t), dtor = destructorThunk(ty, t);
        destroyTemps();
        emitCall("", "void @kpp_throw(ptr " + obj + ", ptr " + ti + ", ptr " + dtor + ")");
      }
      // No `unreachable` here, on purpose: the code after a throw stays, so that the optimizer never
      // proves a function noreturn. The SEH scope tables need an instruction after every throwing
      // call; a catch handler placed right at a call's return address would make the runtime skip
      // the cleanups between them, taking the unwind for a jump within the scope.
    }

    // try { body } catch (T1 a) {...} catch (...) {...}: the body's calls unwind to a catchswitch
    // whose filter function (run by the OS during the search phase) matches the thrown type against
    // the handlers in order and records the exception for the one that takes it. The handler code
    // is a catchpad funclet reading that record; leaving it normally ends the exception.
    // Off Windows the Itanium model's landing pads and a dispatch block (itaniumTry).
    void tryStatement(TryStmt* t)
    {
      usesExceptions = true;
      if (!sh.target.windows())
      {
        itaniumTry(t);
        return;
      }
      markEH();
      std::string endL = newLabel("try.end"), dispatchL = newLabel("catch.dispatch"), handlerL = newLabel("catch.pad");
      ehFrames.push_back({false, dispatchL, currentFunclet(), "", "", scopes.size()});
      statement(t->body);
      emitBranch(endL);
      EHFrame frame = ehFrames.back();
      ehFrames.pop_back();
      if (!frame.used)
      {
        startBlock(endL); // nothing in the body can throw to the handlers
        return;
      }
      // The filter function, named by the function and where the try is written (the same in every
      // run, whatever order the functions are emitted in).
      std::string owner = currentFunctionName.size() > 2 && currentFunctionName[1] == '"' ? currentFunctionName.substr(2, currentFunctionName.size() - 3)
                                                                                          : currentFunctionName.substr(currentFunctionName.empty() ? 0 : 1);
      std::string filter = "@\"kpp.filter." + owner + "." + std::to_string(t->token) + "\"";
      std::string ft = "define i32 " + filter + "(ptr %pointers, ptr %frame) {\nentry:\n"
                       "  %rec = load ptr, ptr %pointers\n  %code = load i32, ptr %rec\n"
                       "  %tip = getelementptr inbounds i8, ptr %rec, i64 40\n  %ti = load ptr, ptr %tip\n"
                       "  %is = icmp eq i32 %code, -536870911\n  br i1 %is, label %check0, label %no\n";
      for (size_t i = 0; i < t->handlers.size(); ++i)
      {
        std::string n = std::to_string(i), nextL = i + 1 < t->handlers.size() ? "check" + std::to_string(i + 1) : "no";
        ft += "check" + n + ":\n";
        if (!t->handlers[i].param)
        {
          ft += "  call void @kpp_eh_push(ptr %rec, i32 " + n + ", i64 0)\n  ret i32 1\n";
          break;
        }
        const Ty* pt = declType(t->handlers[i].param);
        if (!pt || !pt->isKnown())
          unsupported(t->handlers[i].param, "a catch parameter of unknown type");
        ft += "  %off" + n + " = call i64 @kpp_eh_offset(ptr %ti, ptr " + typeinfoFor(pt, t) + ")\n"
              "  %ok" + n + " = icmp sge i64 %off" + n + ", 0\n  br i1 %ok" + n + ", label %yes" + n + ", label %" + nextL + "\n"
              "yes" + n + ":\n  call void @kpp_eh_push(ptr %rec, i32 " + n + ", i64 %off" + n + ")\n  ret i32 1\n";
      }
      ft += "no:\n  ret i32 0\n}\n\n";
      addDefinition(filter, ft);
      // The dispatch and the handlers.
      std::string outer = unwindDestination(); // objects outside the try, then the enclosing try
      startBlock(dispatchL);
      std::string cs = newTemp(), pad = newTemp();
      fn += "  " + cs + " = catchswitch within " + (frame.parentFunclet.empty() ? "none" : frame.parentFunclet) + " [label %" + handlerL +
            "] unwind " + (outer.empty() ? "to caller" : "label %" + outer) + "\n";
      startBlock(handlerL);
      fn += "  " + pad + " = catchpad within " + cs + " [ptr " + filter + "]\n";
      std::string slot = "%v" + std::to_string(temp++);
      allocas += "  " + slot + " = alloca [6 x i64]\n"; // object, typeinfo, destructor, handler, adjusted object, the handler around this one (kpp.eh.current)
      ehFrames.push_back({true, outer, frame.parentFunclet, pad, slot, scopes.size()});
      fn += "  call void @kpp_eh_begin(ptr " + slot + ")" + bundle() + "\n";
      std::vector<std::string> handlerLabels;
      for (size_t i = 0; i < t->handlers.size(); ++i)
        handlerLabels.push_back(newLabel("catch.body"));
      if (t->handlers.size() == 1)
        emitBranch(handlerLabels[0]);
      else
      {
        std::string hp = newTemp(), h = newTemp();
        fn += "  " + hp + " = getelementptr inbounds i8, ptr " + slot + ", i64 24\n  " + h + " = load i64, ptr " + hp + "\n";
        fn += "  switch i64 " + h + ", label %" + handlerLabels[0] + " [";
        for (size_t i = 1; i < t->handlers.size(); ++i)
          fn += " i64 " + std::to_string(i) + ", label %" + handlerLabels[i];
        fn += " ]\n";
        blockTerminated = true;
      }
      catchHandlers(t, slot, handlerLabels, pad, endL);
      ehFrames.pop_back();
      startBlock(endL);
    }

    // The first handlerLabels.size() handlers of t, each at its label, with the caught exception's
    // record in slot (the object at offset 32): each binds its parameter, runs, ends the exception
    // (the record, a scope object) and leaves for endL, out of its catchpad on Windows (pad), by a
    // branch elsewhere (pad "").
    void catchHandlers(TryStmt* t, const std::string& slot, const std::vector<std::string>& handlerLabels, const std::string& pad,
                       const std::string& endL)
    {
      for (size_t i = 0; i < handlerLabels.size(); ++i)
      {
        const TryStmt::Handler& h = t->handlers[i];
        startBlock(handlerLabels[i]);
        pushScope();
        scopes.back().push_back({slot, nullptr, true});
        if (h.param)
        {
          const Ty* pt = declType(h.param);
          std::string ap = newTemp(), obj = newTemp();
          fn += "  " + ap + " = getelementptr inbounds i8, ptr " + slot + ", i64 32\n  " + obj + " = load ptr, ptr " + ap + "\n";
          if (pt->isReference())
          {
            std::string ref = "%v" + std::to_string(temp++);
            allocas += "  " + ref + " = alloca ptr\n";
            fn += "  store ptr " + obj + ", ptr " + ref + "\n";
            locals[h.param] = {ref, pt, true};
          }
          else if (isClass(pt))
          {
            std::string copy = newObjectSlot(pt, h.param);
            copyConstruct(copy, obj, pt, h.param);
            locals[h.param] = {copy, pt, false};
            if (needsDestruction(pt, h.param))
              scopes.back().push_back({copy, pt});
          }
          else
          {
            std::string v = "%v" + std::to_string(temp++), loaded = newTemp();
            allocas += "  " + v + " = alloca " + llType(pt, h.param) + "\n";
            fn += "  " + loaded + " = load " + llType(pt, h.param) + ", ptr " + obj + "\n  store " + llType(pt, h.param) + " " + loaded + ", ptr " + v + "\n";
            locals[h.param] = {v, pt, false};
          }
        }
        statement(h.body);
        popScope();
        if (!blockTerminated && pad.empty())
          emitBranch(endL);
        else if (!blockTerminated)
        {
          fn += "  catchret from " + pad + " to label %" + endL + "\n";
          blockTerminated = true;
        }
      }
    }

    // try off Windows (the Itanium model, above unwindDestination): the body's landing pads go on to
    // the dispatch, a plain block that compares the selector with each handler's descriptor's index
    // (llvm.eh.typeid.for), in order; the handler it finds takes the exception into its record
    // (kpp_eh_enter). With none (a cleanup of this frame's on the way to a handler further out), the
    // exception goes on through the objects outside the try to the enclosing try, or to the caller.
    void itaniumTry(TryStmt* t)
    {
      std::string endL = newLabel("try.end"), dispatchL = newLabel("catch.dispatch");
      EHFrame frame{false, dispatchL, "", "", "", scopes.size()};
      for (const TryStmt::Handler& h : t->handlers)
      {
        if (!h.param)
        {
          frame.catchTypes.push_back("null");
          break; // (a handler after catch (...) is never reached)
        }
        const Ty* pt = declType(h.param);
        if (!pt || !pt->isKnown())
          unsupported(h.param, "a catch parameter of unknown type");
        frame.catchTypes.push_back(typeinfoFor(pt, t));
      }
      ehFrames.push_back(frame);
      statement(t->body);
      emitBranch(endL);
      frame = ehFrames.back();
      ehFrames.pop_back();
      if (!frame.used)
      {
        startBlock(endL); // nothing in the body can throw to the handlers
        return;
      }
      // Where the exception goes on when no handler takes it: the objects since the enclosing try or
      // handler, then its dispatch (or where it goes), else the caller.
      std::vector<Object> objects = itaniumObjects();
      std::string next = ehFrames.empty() ? std::string() : ehFrames.back().dispatchLabel;
      if (!ehFrames.empty() && !ehFrames.back().isHandler)
        ehFrames.back().used = true;
      std::string outer = itaniumChain(objects, 0, next);
      itaniumSlots();
      if (declaredNames.claim("@llvm.eh.typeid.for.p0"))
        decls += "declare i32 @llvm.eh.typeid.for.p0(ptr)\n";
      startBlock(dispatchL);
      std::string sel = newTemp();
      fn += "  " + sel + " = load i32, ptr %eh.sel\n";
      std::string slot = "%v" + std::to_string(temp++);
      allocas += "  " + slot + " = alloca [7 x i64]\n"; // object, typeinfo, destructor, handler, adjusted object, the handler around this one, the exception's header
      std::vector<std::string> handlerLabels, enterLabels;
      for (size_t i = 0; i < frame.catchTypes.size(); ++i)
      {
        handlerLabels.push_back(newLabel("catch.body"));
        enterLabels.push_back(newLabel("catch.enter"));
      }
      for (size_t i = 0; i < frame.catchTypes.size(); ++i)
      {
        std::string id = newTemp(), is = newTemp(), nextL = i + 1 < frame.catchTypes.size() ? newLabel("catch.next") : outer;
        fn += "  " + id + " = call i32 @llvm.eh.typeid.for.p0(ptr " + frame.catchTypes[i] + ")\n  " + is + " = icmp eq i32 " + sel + ", " + id +
              "\n  br i1 " + is + ", label %" + enterLabels[i] + ", label %" + nextL + "\n";
        blockTerminated = true;
        startBlock(enterLabels[i]);
        std::string exc = newTemp();
        fn += "  " + exc + " = load ptr, ptr %eh.exc\n  call void @kpp_eh_enter(ptr " + slot + ", ptr " + exc + ", i32 " + std::to_string(i) +
              ", ptr " + frame.catchTypes[i] + ")\n";
        emitBranch(handlerLabels[i]);
        if (nextL != outer)
          startBlock(nextL);
      }
      ehFrames.push_back({true, outer, "", "", slot, scopes.size()});
      catchHandlers(t, slot, handlerLabels, "", endL);
      ehFrames.pop_back();
      startBlock(endL);
    }

    // The lines of the report of an exception nobody caught that print its message (format: ": %s")
    // when it is an Exception: from %object (its address) and %typeinfo, on to %end. Exception's
    // message is a String (core/Utility.kpp): its characters are in the object when its last byte
    // (markIndex, exceptionMessageMark: 31 with 64-bit sizes) is a small string's length, at the
    // pointer in its first word when it is largeMark (0x80; core/String.kpp). Read as a pointer, a
    // small one's characters crashed.
    std::string exceptionMessage(const std::string& format)
    {
      if (exceptionTypeinfo.empty() || !exceptionMessageMark)
        return "  br label %end\n";
      return "  %off = call i64 @kpp_eh_offset(ptr %typeinfo, ptr " + exceptionTypeinfo + ")\n  %isExc = icmp sge i64 %off, 0\n"
             "  br i1 %isExc, label %msg, label %end\n"
             "msg:\n  %ep = getelementptr inbounds i8, ptr %object, i64 %off\n"
             "  %markp = getelementptr inbounds i8, ptr %ep, i64 " + std::to_string(exceptionMessageMark) + "\n  %mark = load i8, ptr %markp\n"
             "  %large = icmp eq i8 %mark, -128\n  br i1 %large, label %heap, label %print\n"
             "heap:\n  %heapChars = load ptr, ptr %ep\n  %nodata = icmp eq ptr %heapChars, null\n  br i1 %nodata, label %end, label %print\n"
             "print:\n  %data = phi ptr [ %ep, %msg ], [ %heapChars, %heap ]\n"
             "  %r2 = call i32 (ptr, ptr, ...) @fprintf(ptr %err, ptr " + format + ", ptr %data)\n  br label %end\n";
    }

    // The lines that call the terminate handler setTerminate installed (the stdlib's `extern "C"
    // kpp_terminate_handler`, platform/), if one is, after the report of an exception that ends the
    // program; the caller's abort follows. A program without the stdlib (--no-std) has none: the name
    // is claimed only by its definition.
    std::string terminateHandlerCall()
    {
      if (!declaredNames.contains("@kpp_terminate_handler"))
        return "";
      return "  %handler = call ptr @kpp_terminate_handler()\n  %unset = icmp eq ptr %handler, null\n  br i1 %unset, label %abort, label %call\n"
             "call:\n  call void %handler()\n  br label %abort\nabort:\n";
    }

    // The exception runtime (linkonce_odr, in every module) and the real @main, which calls the
    // program's main and reports an exception nobody caught.
    void finishExceptionRuntime()
    {
      auto declareOnce = [&](const char* name, const std::string& line)
      {
        if (declaredNames.claim(name))
          decls += line + "\n";
      };
      if (usesExceptions && !sh.target.windows())
      {
        // Off Windows, the Itanium model (itaniumUnwindDestination, itaniumTry, main.cpp's
        // itaniumRuntime). kpp_throw puts the object, its descriptor and its destructor after the
        // unwinder's 32-byte header, class "KPP\0KPP\0", and raises it. _Unwind_RaiseException returns
        // only when the search finds no handler: kpp_eh_terminate then reports it, as the personality
        // does for a noexcept function in the way, with the throw site still on the stack (kovarex,
        // 2026-11-03), and aborts. A handler takes the exception into its record (kpp_eh_enter), whose
        // end destroys and frees the object and frees the header.
        ensureMalloc();
        declareOnce("@fprintf", "declare i32 @fprintf(ptr, ptr, ...)");
        declareOnce("@fflush", "declare i32 @fflush(ptr)");
        declareOnce("@abort", "declare void @abort()");
        declareOnce("@_Unwind_RaiseException", "declare i32 @_Unwind_RaiseException(ptr)");
        emittingShared = true;
        std::string uncaught = cStringConstant("unhandled exception of type %s"), throughNoexcept = cStringConstant("exception of type %s thrown through a noexcept function"),
                    unlisted = cStringConstant("exception of type %s thrown from a call its function's exception table does not list"),
                    foreign = cStringConstant("unhandled exception of another language"), noMemory = cStringConstant("no memory for an exception\n"),
                    fmt2 = cStringConstant(": %s"), fmt3 = cStringConstant("\n");
        emittingShared = false;
        std::string err = stderrStream();
        decls += sharedComdat("@kpp.eh.current") + kppEhCurrentLine + "\n";
        shared += baseSearchFunction("@kpp_eh_offset");
        std::string message = exceptionMessage(fmt2), handler = terminateHandlerCall();
        shared += sharedComdat("@kpp_throw") +
                  std::string(kppThrowHeader) + "\nentry:\n"
                  "  %h = call ptr @malloc(i64 64)\n  %none = icmp eq ptr %h, null\n  br i1 %none, label %nomemory, label %raise\n"
                  "nomemory:\n  %flushed = call i32 @fflush(ptr null)\n" + err +
                  "  %r0 = call i32 (ptr, ptr, ...) @fprintf(ptr %err, ptr " + noMemory + ")\n  call void @abort()\n  unreachable\n"
                  "raise:\n  store i64 21198927785840720, ptr %h\n" // "KPP\0KPP\0"
                  "  %cleanup = getelementptr inbounds i8, ptr %h, i64 8\n  store ptr null, ptr %cleanup\n"
                  "  %private = getelementptr inbounds i8, ptr %h, i64 16\n  call void @llvm.memset.p0.i64(ptr %private, i8 0, i64 16, i1 false)\n"
                  "  %op = getelementptr inbounds i8, ptr %h, i64 32\n  store ptr %object, ptr %op\n"
                  "  %tp = getelementptr inbounds i8, ptr %h, i64 40\n  store ptr %typeinfo, ptr %tp\n"
                  "  %dp = getelementptr inbounds i8, ptr %h, i64 48\n  store ptr %dtor, ptr %dp\n"
                  "  %r = call i32 @_Unwind_RaiseException(ptr %h)\n  call void @kpp_eh_terminate(ptr %h, i32 1)\n  unreachable\n}\n";
        declareOnce("@llvm.memset.p0.i64", "declare void @llvm.memset.p0.i64(ptr, i8, i64, i1)");
        shared += sharedComdat("@kpp_eh_terminate") +
                  std::string(kppEhTerminateHeader) + "\nentry:\n"
                  "  %flushed = call i32 @fflush(ptr null)\n" + // (the program's output first: abort writes no buffer out)
                  err +
                  "  %class = load i64, ptr %h\n  %isKpp = icmp eq i64 %class, 21198927785840720\n  br i1 %isKpp, label %kpp, label %foreign\n"
                  "foreign:\n  %r0 = call i32 (ptr, ptr, ...) @fprintf(ptr %err, ptr " + foreign + ")\n  br label %end\n"
                  "kpp:\n  %op = getelementptr inbounds i8, ptr %h, i64 32\n  %object = load ptr, ptr %op\n"
                  "  %tp = getelementptr inbounds i8, ptr %h, i64 40\n  %typeinfo = load ptr, ptr %tp\n"
                  "  %isNoexcept = icmp eq i32 %why, 2\n  %isUnlisted = icmp eq i32 %why, 3\n"
                  "  %fmt0 = select i1 %isNoexcept, ptr " + throughNoexcept + ", ptr " + uncaught + "\n"
                  "  %fmt = select i1 %isUnlisted, ptr " + unlisted + ", ptr %fmt0\n"
                  "  %name = load ptr, ptr %typeinfo\n  %r1 = call i32 (ptr, ptr, ...) @fprintf(ptr %err, ptr %fmt, ptr %name)\n" + message +
                  "end:\n  %r3 = call i32 (ptr, ptr, ...) @fprintf(ptr %err, ptr " + fmt3 + ")\n" + handler + "  call void @abort()\n  unreachable\n}\n";
        shared += sharedComdat("@kpp_eh_enter") +
                  std::string(kppEhEnterHeader) + "\nentry:\n"
                  "  %op = getelementptr inbounds i8, ptr %h, i64 32\n  %object = load ptr, ptr %op\n"
                  "  %tp = getelementptr inbounds i8, ptr %h, i64 40\n  %ti = load ptr, ptr %tp\n"
                  "  %dp = getelementptr inbounds i8, ptr %h, i64 48\n  %dtor = load ptr, ptr %dp\n"
                  "  %any = icmp eq ptr %target, null\n  br i1 %any, label %join, label %search\n"
                  "search:\n  %found = call i64 @kpp_eh_offset(ptr %ti, ptr %target)\n  br label %join\n"
                  "join:\n  %offset = phi i64 [ 0, %entry ], [ %found, %search ]\n  %adjusted = getelementptr inbounds i8, ptr %object, i64 %offset\n"
                  "  %s1 = getelementptr inbounds i64, ptr %out, i64 1\n  %s2 = getelementptr inbounds i64, ptr %out, i64 2\n"
                  "  %s3 = getelementptr inbounds i64, ptr %out, i64 3\n  %s4 = getelementptr inbounds i64, ptr %out, i64 4\n"
                  "  %s5 = getelementptr inbounds i64, ptr %out, i64 5\n  %s6 = getelementptr inbounds i64, ptr %out, i64 6\n"
                  "  store ptr %object, ptr %out\n  store ptr %ti, ptr %s1\n  store ptr %dtor, ptr %s2\n  %hz = zext i32 %handler to i64\n  store i64 %hz, ptr %s3\n"
                  "  store ptr %adjusted, ptr %s4\n  store ptr %h, ptr %s6\n"
                  "  %outer = load ptr, ptr @kpp.eh.current\n  store ptr %outer, ptr %s5\n  store ptr %out, ptr @kpp.eh.current\n  ret void\n}\n";
        shared += sharedComdat("@kpp_eh_end") +
                  std::string(kppEhEndHeader) + "\nentry:\n"
                  "  %op = getelementptr inbounds i8, ptr %out, i64 40\n  %outer = load ptr, ptr %op\n  store ptr %outer, ptr @kpp.eh.current\n"
                  "  %obj = load ptr, ptr %out\n  %null = icmp eq ptr %obj, null\n  br i1 %null, label %header, label %live\n"
                  "live:\n  %dp = getelementptr inbounds i8, ptr %out, i64 16\n  %dtor = load ptr, ptr %dp\n"
                  "  %nodtor = icmp eq ptr %dtor, null\n  br i1 %nodtor, label %release, label %destroy\n"
                  "destroy:\n  call void %dtor(ptr %obj)\n  br label %release\n"
                  "release:\n  call void @free(ptr %obj)\n  store ptr null, ptr %out\n  br label %header\n"
                  "header:\n  %hp = getelementptr inbounds i8, ptr %out, i64 48\n  %h = load ptr, ptr %hp\n  call void @free(ptr %h)\n"
                  "  store ptr null, ptr %hp\n  ret void\n}\n";
        shared += sharedComdat("@kpp_eh_rethrow") +
                  std::string(kppEhRethrowHeader) + "\nentry:\n"
                  "  %obj = load ptr, ptr %out\n  %tp = getelementptr inbounds i8, ptr %out, i64 8\n  %ti = load ptr, ptr %tp\n"
                  "  %dp = getelementptr inbounds i8, ptr %out, i64 16\n  %dtor = load ptr, ptr %dp\n  store ptr null, ptr %out\n"
                  "  call void @kpp_throw(ptr %obj, ptr %ti, ptr %dtor)\n  ret void\n}\n";
      }
      else if (usesExceptions)
      {
        ensureMalloc();
        declareOnce("@RaiseException", "declare dllimport void @RaiseException(i32, i32, i32, ptr)");
        declareOnce("@fprintf", "declare i32 @fprintf(ptr, ptr, ...)");
        declareOnce("@fflush", "declare i32 @fflush(ptr)");
        declareOnce("@__acrt_iob_func", "declare ptr @__acrt_iob_func(i32)");
        declareOnce("@abort", "declare void @abort()");
        markEH();
        emittingShared = true;
        std::string fmt1 = cStringConstant("unhandled exception of type %s"), fmt2 = cStringConstant(": %s"), fmt3 = cStringConstant("\n");
        emittingShared = false;
        shared += sharedComdat("@kpp.eh.stack") + "@kpp.eh.stack = linkonce_odr thread_local global [160 x i64] zeroinitializer, comdat\n" +
                  sharedComdat("@kpp.eh.depth") + "@kpp.eh.depth = linkonce_odr thread_local global i64 0, comdat\n";
        shared += sharedComdat("@kpp_throw") +
                  std::string(kppThrowHeader) + "\nentry:\n"
                  "  %args = alloca [3 x i64]\n  %a1 = getelementptr inbounds i64, ptr %args, i64 1\n  %a2 = getelementptr inbounds i64, ptr %args, i64 2\n"
                  "  store ptr %object, ptr %args\n  store ptr %typeinfo, ptr %a1\n  store ptr %dtor, ptr %a2\n"
                  "  call void @RaiseException(i32 -536870911, i32 1, i32 3, ptr %args)\n  ret void\n}\n";
        shared += baseSearchFunction("@kpp_eh_offset");
        shared += sharedComdat("@kpp_eh_push") +
                  std::string(kppEhPushHeader) + "\nentry:\n"
                  "  %d = load i64, ptr @kpp.eh.depth\n  %i = mul i64 %d, 5\n  %slot = getelementptr inbounds i64, ptr @kpp.eh.stack, i64 %i\n"
                  "  %info = getelementptr inbounds i8, ptr %record, i64 32\n  %obj = load i64, ptr %info\n"
                  "  %tip = getelementptr inbounds i8, ptr %record, i64 40\n  %ti = load i64, ptr %tip\n"
                  "  %dp = getelementptr inbounds i8, ptr %record, i64 48\n  %dtor = load i64, ptr %dp\n"
                  "  %h = zext i32 %handler to i64\n  %adj = add i64 %obj, %offset\n"
                  "  %s1 = getelementptr inbounds i64, ptr %slot, i64 1\n  %s2 = getelementptr inbounds i64, ptr %slot, i64 2\n"
                  "  %s3 = getelementptr inbounds i64, ptr %slot, i64 3\n  %s4 = getelementptr inbounds i64, ptr %slot, i64 4\n"
                  "  store i64 %obj, ptr %slot\n  store i64 %ti, ptr %s1\n  store i64 %dtor, ptr %s2\n  store i64 %h, ptr %s3\n  store i64 %adj, ptr %s4\n"
                  "  %d1 = add i64 %d, 1\n  store i64 %d1, ptr @kpp.eh.depth\n  ret void\n}\n";
        // A handler's record is the current exception while it runs (kpp.eh.current, the record of
        // the handler around it kept in its sixth word): what `throw;` in a function it calls raises.
        decls += sharedComdat("@kpp.eh.current") + kppEhCurrentLine + "\n";
        shared += sharedComdat("@kpp_eh_begin") +
                  std::string(kppEhBeginHeader) + "\nentry:\n"
                  "  %d = load i64, ptr @kpp.eh.depth\n  %d1 = sub i64 %d, 1\n  store i64 %d1, ptr @kpp.eh.depth\n"
                  "  %i = mul i64 %d1, 5\n  %slot = getelementptr inbounds i64, ptr @kpp.eh.stack, i64 %i\n"
                  "  call void @llvm.memcpy.p0.p0.i64(ptr %out, ptr %slot, i64 40, i1 false)\n"
                  "  %outer = load ptr, ptr @kpp.eh.current\n  %op = getelementptr inbounds i8, ptr %out, i64 40\n  store ptr %outer, ptr %op\n"
                  "  store ptr %out, ptr @kpp.eh.current\n  ret void\n}\n";
        shared += sharedComdat("@kpp_eh_end") +
                  std::string(kppEhEndHeader) + "\nentry:\n"
                  "  %op = getelementptr inbounds i8, ptr %out, i64 40\n  %outer = load ptr, ptr %op\n  store ptr %outer, ptr @kpp.eh.current\n"
                  "  %obj = load ptr, ptr %out\n  %null = icmp eq ptr %obj, null\n  br i1 %null, label %done, label %live\n"
                  "live:\n  %dp = getelementptr inbounds i8, ptr %out, i64 16\n  %dtor = load ptr, ptr %dp\n"
                  "  %nodtor = icmp eq ptr %dtor, null\n  br i1 %nodtor, label %release, label %destroy\n"
                  "destroy:\n  call void %dtor(ptr %obj)\n  br label %release\n"
                  "release:\n  call void @free(ptr %obj)\n  store ptr null, ptr %out\n  br label %done\n"
                  "done:\n  ret void\n}\n";
        shared += sharedComdat("@kpp_eh_rethrow") +
                  std::string(kppEhRethrowHeader) + "\nentry:\n"
                  "  %obj = load ptr, ptr %out\n  %tp = getelementptr inbounds i8, ptr %out, i64 8\n  %ti = load ptr, ptr %tp\n"
                  "  %dp = getelementptr inbounds i8, ptr %out, i64 16\n  %dtor = load ptr, ptr %dp\n  store ptr null, ptr %out\n"
                  "  call void @kpp_throw(ptr %obj, ptr %ti, ptr %dtor)\n  ret void\n}\n";
        shared += sharedComdat("@kpp.eh.filter.any") +
                  std::string(kppEhFilterAnyHeader) + "\nentry:\n"
                  "  %rec = load ptr, ptr %pointers\n  %code = load i32, ptr %rec\n  %is = icmp eq i32 %code, -536870911\n"
                  "  br i1 %is, label %yes, label %no\n"
                  // (no handler took it: the search ends the program here, the throw site on the stack)
                  "yes:\n  %op = getelementptr inbounds i8, ptr %rec, i64 32\n  %obj = load ptr, ptr %op\n"
                  "  %tp = getelementptr inbounds i8, ptr %rec, i64 40\n  %ti = load ptr, ptr %tp\n"
                  "  call void @kpp_eh_unhandled(ptr %obj, ptr %ti)\n  unreachable\nno:\n  ret i32 0\n}\n";
        std::string message = exceptionMessage(fmt2), handler = terminateHandlerCall();
        shared += sharedComdat("@kpp_eh_unhandled") +
                  std::string(kppEhUnhandledHeader) + "\nentry:\n"
                  "  %flushed = call i32 @fflush(ptr null)\n" // (the program's output first: abort writes no buffer out)
                  "  %err = call ptr @__acrt_iob_func(i32 2)\n  %name = load ptr, ptr %typeinfo\n"
                  "  %r1 = call i32 (ptr, ptr, ...) @fprintf(ptr %err, ptr " + fmt1 + ", ptr %name)\n" + message +
                  "end:\n  %r3 = call i32 (ptr, ptr, ...) @fprintf(ptr %err, ptr " + fmt3 + ")\n" + handler + "  call void @abort()\n  unreachable\n}\n";
        declareOnce("@llvm.memcpy.p0.p0.i64", "declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)");
      }
      // @main, or @wmain for a program whose entry takes wide arguments (the game's, on Windows: the
      // C runtime calls whichever the program defines).
      for (const char* entry : {"main", "wmain"})
        if (sh.target.windows() || std::string_view(entry) == "main") // (wmain is Windows' entry alone)
          emitEntry(entry, "@kpp." + std::string(entry));
    }

    void emitEntry(const std::string& entry, const std::string& mainName)
    {
      std::string declare = declareLines[mainName];
      if (moduleCount <= 1)
      {
        std::string definition = body.fromFirst("define i32 " + mainName + "(");
        if (definition.empty())
          return;
        declare = definition.substr(0, definition.find(")") + 1);
      }
      if (declare.empty())
        return;
      bool args = declare.find("(i32, ptr)") != std::string::npos || declare.find("(i32 %p0, ptr %p1)") != std::string::npos;
      std::string call = "i32 " + mainName + (args ? "(i32 %argc, ptr %argv)" : "()");
      // On Windows the program's main runs under a handler whose filter (kpp.eh.filter.any) the search
      // reaches when no handler of the program takes a kpp exception: it reports the exception and ends
      // the program from the search, so the handler itself is never entered. (Off Windows, kpp_throw
      // reports and aborts itself.)
      bool reports = usesExceptions && sh.target.windows();
      std::string text = "define i32 @" + entry + "(i32 %argc, ptr %argv) " + std::string(reports ? "personality ptr @__C_specific_handler " : "") + "{\nentry:\n";
      if (!reports)
        text += "  %r = call " + call + "\n  ret i32 %r\n}\n\n";
      else
        text += "  %r = invoke " + call + " to label %done unwind label %dispatch\n"
                "done:\n  ret i32 %r\n"
                "dispatch:\n  %cs = catchswitch within none [label %handler] unwind to caller\n"
                "handler:\n  %pad = catchpad within %cs [ptr @kpp.eh.filter.any]\n  unreachable\n}\n\n";
      if (moduleCount <= 1)
        body += text;
      else
      {
        Shared::Definition definition = Shared::definitionOf(text);
        auto lock = sharedLock();
        sh.definitions[definedIn[mainName]].push_back(std::move(definition));
      }
    }

