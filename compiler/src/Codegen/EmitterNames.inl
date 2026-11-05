// struct Emitter's members: names and IR types. Part of codegen.cpp, included in the struct's body
// in Codegen/Emitter.hpp (a file of its own since 2026-11-05, moved without a change).
    // --- names and types ---

    // GlobalScope::qualifiedName, computed once per declaration.
    const std::string& qualifiedName(Decl* d)
    {
      auto it = qualifiedNames.find(d);
      if (it == qualifiedNames.end())
        it = qualifiedNames.emplace(d, global.qualifiedName(d)).first;
      return it->second;
    }

    std::string sym(uint32_t s) const { return s == UINT32_MAX ? "?" : std::string(symbols.name(s)); }
    // A type as IR names print it: every class and enum with its full path (and a local class with
    // where it is declared), so that two types never give one name (DebugInfoRenderer::LineInfo
    // and DebugVisualizationRenderer::LineInfo as template arguments made one RbTree<...>::newNode
    // for both). Messages print types short (types.toString).
    std::string irString(const Ty* t) const { return types.toString(t, &symbols, true); }

    // The variable whose initializer is being emitted (a global's or static member's in its
    // startup function, a default member initializer in a constructor), or null: names written
    // in it are typed from its declaration (the class's member aliases are in scope there), not
    // from the function that happens to be emitted (none, for a startup function).
    Decl* initializerOf = nullptr;
    // The class whose implicit construction is being emitted where its object is made (not in a
    // constructor of its own): the constructors it calls are its to call (--access-warnings).
    Decl* implicitConstructionOf = nullptr;
    // The closure of the lambda whose body is being emitted, or null: names written in the body
    // are typed from it (the lambda's template parameters, the aliases its body declares), not
    // from the function that happens to emit it (a generic lambda's is the one calling it).
    Decl* lambdaContext = nullptr;
    Decl* typingContext() const { return initializerOf ? initializerOf : lambdaContext ? lambdaContext : static_cast<Decl*>(currentFunction); }

    // A class declared in a function body, or a member of one: the main pass does not reach its
    // member functions (they are emitted when used), and its name alone is not unique.
    static bool declaredInFunction(const Decl* d)
    {
      for (const Decl* p = d ? d->parent : nullptr; p; p = p->parent)
        if (p->kind == NodeKind::FunctionDecl)
          return true;
      return false;
    }
    std::string newTemp() { return "%t" + std::to_string(temp++); }
    std::string newLabel(const char* hint) { return std::string(hint) + std::to_string(label++); }

    static bool isClass(const Ty* t) { return t && t->kind == TyKind::Class; }

    const Ty* declType(Decl* d) { return currentArgs ? global.typeOfDeclIn(d, *currentArgs) : global.typeOfDecl(d); }

    const Ty* classKey(const Ty* t) { return types.unqualified(types.stripRef(t)); }

    std::string llType(const Ty* t, Node* at = nullptr)
    {
      if (!t)
        unsupported(at, "an expression without a type");
      switch (t->kind)
      {
        case TyKind::Void: return "void";
        case TyKind::Bool: return "i1";
        case TyKind::Char: case TyKind::Int: return "i" + std::to_string(t->bits);
        case TyKind::Float: return t->bits == 32 ? "float" : "double";
        case TyKind::Nullptr: case TyKind::Pointer: case TyKind::LRef: case TyKind::RRef: case TyKind::Function:
          return "ptr";
        case TyKind::MemberPointer: return t->inner && t->inner->kind == TyKind::Function ? "ptr" : "i64"; // a function, or a data member's offset within its class
        case TyKind::Lambda: return t->decl ? closureStructName(t) : "ptr";
        case TyKind::Array:
          return "[" + std::to_string(t->count) + " x " + llType(t->inner, at) + "]";
        case TyKind::Vector:
          return "<" + std::to_string(t->count) + " x " + llType(t->inner, at) + ">";
        case TyKind::Class: return structFor(t, at).name;
        case TyKind::Enum: return llType(enumUnderlying(t->decl), at);
        default:
          unsupported(at, "the type " + types.toString(t, &symbols) + (t->kind == TyKind::Pack ? " (a parameter pack)" : t->kind == TyKind::Param ? " (a template parameter)" : "") +
                            (at ? std::string(" of ") + nodeKindName(at->kind) : std::string()));
      }
    }

    const Ty* enumUnderlying(Decl* d)
    {
      Decl* def = global.definitionOf(d);
      auto* e = static_cast<EnumDecl*>(def ? def : d);
      if (e->underlying)
        if (const Ty* u = global.typeOfAst(e->underlying, e))
          if (u->isKnown())
            return u;
      return types.intType(32);
    }

    // Zeroes an object of type t at addr (value initialization): a struct or an array with a memset of
    // its size, anything else with a store. The instruction selector splits `store [1048576 x i8]
    // zeroinitializer` into a node per element and crashed on a member `char buffer[1 << 20]`
    // (BufferedReadStream); the size is LLVM's own (`getelementptr (T, ptr null, i32 1)`).
    void storeZero(const Ty* t, const std::string& addr, Node* at)
    {
      // An empty class has no state to clear, and as a base it may have no bytes of its own (laid
      // out with no size: its byte would be the next member's, or past the object).
      if (isClass(t) && structFor(t, at).empty)
        return;
      std::string ty = llType(t, at);
      if (ty[0] == '%' || ty[0] == '[' || ty.compare(0, 2, "<{") == 0)
      {
        if (declaredNames.claim("@llvm.memset.p0.i64"))
          decls += "declare void @llvm.memset.p0.i64(ptr, i8, i64, i1)\n";
        fn += "  call void @llvm.memset.p0.i64(ptr " + addr + ", i8 0, i64 ptrtoint (ptr getelementptr (" + ty + ", ptr null, i32 1) to i64), i1 false)\n";
        return;
      }
      fn += "  store " + ty + " zeroinitializer, ptr " + addr + "\n";
    }

    // Copies the bytes of an object of type t from src to dst: a struct or an array with a memcpy of
    // its size, anything else with a load and a store. A struct loaded as one value crashed the
    // instruction selector as storeZero's did (minizip's `*zi = ziinit;`, a struct holding a 64 KB
    // buffer). src and dst are the same object or apart (`*p = *p`), as memcpy allows.
    void copyBytes(const std::string& dst, const std::string& src, const Ty* t, Node* at)
    {
      std::string ty = llType(t, at);
      if (ty[0] == '%' || ty[0] == '[' || ty.compare(0, 2, "<{") == 0)
      {
        if (declaredNames.claim("@llvm.memcpy.p0.p0.i64"))
          decls += "declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)\n";
        fn += "  call void @llvm.memcpy.p0.p0.i64(ptr " + dst + ", ptr " + src + ", i64 ptrtoint (ptr getelementptr (" + ty + ", ptr null, i32 1) to i64), i1 false)\n";
        return;
      }
      std::string v = newTemp();
      fn += "  " + v + " = load " + ty + ", ptr " + src + "\n";
      fn += "  store " + ty + " " + v + ", ptr " + dst + "\n";
    }

