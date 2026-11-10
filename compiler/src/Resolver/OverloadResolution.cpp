// GlobalScope::Impl, overload resolution (its declarations in OverloadResolution.hpp, part of GlobalScopeImpl.hpp): a translation unit
// of its own since 2026-11-05 (resolver.cpp split, stage 2), moved without a change.
#include "GlobalScopeImpl.hpp"

auto GlobalScope::Impl::overridesVirtual(const Ty* t, const std::string& key, int depth) -> std::optional<bool>
{
  if (!t || !t->isKnown() || types.isDependent(t) || t->kind != TyKind::Class || depth > maxNestedLevels)
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
    if (key == "~" && hasVirtualDestructor(b))
      return true; // (a base's destructor, virtual by kpp's rule too: design.md 23b)
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
  if (!cls || depth > maxNestedLevels || cls->baseTypes.empty())
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
  if (!base || !derived || depth > maxNestedLevels)
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
auto GlobalScope::Impl::chooseConversion(const Ty* from, const Ty* to, ConversionKind kind, bool explicitCast, const Ty** foundIn, Subst* opArgs,
                                         bool contextualBool, bool* ambiguousOut) -> FunctionDecl*
{
  if (ambiguousOut)
    *ambiguousOut = false;
  if (foundIn)
    *foundIn = nullptr;
  if (opArgs)
    opArgs->pairs.clear();
  const Ty* object = from ? types.stripRef(from) : nullptr; // (its const: a const object calls const operators only)
  const Ty* start = object ? types.unqualified(object) : nullptr;
  const Ty* target = to ? types.unqualified(types.stripRef(to)) : nullptr;
  bool toType = kind == ConversionKind::ToType || kind == ConversionKind::ToValue || kind == ConversionKind::ToLvalue;
  if (!start || start->kind != TyKind::Class || types.isDependent(start) || (toType && !target))
    return nullptr;
  bool constObject = object->isConst;
  bool volatileObject = object->isVolatile; // (a volatile object calls volatile operators only, as const)
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
        if (!bound || !constraintsHold(f, fargs))
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
  // The object's value category where the caller gives it (an lvalue as T&, an rvalue as T&&;
  // a plain T: not known, every candidate as for both): its implicit object parameter binds an
  // lvalue for `operator T() &`, an rvalue for `operator T() &&`, and a non-const `&` binds no
  // rvalue ([over.match.funcs] 5); `operator int() &` beside `operator int() &&` (until
  // 2026-11-08 ambiguous to every caller).
  bool objectLvalue = from->kind == TyKind::LRef, objectRvalue = from->kind == TyKind::RRef;
  // Each viable candidate with its rank, lower better: for a pointer or an integral type 0 (the
  // first of them taken); to a type, the standard conversion from its result to the target (an
  // exact match 0, a class derived from the target 1, then C++'s ranking of the scalar conversions).
  struct Viable
  {
    const Candidate* c;
    int rank;
    int addedQualifiers; // const and volatile the operator adds to the object's (fewer is better: a subset of the other's)
    bool refQualified;
    bool rvalueBinding; // its implicit object parameter an rvalue reference binding the object
    bool isTemplate;    // a conversion function template's specialization
  };
  std::vector<Viable> viable;
  for (const Candidate& c : candidates)
  {
    bool constOp = (c.f->flags & DF_Const) != 0;
    bool volatileOp = (c.f->flags & DF_Volatile) != 0;
    if ((constObject && !constOp) || (volatileObject && !volatileOp))
      continue; // a const object calls a const operator only, a volatile object a volatile one
    auto qualifier = FunctionDecl::RefQualifier(c.f->refQualifier);
    if (objectLvalue && qualifier == FunctionDecl::RefQualifier::RValue)
      continue;
    if (objectRvalue && qualifier == FunctionDecl::RefQualifier::LValue && !constOp)
      continue;
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
      case ConversionKind::ToValue:
      case ConversionKind::ToLvalue:
        if (kind == ConversionKind::ToValue ? c.result->kind == TyKind::LRef : (c.result->kind != TyKind::LRef || types.stripRef(c.result)->isConst))
          break;
        [[fallthrough]];
      case ConversionKind::ToType:
      {
        bool exact = ret == target;
        bool derived = !exact && ret->kind == TyKind::Class && target->kind == TyKind::Class && !types.isDependent(ret) && isBaseClass(target, ret);
        // An explicit one only in a direct-initialization (a cast; a condition, contextually to bool) and
        // only where it gives the type itself ([over.match.conv] 1.1): `explicit operator float()` is no
        // way to bool beside an implicit `operator short()` (until 2026-11-08 both were, ambiguous).
        if (isExplicitOne && !((explicitCast || (contextualBool && target->kind == TyKind::Bool)) && (exact || derived)))
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
    viable.push_back({&c, rank, int(constOp && !constObject) + int(volatileOp && !volatileObject), qualifier != FunctionDecl::RefQualifier::None,
                      objectRvalue && qualifier == FunctionDecl::RefQualifier::RValue, !c.f->templateParams.empty()});
  }
  // The implicit object parameter first, then the result's conversion ([over.match.best] 2.1 before
  // 2.2): of two reference bindings of the object, an rvalue one to an rvalue where both functions are
  // ref-qualified ([over.ics.rank] 3.2.3: `operator long() &&` over `operator long() const&` for a
  // temporary), then the one adding no const (3.2.6: of a non-const object a non-const operator beats a
  // const one whatever their results, `operator int&()` over `operator bool() const` in `if (x)`; until
  // 2026-11-08 the result first), then the result's standard conversion, then a function that is no
  // template's specialization beats one that is ([over.match.best] 2.4: `operator bool()` over
  // `template<class T> operator T()` with T = bool in `if (x)`). -1: a better, 1: b better.
  auto compare = [](const Viable& a, const Viable& b) -> int {
    if (a.refQualified && b.refQualified && a.rvalueBinding != b.rvalueBinding)
      return a.rvalueBinding ? -1 : 1;
    if (a.addedQualifiers != b.addedQualifiers)
      return a.addedQualifiers < b.addedQualifiers ? -1 : 1;
    if (a.rank != b.rank)
      return a.rank < b.rank ? -1 : 1;
    return a.isTemplate == b.isTemplate ? 0 : a.isTemplate ? 1 : -1;
  };
  // The best: better than every other (two indistinguishable ones C++'s ambiguity; for a pointer or an
  // integral type the first of the best taken).
  const Viable* chosen = nullptr;
  bool ambiguous = false;
  for (const Viable& v : viable)
  {
    bool beatsAll = true, tiesOnly = true;
    for (const Viable& w : viable)
      if (w.c->f != v.c->f) // (one operator reached through two bases is one candidate)
      {
        int c = compare(v, w);
        beatsAll = beatsAll && c < 0;
        tiesOnly = tiesOnly && c <= 0;
      }
    if (beatsAll)
    {
      chosen = &v;
      ambiguous = false;
      break;
    }
    if (tiesOnly && !chosen)
    {
      chosen = &v;
      ambiguous = toType;
    }
  }
  if (!chosen && !viable.empty())
    ambiguous = true; // (no candidate at least as good as every other)
  const Candidate* best = chosen ? chosen->c : nullptr;
  if (ambiguousOut)
    *ambiguousOut = ambiguous;
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
  if (p->kind == TyKind::Array && p->inner)
  {
    // `take({1, 2, 3})` for a `const int (&)[3]`: an array the list initializes, each element
    // converted to the array's element type, the worst of them ([over.ics.list] 6); more elements
    // than its length, none.
    if (p->count && elems.size() > p->count)
      return NotViable;
    int worst = 0;
    for (const Ty* e : elems)
      if (int r = conversionRank(e, p->inner, depth + 1); r >= NotViable)
        return NotViable;
      else
        worst = std::max(worst, r);
    return worst;
  }
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
    if (!deductionsAgree(f, ft, argTypes, d, s.pairs.size()))
      return false; // `template<U> Box(U, U)` given an int and a double: no deduction
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
    if (!constraintsHold(f, d))
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

auto GlobalScope::Impl::isConstObject(const Ty* t) -> bool
{
  while (t && t->kind == TyKind::Array)
    t = t->inner;
  return t && t->isConst;
}

auto GlobalScope::Impl::isVolatileObject(const Ty* t) -> bool
{
  while (t && t->kind == TyKind::Array)
    t = t->inner;
  return t && t->isVolatile;
}

auto GlobalScope::Impl::unqualifiedDeep(const Ty* t) -> const Ty*
{
  if (t && t->kind == TyKind::Array && t->inner && !t->decl) // (`T[N]` with N a parameter: left as it is)
    return types.array(unqualifiedDeep(t->inner), t->count);
  return types.unqualified(t);
}

auto GlobalScope::Impl::namedFunctionType(Decl* d, const Ty* t, const Subst* subst) -> const Ty*
{
  if (!t || t->kind != TyKind::Function || !d || d->kind != NodeKind::FunctionDecl)
    return t;
  auto* f = static_cast<FunctionDecl*>(d);
  uint8_t qualifiers = ((f->flags & DF_Const) ? TypeTable::MemberConst : 0) | ((f->flags & DF_Volatile) ? TypeTable::MemberVolatile : 0) |
                       (f->refQualifier == FunctionDecl::RefQualifier::LValue ? TypeTable::MemberLValue : 0) |
                       (f->refQualifier == FunctionDecl::RefQualifier::RValue ? TypeTable::MemberRValue : 0);
  t = types.withMemberQualifiers(t, qualifiers);
  return (f->flags & DF_Noexcept) && isNoexcept(f, subst) == std::optional<bool>(true) ? types.withNoexcept(t, true) : t;
}

auto GlobalScope::Impl::overriddenNoexcept(const Ty* t, FunctionDecl* f, const std::string& key, int depth) -> FunctionDecl*
{
  if (!t || !t->isKnown() || types.isDependent(t) || t->kind != TyKind::Class || depth > maxNestedLevels)
    return nullptr;
  ClassDecl* def = classDefinition(t);
  std::vector<const Ty*> bases;
  if (!def || !directBases(t, def, bases))
    return nullptr;
  const NamePart& name = f->name.last();
  for (const Ty* b : bases)
  {
    ClassDecl* bd = b && b->kind == TyKind::Class ? classDefinition(b) : nullptr;
    if (!bd)
      continue;
    for (Decl* m : membersRead(bd))
    {
      if (m->kind != NodeKind::FunctionDecl || !(m->flags & (DF_Virtual | DF_Override | DF_Pure)) || (m->flags & (DF_Static | DF_Constructor | DF_Destructor)) || !m->templateParams.empty())
        continue;
      auto* g = static_cast<FunctionDecl*>(m);
      if (g->name.empty() || g->name.last().symbol != name.symbol || g->name.last().special != name.special || overrideKey(g, b) != key)
        continue;
      Subst s = substFor(b);
      if (isNoexcept(g, s.pairs.empty() ? nullptr : &s) == std::optional<bool>(true))
        return g;
    }
    if (FunctionDecl* deeper = overriddenNoexcept(b, f, key, depth + 1))
      return deeper;
  }
  return nullptr;
}

auto GlobalScope::Impl::losesNoexcept(const Ty* from, const Ty* to) -> bool
{
  return from && to && from->isNoexcept() && !to->isNoexcept() && types.withNoexcept(from, false) == to;
}

auto GlobalScope::Impl::conversionRank(const Ty* arg, const Ty* param, int depth, bool userDefined) -> int
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
    if (param->kind == TyKind::LRef && !isConstObject(param->inner))
      return NotViable;
    int r = listInitRank(a, types.unqualified(types.stripRef(param)), depth);
    if (r >= NotViable)
      return NotViable;
    return param->kind == TyKind::LRef ? (r >= 4 ? 5 : std::max(r, 3)) : r;
  }
  if (a->isBraceList())
  {
    // `{"ab", text}` for an InitializerList<StringView>: as its elements convert, the worst of
    // them (the first one's type stands for them all when they are not known).
    const Ty* p = types.unqualified(types.stripRef(param));
    if (p->args.empty() || a->args.empty())
      return 2;
    if (!a->inner || a->inner->kind != TyKind::Pack)
    {
      int r = conversionRank(a->args[0], p->args[0], depth + 1, userDefined);
      return r >= NotViable ? NotViable : r;
    }
    int worst = 0;
    for (const Ty* e : a->inner->args)
      if (int r = conversionRank(e, p->args[0], depth + 1, userDefined); r >= NotViable)
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
      return types.unqualified(a) == types.unqualified(pt) ? 0 : losesNoexcept(types.unqualified(a), types.unqualified(pt)) ? 1 : NotViable;
    bool lvalueToRvalueRef = param->kind == TyKind::RRef && arg->kind == TyKind::LRef; // `T&&` takes no lvalue directly
    // `frame << agui::horizontalLine` with the helper's `operator HorizontalLine&()`: a class
    // whose conversion operator gives an lvalue of pt's type (of a class derived from pt's class)
    // binds a non-const `pt&` (a user-defined conversion), whatever the object's own value category;
    // a pointer's too (Agui's ValueComparePointer<Image> with `operator T*&()` for an `Image*&`).
    if (userDefined && param->kind == TyKind::LRef && !isConstObject(pt) && a->kind == TyKind::Class && types.unqualified(a) != types.unqualified(pt) &&
        !(pt->kind == TyKind::Class && isBaseClass(pt, a)) && convertsByOperator(conversionObject(arg), param, depth + 1))
      return 4;
    if (param->kind == TyKind::LRef && !isConstObject(pt) && (!arg->isReference() || arg->kind == TyKind::RRef))
      return NotViable; // `T&` binds no rvalue (a temporary, a lambda, `move(x)`): lvalues present themselves as `T&` (argumentType)
    if (types.unqualified(a) == types.unqualified(pt))
    {
      if (lvalueToRvalueRef || (!pt->isConst && a->isConst) || (!pt->isVolatile && a->isVolatile))
        return NotViable;
      return (pt->isConst && !a->isConst) || (pt->isVolatile && !a->isVolatile) ? 1 : 0;
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
    if (param->kind == TyKind::LRef && !isConstObject(pt))
      return NotViable; // A non-const lvalue reference binds only to the same type.
    // The argument as it is (an lvalue stays one): a converting constructor taking `T&`
    // binds it (`implicit EntityAndIndex(T& entity)` given a Tower lvalue).
    int r = conversionRank(arg, pt, depth + 1, userDefined);
    if (r >= NotViable)
      return NotViable;
    if (r >= 4 && param->kind == TyKind::LRef)
      return 5; // the converted temporary binds better to `T&&` than to `const T&`
    // An rvalue reference binds no lvalue that a conversion function gives, neither the lvalue
    // itself nor a copy of it (C++ [over.ics.ref] 3, as clang applies it): take(String&&) given
    // a class whose conversion is `operator const String&()` is no candidate, take(const
    // String&) is called (UnitTest's names, moved out of their objects through try_emplace).
    if (r >= 4 && param->kind == TyKind::RRef && a->kind == TyKind::Class && convertsByOperator(conversionObject(arg), pt, depth + 1) &&
        !convertsByOperator(conversionObject(arg), pt, depth + 1, 0, false, true))
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
        // to int only, never long; char32_t to unsigned int, not int ([conv.prom] 2)
        bool promotion = p != a && p == types.promoted(a);
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
          return p == types.promoted(types.unqualified(u)) ? 2 : 3;
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
      if (a->kind == TyKind::MemberPointer && a->decl == p->decl && losesNoexcept(types.unqualified(a->inner), types.unqualified(p->inner)))
        return 1;
      // `&LuaControlBehavior::luaReadEntity` for an `int32 (T::*)(lua_State*)` with T derived from
      // it: a pointer to a member of a base converts to one of the derived class.
      if (a->kind == TyKind::MemberPointer && (types.unqualified(a->inner) == types.unqualified(p->inner) || losesNoexcept(types.unqualified(a->inner), types.unqualified(p->inner))) && !a->args.empty() &&
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
        {
          if ((ai->isConst && !pi->isConst) || (ai->isVolatile && !pi->isVolatile))
            return NotViable;
          return pi->isConst == ai->isConst && pi->isVolatile == ai->isVolatile ? 0 : 1;
        }
        if (losesNoexcept(types.unqualified(ai), types.unqualified(pi)))
          return 1; // the function pointer conversion, an exact match worse than the identity ([conv.fctptr])
        // `int (*)[2]` to `const int (*)[2]`, `int**` to `const int* const*`: the qualification
        // conversion at a deeper level or through an array's elements, an exact match ([conv.qual];
        // it was no candidate, so f(const void*) or f(bool) was called instead, a silent wrong answer)
        if (types.qualificationConverts(types.unqualified(a), p))
          return 1;
        if (pi->kind == TyKind::Void)
          return (ai->isConst && !pi->isConst) || (ai->isVolatile && !pi->isVolatile) ? NotViable : 3; // (`const char*` is no `void*`)
        if (ai->kind == TyKind::Class && pi->kind == TyKind::Class && isBaseClass(pi, ai))
          return !pi->isConst && ai->isConst ? NotViable : 3;
        return NotViable;
      }
      if (a->kind == TyKind::Lambda && p->inner->kind == TyKind::Function)
      {
        // (a closure's call operator noexcept or not: its pointer's type, which converts as one does)
        if (ClosureCall* c = p->inner->isNoexcept() ? closureCall(a) : nullptr; c && isNoexcept(&c->decl, nullptr) != std::optional<bool>(true))
          return NotViable;
        return 3;
      }
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
      if (!userDefined)
        return NotViable;
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
          if (!constraintsHold(f, d))
            continue; // `template<typename F> Wrapper(F f) requires(!isPointer<F>)` given a pointer: no conversion
          const Ty* pt = declaredTypeOf(f->params[0], &d, 0);
          if (pt && types.stripRef(pt)->kind == TyKind::Class && types.unqualified(types.stripRef(pt)) == types.unqualified(ownerTy))
            continue; // The copy constructor is not a conversion (ReverseIterator<const T> from ReverseIterator<T> is).
          // (its argument by a standard conversion only: no second user-defined one, [over.best.ics] 4)
          if (conversionRank(arg, pt, depth + 1, false) < 4)
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
  // Conversion operator of the argument's class, or one it inherits (the object as it is, an lvalue
  // or an rvalue: `operator int() &` converts no temporary).
  if (userDefined && a->kind == TyKind::Class && convertsByOperator(conversionObject(arg), param, depth))
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

auto GlobalScope::Impl::builtinOperatorTakes(P op, const Ty* l, const Ty* r) -> bool
{
  auto unscopedEnum = [](const Ty* t) { return t->kind == TyKind::Enum && !(t->decl && static_cast<EnumDecl*>(t->decl)->isClass); };
  auto arithmetic = [&](const Ty* t) { return t->isArithmetic() || unscopedEnum(t) || t->kind == TyKind::Vector; };
  auto integral = [&](const Ty* t) {
    return t->kind == TyKind::Bool || t->kind == TyKind::Char || t->kind == TyKind::Int || unscopedEnum(t) ||
           (t->kind == TyKind::Vector && t->inner && t->inner->kind == TyKind::Int); // (no `&` of two float vectors)
  };
  auto pointer = [](const Ty* t) { return t->kind == TyKind::Pointer || t->kind == TyKind::Array; };
  auto truth = [&](const Ty* t) {
    return (arithmetic(t) && t->kind != TyKind::Vector) || pointer(t) || t->kind == TyKind::MemberPointer || t->kind == TyKind::Nullptr;
  };
  l = l ? types.unqualified(types.stripRef(l)) : nullptr;
  r = r ? types.unqualified(types.stripRef(r)) : nullptr;
  if (!l)
    return false;
  if (!r)
    switch (op)
    {
      case P::Plus: return arithmetic(l) || pointer(l);
      case P::Minus: return arithmetic(l);
      case P::Tilde: return integral(l);
      case P::Bang: return truth(l);
      case P::Star: return pointer(l);
      default: return true; // (++, --: not told apart here)
    }
  switch (op)
  {
    case P::Plus: return (arithmetic(l) && arithmetic(r)) || (pointer(l) && integral(r)) || (integral(l) && pointer(r));
    case P::Minus: return (arithmetic(l) && arithmetic(r)) || (pointer(l) && (integral(r) || pointer(r)));
    case P::Star: case P::Slash: return arithmetic(l) && arithmetic(r);
    case P::Percent: case P::Amp: case P::Pipe: case P::Caret: case P::Shl: case P::Shr: return integral(l) && integral(r);
    case P::Less: case P::Greater: case P::LessEq: case P::GreaterEq: case P::Spaceship:
      return (arithmetic(l) && arithmetic(r)) || (l->kind == TyKind::Enum && l == r) || (pointer(l) && pointer(r));
    case P::EqEq: case P::NotEq:
    {
      auto address = [&](const Ty* t) { return pointer(t) || t->kind == TyKind::Nullptr || t->kind == TyKind::MemberPointer; };
      return (arithmetic(l) && arithmetic(r)) || (l->kind == TyKind::Enum && l == r) || (address(l) && address(r));
    }
    case P::AndAnd: case P::OrOr: return truth(l) && truth(r);
    default: return true;
  }
}

auto GlobalScope::Impl::builtinOperands(P op, const Ty* l, const Ty* r, const Ty** lc, const Ty** rc, bool* ambiguousOut) -> bool
{
  if (ambiguousOut)
    *ambiguousOut = false;
  bool integralOnly = false;
  switch (op)
  {
    case P::Plus: case P::Minus:
      break;
    case P::Star: case P::Slash: case P::Less: case P::Greater: case P::LessEq: case P::GreaterEq: case P::EqEq: case P::NotEq: case P::Spaceship:
      if (!r)
        return false; // (unary *: a pointer's)
      break;
    case P::Percent: case P::Amp: case P::Pipe: case P::Caret: case P::Shl: case P::Shr:
      if (!r)
        return false;
      integralOnly = true;
      break;
    case P::Tilde:
      if (r)
        return false;
      integralOnly = true;
      break;
    default:
      return false;
  }
  const Ty* operands[2] = {l, r};
  size_t count = r ? 2 : 1;
  bool anyClass = false;
  for (size_t i = 0; i < count; ++i)
  {
    const Ty* t = operands[i] ? types.unqualified(types.stripRef(operands[i])) : nullptr;
    if (!t || !t->isKnown() || types.isDependent(t))
      return false;
    if (t->kind == TyKind::Class)
    {
      // (A class converting to a pointer or an enumeration, or by an operator template: candidates
      // not taken here.)
      bool other = false;
      std::function<void(const Ty*, int)> scan = [&](const Ty* k, int level) {
        ClassDecl* kd = k && k->kind == TyKind::Class ? classDefinition(k) : nullptr;
        if (!kd || level > 8)
          return;
        Subst ks = substFor(k);
        for (Decl* m : membersRead(kd))
        {
          if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Conversion))
            continue;
          if (!static_cast<FunctionDecl*>(m)->templateParams.empty())
          {
            other = true;
            continue;
          }
          const Ty* ft = declaredTypeOf(m, ks.empty() ? nullptr : &ks, 0);
          const Ty* res = ft && ft->kind == TyKind::Function && ft->inner ? types.unqualified(types.stripRef(ft->inner)) : nullptr;
          if (!res || !res->isKnown() || types.isDependent(res))
            other = true;
          else if (!(res->isArithmetic() && res->kind != TyKind::Vector) && res->kind != TyKind::Class)
            other = true; // (a class result converts the operand to no scalar)
        }
        std::vector<const Ty*> bases;
        if (directBases(k, kd, bases))
          for (const Ty* b : bases)
            scan(b, level + 1);
      };
      scan(t, 0);
      if (other)
        return false;
      anyClass = true;
    }
    else if (!(t->isArithmetic() && t->kind != TyKind::Vector) && !(t->kind == TyKind::Enum && !(t->decl && static_cast<EnumDecl*>(t->decl)->isClass)))
      return false;
  }
  if (!anyClass)
    return false;
  // The promoted arithmetic types ([over.built] 2), the integral ones first.
  std::vector<const Ty*> promoted{types.intType(32), types.intType(32, true), types.longType(), types.longType(true), types.intType(64), types.intType(64, true)};
  if (!integralOnly)
  {
    promoted.push_back(types.floatType(32));
    promoted.push_back(types.floatType(64));
  }
  // An operand's implicit conversion sequence to a parameter: a standard one (rank: exact 0 ...
  // conversion 3) or a user-defined one (by f, then its second standard conversion's rank; an
  // ambiguous one, f null, indistinguishable from any other user-defined one, [over.best.ics] 10).
  struct Sequence
  {
    bool userDefined = false;
    FunctionDecl* f = nullptr;
    int rank = 0;
  };
  auto sequence = [&](const Ty* operand, const Ty* param, Sequence& out) -> bool
  {
    const Ty* t = types.unqualified(types.stripRef(operand));
    if (t->kind != TyKind::Class)
    {
      int rank = conversionRank(t, param, 0, false);
      if (rank >= 4)
        return false;
      out = {false, nullptr, rank};
      return true;
    }
    Subst opArgs;
    const Ty* foundIn = nullptr;
    bool ambiguous = false;
    FunctionDecl* f = chooseConversion(operand, param, GlobalScope::ConversionKind::ToType, false, &foundIn, &opArgs, false, &ambiguous);
    if (ambiguous)
    {
      out = {true, nullptr, 0};
      return true;
    }
    if (!f)
      return false;
    const Ty* ft = declaredTypeOf(f, opArgs.empty() ? nullptr : &opArgs, 0);
    const Ty* res = ft && ft->kind == TyKind::Function && ft->inner ? types.unqualified(types.stripRef(ft->inner)) : nullptr;
    int rank = res ? conversionRank(res, param, 0, false) : NotViable;
    if (rank >= 4)
      return false;
    out = {true, f, rank};
    return true;
  };
  // -1: a better, 1: b better, 0: neither ([over.ics.rank] 2, 3.2, 3.3).
  auto compare = [](const Sequence& a, const Sequence& b) -> int {
    if (a.userDefined != b.userDefined)
      return a.userDefined ? 1 : -1;
    if (a.userDefined && (!a.f || a.f != b.f))
      return 0;
    return a.rank < b.rank ? -1 : a.rank > b.rank ? 1 : 0;
  };
  struct Candidate
  {
    const Ty* params[2];
    Sequence sequences[2];
  };
  std::vector<Candidate> viable;
  for (const Ty* p0 : promoted)
    for (size_t j = 0; j < (r ? promoted.size() : 1); ++j)
    {
      Candidate c{{p0, r ? promoted[j] : nullptr}, {}};
      bool ok = true;
      for (size_t i = 0; i < count && ok; ++i)
        ok = sequence(operands[i], c.params[i], c.sequences[i]);
      if (ok)
        viable.push_back(c);
    }
  const Candidate* best = nullptr;
  for (const Candidate& c : viable)
  {
    bool beatsAll = true;
    for (const Candidate& d : viable)
    {
      if (&d == &c)
        continue;
      bool better = false, worse = false;
      for (size_t i = 0; i < count; ++i)
      {
        int k = compare(c.sequences[i], d.sequences[i]);
        better = better || k < 0;
        worse = worse || k > 0;
      }
      if (!better || worse)
      {
        beatsAll = false;
        break;
      }
    }
    if (beatsAll)
    {
      best = &c;
      break;
    }
  }
  // (none better than all: ambiguous; an ambiguous conversion in the best candidate too, [over.best.ics] 10)
  bool ambiguousConversion = false;
  for (size_t i = 0; best && i < count; ++i)
    ambiguousConversion = ambiguousConversion || (best->sequences[i].userDefined && !best->sequences[i].f);
  if (!best || ambiguousConversion)
  {
    if (ambiguousOut)
      *ambiguousOut = !viable.empty();
    return false;
  }
  if (lc)
    *lc = best->params[0];
  if (rc)
    *rc = best->params[1];
  return true;
}

auto GlobalScope::Impl::freeOperatorAgainstBuiltin(FunctionDecl* f, const Subst* subst, P op, const std::vector<const Ty*>& operands) -> int
{
  const Ty* ft = declaredTypeOf(f, subst, 0);
  if (!ft || ft->kind != TyKind::Function || ft->args.size() < operands.size() || operands.empty() || operands.size() > 2)
    return 1;
  bool contextualBool = op == P::Bang || op == P::AndAnd || op == P::OrOr; // (an explicit operator bool converts there)
  bool fBetter = false, builtinBetter = false, anyClass = false;
  const Ty* reached[2] = {nullptr, nullptr}; // what the built-in operator takes each operand as
  for (size_t i = 0; i < operands.size(); ++i)
  {
    const Ty* t = types.unqualified(types.stripRef(operands[i]));
    bool isClass = t && t->kind == TyKind::Class;
    reached[i] = t;
    if (isClass)
    {
      anyClass = true;
      reached[i] = contextualBool ? (convertsByOperator(t, types.boolType(), 0, 0, true) ? types.boolType() : nullptr) : scalarConversionOf(t);
      if (!reached[i])
        return 1; // no built-in operator takes it
    }
    bool fUserDefined = conversionRank(operands[i], ft->args[i]) >= 4;
    fBetter = fBetter || (!fUserDefined && isClass);
    builtinBetter = builtinBetter || (fUserDefined && !isClass);
  }
  if (!anyClass || (fBetter && !builtinBetter) || !reached[0] || (operands.size() == 2 && !reached[1]) || !builtinOperatorTakes(op, reached[0], reached[1]))
    return 1;
  return builtinBetter && !fBetter ? 0 : -1;
}

auto GlobalScope::Impl::memberOperatorAgainstBuiltin(FunctionDecl* f, const Subst* subst, P op, const Ty* lhs, const Ty* rhs) -> int
{
  switch (op)
  {
    case P::Plus: case P::Minus: case P::Star: case P::Slash: case P::Percent: case P::Less: case P::Greater: case P::LessEq:
    case P::GreaterEq: case P::EqEq: case P::NotEq: case P::Amp: case P::Pipe: case P::Caret: case P::Shl: case P::Shr:
    case P::AndAnd: case P::OrOr:
      break;
    default:
      return 1;
  }
  const Ty* ft = f ? declaredTypeOf(f, subst, 0) : nullptr;
  const Ty* l = lhs ? types.unqualified(types.stripRef(lhs)) : nullptr;
  const Ty* r = rhs ? types.unqualified(types.stripRef(rhs)) : nullptr;
  if (!ft || ft->kind != TyKind::Function || ft->args.empty() || !l || !r || l->kind != TyKind::Class || !l->isKnown() || !r->isKnown() ||
      types.isDependent(l) || types.isDependent(r))
    return 1;
  bool contextualBool = op == P::AndAnd || op == P::OrOr; // (an explicit operator bool converts there)
  auto reached = [&](const Ty* t) -> const Ty* {
    if (t->kind != TyKind::Class)
      return t;
    if (contextualBool)
      return convertsByOperator(t, types.boolType(), 0, 0, true) ? types.boolType() : nullptr;
    return scalarConversionOf(t);
  };
  const Ty* lb = reached(l);
  const Ty* rb = reached(r);
  if (!lb || !rb || !builtinOperatorTakes(op, lb, rb))
    return 1; // no built-in operator takes the operands
  bool userDefined = conversionRank(rhs, ft->args[0]) >= 4;
  return userDefined && r->kind != TyKind::Class ? -1 : 1;
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
  (void)depth;
  (void)baseDepth;
  const Ty* object = a; // (an lvalue as T&, an rvalue as T&&, for a ref-qualified operator; chooseConversion)
  a = a ? types.stripRef(a) : nullptr;
  if (!a || a->kind != TyKind::Class)
    return false;
  if (!param || !param->isKnown() || types.isDependent(param))
  {
    // A parameter not known decides no overload (conversionRank): any conversion operator takes it.
    std::function<bool(const Ty*, int)> any = [&](const Ty* k, int level) -> bool
    {
      ClassDecl* kd = k && k->kind == TyKind::Class ? classDefinition(k) : nullptr;
      if (!kd || level > 8)
        return false;
      for (Decl* m : membersRead(kd))
        if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Conversion))
          return true;
      std::vector<const Ty*> bases;
      if (directBases(k, kd, bases))
        for (const Ty* b : bases)
          if (any(b, level + 1))
            return true;
      return false;
    };
    return any(a, 0);
  }
  // The one rule codegen converts by (design.md 24, chooseConversion), so that an overload is viable
  // through an operator exactly when the call can run it. rvalues: a parameter binding an rvalue,
  // which an operator returning an lvalue reference does not give.
  Subst args;
  bool ambiguous = false;
  bool lvalue = param->kind == TyKind::LRef && param->inner && !param->inner->isConst; // a non-const `T&`
  // (which binds only what an operator giving a non-const lvalue gives: `int f(int&)` given a class with `operator float() &`
  // and `operator unsigned() &` is no candidate, never an ambiguous one)
  FunctionDecl* f = chooseConversion(object, param, lvalue ? GlobalScope::ConversionKind::ToLvalue : GlobalScope::ConversionKind::ToType, allowExplicit, nullptr,
                                     &args, allowExplicit, &ambiguous);
  if (ambiguous)
    return true; // (C++: an ambiguous conversion is still a user-defined one, the overload viable; a call that chooses it is an error)
  if (!f)
    return false;
  if (rvalues || lvalue)
  {
    const Ty* ft = declaredTypeOf(f, args.empty() ? nullptr : &args, 0);
    const Ty* result = ft && ft->kind == TyKind::Function ? ft->inner : nullptr;
    if (result && !f->templateParams.empty())
      result = substitute(result, args);
    if (rvalues && result && result->kind == TyKind::LRef && result->inner && result->inner->kind != TyKind::Function)
      return false;
    // A non-const `T&` binds the lvalue an operator returning `T&` gives, never the value one
    // returning T gives (`operator T*()` for an `int*&`: a temporary, [dcl.init.ref] 5).
    if (lvalue && result && !(result->kind == TyKind::LRef && result->inner && !result->inner->isConst))
      return false;
  }
  return true;
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

// [over.call.object] 2: an object no call operator of which takes the arguments is called through
// what a conversion function of it gives, a pointer or a reference to a function: the type (as the
// conversion gives it) of the one whose parameters take them best; null for none. A surrogate never
// beats a call operator that takes them (the object's conversion is the worse), so these are asked after.
auto GlobalScope::Impl::surrogateFunction(const Ty* object, const std::vector<const Ty*>& args, bool& ambiguous) -> const Ty*
{
  ambiguous = false;
  bool constObject = isConstObject(types.stripRef(object));
  bool volatileObject = object && types.stripRef(object)->isVolatile;
  std::vector<const Ty*> given;
  std::function<void(const Ty*, int)> collect = [&](const Ty* cls, int depth)
  {
    Scope* s = cls && cls->kind == TyKind::Class && depth <= 8 ? classScopeOf(cls) : nullptr;
    if (!s || !s->owner || s->owner->kind != NodeKind::ClassDecl)
      return;
    Subst subst = substFor(cls);
    for (Decl* m : membersRead(static_cast<ClassDecl*>(s->owner)))
    {
      if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Conversion) || !m->templateParams.empty() ||
          isExplicit(static_cast<FunctionDecl*>(m), &subst) || (constObject && !(m->flags & DF_Const)) ||
          (volatileObject && !(m->flags & DF_Volatile)))
        continue;
      const Ty* ft = declaredTypeOf(m, &subst, 0);
      const Ty* t = ft && ft->kind == TyKind::Function ? ft->inner : nullptr;
      if (!t || !t->isKnown() || types.isDependent(t))
        continue;
      const Ty* callee = types.unqualified(types.stripRef(t));
      if (callee->kind == TyKind::Pointer)
        callee = types.unqualified(callee->inner);
      if (callee->kind == TyKind::Function && std::find(given.begin(), given.end(), t) == given.end())
        given.push_back(t);
    }
    ClassDecl* def = classDefinition(cls);
    std::vector<const Ty*> bases;
    if (def && directBases(cls, def, bases))
      for (const Ty* b : bases)
        collect(b, depth + 1);
  };
  collect(types.unqualified(types.stripRef(object)), 0);
  const Ty* best = nullptr;
  std::vector<int> bestRanks;
  for (const Ty* t : given)
  {
    const Ty* callee = types.unqualified(types.stripRef(t));
    callee = callee->kind == TyKind::Pointer ? types.unqualified(callee->inner) : callee;
    if (args.size() < callee->args.size() || (args.size() > callee->args.size() && !callee->variadic))
      continue;
    std::vector<int> ranks;
    for (size_t i = 0; i < callee->args.size(); ++i)
      ranks.push_back(args[i] ? conversionRank(args[i], callee->args[i]) : 0);
    if (std::any_of(ranks.begin(), ranks.end(), [](int r) { return r >= NotViable; }))
      continue;
    bool better = !best, worse = false;
    for (size_t i = 0; best && i < ranks.size() && i < bestRanks.size(); ++i)
    {
      better = better || ranks[i] < bestRanks[i];
      worse = worse || ranks[i] > bestRanks[i];
    }
    if (better && !worse)
      best = t, bestRanks = ranks, ambiguous = false;
    else if (better == worse)
      ambiguous = true;
  }
  return best;
}

auto GlobalScope::Impl::pushEnclosingTemplateParams(Decl* d, LocalStack& own) -> void
{
  for (Decl* p = d; p; p = p->parent)
    for (Decl* tp : p->templateParams)
      own.push(static_cast<TemplateParamDecl*>(tp)->name, tp);
}

auto GlobalScope::Impl::pushFunctionParams(Decl* d, LocalStack& own) -> void
{
  if (d && d->kind == NodeKind::FunctionDecl)
    for (Decl* p : static_cast<FunctionDecl*>(d)->params)
      if (p && p->kind == NodeKind::ParamDecl)
        own.push(static_cast<ParamDecl*>(p)->name, p);
}

auto GlobalScope::Impl::constraintValue(Expr* e, Decl* f, const Subst& s, int depth, Span<Decl*> local) -> std::optional<bool>
{
  if (!e || depth > 16)
    return std::nullopt;
  switch (e->kind)
  {
    case NodeKind::ParenExpr: return constraintValue(static_cast<ParenExpr*>(e)->inner, f, s, depth + 1, local);
    case NodeKind::UnaryExpr:
    {
      auto* u = static_cast<UnaryExpr*>(e);
      if (u->isKeyword ? Kw(u->op) == Kw::not_ : P(u->op) == P::Bang)
      {
        std::optional<bool> v = constraintValue(u->operand, f, s, depth + 1, local);
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
      std::optional<bool> l = constraintValue(b->lhs, f, s, depth + 1, local), r = constraintValue(b->rhs, f, s, depth + 1, local);
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
  pushFunctionParams(f, own);
  for (Decl* p : local) // (the requires-expression's own parameters: `sizeof(t)` of `requires(T t)`)
    if (p && p->kind == NodeKind::ParamDecl)
      own.push(static_cast<ParamDecl*>(p)->name, p);
  TypeContext ctx{lexicalScopeOf(f), &own, &s, f->file ? f->file->tokens : nullptr, f->file ? f->file->text : std::string_view()};
  if (e->kind == NodeKind::NameExpr)
  {
    auto* n = static_cast<NameExpr*>(e);
    if (const NamePart* trait = boolTraitPart(n->name, ctx.scope, ctx.locals, depth + 1))
    {
      std::vector<const Ty*> args = traitArgs(*trait, ctx, depth + 1);
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
      return traitValue(symbols.name(trait->symbol), args);
    }
    // `has_str<T>` with `template<class T> concept has_str = requires(const T& value) { value.str(); };`.
    if (n->name.last().hasTemplateArgs)
      if (VariableDecl* concept_ = conceptOf(lookupQualified(n->name, ctx.scope, &own, depth + 1).decl))
        return conceptValue(concept_, n->name.last(), ctx, depth + 1);
  }
  if (std::optional<bool> v = constCondition(e, ctx, depth + 1))
    return v;
  // An atomic constraint whose expression does not form for known arguments (`T::value` of a T that has none): a
  // substitution failure, so not satisfied ([temp.constr.atomic] 3), as a trait's arguments above. One that forms and
  // is no constant the compiler evaluates stays unknown, its callers' error, never a guess; so does one naming what
  // nothing declares (`requires (noSuchTrait<T>)`: no substitution fails there, the callers name it).
  for (Decl* p = f; p; p = p->parent)
    for (Decl* tp : p->templateParams)
      if (const Ty* t = s.find(tp); !t || !t->isKnown() || types.isDependent(t) || mentionsUnknown(t))
        return std::nullopt;
  const Ty* t = typeOfSignatureExpression(e, ctx, nullptr, f->file);
  if (t && t->isKnown())
    return std::nullopt;
  return undeclaredNameIn(e, f, own, depth + 1).empty() ? std::optional<bool>(false) : std::nullopt;
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
  return conceptHolds(concept_, given, depth);
}

auto GlobalScope::Impl::conceptHolds(VariableDecl* concept_, const std::vector<const Ty*>& given, int depth) -> std::optional<bool>
{
  if (!concept_ || !concept_->init || depth > 16 || concept_->templateParams.empty())
    return std::nullopt;
  size_t n = concept_->templateParams.size();
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
  std::vector<const Ty*> resultTypes;
  bool undecided = false; // (a `{ e } noexcept` whose e canThrow does not decide yet)
  for (size_t i = 0; i < r->requirements.size(); ++i)
  {
    Expr* copy = copier.expression(r->requirements[i]);
    const Ty* ty = probe.walkExpr(copy);
    if (!ty || !ty->isKnown() || probe.namesFunctionsWithoutTarget(copy))
      return false;
    if (types.isDependent(ty))
      dependent = true;
    else if (!namesOnlyAccessible(copy, fn, context, nullptr))
      return false;
    else if (i < r->noexceptRequired.size() && r->noexceptRequired[i] && !probe.inGenericTemplate())
    {
      // `{ t.size() } noexcept`: not satisfied when the expression is potentially-throwing ([expr.prim.req.compound] 1.2).
      std::string unsupported;
      bool throws = probe.canThrow(copy, unsupported);
      if (!unsupported.empty())
        undecided = true;
      else if (throws)
        return false;
    }
    resultTypes.push_back(argumentType(copy, ty, types));
  }
  if (dependent)
    return std::nullopt;
  std::optional<bool> extra = requirementsExtraHold(r, resultTypes, owner, s);
  return undecided && (!extra || *extra) ? std::nullopt : extra;
}

auto GlobalScope::Impl::resultConstraintHolds(Type* constraint, const Ty* type, Decl* owner, const Subst& s) -> std::optional<bool>
{
  if (!constraint || constraint->kind != NodeKind::NamedType || !type || !type->isKnown() || types.isDependent(type))
    return std::nullopt;
  LocalStack own;
  pushEnclosingTemplateParams(owner, own);
  TypeContext ctx{lexicalScopeOf(owner), &own, &s, owner->file ? owner->file->tokens : nullptr, owner->file ? owner->file->text : std::string_view()};
  const QualifiedName& name = static_cast<NamedType*>(constraint)->name;
  VariableDecl* concept_ = conceptOf(lookupQualified(name, ctx.scope, &own, 1, &s).decl);
  if (!concept_ || concept_->templateParams.empty())
    return false;
  std::vector<const Ty*> given{type};
  size_t n = concept_->templateParams.size();
  for (Node* written : name.last().templateArgs)
  {
    const Ty* a = templateArgument(written, concept_->templateParams[std::min(given.size(), n - 1)], ctx, 1);
    if (!a || !a->isKnown() || types.isDependent(a) || mentionsUnknown(a))
      return std::nullopt;
    given.push_back(a);
  }
  return conceptHolds(concept_, given, 1);
}

auto GlobalScope::Impl::requirementsExtraHold(RequiresExpr* r, const std::vector<const Ty*>& resultTypes, Decl* owner, const Subst& s) -> std::optional<bool>
{
  bool unknownYet = false;
  for (size_t i = 0; i < r->resultConstraints.size(); ++i)
  {
    if (!r->resultConstraints[i])
      continue;
    std::optional<bool> v = resultConstraintHolds(r->resultConstraints[i], i < resultTypes.size() ? resultTypes[i] : nullptr, owner, s);
    if (v && !*v)
      return false;
    unknownYet = unknownYet || !v;
  }
  for (Expr* e : r->nested)
  {
    std::optional<bool> v = constraintValue(e, owner, s, 1, r->params);
    if (v && !*v)
      return false;
    unknownYet = unknownYet || !v;
  }
  return unknownYet ? std::nullopt : std::optional<bool>(true);
}

auto GlobalScope::Impl::typeOfSignatureExpression(Expr* e, const TypeContext& ctx, Expr** typed, const ParsedFile* lookupFile, const Ty* objectType, bool substituting) -> const Ty*
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
  FileOfLookups walkedFile(f ? f->file : lookupFile);
  probe.scope = ctx.scope;
  probe.activeSubst = ctx.subst;
  probe.substituting = substituting;
  if (objectType)
    probe.thisType = types.pointer(objectType);
  else if (Decl* cls = ctx.subst && ctx.scope ? enclosingClassDecl(ctx.scope) : nullptr; cls && cls->kind == NodeKind::ClassDecl)
    probe.thisType = types.pointer(substitute(selfType(cls), *ctx.subst)); // `noexcept(noexcept(this.v = x))` in a member: the instance's object
  if (ctx.elementPacks && !ctx.elementPacks->empty())
  {
    // One element of a pack expansion's pattern: the packs' mentions name it, ctx.subst binding them whole.
    probe.elementPacks = ctx.elementPacks;
    probe.elementIndex = ctx.elementIndex;
    probe.retypingElements = true;
  }
  if (ctx.locals)
    probe.locals = *ctx.locals;
  if (f)
    for (Decl* p : f->params)
      probe.locals.push(static_cast<ParamDecl*>(p)->name, p);
  const Ty* t = probe.walkExpr(copy);
  --nesting;
  if (typed)
    *typed = copy;
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
      if (n->name.empty() || (n->name.parts.size() == 1 && (isBoolTraitName(n->name.parts[0].symbol) || builtinByName(n->name.parts[0].symbol))) ||
          boolTraitPart(n->name, lexicalScopeOf(f), &own, depth + 1))
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

auto GlobalScope::Impl::typeConstraintOf(const TemplateParamDecl* p, Decl* owner) -> VariableDecl*
{
  if (!p || !p->isType || !p->valueType || p->valueType->kind != NodeKind::NamedType || !owner)
    return nullptr;
  auto* named = static_cast<NamedType*>(p->valueType);
  Decl* d = named->resolved;
  if (!d)
  {
    LocalStack own;
    pushEnclosingTemplateParams(owner, own);
    d = lookupQualified(named->name, lexicalScopeOf(owner), &own, 0).decl;
  }
  return conceptOf(d);
}

auto GlobalScope::Impl::typeConstraintHolds(TemplateParamDecl* p, Decl* owner, const Subst& s, int depth) -> std::optional<bool>
{
  VariableDecl* concept_ = typeConstraintOf(p, owner);
  if (!concept_)
    return true;
  const Ty* t = s.find(p);
  if (!t || !t->isKnown() || types.isDependent(t) || mentionsUnknown(t))
    return std::nullopt;
  // The arguments written after the concept's name (`SameAs<U> T`: U), under s; the parameter's
  // own argument comes first.
  LocalStack own;
  pushEnclosingTemplateParams(owner, own);
  TypeContext ctx{lexicalScopeOf(owner), &own, &s, owner->file ? owner->file->tokens : nullptr, owner->file ? owner->file->text : std::string_view()};
  std::vector<const Ty*> rest;
  const NamePart& part = static_cast<NamedType*>(p->valueType)->name.last();
  size_t n = concept_->templateParams.size();
  for (Node* written : part.templateArgs)
  {
    Decl* param = concept_->templateParams[std::min(rest.size() + 1, n - 1)];
    const Ty* a = templateArgument(written, param, ctx, depth + 1);
    if (!a || !a->isKnown() || types.isDependent(a) || mentionsUnknown(a))
      return std::nullopt;
    if (written && written->kind == NodeKind::PackType && a->kind == TyKind::Pack)
      rest.insert(rest.end(), a->args.begin(), a->args.end());
    else
      rest.push_back(a);
  }
  // A constrained pack (`template<Small... Ts>`): each element.
  std::vector<const Ty*> elements;
  if (p->isPack && t->kind == TyKind::Pack)
    elements.assign(t->args.begin(), t->args.end());
  else
    elements.push_back(t);
  for (const Ty* e : elements)
  {
    std::vector<const Ty*> given{e};
    given.insert(given.end(), rest.begin(), rest.end());
    std::optional<bool> v = conceptHolds(concept_, given, depth + 1);
    if (!v || !*v)
      return v;
  }
  return true;
}

auto GlobalScope::Impl::constrained(const FunctionDecl* f) -> bool
{
  if (f->requiresClause)
    return true;
  for (Decl* tp : f->templateParams)
    if (typeConstraintOf(static_cast<TemplateParamDecl*>(tp), const_cast<FunctionDecl*>(f)))
      return true;
  return false;
}

auto GlobalScope::Impl::constraintsHold(FunctionDecl* f, const Subst& deduced) -> bool
{
  // The template parameters' type-constraints first, then the requires clause, as C++ conjoins
  // them ([temp.constr.decl] 3). One that cannot be told while an argument is not known holds for
  // now; for known arguments it is an error, as an unevaluable requires clause is.
  for (Decl* tp : f->templateParams)
  {
    auto* p = static_cast<TemplateParamDecl*>(tp);
    std::optional<bool> v = typeConstraintHolds(p, f, deduced, 0);
    if (v && !*v)
      return false;
    if (v)
      continue;
    const Ty* t = deduced.find(p);
    if (!t || !t->isKnown() || types.isDependent(t) || mentionsUnknown(t))
      continue;
    std::string name = f->name.empty() ? std::string("?") : debugName(f->name.last().symbol);
    std::string param(symbols.name(p->name));
    reportCheckerError(f->file, p->token, "the constraint on " + param + " of " + name + " cannot be evaluated for " + param + " = " + types.toString(t, &symbols));
    return false;
  }
  return !f->requiresClause || requiresHolds(f, deduced);
}

auto GlobalScope::Impl::normalizeConstraint(NormalConstraint& n, const Node* e, Decl* owner, const Subst& s, int depth) -> int
{
  if (!e || !owner || depth > 32 || n.terms.size() > 256)
    return -1;
  auto join = [&](uint8_t kind, int l, int r) {
    if (l < 0 || r < 0)
      return -1;
    n.terms.push_back(ConstraintTerm{kind, l, r, -1});
    return int(n.terms.size() - 1);
  };
  switch (e->kind)
  {
    case NodeKind::ParenExpr: return normalizeConstraint(n, static_cast<const ParenExpr*>(e)->inner, owner, s, depth + 1);
    case NodeKind::BinaryExpr:
    {
      auto* b = static_cast<const BinaryExpr*>(e);
      bool andOp = b->isKeyword ? Kw(b->op) == Kw::and_ : P(b->op) == P::AndAnd;
      bool orOp = b->isKeyword ? Kw(b->op) == Kw::or_ : P(b->op) == P::OrOr;
      if (andOp || orOp)
        return join(andOp ? 1 : 2, normalizeConstraint(n, b->lhs, owner, s, depth + 1), normalizeConstraint(n, b->rhs, owner, s, depth + 1));
      break;
    }
    case NodeKind::NameExpr:
    {
      // A concept-id: the concept's definition, its parameters bound to the arguments named here.
      auto* name = static_cast<const NameExpr*>(e);
      if (!name->name.last().hasTemplateArgs)
        break;
      LocalStack own;
      pushEnclosingTemplateParams(owner, own);
      VariableDecl* concept_ = conceptOf(lookupQualified(name->name, lexicalScopeOf(owner), &own, depth + 1).decl);
      if (!concept_)
        break;
      TypeContext ctx{lexicalScopeOf(owner), &own, &s, owner->file ? owner->file->tokens : nullptr, owner->file ? owner->file->text : std::string_view()};
      std::vector<const Ty*> args;
      size_t count = concept_->templateParams.size();
      for (Node* written : name->name.last().templateArgs)
      {
        const Ty* a = templateArgument(written, concept_->templateParams[std::min(args.size(), count - 1)], ctx, depth + 1);
        if (!a || !a->isKnown())
          return -1;
        if (written && written->kind == NodeKind::PackType && a->kind == TyKind::Pack)
          args.insert(args.end(), a->args.begin(), a->args.end());
        else
          args.push_back(a);
      }
      Subst cs;
      for (size_t i = 0; i < count; ++i)
      {
        auto* param = static_cast<TemplateParamDecl*>(concept_->templateParams[i]);
        if (param->isPack && i + 1 == count)
        {
          std::vector<const Ty*> rest(args.begin() + std::min(i, args.size()), args.end());
          cs.pairs.push_back({param, types.pack(Span<const Ty*>{rest.data(), uint32_t(rest.size())})});
          break;
        }
        if (i >= args.size())
          return -1;
        cs.pairs.push_back({param, args[i]});
      }
      return normalizeConstraint(n, concept_->init, concept_, cs, depth + 1);
    }
    default: break;
  }
  ConstraintAtom atom;
  atom.e = e;
  for (Decl* tp : owner->templateParams)
    atom.mapping.push_back(s.find(tp));
  n.atoms.push_back(std::move(atom));
  n.terms.push_back(ConstraintTerm{0, -1, -1, int(n.atoms.size() - 1)});
  return int(n.terms.size() - 1);
}

auto GlobalScope::Impl::associatedConstraints(NormalConstraint& n, Decl* f, const Subst& s) -> int
{
  int root = -1;
  auto conjoin = [&](int t) {
    if (root == -2 || t < 0)
      return void(root = -2);
    if (root == -1)
      return void(root = t);
    n.terms.push_back(ConstraintTerm{1, root, t, -1});
    root = int(n.terms.size() - 1);
  };
  for (Decl* tp : f->templateParams)
  {
    auto* p = static_cast<TemplateParamDecl*>(tp);
    VariableDecl* concept_ = typeConstraintOf(p, f);
    if (!concept_)
      continue;
    if (p->isPack)
    {
      // `template<C... Ts>`: (C<Ts> && ...), an atom of its own (its own written constraint).
      ConstraintAtom atom;
      atom.e = p->valueType;
      for (Decl* q : f->templateParams)
        atom.mapping.push_back(s.find(q));
      n.atoms.push_back(std::move(atom));
      n.terms.push_back(ConstraintTerm{0, -1, -1, int(n.atoms.size() - 1)});
      conjoin(int(n.terms.size() - 1));
      continue;
    }
    // `template<C<X> T>` is `C<T, X>`: the concept's definition with T and the written arguments.
    LocalStack own;
    pushEnclosingTemplateParams(f, own);
    TypeContext ctx{lexicalScopeOf(f), &own, &s, f->file ? f->file->tokens : nullptr, f->file ? f->file->text : std::string_view()};
    const Ty* self = s.find(p);
    std::vector<const Ty*> args{self ? self : types.param(p)};
    size_t count = concept_->templateParams.size();
    bool formed = true;
    for (Node* written : static_cast<NamedType*>(p->valueType)->name.last().templateArgs)
    {
      const Ty* a = templateArgument(written, concept_->templateParams[std::min(args.size(), count - 1)], ctx, 1);
      formed = formed && a && a->isKnown();
      if (formed)
        args.push_back(a);
    }
    Subst cs;
    for (size_t i = 0; formed && i < count; ++i)
    {
      auto* param = static_cast<TemplateParamDecl*>(concept_->templateParams[i]);
      if (param->isPack && i + 1 == count)
      {
        std::vector<const Ty*> rest(args.begin() + std::min(i, args.size()), args.end());
        cs.pairs.push_back({param, types.pack(Span<const Ty*>{rest.data(), uint32_t(rest.size())})});
        break;
      }
      formed = i < args.size();
      if (formed)
        cs.pairs.push_back({param, args[i]});
    }
    conjoin(formed ? normalizeConstraint(n, concept_->init, concept_, cs, 1) : -1);
  }
  if (f->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(f)->requiresClause)
    conjoin(normalizeConstraint(n, static_cast<FunctionDecl*>(f)->requiresClause, f, s, 1));
  else if (f->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(f)->requiresClause)
    conjoin(normalizeConstraint(n, static_cast<ClassDecl*>(f)->requiresClause, f, s, 1));
  return root;
}

auto GlobalScope::Impl::constraintsSubsume(Decl* a, Decl* b) -> bool
{
  if (a->templateParams.size() != b->templateParams.size())
    return false;
  Subst sa, sb;
  for (size_t i = 0; i < a->templateParams.size(); ++i)
  {
    sa.pairs.push_back({a->templateParams[i], types.param(a->templateParams[i])});
    sb.pairs.push_back({b->templateParams[i], types.param(a->templateParams[i])});
  }
  NormalConstraint an, bn;
  int at = associatedConstraints(an, a, sa), bt = associatedConstraints(bn, b, sb);
  if (at == -2 || bt == -2)
    return false;
  return subsumes(an, at, bn, bt);
}

auto GlobalScope::Impl::constraintClauses(const NormalConstraint& n, int t, bool disjunction, std::vector<std::vector<int>>& out) -> bool
{
  out.clear();
  if (t < 0)
  {
    if (disjunction)
      out.push_back({}); // true: one empty conjunction
    return true;          // (as a conjunction of disjunctions: none)
  }
  const auto& term = n.terms[size_t(t)];
  if (term.kind == 0)
  {
    out.push_back({term.atom});
    return true;
  }
  std::vector<std::vector<int>> l, r;
  if (!constraintClauses(n, term.left, disjunction, l) || !constraintClauses(n, term.right, disjunction, r))
    return false;
  // The operator that keeps the clauses apart (|| for a disjunctive form, && for a conjunctive
  // one) unites the lists; the other distributes, each clause of one with each of the other.
  bool unites = (term.kind == 2) == disjunction;
  if (unites)
  {
    out = std::move(l);
    out.insert(out.end(), r.begin(), r.end());
  }
  else
    for (const auto& x : l)
      for (const auto& y : r)
      {
        std::vector<int> c = x;
        c.insert(c.end(), y.begin(), y.end());
        out.push_back(std::move(c));
      }
  return out.size() <= 256;
}

auto GlobalScope::Impl::subsumes(const NormalConstraint& pn, int pt, const NormalConstraint& qn, int qt) -> bool
{
  std::vector<std::vector<int>> disjuncts, conjuncts;
  if (!constraintClauses(pn, pt, true, disjuncts) || !constraintClauses(qn, qt, false, conjuncts))
    return false;
  auto identical = [&](int i, int j) { return pn.atoms[size_t(i)].e == qn.atoms[size_t(j)].e && pn.atoms[size_t(i)].mapping == qn.atoms[size_t(j)].mapping; };
  for (const auto& p : disjuncts)
    for (const auto& q : conjuncts)
    {
      bool shared = false;
      for (int i : p)
        for (int j : q)
          shared = shared || identical(i, j);
      if (!shared)
        return false;
    }
  return true;
}

auto GlobalScope::Impl::moreConstrained(FunctionDecl* a, FunctionDecl* b, const Subst* classSubst) -> bool
{
  if (a == b || a->templateParams.size() != b->templateParams.size() || (!constrained(a) && !constrained(b)))
    return false;
  // a's parameters stand for themselves, b's for a's at the same position.
  Subst sa = classSubst ? *classSubst : Subst{}, sb = sa;
  for (size_t i = 0; i < a->templateParams.size(); ++i)
  {
    sa.pairs.push_back({a->templateParams[i], types.param(a->templateParams[i])});
    sb.pairs.push_back({b->templateParams[i], types.param(a->templateParams[i])});
  }
  NormalConstraint an, bn;
  int at = associatedConstraints(an, a, sa), bt = associatedConstraints(bn, b, sb);
  if (at == -2 || bt == -2)
    return false;
  return subsumes(an, at, bn, bt) && !subsumes(bn, bt, an, at);
}

auto GlobalScope::Impl::equivalentParameters(FunctionDecl* a, FunctionDecl* b, const Subst* classSubst) -> bool
{
  if (a->templateParams.size() != b->templateParams.size() || a->params.size() != b->params.size())
    return false;
  const Ty* ta = declaredTypeOf(a, classSubst, 0);
  const Ty* tb = declaredTypeOf(b, classSubst, 0);
  if (!ta || !tb || ta->kind != TyKind::Function || tb->kind != TyKind::Function || ta->variadic != tb->variadic || ta->args.size() != tb->args.size())
    return false;
  Subst s;
  for (size_t i = 0; i < a->templateParams.size(); ++i)
    s.pairs.push_back({b->templateParams[i], types.param(a->templateParams[i])});
  for (size_t i = 0; i < ta->args.size(); ++i)
    if (!ta->args[i] || !tb->args[i] || (s.empty() ? tb->args[i] : substitute(tb->args[i], s)) != ta->args[i])
      return false;
  return (a->flags & (DF_Const | DF_Volatile | DF_RefQual)) == (b->flags & (DF_Const | DF_Volatile | DF_RefQual)) && a->refQualifier == b->refQualifier;
}

auto GlobalScope::Impl::deductionsAgree(FunctionDecl* f, const Ty* ft, const std::vector<const Ty*>& argTypes, const Subst& deduced, size_t given) -> bool
{
  if (!ft || ft->kind != TyKind::Function)
    return true;
  // Each argument's deduction on its own, from the given pairs (as deduceParams walks them; a
  // pack's elements deduce one by one there), against the call's.
  for (size_t i = 0; i < f->params.size() && i < ft->args.size() && i < argTypes.size(); ++i)
  {
    if (static_cast<ParamDecl*>(f->params[i])->isPack)
      break;
    const Ty* at = argTypes[i];
    if (!at || !at->isKnown() || types.isDependent(at) || mentionsUnknown(at) || at->isBraceList())
      continue;
    Subst one;
    for (size_t k = 0; k < given && k < deduced.pairs.size(); ++k)
      one.pairs.push_back(deduced.pairs[k]);
    size_t from = one.pairs.size();
    // A pointer parameter whose pointee deduces a template parameter takes a pointer, an array or
    // a function and nothing else ([temp.deduct.call] 4: no conversion makes an argument match the
    // pattern): `template<class T> f(T*, T)` given nullptr and a char is no candidate, rather than
    // one with T = char that converts nullptr (thread J's overload generator: it beat
    // `f(const char*, short)`). A pointee the given arguments name already deduces nothing.
    if (const Ty* p = types.unqualified(types.stripRef(ft->args[i])); p->kind == TyKind::Pointer && types.isDependent(one.empty() ? p : substitute(p, one)))
    {
      const Ty* pointee = p->inner;
      while (pointee && pointee->kind == TyKind::Pointer)
        pointee = pointee->inner;
      const Ty* a = types.stripRef(at);
      if (pointee && pointee->kind != TyKind::DependentMember && a->kind != TyKind::Pointer && a->kind != TyKind::Array && a->kind != TyKind::Function)
        return false;
    }
    deduce(ft->args[i], at, one);
    for (size_t k = from; k < one.pairs.size(); ++k)
      if (const Ty* all = deduced.find(one.pairs[k].param); all && one.pairs[k].type && all != one.pairs[k].type)
        return false;
  }
  return true;
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
  pushFunctionParams(f, own);
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

auto GlobalScope::Impl::closureCall(const Ty* lambdaTy) -> ClosureCall*
{
  LambdaExpr* l = lambdaOf(lambdaTy);
  if (!l)
    return nullptr;
  auto* closure = static_cast<ClassDecl*>(lambdaTy->decl);
  std::lock_guard lock(closureCallMutex);
  std::unique_ptr<ClosureCall>& slot = closureCalls[lambdaTy];
  if (slot)
    return slot.get();
  slot = std::make_unique<ClosureCall>();
  ClosureCall& c = *slot;
  FunctionDecl& f = c.decl;
  f.kind = NodeKind::FunctionDecl;
  f.token = l->token;
  f.file = closure->file;
  f.parent = closure;
  f.access = Access::Public;
  f.flags = DF_Operator | (l->isMutable ? 0u : uint32_t(DF_Const)) | (l->isNoexcept ? uint32_t(DF_Noexcept) : 0u);
  f.noexceptCondition = l->noexceptCondition;
  f.templateParams = l->templateParams;
  f.isTemplate = !l->templateParams.empty();
  f.params = l->params;
  f.endToken = closure->endToken;
  c.name.special = 2;
  c.name.opToken = uint32_t(P::LParen);
  f.name.parts = Span<NamePart>{&c.name, 1};
  c.closure = lambdaTy;
  c.signature = types.function(lambdaTy->inner ? lambdaTy->inner : types.unknown(), lambdaTy->args, lambdaTy->variadic);
  c.entries.push_back(Entry{&f, nullptr});
  closureCallsByDecl[&f] = &c;
  return &c;
}

auto GlobalScope::Impl::closureCallOf(const FunctionDecl* f) -> ClosureCall*
{
  if (!maybeClosureCall(f))
    return nullptr;
  std::lock_guard lock(closureCallMutex);
  auto it = closureCallsByDecl.find(f);
  return it == closureCallsByDecl.end() ? nullptr : it->second;
}

auto GlobalScope::Impl::sameFunction(FunctionDecl* a, FunctionDecl* b) -> bool
{
  if (a == b)
    return true;
  // Two closures' call operators are two functions, whatever their types (`[](int) {...}` twice in
  // one overload set: ambiguous, as in C++).
  if (closureCallOf(a) || closureCallOf(b))
    return false;
  // A C function's static is its linkage, which a later extern declaration takes from a visible
  // static one (C11 6.2.2p4): MSVC 14.44's time.h defines time static inline, CTime.kpp declares
  // it extern "C", one function.
  uint32_t compared = isCFunction(a) && isCFunction(b) ? DF_Const | DF_Volatile | DF_RefQual : DF_Const | DF_Volatile | DF_RefQual | DF_Static;
  if (a->params.size() != b->params.size() || a->name.empty() || b->name.empty() || functionSymbol(a->name) != functionSymbol(b->name) || !sameNamePart(a->name.last(), b->name.last()) ||
      a->templateParams.size() != b->templateParams.size() || (a->flags & compared) != (b->flags & compared) || a->refQualifier != b->refQualifier)
    return false;
  // A function declared `extern "C"` and then defined without it is one C function (C++
  // [dcl.link] 5: the later declaration takes the earlier one's linkage), as Lua's
  // lua_getnresults is in lua.kpp and ldebug.kpp.
  if (!(isCFunction(a) && isCFunction(b)) && homeScope(a) != homeScope(b))
    return false;
  // Two definitions of one signature that constraints tell apart are two overloads (`requires A<T>`
  // and `requires B<T>`, `template<Small T>` and `template<SmallInt T>`), never one function
  // declared twice, whose first declaration a tie took.
  if (a->body && b->body && (constrained(a) || constrained(b)))
    return false;
  // Two definitions of a function template are two templates: one template defined twice is an
  // error, and their types may differ only where the comparison cannot tell (results that depend on
  // a value, `typename OnlyIf<sizeof(T) == 4, int>::type` against `== 8`), which made the second a
  // redeclaration of the first and its calls the first's. A `when` pair, one of them absent in the
  // configuration, stays one function.
  if (a->body && b->body && !a->templateParams.empty() && !b->templateParams.empty() && !a->when && !b->when)
    return false;
  return typeAs(b, a) == matchedType(a);
}

// Whether t names a member of a class that is no longer dependent and has no such member (what
// substitute leaves of `typename OnlyIf<false, int>::type`), or a member of a type that is no class
// (`typename T::type` with T = int): a substitution failure.
auto GlobalScope::Impl::namesMissingMember(const Ty* t, int depth) -> bool
{
  if (!t || depth > 16)
    return false;
  if (t->kind == TyKind::DependentMember && t->inner && t->inner->kind == TyKind::Class && !types.isDependent(t->inner) &&
      std::all_of(t->inner->args.begin(), t->inner->args.end(), [](const Ty* a) { return a && a->isKnown(); }))
    return true;
  if (t->kind == TyKind::DependentMember && t->inner && t->inner->isKnown() && t->inner->kind != TyKind::Class && t->inner->kind != TyKind::Enum &&
      t->inner->kind != TyKind::Pack && !types.isDependent(t->inner) && !mentionsUnknown(t->inner))
    return true;
  if (namesMissingMember(t->inner, depth + 1))
    return true;
  for (const Ty* a : t->args)
    if (namesMissingMember(a, depth + 1))
      return true;
  return false;
}

// Whether f's result or a parameter's type, as written, has a decltype (`-> decltype(t.size())`,
// `decltype(T::value)* = nullptr`), which typing under the deduced arguments may find does not form.
static bool signatureHasDecltype(const FunctionDecl* f)
{
  bool found = false;
  auto look = [&](Type* t) {
    if (t && !found)
      forEachNode(t, [&](Node* n) { found = found || (n->kind == NodeKind::DecltypeType && !static_cast<DecltypeType*>(n)->isAuto); });
  };
  look(f->returnType);
  look(f->trailingReturn);
  for (Decl* p : f->params)
    if (p && p->kind == NodeKind::ParamDecl)
      look(static_cast<ParamDecl*>(p)->type);
  return found;
}

// Whether t names a member of a class (`typename X<...>::type`), anywhere in it.
auto GlobalScope::Impl::namesDependentMember(const Ty* t, int depth) -> bool
{
  if (!t || depth > 16)
    return false;
  if (t->kind == TyKind::DependentMember)
    return true;
  if (namesDependentMember(t->inner, depth + 1))
    return true;
  for (const Ty* a : t->args)
    if (namesDependentMember(a, depth + 1))
      return true;
  return false;
}

auto GlobalScope::Impl::chooseAmong(const std::vector<FunctionDecl*>& candidates, const std::vector<const Ty*>& argTypes, const Subst* classSubst, const Ty* objectTy, bool& ambiguous, const std::function<bool(FunctionDecl*, Subst&)>* bindExplicit, const std::vector<bool>* nullConstants, std::vector<FunctionDecl*>* tiedOut, const std::vector<const Ty*>* bitFieldPromotions) -> FunctionDecl*
{
  bool argsConcrete = true;
  for (const Ty* at : argTypes)
    argsConcrete = argsConcrete && at && at->isKnown() && !types.isDependent(at);
  std::vector<Candidate> viable;
  bool objectConst = objectTy && types.stripRef(objectTy)->isConst;
  bool objectVolatile = objectTy && types.stripRef(objectTy)->isVolatile;
  // The object's value category where the caller gives it, an lvalue as T&, an rvalue as T&& (a
  // member call's object, argumentType's): a ref-qualified member function takes only the objects
  // its implicit object parameter binds ([over.match.funcs] 4, 5): `f() &&` no lvalue, a non-const
  // `f() &` no rvalue. A plain T: not known, every one is a candidate as before.
  bool objectLvalue = objectTy && objectTy->kind == TyKind::LRef, objectRvalue = objectTy && objectTy->kind == TyKind::RRef;
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
      bool volatileMethod = (f->flags & DF_Volatile) != 0;
      if ((objectConst && !constMethod) || (objectVolatile && !volatileMethod))
        continue;
      c.thisRank = ((!objectConst && constMethod) ? 1 : 0) + ((!objectVolatile && volatileMethod) ? 1 : 0); // (the fewer it adds the better; const against volatile ties)
      if (objectLvalue && f->refQualifier == FunctionDecl::RefQualifier::RValue)
        continue;
      if (objectRvalue && f->refQualifier == FunctionDecl::RefQualifier::LValue && (!constMethod || (f->flags & DF_Volatile)))
        continue;
      if (objectRvalue && f->refQualifier == FunctionDecl::RefQualifier::LValue)
        c.refRank = 1;
    }
    const Ty* ft = declaredTypeOf(f, classSubst, 0);
    Subst deduced = classSubst ? *classSubst : Subst{};
    if (bindExplicit && !(*bindExplicit)(f, deduced))
      continue;
    size_t givenCount = deduced.pairs.size(); // the class's and the explicit arguments: not deduced
    if (!f->templateParams.empty() && ft && ft->kind == TyKind::Function)
    {
      deduceCall(f, ft, argTypes, deduced);
      if (!deductionsAgree(f, ft, argTypes, deduced, givenCount))
        continue; // `f(1, 2.5)` of `f(T, T)`: T deduced as int and as double, no candidate
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
      // (a bit-field's conversion to the type it promotes to by its width is a promotion; to its
      // declared type it stays an exact match)
      if (bitFieldPromotions && i < bitFieldPromotions->size() && (*bitFieldPromotions)[i] && r < NotViable && r > 2 &&
          (!pt->isReference() || (pt->kind == TyKind::LRef && types.stripRef(pt)->isConst)) &&
          types.unqualified(types.stripRef(pt)) == (*bitFieldPromotions)[i])
        r = 2;
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
        c.zeroAsPointer = c.zeroAsPointer || r < NotViable;
      }
      if (r >= NotViable)
        ok = false;
      c.ranks.push_back(r);
      c.refs.push_back(pt->isReference());
      c.params.push_back(pt);
    }
    if (ok && !constraintsHold(f, deduced))
      ok = false; // `requires isIntegral<T>`: the constraint fails for these arguments
    // SFINAE on the function type ([temp.deduct] 7, 8): a template whose result or parameter type (one no argument
    // is given for too: `typename T::type* = nullptr`), under the deduced arguments, names a member its class does
    // not have (`typename OnlyIf<sizeof(T) == 4, int>::type` for a double, OnlyIf<false> having no type), a member of
    // a type that is no class (`typename T::type` with T = int), or has a decltype whose expression does not form
    // (`-> decltype(t.size())` for an int), is no candidate. Until 2026-11-10 the result's members only.
    if (ok && argsConcrete && !f->templateParams.empty() && ft && ft->kind == TyKind::Function && (namesDependentMember(ft) || signatureHasDecltype(f)))
    {
      // (typed again under the deduced arguments, as the call's result is: the declaration's own
      // typing leaves a value such as `sizeof(T) == 4` uncomputed)
      const Ty* typed = declaredTypeOf(f, &deduced, 0);
      if (typed && typed->kind == TyKind::Function && (!typed->inner || mentionsUnknown(typed) || namesMissingMember(typed)))
        ok = false; // (a member the class does not have types as no type at all, or as that member of the class)
    }
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
        // (the conversion function each takes, the object as the argument is, an lvalue or an rvalue: one
        // conversion function to both, its result's conversions compare; until 2026-11-08 only for a class
        // converting to one scalar type, so that of `operator int&()` beside `operator double()` A5(int, double
        // = 0.5) and A5(long) given the int& tied, and A5(long) was called)
        auto viaFunction = [&](const Ty* param, FunctionDecl*& f) -> const Ty* {
          Subst opArgs;
          bool twoOperators = false;
          f = chooseConversion(conversionObject(argTypes[i]), param, GlobalScope::ConversionKind::ToType, false, nullptr, &opArgs, false, &twoOperators);
          const Ty* ft = f && !twoOperators ? declaredTypeOf(f, opArgs.empty() ? nullptr : &opArgs, 0) : nullptr;
          if (ft && ft->kind == TyKind::Function && ft->inner && !f->templateParams.empty())
            return substitute(ft->inner, opArgs);
          return ft && ft->kind == TyKind::Function ? ft->inner : nullptr;
        };
        FunctionDecl* fa = nullptr, *fb = nullptr;
        const Ty* resultA = from->kind == TyKind::Class && !from->isBraceList() && pa != pb && pa->kind != TyKind::Class && pb->kind != TyKind::Class
                              ? viaFunction(pa, fa) : nullptr;
        const Ty* resultB = resultA ? viaFunction(pb, fb) : nullptr;
        if (resultA && resultB && fa == fb && resultA->isKnown() && !types.isDependent(resultA))
          if (const Ty* result = resultA)
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
            // Of one rank, the tie-breakers of two standard conversions from one type: a pointer made a bool is the
            // worse ([over.ics.rank] 4.1), and a pointer to a class made a base's pointer beats one made void*, a
            // nearer base's a farther's (4.4.2, 4.4.4). `operator int*()` given f(bool) and f(void*) calls f(void*)
            // (it was ambiguous: thread J's overload battery).
            if (ra == 3 && (result->kind == TyKind::Pointer || result->kind == TyKind::MemberPointer || result->kind == TyKind::Nullptr))
            {
              bool aBool = pa->kind == TyKind::Bool, bBool = pb->kind == TyKind::Bool;
              if (aBool != bBool)
              {
                if (aBool)
                  return false;
                someBetter = true;
                continue;
              }
              const Ty* target = result->kind == TyKind::Pointer ? types.unqualified(result->inner) : nullptr;
              if (target && target->kind == TyKind::Class && pa->kind == TyKind::Pointer && pb->kind == TyKind::Pointer)
              {
                const Ty* ua = types.unqualified(pa->inner);
                const Ty* ub = types.unqualified(pb->inner);
                int order = ua == ub                                                         ? 0
                            : ua->kind == TyKind::Class && ub->kind == TyKind::Void          ? 1
                            : ua->kind == TyKind::Void && ub->kind == TyKind::Class          ? -1
                            : ua->kind == TyKind::Class && ub->kind == TyKind::Class && isBaseClass(ub, ua) ? 1
                            : ua->kind == TyKind::Class && ub->kind == TyKind::Class && isBaseClass(ua, ub) ? -1
                                                                                                           : 0;
                if (order < 0)
                  return false;
                if (order > 0)
                {
                  someBetter = true;
                  continue;
                }
              }
            }
            // Of one rank, an rvalue reference binding the converted value beats an lvalue reference
            // ([over.ics.rank] 3.2.3 in the second standard conversion, 3.3): `operator bool()` given
            // f(double&&) and f(const long&) calls f(double&&) (it was ambiguous: thread J's battery).
            bool aRvalue = a.params[i]->kind == TyKind::RRef, bRvalue = b.params[i]->kind == TyKind::RRef;
            bool aLvalue = a.params[i]->kind == TyKind::LRef, bLvalue = b.params[i]->kind == TyKind::LRef;
            if ((aRvalue && bLvalue) || (aLvalue && bRvalue))
            {
              if (aLvalue)
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
      if (a.ranks[i] == b.ranks[i] && a.ranks[i] >= 1 && a.ranks[i] <= 3 && i < a.params.size() && i < b.params.size() &&
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
        else if (argument && argument->kind == TyKind::Class && pa->kind == TyKind::Class && pb->kind == TyKind::Class && pa != pb)
          ta = pa, tb = pb; // a class by value beside another by value or by reference: the nearer base (m(D&) over m(B) given a DD)
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
          else if (ua->kind == TyKind::Class && ub->kind == TyKind::Class)
          {
            // (whatever the qualifiers: `pick(const D&)` over `pick(B&)` and `pick(D*)` over
            // `pick(const B* const&)` given a DD, as clang and g++ have it; they were ambiguous)
            if (isBaseClass(ub, ua))
              someBetter = true;
            else if (isBaseClass(ua, ub))
              return false;
          }
          else if (ua->kind != ub->kind && (ua->kind == TyKind::Void || ub->kind == TyKind::Void))
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
      // A function lvalue the other way round: the `T&` binds it better (3.2.5), c(int (&)(int))
      // over c(int (&&)(int)) given fun.
      if (a.ranks[i] == b.ranks[i] && i < a.params.size() && i < b.params.size() && a.params[i] && b.params[i] &&
          a.params[i]->kind != b.params[i]->kind && a.params[i]->isReference() && b.params[i]->isReference())
      {
        if ((a.params[i]->kind == TyKind::RRef) != (argument && argument->kind == TyKind::Function))
          someBetter = true;
        else
          return false;
      }
    }
    // The implicit object parameters: an rvalue object binding `&&` over `const&` ([over.ics.rank]
    // 3.2.3), then their constness (3.2.6).
    int aObject = a.refRank != b.refRank ? a.refRank : a.thisRank, bObject = a.refRank != b.refRank ? b.refRank : b.thisRank;
    if (aObject > bObject)
      return false;
    if (aObject < bObject)
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
    // Alike but for their constraints (the same parameters, b's template parameters taken as a's):
    // the more constrained ([temp.func.order] 6.2.2; of two non-templates, [over.match.best] 2.6):
    // `template<SmallInt T>` over `template<Small T>` with SmallInt = Small<T> && integral<T>,
    // `requires integral<T>` over no requires clause. Two whose constraints neither subsumes stay
    // tied: ambiguous (sameFunction keeps them apart). Until 2026-11-05 the first declared won.
    if (!someBetter && a.f->templateParams.empty() == b.f->templateParams.empty() && (constrained(a.f) || constrained(b.f)) &&
        equivalentParameters(a.f, b.f, classSubst) && moreConstrained(a.f, b.f, classSubst))
      return true;
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
  // A literal 0 is no pointer in kpp (design.md 4): of candidates C++ finds alike, one that would
  // take it as a pointer is none while another remains (`take(0)` with take(u64) and take(const
  // char*) is take(u64), the two conversions alike in C++, the call ambiguous). One that C++
  // prefers, its pointer conversion the better, stays the choice, and the walk reports its 0 (so
  // the converter writes nullptr there), as imported C's 0 stays C's null pointer.
  auto zeroAsPointer = [](const Candidate* c) { return c->zeroAsPointer; };
  if (distinct.size() > 1 && std::any_of(distinct.begin(), distinct.end(), zeroAsPointer) &&
      !std::all_of(distinct.begin(), distinct.end(), zeroAsPointer))
  {
    tied.erase(std::remove_if(tied.begin(), tied.end(), zeroAsPointer), tied.end());
    distinct.erase(std::remove_if(distinct.begin(), distinct.end(), zeroAsPointer), distinct.end());
  }
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

auto GlobalScope::Impl::binaryOperatorRanks(FunctionDecl* f, const Subst& subst, bool member, const Ty* lhs, const Ty* rhs) -> std::vector<int>
{
  if (!f)
    return {};
  // (a template's own parameters deduced from the operands, beside its class's in subst)
  Subst deduced = subst;
  if (!f->templateParams.empty())
  {
    std::vector<const Ty*> operands;
    if (!member)
      operands.push_back(lhs);
    operands.push_back(rhs);
    deduceCall(f, declaredTypeOf(f, deduced.empty() ? nullptr : &deduced, 0), operands, deduced);
    for (Decl* p : f->templateParams)
      if (const Ty* bound = deduced.find(p); !bound || !bound->isKnown() || types.isDependent(bound))
        return {};
  }
  const Ty* ft = declaredTypeOf(f, deduced.empty() ? nullptr : &deduced, 0);
  if (!ft || ft->kind != TyKind::Function || types.isDependent(ft) || mentionsUnknown(ft) || ft->args.size() < (member ? 1u : 2u))
    return {};
  std::vector<int> ranks;
  if (member)
    ranks = {0, std::max(1, conversionRank(rhs, ft->args[0])) - 1};
  else
    ranks = {std::max(1, conversionRank(lhs, ft->args[0])) - 1, std::max(1, conversionRank(rhs, ft->args[1])) - 1};
  for (int r : ranks)
    if (r >= NotViable - 1)
      return {};
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
  return compareFreeWithMember(free, freeSubst, member, memberSubst, lhs, rhs, arity) == 1;
}

auto GlobalScope::Impl::compareFreeWithMember(FunctionDecl* free, const Subst& freeSubst, FunctionDecl* member, const Subst& memberSubst,
                                                       const Ty* lhs, const Ty* rhs, size_t arity) -> int
{
  if (arity != 2 || !free || !free->templateParams.empty() || !member || !member->templateParams.empty())
    return 2;
  std::vector<int> mine = memberOperatorRanks(member, memberSubst, rhs, arity);
  if (mine.size() != 2)
    return 2;
  const Ty* ft = declaredTypeOf(free, freeSubst.empty() ? nullptr : &freeSubst, 0);
  if (!ft || ft->kind != TyKind::Function || ft->args.size() != 2)
    return 2;
  int first = std::max(1, conversionRank(lhs, ft->args[0])) - 1;
  int second = std::max(1, conversionRank(rhs, ft->args[1])) - 1;
  if (first >= NotViable - 1 || second >= NotViable - 1)
    return -1; // (the free one does not take them)
  if (first <= mine[0] && second <= mine[1] && (first < mine[0] || second < mine[1]))
    return 1;
  if (mine[0] <= first && mine[1] <= second && (mine[0] < first || mine[1] < second))
    return -1;
  return 0;
}
