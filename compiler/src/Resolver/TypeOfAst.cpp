// GlobalScope::Impl, phase 2, types (its declarations in TypeOfAst.hpp, part of GlobalScopeImpl.hpp): a translation unit
// of its own since 2026-11-05 (resolver.cpp split, stage 2), moved without a change.
#include "GlobalScopeImpl.hpp"

auto GlobalScope::Impl::elementOf(const Decl* d, const Ty* bound, const TypeContext& ctx) -> const Ty*
{
  if (!ctx.elementPacks || !bound || bound->kind != TyKind::Pack || ctx.elementIndex >= bound->args.size())
    return bound;
  for (const Decl* p : *ctx.elementPacks)
    if (p == d)
      return bound->args[ctx.elementIndex];
  return bound;
}

auto GlobalScope::Impl::wellFormedArgument(Node* n, const TypeContext& ctx, int depth) -> bool
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

auto GlobalScope::Impl::variableSpecialization(VariableDecl* primary, const std::vector<const Ty*>& args, Subst& out) -> VariableDecl*
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

auto GlobalScope::Impl::isTypeNode(Node* n) -> bool
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

auto GlobalScope::Impl::traitArgs(const NamePart& part, const TypeContext& ctx, int depth) -> std::vector<const Ty*>
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

auto GlobalScope::Impl::enumUnderlyingType(const Ty* e, const TypeContext& ctx, int depth) -> const Ty*
{
  if (const Ty* u = declaredUnderlying(types.unqualified(e)); u && u->isKnown())
    return u; // (of an instance, and off Windows one as wide as its enumerators)
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

auto GlobalScope::Impl::isTypeTraitName(uint32_t symbol) -> bool
{
  std::string_view name = symbols.name(symbol);
  return name == "RemoveConst" || name == "RemoveCv" || name == "RemoveReference" || name == "RemoveExtent" || name == "RemovePointer" ||
         name == "Decay" || name == "UnderlyingType" || name == "AddConst" || name == "AddPointer" || name == "RemoveCvRef" ||
         name == "Conditional" || name == "EnableIf" || name == "VariantAlternative";
}

auto GlobalScope::Impl::typeTrait(const NamePart& part, const TypeContext& ctx, int depth) -> const Ty*
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

auto GlobalScope::Impl::isArithmeticKind(const Ty* t) -> bool
{
  return t->kind == TyKind::Int || t->kind == TyKind::Char || t->kind == TyKind::Bool || t->kind == TyKind::Float || t->kind == TyKind::Enum;
}

auto GlobalScope::Impl::compositePointer(const Ty* a, const Ty* b) -> const Ty*
{
  if (a->kind != TyKind::Pointer || b->kind != TyKind::Pointer || !a->inner || !b->inner)
    return nullptr;
  const Ty* pa = types.unqualified(a->inner);
  const Ty* pb = types.unqualified(b->inner);
  const Ty* pointee = nullptr;
  if (pa == pb || pb->kind == TyKind::Void || losesNoexcept(pa, pb))
    pointee = pb;
  else if (pa->kind == TyKind::Void || losesNoexcept(pb, pa))
    pointee = pa;
  else if (pa->kind == TyKind::Class && pb->kind == TyKind::Class && pa->decl != pb->decl)
    pointee = derivesFrom(pa, pb) ? pb : derivesFrom(pb, pa) ? pa : nullptr;
  if (!pointee)
    return nullptr;
  return types.pointer(types.qualified(pointee, a->inner->isConst || b->inner->isConst, a->inner->isVolatile || b->inner->isVolatile));
}

auto GlobalScope::Impl::derivesFrom(const Ty* derived, const Ty* base, int depth) -> bool
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
    for (const Ty* bt : baseInstances(b.type, TypeContext{dc.scope, &dc.params, own.pairs.empty() ? nullptr : &own, def->file ? def->file->tokens : nullptr,
                                                          def->file ? def->file->text : std::string_view()}, 0)) // (a pack of bases: each element)
      if (bt && derivesFrom(bt, base, depth + 1))
        return true;
  return false;
}

auto GlobalScope::Impl::isExplicit(FunctionDecl* f, const Subst* subst) -> bool
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

auto GlobalScope::Impl::isNoexcept(FunctionDecl* f, const Subst* subst) -> std::optional<bool>
{
  if (!(f->flags & DF_Noexcept))
    return false;
  if (!f->noexceptCondition)
    return true;
  DeclContext dc;
  declContextFor(f, dc);
  ParsedFile* file = f->file;
  TypeContext ctx{dc.scope, &dc.params, subst, file ? file->tokens : nullptr, file ? file->text : std::string_view()};
  ctx.function = f; // (its parameters, which `noexcept(noexcept(a = b))` names)
  std::optional<int64_t> v = constEval(f->noexceptCondition, ctx, 0);
  if (!v)
    return std::nullopt;
  return *v != 0;
}

auto GlobalScope::Impl::traitValue(std::string_view name, const std::vector<const Ty*>& a) -> std::optional<bool>
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

auto GlobalScope::Impl::traitValueUncached(std::string_view name, const std::vector<const Ty*>& a) -> std::optional<bool>
{
  auto one = [&]() { return a.size() == 1 ? a[0] : nullptr; };
  if (name == "isArray") return one() && one()->kind == TyKind::Array;
  if (name == "isPointer") return one() && one()->kind == TyKind::Pointer;
  if (name == "isEnum") return one() && one()->kind == TyKind::Enum;
  if (name == "isClass")
  {
    // (A union is none, [meta.unary.cat]; a `union enum` is a class of kpp's, a closure a class.)
    if (one() && one()->kind == TyKind::Lambda)
      return true;
    if (!one() || one()->kind != TyKind::Class)
      return false;
    ClassDecl* def = classDefinition(types.unqualified(one()));
    return !def || def->keyword != Kw::union_ || def->isTypedUnion;
  }
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
      if (copy)
        return true;
      if (ctors.empty())
        return destructibleOutside(static_cast<ClassDecl*>(def)) && aggregateTakes(t, static_cast<ClassDecl*>(def), args);
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
    return one() && ((isArithmeticKind(one()) && one()->kind != TyKind::Enum) || one()->kind == TyKind::Void || one() == types.nullptrType());
  if (name == "isTriviallyDestructible") return one() ? triviallyDestructible(one()) : std::nullopt;
  if (name == "destructorMayThrow") return one() ? destructorMayThrowUncached(one()) : std::nullopt; // (no trait of the language: destructorMayThrow's cache)
  // (These three define a variable, which is also destroyed: a destructor of its own is no trivial
  // construction, [meta.unary.prop] is_constructible.)
  auto endsTrivially = [&](std::optional<bool> constructs) -> std::optional<bool> {
    if (!constructs || !*constructs)
      return constructs;
    return triviallyDestructible(one());
  };
  if (name == "isTriviallyCopyConstructible") return one() ? endsTrivially(constructibleFromSelf(one(), false, TraitForm::Trivially)) : std::nullopt;
  if (name == "isTriviallyCopyAssignable") return one() ? assignableFromSelf(one(), false, TraitForm::Trivially) : std::nullopt;
  if (name == "isTriviallyDefaultConstructible") return one() ? endsTrivially(triviallyDefaultConstructible(one())) : std::nullopt;
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
  if (name == "isTriviallyMoveConstructible")
  {
    std::optional<bool> constructs = one() ? constructibleFromSelf(one(), true, TraitForm::Trivially) : std::nullopt;
    return constructs && *constructs ? triviallyDestructible(one()) : constructs;
  }
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

auto GlobalScope::Impl::isBoolTraitName(uint32_t symbol) -> bool
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

auto GlobalScope::Impl::classDefinition(const Ty* t) -> ClassDecl*
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

auto GlobalScope::Impl::directBases(const Ty* t, ClassDecl* def, std::vector<const Ty*>& out) -> bool
{
  DeclContext dc;
  declContextFor(def, dc);
  Subst s = substFor(t);
  TypeContext ctx{dc.scope, &dc.params, s.pairs.empty() ? nullptr : &s, def->file ? def->file->tokens : nullptr,
                  def->file ? def->file->text : std::string_view()};
  for (const ClassDecl::Base& b : def->bases)
  {
    const Ty* bt = convertType(b.type, ctx, 0);
    // `struct overloaded : Ts...`: a base per element.
    if (b.isPack && bt && bt->kind == TyKind::Pack)
    {
      for (const Ty* element : bt->args)
      {
        if (!element || !element->isKnown() || types.isDependent(element))
          return false;
        out.push_back(element);
      }
      continue;
    }
    if (b.isPack || !bt || !bt->isKnown() || types.isDependent(bt))
      return false;
    out.push_back(bt);
  }
  return true;
}

auto GlobalScope::Impl::fieldTypes(const Ty* t, ClassDecl* def, std::vector<const Ty*>& out) -> bool
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

auto GlobalScope::Impl::overrideKey(FunctionDecl* f, const Ty* cls) -> std::string
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
  if (f->flags & DF_Const)
    k += " const";
  if (f->flags & DF_Volatile)
    k += " volatile";
  return f->refQualifier == FunctionDecl::RefQualifier::None ? k : k + (f->refQualifier == FunctionDecl::RefQualifier::LValue ? " &" : " &&");
}

auto GlobalScope::Impl::polymorphicClass(const Ty* t, int depth) -> std::optional<bool>
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

auto GlobalScope::Impl::abstractClass(const Ty* t, std::unordered_set<std::string> overridden, int depth) -> std::optional<bool>
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

auto GlobalScope::Impl::selfParameter(Decl* p, ClassDecl* def) -> int
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

auto GlobalScope::Impl::specialMembersOf(ClassDecl* def) -> SpecialMembers
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

auto GlobalScope::Impl::declaredSpecial(FunctionDecl* f, TraitForm form, const Ty* of) -> std::optional<bool>
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

auto GlobalScope::Impl::constructibleFromSelf(const Ty* t, bool move, TraitForm form, int depth) -> std::optional<bool>
{
  if (!t || !t->isKnown() || types.isDependent(t) || depth > 16)
    return std::nullopt;
  if (t->isReference())
    return t->kind != TyKind::RRef || move; // (`int&&` binds no const int&)
  if (t->isVolatile && types.unqualified(t)->kind == TyKind::Class)
    return false; // (an implicit copy takes a const T&, which binds no volatile object)
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
    if (!destructibleOutside(def))
      return false;
  }
  SpecialMembers s = specialMembersOf(def);
  FunctionDecl* chosen = move && s.moveCtor ? s.moveCtor : s.copyCtor;
  if (chosen)
  {
    // (A trait asks from outside the class, and a class's implicit one from a derived class:
    // private is out of reach of both, protected of the first.)
    if (chosen->access == Access::Private || (depth == 0 && chosen->access == Access::Protected))
      return false;
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
  // A union's implicit one copies its bytes, so a member that copies any other way deletes it
  // ([class.copy.ctor] 10).
  TraitForm parts = def->keyword == Kw::union_ && !def->isTypedUnion ? TraitForm::Trivially : form;
  return membersAllow(u, def, form, [&](const Ty* m, int d) { return constructibleFromSelf(m, move, parts, d); }, false, depth);
}

auto GlobalScope::Impl::aggregateTakes(const Ty* t, ClassDecl* def, const std::vector<const Ty*>& args) -> bool
{
  for (Decl* m : membersRead(def))
    if (((m->flags & DF_Constructor) || (m->kind == NodeKind::VariableDecl && !(m->flags & (DF_Static | DF_Typedef)))) &&
        ((m->flags & DF_Constructor) || m->access == Access::Private || m->access == Access::Protected))
      return false; // no aggregate: it declares a constructor (a deleted one too), or hides a member
  for (const ClassDecl::Base& b : def->bases)
    if (b.isVirtual || b.access == Access::Private || b.access == Access::Protected)
      return false;
  if (polymorphicClass(t).value_or(true))
    return false;
  std::vector<const Ty*> parts;
  if (!directBases(t, def, parts) || !fieldTypes(t, def, parts))
    return false;
  if (def->keyword == Kw::union_ && parts.size() > 1)
    parts.resize(1); // (a union's first member: the one a list initializes)
  if (args.size() > parts.size())
    return false;
  for (size_t i = 0; i < args.size(); ++i)
    if (!traitValue("isConvertible", {args[i], parts[i]}).value_or(false))
      return false;
  return true;
}

auto GlobalScope::Impl::destructibleOutside(ClassDecl* def) -> bool
{
  for (Decl* m : membersRead(def))
    if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Destructor))
      return !(m->flags & DF_Delete) && m->access != Access::Private && m->access != Access::Protected;
  return true;
}

auto GlobalScope::Impl::assignableFromSelf(const Ty* t, bool move, TraitForm form, int depth) -> std::optional<bool>
{
  if (!t || !t->isKnown() || types.isDependent(t) || depth > 16)
    return std::nullopt;
  if (t->isReference())
  {
    t = types.stripRef(t); // `T& = T&`: a copy assignment of T
    move = false;
  }
  if (t->isConst || (t->isVolatile && types.unqualified(t)->kind == TyKind::Class))
    return false; // (an implicit assignment is no volatile member)
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

auto GlobalScope::Impl::membersAllow(const Ty* t, ClassDecl* def, TraitForm form, const std::function<std::optional<bool>(const Ty*, int)>& each, bool assignment, int depth) -> std::optional<bool>
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
  if (!directBases(t, def, parts))
    return std::nullopt;
  size_t baseCount = parts.size();
  if (!fieldTypes(t, def, parts))
    return std::nullopt;
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

auto GlobalScope::Impl::trivialSubject(const Ty* t) -> TrivialSubject
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

auto GlobalScope::Impl::triviallyDestructible(const Ty* t, int depth) -> std::optional<bool>
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

auto GlobalScope::Impl::destructorMayThrow(const Ty* t) -> std::optional<bool>
{
  // (a class holding itself by value is an error of its own: the walk must only end)
  static thread_local int depth = 0;
  if (!t || !t->isKnown() || types.isDependent(t) || depth > 64)
    return std::nullopt;
  if (t->isReference())
    return false;
  const Ty* u = types.unqualified(t);
  while (u->kind == TyKind::Array)
    u = types.unqualified(u->inner);
  TrivialSubject s = trivialSubject(u);
  if (!s.cls)
  {
    if (s.answer)
      return false; // (a scalar, a pointer, a closure holding nothing)
    LambdaExpr* l = u->kind == TyKind::Lambda ? lambdaOf(u) : nullptr;
    bool byReference = l && l->defaultCapture != 1 && std::all_of(l->captures.begin(), l->captures.end(), [](const LambdaCapture& c) { return c.byRef || c.isThis; });
    return byReference ? std::optional<bool>(false) : std::nullopt;
  }
  ++depth;
  std::optional<bool> answer = traitValue("destructorMayThrow", {resolveSpecialization(s.cls, 0)});
  --depth;
  return answer;
}

auto GlobalScope::Impl::destructorMayThrowUncached(const Ty* t) -> std::optional<bool>
{
  ClassDecl* def = classDefinition(t);
  if (!def)
    return std::nullopt;
  Subst subst = substFor(t);
  for (Decl* m : membersRead(def))
    if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Destructor) && (m->flags & DF_Noexcept))
    {
      std::optional<bool> declared = isNoexcept(static_cast<FunctionDecl*>(m), subst.pairs.empty() ? nullptr : &subst);
      return declared ? std::optional<bool>(!*declared) : std::nullopt;
    }
  return destructorPartsMayThrow(t);
}

auto GlobalScope::Impl::destructorPartsMayThrow(const Ty* t) -> std::optional<bool>
{
  ClassDecl* def = classDefinition(t);
  std::vector<const Ty*> parts;
  if (!def || !directBases(t, def, parts))
    return std::nullopt;
  Subst subst = substFor(t);
  const Subst* sp = subst.pairs.empty() ? nullptr : &subst;
  DeclContext dc;
  declContextFor(def, dc);
  TypeContext ctx{dc.scope, &dc.params, sp, def->file ? def->file->tokens : nullptr, def->file ? def->file->text : std::string_view()};
  for (Decl* m : membersRead(def))
  {
    // An anonymous struct's or union's: a part, as its members are the class's own.
    if (m->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(m)->name.empty() && static_cast<ClassDecl*>(m)->isDefinition)
    {
      const Ty* anonymous = selfType(m);
      parts.push_back(anonymous && sp ? substitute(anonymous, *sp) : anonymous);
      continue;
    }
    if (m->kind != NodeKind::VariableDecl || (m->flags & (DF_Static | DF_Typedef | DF_Friend)))
      continue;
    if (m->when)
    {
      // (design.md 19: a false `when`'s member is not there)
      std::optional<int64_t> holds = m->whenKnown >= 0 ? std::optional<int64_t>(m->whenKnown) : constEval(m->when, ctx, 0);
      if (!holds)
        return std::nullopt;
      if (!*holds)
        continue;
    }
    parts.push_back(declaredTypeOf(m, sp, 0));
  }
  if (def->isTypedUnion)
    for (Decl* a : def->alternatives)
      parts.push_back(declaredTypeOf(a, sp, 0));
  bool unknown = false;
  for (const Ty* part : parts)
  {
    std::optional<bool> r = destructorMayThrow(part);
    if (r && *r)
      return true;
    unknown = unknown || !r;
  }
  return unknown ? std::nullopt : std::optional<bool>(false);
}

auto GlobalScope::Impl::hasVirtualDestructor(const Ty* t, int depth) -> bool
{
  ClassDecl* def = depth <= 64 ? classDefinition(t) : nullptr;
  std::vector<const Ty*> bases;
  if (!def)
    return false;
  for (Decl* m : membersRead(def))
    if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Destructor) && (m->flags & (DF_Virtual | DF_Override)))
      return true;
  if (directBases(t, def, bases))
    for (const Ty* b : bases)
      if (hasVirtualDestructor(b, depth + 1))
        return true;
  return false;
}

auto GlobalScope::Impl::noteNonVirtualDestructor(const Ty* t, ClassDecl* c) -> void
{
  if (!c || !c->file || c->file->isImported || c->name.empty() || classDefinition(t) != c || polymorphicClass(t) != std::optional<bool>(true) ||
      hasVirtualDestructor(t))
    return;
  Decl* destructor = nullptr;
  for (Decl* m : membersRead(c))
    if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Destructor))
      destructor = m;
  bool rootOfVtable = true;
  std::vector<const Ty*> bases;
  if (directBases(t, c, bases))
    for (const Ty* b : bases)
      if (polymorphicClass(b) == std::optional<bool>(true))
        rootOfVtable = false;
  std::string kind = !destructor                                   ? "an implicit destructor"
                     : destructor->access == Access::Protected ? "a protected non-virtual destructor"
                     : destructor->access == Access::Private   ? "a private non-virtual destructor"
                                                                 : "a non-virtual destructor";
  noteOverrideWarning(locationOf(c->file, c->token) + ": warning: " + debugName(c->name.last().symbol) + " has a virtual function and " + kind +
                      ", which becomes virtual" + (rootOfVtable ? " (its vtable gains the slot)" : ""));
}

auto GlobalScope::Impl::destructorLoosens(const Ty* t) -> const Ty*
{
  ClassDecl* def = classDefinition(t);
  std::vector<const Ty*> bases;
  if (!def || def->bases.empty() || destructorMayThrow(t) != std::optional<bool>(true) || !directBases(t, def, bases))
    return nullptr;
  for (const Ty* b : bases)
    if (hasVirtualDestructor(b) && destructorMayThrow(b) == std::optional<bool>(false))
      return b;
  return nullptr;
}

auto GlobalScope::Impl::triviallyCopyable(const Ty* t) -> std::optional<bool>
{
  if (!t || !t->isKnown() || types.isDependent(t))
    return std::nullopt;
  if (t->isReference())
    return false;
  TrivialSubject s = trivialSubject(t);
  if (!s.cls)
    return s.answer;
  ClassDecl* def = classDefinition(s.cls);
  if (!def)
    return std::nullopt;
  // Its own members decide, whoever may call them: a private copy constructor, `C(C&)` and
  // `operator=(C&)` copy as a public one does; a deleted destructor is none of its own.
  for (Decl* m : membersRead(def))
    if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Destructor) && (!(m->flags & (DF_Default | DF_Delete)) || (m->flags & (DF_Virtual | DF_Override))))
      return false;
  SpecialMembers own = specialMembersOf(def);
  for (FunctionDecl* f : {own.copyCtor, own.copyCtorNonConst, own.moveCtor, own.copyAssign, own.copyAssignNonConst, own.moveAssign})
    if (f && !(f->flags & (DF_Default | DF_Delete)))
      return false;
  // Then its parts, whose copies it deleted or not (a union's member that copies otherwise).
  std::vector<const Ty*> parts;
  if (!directBases(s.cls, def, parts) || !fieldTypes(s.cls, def, parts))
    return std::nullopt;
  for (const Ty* part : parts)
    if (!part->isReference())
      if (std::optional<bool> whole = triviallyCopyable(part); !whole || !*whole)
        return whole;
  // (Every operation deleted leaves the class trivially copyable to clang and g++, as to
  // their libraries; CWG 1734's "at least one" is theirs to take up.)
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
  }
  return true;
}

auto GlobalScope::Impl::defaultConstructible(const Ty* t, int depth) -> std::optional<bool>
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
  // (As a base, deeper, an abstract class's constructors serve its derived classes.)
  if (depth == 0)
  {
    if (std::optional<bool> isAbstract = abstractClass(u, {}); !isAbstract || *isAbstract)
      return isAbstract ? std::optional<bool>(false) : std::nullopt;
    if (!destructibleOutside(def))
      return false;
  }
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
    if ((f->flags & DF_Delete) || f->access == Access::Private || (depth == 0 && f->access == Access::Protected))
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

auto GlobalScope::Impl::triviallyDefaultConstructible(const Ty* t, int depth) -> std::optional<bool>
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

auto GlobalScope::Impl::invocable(const Ty* f, const std::vector<const Ty*>& args, const Ty** result) -> std::optional<bool>
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

auto GlobalScope::Impl::traitObjectValue(const Ty* cls, int depth) -> std::optional<ConstValue>
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

auto GlobalScope::Impl::namesTupleBinding(const Expr* e) -> bool
{
  Decl* d = e && e->kind == NodeKind::NameExpr ? static_cast<const NameExpr*>(e)->resolved : nullptr;
  return d && d->kind == NodeKind::VariableDecl && static_cast<VariableDecl*>(d)->tupleBinding;
}

auto GlobalScope::Impl::tupleSizeOf(const Ty* t, std::optional<int64_t>& size, int depth) -> bool
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

auto GlobalScope::Impl::hasMemberGet(const Ty* t, int depth) -> bool
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

auto GlobalScope::Impl::findStaticBoolValue(ClassDecl* c, int depth) -> VariableDecl*
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
static std::string placeOf(const ParsedFile* file, uint32_t token)
{
  if (!file || !file->tokens || file->tokens->empty())
    return file ? file->path : std::string("?");
  const Token& t = (*file->tokens)[std::min<size_t>(token, file->tokens->size() - 1)];
  size_t offset = std::min<size_t>(t.offset, file->text.size());
  LineStarts starts(file->text);
  return sourceLocation(file->path, starts.line(offset), starts.column(offset));
}

auto GlobalScope::Impl::locationOf(const ParsedFile* file, uint32_t token) -> std::string { return placeOf(file, token); }

auto GlobalScope::Impl::spelledTokens(const ParsedFile* file, uint32_t first, uint32_t last) -> std::string
{
  if (!file || !file->tokens || first > last || last >= file->tokens->size())
    return {};
  const Token& a = (*file->tokens)[first];
  const Token& b = (*file->tokens)[last];
  size_t end = std::min<size_t>(size_t(b.offset) + b.length, file->text.size());
  return a.offset < end ? std::string(file->text.substr(a.offset, end - a.offset)) : std::string();
}

auto GlobalScope::Impl::declaratorEnd(const ParsedFile* file, uint32_t name, uint32_t* overrideToken) -> uint32_t
{
  if (overrideToken)
    *overrideToken = UINT32_MAX;
  if (!file || !file->tokens || name >= file->tokens->size())
    return name;
  const std::vector<Token>& tokens = *file->tokens;
  int depth = 0;
  uint32_t last = name;
  for (uint32_t i = name; i < tokens.size() && i < name + 4096; ++i)
  {
    const Token& t = tokens[i];
    if (t.is(P::LParen) || t.is(P::LBracket))
      ++depth;
    else if (t.is(P::RParen) || t.is(P::RBracket))
      --depth;
    else if (depth == 0 && i > name && (t.is(P::LBrace) || t.is(P::Semi) || t.is(P::Assign) || t.is(P::RBrace)))
      break;
    if (overrideToken && depth == 0 && t.getKind() == TokenKind::Identifier && file->text.substr(t.offset, t.length) == "override")
      *overrideToken = i;
    last = i;
  }
  return last;
}

std::string CheckerError::text() const { return placeOf(this->file, this->token) + ": error: " + this->message; }

auto GlobalScope::Impl::reportCheckerError(const ParsedFile* file, uint32_t token, const std::string& message) -> void
{
  reportCheckerError(CheckerError{file, token, message});
}

auto GlobalScope::Impl::reportCheckerError(const CheckerError& error) -> void
{
  std::lock_guard lock(checkerErrorsMutex);
  std::vector<const void*>& units = checkerErrorSet[error];
  if (std::find(units.begin(), units.end(), reportingUnit) == units.end())
    units.push_back(reportingUnit);
  if (usesCollector) // (a cached answer's computation: its later askers report it too, replayUses)
    usesCollector->push_back({USE_ReportedError, &*reportedErrors.insert(error).first});
}

auto GlobalScope::Impl::reportCheckerWarning(const ParsedFile* file, uint32_t token, const std::string& message) -> void
{
  std::lock_guard lock(checkerErrorsMutex);
  std::vector<const void*>& units = checkerWarningSet[CheckerError{file, token, message}];
  if (std::find(units.begin(), units.end(), reportingUnit) == units.end())
    units.push_back(reportingUnit);
}

auto GlobalScope::Impl::forgetErrorsOf(const std::unordered_set<const void*>& units) -> void
{
  if (units.empty())
    return;
  std::lock_guard lock(checkerErrorsMutex);
  for (auto it = checkerWarningSet.begin(); it != checkerWarningSet.end();)
  {
    std::erase_if(it->second, [&](const void* u) { return u && units.count(u); });
    if (it->second.empty())
      it = checkerWarningSet.erase(it);
    else
      ++it;
  }
  for (auto it = checkerErrorSet.begin(); it != checkerErrorSet.end();)
  {
    std::erase_if(it->second, [&](const void* u) { return u && units.count(u); });
    if (it->second.empty())
      it = checkerErrorSet.erase(it);
    else
      ++it;
  }
  std::erase_if(layoutAsserts, [&](const LayoutAssert& a) { return a.unit && units.count(a.unit); });
}

auto GlobalScope::Impl::forgetUnitsOf(const Decl* d) -> void
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
