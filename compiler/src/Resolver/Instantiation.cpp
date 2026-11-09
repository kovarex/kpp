// GlobalScope::Impl, templates: class arguments, substitutions, declared types, deduction (its declarations in Instantiation.hpp, part of
// GlobalScopeImpl.hpp): a translation unit of its own since 2026-11-05 (resolver.cpp split, stage 2), moved without a change.
#include "GlobalScopeImpl.hpp"

auto GlobalScope::Impl::enclosingClassDecl(Scope* s) -> Decl*
{
  for (; s; s = s->parent)
    if (s->kind == ScopeKind::Class || s->kind == ScopeKind::Enum)
      return s->owner;
  return nullptr;
}

auto GlobalScope::Impl::enumeratorCount(Decl* e) -> size_t
{
  size_t n = 0;
  for (Decl* d : static_cast<EnumDecl*>(e)->enumerators)
    n += d->kind == NodeKind::EnumeratorDecl;
  return n;
}

auto GlobalScope::Impl::effectiveParams(Decl* cls) -> Span<Decl*>
{
  for (Decl* d = cls; d; d = d->parent)
  {
    if (d->kind != NodeKind::ClassDecl && d->kind != NodeKind::EnumDecl)
      break;
    if (d->kind == NodeKind::ClassDecl && !static_cast<ClassDecl*>(d)->instanceParams.empty())
      return static_cast<ClassDecl*>(d)->instanceParams;
    if (!d->templateParams.empty())
      return d->templateParams;
  }
  return {};
}

auto GlobalScope::Impl::enclosingParamCount(Decl* cls) -> size_t
{
  if (!cls || cls->kind != NodeKind::ClassDecl || static_cast<ClassDecl*>(cls)->instanceParams.empty())
    return 0;
  return static_cast<ClassDecl*>(cls)->instanceParams.size() - cls->templateParams.size();
}

auto GlobalScope::Impl::enumTypeIn(Decl* e, const Subst* subst) -> const Ty*
{
  Span<Decl*> params = effectiveParams(e);
  if (params.empty())
    return types.enumType(e);
  std::vector<const Ty*> args;
  for (Decl* p : params)
  {
    const Ty* a = subst ? subst->find(p) : nullptr;
    if (a && a->kind == TyKind::Pack)
      args.insert(args.end(), a->args.begin(), a->args.end()); // a pack's elements, as a class's arguments
    else
      args.push_back(a ? a : types.param(p));
  }
  return types.enumType(e, Span<const Ty*>{args.data(), uint32_t(args.size())});
}

auto GlobalScope::Impl::tagTypeOf(ClassDecl* u, const Ty* of) -> const Ty*
{
  Subst s = of ? substFor(types.unqualified(types.stripRef(of))) : Subst{};
  return enumTypeIn(u->tagEnum, s.empty() ? nullptr : &s);
}

auto GlobalScope::Impl::selfType(Decl* d) -> const Ty*
{
  if (d->kind == NodeKind::EnumDecl)
    return enumTypeIn(d, nullptr);
  std::vector<const Ty*> args;
  for (Decl* p : effectiveParams(d))
    args.push_back(types.param(p));
  return types.classType(d, Span<const Ty*>{args.data(), uint32_t(args.size())});
}

auto GlobalScope::Impl::thisTypeOf(const Ty* objectType) -> const Ty*
{
  const Ty* t = types.unqualified(types.stripRef(objectType));
  return t->kind == TyKind::Enum ? types.lref(t) : types.pointer(t);
}

auto GlobalScope::Impl::substFor(const Ty* classTy) -> Subst
{
  Subst s;
  // (An enum of a class template's instance has the instance's arguments: `Flag<A>::Enum::True`.)
  if (!classTy || !classTy->decl || (classTy->kind != TyKind::Class && !(classTy->kind == TyKind::Enum && !classTy->args.empty())))
    return s;
  Span<Decl*> params = effectiveParams(classTy->decl);
  for (size_t i = 0; i < params.size() && i < classTy->args.size(); ++i)
  {
    if (params[i]->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(params[i])->isPack)
    {
      // `template<typename R, typename... Args>`: the pack takes the rest of the arguments.
      std::vector<const Ty*> rest(classTy->args.begin() + i, classTy->args.end());
      if (rest.size() == 1 && rest[0]->kind == TyKind::Pack)
        s.pairs.push_back({params[i], rest[0]});
      else
        s.pairs.push_back({params[i], types.pack(Span<const Ty*>{rest.data(), uint32_t(rest.size())})});
      break;
    }
    s.pairs.push_back({params[i], classTy->args[i]});
  }
  if (!params.empty() && classTy->args.size() == params.size() - 1)
  {
    Decl* lastParam = params[params.size() - 1];
    if (lastParam->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(lastParam)->isPack)
      s.pairs.push_back({lastParam, types.pack({})}); // `Function<void()>`: an empty pack
  }
  if (instanceSubstOf(classTy->decl))
    return withInstanceSubst(classTy->decl, &s); // a class of an instance's body: that instance's arguments too
  return s;
}

auto GlobalScope::Impl::memberTypeOf(const Ty* t, uint32_t symbol, int depth) -> const Ty*
{
  Scope* inside = classScopeOf(t);
  if (!inside || depth > 16)
    return nullptr;
  Lookup inner;
  lookupInScope(inside, symbol, inner, depth + 1);
  lookupThroughBaseInstances(t, symbol, inner, depth + 1); // (in the specialisation a base instance is)
  if (inner.found != Found::No && inner.decl)
  {
    Subst s2 = memberSubst(t, inner.foundIn, depth + 1); // a member of a base: that base's arguments
    TypeContext c2{inside, nullptr, &s2};
    NamePart part{};
    part.symbol = symbol;
    return typeOfTypeDecl(inner.decl, inner, part, c2, depth + 1);
  }
  if (types.isDependent(t) || inside->baseTypes.empty() || !inside->owner)
    return nullptr;
  Subst own = substFor(t);
  LocalStack params;
  for (Decl* p : inside->templateParams)
    params.push(static_cast<TemplateParamDecl*>(p)->name, p);
  pushEnclosingTemplateParams(inside->owner->parent, params);
  ParsedFile* file = inside->owner->file;
  TypeContext ctx{inside->parent, &params, own.pairs.empty() ? nullptr : &own, file ? file->tokens : nullptr, file ? file->text : std::string_view()};
  for (Type* b : inside->baseTypes)
  {
    Type* named = b->kind == NodeKind::PackType ? static_cast<PackType*>(b)->pattern : b; // (a pack of bases: its pattern, `Ts` of `Ts...`)
    bool parameter = named->kind == NodeKind::NamedType && static_cast<NamedType*>(named)->name.parts.size() == 1 &&
                     !static_cast<NamedType*>(named)->name.parts[0].hasTemplateArgs;
    // `struct Const : Conditional<isIntegral<T>, Value<T>, T>`: a type trait, the class it picks
    // for these arguments (the lookup above searched the trait's own scope).
    bool trait = named->kind == NodeKind::NamedType && static_cast<NamedType*>(named)->name.parts.size() == 1 &&
                 static_cast<NamedType*>(named)->name.parts[0].hasTemplateArgs && isTypeTraitName(static_cast<NamedType*>(named)->name.parts[0].symbol);
    if (!parameter && !trait)
      continue; // a written class's members were found by the lookup above
    for (const Ty* bt : baseInstances(b, ctx, depth + 1))
      if (bt && bt->kind == TyKind::Class && bt != t)
        if (const Ty* r = memberTypeOf(bt, symbol, depth + 1))
          return r;
  }
  return nullptr;
}

auto GlobalScope::Impl::instantiationOfBase(const Ty* t, Scope* cls, Scope* in, int depth) -> const Ty*
{
  if (cls == in)
    return t;
  if (!cls || depth > 16 || cls->baseTypes.empty() || !cls->owner)
    return nullptr;
  Subst own = substFor(t);
  LocalStack params;
  for (Decl* p : cls->templateParams)
    params.push(static_cast<TemplateParamDecl*>(p)->name, p);
  pushEnclosingTemplateParams(cls->owner->parent, params); // a class of a function template's body: `ElemProvider<ElemType>`
  ParsedFile* file = cls->owner->file;
  TypeContext ctx{cls->parent, &params, own.pairs.empty() ? nullptr : &own, file ? file->tokens : nullptr, file ? file->text : std::string_view()};
  for (Type* b : cls->baseTypes)
    for (const Ty* bt : baseInstances(b, ctx, depth + 1))
    {
      if (!bt || bt->kind != TyKind::Class || !bt->decl)
        continue;
      Scope* bs = bt->decl->ownScope;
      if (!bs)
      {
        Entry e{bt->decl, nullptr};
        bs = scopeOfEntry(e, 0);
      }
      if (const Ty* found = instantiationOfBase(bt, bs, in, depth + 1))
        return found;
    }
  return nullptr;
}

auto GlobalScope::Impl::lookupThroughBaseInstances(const Ty* t, uint32_t symbol, Lookup& l, int depth) -> void
{
  if (!t || t->kind != TyKind::Class || !t->decl || depth > 16 || types.isDependent(t))
    return;
  Scope* cls = classScopeOf(t);
  if (!cls || cls->baseTypes.empty() || !cls->owner || l.found == Found::Dependent || l.absent)
    return; // (a false `when`'s name is no base's)
  if (l.found != Found::No)
  {
    // Found in the class itself, in a base no specialisation can replace, or in the base t
    // derives from as found: the static lookup is t's.
    Scope* in = l.foundIn;
    if (!in || in == cls || in->kind != ScopeKind::Class || !in->owner || !(in->owner->attributes & ATTR_Specialized) ||
        instantiationOfBase(t, cls, in, depth + 1))
      return;
  }
  Lookup instead;
  if (lookupInBaseInstances(t, cls, symbol, instead, depth + 1))
    l = instead;
}

auto GlobalScope::Impl::lookupInBaseInstances(const Ty* t, Scope* cls, uint32_t symbol, Lookup& out, int depth) -> bool
{
  if (!cls || depth > 16 || cls->baseTypes.empty() || !cls->owner)
    return false;
  Subst own = substFor(t);
  LocalStack params;
  for (Decl* p : cls->templateParams)
    params.push(static_cast<TemplateParamDecl*>(p)->name, p);
  pushEnclosingTemplateParams(cls->owner->parent, params);
  ParsedFile* file = cls->owner->file;
  TypeContext ctx{cls->parent, &params, own.pairs.empty() ? nullptr : &own, file ? file->tokens : nullptr, file ? file->text : std::string_view()};
  bool any = false;
  for (Type* b : cls->baseTypes)
    for (const Ty* bt : baseInstances(b, ctx, depth + 1))
    {
      if (!bt || bt->kind != TyKind::Class || !bt->decl || bt == t || types.isDependent(bt))
        continue;
      Scope* bs = classScopeOf(bt);
      if (!bs)
        continue;
      Lookup l;
      lookupInScope(bs, symbol, l, depth + 1);
      lookupThroughBaseInstances(bt, symbol, l, depth + 1);
      if (l.found == Found::No)
        continue;
      if (!any)
      {
        out = l;
        any = true;
        continue;
      }
      // Two bases that have the name: one static member, type or enumerator reached twice is one
      // name; anything else is ambiguous, as lookupInScope has it ([class.member.lookup] 6).
      if (out.ambiguousWith || l.found == Found::Dependent || out.found == Found::Dependent || symbol == opSymbols[size_t(P::Assign)])
        continue;
      if (l.foundIn == out.foundIn && l.decl == out.decl && !isInstanceMember(l.decl))
        continue;
      out.ambiguousWith = l.decl;
      out.ambiguousIn = l.foundIn;
    }
  return any;
}

auto GlobalScope::Impl::instantiationsOfBase(const Ty* t, Scope* cls, Scope* in, int depth, std::vector<const Ty*>& out) -> void
{
  if (cls == in)
  {
    if (std::find(out.begin(), out.end(), t) == out.end())
      out.push_back(t);
    return;
  }
  if (!cls || depth > 16 || cls->baseTypes.empty() || !cls->owner)
    return;
  Subst own = substFor(t);
  LocalStack params;
  for (Decl* p : cls->templateParams)
    params.push(static_cast<TemplateParamDecl*>(p)->name, p);
  pushEnclosingTemplateParams(cls->owner->parent, params);
  ParsedFile* file = cls->owner->file;
  TypeContext ctx{cls->parent, &params, own.pairs.empty() ? nullptr : &own, file ? file->tokens : nullptr, file ? file->text : std::string_view()};
  for (Type* b : cls->baseTypes)
    for (const Ty* bt : baseInstances(b, ctx, depth + 1))
    {
      if (!bt || bt->kind != TyKind::Class || !bt->decl)
        continue;
      Scope* bs = bt->decl->ownScope;
      if (!bs)
      {
        Entry e{bt->decl, nullptr};
        bs = scopeOfEntry(e, 0);
      }
      instantiationsOfBase(bt, bs, in, depth + 1, out);
    }
}

auto GlobalScope::Impl::bindingMembersClass(const Ty* t, int depth) -> const Ty*
{
  t = types.unqualified(types.stripRef(t));
  Scope* cs = classScopeOf(t);
  if (!cs || !cs->owner || cs->owner->kind != NodeKind::ClassDecl || depth > 16)
    return nullptr;
  for (Decl* m : membersRead(static_cast<ClassDecl*>(cs->owner)))
    if (m->kind == NodeKind::VariableDecl && !(m->flags & (DF_Static | DF_Typedef)))
      return t;
  Subst own = substFor(t);
  LocalStack params;
  for (Decl* p : cs->templateParams)
    params.push(static_cast<TemplateParamDecl*>(p)->name, p);
  pushEnclosingTemplateParams(cs->owner->parent, params);
  ParsedFile* file = cs->owner->file;
  TypeContext ctx{cs->parent, &params, own.pairs.empty() ? nullptr : &own, file ? file->tokens : nullptr, file ? file->text : std::string_view()};
  const Ty* found = nullptr;
  for (Type* b : cs->baseTypes)
    for (const Ty* bt : baseInstances(b, ctx, depth + 1))
      if (const Ty* holder = bt && bt->kind == TyKind::Class ? bindingMembersClass(bt, depth + 1) : nullptr)
      {
        if (found)
          return nullptr; // members in two bases: no binding
        found = holder;
      }
  return found;
}

auto GlobalScope::Impl::baseInstances(Type* b, const TypeContext& ctx, int depth) -> std::vector<const Ty*>
{
  const Ty* bt = convertType(b, ctx, depth);
  if (bt && bt->kind == TyKind::Pack)
    return std::vector<const Ty*>(bt->args.begin(), bt->args.end());
  return {bt};
}

auto GlobalScope::Impl::normalizeValueArgs(Span<Decl*> params, std::vector<const Ty*>& args, Decl* def, int depth) -> void
{
  for (size_t i = 0; i < args.size() && i < params.size(); ++i)
  {
    auto* p = static_cast<TemplateParamDecl*>(params[i]);
    const Ty* a = args[i];
    if (p->isType || p->isPack || !p->valueType || !a || a->kind != TyKind::Value || a->decl)
      continue; // a function as a constant keeps its own type
    LocalStack own;
    Subst s;
    for (size_t k = 0; k < params.size(); ++k)
      own.push(static_cast<TemplateParamDecl*>(params[k])->name, params[k]);
    for (size_t k = 0; k < i; ++k)
      s.pairs.push_back({params[k], args[k]});
    TypeContext vctx{lexicalScopeOf(def), &own, s.pairs.empty() ? nullptr : &s, def->file ? def->file->tokens : nullptr,
                     def->file ? def->file->text : std::string_view()};
    const Ty* of = types.unqualified(convertType(p->valueType, vctx, depth + 1));
    if (of && of->isKnown() && !types.isDependent(of) && of != a->inner)
    {
      // (a floating one by its value, its bits in the parameter's type: a float's 0.5f for a double parameter is 0.5)
      bool floatingArg = a->inner && types.unqualified(a->inner)->kind == TyKind::Float;
      if (of->kind == TyKind::Float)
        args[i] = types.valueType(of, TypeTable::floatBits(floatingArg ? TypeTable::floatOfBits(a->value, types.unqualified(a->inner)->bits)
                                                                          : a->inner && a->inner->isUnsigned ? double(uint64_t(a->value)) : double(a->value), of->bits));
      else if (!floatingArg)
        args[i] = types.valueType(of, of->kind == TyKind::Bool ? int64_t(a->value != 0) : a->value);
    }
  }
}

auto GlobalScope::Impl::derivesFromInstance(const Ty* t, Scope* cls, const Ty* base, int depth) -> bool
{
  if (!t || !cls || depth > 16)
    return false;
  if (types.unqualified(t) == base)
    return true;
  if (cls->baseTypes.empty() || !cls->owner)
    return false;
  Subst own = substFor(t);
  LocalStack params;
  for (Decl* p : cls->templateParams)
    params.push(static_cast<TemplateParamDecl*>(p)->name, p);
  pushEnclosingTemplateParams(cls->owner->parent, params); // a class of a function template's body: `ElemProvider<ElemType>`
  ParsedFile* file = cls->owner->file;
  TypeContext ctx{cls->parent, &params, own.pairs.empty() ? nullptr : &own, file ? file->tokens : nullptr, file ? file->text : std::string_view()};
  for (Type* b : cls->baseTypes)
    for (const Ty* bt : baseInstances(b, ctx, depth + 1))
    {
      if (!bt || bt->kind != TyKind::Class || !bt->decl)
        continue;
      Scope* bs = bt->decl->ownScope;
      if (!bs)
      {
        Entry e{bt->decl, nullptr};
        bs = scopeOfEntry(e, 0);
      }
      if (derivesFromInstance(bt, bs, base, depth + 1))
        return true;
    }
  return false;
}

auto GlobalScope::Impl::memberSubst(const Ty* t, Scope* foundIn, int depth) -> Subst
{
  Scope* cls = t && t->decl ? t->decl->ownScope : nullptr;
  if (cls && foundIn && foundIn != cls && foundIn->kind == ScopeKind::Class)
    if (const Ty* base = instantiationOfBase(t, cls, foundIn, depth + 1))
      return substFor(base);
  return substFor(t);
}

auto GlobalScope::Impl::baseSubstForUnqualified(const Lookup& l, const TypeContext& ctx, int depth) -> Subst
{
  if (l.found != Found::Member || !l.foundIn || l.foundIn->kind != ScopeKind::Class || depth > 16)
    return {};
  Scope* cls = ctx.scope;
  while (cls && cls->kind != ScopeKind::Class)
    cls = cls->parent;
  if (!cls || cls == l.foundIn || !cls->owner || cls->baseTypes.empty())
    return {};
  const Ty* self = selfType(cls->owner);
  if (self && ctx.subst)
    self = substitute(self, *ctx.subst);
  if (!self || self->kind != TyKind::Class)
    return {};
  const Ty* base = instantiationOfBase(self, cls, l.foundIn, depth + 1);
  if (!base || base == self || types.isDependent(base))
    return {};
  return substFor(base);
}

auto GlobalScope::Impl::qualifierSubst(const QualifiedName& name, const TypeContext& ctx, int depth, Scope* foundIn, const Ty** qualifierOut) -> Subst
{
  Subst s;
  if (name.typeQualifier && name.parts.size() == 1 && depth <= 96)
  {
    // `decltype(this->inventories)::mapped_type`: under the arguments of the class decltype gives.
    const Ty* q = convertType(name.typeQualifier, ctx, depth + 1);
    q = q ? types.unqualified(types.stripRef(q)) : nullptr;
    if (q && q->kind == TyKind::Enum && !types.isDependent(q))
      return substFor(q);
    return q && q->kind == TyKind::Class && !types.isDependent(q) ? memberSubst(q, foundIn, depth) : s;
  }
  if (name.parts.size() < 2 || depth > 96) // (called with constEval's depth, which nests calls)
    return s;
  // The qualifier as a type: `Vector<int>::`, or `VT::` with `using VT = Vector<int>`.
  QualifiedName prefix = name;
  prefix.parts.count -= 1;
  Lookup l = lookupQualified(prefix, ctx.scope, ctx.locals, depth + 1, ctx.subst, ctx.elementPacks, ctx.elementIndex);
  if (!l.decl)
    return s;
  if (l.decl->kind == NodeKind::TemplateParamDecl && ctx.subst)
  {
    // `T::value_type` where T is a template parameter: the substituted class carries the
    // arguments that resolve the member (`using value_type = T;` inside Vec<int> needs
    // Vec's T mapped to int).
    const Ty* sub = ctx.subst->find(l.decl);
    if (sub && sub->kind == TyKind::Class)
      return memberSubst(sub, foundIn, depth);
    if (sub && sub->kind == TyKind::Enum)
      return substFor(sub); // `E::values` with E = Flag<A>::Enum
    return s;
  }
  // (A `typedef Vector<Result> ResultList;` as the qualifier too, as a `using` alias.)
  if (l.decl->kind != NodeKind::ClassDecl && l.decl->kind != NodeKind::UsingAliasDecl && l.decl->kind != NodeKind::EnumDecl &&
      !(l.decl->kind == NodeKind::VariableDecl && (l.decl->flags & DF_Typedef)))
    return s;
  const Ty* qualifier = nullptr;
  if (prefix.parts.size() > 1 || l.found == Found::Member)
  {
    // `PrototypeType::IDType::IndexType`: the qualifier converted as the type it names, so that
    // an alias it ends in has the arguments of the base declaring it (ID<EntityPrototype, uint16>);
    // `IDType::IndexType` inside a class deriving from Prototype<CollisionLayerID> likewise.
    NamedType written{};
    written.kind = NodeKind::NamedType;
    written.name = prefix;
    qualifier = convertType(&written, ctx, depth + 1);
  }
  else
    qualifier = typeOfTypeDecl(l.decl, l, prefix.last(), ctx, depth + 1);
  if (qualifier && qualifier->kind == TyKind::Enum)
    return substFor(qualifier); // `Fast::Enum::True`: the instance's enumerator
  if (qualifierOut)
    *qualifierOut = qualifier;
  if (!qualifier || qualifier->kind != TyKind::Class)
    return s;
  return memberSubst(qualifier, foundIn, depth);
}

auto GlobalScope::Impl::functionArgument(const Lookup& l, Decl* param, int depth) -> const Ty*
{
  auto* p = param && param->kind == NodeKind::TemplateParamDecl ? static_cast<TemplateParamDecl*>(param) : nullptr;
  if (!p || p->isType || !p->valueType)
    return types.unknown();
  const Ty* pt = types.unqualified(convertType(p->valueType, TypeContext{lexicalScopeOf(param), nullptr, nullptr}, depth + 1));
  const Ty* wanted = pt && (pt->kind == TyKind::Pointer || pt->kind == TyKind::MemberPointer) ? pt->inner : pt;
  // `template<auto F>` given one function: a pointer to it, noexcept as it is.
  if (pt && pt->kind == TyKind::Auto && l.decl && l.decl->kind == NodeKind::FunctionDecl && (!l.overloads || l.overloads->size() <= 1) && l.decl->templateParams.empty())
    if (const Ty* t = declaredTypeOf(l.decl, nullptr, depth + 1); t && t->kind == TyKind::Function)
      return types.functionValue(types.pointer(namedFunctionType(l.decl, t, nullptr)), l.decl);
  if (!wanted || wanted->kind != TyKind::Function)
    return types.unknown();
  std::vector<Decl*> candidates;
  if (l.overloads)
    for (const Entry& e : *l.overloads)
      if (e.decl && e.decl->kind == NodeKind::FunctionDecl)
        candidates.push_back(e.decl);
  if (candidates.empty())
    candidates.push_back(l.decl);
  for (Decl* f : candidates)
    if (const Ty* t = namedFunctionType(f, declaredTypeOf(f, nullptr, depth + 1), nullptr); t == wanted || losesNoexcept(t, wanted))
      return types.functionValue(pt->kind == TyKind::MemberPointer ? pt : types.pointer(wanted), f);
  return types.unknown();
}

auto GlobalScope::Impl::templateArgument(Node* arg, Decl* param, const TypeContext& ctx, int depth) -> const Ty*
{
  if (!arg)
    return types.unknown();
  if (arg->kind == NodeKind::FunctionType && static_cast<FunctionType*>(arg)->asCall && param && param->kind == NodeKind::TemplateParamDecl &&
      !static_cast<TemplateParamDecl*>(param)->isType)
    arg = static_cast<FunctionType*>(arg)->asCall; // `Frac<gcd(N, M)>`: a call, for a constant parameter
  if (arg->kind == NodeKind::ArrayType && static_cast<ArrayType*>(arg)->asIndex && param && param->kind == NodeKind::TemplateParamDecl &&
      !static_cast<TemplateParamDecl*>(param)->isType)
    arg = static_cast<ArrayType*>(arg)->asIndex; // `Take<table[2]>`: an index, for a constant parameter
  std::optional<ConstValue> v;
  if (arg->kind == NodeKind::NamedType)
  {
    const QualifiedName& an = static_cast<NamedType*>(arg)->name;
    if (an.parts.size() == 1 && an.parts[0].hasTemplateArgs && isBoolTraitName(an.parts[0].symbol))
    {
      // `EnableIf<isIntegral<T>>`: a trait's value, once its arguments are known.
      std::vector<const Ty*> targs = traitArgs(an.parts[0], ctx, depth + 1);
      std::optional<bool> b = targs.empty() ? std::nullopt : traitValue(symbols.name(an.parts[0].symbol), targs);
      return b ? types.valueType(types.boolType(), *b ? 1 : 0) : types.unknown();
    }
    // `Array<char, bufferSize>`: a bare name is a type to the parser; it may be a constant.
    Lookup l = lookupQualified(an, ctx.scope, ctx.locals, depth + 1, ctx.subst, ctx.elementPacks, ctx.elementIndex);
    if (!l.decl && an.parts.size() == 1)
      if (Decl* r = static_cast<NamedType*>(arg)->resolved; r && r->kind == NodeKind::VariableDecl && r->parent && r->parent->kind == NodeKind::FunctionDecl)
        l.decl = r; // `Flag<sign>` with a local constant, where the context has no locals: what the walk found
    if (l.decl && l.decl->kind == NodeKind::FunctionDecl)
      return functionArgument(l, param, depth); // `BinaryOperation<Add, Functions::add>`
    bool constant = l.decl && ((l.decl->kind == NodeKind::VariableDecl && !(l.decl->flags & DF_Typedef)) || l.decl->kind == NodeKind::EnumeratorDecl ||
                               (l.decl->kind == NodeKind::TemplateParamDecl && !static_cast<TemplateParamDecl*>(l.decl)->isType));
    if (constant && l.decl->kind == NodeKind::TemplateParamDecl && ctx.subst)
      if (const Ty* bound = ctx.subst->find(l.decl); bound && bound->kind == TyKind::Value && bound->decl)
        return bound; // a function passed on (`Op<func>` inside a template given one)
    if (constant)
    {
      Subst qs; // `Array<int, Bits<int16_t>::valueBits>`: the member under the instantiation's arguments
      if (an.parts.size() > 1 && (l.decl->kind == NodeKind::VariableDecl || l.decl->kind == NodeKind::EnumeratorDecl))
        qs = qualifierSubst(an, ctx, depth + 1, l.foundIn);
      v = constValueOfDecl(l.decl, ctx, depth, qs.pairs.empty() ? nullptr : &qs);
      if (!v && l.decl->kind == NodeKind::TemplateParamDecl)
        return types.param(l.decl); // `Num<I>` in a template: the argument is still the parameter (dependent)
    }
    else if (l.decl && l.decl->kind == NodeKind::ClassDecl && !l.decl->templateParams.empty() && !an.last().hasTemplateArgs && param &&
             param->kind == NodeKind::TemplateParamDecl && !static_cast<TemplateParamDecl*>(param)->innerParams.empty())
      // `IsSpec<T, Box>`: a class template given to a template template parameter, the template
      // itself (its class without arguments, as deduction binds such a parameter).
      return types.classType(l.scope && l.scope->owner ? l.scope->owner : l.decl, {});
    else
      return convertType(static_cast<Type*>(arg), ctx, depth); // (a written type's parts keep the depth: convertType)
  }
  else if (uint8_t(arg->kind) <= uint8_t(NodeKind::PackType))
    return convertType(static_cast<Type*>(arg), ctx, depth);
  else if (arg->kind == NodeKind::UnaryExpr && !static_cast<UnaryExpr*>(arg)->isKeyword && P(static_cast<UnaryExpr*>(arg)->op) == P::Amp &&
           static_cast<UnaryExpr*>(arg)->operand && static_cast<UnaryExpr*>(arg)->operand->kind == NodeKind::NameExpr)
  {
    // `BinaryOperation<Mul, &Functions::mul>`: the address of a function.
    Lookup l = lookupQualified(static_cast<NameExpr*>(static_cast<UnaryExpr*>(arg)->operand)->name, ctx.scope, ctx.locals, depth + 1, ctx.subst, ctx.elementPacks, ctx.elementIndex);
    // The class a member's qualifier names, of a class template's member: the declaration is every
    // instance's. Null when the member's class is no template, unknown when it says none.
    auto instanceNamed = [&]() -> const Ty*
    {
      if (!l.decl || !l.decl->parent || l.decl->parent->kind != NodeKind::ClassDecl || effectiveParams(l.decl->parent).empty())
        return nullptr;
      const QualifiedName& written = static_cast<NameExpr*>(static_cast<UnaryExpr*>(arg)->operand)->name;
      if (written.parts.size() < 2)
        return types.unknown();
      NamedType qualifier{};
      qualifier.kind = NodeKind::NamedType;
      qualifier.token = arg->token;
      qualifier.name = written;
      qualifier.name.parts.count -= 1;
      const Ty* instance = convertType(&qualifier, ctx, depth + 1);
      return instance && instance->kind == TyKind::Class && instance->isKnown() && !types.isDependent(instance) ? instance : types.unknown();
    };
    if (l.decl && l.decl->kind == NodeKind::FunctionDecl && !(l.decl->flags & DF_Static))
      if (const Ty* instance = instanceNamed())
      {
        // `&Holder<int>::get`: of the name's functions under this instance, the one of the parameter's
        // type; the only one where that type depends on what is not bound here.
        if (!instance->isKnown())
          return types.unknown();
        auto* p = param && param->kind == NodeKind::TemplateParamDecl ? static_cast<TemplateParamDecl*>(param) : nullptr;
        const Ty* of = p && !p->isType && p->valueType ? types.unqualified(convertType(p->valueType, TypeContext{lexicalScopeOf(param), nullptr, nullptr}, depth + 1)) : nullptr;
        const Ty* wanted = of && of->kind == TyKind::MemberPointer && !types.isDependent(of) && !mentionsUnknown(of) ? of->inner : nullptr;
        Subst s = substFor(instance);
        std::vector<Decl*> candidates;
        if (l.overloads)
          for (const Entry& e : *l.overloads)
            if (e.decl && e.decl->kind == NodeKind::FunctionDecl && e.decl->templateParams.empty())
              candidates.push_back(e.decl);
        if (candidates.empty())
          candidates.push_back(l.decl);
        if (!wanted && candidates.size() != 1)
          return types.unknown();
        for (Decl* f : candidates)
          if (const Ty* t = namedFunctionType(f, declaredTypeOf(f, &s, depth + 1), &s); !wanted || t == wanted || losesNoexcept(t, wanted))
            return types.functionValue(types.memberPointer(wanted ? wanted : t, instance), f);
        return types.unknown();
      }
    if (l.decl && l.decl->kind == NodeKind::FunctionDecl)
      return functionArgument(l, param, depth);
    // `readField<&Pos::x>` for `template<int Pos::*Field>`: a data member of a class, the pointer
    // to it a value of the member pointer type that holds the member's declaration, as a function's
    // value holds the function (D, 2026-11-06: no argument was made, and the instance could not be
    // emitted). Of a class template's member the qualifier says the instance (`&Holder<int>::a`),
    // which the value's type holds, so that two instances are two arguments.
    if (l.decl && l.decl->kind == NodeKind::VariableDecl && !(l.decl->flags & (DF_Static | DF_Typedef)) && l.decl->parent &&
        l.decl->parent->kind == NodeKind::ClassDecl)
    {
      const Ty* instance = instanceNamed();
      if (instance && !instance->isKnown())
        return types.unknown();
      const Ty* of = nullptr;
      if (param && param->kind == NodeKind::TemplateParamDecl)
        if (auto* p = static_cast<TemplateParamDecl*>(param); !p->isType && p->valueType)
          of = convertType(p->valueType, TypeContext{lexicalScopeOf(param), nullptr, nullptr}, depth + 1);
      const Ty* u = of ? types.unqualified(types.stripRef(of)) : nullptr;
      if (!u || u->kind != TyKind::MemberPointer || types.isDependent(u) || mentionsUnknown(u)) // (`int C::*F` beside `class C`: the field's own)
      {
        Subst of = instance ? substFor(instance) : Subst{};
        const Ty* member = declaredTypeOf(l.decl, of.pairs.empty() ? nullptr : &of, depth + 1);
        u = member && member->isKnown() ? types.memberPointer(member, instance ? instance : selfType(l.decl->parent)) : nullptr;
      }
      else if (instance && (u->args.empty() || !u->args[0] || u->args[0] != instance))
        return types.unknown(); // (of another class: a conversion no argument has here)
      return u && u->isKnown() && !types.isDependent(u) ? types.functionValue(u, l.decl) : types.unknown();
    }
    v = constValue(static_cast<Expr*>(arg), ctx, depth + 1);
  }
  else
    v = constValue(static_cast<Expr*>(arg), ctx, depth + 1);
  if (v && v->isObject() && v->type && types.unqualified(v->type)->kind == TyKind::Class)
  {
    // `scaled<Fixed{3, 14}>()`, `scaled<twoTen>()` for `template<Fixed F>`: the object's value, of
    // the parameter's class (C++20 [temp.param] 6); any other parameter takes no object.
    const Ty* of = nullptr;
    if (param && param->kind == NodeKind::TemplateParamDecl)
      if (auto* p = static_cast<TemplateParamDecl*>(param); !p->isType && p->valueType)
        of = convertType(p->valueType, TypeContext{lexicalScopeOf(param), nullptr, nullptr}, depth + 1);
    const Ty* u = of ? types.unqualified(types.stripRef(of)) : nullptr;
    if (u && u->kind != TyKind::Auto && u != types.unqualified(v->type))
      return types.unknown();
    const Ty* object = objectArgument(*v);
    return object ? object : types.unknown();
  }
  if (!v)
    return types.unknown();
  if (v->isFloat())
  {
    // `twice<1.25>()`, `Scaled<0.5f>`, `sizeOfAuto<2.5>()` (C++20, [temp.arg.nontype]): converted to the parameter's
    // floating type, an auto parameter's the value's own, and kept by its bits (TypeTable::floatBits). Until 2026-11-08
    // no argument, codegen's "a template parameter whose value is not known here".
    const Ty* of = nullptr;
    if (param && param->kind == NodeKind::TemplateParamDecl)
      if (auto* p = static_cast<TemplateParamDecl*>(param); !p->isType && p->valueType)
        of = convertType(p->valueType, TypeContext{lexicalScopeOf(param), nullptr, nullptr}, depth + 1);
    const Ty* u = of ? types.unqualified(types.stripRef(of)) : nullptr;
    if (!u || !u->isKnown() || u->kind == TyKind::Auto)
      u = v->type ? types.unqualified(types.stripRef(v->type)) : nullptr;
    if (!u || u->kind != TyKind::Float)
      return types.unknown(); // (a floating value for an integer parameter: no converted constant expression gives it)
    return types.valueType(u, TypeTable::floatBits(v->f, u->bits));
  }
  if (v->isAddress())
  {
    // `Holder<&table>`, a function pointer a constant holds: the declaration's address, as an
    // argument naming the function makes it (functionArgument); a part of a variable, or a member
    // of a class template's instance, is no argument it can hold yet.
    const ConstTarget& t = *v->target;
    if (!t.path.empty() || t.owner || !t.args.empty() || !v->type)
      return types.unknown();
    return types.functionValue(types.unqualified(v->type), t.decl);
  }
  if (v->kind != ConstValue::Kind::Int)
    return types.unknown(); // an object, a pointer into an array the evaluation made: never a number
  // Converted to the parameter's type (`Bits<300>` for a uint8_t parameter is 44); for an auto
  // parameter, or none in sight, the value's own type.
  const Ty* of = nullptr;
  if (param && param->kind == NodeKind::TemplateParamDecl)
    if (auto* p = static_cast<TemplateParamDecl*>(param); !p->isType && p->valueType)
      of = convertType(p->valueType, TypeContext{lexicalScopeOf(param), nullptr, nullptr}, depth + 1);
  const Ty* u = of ? types.unqualified(types.stripRef(of)) : nullptr;
  if (u && (u->isArithmetic() || u->kind == TyKind::Enum))
    v = constArith().convert(*v, u);
  else if (!u || !u->isKnown() || u->kind == TyKind::Auto)
    of = v->type && (v->type->isArithmetic() || v->type->kind == TyKind::Enum || v->type->kind == TyKind::Nullptr) ? v->type : types.intType(); // (`N<nullptr>` is no N<0>: until 2026-11-07 an int 0, the same instance)
  if (!v)
    return types.unknown();
  return types.valueType(of, v->i);
}

auto GlobalScope::Impl::objectArgument(const ConstValue& v, int depth) -> const Ty*
{
  if (!v.isObject() || !v.type || !v.parts || depth > 32)
    return nullptr;
  std::vector<const Ty*> parts;
  for (const ConstValue& p : *v.parts)
  {
    const Ty* pt = p.type ? types.unqualified(p.type) : nullptr;
    if (p.isObject())
    {
      const Ty* inner = objectArgument(p, depth + 1);
      if (!inner)
        return nullptr;
      parts.push_back(inner);
    }
    else if (p.kind == ConstValue::Kind::Int && pt && (pt->isIntegral() || pt->kind == TyKind::Enum))
      parts.push_back(types.valueType(pt, p.i));
    else if (p.isUnset() && pt && (pt->isIntegral() || pt->kind == TyKind::Enum))
      parts.push_back(types.valueType(pt, 0));
    else
      return nullptr;
  }
  return types.objectValue(types.unqualified(v.type), Span<const Ty*>{parts.data(), uint32_t(parts.size())});
}

auto GlobalScope::Impl::argumentObject(const Ty* t, int depth) -> std::optional<ConstValue>
{
  if (!t || !t->isObjectValue() || depth > 32)
    return std::nullopt;
  std::vector<ConstValue> parts;
  for (const Ty* p : t->args)
  {
    if (p && p->isObjectValue())
    {
      std::optional<ConstValue> inner = argumentObject(p, depth + 1);
      if (!inner)
        return std::nullopt;
      parts.push_back(*inner);
    }
    else if (p && p->kind == TyKind::Value && !p->decl)
      parts.push_back(ConstValue::integer(p->value, p->inner));
    else
      return std::nullopt;
  }
  return ConstValue::object(t->inner, std::move(parts));
}

auto GlobalScope::Impl::functionSymbolId() -> uint32_t
{
  if (functionSymbol_ == UINT32_MAX)
    functionSymbol_ = symbols.intern("Function");
  return functionSymbol_;
}

auto GlobalScope::Impl::functionImplSymbolId() -> uint32_t
{
  if (functionImplSymbol_ == UINT32_MAX)
    functionImplSymbol_ = symbols.intern("FunctionImpl");
  return functionImplSymbol_;
}

auto GlobalScope::Impl::isLibraryFunction(const Decl* d, bool impl) -> bool
{
  if (!d || d->kind != NodeKind::ClassDecl || static_cast<const ClassDecl*>(d)->name.empty() ||
      static_cast<const ClassDecl*>(d)->name.last().symbol != (impl ? functionImplSymbolId() : functionSymbolId()))
    return false;
  Lookup l;
  return lookupInScope(&root, impl ? functionImplSymbolId() : functionSymbolId(), l, 0) && (l.decl == d || (l.scope && l.scope->owner == d));
}

auto GlobalScope::Impl::functionOfSignature(const Ty* signature) -> const Ty*
{
  if (!signature || signature->kind != TyKind::Function)
    return nullptr;
  Lookup implLookup;
  if (!lookupInScope(&root, functionImplSymbolId(), implLookup, 0) || !implLookup.scope || !implLookup.scope->owner)
    return nullptr;
  std::vector<const Ty*> implArgs{signature->inner};
  for (const Ty* a : signature->args)
    implArgs.push_back(a);
  return types.classType(implLookup.scope->owner, Span<const Ty*>{implArgs.data(), uint32_t(implArgs.size())});
}

auto GlobalScope::Impl::initializerListDecl() -> Decl*
{
  if (initializerListSymbol_ == UINT32_MAX)
    initializerListSymbol_ = symbols.intern("InitializerList");
  Lookup l;
  if (lookupInScope(&root, initializerListSymbol_, l, 0) && l.scope && l.scope->owner)
    return l.scope->owner;
  return nullptr;
}

auto GlobalScope::Impl::isInitializerList(const Ty* t) -> bool
{
  return t && t->kind == TyKind::Class && t->decl && t->decl == initializerListDecl();
}

auto GlobalScope::Impl::textDecl() -> Decl*
{
  if (textSymbol_ == UINT32_MAX)
    textSymbol_ = symbols.intern("Text");
  Lookup l;
  if (lookupInScope(&root, textSymbol_, l, 0) && l.scope && l.scope->owner && l.scope->owner->kind == NodeKind::ClassDecl)
    return l.scope->owner;
  return nullptr;
}

auto GlobalScope::Impl::isText(const Ty* t) -> bool
{
  t = t ? types.unqualified(types.stripRef(t)) : nullptr;
  return t && t->kind == TyKind::Class && t->decl && t->decl == textDecl();
}

auto GlobalScope::Impl::intrinsicOf(const FunctionDecl* f) -> int
{
  if (!f || f->body || !f->file || !f->file->isLibrary || f->name.empty() || f->name.last().symbol == UINT32_MAX)
    return 0;
  std::string_view n = symbols.name(f->name.last().symbol);
  return n == "kppEnumeratorName" ? 1 : n == "kppEnumTypeName" ? 2 : 0;
}

auto GlobalScope::Impl::callIntrinsicOf(const FunctionDecl* f) -> CallIntrinsic
{
  if (!f || !f->file || !f->file->isLibrary || f->templateParams.size() != 1 || f->params.size() != 1)
    return CallIntrinsic::None;
  std::call_once(callIntrinsicsOnce, [this]
  {
    auto isReference = [](const Type* t) { return t && (t->kind == NodeKind::LRefType || t->kind == NodeKind::RRefType); };
    // (the root scope's entries as indexed, read directly: no walk's lookup, whose dependencies this would join)
    for (auto [name, which] : {std::pair<const char*, CallIntrinsic>{"move", CallIntrinsic::Move}, {"forward", CallIntrinsic::Forward},
                               {"asConst", CallIntrinsic::AsConst}})
      if (const std::vector<Entry>* entries = root.find(symbols.intern(name)))
        for (const Entry& e : *entries)
        {
          if (!e.decl || e.decl->kind != NodeKind::FunctionDecl)
            continue;
          auto* d = static_cast<const FunctionDecl*>(e.decl);
          if (!d->file || !d->file->isLibrary || d->templateParams.size() != 1 || d->params.size() != 1 || d->trailingReturn ||
              d->templateParams[0]->kind != NodeKind::TemplateParamDecl)
            continue;
          auto* parameter = static_cast<const TemplateParamDecl*>(d->templateParams[0]);
          if (parameter->isType && !parameter->isPack && d->params[0]->kind == NodeKind::ParamDecl &&
              isReference(static_cast<const ParamDecl*>(d->params[0])->type) && isReference(d->returnType))
            callIntrinsics.push_back({d, which});
        }
  });
  for (const auto& [d, which] : callIntrinsics)
    if (d == f)
      return which;
  return CallIntrinsic::None;
}

auto GlobalScope::Impl::constIntrinsic(int which, FunctionDecl* f, const std::vector<ConstValue>& args, const Subst* subst, int depth) -> std::optional<ConstValue>
{
  const Ty* charPointer = types.pointer(types.qualified(types.charType(8, false), true, false));
  auto text = [&](std::string_view s)
  {
    std::vector<ConstValue> chars;
    for (char c : s)
      chars.push_back(ConstValue::integer(int64_t(static_cast<signed char>(c)), types.charType(8, false)));
    chars.push_back(ConstValue::integer(0, types.charType(8, false)));
    return ConstValue::pointer(charPointer, std::make_shared<const std::vector<ConstValue>>(std::move(chars)), 0);
  };
  if (which == 2)
  {
    const Ty* e = subst && !f->templateParams.empty() ? subst->find(f->templateParams[0]) : nullptr;
    if (!e || !e->isKnown() || types.isDependent(e))
      return std::nullopt;
    return text(types.toString(types.unqualified(e), &symbols));
  }
  if (args.size() != 1 || args[0].kind != ConstValue::Kind::Int || !args[0].type)
    return std::nullopt;
  const Ty* e = types.unqualified(types.stripRef(args[0].type));
  if (e->kind != TyKind::Enum || !e->decl || e->decl->kind != NodeKind::EnumDecl)
    return std::nullopt;
  for (Decl* d : static_cast<EnumDecl*>(e->decl)->enumerators)
    if (d->kind == NodeKind::EnumeratorDecl)
      if (std::optional<ConstValue> v = constValueOfDecl(d, TypeContext{lexicalScopeOf(e->decl), nullptr, nullptr}, depth + 1);
          v && v->kind == ConstValue::Kind::Int && v->i == args[0].i)
        return text(symbols.name(static_cast<EnumeratorDecl*>(d)->name));
  return ConstValue::integer(0, charPointer);
}

auto GlobalScope::Impl::markTextClosure(const Decl* closure) -> void
{
  std::lock_guard lock(textClosuresMutex);
  textClosures.insert(closure);
}

auto GlobalScope::Impl::isTextClosure(const Ty* t) -> bool
{
  t = t ? types.unqualified(types.stripRef(t)) : nullptr;
  if (!t || t->kind != TyKind::Lambda || !t->decl)
    return false;
  std::lock_guard lock(textClosuresMutex);
  return textClosures.count(t->decl) != 0;
}

auto GlobalScope::Impl::holdsText(const Ty* t, int depth) -> bool
{
  if (!t || depth > 16 || !textDecl())
    return false;
  t = types.unqualified(types.stripRef(t));
  switch (t->kind)
  {
    case TyKind::Pointer: case TyKind::Array: return holdsText(t->inner, depth + 1);
    case TyKind::Lambda: return isTextClosure(t);
    case TyKind::Class:
      if (isText(t))
        return true;
      for (const Ty* a : t->args)
        if (holdsText(a, depth + 1))
          return true;
      return false;
    default: return false;
  }
}

auto GlobalScope::Impl::namesTextLast(FunctionDecl* f) -> bool
{
  if (f->params.empty())
    return false;
  Type* t = static_cast<ParamDecl*>(f->params[f->params.size() - 1])->type;
  while (t && (t->kind == NodeKind::LRefType || t->kind == NodeKind::ConstType))
    t = t->kind == NodeKind::LRefType ? static_cast<LRefType*>(t)->referenced : static_cast<ConstType*>(t)->inner;
  if (!t || t->kind != NodeKind::NamedType || static_cast<NamedType*>(t)->name.empty())
    return false;
  if (textSymbol_ == UINT32_MAX)
    textSymbol_ = symbols.intern("Text");
  return static_cast<NamedType*>(t)->name.last().symbol == textSymbol_;
}

auto GlobalScope::Impl::textParameterOf(const Ty* ft) -> size_t
{
  if (!ft || ft->kind != TyKind::Function || ft->args.empty() || ft->variadic || !isText(ft->args[ft->args.size() - 1]))
    return SIZE_MAX;
  return ft->args.size() - 1;
}

auto GlobalScope::Impl::libraryDecl(std::string_view name, NodeKind kind) -> Decl*
{
  Lookup l;
  if (!lookupInScope(&root, symbols.intern(name), l, 0))
    return nullptr;
  if (kind == NodeKind::ClassDecl)
    return l.scope && l.scope->owner && l.scope->owner->kind == NodeKind::ClassDecl ? l.scope->owner : nullptr;
  return l.decl && l.decl->kind == kind ? l.decl : nullptr;
}

auto GlobalScope::Impl::textType() -> const Ty*
{
  Decl* d = textDecl();
  return d ? selfType(d) : nullptr;
}

auto GlobalScope::Impl::textPieceOf(const Expr* e, const Ty* given) -> TextPieceKind
{
  TextPieceKind out;
  const Ty* t = given ? types.unqualified(types.stripRef(given)) : nullptr;
  if (!t || !t->isKnown() || types.isDependent(t) || mentionsUnknown(t))
    return out;
  auto thunk = [&](std::string_view name)
  {
    out.thunk = static_cast<FunctionDecl*>(libraryDecl(name, NodeKind::FunctionDecl));
    if (!out.thunk)
      out.error = "the standard library has no " + std::string(name) + " for a piece of a Text (Text.kpp, design.md 14)";
    return out;
  };
  std::string name = types.toString(t, &symbols);
  auto isNarrowChar = [&](const Ty* c) { return c && types.unqualified(c)->kind == TyKind::Char && types.unqualified(c)->bits == 8; };
  // A string literal or an array of chars: C's text, to its first zero (as C++'s `out << text`).
  if ((e && stringLiteralArray(e, given, types)) || (t->kind == TyKind::Array && isNarrowChar(t->inner)))
    return thunk("appendCTextPiece");
  if (t->kind == TyKind::Pointer || t->kind == TyKind::Nullptr)
  {
    out.error = t->kind == TyKind::Pointer && isNarrowChar(t->inner)
                  ? "a pointer (" + name + ") is no piece of a Text: write StringView(p) for C text or address(p) for its address (design.md 14)"
                  : "a pointer (" + name + ") is no piece of a Text: write address(p) for its address (design.md 14)";
    return out;
  }
  if (t->kind == TyKind::Array || t->kind == TyKind::Function || t->kind == TyKind::MemberPointer || t->kind == TyKind::Void)
  {
    out.error = name + " is no piece of a Text (design.md 14)";
    return out;
  }
  const Ty* string = nullptr;
  if (Decl* d = libraryDecl("String", NodeKind::ClassDecl))
    string = selfType(d);
  uint32_t appendTo = symbols.intern("appendTo");
  if (string && t->kind == TyKind::Class)
  {
    // Its own `void appendTo(String& out) const`, or a base's.
    Subst subst;
    std::vector<FunctionDecl*> members = constMembersNamed(t, appendTo, subst, 0);
    std::vector<FunctionDecl*> constOnes;
    for (FunctionDecl* f : members)
      if ((f->flags & DF_Const) && !(f->flags & DF_Static))
        constOnes.push_back(f);
    bool ambiguous = false;
    if (!constOnes.empty() && chooseAmong(constOnes, {types.lref(string)}, subst.empty() ? nullptr : &subst, types.lref(types.qualified(t, true, false)),
                                          ambiguous) && !ambiguous)
    {
      out.type = t;
      return thunk("appendMemberPiece");
    }
  }
  if (string && t->kind != TyKind::Class)
  {
    // A built-in type's or an enumeration's appendTo, which the standard library writes as a
    // free `void appendTo(T value, String& out)` (design.md 14: the member of every type, the
    // program writing none of its own), taking the type itself: no promotion or conversion
    // reaches one (an unscoped enumeration prints as itself, never as the integer it converts to).
    FunctionDecl* f = constFreeFunction(appendTo, {types.lref(types.qualified(t, true, false)), types.lref(string)}, &root, true);
    const Ty* ft = f && f->file && f->file->isLibrary ? declaredTypeOf(f, nullptr, 0) : nullptr;
    const Ty* takes = ft && ft->kind == TyKind::Function && ft->args.size() == 2 ? types.unqualified(types.stripRef(ft->args[0])) : nullptr;
    if (takes && (!f->templateParams.empty() || takes == t))
    {
      out.type = t;
      return thunk("appendFreePiece");
    }
  }
  out.error = name + " has no appendTo, so it cannot be a piece of a Text (design.md 14)";
  return out;
}

auto GlobalScope::Impl::enumFromStrDecl() -> Decl*
{
  if (enumFromStrSymbol_ == UINT32_MAX)
    enumFromStrSymbol_ = symbols.intern("enumFromStr");
  Lookup l;
  if (lookupInScope(&root, enumFromStrSymbol_, l, 0) && l.decl && l.decl->kind == NodeKind::FunctionDecl)
    return l.decl;
  return nullptr;
}

auto GlobalScope::Impl::optionalDecl() -> Decl*
{
  if (optionalSymbol_ == UINT32_MAX)
    optionalSymbol_ = symbols.intern("Optional");
  Lookup l;
  if (lookupInScope(&root, optionalSymbol_, l, 0) && l.decl && l.decl->kind == NodeKind::ClassDecl && !l.decl->templateParams.empty())
    return l.decl;
  return nullptr;
}

auto GlobalScope::Impl::strongOrderingDecl() -> Decl*
{
  if (strongOrderingSymbol_ == UINT32_MAX)
    strongOrderingSymbol_ = symbols.intern("strong_ordering");
  Lookup l;
  if (lookupInScope(&root, strongOrderingSymbol_, l, 0) && l.scope && l.scope->owner)
    return l.scope->owner;
  return nullptr;
}

auto GlobalScope::Impl::orderingType() -> const Ty*
{
  Decl* d = strongOrderingDecl();
  return d ? types.classType(d, {}) : types.intType();
}

auto GlobalScope::Impl::typeInfoDecl() -> Decl*
{
  if (typeInfoSymbol_ == UINT32_MAX)
    typeInfoSymbol_ = symbols.intern("TypeInfo");
  Lookup l;
  if (lookupInScope(&root, typeInfoSymbol_, l, 0) && l.scope && l.scope->owner)
    return l.scope->owner;
  return nullptr;
}

auto GlobalScope::Impl::isFunctionImpl(const Ty* t) -> bool
{
  if (!t || t->kind != TyKind::Class || !t->decl || t->decl->kind != NodeKind::ClassDecl)
    return false;
  const QualifiedName& n = static_cast<ClassDecl*>(t->decl)->name;
  return !n.empty() && n.last().symbol == functionImplSymbolId();
}

auto GlobalScope::Impl::callableWith(const Ty* a, const Ty* impl, int depth) -> bool
{
  a = types.decay(a);
  const Ty* fn = nullptr;
  if (a->kind == TyKind::Lambda || a->kind == TyKind::Function)
    fn = a;
  else if (a->kind == TyKind::Pointer && a->inner->kind == TyKind::Function)
    fn = a->inner;
  else
    return true;
  if (impl->args.empty())
    return true;
  size_t n = impl->args.size() - 1;
  if (fn->args.size() != n)
    return false;
  for (size_t i = 0; i < n; ++i)
    if (conversionRank(impl->args[i + 1], fn->args[i], depth + 1) >= NotViable)
      return false;
  return true;
}

auto GlobalScope::Impl::isUndeducedClassTemplate(const Ty* t) -> bool
{
  if (!t || t->kind != TyKind::Class || !t->decl)
    return false;
  Span<Decl*> params = t->decl->templateParams;
  if (params.empty() || t->args.size() != params.size())
    return false;
  for (size_t i = 0; i < params.size(); ++i)
    if (t->args[i] != types.param(params[i]))
      return false;
  return true;
}

auto GlobalScope::Impl::takesArgumentCount(const FunctionDecl* f, size_t count) -> bool
{
  size_t required = 0;
  bool pack = false;
  for (Decl* p : f->params)
  {
    auto* pd = static_cast<ParamDecl*>(p);
    if (pd->isPack)
      pack = true;
    else if (!pd->defaultValue)
      ++required;
  }
  return count >= required && (pack || count <= f->params.size());
}

auto GlobalScope::Impl::deduceFromGuides(ClassDecl* cls, const std::vector<const Ty*>& argTypes, bool copyInit, std::vector<int>& ranks) -> const Ty*
{
  auto it = guidesOf.find(cls);
  if (it == guidesOf.end())
    return nullptr;
  const Ty* best = nullptr;
  FunctionDecl* bestGuide = nullptr;
  for (FunctionDecl* g : it->second)
  {
    if ((copyInit && (g->flags & DF_Explicit)) || !takesArgumentCount(g, argTypes.size()))
      continue;
    const Ty* ft = declaredTypeOf(g, nullptr, 0);
    if (!ft || ft->kind != TyKind::Function)
      continue;
    Subst d;
    deduceCall(g, ft, argTypes, d);
    bool all = true;
    for (Decl* p : g->templateParams)
      all = all && d.find(p);
    if (!all)
      continue;
    // Its constraints (`template<Small U> G(U) -> G<Box<U>>;`, a requires clause): a guide whose
    // constraints do not hold is no candidate ([over.match.class.deduct] 1 as [over.match.viable] 3);
    // until 2026-11-08 they were never checked, G(2.5) took it.
    if (!constraintsHold(g, d))
      continue;
    const Ty* inst = substitute(ft, d);
    const Ty* result = inst ? types.unqualified(inst->inner) : nullptr;
    // (The class's arguments may pick a specialization: `Box<M...>` with one M is `Box<M>`'s instance.)
    if (!result || result->kind != TyKind::Class || !result->isKnown() || types.isDependent(result) ||
        (result->decl != cls && primaryArgumentsOf(result, cls, 0).empty()))
      continue;
    std::vector<int> r;
    for (size_t i = 0; i < argTypes.size() && i < inst->args.size(); ++i)
      r.push_back(conversionRank(argTypes[i], inst->args[i]));
    if (std::any_of(r.begin(), r.end(), [](int x) { return x >= NotViable; }))
      continue;
    // Of two guides that match alike, the more specialized ([over.match.best] 2.5):
    // `ScopedLock(AdoptLock, M&...)` over `ScopedLock(M&...)` given adoptLock and two mutexes
    // (the first declared won, which the second took only while `M&...` decayed the const
    // AdoptLock and could not bind it).
    // Of two alike either way, the one whose constraints subsume the other's ([over.match.best] 2.6):
    // `template<Small U> G(U)` over `template<class U> G(U)` given a short.
    auto beats = [&](FunctionDecl* a, FunctionDecl* b)
    {
      if (moreSpecialized(a, b, argTypes.size(), nullptr))
        return true;
      return !moreSpecialized(b, a, argTypes.size(), nullptr) && constraintsSubsume(a, b) && !constraintsSubsume(b, a);
    };
    if (best && !strictlyBetter(r, ranks) && (strictlyBetter(ranks, r) || !beats(g, bestGuide)))
      continue;
    best = result;
    bestGuide = g;
    ranks = std::move(r);
  }
  return best;
}

auto GlobalScope::Impl::classConstraintsHold(ClassDecl* cls, const Subst& d) -> bool
{
  for (Decl* p : cls->templateParams)
    if (std::optional<bool> holds = typeConstraintHolds(static_cast<TemplateParamDecl*>(p), cls, d, 1); holds && !*holds)
      return false;
  if (!cls->requiresClause)
    return true;
  std::optional<bool> holds = constraintValue(cls->requiresClause, cls, d);
  return !holds || *holds;
}

auto GlobalScope::Impl::strictlyBetter(const std::vector<int>& a, const std::vector<int>& b) -> bool
{
  bool better = false;
  for (size_t i = 0; i < a.size() && i < b.size(); ++i)
  {
    if (a[i] > b[i])
      return false;
    better = better || a[i] < b[i];
  }
  return better;
}

auto GlobalScope::Impl::deduceClassArgs(const Ty* primary, const std::vector<const Ty*>& argTypes, bool copyInit) -> const Ty*
{
  Scope* s = classScopeOf(primary);
  if (!s || !s->owner || s->owner->kind != NodeKind::ClassDecl)
    return nullptr;
  auto* cls = static_cast<ClassDecl*>(s->owner);
  // `Pairish p = Pairish<int, double>(1, 2.5);`, `Duration budget = Milliseconds(5);`: a value of
  // an instantiation of the same template gives its arguments (C++'s copy deduction candidate).
  if (argTypes.size() == 1 && argTypes[0])
    if (const Ty* a = types.unqualified(types.stripRef(argTypes[0])); a && a->kind == TyKind::Class && ((a->decl == cls && !a->args.empty()) || !primaryArgumentsOf(a, cls, 0).empty()))
      return a;
  std::vector<int> guideRanks;
  const Ty* fromGuide = deduceFromGuides(cls, argTypes, copyInit, guideRanks);
  // The class's arguments from what d deduced: what it does not say takes its default, a pack is empty.
  auto argumentsFrom = [&](Subst& d, std::vector<const Ty*>& args)
  {
    LocalStack own; // the parameters by name, for defaults that name earlier ones
    for (Decl* p : cls->templateParams)
      own.push(static_cast<TemplateParamDecl*>(p)->name, p);
    bool all = true;
    for (Decl* p : cls->templateParams)
    {
      const Ty* t = d.find(p);
      if (!t && static_cast<TemplateParamDecl*>(p)->isPack)
      {
        t = types.pack({}); // (`NP p;` of `template<typename... T> struct NP`)
        d.pairs.push_back({p, t});
      }
      if (!t && static_cast<TemplateParamDecl*>(p)->defaultArg)
      {
        // `Tuple(mask, gap, id)` of `template<A, B = Nothing, C = Nothing, D = Nothing>`: what the
        // constructor does not deduce takes its default.
        TypeContext dctx{lexicalScopeOf(cls), &own, &d, cls->file ? cls->file->tokens : nullptr, cls->file ? cls->file->text : std::string_view()};
        t = templateArgument(static_cast<TemplateParamDecl*>(p)->defaultArg, p, dctx, 0);
        if (t && t->isKnown())
          d.pairs.push_back({p, t});
        else
          t = nullptr;
      }
      if (!t)
        all = false;
      else if (!spliceIfPack(t, args)) // `Box(M&... values)`: Box<int, double>, its elements
        args.push_back(t);
    }
    return all;
  };
  bool hasConstructor = false;
  for (Decl* m : membersRead(cls))
  {
    if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Constructor))
      continue;
    hasConstructor = true;
    auto* ctor = static_cast<FunctionDecl*>(m);
    size_t required = 0;
    bool pack = false;
    for (Decl* p : ctor->params)
    {
      auto* pd = static_cast<ParamDecl*>(p);
      if (pd->isPack)
        pack = true;
      else if (!pd->defaultValue)
        ++required;
    }
    if (argTypes.size() < required || (!pack && argTypes.size() > ctor->params.size()))
      continue;
    const Ty* ft = declaredTypeOf(ctor, nullptr, 0);
    if (!ft || ft->kind != TyKind::Function)
      continue;
    Subst d;
    deduceCall(ctor, ft, argTypes, d);
    // A constructor template whose own parameters the arguments leave undeduced (and without
    // a default) failed deduction: no candidate. `template<class PrototypeType> Set(StringView,
    // ValueT PrototypeType::*, ValueT)` given a Cast would read ValueT off the last argument.
    bool undeduced = false;
    for (Decl* tp : ctor->templateParams)
    {
      auto* param = static_cast<TemplateParamDecl*>(tp);
      undeduced = undeduced || (!d.find(tp) && !param->defaultArg && !param->isPack);
    }
    if (undeduced)
      continue;
    std::vector<const Ty*> args;
    if (!argumentsFrom(d, args))
      continue;
    // `Box b = 5;`: an explicit constructor's guide is no candidate of a copy-initialization ([over.match.class.deduct]
    // 3; an unmarked one's too, design.md "Explicit by default", rule 5). (After the
    // class's defaults are in d: an explicit(cond) may read them.)
    if (copyInit && isExplicit(ctor, &d))
      continue;
    // The guide's constraints, the class template's and the constructor's ([over.match.class.deduct]
    // 1.1: `template<class T> requires Small<T> struct W { W(T); };` deduces nothing from a double);
    // until 2026-11-08 neither was checked.
    if (!classConstraintsHold(cls, d) || !constraintsHold(ctor, d))
      continue;
    bool ok = true;
    std::vector<int> ranks;
    const Ty* inst = substitute(ft, d); // (as a whole: `M&... values` becomes one parameter for each element)
    for (size_t i = 0; i < argTypes.size() && i < inst->args.size() && ok; ++i)
    {
      ranks.push_back(conversionRank(argTypes[i], inst->args[i]));
      ok = ranks.back() < NotViable;
    }
    if (ok)
      // (The arguments may pick a specialization: `W w(&a)` is W<int*>, an instance of W<T*> if there is one.)
      return fromGuide && !strictlyBetter(ranks, guideRanks) ? fromGuide : resolveSpecialization(types.classType(cls, Span<const Ty*>{args.data(), uint32_t(args.size())}), 0);
  }
  // A class of no constructor has the guide of `C()` ([over.match.class.deduct] 1.2).
  if (!fromGuide && !hasConstructor && argTypes.empty())
  {
    Subst d;
    std::vector<const Ty*> args;
    if (argumentsFrom(d, args))
      return resolveSpecialization(types.classType(cls, Span<const Ty*>{args.data(), uint32_t(args.size())}), 0);
  }
  return fromGuide;
}

auto GlobalScope::Impl::deduceAggregateArgs(const Ty* primary, const std::vector<const Ty*>& argTypes) -> const Ty*
{
  Scope* s = classScopeOf(primary);
  if (!s || !s->owner || s->owner->kind != NodeKind::ClassDecl || argTypes.empty())
    return nullptr;
  auto* cls = static_cast<ClassDecl*>(s->owner);
  std::vector<VariableDecl*> fields;
  for (Decl* m : membersRead(cls))
  {
    if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Constructor))
      return nullptr; // not an aggregate: its constructors decide
    if (m->kind == NodeKind::VariableDecl && !(m->flags & (DF_Static | DF_Typedef)))
      fields.push_back(static_cast<VariableDecl*>(m));
  }
  Subst d;
  std::vector<std::pair<const Ty*, const Ty*>> checks; // (argument, the member type it initializes)
  size_t next = 0;
  LocalStack own;
  for (Decl* p : cls->templateParams)
    own.push(static_cast<TemplateParamDecl*>(p)->name, p);
  for (size_t i = 0; i < cls->bases.size() && next < argTypes.size(); ++i)
  {
    const ClassDecl::Base& b = cls->bases[i];
    if (b.isVirtual || b.access == Access::Private || b.access == Access::Protected || (b.access == Access::Default && cls->keyword == Kw::class_))
      return nullptr; // not an aggregate
    const Ty* baseTy = convertType(b.type, TypeContext{lexicalScopeOf(cls), &own, nullptr}, 1);
    if (!baseTy || !baseTy->isKnown())
      return nullptr;
    if (!b.isPack)
    {
      deduce(baseTy, argTypes[next], d);
      checks.push_back({argTypes[next++], baseTy});
      continue;
    }
    if (i + 1 != cls->bases.size() || !fields.empty() || baseTy->kind != TyKind::Param || !baseTy->decl || d.find(baseTy->decl))
      return nullptr; // a pack of bases before other elements: not deduced here
    std::vector<const Ty*> elements;
    for (; next < argTypes.size(); ++next)
    {
      if (!argTypes[next] || !argTypes[next]->isKnown() || argTypes[next]->isBraceList())
        return nullptr;
      elements.push_back(types.decay(argTypes[next]));
    }
    d.pairs.push_back({baseTy->decl, types.pack(Span<const Ty*>{elements.data(), uint32_t(elements.size())})});
  }
  for (size_t i = 0; i < fields.size() && next < argTypes.size(); ++i)
  {
    const Ty* fieldTy = declaredTypeOf(fields[i], nullptr, 0);
    if (!fieldTy || !fieldTy->isKnown())
      return nullptr;
    Decl* bound = nullptr;
    if (i + 1 == fields.size() && fieldTy->kind == TyKind::Array && fields[i]->type && fields[i]->type->kind == NodeKind::ArrayType)
      if (Expr* size = static_cast<ArrayType*>(fields[i]->type)->size; size && size->kind == NodeKind::NameExpr)
        if (const QualifiedName& n = static_cast<NameExpr*>(size)->name; n.parts.size() == 1)
          for (Decl* p : cls->templateParams)
            if (auto* tp = static_cast<TemplateParamDecl*>(p); !tp->isType && !tp->isPack && tp->name == n.parts[0].symbol)
              bound = p;
    // Each element deduces as a function's argument does (C++20 [over.match.class.deduct]): a
    // member not a reference decays it, so `Array{"a", "bc"}` holds const char*, as std::array's
    // deduction guide has it.
    auto passed = [&](const Ty* member, const Ty* arg) { return member->isReference() || !arg ? arg : types.decay(arg); };
    if (!bound)
    {
      deduce(fieldTy, passed(fieldTy, argTypes[next]), d);
      checks.push_back({argTypes[next++], fieldTy});
      continue;
    }
    // `T items[N]`: the elements go into the array (brace elision).
    int64_t count = int64_t(argTypes.size() - next);
    for (; next < argTypes.size(); ++next)
    {
      deduce(fieldTy->inner, passed(fieldTy->inner, argTypes[next]), d);
      checks.push_back({argTypes[next], fieldTy->inner});
    }
    auto* tp = static_cast<TemplateParamDecl*>(bound);
    const Ty* of = tp->valueType ? convertType(tp->valueType, TypeContext{lexicalScopeOf(bound), nullptr, nullptr}, 1) : types.intType();
    if (!d.find(bound))
      d.pairs.push_back({bound, types.valueType(of, count)});
  }
  if (next < argTypes.size())
    return nullptr; // more elements than members
  std::vector<const Ty*> args;
  for (Decl* p : cls->templateParams)
  {
    const Ty* t = d.find(p);
    if (!t || !t->isKnown() || types.isDependent(t))
      return nullptr;
    if (!spliceIfPack(t, args))
      args.push_back(t);
  }
  for (const auto& [arg, member] : checks)
    if (conversionRank(arg, substitute(member, d)) >= NotViable)
      return nullptr;
  return resolveSpecialization(types.classType(cls, Span<const Ty*>{args.data(), uint32_t(args.size())}), 0);
}

auto GlobalScope::Impl::typeOfTypeDecl(Decl* d, const Lookup& l, const NamePart& lastPart, const TypeContext& ctx, int depth) -> const Ty*
{
  switch (d->kind)
  {
    case NodeKind::ClassDecl:
    {
      // Use the definition (the scope owner) so that all mentions share one decl.
      Decl* def = l.scope && l.scope->owner && l.scope->kind == ScopeKind::Class ? l.scope->owner : d;
      // A specialisation, picked for its scope because the primary template has no definition
      // (`template<typename...> struct IndexInTemplateList;` beside partial specialisations):
      // the arguments are the primary's, and they select the specialisation (or none).
      bool noPrimary = false;
      if ((def->flags & DF_Specialization) && lastPart.hasTemplateArgs && l.overloads)
      {
        if (Decl* primary = primaryOf(*l.overloads))
          def = primary;
        else
          noPrimary = true;
      }
      std::vector<const Ty*> args;
      Span<Decl*> params = noPrimary ? Span<Decl*>{} : effectiveParams(def);
      if (!lastPart.hasTemplateArgs && !params.empty() && l.found == Found::Member && l.foundIn && l.foundIn == def->ownScope &&
          depth < 32)
      {
        // `using super = TargeterWithOwner;` in a class deriving from TargeterWithOwner<Entity,
        // Robot, 7>: the base's injected class name, found in the base, names that instance.
        Scope* cls = ctx.scope;
        while (cls && cls->kind != ScopeKind::Class)
          cls = cls->parent;
        if (cls && cls != l.foundIn && cls->owner)
        {
          const Ty* self = selfType(cls->owner);
          if (self && ctx.subst)
            self = substitute(self, *ctx.subst);
          if (const Ty* base = self ? instantiationOfBase(self, cls, l.foundIn, depth + 1) : nullptr; base && base != self)
            return base;
        }
      }
      // A class template declared in a class template (`IteratorBase<ValueType>` in a member of
      // Heap<ValueType, Tag>): the instance has the enclosing instance's arguments first, as the
      // context binds the enclosing template's parameters (the parameters themselves where it
      // binds none: a mention in the template), then the ones written, its own.
      size_t enclosing = noPrimary ? 0 : enclosingParamCount(def);
      for (size_t i = 0; i < enclosing; ++i)
        args.push_back(ctx.subst && ctx.subst->find(params[i]) ? ctx.subst->find(params[i]) : types.param(params[i]));
      if (lastPart.templateArgs.size() == 1 && lastPart.symbol == functionSymbolId())
      {
        // The library's `Function<R(Args...)>`: the compiler takes the signature apart, the
        // class behind it is FunctionImpl<R, Args...> (stdlib/core/Function.kpp).
        const Ty* signature = templateArgument(lastPart.templateArgs[0], nullptr, ctx, depth);
        if (signature && signature->kind == TyKind::Function)
        {
          Lookup implLookup;
          if (lookupInScope(&root, symbols.intern("FunctionImpl"), implLookup, 0) && implLookup.scope && implLookup.scope->owner)
          {
            std::vector<const Ty*> implArgs{signature->inner};
            for (const Ty* a : signature->args)
              implArgs.push_back(a);
            return types.classType(implLookup.scope->owner, Span<const Ty*>{implArgs.data(), uint32_t(implArgs.size())});
          }
        }
      }
      for (size_t i = 0; i < lastPart.templateArgs.size(); ++i)
      {
        Decl* param = enclosing + i < params.size() ? params[enclosing + i] : nullptr;
        if (!param && params.size() > enclosing && static_cast<TemplateParamDecl*>(params[params.size() - 1])->isPack)
          param = params[params.size() - 1]; // the pack takes the rest
        const Ty* a = templateArgument(lastPart.templateArgs[i], param, ctx, depth);
        if (a && a->kind == TyKind::Pack)
          for (const Ty* e : a->args)
            args.push_back(e); // `FunctionImpl<R, Args...>` inside the template: the elements
        else
          args.push_back(a);
      }
      if (args.size() > enclosing || (lastPart.hasTemplateArgs && params.size() > enclosing))
      {
        // Parameters not written take their defaults, which may use the earlier arguments
        // (`Less<>`: all of them).
        for (size_t i = args.size(); i < params.size(); ++i)
        {
          auto* p = static_cast<TemplateParamDecl*>(params[i]);
          if (!p->defaultArg)
            break;
          Subst s;
          LocalStack own; // the parameters by name: `= Less<K>` names an earlier one
          for (size_t k = 0; k < i; ++k)
          {
            s.pairs.push_back({params[k], args[k]});
            own.push(static_cast<TemplateParamDecl*>(params[k])->name, params[k]);
          }
          TypeContext dctx{lexicalScopeOf(def), &own, &s, def->file ? def->file->tokens : nullptr, def->file ? def->file->text : std::string_view()};
          args.push_back(templateArgument(p->defaultArg, p, dctx, depth + 1)); // (another declaration's: a step of depth, as a hop through a name is)
        }
      }
      // `Signal<>` of `template<typename... Args>`: written, its pack empty (no arguments at all).
      bool emptyPack = lastPart.hasTemplateArgs && params.size() == enclosing + 1 && static_cast<TemplateParamDecl*>(params[enclosing])->isPack;
      if (args.size() == enclosing && params.size() > enclosing && !emptyPack)
      {
        // In a specialization of the template its name, unwritten arguments, is that specialization's
        // (`Spec self;` in a member of `Spec<T*>`, where lookup finds the primary: a specialization's
        // own scope holds no name of its own, as `Spec<int>` there means the template).
        if (!lastPart.hasTemplateArgs)
          for (Scope* s = ctx.scope; s; s = s->parent)
            if (s->owner && s->owner->kind == NodeKind::ClassDecl && (s->owner->flags & DF_Specialization) && !static_cast<ClassDecl*>(s->owner)->name.empty() &&
                static_cast<ClassDecl*>(s->owner)->name.last().symbol == lastPart.symbol && lexicalScopeOf(s->owner) == lexicalScopeOf(def))
            {
              const Ty* self = selfType(s->owner);
              return self && ctx.subst ? substitute(self, *ctx.subst) : self;
            }
        // Injected class name inside its own template, or a dependent mention: keep parameters.
        args.clear();
        for (Decl* p : params)
          args.push_back(ctx.subst && ctx.subst->find(p) ? ctx.subst->find(p) : types.param(p));
      }
      normalizeValueArgs(params, args, def, depth);
      if (enclosing)
      {
        // Specialisations of a member template would be matched by their own arguments, which
        // this does not do yet: no class rather than the primary's.
        if (l.overloads)
          for (const Entry& e : *l.overloads)
            if (e.decl && e.decl->kind == NodeKind::ClassDecl && (e.decl->flags & DF_Specialization))
              return types.unknown();
      }
      else if (!params.empty() || noPrimary)
      {
        // `Nest<I - 1, T*>` in a member of Nest's primary: the injected class name, written with
        // arguments, names the template, whose specialisations they may pick (where it is declared).
        Lookup entries = l;
        if (!entries.overloads && lastPart.hasTemplateArgs && (def->attributes & ATTR_Specialized) && lastPart.symbol != UINT32_MAX)
          if (Lookup home; lookupInScope(lexicalScopeOf(def), lastPart.symbol, home, depth + 1, false) && home.overloads)
            entries = home;
        // As if asked from the top, whoever asks from how deep (a type nested 20 deep picked the
        // primary template: the match's own bounds counted from the asker's depth); a level as a
        // cached computation is (CachedEvaluation), which bounds a recursion through matches.
        CachedEvaluation level;
        if (!level.ok)
          return types.unknown();
        if (const Ty* spec = matchSpecialization(entries, args, 1))
          return spec; // a full specialisation is its own class; a partial one keeps its own parameters' arguments
      }
      if (noPrimary && !argsDependent(args))
        return types.unknown(); // specialisations only, none for these arguments: no class
      return types.classType(def, Span<const Ty*>{args.data(), uint32_t(args.size())});
    }
    case NodeKind::EnumDecl:
    {
      Decl* def = l.scope && l.scope->owner && l.scope->kind == ScopeKind::Enum ? l.scope->owner : d;
      return enumTypeIn(def, ctx.subst);
    }
    case NodeKind::TemplateParamDecl:
      if (ctx.subst)
        if (const Ty* t = ctx.subst->find(d))
          return elementOf(d, t, ctx);
      return types.param(d);
    case NodeKind::UsingAliasDecl:
    {
      auto* u = static_cast<UsingAliasDecl*>(d);
      if (!u->templateParams.empty())
      {
        // An alias template: its type with the written arguments (and defaults) substituted.
        Span<Decl*> params = u->templateParams;
        Subst s;
        std::vector<const Ty*> args;
        for (size_t i = 0; i < params.size(); ++i)
        {
          if (static_cast<TemplateParamDecl*>(params[i])->isPack)
          {
            // `template<typename... T> using PlusOne = Count<int, T...>`: the pack is the rest of
            // the arguments, none for `PlusOne<>`, an argument's pack spliced (`PlusOne<U...>`).
            // It took the first argument alone: `Same<char, short>` was Count<char>.
            std::vector<const Ty*> rest;
            for (size_t k = i; k < lastPart.templateArgs.size(); ++k)
              if (const Ty* a = templateArgument(lastPart.templateArgs[k], params[i], ctx, depth); !spliceIfPack(a, rest))
                rest.push_back(a);
            args.push_back(types.pack(Span<const Ty*>{rest.data(), uint32_t(rest.size())}));
            s.pairs.push_back({params[i], args.back()});
            break;
          }
          if (i >= lastPart.templateArgs.size())
            break;
          args.push_back(templateArgument(lastPart.templateArgs[i], params[i], ctx, depth));
          s.pairs.push_back({params[i], args.back()});
        }
        LocalStack own; // the alias's parameters, found by name while its defaults and type are converted
        for (Decl* p : params)
          own.push(static_cast<TemplateParamDecl*>(p)->name, p);
        for (size_t i = args.size(); i < params.size(); ++i)
        {
          auto* p = static_cast<TemplateParamDecl*>(params[i]);
          if (!p->defaultArg)
            break;
          TypeContext dctx{lexicalScopeOf(u), &own, &s, u->file ? u->file->tokens : nullptr, u->file ? u->file->text : std::string_view()};
          args.push_back(templateArgument(p->defaultArg, p, dctx, depth + 1)); // `= Less<K>`: K is an earlier parameter, bound in s
          s.pairs.push_back({params[i], args.back()});
        }
        if (ctx.subst)
          for (const Subst::Pair& p : ctx.subst->pairs)
            s.pairs.push_back(p);
        TypeContext actx{lexicalScopeOf(u), &own, &s, u->file ? u->file->tokens : nullptr, u->file ? u->file->text : std::string_view()};
        // As a declared type is (declaredTypeOf): as if asked from the top, a level of its own.
        CachedEvaluation level;
        if (!level.ok)
          return types.unknown();
        return convertType(u->type, actx, 0);
      }
      // An alias local to a function body, named as a qualifier (`PrototypeType::IDType` after
      // `using PrototypeType = ListType::ThePrototypeType;` in a generic lambda): its type where
      // it is named, as convertType has a local alias named alone; the declaration's own context
      // does not see a lambda's template parameters (the body's declarations belong to the
      // enclosing function).
      if (ctx.locals && d->parent && d->parent->kind == NodeKind::FunctionDecl)
        return convertType(u->type, ctx, depth + 1);
      return declaredTypeOf(d, ctx.subst, depth + 1); // under the instantiation's arguments, if any
    }
    case NodeKind::VariableDecl:
      if (d->flags & DF_Typedef)
      {
        const Ty* t = declaredTypeOf(d, ctx.subst, depth + 1);
        // An imported C header's typedef of a name kpp builds in (int64_t, uint64_t, size_t,
        // ptrdiff_t, intptr_t, uintptr_t) is kpp's own type where the two agree in width and
        // signedness (thread B, 2026-11-05: design.md, "Targets and hosts"): glibc's `typedef long
        // int64_t;` and `typedef unsigned long size_t;` on LP64 targets made them C's long, a type of
        // its own (TypeTable::longType), so a C function taking int64_t* or size_t* refused kpp's i64*
        // and usize*, and kpp's int64_t meant long wherever stdint.h was imported. On the target they
        // are one C type (the same size and convention). One that differs in width stays the
        // header's (glibc's 64-bit int_fast32_t), as does one that names C's long itself.
        auto* v = static_cast<VariableDecl*>(d);
        if (t && t->kind == TyKind::Int && !t->isConst && !t->isVolatile && d->file && d->file->isImported && v->name.parts.size() == 1)
          if (const Ty* b = builtinByName(v->name.parts[0].symbol); b && b->kind == TyKind::Int && b->bits == t->bits && b->isUnsigned == t->isUnsigned)
            return b;
        return t;
      }
      return types.unknown();
    case NodeKind::UsingNameDecl:
    {
      Lookup inner = lookupQualified(static_cast<UsingNameDecl*>(d)->name, lexicalScopeOf(d), nullptr, depth + 1);
      if (inner.decl && inner.decl != d)
        return typeOfTypeDecl(inner.decl, inner, static_cast<UsingNameDecl*>(d)->name.last(), TypeContext{lexicalScopeOf(d), nullptr, nullptr}, depth + 1);
      return types.unknown();
    }
    default:
      return types.unknown();
  }
}

// The depth steps where a name leads to another declaration's text, never for a written type's own
// parts (a pointee, an argument: the parser bounds how deep they nest), and a declaration's type is
// converted from depth 0 (declaredTypeOf): a type written 21 class templates deep, `W<W<...>>`, named
// its innermost class 'not declared'.
auto GlobalScope::Impl::convertType(Type* t, const TypeContext& ctx, int depth) -> const Ty*
{
  if (!t || depth > 40)
    return types.unknown();
  switch (t->kind)
  {
    case NodeKind::BuiltinType:
    {
      auto* b = static_cast<BuiltinType*>(t);
      switch (b->keyword)
      {
        case Kw::void_: return types.voidType();
        case Kw::bool_: return types.boolType();
        // char, signed char and unsigned char are three types, as in C++; the last two are
        // int8_t and uint8_t (i8, u8).
        case Kw::char_: return b->isUnsigned ? types.intType(8, true) : b->isSigned ? types.intType(8) : types.charType(8);
        case Kw::char8_t_: return types.charType(8, true);
        case Kw::char16_t_: return types.charType(16, true);
        case Kw::char32_t_: return types.charType(32, true);
        case Kw::wchar_t_: return types.wcharType(); // (the target's C wchar_t)
        case Kw::float_: return types.floatType(32);
        case Kw::double_: return types.floatType(64);
        case Kw::short_: return types.intType(16, b->isUnsigned);
        default:
          // int, long, long long, unsigned, signed: long a type of its own (C's, sized by the
          // target), long long 64 bits (i64).
          return b->longCount == 1 ? types.longType(b->isUnsigned) : types.intType(b->longCount >= 2 ? 64 : 32, b->isUnsigned);
      }
    }
    case NodeKind::NamedType:
    {
      auto* n = static_cast<NamedType*>(t);
      if (n->name.empty())
        return types.unknown();
      // An alias local to a function body (`using T = VectorType::value_type;`), as the walk
      // found it: also where the context has no locals (the code generator's typeOfAstIn).
      if (n->name.parts.size() == 1 && n->resolved && n->resolved->kind == NodeKind::UsingAliasDecl &&
          n->resolved->parent && n->resolved->parent->kind == NodeKind::FunctionDecl && n->resolved->templateParams.empty())
        return convertType(static_cast<UsingAliasDecl*>(n->resolved)->type, ctx, depth + 1);
      // `sizeof(prefixes)` with prefixes a local array: the parser read a type, the walk found a
      // variable, so this is the variable's type (C++ reads `sizeof(name)` by what the name is;
      // template arguments that name constants never get here).
      if (n->resolved && (n->resolved->kind == NodeKind::ParamDecl || (n->resolved->kind == NodeKind::VariableDecl && !(n->resolved->flags & DF_Typedef))))
        if (const Ty* vt = declaredTypeOf(n->resolved, ctx.subst, depth + 1); vt && vt->isKnown() && !containsAuto(vt))
          return vt;
      if (n->name.parts.size() == 1 && n->name.parts[0].hasTemplateArgs)
        if (const Ty* trait = typeTrait(n->name.parts[0], ctx, depth))
          return trait;
      // Under an instantiation the qualifier's arguments are known: `Pick<1, A, B>::Type` with
      // A = int finds the member in the specialisation those arguments pick.
      Lookup l = lookupTypeName(n, ctx.scope, ctx.locals, depth + 1, ctx.subst, ctx.elementPacks, ctx.elementIndex);
      // `using ThrowOnError = NamedBool<class ThrowOnError>;` (SpriteBank): an elaborated `class X`
      // names a class, never a type alias of the name (C++ looks it up before the alias is declared,
      // and an alias it found would be ill-formed). Found, the alias was its own argument: an endless
      // NamedBool<NamedBool<...>>, cut at some depth, so that a call taking it resolved or not by the
      // order of the threads (the game's tests, 2026-11-05). The class the namespace gets instead.
      if (n->elaborated && n->name.parts.size() == 1 && !n->name.parts[0].hasTemplateArgs && l.found != Found::No && l.decl &&
          (l.decl->kind == NodeKind::UsingAliasDecl || (l.decl->kind == NodeKind::VariableDecl && (l.decl->flags & DF_Typedef))))
        l = Lookup{};
      if (l.found == Found::Local && l.decl && l.decl->when && n->resolved && n->resolved != l.decl && n->resolved->when)
      {
        l.decl = n->resolved; // (of several `when` locals of the name, the one the walk found existing: design.md 19)
        l.scope = n->resolved->ownScope;
      }
      if (l.found == Found::Local && l.decl && l.decl->when && l.decl->whenKnown == 0)
        return types.unknown(); // a local a false `when` declares: no type (the walk said so where it is used)
      if (l.decl && l.decl->kind == NodeKind::UsingAliasDecl && static_cast<UsingAliasDecl*>(l.decl)->type == t && n->name.parts.size() == 1)
        l = lookupPastAlias(static_cast<UsingAliasDecl*>(l.decl), n->name.parts[0].symbol, depth + 1); // `using Option = Option<DataType>;`
      if (n->name.parts.size() == 2 && !n->name.parts[1].hasTemplateArgs && n->name.parts[0].hasTemplateArgs)
      {
        // `Num<I>::Type` with I still a parameter: the member of whichever specialisation the
        // arguments pick once they are known; the primary's member must not stand in for it.
        QualifiedName prefix = n->name;
        prefix.parts.count -= 1;
        Lookup pl = lookupQualified(prefix, ctx.scope, ctx.locals, depth + 1, ctx.subst, ctx.elementPacks, ctx.elementIndex);
        if (pl.decl && pl.decl->kind == NodeKind::ClassDecl)
        {
          const Ty* q = typeOfTypeDecl(pl.decl, pl, prefix.last(), ctx, depth + 1);
          if (q && q->kind == TyKind::Class && types.isDependent(q))
            return types.dependentMember(q, n->name.parts[1].symbol);
        }
      }
      if (l.found == Found::No && n->name.parts.size() > 1 && !n->name.last().hasTemplateArgs && depth < 64)
      {
        // `typename C::value_type` with C a `Const<Base>` that is `template<class T> struct
        // Const : T`: the member of the base the instance's argument makes, which the lookup in
        // the class's own scope does not see.
        NamedType prefix{};
        prefix.kind = NodeKind::NamedType;
        prefix.token = n->token;
        prefix.name = n->name;
        prefix.name.parts.count -= 1;
        const Ty* q = convertType(&prefix, ctx, depth + 1);
        q = q ? types.unqualified(q) : nullptr;
        if (q && q->kind == TyKind::Class && q->decl && !types.isDependent(q))
          if (const Ty* r = memberTypeOf(q, n->name.last().symbol, depth + 1))
            return r;
      }
      if (l.found == Found::No)
      {
        if (n->name.parts.size() >= 1)
        {
          const NamePart& first = n->name.parts[0];
          if (n->name.parts.size() == 1 && !first.hasTemplateArgs)
            if (const Ty* b = builtinByName(first.symbol))
              return b;
          // A class or enumeration the walk found where this context does not see it: one a
          // function body declares, met again in a lambda's signature (`enum class Row` of a
          // constructor) or in a member of a class of that body (`Vector<PowerSwitchConnection>`).
          if (n->resolved && (n->resolved->kind == NodeKind::ClassDecl || n->resolved->kind == NodeKind::EnumDecl) &&
              n->name.parts.size() == 1 && !first.hasTemplateArgs && n->resolved->templateParams.empty())
          {
            Lookup found;
            found.found = Found::Local;
            found.decl = n->resolved;
            found.scope = n->resolved->ownScope;
            if (!found.scope)
            {
              Entry e{n->resolved, nullptr};
              found.scope = scopeOfEntry(e, depth + 1); // `struct ExtendedSupportData;` before its definition
            }
            return typeOfTypeDecl(n->resolved, found, first, ctx, depth + 1);
          }
          uint32_t sym = first.symbol;
          if (sym != UINT32_MAX && !templateParamNames.count(sym))
          {
            Scope* target = ctx.scope ? ctx.scope : &root;
            while (target->parent && target->kind != ScopeKind::Namespace)
              target = target->parent;
            // `StrongTypedef<double, class ScaleTag, PositiveDoubleValidator>`: the elaborated
            // type specifier declares the class ScaleTag in the namespace, as C++ has it.
            if (n->elaborated && n->name.parts.size() == 1 && !first.hasTemplateArgs)
              return types.classType(getOrCreateTag(target, sym), {});
            ClassDecl* draft = getOrCreateDraft(target, sym);
            // The arguments live until the type is interned: a span into a vector destroyed
            // earlier read freed memory, which other threads' allocations overwrote (the
            // intermittent crash in isDependent on a garbage argument).
            std::vector<const Ty*> args;
            if (first.hasTemplateArgs)
              for (Node* a : first.templateArgs)
                args.push_back(templateArgument(a, nullptr, ctx, depth + 1));
            const Ty* base = types.classType(draft, Span<const Ty*>{args.data(), uint32_t(args.size())});
            for (size_t i = 1; i < n->name.parts.size(); ++i)
              base = types.dependentMember(base, n->name.parts[i].symbol);
            return base;
          }
        }
        return types.unknown();
      }
      if (l.found == Found::Dependent && l.decl && l.decl->kind == NodeKind::TemplateParamDecl)
      {
        if (n->name.parts.size() > 1)
        {
          // `T::Unit`, `T::Option::BuilderMapping`: a member of a member of T, each step resolved
          // once the one before is a class (substitute), under the instantiation's arguments
          // right away when T is bound.
          bool plain = true;
          for (size_t i = 1; i < n->name.parts.size(); ++i)
            plain = plain && !n->name.parts[i].hasTemplateArgs && n->name.parts[i].special == 0;
          if (plain)
          {
            const Ty* t = types.param(l.decl);
            for (size_t i = 1; i < n->name.parts.size(); ++i)
              t = types.dependentMember(t, n->name.parts[i].symbol);
            if (const Ty* bound = ctx.subst ? ctx.subst->find(l.decl) : nullptr)
            {
              const Ty* sub = substitute(t, *ctx.subst, depth + 1);
              // A step left unresolved with T known names no member (`T::force_memcpy_save` with
              // T = int*): no type, which is what makes a detection idiom's argument ill-formed.
              if (!sub || (sub->kind == TyKind::DependentMember && !types.isDependent(bound)))
                return types.unknown();
              return sub;
            }
            return t;
          }
          if (ctx.subst)
            if (const Ty* sub = ctx.subst->find(l.decl))
            {
              if (sub->kind == TyKind::Class && sub->decl && sub->decl->ownScope)
              {
                // Resolve the rest of the name inside the substituted class.
                QualifiedName rest = n->name;
                rest.parts.data += 1;
                rest.parts.count -= 1;
                Lookup inner = lookupQualified(rest, sub->decl->ownScope, nullptr, depth + 1);
                if (inner.decl && inner.found != Found::No)
                {
                  Subst s2 = memberSubst(sub, inner.foundIn, depth); // a member of a base: that base's arguments
                  TypeContext c2{sub->decl->ownScope, nullptr, &s2};
                  return typeOfTypeDecl(inner.decl, inner, rest.last(), c2, depth + 1);
                }
              }
              return types.unknown();
            }
          return types.param(l.decl);
        }
        if (n->name.parts[0].hasTemplateArgs && !static_cast<TemplateParamDecl*>(l.decl)->innerParams.empty())
        {
          // `Container<T>` with Container a template template parameter: the parameter applied
          // to the written arguments; under an instantiation that binds Container to a class
          // template, that template with the arguments.
          std::vector<const Ty*> args;
          for (Node* a : n->name.parts[0].templateArgs)
            args.push_back(templateArgument(a, nullptr, ctx, depth + 1));
          Span<const Ty*> span{args.data(), uint32_t(args.size())};
          if (ctx.subst)
            if (const Ty* sub = ctx.subst->find(l.decl); sub && sub->kind == TyKind::Class && sub->decl)
            {
              // (`TT<Ts...>`: the pack's elements, as substitute gives them: Two<int, int>, not
              // Two<{int, int}>, which codegen could not bind a Two<int, int> to.)
              std::vector<const Ty*> spliced;
              for (const Ty* a : args)
                if (!spliceIfPack(a, spliced))
                  spliced.push_back(a);
              return resolveSpecialization(types.classType(sub->decl, Span<const Ty*>{spliced.data(), uint32_t(spliced.size())}), depth + 1);
            }
          return types.param(l.decl, span);
        }
        if (ctx.subst)
          if (const Ty* sub = ctx.subst->find(l.decl))
            return elementOf(l.decl, sub, ctx);
        return types.param(l.decl);
      }
      if (!l.decl)
        return types.unknown();
      if ((n->name.parts.size() > 1 || n->name.typeQualifier) &&
          (l.decl->kind == NodeKind::UsingAliasDecl || (l.decl->kind == NodeKind::VariableDecl && (l.decl->flags & DF_Typedef)) ||
           ((l.decl->kind == NodeKind::ClassDecl || l.decl->kind == NodeKind::EnumDecl) &&
            effectiveParams(l.decl).size() > l.decl->templateParams.size())))
      {
        // `Vector<int>::iterator`: the alias as the instantiation named by the qualifier has it;
        // `Holder<int>::Node`, `Flag<A>::Enum`, `Heap<Node>::IteratorBase<Node>`: the nested class
        // or enum with the enclosing instantiation's arguments; `ItemPrototype::IDType`: the alias
        // as the base it is declared in has it.
        const Ty* qualifier = nullptr;
        Subst qs = qualifierSubst(n->name, ctx, depth, l.foundIn, &qualifier);
        if (!qs.empty())
        {
          if (ctx.subst)
            for (const Subst::Pair& p : ctx.subst->pairs)
              qs.pairs.push_back(p);
          TypeContext c2{ctx.scope, ctx.locals, &qs, ctx.tokens, ctx.text};
          c2.elementPacks = ctx.elementPacks; // (its written arguments are a pattern's own nodes still)
          c2.elementIndex = ctx.elementIndex;
          return typeOfTypeDecl(l.decl, l, n->name.last(), c2, depth);
        }
        // `typename Dialog<T>::ResultMapping::value_type` in Dialog's out-of-line member: the
        // qualifier still a member of a dependent class, so the member of whatever it becomes
        // (substitute), not the member with its template's own parameters (`Pair<K, V>`).
        if (qualifier && qualifier->kind == TyKind::DependentMember && !n->name.last().hasTemplateArgs)
          return types.dependentMember(qualifier, n->name.last().symbol);
      }
      if (n->name.parts.size() == 1)
      {
        // `const_iterator it(*this);` in a member of ParticleSurface : InfiniteRectangle<ParticleChunk*>:
        // the base's nested class with the base's arguments.
        Subst bs = baseSubstForUnqualified(l, ctx, depth);
        if (!bs.empty())
        {
          if (ctx.subst)
            for (const Subst::Pair& p : ctx.subst->pairs)
              bs.pairs.push_back(p);
          TypeContext c2{ctx.scope, ctx.locals, &bs, ctx.tokens, ctx.text};
          // Its written arguments are a pattern's own nodes still: `Leaf<A>::width()` in a fold over a
          // pack of bases names element k's base (until 2026-11-07 the whole pack's, Leaf<int, char>).
          c2.elementPacks = ctx.elementPacks;
          c2.elementIndex = ctx.elementIndex;
          return typeOfTypeDecl(l.decl, l, n->name.last(), c2, depth);
        }
      }
      return typeOfTypeDecl(l.decl, l, n->name.last(), ctx, depth);
    }
    case NodeKind::PointerType: return types.pointer(convertType(static_cast<PointerType*>(t)->pointee, ctx, depth));
    case NodeKind::LRefType: return types.lref(convertType(static_cast<LRefType*>(t)->referenced, ctx, depth));
    case NodeKind::RRefType: return types.rref(convertType(static_cast<RRefType*>(t)->referenced, ctx, depth));
    case NodeKind::ConstType:
    {
      auto* c = static_cast<ConstType*>(t);
      return types.qualified(convertType(c->inner, ctx, depth), c->isConst, c->isVolatile);
    }
    case NodeKind::ArrayType:
    {
      auto* a = static_cast<ArrayType*>(t);
      // `sizeof(nums[1])` with nums a variable: the parser read an array type, the walk found a
      // variable, so this is one of its elements (a level per subscript), not an array of them.
      {
        int levels = 0;
        Type* inner = a;
        for (; inner && inner->kind == NodeKind::ArrayType && levels < 8; ++levels)
          inner = static_cast<ArrayType*>(inner)->element;
        Decl* r = inner && inner->kind == NodeKind::NamedType ? static_cast<NamedType*>(inner)->resolved : nullptr;
        if (r && (r->kind == NodeKind::ParamDecl || (r->kind == NodeKind::VariableDecl && !(r->flags & DF_Typedef))))
        {
          const Ty* e = declaredTypeOf(r, ctx.subst, depth + 1);
          for (; levels > 0 && e && e->isKnown(); --levels)
          {
            e = types.stripRef(e);
            e = e->kind == TyKind::Array || e->kind == TyKind::Pointer ? e->inner : types.unknown();
          }
          return e ? e : types.unknown();
        }
      }
      auto n = constEval(a->size, ctx);
      const Ty* element = convertType(a->element, ctx, depth);
      if (!n && a->size && a->size->kind == NodeKind::NameExpr)
      {
        // `const T (&array)[N]` with N a constant parameter not yet bound: the bound is N.
        Lookup l = lookupQualified(static_cast<NameExpr*>(a->size)->name, ctx.scope, ctx.locals, depth + 1, ctx.subst, ctx.elementPacks, ctx.elementIndex);
        if (l.decl && l.decl->kind == NodeKind::TemplateParamDecl && !static_cast<TemplateParamDecl*>(l.decl)->isType &&
            !(ctx.subst && ctx.subst->find(l.decl)))
          return types.dependentArray(element, l.decl);
      }
      return types.array(element, n && *n > 0 ? uint32_t(*n) : 0);
    }
    case NodeKind::FunctionType:
    {
      auto* f = static_cast<FunctionType*>(t);
      std::vector<const Ty*> params;
      for (Decl* p : f->params)
      {
        const Ty* pt = convertType(static_cast<ParamDecl*>(p)->type, ctx, depth);
        // As a function's own (FunctionDecl): an array parameter is a pointer to its element
        // (`unsigned (*)(unsigned rep[3])` is `unsigned (*)(unsigned*)`), a top-level const is no
        // part of the type.
        if (pt && pt->kind == TyKind::Array)
          pt = types.decay(pt);
        if (pt && !pt->isReference() && pt->kind != TyKind::Pack)
          pt = types.unqualified(pt);
        if (pt && pt->kind == TyKind::Pack)
          for (const Ty* e : pt->args)
            params.push_back(e); // `R (*)(Args...)` under an instantiation: one parameter per element
        else
          params.push_back(pt);
      }
      // `noexcept(cond)`: as cond says; a template's own `noexcept(N)`, N not bound yet, is a pattern.
      bool isNoexcept = f->isNoexcept;
      Decl* noexceptParam = nullptr;
      if (f->isNoexcept && f->noexceptCondition)
      {
        std::optional<int64_t> v = constEval(f->noexceptCondition, ctx);
        isNoexcept = v && *v != 0;
        if (!v && f->noexceptCondition->kind == NodeKind::NameExpr)
        {
          Lookup l = lookupQualified(static_cast<NameExpr*>(f->noexceptCondition)->name, ctx.scope, ctx.locals, depth + 1, ctx.subst, ctx.elementPacks, ctx.elementIndex);
          if (l.decl && l.decl->kind == NodeKind::TemplateParamDecl && !static_cast<TemplateParamDecl*>(l.decl)->isType)
            noexceptParam = l.decl;
        }
      }
      uint8_t memberQualifiers = (f->isConst ? TypeTable::MemberConst : 0) | (f->isVolatile ? TypeTable::MemberVolatile : 0) |
                                 (f->refQualifier == uint8_t(FunctionDecl::RefQualifier::LValue) ? TypeTable::MemberLValue : 0) |
                                 (f->refQualifier == uint8_t(FunctionDecl::RefQualifier::RValue) ? TypeTable::MemberRValue : 0);
      return types.function(convertType(f->result, ctx, depth), Span<const Ty*>{params.data(), uint32_t(params.size())}, f->isVariadic, false, isNoexcept, noexceptParam, memberQualifiers);
    }
    case NodeKind::AutoType: return static_cast<AutoType*>(t)->isDecltypeAuto ? types.decltypeAutoType() : types.autoType();
    case NodeKind::DecltypeType:
    {
      auto* d = static_cast<DecltypeType*>(t);
      // decltype's own rule ([dcl.type.decltype] 1): a plain name or member access is its declared
      // type, the type the walk gave it; any other lvalue a reference to its type (`decltype((x))`,
      // `decltype(++x)` are int&), an xvalue or prvalue its type (references the walk keeps).
      auto decltypeOfExpr = [&](const Ty* typed) -> const Ty* {
        const Expr* e = d->expr;
        if (d->isAuto || !e || !typed || !typed->isKnown() || typed->kind == TyKind::Pack ||
            e->kind == NodeKind::NameExpr || e->kind == NodeKind::MemberExpr || typed->kind == TyKind::Void)
          return typed;
        if (typed->kind == TyKind::RRef && isLvalueExpression(e))
          return types.lref(typed->inner); // (`(x.r)` of an `int&& r`: what a name says is an lvalue)
        return typed->isReference() || !isLvalueExpression(e) ? typed : types.lref(typed);
      };
      const Ty* generic = d->expr && d->expr->ty ? decltypeOfExpr(d->expr->ty) : types.unknown();
      if (namesTupleBinding(d->expr))
        generic = types.stripRef(generic); // `decltype(a)` of a tuple-like binding: the type get gives it, not its reference
      if (d->expr && !d->expr->ty && !d->isAuto)
        if (const Ty* typed = typeOfSignatureExpression(d->expr, ctx); typed && typed->isKnown())
          generic = decltypeOfExpr(typed); // `decltype(Definition::migrations)` in a member's type, before any walk typed it
      // `-> Future<decltype(callable())>` under an instantiation's arguments: the generic type of
      // a dependent expression (F) stands in for no call's result, so the expression is typed
      // anew with them. So is one that names a parameter they bind, whatever its generic type:
      // `decltype(check<T>(nullptr))` took the generic walk to the overload it could still judge
      // (`FalseType check(...)`, the other's return type depending on T).
      // (In a context with no tokens, a qualified name's template arguments as a lookup converts them,
      // `typename Conditional<isSame<M, N>, M, decltype(true ? declval<M>() : declval<N>())>::Type` in
      // an alias template: its own file's, so that it is typed anew there too; it kept the generic
      // walk's M&&, so lcm(u32, int) returned a type naming a template parameter, D 2026-11-06.)
      TypeContext own = ctx;
      if (!own.function && !own.tokens && d->file && d->file->tokens)
      {
        own.tokens = d->file->tokens;
        own.text = d->file->text;
      }
      if ((own.function || own.tokens) && own.subst && !own.subst->empty() && d->expr &&
          (!generic->isKnown() || types.isDependent(generic) || mentionsBound(d->expr, *own.subst)))
        if (const Ty* typed = typeOfSignatureExpression(d->expr, own); typed && typed->isKnown() && !types.isDependent(typed))
          return decltypeOfExpr(typed);
      return generic;
    }
    case NodeKind::MemberPointerType:
    {
      auto* m = static_cast<MemberPointerType*>(t);
      const Ty* cls = convertType(m->classType, ctx, depth + 1);
      return types.memberPointer(convertType(m->pointee, ctx, depth + 1), cls);
    }
    case NodeKind::PackType:
    {
      // `RemoveReference<Args>...`, `typename Unwrapped<Args>::Type...` with Args bound: the
      // pattern once per element, each pack it names bound to that element, as a pack (a bare
      // `Args...` is the pack itself).
      Type* pattern = static_cast<PackType*>(t)->pattern;
      TypeContext whole = ctx;
      whole.elementPacks = nullptr; // (an expansion in an expansion's pattern: its packs whole, `g<Ts...>()`)
      std::vector<Decl*> packs;
      if (ctx.subst && !isBarePack(pattern))
        packsNamedIn(pattern, *ctx.subst, packs);
      if (packs.empty())
        return convertType(pattern, whole, depth + 1);
      size_t n = ctx.subst->find(packs[0])->args.size();
      for (Decl* pack : packs)
        if (ctx.subst->find(pack)->args.size() != n)
          return types.unknown(); // packs of different lengths in one expansion
      std::vector<const Ty*> elements;
      for (size_t i = 0; i < n; ++i)
      {
        Subst element = *ctx.subst;
        for (Subst::Pair& pair : element.pairs)
          if (std::find(packs.begin(), packs.end(), pair.param) != packs.end())
            pair.type = pair.type->args[i];
        TypeContext ectx = whole;
        ectx.subst = &element;
        elements.push_back(convertType(pattern, ectx, depth + 1));
      }
      return types.pack(Span<const Ty*>{elements.data(), uint32_t(elements.size())});
    }
    default: return types.unknown();
  }
}

auto GlobalScope::Impl::declContextFor(Decl* d, DeclContext& out) -> void
{
  out.scope = lexicalScopeOf(d);
  if (d->kind == NodeKind::VariableDecl && static_cast<VariableDecl*>(d)->name.parts.size() > 1 &&
      !(d->parent && d->parent->kind == NodeKind::FunctionDecl))
  {
    // `const PositionFunction Belt::functions[2][2] = {...}` outside its class: the class scope,
    // as C++ has it for what follows the name (the initializer's `AlignTo::Begin`).
    auto* v = static_cast<VariableDecl*>(d);
    QualifiedName qualifier = v->name;
    qualifier.parts.count -= 1;
    LocalStack params;
    for (Decl* tp : v->templateParams)
      params.push(static_cast<TemplateParamDecl*>(tp)->name, tp);
    Lookup l = lookupQualified(qualifier, out.scope, &params, 0);
    if (l.scope && l.scope->kind == ScopeKind::Class)
      out.scope = l.scope;
  }
  for (Decl* p = d; p; p = p->parent)
    if (p != d && (p->kind == NodeKind::ClassDecl || p->kind == NodeKind::EnumDecl) && p->ownScope)
      break; // a member of a class (one declared in a function body included): that class's scope
    else if (p->kind == NodeKind::FunctionDecl)
    {
      // `int Box::f(const Terminal t) const` outside its class: the class scope, as C++ has it,
      // for the function and everything declared inside it.
      auto* f = static_cast<FunctionDecl*>(p);
      if (auto it = specializationMembers.find(f); it != specializationMembers.end())
        out.scope = it->second; // `int Flag<true>::f(Inner i)`: the specialisation's scope
      else if (f->name.parts.size() > 1)
      {
        QualifiedName qualifier = f->name;
        qualifier.parts.count -= 1;
        LocalStack params;
        for (Decl* tp : f->templateParams)
          params.push(static_cast<TemplateParamDecl*>(tp)->name, tp);
        Lookup l = lookupQualified(qualifier, lexicalScopeOf(f), &params, 0);
        if (l.scope && l.scope->kind == ScopeKind::Class)
          out.scope = l.scope;
      }
      break;
    }
  std::vector<Decl*> chain;
  for (Decl* p = d; p; p = p->parent)
    chain.push_back(p);
  for (size_t i = chain.size(); i-- > 0;)
    for (Decl* tp : chain[i]->templateParams)
      out.params.push(static_cast<TemplateParamDecl*>(tp)->name, tp);
  // Inside function bodies (a member of a class declared in one, `RailWithDirection
  // railWithDirection;` with `using RailWithDirection = ...;` before the class): the aliases,
  // classes and enums the bodies declare before it, outermost first.
  for (size_t i = chain.size(); i-- > 1;)
    if (chain[i]->kind == NodeKind::FunctionDecl)
      if (Stmt* body = static_cast<FunctionDecl*>(chain[i])->body; body && body->token < d->token)
        collectVisibleLocalAliases(body, d->token, out.params);
}

auto GlobalScope::Impl::declaredTypeOf(Decl* d, const Subst* subst, int /*depth*/) -> const Ty*
{
  if (!d)
    return types.unknown();
  // A closure type's call operator (closureCall): the closure type's signature, under any
  // substitution (a class derived from the closure asks with its own arguments, which name none of
  // the lambda's types). Made from the lambda's walk: no declaration's type to record or cache.
  if (maybeClosureCall(d))
    if (ClosureCall* c = closureCallOf(static_cast<FunctionDecl*>(d)))
      return c->signature;
  recordUse(USE_DeclaredType, d);
  bool useCache = !subst || subst->empty();
  if (useCache)
    if (const Ty* cached = d->cachedType.load(std::memory_order_acquire))
      return cachedNestingFits(unpackNesting(d->typeNeeded.load(std::memory_order_relaxed))) ? cached : types.unknown();
  // What computing it reads belongs to the declaration's type, which any later asker shares;
  // seen through a substitution it is the asker's own (and not cached).
  std::optional<RecordInto> into;
  if (useCache && recordingDependencies)
    into.emplace(typeDependenciesOf(d));
  // A declaration whose type this thread is computing already (a C struct's member `UOW UOW;`, whose
  // type's name finds the member first, then the typedef): no type from here, at once, and the way
  // past it is taken (a name's other meaning); it ran on to the bound of 40 before, and the bound of
  // levels now. Not kept by what it was cut through from (cycleCutAt).
  if (useCache)
    for (const TypeInProgress& p : typesInProgress)
      if (p.decl == d)
      {
        cycleCutAt = std::min(cycleCutAt, p.level);
        return types.unknown();
      }
  // Under a substitution, one computed earlier in the outermost such computation (SubstTypeMemo).
  std::optional<SubstTypeMemo> ownMemo;
  std::vector<const void*> memoKey;
  if (!useCache && !substTypeMemo)
  {
    ownMemo.emplace(); // the outermost one under a substitution: what it computes inside is kept there
    ownMemo->rootLevel = cachedLevels + 1;
  }
  else if (!useCache && cachedLevels + 1 - substTypeMemo->rootLevel >= SubstTypeMemo::fromLevel)
  {
    memoKey.push_back(d);
    for (const Subst::Pair& p : subst->pairs)
    {
      memoKey.push_back(p.param);
      memoKey.push_back(p.type);
    }
    if (substTypeMemo->types)
      if (auto it = substTypeMemo->types->find(memoKey); it != substTypeMemo->types->end())
        return cachedNestingFits(it->second.needed) ? it->second.type : types.unknown();
  }
  struct MemoScope
  {
    explicit MemoScope(std::optional<SubstTypeMemo>& own) : active(own.has_value())
    {
      if (active)
        substTypeMemo = &*own;
    }
    ~MemoScope()
    {
      if (active)
        substTypeMemo = nullptr;
    }
    MemoScope(const MemoScope&) = delete;
    MemoScope& operator=(const MemoScope&) = delete;

    bool active;
  } memoScope(ownMemo);
  // As if asked from the top, whoever asks from how deep: a level of its own (CachedEvaluation),
  // which bounds a recursion through declarations (`using A2 = W<A1>;`, a member alias naming
  // its class's next instance's) and keeps how much deeper it nested beside the type it keeps.
  // (Each declaration counted the depth of its asker on, cut at 40: an alias of an alias of ... 8
  // deep, or a type written 21 class templates deep, named a class 'not declared'.)
  CachedEvaluation level;
  if (!level.ok)
    return types.unknown();
  struct InProgress
  {
    InProgress(Decl* d, bool own) : outerCut(cycleCutAt), own(own)
    {
      cycleCutAt = INT_MAX;
      if (own)
        typesInProgress.push_back({d, cachedLevels});
    }
    ~InProgress()
    {
      if (own)
        typesInProgress.pop_back();
      cycleCutAt = std::min(outerCut, cycleCutAt);
    }
    InProgress(const InProgress&) = delete;
    InProgress& operator=(const InProgress&) = delete;

    int outerCut;
    bool own;
  } inProgress(d, useCache);
  const Ty* result = computeDeclaredType(d, subst, 0);
  // A cycle cut through an outer computation's declaration: this answer holds for the way in this
  // asker came, not for every asker (kept, it would answer by which came first).
  bool cutFromOutside = cycleCutAt < cachedLevels;
  // C's calling convention on what a C header declares (its functions' types, its typedefs', fields'
  // and parameters' function pointers) and on an extern "C" function's own type (withCConvention).
  if (result && result->isKnown() && d->file && d->file->isImported)
    result = withCConvention(result, true);
  else if (result && d->kind == NodeKind::FunctionDecl && (d->flags & DF_ExternC) && result->kind == TyKind::Function)
    result = withCConvention(result, false);
  // Only a known type, auto-free, computed without a trip: an unknown one may come from a deduction
  // not possible yet, and kept it would answer every later asker, so later uses were typed or not by
  // which thread asked first; one computed past a bound may hold unknown parts (W<?>).
  if (!memoKey.empty() && !level.tripped() && !cutFromOutside)
  {
    if (!substTypeMemo->types)
      substTypeMemo->types.emplace();
    substTypeMemo->types->emplace(std::move(memoKey), SubstTypeMemo::Entry{result, level.needed()});
  }
  if (useCache && result->isKnown() && !containsAuto(result) && !level.tripped() && !cutFromOutside)
    if (std::optional<uint32_t> needed = packNesting(level.needed()))
    {
      d->typeNeeded.store(*needed, std::memory_order_relaxed);
      d->cachedType.store(result, std::memory_order_release);
    }
  return result;
}

auto GlobalScope::Impl::withCConvention(const Ty* t, bool nested, int depth) -> const Ty*
{
  if (!t || depth > 16)
    return t;
  switch (t->kind)
  {
    case TyKind::Pointer:
    case TyKind::LRef:
    case TyKind::RRef:
    case TyKind::Array:
    {
      const Ty* inner = withCConvention(t->inner, true, depth + 1);
      if (inner == t->inner)
        return t;
      const Ty* r = t->kind == TyKind::Pointer ? types.pointer(inner)
                    : t->kind == TyKind::LRef  ? types.lref(inner)
                    : t->kind == TyKind::RRef  ? types.rref(inner)
                                               : types.array(inner, t->count);
      return types.qualified(r, t->isConst, t->isVolatile);
    }
    case TyKind::Function:
    {
      std::vector<const Ty*> params(t->args.begin(), t->args.end());
      const Ty* result = t->inner;
      bool changed = false;
      if (nested)
      {
        for (const Ty*& p : params)
        {
          const Ty* q = withCConvention(p, true, depth + 1);
          changed = changed || q != p;
          p = q;
        }
        const Ty* r = withCConvention(result, true, depth + 1);
        changed = changed || r != result;
        result = r;
      }
      auto byValue = [&](const Ty* x)
      {
        x = x ? types.unqualified(x) : nullptr;
        return x && x->kind == TyKind::Class;
      };
      bool c = t->isCConvention() || byValue(result) || std::any_of(params.begin(), params.end(), byValue);
      if (!changed && c == t->isCConvention())
        return t;
      return types.function(result, Span<const Ty*>{params.data(), uint32_t(params.size())}, t->variadic, c, t->isNoexcept(), t->decl, t->memberQualifiers());
    }
    default:
      return t;
  }
}

auto GlobalScope::Impl::instanceSubstShard(const Decl* copy) -> InstanceSubstShard&
{
  return instanceSubstShards[(reinterpret_cast<uintptr_t>(copy) >> 6) % std::size(instanceSubstShards)];
}

auto GlobalScope::Impl::recordInstanceSubst(const FunctionDecl* copy, const Subst& s) -> void
{
  Subst own = s; // copied outside the lock
  InstanceSubstShard& shard = instanceSubstShard(copy);
  std::lock_guard lock(shard.mutex);
  shard.substs.emplace(copy, std::move(own));
}

auto GlobalScope::Impl::instanceSubstOf(const Decl* d) -> const Subst*
{
  bool inClass = false;
  for (const Decl* p = d; p; p = p->parent)
  {
    if (p->kind == NodeKind::ClassDecl || p->kind == NodeKind::EnumDecl)
      inClass = true;
    else if (p->kind == NodeKind::FunctionDecl && (p->attributes & ATTR_InstanceCopy))
    {
      if (!inClass)
        return nullptr;
      InstanceSubstShard& shard = instanceSubstShard(p);
      std::lock_guard lock(shard.mutex);
      auto it = shard.substs.find(p);
      return it == shard.substs.end() ? nullptr : &it->second;
    }
  }
  return nullptr;
}

auto GlobalScope::Impl::withInstanceSubst(const Decl* d, const Subst* subst) -> Subst
{
  Subst out = subst ? *subst : Subst{};
  if (const Subst* inst = instanceSubstOf(d))
    for (const Subst::Pair& p : inst->pairs)
      if (!out.find(p.param))
        out.pairs.push_back(p);
  return out;
}

auto GlobalScope::Impl::deducedFromInitializer(const Ty* t, VariableDecl* v, const TypeContext& ctx) -> const Ty*
{
  Span<Expr*> args{&v->init, 1};
  bool direct = v->bracedInit;
  if (v->init->kind == NodeKind::BraceInitExpr)
    args = static_cast<BraceInitExpr*>(v->init)->args;
  else if (v->init->kind == NodeKind::FunctionalCastExpr && static_cast<FunctionalCastExpr*>(v->init)->type == v->type)
  {
    args = static_cast<FunctionalCastExpr*>(v->init)->args; // the parser's `T x(args)`
    direct = true;
  }
  std::vector<const Ty*> argTypes;
  for (Expr* a : args)
  {
    const Ty* at = a && a->kind != NodeKind::BraceInitExpr ? typeOfSignatureExpression(a, ctx) : nullptr;
    if (!at || !at->isKnown() || types.isDependent(at))
      return nullptr;
    argTypes.push_back(argumentType(a, at, types));
  }
  const Ty* deduced = deduceClassArgs(t, argTypes, !direct);
  if (!deduced && v->init->kind == NodeKind::BraceInitExpr)
    deduced = deduceAggregateArgs(t, argTypes);
  return deduced;
}

auto GlobalScope::Impl::computeDeclaredType(Decl* d, const Subst* subst, int depth) -> const Ty*
{
  DeclContext dc;
  declContextFor(d, dc);
  Subst withInstance; // `const PrototypeType* prototype` in a class of an instance's body
  if (instanceSubstOf(d))
  {
    withInstance = withInstanceSubst(d, subst);
    subst = &withInstance;
  }
  TypeContext ctx{dc.scope, &dc.params, subst, d->file ? d->file->tokens : nullptr, d->file ? d->file->text : std::string_view()};
  switch (d->kind)
  {
    case NodeKind::VariableDecl:
    {
      auto* v = static_cast<VariableDecl*>(d);
      if (v->builtin == BUILTIN_EnumValues && v->parent && v->parent->kind == NodeKind::EnumDecl)
        return types.array(enumTypeIn(v->parent, subst), uint32_t(enumeratorCount(v->parent))); // E::values
      const Ty* t = convertType(v->type, ctx, depth + 1);
      // A string literal initialising an array of characters may stand in braces (`uint8_t
      // text[] = {"abc"}` is `= "abc"`, C++ [dcl.init.string]).
      Expr* literal = v->init;
      if (t && t->kind == TyKind::Array && literal && literal->kind == NodeKind::BraceInitExpr && static_cast<BraceInitExpr*>(literal)->args.size() == 1 &&
          (types.unqualified(t->inner)->kind == TyKind::Char || types.unqualified(t->inner)->kind == TyKind::Int))
        literal = static_cast<BraceInitExpr*>(literal)->args[0];
      if (t && t->kind == TyKind::Array && t->count == 0 && literal && literal->kind == NodeKind::LiteralExpr &&
          static_cast<LiteralExpr*>(literal)->literalKind == TokenKind::String)
      {
        if (std::optional<ConstValue> s = constString(static_cast<LiteralExpr*>(literal), ctx))
          t = types.array(t->inner, uint32_t(s->parts->size())); // `char s[] = "abc"`: its four chars
        else if (const Ty* wide = stringLiteralArray(literal, types.pointer(t->inner), types))
          t = types.array(t->inner, wide->count); // `const wchar_t w[] = L"abc"`: its code units and the null
      }
      else if (t && t->kind == TyKind::Array && t->count == 0 && v->init && v->init->kind == NodeKind::BraceInitExpr)
      {
        // `double a[] = {1, 2}`: an element for each value, or for each run of values an element
        // takes with its braces elided (`In a[] = {1, 2, 3, 4}`: two, D, 2026-11-06: it was four).
        // The values as the walk typed them (a local's, walked before its type is asked for), else
        // typed as a signature's (a global's); the walk checks the bound again (checkElidedBound).
        Span<Expr*> values = static_cast<BraceInitExpr*>(v->init)->args;
        auto typeOf = [&](Expr* x) -> const Ty*
        {
          if (x->ty && x->ty->isKnown() && !types.isDependent(x->ty))
            return x->ty;
          return typeOfSignatureExpression(x, ctx);
        };
        t = types.array(t->inner, uint32_t(elementsFromValues(t->inner, values, typeOf, ctx.subst)));
      }
      else if (t && t->kind == TyKind::Array && t->count == 0 && v->init && v->init->kind == NodeKind::FunctionalCastExpr &&
               !static_cast<FunctionalCastExpr*>(v->init)->braced && !static_cast<FunctionalCastExpr*>(v->init)->args.empty())
      {
        // `int a[](1, 2, 3)` (C++20, [dcl.init.general] 16.5): an element for each value, none elided.
        bool expands = false;
        for (Expr* a : static_cast<FunctionalCastExpr*>(v->init)->args)
          expands = expands || (a->kind == NodeKind::UnaryExpr && static_cast<UnaryExpr*>(a)->postfix && P(static_cast<UnaryExpr*>(a)->op) == P::Ellipsis);
        if (!expands)
          t = types.array(t->inner, uint32_t(static_cast<FunctionalCastExpr*>(v->init)->args.size()));
      }
      // A static data member declared without its bound (`static Type const types[];`) has the
      // bound of its out-of-line definition (`Highlights::types[] = {...}`), as in C++.
      if (t && t->kind == TyKind::Array && t->count == 0 && !v->init)
      {
        auto it = variableDefinitions.find(v);
        if (it != variableDefinitions.end() && it->second != v)
          if (const Ty* defined = declaredTypeOf(it->second, subst, depth + 1); defined && defined->kind == TyKind::Array && defined->count != 0)
            t = types.array(t->inner, defined->count);
      }
      // `static constexpr Array accessors = {&S::a, &S::b};` used before the walk reaches the
      // initializer (by a member function written above it): the class's arguments from the
      // initializer (CTAD), as the walk deduces a local's.
      if (v->init && isUndeducedClassTemplate(t) && !(v->parent && v->parent->kind == NodeKind::FunctionDecl))
        if (const Ty* deduced = deducedFromInitializer(t, v, ctx))
          t = t->isConst || t->isVolatile ? types.qualified(deduced, t->isConst, t->isVolatile) : deduced; // (`const Box g{1};` is a const Box<int>)
      // A constexpr variable is const (`decltype(COUNT)` is `const int`), an array of them of
      // const elements; one whose type is deduced stays as deduced.
      if ((v->flags & DF_Constexpr) && t && t->isKnown() && !t->isReference() && !containsAuto(t))
        t = t->kind == TyKind::Array ? types.array(types.qualified(t->inner, true, false), t->count) : types.qualified(t, true, false);
      if (containsAuto(t) && v->init && subst && !subst->empty() && v->parent && v->parent->kind == NodeKind::ClassDecl &&
          (v->flags & DF_Static))
      {
        // `static constexpr auto FOLD = fold;` in a class template: the initializer's type under
        // this instance's arguments (the walker deduces it only for the template itself).
        if (const Ty* init = typeOfSignatureExpression(v->init, ctx); init && init->isKnown() && !types.isDependent(init))
          if (const Ty* deduced = deduceVariable(v, t, init, v->init); deduced && deduced->isKnown() && !containsAuto(deduced))
            return (v->flags & DF_Constexpr) && !deduced->isReference() ? types.qualified(deduced, true, false) : deduced;
      }
      // (Also when its own file's walk typed the initializer as unknown: that walk came before
      // some uses and after others, so the uses saw it or this deduction by the threads' order.)
      if (containsAuto(t) && v->init && (!v->init->ty || !v->init->ty->isKnown()) && v->init->kind != NodeKind::BraceInitExpr &&
          (!subst || subst->empty() || !v->parent || v->parent->kind == NodeKind::NamespaceDecl) && !(v->parent && v->parent->kind == NodeKind::FunctionDecl) && !hasLambda(v->init))
      {
        // `const auto CONTEXT_FULL = (CONTEXT_CONTROL | ...);` naming `const auto` globals the
        // walk has not reached yet: theirs from their initializers now, in any order (no lambda
        // among them, whose copy would be another closure type than the walk's).
        // Also inside a template instance: a namespace-scope variable's type depends on no arguments.
        if (const Ty* init = typeOfSignatureExpression(v->init, ctx); init && init->isKnown() && !types.isDependent(init))
          if (const Ty* deduced = deduceVariable(v, t, init, v->init); deduced && deduced->isKnown() && !containsAuto(deduced))
            return (v->flags & DF_Constexpr) && !deduced->isReference() ? types.qualified(deduced, true, false) : deduced;
      }
      // `auto migrate = [](auto& value, auto from, auto to) { ... };` at namespace scope, named above
      // it (a member function's body before it, order-independent symbols): its closure type from
      // a walk of its own of the initializer (constInitializerOf: a copy that keeps the walk's
      // closures, typed once and shared), the lambda's environment registered there, so that the
      // use above can call it. (Without it the use saw an unknown type until its file's walk
      // reached the declaration, by the walks' order.)
      if (containsAuto(t) && v->init && hasLambda(v->init) && (!subst || subst->empty()) && (!v->parent || v->parent->kind == NodeKind::NamespaceDecl) &&
          v->templateParams.empty() && (!v->init->ty || !v->init->ty->isKnown()))
        if (Expr* walked = constInitializerOf(v, nullptr); walked && walked != v->init && walked->ty && walked->ty->isKnown() && !types.isDependent(walked->ty))
          if (const Ty* deduced = deduceVariable(v, t, walked->ty, walked); deduced && deduced->isKnown() && !containsAuto(deduced))
            return (v->flags & DF_Constexpr) && !deduced->isReference() ? types.qualified(deduced, true, false) : deduced;
      if (types.isDependent(t) && containsAuto(t))
        return t; // Deduced from the initializer by the walker when it reaches the declaration.
      return t;
    }
    case NodeKind::ParamDecl:
    {
      const Ty* t = convertType(static_cast<ParamDecl*>(d)->type, ctx, depth + 1);
      return t && t->kind == TyKind::Array ? types.decay(t) : t; // `double m[3][3]` is a pointer parameter
    }
    case NodeKind::EnumeratorDecl:
      return d->parent && d->parent->kind == NodeKind::EnumDecl ? enumTypeIn(d->parent, subst) : types.intType();
    case NodeKind::FunctionDecl:
    {
      auto* f = static_cast<FunctionDecl*>(d);
      const Ty* result;
      if (f->flags & (DF_Constructor | DF_Destructor))
        result = types.voidType();
      else if (f->flags & DF_Conversion)
        result = convertType(f->name.last().convType, ctx, depth + 1);
      else
      {
        TypeContext rctx = ctx;
        rctx.function = f; // a decltype in the return type is typed anew under an instantiation's arguments
        result = convertType(f->trailingReturn ? f->trailingReturn : f->returnType, rctx, depth + 1);
      }
      std::vector<const Ty*> params;
      bool variadic = (f->flags & DF_Variadic) != 0;
      for (Decl* p : f->params)
      {
        auto* pd = static_cast<ParamDecl*>(p);
        const Ty* pt = convertType(pd->type, ctx, depth + 1);
        if (pt && pt->kind == TyKind::Array)
          pt = types.decay(pt); // `f(double m[3][3])` takes a pointer, as in C
        if (pt && !pt->isReference() && pt->kind != TyKind::Pack)
          pt = types.unqualified(pt); // `f(const int x)` is f(int): the const is the definition's business
        if (pd->isPack && spliceIfPack(pt, params))
        {
          // deduced pack (`Args...`, `Args&&...`): one parameter per element
        }
        else
        {
          params.push_back(pt);
          if (pd->isPack)
            variadic = true; // still a pack
        }
      }
      return types.function(result, Span<const Ty*>{params.data(), uint32_t(params.size())}, variadic);
    }
    case NodeKind::ClassDecl:
    case NodeKind::EnumDecl:
      return selfType(d);
    case NodeKind::TemplateParamDecl:
    {
      auto* p = static_cast<TemplateParamDecl*>(d);
      if (subst)
        if (const Ty* t = subst->find(d))
          return p->isType || t->kind != TyKind::Value ? t : t->inner; // a value parameter has its type
      if (p->isType)
        return types.param(d);
      return p->valueType ? convertType(p->valueType, ctx, depth + 1) : types.intType();
    }
    case NodeKind::UsingAliasDecl:
      return convertType(static_cast<UsingAliasDecl*>(d)->type, ctx, depth + 1);
    default:
      return types.unknown();
  }
}

auto GlobalScope::Impl::containsAuto(const Ty* t) const -> bool
{
  for (; t; t = t->inner)
    if (t->kind == TyKind::Auto)
      return true;
  return false;
}

auto GlobalScope::Impl::mentionsUnknown(const Ty* t, int depth) -> bool
{
  if (!t || depth > 16)
    return false;
  if (!t->isKnown() || (t->kind == TyKind::Class && t->decl && (t->decl->attributes & ATTR_Draft)))
    return true;
  if (mentionsUnknown(t->inner, depth + 1))
    return true;
  for (const Ty* a : t->args)
    if (mentionsUnknown(a, depth + 1))
      return true;
  return false;
}

auto GlobalScope::Impl::initializerListOfElements(const BraceInitExpr* list) -> const Ty*
{
  Decl* il = initializerListDecl();
  if (!il || !list || list->args.empty() || !list->designators.empty())
    return nullptr;
  const Ty* element = nullptr;
  for (const Expr* a : list->args)
  {
    if (!a || a->kind == NodeKind::BraceInitExpr || !a->ty || !a->ty->isKnown() || a->ty->kind == TyKind::Pack || types.isDependent(a->ty))
      return nullptr;
    const Ty* t = types.unqualified(types.decay(types.stripRef(a->ty)));
    if (element && t != element)
      return nullptr;
    element = t;
  }
  return types.classType(il, Span<const Ty*>{&element, 1});
}

auto GlobalScope::Impl::deduceAuto(const Ty* pattern, const Ty* init, const Expr* initExpr) -> const Ty*
{
  if (!pattern || !init || !init->isKnown())
    return types.unknown();
  if (pattern == types.decltypeAutoType())
    return init; // `decltype(auto)`: the expression's decltype, references kept
  if (pattern->kind == TyKind::Auto)
    return pattern->isConst ? types.qualified(types.decay(init), true, false) : types.decay(init);
  auto leaf = [&](const Ty* p) -> const Ty*
  {
    const Ty* base = types.stripRef(init);
    return types.qualified(base, p->isConst, p->isVolatile);
  };
  switch (pattern->kind)
  {
    case TyKind::LRef:
      return pattern->inner->kind == TyKind::Auto ? types.lref(leaf(pattern->inner)) : types.lref(deduceAuto(pattern->inner, init));
    case TyKind::RRef:
    {
      if (pattern->inner->kind != TyKind::Auto)
        return types.rref(deduceAuto(pattern->inner, init));
      // `auto&&`, a forwarding reference ([dcl.type.auto.deduct] 4, deduced as for a `T&&`
      // parameter): an lvalue reference for an lvalue (a named variable, a member, `*p`: their
      // type no reference, so initExpr says it; a call returning T&: its type), an rvalue
      // reference for an rvalue. Until 2026-11-05 `auto&& value = this->state;` was an AIState&&,
      // so decltype(value) and forward<decltype(value)>(value), which moved it, were not C++'s.
      // `const auto&&` is no forwarding reference: an rvalue reference to const, always.
      if (pattern->inner->isConst || pattern->inner->isVolatile)
        return types.rref(leaf(pattern->inner));
      if (init->kind == TyKind::LRef || isLvalueExpression(initExpr))
        return types.lref(types.stripRef(init));
      return init->kind == TyKind::RRef ? init : types.rref(init);
    }
    case TyKind::Pointer:
    {
      const Ty* base = types.stripRef(init);
      if (base->kind == TyKind::Array || base->kind == TyKind::Function)
        base = types.decay(base); // `auto* m = uniforms.matrix;`: the array decays to a pointer to its first element
      if (pattern->inner->kind == TyKind::Auto && base->kind == TyKind::Pointer)
        return types.qualified(types.pointer(types.qualified(base->inner, pattern->inner->isConst, false)), pattern->isConst, false);
      if (base->kind == TyKind::Pointer)
        return types.pointer(deduceAuto(pattern->inner, base->inner));
      return types.unknown();
    }
    default:
      return types.unknown();
  }
}

auto GlobalScope::Impl::deduceVariable(const VariableDecl* v, const Ty* pattern, const Ty* init, const Expr* initExpr) -> const Ty*
{
  if (v && !v->structuredBindings.empty() && pattern && pattern->kind == TyKind::Auto && pattern != types.decltypeAutoType() && init && init->isKnown())
    if (const Ty* object = types.stripRef(init); object->kind == TyKind::Array)
      return types.qualified(object, pattern->isConst, pattern->isVolatile);
  return deduceAuto(pattern, init, initExpr);
}

auto GlobalScope::Impl::deducedShape(const Ty* pattern, const Ty* deduced) -> const Ty*
{
  if (pattern && containsAuto(pattern) && deduced && deduced->isKnown())
    return deduceAuto(pattern, deduced);
  return types.decay(deduced);
}

auto GlobalScope::Impl::astMentionsAuto(const Type* t) -> bool
{
  for (;;)
  {
    if (!t)
      return false;
    switch (t->kind)
    {
      case NodeKind::AutoType: return true;
      case NodeKind::DecltypeType: return static_cast<const DecltypeType*>(t)->isAuto;
      case NodeKind::ConstType: t = static_cast<const ConstType*>(t)->inner; break;
      case NodeKind::PointerType: t = static_cast<const PointerType*>(t)->pointee; break;
      case NodeKind::LRefType: t = static_cast<const LRefType*>(t)->referenced; break;
      case NodeKind::RRefType: t = static_cast<const RRefType*>(t)->referenced; break;
      default: return false;
    }
  }
}

auto GlobalScope::Impl::astMentionsDecltype(Node* n, int depth) -> bool
{
  if (!n || depth > 16 || !isTypeNode(n))
    return false;
  switch (n->kind)
  {
    case NodeKind::DecltypeType: return !static_cast<DecltypeType*>(n)->isAuto;
    case NodeKind::ConstType: return astMentionsDecltype(static_cast<ConstType*>(n)->inner, depth + 1);
    case NodeKind::PointerType: return astMentionsDecltype(static_cast<PointerType*>(n)->pointee, depth + 1);
    case NodeKind::LRefType: return astMentionsDecltype(static_cast<LRefType*>(n)->referenced, depth + 1);
    case NodeKind::RRefType: return astMentionsDecltype(static_cast<RRefType*>(n)->referenced, depth + 1);
    case NodeKind::NamedType:
      for (const NamePart& part : static_cast<NamedType*>(n)->name.parts)
        for (Node* arg : part.templateArgs)
          if (astMentionsDecltype(arg, depth + 1))
            return true;
      return false;
    default: return false;
  }
}

auto GlobalScope::Impl::mentionsBound(Node* n, const Subst& s, int depth) -> bool
{
  if (!n)
    return false;
  if (depth > 32)
    return true;
  auto bound = [&](uint32_t symbol)
  {
    for (const Subst::Pair& p : s.pairs)
      if (p.param && p.param->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(p.param)->name == symbol)
        return true;
    return false;
  };
  auto inName = [&](const QualifiedName& q)
  {
    if (q.typeQualifier && mentionsBound(q.typeQualifier, s, depth + 1))
      return true;
    for (const NamePart& part : q.parts)
    {
      if (part.symbol != UINT32_MAX && bound(part.symbol))
        return true;
      for (Node* a : part.templateArgs)
        if (mentionsBound(a, s, depth + 1))
          return true;
      if (part.convType && mentionsBound(part.convType, s, depth + 1))
        return true;
    }
    return false;
  };
  auto any = [&](Span<Expr*> list)
  {
    for (Expr* e : list)
      if (mentionsBound(e, s, depth + 1))
        return true;
    return false;
  };
  switch (n->kind)
  {
    case NodeKind::NamedType: return inName(static_cast<NamedType*>(n)->name);
    case NodeKind::PointerType: return mentionsBound(static_cast<PointerType*>(n)->pointee, s, depth + 1);
    case NodeKind::LRefType: return mentionsBound(static_cast<LRefType*>(n)->referenced, s, depth + 1);
    case NodeKind::RRefType: return mentionsBound(static_cast<RRefType*>(n)->referenced, s, depth + 1);
    case NodeKind::ConstType: return mentionsBound(static_cast<ConstType*>(n)->inner, s, depth + 1);
    case NodeKind::ArrayType:
      return mentionsBound(static_cast<ArrayType*>(n)->element, s, depth + 1) || mentionsBound(static_cast<ArrayType*>(n)->size, s, depth + 1);
    case NodeKind::DecltypeType: return mentionsBound(static_cast<DecltypeType*>(n)->expr, s, depth + 1);
    case NodeKind::PackType: return mentionsBound(static_cast<PackType*>(n)->pattern, s, depth + 1);
    case NodeKind::BuiltinType: case NodeKind::AutoType: return false;
    case NodeKind::NameExpr: return inName(static_cast<NameExpr*>(n)->name);
    case NodeKind::LiteralExpr: case NodeKind::ThisExpr: return false;
    case NodeKind::CallExpr: return mentionsBound(static_cast<CallExpr*>(n)->callee, s, depth + 1) || any(static_cast<CallExpr*>(n)->args);
    case NodeKind::MemberExpr: return mentionsBound(static_cast<MemberExpr*>(n)->base, s, depth + 1) || inName(static_cast<MemberExpr*>(n)->member);
    case NodeKind::IndexExpr: return mentionsBound(static_cast<IndexExpr*>(n)->base, s, depth + 1) || any(static_cast<IndexExpr*>(n)->args);
    case NodeKind::UnaryExpr: return mentionsBound(static_cast<UnaryExpr*>(n)->operand, s, depth + 1);
    case NodeKind::BinaryExpr: return mentionsBound(static_cast<BinaryExpr*>(n)->lhs, s, depth + 1) || mentionsBound(static_cast<BinaryExpr*>(n)->rhs, s, depth + 1);
    case NodeKind::ConditionalExpr:
      return mentionsBound(static_cast<ConditionalExpr*>(n)->cond, s, depth + 1) || mentionsBound(static_cast<ConditionalExpr*>(n)->then, s, depth + 1) ||
             mentionsBound(static_cast<ConditionalExpr*>(n)->else_, s, depth + 1);
    case NodeKind::CastExpr: return mentionsBound(static_cast<CastExpr*>(n)->type, s, depth + 1) || mentionsBound(static_cast<CastExpr*>(n)->operand, s, depth + 1);
    case NodeKind::NamedCastExpr:
      return mentionsBound(static_cast<NamedCastExpr*>(n)->type, s, depth + 1) || mentionsBound(static_cast<NamedCastExpr*>(n)->operand, s, depth + 1);
    case NodeKind::SizeofExpr: return mentionsBound(static_cast<SizeofExpr*>(n)->type, s, depth + 1) || mentionsBound(static_cast<SizeofExpr*>(n)->operand, s, depth + 1);
    case NodeKind::FunctionalCastExpr:
      return mentionsBound(static_cast<FunctionalCastExpr*>(n)->type, s, depth + 1) || any(static_cast<FunctionalCastExpr*>(n)->args);
    case NodeKind::BraceInitExpr: return any(static_cast<BraceInitExpr*>(n)->args);
    case NodeKind::ParenExpr: return mentionsBound(static_cast<ParenExpr*>(n)->inner, s, depth + 1);
    case NodeKind::NewExpr: return mentionsBound(static_cast<NewExpr*>(n)->type, s, depth + 1) || any(static_cast<NewExpr*>(n)->args);
    default: return true;
  }
}

auto GlobalScope::Impl::hasLambda(Node* n, int depth) -> bool
{
  if (!n)
    return false;
  if (depth > 32)
    return true;
  auto any = [&](Span<Expr*> list)
  {
    for (Expr* e : list)
      if (hasLambda(e, depth + 1))
        return true;
    return false;
  };
  switch (n->kind)
  {
    case NodeKind::LambdaExpr: return true;
    case NodeKind::NameExpr: case NodeKind::LiteralExpr: case NodeKind::ThisExpr: case NodeKind::SizeofExpr: return false;
    case NodeKind::CallExpr: return hasLambda(static_cast<CallExpr*>(n)->callee, depth + 1) || any(static_cast<CallExpr*>(n)->args);
    case NodeKind::MemberExpr: return hasLambda(static_cast<MemberExpr*>(n)->base, depth + 1);
    case NodeKind::IndexExpr: return hasLambda(static_cast<IndexExpr*>(n)->base, depth + 1) || any(static_cast<IndexExpr*>(n)->args);
    case NodeKind::UnaryExpr: return hasLambda(static_cast<UnaryExpr*>(n)->operand, depth + 1);
    case NodeKind::BinaryExpr: return hasLambda(static_cast<BinaryExpr*>(n)->lhs, depth + 1) || hasLambda(static_cast<BinaryExpr*>(n)->rhs, depth + 1);
    case NodeKind::ConditionalExpr:
      return hasLambda(static_cast<ConditionalExpr*>(n)->cond, depth + 1) || hasLambda(static_cast<ConditionalExpr*>(n)->then, depth + 1) ||
             hasLambda(static_cast<ConditionalExpr*>(n)->else_, depth + 1);
    case NodeKind::CastExpr: return hasLambda(static_cast<CastExpr*>(n)->operand, depth + 1);
    case NodeKind::NamedCastExpr: return hasLambda(static_cast<NamedCastExpr*>(n)->operand, depth + 1);
    case NodeKind::FunctionalCastExpr: return any(static_cast<FunctionalCastExpr*>(n)->args);
    case NodeKind::BraceInitExpr: return any(static_cast<BraceInitExpr*>(n)->args);
    case NodeKind::ParenExpr: return hasLambda(static_cast<ParenExpr*>(n)->inner, depth + 1);
    default: return true;
  }
}

auto GlobalScope::Impl::isBarePack(const Type* pattern) -> bool
{
  if (!pattern || pattern->kind != NodeKind::NamedType)
    return false;
  const QualifiedName& name = static_cast<const NamedType*>(pattern)->name;
  return name.parts.size() == 1 && !name.parts[0].hasTemplateArgs && !name.typeQualifier;
}

auto GlobalScope::Impl::packsNamedIn(Node* n, const Subst& s, std::vector<Decl*>& out, int depth) -> void
{
  if (!n || depth > 32)
    return;
  auto named = [&](uint32_t symbol)
  {
    for (const Subst::Pair& p : s.pairs)
      if (p.param && p.param->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(p.param)->isPack &&
          static_cast<TemplateParamDecl*>(p.param)->name == symbol && p.type && p.type->kind == TyKind::Pack &&
          std::find(out.begin(), out.end(), p.param) == out.end())
        out.push_back(p.param);
  };
  auto inName = [&](const QualifiedName& q)
  {
    packsNamedIn(q.typeQualifier, s, out, depth + 1);
    for (const NamePart& part : q.parts)
    {
      if (part.symbol != UINT32_MAX)
        named(part.symbol);
      for (Node* a : part.templateArgs)
        if (!a || a->kind != NodeKind::PackType) // an inner expansion expands its own packs
          packsNamedIn(a, s, out, depth + 1);
      packsNamedIn(part.convType, s, out, depth + 1);
    }
  };
  switch (n->kind)
  {
    case NodeKind::NamedType: inName(static_cast<NamedType*>(n)->name); break;
    case NodeKind::PointerType: packsNamedIn(static_cast<PointerType*>(n)->pointee, s, out, depth + 1); break;
    case NodeKind::LRefType: packsNamedIn(static_cast<LRefType*>(n)->referenced, s, out, depth + 1); break;
    case NodeKind::RRefType: packsNamedIn(static_cast<RRefType*>(n)->referenced, s, out, depth + 1); break;
    case NodeKind::ConstType: packsNamedIn(static_cast<ConstType*>(n)->inner, s, out, depth + 1); break;
    case NodeKind::ArrayType:
      packsNamedIn(static_cast<ArrayType*>(n)->element, s, out, depth + 1);
      packsNamedIn(static_cast<ArrayType*>(n)->size, s, out, depth + 1);
      break;
    case NodeKind::DecltypeType: packsNamedIn(static_cast<DecltypeType*>(n)->expr, s, out, depth + 1); break;
    case NodeKind::FunctionType:
      packsNamedIn(static_cast<FunctionType*>(n)->result, s, out, depth + 1);
      for (Decl* p : static_cast<FunctionType*>(n)->params)
        if (p && p->kind == NodeKind::ParamDecl)
          packsNamedIn(static_cast<ParamDecl*>(p)->type, s, out, depth + 1);
      break;
    case NodeKind::NameExpr: inName(static_cast<NameExpr*>(n)->name); break;
    case NodeKind::CallExpr:
      packsNamedIn(static_cast<CallExpr*>(n)->callee, s, out, depth + 1);
      for (Expr* a : static_cast<CallExpr*>(n)->args)
        packsNamedIn(a, s, out, depth + 1);
      break;
    case NodeKind::SizeofExpr:
      packsNamedIn(static_cast<SizeofExpr*>(n)->type, s, out, depth + 1);
      packsNamedIn(static_cast<SizeofExpr*>(n)->operand, s, out, depth + 1);
      break;
    case NodeKind::UnaryExpr: packsNamedIn(static_cast<UnaryExpr*>(n)->operand, s, out, depth + 1); break;
    case NodeKind::BinaryExpr:
      packsNamedIn(static_cast<BinaryExpr*>(n)->lhs, s, out, depth + 1);
      packsNamedIn(static_cast<BinaryExpr*>(n)->rhs, s, out, depth + 1);
      break;
    case NodeKind::ParenExpr: packsNamedIn(static_cast<ParenExpr*>(n)->inner, s, out, depth + 1); break;
    default: break;
  }
}

auto GlobalScope::Impl::astNeedsArguments(Node* n, Scope* scope, const LocalStack* locals, int depth) -> bool
{
  if (!n || depth > 12)
    return false;
  switch (n->kind)
  {
    case NodeKind::DecltypeType: return !static_cast<DecltypeType*>(n)->isAuto;
    case NodeKind::PackType: return !isBarePack(static_cast<PackType*>(n)->pattern); // `RemoveReference<Args>...`: per element
    case NodeKind::ConstType: return astNeedsArguments(static_cast<ConstType*>(n)->inner, scope, locals, depth + 1);
    case NodeKind::PointerType: return astNeedsArguments(static_cast<PointerType*>(n)->pointee, scope, locals, depth + 1);
    case NodeKind::LRefType: return astNeedsArguments(static_cast<LRefType*>(n)->referenced, scope, locals, depth + 1);
    case NodeKind::RRefType: return astNeedsArguments(static_cast<RRefType*>(n)->referenced, scope, locals, depth + 1);
    case NodeKind::NamedType:
    {
      auto* named = static_cast<NamedType*>(n);
      if (named->name.parts.size() == 1 && named->name.parts[0].hasTemplateArgs && isTypeTraitName(named->name.parts[0].symbol))
        return true; // `RemoveReference<T>&&` (std::move's result): the trait of the deduced T, not T
      bool hasArgs = false;
      for (const NamePart& part : named->name.parts)
        for (Node* arg : part.templateArgs)
        {
          hasArgs = true;
          if (!arg)
            continue;
          if (isTypeNode(arg))
          {
            if (astNeedsArguments(arg, scope, locals, depth + 1))
              return true;
          }
          else if (arg->kind != NodeKind::LiteralExpr &&
                   !(arg->kind == NodeKind::NameExpr && static_cast<NameExpr*>(arg)->name.parts.size() == 1 &&
                     !static_cast<NameExpr*>(arg)->name.parts[0].hasTemplateArgs))
            return true; // a value computed from the parameters
        }
      if (!hasArgs)
        return false;
      Lookup l = lookupQualified(named->name, scope, locals, depth + 1);
      if (l.decl && l.decl->kind == NodeKind::UsingAliasDecl && !l.decl->templateParams.empty())
      {
        auto* alias = static_cast<UsingAliasDecl*>(l.decl);
        LocalStack own;
        for (Decl* p : alias->templateParams)
          own.push(static_cast<TemplateParamDecl*>(p)->name, p);
        return astNeedsArguments(alias->type, lexicalScopeOf(alias), &own, depth + 1);
      }
      return false;
    }
    default: return false;
  }
}

auto GlobalScope::Impl::dependsOn(const Ty* t, Span<Decl*> params) -> bool
{
  if (!t)
    return false;
  switch (t->kind)
  {
    case TyKind::Param:
      for (Decl* p : params)
        if (p == t->decl)
          return true;
      return false;
    case TyKind::Array:
      if (t->decl)
        for (Decl* p : params)
          if (p == t->decl)
            return true; // `T[N]`
      return dependsOn(t->inner, params);
    case TyKind::Pointer: case TyKind::LRef: case TyKind::RRef: case TyKind::MemberPointer:
    case TyKind::DependentMember:
      return dependsOn(t->inner, params);
    case TyKind::Function: case TyKind::Class: case TyKind::Lambda:
      if (dependsOn(t->inner, params))
        return true;
      for (const Ty* a : t->args)
        if (dependsOn(a, params))
          return true;
      return false;
    default:
      return false;
  }
}

auto GlobalScope::Impl::deduceClassArgs(Span<const Ty*> patterns, Span<const Ty*> args, Subst& out, int depth) -> void
{
  for (size_t i = 0; i < patterns.size(); ++i)
  {
    const Ty* p = patterns[i];
    if (i + 1 == patterns.size() && isPackParam(p))
    {
      if (!out.find(p->decl))
      {
        std::vector<const Ty*> rest;
        for (size_t k = i; k < args.size(); ++k)
          rest.push_back(args[k]);
        out.pairs.push_back({p->decl, types.pack(Span<const Ty*>{rest.data(), uint32_t(rest.size())})});
      }
      return;
    }
    if (i < args.size())
      deduceArgument(p, args[i], out, depth + 1);
  }
}

auto GlobalScope::Impl::deduceArgument(const Ty* pattern, const Ty* arg, Subst& out, int depth) -> void
{
  if (pattern && arg && pattern->kind == TyKind::Param && pattern->args.empty() && arg->isReference() && !pattern->isConst && !pattern->isVolatile)
  {
    if (!out.find(pattern->decl))
      out.pairs.push_back({pattern->decl, arg});
    return;
  }
  deduce(pattern, arg, out, depth);
}

auto GlobalScope::Impl::arrayQualifiersOff(const Ty* a, bool isConst, bool isVolatile) -> const Ty*
{
  if (!a || a->kind != TyKind::Array || !a->inner || (!isConst && !isVolatile))
    return a;
  const Ty* e = a->inner->kind == TyKind::Array ? arrayQualifiersOff(a->inner, isConst, isVolatile)
                : types.qualified(types.unqualified(a->inner), a->inner->isConst && !isConst, a->inner->isVolatile && !isVolatile);
  return e == a->inner ? a : types.array(e, a->count);
}

auto GlobalScope::Impl::deduce(const Ty* pattern, const Ty* arg, Subst& out, int depth) -> void
{
  if (!pattern || !arg || depth > 16 || !arg->isKnown())
    return;
  if (pattern->kind == TyKind::DependentMember)
    return; // `typename T::Unit` is not a deduced context
  if (arg->isBraceList())
  {
    // `{a, b}` deduces only an InitializerList<P> parameter, its P from the elements as from
    // the arguments of a by-value parameter (P = Box given `{cb, cb}` of const Box lvalues),
    // and an array one, `P[N]` ([temp.deduct.call] 1: `const T (&)[N]` given `{1, 2, 3}` is T int,
    // N 3; D, 2026-11-06: no call of `count({1, 2, 3})` resolved); for any other parameter,
    // `const T&` included, it is a non-deduced context.
    const Ty* p = types.unqualified(types.stripRef(pattern));
    if (p->kind == TyKind::Array)
    {
      // P from each element as from a by-value parameter's argument, all of them alike (a list
      // whose elements deduce two types deduces nothing: no call takes it); N the list's length.
      const Ty* elements = arg->inner && arg->inner->kind == TyKind::Pack ? arg->inner : nullptr;
      if (!elements || elements->args.empty())
        return;
      Subst first;
      deduce(p->inner, elements->args[0], first, depth + 1);
      for (size_t i = 1; i < elements->args.size(); ++i)
      {
        Subst each;
        deduce(p->inner, elements->args[i], each, depth + 1);
        for (const Subst::Pair& q : each.pairs)
          if (const Ty* had = first.find(q.param); had && had != q.type)
            return;
      }
      for (const Subst::Pair& q : first.pairs)
        if (!out.find(q.param))
          out.pairs.push_back(q);
      if (p->decl && !out.find(p->decl))
      {
        auto* tp = static_cast<TemplateParamDecl*>(p->decl);
        const Ty* of = tp->valueType ? types.unqualified(convertType(tp->valueType, TypeContext{lexicalScopeOf(tp), nullptr, nullptr}, depth + 1))
                                     : types.sizeType();
        out.pairs.push_back({p->decl, types.valueType(of && of->isKnown() ? of : types.sizeType(), int64_t(elements->args.size()))});
      }
      return;
    }
    if (p->kind != TyKind::Class || p->decl != arg->decl)
      return;
    if (p->args.size() == 1 && arg->args.size() == 1)
      deduce(p->args[0], types.decay(arg->args[0]), out, depth + 1);
    return;
  }
  if (pattern->kind == TyKind::Param && !pattern->args.empty())
  {
    // `Container<T>` (Container a template template parameter) against `Vector<int>`: the
    // parameter binds to the template itself (its class type without arguments), its written
    // arguments to the instantiation's.
    const Ty* a = types.unqualified(types.stripRef(arg));
    // Partial ordering's argument, another template template parameter applied (`UU<int>` of
    // `S<UU<int>>` against `S<TT<T>>`): TT is UU, T int. Until 2026-11-08 nothing deduced from it,
    // so neither of `S<TT<int>>` and `S<TT<T>>` was at least as specialized as the other: ambiguous.
    if (a->kind == TyKind::Param && a->decl && !a->args.empty())
    {
      if (!out.find(pattern->decl))
        out.pairs.push_back({pattern->decl, types.param(a->decl)});
      for (size_t i = 0; i < pattern->args.size(); ++i)
      {
        const Ty* p = pattern->args[i];
        if (i + 1 == pattern->args.size() && isPackParam(p))
        {
          if (!out.find(p->decl))
          {
            std::vector<const Ty*> rest(a->args.begin() + std::min(i, size_t(a->args.size())), a->args.end());
            out.pairs.push_back({p->decl, types.pack(Span<const Ty*>{rest.data(), uint32_t(rest.size())})});
          }
          break;
        }
        // ([temp.deduct.type] 9: a pattern that is no expansion against one that is fails.)
        if (i < a->args.size() && isPackParam(a->args[i]))
        {
          out.failed = true;
          return;
        }
        if (i < a->args.size())
          deduce(p, a->args[i], out, depth + 1);
      }
      return;
    }
    if (a->kind != TyKind::Class || !a->decl)
      return;
    // An instance of an explicit or partial specialization (`One<int>` with `template<> struct
    // One<int>`): the template is the primary, its arguments the primary's. Until 2026-11-08 the
    // parameter was the specialization's own declaration with its own arguments (none), so
    // `Q<TT<T>>` did not match Q<One<int>> and `Q<TT<Ts...>>` matched it with Ts empty.
    if (a->decl->kind == NodeKind::ClassDecl && (a->decl->flags & DF_Specialization) && !static_cast<ClassDecl*>(a->decl)->name.empty())
    {
      auto* c = static_cast<ClassDecl*>(a->decl);
      Lookup l;
      if (lookupInScope(lexicalScopeOf(c), c->name.last().symbol, l, 0) && l.overloads)
        if (Decl* primary = primaryOf(*l.overloads); primary && primary != c)
          if (std::vector<const Ty*> args = primaryArgumentsOf(a, primary, depth + 1); !args.empty())
            a = types.classType(primary, Span<const Ty*>{args.data(), uint32_t(args.size())});
    }
    if (!out.find(pattern->decl))
      out.pairs.push_back({pattern->decl, types.classType(a->decl, {})});
    std::vector<const Ty*> given; // the instance's arguments, a pack among them by its elements
    for (const Ty* e : a->args)
      if (!spliceIfPack(e, given))
        given.push_back(e);
    for (size_t i = 0; i < pattern->args.size(); ++i)
    {
      const Ty* p = pattern->args[i];
      if (i + 1 == pattern->args.size() && isPackParam(p))
      {
        // `Spec<Args...>`: the pack takes the rest of the arguments.
        if (!out.find(p->decl))
        {
          std::vector<const Ty*> rest(given.begin() + std::min(i, given.size()), given.end());
          out.pairs.push_back({p->decl, types.pack(Span<const Ty*>{rest.data(), uint32_t(rest.size())})});
        }
        break;
      }
      if (i < given.size())
        deduce(p, given[i], out, depth + 1);
    }
    return;
  }
  if (pattern->kind == TyKind::Param)
  {
    // A non-type parameter given a value of another type deduces nothing ([temp.deduct.type] 20):
    // `template<int I> struct N<I>` does not match N<'a'> nor N<nullptr> of `template<auto V>
    // struct N`, which took the int one for both, the value converted. g++ has it so; clang calls
    // N<'a'> ambiguous between `int I` and `char C`.
    if (arg->kind == TyKind::Value && arg->inner && pattern->decl && pattern->decl->kind == NodeKind::TemplateParamDecl)
      if (auto* tp = static_cast<TemplateParamDecl*>(pattern->decl); tp->valueType)
        if (const Ty* of = convertType(tp->valueType, TypeContext{lexicalScopeOf(tp), nullptr, nullptr}, depth + 1);
            of && of->isKnown() && !types.isDependent(of) && of->kind != TyKind::Auto && !(of->inner && of->inner->kind == TyKind::Auto) &&
            types.unqualified(of) != types.unqualified(arg->inner))
          return;
    // `const T` and `volatile T` given a function type deduce nothing: no function type has a
    // qualifier, so none is `const T` (S<const T, U> matched S<int(char), U>, T the function; clang
    // and g++ have it so, as for `const T*` given a function below).
    if (depth > 0 && (pattern->isConst || pattern->isVolatile) && types.stripRef(arg)->kind == TyKind::Function)
    {
      out.failed = true;
      return;
    }
    // `T value` takes the decayed argument; inside a pointer or a class (`T*`, `Vector<T>`) T
    // keeps the argument's qualifiers: `const int*` against `T*` makes T `const int`, but
    // against `const T*` int (the pattern's own qualifiers come off, as C++ deduces).
    if (!out.find(pattern->decl))
    {
      const Ty* bound = depth == 0 ? types.decay(arg) : types.stripRef(arg);
      if (depth > 0 && bound && (pattern->isConst || pattern->isVolatile))
        bound = types.qualified(types.unqualified(bound), bound->isConst && !pattern->isConst, bound->isVolatile && !pattern->isVolatile);
      if (depth > 0 && bound && bound->kind == TyKind::Array)
        bound = arrayQualifiersOff(bound, pattern->isConst, pattern->isVolatile);
      out.pairs.push_back({pattern->decl, bound});
    }
    return;
  }
  if (pattern->isReference())
  {
    // `const T&` binds T to the argument without its reference and top-level const; `T&`
    // keeps the argument's const (`forward(const Event&)` deduces T = const Event, as C++
    // does); the forwarding reference `T&&` given an lvalue of type U binds T to U& (which
    // collapses the parameter to U&), so that the callee sees an lvalue and copies rather
    // than moves.
    const Ty* a = types.stripRef(arg);
    if (pattern->inner->kind == TyKind::Param && pattern->inner->args.empty())
    {
      bool forwarding = pattern->kind == TyKind::RRef && !pattern->inner->isConst && depth == 0;
      if (!out.find(pattern->inner->decl))
      {
        // (of the argument's const and volatile what the parameter does not take itself: `const T&` given a volatile int is T volatile int)
        const Ty* bound = pattern->inner->isConst || pattern->inner->isVolatile ? types.qualified(types.unqualified(a), a->isConst && !pattern->inner->isConst, a->isVolatile && !pattern->inner->isVolatile) : a;
        if (bound->kind == TyKind::Array)
          bound = arrayQualifiersOff(bound, pattern->inner->isConst, pattern->inner->isVolatile);
        if (forwarding && (arg->kind == TyKind::LRef || a->kind == TyKind::Function))
          bound = types.lref(a); // (a function is an lvalue: T = int (&)(int) given fun)
        out.pairs.push_back({pattern->inner->decl, bound});
      }
      return;
    }
    deduce(pattern->inner, a, out, depth + 1);
    return;
  }
  const Ty* a = types.stripRef(arg);
  switch (pattern->kind)
  {
    case TyKind::Pointer:
      if (a->kind == TyKind::Pointer)
        deduce(pattern->inner, a->inner, out, depth + 1);
      else if (a->kind == TyKind::Array)
        deduce(pattern->inner, a->inner, out, depth + 1);
      else if (a->kind == TyKind::Function && pattern->inner &&
               (pattern->inner->kind == TyKind::Function || (depth == 0 && !pattern->inner->isConst && !pattern->inner->isVolatile)))
        deduce(pattern->inner, a, out, depth + 1); // `StringType (*toString)(T)` or `T*` given a function: its pointer ([temp.deduct.call] 2; `const T*` deduces nothing, as clang and g++ have it)
      break;
    case TyKind::Array:
      if (a->kind == TyKind::Array || a->kind == TyKind::Pointer)
        deduce(pattern->inner, a->inner, out, depth + 1);
      if (pattern->decl && a->kind == TyKind::Array && a->count > 0 && !out.find(pattern->decl))
      {
        // `const T (&array)[N]` given an int[5]: N = 5, a constant of N's declared type.
        auto* p = static_cast<TemplateParamDecl*>(pattern->decl);
        const Ty* of = p->valueType ? types.unqualified(convertType(p->valueType, TypeContext{lexicalScopeOf(p), nullptr, nullptr}, depth + 1))
                                    : types.sizeType();
        out.pairs.push_back({pattern->decl, types.valueType(of && of->isKnown() ? of : types.sizeType(), int64_t(a->count))});
      }
      else if (pattern->decl && a->kind == TyKind::Array && a->decl && !out.find(pattern->decl))
        out.pairs.push_back({pattern->decl, types.param(a->decl)}); // `T[N]` from `U[M][K]` (partial ordering): N is M, the other's bound
      break;
    case TyKind::MemberPointer:
      if (a->kind == TyKind::MemberPointer)
      {
        deduce(pattern->inner, a->inner, out, depth + 1);
        if (!pattern->args.empty() && !a->args.empty())
          deduce(pattern->args[0], a->args[0], out, depth + 1);
      }
      break;
    case TyKind::Class:
      if (a->kind == TyKind::Class && pattern->args.size() == 1 && !a->args.empty() && a->decl != pattern->decl &&
          isLibraryFunction(pattern->decl, false) && isLibraryFunction(a->decl, true))
      {
        // `Function<T>` (T a parameter) given FunctionImpl<R, Args...>: T is the signature R(Args...).
        std::vector<const Ty*> params;
        for (size_t i = 1; i < a->args.size(); ++i)
          if (!spliceIfPack(a->args[i], params))
            params.push_back(a->args[i]);
        deduce(pattern->args[0], types.function(a->args[0], Span<const Ty*>{params.data(), uint32_t(params.size())}, false), out, depth + 1);
      }
      else if (a->kind == TyKind::Class && a->decl == pattern->decl)
        deduceClassArgs(pattern->args, a->args, out, depth);
      else if (std::vector<const Ty*> given; a->kind == TyKind::Class && !pattern->args.empty() &&
               !(given = primaryArgumentsOf(types.unqualified(a), pattern->decl, depth + 1)).empty())
      {
        // `Owner<T>` given an instance of Owner's specialisation `Owner<T, Del<T>>`: deduced from
        // the primary template's arguments it stands for (Owner<int, Del<int>>).
        deduceClassArgs(pattern->args, Span<const Ty*>{given.data(), uint32_t(given.size())}, out, depth);
      }
      else if (a->kind == TyKind::Class && a->decl && pattern->decl && pattern->decl->ownScope && !pattern->args.empty())
      {
        // `const Opt<T>&` given a class derived from Opt<int>: T from that base, as C++ has it.
        // Of several bases of the template, direct or not (Holder<2, int>, Holder<1, double> of
        // a tuple), the one that the pattern with what is bound already matches
        // (`holderOf<1>(t)`, `Holder<I, X>&` with I = 1); none when more than one does.
        std::vector<const Ty*> bases;
        instantiationsOfBase(a, classScopeOf(a), pattern->decl->ownScope, depth + 1, bases);
        bases.erase(std::remove(bases.begin(), bases.end(), a), bases.end());
        if (bases.size() == 1)
          deduceClassArgs(pattern->args, bases[0]->args, out, depth);
        else if (bases.size() > 1)
        {
          std::optional<Subst> only;
          size_t matching = 0;
          for (const Ty* base : bases)
          {
            Subst tried = out;
            deduceClassArgs(pattern->args, base->args, tried, depth);
            const Ty* deduced = substitute(pattern, tried);
            if (deduced && !types.isDependent(deduced) && !sameArgument(types.unqualified(deduced), types.unqualified(base)))
              continue; // another instance than the pattern names
            if (matching++ == 0)
              only = std::move(tried);
          }
          if (matching == 1)
            out = std::move(*only);
        }
      }
      break;
    case TyKind::Function:
      if (a->kind == TyKind::Function || a->kind == TyKind::Lambda)
      {
        // A function type's return and parameter types are deduced as they are, references kept
        // (`ValueType (Desc::*)() const` given `&Desc::getName` returning `const Name&`).
        deduceArgument(pattern->inner, a->inner, out, depth + 1);
        // `R (*)(A...) noexcept(N)`: N, as the function is.
        if (pattern->decl && a->kind == TyKind::Function && !out.find(pattern->decl))
          out.pairs.push_back({pattern->decl, types.valueType(types.boolType(), a->isNoexcept() ? 1 : 0)});
        // `R (*)(A...)`: the pack the parameter types from its place on (none for `short()`).
        size_t n = pattern->args.size();
        bool packLast = n > 0 && isPackParam(pattern->args[n - 1]);
        for (size_t i = 0; i < (packLast ? n - 1 : n) && i < a->args.size(); ++i)
          deduceArgument(pattern->args[i], a->args[i], out, depth + 1);
        if (packLast && a->kind == TyKind::Function && a->args.size() + 1 >= n && !out.find(pattern->args[n - 1]->decl))
        {
          std::vector<const Ty*> rest(a->args.begin() + (n - 1), a->args.end());
          out.pairs.push_back({pattern->args[n - 1]->decl, types.pack(Span<const Ty*>{rest.data(), uint32_t(rest.size())})});
        }
      }
      break;
    default:
      break;
  }
}

auto GlobalScope::Impl::packParamOf(const Ty* t) -> Decl*
{
  for (int i = 0; t && i < 8; ++i)
  {
    if (t->kind == TyKind::Param)
      return t->decl && t->decl->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(t->decl)->isPack ? t->decl : nullptr;
    if (t->isReference())
      t = t->inner;
    else
      return nullptr; // `Args*...`: each element from its argument's pointee (deduceCall's general case)
  }
  return nullptr;
}

auto GlobalScope::Impl::packParamsIn(const Ty* t, std::vector<Decl*>& out, int depth) -> void
{
  if (!t || depth > 16)
    return;
  if (t->kind == TyKind::Param && t->decl && t->decl->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(t->decl)->isPack)
  {
    if (std::find(out.begin(), out.end(), t->decl) == out.end())
      out.push_back(t->decl);
    return;
  }
  packParamsIn(t->inner, out, depth + 1);
  for (const Ty* a : t->args)
    packParamsIn(a, out, depth + 1);
}

auto GlobalScope::Impl::fillDefaultTemplateArgs(FunctionDecl* f, Subst& s) -> bool
{
  LocalStack own;
  for (Decl* p : f->templateParams)
    own.push(static_cast<TemplateParamDecl*>(p)->name, p);
  for (Decl* d : f->templateParams)
  {
    auto* p = static_cast<TemplateParamDecl*>(d);
    if (s.find(d) || !p->defaultArg || p->isPack)
      continue;
    TypeContext ctx{lexicalScopeOf(f), &own, &s, f->file ? f->file->tokens : nullptr, f->file ? f->file->text : std::string_view()};
    const Ty* t = templateArgument(p->defaultArg, d, ctx, 0);
    if (!t || !t->isKnown())
      return false;
    s.pairs.push_back({d, t});
  }
  // C++: a trailing pack nothing deduces is empty (`assignIf<Index + 1, Tail...>(widget)` with
  // Tail empty names `assignIf<Index + 1>`, whose `template<size_t Index, class Head, class...
  // Tail>` has an empty Tail). One a parameter takes is the call's to deduce.
  if (!f->templateParams.empty())
    if (Decl* last = f->templateParams[f->templateParams.size() - 1]; static_cast<TemplateParamDecl*>(last)->isPack && !s.find(last))
    {
      bool deducible = false;
      if (const Ty* ft = declaredTypeOf(f, nullptr, 0); ft && ft->kind == TyKind::Function)
        for (const Ty* pt : ft->args)
        {
          std::vector<Decl*> packs;
          packParamsIn(pt, packs);
          deducible = deducible || std::find(packs.begin(), packs.end(), last) != packs.end();
        }
      if (!deducible)
        s.pairs.push_back({last, types.pack({})});
    }
  return true;
}

auto GlobalScope::Impl::isMemberSpecialization(const FunctionDecl* f) -> bool
{
  if (!(f->flags & DF_Specialization) || !f->templateParams.empty() || f->name.parts.size() < 2 || f->name.last().hasTemplateArgs)
    return false;
  if (f->attributes & ATTR_MemberSpecialization)
    return true; // the instance named through an alias (finishIndex)
  for (size_t i = 0; i + 1 < f->name.parts.size(); ++i)
    if (f->name.parts[i].hasTemplateArgs)
      return true;
  return false;
}

auto GlobalScope::Impl::specializedClassOf(Decl* d, const QualifiedName& name) -> const Ty*
{
  QualifiedName qualifier = name;
  qualifier.parts.count -= 1;
  NamedType written{};
  written.kind = NodeKind::NamedType;
  written.token = d->token;
  written.name = qualifier;
  TypeContext ctx{lexicalScopeOf(d), nullptr, nullptr, d->file ? d->file->tokens : nullptr, d->file ? d->file->text : std::string_view()};
  const Ty* t = convertType(&written, ctx, 0);
  return t && t->kind == TyKind::Class && !types.isDependent(t) ? t : nullptr;
}

auto GlobalScope::Impl::variableDefinitionFor(VariableDecl* v, const Subst* s) -> VariableDecl*
{
  if (s && !s->pairs.empty() && v->parent && v->parent->kind == NodeKind::ClassDecl)
    if (auto it = variableSpecializations.find(v); it != variableSpecializations.end())
      for (VariableDecl* candidate : it->second)
      {
        TokenScope tokenScope(candidate->file ? candidate->file->tokens : nullptr, candidate->file ? candidate->file->text : std::string_view());
        const Ty* written = specializedClassOf(candidate, candidate->name);
        if (!written)
          continue;
        Subst ws = substFor(written);
        bool same = true;
        for (Decl* p : effectiveParams(v->parent))
          same = same && ws.find(p) && ws.find(p) == s->find(p);
        if (same)
          return candidate;
      }
  auto it = variableDefinitions.find(v);
  return it != variableDefinitions.end() ? it->second : v;
}

auto GlobalScope::Impl::fullSpecializationScope(const QualifiedName& qualifier, Scope* context, const LocalStack* locals, const ParsedFile* file, uint32_t token) -> Scope*
{
  if (qualifier.empty() || !qualifier.last().hasTemplateArgs)
    return nullptr;
  NamedType written{};
  written.kind = NodeKind::NamedType;
  written.token = token;
  written.name = qualifier;
  TokenScope tokenScope(file ? file->tokens : nullptr, file ? file->text : std::string_view());
  TypeContext ctx{context, locals, nullptr, file ? file->tokens : nullptr, file ? file->text : std::string_view()};
  const Ty* t = convertType(&written, ctx, 0);
  if (!t || t->kind != TyKind::Class || !t->decl || !(t->decl->flags & DF_Specialization) || !t->decl->templateParams.empty())
    return nullptr;
  return t->decl->ownScope;
}

auto GlobalScope::Impl::partialSpecializationScope(const QualifiedName& qualifier, const Lookup& l, Scope* context, Span<Decl*> defParams, const ParsedFile* file) -> Scope*
{
  if (qualifier.empty() || !qualifier.last().hasTemplateArgs || !l.overloads || defParams.empty())
    return nullptr;
  Decl* primary = primaryOf(*l.overloads);
  const NamePart& part = qualifier.last();
  std::vector<const Ty*> written;
  {
    LocalStack params;
    for (Decl* p : defParams)
      params.push(static_cast<TemplateParamDecl*>(p)->name, p);
    TokenScope tokenScope(file ? file->tokens : nullptr, file ? file->text : std::string_view());
    TypeContext dctx{context, &params, nullptr, file ? file->tokens : nullptr, file ? file->text : std::string_view()};
    for (size_t i = 0; i < part.templateArgs.size(); ++i)
      written.push_back(templateArgument(part.templateArgs[i], parameterFor(primary, i), dctx, 0));
  }
  for (const Entry& e : *l.overloads)
  {
    if (!e.decl || e.decl->kind != NodeKind::ClassDecl || !(e.decl->flags & DF_Specialization) || !e.decl->ownScope)
      continue;
    auto* c = static_cast<ClassDecl*>(e.decl);
    if (!c->isDefinition || c->name.empty() || c->templateParams.size() != defParams.size() ||
        c->name.last().templateArgs.size() != written.size())
      continue;
    LocalStack own;
    for (Decl* p : c->templateParams)
      own.push(static_cast<TemplateParamDecl*>(p)->name, p);
    Subst s;
    for (size_t i = 0; i < defParams.size(); ++i)
      s.pairs.push_back({c->templateParams[i], types.param(defParams[i])});
    TokenScope tokenScope(c->file ? c->file->tokens : nullptr, c->file ? c->file->text : std::string_view());
    TypeContext cctx{lexicalScopeOf(c), &own, nullptr, c->file ? c->file->tokens : nullptr, c->file ? c->file->text : std::string_view()};
    bool same = true;
    for (size_t i = 0; i < written.size() && same; ++i)
      same = sameArgument(substitute(templateArgument(c->name.last().templateArgs[i], parameterFor(primary, i), cctx, 0), s), written[i]);
    if (same)
      return c->ownScope;
  }
  return nullptr;
}

auto GlobalScope::Impl::memberSpecializationIsFor(FunctionDecl* f, const Ty* classTy) -> bool
{
  TokenScope tokenScope(f->file ? f->file->tokens : nullptr, f->file ? f->file->text : std::string_view());
  const Ty* written = specializedClassOf(f);
  if (!written || !classTy || !classTy->decl)
    return false;
  Subst ws = substFor(written), cs = substFor(classTy);
  for (Decl* p : effectiveParams(classTy->decl))
    if (!ws.find(p) || ws.find(p) != cs.find(p))
      return false;
  return true;
}

auto GlobalScope::Impl::isFunctionSpecialization(const FunctionDecl* f) -> bool
{
  return (f->flags & DF_Specialization) && f->isTemplate && f->templateParams.empty() && !isMemberSpecialization(f);
}

auto GlobalScope::Impl::functionSpecializationFor(FunctionDecl* f, const Subst& args) -> FunctionDecl*
{
  if (f->templateParams.empty() || f->name.empty())
    return nullptr;
  uint32_t symbol = functionSymbol(f->name);
  std::vector<FunctionDecl*> candidates;
  auto consider = [&](Decl* d)
  {
    if (d && d != f && d->kind == NodeKind::FunctionDecl && isFunctionSpecialization(static_cast<FunctionDecl*>(d)) &&
        static_cast<FunctionDecl*>(d)->body && functionSymbol(static_cast<FunctionDecl*>(d)->name) == symbol)
      candidates.push_back(static_cast<FunctionDecl*>(d));
  };
  if (Scope* s = lexicalScopeOf(f))
    if (const std::vector<Entry>* entries = s->find(symbol))
      for (const Entry& e : *entries)
        consider(e.decl);
  if (f->parent && f->parent->kind == NodeKind::ClassDecl && f->parent->ownScope)
    for (FunctionDecl* m : f->parent->ownScope->outOfLineMembers) // `template<> void Box::put<int>(int)`
      consider(m);
  const Ty* primary = declaredTypeOf(f, nullptr, 0);
  if (!primary || primary->kind != TyKind::Function)
    return nullptr;
  for (FunctionDecl* spec : candidates)
  {
    TokenScope tokenScope(spec->file ? spec->file->tokens : nullptr, spec->file ? spec->file->text : std::string_view());
    const Ty* st = declaredTypeOf(spec, nullptr, 0);
    if (!st || st->kind != TyKind::Function)
      continue;
    Subst written;
    TypeContext ctx{lexicalScopeOf(spec), nullptr, nullptr, spec->file ? spec->file->tokens : nullptr, spec->file ? spec->file->text : std::string_view()};
    const NamePart& last = spec->name.last();
    for (size_t i = 0; i < last.templateArgs.size() && i < f->templateParams.size(); ++i)
      written.pairs.push_back({f->templateParams[i], templateArgument(last.templateArgs[i], f->templateParams[i], ctx, 0)});
    deduceCall(f, primary, std::vector<const Ty*>(st->args.begin(), st->args.end()), written);
    // `template<> SpriteDrawOrder* DrawOrderProvider::allocate()`: T only in the result type.
    bool allBound = true;
    for (Decl* p : f->templateParams)
      allBound = allBound && written.find(p);
    if (!allBound && primary->inner && st->inner)
      deduce(primary->inner, st->inner, written, 0);
    bool same = true;
    for (Decl* p : f->templateParams)
      same = same && written.find(p) && written.find(p) == args.find(p);
    if (same && substitute(primary, written) == st)
      return spec;
  }
  return nullptr;
}

auto GlobalScope::Impl::typeAs(FunctionDecl* candidate, FunctionDecl* f) -> const Ty*
{
  const Ty* t = declaredTypeOf(candidate, nullptr, 0);
  if (candidate == f)
    return t;
  // The definition names the class's parameters first (`template<typename T, int N> void
  // Ring<T, N>::push(...)`), then the member's own; the declaration inside the class has only
  // its own, the class's being in scope.
  Span<Decl*> classParams; // a nested class's are the enclosing template's (`Outer<K, V>::Inner::load`)
  if (f->parent && (f->parent->kind == NodeKind::ClassDecl || f->parent->kind == NodeKind::EnumDecl))
    classParams = effectiveParams(f->parent);
  if (candidate->templateParams.size() != classParams.size() + f->templateParams.size())
    return t;
  Subst s;
  for (size_t i = 0; i < classParams.size(); ++i)
    s.pairs.push_back({candidate->templateParams[i], types.param(classParams[i])});
  for (size_t i = 0; i < f->templateParams.size(); ++i)
    s.pairs.push_back({candidate->templateParams[classParams.size() + i], types.param(f->templateParams[i])});
  return s.pairs.empty() ? t : substitute(t, s);
}

auto GlobalScope::Impl::matchedType(FunctionDecl* f) -> const Ty*
{
  const Ty* t = declaredTypeOf(f, nullptr, 0);
  Subst s;
  if (f->parent && (f->parent->kind == NodeKind::ClassDecl || f->parent->kind == NodeKind::EnumDecl))
    for (Decl* p : effectiveParams(f->parent))
      s.pairs.push_back({p, types.param(p)});
  for (Decl* p : f->templateParams)
    s.pairs.push_back({p, types.param(p)});
  return s.pairs.empty() ? t : substitute(t, s);
}

auto GlobalScope::Impl::deduceCall(FunctionDecl* f, const Ty* fnTy, const std::vector<const Ty*>& argTypes, Subst& out) -> void
{
  if (!fnTy || fnTy->kind != TyKind::Function)
    return;
  deduceParams(f->params, fnTy->args, argTypes, out);
}

auto GlobalScope::Impl::deduceParams(Span<Decl*> params, Span<const Ty*> patterns, const std::vector<const Ty*>& argTypes, Subst& out) -> void
{
  size_t i = 0;
  for (size_t p = 0; p < params.size() && i < patterns.size(); ++p)
  {
    auto* pd = static_cast<ParamDecl*>(params[p]);
    const Ty* pattern = patterns[i];
    if (pd->isPack)
    {
      if (Decl* packDecl = packParamOf(pattern))
      {
        if (!out.find(packDecl))
        {
          bool forwarding = pattern->kind == TyKind::RRef && pattern->inner->kind == TyKind::Param && !pattern->inner->isConst;
          std::vector<const Ty*> elems;
          for (size_t j = i; j < argTypes.size(); ++j)
          {
            const Ty* at = argTypes[j];
            if (at && at->isBraceList())
              return; // `const Args&...` given `{a, b}`: a non-deduced context, the pack stays unbound
            if (!at)
              elems.push_back(types.unknown());
            else if (forwarding && (at->kind == TyKind::LRef || at->kind == TyKind::Function))
              elems.push_back(types.lref(types.stripRef(at))); // `emplace_back(name)`: Args = String&
            else if (forwarding)
            {
              // A const prvalue of a class, `move(constObject)`: Args = const Program, as a single
              // T&& deduces; a prvalue of a scalar is never const ([expr.type]).
              const Ty* e = types.stripRef(at);
              if (at->kind != TyKind::RRef && e->kind != TyKind::Class && e->kind != TyKind::Array)
                e = types.unqualified(e);
              elems.push_back(e);
            }
            else if (pattern->isReference())
            {
              // `const Ts&...`, `Ts&...`: each element as a single `const T&` or `T&` deduces, an
              // array or a function as it is (it decayed: `sz(arr)` took sizeof of a pointer, and
              // `const Ts&...` given a function lost to `Ts&&...`).
              Subst one;
              deduce(pattern, at, one);
              const Ty* e = one.find(packDecl);
              elems.push_back(e ? e : types.decay(at));
            }
            else
              elems.push_back(types.decay(at));
          }
          out.pairs.push_back({packDecl, types.pack(Span<const Ty*>{elems.data(), uint32_t(elems.size())})});
        }
      }
      else
      {
        // A pattern built around the packs (`auto C::*... members`, `const Vector<Args>&...`):
        // every remaining argument deduces one element of each, the elements explicit
        // arguments gave staying as given.
        std::vector<Decl*> packs;
        packParamsIn(pattern, packs);
        std::vector<std::vector<const Ty*>> elems(packs.size());
        for (size_t j = i; j < argTypes.size() && !packs.empty(); ++j)
        {
          const Ty* at = argTypes[j];
          if (!at || !at->isKnown() || at->isBraceList())
            return;
          Subst one;
          for (Decl* pack : packs)
            if (const Ty* given = out.find(pack); given && given->kind == TyKind::Pack && j - i < given->args.size())
              one.pairs.push_back({pack, given->args[j - i]});
          deduce(pattern, at, one);
          for (size_t k = 0; k < packs.size(); ++k)
          {
            const Ty* e = one.find(packs[k]);
            if (!e)
              return;
            elems[k].push_back(e);
          }
        }
        for (size_t k = 0; k < packs.size(); ++k)
          if (!out.find(packs[k]))
            out.pairs.push_back({packs[k], types.pack(Span<const Ty*>{elems[k].data(), uint32_t(elems[k].size())})});
      }
      return;
    }
    if (i < argTypes.size())
      deduce(pattern, argTypes[i], out);
    ++i;
  }
}

// (whoever asks, from how deep: the match is computed as if asked from the top, CachedEvaluation;
// a type nested 20 deep got the primary template from a bound here)
auto GlobalScope::Impl::resolveSpecialization(const Ty* classTy, int /*depth*/) -> const Ty*
{
  Decl* d = classTy ? classTy->decl : nullptr;
  if (!d || d->kind != NodeKind::ClassDecl || d->templateParams.empty() || (d->flags & DF_Specialization) || !(d->attributes & ATTR_Specialized) ||
      classTy->args.empty())
    return classTy;
  const QualifiedName& name = static_cast<ClassDecl*>(d)->name;
  if (name.empty())
    return classTy;
  bool known = classTy->isKnown() && !types.isDependent(classTy) && !mentionsUnknown(classTy);
  if (known)
  {
    std::lock_guard lock(specializationCacheMutex);
    if (auto it = specializationCache.find(classTy); it != specializationCache.end())
    {
      replayUses(it->second.uses);
      // (past a bound, no specialization was matched: as computing it here would have tripped)
      return cachedNestingFits(it->second.needed) ? it->second.type : classTy;
    }
  }
  CollectUses collected;
  CachedEvaluation evaluation;
  if (!evaluation.ok)
    return classTy;
  Lookup l;
  if (!lookupInScope(lexicalScopeOf(d), name.last().symbol, l, 1) || !l.overloads)
    return classTy;
  std::vector<const Ty*> args(classTy->args.begin(), classTy->args.end());
  const Ty* spec = matchSpecialization(l, args, 1);
  const Ty* result = spec ? spec : classTy;
  if (known && !evaluation.tripped())
  {
    std::lock_guard lock(specializationCacheMutex);
    specializationCache.emplace(classTy, Specialization{result, collected.uses, evaluation.needed()});
  }
  return result;
}

auto GlobalScope::Impl::primaryArgumentsOf(const Ty* t, Decl* primary, int depth) -> std::vector<const Ty*>
{
  std::vector<const Ty*> out;
  auto* c = t && t->kind == TyKind::Class && t->decl && t->decl->kind == NodeKind::ClassDecl && (t->decl->flags & DF_Specialization)
              ? static_cast<ClassDecl*>(t->decl) : nullptr;
  if (!c || c->name.empty() || !primary || primary->kind != NodeKind::ClassDecl || static_cast<ClassDecl*>(primary)->name.empty() ||
      static_cast<ClassDecl*>(primary)->name.last().symbol != c->name.last().symbol || lexicalScopeOf(primary) != lexicalScopeOf(c))
    return out;
  std::vector<const Ty*> patterns;
  if (!specializationPatterns(c, primary, patterns, depth))
    return out;
  Subst s = substFor(t);
  for (const Ty* p : patterns)
    if (const Ty* sub = substitute(p, s, depth + 1); !spliceIfPack(sub, out))
      out.push_back(sub);
  return out;
}

auto GlobalScope::Impl::spliceIfPack(const Ty* sub, std::vector<const Ty*>& out) -> bool
{
  if (!sub)
    return false;
  if (sub->kind == TyKind::Pack)
  {
    for (const Ty* e : sub->args)
      out.push_back(e);
    return true;
  }
  if ((sub->kind == TyKind::LRef || sub->kind == TyKind::RRef || sub->kind == TyKind::Pointer) && sub->inner && sub->inner->kind == TyKind::Pack)
  {
    for (const Ty* e : sub->inner->args)
      out.push_back(sub->kind == TyKind::LRef ? types.lref(e) : sub->kind == TyKind::RRef ? types.rref(e)
                                              : types.qualified(types.pointer(e), sub->isConst, sub->isVolatile));
    return true;
  }
  return false;
}

// A type's own parts (arguments, a pointee) keep the depth, as a written type's do (convertType): a
// type 17 class templates deep kept its parameters past the bound (a function template's return type
// `W<W<...<T>...>>` was unknown). What leads to another declaration (resolveSpecialization,
// memberTypeOf) takes a step.
auto GlobalScope::Impl::substitute(const Ty* t, const Subst& s, int depth) -> const Ty*
{
  if (!t || s.empty() || depth > 16)
    return t;
  switch (t->kind)
  {
    case TyKind::Param:
      if (const Ty* r = s.find(t->decl))
      {
        if (!t->args.empty() && r->kind == TyKind::Class && r->decl)
        {
          // `Container<T>` with Container bound to a class template: that template with the
          // arguments (a pack's elements for `Spec<Args...>`).
          std::vector<const Ty*> args;
          for (const Ty* a : t->args)
            if (const Ty* sub = substitute(a, s, depth); !spliceIfPack(sub, args))
              args.push_back(sub);
          const Ty* applied = resolveSpecialization(types.classType(r->decl, Span<const Ty*>{args.data(), uint32_t(args.size())}), depth + 1);
          return types.qualified(applied, t->isConst, t->isVolatile);
        }
        if (!t->args.empty() && r->kind == TyKind::Param && r->decl && r->args.empty())
        {
          // `TT<T>` with TT bound to another template template parameter (partial ordering): that
          // one applied to the arguments (a pack's elements for `TT<Ts...>`).
          std::vector<const Ty*> args;
          for (const Ty* a : t->args)
            if (const Ty* sub = substitute(a, s, depth); !spliceIfPack(sub, args))
              args.push_back(sub);
          return types.qualified(types.param(r->decl, Span<const Ty*>{args.data(), uint32_t(args.size())}), t->isConst, t->isVolatile);
        }
        return types.qualified(r, t->isConst || r->isConst, t->isVolatile || r->isVolatile); // T = const int keeps its const
      }
      if (!t->args.empty())
      {
        std::vector<const Ty*> args;
        for (const Ty* a : t->args)
          args.push_back(substitute(a, s, depth));
        return types.qualified(types.param(t->decl, Span<const Ty*>{args.data(), uint32_t(args.size())}), t->isConst, t->isVolatile);
      }
      return t;
    case TyKind::DependentMember:
    {
      // `T::Unit` with T now a class, `Num<I>::Type` with I now known: the member type inside
      // it (inside the specialisation the arguments pick, when there is one).
      const Ty* sub = substitute(t->inner, s, depth);
      if (sub && sub->kind == TyKind::Class && !types.isDependent(sub))
        sub = resolveSpecialization(sub, depth + 1);
      // (Also the current instantiation, the class with its own parameters: `typename ID<T,
      // I>::IndexType` in `ID<T, I>::loadIndex`'s definition is ID's IndexType, I.)
      // (And a dependent instance of a template that has no specialisations, `Map<int*, T>`:
      // whatever T becomes, its member is the primary's, under these arguments.)
      if (sub && sub->kind == TyKind::Class && sub->decl &&
          (!types.isDependent(sub) || sub == selfType(sub->decl) || !(sub->decl->attributes & ATTR_Specialized)))
        if (const Ty* r = memberTypeOf(sub, uint32_t(t->value), depth + 1))
          return types.qualified(r, t->isConst || r->isConst, t->isVolatile);
      return sub == t->inner ? t : types.qualified(types.dependentMember(sub, uint32_t(t->value)), t->isConst, t->isVolatile);
    }
    case TyKind::Pointer: return types.qualified(types.pointer(substitute(t->inner, s, depth)), t->isConst, t->isVolatile);
    case TyKind::LRef: return types.lref(substitute(t->inner, s, depth));
    case TyKind::RRef: return types.rref(substitute(t->inner, s, depth));
    case TyKind::Array:
      if (t->decl) // `T[N]`: N's value once bound
      {
        const Ty* bound = s.find(t->decl);
        if (bound && bound->kind == TyKind::Value && !bound->decl && bound->value > 0)
          return types.array(substitute(t->inner, s, depth), uint32_t(bound->value));
        if (bound && bound->kind == TyKind::Param && bound->decl && bound->args.empty())
          return types.dependentArray(substitute(t->inner, s, depth), bound->decl); // N bound to another parameter M: `T[M]`
        return types.dependentArray(substitute(t->inner, s, depth), t->decl);
      }
      return types.array(substitute(t->inner, s, depth), t->count);
    case TyKind::MemberPointer:
      return types.memberPointer(substitute(t->inner, s, depth), t->args.empty() ? nullptr : substitute(t->args[0], s, depth));
    case TyKind::Member:
      return types.member(substitute(t->inner, s, depth), t->decl, t->args.empty() ? nullptr : substitute(t->args[0], s, depth));
    case TyKind::Enum:
    {
      if (t->args.empty())
        return t;
      std::vector<const Ty*> args; // `Flag<Tag>::Enum` with Tag bound: that instance's enum
      for (const Ty* a : t->args)
        if (const Ty* sub = substitute(a, s, depth); !spliceIfPack(sub, args))
          args.push_back(sub);
      return types.qualified(types.enumType(t->decl, Span<const Ty*>{args.data(), uint32_t(args.size())}), t->isConst, t->isVolatile);
    }
    case TyKind::Function:
    case TyKind::Class:
    case TyKind::Lambda:
    case TyKind::Pack:
    {
      std::vector<const Ty*> args;
      for (const Ty* a : t->args)
      {
        const Ty* sub = substitute(a, s, depth);
        if (t->kind != TyKind::Pack && spliceIfPack(sub, args))
          continue;
        args.push_back(sub);
      }
      Span<const Ty*> span{args.data(), uint32_t(args.size())};
      if (t->kind == TyKind::Pack)
        return types.pack(span);
      if (t->kind == TyKind::Function)
      {
        bool isNoexcept = t->isNoexcept();
        Decl* noexceptParam = t->decl; // `noexcept(N)` with N bound: as N says
        if (const Ty* n = noexceptParam ? s.find(noexceptParam) : nullptr; n && n->kind == TyKind::Value)
        {
          isNoexcept = n->value != 0;
          noexceptParam = nullptr;
        }
        else if (n && n->kind == TyKind::Param)
          noexceptParam = n->decl;
        return types.function(substitute(t->inner, s, depth), span, t->variadic, t->isCConvention(), isNoexcept, noexceptParam, t->memberQualifiers());
      }
      if (t->kind == TyKind::Lambda)
        return types.lambda(substitute(t->inner, s, depth), span, t->decl, t->value);
      if (args.size() == 1 && args[0] && args[0]->kind == TyKind::Function && isLibraryFunction(t->decl, false))
        if (const Ty* f = functionOfSignature(args[0]))
          return types.qualified(f, t->isConst, t->isVolatile); // `Function<T>` with T = void(): FunctionImpl<void>
      const Ty* c = types.classType(t->decl, span);
      if (t->decl && (t->decl->attributes & ATTR_Specialized) && !types.isDependent(c))
        c = resolveSpecialization(c, depth + 1); // `Owner<T>` with T = int: the specialisation Owner<int, Del<int>> picks
      return types.qualified(c, t->isConst, t->isVolatile);
    }
    default:
      return t;
  }
}
