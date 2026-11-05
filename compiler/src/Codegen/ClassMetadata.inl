// struct Emitter's members: constructors, destructors, member initialisation and copies. Part of
// codegen.cpp, included in the struct's body in Codegen/Emitter.hpp (a file of its own since
// 2026-11-05, moved without a change).
    // --- class metadata: constructors, destructors ---

    // The constructor overload resolution picks for a class and argument types, chosen once.
    struct ConstructorKey
    {
      const Ty* classTy;
      std::vector<const Ty*> argTypes;
      std::vector<bool> nulls;
      bool operator==(const ConstructorKey&) const = default;
    };
    struct ConstructorKeyHash
    {
      size_t operator()(const ConstructorKey& k) const
      {
        size_t h = std::hash<const void*>()(k.classTy);
        for (const Ty* a : k.argTypes)
          h = (h * 1000003) ^ std::hash<const void*>()(a);
        return h ^ std::hash<std::vector<bool>>()(k.nulls);
      }
    };
    std::unordered_map<ConstructorKey, FunctionDecl*, ConstructorKeyHash> constructors;

    // The constructor for these arguments: their types, and which are the literal 0 (a null pointer
    // constant, which reaches `FromText(const char*)` as the checker ranks it).
    FunctionDecl* constructorFor(const Ty* classTy, Span<Expr*> args, Node* at)
    {
      std::vector<bool> nulls;
      bool anyNull = false;
      for (Expr* a : args)
      {
        if (a && isPackExpansion(a))
          return findConstructor(classTy, argTypesOf(args), at); // (the elements are no literals)
        nulls.push_back(nullPointerConstant(a));
        anyNull = anyNull || nulls.back();
      }
      return findConstructor(classTy, argTypesOf(args), at, anyNull ? nulls : std::vector<bool>{});
    }

    // nulls: per argument, whether it is the literal 0; empty when none is.
    FunctionDecl* findConstructor(const Ty* classTy, const std::vector<const Ty*>& argTypes, Node* at, const std::vector<bool>& nulls = {})
    {
      const Ty* canonical = canonicalClass(classTy, at);
      ConstructorKey key{canonical, argTypes, nulls};
      if (auto it = constructors.find(key); it != constructors.end())
        return it->second;
      auto* c = static_cast<ClassDecl*>(canonical->decl);
      TemplateArgs args = global.classArgsOf(canonical);
      std::vector<FunctionDecl*> candidates;
      for (Decl* m : c->members)
        if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Constructor))
          candidates.push_back(static_cast<FunctionDecl*>(m));
      // `using Text::Text;`: the base's constructors compete too, its copy and move ones aside (C++
      // [over.match.funcs]/8) and one with an own constructor's parameters hidden by it. When one of
      // them wins, null: constructInto builds that base from the arguments (`MoveOnly direct("abc")`
      // takes Text(const char*), not the move constructor through a conversion by it).
      size_t own = candidates.size();
      auto parameterTypes = [&](FunctionDecl* f, const TemplateArgs& of)
      {
        ArgsScope scope(*this, &of);
        std::vector<const Ty*> out;
        for (Decl* p : f->params)
          out.push_back(declType(p));
        return out;
      };
      std::vector<std::vector<const Ty*>> ownParameters;
      for (FunctionDecl* f : candidates)
        ownParameters.push_back(f->templateParams.empty() ? parameterTypes(f, args) : std::vector<const Ty*>{});
      std::vector<const Ty*> chain{canonical}; // the class, then each base whose constructors it inherits
      for (int level = 0; level < 8; ++level)
      {
        std::optional<QualifiedName> base = inheritedConstructorBase(classDef(chain.back()->decl, at));
        if (!base)
          break;
        FunctionDecl::CtorInit init{};
        init.member = *base;
        const Ty* bt = initializerClass(chain.back(), init, at);
        if (!bt)
          break;
        chain.push_back(bt);
        TemplateArgs baseArgs = global.classArgsOf(bt);
        for (Decl* m : classDef(bt->decl, at)->members)
        {
          if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Constructor))
            continue;
          auto* f = static_cast<FunctionDecl*>(m);
          std::vector<const Ty*> params = f->templateParams.empty() ? parameterTypes(f, baseArgs) : std::vector<const Ty*>{};
          const Ty* first = !params.empty() && params[0] ? types.unqualified(types.stripRef(params[0])) : nullptr;
          if (argTypes.size() == 1 && first && isClass(first) && !types.isDependent(first) &&
              std::find(chain.begin(), chain.end(), canonicalClass(first, at)) != chain.end())
            continue; // a copy or move constructor of the base, or one taking the class itself
          if (!params.empty() && std::find(ownParameters.begin(), ownParameters.end(), params) != ownParameters.end())
            continue;
          candidates.push_back(f);
        }
        args.pairs.insert(args.pairs.end(), baseArgs.pairs.begin(), baseArgs.pairs.end());
      }
      FunctionDecl* chosen = global.chooseOverload(candidates, argTypes, args, nullptr, nullptr, nulls.empty() ? nullptr : &nulls);
      if (chosen && std::find(candidates.begin() + own, candidates.end(), chosen) != candidates.end())
        chosen = nullptr;
      constructors.emplace(std::move(key), chosen);
      return chosen;
    }

    std::vector<const Ty*> argTypesOf(Span<Expr*> args)
    {
      std::vector<const Ty*> out;
      for (Expr* a : args)
      {
        if (a && isPackExpansion(a))
        {
          // `args...`: one argument per element of the pack, under the current instantiation: the
          // parameter's own element types, or for a longer pattern the elements of its first pack.
          Expr* pattern = static_cast<UnaryExpr*>(a)->operand;
          Decl* param = packIn(a);
          if (param && pattern->kind == NodeKind::NameExpr && static_cast<NameExpr*>(pattern)->resolved == param)
          {
            for (const Local& element : packLocals[param])
              out.push_back(argumentType(pattern, element.ty, types));
            continue;
          }
          std::vector<Decl*> packs = param ? templatePacksOf(param) : std::vector<Decl*>{};
          const Ty* pack = nullptr;
          if (!packs.empty() && currentArgs)
            for (auto& [d, t] : currentArgs->pairs)
              if (d == packs[0])
                pack = t;
          if (pack && pack->kind == TyKind::Pack)
          {
            for (const Ty* element : pack->args)
              out.push_back(argumentType(a, element, types));
            continue;
          }
          // `values[I]...` over a template parameter pack: the pattern's type, once per element.
          if (Decl* valuePack = !param && currentArgs ? typePackIn(pattern) : nullptr)
            if (const Ty* bound = boundPack(valuePack); bound && bound->kind == TyKind::Pack)
            {
              for (size_t i = 0; i < bound->args.size(); ++i)
                out.push_back(argumentType(pattern, pattern->ty, types));
              continue;
            }
        }
        if (a && a->kind == NodeKind::BraceInitExpr)
        {
          out.push_back(braceListType(a, global.initializerListDecl(), types));
          continue;
        }
        out.push_back(a ? argumentType(a, a->ty, types) : nullptr);
      }
      return out;
    }

    bool isInitializerListType(const Ty* t)
    {
      Decl* d = global.initializerListDecl();
      return d && t && t->kind == TyKind::Class && t->decl == d;
    }

    // `{a, b, c}` for an InitializerList<E> parameter: the elements in a temporary array, the list
    // object (a pointer and a count) pointing at them; both live to the end of the statement.
    // variable: the address of a variable the list initializes (`InitializerList<int> v = {1, 2}`,
    // `auto names = {"a", "b"}`), whose array lives as long as it does, as C++ extends it: in the
    // function's frame for a local, in static storage for a global or a static local.
    std::string initializerListTemporary(const Ty* listTy, BraceInitExpr* b, Node* at, const std::string& variable = "")
    {
      if (listTy->args.empty())
        unsupported(at, "an InitializerList without its element type");
      const Ty* element = listTy->args[0];
      // A pack expansion among the elements (`{args...}`) gives one element per element of the pack.
      uint32_t n = 0;
      for (Expr* a : b->args)
        n += isPackExpansion(a) ? uint32_t(packElementCount(static_cast<UnaryExpr*>(a)->operand, at)) : 1;
      const Ty* arrayTy = types.array(element, n);
      bool staticArray = !variable.empty() && variable[0] == '@';
      std::string array;
      if (staticArray)
      {
        if (needsDestruction(arrayTy, at))
          unsupported(at, "an InitializerList in static storage of elements that need destruction");
        std::string owner = variable.substr(0, variable.size() - (variable.back() == '"' ? 1 : 0));
        array = owner + ".list." + std::to_string(b->token) + (variable.back() == '"' ? "\"" : "");
        addOwnedEntry(array + " = internal global " + llType(arrayTy, at) + " zeroinitializer\n");
      }
      else
        array = newObjectSlot(arrayTy, at);
      uint32_t i = 0;
      auto initializeElement = [&](Expr* value)
      {
        std::string slot = newTemp();
        fn += "  " + slot + " = getelementptr inbounds " + llType(arrayTy, at) + ", ptr " + array + ", i64 0, i64 " + std::to_string(i++) + "\n";
        initializeFrom(slot, element, value, at);
      };
      for (Expr* a : b->args)
      {
        if (isPackExpansion(a))
        {
          Expr* pattern = static_cast<UnaryExpr*>(a)->operand;
          forEachPackElement(pattern, at, [&](size_t) { initializeElement(pattern); });
        }
        else
          initializeElement(a);
      }
      if (!variable.empty() && !staticArray && needsDestruction(arrayTy, at))
        scopes.back().push_back({array, arrayTy}); // destroyed with the variable's scope
      else if (variable.empty() && needsDestruction(arrayTy, at))
        statementTemps.push_back({array, arrayTy});
      std::string list = variable.empty() ? newObjectSlot(listTy, at) : variable;
      const StructInfo& info = structFor(listTy, at);
      if (info.fieldDecls.size() < 2)
        unsupported(at, "an InitializerList class without its two fields");
      fn += "  store ptr " + array + ", ptr " + gep(info.name, list, fieldIndexOf(info, info.fieldDecls[0])) + "\n";
      // The count in its field's own type (the library's ContainerSize).
      uint32_t countField = fieldIndexOf(info, info.fieldDecls[1]);
      fn += "  store " + llType(info.fieldTypes[countField], at) + " " + std::to_string(n) + ", ptr " + gep(info.name, list, countField) + "\n";
      return list;
    }

    // A call's arguments packed for its Text parameter (design.md 14, BraceInitExpr::text): the
    // pieces in a temporary array, each the address of its argument (a temporary of the statement
    // for one that is no lvalue, bound as a const reference binds it) and the library's function
    // that appends it (resolver: textPieceOf); the Text a pointer to them and their count.
    std::string textTemporary(BraceInitExpr* b, Node* at)
    {
      const Ty* textTy = b->ty ? types.unqualified(types.stripRef(b->ty)) : nullptr;
      if (!textTy || !isClass(textTy))
        unsupported(at, "a Text without its type");
      const StructInfo& info = structFor(textTy, at);
      if (info.fieldDecls.size() < 2)
        unsupported(at, "a Text class without its two fields");
      uint32_t piecesField = fieldIndexOf(info, info.fieldDecls[0]);
      uint32_t countField = fieldIndexOf(info, info.fieldDecls[1]);
      const Ty* piecesTy = info.fieldTypes[piecesField];
      const Ty* pieceTy = piecesTy && piecesTy->kind == TyKind::Pointer ? types.unqualified(piecesTy->inner) : nullptr;
      if (!pieceTy || !isClass(pieceTy))
        unsupported(at, "a Text whose first field is no pointer to its pieces");
      const StructInfo& pieceInfo = structFor(pieceTy, at);
      if (pieceInfo.fieldDecls.size() < 2)
        unsupported(at, "a TextPiece class without its two fields");
      uint32_t n = 0;
      for (Expr* a : b->args)
        n += isPackExpansion(a) ? uint32_t(packElementCount(static_cast<UnaryExpr*>(a)->operand, at)) : 1;
      const Ty* arrayTy = types.array(pieceTy, n);
      std::string array = newObjectSlot(arrayTy, at);
      uint32_t i = 0;
      auto piece = [&](Expr* value)
      {
        const Ty* t = value->ty ? withCurrentArgs(value->ty) : nullptr;
        TextPieceKind kind = global.textPieceOf(value, t);
        if (!kind.thunk)
          unsupported(value, kind.error.empty() ? "a piece of a Text of no known type" : kind.error);
        // Its address: a string literal's chars, an lvalue's own object, a temporary otherwise.
        const Ty* object = kind.type ? types.qualified(kind.type, true, false) : nullptr;
        if (!object)
        {
          object = stringLiteralArray(value, t, types);
          if (!object)
            object = types.unqualified(types.stripRef(t)); // an array of chars
        }
        std::string address = argumentText(types.lref(object), value, at).substr(4); // ("ptr " ...)
        TemplateArgs fargs;
        if (kind.type && !kind.thunk->templateParams.empty())
          fargs.pairs.push_back({kind.thunk->templateParams[0], kind.type});
        std::string function = useFunction(kind.thunk, fargs, nullptr, at);
        std::string slot = newTemp();
        fn += "  " + slot + " = getelementptr inbounds " + llType(arrayTy, at) + ", ptr " + array + ", i64 0, i64 " + std::to_string(i++) + "\n";
        fn += "  store ptr " + address + ", ptr " + gep(pieceInfo.name, slot, fieldIndexOf(pieceInfo, pieceInfo.fieldDecls[0])) + "\n";
        fn += "  store ptr " + function + ", ptr " + gep(pieceInfo.name, slot, fieldIndexOf(pieceInfo, pieceInfo.fieldDecls[1])) + "\n";
      };
      for (Expr* a : b->args)
      {
        if (isPackExpansion(a))
        {
          Expr* pattern = static_cast<UnaryExpr*>(a)->operand;
          forEachPackElement(pattern, at, [&](size_t) { piece(pattern); });
        }
        else
          piece(a);
      }
      std::string text = newObjectSlot(textTy, at);
      fn += "  store ptr " + array + ", ptr " + gep(info.name, text, piecesField) + "\n";
      fn += "  store " + llType(info.fieldTypes[countField], at) + " " + std::to_string(n) + ", ptr " + gep(info.name, text, countField) + "\n";
      return text;
    }

    // Default-constructs every element of an array of objects at addr: one after the other for a
    // few, in a loop for more (an array of a thousand would otherwise be a thousand copies).
    void constructArray(const std::string& addr, const Ty* t, Node* at)
    {
      auto constructAt = [&](const std::string& index)
      {
        std::string element = newTemp();
        fn += "  " + element + " = getelementptr inbounds " + llType(t, at) + ", ptr " + addr + ", i64 0, i64 " + index + "\n";
        if (isClass(t->inner))
          defaultConstruct(element, t->inner, at);
        else if (t->inner->kind == TyKind::Array)
          constructArray(element, t->inner, at);
        else
          storeZero(t->inner, element, at);
      };
      if (t->count <= 8)
      {
        for (uint32_t i = 0; i < t->count; ++i)
          constructAt(std::to_string(i));
        return;
      }
      std::string slot = "%v" + std::to_string(temp++);
      allocas += "  " + slot + " = alloca i64\n";
      fn += "  store i64 0, ptr " + slot + "\n";
      std::string loopL = newLabel("array.loop"), bodyL = newLabel("array.body"), endL = newLabel("array.end");
      emitBranch(loopL);
      startBlock(loopL);
      std::string index = newTemp(), done = newTemp();
      fn += "  " + index + " = load i64, ptr " + slot + "\n";
      fn += "  " + done + " = icmp eq i64 " + index + ", " + std::to_string(t->count) + "\n";
      fn += "  br i1 " + done + ", label %" + endL + ", label %" + bodyL + "\n";
      startBlock(bodyL);
      constructAt(index);
      std::string next = newTemp();
      fn += "  " + next + " = add i64 " + index + ", 1\n";
      fn += "  store i64 " + next + ", ptr " + slot + "\n";
      emitBranch(loopL);
      startBlock(endL);
    }

    // A destructor written by the user; `~T() = default` is the compiler's memberwise one.
    FunctionDecl* findDestructor(ClassDecl* c)
    {
      for (Decl* m : c->members)
        if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Destructor) && !(m->flags & DF_Default))
          return static_cast<FunctionDecl*>(m);
      return nullptr;
    }

    // Constructors written by the user (`= default` ones are the compiler's memberwise ones).
    bool hasUserConstructors(ClassDecl* c)
    {
      for (Decl* m : c->members)
        if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Constructor) && !(m->flags & DF_Default))
          return true;
      return false;
    }

    // Default construction does something: user constructors, a vtable pointer, default member
    // initializers, or a base/member class that needs constructing (a zeroed global is not enough).
    bool needsConstruction(const Ty* t, Node* at, int depth = 0)
    {
      t = types.stripRef(t);
      if (t->kind == TyKind::Array)
        return needsConstruction(t->inner, at, depth);
      if (!isClass(t) || !t->decl || depth > 16)
        return false;
      if (hasUserConstructors(classDef(t->decl, at)))
        return true;
      const StructInfo& info = structFor(t, at);
      if (info.polymorphic)
        return true;
      for (Decl* m : info.fieldDecls)
        if (m->kind == NodeKind::VariableDecl && static_cast<VariableDecl*>(m)->init)
          return true;
      for (const Ty* ft : info.fieldTypes)
        if (needsConstruction(ft, at, depth + 1))
          return true;
      return false;
    }

    bool needsDestruction(const Ty* t, Node* at)
    {
      t = types.stripRef(t);
      if (t->kind == TyKind::Array)
        return needsDestruction(t->inner, at);
      if (isLambda(t))
      {
        for (auto& [index, ft] : closureValueFields(t, at))
          if (needsDestruction(ft, at))
            return true;
        return false;
      }
      if (!isClass(t))
        return false;
      noteLayout(t);
      if (auto mine = destructionCache.find(t); mine != destructionCache.end())
        return mine->second;
      if (bool known; sh.finalDestruction.find(t, known))
        return destructionCache.emplace(t, known).first->second;
      auto lock = sharedLock(); // remembered in the shared layout, computed under the lock
      StructInfo& info = structFor(t, at);
      if (info.needsDestruction)
      {
        // Final unless this emitter is in the middle of computing it (the recursion guard below):
        // nobody else computes while the lock is held.
        if (destructionDepth == 0)
        {
          destructionCache.emplace(t, *info.needsDestruction);
          if (mayPublish())
            sh.finalDestruction.publish(t, *info.needsDestruction);
        }
        return *info.needsDestruction;
      }
      info.needsDestruction = false; // Guard against recursion.
      struct Depth { int& d; Depth(int& d) : d(d) { ++d; } ~Depth() { --d; } } depth(destructionDepth);
      ClassDecl* c = classDef(t->decl, at);
      if (c->isTypedUnion)
      {
        // A typed union destroys whichever alternative it holds.
        bool any = false;
        for (size_t i = 0; i < c->alternatives.size(); ++i)
          any = any || needsDestruction(alternativeType(c, t, i, at), at);
        structFor(t, at).needsDestruction = any;
        return any;
      }
      bool result = findDestructor(c) != nullptr;
      for (const Ty* ft : std::vector<const Ty*>(info.fieldTypes))
        result = result || (!ft->isReference() && needsDestruction(ft, at)); // a reference member owns nothing
      structFor(t, at).needsDestruction = result;
      return result;
    }
    std::unordered_map<const Ty*, bool> destructionCache; // this emitter's view of the answers above
    int destructionDepth = 0;

    // Destroys the object at addr: user destructor if any (it destroys members itself), else
    // members and bases that need it, in reverse order.
    void destroy(const std::string& addr, const Ty* t, Node* at, bool dynamic = false)
    {
      t = types.stripRef(t);
      if (t->kind == TyKind::Array)
      {
        if (!needsDestruction(t->inner, at))
          return;
        if (destructionMayThrow(t->inner, at))
        {
          // (an element's destructor that throws leaves the elements before it to be destroyed)
          pushScope();
          for (uint32_t i = 0; i < t->count; ++i)
          {
            std::string e = newTemp();
            fn += "  " + e + " = getelementptr inbounds " + llType(t, at) + ", ptr " + addr + ", i64 0, i64 " + std::to_string(i) + "\n";
            scopes.back().push_back({e, t->inner});
          }
          popScope();
          return;
        }
        for (uint32_t i = t->count; i-- > 0;)
        {
          std::string e = newTemp();
          fn += "  " + e + " = getelementptr inbounds " + llType(t, at) + ", ptr " + addr + ", i64 0, i64 " + std::to_string(i) + "\n";
          destroy(e, t->inner, at);
        }
        return;
      }
      if (isLambda(t))
      {
        std::vector<std::pair<uint32_t, const Ty*>> fields = closureValueFields(t, at);
        std::string structName = closureFor(t, at).structName;
        if (destructionMayThrow(t, at))
        {
          pushScope();
          for (size_t i = 0; i < fields.size(); ++i)
            if (needsDestruction(fields[i].second, at))
              scopes.back().push_back({gep(structName, addr, fields[i].first), fields[i].second});
          popScope();
          return;
        }
        for (size_t i = fields.size(); i-- > 0;)
          if (needsDestruction(fields[i].second, at))
            destroy(gep(structName, addr, fields[i].first), fields[i].second, at);
        return;
      }
      if (!isClass(t))
        return;
      if (dynamic)
      {
        // A virtual destructor: the dynamic class's, also when this class has nothing to destroy
        // (`virtual ~Shape() = default`) and the derived one does.
        if (int slot = slotKeyed(structFor(t, at), "~"); slot >= 0)
        {
          std::string fp = loadSlot(addr, slot);
          callDestructor(fp, addr, destructionMayThrow(t, at));
          return;
        }
      }
      if (!needsDestruction(t, at))
        return;
      ClassDecl* c = classDef(t->decl, at);
      if (c->isTypedUnion)
      {
        destroyAlternative(c, t, addr, at);
        return;
      }
      if (FunctionDecl* dtor = findDestructor(c))
      {
        TemplateArgs args = global.classArgsOf(canonicalClass(t, at));
        callDestructor(useFunction(dtor, args, canonicalClass(t, at)), addr, destructionMayThrow(t, at));
        return;
      }
      // A class whose destruction spells out more than a few calls is destroyed through its
      // destructor helper (one definition, `@kpp.dtor.<class>`), not inline wherever one is
      // destroyed: a constructor's cleanups destroy every member built so far, and inline each
      // member's members (ControlSettings' constructor held 100,648 String destructor calls, 25.9 MB
      // of IR, 70% of the game's largest module). The helper's own body spells them out.
      if (t != destructorHelperFor && destructionCalls(t, at) > 8)
      {
        callDestructor(destructorThunk(t, at), addr, destructionMayThrow(t, at));
        return;
      }
      destroyMembers(addr, t, at);
    }

    // Whether destroying an object of type t may throw: its destructor is declared `noexcept(false)`
    // (or noexcept with a condition that is false), or has no specifier and a member's or base's
    // destruction may throw (C++ [except.spec]/8: such a destructor is noexcept unless one of
    // theirs is potentially-throwing). The game has eleven that throw by design (ChartedChunksEventHelper,
    // RollingStock::CarriageGuard, ForceManager, MapSerialiser, ...); every other destructor call
    // stays a plain call.
    std::unordered_map<const Ty*, bool> throwingDestructionCache;
    bool destructionMayThrow(const Ty* t, Node* at, int depth = 0)
    {
      t = types.stripRef(t);
      if (t->kind == TyKind::Array)
        return destructionMayThrow(t->inner, at, depth + 1);
      if (depth > 16 || !(isClass(t) || isLambda(t)) || !needsDestruction(t, at))
        return false;
      if (auto it = throwingDestructionCache.find(t); it != throwingDestructionCache.end())
        return it->second;
      bool result = false;
      if (isLambda(t))
      {
        for (const auto& [index, ft] : closureValueFields(t, at))
          result = result || destructionMayThrow(ft, at, depth + 1);
      }
      else
      {
        noteLayout(t);
        ClassDecl* c = classDef(t->decl, at);
        FunctionDecl* dtor = c->isTypedUnion ? nullptr : findDestructor(c);
        if (dtor && (dtor->flags & DF_Noexcept))
          result = !global.isNoexcept(dtor, global.classArgsOf(canonicalClass(t, at)));
        else if (c->isTypedUnion)
        {
          for (size_t i = 0; i < c->alternatives.size() && !result; ++i)
            result = destructionMayThrow(alternativeType(c, t, i, at), at, depth + 1);
        }
        else
          for (const Ty* ft : std::vector<const Ty*>(structFor(t, at).fieldTypes))
            result = result || (!ft->isReference() && destructionMayThrow(ft, at, depth + 1));
      }
      throwingDestructionCache.emplace(t, result);
      return result;
    }

    // A destructor's call (dtor: the function, object: its address). One that may throw is invoked:
    // its exception unwinds through what was built before the object (the caller keeps that out of
    // the lists: popScope). While an exception is already unwinding (in a cleanup pad) its exception
    // ends the program, C++'s std::terminate ([except.terminate]): the call goes through
    // @kpp_destroy_unwinding, which invokes it with abort as the unwind (a cleanup funclet of SEH's
    // personality can hold no invoke itself).
    void callDestructor(const std::string& dtor, const std::string& object, bool mayThrow)
    {
      if (!mayThrow || funcletOverride.empty())
      {
        emitCall("", "void " + dtor + "(ptr " + object + ")", mayThrow);
        return;
      }
      markEH();
      if (declaredNames.claim("@abort"))
        decls += "declare void @abort()\n";
      {
        auto lock = sharedLock();
        if (!emittedShared["@kpp_destroy_unwinding"])
        {
          emittedShared["@kpp_destroy_unwinding"] = true;
          shared += sharedComdat("@kpp_destroy_unwinding") +
                    "define linkonce_odr void @kpp_destroy_unwinding(ptr %dtor, ptr %object) comdat personality ptr @__C_specific_handler {\nentry:\n"
                    "  invoke void %dtor(ptr %object) to label %done unwind label %terminate\n"
                    "done:\n  call void asm sideeffect \"nop\", \"\"()\n  ret void\n"
                    "terminate:\n  %pad = cleanuppad within none []\n  call void @abort() [ \"funclet\"(token %pad) ]\n  unreachable\n}\n";
        }
      }
      fn += "  call void @kpp_destroy_unwinding(ptr " + dtor + ", ptr " + object + ")" + bundle() + "\n";
    }

    // How many calls destroying an object of class type t spells out inline (destroy without a
    // helper), at most 1000.
    std::unordered_map<const Ty*, size_t> destructionCallCounts;
    size_t destructionCalls(const Ty* t, Node* at, int depth = 0)
    {
      t = types.stripRef(t);
      noteLayout(t);
      if (auto it = destructionCallCounts.find(t); it != destructionCallCounts.end())
        return it->second;
      size_t n = 0;
      if (depth > 16)
        n = 1000;
      else if (t->kind == TyKind::Array)
        n = needsDestruction(t->inner, at) ? std::min<size_t>(1000, size_t(t->count) * destructionCalls(t->inner, at, depth + 1)) : 0;
      else if (isLambda(t))
      {
        for (const auto& [index, ft] : closureValueFields(t, at))
          if (needsDestruction(ft, at))
            n += destructionCalls(ft, at, depth + 1);
      }
      else if (isClass(t) && needsDestruction(t, at))
      {
        ClassDecl* c = classDef(t->decl, at);
        if (c->isTypedUnion || findDestructor(c))
          n = 1;
        else
        {
          const StructInfo& info = structFor(t, at);
          for (const Ty* ft : info.fieldTypes)
            if (!ft->isReference() && needsDestruction(ft, at))
              n += destructionCalls(ft, at, depth + 1);
        }
      }
      n = std::min<size_t>(n, 1000);
      destructionCallCounts.emplace(t, n);
      return n;
    }

    void destroyMembers(const std::string& addr, const Ty* t, Node* at)
    {
      StructInfo& info = structFor(t, at);
      std::string name = info.name;
      std::vector<Decl*> fieldDecls = info.fieldDecls;
      std::vector<const Ty*> fieldTypes = info.fieldTypes;
      std::unordered_map<Decl*, uint32_t> fieldIndex = info.fieldIndex;
      std::vector<std::pair<Decl*, uint32_t>> bases = info.bases;
      if (std::any_of(fieldTypes.begin(), fieldTypes.end(), [&](const Ty* ft) { return !ft->isReference() && destructionMayThrow(ft, at); }))
      {
        // A member's or base's destructor may throw: they are a scope of their own, built in order
        // (bases, then members), so that one throwing leaves those built before it to be destroyed.
        pushScope();
        for (auto& [base, index] : bases)
          if (needsDestruction(fieldTypes[index], at))
            scopes.back().push_back({gep(name, addr, index), fieldTypes[index]});
        for (Decl* d : fieldDecls)
          if (const Ty* ft = fieldTypes[fieldIndex[d]]; !ft->isReference() && needsDestruction(ft, at))
            scopes.back().push_back({gep(name, addr, fieldIndex[d]), ft});
        popScope();
        return;
      }
      for (size_t i = fieldDecls.size(); i-- > 0;)
      {
        const Ty* ft = fieldTypes[fieldIndex[fieldDecls[i]]];
        if (!ft->isReference() && needsDestruction(ft, at))
          destroy(gep(name, addr, fieldIndex[fieldDecls[i]]), ft, at);
      }
      for (size_t i = bases.size(); i-- > 0;)
      {
        const Ty* bt = fieldTypes[bases[i].second];
        if (needsDestruction(bt, at))
          destroy(gep(name, addr, bases[i].second), bt, at);
      }
    }

    // Default construction: zero, then default member initializers, then class members' and
    // bases' constructors.
    void defaultConstruct(const std::string& addr, const Ty* t, Node* at)
    {
      ClassDecl* c = classDef(t->decl, at);
      if (c->isTypedUnion)
      {
        // `Property p;`: the first alternative, default-constructed.
        storeZero(t, addr, at);
        if (!c->alternatives.empty())
          constructAlternative(c, t, addr, 0, nullptr, at);
        return;
      }
      if (hasUserConstructors(c))
      {
        FunctionDecl* ctor = findConstructor(t, {}, at);
        if (!ctor && !inheritedConstructorBase(c))
          unsupported(at, "default construction of " + qualifiedName(c) + ", which has no default constructor");
        if (ctor && !(ctor->flags & DF_Default))
        {
          // Through callFunction: `Style(const Style* parent = nullptr)` wants its default argument.
          TemplateArgs args = global.classArgsOf(canonicalClass(t, at));
          if (!ctor->templateParams.empty())
            global.deduceArgs(ctor, {}, args); // `template<typename... Args> OnHeap(Args&&...)`: Args is empty
          callFunction(ctor, args, canonicalClass(t, at), addr, "", {}, at);
          return;
        }
        // `T() = default;` next to other constructors: the memberwise default construction below.
      }
      storeZero(t, addr, at);
      initializeMembers(addr, t, {}, at);
    }

    // The class a constructor initializer of class t names (`super(v)` with `using super = ...`,
    // `Chain<Tail...>(tail...)`): its name converted as a type in the class under the class's
    // arguments; null for a member's name or one that names no class there.
    const Ty* initializerClass(const Ty* t, const FunctionDecl::CtorInit& init, Node* at)
    {
      if (init.member.empty())
        return nullptr;
      const Ty* cls = canonicalClass(t, at);
      ClassDecl* c = classDef(cls->decl, at);
      NamedType named{};
      named.kind = NodeKind::NamedType;
      named.token = at ? at->token : 0;
      named.name = init.member;
      // Looked up from inside the class: from the constructor when it is the one being emitted.
      Decl* context = currentFunction && global.owningClass(currentFunction) && classDef(global.owningClass(currentFunction), at) == c ? static_cast<Decl*>(currentFunction) : c;
      const Ty* nt = global.typeOfAstIn(&named, context, global.classArgsOf(cls));
      return nt && isClass(nt) && !types.isDependent(nt) ? canonicalClass(nt, at) : nullptr;
    }

    // Whether a constructor initializer written with template arguments names another class than
    // cls: `Chain<Tail...>(tail...)` in Chain<Head, Tail...> (its base, an instance of the same
    // template), `Holder<1, T>(b)` beside a base Holder<0, H>.
    bool initializerNamesOther(const Ty* t, const FunctionDecl::CtorInit& init, const Ty* cls, Node* at)
    {
      // Only a name with template arguments or a qualified one can name another class than its
      // last part spells: `Chain<Tail...>(...)`, `UnitTest::TestRunner(reporter)` in a TestRunner.
      if (init.member.empty() || (!init.member.last().hasTemplateArgs && init.member.parts.size() == 1))
        return false;
      const Ty* named = initializerClass(t, init, at);
      return named && named != canonicalClass(cls, at);
    }

    // The constructor initializer that names base `baseTy` of class t by its type (through an
    // alias, `using super = Conditional<...>; Const(const T& v) : super(v) {}`, or one of several
    // instances of a template); null if none does.
    const FunctionDecl::CtorInit* initializerNamingBase(const Ty* t, const Ty* baseTy, Span<FunctionDecl::CtorInit> inits, Node* at)
    {
      const Ty* base = canonicalClass(baseTy, at);
      for (const FunctionDecl::CtorInit& init : inits)
        if (initializerClass(t, init, at) == base)
          return &init;
      return nullptr;
    }

    // Runs emit with the default member initializer of v, a member of the class t whose object is
    // at addr: the initializer as typed for t's instantiation, with `this` the object, t the class
    // of the moment and its arguments the current ones. A base's `Hook* next = this;` refers to the
    // base subobject, also when a derived class's implicit constructor is emitted into a caller.
    template<typename F>
    void withMemberInitializer(const std::string& addr, const Ty* t, VariableDecl* v, F&& emit)
    {
      const Ty* cls = canonicalClass(t, v);
      TemplateArgs classArgs = global.classArgsOf(cls);
      Expr* init = global.instanceInitializer(v, classArgs);
      std::string slot;
      if (!inlining.empty())
        slot = inlineSlot(std::to_string(inlining.size()) + ".member", "ptr"); // in a constructor inlined here: one slot for its members' initializers
      else
      {
        slot = "%v" + std::to_string(temp++);
        allocas += "  " + slot + " = alloca ptr\n";
      }
      fn += "  store ptr " + addr + ", ptr " + slot + "\n";
      struct Restore
      {
        Emitter& e;
        std::optional<Local> self;
        const Ty* classTy;
        const TemplateArgs* args;
        ParsedFile* file;
        Decl* initializer;
        ~Restore()
        {
          if (self)
            e.locals[nullptr] = *self;
          else
            e.locals.erase(nullptr);
          e.currentClassTy = classTy;
          e.currentArgs = args;
          e.currentFile = file;
          e.initializerOf = initializer;
        }
      } restore{*this, locals.count(nullptr) ? std::optional<Local>(locals[nullptr]) : std::nullopt, currentClassTy, currentArgs, currentFile, initializerOf};
      locals[nullptr] = {slot, nullptr, false};
      currentClassTy = cls;
      currentArgs = &classArgs;
      currentFile = v->file;
      initializerOf = v;
      emit(init);
    }

    // A reference member (of type refTy) bound to what source names: the field holds its address, of
    // its base subobject for a reference to a base (EntityAndIndex's `UpdatableEntity& entity` from an
    // InfinityContainer, whose UpdatableEntity comes after its LogisticContainer).
    void bindReferenceMember(const std::string& fieldAddr, const Ty* refTy, Expr* source, Node* at)
    {
      withSingleElement(source, at, [&](Expr* one)
      {
        std::string target;
        if (isClass(types.stripRef(one->ty)))
        {
          Value object = rvalue(one);
          target = object.text;
          const Ty* referenced = refTy ? types.unqualified(types.stripRef(refTy)) : nullptr;
          if (referenced && isClass(referenced) && !types.isDependent(referenced) && isClass(object.ty) &&
              canonicalClass(object.ty, at) != canonicalClass(referenced, at))
            target = convert(object, referenced, at).text; // to the base, or through a conversion operator
        }
        else
          target = lvalue(one);
        fn += "  store ptr " + target + ", ptr " + fieldAddr + "\n";
      });
    }

    // Points the vtable pointer of the object at addr at its class's vtable: once its bases are
    // built (their constructors pointed it at theirs), and after its bytes were copied from an
    // object that may be of a class derived from it.
    void storeVptrs(const std::string& addr, const StructInfo& info)
    {
      if (!info.polymorphic)
        return;
      useVtable(info);
      std::vector<std::string> where = vptrAddresses(addr, info);
      for (size_t i = 0; i < where.size(); ++i)
        fn += "  store ptr " + info.vtables[i].vptrValue + ", ptr " + where[i] + "\n";
    }

    // The vtable pointers of the object at addr, loaded before an assignment copies its bytes and
    // stored back after it: an object keeps its own class's vtables (it may be of a class derived
    // from the one assigned).
    std::vector<std::string> loadVptrs(const std::string& addr, const StructInfo& info)
    {
      std::vector<std::string> saved; // pairs: the value, then where it goes back
      if (!info.polymorphic)
        return saved;
      for (const std::string& at : vptrAddresses(addr, info))
      {
        saved.push_back(newTemp());
        fn += "  " + saved.back() + " = load ptr, ptr " + at + "\n";
        saved.push_back(at);
      }
      return saved;
    }
    void restoreVptrs(const std::vector<std::string>& saved)
    {
      for (size_t i = 0; i + 1 < saved.size(); i += 2)
        fn += "  store ptr " + saved[i] + ", ptr " + saved[i + 1] + "\n";
    }

    // Where the vtable pointers of the object at addr are, in the order of its class's vtables.
    std::vector<std::string> vptrAddresses(const std::string& addr, const StructInfo& info)
    {
      std::vector<std::string> out;
      for (const Vtable& t : info.vtables)
      {
        std::string a = addr;
        const StructInfo* s = &info;
        for (uint32_t index : t.path)
        {
          a = gep(s->name, a, index);
          s = &structFor(s->fieldTypes[index], nullptr);
        }
        out.push_back(a);
      }
      return out;
    }

    // Initialises bases and members of the object at addr from constructor initializers (or
    // default member initializers / default construction when absent).
    // built: a constructor's own initialization: each base and member that needs destruction is
    // destroyed if an exception leaves the constructor after it was built (a later member's
    // initializer or the body throwing), as C++ does.
    void initializeMembers(const std::string& addr, const Ty* t, Span<FunctionDecl::CtorInit> inits, Node* at, bool built = false)
    {
      const StructInfo& info = structFor(t, at);
      struct ImplicitScope
      {
        Emitter& e;
        Decl* saved;
        ~ImplicitScope() { e.implicitConstructionOf = saved; }
      } implicitScope{*this, implicitConstructionOf};
      if (isClass(t) && t->decl)
        if (Decl* own = currentFunction && (currentFunction->flags & DF_Constructor) ? global.owningClass(currentFunction) : nullptr;
            !own || classDef(own, at) != classDef(t->decl, at))
          implicitConstructionOf = classDef(t->decl, at);
      auto builtPart = [&](const std::string& part, const Ty* pt)
      {
        if (built && needsDestruction(pt, at))
        {
          Object o{part, pt};
          o.unwindOnly = true;
          scopes.back().push_back(o);
        }
      };
      auto initFor = [&](uint32_t symbol) -> const FunctionDecl::CtorInit*
      {
        for (const FunctionDecl::CtorInit& init : inits)
          if (!init.member.empty() && init.member.last().symbol == symbol)
            return &init;
        return nullptr;
      };
      if (isClass(t) && t->decl)
      {
        // `Frame(x) : Frame(x, "") {}`: a delegating constructor builds the whole object (not
        // `Chain<Tail...>(tail...)` in Chain<Head, Tail...>, which initializes its base).
        const QualifiedName& own = classDef(t->decl, at)->name;
        if (const FunctionDecl::CtorInit* self = own.empty() ? nullptr : initFor(own.last().symbol); self && !initializerNamesOther(t, *self, t, at))
        {
          constructInto(addr, t, self->args, at);
          return;
        }
      }
      // Each base's and member's initializer is a full-expression of its own ([class.base.init] 7):
      // the temporaries it made die when it is done, not when every member is (ControlSettings'
      // constructor kept 980 temporaries alive to its end, and each member's cleanup destroyed all
      // the earlier ones again: 97,512 destructor calls in 96,730 cleanup pads).
      for (auto& [baseDef, index] : info.bases)
      {
        size_t partTemps = statementTemps.size();
        const Ty* bt = info.fieldTypes[index];
        std::string baseAddr = gep(info.name, addr, index);
        const QualifiedName& bn = static_cast<ClassDecl*>(baseDef)->name;
        const FunctionDecl::CtorInit* init = bn.empty() ? nullptr : initFor(bn.last().symbol);
        if (init && initializerNamesOther(t, *init, bt, at))
          init = nullptr; // `Holder<0, H>(a)` met first for the base Holder<1, T>: by the type each names
        if (!init)
          init = initializerNamingBase(t, bt, inits, at);
        if (init)
          constructInto(baseAddr, bt, init->args, at);
        else if (hasUserConstructors(static_cast<ClassDecl*>(baseDef)))
          defaultConstruct(baseAddr, bt, at);
        else
          initializeMembers(baseAddr, bt, {}, at);
        destroyTempsFrom(partTemps);
        builtPart(baseAddr, bt);
      }
      storeVptrs(addr, info);
      if (info.isUnion)
      {
        // A union with no member named by a constructor initializer: its member with a default
        // member initializer is the one initialized, as in C++ (`union { int64_t raw = 0; ... }`).
        bool named = false;
        for (Decl* d : info.fieldDecls)
          if (d->kind == NodeKind::VariableDecl && !static_cast<VariableDecl*>(d)->name.empty())
            named = named || initFor(static_cast<VariableDecl*>(d)->name.last().symbol);
        // The member a constructor initializer names, at the union's start (UpgradableEntity's
        // `WrappedValue(EntityWithHealth& wrapped) : wrappedEntityWithHealth(&wrapped) {}`).
        if (named)
          for (Decl* d : info.fieldDecls)
            if (d->kind == NodeKind::VariableDecl && !static_cast<VariableDecl*>(d)->name.empty())
              if (const FunctionDecl::CtorInit* init = initFor(static_cast<VariableDecl*>(d)->name.last().symbol))
              {
                const Ty* ft = unionMemberType(t, d, at);
                if (isClass(ft))
                  constructInto(addr, ft, init->args, at);
                else if (init->args.empty())
                  storeZero(ft, addr, at); // `: raw()`: value-initialized
                else if (init->args.size() == 1)
                  withSingleElement(init->args[0], at, [&](Expr* one) { initializeFrom(addr, ft, one, at); });
                else
                  unsupported(at, "a union member initializer of several values");
                break;
              }
        if (!named)
          for (Decl* d : info.fieldDecls)
            if (d->kind == NodeKind::VariableDecl && static_cast<VariableDecl*>(d)->init && !bitfieldIn(info, d))
            {
              auto* v = static_cast<VariableDecl*>(d);
              const Ty* ft = unionMemberType(t, d, at);
              if (const Ty* declared = global.typeOfDecl(v); !ft->isReference() && !(declared && declared->isReference()))
                withMemberInitializer(addr, t, v, [&](Expr* value) { initializeFrom(addr, ft, value, v); });
              break;
            }
        return;
      }
      std::set<size_t> zeroedUnits; // the storage units of bit fields, each zeroed at its first member
      for (Decl* d : info.fieldDecls)
      {
        if (d->kind == NodeKind::ClassDecl)
        {
          // An anonymous struct or union member (`union { SharedString sharedStr; LongStr longStr;
          // ... }` with `: longStr{nullptr, 0}`): its members are initialised as the class's own,
          // by the same constructor initializers.
          initializeAnonymous(gep(info.name, addr, fieldIndexOf(info, d)), info.fieldTypes[fieldIndexOf(info, d)], static_cast<ClassDecl*>(d),
                              inits, at);
          continue;
        }
        if (d->kind != NodeKind::VariableDecl)
          continue;
        auto* v = static_cast<VariableDecl*>(d);
        size_t partTemps = statementTemps.size();
        const Ty* ft = info.fieldTypes[fieldIndexOf(info, d)];
        std::string fieldAddr = gep(info.name, addr, fieldIndexOf(info, d));
        const FunctionDecl::CtorInit* init = v->name.empty() ? nullptr : initFor(v->name.last().symbol);
        if (bitfieldIn(info, d))
        {
          // The unit's first bit field stands for the unit: each bit field in it is initialised.
          for (Decl* m : unitMembers(info, classDef(t->decl, at), d))
          {
            auto* bv = static_cast<VariableDecl*>(m);
            const FunctionDecl::CtorInit* bitInit = bv->name.empty() ? nullptr : initFor(bv->name.last().symbol);
            if (bitInit && bitInit->args.size() > 1)
              unsupported(at, "this member initializer");
            initializeBitfield(addr, t, info, bv, *bitfieldIn(info, m), bitInit != nullptr, bitInit && bitInit->args.size() == 1 ? bitInit->args[0] : nullptr,
                               zeroedUnits, at);
          }
          continue;
        }
        if (const Ty* declared = global.typeOfDecl(v); ft->isReference() || (declared && declared->isReference()))
        {
          // A reference member: the field holds the address of what it refers to.
          if (!init && v->init)
          {
            withMemberInitializer(addr, t, v, [&](Expr* source) { bindReferenceMember(fieldAddr, ft->isReference() ? ft : declared, source, at); });
            continue;
          }
          Expr* source = init && init->args.size() == 1 ? init->args[0] : nullptr;
          if (!source)
            unsupported(at, "a reference member without an initializer");
          bindReferenceMember(fieldAddr, ft->isReference() ? ft : declared, source, at);
          continue;
        }
        if (init)
        {
          if (isClass(ft))
            constructInto(fieldAddr, ft, init->args, at);
          else if (isLambda(ft) && init->args.size() == 1)
            withSingleElement(init->args[0], at, [&](Expr* one) { initializeFrom(fieldAddr, ft, one, at); }); // `f(f)`: the closure's bytes, not its address
          else if (size_t values = expandedCount(init->args, at); values == 0)
            storeZero(ft, fieldAddr, at); // `value()`, `value(forward<Args>(args)...)` with Args empty: value-initialized
          else if (values == 1)
            forEachExpanded(init->args, at, [&](Expr* one)
            {
              // A member initializer is direct-initialization: a class value converts through an
              // explicit conversion operator too (Pair's `second(forward<U2>(second))` with
              // ParsedNoiseExpression's `explicit operator const NoiseExpression*()`).
              Value value = rvalue(one);
              if (isClass(value.ty) && !isClass(ft) && ft->kind != TyKind::Lambda && !ft->isReference())
                value = convert(value, ft, at, true);
              store(value, fieldAddr, ft, at);
            }); // `value(first, args...)`, args empty
          else
            unsupported(at, "this member initializer");
        }
        else if (v->init)
          withMemberInitializer(addr, t, v, [&](Expr* value) { initializeFrom(fieldAddr, ft, value, v); });
        else if (isClass(ft) && hasUserConstructors(classDef(ft->decl, at)))
          defaultConstruct(fieldAddr, ft, at);
        else if (isClass(ft))
          initializeMembers(fieldAddr, ft, {}, at);
        else if (ft->kind == TyKind::Array && (needsDestruction(ft, at) || needsConstruction(ft, at))) // elements with constructors or default member initializers
          constructArray(fieldAddr, ft, at);
        else
          storeZero(ft, fieldAddr, at);
        destroyTempsFrom(partTemps);
        builtPart(fieldAddr, ft);
      }
    }

    // Whether a constructor initializer names a data member of the anonymous struct or union c (or
    // of one nested in it).
    static bool namesMemberOf(ClassDecl* c, Span<FunctionDecl::CtorInit> inits)
    {
      for (Decl* m : c->members)
      {
        if (m->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(m)->name.empty() && static_cast<ClassDecl*>(m)->isDefinition)
        {
          if (namesMemberOf(static_cast<ClassDecl*>(m), inits))
            return true;
          continue;
        }
        if (m->kind != NodeKind::VariableDecl || (m->flags & (DF_Static | DF_Typedef)) || static_cast<VariableDecl*>(m)->name.empty())
          continue;
        for (const FunctionDecl::CtorInit& init : inits)
          if (!init.member.empty() && init.member.last().symbol == static_cast<VariableDecl*>(m)->name.last().symbol)
            return true;
      }
      return false;
    }

    // An anonymous struct or union member of a class being constructed, at addr, of type t (c its
    // declaration). A struct: its members as the class's own (initializeMembers). A union: zeroed
    // (members are zero-initialised in kpp), then the member a constructor initializer names, or
    // else the one with a default member initializer, is initialised: C++ initialises one member.
    void initializeAnonymous(const std::string& addr, const Ty* t, ClassDecl* c, Span<FunctionDecl::CtorInit> inits, Node* at)
    {
      if (!structFor(t, at).isUnion)
      {
        initializeMembers(addr, t, inits, at);
        return;
      }
      storeZero(t, addr, at);
      Decl* chosen = nullptr;
      for (Decl* m : c->members)
        if (((m->kind == NodeKind::VariableDecl && !(m->flags & (DF_Static | DF_Typedef))) ||
             (m->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(m)->name.empty() && static_cast<ClassDecl*>(m)->isDefinition)) &&
            !chosen)
        {
          if (m->kind == NodeKind::ClassDecl ? namesMemberOf(static_cast<ClassDecl*>(m), inits) : false)
            chosen = m;
          else if (m->kind == NodeKind::VariableDecl && !static_cast<VariableDecl*>(m)->name.empty())
            for (const FunctionDecl::CtorInit& init : inits)
              if (!init.member.empty() && init.member.last().symbol == static_cast<VariableDecl*>(m)->name.last().symbol)
                chosen = m;
        }
      if (!chosen)
        for (Decl* m : c->members)
          if (m->kind == NodeKind::VariableDecl && !(m->flags & (DF_Static | DF_Typedef)) && static_cast<VariableDecl*>(m)->init && !chosen)
            chosen = m;
      if (!chosen)
        return;
      const Ty* mt = unionMemberType(t, chosen, at);
      if (chosen->kind == NodeKind::ClassDecl)
      {
        initializeAnonymous(addr, mt, static_cast<ClassDecl*>(chosen), inits, at); // a struct inside the union, at its start
        return;
      }
      auto* v = static_cast<VariableDecl*>(chosen);
      if (v->bitfield) // (a union's layout has no bit-field places: it would be its whole member)
        unsupported(at, "a bit field of an anonymous union initialised by a constructor");
      const FunctionDecl::CtorInit* init = nullptr;
      for (const FunctionDecl::CtorInit& i : inits)
        if (!i.member.empty() && i.member.last().symbol == v->name.last().symbol)
          init = &i;
      if (const Ty* declared = global.typeOfDecl(v); mt->isReference() || (declared && declared->isReference()))
        unsupported(at, "a reference member of an anonymous union");
      if (!init)
      {
        withMemberInitializer(addr, t, v, [&](Expr* value) { initializeFrom(addr, mt, value, v); });
        return;
      }
      if (isClass(mt))
        constructInto(addr, mt, init->args, at);
      else if (size_t values = expandedCount(init->args, at); values == 0)
        storeZero(mt, addr, at);
      else if (values == 1)
        forEachExpanded(init->args, at, [&](Expr* one) { initializeFrom(addr, mt, one, at); });
      else
        unsupported(at, "this member initializer");
    }

    // The type of member m of union t (every member at the union's start, field 0 standing for none
    // of them): a data member's, or an anonymous struct's with the union's arguments.
    const Ty* unionMemberType(const Ty* t, Decl* m, Node* at)
    {
      const Ty* key = canonicalClass(t, at);
      if (m->kind == NodeKind::VariableDecl)
        return global.typeOfDeclIn(m, global.classArgsOf(key));
      return types.classType(m, key->args);
    }

    // Whether every element of the brace list converts to the element type of the InitializerList
    // constructor ctor of class t: only then is ctor the one, otherwise the other constructors take
    // the elements (C++'s two phases: `Vector<int*> v{set.begin(), set.end()}` is the range). An
    // element of unknown type, or a list itself, counts as converting.
    bool listConvertsTo(FunctionDecl* ctor, BraceInitExpr* list, const Ty* t, Node* at)
    {
      TemplateArgs classArgs = global.classArgsOf(canonicalClass(t, at));
      ArgsScope scope(*this, &classArgs);
      const Ty* pt = declType(ctor->params[0]);
      pt = pt ? types.unqualified(types.stripRef(pt)) : nullptr;
      if (!pt || pt->args.empty())
        return true;
      const Ty* element = pt->args[0];
      for (Expr* a : list->args)
      {
        // A list as an element (`Vector<ID> ids{{ID(1), ID(2), ID(3)}}`): one only when the element
        // type is list-initialized from it; otherwise the list is the InitializerList itself, the
        // argument of the second phase.
        if (a && a->kind == NodeKind::BraceInitExpr && !types.isDependent(element))
          if (const Ty* inner = braceListType(a, global.initializerListDecl(), types); inner && !global.converts(inner, element))
            return false;
        if (!a || a->kind == NodeKind::BraceInitExpr || isPackExpansion(a) || !a->ty || !a->ty->isKnown())
          continue;
        const Ty* at = withCurrentArgs(a->ty);
        if (types.isDependent(at) || types.isDependent(element))
          continue;
        if (!global.converts(argumentType(a, at, types), element))
          return false;
      }
      return true;
    }

    // The constructor of class t that takes an InitializerList (the rest defaulted), or null.
    FunctionDecl* initializerListConstructor(ClassDecl* c, const Ty* t, Node* at)
    {
      TemplateArgs classArgs = global.classArgsOf(canonicalClass(t, at));
      ArgsScope scope(*this, &classArgs);
      for (Decl* m : c->members)
      {
        if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Constructor) || (m->flags & DF_Delete))
          continue;
        auto* f = static_cast<FunctionDecl*>(m);
        if (f->params.empty() || !f->templateParams.empty())
          continue;
        bool othersDefaulted = true;
        for (size_t i = 1; i < f->params.size(); ++i)
          othersDefaulted = othersDefaulted && static_cast<ParamDecl*>(f->params[i])->defaultValue != nullptr;
        if (!othersDefaulted)
          continue;
        const Ty* pt = declType(f->params[0]);
        if (pt && isInitializerListType(types.stripRef(pt)))
          return f;
      }
      return nullptr;
    }

    // Constructs an object of class type t at addr from constructor arguments.
    // (class, argument) of the one-argument constructions under way: a constructor whose parameter
    // needs the same conversion again would otherwise recurse until the stack runs out.
    std::vector<std::pair<const Ty*, Expr*>> constructing;
    struct ConstructingScope
    {
      std::vector<std::pair<const Ty*, Expr*>>& list;
      ConstructingScope(std::vector<std::pair<const Ty*, Expr*>>& list, const Ty* t, Expr* arg) : list(list) { list.push_back({t, arg}); }
      ~ConstructingScope() { list.pop_back(); }
    };

    // `T(x)` of class type t into addr: the constructors take the arguments; and, as
    // direct-initialization lets the copy or move constructor take what an explicit conversion
    // operator gives (C++'s rule for a constructor's first parameter), `Constant(parsed)` with
    // Parsed's `explicit operator const Constant&()` and `AchievementId(id)` with `explicit operator
    // AchievementId()` when no constructor takes the object itself.
    void directInitialize(const std::string& addr, const Ty* t, Span<Expr*> args, bool braced, Node* at, bool listElements)
    {
      if (braced && isInitializerListType(types.unqualified(t)))
      {
        // `InitializerList<TestCase>{{...}, {...}}` (a range-for's range in the tests): the list
        // over its elements' array, which lives as long as the object made here does.
        if (!listElements && args.size() == 1 && args[0] && args[0]->kind == NodeKind::BraceInitExpr && !static_cast<BraceInitExpr*>(args[0])->text)
        {
          initializerListTemporary(types.unqualified(t), static_cast<BraceInitExpr*>(args[0]), at, addr);
          return;
        }
        BraceInitExpr list{};
        list.kind = NodeKind::BraceInitExpr;
        list.token = at ? at->token : 0;
        list.args = args;
        initializerListTemporary(types.unqualified(t), &list, at, addr);
        return;
      }
      Expr* arg = args.size() == 1 && !braced ? args[0] : nullptr;
      const Ty* argTy = arg && arg->ty && arg->ty->isKnown() ? withCurrentArgs(types.unqualified(types.stripRef(arg->ty))) : nullptr;
      if (argTy && isClass(argTy) && !types.isDependent(argTy) && classKey(argTy) != classKey(t) && !baseInstantiation(argTy, classDef(t->decl, at), at) &&
          !constructorFor(t, args, at) && !global.converts(argumentType(arg, arg->ty, types), types.unqualified(t)))
      {
        // The operator gives the class (`operator AchievementId()`) or a reference to it (`operator
        // const Constant&()`): either way the copy or move constructor's argument.
        const Ty* value = types.unqualified(t);
        const Ty* reference = types.lref(types.qualified(value, true, false));
        const Ty* target = conversionOperator(argTy, value, at, nullptr, true) ? value : conversionOperator(argTy, reference, at, nullptr, true) ? reference : nullptr;
        if (target)
        {
          Value converted = convert(rvalue(arg), target, at, true);
          if (converted.isTemporary)
            moveTemporary(addr, converted, value, at);
          else
            copyConstruct(addr, converted.text, value, at);
          return;
        }
      }
      constructInto(addr, t, args, at, listElements);
    }

    // listElements: args are a brace list's elements (the list taken apart once already).
    void constructInto(const std::string& addr, const Ty* t, Span<Expr*> args, Node* at, bool listElements = false)
    {
      ClassDecl* c = classDef(t->decl, at);
      if (args.empty())
      {
        defaultConstruct(addr, t, at);
        return;
      }
      std::optional<ConstructingScope> guard;
      if (args.size() == 1)
      {
        const Ty* key = canonicalClass(t, at);
        for (auto& [k, a] : constructing)
          if (k == key && a == args[0])
            unsupported(at, "converting " + (args[0]->ty ? types.toString(args[0]->ty, &symbols) : std::string("a value")) + " to " +
                                types.toString(key, &symbols) + ": the constructor chosen needs the same conversion for its own parameter");
        guard.emplace(constructing, key, args[0]);
      }
      // (`return {{a, b}};` for an Optional<Pair<int, int>>: the inner list, one element of the
      // outer one, is an argument of a constructor, not the class's own list again; nor is the
      // one value of `{.effectTexture = {bitmap}}`, which is the designated member's.)
      if (args.size() == 1 && args[0]->kind == NodeKind::BraceInitExpr && !static_cast<BraceInitExpr*>(args[0])->text && !(listElements && hasUserConstructors(c)) &&
          !(listElements && !pendingDesignators.empty()))
      {
        // `T x{y}` with y a T, or of a class derived from T: a copy or a move of y, before any
        // initializer-list constructor (C++ DR 1467): `[names{::move(names)}]` moves the Vector.
        auto* list = static_cast<BraceInitExpr*>(args[0]);
        if (list->args.size() == 1 && list->designators.empty() && list->args[0]->kind != NodeKind::BraceInitExpr && list->args[0]->ty)
        {
          const Ty* et = withCurrentArgs(types.stripRef(list->args[0]->ty));
          std::vector<std::pair<std::string, uint32_t>> path;
          if (et && isClass(et) && !types.isDependent(et) &&
              (canonicalClass(et, at) == canonicalClass(t, at) || basePath(et, t, path, at)))
          {
            constructInto(addr, t, list->args, at);
            return;
          }
        }
        // `Vector<Item> items = {}` of a class with a default constructor: value-initialisation (C++
        // [dcl.init.list] 3.5), not the InitializerList constructor, whose body copies elements (a
        // Vector of a move-only class: CommandProcessor's `Vector<SavedSpecialItemReference> ... = {}`).
        bool valueInitialised = list->args.empty() && findConstructor(t, {}, at);
        if (FunctionDecl* ctor = valueInitialised ? nullptr : initializerListConstructor(c, t, at); ctor && listConvertsTo(ctor, list, t, at))
        {
          // `Vector<int> v = {1, 2, 3}`, `Poly p({1, 2, 3, 4})`: the list as one InitializerList argument.
          TemplateArgs classArgs = global.classArgsOf(canonicalClass(t, at));
          callFunction(ctor, classArgs, canonicalClass(t, at), addr, "", args, at);
          return;
        }
        pendingDesignators = static_cast<BraceInitExpr*>(args[0])->designators;
        constructInto(addr, t, static_cast<BraceInitExpr*>(args[0])->args, at, true);
        return;
      }
      Span<uint32_t> designators = pendingDesignators;
      pendingDesignators = {};
      // `U(U::Text, x)`, `U u(U::Where, 1, 2)`, `return U(U::Comment, s)`: a tag first names the
      // alternative, the rest build it (its value, or its constructor's arguments), whichever others
      // share its type, and nothing else is built first (design.md 8.5, decided by kovarex 2026-10-31).
      if (c->isTypedUnion && !args.empty() && args[0]->ty && types.unqualified(types.stripRef(args[0]->ty)) == tagType(c))
      {
        std::optional<int64_t> index = constantTag(c, args[0]);
        if (!index || *index < 0 || size_t(*index) >= c->alternatives.size())
          fail(args[0], "the tag constructing the typed union " + qualifiedName(c) + " is no constant one of its alternatives");
        storeZero(t, addr, at);
        if (args.size() <= 2)
          constructAlternative(c, t, addr, size_t(*index), args.size() == 2 ? args[1] : nullptr, at);
        else
        {
          setTag(c, t, addr, size_t(*index), at);
          constructInto(alternativeAddress(c, t, addr, size_t(*index), at), alternativeType(c, t, size_t(*index), at),
                        Span<Expr*>{args.data + 1, uint32_t(args.size() - 1)}, at);
        }
        return;
      }
      if (c->isTypedUnion && args.size() == 1 && args[0]->ty && args[0]->ty->isKnown() && !isPackExpansion(args[0])) // an expansion: its element, below
      {
        // `Shape a(s)`, `Shape(7)`, a member initializer `shape(s)`, `new (slot) Shape(x)`: as
        // copy-initialization, a value of the union itself copies or moves it, any other the
        // alternative of its type (design.md 8.5).
        const Ty* argTy = types.stripRef(args[0]->ty);
        if (!(isClass(argTy) && classKey(argTy) == classKey(t)))
        {
          storeZero(t, addr, at);
          constructAlternative(c, t, addr, alternativeForValue(c, t, args[0]->ty, at), args[0], at);
          return;
        }
        initializeFrom(addr, t, args[0], at);
        return;
      }
      // `using Base::Base;`: the base's constructors, when none of the class's own takes the
      // arguments (a copy stays a copy).
      bool copyLike = args.size() == 1 && args[0]->ty && isClass(types.stripRef(args[0]->ty)) && classKey(types.stripRef(args[0]->ty)) == classKey(t);
      // (A value whose class converts to t itself takes that conversion: `return connector;` for a
      // SignalID through SignalIDConnector's `operator SignalID()`, not a base's constructors.)
      bool converts = args.size() == 1 && !copyLike && args[0]->ty && isClass(types.stripRef(args[0]->ty)) &&
                      !types.isDependent(types.stripRef(args[0]->ty)) && conversionOperator(types.stripRef(args[0]->ty), t, at);
      if (!copyLike && !converts && (!hasUserConstructors(c) || !constructorFor(t, args, at)))
        if (std::optional<QualifiedName> base = inheritedConstructorBase(c))
        {
          // Inherited constructors: the arguments build that base, everything else is default.
          FunctionDecl::CtorInit init{};
          init.member = *base;
          init.args = args;
          initializeMembers(addr, t, Span<FunctionDecl::CtorInit>{&init, 1}, at);
          return;
        }
      if (args.size() == 1 && isPackExpansion(args[0]))
      {
        // `T(forward<Args>(args)...)`: the elements are the arguments. With one element the
        // constructor choice (copy, move, converting) sees that element's type; several elements
        // of a class without constructors fill its fields in order; otherwise the constructor
        // call below expands the pack itself.
        Expr* pattern = static_cast<UnaryExpr*>(args[0])->operand;
        size_t count = packElementCount(pattern, at);
        if (count == 0)
        {
          defaultConstruct(addr, t, at);
          return;
        }
        if (count == 1)
        {
          forEachPackElement(pattern, at, [&](size_t)
          {
            Expr* one = pattern;
            constructInto(addr, t, Span<Expr*>{&one, 1}, at);
          });
          return;
        }
        if (!hasUserConstructors(c))
        {
          const StructInfo& info = structFor(t, at);
          storeZero(t, addr, at);
          size_t i = 0;
          forEachPackElement(pattern, at, [&](size_t)
          {
            if (i >= info.fieldDecls.size())
              unsupported(at, "more constructor arguments than " + qualifiedName(c) + " has fields");
            Decl* d = info.fieldDecls[i++];
            initializeFrom(gep(info.name, addr, fieldIndexOf(info, d)), info.fieldTypes[fieldIndexOf(info, d)], pattern, at);
          });
          return;
        }
      }
      const Ty* firstTy = args.size() == 1 ? args[0]->ty : nullptr;
      FunctionDecl* ctor = constructorFor(t, args, at);
      if (ctor && firstTy && isClass(types.stripRef(firstTy)) && classKey(firstTy) == classKey(t) && !ctor->params.empty())
      {
        // The same class: a copy, unless the chosen constructor is a copy constructor itself (a
        // converting one, deleted or not, reached through a conversion of the class, loses to
        // the implicit copy: `Ref(const int&&) = delete` beside Ref's `operator const int&()`;
        // `ConvertingElement(const Tree&, uint32_t index = 0)` beside Tree's converting template).
        TemplateArgs classArgs = global.classArgsOf(canonicalClass(t, at));
        if (!ctor->templateParams.empty())
          global.deduceArgs(ctor, argTypesOf(args), classArgs); // `template<typename... Args> Str(Args&&...)` as Str(Str&): as deduced
        const Ty* pt;
        {
          ArgsScope scope(*this, &classArgs);
          pt = declType(ctor->params[0]);
        }
        if (pt && pt->kind == TyKind::Pack)
          pt = pt->args.empty() ? nullptr : pt->args[0];
        if (!pt || !isClass(types.stripRef(pt)) || classKey(pt) != classKey(t))
          ctor = nullptr; // a converting constructor of another class: not for a copy
        else if (!ctor->templateParams.empty() && implicitCopyOrMoveTakes(c, t, pt, at))
          ctor = nullptr; // as good as the implicit copy or move constructor, which wins as no template
      }
      if (ctor && (ctor->flags & DF_Delete))
        unsupported(at, "constructing " + types.toString(t, &symbols) + " with a deleted constructor (copying a move-only object?)");
      if (ctor && firstTy && isClass(types.stripRef(firstTy)) && classKey(firstTy) != classKey(t) && ctor->params.size() == 1)
      {
        TemplateArgs classArgs = global.classArgsOf(canonicalClass(t, at));
        const Ty* pt;
        {
          ArgsScope scope(*this, &classArgs);
          pt = declType(ctor->params[0]);
        }
        if (pt && isClass(types.stripRef(pt)) && classKey(pt) == classKey(t))
        {
          // The copy constructor, chosen for a value of another class: that class converts to t
          // first (a base, or its conversion operator), the constructor is not called with it.
          Value src = convert(rvalue(args[0]), types.unqualified(t), at);
          if (src.isTemporary)
            moveTemporary(addr, src, t, at);
          else if (movable(args[0]))
            moveConstruct(addr, src.text, t, at);
          else
            copyConstruct(addr, src.text, t, at);
          return;
        }
      }
      if (ctor)
      {
        TemplateArgs ctorArgs = global.classArgsOf(canonicalClass(t, at));
        if (!ctor->templateParams.empty())
          global.deduceArgs(ctor, argTypesOf(args), ctorArgs); // `template<typename F> Function(F)`: F from the argument
        callFunction(ctor, ctorArgs, canonicalClass(t, at), addr, "", args, at);
        return;
      }
      if (args.size() == 1 && firstTy && isClass(types.stripRef(firstTy)) && classKey(firstTy) != classKey(t))
      {
        // A value of a class derived from t, no constructor of t taking it as written (t's copy
        // and move constructors are implicit, or t is an aggregate): its t part, copied or moved
        // (`Name(const Name& n) : Text(n)`, `: Plain(d)`).
        std::vector<std::pair<std::string, uint32_t>> path;
        if (const Ty* from = withCurrentArgs(types.unqualified(types.stripRef(firstTy))); !types.isDependent(from) && basePath(from, t, path, at))
        {
          Value src = convert(rvalue(args[0]), types.unqualified(t), at);
          if (movable(args[0]))
            moveConstruct(addr, src.text, t, at);
          else
            copyConstruct(addr, src.text, t, at);
          return;
        }
      }
      if (args.size() == 1 && firstTy && isClass(types.stripRef(firstTy)) && classKey(firstTy) != classKey(t) &&
          conversionOperator(types.stripRef(firstTy), t, at))
      {
        // No constructor of t takes the value, its class converts to t: `ProtoID copy = connector;`
        // and `h.chosen = connector;` through IDConnector's `implicit operator IDType()`.
        Value src = convert(rvalue(args[0]), types.unqualified(t), at);
        if (src.isTemporary)
          moveTemporary(addr, src, t, at);
        else
          copyConstruct(addr, src.text, t, at);
        return;
      }
      if (args.size() == 1 && firstTy && isClass(types.stripRef(firstTy)) && classKey(firstTy) == classKey(t))
      {
        if (args[0]->kind == NodeKind::CallExpr && !firstTy->isReference())
        {
          // `: simulations(loadSimulations(input))`: the result built in the object itself (C++17),
          // as `T x = f();` is; a Map moved bitwise afterwards kept its header pointing at the dead
          // temporary.
          Value v = callValue(static_cast<CallExpr*>(args[0]), addr);
          if (!isClass(v.ty) || v.text == addr)
            return;
          if (v.isTemporary)
            moveTemporary(addr, v, t, at);
          else
            copyConstruct(addr, v.text, t, at);
          return;
        }
        Value src = rvalue(args[0]);
        if (src.isTemporary)
          moveTemporary(addr, src, t, at);
        else if (movable(args[0]))
          moveConstruct(addr, src.text, t, at);
        else
          copyConstruct(addr, src.text, t, at);
        return;
      }
      if (!hasUserConstructors(c))
      {
        // An aggregate: the values fill the fields in order; the fields left over get their
        // default member initializers (or default construction), as in C++.
        const StructInfo& info = structFor(t, at);
        // `Item {load, ifNoPrototype, ::forward<Args>(args)...}` with Args empty: the expansion of an
        // empty pack is no value (InstanceLoader::registerLoader).
        std::vector<Expr*> values;
        bool expanded = false;
        for (Expr* a : args)
        {
          if (!isPackExpansion(a))
          {
            values.push_back(a);
            continue;
          }
          expanded = true;
          if (expandedCount(Span<Expr*>{&a, 1}, at) != 0)
            unsupported(at, "a pack expanded into the values of an aggregate");
        }
        if (expanded)
          args = Span<Expr*>{values.data(), uint32_t(values.size())};
        storeZero(t, addr, at);
        storeVptrs(addr, info);
        if (info.isUnion && designators.empty() && args.size() == 1 && !info.fieldDecls.empty())
        {
          // `{nullptr}` for a union: its first member, at the union's start (Lua's TValue's Value).
          initializeFrom(addr, unionMemberType(t, info.fieldDecls[0], at), args[0], at);
          return;
        }
        if (info.fieldDecls.size() == 1 && info.bases.empty() && designators.empty() && args.size() > 1)
        {
          // Brace elision: `Array<int, 3> a = {1, 2, 3}` fills the one array field element by element.
          const Ty* ft = info.fieldTypes[fieldIndexOf(info, info.fieldDecls[0])];
          if (ft->kind == TyKind::Array)
          {
            std::string fieldAddr = gep(info.name, addr, fieldIndexOf(info, info.fieldDecls[0]));
            if (args.size() > ft->count)
              unsupported(at, "more values than the array of " + qualifiedName(c) + " holds");
            for (size_t i = 0; i < args.size(); ++i)
            {
              std::string element = newTemp();
              fn += "  " + element + " = getelementptr inbounds " + llType(ft, at) + ", ptr " + fieldAddr + ", i64 0, i64 " + std::to_string(i) + "\n";
              initializeFrom(element, ft->inner, args[i], at);
            }
            return;
          }
        }
        // C++17 aggregate init: base sub-objects consume the first args, then data members.
        size_t argIdx = 0;
        if (!info.bases.empty() && designators.empty())
        {
          for (auto& [baseDef, baseFieldIdx] : info.bases)
          {
            if (argIdx >= args.size())
              break;
            std::string baseAddr = gep(info.name, addr, baseFieldIdx);
            const Ty* bt = info.fieldTypes[baseFieldIdx];
            initializeFrom(baseAddr, bt, args[argIdx++], at);
          }
          storeVptrs(addr, info); // the bases built pointed it at theirs
        }
        // The members the values go to: the fields, and each bit field of a storage unit on its
        // own (the unit's first bit field stands for the unit among the fields).
        std::vector<Decl*> members;
        for (Decl* d : info.fieldDecls)
          if (d->kind == NodeKind::VariableDecl && bitfieldIn(info, d))
            for (Decl* m : unitMembers(info, c, d))
              members.push_back(m);
          else
            members.push_back(d);
        // Which value goes to which member: in order, or by name for `.x = 1` designators.
        std::vector<Expr*> valueOf(members.size(), nullptr);
        std::vector<std::vector<Expr*>> elided(members.size()); // an array member's values without their braces
        size_t next = 0;
        for (size_t k = argIdx; k < args.size(); ++k)
        {
          uint32_t designator = k < designators.size() ? designators[k] : UINT32_MAX;
          if (designator != UINT32_MAX)
          {
            size_t found = members.size();
            for (size_t j = 0; j < members.size(); ++j)
              if (members[j]->kind == NodeKind::VariableDecl && !static_cast<VariableDecl*>(members[j])->name.empty() &&
                  static_cast<VariableDecl*>(members[j])->name.last().symbol == designator)
                found = j;
            if (found == members.size())
              unsupported(at, "a designator naming no field of " + qualifiedName(c) + " (" + sym(designator) + ")");
            valueOf[found] = args[k];
            next = found + 1;
            continue;
          }
          if (next >= valueOf.size())
          {
            if (valueOf.empty())
              break; // anonymous/incomplete class with no known fields — already zeroed
            unsupported(at, "more values than " + qualifiedName(c) + " has fields");
          }
          // Brace elision: an array member takes its elements' values from the list itself when the
          // value there is no list and no array (`GUID g = { 0x429b8c22, 0x9188, 0x4b0c, 0x87, ... }`
          // fills Data4[8] from the last eight).
          if (members[next]->kind == NodeKind::VariableDecl && !bitfieldIn(info, members[next]))
            if (const Ty* ft = types.unqualified(info.fieldTypes[fieldIndexOf(info, members[next])]); ft->kind == TyKind::Array && ft->count > 0)
              if (Expr* a = args[k]; a->kind != NodeKind::BraceInitExpr && a->ty && a->ty->isKnown() &&
                                     !(a->kind == NodeKind::LiteralExpr && static_cast<LiteralExpr*>(a)->literalKind == TokenKind::String) &&
                                     types.unqualified(types.stripRef(a->ty))->kind != TyKind::Array)
              {
                size_t take = std::min<size_t>(ft->count, args.size() - k);
                elided[next] = std::vector<Expr*>(args.begin() + k, args.begin() + k + take);
                k += take - 1;
                ++next;
                continue;
              }
          valueOf[next++] = args[k];
        }
        if (info.flexible && !valueOf.empty() && valueOf.back())
          fail(at, "a value for " + qualifiedName(c) + "::" + sym(static_cast<VariableDecl*>(members.back())->name.last().symbol) + ", a flexible array member, which has no elements of its own to initialize");
        size_t i = 0;
        std::set<size_t> zeroedUnits; // the storage units of bit fields, each zeroed at its first member
        // A union holds one member: the one a value is for (`{.effectTexture = {bitmap}}`), else
        // the one with a default member initializer, else its first (C++'s rule), each at the
        // union's start with its own type.
        Decl* unionMember = nullptr;
        if (info.isUnion)
        {
          for (size_t j = 0; j < members.size() && !unionMember; ++j)
            if (valueOf[j] || !elided[j].empty())
              unionMember = members[j];
          for (size_t j = 0; j < members.size() && !unionMember; ++j)
            if (members[j]->kind == NodeKind::VariableDecl && static_cast<VariableDecl*>(members[j])->init)
              unionMember = members[j];
          if (!unionMember && !members.empty())
            unionMember = members[0];
        }
        for (Decl* d : members)
        {
          if (info.isUnion && d != unionMember)
          {
            ++i;
            continue;
          }
          if (!elided[i].empty())
          {
            const Ty* ft = types.unqualified(info.fieldTypes[fieldIndexOf(info, d)]);
            std::string fieldAddr = gep(info.name, addr, fieldIndexOf(info, d));
            for (size_t e = 0; e < elided[i].size(); ++e)
            {
              std::string element = newTemp();
              fn += "  " + element + " = getelementptr inbounds " + llType(ft, at) + ", ptr " + fieldAddr + ", i64 0, i64 " + std::to_string(e) + "\n";
              initializeFrom(element, ft->inner, elided[i][e], at);
            }
            ++i;
            continue;
          }
          Expr* value = valueOf[i++];
          if (d->kind == NodeKind::VariableDecl)
            if (const BitfieldInfo* bits = bitfieldIn(info, d))
            {
              initializeBitfield(addr, t, info, static_cast<VariableDecl*>(d), *bits, value != nullptr, value, zeroedUnits, at);
              continue;
            }
          std::string fieldAddr = info.isUnion ? addr : gep(info.name, addr, fieldIndexOf(info, d));
          const Ty* ft = info.isUnion ? unionMemberType(t, d, at) : info.fieldTypes[fieldIndexOf(info, d)];
          const Ty* declared = d->kind == NodeKind::VariableDecl ? global.typeOfDecl(d) : nullptr;
          bool reference = ft->isReference() || (declared && declared->isReference());
          if (value)
          {
            if (reference)
              bindReferenceMember(fieldAddr, ft->isReference() ? ft : declared, value, at); // `Ref r{x}`: the member refers to x
            else
              initializeFrom(fieldAddr, ft, value, at);
            continue;
          }
          if (d->kind != NodeKind::VariableDecl)
            continue;
          auto* v = static_cast<VariableDecl*>(d);
          if (v->init && reference)
            withMemberInitializer(addr, t, v, [&](Expr* value) { bindReferenceMember(fieldAddr, ft->isReference() ? ft : declared, value, v); });
          else if (v->init)
            withMemberInitializer(addr, t, v, [&](Expr* value) { initializeFrom(fieldAddr, ft, value, v); });
          else if (isClass(ft))
            defaultConstruct(fieldAddr, ft, at);
          else if (ft->kind == TyKind::Array && (needsDestruction(ft, at) || needsConstruction(ft, at))) // elements with constructors or default member initializers
            constructArray(fieldAddr, ft, at);
        }
        return;
      }
      unsupported(at, "a constructor of " + qualifiedName(c) + " taking " + std::to_string(args.size()) + " arguments");
    }

    // `auto [a, b] = pair;`: e names a binding of the hidden local v; out gets the address of the
    // matching data member (or array element). False when e is not one of v's bindings (the
    // nameless name of v itself, which a tuple-like binding's get<i>(v) passes, included).
    bool bindingAddress(VariableDecl* v, Node* e, std::string& out)
    {
      if (v->structuredBindings.empty() || !e || e->kind != NodeKind::NameExpr || static_cast<NameExpr*>(e)->name.empty())
        return false;
      uint32_t symbol = static_cast<NameExpr*>(e)->name.last().symbol;
      size_t index = v->structuredBindings.size();
      for (size_t i = 0; i < v->structuredBindings.size(); ++i)
        if (static_cast<NameExpr*>(v->structuredBindings[i])->name.last().symbol == symbol)
          index = i;
      if (index == v->structuredBindings.size())
        return false;
      auto it = locals.find(v);
      if (it == locals.end())
        unsupported(e, "a structured binding outside its function");
      std::string base = it->second.addr;
      if (it->second.isRef)
      {
        std::string r = newTemp();
        fn += "  " + r + " = load ptr, ptr " + base + "\n";
        base = r;
      }
      const Ty* object = types.unqualified(types.stripRef(it->second.ty));
      if (object->kind == TyKind::Array)
      {
        std::string r = newTemp();
        fn += "  " + r + " = getelementptr inbounds " + llType(object->inner, e) + ", ptr " + base + ", i64 " + std::to_string(index) + "\n";
        out = r;
        return true;
      }
      if (!isClass(object))
        unsupported(e, "a structured binding of something that is neither a class nor an array");
      // The class whose direct members the bindings name: the object's own class, or, when that
      // declares none, the one base that has them (C++'s rule: Vector2D's x and y are its CRTP
      // base Vector2's).
      const Ty* owner = object;
      for (int depth = 0; depth < 16 && structFor(owner, e).fieldDecls.empty(); ++depth)
      {
        const Ty* withMembers = nullptr;
        for (auto& [baseDef, baseIndex] : std::vector<std::pair<Decl*, uint32_t>>(structFor(owner, e).bases))
        {
          const Ty* bt = structFor(owner, e).fieldTypes[baseIndex];
          if (!structFor(bt, e).fieldDecls.empty() || !structFor(bt, e).bases.empty())
          {
            if (withMembers)
              unsupported(e, "a structured binding of a class whose data members are in several bases");
            withMembers = bt;
          }
        }
        if (!withMembers)
          break;
        std::string part;
        if (!baseAddress(owner, withMembers, base, part, e))
          unsupported(e, "a structured binding through a base the layout does not have");
        base = part;
        owner = withMembers;
      }
      const StructInfo& info = structFor(owner, e);
      if (index >= info.fieldDecls.size() || !fieldAddress(owner, info.fieldDecls[index], base, out, e))
        unsupported(e, "a structured binding of a class without that many data members");
      // A reference member's binding names the object the member refers to, as `pair.second` does
      // (TestBuildabilityChecks' `[testedLocation, surface]` over Pair<Locations, Surface&> read the
      // member's own storage as the Surface).
      if (const Ty* ft = fieldTypeIn(owner, info.fieldDecls[index], e); ft && ft->isReference())
      {
        std::string referenced = newTemp();
        fn += "  " + referenced + " = load ptr, ptr " + out + "\n";
        out = referenced;
      }
      return true;
    }

    // `return result;` naming a local object or a by-value parameter of the returned class: it is
    // moved into the result, not copied (C++'s implicit move on return).
    // Whether a loop variable of type vt, given an element of elementTy, is of another class than
    // the element (`for (const Label& l : wrappers)`), which the element converts to.
    bool elementOfOtherClass(const Ty* elementTy, const Ty* vt, Node* at)
    {
      const Ty* target = types.unqualified(types.stripRef(vt));
      const Ty* et = elementTy ? types.unqualified(types.stripRef(elementTy)) : nullptr;
      return et && isClass(target) && isClass(et) && !types.isDependent(et) && canonicalClass(et, at) != canonicalClass(target, at);
    }

    // What a loop variable of type vt refers to or copies, from the element at addr: the element,
    // or the element converted to vt's class (to its base, or through its conversion operator:
    // a Vector of reference_wrapper read as references), never the element read as that class.
    std::string elementAs(const std::string& addr, const Ty* elementTy, const Ty* vt, Node* at)
    {
      if (!elementOfOtherClass(elementTy, vt, at))
        return addr;
      return convert(Value{addr, types.unqualified(types.stripRef(elementTy)), false}, types.unqualified(types.stripRef(vt)), at).text;
    }

    // Whether a loop variable of type vt (or referring to one) is of a class and the element is no
    // object (`for (String name : {"stone-wall", "transport-belt"})`): the variable is made from it.
    bool elementMadeInto(const Ty* elementTy, const Ty* vt)
    {
      return isClass(types.unqualified(types.stripRef(vt))) && elementTy && !isAggregate(types.unqualified(types.stripRef(elementTy)));
    }

    // Builds the loop variable's object of class cls at slot from the element at elementAddr as
    // `String name = element;` would (a converting constructor), never from the element's bytes:
    // TestDragBuilding's `for (std::string blockingEntity : {"stone-wall", ...})` read each
    // `const char*` as a String, and every name came out empty.
    void makeFromElement(const std::string& slot, const Ty* cls, const std::string& elementAddr, const Ty* elementTy, Node* at)
    {
      VariableDecl element{};
      element.kind = NodeKind::VariableDecl;
      element.token = at->token;
      NameExpr name{};
      name.kind = NodeKind::NameExpr;
      name.token = at->token;
      name.resolved = &element;
      name.ty = types.stripRef(elementTy);
      locals[&element] = {elementAddr, types.stripRef(elementTy), false};
      struct Forget { std::unordered_map<Decl*, Local>& locals; Decl* d; ~Forget() { locals.erase(d); } } forget{locals, &element};
      initializeFrom(slot, cls, &name, at);
    }

    // The class of the local object (not a reference) e names, when it is not the function's
    // result class (whose own `return obj;` is a move: returnedLocalObject); null otherwise.
    const Ty* localObjectOfOtherClass(Expr* e)
    {
      if (!e || e->kind != NodeKind::NameExpr)
        return nullptr;
      Decl* d = static_cast<NameExpr*>(e)->resolved;
      if (!d || (d->kind != NodeKind::VariableDecl && d->kind != NodeKind::ParamDecl) || (d->flags & DF_Static))
        return nullptr;
      auto it = locals.find(d);
      if (it == locals.end() || it->second.isRef || !isClass(it->second.ty) || classKey(it->second.ty) == classKey(currentReturn))
        return nullptr;
      if (const Ty* declared = declType(d); declared && declared->isConst)
        return nullptr; // (a const local converts as an lvalue: copied, as returnedLocalObject's)
      return types.unqualified(it->second.ty);
    }

    Decl* returnedLocalObject(Expr* e)
    {
      if (!e || e->kind != NodeKind::NameExpr)
        return nullptr;
      Decl* d = static_cast<NameExpr*>(e)->resolved;
      if (!d || (d->kind != NodeKind::VariableDecl && d->kind != NodeKind::ParamDecl) || (d->flags & DF_Static))
        return nullptr;
      if (d->kind == NodeKind::VariableDecl && !static_cast<VariableDecl*>(d)->structuredBindings.empty())
        return nullptr;
      auto it = locals.find(d);
      if (it == locals.end() || it->second.isRef || !isClass(it->second.ty) || classKey(it->second.ty) != classKey(currentReturn))
        return nullptr;
      // A const local is copied (`const T t; return t;`: the implicit move's `const T&&` binds the
      // copy constructor, never the move constructor).
      if (const Ty* declared = declType(d); declared && declared->isConst)
        return nullptr;
      return d;
    }

    // Named return elision, guaranteed (design.md 23, kovarex 2026-11-04): the variable every return of
    // the function returns, by the resolver's rule (GlobalScope::namedReturnVariable, given the body,
    // the result type and the template arguments codegen emits it under: an instance's typed copy, a
    // lambda's operator), is built in the caller's result (sretAddr) instead of a slot of its own, so
    // that no return copies or moves it and its class needs neither constructor. Not when the result
    // comes back in registers (no sretAddr: a class copied as bits, with no constructor to skip). A
    // variable of another type than the result's would be the rule's error: never built there, a
    // compile error.
    VariableDecl* builtInResult(VariableDecl* v, Node* at)
    {
      if (!v || sretAddr.empty())
        return nullptr;
      const Ty* t = localType(v);
      if (!t || !t->isKnown() || types.isDependent(t) || t->isReference() || !isClass(types.unqualified(t)) || !currentReturn ||
          classKey(types.unqualified(t)) != classKey(types.unqualified(currentReturn)))
      {
        fail(at, "the named return " + (v->name.empty() ? std::string("?") : sym(v->name.last().symbol)) + " is a " +
                     (t ? types.toString(t, &symbols) : std::string("?")) + ", not the returned " +
                     (currentReturn ? types.toString(currentReturn, &symbols) : std::string("?")) +
                     " (namedReturnVariable and codegen disagree)");
        return nullptr;
      }
      return v;
    }

    // Whether e (a return's value) names the function's namedReturn.
    bool returnsNamedReturn(Expr* e)
    {
      while (e && e->kind == NodeKind::ParenExpr)
        e = static_cast<ParenExpr*>(e)->inner;
      return namedReturn && e && e->kind == NodeKind::NameExpr && static_cast<NameExpr*>(e)->resolved == namedReturn;
    }

    // A bit field's value: the storage unit shifted and masked (sign-extended for signed types).
    Value loadBitfield(const std::string& addr, const BitfieldInfo& bits, const Ty* memberTy, Node* at)
    {
      std::string unitLl = llType(bits.unit, at);
      uint32_t unitBits = uint32_t(sizeOf(bits.unit, at) * 8);
      std::string unit = newTemp(), r;
      fn += "  " + unit + " = load " + unitLl + ", ptr " + addr + "\n";
      const Ty* valueTy = types.unqualified(memberTy);
      bool isSigned = (valueTy->kind == TyKind::Int || valueTy->kind == TyKind::Char) && !valueTy->isUnsigned;
      if (isSigned)
      {
        std::string left = newTemp();
        fn += "  " + left + " = shl " + unitLl + " " + unit + ", " + std::to_string(unitBits - bits.shift - bits.width) + "\n";
        r = newTemp();
        fn += "  " + r + " = ashr " + unitLl + " " + left + ", " + std::to_string(unitBits - bits.width) + "\n";
      }
      else
      {
        std::string shifted = newTemp();
        fn += "  " + shifted + " = lshr " + unitLl + " " + unit + ", " + std::to_string(bits.shift) + "\n";
        r = newTemp();
        uint64_t mask = bits.width >= 64 ? ~0ull : ((1ull << bits.width) - 1);
        fn += "  " + r + " = and " + unitLl + " " + shifted + ", " + std::to_string(mask) + "\n";
      }
      Value asUnit{r, bits.unit};
      if (valueTy->kind == TyKind::Bool)
      {
        std::string b = newTemp();
        fn += "  " + b + " = icmp ne " + unitLl + " " + r + ", 0\n";
        return {b, valueTy};
      }
      return convert(asUnit, valueTy, at);
    }

    // Writes a bit field v, already of the member's type: the other bits of the storage unit stay
    // as they are.
    void storeBitfield(Value v, const std::string& addr, const BitfieldInfo& bits, Node* at)
    {
      std::string unitLl = llType(bits.unit, at);
      Value asUnit = convert(v, bits.unit, at);
      uint64_t mask = bits.width >= 64 ? ~0ull : ((1ull << bits.width) - 1);
      std::string old = newTemp(), cleared = newTemp(), masked = newTemp(), shifted = newTemp(), merged = newTemp();
      fn += "  " + old + " = load " + unitLl + ", ptr " + addr + "\n";
      fn += "  " + cleared + " = and " + unitLl + " " + old + ", " + std::to_string(~(mask << bits.shift)) + "\n";
      fn += "  " + masked + " = and " + unitLl + " " + asUnit.text + ", " + std::to_string(mask) + "\n";
      fn += "  " + shifted + " = shl " + unitLl + " " + masked + ", " + std::to_string(bits.shift) + "\n";
      fn += "  " + merged + " = or " + unitLl + " " + cleared + ", " + shifted + "\n";
      fn += "  store " + unitLl + " " + merged + ", ptr " + addr + "\n";
    }

    // The bit fields that share first's storage unit, in declaration order (first among them).
    std::vector<Decl*> unitMembers(const StructInfo& info, ClassDecl* c, Decl* first)
    {
      std::vector<Decl*> out;
      uint32_t index = fieldIndexOf(info, first);
      if (c)
        for (Decl* m : c->members)
          if (info.bitfields.count(m) && fieldIndexOf(info, m) == index)
            out.push_back(m);
      if (out.empty())
        out.push_back(first);
      return out;
    }

    // A bit field initialised: from value (an aggregate's element, a constructor initializer's
    // argument; `: flag()` is explicit with none, a zero), else from its default member
    // initializer (`bool known : 1 = true;`). It shares its storage unit with its neighbours: the
    // unit is zeroed once, at its first member (zeroed), and each value stored into its own bits.
    void initializeBitfield(const std::string& addr, const Ty* t, const StructInfo& info, VariableDecl* v, const BitfieldInfo& bits,
                            bool explicitInit, Expr* value, std::set<size_t>& zeroed, Node* at)
    {
      size_t index = fieldIndexOf(info, v);
      std::string fieldAddr = gep(info.name, addr, index);
      if (zeroed.insert(index).second)
        fn += "  store " + llType(info.fieldTypes[index], at) + " 0, ptr " + fieldAddr + "\n";
      const Ty* memberTy = types.unqualified(types.stripRef(declType(v)));
      auto storeInto = [&](Expr* e, Node* where)
      {
        if (e && e->kind == NodeKind::BraceInitExpr)
        {
          auto* list = static_cast<BraceInitExpr*>(e);
          if (list->args.empty())
            return; // `{}`: zero, as the unit already is
          if (list->args.size() != 1)
            unsupported(where, "a bit field initialised with a list of several values");
          e = list->args[0];
        }
        storeBitfield(convert(rvalue(e), memberTy, where), fieldAddr, bits, where);
      };
      if (value)
        withSingleElement(value, at, [&](Expr* one) { storeInto(one, at); });
      else if (explicitInit)
        storeBitfield(Value{"0", bits.unit, false}, fieldAddr, bits, at);
      else if (v->init)
        withMemberInitializer(addr, t, v, [&](Expr* init) { storeInto(init, v); });
    }

    // The address of the object a variable or parameter declaration names (what lvalue gives a
    // name expression): a local, a field of this, or a global.
    std::string addressOfDecl(Decl* d, Node* e)
    {
      if (d->kind == NodeKind::VariableDecl)
        if (std::string bound; bindingAddress(static_cast<VariableDecl*>(d), e, bound))
          return bound;
      auto it = locals.find(d);
      if (it != locals.end())
      {
        if (it->second.isRef)
        {
          std::string r = newTemp();
          fn += "  " + r + " = load ptr, ptr " + it->second.addr + "\n";
          return r;
        }
        return it->second.addr;
      }
      if (d->kind == NodeKind::VariableDecl)
      {
        if (Decl* cls = global.owningClass(d); cls && !(d->flags & DF_Static))
        {
          if (!currentClassTy)
            unsupported(e, "a member used outside a method");
          return memberObject(currentClassTy, d, thisPointer(e), e);
        }
        return globalAddress(static_cast<VariableDecl*>(d), e, qualifierClass(e, e));
      }
      unsupported(e, "taking the address of " + qualifiedName(d) + " (" + nodeKindName(d->kind) + (packLocals.count(d) ? ", a parameter pack outside its expansion" : "") + ")");
    }

    // The object a data member of the object at `object` (of class classTy) is: its field, or for a
    // reference member the object the field refers to (its address is what the field holds), also
    // when a template argument makes the member a reference (Holder<int&>'s `A value`). bits, when
    // given, gets the member's bit-field place in the layout that holds it (fieldAddress).
    std::string memberObject(const Ty* classTy, Decl* d, const std::string& object, Node* e, const BitfieldInfo** bits = nullptr)
    {
      std::string addr;
      if (!fieldAddress(classTy, d, object, addr, e, bits))
      {
        // A `when` member on a class template's parameters that this instance does not have (design.md
        // 19): a use of it is an error in the instance being built.
        if (d->when && d->whenKnown < 0 && d->kind == NodeKind::VariableDecl && !static_cast<VariableDecl*>(d)->name.empty())
          fail(e, "'" + sym(static_cast<VariableDecl*>(d)->name.last().symbol) + "' does not exist in " +
                      types.toString(types.unqualified(types.stripRef(classTy)), &symbols) + ": its `when` condition does not hold there (design.md 19)");
        unsupported(e, "a member that is not a field of the object's class");
      }
      const Ty* ft = fieldTypeIn(classTy, d, e);
      if (!ft)
        ft = d->kind == NodeKind::VariableDecl ? declType(d) : nullptr;
      if (ft && ft->isReference())
      {
        std::string referenced = newTemp();
        fn += "  " + referenced + " = load ptr, ptr " + addr + "\n";
        return referenced;
      }
      return addr;
    }

    // Whether an expression is an xvalue: an rvalue reference to an object that may be moved from
    // (`move(x)`); a named rvalue reference is an lvalue.
    static bool isXvalue(Expr* e) { return e && e->ty && e->ty->kind == TyKind::RRef && !isLvalueExpression(e); }
    // An xvalue a move may take: not a const one (`move(constObject)`), which a `T&&` does not bind,
    // so the copy constructor or copy assignment takes it and the source keeps its value.
    static bool movable(Expr* e) { return isXvalue(e) && e->ty->inner && !e->ty->inner->isConst; }

    // Move-constructs the object at src into dst: through the move constructor where the class has
    // one, as a copy otherwise.
    // Whether class c (of type t) has an implicit copy or move constructor taking a parameter of
    // type pt (T&& or const T&) as well as a constructor template given the same argument does:
    // the tie goes to the implicit one, no template ([over.match.best]). The implicit move
    // constructor is declared when c declares no copy constructor, copy or move assignment and no
    // destructor; the implicit copy constructor when c declares none ([class.copy.ctor] 6 and 8).
    bool implicitCopyOrMoveTakes(ClassDecl* c, const Ty* t, const Ty* pt, Node* at)
    {
      if (!pt || !pt->isReference() || !isClass(types.stripRef(pt)) || classKey(types.stripRef(pt)) != classKey(t))
        return false;
      bool copyCtor = false, moveCtor = false, assignment = false, destructor = false;
      TemplateArgs args = global.classArgsOf(canonicalClass(t, at));
      ArgsScope scope(*this, &args);
      for (Decl* m : c->members)
      {
        if (m->kind != NodeKind::FunctionDecl)
          continue;
        auto* f = static_cast<FunctionDecl*>(m);
        if (f->flags & DF_Destructor)
          destructor = true;
        else if (!f->name.empty() && f->name.last().special == 2 && P(f->name.last().opToken) == P::Assign)
          assignment = true;
        else if ((f->flags & DF_Constructor) && f->templateParams.empty() && f->params.size() == 1)
        {
          const Ty* q = declType(f->params[0]);
          if (q && q->isReference() && isClass(types.stripRef(q)) && classKey(types.stripRef(q)) == classKey(t))
            (q->kind == TyKind::RRef ? moveCtor : copyCtor) = true;
        }
      }
      if (pt->kind == TyKind::RRef)
        return !copyCtor && !moveCtor && !assignment && !destructor;
      return !copyCtor && types.stripRef(pt)->isConst; // `const T&`; a non-const T& binds better than the copy's
    }

    void moveConstruct(const std::string& dst, const std::string& src, const Ty* t, Node* at)
    {
      ClassDecl* c = classDef(t->decl, at);
      TemplateArgs args = global.classArgsOf(canonicalClass(t, at));
      FunctionDecl* move = nullptr;
      bool defaultedMove = false;
      {
        ArgsScope scope(*this, &args);
        for (Decl* m : c->members)
        {
          if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Constructor))
            continue;
          auto* f = static_cast<FunctionDecl*>(m);
          if (f->params.size() != 1)
            continue;
          const Ty* pt = declType(f->params[0]);
          bool ownMove = pt && pt->kind == TyKind::RRef && isClass(pt->inner) && classKey(pt->inner) == classKey(t);
          if (f->flags & DF_Default)
          {
            defaultedMove = defaultedMove || ownMove; // `T(T&&) = default`: the memberwise move below
            continue;
          }
          if (ownMove)
            move = f;
        }
      }
      if (move && !(move->flags & DF_Delete))
      {
        emitCall("", "void " + useFunction(move, args, canonicalClass(t, at)) + "(ptr " + dst + ", ptr " + src + ")");
        return;
      }
      if (!move && !defaultedMove && !implicitCopyOrMoveTakes(c, t, types.rref(types.unqualified(t)), at))
      {
        // No move constructor, not even an implicit one: a constructor template taking the rvalue
        // as a T&& is the one C++ chooses, over a copy constructor's const T& (`template<typename...
        // Args> Str(Args&&...)` beside an explicit copy constructor: ImmutableStringTemplate's moves
        // were copies).
        const Ty* rvalueTy = types.rref(types.unqualified(t));
        FunctionDecl* chosen = findConstructor(t, {rvalueTy}, at);
        if (chosen && !chosen->templateParams.empty() && !(chosen->flags & DF_Delete) && !global.isExplicit(chosen, args))
        {
          TemplateArgs ctorArgs = args;
          global.deduceArgs(chosen, {rvalueTy}, ctorArgs);
          const Ty* pt;
          {
            ArgsScope scope(*this, &ctorArgs);
            pt = declType(chosen->params[0]);
          }
          if (pt && pt->kind == TyKind::Pack)
            pt = pt->args.size() == 1 ? pt->args[0] : nullptr;
          if (pt && pt->kind == TyKind::RRef && isClass(pt->inner) && classKey(pt->inner) == classKey(t))
          {
            emitCall("", "void " + useFunction(chosen, ctorArgs, canonicalClass(t, at)) + "(ptr " + dst + ", ptr " + src + ")");
            return;
          }
        }
      }
      if (defaultedMove && !c->isTypedUnion)
      {
        // `explicit Animation(Animation&&) = default` beside a deleted copy constructor: the
        // members moved, the copy constructor not looked at (a move is no copy).
        memberwiseConstruct(dst, src, t, at, true);
        return;
      }
      copyConstruct(dst, src, t, at, true); // the user copy constructor if any, else memberwise moves
    }

    // Copy-constructs the object at src into dst (user copy constructor or bitwise copy); with
    // `move`, a class without a user copy constructor moves its members instead.
    void copyConstruct(const std::string& dst, const std::string& src, const Ty* t, Node* at, bool move = false)
    {
      ClassDecl* c = classDef(t->decl, at);
      if (c->isTypedUnion)
      {
        copyAlternative(c, t, dst, src, move, at);
        return;
      }
      TemplateArgs args = global.classArgsOf(canonicalClass(t, at));
      FunctionDecl* copy = nullptr;
      bool hasMove = false, defaultedCopy = false;
      {
        ArgsScope scope(*this, &args);
        for (Decl* m : c->members)
        {
          if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Constructor))
            continue;
          auto* f = static_cast<FunctionDecl*>(m);
          if (f->params.size() != 1)
            continue;
          const Ty* pt = declType(f->params[0]);
          if (!pt || !pt->isReference() || !isClass(pt->inner) || classKey(pt->inner) != classKey(t))
            continue;
          if (f->flags & DF_Default)
          {
            // `T(const T&) = default`: the memberwise copy below, whatever else the class declares
            // (BuildResult: a move constructor and a destructor of its own besides).
            defaultedCopy = defaultedCopy || pt->kind == TyKind::LRef;
            continue;
          }
          if (pt->kind == TyKind::LRef)
            copy = f; // `T(const T&)`; the move constructor `T(T&&)` would empty the source
          else
            hasMove = true;
        }
      }
      if (copy && (copy->flags & DF_Delete))
        fail(at, "copying " + types.toString(types.unqualified(t), &symbols) + ", whose copy constructor is deleted (move it instead)"); // (the program's mistake, no gap: as errors print it, `Item^`)
      if (!copy && hasMove && !defaultedCopy)
        fail(at, "copying " + types.toString(types.unqualified(t), &symbols) + ", which has a move constructor but no copy constructor");
      if (copy)
      {
        emitCall("", "void " + useFunction(copy, args, canonicalClass(t, at)) + "(ptr " + dst + ", ptr " + src + ")");
        return;
      }
      // A destructor of its own leaves the class its implicit copy constructor (C++ only deprecates
      // it): the members copied (Achievement's `new Achievement(*this)`, InputAction's ActionData).
      // A declared move assignment deletes it; and a union with a destructor of its own manages its
      // member's lifetime (an Optional's storage), whose bytes a copy would share.
      if (!defaultedCopy && declaresMoveMember(c, t, true, at))
        unsupported(at, "copying " + qualifiedName(c) + ", which declares a move assignment but no copy constructor");
      if (!defaultedCopy && c->keyword == Kw::union_ && findDestructor(c))
        unsupported(at, "copying " + qualifiedName(c) + ", a union with a destructor but no copy constructor");
      memberwiseConstruct(dst, src, t, at, move);
    }

    // Whether class c (of type t) declares a move assignment (assignment) or a move constructor
    // (`= default` and `= delete` ones included): what deletes C++'s implicit copy members.
    bool declaresMoveMember(ClassDecl* c, const Ty* t, bool assignment, Node* at)
    {
      TemplateArgs args = global.classArgsOf(canonicalClass(t, at));
      ArgsScope scope(*this, &args);
      for (Decl* m : c->members)
      {
        if (m->kind != NodeKind::FunctionDecl)
          continue;
        auto* f = static_cast<FunctionDecl*>(m);
        bool isAssignment = !f->name.empty() && f->name.last().special == 2 && P(f->name.last().opToken) == P::Assign;
        if ((assignment ? !isAssignment : !(f->flags & DF_Constructor)) || f->params.size() != 1)
          continue;
        const Ty* pt = declType(f->params[0]);
        if (pt && pt->kind == TyKind::RRef && isClass(pt->inner) && classKey(pt->inner) == classKey(t))
          return true;
      }
      return false;
    }

    // Constructs the object at dst from the one at src base by base and member by member
    // (bitwise when nothing needs destruction): copies, or moves with `move`.
    void memberwiseConstruct(const std::string& dst, const std::string& src, const Ty* t, Node* at, bool move)
    {
      // Bitwise when nothing needs destruction and no base or member has a copy or move constructor
      // of its own (agui::GenericTargetable's copy targets nothing: a widget copied bitwise shared
      // the original's targeters).
      if (!needsDestruction(t, at) && relocatesBitwise(t, at))
      {
        if (isClass(t) && structFor(t, at).empty)
          return; // no bytes to copy (and as a base, none of its own: its byte is the next member's)
        copyBytes(dst, src, t, at);
        if (isClass(t))
          storeVptrs(dst, structFor(t, at)); // the source may be of a derived class (a slicing copy)
        return;
      }
      // Memberwise: bases and members in order, each with its own copy semantics.
      const StructInfo& info = structFor(t, at);
      for (auto& [baseDef, index] : info.bases)
      {
        if (move)
          moveConstruct(gep(info.name, dst, index), gep(info.name, src, index), info.fieldTypes[index], at);
        else
          copyConstruct(gep(info.name, dst, index), gep(info.name, src, index), info.fieldTypes[index], at);
      }
      storeVptrs(dst, info); // after the bases, whose constructors pointed it at theirs
      for (Decl* d : info.fieldDecls)
      {
        uint32_t index = fieldIndexOf(info, d);
        const Ty* ft = info.fieldTypes[index];
        std::string to = gep(info.name, dst, index), from = gep(info.name, src, index);
        if (isClass(ft) && move)
          moveConstruct(to, from, ft, at);
        else if (isClass(ft))
          copyConstruct(to, from, ft, at);
        else if (isLambda(ft))
          copyClosure(to, from, ft, at, move); // FunctionImpl<F>'s `F f`
        else if (ft->kind == TyKind::Array)
          transferArray(to, from, ft, at, move, false);
        else
        {
          copyBytes(to, from, ft, at);
        }
      }
    }

    // Copies or moves the array of type t at src into the one at dst, constructing (assign: assigning)
    // element by element, each through its own copy or move: a member `T items[N]` of a class whose
    // copy or move is memberwise (the library's Array<Vector<T>, N> returned by value; bitwise, the
    // vectors were freed twice). One after the other for a few, in a loop for more; an array of what
    // needs no destruction and copies (assigns) bitwise is copied bitwise (a `Counted cs[2]` whose
    // copy constructor counts is copied by it).
    void transferArray(const std::string& dst, const std::string& src, const Ty* t, Node* at, bool move, bool assign)
    {
      t = types.unqualified(t);
      if ((!needsDestruction(t, at) && (assign ? assignsBitwise(t, at) : relocatesBitwise(t, at))) || t->count == 0)
      {
        copyBytes(dst, src, t, at);
        return;
      }
      auto transferAt = [&](const std::string& index)
      {
        std::string to = newTemp(), from = newTemp();
        fn += "  " + to + " = getelementptr inbounds " + llType(t, at) + ", ptr " + dst + ", i64 0, i64 " + index + "\n";
        fn += "  " + from + " = getelementptr inbounds " + llType(t, at) + ", ptr " + src + ", i64 0, i64 " + index + "\n";
        const Ty* element = types.unqualified(t->inner);
        if (element->kind == TyKind::Array)
          transferArray(to, from, element, at, move, assign);
        else if (assign)
          assignObject(to, from, element, at, move);
        else if (move)
          moveConstruct(to, from, element, at);
        else
          copyConstruct(to, from, element, at);
      };
      if (t->count <= 8)
      {
        for (uint32_t i = 0; i < t->count; ++i)
          transferAt(std::to_string(i));
        return;
      }
      std::string slot = "%v" + std::to_string(temp++);
      allocas += "  " + slot + " = alloca i64\n";
      fn += "  store i64 0, ptr " + slot + "\n";
      std::string loopL = newLabel("array.loop"), bodyL = newLabel("array.body"), endL = newLabel("array.end");
      emitBranch(loopL);
      startBlock(loopL);
      std::string index = newTemp(), done = newTemp();
      fn += "  " + index + " = load i64, ptr " + slot + "\n";
      fn += "  " + done + " = icmp eq i64 " + index + ", " + std::to_string(t->count) + "\n";
      fn += "  br i1 " + done + ", label %" + endL + ", label %" + bodyL + "\n";
      startBlock(bodyL);
      transferAt(index);
      std::string next = newTemp();
      fn += "  " + next + " = add i64 " + index + ", 1\n";
      fn += "  store i64 " + next + ", ptr " + slot + "\n";
      emitBranch(loopL);
      startBlock(endL);
    }

    // Implicit copy assignment: user operator= where one exists, memberwise otherwise.
    void assignObject(const std::string& dst, const std::string& src, const Ty* t, Node* at, bool move = false)
    {
      if (ClassDecl* c = typedUnionOf(t, at))
      {
        // The alternative held goes, the source's alternative and tag come.
        destroyAlternative(c, t, dst, at);
        copyAlternative(c, t, dst, src, move, at);
        return;
      }
      const Ty* foundIn = nullptr;
      // From an lvalue: operator=(const T&). From a temporary or `move(x)`: operator=(T&&) if there is one.
      std::vector<const Ty*> argTypes{move ? types.rref(types.unqualified(t)) : types.lref(types.qualified(t, true, false))};
      FunctionDecl* op = declaresAssignment(t, at) ? findMemberFunction(t, "operator=", argTypes, t, at, &foundIn) : nullptr;
      // A base's operator=, the class's own taking no T (`Mixed& operator=(int)`): the class's implicit
      // assignment, which hides the base's (C++ [class.copy.assign]) and assigns the class's own
      // members too, unless a using-declaration brought the base's in.
      if (op && foundIn && foundIn != canonicalClass(t, at) && !declaresAssignment(t, at, true))
        op = nullptr;
      bool defaulted = op && (op->flags & DF_Default);
      if (defaulted)
        op = nullptr; // `operator=(const T&) = default`: the memberwise assignment below
      if (op)
      {
        if (op->flags & DF_Delete)
          unsupported(at, "assignment to " + qualifiedName(t->decl) + " through its deleted operator=");
        std::string thisArg = dst;
        if (canonicalClass(t, at) != foundIn)
          baseAddress(t, foundIn, dst, thisArg, at);
        std::string name = useFunction(op, global.classArgsOf(foundIn), foundIn);
        Signature s;
        {
          TemplateArgs args = global.classArgsOf(foundIn);
          ArgsScope scope(*this, &args);
          s = signatureOf(op);
        }
        if (s.returnType == "void")
          emitCall("", "void " + name + "(ptr " + thisArg + ", ptr " + src + ")");
        else
          emitCall(newTemp() + " = ", s.returnType + " " + name + "(ptr " + thisArg + ", ptr " + src + ")");
        return;
      }
      if (!needsDestruction(t, at) && assignsBitwise(t, at))
      {
        if (isClass(t) && structFor(t, at).empty)
          return; // no state, and as a base maybe no bytes of its own (the next member's)
        std::vector<std::string> vptrs = loadVptrs(dst, structFor(t, at));
        copyBytes(dst, src, t, at);
        restoreVptrs(vptrs);
        return;
      }
      // A destructor of its own leaves the class its implicit copy assignment (C++ only deprecates
      // it), a defaulted one is memberwise anyway (Trigger); a declared move constructor deletes the
      // implicit copy assignment.
      if (!defaulted && !move && declaresMoveMember(classDef(t->decl, at), t, false, at))
        unsupported(at, "assignment to " + qualifiedName(t->decl) + ", which declares a move constructor but no operator=");
      if (!defaulted && classDef(t->decl, at)->keyword == Kw::union_ && findDestructor(classDef(t->decl, at)))
        unsupported(at, "assignment to " + qualifiedName(t->decl) + ", a union with a destructor but no operator=");
      memberwiseAssign(dst, src, t, at, move);
    }

    // Whether class t declares an operator= of its own (`using Base::operator=;` included): one that
    // does not has implicit ones (memberwise), which hide its bases'.
    // Whether class t declares an operator= of its own, or brings a base's in by a using-declaration;
    // usingOnly: only the latter.
    bool declaresAssignment(const Ty* t, Node* at, bool usingOnly = false)
    {
      ClassDecl* c = classDef(t->decl, at);
      for (Decl* m : c->members)
      {
        if (m->kind == NodeKind::FunctionDecl)
        {
          if (usingOnly)
            continue;
          auto* f = static_cast<FunctionDecl*>(m);
          if (!f->name.empty() && f->name.last().special == 2 && P(f->name.last().opToken) == P::Assign)
            return true;
        }
        else if (m->kind == NodeKind::UsingNameDecl)
        {
          const QualifiedName& name = static_cast<UsingNameDecl*>(m)->name;
          if (!name.empty() && name.last().special == 2 && P(name.last().opToken) == P::Assign)
            return true;
        }
      }
      return false;
    }

    // Assigns the object at src to the one at dst base by base and member by member.
    void memberwiseAssign(const std::string& dst, const std::string& src, const Ty* t, Node* at, bool move)
    {
      const StructInfo& info = structFor(t, at);
      for (auto& [baseDef, index] : info.bases)
        assignObject(gep(info.name, dst, index), gep(info.name, src, index), info.fieldTypes[index], at, move);
      for (Decl* d : info.fieldDecls)
      {
        uint32_t index = fieldIndexOf(info, d);
        const Ty* ft = info.fieldTypes[index];
        std::string to = gep(info.name, dst, index), from = gep(info.name, src, index);
        if (isClass(ft))
          assignObject(to, from, ft, at, move);
        else if (ft->kind == TyKind::Array)
          transferArray(to, from, ft, at, move, true);
        else
        {
          copyBytes(to, from, ft, at);
        }
      }
    }

    // Initialises the object/scalar at addr of type t from an initializer expression,
    // constructing in place where the expression is a temporary.
    // Whether both branches of conditional c are prvalues of class t itself (`c ? T(1) : make(2)`),
    // which build the object initialised from c in place: no lvalue (that one is copied) and no other
    // class (converted).
    bool conditionalOfPrvalues(ConditionalExpr* c, const Ty* t, Node* at)
    {
      if (!c->then || !c->else_)
        return false;
      for (Expr* branch : {c->then, c->else_})
      {
        const Ty* bt = branch->ty ? withCurrentArgs(branch->ty) : nullptr;
        if (!bt || bt->isReference() || isLvalueExpression(branch) || !isClass(bt) || types.isDependent(bt) ||
            canonicalClass(types.unqualified(bt), at) != canonicalClass(types.unqualified(t), at))
          return false;
      }
      return true;
    }

    void initializeFrom(const std::string& addr, const Ty* t, Expr* init, Node* at)
    {
      t = types.stripRef(t);
      // `[callback{::move(callback)}]`, `int x{5}`: one value in braces initialises what is not a
      // class as the value itself would.
      if (init && init->kind == NodeKind::BraceInitExpr && !isClass(t) && t->kind != TyKind::Array)
        if (auto* list = static_cast<BraceInitExpr*>(init); list->args.size() == 1 && list->designators.empty() &&
                                                            list->args[0]->kind != NodeKind::BraceInitExpr)
          init = list->args[0];
      if (isLambda(t))
      {
        initializeClosure(addr, t, init, at);
        return;
      }
      if (isClass(t) && init && init->kind == NodeKind::BraceInitExpr && isInitializerListType(t))
      {
        // `InitializerList<int> numbers = {1, 2, 3}`, `const auto entityNames = {"pipe", "tank"}`:
        // the list over an array that lives as long as the variable.
        initializerListTemporary(t, static_cast<BraceInitExpr*>(init), at, addr);
        return;
      }
      if (isClass(t) && init && init->kind == NodeKind::ConditionalExpr && conditionalOfPrvalues(static_cast<ConditionalExpr*>(init), t, at))
      {
        // `T a = c ? T(1) : make(2);`: the chosen branch builds the object itself, no temporary
        // moved from (guaranteed elision, [expr.cond]/7 and [dcl.init]/17.6.1).
        auto* c = static_cast<ConditionalExpr*>(init);
        Value cond = convert(rvalue(c->cond), types.boolType(), at);
        std::string tL = newLabel("cond.then"), eL = newLabel("cond.else"), jL = newLabel("cond.end");
        fn += "  br i1 " + cond.text + ", label %" + tL + ", label %" + eL + "\n";
        for (int branch = 0; branch < 2; ++branch)
        {
          startBlock(branch == 0 ? tL : eL);
          size_t temps = statementTemps.size();
          initializeFrom(addr, t, branch == 0 ? c->then : c->else_, at);
          destroyTempsFrom(temps); // (a branch's own temporaries end with it)
          emitBranch(jL);
        }
        startBlock(jL);
        return;
      }
      if (isClass(t))
      {
        // `T a = T(x)` and `T a = makeT()` build the object in place; an initializer of another
        // class (`Target a = make();` returning a Source) goes through the converting constructor below.
        const Ty* initTy = withCurrentArgs(init->ty);
        bool initIsClass = initTy && isClass(types.stripRef(initTy));
        // A template parameter the type checker left in (`shared_from_this()` of a base class
        // template, typed SharedPtr<T>): the value's own class decides once it is emitted.
        bool dependent = initIsClass && types.isDependent(initTy);
        bool sameClass = initIsClass && !dependent && canonicalClass(initTy, at) == canonicalClass(t, at);
        if (ClassDecl* u = typedUnionOf(t, at); u && !sameClass && !dependent && init->kind != NodeKind::BraceInitExpr && initTy && initTy->isKnown())
        {
          // `AIState s = Patrolling{1};`, `return Attacking{n};`: the alternative of the value's
          // type, constructed from it (design.md 8.5).
          size_t index = alternativeForValue(u, t, initTy, at);
          storeZero(t, addr, at);
          constructAlternative(u, t, addr, index, init, at);
          return;
        }
        if (init->kind == NodeKind::FunctionalCastExpr && sameClass)
        {
          // `T(list)`: the list is a constructor's argument; `T{...}` has its list as the one argument.
          directInitialize(addr, t, static_cast<FunctionalCastExpr*>(init)->args, static_cast<FunctionalCastExpr*>(init)->braced, at,
                           !static_cast<FunctionalCastExpr*>(init)->braced);
          return;
        }
        if (init->kind == NodeKind::BraceInitExpr)
        {
          Expr* one = init; // constructInto sees the whole list: an InitializerList constructor, designators, or the fields
          constructInto(addr, t, Span<Expr*>{&one, 1}, at);
          return;
        }
        if (init->kind == NodeKind::CallExpr && sameClass && !init->ty->isReference())
        {
          // The result is built in place; a call that put it anywhere else would leave the
          // variable unset (`Chunk c = maker(5)` through operator() once did).
          Value v = callValue(static_cast<CallExpr*>(init), addr);
          if (isClass(v.ty) && v.text != addr)
            unsupported(at, "a call that built its " + types.toString(t, &symbols) + " result somewhere else than the object it initialises");
          return;
        }
        if (init->kind == NodeKind::CallExpr && dependent && !init->ty->isReference())
        {
          Value v = callValue(static_cast<CallExpr*>(init), "");
          if (!isClass(v.ty) || !v.isTemporary || canonicalClass(v.ty, at) != canonicalClass(t, at))
            unsupported(at, "initialising a " + types.toString(t, &symbols) + " from a call returning " + (v.ty ? types.toString(v.ty, &symbols) : std::string("?")));
          moveTemporary(addr, v, t, at);
          return;
        }
        if (init->kind == NodeKind::ParenExpr)
        {
          initializeFrom(addr, t, static_cast<ParenExpr*>(init)->inner, at);
          return;
        }
        // An SSE operation the checker could not type (one intrin.h has as a macro, _mm_shuffle_ps)
        // initialising a wrapper class (float32x4): the vector stored into its first field.
        if (init->kind == NodeKind::CallExpr && (!init->ty || !init->ty->isKnown()))
        {
          Value v = rvalue(init);
          if (v.ty && v.ty->kind == TyKind::Vector)
          {
            StructInfo& info = structFor(t, at);
            if (info.fieldTypes.empty() || llType(info.fieldTypes[0], at) != llType(v.ty, at))
              unsupported(at, "initialising " + types.toString(t, &symbols) + ", whose first member is not that vector, from a vector");
            fn += "  store " + llType(v.ty, at) + " " + v.text + ", ptr " + gep(info.name, addr, 0) + "\n";
            return;
          }
          // Not a vector — fall through to the non-class converting constructor path.
        }
        if (init->ty && init->ty->isKnown() && !isClass(types.stripRef(init->ty)) && isClass(t))
        {
          // A non-class value: through a converting constructor (`String s = "text"`).
          Expr* one = init;
          constructInto(addr, t, Span<Expr*>{&one, 1}, at);
          return;
        }
        if (initIsClass && !dependent && !sameClass)
        {
          // Another class: through a converting constructor, a base, or its conversion operator
          // (`String text = vec;` with `operator String()`), never by copying it as a t.
          Expr* one = init;
          constructInto(addr, t, Span<Expr*>{&one, 1}, at);
          return;
        }
        Value src = rvalue(init);
        if (!isClass(src.ty))
          unsupported(at, "initialising a class from a non-class value");
        if (canonicalClass(src.ty, at) != canonicalClass(t, at))
          unsupported(at, "initialising a " + types.toString(t, &symbols) + " from a " + types.toString(src.ty, &symbols) + " whose type the checker left open");
        if (src.isTemporary)
          moveTemporary(addr, src, t, at);
        else if (movable(init))
          moveConstruct(addr, src.text, t, at); // `T tmp = move(a);`
        else
          copyConstruct(addr, src.text, t, at);
        return;
      }
      // `char text[16] = "edit me"` (also in braces or parentheses), `wchar_t name[8] = L"x"`: the
      // literal's characters and its terminating zero, the rest of the array zero.
      Expr* literal = init;
      while (literal && literal->kind == NodeKind::ParenExpr)
        literal = static_cast<ParenExpr*>(literal)->inner;
      if (literal && literal->kind == NodeKind::BraceInitExpr && static_cast<BraceInitExpr*>(literal)->args.size() == 1)
        literal = static_cast<BraceInitExpr*>(literal)->args[0];
      if (t->kind == TyKind::Array && (types.unqualified(t->inner)->kind == TyKind::Char || types.unqualified(t->inner)->kind == TyKind::Int) &&
          literal && literal->kind == NodeKind::LiteralExpr && static_cast<LiteralExpr*>(literal)->literalKind == TokenKind::String)
      {
        auto* l = static_cast<LiteralExpr*>(literal);
        uint32_t unitBytes = 1;
        size_t units = stringLiteralUnits(l, unitBytes);
        if (sizeOf(t->inner, at) != unitBytes)
          fail(at, "an array of " + types.toString(t->inner, &symbols) + " initialised from a string literal of " + std::to_string(unitBytes * 8) + "-bit characters");
        if (units > t->count)
          fail(at, "a string literal of " + std::to_string(units) + " characters initialises an array of " + std::to_string(t->count));
        storeZero(t, addr, at);
        if (declaredNames.claim("@llvm.memcpy.p0.p0.i64"))
          decls += "declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)\n";
        fn += "  call void @llvm.memcpy.p0.p0.i64(ptr " + addr + ", ptr " + stringConstant(l) + ", i64 " +
              std::to_string(std::min<uint64_t>(units + 1, t->count) * unitBytes) + ", i1 false)\n";
        return;
      }
      if (init->kind == NodeKind::BraceInitExpr)
      {
        auto* bi = static_cast<BraceInitExpr*>(init);
        if (bi->args.empty())
        {
          storeZero(t, addr, at);
          return;
        }
        if (t->kind == TyKind::Array)
        {
          // Element by element (nested lists for arrays of arrays); the rest stays zero. An element
          // built is destroyed if a later one's initializer throws ([except.ctor]/3): an unwind-only
          // cleanup each, dropped once the array is whole (its owner destroys it then).
          storeZero(t, addr, at);
          bool cleanups = !scopes.empty() && needsDestruction(t->inner, at);
          std::vector<std::string> built;
          for (size_t i = 0; i < bi->args.size() && i < t->count; ++i)
          {
            std::string element = newTemp();
            fn += "  " + element + " = getelementptr inbounds " + llType(t, at) + ", ptr " + addr + ", i64 0, i64 " + std::to_string(i) + "\n";
            initializeFrom(element, t->inner, bi->args[i], at);
            if (cleanups && i + 1 < bi->args.size() && i + 1 < t->count)
            {
              Object o{element, t->inner};
              o.unwindOnly = true;
              scopes.back().push_back(o);
              built.push_back(element);
            }
          }
          if (!built.empty())
          {
            std::vector<Object>& scope = scopes.back();
            scope.erase(std::remove_if(scope.begin(), scope.end(),
                                       [&](const Object& o) { return o.unwindOnly && std::find(built.begin(), built.end(), o.addr) != built.end(); }),
                        scope.end());
          }
          return;
        }
        if (bi->args.size() != 1)
          unsupported(at, "this brace initializer");
        init = bi->args[0];
      }
      store(rvalue(init), addr, t, at);
    }

    // t with the template parameters of the instantiation being emitted replaced by their
    // arguments, for an expression the type checker typed with a parameter left in.
    const Ty* withCurrentArgs(const Ty* t, int depth = 0)
    {
      if (!t || !currentArgs || depth > 16 || !t->isKnown() || !types.isDependent(t))
        return t;
      switch (t->kind)
      {
        case TyKind::Param:
          for (auto& [d, a] : currentArgs->pairs)
            if (d == t->decl && a)
            {
              const Ty* bound = a;
              // (of one element's expansion: the pattern's mention of the pack, that element)
              if (a->kind == TyKind::Pack && currentArgs->elementIndex < a->args.size() &&
                  std::find(currentArgs->elementPacks.begin(), currentArgs->elementPacks.end(), d) != currentArgs->elementPacks.end())
                bound = a->args[currentArgs->elementIndex];
              return types.qualified(bound, t->isConst || bound->isConst, t->isVolatile || bound->isVolatile);
            }
          return t;
        case TyKind::Pointer: return types.qualified(types.pointer(withCurrentArgs(t->inner, depth + 1)), t->isConst, t->isVolatile);
        case TyKind::LRef: return types.lref(withCurrentArgs(t->inner, depth + 1));
        case TyKind::RRef: return types.rref(withCurrentArgs(t->inner, depth + 1));
        case TyKind::Class:
        {
          std::vector<const Ty*> args;
          for (const Ty* a : t->args)
            args.push_back(withCurrentArgs(a, depth + 1));
          return types.qualified(types.classType(t->decl, Span<const Ty*>{args.data(), uint32_t(args.size())}), t->isConst, t->isVolatile);
        }
        default: return t;
      }
    }

    // Whether a temporary of class t may move to the object it initialises bitwise, as if it had been
    // built there (C++17 builds a prvalue in its object; kppc may build it in a slot of its own first):
    // no user-provided copy or move constructor in the class, its bases or its members. A class with
    // one may keep its own address somewhere: IDConnector hands `this` to its resolver, and its move
    // constructor gives the resolver the new address (a bitwise move left the resolver writing to the
    // dead temporary: assignPrototypeIDs then deleted garbage).
    std::unordered_map<const Ty*, bool> bitwiseRelocation; // this emitter's answers
    bool relocatesBitwise(const Ty* t, Node* at, int depth = 0)
    {
      t = types.unqualified(types.stripRef(t));
      while (t->kind == TyKind::Array && t->inner)
        t = types.unqualified(t->inner);
      if (isLambda(t) && depth <= 32)
      {
        for (auto& [index, ft] : closureValueFields(t, at))
          if (!relocatesBitwise(ft, at, depth + 1))
            return false;
        return true;
      }
      if (!isClass(t) || depth > 32)
        return true;
      const Ty* key = canonicalClass(t, at);
      noteLayout(key);
      if (auto it = bitwiseRelocation.find(key); it != bitwiseRelocation.end())
        return it->second;
      bitwiseRelocation[key] = true; // (a member pointing back at its class is no member by value)
      ClassDecl* c = classDef(key->decl, at);
      bool bitwise = !c->isTypedUnion;
      if (bitwise)
      {
        TemplateArgs args = global.classArgsOf(key);
        ArgsScope scope(*this, &args);
        for (Decl* m : c->members)
        {
          if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Constructor) || (m->flags & (DF_Default | DF_Delete)) ||
              !m->templateParams.empty() || static_cast<FunctionDecl*>(m)->params.size() != 1)
            continue;
          const Ty* pt = declType(static_cast<FunctionDecl*>(m)->params[0]);
          if (pt && pt->isReference() && isClass(pt->inner) && classKey(pt->inner) == classKey(key))
            bitwise = false; // `IDConnector(IDConnector&& other)`, `T(const T&)` of its own
        }
      }
      if (bitwise)
      {
        const StructInfo& info = structFor(key, at);
        std::vector<const Ty*> fields = info.fieldTypes;
        for (const Ty* f : fields)
          if (!relocatesBitwise(f, at, depth + 1))
            bitwise = false;
      }
      bitwiseRelocation[key] = bitwise;
      return bitwise;
    }

    // Whether objects of type t are assigned bitwise: no copy or move assignment of its own (written,
    // not defaulted) in the class, its bases or its members (GenericTargetable's `operator=` keeps
    // the object's own targeters).
    std::unordered_map<const Ty*, bool> bitwiseAssignment;
    bool assignsBitwise(const Ty* t, Node* at, int depth = 0)
    {
      t = types.unqualified(types.stripRef(t));
      while (t->kind == TyKind::Array && t->inner)
        t = types.unqualified(t->inner);
      if (!isClass(t) || depth > 32)
        return true;
      const Ty* key = canonicalClass(t, at);
      noteLayout(key);
      if (auto it = bitwiseAssignment.find(key); it != bitwiseAssignment.end())
        return it->second;
      bitwiseAssignment[key] = true; // (a member pointing back at its class is no member by value)
      ClassDecl* c = classDef(key->decl, at);
      bool bitwise = !c->isTypedUnion;
      if (bitwise)
      {
        TemplateArgs args = global.classArgsOf(key);
        ArgsScope scope(*this, &args);
        for (Decl* m : c->members)
        {
          if (m->kind != NodeKind::FunctionDecl || (m->flags & (DF_Default | DF_Delete)) || !m->templateParams.empty())
            continue;
          auto* f = static_cast<FunctionDecl*>(m);
          bool isAssignment = !f->name.empty() && f->name.last().special == 2 && P(f->name.last().opToken) == P::Assign;
          if (!isAssignment || f->params.size() != 1)
            continue;
          const Ty* pt = declType(f->params[0]);
          if (pt && pt->isReference() && isClass(pt->inner) && classKey(pt->inner) == classKey(key))
            bitwise = false;
        }
      }
      if (bitwise)
      {
        const StructInfo& info = structFor(key, at);
        std::vector<const Ty*> fields = info.fieldTypes;
        for (const Ty* f : fields)
          if (!assignsBitwise(f, at, depth + 1))
            bitwise = false;
      }
      bitwiseAssignment[key] = bitwise;
      return bitwise;
    }

    // Whether class t has a move or copy constructor that is not deleted (an implicit one counts).
    bool movableByConstructor(const Ty* t, Node* at)
    {
      ClassDecl* c = classDef(t->decl, at);
      TemplateArgs args = global.classArgsOf(canonicalClass(t, at));
      ArgsScope scope(*this, &args);
      bool declared = false, usable = false;
      for (Decl* m : c->members)
      {
        if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Constructor) || !m->templateParams.empty() ||
            static_cast<FunctionDecl*>(m)->params.size() != 1)
          continue;
        const Ty* pt = declType(static_cast<FunctionDecl*>(m)->params[0]);
        if (!pt || !pt->isReference() || !isClass(pt->inner) || classKey(pt->inner) != classKey(t))
          continue;
        declared = true;
        usable = usable || !(m->flags & DF_Delete);
      }
      return !declared || usable;
    }

    // A temporary that is used to initialise an object is moved there and no longer destroyed:
    // bitwise when its class allows it (relocatesBitwise), else by its move (or copy) constructor,
    // and the temporary destroyed then (a class that can be neither moved nor copied: bitwise, as
    // C++17 builds such a prvalue in its object).
    void moveTemporary(const std::string& dst, Value src, const Ty* t, Node* at)
    {
      for (size_t i = statementTemps.size(); i-- > 0;)
        if (statementTemps[i].addr == src.text)
        {
          statementTemps.erase(statementTemps.begin() + i);
          break;
        }
      if (isClass(t) && structFor(t, at).empty)
        return; // no state, and as a base maybe no bytes of its own (the next member's)
      if (isClass(t) && !relocatesBitwise(t, at) && !(src.ty && classKey(types.stripRef(src.ty)) != classKey(t)) && movableByConstructor(t, at))
      {
        // (Where the object could be built in place, it is: a call's result, an argument. Here it was
        // built elsewhere first; a class that may know its own address is moved by its constructor.)
        moveConstruct(dst, src.text, t, at);
        destroy(src.text, t, at);
        return;
      }
      copyBytes(dst, src.text, t, at);
      if (isClass(t) && src.ty && classKey(types.stripRef(src.ty)) != classKey(t))
        storeVptrs(dst, structFor(t, at)); // a temporary of a derived class, sliced
    }

    std::string newObjectSlot(const Ty* t, Node* at)
    {
      std::string slot = "%o" + std::to_string(temp++);
      allocas += "  " + slot + " = alloca " + llType(t, at) + alignSuffix(t, 0, at) + "\n";
      return slot;
    }

