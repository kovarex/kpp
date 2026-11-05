// GlobalScope::Impl, phase 2, walking: the Walker, which types and checks bodies: part of resolver.cpp, included in the class's body there (a file of its
// own since 2026-11-05, moved without a change; docs/design.md, "Compilation model").
  // ---------------- phase 2: walking ----------------

  struct Walker
  {
    Impl& impl;
    TypeTable& types;
    ResolveStats& stats;
    const SymbolTable& symbols;
    const std::vector<Token>& tokens;
    std::string_view text;
    LocalStack locals;
    Scope* scope;            // Current lexical/semantic scope.
    const Ty* thisType = nullptr;
    const Subst* activeSubst = nullptr; // Set while checking a template body for an instantiation.
    int instantiationDepth = 0;
    const ParsedFile* file = nullptr; // The file the tokens belong to (for --why sites).
    Expr* current = nullptr;          // The expression being typed (for --why sites).
    // Of an instance's walk: where it was first asked for (the asking walk's expression), for the
    // messages about it.
    const ParsedFile* requestFile = nullptr;
    uint32_t requestToken = UINT32_MAX;
    FunctionDecl* bodyOf = nullptr;   // The function whose body is being walked.
    int instantiatedBaseDepth = 0;    // memberOfInstantiatedBases nesting
    Expr* const* walkedArgs = nullptr; // see WalkedArgs
    std::vector<const Ty*> walkedArgTypes;
    const Ty* deducedLocal = nullptr;  // the `auto` type walkDecl just deduced, for walkLocalDecl
    size_t blockLocals = 0;            // where the innermost block's locals start (a `when` clash: whenHolds)
    std::vector<size_t> blockStarts;   // where each enclosing block's locals start, the innermost last (whenUse)
    int undecidedBranches = 0;         // inside an instance's `if constexpr` branch this walk could not decide
    // The code generator's re-typing of an instance's copy with a type pack bound to one element
    // (a fold over the pack, GlobalScope::retypeFor): its static_asserts were checked with the
    // whole pack, and a fold over the pack cannot be evaluated against one element.
    bool retypingElements = false;
    // Of that re-typing for one element of a parameter pack (to emit its expansion's pattern): an
    // if constexpr keeps the branch the instance took with the whole pack, which one element
    // would decide anew (`isConstructible<T, Input&, Args&&...>`, false for each element alone).
    // Not for a `for constexpr` element, whose body's if constexpr depends on it.
    bool keepConstexprBranches = false;
    // Of that re-typing: the packs whose mentions name one element (TemplateArgs::elementPacks),
    // activeSubst binding them whole; null otherwise.
    const std::vector<Decl*>* elementPacks = nullptr;
    uint32_t elementIndex = 0;
    // Return statements inside lambdas / auto functions record their type here.
    std::vector<const Ty*> returnFrames;
    // Set on the scratch walkers that probe a requires-expression's requirements: an explicit
    // conversion of a class to a type that is no class (`bool(instance)`) is typed only when
    // the class has an operator for it, since a requirement holds exactly when it is valid.
    bool probing = false;
    bool walkingQueued = false; // a queued walk (Impl::DeclWalk): its own declaration is walked here, not queued again

    // The type of an explicit conversion of operand to t (a cast) while probing: unknown when the
    // operand is a class with no conversion operator to t (explicit ones count), t otherwise.
    const Ty* probedCast(const Ty* t, const Ty* operand)
    {
      if (!probing || !t || !operand || !t->isKnown() || !operand->isKnown() || types.isDependent(t) || types.isDependent(operand))
        return t;
      const Ty* from = types.unqualified(types.stripRef(operand));
      const Ty* to = types.unqualified(types.stripRef(t));
      if (from->kind != TyKind::Class || to->kind == TyKind::Class || to->kind == TyKind::Void || from->isBraceList())
        return t;
      return impl.convertsByOperator(from, to, 0, 0, true) ? t : unknown(Why::Unsupported);
    }

    int forConstexprDepth = 0; // for constexpr bodies this walk is inside (typed with the variable unbound)

    // The elements of a for constexpr list under the current substitution: the types of
    // `types<A, B>`, the constants of `{1, 2}`, the descriptors of `alternatives(U)` and of
    // `members(T)` (a class without bases). False when one cannot be told yet.
    bool forConstexprElements(ForConstexprStmt* f, std::vector<const Ty*>& out)
    {
      auto* v = static_cast<TemplateParamDecl*>(f->var);
      Node* list = f->list;
      if (list && list->kind == NodeKind::NamedType && isTypesList(static_cast<NamedType*>(list)))
      {
        for (Node* arg : static_cast<NamedType*>(list)->name.parts[0].templateArgs)
        {
          const Ty* t = arg && uint8_t(arg->kind) <= uint8_t(NodeKind::PackType) ? typeOf(static_cast<Type*>(arg)) : nullptr;
          if (!t || !t->isKnown() || types.isDependent(t))
            return false;
          out.push_back(t);
        }
        return v->isType;
      }
      if (list && list->kind == NodeKind::BraceInitExpr)
      {
        const Ty* elementTy = v->valueType ? typeOf(v->valueType) : nullptr;
        if (v->isType || !elementTy || !elementTy->isKnown() || types.isDependent(elementTy) || elementTy->kind == TyKind::Auto)
          return false;
        for (Expr* e : static_cast<BraceInitExpr*>(list)->args)
        {
          std::optional<int64_t> c = impl.constEval(e, ctx());
          if (!c)
            return false;
          out.push_back(types.valueType(elementTy, *c));
        }
        return true;
      }
      bool alternatives = list && list->kind == NodeKind::CallExpr && static_cast<CallExpr*>(list)->callee &&
                          static_cast<CallExpr*>(list)->callee->kind == NodeKind::NameExpr &&
                          symbols.name(static_cast<NameExpr*>(static_cast<CallExpr*>(list)->callee)->name.last().symbol) == "alternatives";
      Expr* subject = reflectionSubject(list);
      if (!subject || subject->kind != NodeKind::NameExpr || v->isType)
        return false;
      NamedType named{};
      named.kind = NodeKind::NamedType;
      named.token = subject->token;
      named.name = static_cast<NameExpr*>(subject)->name;
      const Ty* cls = typeOf(&named);
      cls = cls ? types.unqualified(types.stripRef(cls)) : nullptr;
      if (!cls || cls->kind != TyKind::Class || types.isDependent(cls))
        return false;
      Subst s = cls->args.empty() ? Subst{} : impl.substFor(cls);
      if (alternatives)
      {
        ClassDecl* u = typedUnionOf(cls);
        if (!u)
          return false;
        for (Decl* alt : u->alternatives)
          out.push_back(types.member(impl.declaredTypeOf(alt, s.pairs.empty() ? nullptr : &s, 0), alt, cls));
        return true;
      }
      ClassDecl* def = impl.classDefinition(cls);
      if (!def || !def->bases.empty())
        return false;
      for (Decl* d : membersRead(def))
        if (d->kind == NodeKind::VariableDecl && !(d->flags & (DF_Static | DF_Typedef)) && !static_cast<VariableDecl*>(d)->name.empty())
          out.push_back(types.member(impl.declaredTypeOf(d, s.pairs.empty() ? nullptr : &s, 0), d, cls));
      return true;
    }

    // Per lambda / auto function this walk entered: whether it returns `decltype(auto)`, so that
    // its returns record their expression's decltype (`f(x)` returning int& stays int&).
    std::vector<bool> decltypeReturns;

    // The type `decltype(e)` names, for a `decltype(auto)` return, given e's type t: a variable
    // or member named without parentheses has its declared type, any other lvalue is a reference
    // to its type, a call or cast keeps the reference its type has.
    const Ty* decltypeOf(Expr* e, const Ty* t)
    {
      if (Impl::namesTupleBinding(e) && t)
        return types.stripRef(t); // `decltype(auto) x = a;` of a tuple-like binding a: what get gives, no reference
      if (!e || !t || !t->isKnown() || t->isReference() || e->kind == NodeKind::NameExpr || e->kind == NodeKind::MemberExpr)
        return t;
      return isLvalueExpression(e) ? types.lref(t) : t;
    }

    // A typed union (`union enum`, design.md 8.5), given its class type; null for anything else.
    static ClassDecl* typedUnionOf(const Ty* t)
    {
      if (!t || t->kind != TyKind::Class || !t->decl || t->decl->kind != NodeKind::ClassDecl)
        return nullptr;
      auto* c = static_cast<ClassDecl*>(t->decl);
      return c->isTypedUnion && c->isDefinition ? c : nullptr;
    }

    // `types<A, B, C>`: the compile-time list of types of design.md 8.1 (a builtin name).
    bool isTypesList(const NamedType* n) const
    {
      return n->name.parts.size() == 1 && n->name.parts[0].hasTemplateArgs && symbols.name(n->name.parts[0].symbol) == "types";
    }

    // `members(T)` (design.md 8.3), `alternatives(U)` (8.5): the type reflected on, or null when
    // the node is no such list.
    Expr* reflectionSubject(Node* list) const
    {
      if (!list || list->kind != NodeKind::CallExpr)
        return nullptr;
      auto* c = static_cast<CallExpr*>(list);
      if (!c->callee || c->callee->kind != NodeKind::NameExpr || c->args.size() != 1)
        return nullptr;
      const QualifiedName& name = static_cast<NameExpr*>(c->callee)->name;
      if (name.parts.size() != 1 || name.parts[0].hasTemplateArgs)
        return nullptr;
      std::string_view which = symbols.name(name.parts[0].symbol);
      return which == "members" || which == "alternatives" ? c->args[0] : nullptr;
    }

    // The enumerator of union u's tag that an index written as a bare name names, when the name
    // means nothing where it is written (`p[Integer]` outside the union: design.md 8.5).
    EnumeratorDecl* alternativeNamed(ClassDecl* u, Expr* index)
    {
      if (!index || index->kind != NodeKind::NameExpr || !u->tagEnum)
        return nullptr;
      auto* n = static_cast<NameExpr*>(index);
      if (n->name.parts.size() != 1 || n->name.parts[0].hasTemplateArgs ||
          (n->resolved && n->resolved->kind != NodeKind::EnumeratorDecl))
        return nullptr;
      if (n->resolved)
        return nullptr; // already an enumerator: the qualified spelling, or one in scope
      for (Decl* d : static_cast<EnumDecl*>(u->tagEnum)->enumerators)
        if (d->kind == NodeKind::EnumeratorDecl && static_cast<EnumeratorDecl*>(d)->name == n->name.parts[0].symbol)
          return static_cast<EnumeratorDecl*>(d);
      return nullptr;
    }

    // The typed union whose alternative a descriptor names (`for constexpr (auto a :
    // alternatives(U))`), with the alternative's index; null for a member descriptor.
    static ClassDecl* alternativeOf(const Ty* descriptor, size_t& index)
    {
      if (!descriptor || descriptor->kind != TyKind::Member || descriptor->args.empty())
        return nullptr;
      ClassDecl* u = typedUnionOf(descriptor->args[0]);
      for (size_t i = 0; u && i < u->alternatives.size(); ++i)
        if (u->alternatives[i] == descriptor->decl)
        {
          index = i;
          return u;
        }
      return nullptr;
    }

    TokenScope tokenScope; // this walker's file is where literals are read from, lookups included
    Walker(Impl& impl, ResolveStats& stats, const std::vector<Token>& tokens, std::string_view text)
      : impl(impl), types(impl.types), stats(stats), symbols(impl.symbols), tokens(tokens), text(text), scope(&impl.root), tokenScope(&tokens, text)
    {
    }

    std::string_view spelling(uint32_t token) const
    {
      const Token& t = tokens[std::min<size_t>(token, tokens.size() - 1)];
      return text.substr(t.offset, t.length);
    }
    // A literal's spelling, from the file it was parsed from (the walk may be another file's).
    std::string_view spelling(const LiteralExpr* l) const
    {
      if (!l->file || !l->file->tokens || l->token >= l->file->tokens->size())
        return spelling(l->token);
      const Token& t = (*l->file->tokens)[l->token];
      return l->file->text.substr(t.offset, t.length);
    }

    // --- statistics helpers ---

    void countUnresolved(const QualifiedName& name, uint32_t token)
    {
      uint32_t first = name.parts[0].symbol;
      if (first == impl.symStd && name.parts.size() > 1)
      {
        ++stats.qualifiedUnresolvedStd;
        return;
      }
      uint32_t key = name.last().symbol;
      if (key == UINT32_MAX)
        return;
      if (name.parts.size() == 1 && isAllCaps(symbols.name(key)))
      {
        ++stats.unresolvedMacroLike;
        return;
      }
      ++stats.unresolvedSymbols[key];
      stats.unresolvedFirst.emplace(key, ResolveStats::Where{stats.fileIndex, token});
    }

    // A name nothing declares: an error at the name. In a template, as C++'s two phases have it
    // ([temp.res]): in its own walk when the name does not depend on its parameters, and a call
    // whose argument does (argument-dependent lookup with the instance's types), or a name a base
    // that is a parameter may declare, in each instance whose function the program emits.
    // dependentCall: the name is called with an argument whose type depends on a parameter (or is
    // not known).
    const NameExpr* undeclaredLater = nullptr; // a subscript's lone index name, judged by the subscript (a typed union's tag)
    // A scoped library's names are written NAME::name (design.md 16): no using-directive or
    // namespace alias brings them in.
    void checkNotLibrary(const QualifiedName& name, uint32_t token)
    {
      if (name.empty() || !file || probing || activeSubst)
        return;
      Lookup l = impl.lookupQualified(name, scope, &locals, 0);
      if (l.scope && l.scope->library && l.decl && l.decl->kind == NodeKind::NamespaceDecl)
      {
        std::string library(symbols.name(l.scope->name));
        impl.reportCheckerError(file, token, "'" + library + "' is a library: its names are written " + library + "::name, never brought in by a using-directive or a namespace alias (design.md 16)");
      }
    }
    void checkNotLibrary(const Type* type, uint32_t token)
    {
      if (type && type->kind == NodeKind::NamedType)
        checkNotLibrary(static_cast<const NamedType*>(type)->name, token);
    }

    void reportUndeclared(const QualifiedName& name, uint32_t token, const NameExpr* at = nullptr, bool dependentCall = false)
    {
      if ((at && at == undeclaredLater) || !file || file->isImported || probing || name.parts.empty() || name.parts[0].symbol == UINT32_MAX)
        return;
      // `util::LIMIT` of an unscoped library util, whose names are bare (design.md 16).
      if (name.parts.size() > 1 && !name.global && !name.typeQualifier && impl.unscopedLibraries.count(name.parts[0].symbol) &&
          impl.lookupUnqualified(name.parts[0].symbol, scope, &locals, 0, true).found == Found::No)
      {
        std::string library(symbols.name(name.parts[0].symbol));
        std::string message = "'" + library + "' is an unscoped library: its names are written bare, never " + library + "::name (design.md 16)";
        if (activeSubst)
          reportInInstance(token, message);
        else if (!inGenericTemplate())
          impl.reportCheckerError(file, token, message);
        return;
      }
      if (name.parts.size() != 1)
        return; // (the C import's inline bodies name the C compiler's builtins: codegen's to judge, if emitted)
      // What the code generator implements by name, macros of the C headers: va_start and its kin,
      // the SSE intrinsics and their _MM_ helpers (one it does not implement is its error).
      std::string_view spelled = symbols.name(name.parts[0].symbol);
      if (spelled == "va_start" || spelled == "va_end" || spelled == "va_copy" || spelled == "va_arg" || spelled.starts_with("_mm_") ||
          spelled.starts_with("_MM_"))
        return;
      std::string message = "'" + std::string(spelled) + "' is not declared";
      if (Lookup again = impl.lookupUnqualified(name.parts[0].symbol, scope, &locals, 0); again.absent)
      {
        // A false `when` declaration's name (design.md 19), which stopped the lookup.
        std::string condition = conditionText(again.absent);
        message = "'" + std::string(spelled) + "' exists only when " + condition + " holds: use it inside `if constexpr (" + condition + ")` or a `when` on it";
      }
      else if (const ParsedFile* owner = impl.filePrivateTo(name.parts[0].symbol, file))
        message = "'" + std::string(spelled) + "' is private to " + owner->path;
      else
        for (Scope* library : impl.libraryScopes)
          if (library->find(name.parts[0].symbol))
          {
            message += "; did you mean " + std::string(symbols.name(library->name)) + "::" + std::string(spelled) + "?";
            break;
          }
      if (activeSubst)
      {
        reportInInstance(token, message);
        return;
      }
      if (inGenericTemplate() && (dependentCall || inClassWithParameterBase()))
        return;
      impl.reportCheckerError(file, token, message);
    }

    // An error in an instance's body: counts once its function is emitted (an `if constexpr` branch
    // the walk could not decide may be the discarded one, and a member of a class template's
    // instance that nothing calls is never instantiated in C++: Array<const char, N>::fill).
    void reportInInstance(uint32_t token, std::string message)
    {
      if (!bodyOf || undecidedBranches > 0)
        return;
      // (not where the instance was asked for: the first request to claim it, which varies with the
      // threads' order, and the error is to be the same at every thread count)
      std::string instance = instanceDescription();
      if (!instance.empty())
        message += " (in " + instance + ")";
      std::lock_guard lock(impl.checkerErrorsMutex);
      impl.instanceNameErrors[bodyOf].push_back(Impl::locationOf(file, token) + ": error: " + message);
    }

    // A built-in assignment (`=`, `+=` and the others on a scalar), increment or decrement of a const
    // object is an error, as in C++ ([expr.ass]/1, [expr.pre.incr]: the operand is a modifiable
    // lvalue): through a `const T&`, a const local or member, a const getter's result. Not in a
    // template's own walk (its instances say). what: "assignment to", "increment of", "decrement of".
    void checkConstModification(uint32_t token, const Ty* object, const char* what)
    {
      if (!object || !object->isConst || !object->isKnown())
        return;
      reportCheck(token, std::string(what) + " a const value (" + types.toString(object, &impl.symbols) + ")");
    }

    // A checker error at token: in a template's instance once its function is emitted, never in the
    // template's own walk (its instances say), nowhere while probing or in imported C.
    void reportCheck(uint32_t token, const std::string& message)
    {
      if (!file || file->isImported || probing)
        return;
      if (activeSubst)
        reportInInstance(token, message);
      else if (!inGenericTemplate())
        impl.reportCheckerError(file, token, message);
    }

    // Whether f, a member function, is a non-const one called on a const object (objectTy): no
    // candidate in C++, whose implicit object parameter `C&` a const object does not bind
    // ([over.match.funcs]/4-5). A static member, a constructor or a destructor takes any object.
    bool nonConstOnConstObject(FunctionDecl* f, const Ty* objectTy)
    {
      const Ty* o = objectTy ? types.stripRef(objectTy) : nullptr;
      return f && o && o->isConst && o->isKnown() && o->kind == TyKind::Class && !(f->flags & (DF_Const | DF_Static | DF_Constructor | DF_Destructor));
    }

    void checkConstObjectCall(FunctionDecl* f, const Ty* objectTy, uint32_t token)
    {
      if (!nonConstOnConstObject(f, objectTy))
        return;
      uint32_t symbol = impl.functionSymbol(f->name);
      reportCheck(token, "the member function " + std::string(symbol == UINT32_MAX ? "?" : impl.symbols.name(symbol)) +
                             " is not const, and its object is a " + types.toString(types.stripRef(objectTy), &impl.symbols));
    }

    // A function defined twice (two bodies of one function: `int f(int)` and `int f(int)`, or
    // `f(long)` beside `f(int)`, one function in kpp, design.md "Types") is an error at the later
    // definition, as in C++ ([basic.def.odr]); the code generator would emit both. Not templates
    // (their specialisations and instances have bodies of their own) nor a `conditional(...)` one
    // (alternatives of which one exists).
    void checkDefinedOnce(FunctionDecl* f)
    {
      if (!f->body || activeSubst || probing || !file || file->isImported || !f->templateParams.empty() || f->condition ||
          f->name.empty() || Impl::isFunctionSpecialization(f) || inGenericTemplate())
        return;
      // (not recorded as what f's walk reads: a compile server's build would walk f again for every
      // overload added to its name, and its codegen then emitted f twice. So an edit that adds a
      // definition before an unchanged one is not reported by the server's build: clang rejects the
      // two definitions, a build from scratch reports them.)
      RecordInto none(nullptr);
      uint32_t symbol = impl.functionSymbol(f->name);
      Scope* home = impl.homeScope(f);
      Lookup look;
      if (symbol == UINT32_MAX || !home || !impl.lookupInScope(home, symbol, look, 0, false) || !look.overloads)
        return;
      auto before = [](const FunctionDecl* a, const FunctionDecl* b) // a's definition comes first
      {
        uint32_t fa = a->file ? a->file->index : 0, fb = b->file ? b->file->index : 0;
        return fa != fb ? fa < fb : a->token < b->token;
      };
      const FunctionDecl* first = nullptr;
      for (const Entry& e : *look.overloads)
      {
        if (!e.decl || e.decl == f || e.decl->kind != NodeKind::FunctionDecl)
          continue;
        auto* g = static_cast<FunctionDecl*>(e.decl);
        if (g->body && g->templateParams.empty() && !g->condition && !Impl::isFunctionSpecialization(g) && before(g, f) &&
            (!first || before(g, first)) && impl.sameFunction(f, g))
          first = g;
      }
      if (!first)
        return;
      // (in the same file: its line, which was the text after the place's last ':', the column with
      // --diagnostic-columns and not a line in Visual Studio's form)
      std::string where = Impl::locationOf(first->file, first->token);
      if (first->file == file && first->file->tokens && first->token < first->file->tokens->size())
      {
        size_t offset = std::min<size_t>((*first->file->tokens)[first->token].offset, first->file->text.size());
        where = "line " + std::to_string(1 + std::count(first->file->text.begin(), first->file->text.begin() + offset, '\n'));
      }
      impl.reportCheckerError(file, f->token, "the function " + impl.namePartText(f->name.last()) + " is defined twice (first at " + where + ")");
    }

    // An object's array bound (a variable's, a data member's) that is no constant, or is negative,
    // is an error at the bound, as in C++ ([dcl.array] 1), never an array of no elements. Checked
    // where its template parameters are known (not in a template's own walk: in its instances);
    // `int a[] = {1, 2}` has its bound from the list, `new T[n]` a run-time one.
    void checkArrayBounds(VariableDecl* v)
    {
      if (probing || !file || file->isImported || (v->flags & DF_Typedef) || (inGenericTemplate() && !activeSubst) || undecidedBranches > 0)
        return;
      for (Type* t = v->type; t;)
      {
        if (t->kind == NodeKind::ConstType)
        {
          t = static_cast<ConstType*>(t)->inner;
          continue;
        }
        if (t->kind != NodeKind::ArrayType)
          break;
        auto* a = static_cast<ArrayType*>(t);
        if (a->size)
        {
          Impl::constWhy().clear();
          std::optional<int64_t> n = impl.constEval(a->size, ctx());
          std::string name = v->name.empty() ? std::string("?") : impl.namePartText(v->name.last());
          if (!n)
          {
            std::string why = Impl::constWhy();
            impl.reportCheckerError(file, a->size->token, "the bound of the array " + name + " is no constant" + (why.empty() ? std::string() : ": " + why));
          }
          else if (*n < 0)
            impl.reportCheckerError(file, a->size->token, "the bound of the array " + name + " is negative (" + std::to_string(*n) + ")");
        }
        t = a->element;
      }
    }

    // Whether a class the walk is in has a base no scope stands for until it is instantiated (a
    // template parameter, `class Wrapper : public T`), which may declare any name.
    bool inClassWithParameterBase()
    {
      for (Scope* s = scope; s; s = s->parent)
        if (s->kind == ScopeKind::Class && !s->baseTypes.empty())
        {
          impl.ensureBases(s, 0);
          if (s->baseScopes.size() < s->baseTypes.size())
            return true;
        }
      return false;
    }

    enum class Why : uint8_t { None, Name, Base, Member, Overload, Deduction, AutoReturn, Unsupported, Operator, Auto };

    const Ty* unknown(Why why)
    {
      if (stats.collectWhy && current && file)
      {
        uint32_t symbol = UINT32_MAX;
        switch (current->kind)
        {
          case NodeKind::NameExpr:
            if (!static_cast<NameExpr*>(current)->name.empty())
              symbol = static_cast<NameExpr*>(current)->name.last().symbol;
            break;
          case NodeKind::MemberExpr:
            if (!static_cast<MemberExpr*>(current)->member.empty())
              symbol = static_cast<MemberExpr*>(current)->member.last().symbol;
            break;
          case NodeKind::CallExpr:
            if (Expr* callee = static_cast<CallExpr*>(current)->callee)
            {
              if (callee->kind == NodeKind::NameExpr && !static_cast<NameExpr*>(callee)->name.empty())
                symbol = static_cast<NameExpr*>(callee)->name.last().symbol;
              else if (callee->kind == NodeKind::MemberExpr && !static_cast<MemberExpr*>(callee)->member.empty())
                symbol = static_cast<MemberExpr*>(callee)->member.last().symbol;
            }
            break;
          case NodeKind::BinaryExpr:
            if (!static_cast<BinaryExpr*>(current)->isKeyword && static_cast<BinaryExpr*>(current)->op < uint32_t(P::Count))
              symbol = impl.opSymbols[static_cast<BinaryExpr*>(current)->op];
            break;
          case NodeKind::UnaryExpr:
            if (!static_cast<UnaryExpr*>(current)->isKeyword && static_cast<UnaryExpr*>(current)->op < uint32_t(P::Count))
              symbol = impl.opSymbols[static_cast<UnaryExpr*>(current)->op];
            break;
          default:
            break;
        }
        uint64_t key = uint64_t(why) << 56 | uint64_t(activeSubst ? 1 : 0) << 55 | uint64_t(uint8_t(current->kind)) << 32 | symbol;
        auto [it, inserted] = stats.why.emplace(key, ResolveStats::WhySite{file, current->token, 0});
        ++it->second.count;
      }
      switch (why)
      {
        case Why::Name: ++stats.unknownName; break;
        case Why::Base: ++stats.unknownBase; break;
        case Why::Member: ++stats.unknownMember; break;
        case Why::Overload: ++stats.unknownOverload; break;
        case Why::Deduction: ++stats.unknownDeduction; break;
        case Why::AutoReturn: ++stats.unknownAutoReturn; break;
        case Why::Unsupported: ++stats.unknownUnsupported; break;
        case Why::Operator: ++stats.unknownOperator; break;
        case Why::Auto: ++stats.unknownAuto; break;
        default: break;
      }
      return types.unknown();
    }

    // Records the type of an expression node and counts it.
    // A name of an `auto`-returning function (not a template, nor a member of one) has the type its
    // body deduces, deduced now if its own file's walk has not done it yet: otherwise the name was
    // `auto(...)` or the deduced type depending on which thread walked which file first.
    const Ty* settledCalleeType(Expr* e, const Ty* t)
    {
      if (t->kind != TyKind::Function || !t->inner || !impl.containsAuto(t->inner) ||
          (e->kind != NodeKind::NameExpr && e->kind != NodeKind::MemberExpr))
        return t;
      Decl* d = e->kind == NodeKind::NameExpr ? static_cast<NameExpr*>(e)->resolved : static_cast<MemberExpr*>(e)->resolved;
      if (!d || d->kind != NodeKind::FunctionDecl || !d->templateParams.empty() || !static_cast<FunctionDecl*>(d)->body)
        return t;
      for (Decl* p = d->parent; p; p = p->parent)
        if ((p->kind == NodeKind::ClassDecl || p->kind == NodeKind::FunctionDecl) && !p->templateParams.empty())
          return t; // in a template: its instantiation's arguments decide
      // Through the function's one instance, never through what its own file's walk cached: that is
      // there or not by which file was walked first, and the instance then walked or not.
      const Ty* r = instantiateFunction(static_cast<FunctionDecl*>(d), Subst{}, true, nullptr);
      return r && r->isKnown() && !impl.containsAuto(r) ? types.function(r, t->args, t->variadic) : t;
    }

    const Ty* record(Expr* e, const Ty* t)
    {
      if (!t)
        t = types.unknown();
      t = settledCalleeType(e, t);
      e->ty = t;
      if (!t->isKnown())
        ++stats.exprsUnknown;
      else if (types.isDependent(t))
        ++stats.exprsDependent;
      else
        ++stats.exprsTyped;
      if (!impl.whyAt.empty() && file)
        traceAt(e, t);
      return t;
    }

    // --why-at FILE:LINE,...: every expression typed on those lines, with its type (innermost first).
    void traceAt(Expr* e, const Ty* t)
    {
      const std::string& path = file->path;
      auto inFile = [&](const std::string& want) { return path.size() >= want.size() && path.compare(path.size() - want.size(), want.size(), want) == 0; };
      bool anyInFile = false;
      for (const auto& [want, wantLine] : impl.whyAt)
        anyInFile = anyInFile || inFile(want);
      if (!anyInFile)
        return;
      const Token& tk = tokens[std::min<size_t>(e->token, tokens.size() - 1)];
      uint32_t line = 1 + uint32_t(std::count(text.begin(), text.begin() + std::min<size_t>(tk.offset, text.size()), '\n'));
      bool wanted = false;
      for (const auto& [want, wantLine] : impl.whyAt)
        wanted = wanted || (wantLine == line && inFile(want));
      if (!wanted)
        return;
      // An operator's or a destructor's name part has no symbol of its own (`operator-(a, b)`).
      auto partName = [&](const NamePart& part)
      {
        if (part.special == 2 && part.opToken < uint32_t(P::Count))
          return std::string(symbols.name(impl.opSymbols[part.opToken]));
        if (part.special == 3)
          return std::string("operator <conversion>");
        std::string name = part.symbol == UINT32_MAX ? "?" : std::string(symbols.name(part.symbol));
        return part.special == 1 ? "~" + name : name;
      };
      std::string what = nodeKindName(e->kind);
      if (e->kind == NodeKind::NameExpr && !static_cast<NameExpr*>(e)->name.empty())
        what += " " + partName(static_cast<NameExpr*>(e)->name.last());
      else if (e->kind == NodeKind::MemberExpr && !static_cast<MemberExpr*>(e)->member.empty())
        what += " ." + partName(static_cast<MemberExpr*>(e)->member.last());
      std::string inst;
      if (activeSubst)
        for (const Subst::Pair& p : activeSubst->pairs)
          inst += (inst.empty() ? " [" : ", ") + types.toString(p.type, &symbols);
      fprintf(stderr, "why-at %s:%u %s at '%.*s' -> %s%s%s\n", path.c_str(), line, what.c_str(), int(std::min<uint32_t>(tk.length, 40)),
              text.data() + tk.offset, types.toString(t, &symbols).c_str(), inst.c_str(), inst.empty() ? "" : "]");
    }

    TypeContext ctx() const
    {
      TypeContext c{scope, &locals, activeSubst, &tokens, text};
      c.elementPacks = elementPacks;
      c.elementIndex = elementIndex;
      return c;
    }

    // --- template instantiation: check bodies once per (template, arguments) ---

    static bool argsConcrete(TypeTable& types, const Ty* t)
    {
      if (!t || t->kind != TyKind::Class || t->args.empty())
        return false;
      for (const Ty* a : t->args)
        if (!a->isKnown() || types.isDependent(a))
          return false;
      return true;
    }

    // Runs `body` as a sub-check with the given substitution, scope and this-type, in the
    // context of the template's own file. Expression statistics are merged into ours.
    template<typename F>
    void subCheck(Decl* templ, const Subst& subst, Scope* bodyScope, const Ty* bodyThis, F&& body, const void* owner = nullptr)
    {
      ParsedFile* file = templ->file;
      if (!file || !file->tokens)
        return;
      Impl::OrdinaryLayout ordinary;
      ResolveStats local;
      // The places it records are the template's file's tokens (a statistic's "first at" names them):
      // that file's, not the requester's.
      local.fileIndex = file->index;
      local.collectWhy = stats.collectWhy;
      Walker sub(impl, local, *file->tokens, file->text);
      sub.file = file;
      FileOfLookups walkedFile(file);
      sub.scope = bodyScope;
      sub.thisType = bodyThis;
      sub.activeSubst = &subst;
      sub.instantiationDepth = instantiationDepth + 1;
      if (current && this->file)
      {
        sub.requestFile = this->file;
        sub.requestToken = current->token;
      }
      for (Decl* p : templ->templateParams)
        sub.locals.push(static_cast<TemplateParamDecl*>(p)->name, p);
      body(sub);
      local.instExprsTyped += local.exprsTyped;
      local.instExprsDependent += local.exprsDependent;
      local.instExprsUnknown += local.exprsUnknown;
      impl.addInstanceStats(local, owner); // counted once, whoever asked (Impl::instanceStatsTotal)
    }

    // Types the instantiation of member f for the class's arguments, in a copy of f's nodes kept
    // as that instantiation's body (unless it exists already: an `auto` member asked for its
    // return type first).
    void checkMemberBody(Walker& sub, FunctionDecl* f, const Subst& classSubst)
    {
      // Out-of-line members carry their own template parameter list; map it positionally.
      Subst merged = classSubst;
      Decl* cls = sub.scope->owner;
      if (cls && f->templateParams.size() == cls->templateParams.size() && f->parent != cls)
        for (size_t i = 0; i < f->templateParams.size(); ++i)
          if (const Ty* t = classSubst.find(cls->templateParams[i]))
            merged.pairs.push_back({f->templateParams[i], t});
      bool producer;
      Impl::InstanceRecord* inst = impl.claimInstance(f, merged, sub.thisType, producer);
      if (!producer)
        return;
      RecordInto into(&inst->deps); // what typing the instance reads is its own
      FunctionDecl* copy = impl.copyInstance(f, sub.scope, merged);
      inst->body = copy;
      // Walked by a walker of its own, as sub would walk it, so that its statistics are its own: a
      // compile server checks an instance again when its template's body changes (step 4).
      ResolveStats mine;
      mine.fileIndex = sub.stats.fileIndex;
      mine.collectWhy = sub.stats.collectWhy;
      Walker member(impl, mine, sub.tokens, sub.text);
      member.file = sub.file;
      FileOfLookups walkedFile(sub.file);
      member.scope = sub.scope;
      member.thisType = sub.thisType;
      member.activeSubst = &merged;
      member.instantiationDepth = sub.instantiationDepth;
      member.requestFile = sub.requestFile;
      member.requestToken = sub.requestToken;
      member.current = sub.current;
      member.locals.items = sub.locals.items;
      if (f->parent != cls)
        for (Decl* p : f->templateParams)
          member.locals.push(static_cast<TemplateParamDecl*>(p)->name, p);
      if (recordingDependencies)
      {
        auto r = std::make_unique<Impl::InstanceRecord::Recheck>();
        r->member = true;
        r->source = f;
        r->scope = member.scope;
        r->thisType = member.thisType;
        r->subst = merged;
        r->locals = member.locals.items;
        r->file = member.file;
        r->depth = member.instantiationDepth;
        r->requestFile = member.requestFile;
        r->requestToken = member.requestToken;
        r->fileIndex = mine.fileIndex;
        r->collectWhy = mine.collectWhy;
        inst->recheck = std::move(r);
      }
      member.returnFrames.push_back(nullptr);
      member.walkDecl(copy);
      const Ty* deduced = member.returnFrames.back();
      member.returnFrames.pop_back();
      if (deduced)
        inst->deducedReturn = impl.deducedShape(copy->returnType && !copy->trailingReturn ? member.typeOf(copy->returnType) : nullptr, deduced);
      mine.instExprsTyped += mine.exprsTyped;
      mine.instExprsDependent += mine.exprsDependent;
      mine.instExprsUnknown += mine.exprsUnknown;
      impl.addInstanceStats(mine, inst); // counted once, whoever asked (Impl::instanceStatsTotal)
      ++impl.instBodies;
      inst->counted |= 1;
      inst->state.store(2, std::memory_order_release);
      inst->state.notify_all();
    }

    // Checks every member function body of a class template for one instantiation, once.
    void instantiateClass(const Ty* classTy)
    {
      if (instantiationDepth > 16 || !argsConcrete(types, classTy) || !classTy->decl || !classTy->decl->ownScope)
        return;
      for (const Ty* a : classTy->args)
        if (impl.holdsText(a) && current && classTy->decl->kind == NodeKind::ClassDecl && !static_cast<ClassDecl*>(classTy->decl)->name.empty())
        {
          // `Vector<Text>`: its instance would keep the Text past the call (design.md 14).
          reportTextError(current->token, impl.isTextClosure(a) ? "a lambda capturing a Text cannot be stored (design.md 14)"
                                                                : textStored("an argument of " + std::string(symbols.name(static_cast<ClassDecl*>(classTy->decl)->name.last().symbol))));
          break;
        }
      if (!classTy->decl->isTemplate || classTy->decl->templateParams.empty())
        return;
      bool producer;
      Impl::Instantiation* inst = impl.claimClass(classTy, producer);
      if (!producer)
        return; // Done or being done by someone else; nothing here needs its result.
      RecordInto into(&inst->deps); // what checking the instance reads is its own
      Scope* cls = classTy->decl->ownScope;
      if (impl.queueMemberBodies.load(std::memory_order_relaxed))
      {
        // While the files are resolved: the instance's member bodies a task for whichever worker
        // is free (Impl::MemberTask), not a weight on the thread that happened to ask first.
        const ParsedFile* askedIn = current && file ? file : nullptr;
        uint32_t askedAt = askedIn ? current->token : UINT32_MAX;
        impl.queueMemberBody({classTy, nullptr, askedIn, askedAt, instantiationDepth});
        inst->state.store(2, std::memory_order_release);
        inst->state.notify_all();
        return;
      }
      auto start = std::chrono::steady_clock::now();
      Subst subst = impl.substFor(classTy);
      subCheck(classTy->decl, subst, cls, types.pointer(classTy), [&](Walker& sub)
      {
        auto* c = static_cast<ClassDecl*>(classTy->decl);
        for (Decl* m : membersRead(c))
          if (m->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(m)->body && m->templateParams.empty())
            checkMemberBody(sub, static_cast<FunctionDecl*>(m), subst);
        for (FunctionDecl* f : cls->outOfLineMembers)
          if (f->body && (!Impl::isMemberSpecialization(f) || impl.memberSpecializationIsFor(f, classTy)))
            checkMemberBody(sub, f, subst); // `template<> R Button<RecipeID>::f()`: for its instantiation only
      });
      impl.instNanos += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
      inst->state.store(2, std::memory_order_release);
      inst->state.notify_all();
    }

    // Checks a function template body for one set of deduced arguments, once. Returns the
    // deduced return type when the function returns `auto` (waiting for another producer).
    // record: the instance's record, when one was claimed or found (complete unless this thread
    // is walking it).
    const Ty* instantiateFunction(FunctionDecl* f, const Subst& subst, bool needReturn, const Subst* classSubst = nullptr,
                                  const Ty* objectType = nullptr, Impl::InstanceRecord** record = nullptr)
    {
      for (Decl* p : f->templateParams)
        if (!subst.find(p) || types.isDependent(subst.find(p)) || !subst.find(p)->isKnown())
          return nullptr;
      // A member function template declared in its class and defined after it (`template<bool B>
      // auto* Net::maybeGet(int) const {...}`): the definition's body, its own template parameters
      // taking the declaration's arguments in order; the instance stays the declaration's.
      FunctionDecl* def = f;
      if (!f->body && needReturn && f->returnType && !f->trailingReturn && Impl::astMentionsAuto(f->returnType))
        if (Decl* cls = impl.owningClassOf(f); cls && cls->ownScope && !f->templateParams.empty())
        {
          uint32_t symbol = impl.functionSymbol(f->name);
          const Ty* ft = impl.matchedType(f);
          // (The declaration's `auto` return against whatever the definition's type has deduced:
          // the parameters and the method's constness decide.)
          auto sameParameters = [&](const Ty* a, const Ty* b)
          {
            if (!a || !b || a->kind != TyKind::Function || b->kind != TyKind::Function || a->args.size() != b->args.size() || a->variadic != b->variadic)
              return false;
            for (size_t i = 0; i < a->args.size(); ++i)
              if (a->args[i] != b->args[i])
                return false;
            return true;
          };
          for (FunctionDecl* candidate : cls->ownScope->outOfLineMembers)
            if (candidate->body && impl.functionSymbol(candidate->name) == symbol && !Impl::isMemberSpecialization(candidate) &&
                candidate->templateParams.size() == f->templateParams.size() && (candidate->flags & DF_Const) == (f->flags & DF_Const))
              if (const Ty* ct = impl.typeAs(candidate, f); ct == ft || (ft && ft->kind == TyKind::Function && impl.containsAuto(ft->inner) && sameParameters(ct, ft)))
              {
                def = candidate;
                break;
              }
        }
      if (!def->body || instantiationDepth > 16)
        return nullptr;
      if (classSubst)
        for (const Subst::Pair& pair : classSubst->pairs)
          if (!pair.type->isKnown() || types.isDependent(pair.type))
            return nullptr;
      Subst full = subst;
      if (classSubst)
        for (const Subst::Pair& pair : classSubst->pairs)
          if (!full.find(pair.param))
            full.pairs.push_back(pair);
      Subst defFull = full;
      if (def != f)
        for (size_t i = 0; i < f->templateParams.size(); ++i)
          defFull.pairs.push_back({def->templateParams[i], subst.find(f->templateParams[i])});
      Scope* bodyScope = def->name.parts.size() > 1 ? semanticScopeFor(def->name) : impl.lexicalScopeOf(def);
      const Ty* bodyThis = nullptr;
      if (objectType)
        bodyThis = types.pointer(types.unqualified(types.stripRef(objectType)));
      else if (Decl* cls = enclosingClassDecl(bodyScope))
        bodyThis = types.pointer(impl.substitute(impl.selfType(cls), full));
      ++impl.instRequests;
      bool producer;
      Impl::InstanceRecord* inst = impl.claimInstance(f, full, bodyThis, producer);
      if (record)
        *record = inst;
      if (!producer)
      {
        if (!needReturn)
          return nullptr;
        if (inst->state.load(std::memory_order_acquire) != 2)
        {
          // Queued and not started (by this thread or another): its result is needed now, so
          // this walk takes the check over (never waits for a task no worker may have reached).
          if (inst->queued.exchange(0) == 1)
            return runFunctionInstance(inst, *inst->task);
          if (inst->owner.load() == std::this_thread::get_id())
            return nullptr; // Recursive auto function.
          ++impl.instWaits;
          for (int seen = inst->state.load(std::memory_order_acquire); seen != 2; seen = inst->state.load(std::memory_order_acquire))
            inst->state.wait(seen, std::memory_order_acquire); // blocks (C++20 atomic wait) instead of spinning on yield
        }
        return inst->deducedReturn;
      }
      Impl::FunctionInstanceTask task{f, def, full, defFull, bodyScope, bodyThis, current && file ? file : nullptr,
                                      current && file ? current->token : UINT32_MAX, instantiationDepth};
      if (!needReturn && impl.queueMemberBodies.load(std::memory_order_relaxed))
      {
        // While the files are resolved: a check nobody waits for is a task for whichever worker
        // is free (docs/compile-speed.md, step 3), not a weight on the file that asked first.
        inst->task = std::make_unique<Impl::FunctionInstanceTask>(std::move(task));
        inst->queued.store(1);
        impl.queueMemberBody({nullptr, nullptr, nullptr, 0, 0, inst});
        return nullptr;
      }
      return runFunctionInstance(inst, task);
    }

    // Checks the function instance inst describes (claimed by this walk, or a queued one this walk
    // took over) on this thread, and publishes its body and deduced return.
    const Ty* runFunctionInstance(Impl::InstanceRecord* inst, const Impl::FunctionInstanceTask& t)
    {
      RecordInto into(&inst->deps); // what typing the instance reads is its own
      if (recordingDependencies)
      {
        inst->recheck = std::make_unique<Impl::InstanceRecord::Recheck>();
        inst->recheck->source = t.def;
        inst->recheck->task = std::make_unique<Impl::FunctionInstanceTask>(t);
      }
      inst->owner.store(std::this_thread::get_id());
      // At the depth of the walk that asked for it, whoever runs it (the depth limit decides what
      // is checked, so it must not depend on which walk took a queued check over).
      int savedDepth = std::exchange(instantiationDepth, t.depth);
      struct DepthRestore { int& d; int saved; ~DepthRestore() { d = saved; } } depthRestore{instantiationDepth, savedDepth};
      if (!t.f->templateParams.empty())
        ++impl.instFunctions, inst->counted |= 2; // a function template's (a class template's member claimed here or by its class's check counts as a body either way)
      auto start = std::chrono::steady_clock::now();
      // The instantiation's own copy of f's nodes (its definition's), typed here and kept as its body.
      // One with no arguments at all is a function that is no template, typed before the walk of
      // its file reached it (for its `auto` return): its lambdas are the types that walk gives
      // them, which the code generator emits (`chunkLineByLineScan(...)`'s TransformView).
      FunctionDecl* copy = impl.copyInstance(t.def, t.bodyScope, t.full, t.full.pairs.empty());
      inst->body = copy;
      const Ty* deduced = nullptr;
      const Ty* pattern = nullptr; // the written `auto&`, `const auto*`...: the deduced type takes its shape
      subCheck(t.def, t.defFull, t.bodyScope, t.bodyThis, [&](Walker& sub)
      {
        if (t.requestFile)
        {
          sub.requestFile = t.requestFile;
          sub.requestToken = t.requestToken;
        }
        sub.returnFrames.push_back(nullptr);
        sub.walkDecl(copy);
        deduced = sub.returnFrames.back();
        sub.returnFrames.pop_back();
        if (copy->returnType && !copy->trailingReturn)
          pattern = sub.typeOf(copy->returnType);
        ++impl.instBodies;
        inst->counted |= 1;
      }, inst);
      inst->deducedReturn = deduced ? impl.deducedShape(pattern, deduced) : nullptr;
      impl.instNanos += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
      inst->state.store(2, std::memory_order_release);
      inst->state.notify_all();
      return inst->deducedReturn;
    }

    // --- names ---

    // A name two entities answer equally (a namespace's class and a using-directive's): an error,
    // as in C++, where the first was taken silently.
    void reportAmbiguity(const Lookup& l, const QualifiedName& name, uint32_t token)
    {
      if (l.ambiguousIn && !name.empty())
      {
        reportBaseAmbiguity(l, name.last().symbol, token);
        return;
      }
      if (!l.ambiguousWith || !l.decl || !file || name.parts.size() != 1)
        return;
      std::string spelled(impl.symbols.name(name.parts[0].symbol));
      impl.reportCheckerError(file, token, "'" + spelled + "' is ambiguous: the one declared at " + impl.locationOf(l.decl->file, l.decl->token) + " and the one at " +
                                               impl.locationOf(l.ambiguousWith->file, l.ambiguousWith->token) + " (a using-directive puts both at one level)");
    }

    // A member two bases of a class have ([class.member.lookup] 6): an error where it is used.
    void reportBaseAmbiguity(const Lookup& l, uint32_t symbol, uint32_t token)
    {
      if (!l.ambiguousIn || !l.foundIn || !file || probing || inGenericTemplate() || symbol == UINT32_MAX)
        return;
      auto className = [&](Scope* s)
      {
        if (s && s->owner && s->owner->kind == NodeKind::ClassDecl && !static_cast<ClassDecl*>(s->owner)->name.empty())
          return std::string(impl.symbols.name(static_cast<ClassDecl*>(s->owner)->name.last().symbol));
        return std::string("?");
      };
      impl.reportCheckerError(file, token, "the member " + std::string(impl.symbols.name(symbol)) + " is ambiguous: both bases " + className(l.foundIn) +
                                               " and " + className(l.ambiguousIn) + " have one");
    }

    void resolveTypeName(NamedType* t)
    {
      if (t->name.empty())
        return;
      Lookup l = impl.lookupQualified(t->name, scope, &locals, 0, activeSubst);
      reportAmbiguity(l, t->name, t->token);
      if (l.found == Found::Local && l.decl && l.decl->when && t->name.parts.size() == 1)
      {
        // A local alias, class or enumeration a `when` declares (design.md 19): the one of the name
        // that exists here (convertType takes it from resolved), or the error that none does.
        bool undecided = false;
        Decl* named = whenUse(t->name.parts[0].symbol, t->token, l.decl, undecided);
        if (!named)
        {
          t->resolved = nullptr;
          return;
        }
        l.decl = named;
        l.scope = named->ownScope;
      }
      t->resolved = l.decl;
      if (l.found == Found::No)
      {
        if (t->name.parts.size() == 1 && impl.builtinByName(t->name.parts[0].symbol))
          ++stats.typesBuiltinByName;
        else if (t->name.parts.size() == 1 && t->name.parts[0].hasTemplateArgs && impl.isTypeTraitName(t->name.parts[0].symbol))
          ++stats.typesResolved; // RemoveConst<T> and friends
        else
        {
          ++stats.typesUnresolved;
          countUnresolved(t->name, t->token);
          // A type name nothing declares is an error where it is written, as an undeclared name is
          // (not `class X` in a template argument, which declares X; not a value trait, isConst<T>,
          // written where a type may be; not in a template's own walk: its instances say).
          const NamePart& first = t->name.parts[0];
          if (!t->elaborated && !(t->name.parts.size() == 1 && first.hasTemplateArgs && impl.isBoolTraitName(first.symbol)) && !inGenericTemplate())
            reportUndeclared(t->name, t->token);
        }
      }
      else if (l.found == Found::Dependent)
        ++stats.typesDependent;
      else
        ++stats.typesResolved;
      checkTemplateArgumentCount(t, l);
      for (const NamePart& part : t->name.parts)
        for (Node* arg : part.templateArgs)
          walkNode(arg);
    }

    // `UniquePtr<Thing, CountingDeleter>` with a one-parameter UniquePtr: more template arguments
    // than the class (or alias) template has parameters, and no pack to take the rest, is a
    // compile error, as in C++ (the extra argument was dropped silently).
    void checkTemplateArgumentCount(NamedType* t, const Lookup& l)
    {
      const NamePart& last = t->name.last();
      if (!last.hasTemplateArgs || !l.decl || !file)
        return;
      Decl* def = l.decl->kind == NodeKind::ClassDecl && l.scope && l.scope->owner && l.scope->kind == ScopeKind::Class ? l.scope->owner : l.decl;
      if (def->kind == NodeKind::ClassDecl && l.overloads)
        if (Decl* primary = impl.primaryOf(*l.overloads))
          def = primary; // the primary's parameters, not a partial specialisation's (`FallbackValueHolder<T, true>`)
      if (def->kind != NodeKind::ClassDecl && def->kind != NodeKind::UsingAliasDecl)
        return;
      Span<Decl*> params = def->kind == NodeKind::ClassDecl ? impl.effectiveParams(def) : def->templateParams;
      if (params.empty())
        return; // not a template: other errors say so
      if (static_cast<TemplateParamDecl*>(params[params.size() - 1])->isPack)
        return;
      size_t given = 0;
      for (Node* arg : last.templateArgs)
      {
        if (arg && arg->kind == NodeKind::PackType)
          return; // `X<Args...>`: as many as the pack has, told once it is known
        ++given;
      }
      if (given <= params.size())
        return;
      std::string name = impl.debugName(last.symbol);
      impl.reportCheckerError(file, t->token, name + " takes " + std::to_string(params.size()) + " template argument" +
                                                  (params.size() == 1 ? "" : "s") + ", " + std::to_string(given) + " given");
    }

    // Converts a type written in the current context.
    const Ty* typeOf(Type* t) { return impl.convertType(t, ctx(), 0); }

    // Type of a name expression from the declaration it resolved to.
    // The hidden object of `auto [a, b] = e;` (v) as this walk deduced it; null when not known.
    const Ty* hiddenObjectType(VariableDecl* v)
    {
      const Ty* t = impl.declaredTypeOf(v, activeSubst, 0);
      if (t && impl.containsAuto(t))
      {
        if (const Ty* deduced = locals.deducedTypeOf(v))
          t = deduced; // the object as this walk deduced it (also a range-for variable's)
        else if (v->init && v->init->ty)
          t = impl.deduceAuto(t, v->init->ty);
        else if (const Ty* cached = v->cachedType.load(std::memory_order_acquire); cached && !impl.containsAuto(cached))
          t = cached;
        else
          t = nullptr;
      }
      return t && t->isKnown() ? t : nullptr;
    }

    // `auto [a, b] = pair;`: the type of the binding named `symbol`, the matching data member (or
    // array element) of the hidden object the declaration names; null when symbol is no binding.
    // (A tuple-like binding names a variable of its own: tupleBindings.)
    const Ty* bindingType(VariableDecl* v, uint32_t symbol)
    {
      size_t index = v->structuredBindings.size();
      for (size_t i = 0; i < v->structuredBindings.size(); ++i)
        if (static_cast<NameExpr*>(v->structuredBindings[i])->name.last().symbol == symbol)
          index = i;
      if (index == v->structuredBindings.size())
        return nullptr;
      const Ty* t = hiddenObjectType(v);
      if (!t)
        return unknown(Why::Auto);
      bool isConst = types.stripRef(t)->isConst;
      const Ty* object = types.unqualified(types.stripRef(t));
      if (object->kind == TyKind::Array)
        return isConst ? types.qualified(object->inner, true, false) : object->inner;
      if (object->kind == TyKind::Param)
        return object; // the element of a range that depends on a template parameter
      if (object->kind != TyKind::Class)
        return unknown(Why::Unsupported);
      // The class declaring the members: the object's, or the base all of them are in.
      const Ty* holder = impl.bindingMembersClass(object);
      Scope* cs = holder ? impl.classScopeOf(holder) : nullptr;
      if (!cs || !cs->owner || cs->owner->kind != NodeKind::ClassDecl)
        return unknown(Why::Unsupported);
      size_t seen = 0;
      for (Decl* m : membersRead(static_cast<ClassDecl*>(cs->owner)))
      {
        if (m->kind != NodeKind::VariableDecl || (m->flags & (DF_Static | DF_Typedef)))
          continue;
        if (seen++ == index)
        {
          Subst s = holder->args.empty() ? Subst{} : impl.substFor(holder);
          const Ty* mt = impl.declaredTypeOf(m, holder->args.empty() ? activeSubst : &s, 0);
          return mt && isConst ? types.qualified(mt, true, false) : mt;
        }
      }
      return unknown(Why::Unsupported);
    }

    const Ty* typeOfResolvedName(const Lookup& l, const NamePart& lastPart, Expr* e)
    {
      Decl* d = l.decl;
      if (!d)
        return unknown(Why::Name);
      switch (d->kind)
      {
        case NodeKind::VariableDecl:
        {
          if (auto* v = static_cast<VariableDecl*>(d); !v->structuredBindings.empty())
            if (const Ty* bt = bindingType(v, lastPart.symbol))
              return bt;
          if (lastPart.hasTemplateArgs && Impl::conceptOf(d))
            return types.boolType(); // `has_load<T, Input>` as a value: a concept-id is a bool
          if (lastPart.hasTemplateArgs && !d->templateParams.empty())
            if (const Ty* t = impl.variableTemplateType(static_cast<VariableDecl*>(d), lastPart, ctx()))
              return t; // `two_v<int>`: T is int here
          if (l.found == Found::Local)
            if (const Ty* walked = locals.deducedTypeOf(d); walked && !impl.containsAuto(walked))
              return walked; // the type this walk gave the local (a class of the function's own, under an instance's arguments)
          const Ty* t = impl.declaredTypeOf(d, activeSubst, 0);
          if (impl.containsAuto(t))
          {
            // A local whose initializer was typed already has its deduced type cached by the
            // walker; otherwise (members/globals in other files) deduce from the initializer now.
            auto* v = static_cast<VariableDecl*>(d);
            if (const Ty* deduced = locals.deducedTypeOf(v))
              return deduced; // a local (or range-for variable) as this walk deduced it
            // (Only a known initializer type: an unknown one is there or not by whether the
            // variable's own walk came first, and the use was then counted as auto's or not.)
            if (v->init && v->init->ty && v->init->ty->isKnown() && v->init->kind != NodeKind::BraceInitExpr)
              return impl.deduceAuto(t, v->init->ty);
            return unknown(Why::Auto);
          }
          return t;
        }
        case NodeKind::ParamDecl:
        {
          if (l.found == Found::Local)
            if (const Ty* walked = locals.deducedTypeOf(d))
              return walked; // a lambda's parameter as this walk typed it
          const Ty* t = impl.declaredTypeOf(d, activeSubst, 0);
          // A function parameter pack in the pattern of one element's expansion: that element.
          if (static_cast<ParamDecl*>(d)->isPack && elementPacks && !elementPacks->empty() && t && t->kind == TyKind::Pack && elementIndex < t->args.size())
            return t->args[elementIndex];
          return impl.containsAuto(t) ? unknown(Why::Auto) : t;
        }
        case NodeKind::EnumeratorDecl:
          return impl.declaredTypeOf(d, activeSubst, 0); // (of this instance's enum in a class template)
        case NodeKind::FunctionDecl:
        {
          // `getOrCreate<Expression, const NoiseExpression&>` as a value: the function template
          // with its explicit arguments (a trailing pack takes the rest), once they bind all of
          // its parameters.
          auto* f = static_cast<FunctionDecl*>(d);
          if (lastPart.hasTemplateArgs && !f->templateParams.empty() && (!l.overloads || l.overloads->size() <= 1))
          {
            Subst s;
            bindExplicitArgs(f, lastPart, Span<Expr*>{}, s);
            bool bound = true;
            for (Decl* p : f->templateParams)
              bound = bound && s.find(p);
            if (bound)
            {
              if (activeSubst)
                for (const Subst::Pair& p : activeSubst->pairs)
                  if (!s.find(p.param))
                    s.pairs.push_back(p); // the enclosing instance's (a member of a class template)
              return impl.declaredTypeOf(f, &s, 0);
            }
          }
          return impl.declaredTypeOf(d, activeSubst, 0);
        }
        case NodeKind::ClassDecl:
        case NodeKind::EnumDecl:
        case NodeKind::UsingAliasDecl:
        {
          // A type used as an expression: constructor call `Foo(x)`, `Foo{}` or `Enum(x)`.
          TypeContext c = ctx();
          // `const_iterator(*this)` in a member of ParticleSurface : InfiniteRectangle<ParticleChunk*>:
          // the base's nested class with the base's arguments.
          Subst bs;
          if (e && e->kind == NodeKind::NameExpr && static_cast<NameExpr*>(e)->name.parts.size() == 1)
            bs = impl.baseSubstForUnqualified(l, c, 0);
          if (!bs.empty())
          {
            if (activeSubst)
              for (const Subst::Pair& p : activeSubst->pairs)
                bs.pairs.push_back(p);
            c.subst = &bs;
          }
          return impl.typeOfTypeDecl(d, l, lastPart, c, 0);
        }
        case NodeKind::TemplateParamDecl:
        {
          const Ty* t = impl.declaredTypeOf(d, activeSubst, 0);
          return t && t->kind == TyKind::Value && t->inner ? t->inner : t; // `M == Mode::Fast`: the value's type
        }
        case NodeKind::NamespaceDecl:
          return unknown(Why::Unsupported);
        default:
          (void)e;
          return unknown(Why::Unsupported);
      }
    }

    // The type of a template parameter named as a value or a type: what activeSubst binds it to,
    // a constant parameter's value as its type, and in the pattern of one element's expansion
    // (elementPacks) that element (`values[I]...` over `int... I`, `func(Args())...`).
    const Ty* dependentNameType(Decl* d)
    {
      const Ty* t = impl.declaredTypeOf(d, activeSubst, 0);
      const Ty* element = Impl::elementOf(d, t, ctx());
      if (element != t && element && element->kind == TyKind::Value && d->kind == NodeKind::TemplateParamDecl &&
          !static_cast<TemplateParamDecl*>(d)->isType && element->inner)
        return element->inner; // a constant's element, as declaredTypeOf gives a constant parameter's type
      return element;
    }

    const Ty* resolveName(NameExpr* e)
    {
      if (e->name.empty())
      {
        // The hidden object of `auto [a, b] = e;`, which a tuple-like binding's get<i>(object)
        // names (tupleBindings).
        if (e->resolved && e->resolved->kind == NodeKind::VariableDecl && !static_cast<VariableDecl*>(e->resolved)->structuredBindings.empty())
          if (const Ty* t = hiddenObjectType(static_cast<VariableDecl*>(e->resolved)))
            return t;
        return unknown(Why::Unsupported);
      }
      walkType(e->name.typeQualifier); // `decltype(this->deviation)::CoordinateType::FractionalBits`
      if (e->name.parts.size() == 1 && e->name.parts[0].hasTemplateArgs && impl.isBoolTraitName(e->name.parts[0].symbol))
      {
        // A builtin trait: a bool the code generator evaluates once the arguments are known.
        for (Node* arg : e->name.parts[0].templateArgs)
          walkNode(arg);
        e->resolved = nullptr;
        ++stats.namesGlobal;
        return types.boolType();
      }
      Lookup l = impl.lookupQualified(e->name, scope, &locals, 0, activeSubst);
      if (l.found == Found::No && e->name.parts.size() > 1)
        l = memberOfQualifierInstance(e);
      Impl::preferValue(l); // (`&stat`: the function, not the struct)
      reportAmbiguity(l, e->name, e->token);
      // `using Colors::shade;` in a namespace: the name is the variable it names (functions stay the
      // using-declaration's overload set, which a call takes through withNamespaceUsings).
      if (l.decl && l.decl->kind == NodeKind::UsingNameDecl && !static_cast<UsingNameDecl*>(l.decl)->isEnum && !l.ambiguousWith && l.foundIn &&
          l.foundIn->kind == ScopeKind::Namespace)
      {
        Lookup target = impl.lookupQualified(static_cast<UsingNameDecl*>(l.decl)->name, impl.lexicalScopeOf(l.decl), nullptr, 0);
        if (target.decl && target.decl->kind == NodeKind::VariableDecl)
          l = target;
      }
      e->resolved = l.decl;
      if (!whenLookup(l, e))
        return types.unknown(); // (design.md 19: reported, or the instances' to tell)
      for (const NamePart& part : e->name.parts)
        for (Node* arg : part.templateArgs)
          walkNode(arg);
      switch (l.found)
      {
        case Found::No:
          if (e->name.parts.size() == 1)
            if (const Ty* b = impl.builtinByName(e->name.parts[0].symbol))
              return b; // `uint32_t(x)`
          ++stats.namesUnresolved;
          countUnresolved(e->name, e->token);
          reportUndeclared(e->name, e->token, e);
          return unknown(Why::Name);
        case Found::Local: ++stats.namesLocal; break;
        case Found::Member: ++stats.namesMember; break;
        case Found::Global: ++stats.namesGlobal; break;
        case Found::Dependent: ++stats.namesDependent; break;
      }
      if (l.found == Found::Dependent)
        return l.decl ? dependentNameType(l.decl) : types.unknown();
      // Members of the current class template keep their dependent types; members reached
      // through a qualified name on an instantiation get substituted.
      if ((e->name.parts.size() > 1 || e->name.typeQualifier) && l.found == Found::Member && l.foundIn && l.foundIn->owner &&
          (!l.foundIn->owner->templateParams.empty() ||
           (l.foundIn->owner->kind == NodeKind::EnumDecl && !Impl::effectiveParams(l.foundIn->owner).empty())))
      {
        // Qualifier like `Vector<int>::npos`: substitute from the qualifier type when it is an
        // instantiation; `decltype(check<T>(nullptr))::value` from the class the decltype names.
        QualifiedName qualifier = e->name;
        qualifier.parts.count -= 1;
        NamedType tmp{};
        tmp.kind = NodeKind::NamedType;
        tmp.name = qualifier;
        const Ty* q = qualifier.parts.empty() ? typeOf(e->name.typeQualifier) : typeOf(&tmp);
        q = q ? types.unqualified(types.stripRef(q)) : types.unknown();
        // `IsSmall::value` with `struct IsSmall : Constant<bool, true> {}`: a member of a template
        // base takes that base instantiation's arguments.
        if (q->kind == TyKind::Class && (!q->args.empty() || (q->decl && q->decl->ownScope != l.foundIn)))
          if (Subst s = impl.memberSubst(q, l.foundIn, 0); !s.pairs.empty())
            return impl.declaredTypeOf(l.decl, &s, 0);
        // `Fast::Enum::True` with an enum declared in a class template: that instance's enumerator.
        if (q->kind == TyKind::Enum)
          if (Subst s = impl.substFor(q); !s.pairs.empty())
            return impl.declaredTypeOf(l.decl, &s, 0);
      }
      return typeOfResolvedName(l, e->name.last(), e);
    }

    // `Accumulator::kind` (`&T::kind` in an instance) with kind a member of a base that an
    // instantiation's arguments choose (`class BehaviorTemplate : public Parent`, Parent =
    // Behavior): the class template's scope links no such base, so the name is looked up as a
    // member of the qualifier's type, through its instantiated bases.
    Lookup memberOfQualifierInstance(NameExpr* e)
    {
      QualifiedName qualifier = e->name;
      qualifier.parts.count -= 1;
      NamedType written{};
      written.kind = NodeKind::NamedType;
      written.token = e->token;
      written.name = qualifier;
      const Ty* q = typeOf(&written);
      q = q ? types.unqualified(types.stripRef(q)) : nullptr;
      if (!q || q->kind != TyKind::Class || types.isDependent(q))
        return {};
      MemberResult r = memberOf(q, e->name.last().symbol, nullptr, e->token, false);
      if (!r.decl || !r.decl->parent || r.decl->parent->kind != NodeKind::ClassDecl || !r.decl->parent->ownScope)
        return {};
      Lookup l;
      l.found = Found::Member;
      l.decl = r.decl;
      l.overloads = r.overloads;
      l.foundIn = r.decl->parent->ownScope;
      return l;
    }

    // --- member access through the object's type ---

    struct MemberResult
    {
      const Ty* type = nullptr;
      Decl* decl = nullptr;
      const std::vector<Entry>* overloads = nullptr;
      std::shared_ptr<std::vector<Entry>> merged; // what overloads points to when built here (a using-declaration's set joined)
      Subst subst;
      Why why = Why::None;
    };

    // Follows operator-> on class types for `a->b`.
    const Ty* objectForArrow(const Ty* t, int depth = 0)
    {
      t = types.stripRef(t);
      if (!t || depth > 4)
        return t;
      if (t->kind == TyKind::Pointer)
        return t->inner;
      if (t->kind == TyKind::Class)
      {
        MemberResult arrow = memberOf(t, impl.opSymbols[uint32_t(P::Arrow)], nullptr, 0, false);
        if (arrow.overloads && arrow.overloads->size() > 1)
        {
          // `const T* operator->() const` and `T* operator->()`: the one the object's constness picks.
          Decl* pick = nullptr;
          for (const Entry& entry : *arrow.overloads)
            if (entry.decl && entry.decl->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(entry.decl)->params.empty() &&
                (!pick || ((entry.decl->flags & DF_Const) != 0) == t->isConst))
              pick = entry.decl;
          if (pick)
          {
            arrow.decl = pick;
            arrow.type = impl.declaredTypeOf(pick, arrow.subst.pairs.empty() ? nullptr : &arrow.subst, 0);
          }
        }
        // `auto operator->() { return this->idIterator.operator->(); }`: what the body returns.
        if (arrow.decl && arrow.decl->kind == NodeKind::FunctionDecl &&
            (arrow.why == Why::Auto || (arrow.type && arrow.type->kind == TyKind::Function && impl.containsAuto(arrow.type->inner))))
          return objectForArrow(returnTypeOfCall(static_cast<FunctionDecl*>(arrow.decl), arrow.subst.pairs.empty() ? nullptr : &arrow.subst,
                                                 Span<Expr*>{}, nullptr),
                                depth + 1);
        if (arrow.type && arrow.type->kind == TyKind::Function)
          return objectForArrow(arrow.type->inner, depth + 1);
        return types.unknown();
      }
      return t;
    }

    // Looks a member up in the class of `objectTy` (after stripping references and const).
    // The instantiation a member found in scope `in` belongs to, seen through an object of type t
    // whose class scope is cls: t itself, or the base (as written in cls, under t's substitution)
    // on the way to `in`. `v.x` with x declared in Vector2<T, V> and v a Vec: Vector2<double, Vec>.
    const Ty* instantiationOfBase(const Ty* t, Scope* cls, Scope* in, int depth)
    {
      return impl.instantiationOfBase(t, cls, in, depth);
    }

    // `symbol` in the bases of the instantiation t (class scope cls) as its arguments make them.
    std::optional<MemberResult> memberOfInstantiatedBases(const Ty* t, Scope* cls, uint32_t symbol, uint32_t token)
    {
      if (!cls->owner || instantiatedBaseDepth > 16)
        return std::nullopt;
      Subst own = impl.substFor(t);
      LocalStack params;
      for (Decl* p : cls->templateParams)
        params.push(static_cast<TemplateParamDecl*>(p)->name, p);
      ParsedFile* file = cls->owner->file;
      TypeContext ctx{cls->parent, &params, own.pairs.empty() ? nullptr : &own, file ? file->tokens : nullptr, file ? file->text : std::string_view()};
      // Every base is searched: a name two bases declare (members of two different classes) is
      // ambiguous, as C++'s member lookup says ([class.member.lookup] 6), never the first base's.
      std::optional<MemberResult> result;
      const Ty* resultBase = nullptr;
      ++instantiatedBaseDepth;
      for (Type* b : cls->baseTypes)
        for (const Ty* bt : impl.baseInstances(b, ctx, 1))
        {
          if (!bt || bt->kind != TyKind::Class || !bt->decl || types.isDependent(bt))
            continue;
          MemberResult r = memberOf(bt, symbol, nullptr, token, false);
          if (!r.decl)
            continue;
          if (!result)
          {
            result = std::move(r);
            resultBase = bt;
          }
          else if (r.decl->parent != result->decl->parent && file && !probing && !inGenericTemplate())
            impl.reportCheckerError(file, token,
                                    "the member " + std::string(impl.symbols.name(symbol)) + " is ambiguous: both bases " +
                                        types.toString(resultBase, &impl.symbols) + " and " + types.toString(bt, &impl.symbols) + " have one");
        }
      --instantiatedBaseDepth;
      return result;
    }

    // `using Vector2::save;` beside the class's own save overloads (l found in a class scope; t the
    // class as the object or `this` has it, cls its scope): both sets as candidates, own first,
    // with the arguments of the base instances the using-declarations reach in baseArgs; null when
    // l has no such mix.
    std::shared_ptr<std::vector<Entry>> withUsingOverloads(const Lookup& l, const Ty* t, Scope* cls, Subst& baseArgs)
    {
      if (!l.overloads || l.overloads->size() < 2 || !l.foundIn || l.foundIn->kind != ScopeKind::Class)
        return nullptr;
      auto isUsing = [](Decl* d) { return d && d->kind == NodeKind::UsingNameDecl && !static_cast<UsingNameDecl*>(d)->isEnum; };
      bool hasUsing = false, hasOwn = false;
      for (const Entry& e : *l.overloads)
      {
        hasUsing = hasUsing || isUsing(e.decl);
        hasOwn = hasOwn || (e.decl && e.decl->kind == NodeKind::FunctionDecl);
      }
      if (!hasUsing || !hasOwn)
        return nullptr;
      auto merged = std::make_shared<std::vector<Entry>>();
      for (const Entry& e : *l.overloads)
        if (e.decl && e.decl->kind == NodeKind::FunctionDecl)
          merged->push_back(e);
      size_t ownCount = merged->size();
      // The class's own function hides the base's of the same parameter types and qualifiers
      // (C++ [namespace.udecl] 15): ArtilleryTurret's `getInventory(uint8) override` beside `using
      // super::getInventory;` is one candidate. Only where both are plain functions whose parameter
      // types name no template parameter.
      auto hidden = [&](FunctionDecl* base)
      {
        const uint32_t quals = DF_Const | DF_RefQual | DF_Static | DF_Variadic;
        if (!base->templateParams.empty())
          return false;
        const Ty* bt = impl.declaredTypeOf(base, nullptr, 0);
        if (!bt || bt->kind != TyKind::Function || types.isDependent(bt))
          return false;
        for (size_t i = 0; i < ownCount; ++i)
        {
          auto* own = static_cast<FunctionDecl*>((*merged)[i].decl);
          if (!own->templateParams.empty() || (own->flags & quals) != (base->flags & quals) || own->params.size() != base->params.size())
            continue;
          const Ty* ot = impl.declaredTypeOf(own, nullptr, 0);
          if (!ot || ot->kind != TyKind::Function || ot->args.size() != bt->args.size())
            continue;
          bool same = true;
          for (size_t a = 0; a < ot->args.size() && same; ++a)
            same = ot->args[a] == bt->args[a];
          if (same)
            return true;
        }
        return false;
      };
      for (const Entry& e : *l.overloads)
      {
        if (!isUsing(e.decl))
          continue;
        Lookup target = impl.lookupQualified(static_cast<UsingNameDecl*>(e.decl)->name, l.foundIn, nullptr, 0);
        if (target.overloads)
          for (const Entry& te : *target.overloads)
            if (te.decl && te.decl->kind == NodeKind::FunctionDecl && !hidden(static_cast<FunctionDecl*>(te.decl)))
              merged->push_back(te);
        if (t && cls && target.foundIn && target.foundIn != l.foundIn && target.foundIn->kind == ScopeKind::Class)
          if (const Ty* base = instantiationOfBase(t, cls, target.foundIn, 0))
            for (const Subst::Pair& p : impl.substFor(base).pairs)
              if (!baseArgs.find(p.param))
                baseArgs.pairs.push_back(p);
      }
      return merged;
    }

    MemberResult memberOf(const Ty* objectTy, uint32_t symbol, const QualifiedName* name, uint32_t token, bool countMissing = true)
    {
      MemberResult r;
      const Ty* t = types.stripRef(objectTy);
      if (!t || !t->isKnown())
      {
        r.why = Why::Base;
        return r;
      }
      if (t->kind == TyKind::Param || (t->kind == TyKind::Class && types.isDependent(t) && !t->decl))
      {
        r.type = types.param(t->decl);
        return r;
      }
      if (t->kind == TyKind::DependentMember)
      {
        // `typename T::X` object: known once T is.
        r.type = types.param(nullptr);
        return r;
      }
      if (t->kind != TyKind::Class && t->kind != TyKind::Enum)
      {
        r.why = Why::Base;
        return r;
      }
      Scope* cls = t->decl ? t->decl->ownScope : nullptr;
      if (!cls)
      {
        // Only a forward declaration was seen.
        Entry e{t->decl, nullptr};
        cls = impl.scopeOfEntry(e, 0);
        if (!cls)
        {
          r.why = Why::Base;
          return r;
        }
      }
      Lookup l;
      bool found = true;
      const Ty* foundInBase = nullptr; // the instantiation of the base l was found in, once computed
      if (name && name->parts.size() > 1)
      {
        // `obj.Base::member`: Base as the object's class sees it, else as the expression's context
        // does (`prototype->Super::addValues(result)` with Super a template parameter).
        l = impl.lookupQualified(*name, cls, nullptr, 0);
        if (l.found == Found::No || !l.decl)
        {
          QualifiedName qualifier = *name;
          qualifier.parts.count -= 1;
          NamedType written{};
          written.kind = NodeKind::NamedType;
          written.token = token;
          written.name = qualifier;
          const Ty* q = typeOf(&written);
          if (q && q->isKnown() && types.isDependent(q))
          {
            r.type = types.param(nullptr); // known once Super is
            return r;
          }
          if (Scope* qs = q && q->kind == TyKind::Class ? impl.classScopeOf(q) : nullptr)
          {
            Lookup inQualifier;
            if (impl.lookupInScope(qs, symbol, inQualifier, 0))
              l = inQualifier;
          }
        }
      }
      else
      {
        found = impl.lookupInScope(cls, symbol, l, 0);
        reportBaseAmbiguity(l, symbol, token);
        // A class template's bases are linked to its scope once, without its arguments, so a base
        // the arguments choose (`struct X : BoolConstant<B>`, `Conditional<c, Value<T>, T>`) is
        // missing there or is some other class: an instantiation's members come from its own bases.
        if (!t->args.empty() && !types.isDependent(t) && !cls->baseTypes.empty())
        {
          bool viaOtherClass = false;
          if (found && l.foundIn && l.foundIn != cls && l.foundIn->kind == ScopeKind::Class)
          {
            foundInBase = instantiationOfBase(t, cls, l.foundIn, 0);
            viaOtherClass = !foundInBase;
          }
          if ((!found && !l.absent) || viaOtherClass) // (a false `when` member's name stops it: design.md 19)
            if (std::optional<MemberResult> viaBase = memberOfInstantiatedBases(t, cls, symbol, token))
              return *viaBase;
        }
        else if (!found && !l.absent && !types.isDependent(t) && !cls->baseTypes.empty())
        {
          // A plain class deriving from such an instantiation (`struct Item : H<Item, true>`,
          // whose Hook takes a sibling nested class as its base by the arguments): the member
          // through the bases as types.
          if (std::optional<MemberResult> viaBase = memberOfInstantiatedBases(t, cls, symbol, token))
            return *viaBase;
        }
      }
      if (!found && l.absent)
      {
        // A member a false `when` dropped (design.md 19): used where its condition is not known to hold.
        if (file && !file->isImported && !probing && token)
        {
          std::string condition = conditionText(l.absent);
          impl.reportCheckerError(file, token, "'" + std::string(symbols.name(symbol)) + "' exists only when " + condition +
                                                   " holds: use it inside `if constexpr (" + condition + ")` or a `when` on it");
        }
        r.type = types.unknown();
        return r;
      }
      if (!found)
      {
        if (types.isDependent(t))
        {
          r.type = types.unknown();
          r.why = Why::None; // Member of a dependent base: cannot know yet.
          if (countMissing)
            ++stats.membersDependent;
          return r;
        }
        if (hasUnknownBase(cls, 0))
        {
          // A base class outside the corpus (std::vector...) may declare it.
          r.why = Why::Base;
          if (countMissing)
          {
            ++stats.membersUnknownBase;
            ++stats.unknownBaseMembers[symbol];
          }
          return r;
        }
        r.why = Why::Member;
        if (countMissing)
        {
          ++stats.membersNotFound;
          ++stats.missingMembers[symbol];
          stats.missingMembersFirst.emplace(symbol, ResolveStats::Where{stats.fileIndex, token});
        }
        return r;
      }
      Subst usingArgs;
      std::shared_ptr<std::vector<Entry>> merged = withUsingOverloads(l, t, cls, usingArgs);
      if (merged)
      {
        l.decl = merged->front().decl;
        l.overloads = merged.get();
      }
      if (l.decl && l.decl->kind == NodeKind::UsingNameDecl && !static_cast<UsingNameDecl*>(l.decl)->isEnum)
      {
        // `using Base::f;` in the class: f means the base's overloads, as members of that base
        // (`using super::front;` with `using super = CompactDeque<T>;`: front's T is CompactDeque's).
        Lookup target = impl.lookupQualified(static_cast<UsingNameDecl*>(l.decl)->name, l.foundIn ? l.foundIn : cls, nullptr, 0);
        if (target.found != Found::No && target.decl)
        {
          l.decl = target.decl;
          l.overloads = target.overloads;
          if (target.foundIn && target.foundIn->kind == ScopeKind::Class)
          {
            l.foundIn = target.foundIn;
            foundInBase = nullptr;
          }
        }
      }
      if (l.found == Found::No || !l.decl)
      {
        r.why = Why::Member;
        return r;
      }
      Impl::preferValue(l); // (`p.Reading(3)` beside a nested class Reading: the member function)
      r.decl = l.decl;
      r.overloads = l.overloads;
      r.merged = merged;
      const Ty* owner = t;
      if (l.foundIn && l.foundIn != cls && l.foundIn->kind == ScopeKind::Class)
        if (const Ty* base = foundInBase ? foundInBase : instantiationOfBase(t, cls, l.foundIn, 0))
          owner = base; // `v.x` with x declared in the base Vector2<T, V>: that base's instantiation binds T
      r.subst = impl.substFor(owner);
      if (!r.subst.empty())
        instantiateClass(types.unqualified(owner));
      for (const Subst::Pair& p : usingArgs.pairs)
        if (!r.subst.find(p.param))
          r.subst.pairs.push_back(p);
      if (l.found == Found::Dependent)
      {
        r.type = types.param(l.decl);
        return r;
      }
      // Several `when` data members of the name in a class template (design.md 19, an #ifdef/#else pair: `when
      // (isTriviallyCopyable<T>) T copy; when (!isTriviallyCopyable<T>) short copy;`): the instance's is the one whose
      // condition holds under its arguments, as its layout has it (codegen's whenMemberHolds). None, or two, is an error.
      if (l.decl->kind == NodeKind::VariableDecl && l.decl->when && l.decl->whenKnown < 0 && l.overloads && l.overloads->size() > 1 &&
          !types.isDependent(owner) && !r.subst.empty())
      {
        int holding = 0;
        bool undecided = false;
        if (Decl* chosen = impl.whenMemberOf(l.decl, r.subst, holding, undecided))
        {
          r.decl = chosen;
          r.overloads = nullptr;
        }
        else if (!undecided)
        {
          if (file && !file->isImported && !probing && token)
            impl.reportCheckerError(file, token, "'" + std::string(symbols.name(symbol)) + "' " +
                                                     (holding == 0 ? "does not exist in " + types.toString(owner, &symbols) + ": none of its `when` conditions holds there"
                                                                   : "is declared twice in " + types.toString(owner, &symbols) + ": the `when` conditions of " +
                                                                       std::to_string(holding) + " of its declarations hold there") +
                                                     " (design.md 19)");
          r.type = types.unknown();
          return r;
        }
      }
      const Ty* declared = impl.declaredTypeOf(r.decl, &r.subst, 0);
      if (impl.containsAuto(declared))
      {
        auto* v = r.decl->kind == NodeKind::VariableDecl ? static_cast<VariableDecl*>(r.decl) : nullptr;
        declared = v && v->init && v->init->ty ? impl.deduceAuto(declared, v->init->ty) : types.unknown();
        if (!declared->isKnown())
          r.why = Why::Auto;
      }
      r.type = declared;
      return r;
    }

    // True if the class or one of its bases has a base type that could not be resolved.
    bool hasUnknownBase(Scope* cls, int depth)
    {
      if (!cls || depth > 16 || cls->baseTypes.empty())
        return false;
      impl.ensureBases(cls, 0);
      if (cls->baseScopes.size() < cls->baseTypes.size())
        return true;
      for (Scope* b : cls->baseScopes)
        if (hasUnknownBase(b, depth + 1))
          return true;
      return false;
    }

    const Ty* typeOfMember(MemberExpr* m)
    {
      checkPostfixAfterParenthesisedType(m->base, m->token);
      const Ty* base = walkExpr(m->base);
      if (m->dynamicMember)
      {
        // `object.[m]`: the data member the descriptor m names (reflection, design.md 8.3).
        const Ty* d = types.stripRef(walkExpr(m->dynamicMember));
        const Ty* object = m->op == P::Arrow || m->op == P::SafeArrow ? objectForArrow(base) : base;
        if (!d || !d->isKnown())
          return unknown(Why::Base);
        if (d->kind != TyKind::Member || !d->decl)
        {
          m->resolved = nullptr;
          ++stats.membersDependent;
          return types.isDependent(d) ? d : unknown(Why::Unsupported); // in a template body: dependent until the loop binds m
        }
        m->resolved = d->decl;
        ++stats.membersBound;
        const Ty* t = d->inner ? d->inner : types.unknown();
        const Ty* obj = object ? types.stripRef(object) : nullptr;
        if (obj && obj->isConst && t->isKnown() && !t->isReference())
          t = types.qualified(t, true, false);
        return t;
      }
      for (const NamePart& part : m->member.parts)
        for (Node* arg : part.templateArgs)
          walkNode(arg);
      if (m->member.empty())
        return unknown(Why::Unsupported);
      const NamePart& last = m->member.last();
      uint32_t symbol = last.special == 2 && last.opToken < uint32_t(P::Count) ? impl.opSymbols[last.opToken] : last.symbol;
      if (symbol == UINT32_MAX)
      {
        if (last.special == 1)
        {
          // obj.~Foo()
          ++stats.membersBound;
          return types.function(types.voidType(), {}, false);
        }
        return unknown(Why::Unsupported);
      }
      const Ty* object = base;
      if (m->op == P::Arrow || m->op == P::SafeArrow)
        object = objectForArrow(base);
      if (!object || !object->isKnown())
      {
        ++stats.membersUnknownBase;
        ++stats.unknownBaseMembers[symbol];
        return unknown(Why::Base);
      }
      if (const Ty* descriptor = types.stripRef(object); descriptor->kind == TyKind::Member)
      {
        // A reflection descriptor (`for constexpr (auto m : members(T))`): m.name, m.offset, m.type
        // (written as a type: `a.type value;`, read as decltype(a.type)); an alternative's
        // (`alternatives(U)`) also a.tag, its U::Tag value.
        m->resolved = nullptr;
        ++stats.membersBound;
        std::string_view which = symbols.name(symbol);
        if (which == "name")
          return types.pointer(types.qualified(types.charType(), true, false));
        if (which == "type")
          return descriptor->inner ? descriptor->inner : types.unknown();
        size_t index = 0;
        if (ClassDecl* u = alternativeOf(descriptor, index))
          return which == "tag" && u->tagEnum ? impl.tagTypeOf(u, descriptor->args.empty() ? nullptr : descriptor->args[0]) : unknown(Why::Member);
        if (which == "offset")
          return types.sizeType();
        return unknown(Why::Member);
      }
      MemberResult r = memberOf(object, symbol, &m->member, m->token);
      m->resolved = r.decl;
      if (r.decl)
      {
        if (r.type && types.isDependent(r.type))
          ++stats.membersDependent;
        else
          ++stats.membersBound;
      }
      else if (r.why == Why::Base)
        ++stats.membersUnknownBase;
      if (r.why != Why::None)
        return unknown(r.why);
      if (r.type && r.type->kind == TyKind::Function && last.special == 0)
      {
        // `obj.method` used as a value is only meaningful in a call, which handles overloads.
      }
      // A data member of a const object is const (`rectangle.data.begin()` with `const Rect&
      // rectangle` calls the const begin()), unless it is mutable, static or a reference.
      if (r.type && r.decl && r.decl->kind == NodeKind::VariableDecl && !(r.decl->flags & (DF_Static | DF_Mutable | DF_Typedef)) &&
          !r.type->isReference() && r.type->kind != TyKind::Array && r.type->isKnown() && !r.type->isConst)
        if (const Ty* obj = types.stripRef(object); obj && obj->isConst)
          return types.qualified(r.type, true, r.type->isVolatile);
      return r.type ? r.type : types.unknown();
    }

    // --- calls ---

    // Whether target is cls or one of its bases, directly or not.
    bool reachesClassScope(Scope* cls, Scope* target, int depth)
    {
      if (cls == target)
        return true;
      if (depth > 16 || cls->kind != ScopeKind::Class || cls->baseTypes.empty())
        return false;
      impl.ensureBases(cls, depth);
      for (Scope* b : cls->baseScopes)
        if (reachesClassScope(b, target, depth + 1))
          return true;
      return false;
    }

    // Picks a function among overloads by argument count and conversion ranking.
    // An ambiguous call (tied: one of each function no other beats), a checker error when the
    // types are concrete (a template's own walk decides nothing; its instances do), C++'s
    // `call is ambiguous`; behind Impl::ambiguousCallsAreErrors.
    void reportAmbiguousCall(CallExpr* c, const NamePart& name, const std::vector<const Ty*>& argTypes, const std::vector<FunctionDecl*>& tied,
                             const Subst* classSubst)
    {
      if (!Impl::ambiguousCallsAreErrors || tied.size() < 2 || inGenericTemplate() || probing)
        return;
      for (const Ty* a : argTypes)
        if (!a || !a->isKnown() || types.isDependent(a) || Impl::mentionsUnknown(a))
          return;
      std::vector<std::string> candidates;
      for (FunctionDecl* f : tied)
      {
        const Ty* ft = impl.declaredTypeOf(f, classSubst, 0);
        // (a function template's type names its own parameters: `int32(T)` beside `int32(T&)` is
        // as ambiguous, C++'s partial ordering finding neither more specialised)
        if (!ft || (types.isDependent(ft) && f->templateParams.empty()))
          return;
        candidates.push_back(types.toString(ft, &impl.symbols) + " (" + Impl::locationOf(f->file, f->token) + ")");
      }
      std::sort(candidates.begin(), candidates.end());
      std::string message = "the call of " + impl.namePartText(name) + " with (";
      for (size_t i = 0; i < argTypes.size(); ++i)
        message += (i ? ", " : "") + types.toString(argTypes[i], &impl.symbols);
      message += ") is ambiguous between " + candidates[0];
      for (size_t i = 1; i < candidates.size(); ++i)
        message += (i + 1 == candidates.size() ? " and " : ", ") + candidates[i];
      impl.reportCheckerError(file, c->token, message);
    }

    FunctionDecl* pickOverload(Decl* primary, const std::vector<Entry>* overloads, const std::vector<const Ty*>& argTypes,
                               const Subst* classSubst, const Ty* objectTy, bool& ambiguous, const NamePart* namePart = nullptr,
                               Span<Expr*> callArgs = {}, std::vector<FunctionDecl*>* tied = nullptr)
    {
      std::vector<FunctionDecl*> candidates;
      if (overloads)
      {
        for (const Entry& e : *overloads)
          if (e.decl && e.decl->kind == NodeKind::FunctionDecl)
            candidates.push_back(static_cast<FunctionDecl*>(e.decl));
      }
      else if (primary && primary->kind == NodeKind::FunctionDecl)
        candidates.push_back(static_cast<FunctionDecl*>(primary));
      // The arguments that are the literal 0, a null pointer for a pointer parameter.
      std::vector<bool> nullConstants;
      bool anyNull = false;
      for (Expr* a : callArgs)
      {
        Expr* x = a;
        while (x && x->kind == NodeKind::ParenExpr)
          x = static_cast<ParenExpr*>(x)->inner;
        bool zero = false;
        if (x && x->kind == NodeKind::LiteralExpr && static_cast<LiteralExpr*>(x)->literalKind == TokenKind::Number)
        {
          std::string_view text = spelling(static_cast<LiteralExpr*>(x));
          zero = text == "0" || text == "0u" || text == "0U" || text == "0l" || text == "0L" || text == "0ll" || text == "0LL" || text == "0ull" ||
                 text == "0ULL";
        }
        nullConstants.push_back(zero);
        anyNull = anyNull || zero;
      }
      const std::vector<bool>* nulls = anyNull ? &nullConstants : nullptr;
      if (!namePart && !callSets)
        return impl.chooseAmong(candidates, argTypes, classSubst, objectTy, ambiguous, nullptr, nulls, tied);
      // The explicit template arguments (`load<bool>()`): a candidate with fewer parameters than
      // the list names (`typeIndex<T, Tail...>` with Tail known), and no pack to take the rest,
      // is not one, nor one whose parameter is of another kind than its argument. An overload set
      // among the arguments: only a candidate that takes one of its functions (takesCallSets).
      std::function<bool(FunctionDecl*, Subst&)> bind = [&](FunctionDecl* f, Subst& s)
      {
        if (!takesCallSets(f, s))
          return false;
        if (!namePart || !namePart->hasTemplateArgs || namePart->templateArgs.empty())
          return true;
        size_t n = f->templateParams.size();
        size_t before = s.pairs.size();
        size_t given = bindExplicitArgs(f, *namePart, callArgs, s);
        if (given > n && !(n && static_cast<TemplateParamDecl*>(f->templateParams[n - 1])->isPack))
          return false;
        for (size_t i = before; i < s.pairs.size(); ++i)
          if (!argumentKindFits(s.pairs[i].type, static_cast<TemplateParamDecl*>(s.pairs[i].param)))
            return false; // `create<Abs>` names no `template<VoronoiType TYPE> create`
        return true;
      };
      return impl.chooseAmong(candidates, argTypes, classSubst, objectTy, ambiguous, &bind, nulls, tied);
    }

    // Argument types of a call; a pack expansion `args...` contributes one type per element.
    // Lvalues present themselves as lvalue references (argumentType).
    // The parameter pack an expansion pattern expands (the first pack parameter named in it), or null.
    Decl* packParamIn(Node* n, int depth = 0)
    {
      if (!n || depth > 32)
        return nullptr;
      switch (n->kind)
      {
        case NodeKind::NameExpr:
        {
          // Before the walk resolved the name (an instance's copy, typed the first time), the
          // parameter it names among the locals.
          auto* name = static_cast<NameExpr*>(n);
          Decl* d = name->resolved;
          if (!d && name->name.parts.size() == 1 && !name->name.parts[0].hasTemplateArgs)
            d = locals.find(name->name.parts[0].symbol);
          return d && d->kind == NodeKind::ParamDecl && static_cast<ParamDecl*>(d)->isPack ? d : nullptr;
        }
        case NodeKind::ParenExpr: return packParamIn(static_cast<ParenExpr*>(n)->inner, depth + 1);
        case NodeKind::UnaryExpr: return packParamIn(static_cast<UnaryExpr*>(n)->operand, depth + 1);
        case NodeKind::CastExpr: return packParamIn(static_cast<CastExpr*>(n)->operand, depth + 1);
        case NodeKind::NamedCastExpr: return packParamIn(static_cast<NamedCastExpr*>(n)->operand, depth + 1);
        case NodeKind::MemberExpr: return packParamIn(static_cast<MemberExpr*>(n)->base, depth + 1);
        case NodeKind::CallExpr:
        {
          auto* c = static_cast<CallExpr*>(n);
          if (Decl* d = packParamIn(c->callee, depth + 1))
            return d;
          for (Expr* a : c->args)
            if (Decl* d = packParamIn(a, depth + 1))
              return d;
          return nullptr;
        }
        default: return nullptr;
      }
    }

    // `f(forward<Args>(args)...)` under an instantiation with an empty Args: no argument at all.
    bool isEmptyExpansion(Expr* a)
    {
      if (!a || a->kind != NodeKind::UnaryExpr || static_cast<UnaryExpr*>(a)->isKeyword ||
          P(static_cast<UnaryExpr*>(a)->op) != P::Ellipsis || !static_cast<UnaryExpr*>(a)->postfix)
        return false;
      Decl* pd = packParamIn(static_cast<UnaryExpr*>(a)->operand);
      if (!pd)
        return false;
      const Ty* pt = impl.declaredTypeOf(pd, activeSubst, 0);
      return pt && pt->kind == TyKind::Pack && pt->args.empty();
    }

    std::vector<const Ty*> argTypesOf(const Span<Expr*>& args)
    {
      bool walked = walkedArgs && args.data == walkedArgs && args.size() == walkedArgTypes.size();
      std::vector<const Ty*> out;
      for (size_t i = 0; i < args.size(); ++i)
      {
        Expr* a = args[i];
        if (isEmptyExpansion(a))
          continue;
        const Ty* t = walked ? walkedArgTypes[i] : a ? a->ty : nullptr;
        if (a && t && t->kind == TyKind::Pack)
          for (const Ty* e : t->args)
            out.push_back(argumentType(a, e, types));
        else if (a && a->kind == NodeKind::BraceInitExpr)
          out.push_back(braceListType(a, impl.initializerListDecl(), types));
        else
          out.push_back(a ? argumentType(a, t, types) : nullptr);
      }
      return out;
    }

    // The argument list of the call being typed and the types this walk gave its expressions, which
    // argTypesOf uses for it instead of the types on the AST nodes: the AST is shared, and another
    // thread checking another instantiation of the same body may have rewritten them meanwhile.
    struct WalkedArgs
    {
      Walker& w;
      Expr* const* savedArgs;
      std::vector<const Ty*> savedTypes;
      WalkedArgs(Walker& w, const Span<Expr*>& args, std::vector<const Ty*>&& types)
        : w(w), savedArgs(w.walkedArgs), savedTypes(std::move(w.walkedArgTypes))
      {
        w.walkedArgs = args.data;
        w.walkedArgTypes = std::move(types);
      }
      ~WalkedArgs()
      {
        w.walkedArgs = savedArgs;
        w.walkedArgTypes = std::move(savedTypes);
      }
    };

    // The explicit template arguments of a call (`count<int, int, int>()`): one per parameter;
    // a trailing pack takes the rest, and is empty when the list leaves it nothing and there is
    // no call argument to deduce it from. A pack expansion in the list (`load<V, Z, Args...>`)
    // gives one argument per element of the pack, none for an empty one. Returns how many
    // arguments the list names: an expansion still dependent counts as none, since it may name
    // none.
    size_t bindExplicitArgs(Span<Decl*> templateParams, const NamePart& namePart, Span<Expr*> args, Subst& s, bool emptyPackWithoutArgs = true)
    {
      size_t n = templateParams.size(), named = 0;
      std::vector<const Ty*> given;
      // A parameter pack takes the written arguments from its position on, wherever it is
      // (`template<typename... Ts, typename InputT, typename... Args> dummyLoad(InputT&, Args&&...)`
      // called `dummyLoad<char, int>(input)`): the parameters after it are deduced.
      size_t firstPack = n;
      for (size_t i = 0; i < n; ++i)
        if (static_cast<TemplateParamDecl*>(templateParams[i])->isPack)
        {
          firstPack = i;
          break;
        }
      for (Node* written : namePart.templateArgs)
      {
        Decl* param = n ? templateParams[std::min(std::min(given.size(), n - 1), firstPack)] : nullptr;
        const Ty* t = explicitArgument(written, param);
        bool expansion = written && written->kind == NodeKind::PackType;
        if (expansion && t && t->kind == TyKind::Pack)
        {
          given.insert(given.end(), t->args.begin(), t->args.end());
          named += t->args.size();
        }
        else
        {
          given.push_back(t);
          named += !expansion;
        }
      }
      if (n == 0)
        return named;
      for (size_t i = 0; i < given.size() && i < n; ++i)
      {
        auto* param = static_cast<TemplateParamDecl*>(templateParams[i]);
        if (param->isPack)
        {
          std::vector<const Ty*> rest(given.begin() + i, given.end());
          s.pairs.push_back({param, types.pack(Span<const Ty*>{rest.data(), uint32_t(rest.size())})});
          return named;
        }
        s.pairs.push_back({param, given[i]});
      }
      if (namePart.hasTemplateArgs && args.empty() && emptyPackWithoutArgs)
      {
        auto* last = static_cast<TemplateParamDecl*>(templateParams[n - 1]);
        if (last->isPack && !s.find(last))
          s.pairs.push_back({last, types.pack({})}); // `count<int>()`: nothing left for Args
      }
      return named;
    }
    size_t bindExplicitArgs(FunctionDecl* f, const NamePart& namePart, Span<Expr*> args, Subst& s, bool emptyPackWithoutArgs = true)
    {
      return bindExplicitArgs(f->templateParams, namePart, args, s, emptyPackWithoutArgs);
    }

    // Whether an explicit template argument is of its parameter's kind: a type for a type
    // parameter, a constant for a constant one. One not known yet, or dependent on the
    // enclosing template in a way that does not tell, fits.
    bool argumentKindFits(const Ty* t, const TemplateParamDecl* p)
    {
      if (!t || !t->isKnown() || !p->innerParams.empty())
        return true; // template template parameters take templates: not checked here
      if (t->kind == TyKind::Pack)
      {
        for (const Ty* e : t->args)
          if (!argumentKindFits(e, p))
            return false;
        return true;
      }
      bool constantParam = t->kind == TyKind::Param && t->decl && !static_cast<TemplateParamDecl*>(t->decl)->isType;
      if (p->isType)
        return t->kind != TyKind::Value && !constantParam;
      return t->kind == TyKind::Value || constantParam || t->kind == TyKind::DependentMember; // `T::value` may be either
    }

    // An explicit template argument of a call (`forward<Args>(args)`): under an instantiation that
    // binds the pack Args to one element, a non-pack parameter of the callee gets that element,
    // not a pack of one. An expansion (`typeIndex<T, Tail...>`) stays the pack it expands.
    const Ty* explicitArgument(Node* written, Decl* param)
    {
      const Ty* t = impl.templateArgument(written, param, ctx(), 0);
      bool packParam = param && param->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(param)->isPack;
      if (t && t->kind == TyKind::Pack && t->args.size() == 1 && !packParam && !(written && written->kind == NodeKind::PackType))
        return t->args[0];
      return t;
    }

    // Records f's deduced template arguments on the call for the code generator: built aside and
    // stored with the count last, since instantiations checked on other threads type the same call
    // (the count was incremented in place and could pass the 8 slots).
    static void recordTemplateArgs(CallExpr* call, FunctionDecl* f, const Subst& s)
    {
      const Ty* found[8] = {};
      uint8_t n = 0;
      for (Decl* p : f->templateParams)
      {
        const Ty* t = s.find(p);
        if (!t || n >= 8)
          break;
        found[n++] = t;
      }
      for (uint8_t i = 0; i < n; ++i)
        call->templateArgs[i] = found[i];
      call->templateArgCount = n;
    }

    // Return type of calling `f` with the given arguments, deducing its template parameters. An
    // operator expression has no argument list: its operand types (operands) are deduced from.
    const Ty* returnTypeOfCall(FunctionDecl* f, const Subst* classSubst, const Span<Expr*>& args, const NamePart* namePart,
                               CallExpr* call = nullptr, const std::vector<const Ty*>* operands = nullptr)
    {
      auto argTypes = [&] { return operands ? *operands : argTypesOf(args); };
      if (f->body == nullptr && f->returnType == nullptr && !(f->flags & (DF_Constructor | DF_Destructor | DF_Conversion)))
        return unknown(Why::Unsupported);
      bool autoReturn = false;
      if (f->returnType && Impl::astMentionsAuto(f->returnType) && !f->trailingReturn)
        autoReturn = true; // `auto`, `auto&`, `const auto*`: the body decides
      if (autoReturn)
      {
        if (!f->templateParams.empty())
        {
          Subst s = classSubst ? *classSubst : Subst{};
          const Ty* fnTy = impl.declaredTypeOf(f, classSubst, 0);
          if (namePart)
            bindExplicitArgs(f, *namePart, args, s);
          impl.deduceCall(f, fnTy, argTypes(), s);
          impl.fillDefaultTemplateArgs(f, s);
          if (call)
            recordTemplateArgs(call, f, s);
          if (const Ty* r = instantiateFunction(f, s, true, classSubst))
            return r;
          return unknown(Why::AutoReturn);
        }
        // The body's deduction through the function's one instance (checked once, whoever asks
        // first), never through what its own file's walk cached: that was there or not by which
        // file was walked first, and the instance walked or not with it.
        if (const Ty* r = instantiateFunction(f, Subst{}, true, classSubst))
          return r;
        return unknown(Why::AutoReturn);
      }
      Subst subst = classSubst ? *classSubst : Subst{};
      // The callee's type for deduction: the enclosing instantiation's arguments apply, but never
      // to the callee's own parameters (a template calling itself, `sum(rest...)` inside
      // `sum<int, int, int>`, deduces them afresh from the arguments).
      Subst outer;
      const Subst* forType = classSubst ? classSubst : activeSubst;
      if (!classSubst && activeSubst && !f->templateParams.empty())
      {
        for (const Subst::Pair& p : activeSubst->pairs)
        {
          bool own = false;
          for (Decl* tp : f->templateParams)
            own = own || tp == p.param;
          if (!own)
            outer.pairs.push_back(p);
        }
        forType = outer.pairs.empty() ? nullptr : &outer;
      }
      const Ty* fnTy = impl.declaredTypeOf(f, forType, 0);
      if (!f->templateParams.empty())
      {
        // Explicit template arguments first, then deduction from the arguments.
        if (namePart)
          bindExplicitArgs(f, *namePart, args, subst);
        impl.deduceCall(f, fnTy, argTypes(), subst);
        impl.fillDefaultTemplateArgs(f, subst);
        if (call)
          recordTemplateArgs(call, f, subst);
        {
          static bool debugCalls = getenv("KPP_DEBUG_CALLS") != nullptr;
          if (debugCalls)
          {
            std::string line = std::string(call ? "call " : "call(no expr) ") + impl.debugName(f->name.last().symbol) + ":";
            for (size_t i = 0; call && i < call->templateArgCount; ++i)
              line += " " + types.toString(call->templateArgs[i], &impl.symbols);
            line += " | args:";
            for (const Ty* a : argTypes())
              line += " " + types.toString(a, &impl.symbols);
            line += " | explicit:";
            if (namePart)
              for (size_t i = 0; i < namePart->templateArgs.size() && i < f->templateParams.size(); ++i)
                line += " " + types.toString(impl.templateArgument(namePart->templateArgs[i], f->templateParams[i], ctx(), 0), &impl.symbols);
            line += " | subst:";
            if (activeSubst)
              for (auto& [d, ty] : activeSubst->pairs)
                line += " " + impl.debugName(static_cast<TemplateParamDecl*>(d)->name) + "=" + types.toString(ty, &impl.symbols);
            fprintf(stderr, "%s\n", line.c_str());
          }
        }
        const Ty* result = impl.substitute(fnTy->inner, subst);
        Type* written = f->trailingReturn ? f->trailingReturn : f->returnType;
        LocalStack own;
        Impl::pushEnclosingTemplateParams(f, own);
        if ((f->trailingReturn && Impl::astMentionsDecltype(f->trailingReturn)) || impl.astNeedsArguments(written, impl.lexicalScopeOf(f), &own))
        {
          // `-> Future<decltype(callable())>`, `typename Conditional<(sizeof(A) < sizeof(B)), B,
          // A>::Type`: typed again with the deduced arguments, since the generic type substituted
          // keeps neither what calling the closure gives nor which specialisation the condition picks.
          Subst all = subst;
          if (forType)
            for (const Subst::Pair& p : forType->pairs)
              if (!all.find(p.param))
                all.pairs.push_back(p);
          if (const Ty* full = impl.declaredTypeOf(f, &all, 0); full && full->kind == TyKind::Function && full->inner->isKnown())
            result = full->inner;
        }
        if (impl.dependsOn(result, f->templateParams))
          return unknown(Why::Deduction);
        if (!types.isDependent(result))
          instantiateFunction(f, subst, false, classSubst);
        return result;
      }
      return fnTy->inner;
    }

    // l, an unqualified call's lookup, with the functions of the name that the namespaces
    // associated with the argument types declare (Impl::associatedNamespaces) added, and the
    // friend functions their classes define (Impl::hiddenFriendLookup), whose classes' template
    // arguments go into friendArgs.
    void addArgumentDependentCandidates(Lookup& l, uint32_t symbol, const std::vector<const Ty*>& argTypes, Subst& friendArgs, uint32_t token)
    {
      std::vector<Scope*> namespaces;
      for (const Ty* a : argTypes)
        impl.associatedNamespaces(a, namespaces);
      for (Scope* ns : namespaces)
      {
        if (ns == l.foundIn)
          continue;
        Lookup found;
        if (!impl.lookupInScope(ns, symbol, found, 0, false) || !found.decl || found.decl->kind != NodeKind::FunctionDecl || found.foundIn != ns)
          continue;
        l = l.found == Found::No ? found : impl.mergeOverloads(l, found);
      }
      std::string conflict;
      Lookup friends = impl.hiddenFriendLookup(symbol, argTypes, friendArgs, &conflict);
      if (!conflict.empty())
        reportHere(token, conflict);
      if (friends.decl)
        l = l.found == Found::No ? friends : impl.mergeOverloads(l, friends);
    }

    // An error of the code being walked: in an instance, said with the instance; in a template's
    // own walk, left to its instances.
    void reportHere(uint32_t token, const std::string& message)
    {
      if (!file || file->isImported || probing)
        return;
      if (activeSubst)
        reportInInstance(token, message);
      else if (!inGenericTemplate())
        impl.reportCheckerError(file, token, message);
    }

    // Whether d is the only function of its name there (overloads: the entries found with it)
    // and no template.
    static bool onlyPlainCandidate(Decl* d, const std::vector<Entry>* overloads)
    {
      if (!d || d->kind != NodeKind::FunctionDecl || !d->templateParams.empty())
        return false;
      size_t functions = 0;
      if (overloads)
        for (const Entry& e : *overloads)
          functions += e.decl->kind == NodeKind::FunctionDecl;
      return functions <= 1;
    }

    // Whether e, walked (under parentheses and a `&`), names a function template without template
    // arguments, or several functions: with no target type to choose one by, C++ has no expression
    // there ([over.over]), so a requirement on it is not satisfied (`requires() { &T::f; }` with
    // T's f a member template it inherits: LuaBinder's luaDebugChildren). Nor is a non-static
    // member function an object's member names without calling it (`requires(T t) { t.f; }`).
    bool namesFunctionsWithoutTarget(Expr* e)
    {
      while (e && e->kind == NodeKind::ParenExpr)
        e = static_cast<ParenExpr*>(e)->inner;
      if (e && e->kind == NodeKind::UnaryExpr && !static_cast<UnaryExpr*>(e)->isKeyword && !static_cast<UnaryExpr*>(e)->postfix &&
          P(static_cast<UnaryExpr*>(e)->op) == P::Amp)
        e = static_cast<UnaryExpr*>(e)->operand;
      while (e && e->kind == NodeKind::ParenExpr)
        e = static_cast<ParenExpr*>(e)->inner;
      if (e && e->kind == NodeKind::MemberExpr)
      {
        // `t.f` not called: a non-static member function is no expression unless called
        // ([expr.ref]); static ones as a name of them.
        auto* m = static_cast<MemberExpr*>(e);
        if (!m->resolved || m->resolved->kind != NodeKind::FunctionDecl || m->member.empty())
          return false;
        if (!(m->resolved->flags & DF_Static))
          return true;
        if (m->member.last().hasTemplateArgs)
          return false;
        const Ty* object = m->base && m->base->ty ? types.unqualified(types.stripRef(m->base->ty)) : nullptr;
        if (object && m->op == P::Arrow)
          object = object->kind == TyKind::Pointer && object->inner ? types.unqualified(object->inner) : nullptr;
        if (!object || object->kind != TyKind::Class)
          return false;
        MemberResult r = memberOf(object, m->member.last().symbol, nullptr, m->token, false);
        return r.decl && r.decl->kind == NodeKind::FunctionDecl && !onlyPlainCandidate(r.decl, r.overloads);
      }
      if (!e || e->kind != NodeKind::NameExpr)
        return false;
      auto* n = static_cast<NameExpr*>(e);
      if (!n->resolved || n->resolved->kind != NodeKind::FunctionDecl || n->name.empty() || n->name.last().hasTemplateArgs)
        return false;
      Lookup l = impl.lookupQualified(n->name, scope, &locals, 0, activeSubst);
      if (l.found == Found::No && n->name.parts.size() > 1)
        l = memberOfQualifierInstance(n);
      return l.decl && l.decl->kind == NodeKind::FunctionDecl && !onlyPlainCandidate(l.decl, l.overloads);
    }

    // An argument naming several functions, alone or under `&` (`Expression::computeConstant` with
    // an overload for one constant and one for a Vector of them): C++ takes the overload whose type
    // the parameter names, once the callee is chosen, so meanwhile the argument decides nothing.
    struct OverloadSetArgument
    {
      size_t index;
      NameExpr* name;
      Expr* address; // the `&name` around it, or null
      Lookup set;
    };
    const std::vector<OverloadSetArgument>* callSets = nullptr; // the overload sets among the arguments of the call being typed

    // Whether a function of overload set `set` has exactly the function type target (what a
    // pointer to it as the argument needs: C++ [over.over]).
    bool setHasFunctionOf(const OverloadSetArgument& set, const Ty* target)
    {
      Subst qs = set.name->name.parts.size() > 1 ? impl.qualifierSubst(set.name->name, ctx(), 0, set.set.foundIn) : Subst{};
      for (const Entry& e : *set.set.overloads)
        if (e.decl && e.decl->kind == NodeKind::FunctionDecl && e.decl->templateParams.empty() &&
            impl.declaredTypeOf(e.decl, qs.empty() ? activeSubst : &qs, 0) == target)
          return true;
      return false;
    }

    // Whether candidate f (its class's and explicit arguments in s) can take each overload set
    // among the call's arguments: a parameter that is a pointer to a function or a member function
    // of a type no function of the set has makes it no candidate. One of a type still dependent
    // (`template<class F> add(int, F)`) is decided when the set is bound.
    bool takesCallSets(FunctionDecl* f, Subst& s)
    {
      if (!callSets)
        return true;
      const Ty* ft = impl.declaredTypeOf(f, s.pairs.empty() ? nullptr : &s, 0);
      if (!ft || ft->kind != TyKind::Function)
        return true;
      for (const OverloadSetArgument& set : *callSets)
      {
        if (set.index >= ft->args.size() || !set.set.overloads)
          continue;
        const Ty* pt = types.unqualified(types.stripRef(ft->args[set.index]));
        const Ty* target = pt && (pt->kind == TyKind::Pointer || pt->kind == TyKind::MemberPointer) ? pt->inner : pt;
        if (!target || target->kind != TyKind::Function || types.isDependent(target))
          continue;
        if (!setHasFunctionOf(set, target))
          return false;
      }
      return true;
    }

    bool overloadSetArgument(Expr* a, size_t index, OverloadSetArgument& out)
    {
      while (a && a->kind == NodeKind::ParenExpr)
        a = static_cast<ParenExpr*>(a)->inner;
      Expr* address = nullptr;
      if (a && a->kind == NodeKind::UnaryExpr && !static_cast<UnaryExpr*>(a)->isKeyword && !static_cast<UnaryExpr*>(a)->postfix &&
          P(static_cast<UnaryExpr*>(a)->op) == P::Amp)
      {
        address = a;
        a = static_cast<UnaryExpr*>(a)->operand;
      }
      if (!a || a->kind != NodeKind::NameExpr)
        return false;
      auto* n = static_cast<NameExpr*>(a);
      if (!n->resolved || n->resolved->kind != NodeKind::FunctionDecl || n->name.empty())
        return false;
      Lookup l = impl.lookupQualified(n->name, scope, &locals, 0, activeSubst);
      if (l.found == Found::Global)
        l = impl.withNamespaceUsings(l, 0);
      size_t functions = 0;
      if (l.overloads)
        for (const Entry& e : *l.overloads)
          functions += e.decl && e.decl->kind == NodeKind::FunctionDecl && e.decl->templateParams.empty();
      if (functions < 2)
        return false;
      out = OverloadSetArgument{index, n, address, l};
      return true;
    }

    // `construct<Neg>`, a function template named with some of its arguments (or none), where a
    // pointer to a function of a known type is wanted: its other arguments, a trailing pack's
    // included, deduced from that type's parameters as from a call's, and the name typed as that
    // instance's function (which the code generator takes the address of).
    void bindTemplateAddress(Expr* a, const Ty* target)
    {
      Expr* named = a;
      while (named && named->kind == NodeKind::ParenExpr)
        named = static_cast<ParenExpr*>(named)->inner;
      Expr* address = nullptr;
      if (named && named->kind == NodeKind::UnaryExpr && !static_cast<UnaryExpr*>(named)->isKeyword && P(static_cast<UnaryExpr*>(named)->op) == P::Amp)
      {
        address = named;
        named = static_cast<UnaryExpr*>(named)->operand;
      }
      if (!named || named->kind != NodeKind::NameExpr || !target)
        return;
      auto* n = static_cast<NameExpr*>(named);
      if (!n->resolved || n->resolved->kind != NodeKind::FunctionDecl || n->name.empty())
        return;
      auto* f = static_cast<FunctionDecl*>(n->resolved);
      if (f->templateParams.empty() && n->name.last().hasTemplateArgs)
      {
        // `constructExpression<BitwiseNot>` beside a constructExpression(double) that is no
        // template: the written arguments name the template (the only one of the name).
        Lookup l = impl.lookupQualified(n->name, scope, &locals, 0, activeSubst);
        FunctionDecl* only = nullptr;
        int templates = 0;
        if (l.overloads)
          for (const Entry& entry : *l.overloads)
            if (entry.decl && entry.decl->kind == NodeKind::FunctionDecl && !entry.decl->templateParams.empty())
            {
              only = static_cast<FunctionDecl*>(entry.decl);
              ++templates;
            }
        if (templates != 1)
          return;
        f = only;
      }
      if (f->templateParams.empty())
      {
        // `AreaFn f = Shape::area;` beside an area(int): of the name's functions, the one of the pointer's type, as for
        // an argument (bindOverloadSets).
        if (OverloadSetArgument set; !n->name.last().hasTemplateArgs && overloadSetArgument(a, 0, set))
          bindOverloadSet(set, target);
        return;
      }
      const Ty* pt = types.unqualified(types.stripRef(target));
      const Ty* want = pt && pt->kind == TyKind::Pointer ? pt->inner : pt;
      if (!want || want->kind != TyKind::Function || !want->isKnown() || types.isDependent(want))
        return;
      Subst s;
      bindExplicitArgs(f, n->name.last(), Span<Expr*>{}, s, false);
      if (activeSubst)
        for (const Subst::Pair& p : activeSubst->pairs)
          if (!s.find(p.param))
            s.pairs.push_back(p); // the enclosing instance's (a member of a class template)
      const Ty* fnTy = impl.declaredTypeOf(f, &s, 0);
      std::vector<const Ty*> argTypes(want->args.begin(), want->args.end());
      impl.deduceCall(f, fnTy, argTypes, s);
      for (Decl* p : f->templateParams)
        if (!s.find(p))
        {
          if (!static_cast<TemplateParamDecl*>(p)->isPack)
            return; // not deduced: left as typed
          s.pairs.push_back({p, types.pack({})});
        }
      const Ty* instance = impl.declaredTypeOf(f, &s, 0);
      if (!instance || instance->kind != TyKind::Function || !instance->isKnown() || types.isDependent(instance))
        return;
      n->resolved = f;
      record(n, instance);
      if (address)
        record(address, types.pointer(instance));
    }

    // The function template names among what initializes an object of type target, bound to the
    // function pointer types they initialize (bindTemplateAddress): through brace lists, an
    // InitializerList's elements, an array's and an aggregate's members in order
    // (`InitializerList<OperationTestCase<...>>{{"abs", constructExpression<Abs>, ...}}`).
    void bindTemplateAddresses(Expr* e, const Ty* target, int depth = 0)
    {
      if (!e || !target || depth > 8)
        return;
      const Ty* t = types.unqualified(types.stripRef(target));
      if (e->kind != NodeKind::BraceInitExpr)
      {
        bindTemplateAddress(e, t);
        return;
      }
      auto* list = static_cast<BraceInitExpr*>(e);
      if (!list->designators.empty())
        return;
      if (t->kind == TyKind::Array && t->inner)
      {
        for (Expr* a : list->args)
          bindTemplateAddresses(a, t->inner, depth + 1);
        return;
      }
      if (t->kind != TyKind::Class || !t->isKnown() || types.isDependent(t))
        return;
      if (impl.isInitializerList(t))
      {
        if (!t->args.empty())
          for (Expr* a : list->args)
            bindTemplateAddresses(a, t->args[0], depth + 1);
        return;
      }
      Scope* s = impl.classScopeOf(t);
      if (!s || !s->owner || s->owner->kind != NodeKind::ClassDecl)
        return;
      auto* cls = static_cast<ClassDecl*>(s->owner);
      Subst subst = impl.substFor(t);
      // A class with constructors: the one constructor (copy and move ones aside) whose parameters
      // the list's elements can fill, its parameter types (`OperationTestCase(const char* name,
      // ExpressionBuilder expressionBuilder, ...)`); several or none decide nothing here.
      FunctionDecl* taker = nullptr;
      int takers = 0;
      bool constructors = false;
      for (Decl* m : membersRead(cls))
      {
        if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Constructor) || (m->flags & DF_Delete))
          continue;
        constructors = true;
        auto* f = static_cast<FunctionDecl*>(m);
        size_t required = 0;
        for (Decl* p : f->params)
          required += !static_cast<ParamDecl*>(p)->defaultValue && !static_cast<ParamDecl*>(p)->isPack;
        if (f->params.size() < list->args.size() || required > list->args.size() || !f->templateParams.empty())
          continue;
        if (f->params.size() == 1)
          if (const Ty* only = impl.declaredTypeOf(f->params[0], subst.empty() ? nullptr : &subst, 0);
              only && types.unqualified(types.stripRef(only)) == t)
            continue; // the copy or move constructor
        taker = f;
        ++takers;
      }
      if (constructors)
      {
        if (takers != 1)
          return;
        for (size_t i = 0; i < list->args.size(); ++i)
          bindTemplateAddresses(list->args[i], impl.declaredTypeOf(taker->params[i], subst.empty() ? nullptr : &subst, 0), depth + 1);
        return;
      }
      if (!cls->bases.empty())
        return;
      size_t i = 0;
      for (Decl* m : membersRead(cls))
      {
        if (m->kind != NodeKind::VariableDecl || (m->flags & (DF_Static | DF_Typedef | DF_Friend)))
          continue;
        if (i >= list->args.size())
          break;
        bindTemplateAddresses(list->args[i++], impl.declaredTypeOf(m, subst.empty() ? nullptr : &subst, 0), depth + 1);
      }
    }

    // Binds overload set s to its overload of the type that pt, what it initializes, takes: a function pointer's or
    // reference's function, or a member pointer's (C++ [over.over]). Several or none of that type leave it as it is.
    void bindOverloadSet(const OverloadSetArgument& s, const Ty* pt)
    {
      pt = pt ? types.unqualified(types.stripRef(pt)) : nullptr;
      const Ty* target = pt && (pt->kind == TyKind::Pointer || pt->kind == TyKind::MemberPointer) ? pt->inner : pt;
      if (!target || target->kind != TyKind::Function || types.isDependent(target))
        return;
      Subst qs = s.name->name.parts.size() > 1 ? impl.qualifierSubst(s.name->name, ctx(), 0, s.set.foundIn) : Subst{};
      FunctionDecl* chosen = nullptr;
      const Ty* chosenTy = nullptr;
      bool several = false;
      for (const Entry& e : *s.set.overloads)
      {
        if (!e.decl || e.decl->kind != NodeKind::FunctionDecl || !e.decl->templateParams.empty())
          continue;
        const Ty* ot = impl.declaredTypeOf(e.decl, qs.empty() ? activeSubst : &qs, 0);
        if (ot != target)
          continue;
        several = several || chosen;
        chosen = static_cast<FunctionDecl*>(e.decl);
        chosenTy = ot;
      }
      if (!chosen || several || chosen == s.name->resolved)
        return;
      s.name->resolved = chosen;
      record(s.name, chosenTy);
      if (s.address && s.address->ty)
      {
        const Ty* was = s.address->ty;
        if (was->kind == TyKind::MemberPointer && !was->args.empty())
          record(s.address, types.memberPointer(chosenTy, was->args[0]));
        else if (was->kind == TyKind::Pointer)
          record(s.address, types.pointer(chosenTy));
      }
    }

    // Binds each overload-set argument to the overload of the type the chosen callee's parameter
    // takes (fnTy: the callee's function type), as bindOverloadSet does.
    void bindOverloadSets(const std::vector<OverloadSetArgument>& sets, const Ty* fnTy)
    {
      if (!fnTy || fnTy->kind != TyKind::Function)
        return;
      for (const OverloadSetArgument& s : sets)
        if (s.index < fnTy->args.size())
          bindOverloadSet(s, fnTy->args[s.index]);
    }

    // A class value where a scalar is wanted, an argument for a parameter that is no class or a copy-initialized
    // variable's initializer (`takeInt(item)`, `Item* raw = item;` with a UniquePtr<Item>): C++ converts it only through
    // a conversion operator of the class. Without any it can take the checker's error, where the code generator stopped
    // at it as at a construct not supported yet. What the code generator takes is never one: a non-explicit conversion
    // operator to a type that is no class, an explicit one for a bool (hasScalarConversion), a lambda without captures as
    // a pointer to function. Only in code walked where it is written, not in a template's instance: there the walk's
    // candidate need not be the code generator's callee (a CRTP base's `triggered(x)`, which the derived class's
    // overload answers), and the code generator reports what it cannot convert, as before.
    void checkClassToScalar(const Ty* from, const Ty* to, uint32_t token)
    {
      if (probing || activeSubst || !file || !from || !to || !from->isKnown() || !to->isKnown() || types.isDependent(from) ||
          types.isDependent(to))
        return;
      const Ty* f = types.unqualified(types.stripRef(from));
      const Ty* t = types.unqualified(types.stripRef(to));
      if (f->kind != TyKind::Class || f->isBraceList() || Impl::lambdaOf(f) || Impl::mentionsUnknown(f) || (f->decl && (f->decl->attributes & ATTR_Draft)))
        return;
      if (!t->isArithmetic() && t->kind != TyKind::Enum && t->kind != TyKind::Pointer)
        return;
      if (impl.hasScalarConversion(f, t->kind == TyKind::Bool))
        return;
      impl.reportCheckerError(file, token, "a class (" + types.toString(f, &symbols) + ") does not convert to " + types.toString(t, &symbols) +
                                             ": it has no implicit conversion operator to a scalar");
    }

    // Each argument of a call of f (fnTy: its type), the single plain candidate whose parameters the arguments do not
    // fit (pickOverload chose none), that is a class where its parameter is a scalar, checked (checkClassToScalar). Not
    // for a function template's call, nor one with an expansion among its arguments.
    void checkScalarArguments(FunctionDecl* f, const Ty* fnTy, const Span<Expr*>& args)
    {
      if (probing || !f || !f->templateParams.empty() || !fnTy || fnTy->kind != TyKind::Function || types.isDependent(fnTy))
        return;
      std::vector<const Ty*> argTypes = argTypesOf(args);
      if (argTypes.size() != args.size())
        return;
      for (size_t i = 0; i < args.size() && i < fnTy->args.size(); ++i)
        if (args[i] && args[i]->kind != NodeKind::BraceInitExpr && argTypes[i] && argTypes[i]->kind != TyKind::Pack)
          checkClassToScalar(argTypes[i], fnTy->args[i], args[i]->token);
    }

    // `NamedFunction(move(parameters), create, Expression::computeConstant)`: the constructor of
    // class type t the arguments choose, its parameters binding the overload sets among them.
    void bindOverloadSetsToConstructor(const std::vector<OverloadSetArgument>& sets, const Ty* t, Span<Expr*> args)
    {
      t = t ? types.unqualified(types.stripRef(t)) : nullptr;
      if (!t || t->kind != TyKind::Class || !t->isKnown() || types.isDependent(t))
        return;
      Scope* s = impl.classScopeOf(t);
      if (!s || !s->owner || s->owner->kind != NodeKind::ClassDecl)
        return;
      std::vector<FunctionDecl*> constructors;
      for (Decl* m : membersRead(static_cast<ClassDecl*>(s->owner)))
        if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Constructor))
          constructors.push_back(static_cast<FunctionDecl*>(m));
      Subst subst = impl.substFor(t);
      if (constructors.empty())
      {
        // `NamedFunction{Expression::computeConstant}` of an aggregate: its data members take the
        // arguments in order (a class with bases is left: they would come first).
        auto* cls = static_cast<ClassDecl*>(s->owner);
        if (!cls->bases.empty())
          return;
        std::vector<const Ty*> members;
        for (Decl* m : membersRead(cls))
          if (m->kind == NodeKind::VariableDecl && !(m->flags & (DF_Static | DF_Typedef | DF_Friend)))
            members.push_back(impl.declaredTypeOf(m, subst.empty() ? nullptr : &subst, 0));
        bindOverloadSets(sets, types.function(types.voidType(), Span<const Ty*>{members.data(), uint32_t(members.size())}, false));
        return;
      }
      bool ambiguous = false;
      std::vector<const Ty*> argTypes = argTypesOf(args);
      for (const OverloadSetArgument& set : sets)
        if (set.index < argTypes.size())
          argTypes[set.index] = types.unknown(); // the overload set decides nothing until the constructor is chosen
      if (FunctionDecl* ctor = impl.chooseAmong(constructors, argTypes, subst.empty() ? nullptr : &subst, nullptr, ambiguous); ctor && !ambiguous)
        bindOverloadSets(sets, impl.declaredTypeOf(ctor, subst.empty() ? nullptr : &subst, 0));
    }

    // `EditForceGui gui(0, &EditorAction::SetForceFriend, ...)`, `new EditForceGui(...)`: the
    // overload sets among a construction's arguments, bound once the constructor is chosen.
    void bindOverloadSetsOfConstruction(const Ty* t, Span<Expr*> args)
    {
      std::vector<OverloadSetArgument> sets;
      for (size_t i = 0; i < args.size(); ++i)
        if (OverloadSetArgument s; overloadSetArgument(args[i], i, s))
          sets.push_back(s);
      if (!sets.empty())
        bindOverloadSetsToConstructor(sets, t, args);
    }

    // `(ImU32)(255)`: a parenthesised name of a type before a parenthesised operand, which the
    // parser reads as a call (it cannot tell types from functions). C++ reads a cast. The name
    // inside, or null when it names no type.
    NameExpr* parenthesisedTypeCallee(Expr* e)
    {
      if (!e || e->kind != NodeKind::CallExpr)
        return nullptr;
      auto* c = static_cast<CallExpr*>(e);
      if (!c->callee || c->callee->kind != NodeKind::ParenExpr || c->args.size() != 1)
        return nullptr;
      Expr* inner = static_cast<ParenExpr*>(c->callee)->inner;
      if (!inner || inner->kind != NodeKind::NameExpr || static_cast<NameExpr*>(inner)->name.empty())
        return nullptr;
      auto* n = static_cast<NameExpr*>(inner);
      Lookup l = impl.lookupQualified(n->name, scope, &locals, 0, activeSubst);
      if (l.found == Found::No)
        return n->name.parts.size() == 1 && !n->name.parts[0].hasTemplateArgs && impl.builtinByName(n->name.parts[0].symbol) ? n : nullptr;
      Decl* d = l.decl;
      bool type = d && (d->kind == NodeKind::ClassDecl || d->kind == NodeKind::EnumDecl || d->kind == NodeKind::UsingAliasDecl ||
                        (d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef)) ||
                        (d->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(d)->isType));
      return type ? n : nullptr;
    }

    // `(T)(x).m`, `(T)(x)[i]`, `(T)(x)(y)`: C++ casts the whole postfix expression, the parser read
    // the cast as a call the postfix applies to; said, not compiled as the other one.
    void checkPostfixAfterParenthesisedType(Expr* base, uint32_t token)
    {
      if (file && parenthesisedTypeCallee(base))
        impl.reportCheckerError(file, token, "`(T)(x)` followed by `.`, `->`, `[` or `(` is a cast of the whole postfix expression in C++; "
                                             "write `T(x)` or `(T)((x)...)` to say which");
    }

    // Whether this walk owns the nodes it types, so that it may rewrite them: an instance's copy, a
    // function that is no template, a global's initializer. A template's own nodes are shared
    // with the walks copying them for its instances (and with probes), which rewrite their own.
    bool ownsNodes() const
    {
      if (probing)
        return false;
      if (activeSubst)
        return true;
      for (Decl* d = bodyOf; d; d = d->parent)
        if (!d->templateParams.empty())
          return false;
      for (Scope* s = scope; s; s = s->parent)
        if (s->owner && !s->owner->templateParams.empty())
          return false;
      return true;
    }

    // A lambda whose body names a Text (or a closure capturing one) of the function around it: its
    // closure holds the Text, so it is valid during the call only (holdsText).
    void markTextCapture(LambdaExpr* l, size_t mark)
    {
      if (!impl.textDecl() || !l->closure)
        return;
      auto outsideHoldsText = [&](Decl* d)
      {
        for (size_t i = 0; i < mark && i < locals.items.size(); ++i)
          if (locals.items[i].decl == d)
          {
            const Ty* t = locals.items[i].type ? locals.items[i].type : impl.declaredTypeOf(d, activeSubst, 0);
            return impl.holdsText(t);
          }
        return false;
      };
      bool holds = false;
      for (const LambdaCapture& cap : l->captures)
        if (!cap.decl && cap.symbol != UINT32_MAX)
          for (size_t i = std::min(mark, locals.items.size()); i-- > 0;)
            if (locals.items[i].symbol == cap.symbol)
            {
              holds = holds || outsideHoldsText(locals.items[i].decl);
              break;
            }
      if (!holds && l->body)
        forEachNode(l->body, [&](Node* n)
        {
          if (!holds && n->kind == NodeKind::NameExpr)
            if (Decl* d = static_cast<NameExpr*>(n)->resolved; d && (d->kind == NodeKind::ParamDecl || d->kind == NodeKind::VariableDecl))
              holds = outsideHoldsText(d);
        });
      if (holds)
        impl.markTextClosure(l->closure);
    }

    // A Text's lifetime rule broken (design.md 14): reported where the program asks for it, the
    // request's site for an instance of the library's code (`later = [=] { ... }` making a Function
    // that allocates the closure).
    void reportTextError(uint32_t token, const std::string& message)
    {
      if (probing || !file)
        return;
      if (!file->isLibrary)
        impl.reportCheckerError(file, token, message);
      else if (activeSubst && requestFile && !requestFile->isLibrary)
        impl.reportCheckerError(requestFile, requestToken, message);
      // (else an instance the library asked for, which the program's own request reported)
    }
    // A closure capturing a Text given where another type is wanted (a Function, a class's
    // parameter): it would be kept as that type, past the call (design.md 14). As its own type it
    // goes on (a template's parameter, a local), the rule following it.
    void checkClosureTo(const Expr* value, const Ty* to, uint32_t token)
    {
      if (!value || !value->ty || !to || !impl.isTextClosure(value->ty) || inGenericTemplate())
        return;
      const Ty* target = types.unqualified(types.stripRef(to));
      if (target->kind == TyKind::Param || types.isDependent(target) || target == types.unqualified(types.stripRef(value->ty)))
        return;
      reportTextError(token, "a lambda capturing a Text cannot be stored (design.md 14)");
    }
    static std::string textStored(const std::string& as)
    {
      return "a Text is valid only during the call that gives it, so it cannot be " + as + " (design.md 14)";
    }
    // The Text parameters of f: the last one, without a default, not from C; f's result holds none.
    void checkTextSignature(FunctionDecl* f)
    {
      if (probing || !impl.textDecl() || activeSubst)
        return; // (an instance's parameters are its template's, `const T& a` a Text where the template has none)
      const Ty* ft = impl.declaredTypeOf(f, nullptr, 0);
      if (!ft || ft->kind != TyKind::Function)
        return;
      bool generic = inGenericTemplate();
      // (an operator's or a conversion's last part names no symbol)
      std::string fname = !f->name.empty() && f->name.last().symbol != UINT32_MAX ? std::string(symbols.name(f->name.last().symbol)) : std::string();
      for (size_t i = 0; i < f->params.size() && i < ft->args.size(); ++i)
      {
        auto* p = static_cast<ParamDecl*>(f->params[i]);
        std::string name = p->name != UINT32_MAX ? std::string(symbols.name(p->name)) : std::to_string(i + 1);
        if (impl.isText(ft->args[i]))
        {
          if (i + 1 != f->params.size())
            reportTextError(p->token, "the Text parameter " + name + " takes the remaining arguments, so it must be the last parameter (design.md 14)");
          if (p->defaultValue)
            reportTextError(p->token, "the Text parameter " + name +
                                        " takes the remaining arguments, so it has no default argument; no arguments make an empty Text (design.md 14)");
          if (f->flags & DF_ExternC)
            reportTextError(p->token, "a Text parameter cannot be passed from C: " + fname + " is extern \"C\" (design.md 14)");
        }
        else if (!generic && impl.holdsText(ft->args[i]) && types.unqualified(types.stripRef(ft->args[i]))->kind == TyKind::Pointer)
          reportTextError(p->token, textStored("pointed to"));
      }
      // (A template's instance passes one on, `move(closure)`, as the function it is written in does.)
      if (!generic && ft->inner && impl.holdsText(ft->inner))
        reportTextError(f->token, textStored("the result of " + (fname.empty() ? std::string("an operator") : fname)));
      // `void appendTo(const Meters& m, String& out)` outside a class: a free appendTo the
      // program writes would never be called (design.md 14: the member is the one form).
      if (file && !file->isLibrary && f->name.parts.size() == 1 && fname == "appendTo" &&
          !impl.owningClassOf(f) && !(f->flags & DF_Friend))
        reportTextError(f->token, "appendTo is a member: write `void appendTo(String& out) const` in the type, not a free function (design.md 14)");
    }
    // A variable holding a Text: a parameter's local copy only, never a data member or a global.
    void checkTextVariable(VariableDecl* v, const Ty* declared)
    {
      if (probing || !declared || !impl.holdsText(declared) || inGenericTemplate() || v->name.empty())
        return;
      std::string name(symbols.name(v->name.last().symbol));
      bool member = v->parent && v->parent->kind == NodeKind::ClassDecl && !(v->flags & DF_Static);
      bool local = bodyOf && !member && !(v->flags & (DF_Static | DF_Extern | DF_ThreadLocal));
      if (member)
        reportTextError(v->token, textStored("the type of the data member " + name));
      else if (!local)
        reportTextError(v->token, textStored("the type of the variable " + name));
      else if (types.unqualified(types.stripRef(declared))->kind == TyKind::Pointer)
        reportTextError(v->token, textStored("pointed to"));
      else if (const Ty* c = types.unqualified(types.stripRef(declared)); c->kind == TyKind::Class && !impl.isText(c) && c->decl &&
                                                                         c->decl->kind == NodeKind::ClassDecl && !static_cast<ClassDecl*>(c->decl)->name.empty())
        reportTextError(v->token, textStored("an argument of " + std::string(symbols.name(static_cast<ClassDecl*>(c->decl)->name.last().symbol))));
      else if (impl.isText(declared) && (!v->init || !v->init->ty || !impl.isText(v->init->ty) ||
                                         (v->init->kind == NodeKind::BraceInitExpr && !static_cast<BraceInitExpr*>(v->init)->text)))
        reportTextError(v->token, "a Text is made only by passing arguments to a Text parameter (design.md 14)");
    }

    // A call of f whose last parameter is a Text (design.md 14): the arguments from its position on
    // become one brace list typed Text (BraceInitExpr::text), which code generation and the
    // interpreter build the Text from; each must have an appendTo, an error naming its type
    // otherwise. A call given one Text there passes it on as it is.
    void packText(Span<Expr*>& args, FunctionDecl* f, uint32_t token)
    {
      if (f && impl.namesTextLast(f))
        packTextAt(args, impl.textParameterOf(impl.declaredTypeOf(f, nullptr, 0)), token);
    }
    // The same for a call through a lambda or a function pointer whose type takes a Text.
    void packTextThrough(Span<Expr*>& args, const Ty* callee, uint32_t token)
    {
      const Ty* t = callee ? types.unqualified(types.stripRef(callee)) : nullptr;
      if (t && t->kind == TyKind::Pointer && t->inner)
        t = types.unqualified(t->inner);
      if (t && (t->kind == TyKind::Lambda || t->kind == TyKind::Function) && !t->args.empty() && !t->variadic && impl.isText(t->args[t->args.size() - 1]))
        packTextAt(args, t->args.size() - 1, token);
    }
    void packTextAt(Span<Expr*>& args, size_t k, uint32_t token)
    {
      if (k == SIZE_MAX || args.size() < k)
        return;
      if (args.size() == k + 1 && args[k] &&
          ((args[k]->kind == NodeKind::BraceInitExpr && static_cast<BraceInitExpr*>(args[k])->text) || (args[k]->ty && impl.isText(args[k]->ty))))
        return;
      bool known = true;
      auto check = [&](Expr* a, const Ty* t)
      {
        TextPieceKind piece = impl.textPieceOf(a, t);
        if (!piece.error.empty())
        {
          if (!probing)
            impl.reportCheckerError(file, a->token, piece.error);
          known = false;
        }
        else if (!piece.thunk)
          known = false;
      };
      for (size_t i = k; i < args.size(); ++i)
      {
        Expr* a = args[i];
        if (!a || isEmptyExpansion(a))
          continue;
        if (a->kind == NodeKind::BraceInitExpr)
        {
          if (!probing)
            impl.reportCheckerError(file, a->token, "a brace list is no piece of a Text: it has no type (design.md 14)");
          known = false;
          continue;
        }
        bool expansion = a->kind == NodeKind::UnaryExpr && !static_cast<UnaryExpr*>(a)->isKeyword && static_cast<UnaryExpr*>(a)->postfix &&
                         P(static_cast<UnaryExpr*>(a)->op) == P::Ellipsis;
        if (a->ty && a->ty->kind == TyKind::Pack && !expansion)
        {
          // `(r.append(text(rest)), ...)`: one element of the pack per expansion of the pattern, typed
          // (and its overload chosen: text(int) before text(Text)) where the pattern expands, never
          // the whole pack packed into one Text.
          known = false;
          continue;
        }
        if (a->ty && a->ty->kind == TyKind::Pack)
          for (const Ty* e : a->ty->args)
            check(a, e);
        else
          check(a, a->ty);
      }
      const Ty* text = impl.textType();
      if (!known || !text || !ownsNodes())
        return;
      Arena& arena = impl.instanceArena();
      auto* packed = arena.make<BraceInitExpr>();
      packed->kind = NodeKind::BraceInitExpr;
      packed->token = k < args.size() ? args[k]->token : token;
      packed->args = Span<Expr*>{arena.array(args.data + k, args.size() - k), uint32_t(args.size() - k)};
      packed->text = true;
      record(packed, text);
      std::vector<Expr*> all(args.begin(), args.begin() + k);
      all.push_back(packed);
      args = Span<Expr*>{arena.array(all.data(), all.size()), uint32_t(all.size())};
    }

    // A call of a consteval function, or of a consteval constructor converting an argument (C++20's
    // immediate invocation): a constant where it is written, evaluated here; an error naming the
    // function otherwise. Inside a consteval function's own body it is evaluated with the call of
    // that function (its parameters are constants there).
    FunctionDecl* initializersOf = nullptr; // the constructor whose member initializers are walked
    bool insideConsteval() const
    {
      if (initializersOf && (initializersOf->flags & DF_Consteval))
        return true;
      for (Decl* d = bodyOf; d; d = d->parent)
        if (d->kind == NodeKind::FunctionDecl && (d->flags & DF_Consteval))
          return true;
      return false;
    }
    // Whether an immediate evaluation fails here (why: the evaluator's reason); not when it needs a
    // class layout, which is code generation's to know: not decided here.
    bool immediateFails(const std::function<std::optional<ConstValue>()>& evaluate, std::string& why)
    {
      bool outerExact = impl.exactLayoutOnly, outerNeeded = impl.layoutNeeded;
      impl.exactLayoutOnly = true;
      impl.layoutNeeded = false;
      impl.constWhy().clear();
      std::optional<ConstValue> v = evaluate();
      why = v ? std::string() : impl.constWhy();
      bool needed = impl.layoutNeeded;
      impl.exactLayoutOnly = outerExact;
      impl.layoutNeeded = outerNeeded;
      return !v && !needed;
    }
    void checkImmediate(FunctionDecl* f, uint32_t token, const std::function<std::optional<ConstValue>()>& evaluate)
    {
      if (!f || !(f->flags & DF_Consteval) || probing || inGenericTemplate() || insideConsteval() || f->name.empty() || !file)
        return;
      std::string why;
      if (!immediateFails(evaluate, why))
        return;
      std::string name = f->name.last().symbol != UINT32_MAX ? std::string(symbols.name(f->name.last().symbol)) : std::string("operator");
      if (f->flags & DF_Constructor)
        name += "::" + name;
      reportTextError(token, "the call of consteval " + name + " is no constant" + (why.empty() ? std::string() : ": " + why));
    }
    // An argument converted to its class parameter by a consteval constructor (`format("{}", x)`
    // with format's `FormatString(const char*)`, design.md 14): that conversion is an immediate
    // invocation.
    void checkImmediateConversion(Expr* arg, const Ty* param)
    {
      const Ty* cls = param ? types.unqualified(types.stripRef(param)) : nullptr;
      if (!arg || !arg->ty || !cls || cls->kind != TyKind::Class || types.isDependent(cls) || !cls->isKnown() || impl.isText(cls))
        return;
      if (types.unqualified(types.stripRef(arg->ty)) == cls)
        return;
      Scope* s = impl.classScopeOf(cls);
      if (!s || !s->owner || s->owner->kind != NodeKind::ClassDecl)
        return;
      std::vector<FunctionDecl*> ctors;
      bool immediate = false;
      for (Decl* m : membersRead(static_cast<ClassDecl*>(s->owner)))
        if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Constructor) && !(m->flags & DF_Delete))
        {
          ctors.push_back(static_cast<FunctionDecl*>(m));
          immediate = immediate || (m->flags & DF_Consteval);
        }
      if (!immediate)
        return;
      Subst subst = impl.substFor(cls);
      bool ambiguous = false;
      FunctionDecl* ctor = impl.chooseAmong(ctors, {argumentType(arg, arg->ty, types)}, subst.empty() ? nullptr : &subst, nullptr, ambiguous);
      if (!ctor || ambiguous || !(ctor->flags & DF_Consteval))
        return;
      checkImmediate(ctor, arg->token, [&]() -> std::optional<ConstValue>
      {
        std::optional<ConstValue> value = impl.constValue(arg, ctx(), 0);
        if (!value)
          return std::nullopt;
        return impl.constRunConstructor(ctor, cls, {*value}, 0);
      });
    }

    // The piece count a call gives its Text parameter at k (a pack expansion its elements); SIZE_MAX
    // when it is not known here (the Text passed through, a pack's element in a pattern).
    size_t pieceCount(Span<Expr*> args, size_t k)
    {
      if (args.size() == k + 1 && args[k] &&
          ((args[k]->kind == NodeKind::BraceInitExpr && static_cast<BraceInitExpr*>(args[k])->text) || (args[k]->ty && impl.isText(args[k]->ty))))
        return SIZE_MAX;
      size_t n = 0;
      for (size_t i = k; i < args.size(); ++i)
      {
        Expr* a = args[i];
        if (!a || isEmptyExpansion(a))
          continue;
        bool expansion = a->kind == NodeKind::UnaryExpr && !static_cast<UnaryExpr*>(a)->isKeyword && static_cast<UnaryExpr*>(a)->postfix &&
                         P(static_cast<UnaryExpr*>(a)->op) == P::Ellipsis;
        if (a->ty && a->ty->kind == TyKind::Pack)
        {
          if (!expansion)
            return SIZE_MAX;
          n += a->ty->args.size();
        }
        else if (expansion || !a->ty || !a->ty->isKnown())
          return SIZE_MAX;
        else
          ++n;
      }
      return n;
    }

    // design.md 14: a parameter whose class has `consteval void checkPieces(ContainerSize pieces)
    // const`, right before a Text parameter (format's FormatString), is checked against the pieces
    // at every call: checkPieces runs on the argument's constant value with the call's piece count,
    // and a failure (a throw) is a compile error at the call. An argument that is no constant here
    // (a FormatString parameter passed on) is not checked: its own call was.
    void checkTextPieces(FunctionDecl* f, const Ty* ft, Span<Expr*> args)
    {
      if (probing || inGenericTemplate() || insideConsteval() || !file || !ft || ft->kind != TyKind::Function)
        return;
      size_t k = impl.textParameterOf(impl.declaredTypeOf(f, nullptr, 0));
      if (k == SIZE_MAX || k == 0 || k > ft->args.size() || args.size() < k)
        return;
      const Ty* cls = types.unqualified(types.stripRef(ft->args[k - 1]));
      if (!cls || cls->kind != TyKind::Class || types.isDependent(cls) || !cls->isKnown())
        return;
      Scope* s = impl.classScopeOf(cls);
      if (!s || !s->owner || s->owner->kind != NodeKind::ClassDecl)
        return;
      if (checkPiecesSymbol == UINT32_MAX)
        checkPiecesSymbol = impl.symbols.intern("checkPieces");
      FunctionDecl* check = nullptr;
      std::vector<FunctionDecl*> ctors;
      for (Decl* m : membersRead(static_cast<ClassDecl*>(s->owner)))
        if (m->kind == NodeKind::FunctionDecl && !static_cast<FunctionDecl*>(m)->name.empty() && static_cast<FunctionDecl*>(m)->name.last().symbol == checkPiecesSymbol)
          check = static_cast<FunctionDecl*>(m);
        else if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Constructor) && !(m->flags & DF_Delete))
          ctors.push_back(static_cast<FunctionDecl*>(m));
      Expr* arg = args[k - 1];
      if (!check || !arg)
        return;
      if (!(check->flags & DF_Consteval) || check->params.size() != 1)
      {
        reportTextError(arg->token, "checkPieces of " + types.toString(cls, &symbols) + " is not `consteval void checkPieces(ContainerSize pieces) const` (design.md 14)");
        return;
      }
      size_t n = pieceCount(args, k);
      if (n == SIZE_MAX)
        return;
      Subst subst = impl.substFor(cls);
      const Ty* countTy = impl.declaredTypeOf(check->params[0], subst.empty() ? nullptr : &subst, 0);
      // One evaluation builds the argument's object and runs checkPieces on it, so that what the
      // object or a thrown exception holds (a message built in a String) lives until the end.
      bool built = false;
      std::string why;
      bool fails = immediateFails([&]() -> std::optional<ConstValue>
      {
        return impl.constOutermost([&]() -> std::optional<ConstValue>
        {
          std::optional<ConstValue> value = impl.constValue(arg, ctx(), 0);
          if (!value)
            return std::nullopt;
          if (arg->ty && types.unqualified(types.stripRef(arg->ty)) != cls)
          {
            bool ambiguous = false;
            FunctionDecl* ctor = impl.chooseAmong(ctors, {argumentType(arg, arg->ty, types)}, subst.empty() ? nullptr : &subst, nullptr, ambiguous);
            if (!ctor || ambiguous)
              return std::nullopt;
            value = impl.constRunConstructor(ctor, cls, {*value}, 0);
            if (!value)
              return std::nullopt;
          }
          built = true;
          std::optional<ConstValue> self = value;
          return impl.constCallOn(check, subst, {ConstValue::integer(int64_t(n), countTy)}, &self, 0);
        }, 0);
      }, why);
      // (an argument that is no constant here is not checked; a consteval constructor's own failure
      // is reported by its check)
      if (fails && built)
        reportTextError(arg->token, types.toString(cls, &symbols) + " does not take " + std::to_string(n) + (n == 1 ? " piece" : " pieces") +
                                        (why.empty() ? std::string() : ": " + why) + " (design.md 14)");
    }
    uint32_t checkPiecesSymbol = UINT32_MAX;

    // The constructor of cls a call with these arguments runs when one of cls's takes a Text, else null.
    FunctionDecl* textConstructor(const Ty* cls, Span<Expr*> args)
    {
      cls = cls ? types.unqualified(types.stripRef(cls)) : nullptr;
      if (!cls || cls->kind != TyKind::Class || !cls->isKnown() || types.isDependent(cls))
        return nullptr;
      Scope* s = impl.classScopeOf(cls);
      if (!s || !s->owner || s->owner->kind != NodeKind::ClassDecl)
        return nullptr;
      std::vector<FunctionDecl*> ctors;
      bool any = false;
      for (Decl* m : membersRead(static_cast<ClassDecl*>(s->owner)))
        if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Constructor) && !(m->flags & DF_Delete))
        {
          ctors.push_back(static_cast<FunctionDecl*>(m));
          any = any || impl.namesTextLast(static_cast<FunctionDecl*>(m));
        }
      if (!any)
        return nullptr;
      Subst subst = impl.substFor(cls);
      bool ambiguous = false;
      FunctionDecl* f = impl.chooseAmong(ctors, argTypesOf(args), subst.empty() ? nullptr : &subst, nullptr, ambiguous);
      return f && !ambiguous && impl.namesTextLast(f) ? f : nullptr;
    }

    const Ty* typeOfCall(CallExpr* c)
    {
      const Ty* t = typeOfCallUnpacked(c);
      Expr* callee = c->callee;
      Decl* d = callee && callee->kind == NodeKind::NameExpr     ? static_cast<NameExpr*>(callee)->resolved
                : callee && callee->kind == NodeKind::MemberExpr ? static_cast<MemberExpr*>(callee)->resolved
                                                                 : nullptr;
      if (d && d->kind == NodeKind::FunctionDecl)
      {
        auto* f = static_cast<FunctionDecl*>(d);
        if (const Ty* ft = impl.declaredTypeOf(f, nullptr, 0); ft && ft->kind == TyKind::Function)
        {
          for (size_t i = 0; i < c->args.size() && i < ft->args.size(); ++i)
            checkClosureTo(c->args[i], ft->args[i], c->args[i]->token);
          // A function template's parameters as this call's instance has them (`CountedFormat<sizeof...(A)>`
          // for A = int, String: CountedFormat<2>), its deduced arguments substituted, never as declared;
          // those before a parameter pack only (they are the arguments at their positions), and none
          // while the call's arguments are not deduced.
          size_t checked = c->args.size();
          if (!f->templateParams.empty())
          {
            Subst deduced;
            for (size_t i = 0; i < c->templateArgCount && i < f->templateParams.size(); ++i)
              deduced.pairs.push_back({f->templateParams[i], c->templateArgs[i]});
            ft = deduced.pairs.size() == f->templateParams.size() ? impl.declaredTypeOf(f, &deduced, 0) : nullptr;
            if (ft && (ft->kind != TyKind::Function || types.isDependent(ft)))
              ft = nullptr;
            for (size_t i = 0; i < f->params.size(); ++i)
              if (static_cast<ParamDecl*>(f->params[i])->isPack)
                checked = std::min(checked, i);
          }
          for (size_t i = 0; ft && i < checked && i < ft->args.size(); ++i)
            checkImmediateConversion(c->args[i], ft->args[i]);
          checkTextPieces(f, ft, c->args);
        }
        checkImmediate(f, c->token, [&] { return impl.constValue(c, ctx(), 0); });
        packText(c->args, f, c->token);
      }
      else if (d && t && (d->kind == NodeKind::ClassDecl || d->kind == NodeKind::UsingAliasDecl || (d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef))))
      {
        // In a requirement (`requires { StringView(value); }`), a class built from arguments none of
        // its constructors takes is no well-formed expression: the requirement fails, never holds
        // because the cast has the class's type whatever the constructors.
        if (probing && !c->args.empty() && types.unqualified(t)->kind == TyKind::Class && !types.isDependent(t))
        {
          std::vector<const Ty*> traitArgs{t};
          bool known = true;
          for (const Ty* a : argTypesOf(c->args))
          {
            known = known && a && a->isKnown() && !types.isDependent(a);
            traitArgs.push_back(a);
          }
          if (known && impl.traitValue("isConstructible", traitArgs) == false)
            return unknown(Why::Overload);
        }
        packText(c->args, textConstructor(t, c->args), c->token); // `Problem("bad ", point)`
      }
      else if (callee)
        packTextThrough(c->args, callee->ty, c->token); // `log("a", b)` with `auto log = [](Text text) {...}` or a function pointer
      return t;
    }

    // `x.appendTo(out)` on a built-in type or an enumeration (design.md 14: the member of every
    // type): an enumeration's own appendTo method if it has one, else the standard library's
    // `void appendTo(T value, String& out)` for the type, which the program does not write.
    bool isBuiltinAppendTo(const Ty* t, uint32_t symbol)
    {
      if (!t || symbol == UINT32_MAX || symbols.name(symbol) != "appendTo")
        return false;
      if (t->kind == TyKind::Enum)
      {
        // (a kpp enum's own appendTo method, in its scope, is called as written)
        Decl* e = t->decl;
        Lookup own;
        return e && !(e->ownScope && impl.lookupInScope(e->ownScope, symbol, own, 0) && own.decl && own.decl->kind == NodeKind::FunctionDecl);
      }
      return t->kind == TyKind::Bool || t->kind == TyKind::Char || t->kind == TyKind::Int || t->kind == TyKind::Float;
    }
    // The call written as the library function's (`appendTo(x, out)`), so that code generation and
    // the interpreter see a plain call; typed as such. A walk that does not own the nodes (a
    // template's own) types it without rewriting: its instances rewrite their copies.
    const Ty* builtinMemberCall(CallExpr* c, MemberExpr* m, const Ty* base, const Ty* builtin)
    {
      std::vector<const Ty*> argTypes{argumentType(m->base, base, types)};
      for (Expr* a : c->args)
        argTypes.push_back(argumentType(a, walkExpr(a), types));
      std::vector<FunctionDecl*> candidates;
      Lookup l;
      if (impl.lookupInScope(&impl.root, m->member.last().symbol, l, 0))
      {
        if (l.overloads)
        {
          for (const Entry& e : *l.overloads)
            if (e.decl && e.decl->kind == NodeKind::FunctionDecl && e.decl->file && e.decl->file->isLibrary)
              candidates.push_back(static_cast<FunctionDecl*>(e.decl));
        }
        else if (l.decl && l.decl->kind == NodeKind::FunctionDecl && l.decl->file && l.decl->file->isLibrary)
          candidates.push_back(static_cast<FunctionDecl*>(l.decl));
      }
      bool ambiguous = false;
      FunctionDecl* f = candidates.empty() ? nullptr : impl.chooseAmong(candidates, argTypes, nullptr, nullptr, ambiguous);
      const Ty* ft = f ? impl.declaredTypeOf(f, nullptr, 0) : nullptr;
      const Ty* takes = ft && ft->kind == TyKind::Function && !ft->args.empty() ? types.unqualified(types.stripRef(ft->args[0])) : nullptr;
      if (!f || ambiguous || !takes || (f->templateParams.empty() && takes != builtin))
      {
        // (an exact one only: no promotion makes a u8 print through the i32's)
        if (!probing && !inGenericTemplate())
          impl.reportCheckerError(file, m->token, types.toString(builtin, &symbols) + " has no appendTo taking it (design.md 14)");
        record(m, types.unknown());
        return unknown(Why::Member);
      }
      if (!ownsNodes())
      {
        record(m, ft);
        return ft->inner;
      }
      Arena& arena = impl.instanceArena();
      auto* name = arena.make<NameExpr>();
      name->kind = NodeKind::NameExpr;
      name->token = m->token;
      name->name = m->member;
      std::vector<Expr*> args{m->base};
      args.insert(args.end(), c->args.begin(), c->args.end());
      c->callee = name;
      c->args = Span<Expr*>{arena.array(args.data(), args.size()), uint32_t(args.size())};
      return typeOfCallUnpacked(c);
    }

    const Ty* typeOfCallUnpacked(CallExpr* c)
    {
      if (c->callee && c->callee->kind == NodeKind::CallExpr)
        checkPostfixAfterParenthesisedType(c->callee, c->token);
      if (NameExpr* typeName = parenthesisedTypeCallee(c); typeName && !probesSharedNodes())
        c->callee = typeName; // `(ImU32)(255)` as the functional cast `ImU32(255)`, which means the same
      std::vector<const Ty*> walked;
      walked.reserve(c->args.size());
      for (Expr* a : c->args)
        walked.push_back(walkExpr(a));
      std::vector<OverloadSetArgument> sets;
      for (size_t i = 0; i < c->args.size(); ++i)
        if (OverloadSetArgument s; overloadSetArgument(c->args[i], i, s))
        {
          sets.push_back(s);
          walked[i] = types.unknown();
        }
      WalkedArgs walkedArgsScope(*this, c->args, std::move(walked));
      struct SetsScope
      {
        const std::vector<OverloadSetArgument>*& current;
        const std::vector<OverloadSetArgument>* saved;
        ~SetsScope() { current = saved; }
      } setsScope{callSets, callSets};
      callSets = sets.empty() ? nullptr : &sets;
      Expr* callee = c->callee;
      bool ambiguous = false;
      if (callee->kind == NodeKind::NameExpr)
      {
        auto* n = static_cast<NameExpr*>(callee);
        if (n->name.parts.size() == 1 && !n->name.parts[0].hasTemplateArgs && c->args.size() == 2 &&
            c->args[1]->kind == NodeKind::SizeofExpr && static_cast<SizeofExpr*>(c->args[1])->type &&
            impl.symbols.name(n->name.parts[0].symbol) == "va_arg")
        {
          // `va_arg(list, T)` (the parser's form, Parser::parseVaArg): the next argument, a T.
          n->resolved = nullptr;
          ++stats.callsResolved;
          const Ty* t = typeOf(static_cast<SizeofExpr*>(c->args[1])->type);
          record(n, types.function(t, {}, false));
          return t;
        }
        if (n->name.parts.size() == 1 && n->name.parts[0].hasTemplateArgs && impl.isTypeTraitName(n->name.parts[0].symbol))
          if (const Ty* trait = impl.typeTrait(n->name.parts[0], ctx(), 0); trait && trait->isKnown())
          {
            // `Conditional<isSigned<U>, int64_t, uint64_t>(value)`: a functional cast to the type the
            // built-in trait gives, as the same name gives in a declaration (not the library's
            // Conditional struct).
            n->resolved = nullptr;
            for (Node* arg : n->name.parts[0].templateArgs)
              walkNode(arg);
            ++stats.callsResolved;
            record(n, trait);
            return trait;
          }
        if (n->name.parts.size() > 1 && n->name.last().special == 3 && n->name.last().convType)
        {
          // `super::operator bool()`: the conversion function of the class the qualifier names.
          NamedType written{};
          written.kind = NodeKind::NamedType;
          written.token = n->token;
          written.name = n->name;
          written.name.parts.count -= 1;
          const Ty* q = typeOf(&written);
          walkType(n->name.last().convType);
          Subst cs;
          if (FunctionDecl* conv = q && q->isKnown() && !types.isDependent(q)
                                       ? impl.conversionFunctionTo(types.unqualified(types.stripRef(q)), typeOf(n->name.last().convType), cs)
                                       : nullptr)
          {
            const Ty* ft = impl.declaredTypeOf(conv, cs.pairs.empty() ? nullptr : &cs, 0);
            n->resolved = conv;
            ++stats.callsResolved;
            record(n, ft);
            return ft->inner;
          }
        }
        Lookup l = impl.lookupQualified(n->name, scope, &locals, 0, activeSubst);
        Impl::preferValue(l); // (`stat("/", &s)`: the function, not the struct's aggregate initialisation)
        if (!whenLookup(l, n))
        {
          record(n, types.unknown());
          return types.unknown(); // (design.md 19: reported, or the instances' to tell)
        }
        Subst friendArgs; // the class arguments of the friend functions argument-dependent lookup finds
        if (l.found == Found::Global)
          l = impl.withNamespaceUsings(l, 0); // `using lib::size;` beside the namespace's own size(int)
        if ((!l.decl || l.decl->kind != NodeKind::FunctionDecl) && l.found != Found::Dependent && n->name.parts.size() > 1 &&
            n->name.last().special == 2 && P(n->name.last().opToken) == P::Assign && c->args.size() == 1 && thisType)
        {
          // `super::operator=(other)` of a class declaring none: its implicit assignment, of the
          // class the name is qualified with, on this object, as `this->super::operator=(other)`.
          NamedType written{};
          written.kind = NodeKind::NamedType;
          written.token = n->token;
          written.name = n->name;
          written.name.parts.count -= 1;
          if (const Ty* q = typeOf(&written); q && q->isKnown() && types.unqualified(types.stripRef(q))->kind == TyKind::Class)
          {
            const Ty* target = types.unqualified(types.stripRef(q));
            n->resolved = nullptr;
            ++stats.callsResolved;
            record(n, types.function(types.lref(target), {}, false));
            return types.lref(target);
          }
        }
        MemberResult viaInstance; // keeps a merged overload set alive
        if (l.found == Found::No && n->name.parts.size() > 1)
        {
          // `super::paintBackgroundGlow(...)` with super a DragPaneWidget<agui::VerticalFlow, 1>
          // deriving from its parameter: the member of the instance, found in the base its
          // arguments make, which the class template's own scope does not have; a member template
          // with its arguments too (`super::staticInitGenericOnOffControlBehaviorMembers<T>()`).
          NamedType written{};
          written.kind = NodeKind::NamedType;
          written.token = n->token;
          written.name = n->name;
          written.name.parts.count -= 1;
          const Ty* q = typeOf(&written);
          q = q && q->isKnown() && !types.isDependent(q) ? types.unqualified(types.stripRef(q)) : nullptr;
          if (q && q->kind == TyKind::Class)
          {
            viaInstance = memberOf(q, n->name.last().symbol, nullptr, n->token, false);
            if (viaInstance.decl && viaInstance.decl->kind == NodeKind::FunctionDecl && viaInstance.decl->parent &&
                viaInstance.decl->parent->ownScope)
            {
              l.decl = viaInstance.decl;
              l.overloads = viaInstance.overloads;
              l.found = Found::Member;
              l.foundIn = viaInstance.decl->parent->ownScope;
            }
          }
        }
        // Argument-dependent lookup, as C++ has it: an unqualified call also finds the functions of
        // its name in the namespaces of its arguments' classes and enumerations
        // (`shortNumberFormat(time, StringUtil::Compactness::Loose)` from namespace TimeUtil),
        // unless ordinary lookup found something else than namespace functions (a member, a local).
        if (n->name.parts.size() == 1 && !c->args.empty() && (l.found == Found::No || (l.found == Found::Global && l.decl && l.decl->kind == NodeKind::FunctionDecl)) &&
            !(l.found == Found::No && impl.builtinByName(n->name.parts[0].symbol)))
          addArgumentDependentCandidates(l, n->name.parts[0].symbol, argTypesOf(c->args), friendArgs, n->token);
        n->resolved = l.decl;
        for (const NamePart& part : n->name.parts)
          for (Node* arg : part.templateArgs)
            walkNode(arg);
        switch (l.found)
        {
          case Found::No:
            if (n->name.parts.size() == 1)
              if (const Ty* b = impl.builtinByName(n->name.parts[0].symbol))
              {
                record(n, b); // `uint32_t(x)`: a functional cast, not an unresolved name
                return b;
              }
            ++stats.namesUnresolved;
            countUnresolved(n->name, n->token);
            {
              bool dependentCall = false;
              for (const Ty* a : argTypesOf(c->args))
                dependentCall = dependentCall || !a || !a->isKnown() || types.isDependent(a);
              reportUndeclared(n->name, n->token, nullptr, dependentCall);
            }
            record(n, unknown(Why::Name));
            return types.unknown();
          case Found::Local: ++stats.namesLocal; break;
          case Found::Member: ++stats.namesMember; break;
          case Found::Global: ++stats.namesGlobal; break;
          case Found::Dependent: ++stats.namesDependent; break;
        }
        if (l.found == Found::Dependent)
        {
          // (`Args()` in the pattern of one element's expansion: that element's type.)
          const Ty* t = l.decl ? dependentNameType(l.decl) : types.unknown();
          record(n, t);
          if (l.decl && l.decl->kind == NodeKind::TemplateParamDecl && !static_cast<TemplateParamDecl*>(l.decl)->isType)
            return callResultOfValue(t && t->kind == TyKind::Value && t->inner ? t->inner : t, c); // `func(a, b)`: a call through a constant parameter
          return t;
        }
        // `save(t)` in a member of a class with `using Vector2::save;` beside its own save overloads.
        Subst usingArgs;
        std::shared_ptr<std::vector<Entry>> usingMerged;
        if (n->name.parts.size() == 1 && l.found == Found::Member && l.foundIn && l.foundIn->kind == ScopeKind::Class)
        {
          const Ty* self = thisType && thisType->kind == TyKind::Pointer ? types.unqualified(thisType->inner) : nullptr;
          if ((usingMerged = withUsingOverloads(l, self, self ? impl.classScopeOf(self) : nullptr, usingArgs)))
          {
            l.decl = usingMerged->front().decl;
            l.overloads = usingMerged.get();
          }
        }
        else if (n->name.parts.size() > 1 && l.foundIn && l.foundIn->kind == ScopeKind::Class)
        {
          // `super::transfer(move(data))` with RollingStock's `using Vehicle::transfer;` beside its
          // own transfer(Fluid&): both sets, of the class the qualifier names.
          NamedType written{};
          written.kind = NodeKind::NamedType;
          written.token = n->token;
          written.name = n->name;
          written.name.parts.count -= 1;
          const Ty* q = typeOf(&written);
          q = q && q->isKnown() && !types.isDependent(q) ? types.unqualified(types.stripRef(q)) : nullptr;
          if ((usingMerged = withUsingOverloads(l, q, q ? impl.classScopeOf(q) : nullptr, usingArgs)))
          {
            l.decl = usingMerged->front().decl;
            l.overloads = usingMerged.get();
          }
        }
        Decl* d = l.decl;
        if (d && d->kind == NodeKind::FunctionDecl)
        {
          // `DamageTypeID::fromIndex(i)` with `using DamageTypeID = ID<DamageType, uint8_t>`: a static
          // member named through an instantiation has that instantiation's arguments.
          Subst qualifier = usingArgs;
          for (const Subst::Pair& p : friendArgs.pairs)
            qualifier.pairs.push_back(p); // a friend defined in an instance's class: that instance's arguments
          if (n->name.parts.size() > 1 && l.foundIn && l.foundIn->kind == ScopeKind::Class)
          {
            qualifier = impl.qualifierSubst(n->name, ctx(), 0, l.foundIn);
            for (const Subst::Pair& p : usingArgs.pairs)
              if (!qualifier.find(p.param))
                qualifier.pairs.push_back(p); // the base instances the using-declarations reach
          }
          else if (n->name.parts.size() == 1 && l.found == Found::Member && activeSubst && l.foundIn && l.foundIn->kind == ScopeKind::Class &&
                   l.foundIn->owner)
          {
            // `construct(...)` in a member of UniquePointer<T>: the member of this instantiation (or of
            // the base it was found in), whose parameter types and constraints need T.
            for (const Subst::Pair& p : impl.baseSubstForUnqualified(l, ctx(), 0).pairs)
              if (!qualifier.find(p.param))
                qualifier.pairs.push_back(p);
            // With the enclosing instantiation's arguments, except a candidate's own parameters,
            // which the call deduces afresh (a member template calling itself).
            auto ownOfCandidate = [&](Decl* param)
            {
              auto owns = [&](Decl* f) { return std::find(f->templateParams.begin(), f->templateParams.end(), param) != f->templateParams.end(); };
              if (!l.overloads)
                return owns(d);
              for (const Entry& e : *l.overloads)
                if (e.decl && e.decl->kind == NodeKind::FunctionDecl && owns(e.decl))
                  return true;
              return false;
            };
            for (const Subst::Pair& p : activeSubst->pairs)
              if (!qualifier.find(p.param) && !ownOfCandidate(p.param))
                qualifier.pairs.push_back(p);
          }
          const Subst* classSubst = qualifier.empty() ? nullptr : &qualifier;
          std::vector<FunctionDecl*> tied;
          std::vector<const Ty*> argTypes = argTypesOf(c->args);
          // In a template's own walk, a call of several functions (or of a template) with an argument
          // whose type depends on the template's parameters, or that this walk cannot type
          // (`ValueToWidget::make(*value)` with value a `const UniquePtr<T>&`), is resolved in each
          // instance with the instance's types ([temp.dep.candidate]), not here: ranking the
          // candidates against a type not known yet decides nothing, and costs the most of all in a
          // set of hundreds.
          if (!activeSubst && inGenericTemplate() && !onlyPlainCandidate(d, l.overloads))
          {
            bool dependentArgs = false;
            for (const Ty* a : argTypes)
              dependentArgs = dependentArgs || !a || !a->isKnown() || types.isDependent(a);
            if (dependentArgs)
            {
              c->templateArgCount = 0;
              n->resolved = nullptr;
              record(n, types.unknown());
              return unknown(Why::None);
            }
          }
          // A member called on the implicit object (`getSurface()`, `super::getStack(location)` in a
          // member of the class or of one derived from it): *this, whose const-ness chooses between
          // a const and a non-const overload as an explicit object's does.
          const Ty* implicitObject = nullptr;
          if (thisType && thisType->kind == TyKind::Pointer && thisType->inner && l.foundIn && l.foundIn->kind == ScopeKind::Class)
            if (Scope* self = impl.classScopeOf(types.unqualified(thisType->inner)); self && reachesClassScope(self, l.foundIn, 0))
              implicitObject = thisType->inner;
          FunctionDecl* f = pickOverload(d, l.overloads, argTypes, classSubst, implicitObject, ambiguous, &n->name.last(), c->args, &tied);
          if (f && !ambiguous)
          {
            checkConstObjectCall(f, implicitObject, c->token);
          }
          if (!f)
          {
            // No callee the code generator could trust: not the first of several overloads, nor a
            // template with the arguments an earlier walk of this call recorded (the generic
            // one's). A single plain candidate stays: the code generator converts the arguments
            // itself and says so when it cannot.
            c->templateArgCount = 0;
            if (!onlyPlainCandidate(d, l.overloads))
              n->resolved = nullptr;
            else // `takeInt(item)` with a UniquePtr<Item>: a class where a parameter is a scalar, the checker's error
              checkScalarArguments(static_cast<FunctionDecl*>(d), impl.declaredTypeOf(d, classSubst, 0), c->args);
            record(n, types.unknown());
            return unknown(Why::Overload);
          }
          if (ambiguous)
          {
            ++stats.callsAmbiguousArity;
            reportAmbiguousCall(c, n->name.last(), argTypes, tied, classSubst);
          }
          ++stats.callsResolved;
          n->resolved = f;
          const Ty* fnTy = impl.declaredTypeOf(f, classSubst, 0);
          record(n, fnTy);
          bindOverloadSets(sets, fnTy);
          if (fnTy && fnTy->kind == TyKind::Function)
            for (size_t i = 0; i < c->args.size() && i < fnTy->args.size(); ++i)
              noteZeroNull(c->args[i], fnTy->args[i]); // `count(0)` for a Node*
          return returnTypeOfCall(f, classSubst, c->args, &n->name.last(), c);
        }
        // Constructor call, functional cast, callable variable...
        const Ty* t = typeOfResolvedName(l, n->name.last(), n);
        record(n, t);
        if (!sets.empty())
          bindOverloadSetsToConstructor(sets, t, c->args);
        if (d && (d->kind == NodeKind::ClassDecl || d->kind == NodeKind::EnumDecl || d->kind == NodeKind::UsingAliasDecl ||
                  (d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef))))
        {
          ++stats.callsResolved; // a functional cast, `int64_t(x)` included when int64_t is an imported typedef
          if (n->name.parts.size() > 1 &&
              (d->kind == NodeKind::UsingAliasDecl || d->kind == NodeKind::VariableDecl ||
               (d->kind == NodeKind::ClassDecl && d->templateParams.empty() && !impl.effectiveParams(d).empty())))
          {
            // `Table::value_type(65)`: the alias as the type path reads it, with the qualifier's
            // instantiation substituted (Array<unsigned char, 4>::value_type is unsigned char);
            // `Surface::const_iterator(surface)`: the nested class of the base
            // InfiniteRectangle<Chunk*> with that base's arguments.
            NamedType written{};
            written.kind = NodeKind::NamedType;
            written.token = n->token;
            written.name = n->name;
            if (const Ty* asType = typeOf(&written); asType && asType->isKnown())
              t = asType;
          }
          if (impl.isUndeducedClassTemplate(t))
            if (const Ty* deduced = impl.deduceClassArgs(t, argTypesOf(c->args)))
            {
              record(n, deduced);
              return deduced; // `Pair(a, b)`, `Span(p, n)`: the class's arguments from a constructor (CTAD)
            }
          return t;
        }
        return callResultOfValue(t, c);
      }
      if (callee->kind == NodeKind::MemberExpr)
      {
        auto* m = static_cast<MemberExpr*>(callee);
        checkPostfixAfterParenthesisedType(m->base, m->token);
        const Ty* base = walkExpr(m->base);
        for (const NamePart& part : m->member.parts)
          for (Node* arg : part.templateArgs)
            walkNode(arg);
        const Ty* object = (m->op == P::Arrow || m->op == P::SafeArrow) ? objectForArrow(base) : base;
        if (m->member.empty())
        {
          record(m, types.unknown());
          return unknown(Why::Unsupported);
        }
        const NamePart& last = m->member.last();
        if (!object || !object->isKnown())
        {
          ++stats.membersUnknownBase;
          if (last.symbol != UINT32_MAX)
            ++stats.unknownBaseMembers[last.symbol];
          record(m, types.unknown());
          return unknown(Why::Base);
        }
        if (last.special == 1)
        {
          ++stats.membersBound;
          record(m, types.function(types.voidType(), {}, false));
          return types.voidType();
        }
        if (last.special == 2 && P(last.opToken) == P::LParen && m->member.parts.size() == 1 && types.stripRef(object)->kind == TyKind::Lambda)
        {
          // `runTest.operator()<CraftingMachine>(name)`: the lambda called, its template arguments
          // those written, then those its parameters deduce.
          ++stats.membersBound;
          record(m, types.stripRef(object));
          return callResultOfValue(object, c, &last);
        }
        if (last.special == 3 && last.convType)
        {
          // `commands.operator AllowedCommands::Enum()`: the class's conversion function to that type.
          walkType(last.convType);
          Subst cs;
          if (FunctionDecl* conv = impl.conversionFunctionTo(types.unqualified(types.stripRef(object)), typeOf(last.convType), cs))
          {
            const Ty* ft = impl.declaredTypeOf(conv, cs.pairs.empty() ? nullptr : &cs, 0);
            m->resolved = conv;
            ++stats.membersBound;
            record(m, ft);
            return ft->inner;
          }
        }
        uint32_t symbol = last.special == 2 && last.opToken < uint32_t(P::Count) ? impl.opSymbols[last.opToken] : last.symbol;
        if (symbol == UINT32_MAX)
        {
          record(m, types.unknown());
          return unknown(Why::Unsupported);
        }
        if (const Ty* builtin = types.unqualified(types.stripRef(object)); m->op == P::Dot && m->member.parts.size() == 1 && !last.hasTemplateArgs &&
                                                                           isBuiltinAppendTo(builtin, symbol))
          return builtinMemberCall(c, m, base, builtin);
        MemberResult r = memberOf(object, symbol, &m->member, m->token);
        m->resolved = r.decl;
        if ((!r.decl || r.decl->kind != NodeKind::FunctionDecl) && last.special == 2 && P(last.opToken) == P::Assign && c->args.size() == 1)
        {
          // `this->CollisionMask::operator=(other)` of a class declaring none: its implicit
          // assignment, of the class the name is qualified with (or the object's), giving the object.
          const Ty* target = types.unqualified(types.stripRef(object));
          if (m->member.parts.size() > 1)
          {
            NamedType written{};
            written.kind = NodeKind::NamedType;
            written.token = m->token;
            written.name = m->member;
            written.name.parts.count -= 1;
            if (const Ty* q = typeOf(&written); q && q->isKnown() && types.unqualified(types.stripRef(q))->kind == TyKind::Class)
              target = types.unqualified(types.stripRef(q));
          }
          if (target && target->kind == TyKind::Class)
          {
            ++stats.callsResolved;
            m->resolved = nullptr; // (the qualifier's class, which the name found instead)
            record(m, types.function(types.lref(target), {}, false));
            return types.lref(target);
          }
        }
        if (!r.decl)
        {
          if (r.why == Why::Base)
            ++stats.membersUnknownBase;
          record(m, types.unknown());
          return r.why == Why::None ? types.unknown() : unknown(r.why);
        }
        if (r.decl->kind == NodeKind::FunctionDecl)
        {
          std::vector<FunctionDecl*> tied;
          std::vector<const Ty*> argTypes = argTypesOf(c->args);
          FunctionDecl* f = pickOverload(r.decl, r.overloads, argTypes, &r.subst, object, ambiguous, &last, c->args, &tied);
          if (f && !ambiguous)
          {
            checkConstObjectCall(f, object, m->token);
          }
          if (!f)
          {
            ++stats.membersBound;
            c->templateArgCount = 0; // as for a name
            if (!onlyPlainCandidate(r.decl, r.overloads))
              m->resolved = nullptr;
            else // `counter.add(item)` with a UniquePtr<Item>, as for a name
              checkScalarArguments(static_cast<FunctionDecl*>(r.decl), impl.declaredTypeOf(r.decl, &r.subst, 0), c->args);
            record(m, types.unknown());
            return unknown(Why::Overload);
          }
          if (ambiguous)
          {
            ++stats.callsAmbiguousArity;
            reportAmbiguousCall(c, last, argTypes, tied, &r.subst);
          }
          ++stats.callsResolved;
          m->resolved = f;
          const Ty* fnTy = impl.declaredTypeOf(f, &r.subst, 0);
          record(m, fnTy);
          if (fnTy && fnTy->kind == TyKind::Function)
            for (size_t i = 0; i < c->args.size() && i < fnTy->args.size(); ++i)
              noteZeroNull(c->args[i], fnTy->args[i]);
          if (types.isDependent(fnTy))
            ++stats.membersDependent;
          else
            ++stats.membersBound;
          bindOverloadSets(sets, fnTy);
          return returnTypeOfCall(f, &r.subst, c->args, &last, c);
        }
        if (r.type && types.isDependent(r.type))
          ++stats.membersDependent;
        else
          ++stats.membersBound;
        record(m, r.type ? r.type : types.unknown());
        if (r.why != Why::None)
          return unknown(r.why);
        return callResultOfValue(r.type, c);
      }
      const Ty* t = walkExpr(callee);
      return callResultOfValue(t, c);
    }

    // A call to a generic lambda: its template parameters are deduced from the arguments, its
    // body typed for them once per set of arguments (in the environment it was written in), and
    // the call has the result type found there. The deduced arguments are recorded on the call
    // for the code generator, as for function templates. Written arguments (`f.operator()<T>(x)`)
    // come first, the parameters deduce the rest.
    const Ty* genericLambdaCall(const Ty* t, CallExpr* c, const NamePart* written = nullptr)
    {
      t = types.unqualified(t); // `(*this->f)(x)` with a `const F*` f: the closure type the lambda's environment is kept for
      LambdaExpr* l = Impl::lambdaOf(t);
      std::vector<const Ty*> argTypes = argTypesOf(c->args);
      Subst deduced;
      std::vector<const Ty*> patterns(t->args.begin(), t->args.end());
      if (written && written->hasTemplateArgs)
      {
        bindExplicitArgs(l->templateParams, *written, c->args, deduced);
        for (const Ty*& pattern : patterns)
          pattern = impl.substitute(pattern, deduced);
      }
      impl.deduceParams(l->params, Span<const Ty*>{patterns.data(), uint32_t(patterns.size())}, argTypes, deduced);
      Subst own; // in parameter order: the instantiation's key
      for (Decl* p : l->templateParams)
      {
        const Ty* a = deduced.find(p);
        if (!a || !a->isKnown() || types.isDependent(a))
          return unknown(Why::Deduction);
        own.pairs.push_back({p, a});
      }
      if (own.pairs.size() > 8)
        return unknown(Why::Deduction);
      for (size_t i = 0; i < own.pairs.size(); ++i)
        c->templateArgs[i] = own.pairs[i].type;
      c->templateArgCount = uint8_t(own.pairs.size()); // the count last: other threads type the same call
      // The instantiation's own copy of the body, typed once (null while the lambda calls itself
      // through a capture).
      const Ty* callTy = instantiationDepth > 16 ? nullptr : impl.checkLambdaInstance(t, own, nullptr, &stats);
      return callTy && callTy->kind == TyKind::Function ? callTy->inner : unknown(Why::AutoReturn);
    }

    // Result of calling a value of type t: function pointers, lambdas, functors (written: the
    // template arguments of an explicit `operator()<...>`).
    const Ty* callResultOfValue(const Ty* t, CallExpr* c, const NamePart* written = nullptr)
    {
      t = types.stripRef(t);
      if (!t || !t->isKnown())
        return types.unknown();
      if (t->kind == TyKind::Function)
        return t->inner;
      if (t->kind == TyKind::Pointer && t->inner->kind == TyKind::Function)
        return t->inner->inner;
      if (t->kind == TyKind::Lambda)
      {
        if (c && Impl::isGenericLambda(t))
          return genericLambdaCall(t, c, written);
        return t->inner ? t->inner : unknown(Why::AutoReturn);
      }
      if (t->kind == TyKind::Class)
      {
        MemberResult r = memberOf(t, impl.opSymbols[uint32_t(P::LParen)], nullptr, c->token, false);
        if (r.decl && r.decl->kind == NodeKind::FunctionDecl)
        {
          bool ambiguous;
          if (FunctionDecl* f = pickOverload(r.decl, r.overloads, argTypesOf(c->args), &r.subst, t, ambiguous))
          {
            if (!ambiguous)
              checkConstObjectCall(f, t, c->token);
            return returnTypeOfCall(f, &r.subst, c->args, nullptr);
          }
          return unknown(Why::Overload);
        }
        return r.why == Why::Member ? unknown(Why::Member) : types.unknown();
      }
      if (t->kind == TyKind::Param)
        return t;
      return unknown(Why::Unsupported);
    }

    // --- operators ---

    const Ty* promote(const Ty* t)
    {
      t = types.unqualified(types.stripRef(t));
      switch (t->kind)
      {
        case TyKind::Bool: case TyKind::Char: return types.intType(32);
        case TyKind::Int: return t->bits < 32 ? types.intType(32) : t;
        case TyKind::Enum:
        {
          // One with a fixed underlying type promotes as that type does ([conv.prom]/4: an `enum :
          // long long` stays 64 bits, an `enum : uint8_t` is an int); another is an int (MSVC's).
          const Ty* u = impl.declaredUnderlying(t);
          return u && u->isKnown() && u->kind != TyKind::Enum ? promote(u) : types.intType(32);
        }
        default: return t;
      }
    }

    const Ty* arithmetic(const Ty* a, const Ty* b)
    {
      a = promote(a);
      b = promote(b);
      return types.usualArithmetic(a, b);
    }

    // Overloaded operator lookup: member of the left operand's class, then free functions in
    // the class's namespace and the current scope chain.
    // The function `lhs op rhs` (or `op lhs`) calls: the class's member operator, else a free one
    // from the namespaces of both operand classes or the current scope. For a member, subst is
    // the substitution of its class and isMember is set.
    FunctionDecl* operatorFunction(P op, const Ty* lhs, const Ty* rhs, uint32_t token, size_t arity, Subst& subst, bool& isMember)
    {
      FunctionDecl* constViolation = nullptr; // the class's own operator, non-const, on a const lhs
      Subst violationSubst;
      uint32_t symbol = impl.opSymbols[uint32_t(op)];
      const Ty* l = types.stripRef(lhs);
      isMember = false;
      if (l && l->kind == TyKind::Class)
      {
        MemberResult r = memberOf(l, symbol, nullptr, token, false);
        if (r.decl && r.decl->kind == NodeKind::FunctionDecl)
        {
          bool ambiguous;
          std::vector<const Ty*> argTypes;
          if (arity == 2)
            argTypes.push_back(rhs);
          if (FunctionDecl* f = pickOverload(r.decl, r.overloads, argTypes, &r.subst, lhs, ambiguous))
          {
            if (!nonConstOnConstObject(f, lhs))
            {
              subst = r.subst;
              isMember = true;
              return f;
            }
            constViolation = f;
            violationSubst = r.subst;
          }
        }
      }
      // Free function: look in the namespaces of both operand classes and enums and the current
      // scope; members are not candidates here (a class's own operator== hides no free one). The
      // operands' classes' friend operators are candidates beside them, with their classes'
      // arguments in subst.
      std::vector<const Ty*> argTypes{lhs};
      if (arity == 2)
        argTypes.push_back(rhs);
      Subst friendArgs;
      std::string conflict;
      Lookup friends = impl.hiddenFriendLookup(symbol, argTypes, friendArgs, &conflict);
      if (!conflict.empty())
        reportHere(token, conflict);
      auto pick = [&](const Lookup& look) -> FunctionDecl*
      {
        bool ambiguous;
        FunctionDecl* f = pickOverload(look.decl, look.overloads, argTypes, friendArgs.empty() ? nullptr : &friendArgs, nullptr, ambiguous);
        if (!f)
          return nullptr;
        // Against the built-in operator on the operands' conversions (no member operator here).
        switch (impl.freeOperatorAgainstBuiltin(f, (f->flags & DF_Friend) && !friendArgs.empty() ? &friendArgs : nullptr, op, argTypes))
        {
          case 0:
            return nullptr;
          case -1:
            reportHere(token, "the operator " + std::string(punctuatorSpelling(op)) + " on " + types.toString(types.unqualified(l), &symbols) +
                                  (arity == 2 ? " and " + types.toString(types.unqualified(types.stripRef(rhs)), &symbols) : std::string()) +
                                  " is ambiguous: the free operator" +
                                  std::string(punctuatorSpelling(op)) + " at " + impl.locationOf(f->file, f->token) +
                                  " takes an operand by a user-defined conversion, as the built-in operator does by a conversion operator");
            break;
        }
        if ((f->flags & DF_Friend) && !friendArgs.empty())
          subst = friendArgs;
        return f;
      };
      std::vector<Scope*> places;
      for (const Ty* t : {l, types.stripRef(rhs)})
        if (t && (t->kind == TyKind::Class || t->kind == TyKind::Enum) && t->decl)
          places.push_back(impl.lexicalScopeOf(t->decl));
      places.push_back(scope);
      for (Scope* place : places)
      {
        for (Scope* s = place; s; s = s->parent)
        {
          if (s->kind == ScopeKind::Class)
            continue;
          Lookup look;
          if (!impl.lookupInScope(s, symbol, look, 0))
            continue;
          if (look.decl && look.decl->kind == NodeKind::FunctionDecl)
            if (FunctionDecl* f = pick(friends.decl ? impl.mergeOverloads(look, friends) : look))
              return f;
          break;
        }
      }
      if (friends.decl)
        if (FunctionDecl* f = pick(friends))
          return f;
      if (constViolation)
      {
        // (no other operator takes it: the error, and the operator for the walk to go on with)
        checkConstObjectCall(constViolation, lhs, token);
        subst = violationSubst;
        isMember = true;
        return constViolation;
      }
      return nullptr;
    }

    // Whether `lhs <=> rhs` is a defaulted operator<=>, which gives the class `==` as well.
    bool threeWayDefaulted(const Ty* lhs, const Ty* rhs, uint32_t token)
    {
      Subst subst;
      bool isMember;
      FunctionDecl* f = operatorFunction(P::Spaceship, lhs, rhs, token, 2, subst, isMember);
      return f && (f->flags & DF_Default);
    }

    // The `==` a defaulted operator<=> declares implicitly takes the class itself, so `alertType ==
    // AlertType::ALERTS_COUNT` converts the enumerator to an AlertType (whatever `<=>` the class
    // also has for it): whether lhs's class has it and rhs converts to the class.
    bool implicitEquality(const Ty* lhs, const Ty* rhs, uint32_t token)
    {
      const Ty* cls = types.unqualified(types.stripRef(lhs));
      if (!cls || cls->kind != TyKind::Class || !threeWayDefaulted(cls, cls, token))
        return false;
      return impl.conversionRank(rhs, types.lref(types.qualified(cls, true, false))) < Impl::NotViable;
    }

    // The type of `lhs op rhs` (or `op lhs`) through an operator function, or for what every class
    // has; null when there is none (counted as unknown here only when countFailure). lhsOperand:
    // the left operand as an argument (an lvalue a reference), for a template's deduction.
    FunctionDecl* operatorChosen = nullptr; // overloadedOperator's operator function, null for none
    bool operatorIsMember = false;
    // overloadedOperator's decision (design.md 24): Function (operatorChosen), Rewritten, Builtin, or
    // Undecided when it found nothing.
    OperatorCall operatorCall = OperatorCall::Undecided;
    const Ty* overloadedOperator(P op, const Ty* lhs, const Ty* rhs, uint32_t token, size_t arity, bool countFailure = true,
                                 const Ty* lhsOperand = nullptr)
    {
      const Ty* l = types.stripRef(lhs);
      Subst subst;
      bool isMember;
      Span<Expr*> none;
      // The left operand as an argument (an lvalue a reference): a free `Stream& operator<<(Stream&,
      // const char*)` binds the stream lvalue.
      operatorChosen = nullptr;
      operatorCall = OperatorCall::Undecided;
      if (FunctionDecl* f = operatorFunction(op, lhsOperand ? lhsOperand : lhs, rhs, token, arity, subst, isMember))
      {
        operatorChosen = f;
        operatorIsMember = isMember;
        operatorCall = OperatorCall::Function;
        // The operands a template deduces from: a member's object is not one of them.
        std::vector<const Ty*> operands;
        if (!isMember)
          operands.push_back(lhsOperand ? lhsOperand : lhs);
        if (arity == 2)
          operands.push_back(rhs);
        return returnTypeOfCall(f, isMember || !subst.empty() ? &subst : nullptr, none, nullptr, nullptr, &operands);
      }
      if (arity == 2)
      {
        // The comparisons C++ rewrites when the class defines no operator for them: `a != b` is
        // `!(a == b)`, `a < b` and the other orderings are `(a <=> b) < 0`, and a defaulted
        // operator<=> gives `==` too.
        switch (op)
        {
          case P::NotEq:
            if (operatorFunction(P::EqEq, lhs, rhs, token, 2, subst, isMember) || threeWayDefaulted(lhs, rhs, token) ||
                operatorFunction(P::EqEq, rhs, lhs, token, 2, subst, isMember) || implicitEquality(lhs, rhs, token) ||
                implicitEquality(rhs, lhs, token))
            {
              operatorCall = OperatorCall::Rewritten; // `3 != id` through Id::operator==(int): the operands reversed
              return types.boolType();
            }
            break;
          case P::EqEq:
            if (threeWayDefaulted(lhs, rhs, token) || operatorFunction(P::EqEq, rhs, lhs, token, 2, subst, isMember) ||
                implicitEquality(lhs, rhs, token) || implicitEquality(rhs, lhs, token))
            {
              operatorCall = OperatorCall::Rewritten;
              return types.boolType();
            }
            break;
          case P::Less: case P::Greater: case P::LessEq: case P::GreaterEq:
            // `minimum < scale` through scale's operator<=>(const double&) too: C++20's reversed
            // candidate, `0 < (scale <=> minimum)`.
            if (operatorFunction(P::Spaceship, lhs, rhs, token, 2, subst, isMember) || operatorFunction(P::Spaceship, rhs, lhs, token, 2, subst, isMember))
            {
              operatorCall = OperatorCall::Rewritten;
              return types.boolType();
            }
            break;
          case P::Spaceship:
          {
            // `minimum <=> scale`: the reversed `0 <=> (scale <=> minimum)`, of that call's type.
            Subst reversed;
            if (FunctionDecl* f = operatorFunction(P::Spaceship, rhs, lhs, token, 2, reversed, isMember))
            {
              std::vector<const Ty*> operands;
              if (!isMember)
                operands.push_back(rhs);
              operands.push_back(lhs);
              operatorCall = OperatorCall::Rewritten;
              return returnTypeOfCall(f, isMember || !reversed.empty() ? &reversed : nullptr, none, nullptr, nullptr, &operands);
            }
            break;
          }
          default:
            break;
        }
      }
      // Operators every class has or that always yield bool.
      switch (op)
      {
        case P::Assign:                                   // implicit copy/move assignment
          if (l && l->kind == TyKind::Class)
            checkConstModification(token, l, "assignment to");
          operatorCall = OperatorCall::Builtin;
          return lhs;
        case P::AndAnd: case P::OrOr: case P::Bang:
          operatorCall = OperatorCall::Builtin;
          return types.boolType();
        case P::EqEq: case P::NotEq: case P::Less: case P::Greater: case P::LessEq: case P::GreaterEq:
          if (l && l->kind == TyKind::Enum)
          {
            operatorCall = OperatorCall::Builtin;
            return types.boolType();
          }
          break;
        default:
          break;
      }
      return countFailure ? unknown(Why::Operator) : nullptr;
    }

    static bool isAssignmentOp(P op)
    {
      switch (op)
      {
        case P::Assign: case P::PlusAssign: case P::MinusAssign: case P::StarAssign: case P::SlashAssign: case P::PercentAssign:
        case P::AmpAssign: case P::PipeAssign: case P::CaretAssign: case P::ShlAssign: case P::ShrAssign:
          return true;
        default:
          return false;
      }
    }

    // C++'s built-in candidates for an operator no function defines: a class operand takes part
    // through its conversion operator to a non-class type (`area < smallBlueprintAreaSize` with a
    // Const<float>). The operand's converted type, or null when it has no such conversion; an
    // operand that is no class stays as it is.
    const Ty* builtinOperand(const Ty* t)
    {
      const Ty* s = types.stripRef(t);
      if (!s || s->kind != TyKind::Class)
        return t;
      return impl.scalarConversionOf(s);
    }

    // A user-defined operator on an enumeration operand (`Flags operator|(Flags, Flags)`) beats the
    // built-in one, which a scoped enum does not even have. Only a free non-template function whose
    // parameters take the enum operands as they are (not through a conversion to some class).
    const Ty* enumOperator(P op, const Ty* lhs, const Ty* rhs, uint32_t token, FunctionDecl** chosen = nullptr)
    {
      Subst subst;
      bool isMember;
      FunctionDecl* f = operatorFunction(op, lhs, rhs, token, 2, subst, isMember);
      if (!f || isMember || !impl.takesEnumOperands(f, lhs, rhs))
        return nullptr;
      if (chosen)
        *chosen = f;
      Span<Expr*> none;
      return returnTypeOfCall(f, subst.empty() ? nullptr : &subst, none, nullptr);
    }

    // Two rules of design.md 14 on text. A string literal plus or minus an integer is C++'s pointer
    // arithmetic, never text: an error (`&"abc"[1]` writes the pointer). A `<<` chain into a String
    // mixed with `&`, `|` or `^` without parentheses binds as `(out << a) & b`: an error.
    static Expr* unparenthesized(Expr* e)
    {
      while (e && e->kind == NodeKind::ParenExpr)
        e = static_cast<ParenExpr*>(e)->inner;
      return e;
    }
    void checkTextOperators(BinaryExpr* b, P op, const Ty* l, const Ty* r)
    {
      if (probing || inGenericTemplate() || !file)
        return;
      auto isLiteral = [](Expr* e)
      {
        e = unparenthesized(e);
        return e && e->kind == NodeKind::LiteralExpr && static_cast<LiteralExpr*>(e)->literalKind == TokenKind::String;
      };
      auto isInteger = [&](const Ty* t)
      {
        t = t ? types.unqualified(types.stripRef(t)) : nullptr;
        return t && (t->kind == TyKind::Int || t->kind == TyKind::Char || t->kind == TyKind::Bool || t->kind == TyKind::Enum);
      };
      auto isPointer = [&](const Ty* t)
      {
        t = t ? types.unqualified(types.stripRef(t)) : nullptr;
        return t && (t->kind == TyKind::Pointer || t->kind == TyKind::Array);
      };
      if ((op == P::Plus || op == P::Minus) && ((isLiteral(b->lhs) && isPointer(r)) || (isLiteral(b->rhs) && isPointer(l))))
      {
        impl.reportCheckerError(file, b->token, std::string("a string literal ") + (op == P::Plus ? "+" : "-") +
                                                  " a pointer is no text: write StringView(\"...\") + x for text (design.md 14)");
        return;
      }
      if ((op == P::Plus || op == P::Minus) && ((isLiteral(b->lhs) && isInteger(r)) || (isLiteral(b->rhs) && isInteger(l))))
        impl.reportCheckerError(file, b->token, std::string("a string literal ") + (op == P::Plus ? "+" : "-") +
                                                  " an integer is pointer arithmetic, not text: write &\"...\"[n] for a pointer into it (design.md 14)");
      if (op != P::Amp && op != P::Pipe && op != P::Caret)
        return;
      Decl* string = impl.libraryDecl("String", NodeKind::ClassDecl);
      auto intoString = [&](Expr* e)
      {
        // (written without parentheses: a ParenExpr around it is the program's own grouping)
        if (!e || e->kind != NodeKind::BinaryExpr || static_cast<BinaryExpr*>(e)->isKeyword || P(static_cast<BinaryExpr*>(e)->op) != P::Shl)
          return false;
        while (e->kind == NodeKind::BinaryExpr && !static_cast<BinaryExpr*>(e)->isKeyword && P(static_cast<BinaryExpr*>(e)->op) == P::Shl)
          e = static_cast<BinaryExpr*>(e)->lhs;
        const Ty* t = e && e->ty ? types.unqualified(types.stripRef(e->ty)) : nullptr;
        return string && t && t->kind == TyKind::Class && t->decl == string;
      };
      if (intoString(b->lhs) || intoString(b->rhs))
      {
        const char* o = op == P::Amp ? "&" : op == P::Pipe ? "|" : "^";
        impl.reportCheckerError(file, b->token, std::string("a << chain into a String mixed with ") + o + " needs parentheses: `out << (a " + o +
                                                  " b)` (design.md 14)");
      }
    }

    // The type of `lhs op rhs`, and what the operator calls recorded on b (design.md 24).
    const Ty* typeOfBinary(BinaryExpr* b)
    {
      OperatorCall call = OperatorCall::Undecided;
      FunctionDecl* callee = nullptr;
      bool member = false;
      const Ty* t = typeOfBinaryOperator(b, call, callee, member);
      recordOperatorCall(b, t, b->lhs, b->rhs, call, callee, member);
      return t;
    }

    // Records on an operator's node what it calls: call as the class branch decided it, else the
    // built-in operator when both operands are known and not dependent (a template's own walk leaves
    // its dependent operators undecided). A probe of shared nodes records nothing.
    template<class E>
    void recordOperatorCall(E* e, const Ty* t, Expr* lhs, Expr* rhs, OperatorCall call, FunctionDecl* callee, bool member)
    {
      if (probesSharedNodes())
        return;
      if (call == OperatorCall::Undecided && t && t->isKnown() && !types.isDependent(t))
      {
        auto plain = [&](Expr* x) { return !x || (x->ty && x->ty->isKnown() && !types.isDependent(x->ty)); };
        if (plain(lhs) && plain(rhs))
          call = OperatorCall::Builtin;
      }
      e->call = call;
      e->callee = call == OperatorCall::Function ? callee : nullptr;
      e->calleeMember = call == OperatorCall::Function && member;
    }

    const Ty* typeOfBinaryOperator(BinaryExpr* b, OperatorCall& call, FunctionDecl*& callee, bool& member)
    {
      const Ty* l = walkExpr(b->lhs);
      const Ty* r = walkExpr(b->rhs);
      if (!b->isKeyword && (P(b->op) == P::Assign || P(b->op) == P::EqEq || P(b->op) == P::NotEq))
      {
        noteZeroNull(b->rhs, l); // `p = 0`, `p == 0`
        if (P(b->op) != P::Assign)
          noteZeroNull(b->lhs, r); // `0 != p`
      }
      if (!b->isKeyword && P(b->op) == P::Assign && l && !probesSharedNodes())
        if (const Ty* target = types.unqualified(types.stripRef(l));
            (target->kind == TyKind::Pointer || target->kind == TyKind::MemberPointer) && target->inner && target->inner->kind == TyKind::Function)
        {
          // `h = &twice;` beside a twice(int), `make = construct<Add>;`: the function of the pointer's type, as for an
          // initializer (bindTemplateAddresses).
          bindTemplateAddresses(b->rhs, target);
          r = b->rhs->ty ? b->rhs->ty : r;
        }
      if (b->isKeyword)
      {
        switch (Kw(b->op))
        {
          case Kw::and_: case Kw::or_: case Kw::not_eq_: return types.boolType();
          default: return arithmetic(l, r);
        }
      }
      P op = P(b->op);
      if (op == P::Comma)
        return r;
      checkTextOperators(b, op, l, r);
      const Ty* ls = types.stripRef(l);
      const Ty* rs = types.stripRef(r);
      if (!ls || !rs)
        return types.unknown();
      if (op == P::DotStar || op == P::ArrowStar)
        return rs->kind == TyKind::MemberPointer ? types.lref(rs->inner) : types.unknown();
      if ((op == P::EqEq || op == P::NotEq) &&
          ((typedUnionOf(ls) && rs->kind == TyKind::Enum) || (typedUnionOf(rs) && ls->kind == TyKind::Enum)))
        return types.boolType(); // `p == Property::Integer`: the typed union's tag (design.md 8.5)
      if (ls->kind == TyKind::Class || rs->kind == TyKind::Class)
      {
        if (!ls->isKnown() || !rs->isKnown())
          return types.unknown();
        if (types.isDependent(ls) || types.isDependent(rs))
          return types.param(ls->kind == TyKind::Param ? ls->decl : rs->decl);
        if (op == P::Assign)
          checkClosureTo(b->rhs, l, b->token); // `later = [=] { ... text ... }`
        if (const Ty* t = overloadedOperator(op, l, argumentType(b->rhs, r, types), b->token, 2, false, argumentType(b->lhs, l, types)))
        {
          call = operatorCall;
          callee = operatorChosen;
          member = operatorIsMember;
          if (FunctionDecl* f = operatorChosen)
          {
            // `out << point` through `operator<<(String&, Text)`: the right operand packed (packText).
            Expr* operands[2] = {b->lhs, b->rhs};
            Span<Expr*> args{operatorIsMember ? operands + 1 : operands, operatorIsMember ? 1u : 2u};
            packText(args, f, b->token);
            if (args.size() > 0 && args[args.size() - 1] != b->rhs)
              b->rhs = args[args.size() - 1];
          }
          return t;
        }
        // Two typed unions of one type with no operator of their own compare as std::variant's
        // (design.md 8.5): by the alternative held, then by its values.
        if (typedUnionOf(ls) && types.unqualified(ls) == types.unqualified(rs))
          switch (op)
          {
            case P::EqEq: case P::NotEq: case P::Less: case P::Greater: case P::LessEq: case P::GreaterEq:
              return types.boolType();
            case P::Spaceship:
              return impl.orderingType();
            default:
              break;
          }
        // No operator function: the built-in operator on the operands' scalar conversions (an
        // assignment's left operand is the object itself and converts to nothing).
        const Ty* lc = isAssignmentOp(op) ? (ls->kind == TyKind::Class ? nullptr : l) : builtinOperand(l);
        const Ty* rc = builtinOperand(r);
        if (!lc || !rc)
          return unknown(Why::Operator);
        l = lc;
        r = rc;
        ls = types.stripRef(l);
        rs = types.stripRef(r);
      }
      if (!ls->isKnown() || !rs->isKnown())
      {
        // Comparisons and logic have a known result regardless of the operands.
        switch (op)
        {
          case P::EqEq: case P::NotEq: case P::Less: case P::Greater: case P::LessEq: case P::GreaterEq:
          case P::AndAnd: case P::OrOr:
            return types.boolType();
          default:
            if (op == P::Assign || op == P::PlusAssign || op == P::MinusAssign || op == P::StarAssign ||
                op == P::SlashAssign || op == P::PercentAssign || op == P::AmpAssign || op == P::PipeAssign ||
                op == P::CaretAssign || op == P::ShlAssign || op == P::ShrAssign)
              return ls->isKnown() ? l : types.unknown();
            return types.unknown();
        }
      }
      if (ls->kind == TyKind::Param || rs->kind == TyKind::Param)
        return ls->kind == TyKind::Param ? ls : rs;
      if ((ls->kind == TyKind::Enum || rs->kind == TyKind::Enum) && op != P::Assign)
        if (const Ty* user = enumOperator(op, argumentType(b->lhs, l, types), argumentType(b->rhs, r, types), b->token, &callee))
        {
          call = OperatorCall::Function;
          member = false;
          return user;
        }
      switch (op)
      {
        case P::Assign: case P::PlusAssign: case P::MinusAssign: case P::StarAssign: case P::SlashAssign:
        case P::PercentAssign: case P::AmpAssign: case P::PipeAssign: case P::CaretAssign: case P::ShlAssign:
        case P::ShrAssign:
          checkConstModification(b->token, ls, "assignment to");
          return l;
        case P::EqEq: case P::NotEq: case P::Less: case P::Greater: case P::LessEq: case P::GreaterEq:
        case P::AndAnd: case P::OrOr:
          return types.boolType();
        case P::Spaceship:
          return impl.orderingType();
        case P::Shl: case P::Shr:
          return promote(l);
        case P::Plus: case P::Minus:
        {
          bool lp = ls->kind == TyKind::Pointer || ls->kind == TyKind::Array;
          bool rp = rs->kind == TyKind::Pointer || rs->kind == TyKind::Array;
          if (lp && rp)
            return types.ptrdiffType();
          if (lp)
            return types.decay(ls);
          if (rp)
            return types.decay(rs);
          return arithmetic(l, r);
        }
        case P::Star: case P::Slash: case P::Percent: case P::Amp: case P::Pipe: case P::Caret:
          return arithmetic(l, r);
        case P::DotStar: case P::ArrowStar:
          return rs->kind == TyKind::MemberPointer ? types.lref(rs->inner) : types.unknown();
        default:
          return unknown(Why::Unsupported);
      }
    }

    // `forward<Args>(args)...` under an instantiation: the pattern typed once per element with the
    // packs bound to that element, as C++ instantiates an expansion; its type the pack of the
    // results (Args = (int, String&): (int&&, String&)). Null when the pattern names no pack
    // parameter bound to a pack here.
    const Ty* typeOfExpansion(UnaryExpr* u)
    {
      Decl* pd = activeSubst ? packParamIn(u->operand) : nullptr;
      const Ty* pt = pd ? impl.declaredTypeOf(pd, activeSubst, 0) : nullptr;
      if (!pt || pt->kind != TyKind::Pack)
        return nullptr;
      // An expansion of the packs whose one element the code generator emits (elementPacks): its
      // pattern typed once, for that element, as typeOfUnary types an expansion of no pack.
      if (elementPacks && !elementPacks->empty() && pd->kind == NodeKind::ParamDecl)
      {
        std::vector<Decl*> packs;
        impl.packsNamedIn(static_cast<ParamDecl*>(pd)->type, *activeSubst, packs);
        for (Decl* p : packs)
          if (std::find(elementPacks->begin(), elementPacks->end(), p) != elementPacks->end())
            return nullptr;
      }
      size_t n = pt->args.size();
      if (n == 0)
      {
        // No element: C++ instantiates no pattern, so nothing is typed or counted; its names are
        // still resolved, on a scratch walk, for the code generator, which finds the pack
        // through them.
        ResolveStats scratch;
        Walker probe(impl, scratch, tokens, text);
        probe.file = file;
        FileOfLookups walkedFile(file);
        probe.scope = scope;
        probe.locals = locals;
        probe.activeSubst = activeSubst;
        probe.thisType = thisType;
        probe.walkExpr(u->operand);
        return types.pack({});
      }
      const Subst* outer = activeSubst;
      std::vector<const Ty*> elements;
      for (size_t i = 0; i < n; ++i)
      {
        Subst one;
        for (const Subst::Pair& p : outer->pairs)
        {
          bool pack = p.param && p.param->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(p.param)->isPack;
          one.pairs.push_back({p.param, pack && p.type && p.type->kind == TyKind::Pack && p.type->args.size() == n ? p.type->args[i] : p.type});
        }
        activeSubst = &one;
        const Ty* e = walkExpr(u->operand);
        activeSubst = outer;
        elements.push_back(e ? e : types.unknown());
      }
      return types.pack(Span<const Ty*>{elements.data(), uint32_t(elements.size())});
    }

    // The type of `op operand`, and what the operator calls recorded on u (design.md 24).
    const Ty* typeOfUnary(UnaryExpr* u)
    {
      OperatorCall call = OperatorCall::Undecided;
      FunctionDecl* callee = nullptr;
      bool member = false;
      const Ty* t = typeOfUnaryOperator(u, call, callee, member);
      recordOperatorCall(u, t, u->operand, nullptr, call, callee, member);
      return t;
    }

    const Ty* typeOfUnaryOperator(UnaryExpr* u, OperatorCall& call, FunctionDecl*& callee, bool& member)
    {
      if (!u->isKeyword && P(u->op) == P::Ellipsis && u->postfix)
        if (const Ty* pack = typeOfExpansion(u))
          return pack;
      const Ty* t = walkExpr(u->operand);
      const Ty* s = types.stripRef(t);
      if (u->isKeyword)
        return Kw(u->op) == Kw::not_ ? types.boolType() : promote(t);
      P op = P(u->op);
      if (op == P::Amp && !u->postfix && t && impl.isText(t) && !inGenericTemplate())
        reportTextError(u->token, textStored("pointed to"));
      if (op == P::Ellipsis)
        return t;
      if (!s || !s->isKnown())
        return op == P::Bang ? types.boolType() : types.unknown();
      if (op == P::Amp && u->operand->kind == NodeKind::NameExpr)
      {
        // `&Style::field`: a pointer to a member of the class, not to the field of some object.
        auto* n = static_cast<NameExpr*>(u->operand);
        Decl* d = n->resolved;
        if (n->name.parts.size() > 1 && d && (d->kind == NodeKind::VariableDecl || d->kind == NodeKind::FunctionDecl) &&
            !(d->flags & DF_Static) && d->parent && d->parent->kind == NodeKind::ClassDecl)
          return types.memberPointer(s, impl.selfType(d->parent));
      }
      if (s->kind == TyKind::Param)
        return op == P::Bang ? types.boolType() : op == P::Amp ? types.pointer(s) : s;
      if (s->kind == TyKind::Class)
      {
        // `&x` of a class with its own unary operator& is that operator's result (CComPtr<T>'s
        // gives T**), the built-in address only where the class has none.
        if (op == P::Amp)
        {
          if (!types.isDependent(s))
            if (const Ty* r = overloadedOperator(op, t, nullptr, u->token, 1, false, argumentType(u->operand, t, types)))
            {
              call = operatorCall;
              callee = operatorChosen;
              member = operatorIsMember;
              return r;
            }
          return types.pointer(s);
        }
        if (types.isDependent(s))
          return types.unknown();
        if (const Ty* r = overloadedOperator(op, t, nullptr, u->token, 1, false, argumentType(u->operand, t, types)))
        {
          call = operatorCall;
          callee = operatorChosen;
          member = operatorIsMember;
          return r;
        }
        // No operator function: the built-in one on the operand's scalar conversion (`-speed`
        // with a Const<double>); ++ and -- change the object itself.
        const Ty* c = op == P::PlusPlus || op == P::MinusMinus ? nullptr : builtinOperand(t);
        if (!c)
          return unknown(Why::Operator);
        t = c;
        s = types.stripRef(c);
      }
      switch (op)
      {
        case P::Star:
          if (s->kind == TyKind::Pointer || s->kind == TyKind::Array)
            return types.lref(s->inner);
          return unknown(Why::Operator);
        case P::Amp: return types.pointer(s);
        case P::Bang: return types.boolType();
        case P::Minus: case P::Plus: case P::Tilde: return promote(t);
        case P::PlusPlus: case P::MinusMinus:
          checkConstModification(u->token, s, op == P::PlusPlus ? "increment of" : "decrement of");
          return u->postfix ? types.unqualified(s) : t;
        default: return unknown(Why::Unsupported);
      }
    }

    // --- literals ---

    const Ty* typeOfLiteral(LiteralExpr* l)
    {
      switch (l->literalKind)
      {
        case TokenKind::Keyword:
          return l->keyword == Kw::nullptr_ ? types.nullptrType() : types.boolType();
        case TokenKind::Char:
        {
          std::string_view s = spelling(l);
          if (s.starts_with("u8")) return types.charType(8, true);
          if (s.starts_with("u")) return types.charType(16, true);
          if (s.starts_with("U")) return types.charType(32, true);
          if (s.starts_with("L")) return types.wcharType();
          return types.charType(8, false);
        }
        case TokenKind::String:
        {
          std::string_view s = spelling(l);
          const Ty* ch = types.charType(8, false);
          if (s.starts_with("u8")) ch = types.charType(8, true);
          else if (s.starts_with("u")) ch = types.charType(16, true);
          else if (s.starts_with("U")) ch = types.charType(32, true);
          else if (s.starts_with("L")) ch = types.wcharType();
          return types.pointer(types.qualified(ch, true, false));
        }
        case TokenKind::Number:
        {
          // The type the compile-time evaluator gives the literal (consteval.cpp: C++'s rules for
          // suffixes and the first type that holds the value), so that the two agree.
          ConstArith arith{types};
          if (std::optional<ConstValue> v = arith.literal(TokenKind::Number, spelling(l)); v && v->type)
            return v->type;
          return types.unknown();
        }
        default:
          return types.unknown();
      }
    }

    // --- element type for range-for ---

    // The iterator type of the free begin(range) found for a class without a begin() member (in
    // the class's namespace or from here), the call a range-for makes then; null if none.
    const Ty* freeBeginType(const Ty* t, Expr* rangeExpr)
    {
      std::vector<const Ty*> argTypes{types.lref(t)};
      for (Scope* place : {impl.lexicalScopeOf(t->decl), scope})
        for (Scope* s = place; s; s = s->parent)
        {
          if (s->kind == ScopeKind::Class)
            continue;
          Lookup look;
          if (!impl.lookupInScope(s, impl.symBegin, look, 0))
            continue;
          if (look.decl && look.decl->kind == NodeKind::FunctionDecl)
          {
            bool ambiguous;
            if (FunctionDecl* f = pickOverload(look.decl, look.overloads, argTypes, nullptr, nullptr, ambiguous))
              return returnTypeOfCall(f, nullptr, Span<Expr*>{&rangeExpr, 1}, nullptr);
          }
          break;
        }
      return nullptr;
    }

    // A data member declared as an array without a length (`int items[];`: a flexible array
    // member), as e names it (`s->items`, `items` in a member function), or a lone name in sizeof's
    // type names it; else null. (`extern int table[];` takes its length from its definition.)
    VariableDecl* unboundedArray(Node* e)
    {
      while (e && e->kind == NodeKind::ParenExpr)
        e = static_cast<ParenExpr*>(e)->inner;
      Decl* d = !e ? nullptr
              : e->kind == NodeKind::MemberExpr ? static_cast<MemberExpr*>(e)->resolved
              : e->kind == NodeKind::NameExpr   ? static_cast<NameExpr*>(e)->resolved
              : e->kind == NodeKind::NamedType  ? static_cast<NamedType*>(e)->resolved
                                                : nullptr;
      auto* v = d && d->kind == NodeKind::VariableDecl && !(d->flags & DF_Typedef) ? static_cast<VariableDecl*>(d) : nullptr;
      bool member = v && !(v->flags & DF_Static) && impl.owningClassOf(v);
      return member && v->type && v->type->kind == NodeKind::ArrayType && !static_cast<ArrayType*>(v->type)->size ? v : nullptr;
    }

    // The variable declared const (a local, a parameter by value, a global or a static) whose
    // object, or a member or element of it, a placement new's place visibly names: `&limit`,
    // `&limit.x`, `&table[i]`, `table` (an array); else null. Through a pointer (`p->x`, `*p`, a
    // cast, const_cast included) nothing is visible (design.md 21).
    Decl* constObjectPlaced(Expr* place)
    {
      auto bare = [](Expr* e)
      {
        while (e && e->kind == NodeKind::ParenExpr)
          e = static_cast<ParenExpr*>(e)->inner;
        return e;
      };
      Expr* e = bare(place);
      bool addressTaken = false;
      if (e && e->kind == NodeKind::UnaryExpr)
      {
        auto* u = static_cast<UnaryExpr*>(e);
        if (u->isKeyword || u->postfix || u->op != uint32_t(P::Amp))
          return nullptr;
        e = bare(u->operand);
        addressTaken = true;
      }
      auto declaredArray = [&](Decl* d)
      {
        const Ty* t = d ? impl.declaredTypeOf(d, activeSubst, 0) : nullptr;
        return t && t->kind == TyKind::Array;
      };
      for (;;)
      {
        if (e && e->kind == NodeKind::MemberExpr && static_cast<MemberExpr*>(e)->op == P::Dot && addressTaken)
          e = bare(static_cast<MemberExpr*>(e)->base);
        else if (e && e->kind == NodeKind::IndexExpr && addressTaken)
        {
          Expr* base = bare(static_cast<IndexExpr*>(e)->base);
          if (!base || base->kind != NodeKind::NameExpr || !declaredArray(static_cast<NameExpr*>(base)->resolved))
            return nullptr; // (a pointer's element: not visibly the object)
          e = base;
        }
        else
          break;
      }
      if (!e || e->kind != NodeKind::NameExpr)
        return nullptr;
      Decl* d = static_cast<NameExpr*>(e)->resolved;
      if (!d || (d->kind != NodeKind::VariableDecl && d->kind != NodeKind::ParamDecl) || (d->flags & DF_Typedef))
        return nullptr;
      if (!addressTaken && !declaredArray(d))
        return nullptr; // (a pointer variable's value is the place, not its own object)
      const Ty* t = impl.declaredTypeOf(d, activeSubst, 0);
      if (!t || t->kind == TyKind::LRef || t->kind == TyKind::RRef)
        return nullptr; // (a reference names an object declared elsewhere, maybe not const)
      while (t && t->kind == TyKind::Array)
        t = t->inner;
      return t && t->isConst ? d : nullptr;
    }

    // Whether e can throw, for the noexcept operator ([expr.unary.noexcept]): a call of a function
    // not noexcept (its noexcept condition evaluated under the walk's substitution), a throw, a new
    // that allocates; scalar arithmetic, names, literals and the operators of scalars cannot. What it
    // does not decide yet (an overloaded operator, a constructor, a call through a pointer) is named
    // in unsupported, an error, never a guess.
    bool canThrow(Expr* e, std::string& unsupported)
    {
      if (!e || !unsupported.empty())
        return false;
      auto scalar = [&](const Expr* x)
      {
        const Ty* t = x && x->ty ? types.unqualified(types.stripRef(x->ty)) : nullptr;
        return t && t->isKnown() && t->kind != TyKind::Class;
      };
      switch (e->kind)
      {
        case NodeKind::LiteralExpr: case NodeKind::NameExpr: case NodeKind::ThisExpr: case NodeKind::SizeofExpr:
        case NodeKind::NameofExpr: case NodeKind::NoexceptExpr:
          return false;
        case NodeKind::ParenExpr: return canThrow(static_cast<ParenExpr*>(e)->inner, unsupported);
        case NodeKind::ThrowExpr: return true;
        case NodeKind::UnaryExpr:
          if (!scalar(static_cast<UnaryExpr*>(e)->operand))
            break; // (an overloaded operator)
          return canThrow(static_cast<UnaryExpr*>(e)->operand, unsupported);
        case NodeKind::BinaryExpr:
        {
          auto* b = static_cast<BinaryExpr*>(e);
          if (!scalar(b->lhs) || !scalar(b->rhs))
            break;
          bool l = canThrow(b->lhs, unsupported), r = canThrow(b->rhs, unsupported);
          return l || r;
        }
        case NodeKind::ConditionalExpr:
        {
          auto* c = static_cast<ConditionalExpr*>(e);
          if (!scalar(e))
            break;
          bool any = canThrow(c->cond, unsupported);
          any = canThrow(c->then, unsupported) || any;
          return canThrow(c->else_, unsupported) || any;
        }
        case NodeKind::CallExpr:
        {
          auto* c = static_cast<CallExpr*>(e);
          Decl* callee = c->callee && c->callee->kind == NodeKind::NameExpr     ? static_cast<NameExpr*>(c->callee)->resolved
                         : c->callee && c->callee->kind == NodeKind::MemberExpr ? static_cast<MemberExpr*>(c->callee)->resolved
                                                                                : nullptr;
          if (!callee || callee->kind != NodeKind::FunctionDecl || (callee->flags & DF_Constructor))
          {
            unsupported = callee && (callee->kind == NodeKind::ClassDecl || (callee->flags & DF_Constructor)) ? "a construction of a class"
                                                                                                              : "a call through a pointer or a closure";
            return false;
          }
          if (!scalar(e) && !(e->ty && e->ty->kind == TyKind::Void))
          {
            unsupported = "a call returning a class (its construction and destruction)";
            return false;
          }
          bool any = false;
          for (Expr* a : c->args)
          {
            if (!scalar(a))
            {
              unsupported = "a call with an argument of class type";
              return false;
            }
            any = canThrow(a, unsupported) || any;
          }
          if (c->callee->kind == NodeKind::MemberExpr)
            any = canThrow(static_cast<MemberExpr*>(c->callee)->base, unsupported) || any;
          std::optional<bool> noexcept_ = impl.isNoexcept(static_cast<FunctionDecl*>(callee), activeSubst);
          if (!noexcept_)
          {
            unsupported = "a call of a function whose noexcept condition is no constant here";
            return false;
          }
          return any || !*noexcept_;
        }
        default:
          break;
      }
      unsupported = std::string("its operand (") + nodeKindName(e->kind) + ")";
      return false;
    }

    // --zero-null-warnings (design.md 4): e, the literal 0 of kpp's own sources (in parentheses or
    // not), made a pointer or member pointer of type target without a cast.
    void noteZeroNull(Expr* e, const Ty* target)
    {
      if (!zeroNullWarningsOn() || !file || file->isImported || probing || inGenericTemplate())
        return;
      target = target ? types.unqualified(types.stripRef(target)) : nullptr;
      if (!target || (target->kind != TyKind::Pointer && target->kind != TyKind::MemberPointer))
        return;
      while (e && e->kind == NodeKind::ParenExpr)
        e = static_cast<ParenExpr*>(e)->inner;
      if (!e || e->kind != NodeKind::LiteralExpr || static_cast<LiteralExpr*>(e)->literalKind != TokenKind::Number)
        return;
      auto* l = static_cast<LiteralExpr*>(e);
      const ParsedFile* in = l->file ? l->file : file;
      if (!in || in->isImported || !in->tokens || l->token >= in->tokens->size())
        return;
      const Token& t = (*in->tokens)[l->token];
      std::string_view text = in->text.substr(t.offset, t.length);
      static const char* zeros[] = {"0", "0u", "0U", "0l", "0L", "0ll", "0LL", "0ul", "0UL", "0ull", "0ULL"};
      if (std::none_of(std::begin(zeros), std::end(zeros), [&](const char* z) { return text == z; }))
        return;
      noteZeroNullWarning(Impl::locationOf(in, l->token) + ": warning: the literal 0 is no pointer: write nullptr (design.md 4)");
    }

    // rangeExpr: the range written, for a free begin(range).
    const Ty* elementTypeOf(const Ty* range, Expr* rangeExpr)
    {
      const Ty* t = types.stripRef(range);
      if (!t || !t->isKnown())
        return types.unknown();
      if (t->kind == TyKind::Array || t->kind == TyKind::Pointer)
        return types.lref(t->inner);
      if (t->kind == TyKind::Class)
      {
        if (types.isDependent(t))
          return types.unknown();
        MemberResult begin = memberOf(t, impl.symBegin, nullptr, 0, false);
        const Ty* iterator = nullptr;
        if (begin.decl && begin.decl->kind == NodeKind::FunctionDecl)
        {
          bool ambiguous;
          Span<Expr*> none;
          if (FunctionDecl* f = pickOverload(begin.decl, begin.overloads, {}, &begin.subst, t, ambiguous))
            iterator = returnTypeOfCall(f, &begin.subst, none, nullptr);
        }
        else if (!begin.decl && rangeExpr)
          iterator = freeBeginType(t, rangeExpr);
        if (!iterator || !iterator->isKnown())
          return types.unknown();
        const Ty* it = types.stripRef(iterator);
        if (it->kind == TyKind::Pointer)
          return types.lref(it->inner);
        if (it->kind == TyKind::Class && !types.isDependent(it))
        {
          MemberResult deref = memberOf(it, impl.opSymbols[uint32_t(P::Star)], nullptr, 0, false);
          if (deref.decl && deref.decl->kind == NodeKind::FunctionDecl)
          {
            bool ambiguous;
            Span<Expr*> none;
            if (FunctionDecl* f = pickOverload(deref.decl, deref.overloads, {}, &deref.subst, it, ambiguous))
              return returnTypeOfCall(f, &deref.subst, none, nullptr);
          }
        }
        return types.unknown();
      }
      return types.unknown();
    }

    // --- generic walking ---

    void walkNode(Node* n)
    {
      if (!n)
        return;
      if (uint8_t(n->kind) <= uint8_t(NodeKind::PackType))
        walkType(static_cast<Type*>(n));
      else if (uint8_t(n->kind) >= uint8_t(NodeKind::NameExpr))
        walkExpr(static_cast<Expr*>(n));
      else if (uint8_t(n->kind) >= uint8_t(NodeKind::BlockStmt))
        walkStmt(static_cast<Stmt*>(n));
      else
        walkDecl(static_cast<Decl*>(n));
    }

    void walkType(Type* t)
    {
      if (!t)
        return;
      switch (t->kind)
      {
        case NodeKind::NamedType:
          walkType(static_cast<NamedType*>(t)->name.typeQualifier); // `decltype(x)::member`: x typed first
          resolveTypeName(static_cast<NamedType*>(t));
          break;
        case NodeKind::PointerType: walkType(static_cast<PointerType*>(t)->pointee); break;
        case NodeKind::LRefType: walkType(static_cast<LRefType*>(t)->referenced); break;
        case NodeKind::RRefType: walkType(static_cast<RRefType*>(t)->referenced); break;
        case NodeKind::ConstType: walkType(static_cast<ConstType*>(t)->inner); break;
        case NodeKind::PackType: walkType(static_cast<PackType*>(t)->pattern); break;
        case NodeKind::ArrayType:
          walkType(static_cast<ArrayType*>(t)->element);
          walkExpr(static_cast<ArrayType*>(t)->size);
          break;
        case NodeKind::FunctionType:
        {
          auto* f = static_cast<FunctionType*>(t);
          walkType(f->result);
          size_t mark = locals.mark();
          for (Decl* p : f->params)
            walkParam(static_cast<ParamDecl*>(p));
          locals.pop(mark);
          break;
        }
        case NodeKind::DecltypeType: walkExpr(static_cast<DecltypeType*>(t)->expr); break;
        case NodeKind::MemberPointerType:
          walkType(static_cast<MemberPointerType*>(t)->classType);
          walkType(static_cast<MemberPointerType*>(t)->pointee);
          break;
        default: break;
      }
    }

    // A declaration's type that names a variable (`a * b;`, `a b;`, a parameter `Box box(four)` at
    // namespace scope): an error (design.md 22), never the variable's type, which is the reading only
    // sizeof's operand has (`sizeof(prefixes)`).
    void checkTypeNamesNoVariable(Node* base, uint32_t token)
    {
      for (;;)
        if (base && base->kind == NodeKind::PointerType)
          base = static_cast<PointerType*>(base)->pointee;
        else if (base && base->kind == NodeKind::LRefType)
          base = static_cast<LRefType*>(base)->referenced;
        else if (base && base->kind == NodeKind::RRefType)
          base = static_cast<RRefType*>(base)->referenced;
        else if (base && base->kind == NodeKind::ConstType)
          base = static_cast<ConstType*>(base)->inner;
        else
          break;
      Decl* named = base && base->kind == NodeKind::NamedType ? static_cast<NamedType*>(base)->resolved : nullptr;
      if (named && (named->kind == NodeKind::ParamDecl || (named->kind == NodeKind::VariableDecl && !(named->flags & DF_Typedef))) && file &&
          !file->isImported)
        impl.reportCheckerError(file, token, "'" + std::string(symbols.name(static_cast<NamedType*>(base)->name.last().symbol)) +
                                                 "' is a variable, not a type: `a * b;` and `a b;` declare b, `T f(a)` at namespace scope a function (design.md 22)");
    }

    void walkParam(ParamDecl* p)
    {
      walkType(p->type);
      walkExpr(p->defaultValue);
      locals.push(p->name, p);
    }

    void pushTemplateParams(Span<Decl*> params)
    {
      for (Decl* d : params)
      {
        auto* p = static_cast<TemplateParamDecl*>(d);
        walkType(p->valueType);
        walkNode(p->defaultArg);
        locals.push(p->name, p);
      }
    }

    // Scope in which the body of a (possibly out-of-line) member is resolved.
    Scope* semanticScopeFor(const QualifiedName& name)
    {
      if (name.parts.size() <= 1)
        return scope;
      QualifiedName qualifier = name;
      qualifier.parts.count -= 1;
      Lookup l = impl.lookupQualified(qualifier, scope, &locals, 0);
      if (l.scope && l.scope->kind == ScopeKind::Class)
        if (Scope* special = impl.fullSpecializationScope(qualifier, scope, &locals, file, 0))
          return special; // `int Flag<true>::value() {...}`: the body of the specialisation's member
      return l.scope ? l.scope : scope;
    }

    // A probe of a requires-expression under arguments walks the template's own nodes, which other
    // walks share: nothing it finds is cached on them. (An instance's walk has its own copy.)
    bool probesSharedNodes() const { return probing && activeSubst; }

    // A template's own walk: a template parameter in scope that this walk binds to nothing.
    // Whether a function's signature is typed by its walk: a decltype or a deduced auto in its
    // return type or a parameter's.
    static bool signatureFromWalk(FunctionDecl* f)
    {
      bool found = false;
      auto look = [&](Node* t)
      {
        if (t)
          forEachNode(t, [&](Node* n) { found = found || n->kind == NodeKind::DecltypeType || n->kind == NodeKind::AutoType; });
      };
      look(f->returnType);
      look(f->trailingReturn);
      for (Decl* p : f->params)
        look(static_cast<ParamDecl*>(p)->type);
      return found;
    }

    bool inGenericTemplate() const
    {
      for (const LocalStack::Item& item : locals.items)
        if (item.decl && item.decl->kind == NodeKind::TemplateParamDecl && (!activeSubst || !activeSubst->find(item.decl)))
          return true;
      return false;
    }

    // Whether d is in an instance of a function template that an explicit specialisation replaces
    // for these arguments (`template<> SpriteDrawOrder* DrawOrderProvider::allocate()`): that body
    // is not the one the program runs.
    bool replacedBySpecialization(Decl* d)
    {
      if (!activeSubst)
        return false;
      for (Decl* p = d->parent; p; p = p->parent)
        if (p->kind == NodeKind::FunctionDecl)
        {
          auto* f = static_cast<FunctionDecl*>(p);
          return !f->templateParams.empty() && impl.functionSpecializationFor(f, *activeSubst);
        }
      return false;
    }

    // The instance this walk types, for messages: `T = int32, N = 3`; empty outside templates.
    std::string instanceDescription() const
    {
      std::string out;
      std::vector<std::string> seen; // an out-of-line member's parameters repeat its class's
      if (activeSubst)
        for (const Subst::Pair& p : activeSubst->pairs)
          if (p.param && p.param->kind == NodeKind::TemplateParamDecl)
          {
            uint32_t name = static_cast<TemplateParamDecl*>(p.param)->name;
            std::string one = std::string(name == UINT32_MAX ? "?" : impl.symbols.name(name)) + " = " + types.toString(p.type, &impl.symbols);
            if (std::find(seen.begin(), seen.end(), one) != seen.end())
              continue;
            seen.push_back(one);
            out += (out.empty() ? "" : ", ") + one;
          }
      return out;
    }

    const Ty* computeThisType()
    {
      Decl* cls = enclosingClassDecl(scope);
      if (!cls)
        return nullptr;
      if (cls->kind == NodeKind::EnumDecl)
        return types.lref(impl.enumTypeIn(cls, activeSubst)); // in an enum's methods, `this` is the value
      return types.pointer(impl.selfType(cls));
    }

    void walkDecls(Span<Decl*> decls)
    {
      for (Decl* d : decls)
        walkDecl(d);
    }

    void walkDecl(Decl* d)
    {
      if (!d)
        return;
      // While the files are resolved, a template function's own walk (the generic one, outside any
      // other unit) is a task for whichever worker is free: one file of heavy templates
      // (ValueToWidget.kpp: 18 template functions of 50 to 100 ms each, 1.26 s of the game's 1.31 s
      // resolve) no longer walks them all on one thread. Nothing outside the body reads what its walk
      // leaves on its nodes (the evaluator runs instances' copies), except a signature the walk types
      // (a decltype, a deduced auto: its callers read it), so such a function is walked in place.
      if (d->kind == NodeKind::FunctionDecl && !walkingQueued && file && file->tokens && !recordingInto && !activeSubst && instantiationDepth == 0 &&
          static_cast<FunctionDecl*>(d)->body && (!d->templateParams.empty() || inGenericTemplate()) &&
          impl.queueMemberBodies.load(std::memory_order_relaxed) && !signatureFromWalk(static_cast<FunctionDecl*>(d)))
      {
        auto dw = std::make_shared<Impl::DeclWalk>(Impl::DeclWalk{static_cast<FunctionDecl*>(d), scope, thisType, locals.items, file, &stats});
        Impl::MemberTask task{nullptr, nullptr, nullptr, 0, 0, nullptr, std::move(dw)};
        impl.queueMemberBody(task);
        return;
      }
      // A declaration's walk outside any other unit (a function's body, a variable's initializer, a
      // static_assert, an alias, an enumeration's values) is a unit of its own; what is declared
      // inside it belongs to it. Not a namespace's or a class's: their members are units each.
      std::optional<RecordInto> unit;
      if (!recordingInto && !activeSubst && d->kind != NodeKind::NamespaceDecl && d->kind != NodeKind::ClassDecl)
        unit.emplace(recordingDependencies ? impl.dependenciesOf(d, Impl::UNIT_Walk) : nullptr);
      size_t mark = locals.mark();
      if (!d->templateParams.empty())
        pushTemplateParams(d->templateParams);
      switch (d->kind)
      {
        case NodeKind::NamespaceDecl:
        {
          auto* ns = static_cast<NamespaceDecl*>(d);
          Scope* saved = scope;
          if (ns->ownScope)
            scope = ns->ownScope;
          walkDecls(ns->decls);
          scope = saved;
          break;
        }
        case NodeKind::ClassDecl:
        {
          auto* c = static_cast<ClassDecl*>(d);
          Scope* saved = scope;
          const Ty* savedThis = thisType;
          // The bases are named from where the class is (its scope's parent: `class A::B : Base`
          // sees A's Base, as in C++), not from where its definition is written.
          if (c->ownScope && c->ownScope->parent)
            scope = c->ownScope->parent;
          for (const ClassDecl::Base& b : c->bases)
          {
            walkType(b.type);
            // A virtual base would be laid out as an ordinary one (a diamond's shared base twice,
            // silently): an error until it is supported.
            if (b.isVirtual && b.type && !activeSubst && !probing && file && !file->isImported)
            {
              std::string base = b.type->kind == NodeKind::NamedType && !static_cast<NamedType*>(b.type)->name.empty()
                                   ? impl.namePartText(static_cast<NamedType*>(b.type)->name.last())
                                   : std::string("?");
              std::string of = c->name.empty() ? std::string("a class") : impl.namePartText(c->name.last());
              impl.reportCheckerError(file, b.type->token, "virtual base classes are not supported (" + base + ", a base of " + of + ")");
            }
          }
          // A struct is a public value type (design.md "struct and class"): one deriving from a
          // polymorphic class is none (its own virtual functions the parser reports).
          if (impl.structRuleErrors && c->keyword == Kw::struct_ && c->isDefinition && !c->isClosure && !c->isTypedUnion && !activeSubst && !probing && file &&
              !file->isImported &&
              !Impl::declaresVirtual(c, false))
            if (ClassDecl* base = impl.polymorphicBaseOf(c, 0, false))
              impl.reportCheckerError(file, c->token, "struct " + (c->name.empty() ? std::string("(anonymous)") : impl.namePartText(c->name.last())) +
                                                          " derives from " + (base->name.empty() ? std::string("a") : impl.namePartText(base->name.last()) + ",") +
                                                          " a polymorphic class (a struct is a public value type, design.md \"struct and class\"): make it a class");
          if (c->ownScope)
            scope = c->ownScope;
          else if (c->name.parts.size() > 1)
            scope = semanticScopeFor(c->name);
          if (!activeSubst || c->ownScope != scope)
            thisType = computeThisType();
          walkDecls(c->members);
          scope = saved;
          thisType = savedThis;
          break;
        }
        case NodeKind::EnumDecl:
        {
          auto* e = static_cast<EnumDecl*>(d);
          walkType(e->underlying);
          Scope* saved = scope;
          const Ty* savedThis = thisType;
          if (e->ownScope)
          {
            scope = e->ownScope;
            thisType = computeThisType(); // the enum's methods see `this` as the value
          }
          for (Decl* en : e->enumerators)
            if (en->kind == NodeKind::EnumeratorDecl)
              walkExpr(static_cast<EnumeratorDecl*>(en)->value);
          walkDecls(e->members);
          scope = saved;
          thisType = savedThis;
          break;
        }
        case NodeKind::FunctionDecl:
        {
          auto* f = static_cast<FunctionDecl*>(d);
          if (activeSubst && (f->attributes & ATTR_InstanceCopy))
            impl.recordInstanceSubst(f, *activeSubst); // the classes of its body see these arguments
          // A class's own operator new or operator delete (or a replacement of the global one): `new`
          // and `delete` would allocate with the library's functions and silently pass it by, so its
          // declaration is an error until they call it.
          if (!activeSubst && !probing && file && !file->isImported && !f->name.empty() && f->name.last().special == 2 &&
              (f->name.last().opToken == uint32_t(P::Count) + uint32_t(Kw::new_) || f->name.last().opToken == uint32_t(P::Count) + uint32_t(Kw::delete_)))
            impl.reportCheckerError(file, f->token, std::string(impl.owningClassOf(f) ? "a class's own" : "a replacement of the global") +
                                                        " operator new or operator delete is not supported yet");
          Scope* saved = scope;
          const Ty* savedThis = thisType;
          walkType(f->returnType);
          if (f->name.parts.size() > 1)
          {
            scope = semanticScopeFor(f->name);
            if (!activeSubst)
              thisType = computeThisType();
          }
          else if (f->parent && f->parent->kind == NodeKind::NamespaceDecl && f->parent->ownScope)
          {
            // finishIndex simplified the qualified name but set the parent to the namespace decl.
            scope = f->parent->ownScope;
          }
          for (const NamePart& part : f->name.parts)
          {
            for (Node* arg : part.templateArgs)
              walkNode(arg);
            if (part.special == 3)
              walkType(part.convType);
          }
          for (Decl* p : f->params)
          {
            walkParam(static_cast<ParamDecl*>(p));
            checkTypeNamesNoVariable(static_cast<ParamDecl*>(p)->type, p->token); // (a function's own: not a function type's, `gcd(P1::num, P2::num)`, which may be a call)
          }
          walkType(f->trailingReturn);
          walkExpr(f->requiresClause);
          walkExpr(f->condition);
          checkDefinedOnce(f);
          checkTextSignature(f);
          Decl* initialized = f->ctorInits.empty() ? nullptr : impl.owningClassOf(f);
          for (FunctionDecl::CtorInit& init : f->ctorInits)
          {
            if (initialized && initialized->kind == NodeKind::ClassDecl && absentMemberInit(static_cast<ClassDecl*>(initialized), init))
              continue; // a member a false `when` dropped: its initializer is not there either (design.md 19)
            Lookup l = impl.lookupQualified(init.member, scope, &locals, 0);
            if (l.found == Found::No)
            {
              ++stats.namesUnresolved;
              countUnresolved(init.member, f->token);
            }
            else
              ++stats.namesMember;
            FunctionDecl* savedInitializers = initializersOf;
            initializersOf = f;
            for (Expr* a : init.args)
              walkExpr(a);
            initializersOf = savedInitializers;
            // `problem("bad ", point)` for a member (or base) constructed with a Text (design.md 14).
            if (!init.braced && !init.isPack && l.decl)
            {
              const Ty* initialized = l.decl->kind == NodeKind::VariableDecl ? impl.declaredTypeOf(l.decl, activeSubst, 0)
                                    : l.decl->kind == NodeKind::ClassDecl  ? impl.selfType(l.decl)
                                                                           : nullptr;
              if (initialized && initialized->kind == TyKind::Class && !types.isDependent(initialized))
                packText(init.args, textConstructor(initialized, init.args), f->token);
            }
          }
          bool autoReturn = f->returnType && Impl::astMentionsAuto(f->returnType) && !f->trailingReturn;
          if (autoReturn)
          {
            returnFrames.push_back(nullptr);
            decltypeReturns.push_back(f->returnType->kind == NodeKind::AutoType && static_cast<AutoType*>(f->returnType)->isDecltypeAuto);
          }
          if (f->flags & DF_Static)
            thisType = nullptr;
          else if ((f->flags & DF_Const) && thisType && thisType->kind == TyKind::Pointer && !thisType->inner->isConst)
            thisType = impl.types.pointer(impl.types.qualified(thisType->inner, true, false)); // `const C* this` in a const method
          FunctionDecl* savedBodyOf = bodyOf;
          bodyOf = f;
          walkStmt(f->body);
          bodyOf = savedBodyOf;
          if ((impl.accessWarnings || impl.accessErrors) && !probing && !retypingElements)
          {
            // In an instance of a class template, the instance: its bases are as its arguments make them.
            const Ty* self = nullptr;
            if (activeSubst)
              if (Decl* cls = impl.owningClassOf(f); cls && cls->kind == NodeKind::ClassDecl)
                self = impl.substitute(impl.selfType(cls), *activeSubst);
            impl.checkAccessIn(f, self);
          }
          if (autoReturn)
          {
            const Ty* deduced = returnFrames.back();
            returnFrames.pop_back();
            decltypeReturns.pop_back();
            if (deduced && !returnFrames.empty() && !returnFrames.back())
              returnFrames.back() = deduced; // an instantiation's frame around the body takes it
            // Not from a template's own (generic) walk: what its body deduces there is no
            // instance's type (a closure of the generic body), and callers read it or not by which
            // file was walked first.
            if (deduced && deduced->isKnown() && !types.isDependent(deduced) && !inGenericTemplate())
            {
              // Cache the deduced signature for callers in other files.
              const Ty* fnTy = impl.declaredTypeOf(f, nullptr, 0);
              std::vector<const Ty*> params(fnTy->args.begin(), fnTy->args.end());
              f->cachedType.store(types.function(impl.deducedShape(fnTy->inner, deduced), Span<const Ty*>{params.data(), uint32_t(params.size())}, fnTy->variadic),
                                  std::memory_order_release);
            }
          }
          scope = saved;
          thisType = savedThis;
          break;
        }
        case NodeKind::VariableDecl:
        {
          auto* v = static_cast<VariableDecl*>(d);
          deducedLocal = nullptr; // this variable's own, if it deduces one (not the one walked before)
          // A class template's data member a `when` on its parameters declares (design.md 19): its
          // condition, which codegen folds per instance for the layout, walked with the member.
          if (v->when && v->whenKnown < 0 && v->parent && v->parent->kind == NodeKind::ClassDecl)
            walkExpr(v->when);
          walkType(v->type);
          checkTypeNamesNoVariable(v->type, v->token);
          checkArrayBounds(v);
          walkExpr(v->bitfield);
          Scope* saved = scope;
          if (v->name.parts.size() > 1)
            scope = semanticScopeFor(v->name);
          const Ty* initTy = walkExpr(v->init);
          scope = saved;
          if ((impl.accessWarnings || impl.accessErrors) && !probing && !retypingElements && !bodyOf && returnFrames.empty())
            impl.checkAccessIn(v); // (a local, also a lambda's in a member initializer: with the function it is in)
          // Deduce `auto` now that the initializer is typed, and cache it on the declaration.
          const Ty* declared = impl.declaredTypeOf(v, activeSubst, 0);
          if (v->init && declared && declared->isKnown() && !impl.containsAuto(declared) && !probesSharedNodes())
            bindTemplateAddresses(v->init, declared); // `const Expr& (*binary)(const Expr&, const Expr&) = construct<Add>;`
          checkTextVariable(v, declared);
          if (v->init && !v->bracedInit)
            noteZeroNull(v->init, declared); // `Node* a = 0;`
          if (v->init && declared && !impl.containsAuto(declared))
            checkClosureTo(v->init, declared, v->token); // `Function<void()> f = [&] { ... text ... };`
          // `int count = item;`, `Item* raw = item;`: a class copy-initializing a scalar (not `T x(args)` or a brace list,
          // which take explicit conversion operators too).
          if (v->init && declared && !impl.containsAuto(declared) && !v->bracedInit && v->init->kind != NodeKind::BraceInitExpr &&
              !(v->init->kind == NodeKind::FunctionalCastExpr && static_cast<FunctionalCastExpr*>(v->init)->type == v->type))
            checkClassToScalar(initTy, declared, v->init->token);
          bool isDraft = declared && declared->kind == TyKind::Class && declared->decl && (declared->decl->attributes & ATTR_Draft);
          if ((!declared || isDraft || Impl::mentionsUnknown(declared)) && v->type && !probesSharedNodes())
          {
            // A type only the function's locals know (`using Match = Pair<int, int>;` in the body,
            // `Map<Key, int>` with a local class Key): resolved here, where the locals are, and
            // cached for everyone asking later.
            const Ty* local = typeOf(v->type);
            if (local && local->isKnown() && (!declared || !declared->isKnown() || isDraft || !Impl::mentionsUnknown(local)))
            {
              v->cachedType.store(local, std::memory_order_release);
              declared = local;
              deducedLocal = local; // kept with the local by walkLocalDecl, for this walk's uses of it
            }
          }
          if (v->init && !probesSharedNodes() && impl.isUndeducedClassTemplate(declared))
          {
            // `ScopedSetter guard(x, true)`: the class's arguments from a constructor (CTAD);
            // `Array names = {a, b}`: from an aggregate's members; `Duration budget =
            // Milliseconds(5)`: from the initializer's value, one argument.
            Span<Expr*> args;
            bool copyInit = false;
            if (v->init->kind == NodeKind::FunctionalCastExpr && static_cast<FunctionalCastExpr*>(v->init)->type == v->type)
              args = static_cast<FunctionalCastExpr*>(v->init)->args; // the parser's `T x(args)`
            else if (v->init->kind == NodeKind::BraceInitExpr)
              args = static_cast<BraceInitExpr*>(v->init)->args;
            else
              copyInit = initTy && initTy->isKnown();
            std::vector<const Ty*> argTypes = copyInit ? std::vector<const Ty*>{argumentType(v->init, initTy, types)} : argTypesOf(args);
            // `T x = ...` and `T x = {...}` copy-initialise: an explicit deduction guide is none for them.
            bool direct = (v->init->kind == NodeKind::FunctionalCastExpr && static_cast<FunctionalCastExpr*>(v->init)->type == v->type) || v->bracedInit;
            const Ty* deduced = impl.deduceClassArgs(declared, argTypes, !direct);
            if (!deduced && v->init->kind == NodeKind::BraceInitExpr)
              deduced = impl.deduceAggregateArgs(declared, argTypes);
            if (deduced && (v->flags & DF_Constexpr))
              deduced = types.qualified(deduced, true, false); // `constexpr Array names = {...}` is const
            if (deduced)
            {
              v->cachedType.store(deduced, std::memory_order_release);
              if (v->init->kind == NodeKind::FunctionalCastExpr)
                record(v->init, deduced);
              declared = deduced;
              deducedLocal = deduced; // the uses in this walk (a template instance's body asks under its substitution)
            }
          }
          if (impl.containsAuto(declared))
          {
            // From the type this walk gave the initializer, not the one on its (shared) node.
            if (declared == types.decltypeAutoType())
              initTy = decltypeOf(v->init, initTy); // `decltype(auto) r = (x);` is a reference
            if (v->bracedInit && v->init && v->init->kind == NodeKind::BraceInitExpr && static_cast<BraceInitExpr*>(v->init)->args.size() == 1)
              if (Expr* only = static_cast<BraceInitExpr*>(v->init)->args[0]; only && only->ty)
                initTy = only->ty; // `auto x{e}`, the init-capture `[callback{::move(callback)}]`: e's type
            if (!v->bracedInit && v->init && v->init->kind == NodeKind::BraceInitExpr && declared != types.decltypeAutoType())
              if (const Ty* list = impl.initializerListOfElements(static_cast<BraceInitExpr*>(v->init)))
                initTy = list; // `const auto entityNames = {"pipe", "storage-tank"};` is an InitializerList<const char*>
            const Ty* deduced = v->init && initTy ? impl.deduceAuto(declared, initTy) : types.unknown();
            if (deduced->isKnown() && (v->flags & DF_Constexpr) && !deduced->isReference())
              deduced = types.qualified(deduced, true, false); // `constexpr auto limit = 5;` is a const int
            if (deduced->isKnown())
            {
              deducedLocal = deduced; // kept with the local by walkLocalDecl, per walk
              if (!probesSharedNodes())
                v->cachedType.store(deduced, std::memory_order_release);
            }
            else if ((activeSubst && !activeSubst->empty()) || !d->file || d->file->index == stats.fileIndex)
              ++stats.unknownAuto; // once, by its own file's walk (or its instance's): a global walked for another file's statistics may come before or not
          }
          // A constexpr variable is a context that needs its constant: checked where its type is
          // known, as a static_assert is (not in a template's own walk: its instances are).
          if ((v->flags & DF_Constexpr) && v->init && !(v->flags & DF_Typedef) && undecidedBranches == 0 && !inGenericTemplate() &&
              !retypingElements && !probing)
          {
            const Ty* type = deducedLocal ? deducedLocal : declared;
            impl.checkConstexprVariable(v, type, ctx(), activeSubst ? bodyOf : nullptr, file);
          }
          break;
        }
        case NodeKind::ParamDecl: walkParam(static_cast<ParamDecl*>(d)); break;
        case NodeKind::UsingAliasDecl:
          checkNotLibrary(static_cast<UsingAliasDecl*>(d)->type, d->token);
          walkType(static_cast<UsingAliasDecl*>(d)->type);
          break;
        case NodeKind::UsingNameDecl:
        {
          auto* u = static_cast<UsingNameDecl*>(d);
          Lookup l = impl.lookupQualified(u->name, scope, &locals, 0);
          if (l.found == Found::No)
          {
            ++stats.typesUnresolved;
            countUnresolved(u->name, u->token);
          }
          else
            ++stats.typesResolved;
          break;
        }
        case NodeKind::UsingDirectiveDecl:
        {
          auto* u = static_cast<UsingDirectiveDecl*>(d);
          checkNotLibrary(u->name, u->token);
          Lookup l = impl.lookupQualified(u->name, scope, &locals, 0);
          if (l.found == Found::No)
          {
            ++stats.typesUnresolved;
            countUnresolved(u->name, u->token);
          }
          else
            ++stats.typesResolved;
          break;
        }
        case NodeKind::StaticAssertDecl:
        {
          auto* s = static_cast<StaticAssertDecl*>(d);
          walkExpr(s->cond);
          // Checked where its condition is known: not in a template's own walk (its instances
          // are), nor in a branch of an instance's `if constexpr` left undecided (it may be the
          // discarded one).
          if (undecidedBranches == 0 && !inGenericTemplate() && !replacedBySpecialization(s) && !retypingElements)
          {
            std::string instance = instanceDescription();
            if (!instance.empty() && requestFile)
              instance += ", asked for at " + Impl::locationOf(requestFile, requestToken);
            // In an instance's function, it counts if the program uses the function (C++ does not
            // instantiate the members, or the calls in the branches, nobody uses).
            impl.checkStaticAssert(s, ctx(), instance, activeSubst ? bodyOf : nullptr);
          }
          break;
        }
        case NodeKind::FriendDecl: walkType(static_cast<FriendDecl*>(d)->type); break;
        default: break;
      }
      locals.pop(mark);
    }

    // Declarations inside function bodies: add their names to the local stack.
    void declareLocal(Decl* d)
    {
      switch (d->kind)
      {
        case NodeKind::VariableDecl:
        {
          auto* v = static_cast<VariableDecl*>(d);
          if (v->name.parts.size() == 1)
            locals.push(v->name.parts[0].symbol, v);
          for (Expr* b : v->structuredBindings)
          {
            // A tuple-like binding names its hidden variable (tupleBindings), any other the object.
            Decl* own = static_cast<NameExpr*>(b)->resolved;
            bool tuple = own && own->kind == NodeKind::VariableDecl && static_cast<VariableDecl*>(own)->tupleBinding;
            locals.push(static_cast<NameExpr*>(b)->name.parts[0].symbol, tuple ? own : v);
            if (tuple)
              locals.items.back().type = own->cachedType.load(std::memory_order_acquire); // as its walk deduced it
          }
          break;
        }
        case NodeKind::ClassDecl:
        {
          auto* c = static_cast<ClassDecl*>(d);
          if (c->name.parts.size() == 1)
            locals.push(c->name.parts[0].symbol, c);
          break;
        }
        case NodeKind::EnumDecl:
        {
          auto* e = static_cast<EnumDecl*>(d);
          if (e->name.parts.size() == 1)
            locals.push(e->name.parts[0].symbol, e);
          for (Decl* en : e->isClass ? Span<Decl*>{} : e->enumerators) // a scoped enum's stay in it
            if (en->kind == NodeKind::EnumeratorDecl)
              locals.push(static_cast<EnumeratorDecl*>(en)->name, en);
          break;
        }
        case NodeKind::UsingAliasDecl:
          if (d->whenKnown != 0) // (a false `when`'s is not checked: design.md 19)
            checkNotLibrary(static_cast<UsingAliasDecl*>(d)->type, d->token);
          locals.push(static_cast<UsingAliasDecl*>(d)->name, d);
          break;
        case NodeKind::UsingDirectiveDecl: // `using namespace Helpers;`
          checkNotLibrary(static_cast<UsingDirectiveDecl*>(d)->name, d->token);
          locals.push(LocalStack::usingDirective, d);
          break;
        case NodeKind::UsingNameDecl:
        {
          // `using Graphics::Architecture;` in a block: the name it brings in, for the block.
          auto* u = static_cast<UsingNameDecl*>(d);
          if (u->isEnum)
          {
            impl.pushLocalUsingEnum(u, scope, locals); // `using enum UndoActionDescription::Type;`: its enumerators
            break;
          }
          if (Decl* target = u->name.empty() ? nullptr : impl.localUsingTarget(u, scope, &locals))
            locals.push(u->name.last().symbol, target);
          break;
        }
        case NodeKind::FunctionDecl:
        {
          auto* f = static_cast<FunctionDecl*>(d);
          if (f->name.parts.size() == 1 && f->name.parts[0].special == 0)
            locals.push(f->name.parts[0].symbol, f);
          break;
        }
        default: break;
      }
    }

    // `auto [a, b] = e;` of a class E whose TupleSize<E> (the library's std::tuple_size) has a
    // member value binds tuple-like (design.md 4, structured bindings): binding i names a hidden
    // `auto&& a = get<i>(object)`, or `object.get<i>()` when E has a member template get whose
    // first parameter is a constant, the object passed as an lvalue. The binding's own name node
    // in v's list records that variable (resolved), for declareLocal and the code generator. Any
    // other object (an array, a class without TupleSize<E>::value) binds member by member
    // (bindingType).
    void tupleBindings(VariableDecl* v, const Ty* object)
    {
      if (v->structuredBindings.empty() || probesSharedNodes())
        return;
      for (Expr* b : v->structuredBindings)
        static_cast<NameExpr*>(b)->resolved = nullptr; // what the template's own walk made (an instance copies it)
      const Ty* e = object ? types.unqualified(types.stripRef(object)) : nullptr;
      if (!e || e->kind != TyKind::Class || !e->isKnown() || types.isDependent(e) || Impl::mentionsUnknown(e))
        return;
      std::optional<int64_t> size;
      if (!impl.tupleSizeOf(e, size, 0))
        return;
      size_t n = v->structuredBindings.size();
      if (!size || *size != int64_t(n))
      {
        if (!inGenericTemplate())
          impl.reportCheckerError(v->file, v->token,
                                  !size ? "a structured binding of " + types.toString(e, &impl.symbols) + ", whose TupleSize<>::value is no constant"
                                        : std::to_string(n) + " names bound to " + types.toString(e, &impl.symbols) + ", whose TupleSize<>::value is " +
                                              std::to_string(*size));
        return;
      }
      if (n > impl.indexTokens.size())
        return;
      bool member = impl.hasMemberGet(e, 0);
      Arena& arena = impl.instanceArena();
      size_t mark = locals.mark();
      locals.items.push_back({LocalStack::hiddenObject, v, object}); // the object as the calls name it (resolveName)
      for (size_t i = 0; i < n; ++i)
      {
        auto* b = static_cast<NameExpr*>(v->structuredBindings[i]);
        auto* hidden = arena.make<NameExpr>();
        hidden->kind = NodeKind::NameExpr;
        hidden->token = b->token;
        hidden->resolved = v; // no name: the hidden object itself (resolveName)
        auto* index = arena.make<LiteralExpr>();
        index->kind = NodeKind::LiteralExpr;
        index->token = uint32_t(i);
        index->literalKind = TokenKind::Number;
        index->keyword = Kw{};
        index->file = &impl.indexFile;
        Node* indexArg = index;
        NamePart part{};
        part.symbol = impl.symGet;
        part.templateArgs = Span<Node*>{arena.array(&indexArg, 1), 1};
        part.hasTemplateArgs = true;
        auto* call = arena.make<CallExpr>();
        call->kind = NodeKind::CallExpr;
        call->token = b->token;
        if (member)
        {
          auto* m = arena.make<MemberExpr>();
          m->kind = NodeKind::MemberExpr;
          m->token = b->token;
          m->base = hidden;
          m->member.parts = Span<NamePart>{arena.array(&part, 1), 1};
          m->op = P::Dot;
          call->callee = m;
        }
        else
        {
          // `::get<i>(object)`: the library's get and those argument-dependent lookup finds, never
          // a member or a local named get.
          auto* callee = arena.make<NameExpr>();
          callee->kind = NodeKind::NameExpr;
          callee->token = b->token;
          callee->name.parts = Span<NamePart>{arena.array(&part, 1), 1};
          callee->name.global = true;
          Expr* argument = hidden;
          call->callee = callee;
          call->args = Span<Expr*>{arena.array(&argument, 1), 1};
        }
        auto* deduced = arena.make<AutoType>();
        deduced->kind = NodeKind::AutoType;
        deduced->token = b->token;
        auto* reference = arena.make<RRefType>();
        reference->kind = NodeKind::RRefType;
        reference->token = b->token;
        reference->referenced = deduced;
        auto* var = arena.make<VariableDecl>();
        var->kind = NodeKind::VariableDecl;
        var->token = b->token;
        var->endToken = v->endToken;
        var->file = v->file;
        var->parent = v->parent;
        var->type = reference;
        var->name = b->name;
        var->init = call;
        var->tupleBinding = true;
        deducedLocal = nullptr;
        walkDecl(var);
        deducedLocal = nullptr;
        b->resolved = var;
      }
      locals.pop(mark);
    }

    // A `when` declaration's condition as written (design.md 19), for a message: its tokens from the first
    // to the `)` that closes `when (`.
    std::string conditionText(Decl* d)
    {
      const ParsedFile* f = d->file ? d->file : file;
      if (!d->when || !f || !f->tokens)
        return "its condition";
      uint32_t first = UINT32_MAX;
      forEachNode(d->when, [&](Node* n) { first = std::min(first, n->token); });
      const std::vector<Token>& tokens = *f->tokens;
      uint32_t end = first;
      for (int depth = 0; end < tokens.size(); ++end)
        if (tokens[end].is(P::LParen))
          ++depth;
        else if (tokens[end].is(P::RParen) && depth-- == 0)
          break;
      if (first >= end || end >= tokens.size())
        return "its condition";
      return std::string(f->text.substr(tokens[first].offset, tokens[end - 1].offset + tokens[end - 1].length - tokens[first].offset));
    }

    // Where the walk is, for a message about what exists: "here", or which instance.
    std::string whenWhere()
    {
      std::string where = "here";
      if (activeSubst && bodyOf && !bodyOf->name.empty())
      {
        where = "in this instance (" + std::string(symbols.name(bodyOf->name.last().symbol)) + "<";
        for (size_t k = 0; k < activeSubst->pairs.size(); ++k)
          where += (k ? ", " : "") + types.toString(activeSubst->pairs[k].type, &symbols);
        where += ">)";
      }
      return where;
    }

    // A use of a local a `when` declares (design.md 19), found by lookup as found (its block's last
    // declaration of the name), checked in the configuration and instance being built: the declaration
    // the use names, or null after an error, or where this walk cannot tell (undecided: a template's own
    // walk, a condition on its parameters, of several declarations). The candidates are the declarations
    // of the name in found's block, a plain one existing always. The use names the one that exists here;
    // none existing is an error here, as a `when` that does not exist still hides an outer name (never a
    // fallback to it); two existing are the clash, reported where the later is declared. A use inside a
    // discarded branch is not walked, as any discarded code.
    Decl* whenUse(uint32_t symbol, uint32_t token, Decl* found, bool& undecided)
    {
      undecided = false;
      size_t at = locals.items.size();
      while (at > 0 && locals.items[at - 1].decl != found)
        --at;
      if (at == 0)
        return found;
      size_t start = 0;
      for (size_t s : blockStarts)
        if (s < at)
          start = s;
      std::vector<Decl*> candidates, existing;
      std::string conditions;
      for (size_t i = start; i < locals.items.size(); ++i)
        if (Decl* c = locals.items[i].decl;
            locals.items[i].symbol == symbol && c && std::find(candidates.begin(), candidates.end(), c) == candidates.end())
        {
          candidates.push_back(c);
          if (!c->when || c->whenKnown == 1)
            existing.push_back(c);
          else if (c->whenKnown < 0)
            undecided = true;
          if (c->when)
            conditions += (conditions.empty() ? "" : ", ") + conditionText(c);
        }
      if (undecided)
        return candidates.size() == 1 ? found : nullptr; // (a lone one typed as itself; of several, the instances tell)
      if (!existing.empty())
        return existing[0];
      if (file && !file->isImported && !probing)
      {
        std::string name(symbols.name(symbol));
        std::string message = candidates.size() == 1 ? "'" + name + "' exists only when " + conditions + " holds: use it inside `if constexpr (" +
                                                           conditions + ")` or a `when` on it"
                                                     : "'" + name + "' does not exist here: none of its `when` conditions holds (" + conditions + ")";
        if (activeSubst)
          reportInInstance(token, message);
        else
          impl.reportCheckerError(file, token, message);
      }
      return nullptr;
    }

    // A lookup that found a local a `when` declares (design.md 19): its declaration made the one the use
    // names (whenUse); false after an error, or where this walk cannot tell which.
    bool whenLookup(Lookup& l, NameExpr* n)
    {
      if (l.found == Found::Member && activeSubst && l.decl && l.decl->kind == NodeKind::VariableDecl && l.decl->when && l.decl->whenKnown < 0 &&
          l.overloads && l.overloads->size() > 1 && n->name.parts.size() == 1)
        return whenMemberLookup(l, n);
      if (l.found != Found::Local || !l.decl || !l.decl->when || n->name.parts.size() != 1)
        return true;
      bool undecided = false;
      Decl* named = whenUse(n->name.parts[0].symbol, n->token, l.decl, undecided);
      if (!named)
        return false;
      l.decl = named;
      n->resolved = named;
      return true;
    }

    // An unqualified use, in a member of a class template's instance, of a name that several `when` data members
    // of the class declare (design.md 19): the instance's own, whose condition holds under its arguments, as memberOf
    // takes it for `object.name`; none or two is an error there. As before where this walk does not bind the class's
    // arguments or the name was found in a base.
    bool whenMemberLookup(Lookup& l, NameExpr* n)
    {
      Decl* c = l.decl->parent;
      if (!c || c->kind != NodeKind::ClassDecl || l.foundIn != c->ownScope)
        return true;
      for (Decl* p : Impl::effectiveParams(c))
        if (!activeSubst->find(p))
          return true;
      int holding = 0;
      bool undecided = false;
      Decl* chosen = impl.whenMemberOf(l.decl, *activeSubst, holding, undecided);
      if (undecided)
        return true;
      if (chosen)
      {
        l.decl = chosen;
        l.overloads = nullptr;
        n->resolved = chosen;
        return true;
      }
      reportInInstance(n->token, "'" + std::string(symbols.name(n->name.parts[0].symbol)) + "' " +
                                   (holding == 0 ? std::string("does not exist in this instance: none of its `when` conditions holds there")
                                                 : "is declared twice in this instance: the `when` conditions of " + std::to_string(holding) +
                                                     " of its declarations hold there") +
                                   " (design.md 19)");
      return false;
    }

    // A condition made of config constants and literals (names in the `config` namespace, already
    // resolved: a walked condition): the same in every instance of a template, so its own walk decides it
    // as a function that is no template does (design.md 7.4 and 19).
    bool configCondition(Node* n, int depth = 0)
    {
      if (!n || depth > 32)
        return false;
      switch (n->kind)
      {
        case NodeKind::LiteralExpr: return true;
        case NodeKind::ParenExpr: return configCondition(static_cast<ParenExpr*>(n)->inner, depth + 1);
        case NodeKind::UnaryExpr: return configCondition(static_cast<UnaryExpr*>(n)->operand, depth + 1);
        case NodeKind::BinaryExpr:
          return configCondition(static_cast<BinaryExpr*>(n)->lhs, depth + 1) && configCondition(static_cast<BinaryExpr*>(n)->rhs, depth + 1);
        case NodeKind::NameExpr:
        {
          Decl* d = static_cast<NameExpr*>(n)->resolved;
          if (!d || d->kind != NodeKind::VariableDecl)
            return false;
          Decl* top = d->parent;
          while (top && top->parent)
            top = top->parent;
          return top && top->kind == NodeKind::NamespaceDecl && static_cast<NamespaceDecl*>(top)->name != UINT32_MAX &&
                 symbols.name(static_cast<NamespaceDecl*>(top)->name) == "config";
        }
        default: return false;
      }
    }

    // `when (condition)` on a local (design.md 19): decided as if constexpr's condition is, in an
    // instance and in a function that is no template; false, the variable does not exist: nothing of
    // it is checked or instantiated, as a discarded branch (walkLocalDecl declares it absent). A
    // template's own walk decides a condition of config constants and literals so too, and checks any
    // other as an undecided branch. True when there is no condition.
    bool whenHolds(Decl* d)
    {
      if (!d->when)
        return true;
      if (keepConstexprBranches && d->whenKnown >= 0)
        return d->whenKnown != 0;
      walkExpr(d->when);
      if (!activeSubst && (impl.converting || (inGenericTemplate() && !configCondition(d->when))))
        return true; // (a template's own walk decides only a condition of config constants and literals)
      std::optional<bool> known = impl.constCondition(d->when, ctx(), 0);
      if (!known)
      {
        impl.reportCheckerError(file, d->when->token, "a when condition is a constant expression, as if constexpr's (design.md 19)");
        return true;
      }
      d->whenKnown = int8_t(*known ? 1 : 0);
      // Several `when`s may declare one name, as an #ifdef/#else pair (`when (c) A t; when (!c) B
      // t;`); two that exist at once are a clash.
      uint32_t name = impl.absentName(d); // (a variable's, an alias's, a class's or an enumeration's)
      if (*known && name != UINT32_MAX)
        for (size_t i = blockLocals; i < locals.items.size(); ++i)
          if (Decl* other = locals.items[i].decl; locals.items[i].symbol == name && other && (!other->when || other->whenKnown != 0))
          {
            impl.reportCheckerError(file, d->token, "'" + std::string(symbols.name(name)) + "' is declared twice " + whenWhere() +
                                                        ": both `when` conditions hold");
            break;
          }
      return *known;
    }

    void walkLocalDecl(Decl* d)
    {
      if (!whenHolds(d))
      {
        // Declared all the same, absent (whenKnown 0): a use of its name finds it, never a name of an
        // outer scope or of the library, and is an error unless it is discarded code (whenUse).
        declareLocal(d);
        return;
      }
      // Classes are in scope in their own bodies. A variable whose type is written is in scope
      // in its own initializer, as C++ declares it before that (`Function<void(int)> visit =
      // [&](int n) { visit(n - 1); };`); an `auto` one is not, its type comes from there.
      if (d->kind == NodeKind::ClassDecl || d->kind == NodeKind::EnumDecl)
      {
        declareLocal(d);
        walkDecl(d);
      }
      else
      {
        auto* v = d->kind == NodeKind::VariableDecl ? static_cast<VariableDecl*>(d) : nullptr;
        bool early = v && v->type && v->init && v->structuredBindings.empty() && !Impl::astMentionsAuto(v->type);
        size_t declaredAt = locals.mark();
        if (early)
          declareLocal(d);
        deducedLocal = nullptr;
        walkDecl(d);
        const Ty* deduced = v ? deducedLocal : nullptr;
        deducedLocal = nullptr;
        if (v && !v->structuredBindings.empty())
          tupleBindings(v, deduced ? deduced : hiddenObjectType(v));
        if (!early)
        {
          declaredAt = locals.mark();
          declareLocal(d);
        }
        if (deduced)
          for (size_t i = declaredAt; i < locals.items.size(); ++i)
            if (locals.items[i].decl == d)
              locals.items[i].type = deduced; // `auto x = e;`: the type this walk deduced
      }
    }

    void walkStmt(Stmt* s)
    {
      if (!s)
        return;
      switch (s->kind)
      {
        case NodeKind::BlockStmt:
        {
          size_t mark = locals.mark(), savedBlock = blockLocals;
          blockLocals = mark;
          blockStarts.push_back(mark);
          for (Stmt* inner : static_cast<BlockStmt*>(s)->stmts)
            walkStmt(inner);
          blockStarts.pop_back();
          blockLocals = savedBlock;
          locals.pop(mark);
          break;
        }
        case NodeKind::ExprStmt: walkExpr(static_cast<ExprStmt*>(s)->expr); break;
        case NodeKind::AssertStmt:
          // Where config::debug is false an assert compiles to nothing (design.md 7): its expression is
          // discarded code, not checked (a `when (config::debug)` member it names, design.md 19).
          if (auto* a = static_cast<AssertStmt*>(s); a->release || a->testKind || impl.debug)
            walkExpr(a->cond);
          break;
        case NodeKind::DeclStmt:
          for (Decl* d : static_cast<DeclStmt*>(s)->decls)
            walkLocalDecl(d);
          break;
        case NodeKind::IfStmt:
        {
          auto* i = static_cast<IfStmt*>(s);
          size_t mark = locals.mark();
          walkStmt(i->init);
          if (i->isConstexpr && keepConstexprBranches && i->constexprKnown >= 0)
          {
            walkStmt(i->constexprKnown ? i->then : i->else_); // the instance's branch (keepConstexprBranches)
            locals.pop(mark);
            break;
          }
          walkNode(i->cond);
          // In an instantiation, and in a function that is no template (`if constexpr (config::debug)`,
          // whose discarded branch names what only the other configuration declares: design.md
          // 7.4, discarded branches); a template's own walk leaves both to its instances, but for a
          // condition of config constants and literals, the same in every instance (a `when` member
          // the discarded branch names: design.md 19).
          if (i->isConstexpr && (activeSubst || (!impl.converting && (!inGenericTemplate() || configCondition(i->cond)))) && i->cond &&
              uint8_t(i->cond->kind) > uint8_t(NodeKind::PackType) && uint8_t(i->cond->kind) >= uint8_t(NodeKind::NameExpr))
            if (std::optional<bool> known = impl.constCondition(static_cast<Expr*>(i->cond), ctx(), 0))
            {
              // if constexpr with a known condition: the discarded branch is not checked (nor are
              // its calls instantiated), as in C++.  Store the result so codegen can read it
              // without re-evaluating (codegen's TypeContext lacks function-body locals).
              i->constexprKnown = int8_t(*known ? 1 : 0);
              walkStmt(*known ? i->then : i->else_);
              locals.pop(mark);
              break;
            }
          bool undecided = i->isConstexpr && activeSubst;
          undecidedBranches += undecided;
          walkStmt(i->then);
          walkStmt(i->else_);
          undecidedBranches -= undecided;
          locals.pop(mark);
          break;
        }
        case NodeKind::ForStmt:
        {
          auto* f = static_cast<ForStmt*>(s);
          size_t mark = locals.mark();
          walkStmt(f->init);
          walkNode(f->cond);
          walkExpr(f->step);
          walkStmt(f->body);
          locals.pop(mark);
          break;
        }
        case NodeKind::RangeForStmt:
        {
          auto* f = static_cast<RangeForStmt*>(s);
          size_t mark = locals.mark();
          const Ty* range = walkExpr(f->range);
          if (VariableDecl* unbounded = unboundedArray(f->range))
            impl.reportCheckerError(file, f->token, "a range-for over " + std::string(symbols.name(unbounded->name.last().symbol)) +
                                                        ", an array without a length (a flexible array member): where it ends is not known");
          if (f->range->kind == NodeKind::BraceInitExpr)
          {
            // `for (X* kids : {&a, &b})`: an array of the first element's type.
            auto* b = static_cast<BraceInitExpr*>(f->range);
            const Ty* first = !b->args.empty() && b->args[0]->ty ? types.decay(b->args[0]->ty) : nullptr;
            if (first && first->isKnown())
              range = record(f->range, types.array(first, uint32_t(b->args.size())));
          }
          auto* v = static_cast<VariableDecl*>(f->var);
          walkType(v->type);
          const Ty* declared = impl.declaredTypeOf(v, activeSubst, 0);
          const Ty* loopVarType = nullptr;
          bool isDraft = declared && types.unqualified(types.stripRef(declared))->decl &&
                         (types.unqualified(types.stripRef(declared))->decl->attributes & ATTR_Draft);
          if ((!declared || isDraft || Impl::mentionsUnknown(declared)) && v->type && !probesSharedNodes())
          {
            // `for (ToCall& toCall : ...)` with ToCall a class of the function's own: the type
            // where the locals are (as for any local variable).
            const Ty* local = typeOf(v->type);
            if (local && local->isKnown() && !Impl::mentionsUnknown(local))
            {
              v->cachedType.store(local, std::memory_order_release);
              declared = local;
              loopVarType = local;
            }
          }
          if (impl.containsAuto(declared))
          {
            const Ty* element = elementTypeOf(range, f->range);
            const Ty* deduced = element->isKnown() ? impl.deduceAuto(declared, element) : types.unknown();
            if (deduced->isKnown())
            {
              v->cachedType.store(deduced, std::memory_order_release);
              loopVarType = deduced;
            }
            else if (range && range->isKnown() && types.isDependent(range))
              loopVarType = impl.deduceAuto(declared, types.lref(types.param(nullptr))); // typed per instantiation
            else
              ++stats.unknownAuto;
          }
          if (!v->structuredBindings.empty())
            tupleBindings(v, loopVarType ? loopVarType : hiddenObjectType(v)); // `for (auto& [key, value] : tuples)`, each iteration's get<i>
          size_t declaredAt = locals.mark();
          declareLocal(v);
          if (loopVarType)
            for (size_t i = declaredAt; i < locals.items.size(); ++i)
              if (locals.items[i].decl == v)
                locals.items[i].type = loopVarType;
          walkStmt(f->body);
          locals.pop(mark);
          break;
        }
        case NodeKind::ForConstexprStmt:
        {
          // The loop variable is a template parameter of the body: in the generic pass the body
          // depends on it; the code generator re-types it per element (as a pack expansion).
          auto* f = static_cast<ForConstexprStmt*>(s);
          size_t mark = locals.mark();
          auto* v = static_cast<TemplateParamDecl*>(f->var);
          walkType(v->valueType);
          if (f->list && f->list->kind == NodeKind::NamedType && isTypesList(static_cast<NamedType*>(f->list)))
            for (Node* arg : static_cast<NamedType*>(f->list)->name.parts[0].templateArgs)
              walkNode(arg); // `types<A, B>`: the elements; the list itself is the compiler's
          else if (Expr* subject = reflectionSubject(f->list))
            walkExpr(subject); // `members(T)`: the type reflected on
          else
            walkNode(f->list);
          locals.push(v->name, v);
          ++forConstexprDepth;
          walkStmt(f->body);
          --forConstexprDepth;
          // In an instantiation of an auto function whose return type is still open (`visit(f, u)`
          // returning f's result from inside the loop): the body once per element, the variable
          // bound to it, as the code generator emits it, until a return gives the type.
          std::vector<const Ty*> elements;
          if (activeSubst && !returnFrames.empty() && !returnFrames.back() && forConstexprElements(f, elements))
          {
            Subst bound = *activeSubst;
            bound.pairs.push_back({v, nullptr});
            const Subst* saved = activeSubst;
            activeSubst = &bound;
            for (const Ty* element : elements)
            {
              bound.pairs.back().type = element;
              size_t inner = locals.mark();
              walkStmt(f->body);
              locals.pop(inner);
              if (returnFrames.back())
                break;
            }
            activeSubst = saved;
          }
          locals.pop(mark);
          break;
        }
        case NodeKind::WhileStmt:
        {
          auto* w = static_cast<WhileStmt*>(s);
          size_t mark = locals.mark();
          walkNode(w->cond);
          walkStmt(w->body);
          locals.pop(mark);
          break;
        }
        case NodeKind::DoStmt:
          walkStmt(static_cast<DoStmt*>(s)->body);
          walkExpr(static_cast<DoStmt*>(s)->cond);
          break;
        case NodeKind::SwitchStmt:
        {
          auto* w = static_cast<SwitchStmt*>(s);
          size_t mark = locals.mark();
          walkStmt(w->init);
          walkNode(w->cond);
          walkStmt(w->body);
          locals.pop(mark);
          break;
        }
        case NodeKind::CaseStmt:
          walkExpr(static_cast<CaseStmt*>(s)->value);
          walkExpr(static_cast<CaseStmt*>(s)->rangeEnd);
          break;
        case NodeKind::ReturnStmt:
        {
          const Ty* t = walkExpr(static_cast<ReturnStmt*>(s)->value);
          if (!decltypeReturns.empty() && decltypeReturns.back())
            t = decltypeOf(static_cast<ReturnStmt*>(s)->value, t);
          // Inside a for constexpr body of an instantiation, before the loop binds its variable,
          // a return that depends on it gives no type (the per-element walk does).
          bool open = forConstexprDepth > 0 && activeSubst && t && (types.isDependent(t) || impl.containsAuto(t));
          if (!returnFrames.empty() && !returnFrames.back() && t && t->isKnown() && !open)
            returnFrames.back() = t;
          break;
        }
        case NodeKind::TryStmt:
        {
          auto* t = static_cast<TryStmt*>(s);
          walkStmt(t->body);
          for (const TryStmt::Handler& h : t->handlers)
          {
            size_t mark = locals.mark();
            if (h.param)
              walkParam(static_cast<ParamDecl*>(h.param));
            walkStmt(h.body);
            locals.pop(mark);
          }
          break;
        }
        default: break;
      }
    }

    void walkExprs(Span<Expr*> exprs)
    {
      for (Expr* e : exprs)
        walkExpr(e);
    }

    // Resolves names in and computes the type of an expression.
    const Ty* walkExpr(Expr* e)
    {
      if (!e)
        return nullptr;
      struct Current
      {
        Expr*& slot;
        Expr* saved;
        Current(Expr*& slot, Expr* e) : slot(slot), saved(slot) { slot = e; }
        ~Current() { slot = saved; }
      } guard(current, e);
      switch (e->kind)
      {
        case NodeKind::NameExpr: return record(e, resolveName(static_cast<NameExpr*>(e)));
        case NodeKind::CallerExpr: return record(e, types.unknown()); // typed by the parameter it defaults
        case NodeKind::NameofExpr:
          walkExpr(static_cast<NameofExpr*>(e)->operand);
          return record(e, types.pointer(types.qualified(types.charType(), true, false)));
        case NodeKind::NoexceptExpr:
        {
          auto* n = static_cast<NoexceptExpr*>(e);
          walkExpr(n->operand);
          if (!inGenericTemplate())
          {
            std::string unsupported;
            bool throws = canThrow(n->operand, unsupported);
            if (!unsupported.empty())
              impl.reportCheckerError(file, e->token, "the noexcept operator does not decide " + unsupported + " yet");
            else
              n->value = throws ? 0 : 1;
          }
          return record(e, types.boolType());
        }
        case NodeKind::LiteralExpr: return record(e, typeOfLiteral(static_cast<LiteralExpr*>(e)));
        case NodeKind::ThisExpr: return record(e, thisType ? thisType : unknown(Why::Unsupported));
        case NodeKind::CallExpr: return record(e, typeOfCall(static_cast<CallExpr*>(e)));
        case NodeKind::IndexExpr:
        {
          auto* i = static_cast<IndexExpr*>(e);
          checkPostfixAfterParenthesisedType(i->base, i->token);
          const Ty* base = walkExpr(i->base);
          const Ty* firstIndex = nullptr;
          // `p[Integer]` names a typed union's alternative, which no lookup finds: a lone name as the
          // index is judged undeclared once the subscript had its say (judged: its base is known).
          struct TagLater
          {
            Walker& w;
            NameExpr* tag;
            const NameExpr* saved;
            bool judged = false;
            ~TagLater()
            {
              w.undeclaredLater = saved;
              if (tag && judged && !tag->resolved)
                w.reportUndeclared(tag->name, tag->token);
            }
          } later{*this,
                  i->args.size() == 1 && i->args[0]->kind == NodeKind::NameExpr && static_cast<NameExpr*>(i->args[0])->name.parts.size() == 1
                    ? static_cast<NameExpr*>(i->args[0]) : nullptr,
                  undeclaredLater};
          undeclaredLater = later.tag;
          for (Expr* a : i->args)
          {
            const Ty* at = walkExpr(a);
            if (a == i->args[0])
              firstIndex = at;
          }
          undeclaredLater = later.saved;
          const Ty* b = types.stripRef(base);
          if (!b || !b->isKnown())
            return record(e, types.unknown());
          if (b->kind == TyKind::Pointer || b->kind == TyKind::Array)
          {
            later.judged = true;
            return record(e, types.lref(b->inner));
          }
          if (b->kind == TyKind::Param)
            return record(e, b);
          later.judged = true;
          if (b->kind == TyKind::Class)
          {
            if (ClassDecl* u = typedUnionOf(b); u && i->args.size() == 1)
            {
              // `p[Property::Integer]`: the alternative the enumerator names (design.md 8.5); `p[a]`
              // with a descriptor of alternatives(U): the one it describes.
              std::optional<int64_t> v;
              size_t index = 0;
              if (const Ty* it = firstIndex ? types.stripRef(firstIndex) : nullptr; it && it->kind == TyKind::Member)
              {
                if (alternativeOf(it, index) != u)
                  return record(e, unknown(Why::Operator)); // a descriptor of another union, or of a member
                v = int64_t(index);
              }
              else if (firstIndex && types.isDependent(firstIndex))
                return record(e, types.unknown()); // `u[a]` before the loop binds a
              else if (EnumeratorDecl* en = alternativeNamed(u, i->args[0]))
              {
                // `p[Integer]`: an alternative's name as written in the union (design.md 8.5).
                auto* n = static_cast<NameExpr*>(i->args[0]);
                n->resolved = en;
                record(n, impl.tagTypeOf(u, b));
                v = impl.constOfDecl(en, ctx(), 0);
              }
              else
                v = impl.constEval(i->args[0], ctx());
              if (!v || *v < 0 || size_t(*v) >= u->alternatives.size())
                return record(e, unknown(Why::Operator));
              Subst s = b->args.empty() ? Subst{} : impl.substFor(b);
              const Ty* alt = impl.declaredTypeOf(u->alternatives[size_t(*v)], s.pairs.empty() ? nullptr : &s, 0);
              if (!alt || !alt->isKnown())
                return record(e, types.unknown());
              return record(e, types.lref(b->isConst ? types.qualified(alt, true, false) : alt));
            }
            if (types.isDependent(b))
              return record(e, types.unknown());
            MemberResult r = memberOf(b, impl.opSymbols[uint32_t(P::LBracket)], nullptr, i->token, false);
            if (r.decl && r.decl->kind == NodeKind::FunctionDecl)
            {
              bool ambiguous;
              if (FunctionDecl* f = pickOverload(r.decl, r.overloads, argTypesOf(i->args), &r.subst, b, ambiguous))
                return record(e, returnTypeOfCall(f, &r.subst, i->args, nullptr));
            }
            return record(e, unknown(Why::Operator));
          }
          return record(e, unknown(Why::Operator));
        }
        case NodeKind::MemberExpr: return record(e, typeOfMember(static_cast<MemberExpr*>(e)));
        case NodeKind::UnaryExpr: return record(e, typeOfUnary(static_cast<UnaryExpr*>(e)));
        case NodeKind::BinaryExpr: return record(e, typeOfBinary(static_cast<BinaryExpr*>(e)));
        case NodeKind::ConditionalExpr:
        {
          auto* c = static_cast<ConditionalExpr*>(e);
          const Ty* cond = walkExpr(c->cond);
          const Ty* then = walkExpr(c->then);
          const Ty* other = walkExpr(c->else_);
          if (!c->then)
            return record(e, cond); // Elvis
          const Ty* a = types.stripRef(then);
          const Ty* b = types.stripRef(other);
          if (!a || !b || !a->isKnown())
            return record(e, b && b->isKnown() ? other : types.unknown());
          if (!b->isKnown())
            return record(e, then);
          // `info ? info->buffer : nullptr`: the other branch's type, an array decayed to a pointer.
          if (a->kind == TyKind::Nullptr)
            return record(e, b->kind == TyKind::Array ? types.decay(b) : other);
          if (b->kind == TyKind::Nullptr)
            return record(e, a->kind == TyKind::Array ? types.decay(a) : then);
          // Two arithmetic or unscoped enumeration operands of two types: the usual arithmetic
          // conversions ([expr.cond]/7.2): `c ? U200 : 0.1f` is a float, `c ? small : -7` an int.
          auto arithmeticOrEnum = [](const Ty* t) { return t->isArithmetic() || (t->kind == TyKind::Enum && !static_cast<EnumDecl*>(t->decl)->isClass); };
          if (arithmeticOrEnum(a) && arithmeticOrEnum(b) && types.unqualified(a) != types.unqualified(b))
            return record(e, arithmetic(a, b));
          // `on ? "true" : "false"`, `c ? buffer : fallback`, `c ? derived : base`: arrays decay to
          // pointers, and two pointers meet at their composite pointer type.
          if (a != b && (a->kind == TyKind::Array || a->kind == TyKind::Pointer) && (b->kind == TyKind::Array || b->kind == TyKind::Pointer))
            if (const Ty* composite = impl.compositePointer(types.decay(a), types.decay(b)))
              return record(e, composite);
          // `cond ? "_" : someString`: the class branch's type; the other converts to it. Unless only
          // the class converts to the other (`c ? modifier : 0.0` with `operator double()`): then the
          // other's type, as C++ tries each way.
          if ((a->kind == TyKind::Class) != (b->kind == TyKind::Class))
          {
            const Ty* object = a->kind == TyKind::Class ? then : other;
            const Ty* value = a->kind == TyKind::Class ? other : then;
            const Ty* cls = types.unqualified(types.stripRef(object));
            const Ty* plain = types.unqualified(types.stripRef(value));
            if (plain->kind != TyKind::Pointer && plain->kind != TyKind::Array &&
                impl.conversionRank(object, plain) < Impl::NotViable && !(impl.conversionRank(value, cls) < Impl::NotViable))
              return record(e, plain);
            return record(e, cls);
          }
          // Two classes: the one the other converts to, as C++ tries each way (`first && second ?
          // Undo(first->start, second->end) : first` with first an Optional<Undo> is an Optional<Undo>).
          if (a->kind == TyKind::Class && b->kind == TyKind::Class && types.unqualified(a) != types.unqualified(b))
          {
            // One a base of the other (`negate ? unary : expression` with a UnaryExpression, which
            // derives from NoiseExpression, and a NoiseExpression): the derived one converts to the
            // base, the base never to the derived through a converting constructor (C++ [expr.cond]
            // 4.3.1). Two lvalues, the base qualified at least as the derived, make an lvalue of the
            // base (4.1).
            bool aBase = impl.isBaseClass(types.unqualified(a), types.unqualified(b));
            if (aBase || impl.isBaseClass(types.unqualified(b), types.unqualified(a)))
            {
              const Ty* base = aBase ? a : b;
              const Ty* derived = aBase ? b : a;
              bool lvalues = argumentType(c->then, then, types)->kind == TyKind::LRef && argumentType(c->else_, other, types)->kind == TyKind::LRef;
              bool binds = (base->isConst || !derived->isConst) && (base->isVolatile || !derived->isVolatile);
              return record(e, lvalues && binds ? types.lref(base) : types.unqualified(base));
            }
            bool toB = impl.conversionRank(then, types.unqualified(b)) < Impl::NotViable;
            bool toA = impl.conversionRank(other, types.unqualified(a)) < Impl::NotViable;
            if (toB != toA)
              return record(e, types.unqualified(toB ? b : a));
          }
          return record(e, then);
        }
        case NodeKind::CastExpr:
        {
          auto* c = static_cast<CastExpr*>(e);
          walkType(c->type);
          const Ty* operand = walkExpr(c->operand);
          return record(e, probedCast(typeOf(c->type), operand));
        }
        case NodeKind::NamedCastExpr:
        {
          auto* c = static_cast<NamedCastExpr*>(e);
          walkType(c->type);
          const Ty* operand = walkExpr(c->operand);
          const Ty* t = typeOf(c->type);
          return record(e, c->castKind == Kw::static_cast_ ? probedCast(t, operand) : t);
        }
        case NodeKind::SizeofExpr:
        {
          auto* z = static_cast<SizeofExpr*>(e);
          walkType(z->type);
          walkExpr(z->operand);
          // `sizeof(s->items)` of a flexible array member (or of `extern int table[];`): an array
          // without a length has no size ([expr.sizeof] 1), never 0.
          if (!z->isPack && z->sizeofKind != Kw::alignof_)
            if (VariableDecl* v = unboundedArray(z->operand ? static_cast<Node*>(z->operand) : z->type))
              impl.reportCheckerError(file, e->token, "sizeof of " + std::string(symbols.name(v->name.last().symbol)) +
                                                          ", an array without a length (a flexible array member), which has no size");
          return record(e, types.sizeType());
        }
        case NodeKind::NewExpr:
        {
          auto* n = static_cast<NewExpr*>(e);
          walkExprs(n->placement);
          if (n->placement.size() == 1)
            if (Decl* constant = constObjectPlaced(n->placement[0]))
            {
              uint32_t name = constant->kind == NodeKind::ParamDecl ? static_cast<ParamDecl*>(constant)->name
                                                                     : static_cast<VariableDecl*>(constant)->name.last().symbol;
              impl.reportCheckerError(file, n->token, "a placement new over " + (name == UINT32_MAX ? std::string("an object") : "'" + std::string(symbols.name(name)) + "'") +
                                                          ", which is declared const: a const object is never replaced (design.md 21)");
            }
          walkType(n->type);
          walkExprs(n->args);
          const Ty* t = typeOf(n->type);
          if (t->kind == TyKind::Array)
            t = t->inner;
          else if (impl.isUndeducedClassTemplate(t))
            if (const Ty* deduced = impl.deduceClassArgs(t, argTypesOf(n->args)))
              t = deduced; // `new DeleteHelper(old.release())`: the class's arguments from a constructor (CTAD)
          if (!n->braced && !n->args.empty())
            bindOverloadSetsOfConstruction(t, n->args);
          if (impl.holdsText(t) && !inGenericTemplate())
            reportTextError(n->token, impl.isTextClosure(t) ? "a lambda capturing a Text cannot be stored (design.md 14)" : textStored("allocated"));
          if (!n->braced)
            packText(n->args, textConstructor(t, n->args), n->token); // `new Problem("bad ", point)`
          return record(e, t->isKnown() ? types.pointer(t) : t);
        }
        case NodeKind::DeleteExpr:
          walkExpr(static_cast<DeleteExpr*>(e)->operand);
          return record(e, types.voidType());
        case NodeKind::LambdaExpr:
        {
          auto* l = static_cast<LambdaExpr*>(e);
          bool generic = !l->templateParams.empty(); // a template: its call operator is typed per call
          std::vector<LocalStack::Item> environment;
          if (generic)
            environment = locals.items; // what the body can see, kept for typing it for each call
          size_t mark = locals.mark();
          for (const LambdaCapture& cap : l->captures)
            if (cap.decl)
              walkLocalDecl(cap.decl); // init-capture: a variable of the closure, typed from its initializer
            else if (Decl* captured = cap.isThis ? nullptr : locals.find(cap.symbol); captured && captured->when)
            {
              bool undecided = false;
              whenUse(cap.symbol, l->token, captured, undecided); // (design.md 19: a use of the local)
            }
          pushTemplateParams(l->templateParams);
          std::vector<const Ty*> params;
          for (Decl* p : l->params)
          {
            auto* pd = static_cast<ParamDecl*>(p);
            walkParam(pd);
            const Ty* t = impl.declaredTypeOf(p, activeSubst, 0);
            // As this walk sees the type, its locals included: `const Layers::value_type&` with
            // `using Layers = ...;` in a function around the lambda, which the declaration's own
            // context does not see, nor the template parameters of a lambda around this one
            // (`[&](ChooseButton<IDType>*)` in `[&]<class IDType>(...)`, which it read as a draft
            // class). The parameter's uses take it from the local.
            if (const Ty* walked = pd->type ? typeOf(pd->type) : nullptr;
                walked && walked->isKnown() && !impl.containsAuto(walked) && !Impl::mentionsUnknown(walked) &&
                (((!t || !t->isKnown() || types.isDependent(t)) && !types.isDependent(walked)) || (t && Impl::mentionsUnknown(t))))
            {
              t = walked->kind == TyKind::Array ? types.decay(walked) : walked;
              if (!locals.items.empty() && locals.items.back().decl == p)
                locals.items.back().type = t;
            }
            params.push_back(t);
          }
          walkType(l->returnType);
          const Ty* result = nullptr;
          if (l->returnType)
            result = typeOf(l->returnType);
          returnFrames.push_back(nullptr);
          decltypeReturns.push_back(l->returnType && l->returnType->kind == NodeKind::AutoType && static_cast<AutoType*>(l->returnType)->isDecltypeAuto);
          walkStmt(l->body);
          markTextCapture(l, mark);
          const Ty* deduced = returnFrames.back();
          returnFrames.pop_back();
          decltypeReturns.pop_back();
          if (!result)
            result = deduced ? types.decay(deduced) : types.voidType();
          else if (impl.containsAuto(result))
            result = deduced ? impl.deducedShape(result, deduced) : types.voidType(); // `-> decltype(auto)`, `-> auto&`
          if (generic && (!result->isKnown() || types.isDependent(result)))
            result = nullptr; // deduced per call
          locals.pop(mark);
          // In a template, the lambda is a different type in each instantiation (its body differs).
          // The mark telling them apart is a hash of the arguments as printed (with their classes'
          // full paths: two classes named LineInfo are two arguments), in parameter-name order, so
          // that it (and every name printed with it) is the same in every run. A body typed again
          // with no arguments at all (a function's `auto` return deduced on demand) is no
          // instantiation: its lambda is the one of the plain walk.
          int64_t instance = 0;
          if (activeSubst && !activeSubst->pairs.empty())
          {
            std::vector<std::string> printed;
            for (auto& [d, ty] : activeSubst->pairs)
              printed.push_back(std::string(symbols.name(static_cast<TemplateParamDecl*>(d)->name)) + "=" + types.toString(ty, &symbols, true));
            std::sort(printed.begin(), printed.end());
            uint64_t h = 1469598103934665603ull;
            for (const std::string& p : printed)
              for (unsigned char c : p + ";")
                h = (h ^ c) * 1099511628211ull;
            instance = int64_t(h ? h : 1);
          }
          const Ty* lambdaTy = types.lambda(result, Span<const Ty*>{params.data(), uint32_t(params.size())}, l->closure, instance);
          if (generic)
          {
            Impl::LambdaEnv env;
            env.lambda = l;
            env.locals = std::move(environment);
            env.scope = scope;
            env.thisType = thisType;
            if (activeSubst)
              env.subst = *activeSubst;
            env.tokens = &tokens;
            env.text = text;
            env.file = file;
            env.depth = instantiationDepth;
            std::lock_guard lock(impl.lambdaEnvMutex);
            impl.lambdaEnvs[lambdaTy] = std::move(env);
          }
          return record(e, lambdaTy);
        }
        case NodeKind::BraceInitExpr:
          walkExprs(static_cast<BraceInitExpr*>(e)->args);
          if (static_cast<BraceInitExpr*>(e)->text)
            if (const Ty* text = impl.textType())
              return record(e, text); // a call's arguments packed for its Text parameter (packText)
          return record(e, unknown(Why::Unsupported));
        case NodeKind::ThrowExpr:
          walkExpr(static_cast<ThrowExpr*>(e)->operand);
          return record(e, types.voidType());
        case NodeKind::FunctionalCastExpr:
        {
          auto* c = static_cast<FunctionalCastExpr*>(e);
          walkType(c->type);
          const Ty* operand = nullptr;
          for (Expr* a : c->args)
          {
            const Ty* at = walkExpr(a);
            if (a == c->args[0])
              operand = at;
          }
          const Ty* t = typeOf(c->type);
          if (t->kind == TyKind::Auto && c->args.size() == 1 && operand)
            t = types.decay(operand); // auto(x)
          if (c->args.size() == 1 && !c->braced)
            t = probedCast(t, operand); // `bool(instance)` in a requirement
          if (impl.isUndeducedClassTemplate(t))
          {
            // `Pair(a, b)`, `Span(p, n)`, `Pair{a, b}` in an expression: the class's arguments from a
            // constructor, or from an aggregate's members (CTAD).
            Span<Expr*> args = c->args;
            if (c->braced && args.size() == 1 && args[0]->kind == NodeKind::BraceInitExpr)
              args = static_cast<BraceInitExpr*>(args[0])->args;
            std::vector<const Ty*> argTypes = argTypesOf(args);
            if (const Ty* deduced = impl.deduceClassArgs(t, argTypes))
              t = deduced;
            else if (const Ty* aggregate = c->braced ? impl.deduceAggregateArgs(t, argTypes) : nullptr)
              t = aggregate;
          }
          if (!c->args.empty())
          {
            // (`NamedFunction{set}`: the list's elements, for the aggregate's members or the constructor.)
            Span<Expr*> args = c->args;
            if (c->braced && args.size() == 1 && args[0]->kind == NodeKind::BraceInitExpr)
            {
              bindTemplateAddresses(args[0], t);
              args = static_cast<BraceInitExpr*>(args[0])->args;
            }
            bindOverloadSetsOfConstruction(t, args);
          }
          if (!c->braced)
            packText(c->args, textConstructor(t, c->args), c->token); // `Problem("bad ", point)`, `Problem problem("bad ", point);`
          return record(e, t);
        }
        case NodeKind::TypeidExpr:
        {
          auto* t = static_cast<TypeidExpr*>(e);
          walkNode(t->operand);
          if (t->operand && uint8_t(t->operand->kind) <= uint8_t(NodeKind::PackType))
          {
            // typeid(Widget), or typeid(q) with q a variable the parser read as a type name.
            auto* n = t->operand->kind == NodeKind::NamedType ? static_cast<NamedType*>(t->operand) : nullptr;
            Lookup l = n ? impl.lookupQualified(n->name, scope, &locals, 0, activeSubst) : Lookup{};
            if (l.decl && (l.decl->kind == NodeKind::VariableDecl || l.decl->kind == NodeKind::ParamDecl) && !(l.decl->flags & DF_Typedef))
            {
              t->operandDecl = l.decl;
              t->operandType = impl.declaredTypeOf(l.decl, activeSubst, 0);
            }
            else
              t->operandType = typeOf(static_cast<Type*>(t->operand));
          }
          if (Decl* info = impl.typeInfoDecl())
            return record(e, types.classType(info, {})); // the library's TypeInfo; the code generator fills in the name
          return record(e, unknown(Why::Unsupported));
        }
        case NodeKind::FoldExpr:
        {
          auto* f = static_cast<FoldExpr*>(e);
          const Ty* l = walkExpr(f->lhs);
          const Ty* r = walkExpr(f->rhs);
          if (f->op == uint32_t(P::Comma))
            return record(e, types.voidType());
          const Ty* pattern = l ? l : r;
          if (pattern && pattern->kind == TyKind::Pack)
            pattern = pattern->args.empty() ? types.voidType() : pattern->args[0];
          return record(e, pattern ? pattern : types.unknown());
        }
        case NodeKind::RequiresExpr:
        {
          // `requires { t.save(); }`: true when every requirement is well-typed for the types at
          // hand, probed on a scratch walker so that the misses count in no statistic.
          auto* r = static_cast<RequiresExpr*>(e);
          ResolveStats scratch;
          Walker probe(impl, scratch, tokens, text);
          probe.scope = scope;
          probe.locals = locals;
          probe.activeSubst = activeSubst;
          probe.thisType = thisType;
          probe.probing = true;
          for (Decl* p : r->params)
          {
            // As the walk sees its type (`requires(const T& value)` in `[]<class T>(T) { ... }`, or
            // with `using T = L::ThePrototypeType;` in the lambda's body): the declaration's own
            // context sees neither a lambda's template parameters nor its body's aliases.
            auto* pd = static_cast<ParamDecl*>(p);
            probe.walkParam(pd);
            if (const Ty* walked = pd->type ? probe.typeOf(pd->type) : nullptr;
                walked && walked->isKnown() && !impl.containsAuto(walked) && !Impl::mentionsUnknown(walked))
              probe.locals.items.back().type = walked->kind == TyKind::Array ? types.decay(walked) : walked;
          }
          // Each requirement on a copy of its own, its names' access checked from where the
          // requires-expression is (Impl::namesOnlyAccessible): the function walked, its class and
          // the classes around it, as the instance it is (self).
          std::vector<Scope*> context;
          if (bodyOf)
            impl.accessContext(bodyOf, context);
          for (Scope* s = scope; s; s = s->parent)
            if (s->kind == ScopeKind::Class)
              context.push_back(s);
          const Ty* self = nullptr;
          if (activeSubst && bodyOf)
            if (Decl* cls = impl.owningClassOf(bodyOf); cls && cls->kind == NodeKind::ClassDecl)
              self = impl.substitute(impl.selfType(cls), *activeSubst);
          InstanceCopier copier(impl.instanceArena());
          bool satisfied = true;
          for (Type* t : r->typeRequirements)
          {
            Type* copy = copier.typeAlone(t);
            probe.walkType(copy); // (its names resolved, for their access)
            if (const Ty* ty = probe.typeOf(copy); !ty || !ty->isKnown() || (!impl.types.isDependent(ty) && !impl.namesOnlyAccessible(copy, bodyOf, context, self)))
              satisfied = false;
          }
          for (Expr* requirement : r->requirements)
          {
            Expr* copy = copier.expression(requirement);
            if (const Ty* ty = probe.walkExpr(copy); !ty || !ty->isKnown() || probe.namesFunctionsWithoutTarget(copy) ||
                                                     (!impl.types.isDependent(ty) && !impl.namesOnlyAccessible(copy, bodyOf, context, self)))
              satisfied = false;
          }
          r->satisfied = satisfied;
          return record(e, types.boolType());
        }
        case NodeKind::ParenExpr:
          return record(e, walkExpr(static_cast<ParenExpr*>(e)->inner));
        default:
          return record(e, unknown(Why::Unsupported));
      }
    }
  };

  // The call operator of a generic lambda for one set of its own template arguments: a copy of
  // the lambda's parameters and body (InstanceCopier::lambdaInstance) typed in the environment the
  // lambda was written in, once per (lambda type, arguments) and kept; body gets the copy.
  // Returns the operator's function type (parameters and result), or null when the environment
  // is unknown, the result has no type, or the lambda is asked for while it is being typed.
  const Ty* checkLambdaInstance(const Ty* lambdaTy, const Subst& own, Subst* fullOut, ResolveStats* statsInto, LambdaExpr** body = nullptr)
  {
    LambdaEnv env;
    {
      std::lock_guard lock(lambdaEnvMutex);
      auto it = lambdaEnvs.find(lambdaTy);
      if (it == lambdaEnvs.end())
        return nullptr;
      env = it->second;
    }
    LambdaExpr* original = env.lambda;
    if (!original || !env.tokens || env.depth > 16)
      return nullptr;
    Subst full = env.subst;
    for (const Subst::Pair& p : own.pairs)
      if (!full.find(p.param))
        full.pairs.push_back(p);
    if (fullOut)
      *fullOut = full;
    bool producer;
    InstanceRecord* inst = claimInstance(original->closure, own, lambdaTy, producer);
    std::optional<RecordInto> into; // what typing the instance reads is its own
    if (producer)
      into.emplace(&inst->deps);
    if (!producer)
    {
      if (inst->state.load(std::memory_order_acquire) != 2)
      {
        if (inst->owner == std::this_thread::get_id())
          return nullptr; // the lambda calling itself through a capture
        ++instWaits;
        for (int seen = inst->state.load(std::memory_order_acquire); seen != 2; seen = inst->state.load(std::memory_order_acquire))
          inst->state.wait(seen, std::memory_order_acquire); // blocks (C++20 atomic wait) instead of spinning on yield
      }
      if (body)
        *body = inst->lambda;
      return inst->deducedReturn; // for a lambda: the call operator's type for these arguments
    }
    if (recordingDependencies)
    {
      // How to check it again (recheckInstancesOf): its closure type and own arguments.
      inst->recheck = std::make_unique<InstanceRecord::Recheck>();
      inst->recheck->lambdaTy = lambdaTy;
      inst->recheck->subst = own;
    }
    return runLambdaInstance(inst, env, full, statsInto, body);
  }

  // A generic lambda's call operator for the arguments full: its body copied and walked into inst.
  const Ty* runLambdaInstance(InstanceRecord* inst, const LambdaEnv& env, const Subst& full, ResolveStats* statsInto, LambdaExpr** body = nullptr)
  {
    LambdaExpr* original = env.lambda;
    LambdaExpr* l = InstanceCopier(instanceArena()).lambdaInstance(original);
    inst->lambda = l;
    ++instBodies;
    inst->counted |= 1;
    ResolveStats local;
    if (statsInto)
    {
      local.fileIndex = env.file ? env.file->index : statsInto->fileIndex; // the lambda's file's tokens are what it records
      local.collectWhy = statsInto->collectWhy;
    }
    Walker sub(*this, local, *env.tokens, env.text);
    sub.file = env.file;
    FileOfLookups walkedFile(env.file);
    sub.scope = env.scope;
    sub.thisType = env.thisType;
    sub.activeSubst = &full;
    sub.instantiationDepth = env.depth + 1;
    sub.locals.items = env.locals;
    for (const LambdaCapture& cap : l->captures)
      if (cap.decl)
        sub.walkLocalDecl(cap.decl);
    sub.pushTemplateParams(l->templateParams);
    std::vector<const Ty*> params;
    for (Decl* p : l->params)
    {
      sub.walkParam(static_cast<ParamDecl*>(p));
      const Ty* pt = declaredTypeOf(p, &full, 0);
      if (!(static_cast<ParamDecl*>(p)->isPack && spliceIfPack(pt, params)))
        params.push_back(pt);
    }
    sub.walkType(l->returnType);
    const Ty* result = nullptr;
    if (l->returnType)
      result = sub.typeOf(l->returnType);
    sub.returnFrames.push_back(nullptr);
    sub.walkStmt(l->body);
    const Ty* deduced = sub.returnFrames.back();
    sub.returnFrames.pop_back();
    if (!result)
      result = deduced ? types.decay(deduced) : types.voidType();
    local.instExprsTyped += local.exprsTyped;
    local.instExprsDependent += local.exprsDependent;
    local.instExprsUnknown += local.exprsUnknown;
    addInstanceStats(local, inst); // counted once, whoever asked (instanceStatsTotal)
    const Ty* callTy = nullptr;
    bool known = result->isKnown() && !types.isDependent(result);
    for (const Ty* pt : params)
      known = known && pt && pt->isKnown();
    if (known)
      callTy = types.function(result, Span<const Ty*>{params.data(), uint32_t(params.size())}, false);
    inst->deducedReturn = callTy;
    inst->state.store(2, std::memory_order_release);
    inst->state.notify_all();
    if (body)
      *body = l;
    return callTy;
  }

  // The deduced return type of an auto-returning function template for one instantiation,
  // checking its body now if no call has yet.
  const Ty* deducedReturnOf(FunctionDecl* f, const Subst& subst)
  {
    if (!f->file || !f->file->tokens)
      return nullptr;
    ResolveStats scratch;
    Walker w(*this, scratch, *f->file->tokens, f->file->text);
    for (Decl* p = f; p; p = p->parent)
      for (Decl* tp : p->templateParams)
        w.locals.push(static_cast<TemplateParamDecl*>(tp)->name, tp);
    w.scope = lexicalScopeOf(f);
    if (f->name.parts.size() > 1)
      w.scope = w.semanticScopeFor(f->name);
    return w.instantiateFunction(f, subst, true);
  }

  // The body a constant evaluation runs for def (a function's definition) under subst: the copy
  // its instance's walk typed, made now or waited for, through the record the checker's instance
  // checks use, never def's own nodes, which its file's walk may not have reached yet (kpp's order
  // independence: a constant above the function, or in a file walked first) or may be writing on
  // another thread meanwhile. For a template's function (its own, or its class's) the instance's;
  // there a call with a type-dependent argument names what the instance's types choose (C++
  // [temp.dep.candidate]). For a plain function the one walk of it with no arguments, which an
  // `auto` function's return deduced before its file's walk shares. def itself when an argument is
  // not known, or this thread is walking the copy (a call in its own body).
  FunctionDecl* constInstanceBody(FunctionDecl* def, const Subst* subst)
  {
    if (!def->body || !def->file || !def->file->tokens)
      return def;
    Decl* cls = owningClassOf(def);
    Span<Decl*> classParams = cls ? effectiveParams(cls) : Span<Decl*>{};
    if (!subst && (!def->templateParams.empty() || !classParams.empty()))
      return def;
    Subst classSubst, own;
    for (Decl* p : classParams)
    {
      const Ty* t = subst->find(p);
      if (!t)
        return def;
      classSubst.pairs.push_back({p, t});
    }
    // An out-of-line member's own copy of its class's parameter list takes the class's arguments
    // in order, as the class's check has it (checkMemberBody).
    bool outOfLine = cls && def->parent != cls && def->templateParams.size() == cls->templateParams.size();
    for (size_t i = 0; i < def->templateParams.size(); ++i)
    {
      const Ty* t = outOfLine ? classSubst.find(cls->templateParams[i]) : subst->find(def->templateParams[i]);
      if (!t)
        return def;
      own.pairs.push_back({def->templateParams[i], t});
    }
    ResolveStats scratch;
    Walker w(*this, scratch, *def->file->tokens, def->file->text);
    for (Decl* p = def; p; p = p->parent)
      for (Decl* tp : p->templateParams)
        w.locals.push(static_cast<TemplateParamDecl*>(tp)->name, tp);
    w.scope = lexicalScopeOf(def);
    if (def->name.parts.size() > 1)
      w.scope = w.semanticScopeFor(def->name);
    InstanceRecord* record = nullptr;
    w.instantiateFunction(def, own, true, classSubst.empty() ? nullptr : &classSubst, nullptr, &record);
    if (!record || record->state.load(std::memory_order_acquire) != 2 || !record->body)
      return def;
    return record->body;
  }

  // The initializer a constant evaluation reads for v (a namespace-scope or static constant's
  // definition, a trait's static value, a data member's default) under subst (the arguments of the
  // class template it is a member of, or none): a copy of it as a walk of its own typed it, made now
  // or waited for, through the kind of record GlobalScope::instanceInitializer keeps for variable
  // templates; never v's own nodes, which its file's walk may not have reached yet or may be writing
  // on another thread (a constant of another file read the overload set's first function, an array's
  // bound 100 or 4 by the order the files were walked in). v->init itself when an argument is not
  // known, or this thread is typing the copy.
  Expr* constInitializerOf(VariableDecl* v, const Subst* subst)
  {
    if (!v->init)
      return nullptr;
    Scope* scope = lexicalScopeOf(v);
    if (v->name.parts.size() > 1)
    {
      ResolveStats scratch;
      if (v->file && v->file->tokens)
        scope = Walker(*this, scratch, *v->file->tokens, v->file->text).semanticScopeFor(v->name);
    }
    return constWalkedCopy(v, v->init, v->file, scope, v, subst);
  }

  // e (an expression of key's: a constant's initializer, an enumerator's value) as constInitializerOf
  // has it: a copy typed by a walk of its own in scope, with the template parameters of paramsOf and
  // its parents, under subst; the record of key keeps it.
  Expr* constWalkedCopy(const Decl* key, Expr* e, const ParsedFile* file, Scope* scope, Decl* paramsOf, const Subst* subst)
  {
    if (!e || !file || !file->tokens)
      return e;
    Subst s;
    if (subst)
      for (const Subst::Pair& p : subst->pairs)
      {
        if (!p.type || !p.type->isKnown() || types.isDependent(p.type) || mentionsUnknown(p.type))
          return e;
        s.pairs.push_back(p);
      }
    bool producer;
    InstanceRecord* record = claimInstance(key, s, nullptr, producer);
    if (!producer)
    {
      if (record->state.load(std::memory_order_acquire) != 2)
      {
        if (record->owner == std::this_thread::get_id())
          return e; // (its own expression, while this thread types the copy)
        for (int seen = record->state.load(std::memory_order_acquire); seen != 2; seen = record->state.load(std::memory_order_acquire))
          record->state.wait(seen, std::memory_order_acquire);
      }
      return record->init ? record->init : e;
    }
    RecordInto into(&record->deps); // what typing the copy reads is its own
    InstanceCopier copier(instanceArena());
    Expr* copy = copier.expression(e);
    record->init = copy;
    ResolveStats scratch;
    Walker w(*this, scratch, *file->tokens, file->text);
    for (Decl* p = paramsOf; p; p = p->parent)
      for (Decl* tp : p->templateParams)
        w.locals.push(static_cast<TemplateParamDecl*>(tp)->name, tp);
    w.scope = scope;
    if (!s.empty())
      w.activeSubst = &s;
    if (Decl* cls = enclosingClassDecl(w.scope))
      w.thisType = types.pointer(s.empty() ? selfType(cls) : substitute(selfType(cls), s));
    w.walkExpr(copy);
    record->state.store(2, std::memory_order_release);
    record->state.notify_all();
    return copy;
  }

