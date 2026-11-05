// struct Emitter's members: function definitions and startup globals. Part of codegen.cpp, included
// in the struct's body in Codegen/Emitter.hpp (a file of its own since 2026-11-05, moved without a
// change).
    // --- function definitions ---

    // Whether emitting f re-types its nodes in place, once per element of a pack expansion (f or
    // its class has a parameter pack) or of a `for constexpr` (design.md 8.2).
    static bool retypedPerElement(FunctionDecl* f)
    {
      for (Decl* p = f; p; p = p->parent)
        for (Decl* tp : p->templateParams)
          if (tp->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(tp)->isPack)
            return true;
      return containsForConstexpr(f->body);
    }
    static bool containsForConstexpr(Stmt* s)
    {
      if (!s)
        return false;
      switch (s->kind)
      {
        case NodeKind::ForConstexprStmt: return true;
        case NodeKind::BlockStmt:
          for (Stmt* inner : static_cast<BlockStmt*>(s)->stmts)
            if (containsForConstexpr(inner))
              return true;
          return false;
        case NodeKind::IfStmt: return containsForConstexpr(static_cast<IfStmt*>(s)->then) || containsForConstexpr(static_cast<IfStmt*>(s)->else_);
        case NodeKind::ForStmt: return containsForConstexpr(static_cast<ForStmt*>(s)->body);
        case NodeKind::RangeForStmt: return containsForConstexpr(static_cast<RangeForStmt*>(s)->body);
        case NodeKind::WhileStmt: return containsForConstexpr(static_cast<WhileStmt*>(s)->body);
        case NodeKind::DoStmt: return containsForConstexpr(static_cast<DoStmt*>(s)->body);
        case NodeKind::SwitchStmt: return containsForConstexpr(static_cast<SwitchStmt*>(s)->body);
        case NodeKind::TryStmt:
        {
          auto* t = static_cast<TryStmt*>(s);
          if (containsForConstexpr(t->body))
            return true;
          for (const TryStmt::Handler& h : t->handlers)
            if (containsForConstexpr(h.body))
              return true;
          return false;
        }
        default: return false;
      }
    }

    void emitFunction(FunctionDecl* original, const TemplateArgs& args, const Ty* classTy, const std::string& nameOverride = "")
    {
      // An instantiation is emitted from its own copy of the template's nodes, typed for args once
      // (GlobalScope::instanceBody); its IR name is the template's with the arguments. One that is
      // re-typed per pack element while it is emitted gets a copy of its own for that, so that
      // nobody reading the instantiation's copy meanwhile (a caller's default arguments, on another
      // thread) sees the element's types.
      FunctionDecl* f = args.empty() ? original : global.instanceBody(original, args, classTy);
      FunctionDecl* checked = f; // the body the checker typed, whose static_asserts count now (GlobalScope::functionEmitted)
      if (retypedPerElement(f))
        f = global.privateInstance(original, args, classTy);
      if (!conditionHolds(f))
        return; // conditional(false): the function does not exist in this build
      struct Restore { const FunctionDecl*& at; const FunctionDecl* saved; ~Restore() { at = saved; } } restore{checkerChecked, checkerChecked};
      checkerChecked = checked;
      global.functionEmitted(checked);
      if (f != checked)
        global.functionEmitted(f);
      currentFile = f->file;
      currentArgs = args.empty() ? nullptr : &args;
      currentClassTy = classTy;
      if (!currentClassTy)
        if (Decl* cls = global.owningClass(f))
          currentClassTy = cls->kind == NodeKind::EnumDecl ? types.enumType(cls) : canonicalClass(types.classType(cls, {}), f);
      // The type callers see: the template's own declaration under args. An `auto` instantiation's
      // return type is the one its body (the copy emitted here) deduced; asking for the copy's type
      // would type a copy of the copy, whose lambdas are other closure types than the body's.
      const Ty* ft = declType(original);
      if (!ft || ft->kind != TyKind::Function)
      {
        error(f, "function type unknown, skipped");
        currentArgs = nullptr;
        return;
      }
      resetFunctionState();
      currentFunction = f;
      bodyNow = f->body;
      nestedNamesByBody.clear();
      if (sh.debugInfo)
      {
        debugAt = f;
        debugFile = currentFile;
      }
      currentReturn = ft->inner;
      // If the return type is unknown, pretend it's ptr so the function body can be emitted;
      // return statements will try to convert their value to a pointer (and throw if they can't).
      if (!currentReturn || !currentReturn->isKnown())
        currentReturn = types.pointer(types.voidType());
      sretAddr.clear();
      currentFunctionName = nameOverride.empty() ? functionName(original, args, classTy) : nameOverride;
      currentContext = displayFunctionName(original, args, classTy);
      struct OwnerScope
      {
        std::string& owner;
        bool outermost;
        ~OwnerScope()
        {
          if (outermost)
            owner.clear();
        }
      } ownerScope{owner, owner.empty()};
      if (ownerScope.outermost)
      {
        owner = currentFunctionName;
        emittingName = currentFunctionName;
        emittingNow = {original, args, classTy, currentModule, currentRequestedBy, nameOverride, currentDepth, currentRequestedByContext};
        inlinedNow.clear();
        layoutsNow.clear();
        // One of the program's own functions (a level-0 item): what it owns goes in its module's
        // program piece (Shared::splitPieces).
        if (sh.splitPieces && currentDepth == 0 && currentRequestedBy.empty() && args.empty() && !classTy && nameOverride.empty() &&
            isProgramFunction(original))
          programFunctionsNow.push_back(currentFunctionName);
      }
      try
      {
        Signature s = signatureOf(original);
        // A C-callable function defined in kpp takes a struct by value as kpp passes it (a pointer to
        // a copy): what C passes otherwise (cPassing) is not received yet.
        if (isCFunction(original))
        {
          const Ty* cft = declType(original);
          for (size_t i = 0; i <= cft->args.size(); ++i)
            if (const Ty* t = i < cft->args.size() ? cft->args[i] : cft->inner; t && isAggregate(t) && (!sh.target.windows() || !cPassing(t, f).inMemory))
              unsupported(f, "a C-callable function (extern \"C\") defined in kpp that takes or returns a struct by value (" + irString(t) +
                                 "), which C passes " + (sh.target.windows() ? "in a register" : "by its target's convention") + " (docs/design.md, \"Targets and hosts\")");
        }
        std::string params;
        std::vector<std::string> paramNames;
        for (size_t i = 0; i < s.paramTypes.size(); ++i)
        {
          std::string pn = "%p" + std::to_string(i);
          paramNames.push_back(pn);
          params += (i ? ", " : "") + s.paramTypes[i] + " " + pn;
        }
        if (s.variadic)
          params += params.empty() ? "..." : ", ...";
        std::string name = currentFunctionName;
        declaredNames.claim(name);
        // A C header's inline function (useFunctionOnce: @"kpp.c-inline.NAME") is weak_odr: its home
        // module's copy is the only one, and ThinLTO dropped a linkonce_odr one that only other
        // modules called (`undefined symbol: floorl`).
        bool headerInline = original->file && original->file->isImported;
        std::string header = std::string("define ") + (headerInline ? "weak_odr " : "") + s.returnType + " " + name + "(" + params + ") " +
                             attributeText(f) + "{\nentry:\n";
        pushScope();
        size_t slot = 0;
        if (s.hasThis)
        {
          std::string thisSlot = "%v" + std::to_string(temp++);
          allocas += "  " + thisSlot + " = alloca ptr\n";
          fn += "  store ptr " + paramNames[slot] + ", ptr " + thisSlot + "\n";
          locals[nullptr] = {thisSlot, nullptr, false};
          if (currentClassTy)
            debugVariable("this", f, thisSlot, types.pointer(currentClassTy), 1, true);
          ++slot;
        }
        if (s.hasSret)
          sretAddr = paramNames[slot++];
        packLocals.clear();
        for (size_t i = 0; i < f->params.size(); ++i)
        {
          auto* pd = static_cast<ParamDecl*>(f->params[i]);
          size_t count = 1;
          if (pd->isPack)
          {
            const Ty* packTy = declType(pd);
            if (!packTy || packTy->kind != TyKind::Pack)
              unsupported(f, "a parameter pack that was not deduced");
            count = packTy->args.size();
            packLocals[pd] = {};
          }
          for (size_t k = 0; k < count; ++k, ++slot)
          {
            if (slot >= s.paramTys.size())
              unsupported(f, "parameter " + std::to_string(i + 1) + " (" + types.toString(declType(pd), &symbols) +
                                 ") beyond the parameters of the function's type " + types.toString(ft, &symbols));
            const Ty* pt = s.paramTys[slot];
            Local local;
            if (isAggregate(pt))
            {
              local = {paramNames[slot], pt, false};
              if (needsDestruction(pt, f))
                scopes.back().push_back({paramNames[slot], pt});
            }
            else
            {
              bool isRef = pt->isReference();
              std::string ps = "%v" + std::to_string(temp++);
              allocas += "  " + ps + " = alloca " + (isRef ? std::string("ptr") : llType(pt, f)) + "\n";
              fn += "  store " + (isRef ? std::string("ptr") : llType(pt, f)) + " " + paramNames[slot] + ", ptr " + ps + "\n";
              local = {ps, pt, isRef};
            }
            // (a pack's elements are the debugger's as one each: name#k)
            if (pd->name != UINT32_MAX)
              debugVariable(sym(pd->name) + (pd->isPack ? "#" + std::to_string(k) : std::string()), pd, local.addr, pt,
                            uint32_t(slot + 1 - (s.hasSret ? 1 : 0)));
            if (pd->isPack)
              packLocals[pd].push_back(local);
            else
              locals[pd] = local;
          }
        }
        if ((f->flags & DF_Default) && !f->body)
        {
          // `= default`: the memberwise body; the source object, if any, is the parameter after this.
          size_t sourceIndex = s.hasSret ? 2 : 1;
          std::string source = paramNames.size() > sourceIndex ? paramNames[sourceIndex] : std::string();
          Value r = defaultedOperation(f, types.unqualified(types.stripRef(currentClassTy)), thisPointer(f), source, f);
          destroyTemps();
          popScope();
          if (isClass(currentReturn))
          {
            // A class result (a defaulted operator<=>'s strong_ordering): into the result slot.
            if (!sretAddr.empty() && r.ty && isClass(r.ty))
              fn += "  store i8 " + orderOf(r, f).text + ", ptr " + gep(structFor(currentReturn, f).name, sretAddr, 0) + "\n";
            fn += "  ret void\n";
          }
          else if (currentReturn->kind == TyKind::Void)
            fn += "  ret void\n";
          else if (currentReturn->isReference())
            fn += "  ret ptr " + r.text + "\n";
          else
            fn += "  ret " + llType(currentReturn, f) + " " + r.text + "\n";
          addFunction(name, functionText(header));
          currentArgs = nullptr;
          currentClassTy = nullptr;
          return;
        }
        if (f->flags & DF_Constructor)
        {
          initializeMembers(thisPointer(f), currentClassTy, f->ctorInits, f, true);
          destroyTemps();
        }
        bool destructor = (f->flags & DF_Destructor) && isClass(currentClassTy);
        if (destructor)
        {
          storeVptrs(thisPointer(f), structFor(currentClassTy, f)); // virtual calls while it is destroyed reach this class's functions, as in C++
          // A destructor that may throw: its members and bases are destroyed when its body throws, too.
          if (destructionMayThrow(currentClassTy, f))
          {
            StructInfo& info = structFor(currentClassTy, f);
            std::string structName = info.name, self = thisPointer(f);
            std::vector<std::pair<Decl*, uint32_t>> bases = info.bases;
            std::vector<Decl*> fieldDecls = info.fieldDecls;
            std::vector<const Ty*> fieldTypes = info.fieldTypes;
            std::unordered_map<Decl*, uint32_t> fieldIndex = info.fieldIndex;
            auto member = [&](uint32_t index)
            {
              Object o{gep(structName, self, index), fieldTypes[index]};
              o.unwindOnly = true;
              scopes.back().push_back(o);
            };
            for (auto& [base, index] : bases)
              if (needsDestruction(fieldTypes[index], f))
                member(index);
            for (Decl* d : fieldDecls)
              if (!fieldTypes[fieldIndex[d]]->isReference() && needsDestruction(fieldTypes[fieldIndex[d]], f))
                member(fieldIndex[d]);
          }
          // The members and bases are destroyed after the body however it ends: falling off its end
          // or at a `return` (CargoBayResizeHelper's `if (!this->surface) return;` left its targeter
          // member in the hub's list, and destroying the hub faulted).
          returnLabel = newLabel("dtor.members");
        }
        namedReturn = namedReturnVariable(f->body);
        statement(f->body);
        if (destructor)
        {
          popScope();
          emitBranch(returnLabel);
          startBlock(returnLabel);
          returnLabel.clear();
          destroyMembers(thisPointer(f), currentClassTy, f);
          fn += "  ret void\n";
        }
        else if (!blockTerminated)
        {
          popScope();
          if (currentReturn->kind == TyKind::Void || returnsInMemory(currentReturn))
            fn += "  ret void\n";
          else if (name == "@kpp.main" || name == "@kpp.wmain")
            fn += "  ret i32 0\n";
          else // falling off the end (after a throw, usually): a zero rather than `unreachable`, see emitThrow
            fn += "  ret " + (currentReturn->isReference() ? std::string("ptr null") : llType(currentReturn, f) + " zeroinitializer") + "\n";
        }
        addFunction(name, functionText(header));
      }
      catch (Unsupported&)
      {
        failedFunctions.push_back(currentFunctionName);
        failedAt[currentFunctionName] = qualifiedName(original) + " at " + programLocationOf(original, original->file); // what --stub-missing's stub says
      }
      currentArgs = nullptr;
      currentClassTy = nullptr;
    }

    // A static_assert the checker left to the layouts (`static_assert(sizeof(Pair) == 16)`): its
    // condition under the instance's arguments, with sizes and alignments as laid out. A false one is
    // the checker's error ("static assertion failed"); a layout that fails reports itself.
    void checkLayoutAssert(const GlobalScope::LayoutStaticAssert& a)
    {
      StaticAssertDecl* s = a.decl;
      if (!s || !s->cond)
        return;
      ArgsScope scope(*this, &a.args);
      ConstContextScope context(*this, s);
      currentFile = s->file;
      currentFunctionName.clear();
      currentRequestedBy.clear();
      std::optional<bool> v;
      GlobalScope::clearConstantReason();
      try
      {
        v = evalCondition(s->cond);
      }
      catch (Unsupported&)
      {
        return;
      }
      if (v && *v)
        return;
      // As the checker words it: a condition that is no constant says why (the interpreter's reason).
      std::string why = v ? std::string() : GlobalScope::constantReason();
      std::string message = v                ? "static assertion failed"
                          : !why.empty()     ? "static assertion whose condition is no constant: " + why
                                             : "static assertion whose condition the compiler cannot evaluate yet";
      if (s->message && s->message->kind == NodeKind::LiteralExpr && s->file && s->file->tokens && !s->file->tokens->empty())
      {
        const Token& t = (*s->file->tokens)[std::min<size_t>(s->message->token, s->file->tokens->size() - 1)];
        message += ": " + std::string(s->file->text.substr(t.offset, t.length));
      }
      if (v && !a.instance.empty())
        message += " (in " + a.instance + ")";
      error(s, message);
    }

    // Out-of-line member definitions (`void Text::split(...) {}`, `Flow::~Flow() = default;`, those
    // of class templates too) take `static`, `virtual` and `= default` from their declarations, and
    // the declarations `= default` from them, before any function is emitted: emitters on several
    // threads read these flags.
    void settleDefinitionFlags(Span<Decl*> decls)
    {
      for (Decl* d : decls)
      {
        if (d->kind == NodeKind::NamespaceDecl)
          settleDefinitionFlags(static_cast<NamespaceDecl*>(d)->decls);
        if (d->kind == NodeKind::ClassDecl)
        {
          // `Flow(Flow&&) noexcept;` in the class and `Flow::Flow(Flow&&) noexcept = default;`
          // outside: the declaration is defaulted too. Found from the declaration, as useFunction
          // finds the definition (declarationOf finds no constructor or destructor), so that
          // useFunction never has to write the flag while other threads read it.
          for (Decl* m : static_cast<ClassDecl*>(d)->members)
            if (m->kind == NodeKind::FunctionDecl && !static_cast<FunctionDecl*>(m)->body && !(m->flags & DF_Default))
              if (Decl* def = global.definitionOf(m);
                  def && def != m && def->kind == NodeKind::FunctionDecl && (def->flags & DF_Default) && !static_cast<FunctionDecl*>(def)->body)
                m->flags |= DF_Default;
          settleDefinitionFlags(static_cast<ClassDecl*>(d)->members); // nested classes
          continue;
        }
        if (d->kind != NodeKind::FunctionDecl || static_cast<FunctionDecl*>(d)->name.parts.size() <= 1)
          continue;
        auto* def = static_cast<FunctionDecl*>(d);
        FunctionDecl* declaration = global.declarationOf(def);
        if (declaration && (def->flags & DF_Default) && !def->body && !(declaration->flags & DF_Default))
          declaration->flags |= DF_Default;
        // `template<> void LuaDefines::setKey<const String&>(lua_State*, ...) {...}`: an explicit
        // specialisation of a member function template is static when the template is (`static` is
        // written in the class only, and the declaration lookup matches no template). Without it the
        // definition took a `this` its callers never pass, and read every argument one off.
        if (!declaration && def->isTemplate && def->templateParams.empty() && !(def->flags & DF_Static))
          if (Decl* owner = global.owningClass(def); owner && owner->kind == NodeKind::ClassDecl)
            if (ClassDecl* cls = classDef(owner, def))
            {
              const NamePart& name = def->name.last();
              bool anyTemplate = false, allStatic = true;
              for (Decl* m : cls->members)
                if (m->kind == NodeKind::FunctionDecl && !m->templateParams.empty() && !static_cast<FunctionDecl*>(m)->name.empty())
                  if (const NamePart& mn = static_cast<FunctionDecl*>(m)->name.last(); mn.symbol == name.symbol && mn.special == name.special &&
                                                                                       mn.opToken == name.opToken)
                  {
                    anyTemplate = true;
                    allStatic = allStatic && (m->flags & DF_Static);
                  }
              if (anyTemplate && allStatic)
                def->flags |= DF_Static;
            }
      }
    }

    // The program's globals that C++ builds at startup whether anything reads them or not: a static
    // member whose initializer registers something (`bool FontBank::loaderRegistered =
    // PrototypeLoader::registerPlainLoader(...)`, read by nobody: the game's prototype loaders were
    // never registered). Variables of namespaces and static data members of classes that are no
    // templates, with an initializer that is no literal, or of a class type without one; for an
    // out-of-line definition, the declaration in the class (the one uses name).
    void collectStartupGlobals(Span<Decl*> decls, std::vector<VariableDecl*>& out)
    {
      for (Decl* d : decls)
      {
        if (d->kind == NodeKind::NamespaceDecl)
          collectStartupGlobals(static_cast<NamespaceDecl*>(d)->decls, out);
        else if (d->kind == NodeKind::ClassDecl)
        {
          auto* c = static_cast<ClassDecl*>(d);
          if (!c->templateParams.empty() || !c->isDefinition || c->isClosure || global.isTemplateContext(c))
            continue;
          for (Decl* m : c->members)
          {
            // `static inline T x = ...;` in the class, or `static bool registered;` defined outside
            // (`bool FontBank::loaderRegistered = ...;`, whose initializer considerStartupGlobal reads).
            if (m->kind == NodeKind::VariableDecl && (m->flags & DF_Static))
              considerStartupGlobal(static_cast<VariableDecl*>(m), out);
            else if (m->kind == NodeKind::ClassDecl)
              collectStartupGlobals(Span<Decl*>{&m, 1}, out); // a nested class
          }
        }
        else if (d->kind == NodeKind::VariableDecl && static_cast<VariableDecl*>(d)->name.parts.size() == 1)
          considerStartupGlobal(static_cast<VariableDecl*>(d), out); // (an out-of-line static member: through its class)
      }
    }

    void considerStartupGlobal(VariableDecl* v, std::vector<VariableDecl*>& out)
    {
      VariableDecl* def = variableDefinition(v);
      if ((v->flags & (DF_Typedef | DF_Constexpr | DF_ThreadLocal)) || (def->flags & (DF_Constexpr | DF_ThreadLocal)) || v->isTemplate ||
          !v->structuredBindings.empty() || (v->file && v->file->isImported) || global.isTemplateContext(v))
        return;
      if ((v->flags & DF_Extern) && !def->init && def == v)
        return; // a C library's variable
      if (def->init)
      {
        if (def->init->kind == NodeKind::LiteralExpr)
          return; // a constant: built by nobody
      }
      else
      {
        const Ty* t = global.typeOfDecl(v);
        const Ty* element = t;
        while (element && element->kind == TyKind::Array)
          element = element->inner;
        if (!element || !isClass(element))
          return; // zero
      }
      out.push_back(v);
    }

    // The static data members of class instance cls that an explicit instantiation builds at
    // startup (`template struct Registry<int>;`, as C++ instantiates every member): those with an
    // initializer that is no literal, or of a class type that needs constructing.
    void collectInstanceStartupGlobals(const Ty* cls, std::vector<std::pair<VariableDecl*, const Ty*>>& out)
    {
      Decl* d = global.definitionOf(cls->decl);
      if (!d || d->kind != NodeKind::ClassDecl)
        return;
      for (Decl* m : static_cast<ClassDecl*>(d)->members)
      {
        if (m->kind != NodeKind::VariableDecl || !(m->flags & DF_Static))
          continue;
        auto* v = static_cast<VariableDecl*>(m);
        VariableDecl* def = variableDefinition(v);
        if ((v->flags & (DF_Typedef | DF_Constexpr | DF_ThreadLocal)) || (def->flags & (DF_Constexpr | DF_ThreadLocal)) || v->isTemplate)
          continue;
        if (def->init ? def->init->kind == NodeKind::LiteralExpr : !needsConstruction(global.typeOfDeclIn(v, global.classArgsOf(cls)), v))
          continue;
        out.push_back({v, cls});
      }
    }

    // A global collectStartupGlobals found (or a static data member of the class instance classTy
    // collectInstanceStartupGlobals did): defined, and its startup function registered, as if a
    // function had read it. Its errors are its own (globalFor reports them).
    std::optional<Shared::Startup> startupAt; // where the startup function of the global being defined runs, when not where it is defined
    void emitStartupGlobal(VariableDecl* v, const Ty* classTy = nullptr, std::optional<Shared::Startup> at = std::nullopt)
    {
      struct AtRestore { std::optional<Shared::Startup>& a; ~AtRestore() { a.reset(); } } atRestore{startupAt};
      startupAt = at;
      auto it = fileModule.find(v->file);
      currentModule = it == fileModule.end() ? 0 : it->second;
      currentFunctionName = "@\"kpp.init." + (classTy ? irString(classTy) + "::" + sym(v->name.last().symbol) : qualifiedName(v)) + "\"";
      currentRequestedBy.clear();
      currentContext = "the initializer of " + (classTy ? types.toString(classTy, &symbols) + "::" + sym(v->name.last().symbol) : qualifiedName(v));
      currentRequestedByContext.clear();
      currentFile = v->file; // its errors point at its declaration
      try
      {
        globalFor(v, v, classTy);
      }
      catch (Unsupported&)
      {
      }
      currentFunctionName.clear();
      currentFile = nullptr;
    }

    void collectFunctions(Span<Decl*> decls, std::vector<FunctionDecl*>& out)
    {
      for (Decl* d : decls)
      {
        switch (d->kind)
        {
          case NodeKind::NamespaceDecl: collectFunctions(static_cast<NamespaceDecl*>(d)->decls, out); break;
          case NodeKind::ClassDecl:
            if (static_cast<ClassDecl*>(d)->templateParams.empty())
              collectFunctions(static_cast<ClassDecl*>(d)->members, out);
            break;
          case NodeKind::EnumDecl: collectFunctions(static_cast<EnumDecl*>(d)->members, out); break;
          case NodeKind::FunctionDecl:
          {
            auto* f = static_cast<FunctionDecl*>(d);
            if (f->body && f->templateParams.empty() && !global.isTemplateContext(f) && !(f->file && f->file->isImported))
              out.push_back(f);
            break;
          }
          default: break;
        }
      }
    }

    // Whether the build emits f as one of the program's own functions (collectFunctions reaches it):
    // a definition outside templates and outside function bodies (a local class's functions are
    // emitted where they are used).
    bool isProgramFunction(FunctionDecl* f)
    {
      if (!f->body || !f->templateParams.empty() || global.isTemplateContext(f) || (f->file && f->file->isImported))
        return false;
      for (Decl* p = f->parent; p; p = p->parent)
        if (p->kind == NodeKind::FunctionDecl || (p->kind == NodeKind::ClassDecl && !static_cast<ClassDecl*>(p)->templateParams.empty()))
          return false;
      return true;
    }
