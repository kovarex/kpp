// GlobalScope::Impl, phase 2, types: part of resolver.cpp, included in the class's body there (a file of its
// own since 2026-11-05, moved without a change; docs/design.md, "Compilation model").
  // ---------------- phase 2: types ----------------

  struct ConstPlace;
  struct ConstBinding // a parameter's or a local's value while a function runs at compile time
  {
    uint32_t symbol;
    ConstValue value;
    const Ty* type = nullptr; // what an assignment converts to
    bool writable = true;     // false for a const one, a reference to const, a copy for a reference
    // A reference bound to a place: the locals it is in (this function's or a caller's, which
    // outlive it), the binding and the part; value then unused.
    std::vector<ConstBinding>* aliasEnv = nullptr;
    size_t aliasIndex = 0;
    std::vector<size_t> aliasPath;
    // A function parameter pack (`Args&&... args`): value's parts are its elements, read only one
    // at a time, by a pack expansion (constExpand). packPlaces: where each element's argument is
    // (env null: nowhere the run holds), which a reference element refers to.
    bool pack = false;
    std::vector<ConstPlace> packPlaces;
    // Its address was taken (`&x`): the serial an Address names it by (constStorage).
    uint64_t serial = 0;
    // An object the run made and destroys when the binding ends (constEndLocals): a local, a
    // by-value parameter, a temporary, a reference's that extends its life. Not a reference's that
    // copies an lvalue it cannot alias, nor `this`.
    bool owns = false;
  };
  struct TypeContext
  {
    Scope* scope;
    const LocalStack* locals;
    const Subst* subst;
    const std::vector<Token>* tokens = nullptr; // For evaluating array bounds written as literals.
    std::string_view text;
    std::vector<ConstBinding>* env = nullptr; // The parameters and locals of a function running at compile time.
    const Ty* returnType = nullptr;          // That function's declared result type (`return {a, b};`).
    // That function's reference result wanted as a place (`result[i] = x` through operator[]).
    std::optional<ConstPlace>* returnPlace = nullptr;
    // That function's named return variable (NRVO, constNamedReturn): `return v;` of it is the result.
    const VariableDecl* namedReturn = nullptr;
    FunctionDecl* function = nullptr; // Whose parameters a trailing return type names (`-> decltype(callable())`).
    // The variable template whose initializer is evaluated under subst (`hasBegin<Vector<int>>`):
    // a requires-expression in it holds or not for these arguments, whatever its last typing left.
    Decl* variableTemplate = nullptr;
    // One element of a pack expansion typed (TemplateArgs::elementPacks): the packs whose mentions
    // here name the element at elementIndex, subst binding them whole. Only the pattern's own
    // nodes see it: a context made for a declaration they name starts without it.
    const std::vector<Decl*>* elementPacks = nullptr;
    uint32_t elementIndex = 0;
  };

  // What a mention of template parameter d names, subst binding it to bound: under ctx's pack
  // element (an expansion's pattern), the element of a pack it expands; bound otherwise.
  static const Ty* elementOf(const Decl* d, const Ty* bound, const TypeContext& ctx)
  {
    if (!ctx.elementPacks || !bound || bound->kind != TyKind::Pack || ctx.elementIndex >= bound->args.size())
      return bound;
    for (const Decl* p : *ctx.elementPacks)
      if (p == d)
        return bound->args[ctx.elementIndex];
    return bound;
  }

  // A written template argument that names only types that exist under ctx's substitution,
  // the arguments of an alias template included: `VoidT<T::force_memcpy_save>` is void for any T
  // but well-formed only for a T with that member (the detection idiom's substitution failure).
  bool wellFormedArgument(Node* n, const TypeContext& ctx, int depth = 0)
  {
    if (!n || depth > 16)
      return false;
    if (uint8_t(n->kind) > uint8_t(NodeKind::PackType))
      return true; // a constant
    switch (n->kind)
    {
      case NodeKind::NamedType:
        for (const NamePart& part : static_cast<NamedType*>(n)->name.parts)
          for (Node* a : part.templateArgs)
            if (!wellFormedArgument(a, ctx, depth + 1))
              return false;
        break;
      case NodeKind::PointerType: return wellFormedArgument(static_cast<PointerType*>(n)->pointee, ctx, depth + 1);
      case NodeKind::LRefType: return wellFormedArgument(static_cast<LRefType*>(n)->referenced, ctx, depth + 1);
      case NodeKind::RRefType: return wellFormedArgument(static_cast<RRefType*>(n)->referenced, ctx, depth + 1);
      case NodeKind::ConstType: return wellFormedArgument(static_cast<ConstType*>(n)->inner, ctx, depth + 1);
      case NodeKind::PackType: return wellFormedArgument(static_cast<PackType*>(n)->pattern, ctx, depth + 1);
      default: break;
    }
    const Ty* t = convertType(static_cast<Type*>(n), ctx, depth + 1);
    return t && t->isKnown();
  }

  // The specialisation of a variable template a use with these arguments (one per parameter of
  // the primary) takes its value from, as C++ chooses it: the matching partial and full
  // specialisations, the most specialised of them (its parameters bound in out); the primary
  // when none matches. Null when the matches are ambiguous.
  VariableDecl* variableSpecialization(VariableDecl* primary, const std::vector<const Ty*>& args, Subst& out)
  {
    if (primary->name.empty() || args.empty())
      return primary;
    for (const Ty* a : args)
      if (!a || !a->isKnown() || types.isDependent(a))
        return primary;
    Lookup l;
    if (!lookupInScope(lexicalScopeOf(primary), primary->name.last().symbol, l, 0) || !l.overloads)
      return primary;
    struct Match { VariableDecl* v; Subst s; std::vector<const Ty*> patterns; };
    std::vector<Match> matches;
    for (const Entry& e : *l.overloads)
    {
      if (!e.decl || e.decl == primary || e.decl->kind != NodeKind::VariableDecl)
        continue;
      auto* v = static_cast<VariableDecl*>(e.decl);
      if (v->name.empty() || !v->name.last().hasTemplateArgs || v->name.last().templateArgs.size() != args.size())
        continue;
      Span<Node*> written = v->name.last().templateArgs;
      LocalStack own;
      for (Decl* p : v->templateParams)
        own.push(static_cast<TemplateParamDecl*>(p)->name, p);
      TypeContext sctx{lexicalScopeOf(v), v->templateParams.empty() ? nullptr : &own, nullptr,
                       v->file ? v->file->tokens : nullptr, v->file ? v->file->text : std::string_view()};
      Match m{v, {}, {}};
      bool ok = true;
      for (size_t i = 0; i < args.size() && ok; ++i)
      {
        Decl* param = i < primary->templateParams.size() ? primary->templateParams[i] : nullptr;
        const Ty* pattern = templateArgument(written[i], param, sctx, 0);
        m.patterns.push_back(pattern);
        if (!pattern || !pattern->isKnown())
          ok = false;
        else if (types.isDependent(pattern))
        {
          deduce(pattern, args[i], m.s);
          ok = substitute(pattern, m.s) == args[i];
        }
        else
          ok = pattern == args[i];
      }
      for (Decl* p : v->templateParams)
        ok = ok && m.s.find(p);
      if (!ok)
        continue;
      TypeContext dctx{sctx.scope, sctx.locals, &m.s, sctx.tokens, sctx.text};
      for (Node* w : written)
        ok = ok && wellFormedArgument(w, dctx);
      if (ok)
        matches.push_back(std::move(m));
    }
    if (matches.empty())
      return primary;
    // a is at least as specialised as b when b's arguments deduce from a's.
    auto atLeastAs = [&](const Match& a, const Match& b)
    {
      Subst t;
      for (size_t i = 0; i < a.patterns.size(); ++i)
      {
        if (!types.isDependent(b.patterns[i]))
        {
          if (b.patterns[i] != a.patterns[i])
            return false;
          continue;
        }
        deduce(b.patterns[i], a.patterns[i], t);
        if (substitute(b.patterns[i], t) != a.patterns[i])
          return false;
      }
      return true;
    };
    for (const Match& m : matches)
    {
      bool best = true;
      for (const Match& o : matches)
        if (&o != &m && !(atLeastAs(m, o) && !atLeastAs(o, m)))
          best = false;
      if (best)
      {
        out = m.s;
        return m.v;
      }
    }
    return nullptr;
  }

  // --- builtin type traits ---

  static bool isTypeNode(Node* n)
  {
    switch (n->kind)
    {
      case NodeKind::NamedType: case NodeKind::PointerType: case NodeKind::LRefType: case NodeKind::RRefType: case NodeKind::ConstType:
      case NodeKind::ArrayType: case NodeKind::FunctionType: case NodeKind::DecltypeType: case NodeKind::AutoType: case NodeKind::BuiltinType:
      case NodeKind::MemberPointerType: case NodeKind::PackType:
        return true;
      default: return false;
    }
  }

  // The argument types of a trait, or empty when one is not a type or is still dependent.
  std::vector<const Ty*> traitArgs(const NamePart& part, const TypeContext& ctx, int depth)
  {
    std::vector<const Ty*> args;
    for (Node* a : part.templateArgs)
    {
      if (!isTypeNode(a))
        return {};
      const Ty* t = convertType(static_cast<Type*>(a), ctx, depth + 1);
      if (t && t->kind == TyKind::Pack)
      {
        for (const Ty* element : t->args)
          args.push_back(element); // `Args...` bound to a pack: its elements
        continue;
      }
      if (!t || !t->isKnown() || types.isDependent(t))
        return {};
      args.push_back(t);
    }
    return args;
  }

  // The type an enumeration is declared with, read where the enumeration is declared, not where
  // UnderlyingType<E> is written (`enum class Enum : InternalType` with AlertType's `using
  // InternalType = uint8_t;`, met in DeserialiserHelper's loadEnum); int when it names none.
  const Ty* enumUnderlyingType(const Ty* e, const TypeContext& ctx, int depth)
  {
    Decl* def = e->decl;
    if (def && def->kind == NodeKind::EnumDecl && !def->ownScope)
    {
      Entry entry{def, nullptr};
      if (Scope* s = scopeOfEntry(entry, 0))
        def = s->owner;
    }
    if (!def || def->kind != NodeKind::EnumDecl || !static_cast<EnumDecl*>(def)->underlying)
      return types.intType();
    DeclContext dc;
    declContextFor(def, dc);
    TypeContext own{dc.scope, &dc.params, ctx.subst, def->file ? def->file->tokens : nullptr, def->file ? def->file->text : std::string_view()};
    return convertType(static_cast<EnumDecl*>(def)->underlying, own, depth + 1);
  }

  bool isTypeTraitName(uint32_t symbol)
  {
    std::string_view name = symbols.name(symbol);
    return name == "RemoveConst" || name == "RemoveCv" || name == "RemoveReference" || name == "RemoveExtent" || name == "RemovePointer" ||
           name == "Decay" || name == "UnderlyingType" || name == "AddConst" || name == "AddPointer" || name == "RemoveCvRef" ||
           name == "Conditional" || name == "EnableIf" || name == "VariantAlternative";
  }

  // RemoveConst<T> and the other type-valued traits; null when `part` is not one.
  const Ty* typeTrait(const NamePart& part, const TypeContext& ctx, int depth)
  {
    std::string_view name = symbols.name(part.symbol);
    if (!isTypeTraitName(part.symbol))
      return nullptr;
    if (name == "Conditional" || name == "EnableIf")
    {
      // Conditional<c, A, B> is A or B; EnableIf<c, T = void> is T when c holds (and no type at all
      // otherwise, which removes an overload the way std::enable_if does).
      if (part.templateArgs.empty())
        return types.unknown();
      const Ty* c = templateArgument(part.templateArgs[0], nullptr, ctx, depth);
      if (!c || c->kind != TyKind::Value)
        return types.unknown(); // dependent still: settled under the instantiation
      if (name == "Conditional")
      {
        if (part.templateArgs.size() != 3 || !isTypeNode(part.templateArgs[1]) || !isTypeNode(part.templateArgs[2]))
          return types.unknown();
        return convertType(static_cast<Type*>(part.templateArgs[c->value ? 1 : 2]), ctx, depth + 1);
      }
      if (!c->value)
        return types.unknown();
      if (part.templateArgs.size() == 1)
        return types.voidType();
      return isTypeNode(part.templateArgs[1]) ? convertType(static_cast<Type*>(part.templateArgs[1]), ctx, depth + 1) : types.unknown();
    }
    if (name == "VariantAlternative")
    {
      // VariantAlternative<I, U>: the type of the typed union U's I-th alternative (design.md 8.5),
      // as std::variant_alternative_t<I, V> is of a variant.
      if (part.templateArgs.size() != 2 || !isTypeNode(part.templateArgs[1]))
        return types.unknown();
      const Ty* index = templateArgument(part.templateArgs[0], nullptr, ctx, depth);
      const Ty* u = convertType(static_cast<Type*>(part.templateArgs[1]), ctx, depth + 1);
      if (!index || index->kind != TyKind::Value || !u || !u->isKnown() || types.isDependent(u))
        return types.unknown(); // dependent still: settled under the instantiation
      u = types.unqualified(types.stripRef(u));
      ClassDecl* def = u->kind == TyKind::Class ? classDefinition(u) : nullptr;
      if (!def || !def->isTypedUnion || index->value < 0 || uint64_t(index->value) >= def->alternatives.size())
        return types.unknown();
      Subst s = substFor(u);
      return declaredTypeOf(def->alternatives[size_t(index->value)], s.empty() ? nullptr : &s, depth + 1);
    }
    if (part.templateArgs.size() != 1 || !isTypeNode(part.templateArgs[0]))
      return types.unknown();
    const Ty* t = convertType(static_cast<Type*>(part.templateArgs[0]), ctx, depth + 1);
    if (!t || !t->isKnown() || types.isDependent(t))
      return t; // settled when the instantiation substitutes the argument
    if (name == "RemoveConst" || name == "RemoveCv")
      return types.unqualified(t);
    if (name == "RemoveReference")
      return types.stripRef(t);
    if (name == "RemoveCvRef")
      return types.unqualified(types.stripRef(t));
    if (name == "RemoveExtent")
      return t->kind == TyKind::Array ? t->inner : t;
    if (name == "RemovePointer")
      return t->kind == TyKind::Pointer ? t->inner : t;
    if (name == "AddConst")
      return types.qualified(t, true, false);
    if (name == "AddPointer")
      return types.pointer(types.stripRef(t));
    if (name == "Decay")
      return types.decay(t);
    if (name == "UnderlyingType")
      return t->kind == TyKind::Enum ? enumUnderlyingType(t, ctx, depth) : t;
    return nullptr;
  }

  static bool isArithmeticKind(const Ty* t)
  {
    return t->kind == TyKind::Int || t->kind == TyKind::Char || t->kind == TyKind::Bool || t->kind == TyKind::Float || t->kind == TyKind::Enum;
  }

  // C++'s composite pointer type of two pointers: to their pointee when that is one type up to
  // qualifiers, to void when one is void, to the base when one points to a class derived from the
  // other's; qualified as either pointee is. Null when the pointers have none.
  const Ty* compositePointer(const Ty* a, const Ty* b)
  {
    if (a->kind != TyKind::Pointer || b->kind != TyKind::Pointer || !a->inner || !b->inner)
      return nullptr;
    const Ty* pa = types.unqualified(a->inner);
    const Ty* pb = types.unqualified(b->inner);
    const Ty* pointee = nullptr;
    if (pa == pb || pb->kind == TyKind::Void)
      pointee = pb;
    else if (pa->kind == TyKind::Void)
      pointee = pa;
    else if (pa->kind == TyKind::Class && pb->kind == TyKind::Class && pa->decl != pb->decl)
      pointee = derivesFrom(pa, pb) ? pb : derivesFrom(pb, pa) ? pa : nullptr;
    if (!pointee)
      return nullptr;
    return types.pointer(types.qualified(pointee, a->inner->isConst || b->inner->isConst, a->inner->isVolatile || b->inner->isVolatile));
  }

  // Whether `derived` is `base` or has it among its bases (classes only).
  bool derivesFrom(const Ty* derived, const Ty* base, int depth = 0)
  {
    if (!derived || !base || depth > 16)
      return false;
    if (derived->kind != TyKind::Class || base->kind != TyKind::Class)
      return false;
    // The class itself: one instance of a template, not another (Flag<A> is no base of Flag<B>);
    // either's arguments unknown or dependent, the template.
    if (derived->decl == base->decl)
      return derived->args.empty() || base->args.empty() || types.isDependent(derived) || types.isDependent(base) ||
             types.unqualified(derived) == types.unqualified(base);
    Decl* def = derived->decl;
    if (def && def->kind == NodeKind::ClassDecl && !def->ownScope)
    {
      Entry e{def, nullptr};
      Scope* s = scopeOfEntry(e, 0);
      def = s ? s->owner : def;
    }
    if (!def || def->kind != NodeKind::ClassDecl)
      return false;
    DeclContext dc;
    declContextFor(def, dc);
    // The bases as the instantiation's arguments make them (`Const<Color> : Conditional<..., Value<T>, T>`
    // derives from Color, which the class template alone cannot tell).
    Subst own = derived->args.empty() ? Subst{} : substFor(derived);
    for (const ClassDecl::Base& b : static_cast<ClassDecl*>(def)->bases)
    {
      const Ty* bt = convertType(b.type, TypeContext{dc.scope, &dc.params, own.pairs.empty() ? nullptr : &own, def->file ? def->file->tokens : nullptr,
                                                     def->file ? def->file->text : std::string_view()}, 0);
      if (bt && derivesFrom(bt, base, depth + 1))
        return true;
    }
    return false;
  }

  // Whether a constructor or conversion operator is explicit here: `explicit`, or `explicit(cond)`
  // with cond true under the substitution (FixedPointNumberTemplate's `explicit(Explicit)`).
  bool isExplicit(FunctionDecl* f, const Subst* subst)
  {
    if (!(f->flags & DF_Explicit))
      return false;
    if (!f->explicitCondition)
      return true;
    DeclContext dc;
    declContextFor(f, dc);
    ParsedFile* file = f->file;
    TypeContext ctx{dc.scope, &dc.params, subst, file ? file->tokens : nullptr, file ? file->text : std::string_view()};
    std::optional<int64_t> v = constEval(f->explicitCondition, ctx, 0);
    return !v || *v != 0; // unknown: explicit, the safe side
  }

  // Whether a function is noexcept here: `noexcept`, or `noexcept(cond)` with cond true under the
  // substitution (`noexcept(false)` is not; FlatMap's `noexcept(isNothrowMoveConstructible<T>)` as T
  // is). nullopt: the condition is no constant here.
  std::optional<bool> isNoexcept(FunctionDecl* f, const Subst* subst)
  {
    if (!(f->flags & DF_Noexcept))
      return false;
    if (!f->noexceptCondition)
      return true;
    DeclContext dc;
    declContextFor(f, dc);
    ParsedFile* file = f->file;
    TypeContext ctx{dc.scope, &dc.params, subst, file ? file->tokens : nullptr, file ? file->text : std::string_view()};
    std::optional<int64_t> v = constEval(f->noexceptCondition, ctx, 0);
    if (!v)
      return std::nullopt;
    return *v != 0;
  }

  // Trait answers by (name, argument types): a trait of known types is a fact about them, asked
  // again and again (isTriviallyCopyable of a class walks its special members and bases each time,
  // about a fifth of the front end before this cache). Only definite answers are kept.
  struct TraitShard
  {
    std::shared_mutex mutex;
    std::unordered_map<std::string, bool, PointerKeyHash> answers;
    // What computing an answer read (a recording build's, when it read anything): replayed to each
    // asker it answers.
    std::unordered_map<std::string, std::vector<CollectedUse>, PointerKeyHash> uses;
  };
  TraitShard traitShards[64];

  // isArray<T>, isSame<A, B>, ... ; nullopt when `name` is not a trait.
  std::optional<bool> traitValue(std::string_view name, const std::vector<const Ty*>& a)
  {
    std::string key(name);
    key.push_back('\0');
    for (const Ty* t : a)
      key.append(reinterpret_cast<const char*>(&t), sizeof t);
    TraitShard& shard = traitShards[(PointerKeyHash()(key) >> 58) % std::size(traitShards)];
    {
      std::shared_lock lock(shard.mutex);
      if (auto it = shard.answers.find(key); it != shard.answers.end())
      {
        if (recordingDependencies)
          if (auto u = shard.uses.find(key); u != shard.uses.end())
            replayUses(u->second);
        return it->second;
      }
    }
    CollectUses collected;
    std::optional<bool> answer = traitValueUncached(name, a);
    if (answer)
    {
      std::unique_lock lock(shard.mutex);
      if (!collected.uses.empty())
        shard.uses.emplace(key, collected.uses);
      shard.answers.emplace(std::move(key), *answer);
    }
    return answer;
  }

  std::optional<bool> traitValueUncached(std::string_view name, const std::vector<const Ty*>& a)
  {
    auto one = [&]() { return a.size() == 1 ? a[0] : nullptr; };
    if (name == "isArray") return one() && one()->kind == TyKind::Array;
    if (name == "isPointer") return one() && one()->kind == TyKind::Pointer;
    if (name == "isEnum") return one() && one()->kind == TyKind::Enum;
    if (name == "isClass") return one() && one()->kind == TyKind::Class;
    if (name == "isTypedUnion") // a `union enum` (design.md 8.5)
    {
      const Ty* t = one() ? types.unqualified(one()) : nullptr;
      return t && t->kind == TyKind::Class && t->decl && t->decl->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(t->decl)->isTypedUnion;
    }
    if (name == "isVoid") return one() && one()->kind == TyKind::Void;
    if (name == "isReference") return one() && one()->isReference();
    if (name == "isConst") return one() && one()->isConst;
    if (name == "isNullPointer") return one() && one() == types.nullptrType();
    if (name == "isIntegral") return one() && (one()->kind == TyKind::Int || one()->kind == TyKind::Char || one()->kind == TyKind::Bool);
    if (name == "isFloatingPoint") return one() && one()->kind == TyKind::Float;
    if (name == "isArithmetic") return one() && isArithmeticKind(one()) && one()->kind != TyKind::Enum;
    if (name == "isSigned")
      return one() && (one()->kind == TyKind::Float || ((one()->kind == TyKind::Int || one()->kind == TyKind::Char) && !one()->isUnsigned));
    if (name == "isUnsigned")
      return one() && ((one()->kind == TyKind::Int || one()->kind == TyKind::Char || one()->kind == TyKind::Bool) && (one()->isUnsigned || one()->kind == TyKind::Bool));
    if (name == "isFunction") return one() && one()->kind == TyKind::Function;
    if (name == "isTriviallyCopyable") return one() ? triviallyCopyable(one()) : std::nullopt;
    if (name == "isConstructible")
    {
      // isConstructible<T, Args...>: T has a constructor taking Args (a class without constructors
      // takes none or a copy; a scalar takes one convertible value).
      if (a.empty())
        return false;
      if (a.size() == 1)
        return defaultConstructible(a[0]); // (`int&` is not: a reference binds something)
      if (a[0]->isReference())
      {
        // `T& r(declval<Arg>())`: whether the reference binds the argument, an lvalue for a
        // reference Arg and an xvalue otherwise (`isConstructible<char (&)[4], char (&)[4]>`, a
        // Tuple element forwardAsTuple gives; `int&` binds no int&&).
        if (a.size() != 2 || !a[1])
          return false;
        return conversionRank(a[1]->isReference() ? a[1] : types.rref(a[1]), a[0]) < NotViable;
      }
      const Ty* t = types.unqualified(types.stripRef(a[0]));
      std::vector<const Ty*> args(a.begin() + 1, a.end());
      bool copy = args.size() == 1 && types.unqualified(types.stripRef(args[0])) == t;
      if (t->kind == TyKind::Class)
      {
        Decl* def = t->decl && t->decl->ownScope ? t->decl : (classScopeOf(t) ? classScopeOf(t)->owner : nullptr);
        if (!def || def->kind != NodeKind::ClassDecl)
          return false;
        std::vector<FunctionDecl*> ctors;
        Subst s = substFor(t);
        for (Decl* m : membersRead(static_cast<ClassDecl*>(def)))
        {
          if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Constructor) && !(m->flags & DF_Delete))
            ctors.push_back(static_cast<FunctionDecl*>(m));
          if (m->kind != NodeKind::UsingNameDecl)
            continue;
          // `using Vector2::Vector2;`: the base's constructors, but its copy and move ones, are
          // the class's too, under the base's arguments.
          const QualifiedName& q = static_cast<UsingNameDecl*>(m)->name;
          if (q.parts.size() < 2 || q.parts[q.parts.size() - 1].symbol != q.parts[q.parts.size() - 2].symbol)
            continue;
          QualifiedName baseName = q;
          baseName.parts.count -= 1;
          Lookup bl = lookupQualified(baseName, def->ownScope, nullptr, 0);
          if (!bl.scope || bl.scope == def->ownScope || !bl.scope->owner || bl.scope->owner->kind != NodeKind::ClassDecl)
            continue;
          auto* base = static_cast<ClassDecl*>(bl.scope->owner);
          for (const Subst::Pair& p : memberSubst(t, bl.scope, 0).pairs)
            if (!s.find(p.param))
              s.pairs.push_back(p);
          for (Decl* bm : membersRead(base))
            if (bm->kind == NodeKind::FunctionDecl && (bm->flags & DF_Constructor) && !(bm->flags & DF_Delete))
              if (auto* f = static_cast<FunctionDecl*>(bm); f->params.size() != 1 || selfParameter(f->params[0], base) == 0)
                ctors.push_back(f);
        }
        if (copy || ctors.empty())
          return copy;
        bool ambiguous = false;
        return chooseAmong(ctors, args, s.pairs.empty() ? nullptr : &s, nullptr, ambiguous) != nullptr;
      }
      if (args.empty())
        return t->kind != TyKind::Void && t->kind != TyKind::Function;
      if (args.size() != 1)
        return false;
      if (traitValue("isConvertible", {args[0], t}).value_or(false))
        return true;
      // Direct-initialization takes a class's explicit conversion operators too: ParsedNoiseExpression's
      // `explicit operator const NoiseExpression*()` makes a const NoiseExpression* constructible
      // from it (Pair's converting constructor requires that of its members).
      const Ty* from = types.unqualified(types.stripRef(args[0]));
      return from->kind == TyKind::Class && !types.isDependent(from) && convertsByOperator(from, t, 0, 0, true);
    }
    if (name == "isFundamental")
      return one() && (isArithmeticKind(one()) || one()->kind == TyKind::Void || one() == types.nullptrType());
    if (name == "isTriviallyDestructible") return one() ? triviallyDestructible(one()) : std::nullopt;
    if (name == "isTriviallyCopyConstructible") return one() ? constructibleFromSelf(one(), false, TraitForm::Trivially) : std::nullopt;
    if (name == "isTriviallyCopyAssignable") return one() ? assignableFromSelf(one(), false, TraitForm::Trivially) : std::nullopt;
    if (name == "isTriviallyDefaultConstructible") return one() ? triviallyDefaultConstructible(one()) : std::nullopt;
    if (name == "isSame") return a.size() == 2 && a[0] == a[1];
    if (name == "isBaseOf") return a.size() == 2 && derivesFrom(a[1], a[0]);
    if (name == "isConvertible")
    {
      // isConvertible<From, To>: an rvalue of From (an lvalue for a reference From, as declval
      // gives) converts to To implicitly, by the conversions overload resolution knows: standard
      // ones, derived to base, converting constructors and conversion operators.
      if (a.size() != 2)
        return false;
      const Ty* from = a[0];
      const Ty* to = a[1];
      if (!from->isKnown() || !to->isKnown() || types.isDependent(from) || types.isDependent(to))
        return std::nullopt;
      if (types.unqualified(types.stripRef(to))->kind == TyKind::Void)
        return types.unqualified(types.stripRef(from))->kind == TyKind::Void;
      if (types.unqualified(types.stripRef(from))->kind == TyKind::Void)
        return false;
      if (to->kind == TyKind::LRef && !to->inner->isConst && from->kind != TyKind::LRef)
        return false; // an rvalue binds no non-const lvalue reference
      return conversionRank(from, to) < NotViable;
    }
    // The traits that need a class's members, answered as C++'s intrinsics answer them; nullopt
    // where that cannot be decided here (unknown or dependent types, a class without definition).
    if (name == "isPolymorphic") return one() ? polymorphicClass(one()) : std::nullopt;
    if (name == "isAbstract") return one() ? abstractClass(one(), {}) : std::nullopt;
    if (name == "isFinal")
    {
      if (!one() || !one()->isKnown() || types.isDependent(one()))
        return std::nullopt;
      if (one()->kind != TyKind::Class)
        return false;
      ClassDecl* def = classDefinition(one());
      return def ? std::optional<bool>(def->isFinal) : std::nullopt;
    }
    if (name == "isMoveConstructible") return one() ? constructibleFromSelf(one(), true, TraitForm::Plain) : std::nullopt;
    if (name == "isCopyConstructible") return one() ? constructibleFromSelf(one(), false, TraitForm::Plain) : std::nullopt;
    if (name == "isNothrowMoveConstructible") return one() ? constructibleFromSelf(one(), true, TraitForm::Nothrow) : std::nullopt;
    if (name == "isTriviallyMoveConstructible") return one() ? constructibleFromSelf(one(), true, TraitForm::Trivially) : std::nullopt;
    if (name == "isMoveAssignable") return one() ? assignableFromSelf(one(), true, TraitForm::Plain) : std::nullopt;
    if (name == "isCopyAssignable") return one() ? assignableFromSelf(one(), false, TraitForm::Plain) : std::nullopt;
    if (name == "isNothrowMoveAssignable") return one() ? assignableFromSelf(one(), true, TraitForm::Nothrow) : std::nullopt;
    if (name == "isTriviallyMoveAssignable") return one() ? assignableFromSelf(one(), true, TraitForm::Trivially) : std::nullopt;
    if (name == "isMemberPointer" || name == "isMemberFunctionPointer" || name == "isMemberObjectPointer")
    {
      if (!one() || !one()->isKnown() || types.isDependent(one()))
        return std::nullopt;
      const Ty* t = types.unqualified(one());
      if (t->kind != TyKind::MemberPointer)
        return false;
      bool function = t->inner && t->inner->kind == TyKind::Function;
      return name == "isMemberPointer" || function == (name == "isMemberFunctionPointer");
    }
    if (name == "isInvocable")
    {
      if (a.empty())
        return std::nullopt;
      std::vector<const Ty*> args(a.begin() + 1, a.end());
      return invocable(a[0], args, nullptr);
    }
    if (name == "isInvocableR")
    {
      // isInvocableR<R, F, Args...>: callable, and the result converts to R (anything to void).
      if (a.size() < 2)
        return std::nullopt;
      std::vector<const Ty*> args(a.begin() + 2, a.end());
      const Ty* result = nullptr;
      std::optional<bool> callable = invocable(a[1], args, &result);
      if (!callable || !*callable)
        return callable;
      if (types.unqualified(a[0])->kind == TyKind::Void)
        return true;
      if (!result || !result->isKnown() || types.isDependent(result))
        return std::nullopt;
      return conversionRank(result, a[0]) < NotViable;
    }
    return std::nullopt;
  }

  bool isBoolTraitName(uint32_t symbol)
  {
    std::string_view n = symbols.name(symbol);
    return n == "isArray" || n == "isPointer" || n == "isEnum" || n == "isClass" || n == "isTypedUnion" || n == "isVoid" || n == "isReference" || n == "isConst" ||
           n == "isNullPointer" || n == "isIntegral" || n == "isFloatingPoint" || n == "isArithmetic" || n == "isSigned" ||
           n == "isUnsigned" || n == "isFunction" || n == "isTriviallyCopyable" || n == "isSame" || n == "isBaseOf" || n == "isConvertible" ||
           n == "isConstructible" || n == "isFundamental" || n == "isTriviallyDestructible" || n == "isTriviallyCopyAssignable" ||
           n == "isTriviallyCopyConstructible" || n == "isTriviallyDefaultConstructible" || n == "isPolymorphic" || n == "isAbstract" ||
           n == "isFinal" || n == "isMoveConstructible" || n == "isCopyConstructible" || n == "isNothrowMoveConstructible" ||
           n == "isTriviallyMoveConstructible" || n == "isMoveAssignable" || n == "isCopyAssignable" || n == "isNothrowMoveAssignable" ||
           n == "isTriviallyMoveAssignable" || n == "isMemberPointer" || n == "isMemberFunctionPointer" || n == "isMemberObjectPointer" ||
           n == "isInvocable" || n == "isInvocableR";
  }

  // --- what the class traits read: definitions, bases, members, special members ---

  // The definition of a class type (the declaration it names may be a forward one), or null.
  ClassDecl* classDefinition(const Ty* t)
  {
    if (!t || t->kind != TyKind::Class || !t->decl)
      return nullptr;
    Decl* def = t->decl;
    if (def->kind == NodeKind::ClassDecl && !def->ownScope)
    {
      Entry e{def, nullptr};
      Scope* s = scopeOfEntry(e, 0);
      def = s ? s->owner : def;
    }
    if (!def || def->kind != NodeKind::ClassDecl || !static_cast<ClassDecl*>(def)->isDefinition)
      return nullptr;
    return static_cast<ClassDecl*>(def);
  }

  // The direct bases of a class type, with its template arguments substituted; false when one
  // is not known.
  bool directBases(const Ty* t, ClassDecl* def, std::vector<const Ty*>& out)
  {
    DeclContext dc;
    declContextFor(def, dc);
    Subst s = substFor(t);
    TypeContext ctx{dc.scope, &dc.params, s.pairs.empty() ? nullptr : &s, def->file ? def->file->tokens : nullptr,
                    def->file ? def->file->text : std::string_view()};
    for (const ClassDecl::Base& b : def->bases)
    {
      const Ty* bt = b.isPack ? nullptr : convertType(b.type, ctx, 0);
      if (!bt || !bt->isKnown() || types.isDependent(bt))
        return false;
      out.push_back(bt);
    }
    return true;
  }

  // The types of the non-static data members, with the class's template arguments substituted;
  // false when one is not known.
  bool fieldTypes(const Ty* t, ClassDecl* def, std::vector<const Ty*>& out)
  {
    Subst s = substFor(t);
    for (Decl* m : membersRead(def))
    {
      if (m->kind != NodeKind::VariableDecl || (m->flags & (DF_Static | DF_Typedef)))
        continue;
      const Ty* ft = declaredTypeOf(m, s.pairs.empty() ? nullptr : &s, 0);
      if (!ft || !ft->isKnown() || types.isDependent(ft))
        return false;
      out.push_back(ft);
    }
    // A typed union's alternatives live in an anonymous union, not among the members: they count
    // (an alternative holding a class with a destructor makes the union's copy and destruction
    // non-trivial, as std::variant's are).
    if (def->isTypedUnion)
      for (Decl* a : def->alternatives)
      {
        const Ty* at = declaredTypeOf(a, s.pairs.empty() ? nullptr : &s, 0);
        if (!at || !at->isKnown() || types.isDependent(at))
          return false;
        out.push_back(at);
      }
    return true;
  }

  // What an override has to match: the name, the parameter types under the class's template
  // arguments, const. Every destructor overrides every other.
  std::string overrideKey(FunctionDecl* f, const Ty* cls)
  {
    if (f->flags & DF_Destructor)
      return "~";
    const NamePart& n = f->name.last();
    std::string k = n.special == 2 ? "operator#" + std::to_string(n.opToken) : n.special == 3 ? "conversion" : debugName(n.symbol);
    Subst s = substFor(cls);
    const Ty* ft = declaredTypeOf(f, s.pairs.empty() ? nullptr : &s, 0);
    k += "(";
    for (size_t i = 0; ft && ft->kind == TyKind::Function && i < ft->args.size(); ++i)
      k += (i ? ", " : "") + types.toString(ft->args[i], &symbols);
    k += ")";
    if (n.special == 3 && ft && ft->kind == TyKind::Function && ft->inner)
      k += types.toString(ft->inner, &symbols);
    return (f->flags & DF_Const) ? k + " const" : k;
  }

  // isPolymorphic: the class or a base declares a virtual function.
  std::optional<bool> polymorphicClass(const Ty* t, int depth = 0)
  {
    if (!t->isKnown() || types.isDependent(t) || depth > 16)
      return std::nullopt;
    if (t->kind != TyKind::Class)
      return false;
    ClassDecl* def = classDefinition(t);
    if (!def)
      return std::nullopt;
    for (Decl* m : membersRead(def))
      if (m->kind == NodeKind::FunctionDecl && (m->flags & (DF_Virtual | DF_Override | DF_Pure)))
        return true;
    std::vector<const Ty*> bases;
    if (!directBases(t, def, bases))
      return std::nullopt;
    for (const Ty* b : bases)
    {
      std::optional<bool> p = polymorphicClass(b, depth + 1);
      if (!p || *p)
        return p;
    }
    return false;
  }

  // isAbstract: a pure virtual function of the class or a base that no class from the base down
  // to t overrides. `overridden` holds the keys the classes below (more derived) declare.
  std::optional<bool> abstractClass(const Ty* t, std::unordered_set<std::string> overridden, int depth = 0)
  {
    if (!t->isKnown() || types.isDependent(t) || depth > 16)
      return std::nullopt;
    if (t->kind != TyKind::Class)
      return false;
    ClassDecl* def = classDefinition(t);
    if (!def)
      return std::nullopt;
    std::vector<FunctionDecl*> pure;
    for (Decl* m : membersRead(def))
      if (m->kind == NodeKind::FunctionDecl && !(m->flags & (DF_Static | DF_Constructor)) && m->templateParams.empty())
      {
        auto* f = static_cast<FunctionDecl*>(m);
        if (f->flags & DF_Pure)
          pure.push_back(f);
        else
          overridden.insert(overrideKey(f, t));
      }
    for (FunctionDecl* f : pure)
      if (!overridden.count(overrideKey(f, t)))
        return true;
    overridden.insert("~"); // the class's destructor, declared or implicit, overrides a pure one of a base
    std::vector<const Ty*> bases;
    if (!directBases(t, def, bases))
      return std::nullopt;
    for (const Ty* b : bases)
    {
      std::optional<bool> a = abstractClass(b, overridden, depth + 1);
      if (!a || *a)
        return a;
    }
    return false;
  }

  // The copy and move constructors and assignment operators a class declares (none of them is a
  // template), and whether it declares a destructor: what decides its implicit ones.
  struct SpecialMembers
  {
    FunctionDecl* copyCtor = nullptr;         // C(const C&)
    FunctionDecl* copyCtorNonConst = nullptr; // C(C&)
    FunctionDecl* moveCtor = nullptr;         // C(C&&)
    FunctionDecl* copyAssign = nullptr;       // operator=(const C&) or operator=(C)
    FunctionDecl* copyAssignNonConst = nullptr;
    FunctionDecl* moveAssign = nullptr;       // operator=(C&&)
    bool destructor = false;
  };
  enum class TraitForm { Plain, Trivially, Nothrow };

  // Whether a parameter's type is the class itself: 1 `const C&`, 2 `C&`, 3 `C&&`, 4 `C`; 0 not.
  int selfParameter(Decl* p, ClassDecl* def)
  {
    Type* t = static_cast<ParamDecl*>(p)->type;
    int kind = 4;
    if (t && t->kind == NodeKind::LRefType)
    {
      t = static_cast<LRefType*>(t)->referenced;
      kind = 2;
    }
    else if (t && t->kind == NodeKind::RRefType)
    {
      t = static_cast<RRefType*>(t)->referenced;
      kind = 3;
    }
    bool isConst = t && t->kind == NodeKind::ConstType;
    if (isConst)
      t = static_cast<ConstType*>(t)->inner;
    if (!t || t->kind != NodeKind::NamedType || static_cast<NamedType*>(t)->name.empty() || def->name.empty() ||
        static_cast<NamedType*>(t)->name.last().symbol != def->name.last().symbol)
      return 0;
    if (const QualifiedName& n = static_cast<NamedType*>(t)->name; (n.parts.size() > 1 || n.global) && def->ownScope)
    {
      // `Color(const agui::Color&)`: a class of the same name elsewhere, a converting constructor.
      Lookup l = lookupQualified(n, def->ownScope, nullptr, 0);
      if (l.scope != def->ownScope)
        return 0;
    }
    if (isConst)
      return kind == 2 ? 1 : kind == 4 ? 4 : 0;
    return kind;
  }

  SpecialMembers specialMembersOf(ClassDecl* def)
  {
    SpecialMembers s;
    for (Decl* m : membersRead(def))
    {
      if (m->kind != NodeKind::FunctionDecl || !m->templateParams.empty())
        continue;
      auto* f = static_cast<FunctionDecl*>(m);
      if (f->flags & DF_Destructor)
      {
        s.destructor = true;
        continue;
      }
      size_t required = 0;
      for (Decl* p : f->params)
        required += static_cast<ParamDecl*>(p)->defaultValue ? 0 : 1;
      if (f->params.empty() || required > 1)
        continue;
      int k = selfParameter(f->params[0], def);
      if (f->flags & DF_Constructor)
      {
        if (k == 1)
          s.copyCtor = f;
        else if (k == 2)
          s.copyCtorNonConst = f;
        else if (k == 3)
          s.moveCtor = f;
      }
      else if (!f->name.empty() && f->name.last().special == 2 && f->name.last().opToken == uint32_t(P::Assign))
      {
        if (k == 1 || k == 4)
          s.copyAssign = f;
        else if (k == 2)
          s.copyAssignNonConst = f;
        else if (k == 3)
          s.moveAssign = f;
      }
    }
    return s;
  }

  // A user-declared special member chosen for the trait: deleted is false; a user-provided one
  // is not trivial, and nothrow as declared; a defaulted one is as the implicit one (nullopt:
  // decide by the bases and members).
  std::optional<bool> declaredSpecial(FunctionDecl* f, TraitForm form, const Ty* of)
  {
    if (f->flags & DF_Delete)
      return false;
    if (f->flags & DF_Default)
      return std::nullopt;
    if (form == TraitForm::Trivially)
      return false;
    if (form == TraitForm::Nothrow)
    {
      // (as its noexcept condition says under the class's arguments: `noexcept(false)` is not)
      Subst s = of && !of->args.empty() ? substFor(of) : Subst{};
      return isNoexcept(f, s.pairs.empty() ? nullptr : &s);
    }
    return true;
  }

  // isMoveConstructible (move) and isCopyConstructible, in their plain, trivial and nothrow
  // forms: whether T is built from an rvalue (from a const lvalue) of itself by the constructor
  // C++ chooses, the implicit or defaulted ones by the class's bases and members.
  std::optional<bool> constructibleFromSelf(const Ty* t, bool move, TraitForm form, int depth = 0)
  {
    if (!t || !t->isKnown() || types.isDependent(t) || depth > 16)
      return std::nullopt;
    if (t->isReference())
      return true;
    if (t->isConst)
      move = false; // `const T&&` binds to `const T&`: the copy constructor
    const Ty* u = types.unqualified(t);
    switch (u->kind)
    {
      case TyKind::Void: case TyKind::Function: case TyKind::Array: return false;
      case TyKind::Class: break;
      case TyKind::Lambda:
      {
        LambdaExpr* l = lambdaOf(u);
        if (!l)
          return std::nullopt;
        if (form == TraitForm::Trivially && !l->captures.empty())
          return std::nullopt;
        // A closure copies (moves) its captures by value: an init-capture of a type that cannot be
        // copied (`[buffer = move(buffer)]` of a UniquePtr) makes it move-only, as C++'s deleted
        // implicit copy constructor does, and the library's Function then never copies it. (A capture
        // by copy of a named variable, `[x]`, copies it already where the lambda is written.)
        for (const LambdaCapture& cap : l->captures)
        {
          if (!cap.init || !cap.decl || cap.byRef)
            continue;
          // `[properties { move(properties) }]`: the variable has its one element's type (the
          // brace list itself is typed as a list), as `[properties = move(properties)]`.
          const Expr* init = cap.init;
          if (init->kind == NodeKind::BraceInitExpr && static_cast<const BraceInitExpr*>(init)->args.size() == 1)
            init = static_cast<const BraceInitExpr*>(init)->args[0];
          const Ty* ct = init->ty && init->ty->isKnown() && !types.isDependent(init->ty)
                           ? types.decay(types.unqualified(types.stripRef(init->ty))) : nullptr;
          if (!ct)
            continue; // (typed where the lambda's instance is: as before, copyable)
          if (std::optional<bool> ok = constructibleFromSelf(ct, move, form, depth + 1); ok && !*ok)
            return false;
        }
        return true;
      }
      case TyKind::Unknown: case TyKind::Param: case TyKind::DependentMember: case TyKind::Auto: case TyKind::Pack: return std::nullopt;
      default: return true; // scalars, pointers, enums
    }
    ClassDecl* def = classDefinition(u);
    if (!def)
      return std::nullopt;
    if (depth == 0)
    {
      // An abstract class makes no object of its own; as a base (deeper) its constructors serve
      // its derived classes' all the same.
      std::optional<bool> isAbstract = abstractClass(u, {});
      if (!isAbstract || *isAbstract)
        return isAbstract ? std::optional<bool>(false) : std::nullopt;
    }
    SpecialMembers s = specialMembersOf(def);
    FunctionDecl* chosen = move && s.moveCtor ? s.moveCtor : s.copyCtor;
    if (chosen)
    {
      if (std::optional<bool> d = declaredSpecial(chosen, form, u))
        return d;
    }
    else
    {
      // None declared fits: a declared one that does not (C(C&) given an rvalue, only a move
      // constructor given a const lvalue) means there is no implicit one either.
      if (s.copyCtorNonConst || s.moveCtor || s.moveAssign)
        return false; // a declared move operation deletes the implicit copy constructor
      move = move && !s.copyAssign && !s.copyAssignNonConst && !s.destructor; // else the implicit copy constructor
    }
    return membersAllow(u, def, form, [&](const Ty* m, int d) { return constructibleFromSelf(m, move, form, d); }, false, depth);
  }

  // isMoveAssignable (move) and isCopyAssignable, likewise.
  std::optional<bool> assignableFromSelf(const Ty* t, bool move, TraitForm form, int depth = 0)
  {
    if (!t || !t->isKnown() || types.isDependent(t) || depth > 16)
      return std::nullopt;
    if (t->isReference())
    {
      t = types.stripRef(t); // `T& = T&`: a copy assignment of T
      move = false;
    }
    if (t->isConst)
      return false;
    const Ty* u = types.unqualified(t);
    switch (u->kind)
    {
      case TyKind::Void: case TyKind::Function: case TyKind::Array: return false;
      case TyKind::Class: break;
      case TyKind::Lambda:
      {
        LambdaExpr* l = lambdaOf(u);
        if (!l)
          return std::nullopt;
        return l->captures.empty() && !l->defaultCapture; // a closure with captures has a deleted assignment
      }
      case TyKind::Unknown: case TyKind::Param: case TyKind::DependentMember: case TyKind::Auto: case TyKind::Pack: return std::nullopt;
      default: return true;
    }
    ClassDecl* def = classDefinition(u);
    if (!def)
      return std::nullopt;
    SpecialMembers s = specialMembersOf(def);
    FunctionDecl* chosen = move && s.moveAssign ? s.moveAssign : s.copyAssign;
    if (chosen)
    {
      if (std::optional<bool> d = declaredSpecial(chosen, form, u))
        return d;
    }
    else
    {
      if (s.copyAssignNonConst || s.moveAssign || s.moveCtor)
        return false; // a declared move operation deletes the implicit copy assignment
      move = move && !s.copyCtor && !s.copyCtorNonConst && !s.destructor;
    }
    return membersAllow(u, def, form, [&](const Ty* m, int d) { return assignableFromSelf(m, move, form, d); }, true, depth);
  }

  // The implicit (or defaulted) special member of a class exists, is trivial or nothrow, when
  // every base and member's does; an assignment also needs no reference or const member, and a
  // trivial one no virtual function or base.
  std::optional<bool> membersAllow(const Ty* t, ClassDecl* def, TraitForm form, const std::function<std::optional<bool>(const Ty*, int)>& each,
                                   bool assignment, int depth)
  {
    if (form == TraitForm::Trivially)
    {
      for (const ClassDecl::Base& b : def->bases)
        if (b.isVirtual)
          return false;
      std::optional<bool> p = polymorphicClass(t);
      if (!p || *p)
        return p ? std::optional<bool>(false) : std::nullopt;
    }
    std::vector<const Ty*> parts;
    if (!directBases(t, def, parts) || !fieldTypes(t, def, parts))
      return std::nullopt;
    size_t baseCount = def->bases.size();
    for (size_t i = 0; i < parts.size(); ++i)
    {
      const Ty* m = parts[i];
      bool field = i >= baseCount;
      if (field && assignment && (m->isReference() || types.stripRef(m)->isConst))
        return false;
      if (field && m->isReference())
        continue; // constructing binds it
      while (m->kind == TyKind::Array)
        m = m->inner; // arrays as members are copied element by element
      std::optional<bool> r = each(m, depth + 1);
      if (!r || !*r)
        return r;
    }
    return true;
  }

  // The class an object of type t is made of for the triviality traits (arrays by element), or
  // the answer itself for a type that is no class: nullopt result means "not decided here".
  struct TrivialSubject
  {
    const Ty* cls = nullptr;
    std::optional<bool> answer;
  };
  TrivialSubject trivialSubject(const Ty* t)
  {
    const Ty* u = types.unqualified(t);
    while (u->kind == TyKind::Array)
      u = types.unqualified(u->inner);
    switch (u->kind)
    {
      case TyKind::Class: return {u, std::nullopt};
      case TyKind::Void: case TyKind::Function: return {nullptr, false};
      case TyKind::Lambda:
      {
        LambdaExpr* l = lambdaOf(u);
        return {nullptr, l && l->captures.empty() && !l->defaultCapture ? std::optional<bool>(true) : std::nullopt};
      }
      case TyKind::Unknown: case TyKind::Param: case TyKind::DependentMember: case TyKind::Auto: case TyKind::Pack:
        return {nullptr, std::nullopt};
      default: return {nullptr, true}; // scalars, pointers, enums, member pointers
    }
  }

  // isTriviallyDestructible: no user-provided, deleted or virtual destructor, and every base and
  // member trivially destructible.
  std::optional<bool> triviallyDestructible(const Ty* t, int depth = 0)
  {
    if (!t || !t->isKnown() || types.isDependent(t) || depth > 16)
      return std::nullopt;
    if (t->isReference())
      return true;
    TrivialSubject s = trivialSubject(t);
    if (!s.cls)
      return s.answer;
    ClassDecl* def = classDefinition(s.cls);
    if (!def)
      return std::nullopt;
    for (Decl* m : membersRead(def))
      if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Destructor) &&
          ((m->flags & DF_Delete) || !(m->flags & DF_Default) || (m->flags & (DF_Virtual | DF_Override))))
        return false;
    std::vector<const Ty*> parts;
    if (!directBases(s.cls, def, parts) || !fieldTypes(s.cls, def, parts))
      return std::nullopt;
    for (const Ty* part : parts)
    {
      std::optional<bool> r = triviallyDestructible(part, depth + 1);
      if (!r || !*r)
        return r;
    }
    return true;
  }

  // isTriviallyCopyable: trivially destructible, and every copy and move constructor and
  // assignment the class has is trivial, at least one of them existing.
  std::optional<bool> triviallyCopyable(const Ty* t)
  {
    if (!t || !t->isKnown() || types.isDependent(t))
      return std::nullopt;
    if (t->isReference())
      return false;
    TrivialSubject s = trivialSubject(t);
    if (!s.cls)
      return s.answer;
    std::optional<bool> destructible = triviallyDestructible(s.cls);
    if (!destructible || !*destructible)
      return destructible;
    bool any = false;
    for (int op = 0; op < 4; ++op)
    {
      bool move = op & 1, assign = op & 2;
      auto test = [&](TraitForm form)
      {
        return assign ? assignableFromSelf(s.cls, move, form) : constructibleFromSelf(s.cls, move, form);
      };
      std::optional<bool> exists = test(TraitForm::Plain);
      if (!exists)
        return std::nullopt;
      if (!*exists)
        continue;
      std::optional<bool> trivial = test(TraitForm::Trivially);
      if (!trivial || !*trivial)
        return trivial;
      any = true;
    }
    return any;
  }

  // isTriviallyDefaultConstructible: a default constructor that is neither user-provided nor
  // deleted, no virtual function or base, no default member initializer, and every base and member
  // trivially default constructible.
  // Whether T() builds a T (std::is_default_constructible): a scalar or a pointer does; a reference
  // does not; a class by its declared default constructor (not a deleted one), none if it declares
  // others, else by the implicit one, which needs every base and member default-constructible (or
  // initialised in the class), no reference or const scalar left uninitialised, and for a typed
  // union its first alternative (design.md 8.5: default construction builds that one).
  std::optional<bool> defaultConstructible(const Ty* t, int depth = 0)
  {
    if (!t || !t->isKnown() || types.isDependent(t) || depth > 16)
      return std::nullopt;
    if (t->isReference())
      return false;
    const Ty* u = types.unqualified(t);
    while (u->kind == TyKind::Array)
      u = types.unqualified(u->inner);
    if (u->kind == TyKind::Void || u->kind == TyKind::Function)
      return false;
    if (u->kind != TyKind::Class)
      return true;
    ClassDecl* def = classDefinition(u);
    if (!def)
      return std::nullopt;
    if (std::optional<bool> isAbstract = abstractClass(u, {}); !isAbstract || *isAbstract)
      return isAbstract ? std::optional<bool>(false) : std::nullopt;
    bool anyConstructor = false;
    for (Decl* m : membersRead(def))
    {
      if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Constructor))
        continue;
      anyConstructor = true;
      auto* f = static_cast<FunctionDecl*>(m);
      bool takesNone = true;
      for (Decl* p : f->params)
        takesNone = takesNone && (static_cast<ParamDecl*>(p)->defaultValue || static_cast<ParamDecl*>(p)->isPack);
      if (!takesNone)
        continue;
      if (f->flags & DF_Delete)
        return false;
      if (!(f->flags & DF_Default))
        return true; // a user-provided default constructor
      anyConstructor = false; // `= default`: as the implicit one, below
      break;
    }
    if (anyConstructor)
      return false; // constructors declared, none takes no argument: no implicit one
    Subst s = substFor(u);
    if (def->isTypedUnion)
    {
      if (def->alternatives.empty())
        return true;
      return defaultConstructible(declaredTypeOf(def->alternatives[0], s.pairs.empty() ? nullptr : &s, 0), depth + 1);
    }
    std::vector<const Ty*> bases;
    if (!directBases(u, def, bases))
      return std::nullopt;
    for (const Ty* b : bases)
      if (std::optional<bool> r = defaultConstructible(b, depth + 1); !r || !*r)
        return r;
    for (Decl* m : membersRead(def))
    {
      if (m->kind != NodeKind::VariableDecl || (m->flags & (DF_Static | DF_Typedef)))
        continue;
      auto* v = static_cast<VariableDecl*>(m);
      if (v->init)
        continue; // initialised in the class
      const Ty* mt = declaredTypeOf(m, s.pairs.empty() ? nullptr : &s, 0);
      if (!mt || !mt->isKnown() || types.isDependent(mt))
        return std::nullopt;
      if (mt->isReference())
        return false;
      const Ty* mu = mt;
      while (mu->kind == TyKind::Array)
        mu = mu->inner;
      if (mu->isConst && mu->kind != TyKind::Class)
        return false; // a const scalar left uninitialised
      if (std::optional<bool> r = defaultConstructible(mt, depth + 1); !r || !*r)
        return r;
    }
    return true;
  }

  std::optional<bool> triviallyDefaultConstructible(const Ty* t, int depth = 0)
  {
    if (!t || !t->isKnown() || types.isDependent(t) || depth > 16)
      return std::nullopt;
    if (t->isReference())
      return false;
    TrivialSubject s = trivialSubject(t);
    if (!s.cls)
      return s.answer;
    ClassDecl* def = classDefinition(s.cls);
    if (!def)
      return std::nullopt;
    bool anyConstructor = false, defaultConstructor = false;
    for (Decl* m : membersRead(def))
    {
      if (m->kind == NodeKind::VariableDecl && !(m->flags & (DF_Static | DF_Typedef)) && static_cast<VariableDecl*>(m)->init)
        return false; // `int count = 0;`: the implicit constructor runs it
      if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Constructor))
        continue;
      anyConstructor = true;
      auto* f = static_cast<FunctionDecl*>(m);
      bool takesNone = true;
      for (Decl* p : f->params)
        takesNone = takesNone && static_cast<ParamDecl*>(p)->defaultValue;
      if (!takesNone)
        continue;
      if ((f->flags & DF_Delete) || !(f->flags & DF_Default))
        return false;
      defaultConstructor = true;
    }
    if (anyConstructor && !defaultConstructor)
      return false; // a declared constructor and no default one: none is implicit
    for (const ClassDecl::Base& b : def->bases)
      if (b.isVirtual)
        return false;
    std::optional<bool> polymorphic = polymorphicClass(s.cls);
    if (!polymorphic || *polymorphic)
      return polymorphic ? std::optional<bool>(false) : std::nullopt;
    std::vector<const Ty*> parts;
    if (!directBases(s.cls, def, parts) || !fieldTypes(s.cls, def, parts))
      return std::nullopt;
    for (const Ty* part : parts)
    {
      if (part->isReference())
        return false; // a reference member has no default
      std::optional<bool> r = triviallyDefaultConstructible(part, depth + 1);
      if (!r || !*r)
        return r;
    }
    return true;
  }

  // isInvocable: whether a value of type f can be called with arguments of these types (each a
  // reference type for an lvalue, as in C++'s declval). result, when given, is the call's type
  // where it is known without typing a body.
  std::optional<bool> invocable(const Ty* f, const std::vector<const Ty*>& args, const Ty** result)
  {
    for (const Ty* a : args)
      if (!a || !a->isKnown() || types.isDependent(a))
        return std::nullopt;
    if (!f || !f->isKnown() || types.isDependent(f))
      return std::nullopt;
    const Ty* c = types.unqualified(types.stripRef(f));
    if (c->kind == TyKind::Pointer && c->inner && c->inner->kind == TyKind::Function)
      c = c->inner;
    // Whether params take the arguments from the first-th on.
    auto accepts = [&](Span<const Ty*> params, bool variadic, size_t first = 0) -> std::optional<bool>
    {
      size_t given = args.size() - first;
      if (given < params.size() || (!variadic && given > params.size()))
        return false;
      for (size_t i = 0; i < params.size(); ++i)
      {
        if (!params[i] || !params[i]->isKnown() || types.isDependent(params[i]))
          return std::nullopt;
        if (conversionRank(args[first + i], params[i]) >= NotViable)
          return false;
      }
      return true;
    };
    if (c->kind == TyKind::Function)
    {
      if (result)
        *result = c->inner;
      return accepts(c->args, c->variadic);
    }
    if (c->kind == TyKind::Lambda)
    {
      LambdaExpr* l = lambdaOf(c);
      if (!l)
        return std::nullopt;
      if (l->templateParams.empty())
      {
        if (result)
          *result = c->inner;
        return accepts(c->args, false);
      }
      // A generic lambda: its `auto` parameters deduce from the arguments; its result needs the
      // body typed for them, which is left to the call.
      Subst deduced;
      deduceParams(l->params, c->args, args, deduced);
      std::vector<const Ty*> params;
      bool pack = false;
      for (const Ty* p : c->args)
      {
        const Ty* s = substitute(p, deduced);
        if (s->kind == TyKind::Pack)
        {
          pack = true;
          for (const Ty* e : s->args)
            params.push_back(e);
        }
        else
          params.push_back(s);
      }
      if (!pack && args.size() != params.size())
        return false;
      return accepts(Span<const Ty*>{params.data(), uint32_t(params.size())}, pack);
    }
    if (c->kind == TyKind::Class)
    {
      // A function object: its call operators, its bases' too, by overload resolution.
      std::vector<FunctionDecl*> candidates;
      std::vector<const Ty*> classes{c};
      for (size_t i = 0; i < classes.size() && i < 32; ++i)
      {
        ClassDecl* def = classDefinition(classes[i]);
        if (!def)
          return std::nullopt;
        for (Decl* m : membersRead(def))
          if (m->kind == NodeKind::FunctionDecl)
            if (auto* fn = static_cast<FunctionDecl*>(m); !fn->name.empty() && fn->name.last().special == 2 && fn->name.last().opToken == uint32_t(P::LParen))
              candidates.push_back(fn);
        if (!directBases(classes[i], def, classes))
          return std::nullopt;
      }
      if (candidates.empty())
        return false;
      Subst s = substFor(c);
      bool ambiguous = false;
      FunctionDecl* chosen = chooseAmong(candidates, args, s.pairs.empty() ? nullptr : &s, f, ambiguous);
      if (!chosen || ambiguous)
        return false;
      if (result)
      {
        const Ty* ft = declaredTypeOf(chosen, s.pairs.empty() ? nullptr : &s, 0);
        *result = ft && ft->kind == TyKind::Function ? ft->inner : nullptr;
      }
      return true;
    }
    if (c->kind == TyKind::MemberPointer && c->inner && c->inner->kind == TyKind::Function && !c->args.empty() && c->args[0])
    {
      // A pointer to a member function takes the object first (a reference to it, or a pointer),
      // then the function's arguments, as std::invoke does: isInvocableR<bool, bool (W::*)(int),
      // W&, int>. The function's own const-ness is not in its type here, so not checked.
      if (args.empty())
        return false;
      const Ty* cls = types.unqualified(c->args[0]);
      const Ty* object = types.unqualified(types.stripRef(args[0]));
      if (object->kind == TyKind::Pointer && object->inner)
        object = types.unqualified(object->inner);
      if (cls->kind != TyKind::Class || object->kind != TyKind::Class)
        return object->kind == TyKind::Class || object->kind == TyKind::Unknown ? std::nullopt : std::optional<bool>(false);
      if (object != cls && !isBaseClass(cls, object))
        return false;
      if (result)
        *result = c->inner->inner;
      return accepts(c->inner->args, c->inner->variadic, 1);
    }
    if (c->kind == TyKind::MemberPointer)
      return std::nullopt; // a pointer to a data member: not decided here
    return false; // a pointer to data, a number, an enumeration: nothing to call
  }

  // Walk a class's own members and its bases to find `static constexpr bool value`.
  // Returns the VariableDecl if found, nullptr otherwise.  Used by constEval to fold
  // `SomeTrait<T>()` (a zero-arg constructor of a TrueType/FalseType–derived struct)
  // and `SomeTrait<T>::value` (a qualified access to the inherited static member).
  // A trait object converted to bool (`HasSize<T>()`, `HasSize<T>{}`): the class's static
  // constexpr `value`, its own or a base's, read for the instance its arguments make, a base's
  // with that base's arguments (`HasSize<double> : Constant<bool, false>`: V is false).
  std::optional<ConstValue> traitObjectValue(const Ty* cls, int depth)
  {
    cls = cls ? types.unqualified(types.stripRef(cls)) : nullptr;
    if (!cls || cls->kind != TyKind::Class || !cls->decl || types.isDependent(cls) || depth > 64)
      return std::nullopt;
    Scope* s = classScopeOf(cls);
    Lookup l;
    if (!s || !lookupInScope(s, symValue, l, depth + 1) || !l.decl || l.decl->kind != NodeKind::VariableDecl || !(l.decl->flags & DF_Static))
      return std::nullopt;
    auto* v = static_cast<VariableDecl*>(l.decl);
    Subst sub = memberSubst(cls, l.foundIn, depth + 1);
    VariableDecl* def = v->init ? v : variableDefinitionFor(v, &sub);
    if (!def->init)
      return std::nullopt;
    LocalStack own;
    pushEnclosingTemplateParams(v, own);
    TypeContext vctx{lexicalScopeOf(v), &own, sub.pairs.empty() ? nullptr : &sub, def->file ? def->file->tokens : nullptr,
                     def->file ? def->file->text : std::string_view()};
    return constAsDeclared(constInitValue(def->init, vctx, depth + 1), v->type ? convertType(v->type, vctx, depth + 1) : nullptr);
  }

  // Whether e names a tuple-like structured binding (its hidden `auto&&` variable, Walker::tupleBindings).
  static bool namesTupleBinding(const Expr* e)
  {
    Decl* d = e && e->kind == NodeKind::NameExpr ? static_cast<const NameExpr*>(e)->resolved : nullptr;
    return d && d->kind == NodeKind::VariableDecl && static_cast<VariableDecl*>(d)->tupleBinding;
  }

  // `auto [a, b] = e;` of class type t: whether the library's TupleSize<t> (std::tuple_size) has
  // a member named value, which makes the binding tuple-like (design.md 4, structured bindings);
  // size gets that value, nothing when it is no constant.
  bool tupleSizeOf(const Ty* t, std::optional<int64_t>& size, int depth)
  {
    Lookup l;
    if (!lookupInScope(&root, symTupleSize, l, depth + 1, false) || !l.decl)
      return false;
    Decl* primary = nullptr;
    auto isPrimary = [](Decl* d) { return d && d->kind == NodeKind::ClassDecl && !(d->flags & DF_Specialization) && d->templateParams.size() == 1; };
    if (l.overloads)
    {
      for (const Entry& e : *l.overloads)
        if (isPrimary(e.decl))
          primary = e.decl;
    }
    else if (isPrimary(l.decl))
      primary = l.decl;
    if (!primary)
      return false;
    const Ty* args[1] = {t};
    const Ty* instance = resolveSpecialization(types.classType(primary, Span<const Ty*>{args, 1}), depth + 1);
    Scope* s = classScopeOf(instance);
    Lookup value;
    if (!s || !lookupInScope(s, symValue, value, depth + 1) || !value.decl)
      return false;
    if (std::optional<ConstValue> v = traitObjectValue(instance, depth + 1); v && v->kind == ConstValue::Kind::Int)
      size = v->i;
    return true;
  }

  // Whether class t has a member function template get whose first template parameter is a
  // constant (`template<int I> auto& get()`): its tuple-like bindings call e.get<i>(), as C++'s do.
  bool hasMemberGet(const Ty* t, int depth)
  {
    Scope* s = classScopeOf(t);
    Lookup l;
    if (!s || !lookupInScope(s, symGet, l, depth + 1) || !l.decl || l.decl->kind != NodeKind::FunctionDecl)
      return false;
    auto constantFirst = [](Decl* d)
    {
      return d && d->kind == NodeKind::FunctionDecl && !d->templateParams.empty() && d->templateParams[0]->kind == NodeKind::TemplateParamDecl &&
             !static_cast<TemplateParamDecl*>(d->templateParams[0])->isType;
    };
    if (!l.overloads)
      return constantFirst(l.decl);
    for (const Entry& e : *l.overloads)
      if (constantFirst(e.decl))
        return true;
    return false;
  }

  VariableDecl* findStaticBoolValue(ClassDecl* c, int depth)
  {
    if (!c || depth > 8)
      return nullptr;
    for (Decl* m : membersRead(c))
    {
      if (m->kind != NodeKind::VariableDecl || !(m->flags & DF_Static))
        continue;
      auto* v = static_cast<VariableDecl*>(m);
      if (v->name.empty() || v->name.last().symbol != symValue)
        continue;
      if ((v->flags & DF_Constexpr) && v->init)
        return v;
    }
    if (c->ownScope)
    {
      ensureBases(c->ownScope, depth);
      for (Scope* base : c->ownScope->baseScopes)
        if (base->owner && base->owner->kind == NodeKind::ClassDecl)
          if (VariableDecl* found = findStaticBoolValue(static_cast<ClassDecl*>(base->owner), depth + 1))
            return found;
    }
    return nullptr;
  }

  // `path:line` of a token, `path:line:column` with --diagnostic-columns (sourceLocation).
  static std::string locationOf(const ParsedFile* file, uint32_t token)
  {
    if (!file || !file->tokens || file->tokens->empty())
      return file ? file->path : std::string("?");
    const Token& t = (*file->tokens)[std::min<size_t>(token, file->tokens->size() - 1)];
    size_t offset = std::min<size_t>(t.offset, file->text.size());
    size_t lineStart = offset == 0 ? std::string_view::npos : file->text.rfind('\n', offset - 1);
    lineStart = lineStart == std::string_view::npos ? 0 : lineStart + 1;
    return sourceLocation(file->path, 1 + std::count(file->text.begin(), file->text.begin() + offset, '\n'), 1 + offset - lineStart);
  }

  void reportCheckerError(const ParsedFile* file, uint32_t token, const std::string& message)
  {
    std::string text = locationOf(file, token) + ": error: " + message;
    reportCheckerErrorText(text);
    std::lock_guard lock(checkerErrorsMutex);
    checkerErrorPlaces[text] = {file, token};
  }

  void reportCheckerErrorText(const std::string& text)
  {
    std::lock_guard lock(checkerErrorsMutex);
    std::vector<const void*>& units = checkerErrorSet[text];
    if (std::find(units.begin(), units.end(), reportingUnit) == units.end())
      units.push_back(reportingUnit);
    if (usesCollector) // (a cached answer's computation: its later askers report it too, replayUses)
      usesCollector->push_back({USE_ReportedError, &*reportedErrorTexts.insert(text).first});
  }

  // The errors the units given reported (and the layout asserts they met), taken back: the units
  // are walked or checked again and report what they still find. An error another unit reported
  // too stays.
  void forgetErrorsOf(const std::unordered_set<const void*>& units)
  {
    if (units.empty())
      return;
    std::lock_guard lock(checkerErrorsMutex);
    for (auto it = checkerErrorSet.begin(); it != checkerErrorSet.end();)
    {
      std::erase_if(it->second, [&](const void* u) { return u && units.count(u); });
      if (it->second.empty())
      {
        checkerErrorPlaces.erase(it->first);
        it = checkerErrorSet.erase(it);
      }
      else
        ++it;
    }
    std::erase_if(layoutAsserts, [&](const LayoutAssert& a) { return a.unit && units.count(a.unit); });
  }

  // A declaration removed from a kept build (a compile server's, step 4: unindexFunction,
  // unindexMember): its walk's recorded reads go, and the errors its walk and its declared type
  // reported with them. Its walk is never run again to take them back, as a changed body's is, so a
  // removed function's error stayed in every later build until the server ended (E, 2026-11-04).
  void forgetUnitsOf(const Decl* d)
  {
    std::unordered_set<const void*> units;
    std::unique_ptr<Dependencies> walk; // (freed once its errors are forgotten: they name it)
    {
      uintptr_t key = reinterpret_cast<uintptr_t>(d) + UNIT_Walk;
      DeclDependencyShard& shard = declDependencies[(key >> 6) % std::size(declDependencies)];
      std::lock_guard lock(shard.mutex);
      if (auto it = shard.map.find(key); it != shard.map.end())
      {
        walk = std::move(it->second);
        shard.map.erase(it);
      }
    }
    if (walk)
      units.insert(walk.get());
    if (Dependencies* type = d->typeDependencies.load(std::memory_order_acquire))
      units.insert(type);
    forgetErrorsOf(units);
  }

