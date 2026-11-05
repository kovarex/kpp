// GlobalScope::Impl, overload resolution (its declarations in OverloadResolution.hpp, part of GlobalScopeImpl.hpp): a translation unit
// of its own since 2026-11-05 (resolver.cpp split, stage 2), moved without a change.
#include "GlobalScopeImpl.hpp"

auto GlobalScope::Impl::overridesVirtual(const Ty* t, const std::string& key, int depth) -> std::optional<bool>
{
  if (!t || !t->isKnown() || types.isDependent(t) || t->kind != TyKind::Class || depth > 16)
    return std::nullopt;
  ClassDecl* def = classDefinition(t);
  std::vector<const Ty*> bases;
  if (!def || !directBases(t, def, bases))
    return std::nullopt;
  bool unknown = false;
  for (const Ty* b : bases)
  {
    ClassDecl* bd = b && b->kind == TyKind::Class ? classDefinition(b) : nullptr;
    if (!bd)
    {
      unknown = true;
      continue;
    }
    // A virtual function of the base itself, or (not marked: an override of its own) of a base of it.
    for (Decl* m : membersRead(bd))
      if (m->kind == NodeKind::FunctionDecl && (m->flags & (DF_Virtual | DF_Override | DF_Pure)) && !(m->flags & (DF_Static | DF_Constructor)) &&
          m->templateParams.empty() && overrideKey(static_cast<FunctionDecl*>(m), b) == key)
        return true;
    std::optional<bool> deeper = overridesVirtual(b, key, depth + 1);
    if (deeper && *deeper)
      return true;
    unknown = unknown || !deeper;
  }
  return unknown ? std::nullopt : std::optional<bool>(false);
}

auto GlobalScope::Impl::isBaseClass(const Ty* base, const Ty* derived) -> bool
{
  if (isBaseScope(classScopeOf(base), classScopeOf(derived)))
  {
    // The scopes link a class template's instances as one: `IsPtr<int> : Constant<bool, false>`
    // derives from no Constant<bool, true>. An instance is a base only as one the bases make
    // (when they can be told).
    const Ty* b = types.unqualified(types.stripRef(base));
    const Ty* d = types.unqualified(types.stripRef(derived));
    if (b->kind != TyKind::Class || b->args.empty() || !b->decl || !b->decl->ownScope || types.isDependent(b) || types.isDependent(d))
      return true;
    if (derivesFromInstance(d, classScopeOf(d), b, 0))
      return true;
    const Ty* actual = instantiationOfBase(d, classScopeOf(d), b->decl->ownScope, 0);
    return !actual || !actual->isKnown() || types.isDependent(actual) || mentionsUnknown(actual);
  }
  derived = types.unqualified(types.stripRef(derived));
  base = types.unqualified(types.stripRef(base));
  if (derived->decl == base->decl)
    return false;
  // An instantiation, or a class whose chain of bases, as the scopes link them (without
  // arguments), loses one (`class UtilitySprite : public UtilitySpriteInstance<Sprite>` with
  // `template<class T> class UtilitySpriteInstance : public T`): the bases as types.
  return (!derived->args.empty() || hasUnlinkedBase(classScopeOf(derived))) && derivesFrom(derived, base);
}

auto GlobalScope::Impl::hasUnlinkedBase(Scope* cls, int depth) -> bool
{
  if (!cls || depth > 16 || cls->baseTypes.empty())
    return false;
  ensureBases(cls, depth);
  if (cls->baseScopes.size() < cls->baseTypes.size())
    return true;
  for (Scope* b : cls->baseScopes)
    if (hasUnlinkedBase(b, depth + 1))
      return true;
  return false;
}

auto GlobalScope::Impl::isBaseScope(Scope* base, Scope* derived, int depth) -> bool
{
  if (!base || !derived || depth > 16)
    return false;
  ensureBases(derived, depth);
  for (Scope* b : derived->baseScopes)
    if (b == base || isBaseScope(base, b, depth + 1))
      return true;
  return false;
}

auto GlobalScope::Impl::classScopeOf(const Ty* t) -> Scope*
{
  t = types.unqualified(types.stripRef(t));
  if (!t || t->kind != TyKind::Class || !t->decl)
    return nullptr;
  if (t->decl->ownScope)
    return t->decl->ownScope;
  Entry e{t->decl, nullptr};
  return scopeOfEntry(e, 0);
}

// design.md 24: the one rule for which constructor a construction runs, for codegen, the interpreter
// and the checker (GlobalScope::chooseConstructor says what it does). implicitOnly: an implicit
// conversion, which no explicit constructor makes (an explicit(cond) template's cond is decided by
// the caller, with its arguments deduced). ambiguous, when given: whether the choice was.
auto GlobalScope::Impl::chooseConstructor(const Ty* classType, const std::vector<const Ty*>& argTypes, const std::vector<bool>* nullConstants,
                                          const Ty** inheritedFrom, bool implicitOnly, bool* ambiguousOut) -> FunctionDecl*
{
  if (inheritedFrom)
    *inheritedFrom = nullptr;
  if (ambiguousOut)
    *ambiguousOut = false;
  const Ty* p = classType ? types.unqualified(types.stripRef(classType)) : nullptr;
  ClassDecl* def = p && p->kind == TyKind::Class ? classDefinition(p) : nullptr;
  if (!def || types.isDependent(p))
    return nullptr;
  // (for an implicit conversion: no explicit constructor; a template's explicit(cond) is left to the
  // caller, which knows its arguments)
  auto converts = [&](FunctionDecl* f, const Subst& s)
  { return (f->explicitCondition && !f->templateParams.empty()) || !isExplicit(f, s.empty() ? nullptr : &s); };
  auto parameterTypes = [&](FunctionDecl* f, const Subst& s)
  {
    std::vector<const Ty*> out;
    for (Decl* param : f->params)
      out.push_back(declaredTypeOf(param, s.empty() ? nullptr : &s, 0));
    return out;
  };
  Subst all = substFor(p); // the class's arguments, then each base's whose constructors it inherits
  std::vector<const Ty*> inherited;
  inheritedConstructorSources(def, p, inherited, 0);
  std::vector<FunctionDecl*> candidates;
  // Every own non-template constructor's parameters, an explicit one's too, which hides an inherited
  // one with the same parameters whether or not it converts (D's review); computed only when the
  // class inherits constructors.
  std::vector<std::vector<const Ty*>> ownParameters;
  for (Decl* m : membersRead(def))
    if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Constructor))
    {
      auto* f = static_cast<FunctionDecl*>(m);
      if (!inherited.empty() && f->templateParams.empty())
        ownParameters.push_back(parameterTypes(f, all));
      if (implicitOnly && !converts(f, all))
        continue;
      candidates.push_back(f);
    }
  size_t own = candidates.size();
  std::vector<const Ty*> chain{p};
  chain.insert(chain.end(), inherited.begin(), inherited.end());
  std::vector<const Ty*> declaringBase(candidates.size(), nullptr);
  for (const Ty* bt : inherited)
  {
    ClassDecl* bd = classDefinition(bt);
    if (!bd)
      continue;
    Subst bs = substFor(bt);
    for (Decl* m : membersRead(bd))
    {
      if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Constructor))
        continue;
      auto* f = static_cast<FunctionDecl*>(m);
      std::vector<const Ty*> params = f->templateParams.empty() ? parameterTypes(f, bs) : std::vector<const Ty*>{};
      const Ty* first = !params.empty() && params[0] ? types.unqualified(types.stripRef(params[0])) : nullptr;
      if (argTypes.size() == 1 && first && first->kind == TyKind::Class && !types.isDependent(first) &&
          std::find(chain.begin(), chain.end(), first) != chain.end())
        continue; // a copy or move constructor of the base, or one taking the class itself
      if (!params.empty() && std::find(ownParameters.begin(), ownParameters.end(), params) != ownParameters.end())
        continue; // hidden by the class's own constructor with these parameters
      if (implicitOnly && !converts(f, bs))
        continue;
      candidates.push_back(f);
      declaringBase.push_back(bt);
    }
    for (const Subst::Pair& pair : bs.pairs)
      all.pairs.push_back(pair);
  }
  bool ambiguous = false;
  FunctionDecl* chosen = candidates.empty() ? nullptr : chooseAmong(candidates, argTypes, all.empty() ? nullptr : &all, nullptr, ambiguous, nullptr, nullConstants);
  if (ambiguousOut)
    *ambiguousOut = ambiguous;
  if (chosen && inheritedFrom)
    for (size_t i = own; i < candidates.size(); ++i)
      if (candidates[i] == chosen)
        *inheritedFrom = declaringBase[i];
  return chosen;
}

// design.md 24: the one rule for which conversion operator a conversion runs (GlobalScope::
// chooseConversion says what it does), for codegen, the interpreter and the checker.
auto GlobalScope::Impl::chooseConversion(const Ty* from, const Ty* to, ConversionKind kind, bool explicitCast, const Ty** foundIn, Subst* opArgs)
    -> FunctionDecl*
{
  if (foundIn)
    *foundIn = nullptr;
  if (opArgs)
    opArgs->pairs.clear();
  const Ty* object = from ? types.stripRef(from) : nullptr; // (its const: a const object calls const operators only)
  const Ty* start = object ? types.unqualified(object) : nullptr;
  const Ty* target = to ? types.unqualified(types.stripRef(to)) : nullptr;
  if (!start || start->kind != TyKind::Class || types.isDependent(start) || (kind == ConversionKind::ToType && !target))
    return nullptr;
  bool constObject = object->isConst;
  // Every conversion operator of the class and its bases (C++ [class.conv.fct]: a base's is hidden by
  // one of a derived class to the same type), with the class declaring it and its arguments.
  struct Candidate
  {
    FunctionDecl* f;
    const Ty* in;
    Subst args;
    const Ty* result; // as declared (a reference kept)
  };
  std::vector<Candidate> candidates;
  std::vector<const Ty*> declaredResults; // by the classes searched so far
  std::vector<const Ty*> classes{start};
  for (size_t i = 0; i < classes.size() && i < 64; ++i)
  {
    const Ty* cls = classes[i];
    ClassDecl* def = classDefinition(cls);
    if (!def)
      continue;
    Subst args = substFor(cls);
    std::vector<const Ty*> ownResults;
    for (Decl* m : membersRead(def))
    {
      if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Conversion))
        continue;
      auto* f = static_cast<FunctionDecl*>(m);
      const Ty* ft = declaredTypeOf(m, args.empty() ? nullptr : &args, 0);
      if (!ft || ft->kind != TyKind::Function || !ft->inner || !ft->inner->isKnown())
        continue;
      Subst fargs = args;
      const Ty* result = ft->inner;
      if (!f->templateParams.empty())
      {
        // `template<class T> operator T()`, `operator const T&()`, `operator T*()`: T deduced from
        // the type converted to, the operator a candidate only when its constraints hold for it.
        if (!target)
          continue;
        deduce(types.unqualified(types.stripRef(result)), target, fargs);
        bool bound = true;
        for (Decl* p : f->templateParams)
          bound = bound && fargs.find(p) && fargs.find(p)->isKnown() && !types.isDependent(fargs.find(p));
        if (!bound || (f->requiresClause && !requiresHolds(f, fargs)))
          continue;
        result = substitute(result, fargs);
        if (!result || !result->isKnown())
          continue;
      }
      const Ty* plain = types.unqualified(types.stripRef(result));
      if (f->templateParams.empty() && std::find(declaredResults.begin(), declaredResults.end(), plain) != declaredResults.end())
        continue; // hidden by a derived class's operator to the same type
      ownResults.push_back(plain);
      candidates.push_back({f, cls, fargs, result});
    }
    declaredResults.insert(declaredResults.end(), ownResults.begin(), ownResults.end());
    std::vector<const Ty*> bases;
    if (directBases(cls, def, bases))
      for (const Ty* b : bases)
        if (b && b->kind == TyKind::Class && !types.isDependent(b))
          classes.push_back(types.unqualified(b));
  }
  // Each candidate's rank, lower better, or NotViable: for a pointer or an integral type the first
  // one of that kind (rank 0); to a type, the standard conversion from its result to the target (an
  // exact match 0, a class derived from the target 1, then C++'s ranking of the scalar
  // conversions), the better of two equal ones the one whose constness matches the object.
  const Candidate* best = nullptr;
  int bestRank = NotViable;
  bool bestConstMatch = false;
  bool ambiguous = false;
  for (const Candidate& c : candidates)
  {
    bool constOp = (c.f->flags & DF_Const) != 0;
    if (constObject && !constOp)
      continue; // a const object calls a const operator only
    bool isExplicitOne = isExplicit(c.f, c.args.empty() ? nullptr : &c.args);
    const Ty* ret = types.unqualified(types.stripRef(c.result));
    int rank = NotViable;
    switch (kind)
    {
      case ConversionKind::ToPointer:
        rank = !isExplicitOne && ret->kind == TyKind::Pointer ? 0 : NotViable;
        break;
      case ConversionKind::ToIntegral:
        rank = !isExplicitOne && (ret->kind == TyKind::Enum || ret->kind == TyKind::Int || ret->kind == TyKind::Char || ret->kind == TyKind::Bool) ? 0 : NotViable;
        break;
      case ConversionKind::ToType:
      {
        bool exact = ret == target;
        bool derived = !exact && ret->kind == TyKind::Class && target->kind == TyKind::Class && !types.isDependent(ret) && isBaseClass(target, ret);
        if (isExplicitOne && target->kind != TyKind::Bool && (!explicitCast || !(exact || derived)))
          break;
        if (exact)
          rank = 0;
        else if (derived)
          rank = 1;
        else if (ret->kind != TyKind::Class && target->kind != TyKind::Class)
        {
          // (A scalar is no way to a class: from it a converting constructor would be a second
          // user-defined conversion.)
          int r = conversionRank(ret, target);
          rank = r < 4 ? r : NotViable;
        }
        break;
      }
    }
    if (rank >= NotViable)
      continue;
    bool constMatch = constOp == constObject;
    if (rank < bestRank || (rank == bestRank && constMatch && !bestConstMatch))
    {
      best = &c;
      bestRank = rank;
      bestConstMatch = constMatch;
      ambiguous = false;
    }
    else if (rank == bestRank && constMatch == bestConstMatch && c.f != best->f && kind == ConversionKind::ToType)
      ambiguous = true;
  }
  if (!best || ambiguous)
    return nullptr; // (none, or two equally good: C++'s ambiguity, which codegen reports as no conversion)
  if (foundIn)
    *foundIn = best->in;
  if (opArgs)
    *opArgs = best->args;
  return best->f;
}

auto GlobalScope::Impl::inheritedConstructorSources(ClassDecl* def, const Ty* p, std::vector<const Ty*>& out, int depth, int level) -> void
{
  if (!def || level > 8)
    return;
  Subst s = substFor(p);
  for (Decl* m : membersRead(def))
  {
    if (m->kind != NodeKind::UsingNameDecl)
      continue;
    const QualifiedName& un = static_cast<UsingNameDecl*>(m)->name;
    if (un.parts.size() < 2 || un.last().symbol != un.parts[un.parts.size() - 2].symbol)
      continue;
    NamedType written{};
    written.kind = NodeKind::NamedType;
    written.token = m->token;
    written.name = un;
    written.name.parts.count -= 1;
    LocalStack params;
    for (Decl* tp : def->templateParams)
      params.push(static_cast<TemplateParamDecl*>(tp)->name, tp);
    ParsedFile* file = def->file;
    Scope* inside = classScopeOf(p) ? classScopeOf(p) : def->ownScope;
    TypeContext bctx{inside, &params, s.pairs.empty() ? nullptr : &s, file ? file->tokens : nullptr, file ? file->text : std::string_view()};
    const Ty* bt = convertType(&written, bctx, depth + 1);
    bt = bt ? types.unqualified(types.stripRef(bt)) : nullptr;
    if (!bt || bt->kind != TyKind::Class || !bt->decl || types.isDependent(bt))
      continue;
    out.push_back(bt);
    if (ClassDecl* bd = classDefinition(bt))
      inheritedConstructorSources(bd, bt, out, depth, level + 1);
  }
}

auto GlobalScope::Impl::listInitRank(const Ty* list, const Ty* p, int depth) -> int
{
  if (!list->inner || list->inner->kind != TyKind::Pack || depth > 1 || !p->isKnown() || types.isDependent(p))
    return 2;
  std::vector<const Ty*> elems(list->inner->args.begin(), list->inner->args.end());
  for (const Ty* e : elems)
    if (!e || !e->isKnown() || types.isDependent(e))
      return 2;
  if (p->kind != TyKind::Class)
  {
    if (elems.empty())
      return 0;
    return elems.size() == 1 ? conversionRank(elems[0], p, depth + 1) : NotViable;
  }
  if (elems.size() == 1)
  {
    const Ty* e = types.unqualified(types.stripRef(elems[0]));
    if (e == p)
      return 0;
    if (e->kind == TyKind::Class && !e->isBraceList() && isBaseClass(p, e))
      return 3;
  }
  ClassDecl* def = classDefinition(p);
  if (!def || (def->attributes & ATTR_Draft) || def->isTypedUnion || isFunctionImpl(p) || isInitializerList(p))
    return 2;
  Subst s = substFor(p);
  bool constructors = false;
  for (Decl* m : membersRead(def))
  {
    if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Constructor))
      continue;
    constructors = true;
    auto* f = static_cast<FunctionDecl*>(m);
    if (f->flags & DF_Delete)
      continue;
    if (elems.empty() && takesArgumentCount(f, 0))
      return 4; // `{}`: value-initialized by the default constructor (C++ [dcl.init.list] 3.5)
    if (f->params.empty())
      continue;
    // An InitializerList constructor takes the whole list; another, the elements as arguments.
    const Ty* first = declaredTypeOf(f->params[0], &s, 0);
    if (first && isInitializerList(types.unqualified(types.stripRef(first))) && takesArgumentCount(f, 1))
    {
      if (conversionRank(list, first, depth + 1) < NotViable)
        return 4;
      continue;
    }
    if (constructorTakes(f, elems, s, depth))
      return 4;
  }
  // `{0, 0}` for a Vector2D, whose constructors include Vector2's (`using Vector2::Vector2;`):
  // those take the elements too, its copy and move ones aside.
  std::vector<const Ty*> inherited;
  inheritedConstructorSources(def, p, inherited, depth);
  for (const Ty* bt : inherited)
  {
    ClassDecl* bd = classDefinition(bt);
    if (!bd)
      continue;
    constructors = true;
    Subst bs = substFor(bt);
    for (Decl* m : membersRead(bd))
    {
      if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Constructor))
        continue;
      auto* f = static_cast<FunctionDecl*>(m);
      if (f->flags & DF_Delete)
        continue;
      if (elems.empty() && takesArgumentCount(f, 0))
        return 4; // `{}` for a Vector2D: value-initialized by Vector2's default constructor, which it inherits
      if (f->params.empty())
        continue;
      const Ty* first = declaredTypeOf(f->params[0], &bs, 0);
      if (f->params.size() == 1 && first && types.unqualified(types.stripRef(first)) == bt)
        continue; // the base's copy or move constructor is not inherited
      if (constructorTakes(f, elems, bs, depth))
        return 4;
    }
  }
  if (constructors)
    return NotViable;
  // An aggregate: its bases, then its data members, each from the next element.
  std::vector<const Ty*> members;
  if (!directBases(p, def, members))
    return 2;
  for (Decl* m : membersRead(def))
    if (m->kind == NodeKind::VariableDecl && !(m->flags & (DF_Static | DF_Typedef)))
      members.push_back(declaredTypeOf(m, s.pairs.empty() ? nullptr : &s, 0));
  auto elisionMayFill = [&](const Ty* member)
  {
    member = member ? types.unqualified(types.stripRef(member)) : nullptr;
    return !member || !member->isKnown() || member->kind == TyKind::Array || (member->kind == TyKind::Class && !member->isBraceList());
  };
  for (size_t i = 0; i < elems.size(); ++i)
  {
    if (i >= members.size())
      return std::any_of(members.begin(), members.end(), elisionMayFill) ? 4 : NotViable;
    if (conversionRank(elems[i], members[i], depth + 1) >= NotViable && !elisionMayFill(members[i]))
      return NotViable;
  }
  return 4;
}

auto GlobalScope::Impl::constructorTakes(FunctionDecl* f, const std::vector<const Ty*>& argTypes, const Subst& s, int depth) -> bool
{
  if (!takesArgumentCount(f, argTypes.size()))
    return false;
  const Ty* ft = declaredTypeOf(f, s.pairs.empty() ? nullptr : &s, 0);
  if (!f->templateParams.empty() && ft)
  {
    Subst d = s;
    deduceCall(f, ft, argTypes, d);
    // Deduction fails when the arguments leave one of f's own parameters undeduced (one
    // without a default; a pack that is not the last): a brace list deduces nothing, so
    // `template<U1, U2> Pair(U1&&, U2&&)` takes no two lists (`{{1, false}, {2, true}}` is no
    // Pair), nor `Pair(Tuple<Args1...>&, Tuple<Args2...>&, IndexSequence<I1...>, ...)` four.
    for (size_t i = 0; i < f->templateParams.size(); ++i)
    {
      auto* param = static_cast<TemplateParamDecl*>(f->templateParams[i]);
      if (!d.find(param) && !param->defaultArg && (!param->isPack || i + 1 < f->templateParams.size()))
        return false;
    }
    // Its constraint too: Vector's `template<InputIt> Vector(InputIt, InputIt) requires
    // (!isIntegral<InputIt>)` takes no `{1, 2}`.
    if (f->requiresClause && !requiresHolds(f, d))
      return false;
    ft = substitute(ft, d);
  }
  if (!ft || ft->kind != TyKind::Function)
    return true;
  for (size_t i = 0; i < argTypes.size() && i < ft->args.size(); ++i)
  {
    const Ty* pt = ft->args[i];
    if (!pt || !pt->isKnown() || types.isDependent(pt) || pt->kind == TyKind::Pack)
      continue;
    if (conversionRank(argTypes[i], pt, depth + 1) >= NotViable)
      return false;
  }
  return true;
}

auto GlobalScope::Impl::conversionRank(const Ty* arg, const Ty* param, int depth) -> int
{
  if (!arg || !param || !arg->isKnown() || !param->isKnown() || types.isDependent(arg) || types.isDependent(param))
    return 2; // Unknown types never decide an overload.
  if (depth > 4)
    return NotViable;
  // A prvalue of a scalar type is never const (C++'s [expr.type]): `c ? p.lineLength : p.frameCount`
  // over const members binds an `Args&&`.
  if (!arg->isReference() && arg->isConst && arg->kind != TyKind::Class && arg->kind != TyKind::Array)
    arg = types.unqualified(arg);
  const Ty* a = types.stripRef(arg);
  if (a->isBraceList() && !isInitializerList(types.stripRef(param)))
  {
    // `{a, b}` for anything but an InitializerList: the object it list-initializes, whose
    // temporary binds a `T&&` better than a `const T&` (C++'s rvalue-reference rule) and a `T&`
    // not at all (`take({1, makeUnique<int>(5)})`).
    if (param->kind == TyKind::LRef && !param->inner->isConst)
      return NotViable;
    int r = listInitRank(a, types.unqualified(types.stripRef(param)), depth);
    if (r >= NotViable)
      return NotViable;
    return param->kind == TyKind::LRef ? (r >= 4 ? 5 : std::max(r, 3)) : r;
  }
  if (isInitializerList(a) && !isInitializerList(types.stripRef(param)))
    return 2; // an InitializerList object converts to anything else through its elements, which the code generator checks
  if (a->isBraceList())
  {
    // `{"ab", text}` for an InitializerList<StringView>: as its elements convert, the worst of
    // them (the first one's type stands for them all when they are not known).
    const Ty* p = types.unqualified(types.stripRef(param));
    if (p->args.empty() || a->args.empty())
      return 2;
    if (!a->inner || a->inner->kind != TyKind::Pack)
    {
      int r = conversionRank(a->args[0], p->args[0], depth + 1);
      return r >= NotViable ? NotViable : r;
    }
    int worst = 0;
    for (const Ty* e : a->inner->args)
      if (int r = conversionRank(e, p->args[0], depth + 1); r >= NotViable)
        return NotViable;
      else
        worst = std::max(worst, r);
    return worst;
  }
  if (param->isReference())
  {
    const Ty* pt = param->inner;
    // A function is an lvalue: `const Pred&` with Pred a function type (const has no say on
    // one) binds `Group::ascending` itself.
    if (a->kind == TyKind::Function && pt && pt->kind == TyKind::Function)
      return types.unqualified(a) == types.unqualified(pt) ? 0 : NotViable;
    bool lvalueToRvalueRef = param->kind == TyKind::RRef && arg->kind == TyKind::LRef; // `T&&` takes no lvalue directly
    // `frame << agui::horizontalLine` with the helper's `operator HorizontalLine&()`: a class
    // whose conversion operator gives an lvalue of pt's class or of one derived from it binds a
    // non-const `pt&` (a user-defined conversion), whatever the object's own value category.
    if (param->kind == TyKind::LRef && !pt->isConst && a->kind == TyKind::Class && pt->kind == TyKind::Class &&
        types.unqualified(a) != types.unqualified(pt) && !isBaseClass(pt, a) && convertsByOperator(a, param, depth + 1))
      return 4;
    if (param->kind == TyKind::LRef && !pt->isConst && (!arg->isReference() || arg->kind == TyKind::RRef))
      return NotViable; // `T&` binds no rvalue (a temporary, a lambda, `move(x)`): lvalues present themselves as `T&` (argumentType)
    if (types.unqualified(a) == types.unqualified(pt))
    {
      if (lvalueToRvalueRef || (!pt->isConst && a->isConst))
        return NotViable;
      return pt->isConst && !a->isConst ? 1 : 0;
    }
    if (a->kind == TyKind::Class && pt->kind == TyKind::Class && isBaseClass(pt, a))
      return lvalueToRvalueRef || (!pt->isConst && a->isConst) ? NotViable : 3;
    if (a->kind == TyKind::Array && pt->kind == TyKind::Array)
    {
      // `const int (&)[5]` given an int[5], `const Point (&)[16][2]` given a Point[16][2]: the
      // same lengths at every level, the innermost elements the same type, made const or not.
      const Ty* x = a;
      const Ty* y = pt;
      while (x && y && x->kind == TyKind::Array && y->kind == TyKind::Array && x->count == y->count && x->inner && y->inner)
      {
        x = x->inner;
        y = y->inner;
      }
      if (x && y && x->kind != TyKind::Array && y->kind != TyKind::Array && types.unqualified(x) == types.unqualified(y) && (y->isConst || !x->isConst))
        return lvalueToRvalueRef ? NotViable : (a == pt ? 0 : 1);
    }
    if (param->kind == TyKind::LRef && !pt->isConst)
      return NotViable; // A non-const lvalue reference binds only to the same type.
    // The argument as it is (an lvalue stays one): a converting constructor taking `T&`
    // binds it (`implicit EntityAndIndex(T& entity)` given a Tower lvalue).
    int r = conversionRank(arg, pt, depth + 1);
    if (r >= NotViable)
      return NotViable;
    if (r >= 4 && param->kind == TyKind::LRef)
      return 5; // the converted temporary binds better to `T&&` than to `const T&`
    // An rvalue reference binds no lvalue that a conversion function gives, neither the lvalue
    // itself nor a copy of it (C++ [over.ics.ref] 3, as clang applies it): take(String&&) given
    // a class whose conversion is `operator const String&()` is no candidate, take(const
    // String&) is called (UnitTest's names, moved out of their objects through try_emplace).
    if (r >= 4 && param->kind == TyKind::RRef && a->kind == TyKind::Class && convertsByOperator(a, pt, depth + 1) &&
        !convertsByOperator(a, pt, depth + 1, 0, false, true))
      return NotViable;
    return std::max(r, 2);
  }
  a = types.decay(a);
  const Ty* p = types.unqualified(param);
  if (a == p)
    return 0;
  auto isInt = [](const Ty* t) { return t->kind == TyKind::Int || t->kind == TyKind::Char || t->kind == TyKind::Bool; };
  switch (p->kind)
  {
    case TyKind::Bool:
      // (not nullptr: a bool is initialised from it only directly, `bool(nullptr)` ([conv.bool]),
      // never as an argument or in a copy-initialisation)
      if (isInt(a) || a->kind == TyKind::Float || a->kind == TyKind::Pointer || a->kind == TyKind::MemberPointer)
        return 3;
      if (a->kind == TyKind::Enum && !static_cast<EnumDecl*>(a->decl)->isClass)
        return 3;
      break;
    case TyKind::Int:
    case TyKind::Char:
      if (isInt(a))
      {
        bool promotion = p == types.intType(32) && (a->kind == TyKind::Bool || a->kind == TyKind::Char || a->bits < 32); // to int only, never long
        return promotion ? 2 : 3;
      }
      if (a->kind == TyKind::Float)
        return 3;
      if (a->kind == TyKind::Enum)
      {
        if (static_cast<EnumDecl*>(a->decl)->isClass)
          return NotViable;
        // An unscoped enumeration promotes to int (C++ [conv.prom] 3), one with a fixed
        // underlying type to that type and to the type that one promotes to ([conv.prom] 4):
        // `enum LuaEventType : uint32_t` to uint32, `enum Small : uint8_t` to uint8 and to int.
        bool toInt = p == types.intType(32);
        const Ty* u = declaredUnderlying(a);
        if (u && !u->isKnown())
          return p->kind == TyKind::Int && p->bits == 32 ? 2 : 3; // not known yet: as before it is
        if (u)
        {
          if (p == u)
            return 2;
          bool uPromotes = u->kind == TyKind::Bool || u->kind == TyKind::Char || (u->kind == TyKind::Int && u->bits < 32);
          return toInt && uPromotes ? 2 : 3;
        }
        return toInt ? 2 : 3;
      }
      break;
    case TyKind::Float:
      if (a->kind == TyKind::Float)
        return p->bits > a->bits ? 2 : 3;
      if (isInt(a))
        return 3;
      if (a->kind == TyKind::Enum && !static_cast<EnumDecl*>(a->decl)->isClass)
        return 3;
      break;
    case TyKind::Enum:
      // (One enum is `a == p` above: `Flag<A>::Enum` is not `Flag<B>::Enum`.) The declaration's,
      // its instance unknown on one side (no arguments).
      if (a->kind == TyKind::Enum && a->decl == p->decl && (a->args.empty() || p->args.empty()))
        return 0;
      break;
    case TyKind::MemberPointer:
      if (a->kind == TyKind::MemberPointer && types.unqualified(a->inner) == types.unqualified(p->inner) && a->decl == p->decl)
        return 0;
      // `&LuaControlBehavior::luaReadEntity` for an `int32 (T::*)(lua_State*)` with T derived from
      // it: a pointer to a member of a base converts to one of the derived class.
      if (a->kind == TyKind::MemberPointer && types.unqualified(a->inner) == types.unqualified(p->inner) && !a->args.empty() &&
          !p->args.empty() && a->args[0] && p->args[0] && a->args[0]->kind == TyKind::Class && p->args[0]->kind == TyKind::Class &&
          isBaseClass(a->args[0], p->args[0]))
        return 3;
      if (a->kind == TyKind::Nullptr)
        return 3;
      break;
    case TyKind::Pointer:
      if (a->kind == TyKind::Nullptr)
        return 3;
      if (a->kind == TyKind::Pointer)
      {
        const Ty* ai = a->inner;
        const Ty* pi = p->inner;
        if (types.unqualified(ai) == types.unqualified(pi))
          return pi->isConst == ai->isConst ? 0 : (pi->isConst ? 1 : NotViable);
        if (pi->kind == TyKind::Void)
          return (ai->isConst && !pi->isConst) || (ai->isVolatile && !pi->isVolatile) ? NotViable : 3; // (`const char*` is no `void*`)
        if (ai->kind == TyKind::Class && pi->kind == TyKind::Class && isBaseClass(pi, ai))
          return !pi->isConst && ai->isConst ? NotViable : 3;
        return NotViable;
      }
      if (a->kind == TyKind::Lambda && p->inner->kind == TyKind::Function)
        return 3;
      break;
    case TyKind::Class:
    {
      if (a->kind == TyKind::Class)
      {
        if (types.unqualified(a)->decl == p->decl && a->args.size() == p->args.size())
        {
          bool same = true;
          for (size_t i = 0; i < a->args.size(); ++i)
            same = same && a->args[i] == p->args[i];
          if (same)
            return 0;
        }
        if (isBaseClass(p, a))
          return 3;
      }
      // A typed union from a value of an alternative's type, references and const removed
      // (design.md 8.5): a user-defined conversion, as through a converting constructor. Two
      // alternatives of that type make it ambiguous, which the code generator reports.
      if (p->decl && p->decl->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(p->decl)->isTypedUnion)
      {
        auto* u = static_cast<ClassDecl*>(p->decl);
        Subst s = substFor(p);
        for (Decl* alt : u->alternatives)
          if (types.unqualified(declaredTypeOf(alt, s.pairs.empty() ? nullptr : &s, 0)) == types.unqualified(a))
            return 4;
        return NotViable; // no other conversion makes a union
      }
      // User-defined: a non-explicit single-argument constructor of p accepting a, its own or
      // one a `using Base::Base;` inherits (not the base's copy or move constructor).
      if (Decl* def = p->decl && p->decl->ownScope ? p->decl : (classScopeOf(p) ? classScopeOf(p)->owner : nullptr))
      {
        auto converts = [&](ClassDecl* owner, const Ty* ownerTy, const Subst& s) -> bool
        {
        for (Decl* m : membersRead(owner))
        {
          if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Constructor))
            continue;
          auto* f = static_cast<FunctionDecl*>(m);
          // `explicit(!isConvertible<U, T>)` of a constructor template (Optional's from a value
          // of another type): decided with U deduced, below.
          bool explicitWhenDeduced = f->explicitCondition && !f->templateParams.empty();
          if (!explicitWhenDeduced && isExplicit(f, &s))
            continue;
          // Callable with one argument: the others defaulted or a pack (`implicit
          // DragBuildInfo(DragBuild dragBuild, OptionalMapPosition startPosition = {})`).
          if (f->params.empty() || (f->params.size() > 1 && !static_cast<ParamDecl*>(f->params[1])->defaultValue &&
                                    !static_cast<ParamDecl*>(f->params[1])->isPack))
            continue;
          Subst d = s;
          if (!f->templateParams.empty())
          {
            // `template<typename F> Function(F callable)`: F from the argument (as it is: an
            // lvalue for `template<class T> EntityAndIndex(T& entity)`).
            const Ty* ft = declaredTypeOf(f, &s, 0);
            deduceCall(f, ft, std::vector<const Ty*>{arg}, d);
            if (isFunctionImpl(p) && !callableWith(a, p, depth))
              continue; // Function<void(double)> takes no `[]() {}`
            // A parameter the argument does not deduce and that has no default: deduction failed,
            // so no candidate (`UniquePtr(UniquePtr<U, E>&&)` given a const char*).
            bool undeduced = false;
            for (Decl* tp : f->templateParams)
            {
              auto* param = static_cast<TemplateParamDecl*>(tp);
              undeduced = undeduced || (!d.find(tp) && !param->defaultArg && !param->isPack);
            }
            if (undeduced)
              continue;
          }
          if (explicitWhenDeduced && isExplicit(f, &d))
            continue;
          if (f->requiresClause && !requiresHolds(f, d))
            continue; // `template<typename F> Wrapper(F f) requires(!isPointer<F>)` given a pointer: no conversion
          const Ty* pt = declaredTypeOf(f->params[0], &d, 0);
          if (pt && types.stripRef(pt)->kind == TyKind::Class && types.unqualified(types.stripRef(pt)) == types.unqualified(ownerTy))
            continue; // The copy constructor is not a conversion (ReverseIterator<const T> from ReverseIterator<T> is).
          if (conversionRank(arg, pt, depth + 1) < 4)
            return true;
        }
        return false;
        };
        // `using super::super;` (SignalFilter from IDWithQualityFilter<SignalIDBase>): the base's
        // constructors, with that base's arguments, and those it inherits in turn (CompressionState
        // from MipmapState from Handle).
        std::function<bool(Decl*, const Ty*, int)> inherits = [&](Decl* def, const Ty* p, int level) -> bool
        {
        Subst s = substFor(p);
        if (converts(static_cast<ClassDecl*>(def), p, s))
          return true;
        if (level > 8)
          return false;
        for (Decl* m : membersRead(static_cast<ClassDecl*>(def)))
        {
          if (m->kind != NodeKind::UsingNameDecl)
            continue;
          const QualifiedName& un = static_cast<UsingNameDecl*>(m)->name;
          if (un.parts.size() < 2 || un.last().symbol != un.parts[un.parts.size() - 2].symbol)
            continue;
          NamedType written{};
          written.kind = NodeKind::NamedType;
          written.token = m->token;
          written.name = un;
          written.name.parts.count -= 1;
          LocalStack params;
          for (Decl* tp : def->templateParams)
            params.push(static_cast<TemplateParamDecl*>(tp)->name, tp);
          ParsedFile* file = def->file;
          Scope* inside = classScopeOf(p) ? classScopeOf(p) : def->ownScope;
          TypeContext bctx{inside, &params, s.pairs.empty() ? nullptr : &s, file ? file->tokens : nullptr, file ? file->text : std::string_view()};
          const Ty* bt = convertType(&written, bctx, depth + 1);
          bt = bt ? types.unqualified(types.stripRef(bt)) : nullptr;
          if (!bt || bt->kind != TyKind::Class || !bt->decl || types.isDependent(bt))
            continue;
          Scope* bs = classScopeOf(bt);
          if (bs && bs->owner && bs->owner->kind == NodeKind::ClassDecl && inherits(bs->owner, bt, level + 1))
            return true;
        }
        return false;
        };
        if (inherits(def, p, 0))
          return 4;
      }
      break;
    }
    default:
      break;
  }
  // Conversion operator of the argument's class, or one it inherits.
  if (a->kind == TyKind::Class && convertsByOperator(a, param, depth))
    return 4;
  return NotViable;
}

auto GlobalScope::Impl::scalarConversionOf(const Ty* cls) -> const Ty*
{
  const Ty* found = nullptr;
  bool ambiguous = false;
  collectScalarConversions(types.unqualified(types.stripRef(cls)), found, ambiguous, 0);
  return ambiguous ? nullptr : found;
}

auto GlobalScope::Impl::freeOperatorAgainstBuiltin(FunctionDecl* f, const Subst* subst, P op, const std::vector<const Ty*>& operands) -> int
{
  const Ty* ft = declaredTypeOf(f, subst, 0);
  if (!ft || ft->kind != TyKind::Function || ft->args.size() < operands.size())
    return 1;
  bool contextualBool = op == P::Bang || op == P::AndAnd || op == P::OrOr; // (an explicit operator bool converts there)
  bool fBetter = false, builtinBetter = false, anyClass = false;
  for (size_t i = 0; i < operands.size(); ++i)
  {
    const Ty* t = types.unqualified(types.stripRef(operands[i]));
    bool isClass = t && t->kind == TyKind::Class;
    if (isClass)
    {
      anyClass = true;
      bool converts = contextualBool ? convertsByOperator(t, types.boolType(), 0, 0, true) : scalarConversionOf(t) != nullptr;
      if (!converts)
        return 1; // no built-in operator takes it
    }
    bool fUserDefined = conversionRank(operands[i], ft->args[i]) >= 4;
    fBetter = fBetter || (!fUserDefined && isClass);
    builtinBetter = builtinBetter || (fUserDefined && !isClass);
  }
  if (!anyClass || (fBetter && !builtinBetter))
    return 1;
  return builtinBetter && !fBetter ? 0 : -1;
}

auto GlobalScope::Impl::collectScalarConversions(const Ty* cls, const Ty*& found, bool& ambiguous, int depth) -> void
{
  if (!cls || cls->kind != TyKind::Class || depth > 8)
    return;
  Scope* s = classScopeOf(cls);
  if (!s || !s->owner || s->owner->kind != NodeKind::ClassDecl)
    return;
  Subst subst = substFor(cls);
  for (Decl* m : membersRead(static_cast<ClassDecl*>(s->owner)))
  {
    if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Conversion) || isExplicit(static_cast<FunctionDecl*>(m), &subst))
      continue;
    const Ty* ft = declaredTypeOf(m, &subst, 0);
    if (!ft || ft->kind != TyKind::Function || !ft->inner || !ft->inner->isKnown() || types.isDependent(ft->inner))
      continue;
    const Ty* t = types.unqualified(types.stripRef(ft->inner));
    if (t->kind == TyKind::Class || t->kind == TyKind::Void || t->kind == TyKind::Lambda)
      continue;
    if (found && found != t)
      ambiguous = true;
    found = t;
  }
  ClassDecl* def = classDefinition(cls);
  std::vector<const Ty*> bases;
  if (def && directBases(cls, def, bases))
    for (const Ty* b : bases)
      collectScalarConversions(b, found, ambiguous, depth + 1);
}

auto GlobalScope::Impl::convertsByOperator(const Ty* a, const Ty* param, int depth, int baseDepth, bool allowExplicit, bool rvalues) -> bool
{
  if (baseDepth > 8)
    return false;
  Scope* s = classScopeOf(a);
  if (!s || !s->owner || s->owner->kind != NodeKind::ClassDecl)
    return false;
  Subst subst = substFor(a);
  for (Decl* m : membersRead(static_cast<ClassDecl*>(s->owner)))
  {
    if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Conversion) || (!allowExplicit && isExplicit(static_cast<FunctionDecl*>(m), &subst)))
      continue;
    const Ty* ft = declaredTypeOf(m, &subst, 0);
    if (!ft || ft->kind != TyKind::Function)
      continue;
    const Ty* result = ft->inner;
    if (auto* f = static_cast<FunctionDecl*>(m); !f->templateParams.empty())
    {
      // `template<class T> requires isPointer<T> operator T()`: its arguments deduced from the type
      // wanted, and its constraint holding for them (no way to a nullptr_t, a class).
      const Ty* wanted = param ? types.unqualified(types.stripRef(param)) : nullptr;
      if (!wanted || !wanted->isKnown() || types.isDependent(wanted))
        continue;
      Subst own = subst;
      deduce(types.unqualified(types.stripRef(result)), wanted, own);
      bool bound = true;
      for (Decl* p : f->templateParams)
        bound = bound && own.find(p) && own.find(p)->isKnown();
      if (!bound || !requiresHolds(f, own))
        continue;
      result = substitute(result, own);
    }
    if (rvalues && result->kind == TyKind::LRef && result->inner->kind != TyKind::Function)
      continue;
    if (conversionRank(result, param, depth + 1) < 4)
      return true;
  }
  ClassDecl* def = classDefinition(a);
  std::vector<const Ty*> bases;
  if (def && directBases(a, def, bases))
    for (const Ty* b : bases)
      if (b->kind == TyKind::Class && convertsByOperator(b, param, depth, baseDepth + 1, allowExplicit, rvalues))
        return true;
  return false;
}

auto GlobalScope::Impl::whenMemberHolds(Decl* m, const Subst& subst, int depth) -> std::optional<bool>
{
  Decl* c = m ? m->parent : nullptr;
  if (!m || !m->when || !c || c->kind != NodeKind::ClassDecl)
    return std::nullopt;
  LocalStack params;
  for (Decl* p : effectiveParams(c))
    params.push(static_cast<TemplateParamDecl*>(p)->name, p);
  TypeContext wctx{c->ownScope ? c->ownScope : lexicalScopeOf(m), &params, &subst, m->file ? m->file->tokens : nullptr,
                   m->file ? m->file->text : std::string_view()};
  return constCondition(m->when, wctx, depth + 1);
}

auto GlobalScope::Impl::whenMemberOf(Decl* m, const Subst& subst, int& holding, bool& undecided, int depth) -> Decl*
{
  holding = 0;
  undecided = false;
  Decl* c = m ? m->parent : nullptr;
  if (!m || m->kind != NodeKind::VariableDecl || static_cast<VariableDecl*>(m)->name.empty() || !c || c->kind != NodeKind::ClassDecl || !c->ownScope)
  {
    undecided = true;
    return nullptr;
  }
  Lookup l;
  if (!lookupInScope(c->ownScope, static_cast<VariableDecl*>(m)->name.last().symbol, l, depth + 1) || !l.overloads)
  {
    holding = 1;
    return m;
  }
  Decl* chosen = nullptr;
  for (const Entry& e : *l.overloads)
  {
    if (!e.decl || e.decl->kind != NodeKind::VariableDecl || e.decl->parent != c)
      continue;
    std::optional<bool> holds = e.decl->when && e.decl->whenKnown < 0 ? whenMemberHolds(e.decl, subst, depth) : std::optional<bool>(e.decl->whenKnown != 0);
    if (!holds)
    {
      undecided = true;
      return nullptr;
    }
    if (*holds)
    {
      chosen = chosen ? chosen : e.decl;
      ++holding;
    }
  }
  return holding == 1 ? chosen : nullptr;
}

auto GlobalScope::Impl::namedReturnCandidate(const Stmt* body, std::string* why) -> VariableDecl*
{
  auto no = [&](std::string reason) -> VariableDecl*
  {
    if (why)
      *why = std::move(reason);
    return nullptr;
  };
  // What nested lambdas and local classes hold: their returns are their own.
  std::unordered_set<const Node*> nested;
  std::vector<const ReturnStmt*> returns;
  std::unordered_set<const Decl*> buildable; // declared by a declaration statement or a range-for
  forEachNode(const_cast<Stmt*>(body), [&](Node* n)
  {
    if ((n->kind == NodeKind::LambdaExpr || n->kind == NodeKind::ClassDecl) && !nested.count(n))
      forEachNode(n, [&](Node* inner) { if (inner != n) nested.insert(inner); });
    if (n->kind == NodeKind::ReturnStmt)
      returns.push_back(static_cast<const ReturnStmt*>(n));
    else if (n->kind == NodeKind::DeclStmt)
      for (Decl* d : static_cast<DeclStmt*>(n)->decls)
        buildable.insert(d);
    else if (n->kind == NodeKind::RangeForStmt)
      buildable.insert(static_cast<RangeForStmt*>(n)->var);
  });
  VariableDecl* v = nullptr;
  size_t own = 0;
  for (const ReturnStmt* r : returns)
  {
    if (nested.count(r))
      continue;
    ++own;
    const Expr* e = r->value;
    if (!e || e->kind != NodeKind::NameExpr)
      return no("a return of something other than a variable's name");
    const Decl* d = static_cast<const NameExpr*>(e)->resolved;
    if (d && d->kind == NodeKind::ParamDecl)
      return no("a parameter is returned");
    if (!d || d->kind != NodeKind::VariableDecl)
      return no("a return of something other than a local variable");
    if (v && d != v)
      return no("two variables are returned");
    v = const_cast<VariableDecl*>(static_cast<const VariableDecl*>(d));
  }
  if (!v || !own)
    return no("no return names a variable");
  if (v->flags & (DF_Static | DF_ThreadLocal | DF_Extern))
    return no("the variable is static, thread_local or extern");
  if (!v->structuredBindings.empty() || v->tupleBinding)
    return no("the variable is a structured binding");
  if (v->when && v->whenKnown == 0)
    return no("the variable is absent by its when");
  if (v->alignment)
    return no("the variable is declared alignas (the result's object has its class's alignment)");
  if (!buildable.count(v) || nested.count(v))
    return no("the variable is no declaration statement's or range-for's (a catch's parameter, a compile-time loop's)");
  return v;
}

auto GlobalScope::Impl::constNamedReturn(const Stmt* body, const Ty* result, const TypeContext& ctx, int depth) -> const VariableDecl*
{
  if (!body || !result || result->isReference() || types.unqualified(result)->kind != TyKind::Class)
    return nullptr;
  VariableDecl* candidate = nullptr;
  if (constEvaluating() > 0)
  {
    auto [it, added] = constNamedReturns().try_emplace(body, nullptr);
    if (added)
      it->second = namedReturnCandidate(body, nullptr);
    candidate = it->second;
  }
  else
    candidate = namedReturnCandidate(body, nullptr);
  if (!candidate)
    return nullptr;
  const Ty* vt = declaredTypeOf(candidate, ctx.subst, depth + 1);
  if (!vt || vt->isVolatile || vt->kind != TyKind::Class || types.unqualified(vt) != types.unqualified(result))
    return nullptr;
  return candidate;
}

auto GlobalScope::Impl::conversionFunctionTo(const Ty* a, const Ty* target, Subst& subst, int baseDepth) -> FunctionDecl*
{
  if (baseDepth > 8 || !target)
    return nullptr;
  Scope* s = classScopeOf(a);
  if (!s || !s->owner || s->owner->kind != NodeKind::ClassDecl)
    return nullptr;
  Subst own = substFor(a);
  for (Decl* m : membersRead(static_cast<ClassDecl*>(s->owner)))
  {
    if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Conversion) || !m->templateParams.empty())
      continue;
    const Ty* ft = declaredTypeOf(m, own.pairs.empty() ? nullptr : &own, 0);
    if (ft && ft->kind == TyKind::Function && types.unqualified(ft->inner) == types.unqualified(target))
    {
      subst = own;
      return static_cast<FunctionDecl*>(m);
    }
  }
  ClassDecl* def = classDefinition(a);
  std::vector<const Ty*> bases;
  if (def && directBases(a, def, bases))
    for (const Ty* b : bases)
      if (b->kind == TyKind::Class)
        if (FunctionDecl* f = conversionFunctionTo(b, target, subst, baseDepth + 1))
          return f;
  return nullptr;
}

auto GlobalScope::Impl::pushEnclosingTemplateParams(Decl* d, LocalStack& own) -> void
{
  for (Decl* p = d; p; p = p->parent)
    for (Decl* tp : p->templateParams)
      own.push(static_cast<TemplateParamDecl*>(tp)->name, tp);
}

auto GlobalScope::Impl::constraintValue(Expr* e, Decl* f, const Subst& s, int depth) -> std::optional<bool>
{
  if (!e || depth > 16)
    return std::nullopt;
  switch (e->kind)
  {
    case NodeKind::ParenExpr: return constraintValue(static_cast<ParenExpr*>(e)->inner, f, s, depth + 1);
    case NodeKind::UnaryExpr:
    {
      auto* u = static_cast<UnaryExpr*>(e);
      if (u->isKeyword ? Kw(u->op) == Kw::not_ : P(u->op) == P::Bang)
      {
        std::optional<bool> v = constraintValue(u->operand, f, s, depth + 1);
        return v ? std::optional<bool>(!*v) : std::nullopt;
      }
      break;
    }
    case NodeKind::BinaryExpr:
    {
      auto* b = static_cast<BinaryExpr*>(e);
      bool andOp = b->isKeyword ? Kw(b->op) == Kw::and_ : P(b->op) == P::AndAnd;
      bool orOp = b->isKeyword ? Kw(b->op) == Kw::or_ : P(b->op) == P::OrOr;
      if (!andOp && !orOp)
        break;
      std::optional<bool> l = constraintValue(b->lhs, f, s, depth + 1), r = constraintValue(b->rhs, f, s, depth + 1);
      if (andOp)
        return (l && !*l) || (r && !*r) ? std::optional<bool>(false) : (l && r ? std::optional<bool>(true) : std::nullopt);
      return (l && *l) || (r && *r) ? std::optional<bool>(true) : (l && r ? std::optional<bool>(false) : std::nullopt);
    }
    case NodeKind::RequiresExpr:
      // `requires requires (const T& value, PropertyTree& ptree) { value.save(ptree); }`
      return requirementsHold(static_cast<RequiresExpr*>(e), f, s);
    default:
      break;
  }
  LocalStack own;
  pushEnclosingTemplateParams(f, own);
  TypeContext ctx{lexicalScopeOf(f), &own, &s, f->file ? f->file->tokens : nullptr, f->file ? f->file->text : std::string_view()};
  if (e->kind == NodeKind::NameExpr)
  {
    auto* n = static_cast<NameExpr*>(e);
    if (n->name.parts.size() == 1 && n->name.parts[0].hasTemplateArgs && isBoolTraitName(n->name.parts[0].symbol))
    {
      std::vector<const Ty*> args = traitArgs(n->name.parts[0], ctx, depth + 1);
      if (args.empty())
      {
        // Arguments that do not form for known parameters (`decltype(declval<T>().str())` for
        // a T without str()): a substitution failure, so not satisfied, as C++ has it.
        for (Decl* p = f; p; p = p->parent)
          for (Decl* tp : p->templateParams)
            if (const Ty* t = s.find(tp); !t || !t->isKnown() || types.isDependent(t) || mentionsUnknown(t))
              return std::nullopt;
        return false;
      }
      return traitValue(symbols.name(n->name.parts[0].symbol), args);
    }
    // `has_str<T>` with `template<class T> concept has_str = requires(const T& value) { value.str(); };`.
    if (n->name.last().hasTemplateArgs)
      if (VariableDecl* concept_ = conceptOf(lookupQualified(n->name, ctx.scope, &own, depth + 1).decl))
        return conceptValue(concept_, n->name.last(), ctx, depth + 1);
  }
  return constCondition(e, ctx, depth + 1);
}

auto GlobalScope::Impl::primaryVariableTemplate(const Lookup& l) -> VariableDecl*
{
  auto isPrimary = [](Decl* d) {
    return d && d->kind == NodeKind::VariableDecl && !d->templateParams.empty() && static_cast<VariableDecl*>(d)->init &&
           !static_cast<VariableDecl*>(d)->name.empty() && !static_cast<VariableDecl*>(d)->name.last().hasTemplateArgs;
  };
  if (isPrimary(l.decl))
    return static_cast<VariableDecl*>(l.decl);
  if (l.decl && l.decl->kind == NodeKind::VariableDecl && l.overloads)
    for (const Entry& e : *l.overloads)
      if (isPrimary(e.decl))
        return static_cast<VariableDecl*>(e.decl);
  return nullptr;
}

auto GlobalScope::Impl::variableTemplateSource(VariableDecl* v, const NamePart& part, const TypeContext& ctx, int depth, bool dependent, Subst& out) -> VariableDecl*
{
  size_t n = v->templateParams.size();
  if (n == 0 || depth > 16)
    return nullptr;
  auto* lastParam = static_cast<TemplateParamDecl*>(v->templateParams[n - 1]);
  std::vector<const Ty*> given, packElements;
  for (Node* written : part.templateArgs)
  {
    size_t i = given.size() + packElements.size();
    Decl* param = v->templateParams[std::min(i, n - 1)];
    const Ty* a = templateArgument(written, param, ctx, depth + 1);
    if (!a || !a->isKnown() || (types.isDependent(a) && !dependent))
      return nullptr;
    std::vector<const Ty*> elements;
    if (written && written->kind == NodeKind::PackType && a->kind == TyKind::Pack)
      elements.assign(a->args.begin(), a->args.end());
    else
      elements.push_back(a);
    for (const Ty* t : elements)
      (lastParam->isPack && given.size() + 1 >= n ? packElements : given).push_back(t);
  }
  if (given.size() > n || (!lastParam->isPack && !packElements.empty()))
    return nullptr;
  Subst bound;
  for (size_t i = 0; i < given.size(); ++i)
    bound.pairs.push_back({v->templateParams[i], given[i]});
  if (lastParam->isPack)
    bound.pairs.push_back({lastParam, types.pack(Span<const Ty*>{packElements.data(), uint32_t(packElements.size())})});
  LocalStack own;
  pushEnclosingTemplateParams(v, own);
  for (size_t i = bound.pairs.size(); i < n; ++i)
  {
    auto* p = static_cast<TemplateParamDecl*>(v->templateParams[i]);
    TypeContext dctx{lexicalScopeOf(v), &own, &bound, v->file ? v->file->tokens : nullptr, v->file ? v->file->text : std::string_view()};
    const Ty* t = p->defaultArg ? templateArgument(p->defaultArg, p, dctx, depth + 1) : nullptr;
    if (!t || !t->isKnown() || (types.isDependent(t) && !dependent))
      return nullptr; // `isX<A>` naming fewer arguments than the template has, and no default
    bound.pairs.push_back({p, t});
  }
  std::vector<const Ty*> args;
  for (const Subst::Pair& q : bound.pairs)
    args.push_back(q.type);
  Subst specialised;
  VariableDecl* source = lastParam->isPack ? v : variableSpecialization(v, args, specialised);
  if (source)
    out = source == v ? bound : specialised;
  return source;
}

auto GlobalScope::Impl::variableTemplateValue(VariableDecl* v, const NamePart& part, const TypeContext& ctx, int depth) -> std::optional<ConstValue>
{
  Subst sourceSubst;
  VariableDecl* source = variableTemplateSource(v, part, ctx, depth, false, sourceSubst);
  if (!source || !source->init)
    return std::nullopt;
  LocalStack sourceOwn;
  pushEnclosingTemplateParams(source, sourceOwn);
  TypeContext vctx{lexicalScopeOf(source), &sourceOwn, &sourceSubst, source->file ? source->file->tokens : nullptr,
                   source->file ? source->file->text : std::string_view()};
  vctx.variableTemplate = source;
  return constAsDeclared(constInitValue(source->init, vctx, depth + 1), source->type ? convertType(source->type, vctx, depth + 1) : nullptr);
}

auto GlobalScope::Impl::variableTemplateType(VariableDecl* v, const NamePart& part, const TypeContext& ctx) -> const Ty*
{
  Subst sourceSubst;
  VariableDecl* source = variableTemplateSource(v, part, ctx, 0, true, sourceSubst);
  if (!source)
    return nullptr;
  if (ctx.subst) // inside an instance: its arguments stay for what the declaration names of its own
    for (const Subst::Pair& p : ctx.subst->pairs)
      if (!sourceSubst.find(p.param))
        sourceSubst.pairs.push_back(p);
  const Ty* t = declaredTypeOf(source, &sourceSubst, 0);
  return t && t->isKnown() && !containsAuto(t) ? t : nullptr;
}

auto GlobalScope::Impl::conceptOf(Decl* d) -> VariableDecl*
{
  auto* v = d && d->kind == NodeKind::VariableDecl ? static_cast<VariableDecl*>(d) : nullptr;
  return v && !v->type && v->init && !v->templateParams.empty() ? v : nullptr;
}

auto GlobalScope::Impl::conceptValue(VariableDecl* concept_, const NamePart& part, const TypeContext& ctx, int depth) -> std::optional<bool>
{
  if (!concept_ || depth > 16 || concept_->templateParams.empty())
    return std::nullopt;
  // The written arguments, an expansion (`Args&&...`) giving its elements; a trailing pack
  // parameter takes the rest (`has_load<T, Input, Args&&...>` with Args empty: none).
  size_t n = concept_->templateParams.size();
  std::vector<const Ty*> given;
  for (size_t i = 0; i < part.templateArgs.size(); ++i)
  {
    Node* written = part.templateArgs[i];
    Decl* param = concept_->templateParams[std::min(given.size(), n - 1)];
    const Ty* a = templateArgument(written, param, ctx, depth + 1);
    if (!a || !a->isKnown() || types.isDependent(a))
      return std::nullopt;
    if (written && written->kind == NodeKind::PackType)
    {
      if (a->kind != TyKind::Pack)
        return std::nullopt;
      given.insert(given.end(), a->args.begin(), a->args.end());
    }
    else
      given.push_back(a);
  }
  Subst cs;
  for (size_t i = 0; i < n; ++i)
  {
    auto* param = static_cast<TemplateParamDecl*>(concept_->templateParams[i]);
    if (param->isPack && i + 1 == n)
    {
      std::vector<const Ty*> rest(given.begin() + std::min(i, given.size()), given.end());
      cs.pairs.push_back({param, types.pack(Span<const Ty*>{rest.data(), uint32_t(rest.size())})});
      break;
    }
    if (i >= given.size())
      return std::nullopt; // fewer arguments than parameters (defaults: not read here)
    cs.pairs.push_back({param, given[i]});
  }
  if (given.size() > n && !static_cast<TemplateParamDecl*>(concept_->templateParams[n - 1])->isPack)
    return std::nullopt;
  if (concept_->init->kind == NodeKind::RequiresExpr)
    return requirementsHold(static_cast<RequiresExpr*>(concept_->init), concept_, cs);
  return constraintValue(concept_->init, concept_, cs, depth + 1);
}

auto GlobalScope::Impl::requirementsHold(RequiresExpr* r, Decl* owner, const Subst& s) -> std::optional<bool>
{
  // Not for arguments that are not known: the requirements would be probed on unknown types,
  // and `requires (const T::value_type& v) { toPtree(v); }` would ask the same of toPtree again.
  for (const Subst::Pair& q : s.pairs)
    if (mentionsUnknown(q.type))
      return std::nullopt;
  // A constraint whose satisfaction depends on itself (ill-formed in C++), or a chain of
  // probes too deep to follow: not satisfied.
  struct InProgress { RequiresExpr* r; std::vector<Subst::Pair> pairs; };
  static thread_local std::vector<InProgress> inProgress;
  for (const InProgress& p : inProgress)
    if (p.r == r && p.pairs.size() == s.pairs.size() &&
        std::equal(p.pairs.begin(), p.pairs.end(), s.pairs.begin(), [](const Subst::Pair& x, const Subst::Pair& y) { return x.param == y.param && x.type == y.type; }))
      return false;
  if (inProgress.size() >= 16)
    return false;
  inProgress.push_back({r, std::vector<Subst::Pair>(s.pairs.begin(), s.pairs.end())});
  struct Pop { ~Pop() { inProgress.pop_back(); } } pop;
  ParsedFile* file = owner->file;
  if (!file || !file->tokens)
    return std::nullopt;
  OrdinaryLayout ordinary;
  ResolveStats scratch;
  Walker probe(*this, scratch, *file->tokens, file->text);
  probe.file = file;
  FileOfLookups walkedFile(file);
  probe.scope = lexicalScopeOf(owner);
  probe.activeSubst = &s;
  probe.probing = true;
  pushEnclosingTemplateParams(owner, probe.locals);
  for (Decl* p : r->params)
    probe.locals.push(static_cast<ParamDecl*>(p)->name, p);
  // Each requirement probed on a copy of its own (the template's nodes, which other probes share,
  // keep nothing of these arguments), its names' access checked where owner is: a concept's
  // requirements name no private member, a constrained member's those its class may.
  FunctionDecl* fn = owner->kind == NodeKind::FunctionDecl ? static_cast<FunctionDecl*>(owner) : nullptr;
  std::vector<Scope*> context;
  accessContext(owner, context);
  InstanceCopier copier(instanceArena());
  bool dependent = false;
  for (Type* t : r->typeRequirements)
  {
    Type* copy = copier.typeAlone(t);
    probe.walkType(copy); // (its names resolved, for their access)
    const Ty* ty = probe.typeOf(copy);
    if (!ty || !ty->isKnown())
      return false;
    if (types.isDependent(ty))
      dependent = true;
    else if (!namesOnlyAccessible(copy, fn, context, nullptr))
      return false;
  }
  for (Expr* requirement : r->requirements)
  {
    Expr* copy = copier.expression(requirement);
    const Ty* ty = probe.walkExpr(copy);
    if (!ty || !ty->isKnown() || probe.namesFunctionsWithoutTarget(copy))
      return false;
    if (types.isDependent(ty))
      dependent = true;
    else if (!namesOnlyAccessible(copy, fn, context, nullptr))
      return false;
  }
  return dependent ? std::nullopt : std::optional<bool>(true);
}

auto GlobalScope::Impl::typeOfSignatureExpression(Expr* e, const TypeContext& ctx) -> const Ty*
{
  static thread_local int nesting = 0;
  FunctionDecl* f = ctx.function;
  const std::vector<Token>* tokens = f && f->file ? f->file->tokens : ctx.tokens ? ctx.tokens : fallbackTokens; // an alias template's: its own
  std::string_view text = f && f->file ? f->file->text : ctx.tokens ? ctx.text : fallbackText;
  if (!tokens || nesting > 8)
    return nullptr;
  OrdinaryLayout ordinary;
  ++nesting;
  InstanceCopier copier(instanceArena());
  Expr* copy = copier.expression(e);
  ResolveStats scratch;
  Walker probe(*this, scratch, *tokens, text);
  probe.file = f ? f->file : nullptr;
  FileOfLookups walkedFile(f ? f->file : nullptr);
  probe.scope = ctx.scope;
  probe.activeSubst = ctx.subst;
  if (ctx.locals)
    probe.locals = *ctx.locals;
  if (f)
    for (Decl* p : f->params)
      probe.locals.push(static_cast<ParamDecl*>(p)->name, p);
  const Ty* t = probe.walkExpr(copy);
  --nesting;
  return t;
}

auto GlobalScope::Impl::undeclaredNameIn(Expr* e, Decl* f, const LocalStack& own, int depth) -> std::string
{
  if (!e || depth > 16)
    return {};
  switch (e->kind)
  {
    case NodeKind::ParenExpr: return undeclaredNameIn(static_cast<ParenExpr*>(e)->inner, f, own, depth + 1);
    case NodeKind::UnaryExpr: return undeclaredNameIn(static_cast<UnaryExpr*>(e)->operand, f, own, depth + 1);
    case NodeKind::BinaryExpr:
    {
      std::string l = undeclaredNameIn(static_cast<BinaryExpr*>(e)->lhs, f, own, depth + 1);
      return l.empty() ? undeclaredNameIn(static_cast<BinaryExpr*>(e)->rhs, f, own, depth + 1) : l;
    }
    case NodeKind::NameExpr:
    {
      auto* n = static_cast<NameExpr*>(e);
      if (n->name.empty() || (n->name.parts.size() == 1 && (isBoolTraitName(n->name.parts[0].symbol) || builtinByName(n->name.parts[0].symbol))))
        return {};
      Lookup l = lookupQualified(n->name, lexicalScopeOf(f), &own, depth + 1);
      if (l.found != Found::No || l.decl)
        return {};
      std::string name;
      for (const NamePart& part : n->name.parts)
        name += (name.empty() ? "" : "::") + std::string(part.symbol == UINT32_MAX ? "" : symbols.name(part.symbol));
      return name;
    }
    default:
      return {};
  }
}

auto GlobalScope::Impl::requiresHolds(FunctionDecl* f, const Subst& deduced) -> bool
{
  // No requires clause holds (convertsByOperator asks for every conversion template, a
  // `template<class T> operator T()` without one too).
  if (!f->requiresClause)
    return true;
  std::optional<bool> v = constraintValue(f->requiresClause, f, deduced);
  if (v)
    return *v;
  for (Decl* p = f; p; p = p->parent)
    for (Decl* tp : p->templateParams)
    {
      const Ty* t = deduced.find(tp);
      if (!t || !t->isKnown() || types.isDependent(t) || mentionsUnknown(t))
        return true;
    }
  LocalStack own;
  pushEnclosingTemplateParams(f, own);
  std::string undeclared = undeclaredNameIn(f->requiresClause, f, own);
  std::string name = f->name.empty() ? std::string("?") : debugName(f->name.last().symbol);
  if (undeclared.empty())
  {
    std::string args;
    for (Decl* p = f; p; p = p->parent)
      for (Decl* tp : p->templateParams)
        args += (args.empty() ? "" : ", ") + std::string(symbols.name(static_cast<TemplateParamDecl*>(tp)->name)) + " = " +
                types.toString(deduced.find(tp), &symbols);
    reportCheckerError(f->file, f->requiresClause->token, "the requires clause of " + name + " cannot be evaluated for " + args);
    return false;
  }
  reportCheckerError(f->file, f->requiresClause->token, "the requires clause of " + name + " names '" + undeclared + "', which is not declared");
  return false;
}

auto GlobalScope::Impl::orderingParam(FunctionDecl* f, const Ty* ft, size_t i, bool& fromPack) -> const Ty*
{
  fromPack = false;
  size_t n = std::min<size_t>(f->params.size(), ft->args.size());
  for (size_t k = 0; k < n; ++k)
    if (static_cast<ParamDecl*>(f->params[k])->isPack)
    {
      if (k + 1 != f->params.size())
        return nullptr;
      if (i >= k)
      {
        fromPack = true;
        return ft->args[k];
      }
      break;
    }
  return i < n ? ft->args[i] : nullptr;
}

auto GlobalScope::Impl::orderingDeduces(const Ty* p, bool pFromPack, const Ty* a, bool aFromPack, Subst& s) -> bool
{
  if (aFromPack && !pFromPack)
    return false;
  const Ty* pt = types.unqualified(types.stripRef(p));
  const Ty* at = types.unqualified(types.stripRef(a));
  std::vector<Decl*> packs;
  if (pFromPack)
    packParamsIn(pt, packs);
  auto isPack = [&](Decl* d) { return std::find(packs.begin(), packs.end(), d) != packs.end(); };
  Subst trial;
  for (const Subst::Pair& q : s.pairs)
    if (!isPack(q.param))
      trial.pairs.push_back(q);
  deduce(pt, at, trial, 1);
  if (substitute(pt, trial) != at)
    return false;
  s.pairs.clear();
  for (const Subst::Pair& q : trial.pairs)
    if (!isPack(q.param))
      s.pairs.push_back(q);
  return true;
}

auto GlobalScope::Impl::moreQualified(const Ty* x, const Ty* y) -> bool
{
  return (x->isConst || !y->isConst) && (x->isVolatile || !y->isVolatile) && (x->isConst != y->isConst || x->isVolatile != y->isVolatile);
}

auto GlobalScope::Impl::moreSpecialized(FunctionDecl* a, FunctionDecl* b, size_t count, const Subst* classSubst) -> bool
{
  const Ty* at = declaredTypeOf(a, classSubst, 0);
  const Ty* bt = declaredTypeOf(b, classSubst, 0);
  if (!at || !bt || at->kind != TyKind::Function || bt->kind != TyKind::Function)
    return false;
  Subst fromA, fromB; // b's parameters deduced from a's types, a's from b's
  bool aAtLeast = true, bAtLeast = true;
  for (size_t i = 0; i < count; ++i)
  {
    bool aPack = false, bPack = false;
    const Ty* ta = orderingParam(a, at, i, aPack);
    const Ty* tb = orderingParam(b, bt, i, bPack);
    if (!ta || !tb)
      return false;
    bool aOk = aAtLeast && orderingDeduces(tb, bPack, ta, aPack, fromA);
    bool bOk = bAtLeast && orderingDeduces(ta, aPack, tb, bPack, fromB);
    if (aOk && bOk && ta->isReference() && tb->isReference())
    {
      if (ta->kind == TyKind::LRef && tb->kind == TyKind::RRef)
        bOk = false;
      else if (tb->kind == TyKind::LRef && ta->kind == TyKind::RRef)
        aOk = false;
      else if (moreQualified(ta->inner, tb->inner))
        bOk = false;
      else if (moreQualified(tb->inner, ta->inner))
        aOk = false;
    }
    aAtLeast = aOk;
    bAtLeast = bOk;
    if (!aAtLeast)
      return false;
  }
  if (aAtLeast && bAtLeast)
  {
    // [temp.func.order]/5: b's trailing pack has no parameter of a's to match (it takes no argument here).
    bool aPack = false, bPack = false;
    bool aHasPack = !a->params.empty() && static_cast<ParamDecl*>(a->params[a->params.size() - 1])->isPack;
    bool bHasPack = !b->params.empty() && static_cast<ParamDecl*>(b->params[b->params.size() - 1])->isPack;
    return !aHasPack && bHasPack && !orderingParam(a, at, count, aPack) && orderingParam(b, bt, count, bPack) && bPack;
  }
  return !bAtLeast;
}

auto GlobalScope::Impl::namesDeducedParam(const Ty* t, FunctionDecl* f, const Subst& s, size_t given, int depth) -> bool
{
  if (!t || depth > 16 || t->kind == TyKind::DependentMember)
    return false;
  if (t->kind == TyKind::Param)
  {
    bool own = std::find(f->templateParams.begin(), f->templateParams.end(), t->decl) != f->templateParams.end();
    for (size_t i = 0; own && i < given && i < s.pairs.size(); ++i)
      own = s.pairs[i].param != t->decl;
    return own;
  }
  if (namesDeducedParam(t->inner, f, s, given, depth + 1))
    return true;
  for (const Ty* a : t->args)
    if (namesDeducedParam(a, f, s, given, depth + 1))
      return true;
  return false;
}

auto GlobalScope::Impl::deducedClassFits(const Ty* written, const Ty* substituted, const Ty* arg, FunctionDecl* f, const Subst& s, size_t given) -> bool
{
  const Ty* w = types.unqualified(types.stripRef(written));
  const Ty* p = types.unqualified(types.stripRef(substituted));
  const Ty* a = types.unqualified(types.stripRef(arg));
  if (w->kind == TyKind::Pointer && p->kind == TyKind::Pointer)
  {
    if (a->kind != TyKind::Pointer && a->kind != TyKind::Array)
      return true; // conversions to a pointer are ranked elsewhere
    w = types.unqualified(w->inner);
    p = types.unqualified(p->inner);
    a = types.unqualified(a->inner);
  }
  if (w->kind != TyKind::Class || !w->decl || w->args.empty() || !namesDeducedParam(w, f, s, given))
    return true;
  if (a->isBraceList() || a == p)
    return true;
  if (a->kind != TyKind::Class || !a->decl || p->kind != TyKind::Class)
    return false;
  return a->decl == p->decl || isBaseScope(classScopeOf(p), classScopeOf(a));
}

auto GlobalScope::Impl::homeScope(FunctionDecl* f) -> Scope*
{
  if (Decl* cls = owningClassOf(f); cls && cls->ownScope)
    return cls->ownScope;
  if (f->name.parts.size() > 1)
  {
    QualifiedName qualifier = f->name;
    qualifier.parts.count -= 1;
    if (Lookup l = lookupQualified(qualifier, lexicalScopeOf(f), nullptr, 0); l.scope)
      return l.scope;
  }
  return lexicalScopeOf(f);
}

auto GlobalScope::Impl::checkCFunctionTypes() -> void
{
  std::vector<std::pair<uint32_t, std::vector<FunctionDecl*>*>> names;
  for (auto& [symbol, declarations] : cFunctions)
    if (declarations.size() > 1)
      names.push_back({symbol, &declarations});
  std::sort(names.begin(), names.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  auto signature = [&](FunctionDecl* f) -> const Ty*
  {
    TokenScope tokenScope(f->file ? f->file->tokens : nullptr, f->file ? f->file->text : std::string_view());
    const Ty* t = declaredTypeOf(f, nullptr, 0);
    return t && t->kind == TyKind::Function && t->isKnown() && !mentionsUnknown(t) ? t : nullptr;
  };
  for (auto& [symbol, declarations] : names)
  {
    FunctionDecl* first = nullptr;
    const Ty* firstType = nullptr;
    for (FunctionDecl* f : *declarations)
    {
      const Ty* t = signature(f);
      if (!t)
        continue;
      if (!first)
      {
        first = f;
        firstType = t;
        continue;
      }
      bool same = t->inner == firstType->inner && t->variadic == firstType->variadic && t->args.size() == firstType->args.size();
      for (size_t i = 0; same && i < t->args.size(); ++i)
        same = types.unqualified(t->args[i]) == types.unqualified(firstType->args[i]);
      if (!same)
        reportCheckerError(f->file, f->token,
                           "the C function " + std::string(symbols.name(symbol)) + " is declared with two types: " + types.toString(t, &symbols) +
                               " here and " + types.toString(firstType, &symbols) + " at " + locationOf(first->file, first->token));
    }
  }
  cFunctions.clear();
}

auto GlobalScope::Impl::sameFunction(FunctionDecl* a, FunctionDecl* b) -> bool
{
  if (a == b)
    return true;
  // A C function's static is its linkage, which a later extern declaration takes from a visible
  // static one (C11 6.2.2p4): MSVC 14.44's time.h defines time static inline, CTime.kpp declares
  // it extern "C", one function.
  uint32_t compared = isCFunction(a) && isCFunction(b) ? DF_Const | DF_RefQual : DF_Const | DF_RefQual | DF_Static;
  if (a->params.size() != b->params.size() || a->name.empty() || b->name.empty() || functionSymbol(a->name) != functionSymbol(b->name) || !sameNamePart(a->name.last(), b->name.last()) ||
      a->templateParams.size() != b->templateParams.size() || (a->flags & compared) != (b->flags & compared))
    return false;
  // A function declared `extern "C"` and then defined without it is one C function (C++
  // [dcl.link] 5: the later declaration takes the earlier one's linkage), as Lua's
  // lua_getnresults is in lua.kpp and ldebug.kpp.
  if (!(isCFunction(a) && isCFunction(b)) && homeScope(a) != homeScope(b))
    return false;
  return typeAs(b, a) == matchedType(a);
}

auto GlobalScope::Impl::chooseAmong(const std::vector<FunctionDecl*>& candidates, const std::vector<const Ty*>& argTypes, const Subst* classSubst, const Ty* objectTy, bool& ambiguous, const std::function<bool(FunctionDecl*, Subst&)>* bindExplicit, const std::vector<bool>* nullConstants, std::vector<FunctionDecl*>* tiedOut) -> FunctionDecl*
{
  bool argsConcrete = true;
  for (const Ty* at : argTypes)
    argsConcrete = argsConcrete && at && at->isKnown() && !types.isDependent(at);
  std::vector<Candidate> viable;
  bool objectConst = objectTy && types.stripRef(objectTy)->isConst;
  bool single = candidates.size() == 1; // One candidate: arity decides, no ranking needed.
  for (FunctionDecl* f : candidates)
  {
    if (isFunctionSpecialization(f))
      continue; // `template<> ForceID parse<ForceID>(...)`: its template is the candidate
    size_t required = 0, total = f->params.size();
    bool variadic = (f->flags & DF_Variadic) != 0;
    for (Decl* p : f->params)
    {
      auto* param = static_cast<ParamDecl*>(p);
      if (param->isPack)
        variadic = true;
      else if (!param->defaultValue)
        ++required;
    }
    // A Text parameter (design.md 14), the last, takes the remaining arguments, however many.
    size_t textAt = SIZE_MAX;
    if (!variadic && total > 0 && argTypes.size() + 1 >= required && namesTextLast(f) && !static_cast<ParamDecl*>(f->params[total - 1])->defaultValue)
    {
      textAt = textParameterOf(declaredTypeOf(f, classSubst, 0));
      if (textAt != SIZE_MAX)
        --required;
    }
    if (argTypes.size() < required || (!variadic && textAt == SIZE_MAX && argTypes.size() > total))
      continue;
    Candidate c{f, {}, 0, {}};
    bool constMethod = (f->flags & DF_Const) != 0;
    if (objectTy && !(f->flags & DF_Static) && !single)
    {
      if (objectConst && !constMethod)
        continue;
      c.thisRank = (!objectConst && constMethod) ? 1 : 0;
    }
    const Ty* ft = declaredTypeOf(f, classSubst, 0);
    Subst deduced = classSubst ? *classSubst : Subst{};
    if (bindExplicit && !(*bindExplicit)(f, deduced))
      continue;
    size_t givenCount = deduced.pairs.size(); // the class's and the explicit arguments: not deduced
    if (!f->templateParams.empty() && ft && ft->kind == TyKind::Function)
    {
      deduceCall(f, ft, argTypes, deduced);
      if (!fillDefaultTemplateArgs(f, deduced))
        continue; // a default template argument that is no type: not a candidate
      if (argsConcrete)
      {
        bool complete = true;
        for (Decl* p : f->templateParams)
          complete = complete && (deduced.find(p) || static_cast<TemplateParamDecl*>(p)->isPack);
        if (!complete)
          continue;
      }
    }
    bool ok = true;
    for (size_t i = 0; i < argTypes.size() && ok; ++i)
    {
      const Ty* pt = ft && ft->kind == TyKind::Function && i < ft->args.size() ? ft->args[i] : nullptr;
      if (i >= textAt && !(argTypes.size() == textAt + 1 && isText(argTypes[i])))
      {
        // A piece of the Text: packed as C's `...` takes an argument, worse than any standard or
        // user-defined conversion, so an overload taking the arguments as they are wins. Whether
        // its type has appendTo is checked once the call is chosen (an error, never another
        // overload). A brace list has no type to be a piece of.
        if (argTypes[i] && argTypes[i]->isBraceList())
          ok = false;
        c.ranks.push_back(6);
        c.refs.push_back(false);
        c.params.push_back(nullptr);
        continue;
      }
      if (!pt)
      {
        // The C ellipsis (`f(...)`, `printf(const char*, ...)`): an ellipsis conversion, worse than
        // any standard or user-defined one ([over.ics.rank] 2), so `f(FromInt)` beats `f(...)`
        // given an int; a pack's element not known yet stays a conversion.
        c.ranks.push_back((f->flags & DF_Variadic) ? 6 : 3);
        c.refs.push_back(false);
        c.params.push_back(nullptr);
        continue;
      }
      if (!f->templateParams.empty())
      {
        const Ty* written = pt;
        pt = substitute(pt, deduced);
        const Ty* at = argTypes[i];
        if (at && at->isKnown() && !types.isDependent(at) && dependsOn(pt, f->templateParams))
        {
          // Deduction failed (`operator<<(UniquePtr<T>&)` given a Label&): not a candidate.
          ok = false;
          break;
        }
        if (at && at->isKnown() && !types.isDependent(at) && !deducedClassFits(written, pt, at, f, deduced, givenCount))
        {
          // `Direction != Direction` against `operator!=(const B&, const GameOptional<B>&)`: a
          // parameter deduction reads takes its template's instantiations (and classes derived
          // from one) only; C++ converts no argument to make it deducible.
          ok = false;
          break;
        }
      }
      if (pt->kind == TyKind::Pack)
      {
        // A deduced parameter pack: the remaining arguments match its elements one by one.
        for (size_t j = i; j < argTypes.size() && ok; ++j)
        {
          const Ty* et = j - i < pt->args.size() ? pt->args[j - i] : nullptr;
          int r = et ? conversionRank(argTypes[j], et) : 3;
          if (r >= NotViable)
            ok = false;
          c.ranks.push_back(r);
          c.refs.push_back(et && et->isReference());
          c.params.push_back(et);
        }
        break;
      }
      int r = conversionRank(argTypes[i], pt);
      if (r >= NotViable && nullConstants && i < nullConstants->size() && (*nullConstants)[i])
      {
        // `CoInitializeEx(0, ...)`, `f(0)` with `f(Entity*)`: the literal 0 is a null pointer, a
        // conversion as int's to another arithmetic type is.
        // It reaches a class by a converting constructor taking a pointer (`f(FromText)` with
        // `FromText(const char*)`, a user-defined conversion), as nullptr does.
        const Ty* target = types.unqualified(types.stripRef(pt));
        if (target->kind == TyKind::Pointer || target->kind == TyKind::MemberPointer)
          r = 3;
        else if (target->kind == TyKind::Class)
          r = std::max(conversionRank(types.nullptrType(), pt), 4);
      }
      if (r >= NotViable)
        ok = false;
      c.ranks.push_back(r);
      c.refs.push_back(pt->isReference());
      c.params.push_back(pt);
    }
    if (ok && f->requiresClause && !requiresHolds(f, deduced))
      ok = false; // `requires isIntegral<T>`: the constraint fails for these arguments
    c.emptyText = textAt != SIZE_MAX && argTypes.size() <= textAt;
    if (ok)
      viable.push_back(std::move(c));
  }
  ambiguous = false;
  if (viable.empty())
    return nullptr;
  auto better = [&](const Candidate& a, const Candidate& b)
  {
    bool someBetter = false;
    for (size_t i = 0; i < a.ranks.size(); ++i)
    {
      // A brace list made an InitializerList beats one made anything else, whatever the other
      // rules say (C++ [over.ics.rank] 3.1): `FlatSet<ChunkPosition>({{-1, -1}, {0, -1}})` takes
      // the InitializerList constructor, not the move constructor through a conversion by it.
      if (i < argTypes.size() && argTypes[i] && types.stripRef(argTypes[i])->isBraceList() && i < a.params.size() && i < b.params.size() &&
          a.params[i] && b.params[i])
      {
        bool listA = isInitializerList(types.unqualified(types.stripRef(a.params[i])));
        bool listB = isInitializerList(types.unqualified(types.stripRef(b.params[i])));
        if (listA != listB)
        {
          if (listB)
            return false;
          someBetter = true;
          continue;
        }
      }
      // Exact and const-qualified-reference bindings differ only between two references
      // (`T&` beats `const T&`); a reference binding is an identity against a by-value parameter,
      // the same match as a by-value identity and better than a by-value qualification conversion
      // (`const Entity*` given an Entity*, [over.ics.rank] 3.2.5): `f(const T&)` with T = Derived*
      // beats `f(const T*)` with T = Derived given a Derived*. Between two by-value parameters
      // the qualification conversion loses to the identity, below.
      if (a.refs[i] != b.refs[i] && a.ranks[i] <= 1 && b.ranks[i] <= 1)
      {
        int ea = a.refs[i] ? 0 : a.ranks[i];
        int eb = b.refs[i] ? 0 : b.ranks[i];
        if (ea > eb)
          return false;
        if (ea < eb)
          someBetter = true;
        continue;
      }
      // Two user-defined conversions through the same conversion function: the standard
      // conversion after it decides ([over.ics.rank] 3.3). A class whose one conversion to a
      // non-class type is `operator bool()` takes push(const bool&) over push(uint64_t) and
      // take(bool) over take(int64_t): bool's identity beats a conversion.
      if (a.ranks[i] >= 4 && a.ranks[i] <= 5 && b.ranks[i] >= 4 && b.ranks[i] <= 5 && i < argTypes.size() && argTypes[i] && i < a.params.size() &&
          i < b.params.size() && a.params[i] && b.params[i])
      {
        const Ty* from = types.unqualified(types.stripRef(argTypes[i]));
        const Ty* pa = types.unqualified(types.stripRef(a.params[i]));
        const Ty* pb = types.unqualified(types.stripRef(b.params[i]));
        if (from->kind == TyKind::Class && !from->isBraceList() && pa != pb && pa->kind != TyKind::Class && pb->kind != TyKind::Class)
          if (const Ty* result = scalarConversionOf(from))
          {
            result = types.unqualified(types.stripRef(result));
            int ra = conversionRank(result, pa), rb = conversionRank(result, pb);
            if (ra != rb)
            {
              if (ra > rb)
                return false;
              someBetter = true;
              continue;
            }
          }
      }
      // Two user-defined conversions to different types (a `const char*` made a StringView by
      // one candidate, a String by the other) are indistinguishable (C++ compares only those
      // through one conversion function): the other arguments and the object decide.
      if (a.ranks[i] >= 4 && a.ranks[i] <= 5 && b.ranks[i] >= 4 && b.ranks[i] <= 5 && i < a.params.size() && i < b.params.size() &&
          a.params[i] && b.params[i] && types.unqualified(types.stripRef(a.params[i])) != types.unqualified(types.stripRef(b.params[i])))
        continue;
      // One user-defined conversion to one class, the object taken by value by one candidate and
      // bound to a `const T&` by the other: the same sequence (only two reference bindings
      // compare, `T&&` over `const T&`): `f(FromInt)` beside `f(const FromInt&)` given a float is
      // ambiguous, as in C++.
      if (a.ranks[i] >= 4 && a.ranks[i] <= 5 && b.ranks[i] >= 4 && b.ranks[i] <= 5 && i < a.params.size() && i < b.params.size() &&
          a.params[i] && b.params[i] && a.params[i]->isReference() != b.params[i]->isReference())
        continue;
      if (a.ranks[i] > b.ranks[i])
        return false;
      if (a.ranks[i] < b.ranks[i])
        someBetter = true;
      // Two promotions of an enumeration with a fixed underlying type: the one to that type is
      // better than the one to the type it promotes to (C++ [over.ics.rank] 4.2): take(uint8)
      // over take(int32) given an `enum Small : uint8_t`.
      if (a.ranks[i] == 2 && b.ranks[i] == 2 && i < argTypes.size() && argTypes[i] && i < a.params.size() && i < b.params.size() &&
          a.params[i] && b.params[i])
        if (const Ty* e = types.unqualified(types.stripRef(argTypes[i])); e->kind == TyKind::Enum)
          if (const Ty* u = declaredUnderlying(e))
          {
            bool aU = types.unqualified(types.stripRef(a.params[i])) == u;
            bool bU = types.unqualified(types.stripRef(b.params[i])) == u;
            if (aU != bU)
            {
              if (bU)
                return false;
              someBetter = true;
            }
          }
      // A null pointer constant (nullptr, the literal 0) converts to every pointer alike: none of
      // the pointer tie-breakers below compares two such conversions (`pick(nullptr)` with
      // pick(Base*) and pick(const Base*) is ambiguous, as in C++).
      const Ty* argument = i < argTypes.size() && argTypes[i] ? types.unqualified(types.stripRef(argTypes[i])) : nullptr;
      bool nullArgument = (argument && argument->kind == TyKind::Nullptr) || (nullConstants && i < nullConstants->size() && (*nullConstants)[i]);
      // Of two conversions of one rank, one that makes a pointer (or a member pointer) a bool is
      // the worse ([over.ics.rank] 4.1): pick(const void*) over pick(bool) given a Derived*.
      if (a.ranks[i] == b.ranks[i] && a.ranks[i] == 3 && argument &&
          (argument->kind == TyKind::Pointer || argument->kind == TyKind::MemberPointer || argument->kind == TyKind::Array || argument->kind == TyKind::Function) &&
          i < a.params.size() && i < b.params.size() && a.params[i] && b.params[i])
      {
        bool aBool = types.unqualified(types.stripRef(a.params[i]))->kind == TyKind::Bool;
        bool bBool = types.unqualified(types.stripRef(b.params[i]))->kind == TyKind::Bool;
        if (aBool != bBool)
        {
          if (aBool)
            return false;
          someBetter = true;
          continue;
        }
      }
      // Two standard conversions of one rank to pointers (or to references) of related types, C++'s
      // tie-breakers: `pick(spider)` with a Spider* takes pick(Entity*) over pick(const Entity*)
      // (the same conversion and a qualification more), and pick(Unit*) over pick(Entity*) for
      // Unit deriving from Entity (the conversion to the nearer base).
      if (a.ranks[i] == b.ranks[i] && (a.ranks[i] == 2 || a.ranks[i] == 3) && i < a.params.size() && i < b.params.size() &&
          a.params[i] && b.params[i] && !nullArgument)
      {
        const Ty* pa = types.unqualified(types.stripRef(a.params[i]));
        const Ty* pb = types.unqualified(types.stripRef(b.params[i]));
        const Ty* ta = nullptr;
        const Ty* tb = nullptr;
        if (pa->kind == TyKind::Pointer && pb->kind == TyKind::Pointer)
          ta = pa->inner, tb = pb->inner;
        else if (a.params[i]->isReference() && b.params[i]->isReference() && a.params[i]->kind == b.params[i]->kind)
          ta = types.stripRef(a.params[i]), tb = types.stripRef(b.params[i]);
        if (ta && tb && ta != tb)
        {
          bool aWeaker = (!ta->isConst || tb->isConst) && (!ta->isVolatile || tb->isVolatile); // a's target no more qualified
          bool bWeaker = (!tb->isConst || ta->isConst) && (!tb->isVolatile || ta->isVolatile);
          const Ty* ua = types.unqualified(ta);
          const Ty* ub = types.unqualified(tb);
          if (ua == ub)
          {
            if (aWeaker && !bWeaker)
              someBetter = true;
            else if (bWeaker && !aWeaker)
              return false;
          }
          else if (ua->kind == TyKind::Class && ub->kind == TyKind::Class && aWeaker && bWeaker)
          {
            if (isBaseClass(ub, ua))
              someBetter = true;
            else if (isBaseClass(ua, ub))
              return false;
          }
          else if (ua->kind != ub->kind && (ua->kind == TyKind::Void || ub->kind == TyKind::Void) && aWeaker && bWeaker)
          {
            // A pointer to a class converted to its base's pointer is better than to void*
            // ([over.ics.rank] 4.4.2): getPrototypeInfoStatic(const PrototypeBase*, ...) over
            // getPrototypeInfoStatic(const void*, ...) given a const ItemPrototype*.
            if (ub->kind == TyKind::Void && ua->kind == TyKind::Class)
              someBetter = true;
            else if (ua->kind == TyKind::Void && ub->kind == TyKind::Class)
              return false;
          }
        }
      }
      // An rvalue reference beside an lvalue one of the same rank: the `T&&` binds an rvalue,
      // the argument or the temporary it converts to (it takes no lvalue), which C++ prefers
      // ([over.ics.rank] 3.2.3): `Holder(Holder&& other) : UniquePtr<Entity>(move(other))` takes
      // UniquePtr's move constructor, not its deleted copy; take(const T&) beside take(T&&) given
      // an Item* for T = const Item*, or an int32 for T = int64, takes T&&.
      if (a.ranks[i] == b.ranks[i] && i < a.params.size() && i < b.params.size() && a.params[i] && b.params[i] &&
          a.params[i]->kind != b.params[i]->kind && a.params[i]->isReference() && b.params[i]->isReference())
      {
        if (a.params[i]->kind == TyKind::RRef)
          someBetter = true;
        else
          return false;
      }
    }
    if (a.thisRank > b.thisRank)
      return false;
    if (a.thisRank < b.thisRank)
      someBetter = true;
    // The arguments match alike, and one candidate's Text parameter takes none of them (design.md
    // 14): the other wins, before the template tie-breakers, as it would if the Text took a piece
    // (C's `...`): make(String) over make(String&&, Text) given a String, which C++ calls
    // ambiguous for a `...` taking nothing.
    if (!someBetter && a.emptyText != b.emptyText)
      return b.emptyText;
    if (!someBetter && a.f->templateParams.empty() && !b.f->templateParams.empty())
      return true;
    if (!someBetter && !a.f->templateParams.empty() && !b.f->templateParams.empty() &&
        moreSpecialized(a.f, b.f, argTypes.size(), classSubst))
      return true; // `f(Vector<T>)` over `f(T&)`: C++'s partial ordering of function templates
    return someBetter;
  };
  // beats[i * n + j]: whether viable[i] is better than viable[j], each pair compared once (a
  // comparison costs: moreSpecialized deduces), kept for the ties below.
  size_t n = viable.size();
  char beatsSmall[256] = {};
  std::vector<char> beatsLarge(n * n > sizeof(beatsSmall) ? n * n : 0, 0);
  char* beats = beatsLarge.empty() ? beatsSmall : beatsLarge.data();
  for (size_t i = 0; i < n; ++i)
  {
    bool best = true;
    for (size_t j = 0; j < n; ++j)
      if (j != i && !(beats[i * n + j] = better(viable[i], viable[j])))
        best = false;
    if (best)
      return viable[i].f;
  }
  // No candidate better than all the others: those that no other beats, which may be one
  // function declared twice (sameFunction), then one candidate; otherwise the call is
  // ambiguous, and tiedOut gets one of each function.
  std::vector<const Candidate*> tied;
  for (size_t i = 0; i < n; ++i)
  {
    bool beaten = false;
    for (size_t j = 0; j < n; ++j)
      beaten = beaten || (j != i && beats[j * n + i]);
    if (!beaten)
      tied.push_back(&viable[i]);
  }
  if (tied.empty())
    for (const Candidate& c : viable)
      tied.push_back(&c); // each beaten by another, around a circle: all of them
  std::vector<const Candidate*> distinct;
  for (const Candidate* c : tied)
    if (std::none_of(distinct.begin(), distinct.end(), [&](const Candidate* d) { return sameFunction(c->f, d->f); }))
      distinct.push_back(c);
  ambiguous = distinct.size() > 1;
  if (ambiguous && tiedOut)
    for (const Candidate* c : distinct)
      tiedOut->push_back(c->f);
  // The one taken (the one function, or after an ambiguity that is no error, the choice that
  // was): the smallest total rank, then non-template, then declaration order.
  const Candidate* pick = tied[0];
  auto total = [](const Candidate& c) { int t = c.thisRank; for (int r : c.ranks) t += r; return t; };
  for (const Candidate* c : tied)
    if (total(*c) < total(*pick) || (total(*c) == total(*pick) && c->f->templateParams.empty() && !pick->f->templateParams.empty()))
      pick = c;
  // A C function a header defines (static inline: no symbol of its own) and kpp declares: the
  // definition, which the call compiles, not the declaration's external symbol.
  if (!ambiguous && !pick->f->body && isCFunction(pick->f))
    for (const Candidate* c : tied)
      if (c->f->body && sameFunction(c->f, pick->f))
      {
        pick = c;
        break;
      }
  return pick->f;
}

auto GlobalScope::Impl::takesEnumOperands(FunctionDecl* f, const Ty* lhs, const Ty* rhs) -> bool
{
  if (!f || !f->templateParams.empty())
    return false;
  const Ty* ft = declaredTypeOf(f, nullptr, 0);
  if (!ft || ft->kind != TyKind::Function || ft->args.size() != 2 || !lhs || !rhs)
    return false;
  const Ty* operands[2] = {types.unqualified(types.stripRef(lhs)), types.unqualified(types.stripRef(rhs))};
  for (int i = 0; i < 2; ++i)
    if (operands[i]->kind == TyKind::Enum && types.unqualified(types.stripRef(ft->args[i])) != operands[i])
      return false;
  return true;
}

auto GlobalScope::Impl::memberOperatorRanks(FunctionDecl* f, const Subst& subst, const Ty* rhs, size_t arity) -> std::vector<int>
{
  if (!f || !f->templateParams.empty())
    return {};
  const Ty* ft = declaredTypeOf(f, subst.empty() ? nullptr : &subst, 0);
  if (!ft || ft->kind != TyKind::Function || (arity == 2 && ft->args.empty()))
    return {};
  // The object binds the implicit object parameter as the free operator's first parameter binds
  // it (a reference to its class, or to the base declaring the member): taken as no conversion,
  // which the free one's first operand has to match to be better. Exact and qualification ranks
  // are one here: their order is C++'s tie-breakers', not ranked against a member.
  std::vector<int> ranks{0};
  if (arity == 2)
    ranks.push_back(std::max(1, conversionRank(rhs, ft->args[0])) - 1);
  return ranks;
}

auto GlobalScope::Impl::memberOperatorExact(FunctionDecl* f, const Subst& subst, const Ty* rhs, size_t arity) -> bool
{
  std::vector<int> ranks = memberOperatorRanks(f, subst, rhs, arity);
  return ranks.empty() || arity < 2 || ranks[1] == 0;
}

auto GlobalScope::Impl::freeOperatorBeatsMember(FunctionDecl* free, const Subst& freeSubst, FunctionDecl* member, const Subst& memberSubst,
                                                         const Ty* lhs, const Ty* rhs, size_t arity) -> bool
{
  if (arity != 2 || !free || !free->templateParams.empty())
    return false;
  std::vector<int> mine = memberOperatorRanks(member, memberSubst, rhs, arity);
  if (mine.size() != 2)
    return false;
  const Ty* ft = declaredTypeOf(free, freeSubst.empty() ? nullptr : &freeSubst, 0);
  if (!ft || ft->kind != TyKind::Function || ft->args.size() != 2)
    return false;
  int first = std::max(1, conversionRank(lhs, ft->args[0])) - 1;
  int second = std::max(1, conversionRank(rhs, ft->args[1])) - 1;
  return first <= mine[0] && second <= mine[1] && (first < mine[0] || second < mine[1]);
}
