// GlobalScope::Impl, the compile-time evaluator (its declarations in Interpreter.hpp, part of GlobalScopeImpl.hpp; design.md,
// "Compile-time evaluation: one interpreter"): a translation unit of its own since 2026-11-05 (resolver.cpp split, stage 2), moved without a change.
#include "GlobalScopeImpl.hpp"

namespace
{
  // KPP_VERIFY_DECISIONS=1: the interpreter's own choice of an operator function too, compared with the
  // one the checker recorded (design.md 24), as codegen's are (Emitter::verifyDecisions).
  bool verifyInterpreterDecisions()
  {
    static const bool on = [] { const char* v = std::getenv("KPP_VERIFY_DECISIONS"); return v && *v && *v != '0'; }();
    return on;
  }
}

auto GlobalScope::Impl::constWhy() -> std::string&
{
  static thread_local std::string why;
  return why;
}

auto GlobalScope::Impl::constHooks() -> const ConstHooks*&
{
  static thread_local const ConstHooks* hooks = nullptr;
  return hooks;
}

auto GlobalScope::Impl::ranBody(const FunctionDecl* def) -> void
{
  if (const ConstHooks* h = constHooks(); h && h->ran)
    h->ran(def);
  recordUse(USE_RanBody, def); // (and into a cached answer's collection)
  if (!recordingInto && recordingDependencies && !constHooks())
  {
    std::lock_guard lock(ranOutsideMutex);
    ranOutside.insert(def);
  }
}

auto GlobalScope::Impl::forgetCachedAnswers(const std::function<bool(const CollectedUse&)>& changed) -> void
{
  auto meets = [&](const std::vector<CollectedUse>& uses)
  {
    for (const CollectedUse& u : uses)
      if (changed(u))
        return true;
    return false;
  };
  {
    std::lock_guard lock(memberConstantsMutex);
    std::erase_if(memberConstants, [&](const auto& e) { return meets(e.second.uses); });
  }
  {
    std::lock_guard lock(specializationCacheMutex);
    std::erase_if(specializationCache, [&](const auto& e) { return meets(e.second.uses); });
  }
  {
    std::lock_guard lock(constLayoutsMutex);
    std::erase_if(constLayouts, [&](const auto& e) { return meets(e.second->uses); });
  }
  for (TraitShard& shard : traitShards)
  {
    std::unique_lock lock(shard.mutex);
    for (auto it = shard.uses.begin(); it != shard.uses.end();)
      if (meets(it->second))
      {
        shard.answers.erase(it->first);
        it = shard.uses.erase(it);
      }
      else
        ++it;
  }
}

auto GlobalScope::Impl::replayUses(const std::vector<CollectedUse>& uses) -> void
{
  if (!recordingDependencies)
    return;
  for (const auto& [kind, what] : uses)
    if (kind == USE_RanBody)
      ranBody(static_cast<const FunctionDecl*>(what));
    else if (kind == USE_ReportedError)
      reportCheckerErrorText(*static_cast<const std::string*>(what));
  replayCollected(uses);
}

auto GlobalScope::Impl::constArith() -> ConstArith
{
  auto range = [this](const Ty* e) { return enumValueRange(e); };
  if (const ConstHooks* h = constHooks())
    return ConstArith{types, h->mode, &constWhy(), h->underlying ? h->underlying : [this](const Ty* e) { return declaredUnderlying(e); }, range};
  return ConstArith{types, ConstMode::Required, &constWhy(), [this](const Ty* e) { return declaredUnderlying(e); }, range};
}

auto GlobalScope::Impl::enumValueRange(const Ty* t) -> std::optional<std::pair<int64_t, int64_t>>
{
  t = t ? types.unqualified(t) : nullptr;
  if (!t || t->kind != TyKind::Enum || !t->decl)
    return std::nullopt;
  {
    std::lock_guard lock(enumRangeMutex);
    if (auto it = enumRanges.find(t); it != enumRanges.end())
      return it->second;
  }
  Decl* def = t->decl;
  if (!def->ownScope)
  {
    Entry e{def, nullptr};
    if (Scope* s = scopeOfEntry(e, 0))
      def = s->owner;
  }
  if (!def || def->kind != NodeKind::EnumDecl)
    return std::nullopt;
  auto* e = static_cast<EnumDecl*>(def);
  // A fixed underlying type (written, or an enum class's int) takes every value of it; an
  // instance's enumeration (`Flag<A>::Enum`) is left unchecked.
  if (e->underlying || e->isClass || !e->isDefinition || t != types.enumType(e))
    return std::nullopt;
  // Its enumerators are being evaluated on this thread (`B = A | 4` converts to the enumeration
  // while its range is unknown): no range yet, and none kept.
  for (const EnumeratorValues* f = enumeratorValues; f; f = f->outer)
    if (f->e == e)
      return std::nullopt;
  int64_t low = 0, high = 0;
  TypeContext ctx{e->ownScope ? e->ownScope : lexicalScopeOf(e), nullptr, nullptr, e->file ? e->file->tokens : nullptr,
                  e->file ? e->file->text : std::string_view()};
  for (Decl* m : e->enumerators)
  {
    if (m->kind != NodeKind::EnumeratorDecl)
      continue;
    std::string* outer = &constWhy();
    std::string saved = *outer;
    std::optional<ConstValue> v = constValueOfDecl(m, ctx, 1);
    *outer = saved; // (an enumerator that does not evaluate is the enumeration's error, not this conversion's)
    if (!v || v->kind != ConstValue::Kind::Int)
      return std::nullopt;
    low = std::min(low, v->i), high = std::max(high, v->i);
  }
  // The smallest bit field that holds them all: [0, 2^M - 1] for no negative one, else the
  // two's complement range of M bits; at least one bit (an empty enumeration holds 0 and 1).
  auto bitsFor = [](int64_t x) { int b = 0; while (b < 63 && (x >> b) != 0) ++b; return b; };
  std::pair<int64_t, int64_t> range;
  if (low >= 0)
  {
    int m = std::max(1, bitsFor(high));
    range = {0, m >= 63 ? INT64_MAX : (int64_t(1) << m) - 1};
  }
  else
  {
    int m = std::max(bitsFor(high) + 1, bitsFor(~low) + 1);
    range = m >= 64 ? std::pair<int64_t, int64_t>{INT64_MIN, INT64_MAX} : std::pair<int64_t, int64_t>{-(int64_t(1) << (m - 1)), (int64_t(1) << (m - 1)) - 1};
  }
  std::lock_guard lock(enumRangeMutex);
  enumRanges.emplace(t, range);
  return range;
}

auto GlobalScope::Impl::declaredUnderlying(const Ty* t) -> const Ty*
{
  {
    std::lock_guard lock(underlyingMutex);
    if (auto it = underlyingTypes.find(t); it != underlyingTypes.end())
      return it->second;
  }
  Decl* def = t ? t->decl : nullptr;
  if (def && !def->ownScope)
  {
    Entry e{def, nullptr};
    if (Scope* s = scopeOfEntry(e, 0))
      def = s->owner;
  }
  const Ty* u = nullptr;
  if (def && def->kind == NodeKind::EnumDecl && static_cast<EnumDecl*>(def)->underlying)
  {
    DeclContext dc;
    declContextFor(def, dc);
    TypeContext ctx{dc.scope, &dc.params, nullptr, def->file ? def->file->tokens : nullptr, def->file ? def->file->text : std::string_view()};
    u = convertType(static_cast<EnumDecl*>(def)->underlying, ctx, 1);
    u = u ? types.unqualified(u) : nullptr;
    if (!u || !(u->kind == TyKind::Int || u->kind == TyKind::Char || u->kind == TyKind::Bool))
      return types.unknown(); // not kept: a later conversion may succeed
  }
  std::lock_guard lock(underlyingMutex);
  underlyingTypes.emplace(t, u);
  return u;
}

auto GlobalScope::Impl::constEval(Expr* e, const TypeContext& ctx, int depth) -> std::optional<int64_t>
{
  std::optional<ConstValue> v = constValue(e, ctx, depth);
  if (!v || v->kind != ConstValue::Kind::Int)
    return std::nullopt; // a floating value, an object, a pointer: no integer constant (a bound, a template argument)
  return v->i;
}

auto GlobalScope::Impl::constCondition(Expr* e, const TypeContext& ctx, int depth) -> std::optional<bool>
{
  std::optional<ConstValue> v = constValue(e, ctx, depth);
  return v ? constTruth(*v, depth) : std::nullopt;
}

auto GlobalScope::Impl::constConverted(std::optional<ConstValue> v, const Ty* t) -> std::optional<ConstValue>
{
  if (!v || !t)
    return std::nullopt;
  t = types.unqualified(types.stripRef(t));
  if (t->isArithmetic() || t->kind == TyKind::Enum)
    return v->isScalar() || (v->isPointer() && t->kind == TyKind::Bool) ? constArith().convert(*v, t) : std::nullopt;
  if (t->kind == TyKind::Pointer || t->kind == TyKind::Nullptr)
    return constPointerTo(*v, t);
  if (t->kind == TyKind::Class && v->isObject() && v->type && types.unqualified(types.stripRef(v->type)) == t)
    return v;
  return std::nullopt;
}

auto GlobalScope::Impl::constPointerTo(const ConstValue& v, const Ty* t) -> std::optional<ConstValue>
{
  if (v.kind == ConstValue::Kind::Int && v.i == 0)
    return v;
  if (t->kind != TyKind::Pointer || !t->inner)
    return std::nullopt;
  const Ty* want = types.unqualified(t->inner);
  const Ty* vt = v.type ? types.unqualified(types.stripRef(v.type)) : nullptr;
  if (v.isPointer() && vt && vt->kind == TyKind::Pointer && vt->inner && types.unqualified(vt->inner) == want)
    return ConstValue::pointer(t, v.parts, v.i);
  if (v.isAddress() && vt && vt->kind == TyKind::Pointer && vt->inner)
  {
    // `const Point* p = &origin;`, `void* p = &x;`, `Base* b = &derived;` (the base's part)
    const Ty* have = types.unqualified(vt->inner);
    if (have == want || want->kind == TyKind::Void)
      return ConstValue::address(t, *v.target);
    if (std::optional<std::vector<size_t>> base = have->kind == TyKind::Class && want->kind == TyKind::Class ? constBasePath(have, want, 0) : std::nullopt)
    {
      ConstTarget part = *v.target;
      part.path.insert(part.path.end(), base->begin(), base->end());
      return ConstValue::address(t, std::move(part));
    }
    if (v.target->storage)
    {
      // `(T*)p` of a void* or another pointer to the evaluation's own memory: to what is there,
      // when it is a T; malloc's bytes become Ts, also bytes typed as chars no one wrote yet
      // (`(E**)this->slots`).
      if (const Ty* there = constPointedType(v, 0); there && types.unqualified(there) == want)
        return ConstValue::address(t, *v.target);
      if (std::optional<ConstValue> typed = constTypeAllocation(v, want, 0, have->kind != TyKind::Void))
        return ConstValue::address(t, *typed->target);
      // `static_cast<RbNode<E>*>(base)`: down to the object whose base part it points at.
      if (have->kind == TyKind::Class && want->kind == TyKind::Class)
        if (std::optional<std::vector<size_t>> base = constBasePath(want, have, 0); base && !base->empty() && v.target->path.size() >= base->size() &&
                                                                                     std::equal(base->begin(), base->end(), v.target->path.end() - base->size()))
        {
          ConstTarget whole = *v.target;
          whole.path.resize(whole.path.size() - base->size());
          if (const Ty* there = constPointedType(ConstValue::address(t, whole), 0); there && types.unqualified(there) == want)
            return ConstValue::address(t, std::move(whole));
        }
    }
    return std::nullopt;
  }
  if (v.isObject() && vt && vt->kind == TyKind::Array && vt->inner && types.unqualified(vt->inner) == want)
    return ConstValue::pointer(t, v.parts, 0);
  return std::nullopt;
}

auto GlobalScope::Impl::constAsDeclared(std::optional<ConstValue> v, const Ty* t) -> std::optional<ConstValue>
{
  if (!v || !t)
    return v;
  const Ty* u = types.unqualified(types.stripRef(t));
  switch (u->kind)
  {
    case TyKind::Class:
    case TyKind::Lambda: // (a closure object, its captures its parts: constClosure)
      return v->isObject() && v->type && types.unqualified(types.stripRef(v->type)) == u ? v : std::nullopt;
    case TyKind::Array:
    {
      // An array of the element type, of the declared length (none written: any).
      const Ty* vt = v->isObject() && v->type ? types.unqualified(v->type) : nullptr;
      if (!vt || vt->kind != TyKind::Array || types.unqualified(vt->inner) != types.unqualified(u->inner) || (u->count && u->count != vt->count))
        return std::nullopt;
      return v;
    }
    case TyKind::MemberPointer:
      return v->isMemberPointer() ? std::optional<ConstValue>(ConstValue::address(u, *v->target, ConstValue::Kind::MemberPointer)) : std::nullopt;
    case TyKind::Function: case TyKind::Void: case TyKind::Vector:
      return std::nullopt;
    case TyKind::Pointer: case TyKind::Nullptr:
      return constPointerTo(*v, u);
    default:
      if (u->isArithmetic() || u->kind == TyKind::Enum)
        return v->isScalar() ? constArith().convert(*v, u) : std::nullopt;
      return v;
  }
}

auto GlobalScope::Impl::constInitValue(Expr* init, const TypeContext& ctx, int depth) -> std::optional<ConstValue>
{
  if (init && init->kind == NodeKind::BraceInitExpr)
  {
    auto* b = static_cast<BraceInitExpr*>(init);
    if (b->args.empty())
      return ConstValue::integer(0);
    return b->args.size() == 1 ? constValue(b->args[0], ctx, depth) : std::nullopt;
  }
  return constValue(init, ctx, depth);
}

auto GlobalScope::Impl::constObjectType(Expr* object, const TypeContext& ctx, bool arrow) -> const Ty*
{
  const Ty* t = object->ty && object->ty->isKnown() && !types.isDependent(object->ty) && !mentionsUnknown(object->ty)
              ? object->ty : typeOfSignatureExpression(object, ctx);
  t = t ? types.unqualified(types.stripRef(t)) : nullptr;
  if (t && arrow)
    t = t->kind == TyKind::Pointer && t->inner ? types.unqualified(t->inner) : nullptr;
  return t && t->kind == TyKind::Class && !types.isDependent(t) && !mentionsUnknown(t) ? t : nullptr;
}

auto GlobalScope::Impl::constSteps() -> uint64_t&
{
  static thread_local uint64_t steps = 0;
  return steps;
}

auto GlobalScope::Impl::constCalls() -> int&
{
  static thread_local int calls = 0;
  return calls;
}

auto GlobalScope::Impl::constActive() -> int&
{
  static thread_local int active = 0;
  return active;
}

auto GlobalScope::Impl::constValue(Expr* e, const TypeContext& ctx, int depth) -> std::optional<ConstValue>
{
  return constOutermost([&] { return constValueIn(e, ctx, depth); }, depth);
}

auto GlobalScope::Impl::constEvaluating() -> int&
{
  static thread_local int evaluating = 0;
  return evaluating;
}

auto GlobalScope::Impl::constGlobalValues() -> std::map<std::pair<Decl*, std::vector<const Ty*>>, ConstValue>&
{
  static thread_local std::map<std::pair<Decl*, std::vector<const Ty*>>, ConstValue> values;
  return values;
}

auto GlobalScope::Impl::constNamedReturns() -> std::unordered_map<const Stmt*, VariableDecl*>&
{
  static thread_local std::unordered_map<const Stmt*, VariableDecl*> candidates;
  return candidates;
}

auto GlobalScope::Impl::constValueIn(Expr* e, const TypeContext& ctx, int depth) -> std::optional<ConstValue>
{
  if (!e || depth > 512 || (depth > 64 && !stackRoomLeft()))
    return std::nullopt;
  struct Active
  {
    Active()
    {
      if (constActive()++ == 0)
        constSteps() = 0;
    }
    ~Active() { --constActive(); }
  } active;
  if (++constSteps() > 1000000)
  {
    if (constWhy().empty())
      constWhy() = "an evaluation of more than a million steps";
    return std::nullopt;
  }
  // An array of the run's own (a local, a part of one) where a value is wanted: the address of its
  // first element there (array-to-pointer conversion), as `&a[0]` is: also one of the object a
  // constructor builds, or a member function runs on (where that is a copy, the address dangles
  // once the call ends). One the run holds a copy of through a reference it could not bind is
  // still a pointer into that value.
  if (const Ty* at = e->ty ? types.unqualified(types.stripRef(e->ty)) : nullptr;
      at && at->kind == TyKind::Array && at->inner && !constFrames().empty() &&
      (e->kind == NodeKind::NameExpr || e->kind == NodeKind::MemberExpr || e->kind == NodeKind::IndexExpr) && constPure(e))
    if (std::optional<ConstPlace> place = constPlaceOf(e, ctx, depth + 1))
      if (ConstPlace whole = constResolve(*place); constAddressable(whole))
      {
        const Ty* element = types.unqualified(types.stripRef(place->type ? place->type : at));
        if (element->kind == TyKind::Array && element->inner)
        {
          ConstTarget first = constStorageTarget(whole);
          first.path.push_back(0);
          return ConstValue::address(types.pointer(element->inner), std::move(first));
        }
      }
  switch (e->kind)
  {
    case NodeKind::NameExpr: return constName(static_cast<NameExpr*>(e), ctx);
    case NodeKind::MemberExpr:
    {
      // `Enum::Value` is parsed as a name; `object.member` is a constant when the member is an
      // enumerator or a static constexpr variable (a trait object's value), read for the
      // object's class instance.
      auto* m = static_cast<MemberExpr*>(e);
      if (m->dynamicMember || m->member.parts.empty())
        return std::nullopt;
      if (m->member.parts.size() != 1)
        return m->base && m->resolved && m->resolved->kind == NodeKind::VariableDecl && !(m->resolved->flags & (DF_Static | DF_Typedef))
                 ? constMemberRead(m, ctx, depth) // `b.A::v`: the data member of the base the qualifier names
                 : std::nullopt;
      Decl* d = m->resolved;
      if (d && d->kind == NodeKind::VariableDecl && !(d->flags & DF_Static))
        return constMemberRead(m, ctx, depth); // `point.x`, `this->count`
      if (!d && m->base)
        if (std::optional<ConstValue> field = constMemberRead(m, ctx, depth))
          return field; // in a body no walk has typed yet (constFieldNamed)
      auto isConstant = [](Decl* x) {
        return x && ((x->kind == NodeKind::VariableDecl && (x->flags & DF_Static)) || x->kind == NodeKind::EnumeratorDecl);
      };
      if (d && !isConstant(d))
        return std::nullopt;
      Scope* foundIn = d && d->parent ? d->parent->ownScope : nullptr;
      const Ty* object = m->base && (!d || d->kind == NodeKind::VariableDecl) ? constObjectType(m->base, ctx, m->op == P::Arrow) : nullptr;
      if (!d && object)
        if (Scope* s = classScopeOf(object))
        {
          Lookup l;
          if (lookupInScope(s, m->member.parts[0].symbol, l, depth + 1))
          {
            d = l.decl;
            foundIn = l.foundIn;
          }
        }
      if (!isConstant(d))
        return std::nullopt;
      if (d->kind == NodeKind::VariableDecl && object)
        if (Subst s = memberSubst(object, foundIn, depth + 1); !s.pairs.empty())
          return memberConstant(d, ctx, s);
      return constValueOfDecl(d, ctx, depth + 1);
    }
    case NodeKind::CallExpr: return constCall(static_cast<CallExpr*>(e), ctx, depth);
    case NodeKind::NewExpr: return constNew(static_cast<NewExpr*>(e), ctx, depth);
    case NodeKind::DeleteExpr: return constDelete(static_cast<DeleteExpr*>(e), ctx, depth);
    case NodeKind::ThisExpr:
    {
      // `this` as a pointer: the address of the object the function runs on.
      std::optional<ConstPlace> self = constThisPlace(ctx);
      if (!self || !self->type || !constAddressable(*self))
        return std::nullopt;
      return ConstValue::address(types.pointer(self->type), constStorageTarget(*self));
    }
    case NodeKind::LiteralExpr:
    {
      auto* l = static_cast<LiteralExpr*>(e);
      if (l->literalKind == TokenKind::Keyword)
      {
        if (l->keyword == Kw::true_ || l->keyword == Kw::false_)
          return ConstValue::integer(l->keyword == Kw::true_, types.boolType());
        if (l->keyword == Kw::nullptr_) // the null pointer constant (`fold = nullptr` as a template argument)
          return ConstValue::integer(0, types.nullptrType());
        return std::nullopt;
      }
      if (l->literalKind == TokenKind::String)
        return constString(l, ctx); // an array of chars
      // Spelled by the tokens of the file it was parsed from: the context's may be another file's
      // (a template argument written in one file, typed in another's walk).
      const std::vector<Token>* toks = l->file && l->file->tokens ? l->file->tokens : ctx.tokens ? ctx.tokens : fallbackTokens;
      std::string_view text = l->file && l->file->tokens ? l->file->text : ctx.tokens ? ctx.text : fallbackText;
      if ((l->literalKind != TokenKind::Number && l->literalKind != TokenKind::Char) || !toks || l->token >= toks->size())
        return std::nullopt;
      const Token& t = (*toks)[l->token];
      if (t.getKind() != l->literalKind)
      {
        if (constWhy().empty())
          constWhy() = "a literal read with another file's tokens (a compiler bug)"; // never a wrong value
        return std::nullopt;
      }
      std::string spliced;
      return constArith().literal(l->literalKind, tokenSpelling(text, t, spliced));
    }
    case NodeKind::ParenExpr: return constValue(static_cast<ParenExpr*>(e)->inner, ctx, depth + 1);
    case NodeKind::CastExpr: return constCastValue(static_cast<CastExpr*>(e)->type, static_cast<CastExpr*>(e)->operand, ctx, depth);
    case NodeKind::NamedCastExpr: return constCastValue(static_cast<NamedCastExpr*>(e)->type, static_cast<NamedCastExpr*>(e)->operand, ctx, depth);
    case NodeKind::FunctionalCastExpr:
    {
      auto* f = static_cast<FunctionalCastExpr*>(e);
      bool noValue = f->args.empty() || (f->args.size() == 1 && f->args[0] && f->args[0]->kind == NodeKind::BraceInitExpr &&
                                         static_cast<BraceInitExpr*>(f->args[0])->args.empty());
      const Ty* t = f->type ? convertType(f->type, ctx, depth + 1) : nullptr;
      // `Pair { "mask", X }`, `Array { ... }`: a class template named without its arguments, the
      // ones the walk deduced (class template argument deduction), unless they depend on an
      // instance's.
      if ((!t || !t->isKnown() || isUndeducedClassTemplate(types.unqualified(t))) && f->ty)
        if (const Ty* deduced = types.unqualified(types.stripRef(f->ty));
            deduced->kind == TyKind::Class && deduced->isKnown() && !types.isDependent(deduced) && !mentionsUnknown(deduced))
          t = deduced;
      if (std::optional<std::optional<ConstValue>> made = constClassCast(f, t, ctx, depth))
        return *made;
      if (noValue)
        return constConverted(ConstValue::integer(0), t); // `int()`, `T{}`: zero
      Expr* arg = f->args.size() == 1 ? f->args[0] : nullptr;
      if (arg && arg->kind == NodeKind::BraceInitExpr && static_cast<BraceInitExpr*>(arg)->args.size() == 1)
        arg = static_cast<BraceInitExpr*>(arg)->args[0]; // `int{x}`
      return arg ? constConverted(constValue(arg, ctx, depth + 1), t) : std::nullopt;
    }
    case NodeKind::RequiresExpr:
      if (ctx.variableTemplate && ctx.subst)
      {
        // In `hasBegin<Vector<int>>`'s initializer: the requirements for these arguments.
        std::optional<bool> b = requirementsHold(static_cast<RequiresExpr*>(e), ctx.variableTemplate, *ctx.subst);
        return b ? std::optional<ConstValue>(ConstValue::integer(*b, types.boolType())) : std::nullopt;
      }
      return ConstValue::integer(static_cast<RequiresExpr*>(e)->satisfied, types.boolType()); // set where the expression was typed
    case NodeKind::ConditionalExpr:
    {
      // `isSigned<T> ? count - 1 : count`: only the branch taken has to be a constant. Its value
      // takes the type of the whole expression when the walk typed both branches with types
      // that hold for every instance (`flag ? -1 : 0u` is unsigned).
      auto* c = static_cast<ConditionalExpr*>(e);
      std::optional<ConstValue> cond = constValue(c->cond, ctx, depth + 1);
      std::optional<bool> holds = cond ? constTruth(*cond, depth + 1) : std::nullopt;
      if (!holds)
        return std::nullopt;
      if (!c->then)
        return *holds ? cond : constValue(c->else_, ctx, depth + 1); // `a ?: b`
      std::optional<ConstValue> v = constValue(*holds ? c->then : c->else_, ctx, depth + 1);
      auto scalar = [this](const Ty* t) {
        t = t ? types.unqualified(types.stripRef(t)) : nullptr;
        return t && (t->isArithmetic() || t->kind == TyKind::Enum) && !types.isDependent(t) ? t : nullptr;
      };
      if (const Ty* t = scalar(c->ty); v && t && scalar(c->then->ty) && c->else_ && scalar(c->else_->ty))
        return constArith().convert(*v, t);
      return v;
    }
    case NodeKind::UnaryExpr:
    {
      auto* u = static_cast<UnaryExpr*>(e);
      std::optional<P> op = constOperator(u->op, u->isKeyword);
      if (op && u->operand && u->operand->ty && types.unqualified(types.stripRef(u->operand->ty))->kind == TyKind::Enum)
        if (std::optional<std::optional<ConstValue>> called = constEnumUnary(u, *op, ctx, depth))
          return *called;
      if (op && (*op == P::PlusPlus || *op == P::MinusMinus))
        return constIncrement(u, *op == P::PlusPlus, ctx, depth);
      if (op && *op == P::Star && !u->postfix)
      {
        if (u->operand && u->operand->kind == NodeKind::ThisExpr)
        {
          std::optional<ConstPlace> self = constThisPlace(ctx); // `*this` (a reference to the caller's object, in a place call)
          return self ? constRead(*self) : std::nullopt;
        }
        std::optional<ConstPlace> object = constPure(u->operand) ? constPlaceOf(u->operand, ctx, depth + 1) : std::nullopt;
        std::optional<ConstValue> p = object ? constRead(*object) : constValue(u->operand, ctx, depth + 1);
        if (p && p->isObject() && p->type && types.unqualified(p->type)->kind == TyKind::Class)
        {
          // `*optional`: the class's operator*, run on the object (where it is, when it has a place).
          Subst subst;
          FunctionDecl* f = constClassOperator(p->type, P::Star, {}, subst, depth);
          std::optional<ConstValue> self = p;
          return f && constDefinition(f) ? constCallOn(f, subst, {}, &self, depth + 1, object ? &*object : nullptr) : std::nullopt;
        }
        if (p && p->isAddress())
          return p->target->decl && p->target->decl->kind == NodeKind::FunctionDecl ? p : constReadTarget(*p, depth + 1); // `(*f)(5)`: the function
        return p ? constDeref(*p) : std::nullopt;
      }
      if (op && *op == P::Amp && !u->postfix)
        return constAddressOf(u->operand, ctx, depth + 1); // of a global, a function, a member; no local's
      if (!op || u->postfix || (*op != P::Bang && *op != P::Minus && *op != P::Plus && *op != P::Tilde))
        return std::nullopt;
      std::optional<ConstValue> v = constValue(u->operand, ctx, depth + 1);
      if (v && v->isObject())
      {
        if (*op == P::Bang)
        {
          std::optional<bool> t = constTruth(*v, depth + 1); // `!optional`
          return t ? std::optional<ConstValue>(ConstValue::integer(!*t, types.boolType())) : std::nullopt;
        }
        Subst subst;
        FunctionDecl* f = v->type ? constClassOperator(v->type, *op, {}, subst, depth) : nullptr; // `-vector`
        std::optional<ConstValue> self = v;
        return f && constDefinition(f) ? constCallOn(f, subst, {}, &self, depth + 1) : std::nullopt;
      }
      return v ? constArith().unary(*op, *v) : std::nullopt;
    }
    case NodeKind::BinaryExpr:
    {
      // `1 + 1 + ... + 0` (InputAction::inputActionCount, a term per action): the chain down the
      // left operands is evaluated in a loop, not nested once per term.
      if (auto* b = static_cast<BinaryExpr*>(e); !b->isKeyword && isAssignmentOperator(P(b->op)))
        return constAssign(b, ctx, depth);
      if (auto* b = static_cast<BinaryExpr*>(e); !b->isKeyword && (P(b->op) == P::DotStar || P(b->op) == P::ArrowStar))
        return constMemberPointerRead(b, ctx, depth);
      return constBinaryChain(static_cast<BinaryExpr*>(e), ctx, depth);
    }
    case NodeKind::ThrowExpr:
      constWhy() = constThrown(static_cast<ThrowExpr*>(e), ctx, depth); // (no constant: C++ rejects a throw in one)
      return std::nullopt;
    case NodeKind::LambdaExpr:
      return constClosure(static_cast<LambdaExpr*>(e), e->ty, ctx, depth); // `auto twice = [k](int v) { ... };`
    case NodeKind::NoexceptExpr:
      // The walk's answer (canThrow); its operand is not evaluated.
      if (static_cast<NoexceptExpr*>(e)->value < 0)
        return std::nullopt;
      return ConstValue::integer(static_cast<NoexceptExpr*>(e)->value, types.boolType());
    case NodeKind::SizeofExpr:
    {
      auto* s = static_cast<SizeofExpr*>(e);
      if (s->isPack)
      {
        // `sizeof...(Renderers)` (a template argument: StaticVector<T, sizeof...(Renderers)>):
        // the number of elements the pack, a type pack or a function parameter pack, is bound to.
        const QualifiedName* name = s->type && s->type->kind == NodeKind::NamedType ? &static_cast<NamedType*>(s->type)->name
                                  : s->operand && s->operand->kind == NodeKind::NameExpr ? &static_cast<NameExpr*>(s->operand)->name : nullptr;
        if (!name || name->parts.size() != 1 || !ctx.subst)
          return std::nullopt;
        Lookup l = lookupQualified(*name, ctx.scope, ctx.locals, depth + 1, ctx.subst);
        Decl* d = l.decl;
        if (!d) // `sizeof...(args)` where the context has no locals (the code generator's): what the walk found
          d = s->type && s->type->kind == NodeKind::NamedType ? static_cast<NamedType*>(s->type)->resolved
            : s->operand && s->operand->kind == NodeKind::NameExpr ? static_cast<NameExpr*>(s->operand)->resolved : nullptr;
        const Ty* pack = nullptr;
        if (d && d->kind == NodeKind::TemplateParamDecl)
          pack = ctx.subst->find(d);
        else if (d && d->kind == NodeKind::ParamDecl && static_cast<ParamDecl*>(d)->isPack)
          pack = declaredTypeOf(d, ctx.subst, depth + 1);
        if (!pack || pack->kind != TyKind::Pack)
          return std::nullopt;
        return ConstValue::integer(int64_t(pack->args.size()), types.sizeType());
      }
      const Ty* t = nullptr;
      // `sizeof(grid)`: the parser reads a lone name as a type; a variable's is its declared one, and
      // an enumerator's its enumeration (`sizeof(E::X)`: its underlying type's size, an unscoped
      // enumeration's int's).
      Decl* named = s->type && s->type->kind == NodeKind::NamedType ? static_cast<NamedType*>(s->type)->resolved : nullptr;
      // `sizeof(copy)` in a class template's member, copy one of several `when` data members of the name (design.md 19):
      // the instance's own, whose condition holds under its arguments (none or two: no constant).
      if (named && named->kind == NodeKind::VariableDecl && named->when && named->whenKnown < 0 && ctx.subst && named->parent &&
          named->parent->kind == NodeKind::ClassDecl)
      {
        bool bound = true;
        for (Decl* p : effectiveParams(named->parent))
          bound = bound && ctx.subst->find(p);
        if (bound)
        {
          int holding = 0;
          bool undecided = false;
          named = whenMemberOf(named, *ctx.subst, holding, undecided, depth);
          if (!named)
            return std::nullopt;
        }
      }
      if (named && (((named->kind == NodeKind::VariableDecl || named->kind == NodeKind::ParamDecl) && !(named->flags & DF_Typedef)) ||
                    named->kind == NodeKind::EnumeratorDecl))
        t = declaredTypeOf(named, ctx.subst, depth + 1);
      else if (s->type)
        t = convertType(s->type, ctx, depth + 1);
      Expr* operand = s->operand;
      while (operand && operand->kind == NodeKind::ParenExpr)
        operand = static_cast<ParenExpr*>(operand)->inner;
      if (!t && operand && operand->kind == NodeKind::LiteralExpr && static_cast<LiteralExpr*>(operand)->literalKind == TokenKind::String &&
          s->sizeofKind != Kw::alignof_)
      {
        // `sizeof("\033Lua")`: the literal is an array of its characters and the terminating null
        // (typed as a pointer to them elsewhere), so the array's size, not a pointer's.
        std::optional<ConstValue> chars = constString(static_cast<LiteralExpr*>(operand), ctx);
        const Ty* element = operand->ty ? types.stripRef(operand->ty) : nullptr;
        element = element && (element->kind == TyKind::Pointer || element->kind == TyKind::Array) ? element->inner : nullptr;
        std::optional<int64_t> each = element ? constSizeOf(types.unqualified(element), depth + 1) : std::optional<int64_t>(1);
        if (chars && chars->parts && each)
          return ConstValue::integer(int64_t(chars->parts->size()) * *each, types.sizeType());
      }
      if (!t && s->operand && s->operand->ty)
        t = s->operand->ty;
      if (!t || !t->isKnown() || types.isDependent(t))
        return std::nullopt;
      std::optional<int64_t> n = s->sizeofKind == Kw::alignof_ ? constAlignOf(types.stripRef(t), depth + 1) : constSizeOf(types.stripRef(t), depth + 1);
      return n ? std::optional<ConstValue>(ConstValue::integer(*n, types.sizeType())) : std::nullopt;
    }
    case NodeKind::FoldExpr: return constFold(static_cast<FoldExpr*>(e), ctx, depth);
    case NodeKind::IndexExpr: return constIndex(static_cast<IndexExpr*>(e), ctx, depth);
    case NodeKind::BraceInitExpr:
      if (static_cast<BraceInitExpr*>(e)->text)
      {
        // A call's arguments packed for its Text parameter (design.md 14).
        std::vector<ConstValue> values;
        std::vector<std::optional<ConstPlace>> places;
        if (!constArguments(static_cast<BraceInitExpr*>(e)->args, ctx, depth, values, &places))
          return std::nullopt;
        return constTextOf(values, &places, 0, depth);
      }
      return constBraceList(static_cast<BraceInitExpr*>(e), ctx, depth); // its type decides later
    default:
      return std::nullopt;
  }
}

auto GlobalScope::Impl::packNamedIn(Node* n, const Subst& s, int depth) -> Decl*
{
  if (!n || depth > 32)
    return nullptr;
  auto named = [&](const QualifiedName& name) -> Decl* {
    if (name.parts.size() == 1 && !name.global)
      for (const Subst::Pair& p : s.pairs)
        if (p.param && p.param->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(p.param)->isPack && p.type &&
            p.type->kind == TyKind::Pack && static_cast<TemplateParamDecl*>(p.param)->name == name.parts[0].symbol)
          return p.param;
    for (const NamePart& part : name.parts)
      for (Node* a : part.templateArgs)
        if (Decl* d = packNamedIn(a, s, depth + 1))
          return d;
    return nullptr;
  };
  auto first = [&](std::initializer_list<Node*> nodes) -> Decl* {
    for (Node* x : nodes)
      if (Decl* d = packNamedIn(x, s, depth + 1))
        return d;
    return nullptr;
  };
  // `values` of `Ts&&... values` (`(sizeof(values) + ...)`): the pack its type expands.
  auto parameterPack = [&](Decl* d) -> Decl* {
    return d && d->kind == NodeKind::ParamDecl && static_cast<ParamDecl*>(d)->isPack ? packNamedIn(static_cast<ParamDecl*>(d)->type, s, depth + 1) : nullptr;
  };
  switch (n->kind)
  {
    case NodeKind::NamedType:
      if (Decl* d = named(static_cast<NamedType*>(n)->name))
        return d;
      return parameterPack(static_cast<NamedType*>(n)->resolved);
    case NodeKind::NameExpr:
      if (Decl* d = named(static_cast<NameExpr*>(n)->name))
        return d;
      return parameterPack(static_cast<NameExpr*>(n)->resolved);
    case NodeKind::PointerType: return first({static_cast<PointerType*>(n)->pointee});
    case NodeKind::LRefType: return first({static_cast<LRefType*>(n)->referenced});
    case NodeKind::RRefType: return first({static_cast<RRefType*>(n)->referenced});
    case NodeKind::ConstType: return first({static_cast<ConstType*>(n)->inner});
    case NodeKind::ParenExpr: return first({static_cast<ParenExpr*>(n)->inner});
    case NodeKind::UnaryExpr: return first({static_cast<UnaryExpr*>(n)->operand});
    case NodeKind::MemberExpr: return first({static_cast<MemberExpr*>(n)->base});
    case NodeKind::BinaryExpr: return first({static_cast<BinaryExpr*>(n)->lhs, static_cast<BinaryExpr*>(n)->rhs});
    case NodeKind::ConditionalExpr:
      return first({static_cast<ConditionalExpr*>(n)->cond, static_cast<ConditionalExpr*>(n)->then, static_cast<ConditionalExpr*>(n)->else_});
    case NodeKind::CastExpr: return first({static_cast<CastExpr*>(n)->type, static_cast<CastExpr*>(n)->operand});
    case NodeKind::NamedCastExpr: return first({static_cast<NamedCastExpr*>(n)->type, static_cast<NamedCastExpr*>(n)->operand});
    case NodeKind::SizeofExpr:
      // `sizeof...(Ts)` counts the pack, it expands nothing.
      return static_cast<SizeofExpr*>(n)->isPack ? nullptr : first({static_cast<SizeofExpr*>(n)->type, static_cast<SizeofExpr*>(n)->operand});
    case NodeKind::FunctionalCastExpr:
    {
      auto* c = static_cast<FunctionalCastExpr*>(n);
      if (Decl* d = first({c->type}))
        return d;
      for (Expr* a : c->args)
        if (Decl* d = first({a}))
          return d;
      return nullptr;
    }
    case NodeKind::CallExpr:
    {
      auto* c = static_cast<CallExpr*>(n);
      if (Decl* d = first({c->callee}))
        return d;
      for (Expr* a : c->args)
        if (Decl* d = first({a}))
          return d;
      return nullptr;
    }
    default:
      return nullptr;
  }
}

auto GlobalScope::Impl::constFold(FoldExpr* f, const TypeContext& ctx, int depth) -> std::optional<ConstValue>
{
  std::optional<P> op = constOperator(f->op, false);
  if (!op || !ctx.subst || !isConstBinaryOperator(*op))
    return std::nullopt;
  bool right = f->lhs && packNamedIn(f->lhs, *ctx.subst);
  Expr* pattern = right ? f->lhs : f->rhs;
  Expr* init = right ? f->rhs : f->lhs;
  Decl* pack = pattern ? packNamedIn(pattern, *ctx.subst) : nullptr;
  const Ty* bound = pack ? ctx.subst->find(pack) : nullptr;
  if (!bound || bound->kind != TyKind::Pack)
    return std::nullopt;
  // The pattern's values, each with its element of the packs bound (function parameter packs'
  // values included: `(values + ...)`).
  std::optional<std::vector<ConstValue>> expanded = constExpand(pattern, ctx, depth);
  if (!expanded || expanded->size() != bound->args.size())
    return std::nullopt;
  auto element = [&](size_t i) -> std::optional<ConstValue> { return (*expanded)[i]; };
  // The operands in the order C++ evaluates them: the elements, with init last (a right fold)
  // or first (a left fold); -1 stands for init.
  std::vector<int> order;
  if (init && !right)
    order.push_back(-1);
  for (size_t i = 0; i < bound->args.size(); ++i)
    order.push_back(int(i));
  if (init && right)
    order.push_back(-1);
  auto operand = [&](int k) { return k < 0 ? constValue(init, ctx, depth + 1) : element(size_t(k)); };
  if (*op == P::AndAnd || *op == P::OrOr)
  {
    for (int k : order)
    {
      std::optional<ConstValue> v = operand(k);
      if (!v)
        return std::nullopt;
      if (v->truthy() == (*op == P::OrOr))
        return ConstValue::integer(*op == P::OrOr, types.boolType());
    }
    return ConstValue::integer(*op == P::AndAnd, types.boolType()); // an empty `&&` fold is true, `||` false
  }
  if (order.empty())
    return std::nullopt; // an empty fold of another operator is ill-formed
  std::vector<ConstValue> values;
  for (int k : order)
  {
    std::optional<ConstValue> v = operand(k);
    if (!v)
      return std::nullopt;
    values.push_back(*v);
  }
  // A right fold is E1 op (E2 op (... op En)), a left fold ((E1 op E2) op ...) op En.
  std::optional<ConstValue> value = right ? values.back() : values.front();
  if (right)
    for (size_t i = values.size() - 1; i-- > 0 && value;)
      value = *op == P::Comma ? value : constArith().binary(*op, values[i], *value);
  else
    for (size_t i = 1; i < values.size() && value; ++i)
      value = *op == P::Comma ? std::optional<ConstValue>(values[i]) : constArith().binary(*op, *value, values[i]);
  return value;
}

auto GlobalScope::Impl::constName(NameExpr* n, const TypeContext& ctx) -> std::optional<ConstValue>
{
  if (n->name.empty())
  {
    // A tuple-like binding's `get<i>(object)`: the hidden object of its declaration (no name).
    if (std::optional<ConstPlace> hidden = constLocal(n, ctx))
      return constRead(*hidden);
    return std::nullopt; // `int` read as a name (`v < int > ...`): no part to look up
  }
  if (ctx.env && n->name.parts.size() == 1 && !n->name.global)
    for (size_t i = ctx.env->size(); i-- > 0;)
      if ((*ctx.env)[i].symbol == n->name.parts[0].symbol)
      {
        if ((*ctx.env)[i].pack)
          return std::nullopt; // a parameter pack outside an expansion
        if ((*ctx.env)[i].aliasEnv)
          return constRead(ConstPlace{ctx.env, i, {}, (*ctx.env)[i].type}); // a reference: what it refers to now
        const ConstValue& v = (*ctx.env)[i].value; // a parameter or a local of the function being run
        return v.isUnset() ? std::nullopt : std::optional<ConstValue>(v);
      }
  // `count` alone in a member function: the member of the object it runs on (found by name in a
  // body no walk has typed yet, constFieldNamed).
  if (Decl* d = n->resolved; d && d->kind == NodeKind::VariableDecl && d->parent && d->parent->kind == NodeKind::ClassDecl &&
                             !(d->flags & DF_Static) && constThis(ctx))
  {
    std::optional<ConstPlace> place = constPlaceOf(n, ctx, 0);
    return place ? constRead(*place) : std::nullopt;
  }
  if (!n->resolved && constThis(ctx))
    if (std::optional<ConstPlace> place = constPlaceOf(n, ctx, 0))
      return constRead(*place);
  NameEvaluation nested; // the calls below start at depth 0 (see nameEvaluations)
  if (!nested.ok)
    return std::nullopt;
  auto boolean = [this](std::optional<bool> b) {
    return b ? std::optional<ConstValue>(ConstValue::integer(*b, types.boolType())) : std::nullopt;
  };
  Lookup l = lookupQualified(n->name, ctx.scope, ctx.locals, 1, ctx.subst);
  if (l.found == Found::No && !n->resolved && n->name.parts.size() == 1 && n->name.parts[0].hasTemplateArgs &&
      isBoolTraitName(n->name.parts[0].symbol))
  {
    // `isSame<T, int>` as a constant (an if constexpr condition under the substitution).
    std::vector<const Ty*> a = traitArgs(n->name.parts[0], ctx, 1);
    if (a.empty())
      return std::nullopt;
    return boolean(traitValue(symbols.name(n->name.parts[0].symbol), a));
  }
  // `if constexpr (has_get_entity<OwnerType>)`: a concept for these arguments, not the value its
  // requires-expression was last typed to.
  if (n->name.parts.size() == 1 && n->name.parts[0].hasTemplateArgs)
    if (VariableDecl* concept_ = conceptOf(l.decl))
      return boolean(conceptValue(concept_, n->name.parts[0], ctx, 1));
  // `hasBegin<Vector<int>>`, `bytes<T>` under an instantiation: a variable template for these
  // arguments, not its initializer as written.
  if (n->name.last().hasTemplateArgs)
    if (VariableDecl* primary = primaryVariableTemplate(l))
      return variableTemplateValue(primary, n->name.last(), ctx, 1);
  // `decltype(check<T>(nullptr))::value`: a static member of the class the decltype names (or of
  // its base), under that instance's arguments.
  if (n->name.parts.size() == 1 && n->name.typeQualifier && l.decl && l.decl->kind == NodeKind::VariableDecl && l.foundIn)
    if (const Ty* q = convertType(n->name.typeQualifier, ctx, 1); q && types.unqualified(types.stripRef(q))->kind == TyKind::Class)
      if (Subst qs = memberSubst(types.unqualified(types.stripRef(q)), l.foundIn, 1); !qs.pairs.empty())
        return memberConstant(l.decl, ctx, qs);
  // `Bits<uint64_t>::count`: a static member of a class template, under that instantiation's
  // arguments.
  if (n->name.parts.size() > 1 && l.decl && l.decl->kind == NodeKind::VariableDecl)
    if (Subst qs = qualifierSubst(n->name, ctx, 1, l.foundIn); !qs.pairs.empty())
      return memberConstant(l.decl, ctx, qs);
  // A name the context cannot see (a for constexpr variable in `int buffer[i + 1]`, a function's
  // local where the code generator asks without the function's locals): the declaration the
  // type checker bound where the name is written.
  Decl* local = n->resolved && n->resolved->kind == NodeKind::VariableDecl && n->resolved->parent &&
                n->resolved->parent->kind == NodeKind::FunctionDecl ? n->resolved : nullptr;
  if (local && l.found != Found::Local)
    return constValueOfDecl(local, ctx, 0);
  // `thrice` where a pointer is wanted (`Fn ops[] = {&twice, thrice}`): the function's address,
  // the overload the walk chose when the name has several.
  if (Decl* f = n->resolved && n->resolved->kind == NodeKind::FunctionDecl ? n->resolved : l.decl; f && f->kind == NodeKind::FunctionDecl)
  {
    int functions = 0;
    if (l.overloads && !n->resolved)
      for (const Entry& entry : *l.overloads)
        functions += entry.decl && entry.decl->kind == NodeKind::FunctionDecl;
    return functions > 1 ? std::nullopt : constFunctionAddress(static_cast<FunctionDecl*>(f), n->name, ctx, 1);
  }
  return constValueOfDecl(l.decl ? l.decl : n->resolved, ctx, 0);
}

auto GlobalScope::Impl::constIsExpansion(const Expr* e) -> bool
{
  return e && e->kind == NodeKind::UnaryExpr && !static_cast<const UnaryExpr*>(e)->isKeyword && P(static_cast<const UnaryExpr*>(e)->op) == P::Ellipsis &&
         static_cast<const UnaryExpr*>(e)->postfix;
}

auto GlobalScope::Impl::constNamesIn(Node* n, std::vector<uint32_t>& out, int depth) -> void
{
  if (!n || depth > 64)
    return;
  switch (n->kind)
  {
    case NodeKind::NameExpr:
      if (auto* x = static_cast<NameExpr*>(n); x->name.parts.size() == 1 && !x->name.global)
        out.push_back(x->name.parts[0].symbol);
      return;
    case NodeKind::ParenExpr: constNamesIn(static_cast<ParenExpr*>(n)->inner, out, depth + 1); return;
    case NodeKind::UnaryExpr: constNamesIn(static_cast<UnaryExpr*>(n)->operand, out, depth + 1); return;
    case NodeKind::BinaryExpr:
      constNamesIn(static_cast<BinaryExpr*>(n)->lhs, out, depth + 1);
      constNamesIn(static_cast<BinaryExpr*>(n)->rhs, out, depth + 1);
      return;
    case NodeKind::MemberExpr: constNamesIn(static_cast<MemberExpr*>(n)->base, out, depth + 1); return;
    case NodeKind::IndexExpr:
      constNamesIn(static_cast<IndexExpr*>(n)->base, out, depth + 1);
      for (Expr* a : static_cast<IndexExpr*>(n)->args)
        constNamesIn(a, out, depth + 1);
      return;
    case NodeKind::CallExpr:
      constNamesIn(static_cast<CallExpr*>(n)->callee, out, depth + 1);
      for (Expr* a : static_cast<CallExpr*>(n)->args)
        constNamesIn(a, out, depth + 1);
      return;
    case NodeKind::CastExpr: constNamesIn(static_cast<CastExpr*>(n)->operand, out, depth + 1); return;
    case NodeKind::NamedCastExpr: constNamesIn(static_cast<NamedCastExpr*>(n)->operand, out, depth + 1); return;
    case NodeKind::FunctionalCastExpr:
      for (Expr* a : static_cast<FunctionalCastExpr*>(n)->args)
        constNamesIn(a, out, depth + 1);
      return;
    case NodeKind::ConditionalExpr:
      constNamesIn(static_cast<ConditionalExpr*>(n)->cond, out, depth + 1);
      constNamesIn(static_cast<ConditionalExpr*>(n)->then, out, depth + 1);
      constNamesIn(static_cast<ConditionalExpr*>(n)->else_, out, depth + 1);
      return;
    default:
      return;
  }
}

auto GlobalScope::Impl::constExpand(Expr* pattern, const TypeContext& ctx, int depth, std::vector<std::optional<ConstPlace>>* places, std::vector<uint8_t>* categories) -> std::optional<std::vector<ConstValue>>
{
  if (!pattern || depth > 512)
    return std::nullopt;
  std::optional<size_t> count;
  std::vector<std::pair<Decl*, const Ty*>> typePacks;
  if (ctx.subst)
  {
    Subst probe = *ctx.subst;
    for (int guard = 0; guard < 8; ++guard)
    {
      Decl* pack = packNamedIn(pattern, probe);
      const Ty* bound = pack ? probe.find(pack) : nullptr;
      if (!bound || bound->kind != TyKind::Pack)
        break;
      if (count && *count != bound->args.size())
        return std::nullopt;
      count = bound->args.size();
      typePacks.push_back({pack, bound});
      for (Subst::Pair& q : probe.pairs)
        if (q.param == pack)
          q.type = types.unknown(); // looked at: the next one, if any
    }
  }
  std::vector<size_t> valuePacks; // the run's pack bindings the pattern names
  if (ctx.env)
  {
    std::vector<uint32_t> names;
    constNamesIn(pattern, names);
    for (uint32_t name : names)
      for (size_t i = ctx.env->size(); i-- > 0;)
        if ((*ctx.env)[i].symbol == name)
        {
          if ((*ctx.env)[i].pack && std::find(valuePacks.begin(), valuePacks.end(), i) == valuePacks.end())
          {
            size_t n = (*ctx.env)[i].value.parts ? (*ctx.env)[i].value.parts->size() : 0;
            if (count && *count != n)
              return std::nullopt;
            count = n;
            valuePacks.push_back(i);
          }
          break; // the innermost binding of the name
        }
  }
  if (!count || !constMayMake(*count))
    return std::nullopt;
  std::vector<ConstValue> out;
  for (size_t k = 0; k < *count; ++k)
  {
    Subst one = ctx.subst ? *ctx.subst : Subst{};
    for (const auto& [pack, bound] : typePacks)
      for (Subst::Pair& q : one.pairs)
        if (q.param == pack)
          q.type = bound->args[k];
    TypeContext ectx = ctx;
    ectx.subst = one.empty() ? nullptr : &one;
    size_t mark = ctx.env ? ctx.env->size() : 0;
    const Ty* elementType = nullptr;
    for (size_t i : valuePacks)
    {
      const ConstBinding& b = (*ctx.env)[i];
      ConstBinding element{b.symbol, (*b.value.parts)[k]};
      element.type = b.type && b.type->kind == TyKind::Pack && k < b.type->args.size() ? b.type->args[k] : (*b.value.parts)[k].type;
      element.writable = false;
      elementType = elementType ? elementType : element.type;
      if (k < b.packPlaces.size() && b.packPlaces[k].env)
      {
        // A reference element: the argument's object itself (a move empties it).
        const ConstPlace& at = b.packPlaces[k];
        element.value = ConstValue::unset(element.type);
        element.writable = !(element.type && element.type->inner && element.type->inner->isConst) && !at.readOnly;
        element.type = at.type;
        element.aliasEnv = at.env;
        element.aliasIndex = at.index;
        element.aliasPath = at.path;
      }
      ctx.env->push_back(std::move(element)); // the element, over the pack while the pattern runs
    }
    std::optional<ConstValue> v = constValue(pattern, ectx, depth + 1);
    if (v && (places || categories))
    {
      // `forward<Args>(args)...`: an rvalue (moved from) unless Args is an lvalue reference;
      // `move(args)...` an rvalue; `args...` an lvalue. Its place: its argument's, if any.
      Expr* source = nullptr;
      uint8_t category = constCategory(pattern, source);
      if (category == 2 && pattern->kind == NodeKind::CallExpr && elementType && elementType->kind == TyKind::LRef)
      {
        auto* call = static_cast<CallExpr*>(pattern);
        if (call->callee && call->callee->kind == NodeKind::NameExpr && !static_cast<NameExpr*>(call->callee)->name.empty() &&
            symbols.name(static_cast<NameExpr*>(call->callee)->name.last().symbol) == "forward")
          category = 1;
      }
      std::optional<ConstPlace> at;
      if (category != 0 && constPure(source))
        if (std::optional<ConstPlace> p = constPlaceOf(source, ectx, depth + 1))
          if (ConstPlace resolved = constResolve(*p); resolved.env != ctx.env || resolved.index < mark)
            at = resolved; // (an element's own binding ends with the pattern: no place)
      if (places)
        places->push_back(at);
      if (categories)
        categories->push_back(category);
    }
    if (ctx.env)
      ctx.env->resize(mark);
    if (!v)
      return std::nullopt;
    out.push_back(*v);
  }
  return out;
}

auto GlobalScope::Impl::constArguments(Span<Expr*> args, const TypeContext& ctx, int depth, std::vector<ConstValue>& values, std::vector<std::optional<ConstPlace>>* places, std::vector<uint8_t>* categories) -> bool
{
  for (Expr* a : args)
  {
    if (constIsExpansion(a))
    {
      std::vector<std::optional<ConstPlace>> elementPlaces;
      std::vector<uint8_t> elementCategories;
      std::optional<std::vector<ConstValue>> expanded = constExpand(static_cast<UnaryExpr*>(a)->operand, ctx, depth, &elementPlaces, &elementCategories);
      if (!expanded)
        return false;
      for (size_t k = 0; k < expanded->size(); ++k)
      {
        values.push_back((*expanded)[k]);
        if (places)
          places->push_back(k < elementPlaces.size() ? elementPlaces[k] : std::nullopt);
        if (categories)
          categories->push_back(k < elementCategories.size() ? elementCategories[k] : 0);
      }
      continue;
    }
    // `fill(v.emplace_back())`, `Ref(max(a, b))`: a function's reference result run once for its
    // place, which a reference parameter then refers to (the new element, the object a const
    // reference result refers to: not a copy of it, which a reference member kept stale).
    std::optional<ConstPlace> called;
    std::optional<ConstValue> calledValue;
    Expr* bare = a;
    while (bare && bare->kind == NodeKind::ParenExpr)
      bare = static_cast<ParenExpr*>(bare)->inner;
    if (places && bare && bare->kind == NodeKind::CallExpr && bare->ty && bare->ty->isReference() && bare->ty->inner && static_cast<CallExpr*>(bare)->callee &&
        (static_cast<CallExpr*>(bare)->callee->kind == NodeKind::MemberExpr || static_cast<CallExpr*>(bare)->callee->kind == NodeKind::NameExpr))
    {
      constCallPlaceRan = false;
      called = constPlaceOf(bare, ctx, depth + 1, true, &calledValue);
      if (!called && !calledValue && constCallPlaceRan)
        return false; // it ran, and its result is no place the run holds: never run twice
    }
    std::optional<ConstValue> v = called ? constPartOrUnset(*called) : calledValue ? calledValue : constValue(a, ctx, depth + 1);
    if (!v && places && constPure(a))
      if (std::optional<ConstPlace> at = constPlaceOf(a, ctx, depth + 1); at && !constRead(*at))
        v = ConstValue::unset(at->type); // `appendSlot(grown)` of `T* grown;`: a reference to it reads nothing
    if (!v)
      return false;
    values.push_back(*v);
    if (places)
      places->push_back(called ? called : constPlaceOf(a, ctx, depth + 1));
    if (categories)
    {
      Expr* source = nullptr;
      categories->push_back(constCategory(a, source));
      if (places && source != a && categories->back() == 2)
        places->back() = constPlaceOf(source, ctx, depth + 1); // `move(x)`: x
    }
  }
  return true;
}

auto GlobalScope::Impl::constBindTemplate(FunctionDecl* f, const std::vector<const Ty*>& given, CallExpr* c, const std::vector<ConstValue>& values, const TypeContext& ctx, int depth, Subst& bound, LocalStack& params,
                                          const std::vector<uint8_t>* categories) -> bool
{
  bool variadic = !f->params.empty() && static_cast<ParamDecl*>(f->params[f->params.size() - 1])->isPack;
  Subst fromArgs; // `max(sizeof(Key) + 1u, 2 * sizeof(void*) - 1)`: the parameters not written, deduced from the arguments' types
  if (given.size() < f->templateParams.size())
  {
    LocalStack own;
    for (Decl* p : f->templateParams)
      own.push(static_cast<TemplateParamDecl*>(p)->name, p);
    TypeContext pctx{lexicalScopeOf(f), &own, nullptr, f->file ? f->file->tokens : nullptr, f->file ? f->file->text : std::string_view()};
    // The arguments' types: as the walk types an argument written alone, else (`Math::isNaN(d)`
    // in a function being run, d a binding of the run; an expanded pack's elements) the value's.
    std::vector<const Ty*> argTypes;
    bool expansions = false;
    for (Expr* a : c->args)
      expansions = expansions || constIsExpansion(a);
    for (size_t i = 0; i < values.size(); ++i)
    {
      Expr* a = !expansions && i < c->args.size() ? c->args[i] : nullptr;
      const Ty* argTy = a ? typeOfSignatureExpression(a, ctx) : nullptr;
      if ((!argTy || !argTy->isKnown() || types.isDependent(argTy)) && values[i].type && !values[i].type->isBraceList())
        argTy = values[i].type;
      // A lambda written as the argument: its closure's type, as the walk typed it (typing it again
      // makes another closure type, which the closure made from it is not of).
      if (values[i].isObject() && values[i].type && types.unqualified(values[i].type)->kind == TyKind::Lambda)
        argTy = values[i].type;
      // An expansion's element (`forward<Args>(args)...`) by its value category: a class object
      // that is an lvalue deduces a reference, so that forward copies it (Map's emplace moved the
      // caller's key out of it, at compile time only).
      if (!a && argTy && argTy->isKnown() && !argTy->isReference() && categories && i < categories->size() && (*categories)[i] == 1 &&
          types.unqualified(argTy)->kind == TyKind::Class)
        argTy = types.lref(argTy);
      argTypes.push_back(argTy && argTy->isKnown() ? (a ? argumentType(a, argTy, types) : argTy) : nullptr);
    }
    std::vector<const Ty*> patterns;
    for (Decl* p : f->params)
      patterns.push_back(convertType(static_cast<ParamDecl*>(p)->type, pctx, depth + 1));
    if (variadic)
    {
      // `sum(Ts... values)`: the pack takes the arguments from its position on.
      bool known = true;
      for (const Ty* t : argTypes)
        known = known && t;
      for (const Ty* t : patterns)
        known = known && t && t->isKnown();
      if (known)
        deduceParams(f->params, Span<const Ty*>{patterns.data(), uint32_t(patterns.size())}, argTypes, fromArgs);
    }
    else
      for (size_t i = 0; i < argTypes.size() && i < patterns.size(); ++i)
        if (argTypes[i] && patterns[i] && patterns[i]->isKnown())
          deduce(patterns[i], argTypes[i], fromArgs);
  }
  for (size_t i = 0; i < f->templateParams.size(); ++i)
  {
    auto* param = static_cast<TemplateParamDecl*>(f->templateParams[i]);
    const Ty* t = i < given.size() ? given[i] : fromArgs.find(param);
    if (param->isPack && i + 1 == f->templateParams.size() && (i < given.size() || (!t && c->args.empty())))
    {
      // The pack takes the written arguments from its position on (none: `typeIndex<T>()` of
      // `template<class T, class... Tail>`), as bindExplicitArgs has it.
      std::vector<const Ty*> rest(given.begin() + std::min(i, given.size()), given.end());
      t = types.pack(Span<const Ty*>{rest.data(), uint32_t(rest.size())});
    }
    if (!t && !param->isPack && param->defaultArg)
    {
      // Neither written nor deduced: its default (`template<typename Unused = void>`, a value's
      // `template<class T, int N = 3>`, which `times(4)` was no constant without), which may name
      // the parameters before it.
      TypeContext dctx{lexicalScopeOf(f), &params, bound.empty() ? nullptr : &bound, f->file ? f->file->tokens : nullptr,
                       f->file ? f->file->text : std::string_view()};
      t = param->isType ? convertType(static_cast<Type*>(param->defaultArg), dctx, depth + 1) : templateArgument(param->defaultArg, param, dctx, depth + 1);
    }
    if (!t || !t->isKnown() || types.isDependent(t))
      return false;
    bound.pairs.push_back({f->templateParams[i], t});
    params.push(static_cast<TemplateParamDecl*>(f->templateParams[i])->name, f->templateParams[i]);
  }
  return true;
}

auto GlobalScope::Impl::constCall(CallExpr* c, const TypeContext& ctx, int depth, std::optional<ConstPlace>* returnPlace) -> std::optional<ConstValue>
{
  if (returnPlace)
  {
    auto* named = c->callee && c->callee->kind == NodeKind::NameExpr ? static_cast<NameExpr*>(c->callee) : nullptr;
    if (!named || !named->resolved || named->resolved->kind != NodeKind::FunctionDecl || constLocal(named, ctx))
      return std::nullopt;
  }
  // Set again however the call ends (an evaluation inside it clears it for its own calls): the
  // caller runs it no second time for its value.
  struct Ran
  {
    ~Ran()
    {
      if (on)
        constCallPlaceRan = true;
    }

    bool on;
  } ran{returnPlace != nullptr};
  if (c->callee && c->callee->kind == NodeKind::MemberExpr)
  {
    // `piece.append(piece.object, out)`: through the function pointer a data member holds.
    auto* m = static_cast<MemberExpr*>(c->callee);
    if (m->resolved && m->resolved->kind == NodeKind::VariableDecl && !(m->resolved->flags & DF_Typedef) && m->ty &&
        types.unqualified(types.stripRef(m->ty))->kind == TyKind::Pointer)
    {
      std::optional<ConstValue> fn = constValue(m, ctx, depth + 1);
      return fn ? constCallThrough(*fn, c, ctx, depth) : std::nullopt;
    }
    return constMemberCall(c, ctx, depth);
  }
  if (!c->callee)
    return std::nullopt;
  Expr* called = c->callee;
  while (called->kind == NodeKind::ParenExpr && static_cast<ParenExpr*>(called)->inner)
    called = static_cast<ParenExpr*>(called)->inner;
  if (called->kind == NodeKind::LambdaExpr)
  {
    auto* l = static_cast<LambdaExpr*>(called);
    if (l->captures.empty() && !l->defaultCapture)
      return constCallLambda(l, c, ctx, depth); // `[]() -> Shape { ... }()`
    // `[&]() { ... }()`: its closure made here, then called.
    std::optional<ConstValue> closure = constClosure(l, called->ty, ctx, depth);
    return closure ? constCallClosure(l, *closure, c, ctx, depth) : std::nullopt;
  }
  if (c->callee->kind != NodeKind::NameExpr)
  {
    // `ops[0](5)`, `(*f)(5)`: through the function pointer the callee evaluates to; `D()(held)`
    // (UniquePtr's deleter, which made a UniquePtr no constant): the object's operator().
    std::optional<ConstValue> fn = constValue(c->callee, ctx, depth + 1);
    if (fn && fn->isObject())
      return constCallObjectValue(*fn, std::nullopt, c, ctx, depth);
    return fn ? constCallThrough(*fn, c, ctx, depth) : std::nullopt;
  }
  auto* callee = static_cast<NameExpr*>(c->callee);
  Decl* d = callee->resolved;
  // `f(v)` with f a parameter, local or constant holding a function's address.
  if (constLocal(callee, ctx) || (d && (d->kind == NodeKind::ParamDecl || (d->kind == NodeKind::VariableDecl && !(d->flags & DF_Typedef)))))
  {
    std::optional<ConstValue> fn = constValue(callee, ctx, depth + 1);
    if (fn && fn->isObject())
      return constCallObject(callee, c, ctx, depth); // `compare(a, b)`: an object's operator()
    return fn ? constCallThrough(*fn, c, ctx, depth) : std::nullopt;
  }
  // `UnderlyingType<E>(value)`, `Conditional<isSigned<U>, int64_t, uint64_t>(value)`: a functional cast to the type the
  // built-in trait gives, as the walk types the call (typeOfCall), which converts as `T(value)` does below. The name is
  // no function (or the library's struct of the name, which the trait is not).
  if (!d && callee->name.parts.size() == 1 && callee->name.parts[0].hasTemplateArgs && isTypeTraitName(callee->name.parts[0].symbol))
  {
    const Ty* to = typeTrait(callee->name.parts[0], ctx, depth + 1);
    to = to && to->isKnown() ? types.unqualified(to) : nullptr;
    if (to && (to->isArithmetic() || to->kind == TyKind::Enum) && c->args.size() <= 1)
      return c->args.empty() ? constConverted(ConstValue::integer(0), to) : constConverted(constValue(c->args[0], ctx, depth + 1), to);
    if (to && to->kind == TyKind::Class && !isUndeducedClassTemplate(to))
    {
      // `Conditional<wide, Size, Small>(3, 4)`: the object the constructor the arguments choose makes, as below.
      return constConstructFrom(to, c->args, ctx, depth);
    }
    return std::nullopt; // (a pointer the trait gives: not evaluated yet)
  }
  if (!d || d->kind == NodeKind::TemplateParamDecl)
  {
    Lookup l = lookupQualified(callee->name, ctx.scope, ctx.locals, depth + 1, ctx.subst);
    d = l.decl;
    // Not typed where it is written (a template argument, `Imm<max(a, b)>`): of the
    // functions of the name, the one whose parameters the arguments fill, when one does.
    if (d && d->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(d)->params.size() != c->args.size() && l.overloads)
    {
      Decl* fits = nullptr;
      int fitting = 0;
      for (const Entry& entry : *l.overloads)
        if (entry.decl && entry.decl->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(entry.decl)->params.size() == c->args.size() &&
            static_cast<FunctionDecl*>(entry.decl)->body)
        {
          fits = entry.decl;
          ++fitting;
        }
      if (fitting == 1)
        d = fits;
    }
  }
  // `size_t(5)`, `uint8_t(x)`, `RenderLayer::LayerType(RenderLayer::LAST)`, `Tag(0)`, `U(2 * limit)`:
  // a type's name called with one value converts it, with none makes a zero.
  if (c->args.size() <= 1 && !callee->name.last().hasTemplateArgs)
  {
    const Ty* to = !d && callee->name.parts.size() == 1 ? builtinByName(callee->name.parts[0].symbol) : nullptr;
    if (d && (d->kind == NodeKind::UsingAliasDecl || d->kind == NodeKind::EnumDecl || (d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef)) ||
              (d->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(d)->isType)))
    {
      NamedType named{};
      named.kind = NodeKind::NamedType;
      named.token = callee->token;
      named.name = callee->name;
      to = types.unqualified(convertType(&named, ctx, depth + 1));
    }
    if (to && (to->isArithmetic() || to->kind == TyKind::Enum))
      return c->args.empty() ? constConverted(ConstValue::integer(0), to) : constConverted(constValue(c->args[0], ctx, depth + 1), to);
    if (d && d->kind == NodeKind::EnumDecl)
      return std::nullopt; // an enumeration the checker cannot convert
  }
  if (!d)
    return std::nullopt;
  if (d->kind == NodeKind::ClassDecl && c->args.empty())
  {
    // `is_optional<T>()`: a zero-arg constructor of a type-trait struct used as bool.
    // Return its static constexpr `value` member (inherited from TrueType/FalseType), read
    // for the instance the arguments make (`HasSize<double> : Constant<bool, false>`).
    NamedType named{};
    named.kind = NodeKind::NamedType;
    named.token = callee->token;
    named.name = callee->name;
    if (std::optional<ConstValue> v = traitObjectValue(convertType(&named, ctx, depth + 1), depth + 1))
      return v;
    if (VariableDecl* v = findStaticBoolValue(static_cast<ClassDecl*>(d), depth + 1))
    {
      TypeContext vctx{lexicalScopeOf(v), nullptr, nullptr, v->file ? v->file->tokens : nullptr, v->file ? v->file->text : std::string_view()};
      return constAsDeclared(constInitValue(constInitializerOf(v, nullptr), vctx, depth + 1), v->type ? convertType(v->type, vctx, depth + 1) : nullptr);
    }
    if (const Ty* t = convertType(&named, ctx, depth + 1); t && t->kind == TyKind::Class)
      return constEmpty(t, true, depth + 1); // `Point()`: value-initialized
    return std::nullopt;
  }
  if (d->kind == NodeKind::ClassDecl || (d->kind == NodeKind::UsingAliasDecl && !callee->name.last().hasTemplateArgs) ||
      (d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef)) ||
      (d->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(d)->isType))
  {
    // `BoundingBox(a, b)`, `V(x, y)` with V a type parameter (a CRTP base's derived class): the
    // object the constructor the arguments choose makes.
    NamedType named{};
    named.kind = NodeKind::NamedType;
    named.token = callee->token;
    named.name = callee->name;
    const Ty* t = convertType(&named, ctx, depth + 1);
    t = t ? types.unqualified(t) : nullptr;
    // `Pair(1, "one")`: a class template named without its arguments, the ones the walk deduced
    // (class template argument deduction), as for `Pair { 1, "one" }`.
    if ((!t || !t->isKnown() || isUndeducedClassTemplate(t)) && c->ty)
      if (const Ty* deduced = types.unqualified(types.stripRef(c->ty));
          deduced->kind == TyKind::Class && deduced->isKnown() && !types.isDependent(deduced) && !mentionsUnknown(deduced))
        t = deduced;
    if (!t || t->kind != TyKind::Class || isUndeducedClassTemplate(t))
      return std::nullopt;
    return constConstructFrom(t, c->args, ctx, depth);
  }
  if (d->kind != NodeKind::FunctionDecl)
    return std::nullopt;
  auto* f = static_cast<FunctionDecl*>(d);
  // The written template arguments, an expansion among them by its elements (`typeIndex<T,
  // Renderers...>()`), and of the functions of the name the one whose template parameters
  // they fill: the walk chose where the pack's length was not known.
  const NamePart& written = callee->name.last();
  auto explicitFor = [&](FunctionDecl* g, std::vector<const Ty*>& out)
  {
    out.clear();
    size_t n = g->templateParams.size();
    for (Node* w : written.templateArgs)
    {
      Decl* param = n ? g->templateParams[std::min(out.size(), n - 1)] : nullptr;
      const Ty* t = templateArgument(w, param, ctx, depth + 1);
      if (w && w->kind == NodeKind::PackType && t && t->kind == TyKind::Pack)
        out.insert(out.end(), t->args.begin(), t->args.end());
      else
        out.push_back(t);
    }
  };
  auto fillsTemplate = [&](FunctionDecl* g, const std::vector<const Ty*>& given)
  {
    size_t n = g->templateParams.size();
    if (given.size() > n && !(n && static_cast<TemplateParamDecl*>(g->templateParams[n - 1])->isPack))
      return false;
    if (c->args.empty())
      for (size_t i = given.size(); i < n; ++i)
        if (auto* p = static_cast<TemplateParamDecl*>(g->templateParams[i]); !p->isPack && !p->defaultArg)
          return false; // no argument to deduce it from
    return true;
  };
  std::vector<const Ty*> given;
  if (!written.templateArgs.empty())
  {
    explicitFor(f, given);
    if (!fillsTemplate(f, given))
    {
      Lookup l = lookupQualified(callee->name, ctx.scope, ctx.locals, depth + 1, ctx.subst);
      FunctionDecl* fits = nullptr;
      int fitting = 0;
      std::vector<const Ty*> candidateArgs;
      if (l.overloads)
        for (const Entry& entry : *l.overloads)
          if (entry.decl && entry.decl->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(entry.decl)->params.size() == c->args.size())
          {
            auto* g = static_cast<FunctionDecl*>(entry.decl);
            explicitFor(g, candidateArgs);
            if (fillsTemplate(g, candidateArgs))
            {
              fits = g;
              ++fitting;
            }
          }
      if (fitting != 1)
        return std::nullopt;
      f = fits;
      explicitFor(f, given);
    }
  }
  if (!f->body && (c->args.size() == 1 || c->args.size() == 2) && f->params.size() == c->args.size() && !f->name.empty())
    if (std::string_view name = symbols.name(f->name.last().symbol); name == "malloc" || name == "calloc" || name == "realloc" || name == "free")
    {
      std::vector<ConstValue> values;
      if (!constArguments(c->args, ctx, depth, values))
        return std::nullopt;
      return constCAllocation(name, values, c, depth);
    }
  if (!f->body && c->args.size() == 3 && f->params.size() == 3 && !f->name.empty())
    if (std::string_view name = symbols.name(f->name.last().symbol); name == "memcpy" || name == "memmove" || name == "memset" || name == "memcmp" || name == "memchr")
    {
      std::vector<ConstValue> values;
      if (!constArguments(c->args, ctx, depth, values))
        return std::nullopt;
      return name == "memset"   ? constMemset(values[0], values[1], values[2], depth)
           : name == "memcmp"   ? constMemcmp(values[0], values[1], values[2], depth)
           : name == "memchr"   ? constMemchr(values[0], values[1], values[2], depth)
                                : constMemcpy(values[0], values[1], values[2], depth);
    }
  if (!f->body && c->args.size() == 2 && f->params.size() == 2 && !f->name.empty() && symbols.name(f->name.last().symbol) == "strcmp")
  {
    std::vector<ConstValue> values;
    if (!constArguments(c->args, ctx, depth, values))
      return std::nullopt;
    return constStrcmp(values[0], values[1], depth);
  }
  if (!f->body && c->args.size() == 1 && f->params.size() == 1 && !f->name.empty() && symbols.name(f->name.last().symbol) == "strlen")
  {
    std::optional<ConstValue> text = constValue(c->args[0], ctx, depth + 1);
    if (text && text->isObject())
      text = constPointerTo(*text, types.pointer(types.qualified(types.charType(8, false), true, false)));
    return text ? constStrlen(*text) : std::nullopt;
  }
  if (int which = intrinsicOf(f))
  {
    // The library's enum intrinsics (Text.kpp): E written, or the type the checker deduced.
    std::vector<ConstValue> values;
    if (!constArguments(c->args, ctx, depth, values))
      return std::nullopt;
    const Ty* e = c->templateArgCount ? c->templateArgs[0] : !given.empty() ? given[0] : nullptr;
    if (e && ctx.subst && types.isDependent(e))
      e = substitute(e, *ctx.subst);
    Subst bound;
    if (e && !f->templateParams.empty())
      bound.pairs.push_back({f->templateParams[0], e});
    return constIntrinsic(which, f, values, &bound, depth);
  }
  if (FunctionDecl* def = constDefinition(f); !def || def->body->kind != NodeKind::BlockStmt)
    return constNoBody(f);
  std::vector<ConstValue> values;
  std::vector<std::optional<ConstPlace>> places; // what reference parameters refer to
  std::vector<uint8_t> categories;               // how by-value parameters are initialised
  if (!constArguments(c->args, ctx, depth, values, &places, &categories))
    return std::nullopt;
  bool variadic = !f->params.empty() && static_cast<ParamDecl*>(f->params[f->params.size() - 1])->isPack;
  if (values.size() > f->params.size() && !variadic)
    return std::nullopt;
  // `AreCreatedInOrder<EntityPrototypeList, AchievementPrototypeList>()`: a template's body
  // with its parameters bound to the written arguments (and a class template member's to the
  // instance it is named through); a parameter left unbound decides nothing.
  Subst bound;
  LocalStack params;
  if (callee->name.parts.size() > 1)
    bound = qualifierSubst(callee->name, ctx, depth + 1, lexicalScopeOf(f));
  for (const Subst::Pair& p : bound.pairs)
    params.push(static_cast<TemplateParamDecl*>(p.param)->name, p.param);
  if (!constBindTemplate(f, given, c, values, ctx, depth, bound, params, &categories))
    return std::nullopt;
  if (ctx.subst) // called from inside an instance: its class's arguments stay
    for (const Subst::Pair& p : ctx.subst->pairs)
      if (!bound.find(p.param))
        bound.pairs.push_back(p);
  for (Decl* p : effectiveParams(f->parent && f->parent->kind == NodeKind::ClassDecl ? f->parent : nullptr))
    if (!bound.find(p))
      return std::nullopt; // a member of a class template named with no instance
  TypeContext fctx{lexicalScopeOf(f), params.items.empty() ? nullptr : &params, bound.empty() ? nullptr : &bound,
                   f->file ? f->file->tokens : nullptr, f->file ? f->file->text : std::string_view()};
  // `size()` called in a member function: on the same object, whose changes stay.
  std::optional<ConstPlace> self = f->parent && f->parent->kind == NodeKind::ClassDecl && !(f->flags & (DF_Static | DF_Constructor)) ? constThisPlace(ctx)
                                                                                                                                      : std::nullopt;
  if (!self)
    return constRun(f, values, fctx, depth, nullptr, &places, &categories, nullptr, returnPlace);
  std::optional<ConstValue> object = constRead(*self);
  // `A::f()`, `super::update()` (named with its class): that function, run on the object's part of
  // its class, no virtual call ([class.virtual] 16).
  bool qualified = callee->name.parts.size() > 1;
  if (qualified && object && object->isObject() && object->type && f->parent)
  {
    std::vector<size_t> path;
    if (const Ty* owner = constBaseDeclaring(object->type, f->parent, path, 0); owner && !path.empty())
    {
      self->path.insert(self->path.end(), path.begin(), path.end());
      self->type = owner;
      self->bits = 0;
      object = constRead(*self);
    }
  }
  if (!object)
    return std::nullopt;
  constQualifiedCall() = qualified;
  if (returnPlace)
    return constRun(f, values, fctx, depth, &object, &places, &categories, &*self, returnPlace);
  std::optional<ConstValue> r = constRun(f, values, fctx, depth, &object, &places, &categories);
  if (r && object && self->writable() && !(f->flags & DF_Const))
    constWrite(*self, *object);
  return r;
}

auto GlobalScope::Impl::constMemberCall(CallExpr* c, const TypeContext& ctx, int depth) -> std::optional<ConstValue>
{
  auto* m = static_cast<MemberExpr*>(c->callee);
  // `object.A::f()` (named with its class): the function the walk chose, run on the object's part of
  // its class, no virtual call ([class.virtual] 16).
  bool qualified = m->member.parts.size() > 1;
  if (!m->base || m->dynamicMember || m->member.parts.empty() || (qualified && !(m->resolved && m->resolved->kind == NodeKind::FunctionDecl)))
    return std::nullopt;
  if (!qualified && m->member.parts[0].special == 1 && c->args.empty())
  {
    // `slots[i].~Text()`, `p->~T()`: the object destroyed where it is, its lifetime ended there
    // (what is left is no value: reading it is no constant).
    std::optional<ConstPlace> place;
    if (m->op == P::Dot)
      place = constPlaceOf(m->base, ctx, depth + 1);
    else if (std::optional<ConstValue> p = constValue(m->base, ctx, depth + 1))
      place = constStoragePlace(*p, "a destruction of", depth);
    std::optional<ConstValue> value = place ? constRead(*place) : std::nullopt;
    if (!value || !constDestroy(value->type ? value->type : place->type, *value, depth + 1, &*place) || !constWrite(*place, ConstValue::unset(value->type)))
      return std::nullopt;
    return ConstValue::integer(0, types.voidType());
  }
  // `o->size()` of an Optional, an iterator, a handle (a class with operator->): the call on what
  // its operator-> points at, run before the arguments are evaluated, as the call's postfix
  // expression is ([expr.call] 8); its object the class that pointer's type names. (It was no
  // constant: the object's type was looked for as a pointer's only.)
  bool classArrow = m->op == P::Arrow && m->base->kind != NodeKind::ThisExpr && constObjectType(m->base, ctx, false);
  std::optional<ConstValue> through = classArrow ? constArrowPointer(m->base, constValue(m->base, ctx, depth + 1), ctx, depth) : std::nullopt;
  if (classArrow && (!through || !(through->isAddress() || through->isPointer())))
    return std::nullopt; // (a class object whose operator-> gave no pointer)
  const Ty* pointee = through ? types.unqualified(types.unqualified(through->type)->inner) : nullptr;
  const Ty* object = !through ? constObjectType(m->base, ctx, m->op == P::Arrow)
                   : pointee && pointee->kind == TyKind::Class && !types.isDependent(pointee) && !mentionsUnknown(pointee) ? pointee : nullptr;
  Scope* s = object ? classScopeOf(object) : nullptr;
  Lookup l;
  if (!s || (!qualified && !lookupInScope(s, m->member.parts[0].symbol, l, depth + 1)))
    return std::nullopt;
  if (!qualified && l.decl && l.decl->kind == NodeKind::VariableDecl && !(l.decl->flags & (DF_Static | DF_Typedef)))
    return through ? std::nullopt : constCallObject(m, c, ctx, depth, l.decl); // `this->hasher(key)`: a member object's operator()
  // Of the functions of the name, the one the call's arguments fill.
  FunctionDecl* f = nullptr;
  auto fills = [&](Decl* d) {
    if (!d || d->kind != NodeKind::FunctionDecl || !constDefinition(static_cast<FunctionDecl*>(d)))
      return false;
    // As many arguments as parameters, or fewer and defaults for the rest (`moveToBlock(n)` of
    // `moveToBlock(int, bool strong = true)`), in the declaration or the definition; a trailing
    // parameter pack takes any number (`emplace_back(Args&&... args)`).
    auto* fd = static_cast<FunctionDecl*>(d);
    FunctionDecl* def = constDefinition(fd);
    bool pack = !fd->params.empty() && static_cast<ParamDecl*>(fd->params[fd->params.size() - 1])->isPack;
    if (c->args.size() > fd->params.size() && !pack)
      return false;
    for (size_t i = c->args.size(); i < fd->params.size(); ++i)
      if (!static_cast<ParamDecl*>(fd->params[i])->defaultValue && !static_cast<ParamDecl*>(fd->params[i])->isPack &&
          !(def && i < def->params.size() && static_cast<ParamDecl*>(def->params[i])->defaultValue))
        return false;
    return true;
  };
  if (m->resolved && m->resolved->kind == NodeKind::FunctionDecl)
    f = fills(m->resolved) ? static_cast<FunctionDecl*>(m->resolved) : nullptr;
  else if (l.overloads)
  {
    int fitting = 0;
    for (const Entry& entry : *l.overloads)
      if (fills(entry.decl))
      {
        f = static_cast<FunctionDecl*>(entry.decl);
        ++fitting;
      }
    if (fitting != 1)
      f = nullptr; // `size() const` and `size()` alike: either computes the same here, but which is not decided
  }
  else if (fills(l.decl))
    f = static_cast<FunctionDecl*>(l.decl);
  if (!f || constDefinition(f)->body->kind != NodeKind::BlockStmt)
    return std::nullopt;
  std::vector<ConstValue> values;
  std::vector<std::optional<ConstPlace>> places; // what reference parameters refer to
  std::vector<uint8_t> categories;               // how by-value parameters are initialised
  if (!constArguments(c->args, ctx, depth, values, &places, &categories))
    return std::nullopt;
  Subst bound = memberSubst(object, qualified ? (f->parent ? f->parent->ownScope : nullptr) : l.foundIn, depth + 1);
  LocalStack params;
  for (const Subst::Pair& p : bound.pairs)
    params.push(static_cast<TemplateParamDecl*>(p.param)->name, p.param);
  for (Decl* p : effectiveParams(f->parent && f->parent->kind == NodeKind::ClassDecl ? f->parent : nullptr))
    if (!bound.find(p))
      return std::nullopt;
  // A member function template's arguments written (`object.get<0>()`, a tuple-like structured
  // binding's), as a free one's are.
  std::vector<const Ty*> given;
  if (const NamePart& part = m->member.last(); part.hasTemplateArgs && !f->templateParams.empty())
    for (Node* w : part.templateArgs)
    {
      size_t n = f->templateParams.size();
      const Ty* t = templateArgument(w, f->templateParams[std::min(given.size(), n - 1)], ctx, depth + 1);
      if (w && w->kind == NodeKind::PackType && t && t->kind == TyKind::Pack)
        given.insert(given.end(), t->args.begin(), t->args.end());
      else
        given.push_back(t);
    }
  if (!f->templateParams.empty() && !constBindTemplate(f, given, c, values, ctx, depth, bound, params, &categories))
    return std::nullopt; // a member function template (`emplace_back(Args&&...)`, `value_or(U&&)`): its own parameters deduced
  TypeContext fctx{lexicalScopeOf(f), params.items.empty() ? nullptr : &params, bound.empty() ? nullptr : &bound,
                   f->file ? f->file->tokens : nullptr, f->file ? f->file->text : std::string_view()};
  // The object, when the evaluation holds it (a local, a constant, a temporary): the function
  // runs on it as `this`, and what it changes in a local stays.
  std::optional<ConstPlace> place;
  std::optional<ConstValue> held;
  if (m->base->kind == NodeKind::ThisExpr && m->op == P::Arrow)
    place = constThisPlace(ctx);
  else if (m->op == P::Dot)
    place = constPlaceOf(m->base, ctx, depth + 1, true);
  std::optional<ConstValue> pointer;
  if (through)
  {
    // `o->size()`: on the object operator-> points at, there when the run holds it.
    pointer = through;
    if (pointer->isAddress() && pointer->target && pointer->target->storage)
      if (!(place = constStoragePlace(*pointer, "a use of", depth)))
        return std::nullopt;
  }
  else if (!place && m->op == P::Arrow && constPointsIntoStorage(m->base))
  {
    // `log->add(3)`, log pointing at an object of the evaluation: run on it there.
    pointer = constValue(m->base, ctx, depth + 1);
    if (!pointer)
      return std::nullopt;
    if (pointer->isAddress() && pointer->target && pointer->target->storage)
      if (!(place = constStoragePlace(*pointer, "a use of", depth)))
        return std::nullopt;
  }
  if (!place && !pointer && m->op == P::Dot && m->base->kind != NodeKind::ThisExpr)
  {
    // `make().size()`: the object is a temporary of the full expression, which the function
    // runs on there (and which ends with it).
    Expr* source = nullptr;
    if (constCategory(m->base, source) == 0)
      if (std::optional<ConstValue> object = constValue(m->base, ctx, depth + 1))
      {
        if (object->isObject() && object->type && types.unqualified(object->type)->kind == TyKind::Class)
          place = constMaterialize(*object, object->type);
        if (!place)
          pointer = object; // (no full expression to hold it: run on the value, as before)
      }
  }
  if (place)
    held = constRead(*place);
  else
  {
    held = pointer ? pointer : constValue(m->base, ctx, depth + 1);
    if (held && m->op == P::Arrow)
      held = held->isAddress() ? constReadTarget(*held, depth + 1) : held->isPointer() ? constDeref(*held) : std::nullopt;
  }
  if (held && !held->isObject())
    held.reset();
  if (!held)
    return constRun(f, values, fctx, depth, nullptr, &places, &categories); // what the object's type decides (Array::size of a run-time array)
  if (qualified && held->type && f->parent)
  {
    std::vector<size_t> path;
    if (const Ty* owner = constBaseDeclaring(held->type, f->parent, path, 0); owner && !path.empty())
    {
      if (place)
      {
        place->path.insert(place->path.end(), path.begin(), path.end());
        place->type = owner;
        place->bits = 0;
        held = constRead(*place);
      }
      else
        held = constPart(*held, path);
      if (!held)
        return std::nullopt;
    }
  }
  constQualifiedCall() = qualified;
  std::optional<ConstValue> r = constRun(f, values, fctx, depth, &held, &places, &categories, place ? &*place : nullptr);
  return r;
}

auto GlobalScope::Impl::constBasePath(const Ty* from, const Ty* to, int depth) -> std::optional<std::vector<size_t>>
{
  from = types.unqualified(types.stripRef(from));
  to = types.unqualified(types.stripRef(to));
  if (from == to)
    return std::vector<size_t>{};
  const ConstClassLayout* layout = constLayoutOf(from, depth);
  if (!layout || depth > 16)
    return std::nullopt;
  for (size_t b = 0; b < layout->bases.size(); ++b)
    if (std::optional<std::vector<size_t>> inner = constBasePath(layout->bases[b], to, depth + 1))
    {
      inner->insert(inner->begin(), b);
      return inner;
    }
  return std::nullopt;
}

auto GlobalScope::Impl::constMembersNamed(const Ty* t, uint32_t symbol, Subst& subst, int depth, bool templates) -> std::vector<FunctionDecl*>
{
  std::vector<FunctionDecl*> out;
  Scope* s = classScopeOf(t);
  Lookup l;
  if (!s || !lookupInScope(s, symbol, l, depth + 1))
    return out;
  subst = memberSubst(types.unqualified(types.stripRef(t)), l.foundIn, depth + 1);
  if (l.overloads)
  {
    for (const Entry& entry : *l.overloads)
      if (entry.decl && entry.decl->kind == NodeKind::FunctionDecl && (templates || static_cast<FunctionDecl*>(entry.decl)->templateParams.empty()))
        out.push_back(static_cast<FunctionDecl*>(entry.decl));
  }
  else if (l.decl && l.decl->kind == NodeKind::FunctionDecl && (templates || l.decl->templateParams.empty()))
    out.push_back(static_cast<FunctionDecl*>(l.decl));
  return out;
}

auto GlobalScope::Impl::constFreeFunction(uint32_t symbol, const std::vector<const Ty*>& argTypes, Scope* scope, bool templates) -> FunctionDecl*
{
  // The arguments' classes' friend functions are candidates too; those of a class template's
  // instance would need its arguments, which no caller here passes on: none is chosen then.
  Subst friendArgs;
  Lookup friends = hiddenFriendLookup(symbol, argTypes, friendArgs);
  if (!friendArgs.empty())
    return nullptr;
  auto choose = [&](const Lookup& look) -> FunctionDecl*
  {
    std::vector<FunctionDecl*> candidates;
    if (look.overloads)
    {
      for (const Entry& e : *look.overloads)
        if (e.decl && e.decl->kind == NodeKind::FunctionDecl && (templates || e.decl->templateParams.empty()))
          candidates.push_back(static_cast<FunctionDecl*>(e.decl));
    }
    else if (look.decl && look.decl->kind == NodeKind::FunctionDecl && (templates || look.decl->templateParams.empty()))
      candidates.push_back(static_cast<FunctionDecl*>(look.decl));
    bool ambiguous = false;
    FunctionDecl* f = candidates.empty() ? nullptr : chooseAmong(candidates, argTypes, nullptr, nullptr, ambiguous);
    return f && !ambiguous ? f : nullptr;
  };
  std::vector<Scope*> places;
  for (const Ty* a : argTypes)
    if (const Ty* t = a ? types.stripRef(a) : nullptr; t && (t->kind == TyKind::Class || t->kind == TyKind::Enum) && t->decl)
      places.push_back(lexicalScopeOf(t->decl));
  places.push_back(scope);
  // Every place's functions ranked together, as C++ ranks argument-dependent lookup's and ordinary
  // lookup's (the walker's operatorFunction and GlobalScope::findFree do the same): from each place
  // the innermost scope declaring the name, and the friends.
  std::vector<Entry> all;
  std::unordered_set<Decl*> seen;
  auto add = [&](const Lookup& look) {
    if (look.overloads)
    {
      for (const Entry& e : *look.overloads)
        if (e.decl && e.decl->kind == NodeKind::FunctionDecl && seen.insert(e.decl).second)
          all.push_back(e);
    }
    else if (look.decl && look.decl->kind == NodeKind::FunctionDecl && seen.insert(look.decl).second)
      all.push_back(Entry{look.decl, nullptr});
  };
  for (Scope* place : places)
    for (Scope* s = place; s; s = s->parent)
    {
      if (s->kind == ScopeKind::Class)
        continue; // members are no free functions, and do not hide them
      Lookup look;
      if (!lookupInScope(s, symbol, look, 0))
        continue;
      add(look);
      break;
    }
  if (friends.decl)
    add(friends);
  if (all.empty())
    return nullptr;
  Lookup merged;
  merged.decl = all[0].decl;
  merged.overloads = &all;
  return choose(merged);
}

auto GlobalScope::Impl::constCallPlace(FunctionDecl* f, const Subst& subst, const std::vector<ConstValue>& args, const ConstPlace& self, int depth, const std::vector<std::optional<ConstPlace>>* places, const std::vector<uint8_t>* categories, std::optional<ConstValue>* ran) -> std::optional<ConstPlace>
{
  // Run as any call (constRun: parameter packs, defaults, the arguments' places), on the object
  // where it is, its reference result kept as a place.
  if (!constDefinition(f) || depth > 512)
    return std::nullopt;
  LocalStack params;
  for (const Subst::Pair& p : subst.pairs)
    params.push(static_cast<TemplateParamDecl*>(p.param)->name, p.param);
  TypeContext fctx{lexicalScopeOf(f), params.items.empty() ? nullptr : &params, subst.empty() ? nullptr : &subst,
                   f->file ? f->file->tokens : nullptr, f->file ? f->file->text : std::string_view()};
  const Ty* returns = f->returnType ? convertType(f->returnType, fctx, 1) : nullptr;
  if (!returns || returns->kind != TyKind::LRef || !returns->inner)
    return std::nullopt;
  std::optional<ConstValue> object = constRead(self);
  std::optional<ConstPlace> out;
  constCallPlaceRan = constCallPlaceRan || object;
  if (!object)
    return std::nullopt;
  std::optional<ConstValue> value = constRun(f, args, fctx, depth, &object, places, categories, &self, &out);
  constCallPlaceRan = true; // (an evaluation inside the run clears it for its own calls)
  if (!value)
    return std::nullopt;
  if (!out)
  {
    if (ran)
      *ran = value; // (a reference the run holds no place of: what it refers to, as a value)
    return std::nullopt;
  }
  out->type = returns->inner;
  out->readOnly = out->readOnly || self.readOnly || returns->inner->isConst;
  return out;
}

auto GlobalScope::Impl::constPlaceMember(const Ty* t, uint32_t symbol, size_t args, bool writable, Subst& subst, int depth, Decl* preferred, const std::vector<const Ty*>* argTypes) -> FunctionDecl*
{
  FunctionDecl* found = nullptr;
  auto fills = [&](FunctionDecl* c) {
    bool pack = !c->params.empty() && static_cast<ParamDecl*>(c->params[c->params.size() - 1])->isPack;
    if (args > c->params.size() && !pack)
      return false;
    for (size_t i = args; i < c->params.size(); ++i)
      if (!static_cast<ParamDecl*>(c->params[i])->defaultValue && !static_cast<ParamDecl*>(c->params[i])->isPack)
        return false;
    return true;
  };
  std::vector<FunctionDecl*> fitting;
  for (FunctionDecl* c : constMembersNamed(t, symbol, subst, depth, true))
    if (fills(c) && constDefinition(c) && !(c->flags & DF_Static))
      fitting.push_back(c);
  for (FunctionDecl* c : fitting)
    if (preferred && (c == preferred || constDefinition(c) == constDefinition(static_cast<FunctionDecl*>(preferred))))
      return c;
  // Whether every fitting one takes the same parameters (`front()` and `front() const`).
  const Subst* in = subst.empty() ? nullptr : &subst;
  const Ty* first = fitting.empty() ? nullptr : declaredTypeOf(fitting[0], in, depth + 1);
  bool alike = true;
  for (FunctionDecl* c : fitting)
  {
    const Ty* ft = declaredTypeOf(c, in, depth + 1);
    alike = alike && first && ft && first->kind == TyKind::Function && ft->kind == TyKind::Function && ft->args.size() == first->args.size() &&
            std::equal(ft->args.begin(), ft->args.end(), first->args.begin());
  }
  if (!alike)
  {
    bool known = argTypes != nullptr;
    for (size_t i = 0; known && i < argTypes->size(); ++i)
      known = (*argTypes)[i] && (*argTypes)[i]->isKnown() && !types.isDependent((*argTypes)[i]);
    if (!known)
      return nullptr;
    bool ambiguous = false;
    const Ty* objectTy = writable ? types.unqualified(t) : types.qualified(types.unqualified(t), true, false);
    FunctionDecl* chosen = chooseAmong(fitting, *argTypes, in, objectTy, ambiguous);
    return chosen && !ambiguous ? chosen : nullptr;
  }
  for (FunctionDecl* c : fitting)
    if (!found || bool(c->flags & DF_Const) != writable)
      found = c;
  return found;
}

auto GlobalScope::Impl::constNoBody(FunctionDecl* f) -> std::optional<ConstValue>
{
  if (constWhy().empty() && f && !f->name.empty())
    constWhy() = "a call to " + std::string(symbols.name(functionSymbol(f->name))) +
                 ((f->file && f->file->isImported) || (f->flags & DF_ExternC) ? ", a C function, which has no body to run at compile time" : ", whose body the program does not have");
  return std::nullopt;
}

auto GlobalScope::Impl::constDefinition(FunctionDecl* f) -> FunctionDecl*
{
  if (!f || f->body)
    return f;
  Decl* cls = f->parent && f->parent->kind == NodeKind::ClassDecl ? f->parent : nullptr;
  if (!cls || !cls->ownScope || f->name.empty())
    return nullptr;
  uint32_t symbol = functionSymbol(f->name);
  const Ty* ft = matchedType(f);
  for (FunctionDecl* candidate : cls->ownScope->outOfLineMembers)
    if (candidate->body && functionSymbol(candidate->name) == symbol && !isMemberSpecialization(candidate) && typeAs(candidate, f) == ft)
      return candidate;
  return nullptr;
}

auto GlobalScope::Impl::constCallOn(FunctionDecl* f, const Subst& subst, const std::vector<ConstValue>& args, std::optional<ConstValue>* self, int depth, const ConstPlace* selfPlace, const std::vector<std::optional<ConstPlace>>* places, const std::vector<uint8_t>* categories, std::optional<ConstPlace>* returnPlace) -> std::optional<ConstValue>
{
  if (!constDefinition(f))
    return std::nullopt;
  LocalStack params;
  for (const Subst::Pair& p : subst.pairs)
    params.push(static_cast<TemplateParamDecl*>(p.param)->name, p.param);
  TypeContext fctx{lexicalScopeOf(f), params.items.empty() ? nullptr : &params, subst.empty() ? nullptr : &subst,
                   f->file ? f->file->tokens : nullptr, f->file ? f->file->text : std::string_view()};
  return constRun(f, args, fctx, depth, self, places, categories, selfPlace, returnPlace);
}

auto GlobalScope::Impl::constTruth(const ConstValue& v, int depth) -> std::optional<bool>
{
  if (v.isScalar() || v.isPointer() || v.isAddress() || v.isMemberPointer())
    return v.truthy();
  if (!v.isObject() || !v.type || v.type->isBraceList() || types.unqualified(v.type)->kind != TyKind::Class || depth > 64)
    return std::nullopt;
  const Ty* t = types.unqualified(v.type);
  // The conversion operator the one rule chooses for a condition (design.md 24, chooseConversion), as
  // codegen calls it: one to bool first (an explicit one too), else one to another scalar.
  const Ty* cls = nullptr;
  Subst subst;
  auto* op = chooseConversion(t, types.boolType(), GlobalScope::ConversionKind::ToType, false, &cls, &subst);
  if (!op || !cls || !constDefinition(op))
    return std::nullopt;
  std::optional<ConstValue> self = v;
  if (types.unqualified(cls) != t)
  {
    std::optional<std::vector<size_t>> path = constBasePath(t, cls, depth);
    std::optional<ConstValue> part = path ? constPart(v, *path) : std::nullopt;
    if (!part)
      return std::nullopt;
    self = part;
  }
  std::optional<ConstValue> r = constCallOn(op, subst, {}, &self, depth + 1);
  return r && r->isScalar() ? std::optional<bool>(r->truthy()) : std::nullopt;
}

auto GlobalScope::Impl::constByConversionOperator(const ConstValue& v, const Ty* t, int depth) -> std::optional<ConstValue>
{
  const Ty* from = types.unqualified(types.stripRef(v.type));
  const Ty* to = types.unqualified(types.stripRef(t));
  // The conversion operator the one rule chooses (design.md 24, chooseConversion), as codegen calls
  // it: one to the type itself first, else by the best standard conversion; never an explicit one
  // (a copy-initialization's, an argument's, a return's, not a condition's: constTruth).
  const Ty* cls = nullptr;
  Subst subst;
  FunctionDecl* op = chooseConversion(from, to, GlobalScope::ConversionKind::ToType, false, &cls, &subst, false);
  if (!op || !cls || !constDefinition(op))
  {
    if (constWhy().empty())
      constWhy() = "a conversion of " + types.toString(from, &symbols) + " to " + types.toString(to, &symbols) +
                   " by no conversion operator the evaluator can run";
    return std::nullopt;
  }
  std::optional<ConstValue> self = v;
  if (types.unqualified(cls) != from)
  {
    std::optional<std::vector<size_t>> path = constBasePath(from, cls, depth);
    self = path ? constPart(v, *path) : std::nullopt; // (a base's operator: run on that part)
    if (!self)
      return std::nullopt;
  }
  std::optional<ConstValue> r = constCallOn(op, subst, {}, &self, depth + 1);
  return r ? constAsDeclared(r, t) : std::nullopt; // (then the standard conversion to t)
}

auto GlobalScope::Impl::constClassOperator(const Ty* t, P op, const std::vector<const Ty*>& argTypes, Subst& subst, int depth, bool templates) -> FunctionDecl*
{
  std::vector<FunctionDecl*> candidates = constMembersNamed(t, opSymbols[uint32_t(op)], subst, depth, templates);
  if (candidates.empty())
    return nullptr;
  bool ambiguous = false;
  FunctionDecl* f = chooseAmong(candidates, argTypes, subst.empty() ? nullptr : &subst, types.unqualified(t), ambiguous);
  return f && !ambiguous ? f : nullptr;
}

auto GlobalScope::Impl::constMemberwiseEqual(const ConstValue& l, const ConstValue& r, const TypeContext& ctx, int depth) -> std::optional<bool>
{
  if (!l.isObject() || !r.isObject() || l.parts->size() != r.parts->size() || depth > 64)
    return std::nullopt;
  for (size_t i = 0; i < l.parts->size(); ++i)
  {
    std::optional<ConstValue> same = constBinaryValues(P::EqEq, (*l.parts)[i], (*r.parts)[i], ctx, depth + 1);
    std::optional<bool> b = same ? constTruth(*same, depth + 1) : std::nullopt;
    if (!b)
      return std::nullopt;
    if (!*b)
      return false;
  }
  return true;
}

auto GlobalScope::Impl::constMemberwiseOrder(const ConstValue& l, const ConstValue& r, const TypeContext& ctx, int depth) -> std::optional<int>
{
  if (depth > 64)
    return std::nullopt;
  if (l.isScalar() && r.isScalar())
  {
    std::optional<ConstValue> less = constArith().binary(P::Less, l, r), greater = constArith().binary(P::Greater, l, r);
    if (!less || !greater)
      return std::nullopt;
    return less->i ? -1 : greater->i ? 1 : 0;
  }
  if (!l.isObject() || !r.isObject() || l.parts->size() != r.parts->size())
    return std::nullopt;
  if (l.type && types.unqualified(l.type)->kind == TyKind::Class)
  {
    Subst subst;
    FunctionDecl* spaceship = constClassOperator(l.type, P::Spaceship, {r.type}, subst, depth);
    if (!spaceship)
    {
      // No <=> of its own: C++'s synthesized three-way comparison, equal by ==, else less by <.
      std::optional<ConstValue> equal = constBinaryValues(P::EqEq, l, r, ctx, depth + 1);
      std::optional<ConstValue> less = equal ? constBinaryValues(P::Less, l, r, ctx, depth + 1) : std::nullopt;
      std::optional<bool> e = equal ? constTruth(*equal, depth + 1) : std::nullopt, s = less ? constTruth(*less, depth + 1) : std::nullopt;
      if (!e || !s)
        return std::nullopt;
      return *e ? 0 : *s ? -1 : 1;
    }
    if (!constDefaulted(spaceship))
      return std::nullopt;
  }
  for (size_t i = 0; i < l.parts->size(); ++i)
  {
    std::optional<int> o = constMemberwiseOrder((*l.parts)[i], (*r.parts)[i], ctx, depth + 1);
    if (!o || *o != 0)
      return o;
  }
  return 0;
}

auto GlobalScope::Impl::constReturnsLRef(FunctionDecl* f, const Subst& subst) -> bool
{
  const Ty* ft = declaredTypeOf(f, subst.empty() ? nullptr : &subst, 0);
  return ft && ft->kind == TyKind::Function && ft->inner && ft->inner->kind == TyKind::LRef;
}

auto GlobalScope::Impl::constReferred(const std::optional<ConstValue>& ran, std::optional<ConstPlace>& place) -> std::optional<ConstValue>
{
  if (!ran || !place)
  {
    place.reset();
    return std::nullopt;
  }
  return constRead(*place);
}

auto GlobalScope::Impl::constBinaryChain(BinaryExpr* e, const TypeContext& ctx, int depth, std::optional<ConstPlace>* place) -> std::optional<ConstValue>
{
  auto chained = [](Expr* x) {
    if (!x || x->kind != NodeKind::BinaryExpr)
      return false;
    std::optional<P> op = constOperator(static_cast<BinaryExpr*>(x)->op, static_cast<BinaryExpr*>(x)->isKeyword);
    return op && isConstBinaryOperator(*op);
  };
  std::vector<BinaryExpr*> chain;
  Expr* first = e;
  for (; chained(first); first = static_cast<BinaryExpr*>(first)->lhs)
    chain.push_back(static_cast<BinaryExpr*>(first));
  if (chain.empty())
    return std::nullopt; // an operator that computes no value of scalars (`.*`)
  // `(DWORD)-11` from a C header, parsed as a subtraction from a parenthesized type name:
  // the cast of the negated value.
  const Ty* castTo = nullptr;
  if (first && first->kind == NodeKind::ParenExpr && static_cast<ParenExpr*>(first)->inner &&
      static_cast<ParenExpr*>(first)->inner->kind == NodeKind::NameExpr && !chain.back()->isKeyword &&
      (P(chain.back()->op) == P::Minus || P(chain.back()->op) == P::Plus))
  {
    auto* name = static_cast<NameExpr*>(static_cast<ParenExpr*>(first)->inner);
    Decl* d = name->resolved ? name->resolved : lookupQualified(name->name, ctx.scope, ctx.locals, depth + 1, ctx.subst).decl;
    if (d && (d->kind == NodeKind::UsingAliasDecl || d->kind == NodeKind::EnumDecl || (d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef))))
    {
      NamedType named{};
      named.kind = NodeKind::NamedType;
      named.token = name->token;
      named.name = name->name;
      castTo = convertType(&named, ctx, depth + 1);
    }
  }
  std::optional<ConstValue> value;
  std::optional<ConstPlace> returned; // the last operator's reference result, as a place
  std::optional<ConstPlace> firstPlace; // an operand a call gives (`v[i] == name`), where it is
  if (castTo)
  {
    std::optional<ConstValue> r = constValue(chain.back()->rhs, ctx, depth + 1);
    value = r ? constConverted(constArith().unary(P(chain.back()->op), *r), castTo) : std::nullopt;
    chain.pop_back();
  }
  else if (std::optional<std::optional<ConstValue>> placed = constPlacedOperand(first, ctx, depth, firstPlace))
    value = *placed;
  else
    value = constValue(first, ctx, depth + 1);
  bool failed = !value;
  for (size_t i = chain.size(); i-- > 0;)
  {
    std::optional<P> op = constOperator(chain[i]->op, chain[i]->isKeyword);
    // Short-circuit: `true || <unknown>` is true, `false && <unknown>` is false.
    if (!failed && op && (*op == P::OrOr || *op == P::AndAnd))
      if (std::optional<bool> t = constTruth(*value, depth); t && *t == (*op == P::OrOr))
      {
        value = ConstValue::integer(*op == P::OrOr, types.boolType());
        continue;
      }
    if (!op || !isConstBinaryOperator(*op))
      return std::nullopt;
    // After an operand that is no constant the others are still evaluated: a class's size
    // one of them needs defers a static_assert to the code generator's layouts
    // (`sizeof(grid) == 18 && sizeof(TileID) == 2`, layoutNeeded).
    std::optional<ConstPlace> rightPlace;
    std::optional<std::optional<ConstValue>> placed = constPlacedOperand(chain[i]->rhs, ctx, depth, rightPlace);
    std::optional<ConstValue> r = placed ? *placed : constValue(chain[i]->rhs, ctx, depth + 1);
    if (failed || !r)
    {
      failed = true;
      continue;
    }
    // A class operand's operator runs where the operand is: an lvalue's place, a prvalue (an
    // operator's result too) as a temporary of the full expression, destroyed when it ends.
    ConstOperand left, right;
    if (*op != P::Comma && (value->isObject() || r->isObject()))
    {
      if (i + 1 == chain.size() && !castTo)
      {
        left = constOperandOf(first, *value, ctx, depth);
        if (firstPlace)
          left.place = firstPlace;
      }
      else if (returned)
        left.place = returned; // the previous operator's reference result: the object it refers to (`out << a << b`)
      else if (value->isObject() && value->type && types.unqualified(types.stripRef(value->type))->kind == TyKind::Class)
      {
        left.category = 0; // the previous operator's result
        left.place = constMaterialize(*value, value->type);
      }
      right = constOperandOf(chain[i]->rhs, *r, ctx, depth);
      if (rightPlace)
        right.place = rightPlace;
    }
    returned.reset();
    value = *op == P::Comma ? r : constBinaryValues(*op, *value, *r, ctx, depth, &left, &right, &returned, chain[i]);
    failed = !value;
  }
  if (place && !failed)
    *place = returned;
  return failed ? std::nullopt : value;
}

auto GlobalScope::Impl::constBinaryValues(P op, const ConstValue& l, const ConstValue& r, const TypeContext& ctx, int depth, const ConstOperand* lo, const ConstOperand* ro, std::optional<ConstPlace>* returned, const BinaryExpr* node) -> std::optional<ConstValue>
{
  if (l.isUnset() || r.isUnset() || depth > 512)
    return std::nullopt;
  if ((op == P::AndAnd || op == P::OrOr) && (l.isObject() || r.isObject() || l.isAddress() || r.isAddress() || l.isMemberPointer() || r.isMemberPointer()))
  {
    std::optional<bool> a = constTruth(l, depth + 1), b = constTruth(r, depth + 1);
    if (!a || !b)
      return std::nullopt;
    return ConstValue::integer(op == P::AndAnd ? *a && *b : *a || *b, types.boolType());
  }
  // An object of a class on either side: its operators (`name == "release"` by StringView's).
  auto classObject = [this](const ConstValue& v) {
    return v.isObject() && v.type && !v.type->isBraceList() && types.unqualified(types.stripRef(v.type))->kind == TyKind::Class;
  };
  bool objects = classObject(l) || classObject(r);
  if (!objects && (l.isAddress() || r.isAddress() || l.isMemberPointer() || r.isMemberPointer()))
    return constAddressArith(op, l, r, depth);
  if (!objects && (l.isPointer() || r.isPointer()))
    return constPointerArith(op, l, r);
  // An enumeration operand of the node's operator (`P1 + P2` with an operator+(Plain, Plain)): the
  // operator function the checker chose for it (Walker::enumOperator) as codegen calls it, the
  // built-in operator only where it chose that (language/enum_operand_calls_user_defined_operator).
  auto enumeration = [this](const ConstValue& v) { return v.type && types.unqualified(types.stripRef(v.type))->kind == TyKind::Enum; };
  bool enums = node && !l.isObject() && !r.isObject() && (enumeration(l) || enumeration(r)) && op != P::AndAnd && op != P::OrOr;
  // `this->items + N`: an array an operator meets is a pointer to its first element.
  auto decayed = [this](const ConstValue& v) -> std::optional<ConstValue> {
    const Ty* t = v.isObject() && v.type ? types.unqualified(types.stripRef(v.type)) : nullptr;
    return t && t->kind == TyKind::Array && t->inner ? constPointerTo(v, types.pointer(t->inner)) : std::nullopt;
  };
  if (!objects && (op == P::Plus || op == P::Minus || op == P::EqEq || op == P::NotEq))
    if (std::optional<ConstValue> a = decayed(l), b = decayed(r); a || b)
      return constPointerArith(op, a ? *a : l, b ? *b : r);
  if (!l.isObject() && !r.isObject() && (!enums || (node->call != OperatorCall::Function && !verifyInterpreterDecisions())))
    return constArith().binary(op, l, r);
  auto boolean = [this](bool b) { return ConstValue::integer(b, types.boolType()); };
  bool comparison = op == P::EqEq || op == P::NotEq || op == P::Less || op == P::Greater || op == P::LessEq || op == P::GreaterEq;
  // The class's own operator, run on the left object; then a free one. With the operands' places
  // (ao, bo): run where they are.
  // KPP_VERIFY_DECISIONS: the operator function chosen here, against the one the checker recorded
  // on the node (the node's own operator on its own operands only; a rewritten one is never recorded).
  auto verify = [&](P o, FunctionDecl* chosen, bool member) {
    if (!node || o != op || node->call != OperatorCall::Function || !verifyInterpreterDecisions())
      return;
    if (chosen && member == node->calleeMember && constDefinition(chosen) == constDefinition(node->callee))
      return;
    auto name = [&](FunctionDecl* f) {
      if (!f)
        return std::string("the built-in operator");
      const NamePart& n = f->name.last();
      return (n.special == 2 ? "operator" + std::string(punctuatorSpelling(P(n.opToken))) : debugName(n.symbol)) + " (" + locationOf(f->file, f->token) + ")";
    };
    fprintf(stderr, "%s: error: decision check (interpreter): it calls %s, the checker chose %s%s\n",
            ctx.function && ctx.function->file ? locationOf(ctx.function->file, node->token).c_str() : "<interpreter>", name(chosen).c_str(),
            name(node->callee).c_str(), node->calleeMember ? " (a member)" : "");
  };
  // The operator function the checker recorded on the node (design.md 24), for the node's own
  // operator on its own operands: the one rule for which function `a op b` calls, which ranks a
  // class's member and the free ones together (C++'s [over.match.oper]; this interpreter's own
  // lookup below, for a rewritten or unrecorded operator, ranks them alike). Not a template's (the
  // arguments it was deduced with are not recorded) nor a friend of a class template's instance
  // (its class's arguments neither).
  auto recorded = [&](P o, const ConstOperand* ao, const ConstOperand* bo) -> FunctionDecl* {
    if (!node || o != op || ao != lo || bo != ro || node->call != OperatorCall::Function || !node->callee || verifyInterpreterDecisions())
      return nullptr;
    FunctionDecl* f = node->callee;
    if (!f->templateParams.empty())
      return nullptr;
    if (node->calleeMember)
      return f->parent && f->parent->kind == NodeKind::ClassDecl && f->parent->ownScope ? f : nullptr;
    if (f->flags & DF_Friend)
      for (Decl* p = f->parent; p; p = p->parent)
        if (!p->templateParams.empty())
          return nullptr;
    return f;
  };
  auto direct = [&](P o, const ConstValue& a, const ConstValue& b, const ConstOperand* ao = nullptr,
                    const ConstOperand* bo = nullptr) -> std::optional<std::optional<ConstValue>> {
    FunctionDecl* chosen = recorded(o, ao, bo);
    bool chosenFree = chosen && !node->calleeMember;
    FunctionDecl* ranked = nullptr; // a free operator taking the operands better than the class's member
    if (!chosenFree && a.isObject() && a.type && types.unqualified(a.type)->kind == TyKind::Class)
    {
      Subst subst;
      FunctionDecl* f = chosen;
      if (f)
        subst = memberSubst(types.unqualified(types.stripRef(a.type)), f->parent->ownScope, depth + 1);
      else if ((f = constClassOperator(a.type, o, {constOperandType(b, bo)}, subst, depth)) && !memberOperatorExact(f, subst, constOperandType(b, bo), 2))
      {
        // The member and the free operators ranked together, as the checker ranks them
        // (freeOperatorBeatsMember): a free one taking the operands strictly better is called.
        const Ty* at = constOperandType(a, ao);
        const Ty* bt = constOperandType(b, bo);
        if (FunctionDecl* free = constFreeFunction(opSymbols[uint32_t(o)], {at, bt}, ctx.scope); free && freeOperatorBeatsMember(free, Subst{}, f, subst, at, bt, 2))
        {
          ranked = free;
          f = nullptr;
        }
      }
      if (f)
      {
        if (ao == lo && bo == ro)
          verify(o, f, true);
        if (constDefaulted(f))
        {
          if (o == P::EqEq || o == P::NotEq)
          {
            std::optional<bool> same = constMemberwiseEqual(a, b, ctx, depth + 1);
            return same ? std::optional<ConstValue>(boolean(o == P::EqEq ? *same : !*same)) : std::optional<ConstValue>();
          }
          return std::optional<ConstValue>(); // a defaulted <=> as a value (a strong_ordering): not here
        }
        std::optional<ConstValue> self = a;
        if (ao && bo)
        {
          std::vector<std::optional<ConstPlace>> places{bo->place};
          std::vector<uint8_t> categories{bo->category};
          if (returned && constReturnsLRef(f, subst))
            return constReferred(constCallOn(f, subst, {b}, &self, depth + 1, ao->place ? &*ao->place : nullptr, &places, &categories, returned), *returned);
          return constCallOn(f, subst, {b}, &self, depth + 1, ao->place ? &*ao->place : nullptr, &places, &categories);
        }
        return constCallOn(f, subst, {b}, &self, depth + 1);
      }
    }
    FunctionDecl* free = chosenFree ? chosen : ranked ? ranked : constFreeFunction(opSymbols[uint32_t(o)], {constOperandType(a, ao), constOperandType(b, bo)}, ctx.scope);
    if (ao == lo && bo == ro)
      verify(o, free, false);
    if (FunctionDecl* f = free)
    {
      if (ao && bo)
      {
        std::vector<std::optional<ConstPlace>> places{ao->place, bo->place};
        std::vector<uint8_t> categories{ao->category, bo->category};
        if (returned && constReturnsLRef(f, Subst{}))
          return constReferred(constCallOn(f, Subst{}, {a, b}, nullptr, depth + 1, nullptr, &places, &categories, returned), *returned);
        return constCallOn(f, Subst{}, {a, b}, nullptr, depth + 1, nullptr, &places, &categories);
      }
      return constCallOn(f, Subst{}, {a, b}, nullptr, depth + 1);
    }
    return std::nullopt; // no such operator
  };
  if (std::optional<std::optional<ConstValue>> found = direct(op, l, r, lo, ro))
    return *found;
  if (enums)
    return constArith().binary(op, l, r); // (KPP_VERIFY_DECISIONS: no operator function takes them)
  if (op == P::EqEq || op == P::NotEq)
  {
    // `b == a` for `a == b`, `!(a == b)` for `a != b`, memberwise where `<=>` is defaulted.
    for (int reversed = 0; reversed < 2; ++reversed)
    {
      const ConstValue& a = reversed ? r : l;
      const ConstValue& b = reversed ? l : r;
      if (op == P::NotEq || reversed)
        if (std::optional<std::optional<ConstValue>> eq = direct(P::EqEq, a, b, reversed ? ro : lo, reversed ? lo : ro))
        {
          std::optional<bool> t = *eq ? constTruth(**eq, depth + 1) : std::nullopt;
          return t ? std::optional<ConstValue>(boolean(op == P::EqEq ? *t : !*t)) : std::nullopt;
        }
      if (a.isObject() && a.type && types.unqualified(a.type)->kind == TyKind::Class)
      {
        Subst subst;
        if (constDefaulted(constClassOperator(a.type, P::Spaceship, {constTypeOfValue(b)}, subst, depth)))
        {
          std::optional<bool> same = constMemberwiseEqual(a, b, ctx, depth + 1);
          return same ? std::optional<ConstValue>(boolean(op == P::EqEq ? *same : !*same)) : std::nullopt;
        }
      }
    }
    return std::nullopt;
  }
  if (comparison)
  {
    std::optional<int> order = constMemberwiseOrder(l, r, ctx, depth + 1); // a defaulted `<=>`
    if (!order)
      return std::nullopt;
    bool c = op == P::Less ? *order < 0 : op == P::Greater ? *order > 0 : op == P::LessEq ? *order <= 0 : *order >= 0;
    return boolean(c);
  }
  return std::nullopt;
}

auto GlobalScope::Impl::constOverrider(FunctionDecl* f, const Ty* t, int depth) -> FunctionDecl*
{
  if (!f || !(f->flags & (DF_Virtual | DF_Override)) || f->name.empty() || !t || depth > 16)
    return f;
  const ConstClassLayout* layout = constLayoutOf(t, depth);
  if (!layout)
    return f;
  if (layout->decl == f->parent)
    return f;
  for (Decl* m : membersRead(layout->decl))
  {
    if (m->kind != NodeKind::FunctionDecl || m == f)
      continue;
    auto* g = static_cast<FunctionDecl*>(m);
    if (g->name.empty() || g->name.last().symbol != f->name.last().symbol || g->params.size() != f->params.size() || (g->flags & DF_Static) ||
        ((g->flags & DF_Const) != (f->flags & DF_Const)))
      continue;
    bool same = true;
    for (size_t i = 0; i < g->params.size() && same; ++i)
    {
      const Ty* a = declaredTypeOf(g->params[i], layout->subst.empty() ? nullptr : &layout->subst, 0);
      const Ty* b = declaredTypeOf(f->params[i], nullptr, 0);
      same = a && b && types.unqualified(types.stripRef(a)) == types.unqualified(types.stripRef(b));
    }
    if (same)
      return g;
  }
  for (const Ty* b : layout->bases)
    if (FunctionDecl* g = constOverrider(f, b, depth + 1); g != f)
      return g;
  return f;
}

auto GlobalScope::Impl::constDyingPlaces() -> std::vector<ConstPlace>&
{
  static thread_local std::vector<ConstPlace> places;
  return places;
}

auto GlobalScope::Impl::constQualifiedCall() -> bool&
{
  static thread_local bool qualified = false;
  return qualified;
}

auto GlobalScope::Impl::constDynamicPlace(const ConstPlace& place, int depth) -> ConstPlace
{
  ConstPlace p = constResolve(place);
  if (p.path.empty() || !p.env || p.index >= p.env->size())
    return p;
  // Which of the path's steps are into a base part: the types along it from the binding's object.
  const ConstBinding& b = p.binding();
  const Ty* t = b.value.isObject() && b.value.type ? b.value.type : b.type;
  std::vector<bool> intoBase;
  for (size_t k = 0; k < p.path.size() && t; ++k)
  {
    t = types.unqualified(types.stripRef(t));
    if (t->kind == TyKind::Array)
    {
      intoBase.push_back(false);
      t = t->inner;
      continue;
    }
    const ConstClassLayout* layout = t->kind == TyKind::Class ? constLayoutOf(t, depth + 1) : nullptr;
    if (!layout)
      break;
    size_t i = p.path[k], nb = layout->bases.size();
    intoBase.push_back(i < nb);
    t = i < nb ? layout->bases[i] : i - nb < layout->fieldTypes.size() ? layout->fieldTypes[i - nb] : nullptr;
  }
  if (intoBase.size() != p.path.size())
    return p; // (a path the layouts do not lead along: as it is)
  auto dying = [&](size_t length) {
    for (const ConstPlace& d : constDyingPlaces())
      if (d.env == p.env && d.index == p.index && d.path.size() == length && std::equal(d.path.begin(), d.path.end(), p.path.begin()))
        return true;
    return false;
  };
  size_t keep = p.path.size();
  while (keep > 0 && intoBase[keep - 1] && !dying(keep))
    --keep;
  p.path.resize(keep);
  p.bits = 0;
  return p;
}

auto GlobalScope::Impl::constBaseDeclaring(const Ty* t, const Decl* cls, std::vector<size_t>& path, int depth) -> const Ty*
{
  const ConstClassLayout* layout = t && depth <= 16 ? constLayoutOf(t, depth) : nullptr;
  if (!layout)
    return nullptr;
  if (layout->decl == cls)
    return t;
  for (size_t b = 0; b < layout->bases.size(); ++b)
  {
    path.push_back(b);
    if (const Ty* found = constBaseDeclaring(layout->bases[b], cls, path, depth + 1))
      return found;
    path.pop_back();
  }
  return nullptr;
}

auto GlobalScope::Impl::constValuesMade() -> uint64_t&
{
  static thread_local uint64_t made = 0;
  return made;
}

auto GlobalScope::Impl::constMayMake(size_t n) -> bool
{
  if ((constValuesMade() += n) <= 4000000)
    return true;
  if (constWhy().empty())
    constWhy() = "an evaluation of more than four million values";
  return false;
}

auto GlobalScope::Impl::constInheritsConstructors(const ConstClassLayout& layout) -> bool
{
  for (Decl* m : membersRead(layout.decl))
    if (m->kind == NodeKind::UsingNameDecl)
    {
      const QualifiedName& name = static_cast<UsingNameDecl*>(m)->name;
      if (name.parts.size() >= 2 && name.last().symbol == name.parts[name.parts.size() - 2].symbol)
        return true;
    }
  return false;
}

auto GlobalScope::Impl::constFitBits(ConstValue v, const Ty* fieldType, unsigned bits) -> ConstValue
{
  if (v.kind != ConstValue::Kind::Int || bits == 0 || bits >= 64)
    return v;
  const Ty* t = fieldType ? types.unqualified(types.stripRef(fieldType)) : nullptr;
  if (t && t->kind == TyKind::Enum)
    t = constArith().underlying ? constArith().underlying(t) : nullptr;
  bool isSigned = !t || ((t->kind == TyKind::Int || t->kind == TyKind::Char) && !t->isUnsigned);
  uint64_t mask = (uint64_t(1) << bits) - 1, u = uint64_t(v.i) & mask;
  v.i = isSigned && (u >> (bits - 1)) ? int64_t(u | ~mask) : int64_t(u);
  return v;
}

auto GlobalScope::Impl::constFitBitFields(const ConstClassLayout& layout, std::vector<ConstValue>& parts) -> void
{
  size_t nb = layout.bases.size();
  for (size_t i = 0; i < layout.fields.size() && nb + i < parts.size(); ++i)
    if (layout.fieldBits[i])
      parts[nb + i] = constFitBits(parts[nb + i], layout.fieldTypes[i], layout.fieldBits[i]);
}

auto GlobalScope::Impl::constLayoutOf(const Ty* t, int depth) -> const ConstClassLayout*
{
  t = t ? types.unqualified(types.stripRef(t)) : nullptr;
  if (!t || t->kind != TyKind::Class || types.isDependent(t) || mentionsUnknown(t) || depth > 32)
    return nullptr;
  {
    std::lock_guard lock(constLayoutsMutex);
    if (auto it = constLayouts.find(t); it != constLayouts.end())
    {
      replayUses(it->second->uses);
      return it->second->ok ? it->second.get() : nullptr;
    }
  }
  auto layout = std::make_shared<ConstClassLayout>();
  CollectUses collected;
  Scope* s = classScopeOf(t);
  if (s && s->owner && s->owner->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(s->owner)->isDefinition)
  {
    auto* cls = static_cast<ClassDecl*>(s->owner);
    layout->decl = cls;
    layout->subst = substFor(t);
    const Subst* sp = layout->subst.empty() ? nullptr : &layout->subst;
    layout->ok = true;
    DeclContext dc;
    declContextFor(cls, dc);
    TypeContext bctx{dc.scope, &dc.params, sp, cls->file ? cls->file->tokens : nullptr, cls->file ? cls->file->text : std::string_view()};
    for (const ClassDecl::Base& b : cls->bases)
    {
      const Ty* bt = convertType(b.type, bctx, depth + 1);
      bt = bt ? types.unqualified(bt) : nullptr;
      if (!bt || bt->kind != TyKind::Class || types.isDependent(bt))
        layout->ok = false;
      layout->bases.push_back(bt);
    }
    for (Decl* m : membersRead(cls))
    {
      if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Constructor))
        layout->constructors.push_back(static_cast<FunctionDecl*>(m));
      if (m->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(m)->name.empty() && static_cast<ClassDecl*>(m)->isDefinition)
      {
        // An anonymous struct or union member: one part, its own class's object.
        const Ty* at = selfType(m);
        if (at && sp)
          at = substitute(at, *sp);
        if (!at || !at->isKnown() || types.isDependent(at))
          layout->ok = false;
        layout->anonymous = true;
        layout->fields.push_back(nullptr);
        layout->fieldTypes.push_back(at);
        layout->fieldBits.push_back(0);
        continue;
      }
      if (m->kind != NodeKind::VariableDecl || (m->flags & (DF_Static | DF_Typedef | DF_Friend)))
        continue;
      const Ty* ft = declaredTypeOf(m, sp, depth + 1);
      if (!ft || !ft->isKnown() || types.isDependent(ft))
        layout->ok = false;
      uint8_t bits = 0;
      if (Expr* width = static_cast<VariableDecl*>(m)->bitfield)
      {
        std::optional<int64_t> w = constEval(width, bctx, depth + 1);
        if (w && *w >= 0 && *w <= 64)
          bits = uint8_t(*w);
        else
          layout->ok = false;
      }
      layout->fields.push_back(static_cast<VariableDecl*>(m));
      layout->fieldTypes.push_back(ft);
      layout->fieldBits.push_back(bits);
    }
  }
  layout->uses = collected.uses;
  std::lock_guard lock(constLayoutsMutex);
  auto it = constLayouts.emplace(t, layout).first;
  return it->second->ok ? it->second.get() : nullptr;
}

auto GlobalScope::Impl::constFieldPath(const Ty* t, Decl* field, int depth) -> std::optional<std::vector<size_t>>
{
  const ConstClassLayout* layout = constLayoutOf(t, depth);
  if (!layout || depth > 16)
    return std::nullopt;
  for (size_t i = 0; i < layout->fields.size(); ++i)
    if (layout->fields[i] == field)
      return std::vector<size_t>{layout->bases.size() + i};
  for (size_t i = 0; i < layout->fields.size(); ++i)
    if (!layout->fields[i])
      if (std::optional<std::vector<size_t>> inner = constFieldPath(layout->fieldTypes[i], field, depth + 1))
      {
        inner->insert(inner->begin(), layout->bases.size() + i); // through the anonymous member
        return inner;
      }
  for (size_t b = 0; b < layout->bases.size(); ++b)
    if (std::optional<std::vector<size_t>> inner = constFieldPath(layout->bases[b], field, depth + 1))
    {
      inner->insert(inner->begin(), b);
      return inner;
    }
  return std::nullopt;
}

auto GlobalScope::Impl::constPart(const ConstValue& v, const std::vector<size_t>& path) -> std::optional<ConstValue>
{
  const ConstValue* at = &v;
  for (size_t index : path)
  {
    if (!at->isObject() || index >= at->parts->size())
      return std::nullopt;
    at = &(*at->parts)[index];
  }
  if (at->isUnset())
    return std::nullopt;
  return *at;
}

auto GlobalScope::Impl::constSetPart(ConstValue& v, const std::vector<size_t>& path, size_t from, const ConstValue& part) -> bool
{
  if (from == path.size())
  {
    v = part;
    return true;
  }
  if (!v.isObject() || path[from] >= v.parts->size())
    return false;
  std::vector<ConstValue> copy;
  std::vector<ConstValue>* written = nullptr;
  if (v.parts.use_count() == 1)
    written = const_cast<std::vector<ConstValue>*>(v.parts.get()); // ours alone: nobody else sees it change
  else
  {
    copy = *v.parts;
    written = &copy;
  }
  std::vector<ConstValue>& parts = *written;
  if (!constSetPart(parts[path[from]], path, from + 1, part))
    return false;
  // A union holds one member: writing one ends the others' lifetimes, so reading them is no
  // constant (C++20 [class.union]), never what was there.
  if (const Ty* u = v.type ? v.type : nullptr; u && u->kind == TyKind::Class && u->decl && u->decl->kind == NodeKind::ClassDecl &&
                                                static_cast<ClassDecl*>(u->decl)->keyword == Kw::union_)
    for (size_t i = 0; i < parts.size(); ++i)
      if (i != path[from])
        parts[i] = ConstValue::unset(parts[i].type);
  if (written == &copy)
    v.parts = std::make_shared<const std::vector<ConstValue>>(std::move(copy));
  return true;
}

auto GlobalScope::Impl::constTypeOfValue(const ConstValue& v) -> const Ty*
{
  if (v.type)
    return v.type;
  if (v.isObject())
    return nullptr; // `{}`: a brace list of no type, which decides no overload
  return v.isFloat() ? types.floatType(64) : types.intType(64, false);
}

auto GlobalScope::Impl::literalBytes(LiteralExpr* l, const ParsedFile* in, std::string& bytes) -> bool
{
  const ParsedFile* pf = l->file && l->file->tokens ? l->file : in;
  if (!pf || !pf->tokens)
    return false;
  const std::vector<Token>* toks = pf->tokens;
  if (l->token >= toks->size() || (*toks)[l->token].getKind() != TokenKind::String)
    return false;
  for (size_t i = l->token; i < toks->size() && (*toks)[i].getKind() == TokenKind::String; ++i)
  {
    std::string spliced;
    std::string_view spelled = tokenSpelling(pf->text, (*toks)[i], spliced);
    size_t quote = spelled.rfind('"');
    if (quote != std::string_view::npos && quote + 1 < spelled.size())
      spelled = spelled.substr(0, quote + 1); // (`"..."sv`: the chars the quotes hold)
    if (!decodeNarrowString(spelled, bytes))
      return false;
  }
  return true;
}

auto GlobalScope::Impl::constString(LiteralExpr* l, const TypeContext& ctx) -> std::optional<ConstValue>
{
  const std::vector<Token>* toks = l->file && l->file->tokens ? l->file->tokens : ctx.tokens ? ctx.tokens : fallbackTokens;
  std::string_view text = l->file && l->file->tokens ? l->file->text : ctx.tokens ? ctx.text : fallbackText;
  if (!toks || l->token >= toks->size() || (*toks)[l->token].getKind() != TokenKind::String)
    return std::nullopt; // another file's tokens: never a wrong string
  std::string bytes;
  for (size_t i = l->token; i < toks->size() && (*toks)[i].getKind() == TokenKind::String; ++i)
  {
    // `"type"sv`: the chars the quotes hold, as the walk types the literal (a suffix names no
    // operator it calls).
    std::string spliced;
    std::string_view spelled = tokenSpelling(text, (*toks)[i], spliced);
    size_t quote = spelled.rfind('"');
    if (quote != std::string_view::npos && quote + 1 < spelled.size())
    {
      bool identifier = true;
      for (char c : spelled.substr(quote + 1))
        identifier = identifier && (c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'));
      if (identifier)
        spelled = spelled.substr(0, quote + 1);
    }
    if (!decodeNarrowString(spelled, bytes))
      return std::nullopt;
  }
  if (!constMayMake(bytes.size() + 1))
    return std::nullopt;
  const Ty* ch = types.charType(8, false);
  std::vector<ConstValue> chars;
  chars.reserve(bytes.size() + 1);
  for (char c : bytes)
    chars.push_back(ConstValue::integer(int64_t(static_cast<signed char>(c)), ch));
  chars.push_back(ConstValue::integer(0, ch));
  // (The type first: an argument moving chars may be evaluated before one reading its size, as
  // clang-cl evaluates them, right to left. Until 2026-11-05 every literal was a `const char[]`
  // on Windows, its length 0: a `const char (&)[4]` parameter took none.)
  const Ty* type = types.array(types.qualified(ch, true, false), uint32_t(chars.size()));
  return ConstValue::object(type, std::move(chars));
}

auto GlobalScope::Impl::constEmpty(const Ty* t, bool zero, int depth, const ConstPlace* target) -> std::optional<ConstValue>
{
  t = t ? types.unqualified(types.stripRef(t)) : nullptr;
  if (!t || depth > 64)
    return std::nullopt;
  if (t->isArithmetic() || t->kind == TyKind::Enum || t->kind == TyKind::Pointer)
    return zero ? constConverted(ConstValue::integer(0), t) : std::optional<ConstValue>(ConstValue::unset(t));
  if (t->kind == TyKind::Array)
  {
    if (t->count == 0)
      return ConstValue::object(t, {}); // a flexible array member's: no elements, every index past the end
    if (!constMayMake(t->count))
      return std::nullopt;
    std::optional<ConstValue> element = constEmpty(t->inner, zero, depth + 1);
    if (!element)
      return std::nullopt;
    return ConstValue::object(t, std::vector<ConstValue>(t->count, *element));
  }
  const ConstClassLayout* layout = constLayoutOf(t, depth);
  if (!layout)
    return std::nullopt;
  // A user-provided default constructor (none visible: no constant): one without parameters, or
  // whose parameters all have default arguments (`Style(const Style* parent = nullptr, int width =
  // 7)`, in the declaration or in the definition after the class), which `T()` and `T x;` run with
  // those arguments, where the members' defaults were taken (a silent wrong answer: width 0, found
  // by thread B); two such are ambiguous, as in C++.
  FunctionDecl* defaultConstructor = nullptr;
  int defaultConstructors = 0;
  for (FunctionDecl* c : layout->constructors)
  {
    if ((c->flags & (DF_Default | DF_Delete)) || c->builtin || !c->templateParams.empty())
      continue;
    FunctionDecl* def = constDefinition(c);
    bool defaulted = true;
    for (size_t i = 0; i < c->params.size() && defaulted; ++i)
      defaulted = static_cast<ParamDecl*>(c->params[i])->defaultValue ||
                  (def && i < def->params.size() && static_cast<ParamDecl*>(def->params[i])->defaultValue);
    if (!defaulted)
      continue;
    defaultConstructor = c;
    ++defaultConstructors;
  }
  if (defaultConstructors > 1)
  {
    if (constWhy().empty())
      constWhy() = "a default-initialization of " + types.toString(t, &symbols) + ", which more than one constructor takes without arguments";
    return std::nullopt;
  }
  if (defaultConstructor)
    return constRunConstructor(defaultConstructor, t, {}, depth + 1, nullptr, nullptr, nullptr, target);
  std::vector<ConstValue> parts;
  if (!constMayMake(layout->bases.size() + layout->fields.size()))
    return std::nullopt;
  for (const Ty* b : layout->bases)
  {
    std::optional<ConstValue> part = constEmpty(b, zero, depth + 1);
    if (!part)
      return std::nullopt;
    parts.push_back(*part);
  }
  bool defaults = false;
  for (size_t i = 0; i < layout->fields.size(); ++i)
  {
    std::optional<ConstValue> part = constFieldDefault(*layout, i, zero, depth);
    defaults = defaults || (layout->fields[i] && layout->fields[i]->init);
    if (!part)
      return defaults ? constEmptyInPlace(t, *layout, zero, depth, target) : std::nullopt; // (one may read a member before it)
    parts.push_back(*part);
  }
  constFitBitFields(*layout, parts);
  return ConstValue::object(t, std::move(parts));
}

auto GlobalScope::Impl::constEmptyInPlace(const Ty* t, const ConstClassLayout& layout, bool zero, int depth, const ConstPlace* target) -> std::optional<ConstValue>
{
  // (The parts made again: what the first try made is no object anything refers to.)
  size_t nb = layout.bases.size(), nf = layout.fields.size();
  std::vector<ConstValue> unset;
  for (size_t b = 0; b < nb; ++b)
    unset.push_back(ConstValue::unset(layout.bases[b]));
  for (size_t i = 0; i < nf; ++i)
    unset.push_back(ConstValue::unset(layout.fieldTypes[i]));
  std::vector<ConstBinding> own;
  ConstFrame frame(&own);
  ConstPlace home;
  if (target)
  {
    if (!constWrite(*target, ConstValue::object(t, std::move(unset))))
      return std::nullopt;
    home = constResolve(*target);
  }
  else
  {
    own.push_back(constBinding(constThisSymbol, ConstValue::object(t, std::move(unset)), t));
    own.back().writable = true;
    home = ConstPlace{&own, 0, {}, t};
  }
  bool union_ = layout.decl && layout.decl->keyword == Kw::union_;
  for (size_t k = 0; k < nb + nf; ++k)
  {
    bool base = k < nb;
    if (union_ && !base && !(layout.fields[k - nb] && layout.fields[k - nb]->init))
      continue; // (a union's member without an initializer is none it holds: nothing made, not a String that was never destroyed)
    std::optional<ConstValue> part = base ? constEmpty(layout.bases[k], zero, depth + 1) : constFieldDefaultOn(layout, k - nb, zero, home, depth);
    ConstPlace at = home;
    at.path.push_back(k);
    at.type = base ? layout.bases[k] : layout.fieldTypes[k - nb];
    at.bits = base ? 0 : layout.fieldBits[k - nb];
    if (!part || (!(union_ && !(layout.fields[k - nb] && layout.fields[k - nb]->init)) && !constWrite(at, *part)))
      return std::nullopt; // (a union's member without an initializer is none it holds)
  }
  std::optional<ConstValue> built = constRead(home);
  if (!built || !built->isObject() || !built->parts || built->parts->size() != nb + nf)
    return std::nullopt;
  std::vector<ConstValue> parts(built->parts->begin(), built->parts->end());
  constFitBitFields(layout, parts);
  return ConstValue::object(t, std::move(parts));
}

auto GlobalScope::Impl::constFieldDefault(const ConstClassLayout& layout, size_t i, bool zero, int depth) -> std::optional<ConstValue>
{
  VariableDecl* f = layout.fields[i];
  if (!f || !f->init)
    return constEmpty(layout.fieldTypes[i], zero, depth + 1);
  LocalStack params;
  for (Decl* p : effectiveParams(layout.decl))
    params.push(static_cast<TemplateParamDecl*>(p)->name, p);
  TypeContext fctx{layout.decl->ownScope ? layout.decl->ownScope : lexicalScopeOf(f), &params, layout.subst.empty() ? nullptr : &layout.subst,
                   f->file ? f->file->tokens : nullptr, f->file ? f->file->text : std::string_view()};
  return constInitialize(layout.fieldTypes[i], constInitializerOf(f, layout.subst.empty() ? nullptr : &layout.subst), fctx, depth + 1);
}

auto GlobalScope::Impl::constFieldDefaultOn(const ConstClassLayout& layout, size_t i, bool zero, const ConstPlace& self, int depth) -> std::optional<ConstValue>
{
  VariableDecl* f = layout.fields[i];
  if (!f || !f->init)
    return constEmpty(layout.fieldTypes[i], zero, depth + 1);
  LocalStack params;
  for (Decl* p : effectiveParams(layout.decl))
    params.push(static_cast<TemplateParamDecl*>(p)->name, p);
  std::vector<ConstBinding> env;
  ConstBinding alias{constThisSymbol, ConstValue::unset(self.type)};
  alias.type = self.type;
  alias.aliasEnv = self.env;
  alias.aliasIndex = self.index;
  alias.aliasPath = self.path;
  env.push_back(std::move(alias));
  ConstFrame frame(&env);
  TypeContext fctx{layout.decl->ownScope ? layout.decl->ownScope : lexicalScopeOf(f), &params, layout.subst.empty() ? nullptr : &layout.subst,
                   f->file ? f->file->tokens : nullptr, f->file ? f->file->text : std::string_view()};
  fctx.env = &env;
  return constInitialize(layout.fieldTypes[i], constInitializerOf(f, layout.subst.empty() ? nullptr : &layout.subst), fctx, depth + 1);
}

auto GlobalScope::Impl::constCopyRunsCode(const Ty* t) -> bool
{
  t = t ? types.unqualified(types.stripRef(t)) : nullptr;
  while (t && t->kind == TyKind::Array)
    t = t->inner ? types.unqualified(t->inner) : nullptr;
  if (!t || t->kind != TyKind::Class || !t->isKnown() || types.isDependent(t))
    return false;
  static thread_local std::unordered_map<const Ty*, bool> known;
  if (auto it = known.find(t); it != known.end())
    return it->second;
  std::optional<bool> copy = constructibleFromSelf(t, false, TraitForm::Trivially);
  std::optional<bool> move = constructibleFromSelf(t, true, TraitForm::Trivially);
  bool runs = (copy && !*copy) || (move && !*move);
  known.emplace(t, runs);
  return runs;
}

auto GlobalScope::Impl::constCategory(Expr* e, Expr*& source) -> uint8_t
{
  while (e && e->kind == NodeKind::ParenExpr)
    e = static_cast<ParenExpr*>(e)->inner;
  source = e;
  if (!e)
    return 0;
  // `y = x`: the left object, an lvalue, as the built-in and the implicit operator= give it (the
  // walker types the implicit one as the class itself), never a temporary destroyed after the
  // statement.
  if (e->kind == NodeKind::BinaryExpr && !static_cast<BinaryExpr*>(e)->isKeyword && isAssignmentOperator(P(static_cast<BinaryExpr*>(e)->op)))
    return 1;
  if (e->kind == NodeKind::NamedCastExpr)
  {
    auto* c = static_cast<NamedCastExpr*>(e);
    if (c->castKind == Kw::static_cast_ && c->type && c->type->kind == NodeKind::RRefType)
    {
      source = c->operand;
      return 2;
    }
  }
  if (e->kind == NodeKind::CallExpr)
  {
    auto* c = static_cast<CallExpr*>(e);
    if (c->args.size() == 1 && c->callee && c->callee->kind == NodeKind::NameExpr)
    {
      const QualifiedName& n = static_cast<NameExpr*>(c->callee)->name;
      std::string_view name = n.empty() ? std::string_view() : symbols.name(n.last().symbol);
      if (name == "move" || (name == "forward" && !(e->ty && e->ty->kind == TyKind::LRef)))
      {
        source = c->args[0];
        return 2;
      }
    }
  }
  if (isLvalueExpression(e) || (e->ty && e->ty->kind == TyKind::LRef))
    return 1;
  return e->ty && e->ty->kind == TyKind::RRef ? 2 : 0;
}

auto GlobalScope::Impl::constCopyConstruct(const Ty* t, const ConstValue& v, const std::optional<ConstPlace>& at, bool moving, int depth, const ConstPlace* target) -> std::optional<ConstValue>
{
  t = types.unqualified(types.stripRef(t));
  if (!constCopyRunsCode(t) || !v.isObject())
    return v;
  if (t->kind == TyKind::Array)
  {
    std::vector<ConstValue> elements;
    for (size_t i = 0; i < v.parts->size(); ++i)
    {
      std::optional<ConstPlace> part = at;
      if (part)
      {
        part->path.push_back(i);
        part->type = t->inner; // (the element's: a constructor's reference parameter binds it as one)
        part->bits = 0;
      }
      std::optional<ConstValue> e = constCopyConstruct(t->inner, (*v.parts)[i], part, moving, depth + 1);
      if (!e)
        return std::nullopt;
      elements.push_back(*e);
    }
    return ConstValue::object(t, std::move(elements));
  }
  const ConstClassLayout* layout = constLayoutOf(t, depth);
  ClassDecl* def = classDefinition(t);
  if (!layout || !def || depth > 64)
    return std::nullopt;
  SpecialMembers special = specialMembersOf(def);
  FunctionDecl* ctor = moving && special.moveCtor ? special.moveCtor : special.copyCtor ? special.copyCtor : special.copyCtorNonConst;
  if (ctor && (ctor->flags & DF_Delete))
    return std::nullopt;
  if (ctor && !constDefaulted(ctor))
  {
    std::vector<std::optional<ConstPlace>> places{at};
    return constRunConstructor(ctor, t, {v}, depth + 1, nullptr, nullptr, &places, target);
  }
  std::vector<ConstValue> parts;
  size_t n = layout->bases.size() + layout->fields.size();
  if (v.parts->size() != n)
    return std::nullopt;
  for (size_t i = 0; i < n; ++i)
  {
    const Ty* partType = i < layout->bases.size() ? layout->bases[i] : layout->fieldTypes[i - layout->bases.size()];
    std::optional<ConstPlace> part = at;
    if (part)
    {
      part->path.push_back(i);
      part->type = partType; // (the member's: a constructor's reference parameter binds it as one)
      part->bits = i < layout->bases.size() ? 0 : layout->fieldBits[i - layout->bases.size()];
    }
    std::optional<ConstValue> e = (*v.parts)[i].isObject() ? constCopyConstruct(partType, (*v.parts)[i], part, moving, depth + 1)
                                                          : std::optional<ConstValue>((*v.parts)[i]);
    if (!e)
      return std::nullopt;
    parts.push_back(*e);
  }
  return ConstValue::object(t, std::move(parts));
}

auto GlobalScope::Impl::constAssignRunsCode(const Ty* t) -> bool
{
  t = t ? types.unqualified(types.stripRef(t)) : nullptr;
  while (t && t->kind == TyKind::Array)
    t = t->inner ? types.unqualified(t->inner) : nullptr;
  if (!t || t->kind != TyKind::Class || !t->isKnown() || types.isDependent(t))
    return false;
  static thread_local std::unordered_map<const Ty*, bool> known;
  if (auto it = known.find(t); it != known.end())
    return it->second;
  std::optional<bool> copy = assignableFromSelf(t, false, TraitForm::Trivially);
  std::optional<bool> move = assignableFromSelf(t, true, TraitForm::Trivially);
  bool runs = (copy && !*copy) || (move && !*move);
  known.emplace(t, runs);
  return runs;
}

auto GlobalScope::Impl::constAssignObject(const ConstPlace& place, const Ty* t, const ConstValue& v, const std::optional<ConstPlace>& from, uint8_t category, int depth) -> bool
{
  t = types.unqualified(types.stripRef(t));
  if (!v.isObject() || !constAssignRunsCode(t))
    return constWrite(place, v);
  if (depth > 64)
    return false;
  auto partOf = [](const std::optional<ConstPlace>& whole, size_t i, const Ty* type) {
    std::optional<ConstPlace> part = whole;
    if (part)
    {
      part->path.push_back(i);
      part->type = type;
      part->bits = 0;
    }
    return part;
  };
  if (t->kind == TyKind::Array)
  {
    for (size_t i = 0; i < v.parts->size(); ++i)
      if (!constAssignObject(*partOf(place, i, t->inner), t->inner, (*v.parts)[i], partOf(from, i, t->inner), category, depth + 1))
        return false;
    return true;
  }
  // The part's own copy or move assignment, as the right operand's category chooses it.
  Subst subst;
  ClassDecl* def = classDefinition(t);
  FunctionDecl* f = nullptr;
  if (def)
    for (FunctionDecl* c : constMembersNamed(t, opSymbols[uint32_t(P::Assign)], subst, depth))
      if (c->params.size() == 1)
      {
        int kind = selfParameter(c->params[0], def);
        if (kind == 3 ? category != 1 : kind != 0 && (category == 1 || !f))
          f = c;
      }
  if (f && (f->flags & DF_Delete))
    return false;
  if (f && !constDefaulted(f))
  {
    if (!constDefinition(f))
      return false;
    std::vector<std::optional<ConstPlace>> places{from};
    std::vector<uint8_t> categories{category};
    std::optional<ConstValue> self = constRead(place);
    return self && constCallOn(f, subst, {v}, &self, depth + 1, &place, &places, &categories);
  }
  const ConstClassLayout* layout = constLayoutOf(t, depth);
  size_t n = layout ? layout->bases.size() + layout->fields.size() : 0;
  if (!layout || !v.parts || v.parts->size() != n)
    return false;
  for (size_t i = 0; i < n; ++i)
  {
    const Ty* partType = i < layout->bases.size() ? layout->bases[i] : layout->fieldTypes[i - layout->bases.size()];
    std::optional<ConstPlace> to = partOf(place, i, partType);
    if (i >= layout->bases.size())
      to->bits = 0; // (the value's part is already fitted to a bit field)
    if (!constAssignObject(*to, partType, (*v.parts)[i], partOf(from, i, partType), category, depth + 1))
      return false;
  }
  return true;
}

auto GlobalScope::Impl::constCopyInit(Expr* init, const Ty* t, const TypeContext& ctx, int depth, bool moving, std::optional<ConstValue>* evaluated, const ConstPlace* target)
  -> std::optional<std::optional<ConstValue>>
{
  if (!init || !constCopyRunsCode(t))
    return std::nullopt;
  Expr* source = init;
  uint8_t category = constCategory(init, source);
  if (category == 0 && !moving)
    return std::nullopt; // a prvalue: the object itself
  moving = moving || category == 2;
  std::optional<ConstValue> v = constValue(source, ctx, depth + 1);
  if (!v || !v->isObject() || !v->type || types.unqualified(types.stripRef(v->type)) != types.unqualified(types.stripRef(t)))
  {
    // Another class: its converting constructor, as before, from this value (`move(x)` and
    // `forward<T>(x)` give x's), never from init evaluated again.
    Expr* bare = init;
    while (bare && bare->kind == NodeKind::ParenExpr)
      bare = static_cast<ParenExpr*>(bare)->inner;
    if (evaluated && v && (source == bare || bare->kind == NodeKind::CallExpr))
      *evaluated = v;
    return std::nullopt;
  }
  std::optional<ConstPlace> at = constPlaceOf(source, ctx, depth + 1);
  if (category == 2 && !at)
    return std::optional<std::optional<ConstValue>>(std::optional<ConstValue>()); // (moved where it is not known: no constant)
  return std::optional<std::optional<ConstValue>>(constCopyConstruct(t, *v, at, moving, depth + 1, target));
}

auto GlobalScope::Impl::constIsObjectOf(const ConstValue& v, const Ty* u) -> bool
{
  return u && v.isObject() && v.type && !v.type->isBraceList() && types.unqualified(types.stripRef(v.type)) == types.unqualified(types.stripRef(u));
}

auto GlobalScope::Impl::constFromObject(const Ty* u, const ConstValue& v, const std::optional<ConstPlace>& at, uint8_t category, int depth, const ConstPlace* target)
  -> std::optional<ConstValue>
{
  if (category == 0)
    return v; // a prvalue: the object itself
  if (category == 2 && !at && constCopyRunsCode(u))
    return std::nullopt; // its move would leave the object it names as it was
  return constCopyConstruct(u, v, at, category == 2, depth + 1, target);
}

auto GlobalScope::Impl::constClassCast(FunctionalCastExpr* f, const Ty* t, const TypeContext& ctx, int depth, const ConstPlace* target)
  -> std::optional<std::optional<ConstValue>>
{
  if (!t || t->kind != TyKind::Class)
    return std::nullopt;
  bool noValue = f->args.empty() || (f->args.size() == 1 && f->args[0] && f->args[0]->kind == NodeKind::BraceInitExpr &&
                                     static_cast<BraceInitExpr*>(f->args[0])->args.empty());
  if (noValue)
  {
    if (!target)
      if (std::optional<ConstValue> b = traitObjectValue(t, depth + 1))
        return std::optional<std::optional<ConstValue>>(b); // `HasSize<T>{}`: a trait object as bool
    return std::optional<std::optional<ConstValue>>(constEmpty(t, true, depth + 1, target));
  }
  // `Point(1, 2)`, `Point{1, 2}`: the constructor's object, or an aggregate's.
  if (f->braced || (f->args.size() == 1 && f->args[0] && f->args[0]->kind == NodeKind::BraceInitExpr))
  {
    BraceInitExpr list{};
    list.kind = NodeKind::BraceInitExpr;
    list.token = f->token;
    list.args = f->args.size() == 1 && f->args[0]->kind == NodeKind::BraceInitExpr ? static_cast<BraceInitExpr*>(f->args[0])->args : f->args;
    return std::optional<std::optional<ConstValue>>(constBraceInit(types.unqualified(t), &list, ctx, depth + 1, target));
  }
  return std::optional<std::optional<ConstValue>>(constConstructFrom(t, f->args, ctx, depth, target)); // `T b(a)` copies a by T's copy constructor
}

auto GlobalScope::Impl::constConstructFrom(const Ty* t, Span<Expr*> args, const TypeContext& ctx, int depth, const ConstPlace* target) -> std::optional<ConstValue>
{
  std::vector<ConstValue> values;
  std::vector<std::optional<ConstPlace>> places;
  std::vector<uint8_t> categories;
  if (!constArguments(args, ctx, depth, values, &places, &categories))
    return std::nullopt;
  const Ty* u = types.unqualified(types.stripRef(t));
  if (values.size() == 1 && places.size() == 1 && categories.size() == 1 && constIsObjectOf(values[0], u))
    return constFromObject(u, values[0], places[0], categories[0], depth, target);
  return constConstruct(u, values, false, depth + 1, values.size() == args.size() ? &args : nullptr, target, values.size() == places.size() ? &places : nullptr,
                        values.size() == categories.size() ? &categories : nullptr);
}

auto GlobalScope::Impl::constInitialize(const Ty* t, Expr* init, const TypeContext& ctx, int depth, const ConstPlace* target) -> std::optional<ConstValue>
{
  return constOutermost([&] { return constInitializeIn(t, init, ctx, depth, target); }, depth);
}

auto GlobalScope::Impl::constInitializeIn(const Ty* t, Expr* init, const TypeContext& ctx, int depth, const ConstPlace* target) -> std::optional<ConstValue>
{
  const Ty* u = t ? types.unqualified(types.stripRef(t)) : nullptr;
  if (!init || depth > 512)
    return std::nullopt;
  if (u && (u->kind == TyKind::Class || u->kind == TyKind::Array))
  {
    if (init->kind == NodeKind::BraceInitExpr)
    {
      auto* b = static_cast<BraceInitExpr*>(init);
      return u->kind == TyKind::Array ? constArrayInit(u, b->args, ctx, depth) : constBraceInit(u, b, ctx, depth, target);
    }
    // An object of another class converted by u's converting constructor, the one the value as
    // written chooses, its argument bound by its category: `Optional<String> o = s;` takes s by
    // `const T&`, where it is (by the value alone, an rvalue: `Optional(T&&)` moved from s, a String
    // two objects then freed); `Optional<String> o = String("x");` binds `T&& v` to a temporary of
    // the full expression, which the move leaves empty and that end destroys (it was a copy no one
    // destroyed: "memory allocated at String.kpp:657 is never freed").
    auto otherClass = [&](const std::optional<ConstValue>& value) {
      return !t->isReference() && u->kind == TyKind::Class && value && value->isObject() && value->type && !value->type->isBraceList() &&
             types.unqualified(types.stripRef(value->type)) != u;
    };
    auto converted = [&](const ConstValue& value) {
      Expr* source = nullptr;
      std::vector<std::optional<ConstPlace>> places{constPure(init) ? constPlaceOf(init, ctx, depth + 1) : std::nullopt};
      std::vector<uint8_t> categories{constCategory(init, source)};
      Span<Expr*> written{&init, 1};
      return constConstruct(u, {value}, true, depth + 1, &written, target, &places, &categories);
    };
    if (!t->isReference())
    {
      std::optional<ConstValue> evaluated;
      if (std::optional<std::optional<ConstValue>> copied = constCopyInit(init, u, ctx, depth, false, &evaluated, target))
        return *copied;
      if (otherClass(evaluated))
        return converted(*evaluated);
      if (evaluated)
        return constConvertTo(evaluated, u, depth); // (evaluated once: `C c = *it++;`)
      // `T x(args)`, `T x = T(args)`: the constructor run where the object lives (target).
      Expr* bare = init;
      while (bare && bare->kind == NodeKind::ParenExpr)
        bare = static_cast<ParenExpr*>(bare)->inner;
      if (target && bare && bare->kind == NodeKind::FunctionalCastExpr && u->kind == TyKind::Class)
        if (std::optional<std::optional<ConstValue>> made = constClassCast(static_cast<FunctionalCastExpr*>(bare), u, ctx, depth, target))
          return *made;
      if (target && bare && bare->kind == NodeKind::CallExpr && u->kind == TyKind::Class)
      {
        // `Ring q = Ring(4);`: a call naming the class (constCall's class branch), of this class.
        auto* call = static_cast<CallExpr*>(bare);
        auto* callee = call->callee && call->callee->kind == NodeKind::NameExpr ? static_cast<NameExpr*>(call->callee) : nullptr;
        Decl* d = callee ? callee->resolved : nullptr;
        bool namesClass = d && !callee->name.empty() &&
                          (d->kind == NodeKind::ClassDecl || (d->kind == NodeKind::UsingAliasDecl && !callee->name.last().hasTemplateArgs) ||
                           (d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef)));
        if (namesClass && call->ty && types.unqualified(types.stripRef(call->ty)) == u)
          return call->args.empty() ? constEmpty(u, true, depth + 1, target) : constConstructFrom(u, call->args, ctx, depth, target);
      }
    }
    if (std::optional<ConstValue> chars = constCharArrayFromLiteral(u, init, ctx, depth))
      return chars;
    std::optional<ConstValue> value = constValue(init, ctx, depth + 1);
    if (otherClass(value))
      return converted(*value);
    return constConvertTo(value, u, depth);
  }
  std::optional<ConstValue> v = constInitValue(init, ctx, depth);
  if (v && u && v->isObject() && v->type && !v->type->isBraceList() && types.unqualified(types.stripRef(v->type))->kind == TyKind::Class &&
      (u->isArithmetic() || u->kind == TyKind::Enum || u->kind == TyKind::Pointer))
    return constByConversionOperator(*v, t, depth); // `int x = result;`
  return constAsDeclared(v, t);
}

auto GlobalScope::Impl::constCharArrayFromLiteral(const Ty* t, Expr* init, const TypeContext& ctx, int depth) -> std::optional<ConstValue>
{
  while (init && init->kind == NodeKind::ParenExpr)
    init = static_cast<ParenExpr*>(init)->inner;
  const Ty* element = t->kind == TyKind::Array && t->inner ? types.unqualified(t->inner) : nullptr;
  if (!element || element->kind != TyKind::Char || !init || init->kind != NodeKind::LiteralExpr ||
      static_cast<LiteralExpr*>(init)->literalKind != TokenKind::String)
    return std::nullopt;
  std::optional<ConstValue> literal = constValue(init, ctx, depth + 1);
  if (!literal || !literal->isObject() || !literal->parts)
    return std::nullopt;
  size_t count = t->count ? t->count : literal->parts->size();
  if (literal->parts->size() > count + 1 || (literal->parts->size() == count + 1 && literal->parts->back().i != 0))
    return std::nullopt;
  std::vector<ConstValue> chars;
  for (size_t i = 0; i < count; ++i)
    chars.push_back(ConstValue::integer(i < literal->parts->size() ? (*literal->parts)[i].i : 0, element));
  return ConstValue::object(t->count ? t : types.array(element, uint32_t(count)), std::move(chars));
}

auto GlobalScope::Impl::constArrayInit(const Ty* t, Span<Expr*> values, const TypeContext& ctx, int depth) -> std::optional<ConstValue>
{
  size_t next = 0;
  std::optional<ConstValue> v = constElided(t, values, next, ctx, depth);
  if (v && next < values.size())
    return std::nullopt; // more values than the array holds
  return v;
}

auto GlobalScope::Impl::constElided(const Ty* t, Span<Expr*> values, size_t& next, const TypeContext& ctx, int depth) -> std::optional<ConstValue>
{
  t = types.unqualified(types.stripRef(t));
  if (depth > 64)
    return std::nullopt;
  auto element = [&](const Ty* et) -> std::optional<ConstValue> {
    // An aggregate's reference member (`View v{a}`): the address of what the value names.
    if (et && et->isReference() && next < values.size() && values[next] && values[next]->kind != NodeKind::BraceInitExpr)
      if (std::optional<std::optional<ConstValue>> referred = constReferenceTo(values[next], et, ctx, depth + 1))
      {
        ++next;
        return *referred;
      }
    const Ty* eu = types.unqualified(types.stripRef(et));
    if (next >= values.size())
      return constEmpty(eu, true, depth + 1);
    Expr* x = values[next];
    bool charArray = eu->kind == TyKind::Array && eu->inner && types.unqualified(eu->inner)->kind == TyKind::Char;
    bool nested = (eu->kind == TyKind::Array || (eu->kind == TyKind::Class && constIsAggregate(eu, depth))) && x->kind != NodeKind::BraceInitExpr &&
                  !(charArray && x->kind == NodeKind::LiteralExpr && static_cast<LiteralExpr*>(x)->literalKind == TokenKind::String) &&
                  !(x->ty && types.unqualified(types.stripRef(x->ty)) == eu);
    if (nested)
      return constElided(eu, values, next, ctx, depth + 1); // `Array<int, 3> a = {1, 2, 3}`: the braces of items elided
    ++next;
    return constInitialize(eu, x, ctx, depth + 1);
  };
  if (t->kind == TyKind::Array)
  {
    size_t count = t->count ? t->count : values.size() - std::min(next, values.size());
    if (!constMayMake(count))
      return std::nullopt;
    std::vector<ConstValue> elements;
    for (size_t i = 0; i < count; ++i)
    {
      std::optional<ConstValue> e = element(t->inner);
      if (!e)
        return std::nullopt;
      elements.push_back(*e);
    }
    const Ty* at = t->count ? t : types.array(t->inner, uint32_t(count));
    return ConstValue::object(at, std::move(elements));
  }
  const ConstClassLayout* layout = constLayoutOf(t, depth);
  if (!layout || !constIsAggregate(t, depth) || !constMayMake(layout->bases.size() + layout->fields.size()))
    return std::nullopt;
  std::vector<ConstValue> parts;
  for (const Ty* b : layout->bases)
  {
    std::optional<ConstValue> part = element(b);
    if (!part)
      return std::nullopt;
    parts.push_back(*part);
  }
  for (size_t i = 0; i < layout->fields.size(); ++i)
  {
    std::optional<ConstValue> part = constUnnamedField(*layout, i) ? constEmpty(layout->fieldTypes[i], true, depth + 1)
                                    : next < values.size()        ? element(layout->fieldTypes[i])
                                                                  : constFieldDefault(*layout, i, true, depth);
    if (!part)
      return std::nullopt;
    parts.push_back(*part);
  }
  constFitBitFields(*layout, parts);
  return ConstValue::object(t, std::move(parts));
}

auto GlobalScope::Impl::constIsAggregate(const Ty* t, int depth) -> bool
{
  const ConstClassLayout* layout = constLayoutOf(t, depth);
  if (!layout || !layout->constructors.empty() || constInheritsConstructors(*layout) || layout->anonymous)
    return false;
  for (Decl* m : membersRead(layout->decl))
    if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Virtual))
      return false;
  return true;
}

auto GlobalScope::Impl::constBraceInit(const Ty* t, BraceInitExpr* b, const TypeContext& ctx, int depth, const ConstPlace* target) -> std::optional<ConstValue>
{
  bool expands = false;
  for (Expr* a : b->args)
    expands = expands || constIsExpansion(a);
  if (expands)
  {
    // `Point{args...}`: the values the expansion gives, as a brace list's (no brace elision).
    std::vector<std::optional<ConstPlace>> places;
    std::vector<uint8_t> categories;
    std::optional<ConstValue> list = constBraceList(b, ctx, depth, &places, &categories);
    return list ? constFromBraceList(*list, t, depth, nullptr, &places, &categories) : std::nullopt;
  }
  if (constIsAggregate(t, depth))
  {
    // `W b{a}` with a an object of the aggregate itself (as the walk typed it): a copy of a, as
    // C++ makes it ([dcl.init.list] 3.2), not its first member initialized from it.
    if (Expr* a = b->args.size() == 1 && b->designators.empty() ? b->args[0] : nullptr;
        a && a->kind != NodeKind::BraceInitExpr && !constIsExpansion(a) && a->ty && a->ty->isKnown() && !types.isDependent(a->ty) &&
        types.unqualified(types.stripRef(a->ty)) == types.unqualified(t))
    {
      std::vector<ConstValue> values;
      std::vector<std::optional<ConstPlace>> places;
      std::vector<uint8_t> categories;
      if (!constArguments(b->args, ctx, depth, values, &places, &categories) || values.size() != 1 || places.size() != 1 ||
          categories.size() != 1 || !constIsObjectOf(values[0], t))
        return std::nullopt;
      return constFromObject(types.unqualified(t), values[0], places[0], categories[0], depth);
    }
    if (b->designators.empty())
      return constArrayInit(t, b->args, ctx, depth);
    // `{.x = 1, .z = 3}`: each member its designator names, the rest from their defaults.
    const ConstClassLayout* layout = constLayoutOf(t, depth);
    if (!layout || !layout->bases.empty() || layout->anonymous)
      return std::nullopt;
    std::vector<ConstValue> parts;
    for (size_t i = 0; i < layout->fields.size(); ++i)
    {
      Expr* given = nullptr;
      for (size_t k = 0; k < b->args.size() && k < b->designators.size(); ++k)
        if (!layout->fields[i]->name.empty() && b->designators[k] == layout->fields[i]->name.last().symbol)
          given = b->args[k];
      std::optional<ConstValue> part = given ? constInitialize(layout->fieldTypes[i], given, ctx, depth + 1) : constFieldDefault(*layout, i, true, depth);
      if (!part)
        return std::nullopt;
      parts.push_back(*part);
    }
    constFitBitFields(*layout, parts);
    return ConstValue::object(t, std::move(parts));
  }
  // Not an aggregate: an InitializerList, a class taking the list by an InitializerList
  // constructor, or the constructor the elements choose (`T b{a}` copies a by T's own).
  std::vector<std::optional<ConstPlace>> places;
  std::vector<uint8_t> categories;
  std::optional<ConstValue> list = constBraceList(b, ctx, depth, &places, &categories);
  return list ? constFromBraceList(*list, t, depth, &b->args, &places, &categories, target) : std::nullopt;
}

auto GlobalScope::Impl::constConvertTo(std::optional<ConstValue> v, const Ty* t, int depth) -> std::optional<ConstValue>
{
  if (!v || !t)
    return v;
  if (constIsBraceList(*v))
    return constFromBraceList(*v, t, depth);
  const Ty* u = types.unqualified(types.stripRef(t));
  if (u->kind != TyKind::Class)
  {
    if (v->isObject() && v->type && !v->type->isBraceList() && types.unqualified(types.stripRef(v->type))->kind == TyKind::Class &&
        (u->isArithmetic() || u->kind == TyKind::Enum || u->kind == TyKind::Pointer))
      return constByConversionOperator(*v, t, depth); // `int x = result;`
    return constAsDeclared(v, t);
  }
  if (v->isObject() && v->type && types.unqualified(types.stripRef(v->type)) == u)
    return v;
  if (v->isObject() && v->type)
    if (std::optional<std::vector<size_t>> base = constBasePath(v->type, u, 0); base && !base->empty())
    {
      if (t->isReference())
        return v; // a reference to a base refers to the whole object: its virtual functions stay its class's
      return constPart(*v, *base); // a copy of the base subobject (slicing)
    }
  return constConstruct(u, {*v}, true, depth);
}

auto GlobalScope::Impl::constTypedUnion(const Ty* t, const ConstClassLayout& layout, const std::vector<ConstValue>& args, int depth) -> std::optional<ConstValue>
{
  ClassDecl* def = layout.decl;
  size_t tagAt = SIZE_MAX, storageAt = SIZE_MAX;
  for (size_t i = 0; i < layout.fields.size(); ++i)
    if (layout.fields[i] && layout.fields[i] == def->tagField)
      tagAt = i;
    else if (!layout.fields[i])
      storageAt = i;
  const ConstClassLayout* storage = storageAt != SIZE_MAX ? constLayoutOf(layout.fieldTypes[storageAt], depth + 1) : nullptr;
  if (args.empty() || tagAt == SIZE_MAX || !storage || !layout.bases.empty())
    return std::nullopt;
  const Ty* tagTy = types.unqualified(layout.fieldTypes[tagAt]);
  auto alternativeType = [&](size_t k) -> const Ty* {
    for (size_t i = 0; i < storage->fields.size(); ++i)
      if (storage->fields[i] == def->alternatives[k])
        return storage->fieldTypes[i];
    return nullptr;
  };
  size_t which = SIZE_MAX;
  std::vector<ConstValue> rest;
  if (args[0].kind == ConstValue::Kind::Int && args[0].type && types.unqualified(args[0].type) == tagTy)
  {
    which = size_t(args[0].i); // `U(U::Text, x)`
    rest.assign(args.begin() + 1, args.end());
  }
  else if (args.size() == 1)
  {
    const Ty* given = args[0].type ? types.unqualified(types.stripRef(args[0].type)) : nullptr;
    size_t found = 0;
    for (size_t k = 0; k < def->alternatives.size(); ++k)
      if (const Ty* at = alternativeType(k); given && at && types.unqualified(at) == given)
      {
        which = k;
        ++found;
      }
    if (found != 1)
      return std::nullopt; // no alternative, or several, of the value's type
    rest = args;
  }
  const Ty* alternative = which < def->alternatives.size() ? alternativeType(which) : nullptr;
  if (!alternative)
    return std::nullopt;
  const Ty* au = types.unqualified(alternative);
  std::optional<ConstValue> value = rest.size() == 1 && rest[0].type && types.unqualified(types.stripRef(rest[0].type)) == au
                                      ? constConvertTo(rest[0], au, depth + 1)
                                  : au->kind == TyKind::Class ? constConstruct(au, rest, false, depth + 1)
                                  : rest.size() == 1           ? constConvertTo(rest[0], au, depth + 1)
                                                               : std::nullopt;
  if (!value)
    return std::nullopt;
  std::vector<ConstValue> alternatives;
  for (size_t i = 0; i < storage->fields.size(); ++i)
    alternatives.push_back(storage->fields[i] == def->alternatives[which] ? *value : ConstValue::unset(storage->fieldTypes[i]));
  std::vector<ConstValue> parts;
  for (size_t i = 0; i < layout.fields.size(); ++i)
  {
    std::optional<ConstValue> part = i == tagAt     ? constArith().convert(ConstValue::integer(int64_t(which)), tagTy)
                                   : i == storageAt ? std::optional<ConstValue>(ConstValue::object(layout.fieldTypes[storageAt], std::move(alternatives)))
                                                    : constFieldDefault(layout, i, true, depth);
    if (!part)
      return std::nullopt;
    parts.push_back(*part);
  }
  return ConstValue::object(t, std::move(parts));
}

auto GlobalScope::Impl::constConstruct(const Ty* t, const std::vector<ConstValue>& args, bool implicitOnly, int depth, const Span<Expr*>* written, const ConstPlace* target,
                                        const std::vector<std::optional<ConstPlace>>* places, const std::vector<uint8_t>* categories) -> std::optional<ConstValue>
{
  t = types.unqualified(types.stripRef(t));
  const ConstClassLayout* layout = constLayoutOf(t, depth);
  if (!layout || depth > 512)
    return std::nullopt;
  if (args.size() == 1 && args[0].isObject() && args[0].type && types.unqualified(types.stripRef(args[0].type)) == t)
    return args[0]; // a copy (the interpreter's values are copied by value)
  if (layout->decl && layout->decl->isTypedUnion)
    return constTypedUnion(t, *layout, args, depth);
  if (layout->constructors.empty() && !constInheritsConstructors(*layout))
  {
    if (implicitOnly && !args.empty())
      return std::nullopt;
    if (layout->anonymous && !args.empty())
      return std::nullopt; // (an aggregate with an anonymous member: not initialized from values here)
    if (args.empty())
      return constEmpty(t, true, depth + 1);
    std::vector<ConstValue> parts;
    size_t next = 0;
    for (const Ty* b : layout->bases)
    {
      std::optional<ConstValue> part = next < args.size() ? constConvertTo(args[next++], b, depth + 1) : constEmpty(b, true, depth + 1);
      if (!part)
        return std::nullopt;
      parts.push_back(*part);
    }
    for (size_t i = 0; i < layout->fields.size(); ++i)
    {
      std::optional<ConstValue> part = constUnnamedField(*layout, i) ? constEmpty(layout->fieldTypes[i], true, depth + 1)
                                      : next < args.size()          ? constConvertTo(args[next++], layout->fieldTypes[i], depth + 1)
                                                                    : constFieldDefault(*layout, i, true, depth);
      if (!part)
        return std::nullopt;
      parts.push_back(*part);
    }
    if (next < args.size())
      return std::nullopt;
    constFitBitFields(*layout, parts);
    return ConstValue::object(t, std::move(parts));
  }
  std::vector<const Ty*> argTypes;
  for (const ConstValue& a : args)
    argTypes.push_back(constTypeOfValue(a));
  // The constructor the arguments choose: the one rule (design.md 24, chooseConstructor), as codegen
  // runs it, among the class's own constructors and those it inherits by `using Base::Base;` (for an
  // implicit conversion, those that convert: as the walk converts, all but those explicit, or
  // explicit(cond) with cond true for the class's arguments; isExplicit). An ambiguous choice by the
  // values' types is made again by the arguments' types as written.
  // The arguments as written, where the walk typed them, with their value categories (an lvalue's
  // type a reference), as overload resolution sees them: by the values' types alone every argument
  // was an rvalue, so `Pair<M, M> p(m, n)` chose Pair(A&&, B&&) and moved from m and n at compile
  // time only (language/consteval_forwarding_constructor_copies_lvalue); else the values' types.
  // (A class object's only, where an lvalue is copied and an rvalue moved: a string literal, an
  // lvalue array, stays the value's pointer, as the values' types gave it.)
  std::vector<const Ty*> typed = argTypes;
  bool typedWritten = written && written->size() == args.size();
  for (size_t i = 0; typedWritten && i < typed.size(); ++i)
    if (Expr* a = (*written)[i]; a && a->ty && a->ty->isKnown() && !types.isDependent(a->ty) && !mentionsUnknown(a->ty) && !constIsExpansion(a) &&
                                 types.unqualified(types.stripRef(a->ty))->kind == TyKind::Class)
      typed[i] = argumentType(a, a->ty, types);
  // (Else by the categories constArguments gave: `new (node) E(forward<Args>(args)...)`, Map's node.)
  if (!typedWritten && categories && categories->size() == args.size())
    for (size_t i = 0; i < typed.size(); ++i)
      if (const Ty* at = typed[i] ? types.unqualified(types.stripRef(typed[i])) : nullptr; at && at->kind == TyKind::Class && (*categories)[i] != 0)
      {
        bool readOnly = places && i < places->size() && (*places)[i] && !(*places)[i]->writable();
        const Ty* object = readOnly ? types.qualified(at, true, false) : at;
        typed[i] = (*categories)[i] == 1 ? types.lref(object) : types.rref(object);
      }
  std::vector<const Ty*> chosenTypes = typed; // the argument types the choice was made with
  bool ambiguous = false;
  const Ty* inheritedFrom = nullptr;
  FunctionDecl* ctor = chooseConstructor(t, typed, nullptr, &inheritedFrom, implicitOnly, &ambiguous);
  // (None those take, `W(int& x)` given a's value: by every argument as written, an lvalue a
  // reference parameter binds, a brace list as the list it is.)
  if ((ambiguous || !ctor) && typedWritten)
  {
    std::vector<const Ty*> all = argTypes;
    for (size_t i = 0; i < all.size(); ++i)
      if (Expr* a = (*written)[i]; a && a->ty && a->ty->isKnown() && !types.isDependent(a->ty) && !mentionsUnknown(a->ty))
        all[i] = argumentType(a, a->ty, types);
      else if (a && a->kind == NodeKind::BraceInitExpr)
      {
        if (const Ty* list = braceListType(a, initializerListDecl(), types))
          all[i] = list;
      }
    ctor = chooseConstructor(t, all, nullptr, &inheritedFrom, implicitOnly, &ambiguous);
    chosenTypes = all;
  }
  if (!ctor || ambiguous || (ctor->flags & DF_Delete))
    return std::nullopt; // (a deleted one: no constant; codegen's call of it is an error)
  if (!inheritedFrom)
  {
    if (ctor->templateParams.empty())
      return constRunConstructor(ctor, t, args, depth + 1, nullptr, nullptr, places, target, categories); // (a reference parameter refers to its argument's place)
    // `template<class T1, class T2> GamePair(T1&& key, T2&& value)`: its parameters as the
    // arguments deduce them, with the class's (an lvalue's a reference: `Box(U&& u) :
    // held(forward<U>(u))` given an lvalue copies it).
    Subst own = layout->subst;
    deduceCall(ctor, declaredTypeOf(ctor, layout->subst.empty() ? nullptr : &layout->subst, 0), chosenTypes, own);
    if (!fillDefaultTemplateArgs(ctor, own))
      return std::nullopt;
    for (Decl* p : ctor->templateParams)
      if (const Ty* bound = own.find(p); !bound || !bound->isKnown() || types.isDependent(bound))
        return std::nullopt;
    if (implicitOnly && ctor->explicitCondition && isExplicit(ctor, &own))
      return std::nullopt; // `explicit(!isConvertible<U, T>)` true for these arguments: no implicit conversion
    return constRunConstructor(ctor, t, args, depth + 1, &own, nullptr, places, target, categories);
  }
  // `using Vector2::Vector2;`: a base's constructor makes that base's part, the class's own
  // members coming from their defaults (an inherited constructor).
  if (!ctor->templateParams.empty())
    return std::nullopt;
  for (size_t b = 0; b < layout->bases.size(); ++b)
  {
    if (!layout->bases[b] || types.unqualified(layout->bases[b]) != inheritedFrom)
      continue;
    std::optional<ConstValue> part = constRunConstructor(ctor, layout->bases[b], args, depth + 1);
    if (!part || !constMayMake(layout->bases.size() + layout->fields.size()))
      return std::nullopt;
    std::vector<ConstValue> parts;
    for (size_t k = 0; k < layout->bases.size(); ++k)
    {
      std::optional<ConstValue> p = k == b ? part : constEmpty(layout->bases[k], false, depth + 1);
      if (!p)
        return std::nullopt;
      parts.push_back(*p);
    }
    for (size_t i = 0; i < layout->fields.size(); ++i)
    {
      std::optional<ConstValue> p = constFieldDefault(*layout, i, false, depth);
      if (!p)
        return std::nullopt;
      parts.push_back(*p);
    }
    constFitBitFields(*layout, parts);
    return ConstValue::object(t, std::move(parts));
  }
  return std::nullopt; // (inherited from an indirect base: not evaluated)
}

auto GlobalScope::Impl::constBraceList(BraceInitExpr* b, const TypeContext& ctx, int depth, std::vector<std::optional<ConstPlace>>* places,
                                       std::vector<uint8_t>* categories) -> std::optional<ConstValue>
{
  if (!b->designators.empty() || !constMayMake(b->args.size()))
    return std::nullopt;
  std::vector<ConstValue> elements;
  if (!constArguments(b->args, ctx, depth, elements, places, categories))
    return std::nullopt;
  const Ty* first = nullptr;
  for (const ConstValue& v : elements)
    if (!first)
      first = v.type;
  Decl* list = initializerListDecl();
  if (!elements.empty() && (!first || !list))
    return std::nullopt;
  ConstValue v = ConstValue::object(nullptr, std::move(elements));
  if (first)
    v.type = types.braceList(list, first);
  return v;
}

auto GlobalScope::Impl::constFromBraceList(const ConstValue& list, const Ty* t, int depth, const Span<Expr*>* written,
                                           const std::vector<std::optional<ConstPlace>>* places, const std::vector<uint8_t>* categories, const ConstPlace* target)
  -> std::optional<ConstValue>
{
  const Ty* u = types.unqualified(types.stripRef(t));
  const std::vector<ConstValue>& elements = *list.parts;
  if (depth > 64)
    return std::nullopt;
  if (isInitializerList(u))
    return constInitializerList(u, elements, depth);
  if (u->kind == TyKind::Array)
  {
    size_t count = u->count ? u->count : elements.size();
    if (elements.size() > count || !count || !constMayMake(count))
      return std::nullopt;
    std::vector<ConstValue> parts;
    for (size_t i = 0; i < count; ++i)
    {
      std::optional<ConstValue> e = i < elements.size() ? constConvertTo(elements[i], u->inner, depth + 1) : constEmpty(u->inner, true, depth + 1);
      if (!e)
        return std::nullopt;
      parts.push_back(*e);
    }
    return ConstValue::object(u->count ? u : types.array(u->inner, uint32_t(count)), std::move(parts));
  }
  if (elements.empty())
    return constEmpty(u, true, depth + 1); // `{}`: value-initialized
  if (u->kind == TyKind::Class)
  {
    if (std::optional<ConstValue> v = constByInitializerList(u, elements, depth, target))
      return v;
    // `T b{a}`, `T{move(a)}`: an object of the class itself copied or moved by its own
    // constructor ([dcl.init.list] 3.7), which constConstruct would take as it is.
    if (elements.size() == 1 && places && categories && places->size() == 1 && categories->size() == 1 && constIsObjectOf(elements[0], u))
      return constFromObject(u, elements[0], (*places)[0], (*categories)[0], depth, target);
    return constConstruct(u, elements, false, depth + 1, written, target, places && places->size() == elements.size() ? places : nullptr,
                          categories && categories->size() == elements.size() ? categories : nullptr);
  }
  return elements.size() == 1 && !constIsBraceList(elements[0]) ? constAsDeclared(elements[0], t) : std::nullopt;
}

auto GlobalScope::Impl::constInitializerList(const Ty* u, const std::vector<ConstValue>& elements, int depth) -> std::optional<ConstValue>
{
  const ConstClassLayout* layout = constLayoutOf(u, depth);
  if (!layout || !layout->bases.empty() || layout->fields.size() != 2 || u->args.empty() || !constMayMake(elements.size()))
    return std::nullopt;
  const Ty* pt = types.unqualified(layout->fieldTypes[0]);
  const Ty* nt = types.unqualified(layout->fieldTypes[1]);
  if (pt->kind != TyKind::Pointer || !pt->inner || !nt->isArithmetic())
    return std::nullopt;
  std::vector<ConstValue> converted;
  for (const ConstValue& e : elements)
  {
    std::optional<ConstValue> c = constConvertTo(e, u->args[0], depth + 1);
    if (!c)
      return std::nullopt;
    converted.push_back(*c);
  }
  std::optional<ConstValue> count = constConverted(ConstValue::integer(int64_t(converted.size())), nt);
  if (!count)
    return std::nullopt;
  ConstValue items = ConstValue::integer(0, pt);
  if (!converted.empty())
  {
    const Ty* backingType = types.array(pt->inner, uint32_t(converted.size())); // (before the move: constString's note)
    ConstValue backing = ConstValue::object(backingType, std::move(converted));
    items = ConstValue::pointer(pt, backing.parts, 0);
  }
  return ConstValue::object(u, {items, *count});
}

auto GlobalScope::Impl::constByInitializerList(const Ty* u, const std::vector<ConstValue>& elements, int depth, const ConstPlace* target) -> std::optional<ConstValue>
{
  const ConstClassLayout* layout = constLayoutOf(u, depth);
  if (!layout)
    return std::nullopt;
  for (FunctionDecl* c : layout->constructors)
  {
    if (c->params.empty() || (c->flags & DF_Delete) || !c->templateParams.empty())
      continue;
    bool restDefaulted = true;
    for (size_t i = 1; i < c->params.size(); ++i)
      restDefaulted = restDefaulted && static_cast<ParamDecl*>(c->params[i])->defaultValue;
    const Ty* ft = restDefaulted ? declaredTypeOf(c, layout->subst.empty() ? nullptr : &layout->subst, depth + 1) : nullptr;
    const Ty* pt = ft && ft->kind == TyKind::Function && !ft->args.empty() && ft->args[0] ? types.unqualified(types.stripRef(ft->args[0])) : nullptr;
    if (!pt || !isInitializerList(pt))
      continue;
    std::optional<ConstValue> list = constInitializerList(pt, elements, depth + 1);
    return list ? constRunConstructor(c, u, {*list}, depth + 1, nullptr, nullptr, nullptr, target) : std::nullopt;
  }
  return std::nullopt;
}

auto GlobalScope::Impl::constStaticStorage(Decl* d) -> bool
{
  if (!d || d->kind != NodeKind::VariableDecl || (d->flags & DF_Typedef))
    return false;
  if (d->flags & DF_Static)
    return true;
  for (Decl* p = d->parent; p; p = p->parent)
    if (p->kind == NodeKind::FunctionDecl || p->kind == NodeKind::ClassDecl)
      return false; // a local, a parameter, a data member
  return true;
}

auto GlobalScope::Impl::constInstanceOf(Decl* d, const QualifiedName& name, const TypeContext& ctx, int depth) -> const Ty*
{
  Decl* cls = d && d->parent && d->parent->kind == NodeKind::ClassDecl ? d->parent : nullptr;
  Span<Decl*> params = cls ? effectiveParams(cls) : Span<Decl*>{};
  if (params.empty())
    return nullptr;
  Subst s = name.parts.size() > 1 ? qualifierSubst(name, ctx, depth + 1, lexicalScopeOf(d)) : ctx.subst ? *ctx.subst : Subst{};
  for (Decl* p : params)
    if (!s.find(p))
      return nullptr;
  return substitute(selfType(cls), s);
}

auto GlobalScope::Impl::constPartType(const Ty* t, const std::vector<size_t>& path, size_t count, int depth) -> const Ty*
{
  for (size_t k = 0; k < count && t; ++k)
  {
    t = types.unqualified(types.stripRef(t));
    if (t->kind == TyKind::Array)
      t = t->inner;
    else if (const ConstClassLayout* layout = t->kind == TyKind::Class ? constLayoutOf(t, depth) : nullptr)
      t = path[k] < layout->bases.size() ? layout->bases[path[k]]
        : path[k] - layout->bases.size() < layout->fieldTypes.size() ? layout->fieldTypes[path[k] - layout->bases.size()] : nullptr;
    else
      return nullptr;
  }
  return t;
}

auto GlobalScope::Impl::constTargetType(const ConstTarget& t, int depth) -> const Ty*
{
  if (t.storage)
  {
    auto it = constStorage().find(t.storage);
    return it != constStorage().end() ? it->second.type : nullptr;
  }
  if (!t.decl)
    return nullptr;
  Subst s = t.owner ? substFor(t.owner) : Subst{};
  for (const auto& [param, arg] : t.args)
    s.pairs.push_back({param, arg});
  const Ty* declared = declaredTypeOf(t.decl, s.empty() ? nullptr : &s, depth + 1);
  return declared && declared->isKnown() && !types.isDependent(declared) ? declared : nullptr;
}

auto GlobalScope::Impl::constDesignated(Expr* x, const TypeContext& ctx, int depth) -> std::optional<ConstDesignated>
{
  while (x && x->kind == NodeKind::ParenExpr)
    x = static_cast<ParenExpr*>(x)->inner;
  if (!x || depth > 64)
    return std::nullopt;
  auto pointee = [&](const ConstValue& p) -> std::optional<ConstDesignated> {
    const Ty* pt = p.isAddress() && p.type ? types.unqualified(types.stripRef(p.type)) : nullptr;
    if (!pt || pt->kind != TyKind::Pointer || !pt->inner)
      return std::nullopt;
    return ConstDesignated{*p.target, pt->inner};
  };
  switch (x->kind)
  {
    case NodeKind::NameExpr:
    {
      auto* n = static_cast<NameExpr*>(x);
      if (std::optional<ConstPlace> local = constLocal(x, ctx))
      {
        // A local or parameter of the run (what a reference to one refers to): its storage. Not a
        // copy the run holds of something else's (`this`, a reference it could not bind).
        if (!constAddressable(*local) || !local->type || local->type->isReference())
          return std::nullopt;
        return ConstDesignated{constStorageTarget(*local), local->type};
      }
      Decl* d = n->resolved;
      if (!d || d->kind == NodeKind::TemplateParamDecl)
        d = lookupQualified(n->name, ctx.scope, ctx.locals, depth + 1, ctx.subst).decl;
      if (!constStaticStorage(d))
        return std::nullopt;
      ConstDesignated a;
      a.target.decl = d;
      a.target.owner = constInstanceOf(d, n->name, ctx, depth);
      a.type = constTargetType(a.target, depth);
      if (!a.type || a.type->isReference())
        return std::nullopt;
      return a;
    }
    case NodeKind::MemberExpr:
    {
      auto* m = static_cast<MemberExpr*>(x);
      if (m->dynamicMember || m->member.parts.size() != 1)
        return std::nullopt;
      std::optional<ConstDesignated> base;
      if (m->op == P::Dot)
        base = constDesignated(m->base, ctx, depth + 1);
      else if (m->op == P::Arrow && m->base->kind == NodeKind::ThisExpr)
      {
        // `&this->storage.value`: a part of the object the function runs on.
        std::optional<ConstPlace> self = constThisPlace(ctx);
        if (self && constAddressable(*self) && self->type)
          base = ConstDesignated{constStorageTarget(*self), self->type};
      }
      else if (m->op == P::Arrow)
        if (std::optional<ConstValue> p = constValue(m->base, ctx, depth + 1))
          base = pointee(*p);
      const Ty* cls = base ? types.unqualified(types.stripRef(base->type)) : nullptr;
      if (!cls || cls->kind != TyKind::Class)
        return std::nullopt;
      Decl* d = constFieldNamed(m->resolved, m->member, cls);
      std::optional<std::vector<size_t>> path = d && d->kind == NodeKind::VariableDecl && !(d->flags & DF_Static) ? constFieldPath(cls, d, 0) : std::nullopt;
      if (!path)
        return std::nullopt;
      base->target.path.insert(base->target.path.end(), path->begin(), path->end());
      base->type = constPartType(cls, *path, path->size(), depth + 1);
      if (!base->type || base->type->isReference())
        return std::nullopt;
      return base;
    }
    case NodeKind::IndexExpr:
    {
      auto* ix = static_cast<IndexExpr*>(x);
      if (ix->args.size() != 1)
        return std::nullopt;
      std::optional<ConstValue> index = constValue(ix->args[0], ctx, depth + 1);
      if (!index || index->kind != ConstValue::Kind::Int)
        return std::nullopt;
      if (std::optional<ConstDesignated> base = constDesignated(ix->base, ctx, depth + 1))
      {
        const Ty* at = types.unqualified(types.stripRef(base->type));
        if (at->kind != TyKind::Array || !at->count || index->i < 0 || uint64_t(index->i) >= at->count)
          return std::nullopt;
        base->target.path.push_back(size_t(index->i));
        base->type = at->inner;
        return base;
      }
      std::optional<ConstValue> p = constValue(ix->base, ctx, depth + 1); // `second[1]`
      std::optional<ConstValue> moved = p && p->isAddress() ? constAddressMove(*p, index->i, depth) : std::nullopt;
      return moved && constAddressDereferenceable(*moved, depth) ? pointee(*moved) : std::nullopt;
    }
    case NodeKind::UnaryExpr:
    {
      auto* u = static_cast<UnaryExpr*>(x);
      std::optional<P> op = constOperator(u->op, u->isKeyword);
      if (!op || *op != P::Star || u->postfix)
        return std::nullopt;
      std::optional<ConstValue> p = constValue(u->operand, ctx, depth + 1);
      return p ? pointee(*p) : std::nullopt;
    }
    default:
      return std::nullopt;
  }
}

auto GlobalScope::Impl::constFunctionAddress(FunctionDecl* f, const QualifiedName& name, const TypeContext& ctx, int depth) -> std::optional<ConstValue>
{
  ConstTarget target;
  target.decl = f;
  bool member = f->parent && f->parent->kind == NodeKind::ClassDecl && !(f->flags & DF_Static);
  const NamePart& written = name.last();
  if (written.templateArgs.size() != f->templateParams.size())
    return std::nullopt; // a template's arguments are the target type's to deduce
  for (size_t i = 0; i < f->templateParams.size(); ++i)
  {
    const Ty* t = templateArgument(written.templateArgs[i], f->templateParams[i], ctx, depth + 1);
    if (!t || !t->isKnown() || types.isDependent(t))
      return std::nullopt;
    target.args.push_back({f->templateParams[i], t});
  }
  if (member)
  {
    if (name.parts.size() < 2)
      return std::nullopt;
    target.owner = constInstanceOf(f, name, ctx, depth);
    if (!target.owner && effectiveParams(f->parent).empty())
      target.owner = selfType(f->parent);
    if (!target.owner)
      return std::nullopt;
  }
  else
    target.owner = constInstanceOf(f, name, ctx, depth);
  const Ty* ft = constTargetType(target, depth);
  if (!ft || ft->kind != TyKind::Function)
    return std::nullopt;
  if (member)
    return ConstValue::address(types.memberPointer(ft, target.owner), std::move(target), ConstValue::Kind::MemberPointer);
  return ConstValue::address(types.pointer(ft), std::move(target));
}

auto GlobalScope::Impl::constAddressOf(Expr* operand, const TypeContext& ctx, int depth) -> std::optional<ConstValue>
{
  Expr* x = operand;
  while (x && x->kind == NodeKind::ParenExpr)
    x = static_cast<ParenExpr*>(x)->inner;
  if (!x)
    return std::nullopt;
  if (x->kind == NodeKind::NameExpr && !constLocal(x, ctx))
  {
    auto* n = static_cast<NameExpr*>(x);
    Decl* d = n->resolved;
    Lookup l;
    if (!d || d->kind == NodeKind::TemplateParamDecl)
    {
      l = lookupQualified(n->name, ctx.scope, ctx.locals, depth + 1, ctx.subst);
      d = l.decl;
      if (d && d->kind == NodeKind::FunctionDecl && l.overloads)
      {
        int functions = 0;
        for (const Entry& entry : *l.overloads)
          functions += entry.decl && entry.decl->kind == NodeKind::FunctionDecl;
        if (functions != 1)
          return std::nullopt; // which overload is the target type's to choose
      }
    }
    if (d && d->kind == NodeKind::FunctionDecl)
      return constFunctionAddress(static_cast<FunctionDecl*>(d), n->name, ctx, depth);
    if (d && d->kind == NodeKind::VariableDecl && d->parent && d->parent->kind == NodeKind::ClassDecl && !(d->flags & (DF_Static | DF_Typedef)))
    {
      if (n->name.parts.size() < 2)
      {
        // `&items` in a member function: a part of the object it runs on, as `&this->items`.
        std::optional<ConstPlace> self = constThisPlace(ctx);
        const Ty* cls = self && constAddressable(*self) && self->type ? types.unqualified(types.stripRef(self->type)) : nullptr;
        std::optional<std::vector<size_t>> path = cls && cls->kind == TyKind::Class ? constFieldPath(cls, d, 0) : std::nullopt;
        const Ty* member = path ? constPartType(cls, *path, path->size(), depth + 1) : nullptr;
        if (!member || member->isReference())
          return std::nullopt;
        ConstTarget target = constStorageTarget(*self);
        target.path.insert(target.path.end(), path->begin(), path->end());
        return ConstValue::address(types.pointer(member), std::move(target));
      }
      ConstTarget target;
      target.decl = d;
      target.owner = constInstanceOf(d, n->name, ctx, depth);
      if (!target.owner && effectiveParams(d->parent).empty())
        target.owner = selfType(d->parent);
      Subst s = target.owner ? substFor(target.owner) : Subst{};
      const Ty* member = target.owner ? declaredTypeOf(d, s.empty() ? nullptr : &s, depth + 1) : nullptr;
      if (!member || !member->isKnown() || types.isDependent(member))
        return std::nullopt;
      return ConstValue::address(types.memberPointer(member, target.owner), std::move(target), ConstValue::Kind::MemberPointer);
    }
  }
  std::optional<ConstDesignated> a = constDesignated(x, ctx, depth);
  if (!a)
  {
    // `&v[0]`: an element a class's operator[] designates where the run holds it (a heap block, a
    // local): its address. Only of an operand without side effects, which a designation that
    // failed above may have evaluated already.
    std::optional<ConstPlace> at = constPure(x) ? constPlaceOf(x, ctx, depth + 1, true) : std::nullopt;
    if (!at || !at->type || at->type->isReference() || !constAddressable(*at))
      return std::nullopt;
    return ConstValue::address(types.pointer(at->type), constStorageTarget(*at));
  }
  return ConstValue::address(types.pointer(a->type), std::move(a->target));
}

auto GlobalScope::Impl::constAddressMove(const ConstValue& p, int64_t n, int depth) -> std::optional<ConstValue>
{
  if (!p.isAddress() || !p.target || n == 0)
    return p.isAddress() ? std::optional<ConstValue>(p) : std::nullopt;
  const ConstTarget& t = *p.target;
  if (t.path.empty())
    return std::nullopt; // a whole variable: no array to move along
  const Ty* array = constPartType(constTargetType(t, depth), t.path, t.path.size() - 1, depth);
  array = array ? types.unqualified(types.stripRef(array)) : nullptr;
  if (!array || array->kind != TyKind::Array || !array->count)
    return std::nullopt;
  int64_t index = int64_t(t.path.back()) + n;
  if (index < 0 || uint64_t(index) > array->count)
    return std::nullopt;
  ConstTarget moved = t;
  moved.path.back() = size_t(index);
  return ConstValue::address(p.type, std::move(moved));
}

auto GlobalScope::Impl::constAddressDereferenceable(const ConstValue& p, int depth) -> bool
{
  const ConstTarget& t = *p.target;
  if (t.path.empty())
    return true;
  const Ty* array = constPartType(constTargetType(t, depth), t.path, t.path.size() - 1, depth);
  array = array ? types.unqualified(types.stripRef(array)) : nullptr;
  return !array || array->kind != TyKind::Array || t.path.back() < array->count;
}

auto GlobalScope::Impl::constReadTarget(const ConstValue& p, int depth) -> std::optional<ConstValue>
{
  if (p.isAddress() && p.target && p.target->storage)
  {
    std::optional<ConstPlace> place = constStoragePlace(p, "a read of", depth);
    return place ? constRead(*place) : std::nullopt;
  }
  if (!p.isAddress() || !p.target || !p.target->decl || p.target->decl->kind != NodeKind::VariableDecl || !constAddressDereferenceable(p, depth))
    return std::nullopt;
  const ConstTarget& t = *p.target;
  Subst s = t.owner ? substFor(t.owner) : Subst{};
  TypeContext none{lexicalScopeOf(t.decl), nullptr, nullptr};
  std::optional<ConstValue> v = constValueOfDecl(t.decl, none, 1, s.empty() ? nullptr : &s);
  if (!v)
    return std::nullopt;
  return t.path.empty() ? v : constPart(*v, t.path);
}

auto GlobalScope::Impl::constAddressArith(P op, const ConstValue& l, const ConstValue& r, int depth) -> std::optional<ConstValue>
{
  auto boolean = [this](bool b) { return ConstValue::integer(b, types.boolType()); };
  bool equality = op == P::EqEq || op == P::NotEq;
  // An address of an object whose lifetime ended (a local of a run that returned, a named return
  // variable's among them, whose result the program's address would name) compares as no constant,
  // as C++'s constant evaluation has it, never as another object's.
  if ((l.isAddress() && l.target->storage && !constLiveStorage(*l.target, "a comparison of")) ||
      (r.isAddress() && r.target->storage && !constLiveStorage(*r.target, "a comparison of")))
    return std::nullopt;
  if ((l.isAddress() || l.isMemberPointer()) && (r.isAddress() || r.isMemberPointer()))
  {
    // A pointer to a class and one to a base of it: the first converted to the base, as C++
    // compares them (their composite pointer type), so that `d.prev == &d` holds with prev a Base*
    // holding &d (which points at d's base part).
    ConstTarget a = *l.target, b = *r.target;
    const Ty* lp = l.isAddress() && l.type && l.type->kind == TyKind::Pointer && l.type->inner ? types.unqualified(l.type->inner) : nullptr;
    const Ty* rp = r.isAddress() && r.type && r.type->kind == TyKind::Pointer && r.type->inner ? types.unqualified(r.type->inner) : nullptr;
    if (lp && rp && lp != rp && lp->kind == TyKind::Class && rp->kind == TyKind::Class)
    {
      if (std::optional<std::vector<size_t>> up = constBasePath(rp, lp, 0); up && !up->empty())
        b.path.insert(b.path.end(), up->begin(), up->end());
      else if (std::optional<std::vector<size_t>> down = constBasePath(lp, rp, 0); down && !down->empty())
        a.path.insert(a.path.end(), down->begin(), down->end());
    }
    bool same = l.kind == r.kind && a == b;
    if (equality)
      return boolean(op == P::EqEq ? same : !same);
    bool siblings = l.isAddress() && r.isAddress() && a.decl == b.decl && a.storage == b.storage && a.owner == b.owner && a.args == b.args && !a.path.empty() &&
                    a.path.size() == b.path.size() && std::equal(a.path.begin(), a.path.end() - 1, b.path.begin());
    if (!siblings)
    {
      // Addresses of different objects: no order, as C++ leaves it unspecified ([expr.rel] 4),
      // which no constant evaluation computes ([expr.const] 5).
      if (constWhy().empty())
        constWhy() = "an order or a difference of pointers to different objects, which C++ leaves unspecified";
      return std::nullopt;
    }
    int64_t x = int64_t(a.path.back()), y = int64_t(b.path.back());
    switch (op)
    {
      case P::Minus: return ConstValue::integer(x - y, types.ptrdiffType());
      case P::Less: return boolean(x < y);
      case P::Greater: return boolean(x > y);
      case P::LessEq: return boolean(x <= y);
      case P::GreaterEq: return boolean(x >= y);
      default: return std::nullopt;
    }
  }
  if (equality && ((l.isAddress() || l.isMemberPointer()) != (r.isAddress() || r.isMemberPointer())))
  {
    const ConstValue& other = l.isAddress() || l.isMemberPointer() ? r : l;
    const ConstValue& address = l.isAddress() || l.isMemberPointer() ? l : r;
    if (other.isPointer() && address.isAddress() && address.target->storage)
      return std::nullopt; // an object of the run and a pointer into a copy of a value: maybe the same object
    if ((other.kind == ConstValue::Kind::Int && other.i == 0) || other.isPointer())
      return boolean(op == P::NotEq); // never null, never into an array the evaluation made
    return std::nullopt;
  }
  if (l.isAddress() && r.kind == ConstValue::Kind::Int && (op == P::Plus || op == P::Minus))
    return constAddressMove(l, op == P::Plus ? r.i : -r.i, depth);
  if (r.isAddress() && l.kind == ConstValue::Kind::Int && op == P::Plus)
    return constAddressMove(r, l.i, depth);
  // `text >= chars` of a string literal's text and a String's block (String::insert): an order of
  // pointers into objects the evaluation cannot relate.
  bool order = op == P::Less || op == P::Greater || op == P::LessEq || op == P::GreaterEq || op == P::Minus;
  if (order && (l.isAddress() || l.isPointer()) && (r.isAddress() || r.isPointer()) && constWhy().empty())
    constWhy() = "an order or a difference of pointers the evaluation cannot relate (into different objects, C++ leaves it unspecified)";
  return std::nullopt;
}

auto GlobalScope::Impl::constClosureCaptures(LambdaExpr* l, std::vector<ConstCapture>& out) -> bool
{
  out.clear();
  for (const LambdaCapture& cap : l->captures)
  {
    if (cap.isThis)
      out.push_back({UINT32_MAX, true, true, nullptr, nullptr});
    else if (cap.init)
    {
      // `[x = expr]`, `[&x = expr]`: the variable the capture declares.
      auto* v = cap.decl && cap.decl->kind == NodeKind::VariableDecl ? static_cast<VariableDecl*>(cap.decl) : nullptr;
      if (!v || v->name.parts.size() != 1)
        return false;
      out.push_back({v->name.parts[0].symbol, cap.byRef, false, cap.init, v});
    }
    else
      out.push_back({cap.symbol, cap.byRef, false, nullptr, nullptr});
  }
  if (!l->defaultCapture)
    return true;
  // `[=]`, `[&]`: the variables of the functions around that the body names (not the lambda's own
  // parameters and locals, nor a global), each once in the order the body first names them, and
  // `this` where the body uses it (a member named alone too), as codegen's captureList has them.
  std::unordered_set<const Decl*> inner(l->params.begin(), l->params.end());
  std::unordered_set<uint32_t> seen;
  bool usesThis = false;
  for (const ConstCapture& c : out)
  {
    if (c.decl)
      inner.insert(c.decl);
    if (c.isThis)
      usesThis = true;
    else
      seen.insert(c.symbol);
  }
  bool thisCaptured = usesThis;
  std::vector<uint32_t> names;
  forEachNode(l->body, [&](Node* n) {
    if (n->kind == NodeKind::VariableDecl || n->kind == NodeKind::ParamDecl)
      inner.insert(static_cast<Decl*>(n)); // (the body's own: a nested lambda's parameters too)
    else if (n->kind == NodeKind::ThisExpr)
      usesThis = true;
    else if (n->kind == NodeKind::NameExpr)
    {
      auto* x = static_cast<NameExpr*>(n);
      Decl* d = x->resolved;
      if (!d || x->name.parts.size() != 1 || x->name.global)
        return;
      if (d->kind == NodeKind::VariableDecl && d->parent && d->parent->kind == NodeKind::ClassDecl && !(d->flags & (DF_Static | DF_Typedef)))
      {
        usesThis = true; // `count` alone: this->count
        return;
      }
      bool local = d->kind == NodeKind::ParamDecl ||
                   (d->kind == NodeKind::VariableDecl && !(d->flags & (DF_Static | DF_Typedef)) && d->parent && d->parent->kind == NodeKind::FunctionDecl);
      if (local && !inner.count(d) && seen.insert(x->name.parts[0].symbol).second)
        names.push_back(x->name.parts[0].symbol);
    }
  });
  for (uint32_t s : names)
    out.push_back({s, l->defaultCapture == 2, false, nullptr, nullptr});
  if (usesThis && !thisCaptured)
    out.push_back({UINT32_MAX, true, true, nullptr, nullptr});
  return true;
}

auto GlobalScope::Impl::constClosure(LambdaExpr* l, const Ty* type, const TypeContext& ctx, int depth) -> std::optional<ConstValue>
{
  const Ty* t = type ? types.unqualified(types.stripRef(type)) : nullptr;
  std::vector<ConstCapture> captures;
  if (!t || t->kind != TyKind::Lambda || !l->body || !constClosureCaptures(l, captures) || !constMayMake(captures.size()))
    return std::nullopt;
  // An address of a place the run holds (by reference): what a reference capture keeps.
  auto addressOf = [&](const ConstPlace& p) -> std::optional<ConstValue> {
    if (!p.type || !constAddressable(p))
      return std::nullopt;
    return ConstValue::address(types.pointer(p.type), constStorageTarget(p));
  };
  std::vector<ConstValue> parts;
  for (const ConstCapture& c : captures)
  {
    std::optional<ConstValue> part;
    if (c.isThis)
    {
      std::optional<ConstPlace> self = constThisPlace(ctx);
      part = self ? addressOf(*self) : std::nullopt;
    }
    else if (c.init && c.byRef)
    {
      std::optional<ConstPlace> at = constPlaceOf(c.init, ctx, depth + 1, true);
      part = at ? addressOf(*at) : std::nullopt;
    }
    else if (c.init)
    {
      const Ty* declared = c.decl ? constDeducedType(static_cast<VariableDecl*>(c.decl), nullptr) : nullptr;
      part = declared && !containsAuto(declared) ? constInitialize(declared, c.init, ctx, depth + 1) : constValue(c.init, ctx, depth + 1);
    }
    else
    {
      // A local of the run, by its name: its place (by reference), or a copy of it.
      std::optional<ConstPlace> at;
      for (size_t i = ctx.env ? ctx.env->size() : 0; i-- > 0 && !at;)
        if ((*ctx.env)[i].symbol == c.symbol && !(*ctx.env)[i].pack)
        {
          at = constResolve(ConstPlace{ctx.env, i, {}, (*ctx.env)[i].type});
          at->type = (*ctx.env)[i].type;
        }
      if (at && c.byRef)
        part = addressOf(*at);
      else if (at)
      {
        part = constRead(*at);
        if (part && part->isObject() && part->type && constCopyRunsCode(part->type))
          part = constCopyConstruct(part->type, *part, at, false, depth + 1);
      }
    }
    if (!part || part->isUnset())
    {
      if (constWhy().empty())
        constWhy() = "a lambda's capture the evaluator cannot make";
      return std::nullopt;
    }
    parts.push_back(*part);
  }
  return ConstValue::object(t, std::move(parts));
}

auto GlobalScope::Impl::constCallClosure(LambdaExpr* l, const ConstValue& closure, CallExpr* c, const TypeContext& ctx, int depth, const ConstPlace* at) -> std::optional<ConstValue>
{
  std::vector<ConstCapture> captures;
  if (!l->body || c->args.size() > l->params.size() || !closure.isObject() || !closure.parts || !constClosureCaptures(l, captures) ||
      captures.size() != closure.parts->size())
    return std::nullopt;
  // A generic lambda (`[](auto a, auto b) { return a + b; }`): the instance of its call operator
  // for the template arguments the walk deduced for this call, as code generation calls it
  // (checkLambdaInstance: its parameters and body copied and typed for them, its result deduced).
  // (It was no constant, without a reason.)
  LambdaExpr* run = l;
  const Ty* instanceTy = nullptr;
  Subst instance;
  LocalStack instanceParams;
  if (!l->templateParams.empty())
  {
    // (Its own parameters the closure type's lambda's, as codegen keys the instance: lambdaOf.)
    const Ty* lambdaTy = closure.type ? types.unqualified(types.stripRef(closure.type)) : nullptr;
    LambdaExpr* original = lambdaTy && lambdaTy->kind == TyKind::Lambda ? lambdaOf(lambdaTy) : nullptr;
    if (!original || original->templateParams.size() != l->templateParams.size() || c->templateArgCount != original->templateParams.size())
      return std::nullopt;
    Subst own;
    for (size_t i = 0; i < original->templateParams.size(); ++i)
    {
      const Ty* a = c->templateArgs[i];
      if (!a || !a->isKnown() || types.isDependent(a))
        return std::nullopt;
      own.pairs.push_back({original->templateParams[i], a});
    }
    LambdaExpr* body = nullptr;
    instanceTy = checkLambdaInstance(lambdaTy, own, &instance, nullptr, &body);
    if (!instanceTy || instanceTy->kind != TyKind::Function || !body || !body->body || body->params.size() != l->params.size() ||
        instanceTy->args.size() != body->params.size())
      return std::nullopt;
    run = body;
    for (const Subst::Pair& p : instance.pairs)
      if (p.param && p.param->kind == NodeKind::TemplateParamDecl)
        instanceParams.push(static_cast<TemplateParamDecl*>(p.param)->name, p.param);
  }
  std::vector<ConstValue> values;
  std::vector<std::optional<ConstPlace>> places; // what reference parameters refer to
  std::vector<uint8_t> categories;
  if (!constArguments(c->args, ctx, depth, values, &places, &categories))
    return std::nullopt;
  struct Call
  {
    ~Call() { --constCalls(); }

    bool ok = ++constCalls() <= 512 && stackRoomLeft();
  } call;
  if (!call.ok)
    return std::nullopt;
  std::vector<ConstBinding> env;
  ConstFrame frame(&env);
  // Where the lambda is written (its names, its file's tokens), whoever calls it; none of the
  // caller's locals.
  ParsedFile* file = l->closure && l->closure->file ? l->closure->file : nullptr;
  TypeContext fctx{l->closure ? lexicalScopeOf(l->closure) : ctx.scope, run != l && !instanceParams.items.empty() ? &instanceParams : nullptr,
                   run != l && !instance.empty() ? &instance : nullptr, file ? file->tokens : ctx.tokens, file ? file->text : ctx.text};
  fctx.env = &env;
  // The captures: one by reference, and `this`, referring to the object where it is; a copy, the
  // closure's part where the closure is (what the body changes stays for its next call, a `mutable`
  // lambda's state, as the program keeps it: it was read only, so `[n]() mutable { return ++n; }`
  // was no constant), else a binding of its own.
  for (size_t i = 0; i < captures.size(); ++i)
  {
    const ConstValue& part = (*closure.parts)[i];
    if (!captures[i].isThis && !captures[i].byRef && at)
    {
      ConstPlace resolved = constResolve(*at);
      resolved.path.push_back(i);
      ConstBinding alias{captures[i].symbol, ConstValue::unset(part.type)};
      alias.type = part.type;
      alias.writable = !resolved.readOnly;
      alias.aliasEnv = resolved.env;
      alias.aliasIndex = resolved.index;
      alias.aliasPath = resolved.path;
      env.push_back(std::move(alias));
      continue;
    }
    if (captures[i].isThis || captures[i].byRef)
    {
      std::optional<ConstPlace> at = part.isAddress() ? constStoragePlace(part, "a use of", depth) : std::nullopt;
      if (!at)
        return std::nullopt;
      ConstPlace resolved = constResolve(*at);
      ConstBinding alias{captures[i].isThis ? constThisSymbol : captures[i].symbol, ConstValue::unset(at->type)};
      alias.type = at->type;
      alias.writable = !resolved.readOnly;
      alias.aliasEnv = resolved.env;
      alias.aliasIndex = resolved.index;
      alias.aliasPath = resolved.path;
      env.push_back(std::move(alias));
    }
    else
      env.push_back(constBinding(captures[i].symbol, part, part.type)); // (a temporary closure's: its changes end with it)
  }
  for (size_t i = 0; i < run->params.size(); ++i)
  {
    auto* p = static_cast<ParamDecl*>(run->params[i]);
    if (p->kind != NodeKind::ParamDecl || p->isPack)
      return std::nullopt;
    // (A generic lambda's parameter: of the type its instance has, `auto a` given 2 an int.)
    const Ty* type = run != l ? instanceTy->args[i] : p->type ? convertType(p->type, fctx, 1) : nullptr;
    if (type && containsAuto(type))
      return std::nullopt;
    if (type && type->isReference() && i < places.size() && places[i] && places[i]->type && type->inner &&
        types.unqualified(types.stripRef(places[i]->type)) == types.unqualified(type->inner))
    {
      // `[](int& x) { ++x; }` given a local: a reference to it (it was a copy, read only).
      ConstPlace resolved = constResolve(*places[i]);
      ConstBinding alias{p->name, ConstValue::unset(type)};
      alias.type = places[i]->type;
      alias.writable = !type->inner->isConst && !places[i]->readOnly;
      alias.aliasEnv = resolved.env;
      alias.aliasIndex = resolved.index;
      alias.aliasPath = resolved.path;
      env.push_back(std::move(alias));
      continue;
    }
    std::optional<ConstValue> v = i < values.size() ? constConvertTo(values[i], type, 1)
                                : p->defaultValue ? constInitialize(type, p->defaultValue, fctx, 1) : std::nullopt;
    if (!v)
      return std::nullopt;
    env.push_back(constBinding(p->name, *v, type));
    if (type && type->isReference())
      env.back().writable = false; // a copy of what the argument names
  }
  // (A generic lambda's result: the one its instance deduced.)
  const Ty* returns = run != l ? instanceTy->inner : l->returnType ? convertType(l->returnType, fctx, 1) : nullptr;
  if (returns && containsAuto(returns))
    returns = nullptr;
  fctx.returnType = returns;
  fctx.namedReturn = constNamedReturn(run->body, returns, fctx, 1);
  std::optional<ConstValue> result;
  ConstFlow flow = constStatement(run->body, fctx, env, result, 1);
  if (returns && types.unqualified(returns)->kind == TyKind::Void && (flow == ConstFlow::Next || flow == ConstFlow::Returned))
    return ConstValue::integer(0, types.voidType());
  if ((flow == ConstFlow::Next || flow == ConstFlow::Returned) && !returns && !result && c->ty && types.unqualified(c->ty)->kind == TyKind::Void)
    return ConstValue::integer(0, types.voidType()); // (one whose deduced result is void)
  if (flow != ConstFlow::Returned || !result)
    return std::nullopt;
  return returns ? constConvertTo(result, returns, 1) : result;
}

auto GlobalScope::Impl::constCallLambda(LambdaExpr* l, CallExpr* c, const TypeContext& ctx, int depth) -> std::optional<ConstValue>
{
  if (!l->body || !l->templateParams.empty() || !l->captures.empty() || l->defaultCapture || c->args.size() > l->params.size())
    return std::nullopt;
  std::vector<ConstValue> values;
  for (Expr* a : c->args)
  {
    std::optional<ConstValue> v = constValue(a, ctx, depth + 1);
    if (!v)
      return std::nullopt;
    values.push_back(*v);
  }
  struct Call
  {
    ~Call() { --constCalls(); }

    bool ok = ++constCalls() <= 512 && stackRoomLeft();
  } call;
  if (!call.ok)
    return std::nullopt;
  std::vector<ConstBinding> env;
  ConstFrame frame(&env);
  TypeContext fctx = ctx; // where the lambda is written: its names, the instance's arguments
  fctx.env = &env;        // none of the enclosing run's locals: it captures nothing
  fctx.returnPlace = nullptr;
  for (size_t i = 0; i < l->params.size(); ++i)
  {
    auto* p = static_cast<ParamDecl*>(l->params[i]);
    if (p->kind != NodeKind::ParamDecl || p->isPack)
      return std::nullopt;
    const Ty* type = p->type ? convertType(p->type, fctx, 1) : nullptr;
    if (type && containsAuto(type))
      return std::nullopt;
    std::optional<ConstValue> v = i < values.size() ? constConvertTo(values[i], type, 1)
                                : p->defaultValue ? constInitialize(type, p->defaultValue, fctx, 1) : std::nullopt;
    if (!v)
      return std::nullopt;
    env.push_back(constBinding(p->name, *v, type));
    if (type && type->isReference())
      env.back().writable = false; // a copy of what the argument names
  }
  const Ty* returns = l->returnType ? convertType(l->returnType, fctx, 1) : nullptr;
  if (returns && containsAuto(returns))
    returns = nullptr;
  fctx.returnType = returns;
  fctx.namedReturn = constNamedReturn(l->body, returns, fctx, 1);
  std::optional<ConstValue> result;
  ConstFlow flow = constStatement(l->body, fctx, env, result, 1);
  if (returns && types.unqualified(returns)->kind == TyKind::Void && (flow == ConstFlow::Next || flow == ConstFlow::Returned))
    return ConstValue::integer(0, types.voidType());
  if (flow != ConstFlow::Returned || !result)
    return std::nullopt;
  return returns ? constConvertTo(result, returns, 1) : result;
}

auto GlobalScope::Impl::constCallThrough(const ConstValue& fn, CallExpr* c, const TypeContext& ctx, int depth) -> std::optional<ConstValue>
{
  if (!fn.isAddress() || !fn.target->decl || fn.target->decl->kind != NodeKind::FunctionDecl)
    return std::nullopt;
  auto* f = static_cast<FunctionDecl*>(fn.target->decl);
  if (!constDefinition(f) || constDefinition(f)->body->kind != NodeKind::BlockStmt || c->args.size() > f->params.size())
    return std::nullopt;
  std::vector<ConstValue> values;
  std::vector<std::optional<ConstPlace>> places;
  std::vector<uint8_t> categories;
  if (!constArguments(c->args, ctx, depth, values, &places, &categories))
    return std::nullopt;
  Subst bound = fn.target->owner ? substFor(fn.target->owner) : Subst{};
  for (const auto& [param, arg] : fn.target->args)
    bound.pairs.push_back({param, arg});
  LocalStack params;
  for (const Subst::Pair& p : bound.pairs)
    params.push(static_cast<TemplateParamDecl*>(p.param)->name, p.param);
  TypeContext fctx{lexicalScopeOf(f), params.items.empty() ? nullptr : &params, bound.empty() ? nullptr : &bound,
                   f->file ? f->file->tokens : nullptr, f->file ? f->file->text : std::string_view()};
  return constRun(f, values, fctx, depth, nullptr, &places, &categories);
}

auto GlobalScope::Impl::constMemberPointerRead(BinaryExpr* b, const TypeContext& ctx, int depth) -> std::optional<ConstValue>
{
  std::optional<ConstValue> object = constValue(b->lhs, ctx, depth + 1);
  if (object && P(b->op) == P::ArrowStar)
    object = object->isAddress() ? constReadTarget(*object, depth + 1) : object->isPointer() ? constDeref(*object) : std::nullopt;
  std::optional<ConstValue> member = object ? constValue(b->rhs, ctx, depth + 1) : std::nullopt;
  if (!object || !object->isObject() || !member || !member->isMemberPointer() || member->target->decl->kind != NodeKind::VariableDecl)
    return std::nullopt;
  std::optional<std::vector<size_t>> path = constFieldPath(object->type, member->target->decl, 0);
  return path ? constPart(*object, *path) : std::nullopt;
}

auto GlobalScope::Impl::constRangeEnd(const ConstValue& range, uint32_t symbol, int depth, const ConstPlace* at, bool write) -> std::optional<ConstValue>
{
  Subst subst;
  std::vector<FunctionDecl*> candidates = constMembersNamed(range.type, symbol, subst, depth);
  FunctionDecl* f = nullptr;
  for (FunctionDecl* c : candidates)
    if (c->params.empty() && (!f || ((c->flags & DF_Const) != 0) != write))
      f = c; // `begin() const` where there are both, unless the elements are written
  if (!f)
    return std::nullopt;
  std::optional<ConstValue> self = range;
  return constCallOn(f, subst, {}, &self, depth + 1, at);
}

auto GlobalScope::Impl::constRunConstructor(FunctionDecl* ctor, const Ty* t, const std::vector<ConstValue>& args, int depth, const Subst* own, FunctionDecl* declared, const std::vector<std::optional<ConstPlace>>* places, const ConstPlace* target,
                                             const std::vector<uint8_t>* categories) -> std::optional<ConstValue>
{
  struct Call
  {
    ~Call() { --constCalls(); }

    bool ok = ++constCalls() <= 512 && stackRoomLeft();
  } call;
  if (!call.ok)
  {
    if (constWhy().empty())
      constWhy() = "calls nested more than 512 deep";
    return std::nullopt;
  }
  depth = 0; // as a function's body (constRun): the call guard bounds the chain
  const ConstClassLayout* layout = constLayoutOf(t, depth);
  if (!layout)
    return std::nullopt;
  if (!ctor->body && !(ctor->flags & DF_Default) && !ctor->builtin)
  {
    // Declared in the class, defined after it (`Rectangle::Rectangle(int x, ...)`): that body;
    // defined nowhere the evaluator sees, no constant (never the members' defaults).
    FunctionDecl* def = constDefinition(ctor);
    if (!def || def->params.size() != ctor->params.size())
      return std::nullopt;
    return constRunConstructor(def, t, args, depth, own, ctor, places, target, categories);
  }
  if (!ctor->body)
  {
    // `= default`: the default constructor's members from their defaults; a copy's the source.
    if (args.empty())
      return constEmpty(t, false, depth + 1);
    if (args.size() == 1 && args[0].isObject())
    {
      // `C(const C&) = default`: member by member, each part copied (moved) as its type copies.
      const std::optional<ConstPlace> at = places && !places->empty() ? (*places)[0] : std::nullopt;
      ClassDecl* def = classDefinition(t);
      bool moving = def && !ctor->params.empty() && selfParameter(ctor->params[0], def) == 3;
      return constCopyConstruct(t, args[0], at, moving, depth + 1);
    }
    return std::nullopt;
  }
  const Subst& subst = own ? *own : layout->subst;
  LocalStack params;
  for (const Subst::Pair& p : subst.pairs)
    params.push(static_cast<TemplateParamDecl*>(p.param)->name, p.param);
  TypeContext fctx{lexicalScopeOf(declared ? declared : ctor), params.items.empty() ? nullptr : &params, subst.empty() ? nullptr : &subst,
                   ctor->file ? ctor->file->tokens : nullptr, ctor->file ? ctor->file->text : std::string_view()};
  // A class template's constructor: its instance's (constInstanceBody).
  if (FunctionDecl* instance = constInstanceBody(ctor, fctx.subst); instance != ctor)
  {
    ranBody(ctor);
    ctor = instance;
  }
  std::vector<ConstBinding> env;
  ConstFrame frame(&env);
  ConstRunning running(ctor->file);
  fctx.env = &env;
  for (size_t i = 0; i < ctor->params.size(); ++i)
  {
    auto* p = static_cast<ParamDecl*>(ctor->params[i]);
    Expr* defaultValue = p->defaultValue ? p->defaultValue : declared ? static_cast<ParamDecl*>(declared->params[i])->defaultValue : nullptr;
    if (p->isPack)
    {
      // `Holder(int, Args&&... args)`: the arguments from here on, as constRun binds them.
      const Ty* packTy = i + 1 == ctor->params.size() ? declaredTypeOf(p, fctx.subst, depth + 1) : nullptr;
      if (!packTy || packTy->kind != TyKind::Pack || packTy->args.size() != (args.size() > i ? args.size() - i : 0))
        return std::nullopt;
      std::vector<ConstValue> elements;
      std::vector<ConstPlace> elementPlaces;
      for (size_t k = i; k < args.size(); ++k)
      {
        std::optional<ConstValue> e = constConvertTo(args[k], packTy->args[k - i], depth + 1);
        if (!e)
          return std::nullopt;
        elements.push_back(*e);
        bool refers = packTy->args[k - i]->isReference() && places && k < places->size() && (*places)[k];
        elementPlaces.push_back(refers ? *(*places)[k] : ConstPlace{});
      }
      ConstBinding b{p->name, ConstValue::object(packTy, std::move(elements))};
      b.type = packTy;
      b.writable = false;
      b.pack = true;
      b.packPlaces = std::move(elementPlaces);
      env.push_back(std::move(b));
      break;
    }
    // As a function's (constBindParameter): `Text(Text&& other)` given the source's place refers to
    // it (a move empties it); `Optional(T&& v)` given `String("x")` to a temporary of the caller's
    // full expression, which the move leaves empty and that end destroys (it was a copy no one
    // destroyed: "memory allocated at String.kpp:657 is never freed"); `ByValue(String v)` given
    // an lvalue gets a copy by String's copy constructor (it shared the lvalue's block: "memory
    // freed twice"), which the constructor destroys when it returns (constRunBody).
    const Ty* type = p->type ? convertType(p->type, fctx, depth + 1) : nullptr;
    if (!constBindParameter(p, type, i, args, defaultValue, fctx, depth, places, categories, env))
      return std::nullopt;
  }
  // The members' initial values: the bases' in their order, then the data members' in declaration
  // order, as C++ initializes them whatever order the initializers are written in ([class.base.init]
  // 13: `X(int* l) : m(l, 2), L(l, 1)` runs L's constructor first; written order ran m's first).
  size_t nb = layout->bases.size(), nf = layout->fields.size();
  std::vector<const FunctionDecl::CtorInit*> written(nb + nf, nullptr);
  for (const FunctionDecl::CtorInit& init : ctor->ctorInits)
  {
    if (absentMemberInit(layout->decl, init))
      continue; // (design.md 19)
    if (init.member.empty() || init.isPack)
      return std::nullopt;
    uint32_t symbol = init.member.last().symbol;
    bool found = false;
    for (size_t i = 0; i < nf && !found; ++i)
      if (layout->fields[i] && !layout->fields[i]->name.empty() && layout->fields[i]->name.last().symbol == symbol)
      {
        written[nb + i] = &init;
        found = true;
      }
    for (size_t b = 0; b < nb && !found; ++b)
      if (layout->bases[b]->decl && !static_cast<ClassDecl*>(layout->bases[b]->decl)->name.empty() &&
          static_cast<ClassDecl*>(layout->bases[b]->decl)->name.last().symbol == symbol)
      {
        written[b] = &init;
        found = true;
      }
    // A base named through an alias (Tuple's `using Leaf = TupleLeaf<...>;` and `: Leaf(head)`): the
    // type the name gives where the constructor is, under its arguments. (It was no constant: every
    // Tuple at compile time.)
    if (!found && nb)
    {
      NamedType named{};
      named.kind = NodeKind::NamedType;
      named.token = ctor->token;
      named.name = init.member;
      if (const Ty* bt = convertType(&named, fctx, depth + 1); bt && bt->isKnown() && !types.isDependent(bt))
        for (size_t b = 0; b < nb && !found; ++b)
          if (layout->bases[b] && types.unqualified(types.stripRef(bt)) == types.unqualified(layout->bases[b]))
          {
            written[b] = &init;
            found = true;
          }
    }
    if (!found && !(!layout->decl->name.empty() && symbol == layout->decl->name.last().symbol && ctor->ctorInits.size() == 1))
      return std::nullopt; // a member the layout does not hold
  }
  // The values an initializer gives: one expression, a brace list, none (value-initialized).
  auto initialize = [&](const FunctionDecl::CtorInit& init, const Ty* target) -> std::optional<ConstValue> {
    if (init.args.empty())
      return constEmpty(target, true, depth + 1);
    // `ref(r)`, a reference member: the address of what r names (constReferenceTo).
    if (target->isReference() && init.args.size() == 1 && init.args[0] && init.args[0]->kind != NodeKind::BraceInitExpr &&
        !constIsExpansion(init.args[0]))
      if (std::optional<std::optional<ConstValue>> referred = constReferenceTo(init.args[0], target, fctx, depth + 1))
        return *referred;
    bool expands = false;
    for (Expr* a : init.args)
      expands = expands || constIsExpansion(a);
    if (expands)
    {
      // `value(forward<Args>(args)...)`: the values the expansions give; none value-initializes.
      std::vector<ConstValue> values;
      std::vector<std::optional<ConstPlace>> places;
      std::vector<uint8_t> categories;
      if (!constArguments(init.args, fctx, depth, values, &places, &categories))
        return std::nullopt;
      const Ty* tu = types.unqualified(types.stripRef(target));
      if (values.empty())
        return constEmpty(target, true, depth + 1);
      if (target->isReference() && values.size() == 1 && places.size() == 1 && places[0] && categories[0] != 0 && constAddressable(*places[0]))
        return ConstValue::address(types.pointer(types.stripRef(target)), constStorageTarget(*places[0])); // a reference member: where it refers
      if (tu->kind == TyKind::Class)
        return values.size() == 1 && values[0].isObject() && values[0].type && types.unqualified(types.stripRef(values[0].type)) == tu
                 ? constCopyConstruct(tu, values[0], places[0], categories[0] != 1, depth + 1) // copied, or moved from where it is
                 : constConstruct(tu, values, false, depth + 1, nullptr, nullptr, &places, &categories);
      return values.size() == 1 ? constConvertTo(values[0], target, depth + 1) : std::nullopt;
    }
    if (init.args.size() == 1)
    {
      const Ty* tu = types.unqualified(types.stripRef(target));
      if (init.braced && tu->kind == TyKind::Class && init.args[0]->kind != NodeKind::BraceInitExpr)
      {
        BraceInitExpr list{};
        list.kind = NodeKind::BraceInitExpr;
        list.token = init.args[0]->token;
        list.args = init.args;
        return constBraceInit(tu, &list, fctx, depth + 1);
      }
      if (!init.braced && tu->kind == TyKind::Class && !target->isReference() && init.args[0]->kind != NodeKind::BraceInitExpr)
      {
        // `bits(toBits(values))`: a direct-initialization, which an explicit constructor takes
        // too (SmallBitset(uint64_t)); an object of the class, or of one derived from it
        // (sliced), is copied.
        // `a(x)`: copied (or moved, `a(move(x))`) by the class's copy constructor, an implicit
        // one member by member.
        std::optional<ConstValue> evaluated;
        if (std::optional<std::optional<ConstValue>> copied = constCopyInit(init.args[0], tu, fctx, depth + 1, false, &evaluated))
          return *copied;
        std::optional<ConstValue> v = evaluated ? evaluated : constValue(init.args[0], fctx, depth + 1); // (evaluated once: `c(*it++)`)
        if (!v)
          return std::nullopt;
        if (v->isObject() && v->type && types.unqualified(types.stripRef(v->type)) == tu)
          return v; // a prvalue: the object
        if (v->isObject() && v->type && constBasePath(v->type, tu, 0))
        {
          // Sliced: the base part copied as the base copies (where it is not known: a move,
          // which would empty it there, is no constant).
          std::optional<ConstValue> sliced = constConvertTo(v, tu, depth + 1);
          Expr* source = nullptr;
          uint8_t category = constCategory(init.args[0], source);
          if (!sliced || !constCopyRunsCode(tu))
            return sliced;
          return category == 2 ? std::nullopt : constCopyConstruct(tu, *sliced, std::nullopt, false, depth + 1);
        }
        return constConstruct(tu, {*v}, false, depth + 1);
      }
      return constInitialize(target, init.args[0], fctx, depth + 1);
    }
    std::vector<ConstValue> values;
    if (!constArguments(init.args, fctx, depth, values))
      return std::nullopt;
    return constConstruct(target, values, false, depth + 1);
  };
  for (const FunctionDecl::CtorInit& init : ctor->ctorInits)
    if (!absentMemberInit(layout->decl, init) && std::find(written.begin(), written.end(), &init) == written.end())
    {
      // `Frame(int x) : Frame(x, 0) {}`: the delegated-to constructor makes the object.
      std::optional<ConstValue> whole = initialize(init, t);
      if (!whole)
        return std::nullopt;
      return constRunBody(ctor, t, *whole, fctx, env, depth, target);
    }
  if (!constMayMake(nb + nf))
    return std::nullopt;
  // The object where it is made, `this` while its parts are (the target, else a binding of this
  // run's own): an initializer reads the parts made before it (`b(a + 1)`, `int b = a + 1;`, a
  // member function they call), one not made yet is unset, no constant ([class.base.init] 13; they
  // were no constant: language/consteval_initializer_reads_earlier_member).
  std::vector<ConstValue> unset;
  for (size_t b = 0; b < nb; ++b)
    unset.push_back(ConstValue::unset(layout->bases[b]));
  for (size_t i = 0; i < nf; ++i)
    unset.push_back(ConstValue::unset(layout->fieldTypes[i]));
  ConstPlace home;
  if (target)
  {
    if (!constWrite(*target, ConstValue::object(t, std::move(unset))))
      return std::nullopt;
    home = constResolve(*target);
    ConstBinding self{constThisSymbol, ConstValue::unset(t)};
    self.type = t;
    self.aliasEnv = home.env;
    self.aliasIndex = home.index;
    self.aliasPath = home.path;
    env.push_back(std::move(self));
  }
  else
  {
    env.push_back(constBinding(constThisSymbol, ConstValue::object(t, std::move(unset)), t));
    env.back().writable = true; // (a const object's constructor makes it too)
    home = ConstPlace{&env, env.size() - 1, {}, t};
  }
  size_t made = env.size() - 1;
  bool union_ = layout->decl && layout->decl->keyword == Kw::union_;
  for (size_t k = 0; k < nb + nf; ++k)
  {
    bool base = k < nb;
    const Ty* partType = base ? layout->bases[k] : layout->fieldTypes[k - nb];
    if (union_ && !base && !written[k] && !(layout->fields[k - nb] && layout->fields[k - nb]->init))
      continue; // (a union's member no initializer names: none it holds, nothing made)
    std::optional<ConstValue> part = written[k] ? initialize(*written[k], partType)
                                   : base     ? constEmpty(partType, false, depth + 1)
                                              : constFieldDefaultOn(*layout, k - nb, false, home, depth);
    if (!part || made >= env.size() || env[made].symbol != constThisSymbol)
      return std::nullopt;
    ConstPlace at = home;
    at.path.push_back(k);
    at.type = partType;
    at.bits = base ? 0 : layout->fieldBits[k - nb];
    bool held = !union_ || written[k] || (!base && layout->fields[k - nb] && layout->fields[k - nb]->init); // (a union's member without an initializer is none it holds)
    if (held && !constWrite(at, *part))
      return std::nullopt;
  }
  std::optional<ConstValue> built = constRead(home);
  if (!built || !built->isObject() || !built->parts || built->parts->size() != nb + nf)
    return std::nullopt;
  std::vector<ConstValue> object(built->parts->begin(), built->parts->end());
  constFitBitFields(*layout, object);
  return constRunBody(ctor, t, ConstValue::object(t, std::move(object)), fctx, env, depth, target);
}

auto GlobalScope::Impl::constRunBody(FunctionDecl* ctor, const Ty* t, const ConstValue& object, TypeContext& fctx, std::vector<ConstBinding>& env, int depth, const ConstPlace* target) -> std::optional<ConstValue>
{
  if (target)
  {
    // Made where it lives: the members written there, `this` that place (what the body points at
    // the object points at it there, a List's sentinel at itself).
    if (!constWrite(*target, object))
      return std::nullopt;
    ConstPlace at = constResolve(*target);
    ConstBinding self{constThisSymbol, ConstValue::unset(t)};
    self.type = t;
    self.aliasEnv = at.env;
    self.aliasIndex = at.index;
    self.aliasPath = at.path;
    env.push_back(std::move(self));
  }
  else
    env.push_back(constBinding(constThisSymbol, object, t));
  size_t self = env.size() - 1;
  std::optional<ConstValue> result;
  ranBody(ctor);
  ConstFlow flow = constStatement(ctor->body, fctx, env, result, depth + 1);
  if (flow != ConstFlow::Next && flow != ConstFlow::Returned)
    return std::nullopt;
  if (self >= env.size() || env[self].symbol != constThisSymbol)
    return std::nullopt;
  std::optional<ConstValue> made = target ? constRead(*target) : std::optional<ConstValue>(env[self].value);
  // The parameters end with the constructor: a by-value one it made is destroyed (`ByValue(String
  // v) : s(move(v))`: v's empty String), as a function's are (constRun).
  if (!constEndLocals(env, 0, depth, true))
    return std::nullopt;
  return made;
}

auto GlobalScope::Impl::constThis(const TypeContext& ctx) -> ConstBinding*
{
  if (!ctx.env)
    return nullptr;
  for (size_t i = ctx.env->size(); i-- > 0;)
    if ((*ctx.env)[i].symbol == constThisSymbol)
      return &(*ctx.env)[i];
  return nullptr;
}

auto GlobalScope::Impl::constOperandType(const ConstValue& v, const ConstOperand* o) -> const Ty*
{
  const Ty* t = constTypeOfValue(v);
  if (!o || o->category != 1 || !t || t->isReference() || t->isBraceList())
    return t;
  bool readOnly = o->place && !o->place->writable();
  return types.lref(readOnly ? types.qualified(t, true, false) : t);
}

auto GlobalScope::Impl::constOperandOf(Expr* e, const ConstValue& v, const TypeContext& ctx, int depth) -> ConstOperand
{
  ConstOperand o;
  Expr* source = nullptr;
  o.category = constCategory(e, source);
  if (!v.isObject() || !v.type || v.type->isBraceList() || types.unqualified(types.stripRef(v.type))->kind != TyKind::Class)
    return o;
  if (o.category == 0)
    o.place = constMaterialize(v, v.type); // `name() + "/in"`: a temporary of the full expression
  else if (constPure(source))
    o.place = constPlaceOf(source, ctx, depth + 1);
  return o;
}

auto GlobalScope::Impl::constResolve(ConstPlace p) -> ConstPlace
{
  for (int n = 0; n < 64 && p.binding().aliasEnv; ++n)
  {
    const ConstBinding& b = p.binding();
    std::vector<size_t> path = b.aliasPath;
    path.insert(path.end(), p.path.begin(), p.path.end());
    p.readOnly = p.readOnly || !b.writable;
    p.env = b.aliasEnv;
    p.index = b.aliasIndex;
    p.path = std::move(path);
  }
  return p;
}

auto GlobalScope::Impl::constStorage() -> std::unordered_map<uint64_t, ConstStorage>&
{
  static thread_local std::unordered_map<uint64_t, ConstStorage> storage;
  return storage;
}

auto GlobalScope::Impl::constFrames() -> std::vector<const void*>&
{
  static thread_local std::vector<const void*> frames;
  return frames;
}

auto GlobalScope::Impl::constRunningFile() -> const ParsedFile*&
{
  static thread_local const ParsedFile* file = nullptr;
  return file;
}

auto GlobalScope::Impl::constFailedAt() -> ConstWhere&
{
  static thread_local ConstWhere at;
  return at;
}

auto GlobalScope::Impl::constWhereText(const ConstWhere& w) -> std::string
{
  if (!w.file || !w.file->tokens || w.token >= w.file->tokens->size())
    return "a place the evaluator does not know";
  std::string_view text = w.file->text;
  size_t offset = std::min<size_t>((*w.file->tokens)[w.token].offset, text.size());
  uint32_t line = 1 + uint32_t(std::count(text.begin(), text.begin() + offset, '\n'));
  return w.file->path.substr(w.file->path.find_last_of("/\\") + 1) + ":" + std::to_string(line);
}

auto GlobalScope::Impl::constHeap() -> std::vector<ConstBinding>&
{
  static thread_local std::vector<ConstBinding> heap;
  return heap;
}

auto GlobalScope::Impl::constAllocations() -> std::vector<ConstAllocation>&
{
  static thread_local std::vector<ConstAllocation> allocations;
  return allocations;
}

auto GlobalScope::Impl::constStorageTarget(const ConstPlace& place) -> ConstTarget
{
  static thread_local uint64_t serials = 0;
  ConstBinding& b = place.binding();
  if (!b.serial)
    b.serial = ++serials;
  const Ty* whole = b.type && !b.type->isReference() ? b.type : b.value.type;
  constStorage()[b.serial] = ConstStorage{place.env, place.index, b.symbol, whole};
  ConstTarget t;
  t.storage = b.serial;
  t.path = place.path;
  return t;
}

auto GlobalScope::Impl::constStorageName(const ConstStorage& s) -> std::string
{
  // (The evaluation's own bindings, `this`, a range-for's range, a structured binding's object, are
  // named by no symbol.)
  bool own = s.symbol == constThisSymbol || s.symbol == constRangeSymbol || s.symbol == constHiddenSymbol;
  return s.symbol && !own ? std::string(symbols.name(s.symbol)) : !s.symbol ? std::string("a temporary") : std::string("an object of the evaluation");
}

auto GlobalScope::Impl::constLiveStorage(const ConstTarget& t, const char* use) -> const ConstStorage*
{
  auto it = constStorage().find(t.storage);
  if (it == constStorage().end())
  {
    if (constWhy().empty())
      constWhy() = std::string(use) + " an object whose lifetime ended";
    return nullptr;
  }
  const ConstStorage& s = it->second;
  if (s.env == &constHeap() && s.index < constAllocations().size() && constAllocations()[s.index].isFreed)
  {
    if (constWhy().empty())
      constWhy() = std::string(use) + " memory freed at " + constWhereText(constAllocations()[s.index].freed);
    return nullptr;
  }
  const std::vector<const void*>& frames = constFrames();
  if (std::find(frames.begin(), frames.end(), s.env) == frames.end() || s.index >= s.env->size() || (*s.env)[s.index].serial != t.storage)
  {
    if (constWhy().empty())
      constWhy() = std::string(use) + " " + constStorageName(s) + ", whose lifetime ended";
    return nullptr;
  }
  return &s;
}

auto GlobalScope::Impl::constStoragePlace(const ConstValue& p, const char* use, int depth) -> std::optional<ConstPlace>
{
  if (!p.isAddress() || !p.target || !p.target->storage)
    return std::nullopt;
  const ConstStorage* s = constLiveStorage(*p.target, use);
  if (!s || !constAddressDereferenceable(p, depth))
    return std::nullopt;
  const Ty* pt = p.type ? types.unqualified(types.stripRef(p.type)) : nullptr;
  ConstPlace place{s->env, s->index, p.target->path, pt && pt->kind == TyKind::Pointer ? pt->inner : nullptr};
  place.readOnly = place.type && place.type->isConst;
  return place;
}

auto GlobalScope::Impl::constHeldStorage(const ConstValue& v, int depth) -> const ConstStorage*
{
  if (v.isAddress() && v.target && v.target->storage)
  {
    static const ConstStorage unknown;
    auto it = constStorage().find(v.target->storage);
    return it != constStorage().end() ? &it->second : &unknown;
  }
  if ((v.isObject() || v.isPointer()) && v.parts && depth < 64)
    for (const ConstValue& part : *v.parts)
      if (const ConstStorage* s = constHeldStorage(part, depth + 1))
        return s;
  return nullptr;
}

auto GlobalScope::Impl::constCAllocation(std::string_view name, const std::vector<ConstValue>& args, CallExpr* c, int depth) -> std::optional<ConstValue>
{
  auto fail = [&](const std::string& why) -> std::optional<ConstValue> {
    if (constWhy().empty())
      constWhy() = why;
    return std::nullopt;
  };
  const Ty* voidPointer = types.pointer(types.voidType());
  auto block = [&](const ConstValue& p) -> std::optional<size_t> {
    auto it = p.isAddress() && p.target->storage ? constStorage().find(p.target->storage) : constStorage().end();
    if (it == constStorage().end() || it->second.env != &constHeap())
      return std::nullopt;
    return it->second.index;
  };
  // Whether path designates the start of the object of type t, whose address is that object's:
  // element 0, each class's first base, or its first member where it has none, in classes with no
  // virtual function, own or a base's (no vtable pointer before them, nor bases reordered).
  // `free(node)` with node a ForwardLinks*, the ForwardNode's base: the block malloc gave.
  std::function<bool(const ConstClassLayout*, int)> polymorphic = [&](const ConstClassLayout* l, int d) {
    if (!l || !l->decl || d > 32)
      return true;
    for (Decl* m : membersRead(l->decl))
      if (m->kind == NodeKind::FunctionDecl && (m->flags & (DF_Virtual | DF_Override)))
        return true;
    for (const Ty* b : l->bases)
      if (polymorphic(constLayoutOf(b, depth + 1), d + 1))
        return true;
    return false;
  };
  auto atStart = [&](const Ty* t, const std::vector<size_t>& path) -> bool {
    for (size_t k = 0; k < path.size(); ++k)
    {
      t = t ? types.unqualified(types.stripRef(t)) : nullptr;
      if (!t || path[k] != 0)
        return false;
      if (t->kind == TyKind::Array)
      {
        t = t->inner;
        continue;
      }
      const ConstClassLayout* l = t->kind == TyKind::Class ? constLayoutOf(t, depth + 1) : nullptr;
      if (!l || l->anonymous || polymorphic(l, 0))
        return false;
      if (!l->bases.empty())
        t = l->bases[0];
      else if (!l->fieldTypes.empty() && !l->fieldBits[0])
        t = l->fieldTypes[0];
      else
        return false;
    }
    return true;
  };
  auto release = [&](const ConstValue& p, const char* what) -> std::optional<size_t> {
    std::optional<size_t> index = block(p);
    if (!index || !constAllocations()[*index].raw)
    {
      fail(std::string(what) + " of memory malloc did not allocate" +
           (index ? " (new allocated it at " + constWhereText(constAllocations()[*index].allocated) + ")" : std::string()));
      return std::nullopt;
    }
    ConstAllocation& a = constAllocations()[*index];
    if (a.isFreed)
    {
      fail("memory freed twice (first at " + constWhereText(a.freed) + ")");
      return std::nullopt;
    }
    const Ty* typed = constHeap()[*index].type; // (the array of objects made in it, if any)
    if (typed ? !atStart(typed, p.target->path) : !p.target->path.empty())
    {
      fail(std::string(what) + " of a pointer into memory allocated at " + constWhereText(a.allocated));
      return std::nullopt;
    }
    return index;
  };
  auto allocate = [&](int64_t bytes, bool zero) -> std::optional<ConstValue> {
    if (bytes < 0 || !constMayMake(size_t(bytes) / 64 + 1))
      return std::nullopt;
    std::vector<ConstBinding>& heap = constHeap();
    heap.push_back(ConstBinding{0, ConstValue::unset(nullptr)});
    ConstAllocation a;
    a.allocated = constHere(c);
    a.raw = true;
    a.zero = zero;
    a.bytes = bytes;
    constAllocations().push_back(a);
    return ConstValue::address(voidPointer, constStorageTarget(ConstPlace{&heap, heap.size() - 1, {}, nullptr}));
  };
  for (const ConstValue& a : args)
    if (a.isUnset())
      return std::nullopt;
  if (name == "malloc")
    return args[0].kind == ConstValue::Kind::Int ? allocate(args[0].i, false) : std::nullopt;
  if (name == "calloc")
    return args.size() == 2 && args[0].kind == ConstValue::Kind::Int && args[1].kind == ConstValue::Kind::Int ? allocate(args[0].i * args[1].i, true) : std::nullopt;
  bool null = args[0].kind == ConstValue::Kind::Int && args[0].i == 0;
  if (name == "free")
  {
    if (null)
      return ConstValue::integer(0, types.voidType());
    std::optional<size_t> index = release(args[0], "free");
    if (!index)
      return std::nullopt;
    constAllocations()[*index].isFreed = true; // (no destructor: free ends what it holds without one)
    constAllocations()[*index].freed = constHere(c);
    constHeap()[*index].value = ConstValue::unset(constHeap()[*index].type);
    return ConstValue::integer(0, types.voidType());
  }
  // realloc: a new block, the old one's elements moved over bitwise (trivially copyable only)
  if (args.size() != 2 || args[1].kind != ConstValue::Kind::Int)
    return std::nullopt;
  if (null)
    return allocate(args[1].i, false);
  std::optional<size_t> old = release(args[0], "realloc");
  if (!old)
    return std::nullopt;
  const Ty* oldType = constHeap()[*old].type;
  ConstValue oldValue = constHeap()[*old].value;
  std::optional<ConstValue> grown = allocate(args[1].i, false);
  if (!grown)
    return std::nullopt;
  if (oldType)
  {
    const Ty* element = types.unqualified(oldType->inner);
    std::optional<bool> trivial = triviallyCopyable(element);
    if (!trivial || !*trivial)
      return fail("realloc of " + types.toString(element, &symbols) + ", which is not trivially copyable");
    std::optional<ConstValue> typed = constTypeAllocation(*grown, element, depth);
    if (!typed)
      return std::nullopt;
    std::optional<size_t> index = block(*typed);
    std::vector<ConstValue> elements = *constHeap()[*index].value.parts;
    for (size_t i = 0; i < elements.size() && oldValue.isObject() && i < oldValue.parts->size(); ++i)
      elements[i] = (*oldValue.parts)[i];
    constHeap()[*index].value = ConstValue::object(constHeap()[*index].type, std::move(elements));
    grown = typed;
    grown->type = voidPointer;
  }
  constAllocations()[*old].isFreed = true;
  constAllocations()[*old].freed = constHere(c);
  constHeap()[*old].value = ConstValue::unset(oldType);
  return grown;
}

auto GlobalScope::Impl::constTypeAllocation(const ConstValue& p, const Ty* t, int depth, bool retype) -> std::optional<ConstValue>
{
  auto it = p.isAddress() && p.target->storage ? constStorage().find(p.target->storage) : constStorage().end();
  if (it == constStorage().end() || it->second.env != &constHeap())
    return std::nullopt;
  size_t index = it->second.index;
  ConstAllocation& a = constAllocations()[index];
  if (!a.raw || a.isFreed)
    return std::nullopt;
  if (constHeap()[index].type)
  {
    const ConstValue& held = constHeap()[index].value;
    bool untouched = retype && p.target->path == std::vector<size_t>{0} && held.isObject() && held.parts;
    for (size_t i = 0; untouched && i < held.parts->size(); ++i)
      untouched = (*held.parts)[i].isUnset();
    if (!untouched)
      return std::nullopt;
    constHeap()[index].type = nullptr; // typed anew below
    constHeap()[index].value = ConstValue::unset(nullptr);
  }
  else if (!p.target->path.empty())
    return std::nullopt;
  t = types.unqualified(t);
  std::optional<int64_t> size = constSizeOf(t, depth);
  if (!size || *size <= 0 || t->kind == TyKind::Void)
    return std::nullopt;
  if (a.bytes % *size != 0)
  {
    if (constWhy().empty())
      constWhy() = std::to_string(a.bytes) + " bytes allocated at " + constWhereText(a.allocated) + " used as objects of " + types.toString(t, &symbols) +
                   ", which are " + std::to_string(*size) + " bytes";
    return std::nullopt;
  }
  int64_t count = a.bytes / *size;
  if (!constMayMake(size_t(count)))
    return std::nullopt;
  std::vector<ConstValue> elements;
  for (int64_t i = 0; i < count; ++i)
  {
    std::optional<ConstValue> e = constRawObject(t, a.zero, depth + 1);
    if (!e)
      return std::nullopt;
    elements.push_back(*e);
  }
  const Ty* array = types.array(t, uint32_t(count));
  constHeap()[index].type = array;
  constHeap()[index].value = ConstValue::object(array, std::move(elements));
  it->second.type = array;
  ConstTarget first = *p.target;
  first.path = {0};
  return ConstValue::address(types.pointer(t), std::move(first));
}

auto GlobalScope::Impl::constRawObject(const Ty* t, bool zero, int depth) -> std::optional<ConstValue>
{
  t = t ? types.unqualified(types.stripRef(t)) : nullptr;
  if (!t || depth > 64)
    return std::nullopt;
  if (t->isArithmetic() || t->kind == TyKind::Enum || t->kind == TyKind::Pointer)
    return zero ? constConverted(ConstValue::integer(0), t) : std::optional<ConstValue>(ConstValue::unset(t));
  if (t->kind == TyKind::Array)
  {
    if (t->count == 0)
      return ConstValue::object(t, {});
    if (!constMayMake(t->count))
      return std::nullopt;
    std::optional<ConstValue> element = constRawObject(t->inner, zero, depth + 1);
    if (!element)
      return std::nullopt;
    return ConstValue::object(t, std::vector<ConstValue>(t->count, *element));
  }
  const ConstClassLayout* layout = t->kind == TyKind::Class ? constLayoutOf(t, depth) : nullptr;
  // (A class whose objects malloc makes none of: a union, whose member alive is not known, a
  // typed union, one with a virtual function or one of a base, an anonymous member's.)
  std::function<bool(const ConstClassLayout*, int)> implicitLifetime = [&](const ConstClassLayout* l, int d) {
    if (!l || !l->decl || l->anonymous || l->decl->isTypedUnion || l->decl->keyword == Kw::union_ || d > 32)
      return false;
    for (Decl* m : membersRead(l->decl))
      if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Virtual))
        return false;
    for (const Ty* b : l->bases)
      if (!implicitLifetime(constLayoutOf(b, depth + 1), d + 1))
        return false;
    return true;
  };
  if (!implicitLifetime(layout, 0))
    return zero ? constEmpty(t, true, depth + 1) : std::optional<ConstValue>(ConstValue::unset(t)); // (as before)
  if (!constMayMake(layout->bases.size() + layout->fields.size()))
    return std::nullopt;
  std::vector<ConstValue> parts;
  for (const Ty* b : layout->bases)
  {
    std::optional<ConstValue> part = constRawObject(b, zero, depth + 1);
    if (!part)
      return std::nullopt;
    parts.push_back(*part);
  }
  for (const Ty* f : layout->fieldTypes)
  {
    std::optional<ConstValue> part = f && f->isReference() ? std::optional<ConstValue>(ConstValue::unset(f)) : constRawObject(f, zero, depth + 1);
    if (!part)
      return std::nullopt;
    parts.push_back(*part);
  }
  constFitBitFields(*layout, parts);
  return ConstValue::object(t, std::move(parts));
}

auto GlobalScope::Impl::constPointedType(const ConstValue& p, int depth) -> const Ty*
{
  if (!p.isAddress() || !p.target)
    return nullptr;
  const ConstTarget& t = *p.target;
  const Ty* whole = constTargetType(t, depth);
  const Ty* part = whole ? constPartType(whole, t.path, t.path.size(), depth) : nullptr;
  return part ? types.unqualified(types.stripRef(part)) : nullptr;
}

auto GlobalScope::Impl::constBitCast(const ConstValue& v, const Ty* to, int depth) -> std::optional<ConstValue>
{
  std::optional<int64_t> fromSize = v.type ? constSizeOf(v.type, depth) : std::nullopt, toSize = constSizeOf(to, depth);
  if (!v.isScalar() || !fromSize || !toSize || *fromSize != *toSize || (*toSize != 4 && *toSize != 8))
    return std::nullopt;
  uint64_t bits = 0;
  if (v.isFloat())
  {
    if (*fromSize == 4)
    {
      float f = float(v.f);
      uint32_t b;
      memcpy(&b, &f, 4);
      bits = b;
    }
    else
      memcpy(&bits, &v.f, 8);
  }
  else
    bits = uint64_t(v.i) & (*fromSize == 4 ? 0xFFFFFFFFull : ~0ull);
  if (to->kind == TyKind::Float)
  {
    if (*toSize == 4)
    {
      uint32_t b = uint32_t(bits);
      float f;
      memcpy(&f, &b, 4);
      return ConstValue::floating(f, to);
    }
    double d;
    memcpy(&d, &bits, 8);
    return ConstValue::floating(d, to);
  }
  if (!to->isArithmetic() && to->kind != TyKind::Enum)
    return std::nullopt;
  return constArith().convert(ConstValue::integer(int64_t(bits), types.intType(64, true)), to);
}

auto GlobalScope::Impl::constMemcpy(const ConstValue& to, const ConstValue& from, const ConstValue& count, int depth) -> std::optional<ConstValue>
{
  auto fail = [&](const std::string& why) -> std::optional<ConstValue> {
    if (constWhy().empty())
      constWhy() = why;
    return std::nullopt;
  };
  if (count.kind != ConstValue::Kind::Int || count.i < 0)
    return std::nullopt;
  if (count.i == 0)
    return to;
  if (from.isObject() && from.type && types.unqualified(from.type)->kind == TyKind::Array && types.unqualified(from.type)->inner)
    return constMemcpy(to, ConstValue::pointer(types.pointer(types.unqualified(from.type)->inner), from.parts, 0), count, depth); // `memcpy(text, "abc", 3)`
  const Ty* target = constPointedType(to, depth);
  const Ty* source = from.isAddress() ? constPointedType(from, depth) : from.isPointer() && from.parts && from.i >= 0 && size_t(from.i) < from.parts->size() ? (*from.parts)[size_t(from.i)].type : nullptr;
  if (!to.isAddress() || !to.target->storage || !target || !source)
    return fail("a memcpy the evaluator cannot follow (not between objects of the evaluation)");
  source = types.unqualified(types.stripRef(source));
  std::optional<int64_t> targetSize = constSizeOf(target, depth), sourceSize = constSizeOf(source, depth);
  auto read = [&](const ConstValue& p) -> std::optional<ConstValue> { return p.isAddress() ? constReadTarget(p, depth + 1) : constDeref(p); };
  auto move = [&](const ConstValue& p, int64_t n) -> std::optional<ConstValue> {
    return p.isAddress() ? constAddressMove(p, n, depth) : constPointerArith(P::Plus, p, ConstValue::integer(n, types.intType()));
  };
  if (targetSize && sourceSize && *targetSize == count.i && *sourceSize == count.i)
  {
    std::optional<ConstValue> v = read(from);
    if (v && target != source)
      v = target->kind == TyKind::Class || source->kind == TyKind::Class ? std::nullopt : constBitCast(*v, target, depth);
    std::optional<ConstPlace> place = v ? constStoragePlace(to, "a memcpy into", depth) : std::nullopt;
    if (!place)
      return v ? std::nullopt : fail("a memcpy between objects of different types: " + types.toString(source, &symbols) + " into " + types.toString(target, &symbols));
    place->type = target;
    return constWrite(*place, *v) ? std::optional<ConstValue>(to) : std::nullopt;
  }
  if (target != source || !targetSize || !*targetSize || count.i % *targetSize)
    return fail("a memcpy of " + std::to_string(count.i) + " bytes between " + types.toString(source, &symbols) + " and " + types.toString(target, &symbols));
  int64_t n = count.i / *targetSize;
  std::vector<ConstValue> elements;
  for (int64_t i = 0; i < n; ++i)
  {
    // An element no one wrote is copied as such (memcpy copies indeterminate bytes; reading the
    // copy is what is no constant).
    std::optional<ConstValue> at = move(from, i);
    std::optional<ConstPlace> place = at && at->isAddress() ? constStoragePlace(*at, "a memcpy from", depth) : std::nullopt;
    std::optional<ConstValue> v = place ? constPartOrUnset(*place) : at ? read(*at) : std::nullopt;
    if (!v)
      return fail("a memcpy reading past what the source holds");
    elements.push_back(*v);
  }
  for (int64_t i = 0; i < n; ++i)
  {
    std::optional<ConstValue> at = move(to, i);
    std::optional<ConstPlace> place = at ? constStoragePlace(*at, "a memcpy into", depth) : std::nullopt;
    if (!place)
      return fail("a memcpy writing past what the target holds");
    place->type = target;
    if (!constWrite(*place, elements[size_t(i)]))
      return std::nullopt;
  }
  return to;
}

auto GlobalScope::Impl::constBytePointer(const ConstValue& p) -> std::optional<ConstValue>
{
  if (p.isObject() && p.type && types.unqualified(p.type)->kind == TyKind::Array && types.unqualified(p.type)->inner)
    return ConstValue::pointer(types.pointer(types.unqualified(p.type)->inner), p.parts, 0); // an array given as it is
  return p.isPointer() || p.isAddress() ? std::optional<ConstValue>(p) : std::nullopt;
}

auto GlobalScope::Impl::constByteAt(const ConstValue& p, int64_t i, const char* why, int depth) -> std::optional<int>
{
  std::optional<ConstValue> x = p.isAddress() ? constAddressMove(p, i, depth) : constPointerArith(P::Plus, p, ConstValue::integer(i, types.intType()));
  std::optional<ConstValue> c = x ? (x->isAddress() ? constReadTarget(*x, depth + 1) : constDeref(*x)) : std::nullopt;
  if (!c || c->kind != ConstValue::Kind::Int)
    return std::nullopt;
  if (std::optional<int64_t> width = c->type ? constSizeOf(c->type, depth) : std::nullopt; !width || *width != 1)
  {
    if (constWhy().empty())
      constWhy() = std::string("a ") + why + " of elements wider than a byte";
    return std::nullopt;
  }
  return int(uint8_t(c->i));
}

auto GlobalScope::Impl::constMemcmp(const ConstValue& a, const ConstValue& b, const ConstValue& count, int depth) -> std::optional<ConstValue>
{
  if (count.kind != ConstValue::Kind::Int || count.i < 0)
    return std::nullopt;
  std::optional<ConstValue> pa = constBytePointer(a), pb = constBytePointer(b);
  if (!pa || !pb)
    return std::nullopt;
  for (int64_t i = 0; i < count.i; ++i)
  {
    std::optional<int> ux = constByteAt(*pa, i, "memcmp", depth), uy = ux ? constByteAt(*pb, i, "memcmp", depth) : std::nullopt;
    if (!ux || !uy)
      return std::nullopt;
    if (*ux != *uy)
      return ConstValue::integer(*ux < *uy ? -1 : 1, types.intType());
  }
  return ConstValue::integer(0, types.intType());
}

auto GlobalScope::Impl::constMemchr(const ConstValue& p, const ConstValue& byte, const ConstValue& count, int depth) -> std::optional<ConstValue>
{
  if (count.kind != ConstValue::Kind::Int || count.i < 0 || byte.kind != ConstValue::Kind::Int)
    return std::nullopt;
  std::optional<ConstValue> pa = constBytePointer(p);
  if (!pa)
    return std::nullopt;
  for (int64_t i = 0; i < count.i; ++i)
  {
    std::optional<int> u = constByteAt(*pa, i, "memchr", depth);
    if (!u)
      return std::nullopt;
    if (*u == int(uint8_t(byte.i)))
      return pa->isAddress() ? constAddressMove(*pa, i, depth) : constPointerArith(P::Plus, *pa, ConstValue::integer(i, types.intType()));
  }
  return constConverted(ConstValue::integer(0), types.pointer(types.voidType()));
}

auto GlobalScope::Impl::constStrcmp(const ConstValue& a, const ConstValue& b, int depth) -> std::optional<ConstValue>
{
  std::optional<ConstValue> pa = constBytePointer(a), pb = constBytePointer(b);
  if (!pa || !pb)
    return std::nullopt;
  for (int64_t i = 0;; ++i)
  {
    if (++constSteps() > 1000000)
      return std::nullopt;
    std::optional<int> ux = constByteAt(*pa, i, "strcmp", depth), uy = ux ? constByteAt(*pb, i, "strcmp", depth) : std::nullopt;
    if (!ux || !uy)
      return std::nullopt; // (past an array's end: no terminating zero)
    if (*ux != *uy)
      return ConstValue::integer(*ux < *uy ? -1 : 1, types.intType());
    if (*ux == 0)
      return ConstValue::integer(0, types.intType());
  }
}

auto GlobalScope::Impl::constMemset(const ConstValue& to, const ConstValue& byte, const ConstValue& count, int depth) -> std::optional<ConstValue>
{
  const Ty* target = constPointedType(to, depth);
  std::optional<int64_t> size = target ? constSizeOf(target, depth) : std::nullopt;
  if (!to.isAddress() || !to.target->storage || byte.kind != ConstValue::Kind::Int || count.kind != ConstValue::Kind::Int || count.i < 0 || !size ||
      !*size || count.i % *size)
  {
    if (constWhy().empty())
      constWhy() = "a memset the evaluator cannot follow";
    return std::nullopt;
  }
  bool chars = target->kind == TyKind::Char;
  if (!chars && (byte.i & 0xFF) != 0)
  {
    if (constWhy().empty())
      constWhy() = "a memset of a nonzero byte over " + types.toString(target, &symbols);
    return std::nullopt;
  }
  for (int64_t i = 0; i < count.i / *size; ++i)
  {
    std::optional<ConstValue> at = constAddressMove(to, i, depth);
    std::optional<ConstPlace> place = at ? constStoragePlace(*at, "a memset of", depth) : std::nullopt;
    std::optional<ConstValue> v = chars ? constArith().convert(ConstValue::integer(byte.i & 0xFF), target) : constEmpty(target, true, depth + 1);
    if (!place || !v)
      return std::nullopt;
    place->type = target;
    if (!constWrite(*place, *v))
      return std::nullopt;
  }
  return to;
}

auto GlobalScope::Impl::constAddressable(const ConstPlace& p) -> bool
{
  const ConstBinding& b = p.binding();
  return b.owns || b.symbol == constThisSymbol || p.env == &constHeap();
}

auto GlobalScope::Impl::constTextOf(const std::vector<ConstValue>& values, const std::vector<std::optional<ConstPlace>>* places, size_t from, int depth) -> std::optional<ConstValue>
{
  Decl* pieceDecl = libraryDecl("TextPiece", NodeKind::ClassDecl);
  const Ty* textTy = textType();
  const Ty* countTy = textTy ? constPartType(textTy, {1}, 1, depth + 1) : nullptr;
  if (!pieceDecl || !countTy)
    return std::nullopt;
  const Ty* pieceTy = selfType(pieceDecl);
  std::vector<ConstValue> pieces;
  for (size_t i = from; i < values.size(); ++i)
  {
    const ConstValue& v = values[i];
    std::optional<ConstPlace> place = places && i < places->size() ? (*places)[i] : std::nullopt;
    // (An array variable's value reads as the pointer it decays to: its place has its type.)
    const Ty* type = !v.isObject() && place && place->type && !place->type->isReference() ? place->type : v.type;
    if (const Ty* array = type ? types.unqualified(types.stripRef(type)) : nullptr;
        array && array->kind == TyKind::Array && !array->count && v.isObject() && v.parts)
      type = types.array(array->inner, uint32_t(v.parts->size())); // a string literal's chars, `const char[]` as it reads
    TextPieceKind kind = textPieceOf(nullptr, type);
    if (!kind.thunk)
    {
      if (constWhy().empty())
        constWhy() = kind.error.empty() ? "a piece of a Text of no known type" : kind.error;
      return std::nullopt;
    }
    // Its own object, or a temporary of the full expression (a value, or a string literal, whose
    // place does not know its length).
    if (!place || !constAddressable(*place) || !place->type ||
        types.unqualified(types.stripRef(place->type)) != types.unqualified(types.stripRef(type)))
      place = constMaterialize(v, type);
    if (!place)
      return std::nullopt;
    ConstTarget at = constStorageTarget(*place);
    const Ty* objectTy = types.unqualified(types.stripRef(type));
    if (objectTy->kind == TyKind::Array)
    {
      at.path.push_back(0); // C text: its first char
      objectTy = objectTy->inner;
    }
    ConstTarget fn;
    fn.decl = kind.thunk;
    if (kind.type && !kind.thunk->templateParams.empty())
      fn.args.push_back({kind.thunk->templateParams[0], kind.type});
    const Ty* fnTy = declaredTypeOf(kind.thunk, nullptr, depth + 1);
    pieces.push_back(ConstValue::object(pieceTy, {ConstValue::address(types.pointer(types.qualified(objectTy, true, false)), std::move(at)),
                                                  ConstValue::address(types.pointer(fnTy), std::move(fn))}));
  }
  ConstValue array;
  array.kind = ConstValue::Kind::Pointer;
  array.type = types.pointer(types.qualified(pieceTy, true, false));
  array.i = 0;
  int64_t count = int64_t(pieces.size());
  array.parts = std::make_shared<const std::vector<ConstValue>>(std::move(pieces));
  return ConstValue::object(textTy, {array, ConstValue::integer(count, countTy)});
}

auto GlobalScope::Impl::constTempList() -> std::vector<ConstBinding>*&
{
  static thread_local std::vector<ConstBinding>* list = nullptr;
  return list;
}

auto GlobalScope::Impl::constThrown(ThrowExpr* t, const TypeContext& ctx, int depth) -> std::string
{
  std::string where = " (" + constWhereText(constHere(t)) + ")";
  Expr* operand = t->operand;
  while (operand && operand->kind == NodeKind::ParenExpr)
    operand = static_cast<ParenExpr*>(operand)->inner;
  if (!operand)
    return "it rethrows" + where;
  // The characters a const char* points to, up to the null (nullopt when it is no string the
  // evaluation holds).
  auto text = [&](const ConstValue& p) -> std::optional<std::string> {
    std::string out;
    for (int64_t i = 0; i < 4096; ++i)
    {
      std::optional<ConstValue> at = p.isAddress() ? constAddressMove(p, i, depth + 1) : constPointerArith(P::Plus, p, ConstValue::integer(i, types.intType()));
      std::optional<ConstValue> c = at ? (at->isAddress() ? constReadTarget(*at, depth + 1) : constDeref(*at)) : std::nullopt;
      if (!c || c->kind != ConstValue::Kind::Int)
        return std::nullopt;
      if (c->i == 0)
        return out;
      out.push_back(char(c->i));
    }
    return out + "...";
  };
  if (operand->kind == NodeKind::LiteralExpr && static_cast<LiteralExpr*>(operand)->literalKind == TokenKind::String)
    if (std::optional<ConstValue> chars = constString(static_cast<LiteralExpr*>(operand), ctx); chars && chars->parts)
    {
      std::string out;
      for (const ConstValue& c : *chars->parts)
        if (c.kind == ConstValue::Kind::Int && c.i != 0)
          out.push_back(char(c.i));
      return "it throws \"" + out + "\"" + where;
    }
  std::optional<ConstValue> v = constValue(operand, ctx, depth + 1);
  if (!v)
    return "it throws an exception the evaluator cannot build" + where;
  const Ty* type = v->type ? types.unqualified(types.stripRef(v->type)) : nullptr;
  if (!type || type->kind != TyKind::Class || !v->isObject())
    return "it throws " + (v->kind == ConstValue::Kind::Int ? std::to_string(v->i) : std::string("a value")) + where;
  std::string name = types.toString(type, &symbols);
  std::optional<ConstPlace> place = constMaterialize(*v, type);
  Subst subst;
  for (FunctionDecl* f : constMembersNamed(type, symbols.intern("what"), subst, depth + 1))
    if (f->params.empty())
    {
      std::optional<ConstValue> self = place ? constRead(*place) : v;
      std::optional<ConstValue> p = constCallOn(f, subst, {}, &self, depth + 1, place ? &*place : nullptr);
      if (std::optional<std::string> message = p && (p->isAddress() || p->kind == ConstValue::Kind::Pointer) ? text(*p) : std::nullopt)
        return "it throws " + name + "(\"" + *message + "\")" + where;
      break;
    }
  return "it throws " + name + where;
}

auto GlobalScope::Impl::constMaterialize(const ConstValue& v, const Ty* t) -> std::optional<ConstPlace>
{
  std::vector<ConstBinding>* list = constTempList();
  if (!list)
    return std::nullopt;
  ConstBinding b{0, v};
  b.type = types.unqualified(types.stripRef(t));
  b.owns = true;
  list->push_back(std::move(b));
  return ConstPlace{list, list->size() - 1, {}, list->back().type};
}

auto GlobalScope::Impl::constDestroyRunsCode(const Ty* t) -> bool
{
  if (!t || t->isReference())
    return false;
  std::optional<bool> trivial = triviallyDestructible(types.unqualified(t));
  return !trivial || !*trivial;
}

auto GlobalScope::Impl::constDestroy(const Ty* t, const ConstValue& v, int depth, const ConstPlace* at) -> bool
{
  t = t ? types.unqualified(types.stripRef(t)) : nullptr;
  if (t && t->kind == TyKind::Lambda)
  {
    // A closure: its captures by copy destroyed, the last first (one by reference is an address).
    for (size_t i = v.isObject() && v.parts ? v.parts->size() : 0; i-- > 0;)
      if (const ConstValue& part = (*v.parts)[i]; part.isObject() && part.type && !constDestroy(part.type, part, depth + 1))
        return false;
    return true;
  }
  if (!constDestroyRunsCode(t))
    return true;
  auto cannot = [&](const char* what) {
    if (constWhy().empty())
      constWhy() = std::string(what) + " " + types.toString(t, &symbols);
    return false;
  };
  if (!triviallyDestructible(t) || depth > 64 || !v.isObject() || !v.parts)
    return cannot("a destruction the evaluator cannot run: of");
  // A part's place: the object's, one step in.
  auto partAt = [&](size_t i, const Ty* type) {
    std::optional<ConstPlace> part;
    if (at)
    {
      part = *at;
      part->path.push_back(i);
      part->type = type;
      part->bits = 0;
    }
    return part;
  };
  if (t->kind == TyKind::Array)
  {
    for (size_t i = v.parts->size(); i-- > 0;)
    {
      std::optional<ConstPlace> element = partAt(i, t->inner);
      if (!constDestroy(t->inner, (*v.parts)[i], depth + 1, element ? &*element : nullptr))
        return false;
    }
    return true;
  }
  ClassDecl* def = t->kind == TyKind::Class ? classDefinition(t) : nullptr;
  const ConstClassLayout* layout = def ? constLayoutOf(t, depth) : nullptr;
  if (!def || !layout)
    return cannot("a destruction the evaluator cannot run: of");
  if (def->isTypedUnion)
  {
    // The alternative its tag names, destroyed where it is (design.md 8.5); the others hold nothing.
    size_t tagAt = SIZE_MAX, storageAt = SIZE_MAX;
    for (size_t i = 0; i < layout->fields.size(); ++i)
      if (layout->fields[i] && layout->fields[i] == def->tagField)
        tagAt = i;
      else if (!layout->fields[i])
        storageAt = i;
    size_t nb = layout->bases.size();
    const ConstClassLayout* storage = storageAt != SIZE_MAX ? constLayoutOf(layout->fieldTypes[storageAt], depth + 1) : nullptr;
    if (!storage || tagAt == SIZE_MAX || nb + storageAt >= v.parts->size())
      return cannot("the destruction of a typed union the evaluator cannot run:");
    const ConstValue& tag = (*v.parts)[nb + tagAt];
    const ConstValue& held = (*v.parts)[nb + storageAt];
    if (tag.kind != ConstValue::Kind::Int || tag.i < 0 || uint64_t(tag.i) >= def->alternatives.size() || !held.isObject() || !held.parts)
      return cannot("the destruction of a typed union the evaluator cannot run:");
    for (size_t i = 0; i < storage->fields.size(); ++i)
      if (storage->fields[i] == def->alternatives[size_t(tag.i)] && storage->bases.size() + i < held.parts->size())
      {
        std::optional<ConstPlace> alternative = partAt(nb + storageAt, layout->fieldTypes[storageAt]);
        if (alternative)
        {
          alternative->path.push_back(storage->bases.size() + i);
          alternative->type = storage->fieldTypes[i];
        }
        const ConstValue& part = (*held.parts)[storage->bases.size() + i];
        return part.isUnset() || constDestroy(storage->fieldTypes[i], part, depth + 1, alternative ? &*alternative : nullptr);
      }
    return cannot("the destruction of a typed union the evaluator cannot run:");
  }
  FunctionDecl* dtor = nullptr;
  for (Decl* m : membersRead(def))
    if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Destructor))
      dtor = static_cast<FunctionDecl*>(m);
  ConstValue object = v;
  if (dtor && !(dtor->flags & DF_Default))
  {
    if (dtor->flags & DF_Delete)
      return cannot("the destruction of an object whose destructor is deleted:");
    std::optional<ConstValue> self = v;
    // (its virtual calls are its class's while it runs, on a base part too: constDynamicPlace)
    struct Dying
    {
      ~Dying()
      {
        if (on)
          constDyingPlaces().pop_back();
      }

      bool on;
    } dying{at != nullptr};
    if (at)
      constDyingPlaces().push_back(constResolve(*at));
    if (!constCallOn(dtor, layout->subst, {}, &self, depth + 1, at) || !self || !self->isObject())
      return false;
    if (at)
      self = constRead(*at); // (run where it is: the object as its body left it there)
    if (!self || !self->isObject())
      return false;
    object = *self;
  }
  if (def->keyword == Kw::union_)
    return true; // its destructor's body chose what to destroy
  size_t nb = layout->bases.size(), nf = layout->fields.size();
  if (!object.parts || object.parts->size() != nb + nf)
    return cannot("a destruction the evaluator cannot run: of");
  for (size_t i = nf; i-- > 0;)
  {
    std::optional<ConstPlace> member = partAt(nb + i, layout->fieldTypes[i]);
    if (!constDestroy(layout->fieldTypes[i], (*object.parts)[nb + i], depth + 1, member ? &*member : nullptr))
      return false;
  }
  for (size_t b = nb; b-- > 0;)
  {
    std::optional<ConstPlace> base = partAt(b, layout->bases[b]);
    if (!constDestroy(layout->bases[b], (*object.parts)[b], depth + 1, base ? &*base : nullptr))
      return false;
  }
  return true;
}

auto GlobalScope::Impl::constEndLocals(std::vector<ConstBinding>& env, size_t mark, int depth, bool run) -> bool
{
  bool ok = true;
  for (size_t i = env.size(); run && ok && i-- > mark;)
    if (env[i].owns && !env[i].aliasEnv && !env[i].pack)
    {
      ConstValue v = env[i].value; // (the destructor may write through pointers into env)
      const Ty* t = env[i].type ? env[i].type : v.type;
      ConstPlace at{&env, i, {}, t}; // (run where it is: what it made pointing at itself is its own)
      env[i].writable = true;        // (a const one's destructor changes it too, as C++'s does)
      ok = v.isUnset() || constDestroy(t, v, depth + 1, &at);
    }
  if (mark < env.size())
    env.resize(mark);
  return ok;
}

auto GlobalScope::Impl::constNewObject(NewExpr* n, const Ty* t, const TypeContext& ctx, int depth) -> std::optional<ConstValue>
{
  const Ty* u = types.unqualified(t);
  bool expands = false;
  for (Expr* a : n->args)
    expands = expands || constIsExpansion(a);
  if (expands && !n->braced)
  {
    // `new (slot) T(forward<Args>(args)...)`: what the expansion gives; none value-initializes.
    std::vector<ConstValue> values;
    std::vector<std::optional<ConstPlace>> places;
    std::vector<uint8_t> categories;
    if (!constArguments(n->args, ctx, depth, values, &places, &categories))
      return std::nullopt;
    if (values.empty())
      return constEmpty(u, true, depth + 1);
    if (u->kind == TyKind::Class)
      return values.size() == 1 && values[0].isObject() && values[0].type && types.unqualified(types.stripRef(values[0].type)) == u
               ? constCopyConstruct(u, values[0], places[0], categories[0] != 1, depth + 1) // copied, or moved from where it is
               : constConstruct(u, values, false, depth + 1, nullptr, nullptr, &places, &categories);
    return values.size() == 1 ? constConvertTo(values[0], u, depth + 1) : std::nullopt;
  }
  if (n->braced && n->args.size() == 1)
    return constInitialize(u, n->args[0], ctx, depth + 1);
  if (n->args.empty())
    return constEmpty(u, n->parenthesized, depth + 1);
  if (n->args.size() == 1)
  {
    if (u->kind != TyKind::Class)
      return constConvertTo(constValue(n->args[0], ctx, depth + 1), u, depth + 1);
    std::optional<ConstValue> evaluated;
    if (std::optional<std::optional<ConstValue>> copied = constCopyInit(n->args[0], u, ctx, depth, false, &evaluated))
      return *copied;
    std::optional<ConstValue> v = evaluated ? evaluated : constValue(n->args[0], ctx, depth + 1); // (evaluated once: `new C(*it++)`)
    if (!v)
      return std::nullopt;
    if (v->isObject() && v->type && types.unqualified(types.stripRef(v->type)) == u)
      return constConvertTo(v, u, depth + 1);
    return constConstruct(u, {*v}, false, depth + 1);
  }
  std::vector<ConstValue> values;
  std::vector<std::optional<ConstPlace>> places;
  std::vector<uint8_t> categories;
  if (!constArguments(n->args, ctx, depth, values, &places, &categories))
    return std::nullopt;
  return constConstruct(u, values, false, depth + 1, values.size() == n->args.size() ? &n->args : nullptr, nullptr, &places, &categories);
}

auto GlobalScope::Impl::constNew(NewExpr* n, const TypeContext& ctx, int depth) -> std::optional<ConstValue>
{
  if (!n->type || n->placement.size() > 1)
    return std::nullopt;
  Type* written = n->type;
  Expr* count = nullptr;
  if (written->kind == NodeKind::ArrayType && n->placement.empty())
  {
    count = static_cast<ArrayType*>(written)->size;
    written = static_cast<ArrayType*>(written)->element;
    if (!count)
      return std::nullopt;
  }
  const Ty* t = convertType(written, ctx, depth + 1);
  if (!t || !t->isKnown() || types.isDependent(t) || t->isReference())
    return std::nullopt;
  t = types.unqualified(t);
  if (n->placement.size() == 1)
  {
    std::optional<ConstValue> where = constValue(n->placement[0], ctx, depth + 1);
    if (!where || !where->isAddress() || !where->target->storage)
      return std::nullopt; // (placement new into what the evaluation does not hold)
    ConstValue typed = *where;
    typed.type = types.pointer(t);
    if (std::optional<ConstValue> fresh = constTypeAllocation(*where, t, depth))
      typed = *fresh; // `new (malloc(sizeof(T))) T(...)`
    std::optional<ConstPlace> place = constStoragePlace(typed, "a placement new into", depth);
    std::optional<ConstValue> object = place ? constNewObject(n, t, ctx, depth) : std::nullopt;
    if (!object || !constWrite(*place, *object))
      return std::nullopt;
    return typed;
  }
  std::optional<ConstValue> value;
  if (count)
  {
    std::optional<ConstValue> size = constValue(count, ctx, depth + 1);
    if (!size || size->kind != ConstValue::Kind::Int || size->i < 0 || !constMayMake(size_t(size->i)))
      return std::nullopt;
    const Ty* array = types.array(t, uint64_t(size->i));
    if (n->braced && n->args.size() == 1 && n->args[0]->kind == NodeKind::BraceInitExpr)
      value = constArrayInit(array, static_cast<BraceInitExpr*>(n->args[0])->args, ctx, depth + 1);
    else if (n->args.empty())
    {
      std::vector<ConstValue> elements;
      for (int64_t i = 0; i < size->i; ++i)
      {
        std::optional<ConstValue> e = constEmpty(t, n->parenthesized, depth + 1);
        if (!e)
          return std::nullopt;
        elements.push_back(*e);
      }
      value = ConstValue::object(array, std::move(elements));
    }
    if (!value)
      return std::nullopt;
  }
  else if (!(value = constNewObject(n, t, ctx, depth)))
    return std::nullopt;
  std::vector<ConstBinding>& heap = constHeap();
  ConstBinding b{0, *value};
  b.type = value->type ? value->type : t;
  heap.push_back(std::move(b));
  constAllocations().push_back(ConstAllocation{constHere(n), {}, count != nullptr, false});
  ConstTarget target = constStorageTarget(ConstPlace{&heap, heap.size() - 1, {}, heap.back().type});
  if (count)
    target.path.push_back(0);
  return ConstValue::address(types.pointer(t), std::move(target));
}

auto GlobalScope::Impl::constDelete(DeleteExpr* d, const TypeContext& ctx, int depth) -> std::optional<ConstValue>
{
  std::optional<ConstValue> p = constValue(d->operand, ctx, depth + 1);
  if (!p)
    return std::nullopt;
  ConstValue done = ConstValue::integer(0, types.voidType());
  if (p->kind == ConstValue::Kind::Int && p->i == 0)
    return done;
  auto fail = [&](const std::string& why) -> std::optional<ConstValue> {
    if (constWhy().empty())
      constWhy() = why;
    return std::nullopt;
  };
  auto it = p->isAddress() && p->target->storage ? constStorage().find(p->target->storage) : constStorage().end();
  if (it == constStorage().end() || it->second.env != &constHeap())
    return fail(std::string(d->isArray ? "delete[]" : "delete") + " of memory new did not allocate");
  size_t index = it->second.index;
  ConstAllocation& a = constAllocations()[index];
  if (a.raw)
    return fail(std::string(d->isArray ? "delete[]" : "delete") + " of memory malloc allocated at " + constWhereText(a.allocated));
  if (a.isFreed)
    return fail("memory freed twice (first at " + constWhereText(a.freed) + ")");
  const Ty* pointee = p->type && types.unqualified(types.stripRef(p->type))->kind == TyKind::Pointer ? types.unqualified(types.unqualified(types.stripRef(p->type))->inner) : nullptr;
  // `delete base` of a derived object new made: through its base part, whose destructor is virtual
  // ([expr.delete] 3), the whole object destroyed (its destructor first) and its block freed.
  auto virtualDestructor = [&](const Ty* t) {
    std::function<bool(const Ty*, int)> declares = [&](const Ty* c, int level) -> bool {
      ClassDecl* def = c && level <= 16 ? classDefinition(c) : nullptr;
      const ConstClassLayout* layout = def ? constLayoutOf(c, 0) : nullptr;
      if (!layout)
        return false;
      for (Decl* m : membersRead(def))
        if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Destructor) && (m->flags & (DF_Virtual | DF_Override)))
          return true;
      for (const Ty* b : layout->bases)
        if (declares(b, level + 1))
          return true;
      return false;
    };
    return t && t->kind == TyKind::Class && declares(t, 0);
  };
  bool throughBase = !a.array && !d->isArray && !p->target->path.empty() && virtualDestructor(pointee) &&
                     constDynamicPlace(ConstPlace{&constHeap(), index, p->target->path, pointee}, depth).path.empty();
  if (a.array != d->isArray || (!throughBase && p->target->path != (a.array ? std::vector<size_t>{0} : std::vector<size_t>{})))
    return fail(std::string(d->isArray ? "delete[]" : "delete") + " of memory " + (a.array ? "new[]" : "new") + " allocated at " +
                constWhereText(a.allocated) + (p->target->path.size() > (a.array ? 1u : 0u) ? ", through a pointer into it" : ""));
  const Ty* object = constHeap()[index].type;
  if (!pointee || (!throughBase && types.unqualified(a.array ? object->inner : object) != pointee))
    return fail("a delete through a pointer of another type than the object's is not supported in a constant evaluation yet");
  ConstValue value = constHeap()[index].value;
  ConstPlace held{&constHeap(), index, {}, object};
  if (!value.isUnset() && !constDestroy(object, value, depth + 1, &held))
    return std::nullopt;
  ConstAllocation& freed = constAllocations()[index]; // (the destructor may have allocated)
  freed.isFreed = true;
  freed.freed = constHere(d);
  constHeap()[index].value = ConstValue::unset(object);
  return done;
}

auto GlobalScope::Impl::runBuild(std::string_view configuration, std::string& error) -> std::optional<std::string>
{
  auto named = [&](const char* name, NodeKind kind) -> Decl* {
    Lookup l;
    if (!lookupInScope(&root, symbols.intern(name), l, 0))
      return nullptr;
    if (!l.overloads)
      return l.decl && l.decl->kind == kind ? l.decl : nullptr;
    Decl* only = nullptr;
    for (const Entry& entry : *l.overloads)
      if (entry.decl && entry.decl->kind == kind && (kind != NodeKind::FunctionDecl || static_cast<FunctionDecl*>(entry.decl)->body))
      {
        if (only)
          return nullptr; // several: which is not decided
        only = entry.decl;
      }
    return only;
  };
  auto* project = static_cast<ClassDecl*>(named("Project", NodeKind::ClassDecl));
  auto* configurations = static_cast<EnumDecl*>(named("Configuration", NodeKind::EnumDecl));
  auto* build = static_cast<FunctionDecl*>(named("build", NodeKind::FunctionDecl));
  auto* describe = static_cast<FunctionDecl*>(named("describe", NodeKind::FunctionDecl));
  if (!project || !build || !describe || build->params.size() != 1 || describe->params.size() != 1)
  {
    error = "a build description needs one class Project, one build(Project&) and one describe(const Project&)";
    return std::nullopt;
  }
  const Ty* projectTy = selfType(project);
  const ConstClassLayout* layout = constLayoutOf(projectTy, 0);
  size_t field = SIZE_MAX;
  for (size_t i = 0; layout && i < layout->fields.size(); ++i)
    if (layout->fields[i] && !layout->fields[i]->name.empty() && symbols.name(layout->fields[i]->name.last().symbol) == "configuration")
      field = i;
  Decl* chosen = nullptr;
  if (field != SIZE_MAX)
  {
    if (!configurations)
    {
      error = "Project has a member configuration and there is no one enum Configuration";
      return std::nullopt;
    }
    for (Decl* e : configurations->enumerators)
      if (e->kind == NodeKind::EnumeratorDecl && symbols.name(static_cast<EnumeratorDecl*>(e)->name) == configuration)
        chosen = e;
    if (!chosen)
    {
      error = "no configuration named " + std::string(configuration);
      return std::nullopt;
    }
  }
  auto contextOf = [&](FunctionDecl* f) {
    return TypeContext{lexicalScopeOf(f), nullptr, nullptr, f->file ? f->file->tokens : nullptr, f->file ? f->file->text : std::string_view()};
  };
  std::string text;
  constWhy().clear();
  std::optional<ConstValue> done = constOutermost([&]() -> std::optional<ConstValue> {
    std::vector<ConstBinding> env;
    ConstFrame frame(&env);
    std::optional<ConstValue> object = constConstruct(projectTy, {}, false, 0);
    TypeContext none = contextOf(build);
    std::optional<ConstValue> tag = object && chosen ? constValueOfDecl(chosen, none, 0) : std::nullopt;
    if (!object || (chosen && !tag))
      return std::nullopt;
    env.push_back(constBinding(symbols.intern("project"), *object, projectTy));
    env.back().owns = true;
    ConstPlace whole{&env, 0, {}, projectTy};
    if (chosen && !constWrite(ConstPlace{&env, 0, {layout->bases.size() + field}, layout->fieldTypes[field]}, *tag))
      return std::nullopt;
    std::vector<std::optional<ConstPlace>> places{whole};
    std::vector<uint8_t> lvalue{1};
    if (!constRun(build, {*constRead(whole)}, contextOf(build), 0, nullptr, &places, &lvalue))
      return std::nullopt;
    std::optional<ConstValue> described = constRun(describe, {*constRead(whole)}, contextOf(describe), 0, nullptr, &places, &lvalue);
    if (!described || !described->isObject() || !described->type)
      return std::nullopt;
    env.push_back(constBinding(0, *described, described->type));
    env.back().owns = true; // destroyed with the project, after its characters are read
    ConstPlace string{&env, 1, {}, described->type};
    auto member = [&](const char* name) -> std::optional<ConstValue> {
      Subst subst;
      for (FunctionDecl* f : constMembersNamed(described->type, symbols.intern(name), subst, 0))
        if (f->params.empty())
        {
          std::optional<ConstValue> self = constRead(string);
          return constCallOn(f, subst, {}, &self, 0, &string);
        }
      return std::nullopt;
    };
    std::optional<ConstValue> chars = member("data"), size = member("size");
    if (!chars || !size || size->kind != ConstValue::Kind::Int)
      return std::nullopt;
    for (int64_t i = 0; i < size->i; ++i)
    {
      std::optional<ConstValue> at = chars->isAddress() ? constAddressMove(*chars, i, 0)
                                                        : constPointerArith(P::Plus, *chars, ConstValue::integer(i, types.intType()));
      std::optional<ConstValue> c = at ? (at->isAddress() ? constReadTarget(*at, 1) : constDeref(*at)) : std::nullopt;
      if (!c || c->kind != ConstValue::Kind::Int)
        return std::nullopt;
      text.push_back(char(c->i));
    }
    if (!constEndLocals(env, 0, 0))
      return std::nullopt;
    return ConstValue::integer(1, types.intType());
  }, 0);
  if (done)
    return text;
  std::string why = constWhy().empty() ? "the build description is no constant" : constWhy();
  error = constFailedAt().file ? constWhereText(constFailedAt()) + ": " + why : why;
  return std::nullopt;
}

auto GlobalScope::Impl::constLocal(Expr* e, const TypeContext& ctx) -> std::optional<ConstPlace>
{
  while (e && e->kind == NodeKind::ParenExpr)
    e = static_cast<ParenExpr*>(e)->inner;
  if (!e || e->kind != NodeKind::NameExpr || !ctx.env)
    return std::nullopt;
  auto* n = static_cast<NameExpr*>(e);
  if (n->name.empty() && n->resolved && n->resolved->kind == NodeKind::VariableDecl &&
      !static_cast<VariableDecl*>(n->resolved)->structuredBindings.empty())
  {
    // The hidden object of `auto [a, b] = e;`, which a tuple-like binding's get<i>(object) names.
    for (size_t i = ctx.env->size(); i-- > 0;)
      if ((*ctx.env)[i].symbol == constHiddenSymbol && (*ctx.env)[i].hiddenOf == n->resolved)
      {
        ConstPlace r = constResolve(ConstPlace{ctx.env, i, {}, (*ctx.env)[i].type});
        r.type = (*ctx.env)[i].type;
        return r;
      }
    return std::nullopt;
  }
  if (n->name.parts.size() != 1 || n->name.global || n->name.parts[0].hasTemplateArgs)
    return std::nullopt;
  for (size_t i = ctx.env->size(); i-- > 0;)
    if ((*ctx.env)[i].symbol == n->name.parts[0].symbol)
    {
      ConstPlace p{ctx.env, i, {}, (*ctx.env)[i].type};
      ConstPlace r = constResolve(p);
      r.type = (*ctx.env)[i].type;
      return r;
    }
  return std::nullopt;
}

auto GlobalScope::Impl::constOwnLocal(Expr* e, const TypeContext& ctx) -> bool
{
  while (e && e->kind == NodeKind::ParenExpr)
    e = static_cast<ParenExpr*>(e)->inner;
  if (!e || e->kind != NodeKind::NameExpr || !ctx.env)
    return false;
  auto* n = static_cast<NameExpr*>(e);
  if (n->name.parts.size() != 1 || n->name.global || n->name.parts[0].hasTemplateArgs)
    return false;
  for (size_t i = ctx.env->size(); i-- > 0;)
    if ((*ctx.env)[i].symbol == n->name.parts[0].symbol)
    {
      const ConstBinding& b = (*ctx.env)[i];
      return !b.aliasEnv && !b.pack && b.type && !b.type->isReference();
    }
  return false;
}

auto GlobalScope::Impl::constThisPlace(const TypeContext& ctx) -> std::optional<ConstPlace>
{
  if (!ctx.env)
    return std::nullopt;
  for (size_t i = ctx.env->size(); i-- > 0;)
    if ((*ctx.env)[i].symbol == constThisSymbol)
      return constResolve(ConstPlace{ctx.env, i, {}, (*ctx.env)[i].type});
  return std::nullopt;
}

auto GlobalScope::Impl::constFieldNamed(Decl* resolved, const QualifiedName& name, const Ty* objectType) -> Decl*
{
  if (resolved)
    return resolved;
  if (name.parts.size() == 2 && !name.global && !name.parts[1].hasTemplateArgs)
  {
    // `b.A::v`: the data member of the base the qualifier names (one the derived class hides).
    std::function<Decl*(const Ty*, int)> declaredIn = [&](const Ty* t, int depth) -> Decl* {
      const ConstClassLayout* layout = t && depth <= 16 ? constLayoutOf(t, depth) : nullptr;
      if (!layout)
        return nullptr;
      if (layout->decl && !layout->decl->name.empty() && layout->decl->name.last().symbol == name.parts[0].symbol)
      {
        for (VariableDecl* f : layout->fields)
          if (f && !f->name.empty() && f->name.last().symbol == name.parts[1].symbol)
            return f;
        return nullptr;
      }
      for (const Ty* b : layout->bases)
        if (Decl* d = declaredIn(b, depth + 1))
          return d;
      return nullptr;
    };
    return declaredIn(objectType, 0);
  }
  if (name.parts.size() != 1 || name.global || name.parts[0].hasTemplateArgs)
    return nullptr;
  Scope* s = objectType ? classScopeOf(objectType) : nullptr;
  Lookup l;
  if (!s || !lookupInScope(s, name.parts[0].symbol, l, 1))
    return nullptr;
  return l.decl && l.decl->kind == NodeKind::VariableDecl && !(l.decl->flags & (DF_Static | DF_Typedef)) ? l.decl : nullptr;
}

auto GlobalScope::Impl::constPointsIntoStorage(Expr* e) -> bool
{
  while (e && e->kind == NodeKind::ParenExpr)
    e = static_cast<ParenExpr*>(e)->inner;
  // (One no walk typed, in a template's own body, may be a pointer too: its value says.)
  const Ty* t = e && e->ty ? types.unqualified(types.stripRef(e->ty)) : nullptr;
  return e && (!t || t->kind == TyKind::Pointer) && !constFrames().empty() && constPure(e);
}

auto GlobalScope::Impl::constPure(Expr* e, int depth) -> bool
{
  if (!e || depth > 32)
    return !e;
  switch (e->kind)
  {
    case NodeKind::NameExpr: case NodeKind::LiteralExpr: case NodeKind::ThisExpr: return true;
    case NodeKind::ParenExpr: return constPure(static_cast<ParenExpr*>(e)->inner, depth + 1);
    case NodeKind::MemberExpr: return constPure(static_cast<MemberExpr*>(e)->base, depth + 1);
    case NodeKind::IndexExpr:
    {
      auto* x = static_cast<IndexExpr*>(e);
      for (Expr* a : x->args)
        if (!constPure(a, depth + 1))
          return false;
      return constPure(x->base, depth + 1);
    }
    case NodeKind::UnaryExpr:
    {
      auto* u = static_cast<UnaryExpr*>(e);
      std::optional<P> op = constOperator(u->op, u->isKeyword);
      return op && *op != P::PlusPlus && *op != P::MinusMinus && constPure(u->operand, depth + 1);
    }
    case NodeKind::BinaryExpr:
    {
      auto* b = static_cast<BinaryExpr*>(e);
      return !b->isKeyword && !isAssignmentOperator(P(b->op)) && constPure(b->lhs, depth + 1) && constPure(b->rhs, depth + 1);
    }
    case NodeKind::CastExpr: return constPure(static_cast<CastExpr*>(e)->operand, depth + 1);
    case NodeKind::NamedCastExpr: return constPure(static_cast<NamedCastExpr*>(e)->operand, depth + 1);
    default: return false;
  }
}

auto GlobalScope::Impl::constArrowOf(Expr* e, const ConstValue& v, const TypeContext& ctx, int depth) -> std::optional<ConstValue>
{
  if (!v.isObject() || !v.type || v.type->isBraceList() || types.unqualified(types.stripRef(v.type))->kind != TyKind::Class)
    return std::nullopt;
  Subst subst;
  FunctionDecl* arrow = constClassOperator(v.type, P::Arrow, {}, subst, depth);
  if (!arrow || !constDefinition(arrow))
    return std::nullopt;
  std::optional<ConstPlace> place = e && constPure(e) ? constPlaceOf(e, ctx, depth + 1) : std::nullopt;
  if (!place)
    place = constMaterialize(v, v.type);
  std::optional<ConstValue> self = place ? constRead(*place) : std::optional<ConstValue>(v);
  return constCallOn(arrow, subst, {}, &self, depth + 1, place ? &*place : nullptr);
}

auto GlobalScope::Impl::constArrowPointer(Expr* e, std::optional<ConstValue> v, const TypeContext& ctx, int depth) -> std::optional<ConstValue>
{
  for (int hops = 0; v && v->isObject() && hops < 16; ++hops)
    v = constArrowOf(hops ? nullptr : e, *v, ctx, depth); // (a class an operator-> gives: its own operator-> run on it in turn)
  if (!v || !(v->isAddress() || v->isPointer()) || !v->type || types.unqualified(v->type)->kind != TyKind::Pointer)
    return std::nullopt;
  return v;
}

auto GlobalScope::Impl::constCallObject(Expr* object, CallExpr* c, const TypeContext& ctx, int depth, Decl* field) -> std::optional<ConstValue>
{
  std::optional<ConstPlace> place;
  if (field && object->kind == NodeKind::MemberExpr)
  {
    auto* m = static_cast<MemberExpr*>(object);
    std::optional<ConstPlace> base = m->base->kind == NodeKind::ThisExpr && m->op == P::Arrow ? constThisPlace(ctx)
                                   : m->op == P::Dot && constPure(m->base)                    ? constPlaceOf(m->base, ctx, depth + 1)
                                                                                              : std::nullopt;
    std::optional<ConstValue> owner = base ? constRead(*base) : std::nullopt;
    std::optional<std::vector<size_t>> path = owner && owner->isObject() ? constFieldPath(owner->type, field, 0) : std::nullopt;
    if (path)
    {
      const ConstClassLayout* layout = constLayoutOf(owner->type, 0);
      base->path.insert(base->path.end(), path->begin(), path->end());
      base->type = declaredTypeOf(field, layout && !layout->subst.empty() ? &layout->subst : nullptr, 0);
      place = base;
    }
  }
  if (!place && constPure(object))
    place = constPlaceOf(object, ctx, depth + 1);
  std::optional<ConstValue> self = place ? constRead(*place) : constValue(object, ctx, depth + 1);
  return self ? constCallObjectValue(*self, place, c, ctx, depth) : std::nullopt;
}

auto GlobalScope::Impl::constCallObjectValue(const ConstValue& object, std::optional<ConstPlace> place, CallExpr* c, const TypeContext& ctx, int depth)
  -> std::optional<ConstValue>
{
  std::optional<ConstValue> self = object;
  if (self->isObject() && self->type)
    if (LambdaExpr* l = lambdaOf(types.unqualified(types.stripRef(self->type))))
      return constCallClosure(l, *self, c, ctx, depth, place ? &*place : nullptr); // `twice(5)`: a closure's body, its captures bound
  if (!self->isObject() || !self->type || self->type->isBraceList() || types.unqualified(types.stripRef(self->type))->kind != TyKind::Class)
    return std::nullopt;
  if (!place)
    place = constMaterialize(*self, self->type);
  std::vector<ConstValue> values;
  std::vector<std::optional<ConstPlace>> places;
  std::vector<uint8_t> categories;
  if (!constArguments(c->args, ctx, depth, values, &places, &categories))
    return std::nullopt;
  std::vector<const Ty*> argTypes;
  for (size_t i = 0; i < values.size(); ++i)
  {
    ConstOperand o{i < places.size() ? places[i] : std::nullopt, i < categories.size() ? categories[i] : uint8_t(0)};
    argTypes.push_back(constOperandType(values[i], &o));
  }
  Subst subst;
  FunctionDecl* call = constClassOperator(self->type, P::LParen, argTypes, subst, depth, true);
  if (!call || !constDefinition(call))
    return std::nullopt;
  if (!call->templateParams.empty())
  {
    // `less(a, b)` of a Less<>, sort's default comparator: an operator() template, its own
    // parameters deduced from the arguments (it was run unbound, so sort was no constant).
    LocalStack own;
    if (!constBindTemplate(call, {}, c, values, ctx, depth, subst, own, &categories))
      return std::nullopt;
  }
  return constCallOn(call, subst, values, &self, depth + 1, place ? &*place : nullptr, &places, &categories);
}

auto GlobalScope::Impl::constPlaceOf(Expr* e, const TypeContext& ctx, int depth, bool calls, std::optional<ConstValue>* ran) -> std::optional<ConstPlace>
{
  while (e && e->kind == NodeKind::ParenExpr)
    e = static_cast<ParenExpr*>(e)->inner;
  if (!e || depth > 64)
    return std::nullopt;
  if (e->kind == NodeKind::NameExpr)
  {
    if (std::optional<ConstPlace> local = constLocal(e, ctx))
      return local;
    Decl* d = static_cast<NameExpr*>(e)->resolved;
    std::optional<ConstPlace> self = constThisPlace(ctx);
    std::optional<ConstValue> object = self && !d ? constRead(*self) : std::nullopt;
    if (object && object->isObject())
      d = constFieldNamed(nullptr, static_cast<NameExpr*>(e)->name, object->type);
    if (self && d && d->kind == NodeKind::VariableDecl && d->parent && d->parent->kind == NodeKind::ClassDecl && !(d->flags & DF_Static))
    {
      if (!object)
        object = constRead(*self);
      if (!object || !object->isObject())
        return std::nullopt;
      if (std::optional<std::vector<size_t>> path = constFieldPath(object->type, d, 0))
      {
        const ConstClassLayout* layout = constLayoutOf(object->type, 0);
        self->path.insert(self->path.end(), path->begin(), path->end());
        self->type = declaredTypeOf(d, layout && !layout->subst.empty() ? &layout->subst : nullptr, 0);
        self->bits = constBitsOf(object->type, d);
        return constThroughReference(*self, depth);
      }
    }
    return std::nullopt;
  }
  if (e->kind == NodeKind::ConditionalExpr)
  {
    // `c ? a : b` of two lvalues (an lvalue itself): the place of the one the condition picks, which
    // a reference bound to it refers to (`const int& r = c ? a : b;` followed a's later changes at run
    // time and was a copy here: language/consteval_reference_to_conditional_lvalue). A condition
    // with no side effect only, evaluated again by a caller that finds no place.
    auto* c = static_cast<ConditionalExpr*>(e);
    if (!c->then || !c->else_ || !c->then->ty || !c->else_->ty || !constPure(c->cond))
      return std::nullopt;
    Expr* source = nullptr;
    if (constCategory(c->then, source) != 1 || constCategory(c->else_, source) != 1 ||
        types.unqualified(types.stripRef(c->then->ty)) != types.unqualified(types.stripRef(c->else_->ty)))
      return std::nullopt; // (a prvalue, of the two types' common one)
    std::optional<ConstValue> cond = constValue(c->cond, ctx, depth + 1);
    std::optional<bool> holds = cond ? constTruth(*cond, depth + 1) : std::nullopt;
    return holds ? constPlaceOf(*holds ? c->then : c->else_, ctx, depth + 1, calls, ran) : std::nullopt;
  }
  if (e->kind == NodeKind::UnaryExpr && P(static_cast<UnaryExpr*>(e)->op) == P::Star && !static_cast<UnaryExpr*>(e)->isKeyword &&
      static_cast<UnaryExpr*>(e)->operand && static_cast<UnaryExpr*>(e)->operand->kind == NodeKind::ThisExpr)
    return constThisPlace(ctx); // `*this`
  if (e->kind == NodeKind::UnaryExpr && P(static_cast<UnaryExpr*>(e)->op) == P::Star && !static_cast<UnaryExpr*>(e)->isKeyword &&
      !static_cast<UnaryExpr*>(e)->postfix && static_cast<UnaryExpr*>(e)->operand &&
      (constPointsIntoStorage(static_cast<UnaryExpr*>(e)->operand) || (calls && !constFrames().empty())))
  {
    // (`*this->slotAt(i)` where a place is needed (a reference result): the call evaluated once.)
    // `*p` with p pointing at an object of the evaluation; at a constant's, its value (ran). `*it`,
    // `*optional`: what the class's operator* refers to, run on the operand where it is, a prvalue
    // as a temporary of the full expression (`*list.begin()`: it was a copy of the element).
    Expr* operand = static_cast<UnaryExpr*>(e)->operand;
    bool pure = constPure(operand);
    std::optional<ConstPlace> object = pure && calls ? constPlaceOf(operand, ctx, depth + 1, calls) : std::nullopt;
    std::optional<ConstValue> p = object ? constRead(*object) : constValue(operand, ctx, depth + 1);
    if (!pure)
      constCallPlaceRan = true; // (evaluated here: never a second time)
    if (p && p->isAddress() && p->target->storage)
      return constStoragePlace(*p, "a use of", depth);
    if (p && p->isAddress() && p->target->decl && p->target->decl->kind == NodeKind::VariableDecl)
    {
      if (ran)
        *ran = constReadTarget(*p, depth + 1); // what a variable of static storage duration holds
      return std::nullopt;
    }
    if (calls && p && p->isObject() && p->type && !p->type->isBraceList() && types.unqualified(types.stripRef(p->type))->kind == TyKind::Class)
    {
      if (!object)
        object = constMaterialize(*p, p->type);
      Subst subst;
      FunctionDecl* f = object ? constClassOperator(p->type, P::Star, {}, subst, depth) : nullptr;
      if (!f || !f->templateParams.empty() || !constDefinition(f))
        return std::nullopt;
      if (constReturnsLRef(f, subst))
        return constCallPlace(f, subst, {}, *object, depth + 1, nullptr, nullptr, ran);
      if (ran)
      {
        // (a proxy's operator* giving a value: a temporary, its value)
        std::optional<ConstValue> self = p;
        *ran = constCallOn(f, subst, {}, &self, depth + 1, &*object);
        constCallPlaceRan = true;
      }
    }
    return std::nullopt;
  }
  if (e->kind == NodeKind::MemberExpr)
  {
    auto* m = static_cast<MemberExpr*>(e);
    Decl* d = m->resolved;
    if ((d && (d->kind != NodeKind::VariableDecl || (d->flags & DF_Static))) || m->dynamicMember)
      return std::nullopt;
    std::optional<ConstPlace> base;
    if (m->base && m->base->kind == NodeKind::ThisExpr && m->op == P::Arrow)
      base = constThisPlace(ctx);
    else if (m->op == P::Dot)
      base = constPlaceOf(m->base, ctx, depth + 1, calls);
    else if (m->base && !constFrames().empty() && (constPure(m->base) || calls))
      if (std::optional<ConstValue> p = constValue(m->base, ctx, depth + 1))
      {
        if (p->isAddress() && p->target->storage)
          base = constStoragePlace(*p, "a use of", depth); // `p->m`, p pointing at an object of the evaluation
        else if (std::optional<ConstValue> q = constArrowPointer(m->base, p, ctx, depth); q && q->isAddress() && q->target->storage)
          base = constStoragePlace(*q, "a use of", depth); // `at->second`: through the iterator's operator-> (a class one gives: its own in turn)
      }
    if (!base)
      return std::nullopt;
    std::optional<ConstValue> object = constRead(*base);
    if (!object || !object->isObject())
      return std::nullopt;
    d = constFieldNamed(d, m->member, object->type);
    std::optional<std::vector<size_t>> path = d ? constFieldPath(object->type, d, 0) : std::nullopt;
    if (!path)
      return std::nullopt;
    base->path.insert(base->path.end(), path->begin(), path->end());
    const ConstClassLayout* layout = constLayoutOf(object->type, 0);
    base->type = declaredTypeOf(d, layout && !layout->subst.empty() ? &layout->subst : nullptr, 0);
    base->bits = constBitsOf(object->type, d);
    return constThroughReference(*base, depth);
  }
  if (e->kind == NodeKind::IndexExpr)
  {
    auto* x = static_cast<IndexExpr*>(e);
    if (x->args.size() != 1)
      return std::nullopt;
    const Ty* bt = x->base && x->base->ty ? types.unqualified(types.stripRef(x->base->ty)) : nullptr;
    bool pointer = bt && bt->kind == TyKind::Pointer && !types.isDependent(bt);
    if (constPointsIntoStorage(x->base) || (calls && pointer && !constFrames().empty()))
    {
      // `p[i]`, `this->data()[index]` (String's operator[]): p pointing into an array of the
      // evaluation, evaluated once; into a constant's, the element's value (ran).
      bool pure = constPure(x->base) && constPure(x->args[0]);
      std::optional<ConstValue> p = constValue(x->base, ctx, depth + 1);
      if (!pure)
        constCallPlaceRan = true; // (evaluated here: never a second time)
      if (p && p->isAddress() && (p->target->storage || (p->target->decl && p->target->decl->kind == NodeKind::VariableDecl)))
      {
        std::optional<ConstValue> index = constValue(x->args[0], ctx, depth + 1);
        if (!index || index->kind != ConstValue::Kind::Int)
          return std::nullopt;
        std::optional<ConstValue> moved = constAddressMove(*p, index->i, depth);
        if (moved && !moved->target->storage)
        {
          if (ran && constAddressDereferenceable(*moved, depth))
            *ran = constReadTarget(*moved, depth + 1);
          return std::nullopt;
        }
        return moved ? constStoragePlace(*moved, "a use of", depth) : std::nullopt;
      }
      if (!pure)
        return std::nullopt;
    }
    std::optional<ConstPlace> base = constPlaceOf(x->base, ctx, depth + 1, calls);
    std::optional<ConstValue> array = base ? constRead(*base) : std::nullopt;
    const Ty* at = array && array->type ? types.unqualified(array->type) : nullptr;
    bool classObject = array && array->isObject() && at && at->kind == TyKind::Class && !at->isBraceList();
    if (const ConstClassLayout* layout = classObject ? constLayoutOf(at, depth + 1) : nullptr; layout && layout->decl && layout->decl->isTypedUnion)
    {
      // `v[V::Number]` of a typed union (design.md 8.5): the alternative the enumerator names, which
      // the union must hold (it was no constant at compile time).
      ClassDecl* def = layout->decl;
      size_t tagAt = SIZE_MAX, storageAt = SIZE_MAX;
      for (size_t i = 0; i < layout->fields.size(); ++i)
        if (layout->fields[i] && layout->fields[i] == def->tagField)
          tagAt = i;
        else if (!layout->fields[i])
          storageAt = i;
      const ConstClassLayout* storage = storageAt != SIZE_MAX ? constLayoutOf(layout->fieldTypes[storageAt], depth + 1) : nullptr;
      std::optional<ConstValue> which = storage && tagAt != SIZE_MAX ? constValue(x->args[0], ctx, depth + 1) : std::nullopt;
      size_t nb = layout->bases.size();
      if (!which || which->kind != ConstValue::Kind::Int || which->i < 0 || uint64_t(which->i) >= def->alternatives.size() ||
          nb + tagAt >= array->parts->size())
        return std::nullopt;
      const ConstValue& tag = (*array->parts)[nb + tagAt];
      if (tag.kind != ConstValue::Kind::Int || tag.i != which->i)
      {
        if (constWhy().empty())
          constWhy() = "the typed union holds another alternative";
        return std::nullopt;
      }
      for (size_t i = 0; i < storage->fields.size(); ++i)
        if (storage->fields[i] == def->alternatives[size_t(which->i)])
        {
          base->path.push_back(nb + storageAt);
          base->path.push_back(storage->bases.size() + i);
          base->type = storage->fieldTypes[i];
          base->bits = 0;
          return base;
        }
      return std::nullopt;
    }
    if (!base || (classObject && !calls))
      return std::nullopt; // (no call here: its index is not evaluated either, a String key never made and lost)
    std::optional<ConstValue> index = constValue(x->args[0], ctx, depth + 1);
    if (!index)
      return std::nullopt;
    if (classObject)
    {
      // `result[i]` of an Array, `names[String("b")]` of a Map (a key of any type: it was an
      // integer's only, so a Map keyed by String was no constant): the element its operator[]
      // refers to.
      Subst subst;
      std::vector<const Ty*> indexType{x->args[0]->ty ? argumentType(x->args[0], x->args[0]->ty, types) : nullptr};
      FunctionDecl* f = constPlaceMember(at, opSymbols[uint32_t(P::LBracket)], 1, base->writable(), subst, depth + 1, nullptr, &indexType);
      // (The key where it is, a prvalue's as a temporary of the full expression, destroyed at its end:
      // `String("b")`'s heap block was never freed.)
      Expr* source = nullptr;
      std::vector<uint8_t> categories{constCategory(x->args[0], source)};
      std::vector<std::optional<ConstPlace>> places{categories[0] != 0 && constPure(x->args[0]) ? constPlaceOf(x->args[0], ctx, depth + 1) : std::nullopt};
      return f && f->templateParams.empty() ? constCallPlace(f, subst, {*index}, *base, depth + 1, &places, &categories, ran) : std::nullopt;
    }
    if (index->kind != ConstValue::Kind::Int || !array || !array->isObject() || !at || at->kind != TyKind::Array || index->i < 0 ||
        uint64_t(index->i) >= array->parts->size())
      return std::nullopt;
    base->path.push_back(size_t(index->i));
    base->type = at->inner;
    base->bits = 0;
    return base;
  }
  if (calls && e->kind == NodeKind::BinaryExpr && !static_cast<BinaryExpr*>(e)->isKeyword &&
      (isAssignmentOperator(P(static_cast<BinaryExpr*>(e)->op)) || P(static_cast<BinaryExpr*>(e)->op) == P::Comma))
  {
    // `(a = 3)`, `a += 1`: the left operand's object, once assigned; `(x, a)`: the right operand's,
    // x evaluated first. A reference bound to one was a copy of the value.
    auto* b = static_cast<BinaryExpr*>(e);
    Expr* source = nullptr;
    if (P(b->op) == P::Comma && (b->call == OperatorCall::Function || constCategory(b->rhs, source) == 0))
      return std::nullopt; // (a class's operator, or a prvalue: no place)
    if (b->lhs && b->lhs->ty && types.unqualified(types.stripRef(b->lhs->ty))->kind == TyKind::Enum && P(b->op) != P::Assign && P(b->op) != P::Comma)
      return std::nullopt; // (`flags |= other`: an operator the checker may have chosen, evaluated as a value)
    if (P(b->op) == P::Comma)
    {
      std::optional<ConstPlace> at = constValue(b->lhs, ctx, depth + 1) ? constPlaceOf(b->rhs, ctx, depth + 1, calls, ran) : std::nullopt;
      constCallPlaceRan = true; // (evaluated here: never a second time)
      return at;
    }
    std::optional<ConstPlace> where;
    std::optional<ConstValue> v = constAssign(b, ctx, depth + 1, &where);
    if (v && !where && ran)
      *ran = v;
    constCallPlaceRan = true;
    return v ? where : std::nullopt;
  }
  if (calls && e->kind == NodeKind::UnaryExpr && !static_cast<UnaryExpr*>(e)->isKeyword && !static_cast<UnaryExpr*>(e)->postfix &&
      (P(static_cast<UnaryExpr*>(e)->op) == P::PlusPlus || P(static_cast<UnaryExpr*>(e)->op) == P::MinusMinus))
  {
    // `++a`: its operand's object, once stepped (a copy of the value before).
    auto* u = static_cast<UnaryExpr*>(e);
    if (u->operand && u->operand->ty && types.unqualified(types.stripRef(u->operand->ty))->kind == TyKind::Enum)
      return std::nullopt; // (an enumeration's operator++ the checker may have chosen, evaluated as a value)
    std::optional<ConstPlace> where;
    std::optional<ConstValue> v = constIncrement(u, P(u->op) == P::PlusPlus, ctx, depth + 1, &where);
    constCallPlaceRan = true; // (evaluated here: never a second time)
    return v ? where : std::nullopt;
  }
  if (e->kind == NodeKind::CastExpr ||
      (e->kind == NodeKind::NamedCastExpr && (static_cast<NamedCastExpr*>(e)->castKind == Kw::static_cast_ || static_cast<NamedCastExpr*>(e)->castKind == Kw::const_cast_)))
  {
    // `(T&&)value` (forward's, move's), `static_cast<const Base&>(derived)`: the operand's object
    // (its base part) in another value category, which a reference bound to it refers to (it was a
    // copy, blind to the object's later changes: language/consteval_reference_through_cast).
    Type* written = e->kind == NodeKind::CastExpr ? static_cast<CastExpr*>(e)->type : static_cast<NamedCastExpr*>(e)->type;
    Expr* operand = e->kind == NodeKind::CastExpr ? static_cast<CastExpr*>(e)->operand : static_cast<NamedCastExpr*>(e)->operand;
    const Ty* t = written ? convertType(written, ctx, depth + 1) : nullptr;
    if (!t || !t->isReference() || !t->inner || !t->inner->isKnown() || types.isDependent(t))
      return std::nullopt;
    std::optional<ConstPlace> at = constPlaceOf(operand, ctx, depth + 1, calls, ran);
    if (!at || !at->type)
      return std::nullopt;
    const Ty* want = types.unqualified(t->inner);
    const Ty* have = types.unqualified(types.stripRef(at->type));
    if (want != have)
    {
      std::optional<std::vector<size_t>> base = want->kind == TyKind::Class && have->kind == TyKind::Class ? constBasePath(have, want, depth + 1) : std::nullopt;
      if (!base)
        return std::nullopt; // (a cast to a derived class, or to another type: no place of it here)
      at->path.insert(at->path.end(), base->begin(), base->end());
      at->bits = 0;
    }
    at->type = t->inner;
    at->readOnly = at->readOnly || t->inner->isConst;
    return at;
  }
  if (e->kind == NodeKind::BinaryExpr && calls && e->ty && e->ty->kind == TyKind::LRef && e->ty->inner && !static_cast<BinaryExpr*>(e)->isKeyword &&
      !isAssignmentOperator(P(static_cast<BinaryExpr*>(e)->op)))
  {
    // `out << a`: the object an operator's reference result refers to (String's `<<` returns out).
    std::optional<ConstPlace> out;
    if (!constBinaryChain(static_cast<BinaryExpr*>(e), ctx, depth + 1, &out) || !out)
      return std::nullopt;
    out->type = e->ty->inner;
    out->readOnly = out->readOnly || e->ty->inner->isConst;
    return out;
  }
  if (e->kind == NodeKind::CallExpr && calls && e->ty && e->ty->isReference() && e->ty->inner && static_cast<CallExpr*>(e)->callee &&
      static_cast<CallExpr*>(e)->callee->kind == NodeKind::NameExpr)
  {
    // `add(uses, name)`, `forward<U>(value)`, `move(x)`: what a named function's reference result
    // refers to (a part of an object its arguments refer to), the function run once for it.
    std::optional<ConstPlace> out;
    std::optional<ConstValue> value = constCall(static_cast<CallExpr*>(e), ctx, depth + 1, &out);
    if (value && !out && ran)
      *ran = value; // (a reference the run holds no place of: what it refers to, as a value)
    if (!value || !out)
      return std::nullopt;
    const Ty* referred = e->ty->inner->isKnown() && !types.isDependent(e->ty->inner) ? e->ty->inner : out->type; // (else the run's: a template's own call)
    if (!referred)
      return std::nullopt;
    out->type = referred;
    out->readOnly = out->readOnly || referred->isConst;
    return out;
  }
  if (e->kind == NodeKind::CallExpr && calls)
  {
    // `result.front()`, `this->back()`: the part of the object a member function's reference
    // result refers to.
    auto* c = static_cast<CallExpr*>(e);
    auto* m = c->callee && c->callee->kind == NodeKind::MemberExpr ? static_cast<MemberExpr*>(c->callee) : nullptr;
    if (!m || !m->base || m->dynamicMember || m->member.parts.size() != 1)
      return std::nullopt;
    std::optional<ConstPlace> base = m->base->kind == NodeKind::ThisExpr && m->op == P::Arrow ? constThisPlace(ctx)
                                   : m->op == P::Dot                                         ? constPlaceOf(m->base, ctx, depth + 1, true)
                                                                                              : std::nullopt;
    std::optional<ConstValue> object = base ? constRead(*base) : std::nullopt;
    if (!object || !object->isObject() || !object->type || object->type->isBraceList())
      return std::nullopt;
    Subst subst;
    // The overload the walk chose, else the one the arguments' types choose.
    std::vector<const Ty*> argTypes;
    for (Expr* a : c->args)
      argTypes.push_back(a && a->ty && !constIsExpansion(a) ? argumentType(a, a->ty, types) : nullptr);
    Decl* chosen = m->resolved && m->resolved->kind == NodeKind::FunctionDecl ? m->resolved : nullptr;
    FunctionDecl* f = constPlaceMember(object->type, m->member.parts[0].symbol, c->args.size(), base->writable(), subst, depth + 1, chosen, &argTypes);
    if (f && (f->flags & DF_Virtual))
      if (FunctionDecl* g = constOverrider(f, object->type, 0); g != f && constDefinition(g))
      {
        f = g;
        subst = memberSubst(types.unqualified(object->type), g->parent ? g->parent->ownScope : nullptr, 0);
      }
    if (!f)
      return std::nullopt;
    std::vector<ConstValue> args;
    std::vector<std::optional<ConstPlace>> places;
    std::vector<uint8_t> categories;
    bool argued = constArguments(c->args, ctx, depth, args, &places, &categories);
    for (Expr* a : c->args)
      if (!constPure(a))
        constCallPlaceRan = true; // (an argument with a side effect evaluated: never a second time)
    if (!argued)
      return std::nullopt;
    LocalStack own;
    // (Written: `object.get<0>()`, a tuple-like structured binding's, referring into the object.)
    std::vector<const Ty*> given;
    if (const NamePart& part = m->member.parts[0]; part.hasTemplateArgs && !f->templateParams.empty())
      for (Node* w : part.templateArgs)
      {
        const Ty* t = templateArgument(w, f->templateParams[std::min(given.size(), f->templateParams.size() - 1)], ctx, depth + 1);
        if (w && w->kind == NodeKind::PackType && t && t->kind == TyKind::Pack)
          given.insert(given.end(), t->args.begin(), t->args.end());
        else
          given.push_back(t);
      }
    if (!f->templateParams.empty() && !constBindTemplate(f, given, c, args, ctx, depth, subst, own, &categories))
      return std::nullopt; // `all.emplace_back().roots`: a member function template's own parameters
    return constCallPlace(f, subst, args, *base, depth + 1, &places, &categories, ran);
  }
  return std::nullopt;
}

auto GlobalScope::Impl::constPlacedOperand(Expr* e, const TypeContext& ctx, int depth, std::optional<ConstPlace>& place)
  -> std::optional<std::optional<ConstValue>>
{
  place.reset();
  Expr* source = nullptr;
  uint8_t category = constCategory(e, source);
  if (category == 0 || !source || !source->ty || !source->ty->isKnown() || types.isDependent(source->ty))
    return std::nullopt;
  const Ty* t = types.unqualified(types.stripRef(source->ty));
  // (Its object and index evaluated with no call, or its object so for a member function: a call
  // that ran is the operand's own, never run again.)
  auto* call = source->kind == NodeKind::CallExpr ? static_cast<CallExpr*>(source) : nullptr;
  auto* member = call && call->callee && call->callee->kind == NodeKind::MemberExpr ? static_cast<MemberExpr*>(call->callee) : nullptr;
  bool called = (source->kind == NodeKind::IndexExpr && constPure(source)) ||
                (call && source->ty->kind == TyKind::LRef &&
                 ((member && member->base && constPure(member->base)) || (call->callee && call->callee->kind == NodeKind::NameExpr)));
  if (t->kind != TyKind::Class || !called)
    return std::nullopt;
  constCallPlaceRan = false;
  std::optional<ConstValue> ran;
  if (std::optional<ConstPlace> at = constPlaceOf(source, ctx, depth + 1, true, &ran))
  {
    place = at;
    return std::optional<std::optional<ConstValue>>(constPartOrUnset(*at));
  }
  if (ran)
    return std::optional<std::optional<ConstValue>>(ran);
  if (constCallPlaceRan)
    return std::optional<std::optional<ConstValue>>(std::optional<ConstValue>()); // it ran, and gave nothing: never run twice
  return std::nullopt;
}

auto GlobalScope::Impl::constPartOrUnset(const ConstPlace& place) -> std::optional<ConstValue>
{
  ConstPlace p = constResolve(place);
  if (p.path.empty())
    return p.binding().value;
  std::optional<ConstValue> v = constPart(p.binding().value, p.path);
  return v ? v : std::optional<ConstValue>(ConstValue::unset(place.type));
}

auto GlobalScope::Impl::constFreedPlace(const ConstPlace& p, const char* use) -> bool
{
  if (p.env != &constHeap() || p.index >= constAllocations().size() || !constAllocations()[p.index].isFreed)
    return false;
  if (constWhy().empty())
    constWhy() = std::string(use) + " memory freed at " + constWhereText(constAllocations()[p.index].freed);
  return true;
}

auto GlobalScope::Impl::constRead(const ConstPlace& place) -> std::optional<ConstValue>
{
  ConstPlace p = constResolve(place);
  if (constFreedPlace(p, "a read of"))
    return std::nullopt;
  if (p.path.empty())
    return p.binding().value.isUnset() ? std::nullopt : std::optional<ConstValue>(p.binding().value);
  return constPart(p.binding().value, p.path);
}

auto GlobalScope::Impl::constWrite(const ConstPlace& place, const ConstValue& v) -> bool
{
  ConstPlace p = constResolve(place);
  if (constFreedPlace(p, "a write to"))
    return false;
  return constSetPart(p.binding().value, p.path, 0, place.bits ? constFitBits(v, place.type, place.bits) : v);
}

auto GlobalScope::Impl::constBitsOf(const Ty* t, Decl* field, int depth) -> uint8_t
{
  const ConstClassLayout* layout = constLayoutOf(t, depth);
  if (!layout || depth > 16)
    return 0;
  for (size_t i = 0; i < layout->fields.size(); ++i)
    if (layout->fields[i] == field)
      return layout->fieldBits[i];
  for (size_t i = 0; i < layout->fields.size(); ++i)
    if (!layout->fields[i])
      if (uint8_t bits = constBitsOf(layout->fieldTypes[i], field, depth + 1))
        return bits; // in an anonymous member
  for (const Ty* b : layout->bases)
    if (uint8_t bits = constBitsOf(b, field, depth + 1))
      return bits;
  return 0;
}

auto GlobalScope::Impl::constDeref(const ConstValue& p) -> std::optional<ConstValue>
{
  if (!p.isPointer() || !p.parts || p.i < 0 || uint64_t(p.i) >= p.parts->size())
    return std::nullopt;
  const ConstValue& e = (*p.parts)[size_t(p.i)];
  return e.isUnset() ? std::nullopt : std::optional<ConstValue>(e);
}

auto GlobalScope::Impl::constPointerArith(P op, const ConstValue& l, const ConstValue& r) -> std::optional<ConstValue>
{
  auto into = [](const ConstValue& v) { return v.isPointer() && v.parts; };
  auto isNull = [](const ConstValue& v) { return v.kind == ConstValue::Kind::Int && v.i == 0; };
  if (into(l) && r.kind == ConstValue::Kind::Int && (op == P::Plus || op == P::Minus) && !(op == P::Minus && into(r)))
  {
    int64_t index = op == P::Plus ? l.i + r.i : l.i - r.i;
    if (index < 0 || uint64_t(index) > l.parts->size())
      return std::nullopt; // outside the array: undefined
    return ConstValue::pointer(l.type, l.parts, index);
  }
  if (into(r) && l.kind == ConstValue::Kind::Int && op == P::Plus)
    return constPointerArith(P::Plus, r, l);
  if (into(l) && into(r))
  {
    if (l.parts != r.parts)
    {
      if (constWhy().empty() && op != P::EqEq && op != P::NotEq) // pointers into different arrays: no order, no difference
        constWhy() = "an order or a difference of pointers to different objects, which C++ leaves unspecified";
      return std::nullopt;
    }
    switch (op)
    {
      case P::Minus: return ConstValue::integer(l.i - r.i, types.ptrdiffType());
      case P::EqEq: return ConstValue::integer(l.i == r.i, types.boolType());
      case P::NotEq: return ConstValue::integer(l.i != r.i, types.boolType());
      case P::Less: return ConstValue::integer(l.i < r.i, types.boolType());
      case P::Greater: return ConstValue::integer(l.i > r.i, types.boolType());
      case P::LessEq: return ConstValue::integer(l.i <= r.i, types.boolType());
      case P::GreaterEq: return ConstValue::integer(l.i >= r.i, types.boolType());
      default: return std::nullopt;
    }
  }
  if ((into(l) && isNull(r)) || (isNull(l) && into(r)))
  {
    if (op == P::EqEq || op == P::NotEq)
      return ConstValue::integer(op == P::NotEq, types.boolType());
  }
  return std::nullopt;
}

auto GlobalScope::Impl::constStrlen(const ConstValue& p) -> std::optional<ConstValue>
{
  if (p.isAddress() && p.target->storage)
  {
    // Along chars of the evaluation's own (a local array, malloc's) to the terminating zero.
    for (int64_t n = 0;; ++n)
    {
      std::optional<ConstValue> at = constAddressMove(p, n, 0);
      std::optional<ConstValue> c = at && constAddressDereferenceable(*at, 0) ? constReadTarget(*at, 1) : std::nullopt;
      if (!c || c->kind != ConstValue::Kind::Int)
        return std::nullopt;
      if (c->i == 0)
        return ConstValue::integer(n, types.sizeType());
    }
  }
  if (!p.isPointer() || !p.parts)
    return std::nullopt;
  for (size_t i = size_t(p.i); i < p.parts->size(); ++i)
  {
    const ConstValue& c = (*p.parts)[i];
    if (c.kind != ConstValue::Kind::Int)
      return std::nullopt;
    if (c.i == 0)
      return ConstValue::integer(int64_t(i - size_t(p.i)), types.sizeType());
  }
  return std::nullopt; // no terminating zero: reading past the array
}

auto GlobalScope::Impl::constRefersTo(const ConstValue& part, const Ty* declared) -> bool
{
  if (!declared || !declared->isReference() || !part.isAddress() || !part.target || !part.type)
    return false;
  // (Told from a copy of what it refers to by its type: a pointer to the referred type, where a
  // copy of a pointer, `T*&`'s, is a pointer to what that points at.)
  const Ty* pt = types.unqualified(types.stripRef(part.type));
  return pt->kind == TyKind::Pointer && pt->inner && types.unqualified(pt->inner) == types.unqualified(types.stripRef(declared));
}

auto GlobalScope::Impl::constReferenceTo(Expr* x, const Ty* reference, const TypeContext& ctx, int depth) -> std::optional<std::optional<ConstValue>>
{
  const Ty* referent = types.stripRef(reference);
  Expr* source = nullptr;
  if (!x || !referent || constCategory(x, source) == 0)
    return std::nullopt; // a prvalue: a temporary, which the member keeps a copy of (as before)
  std::optional<ConstValue> ran;
  constCallPlaceRan = false;
  std::optional<ConstPlace> at = constPlaceOf(x, ctx, depth + 1, true, &ran);
  if (at && constAddressable(*at))
    return std::optional<ConstValue>(ConstValue::address(types.pointer(referent), constStorageTarget(*at)));
  if (ran)
    return std::optional<ConstValue>(constConvertTo(ran, referent, depth + 1)); // (a call that ran: its value, never run again)
  if (!at && constCallPlaceRan)
    return std::optional<std::optional<ConstValue>>(std::optional<ConstValue>()); // it ran, and gave nothing: never run twice
  if (!at && constPure(x))
    if (std::optional<ConstValue> a = constAddressOf(x, ctx, depth + 1);
        a && a->isAddress() && a->target && a->target->decl && a->target->decl->kind == NodeKind::VariableDecl)
    {
      ConstValue r = *a; // a global's, or a part of one: read through as the constant it is
      r.type = types.pointer(referent);
      return std::optional<ConstValue>(r);
    }
  return std::nullopt;
}

auto GlobalScope::Impl::constDesignatesConstant(Expr* x, const TypeContext& ctx, int depth) -> bool
{
  std::optional<ConstDesignated> a = constDesignated(x, ctx, depth);
  return a && !a->target.storage && a->target.decl && a->target.decl->kind == NodeKind::VariableDecl;
}

auto GlobalScope::Impl::constThroughReference(const ConstPlace& member, int depth) -> std::optional<ConstPlace>
{
  if (!member.type || !member.type->isReference())
    return member;
  std::optional<ConstValue> part = constRead(member);
  if (!part || !constRefersTo(*part, member.type))
    return member; // (a copy of what it refers to: made where no place of it was known)
  if (!part->target->storage)
    return std::nullopt; // a global's: a constant, read through constMemberRead, no place to write
  return constStoragePlace(*part, "a use of", depth);
}

auto GlobalScope::Impl::constMemberRead(MemberExpr* m, const TypeContext& ctx, int depth) -> std::optional<ConstValue>
{
  if (std::optional<ConstPlace> place = constPlaceOf(m, ctx, depth + 1))
    return constRead(*place);
  if (!m->base || m->base->kind == NodeKind::ThisExpr)
    return std::nullopt;
  std::optional<ConstValue> object = constValue(m->base, ctx, depth + 1);
  if (object && m->op == P::Arrow) // `where->x`: through the address of a constant
    object = object->isAddress() ? constReadTarget(*object, depth + 1) : object->isPointer() ? constDeref(*object) : std::nullopt;
  if (!object || !object->isObject())
    return std::nullopt;
  Decl* d = constFieldNamed(m->resolved, m->member, object->type);
  std::optional<std::vector<size_t>> path = d ? constFieldPath(object->type, d, 0) : std::nullopt;
  std::optional<ConstValue> part = path ? constPart(*object, *path) : std::nullopt;
  const ConstClassLayout* layout = part ? constLayoutOf(object->type, 0) : nullptr;
  const Ty* declared = layout ? declaredTypeOf(d, !layout->subst.empty() ? &layout->subst : nullptr, 0) : nullptr;
  if (part && constRefersTo(*part, declared))
    return constReadTarget(*part, depth + 1); // a reference member: what it refers to, as it is now
  return part;
}

auto GlobalScope::Impl::constIndex(IndexExpr* x, const TypeContext& ctx, int depth) -> std::optional<ConstValue>
{
  if (x->args.size() != 1)
    return std::nullopt;
  if (std::optional<ConstPlace> place = constPlaceOf(x, ctx, depth + 1))
    return constRead(*place);
  std::optional<ConstValue> base = constValue(x->base, ctx, depth + 1);
  if (!base)
    return std::nullopt;
  if (base->isObject() && base->type && types.unqualified(base->type)->kind == TyKind::Class)
    return constOperatorCall(*base, opSymbols[uint32_t(P::LBracket)], x->args, ctx, depth);
  std::optional<ConstValue> index = constValue(x->args[0], ctx, depth + 1);
  if (!index || index->kind != ConstValue::Kind::Int)
    return std::nullopt;
  if (base->isAddress()) // `second[1]`: the element of the global it points into
  {
    std::optional<ConstValue> moved = constAddressMove(*base, index->i, depth);
    return moved ? constReadTarget(*moved, depth + 1) : std::nullopt;
  }
  if (base->isPointer())
    return constPointerArith(P::Plus, *base, *index) ? constDeref(*constPointerArith(P::Plus, *base, *index)) : std::nullopt;
  if (!base->isObject() || index->i < 0 || uint64_t(index->i) >= base->parts->size())
  {
    if (base->isObject() && constWhy().empty())
      constWhy() = "an index of " + std::to_string(index->i) + " into an array of " + std::to_string(base->parts->size());
    return std::nullopt;
  }
  const ConstValue& e = (*base->parts)[size_t(index->i)];
  return e.isUnset() ? std::nullopt : std::optional<ConstValue>(e);
}

auto GlobalScope::Impl::constOperatorCall(const ConstValue& object, uint32_t name, Span<Expr*> args, const TypeContext& ctx, int depth) -> std::optional<ConstValue>
{
  Scope* s = classScopeOf(object.type);
  Lookup l;
  if (!s || !lookupInScope(s, name, l, depth + 1))
    return std::nullopt;
  // The arguments where they are, a prvalue's as a temporary of the full expression, destroyed at
  // its end (`names[String("b")]`'s heap block was never freed), a class object's lvalue typed as one.
  std::vector<ConstValue> values;
  std::vector<std::optional<ConstPlace>> places;
  std::vector<uint8_t> categories;
  if (!constArguments(args, ctx, depth, values, &places, &categories))
    return std::nullopt;
  std::vector<const Ty*> argTypes;
  for (size_t i = 0; i < values.size(); ++i)
  {
    const Ty* t = constTypeOfValue(values[i]);
    if (t && i < categories.size() && categories[i] == 1 && !t->isReference() && types.unqualified(t)->kind == TyKind::Class)
      t = types.lref(i < places.size() && places[i] && !places[i]->writable() ? types.qualified(types.unqualified(t), true, false) : t);
    argTypes.push_back(t);
  }
  std::vector<FunctionDecl*> candidates;
  if (l.overloads)
  {
    for (const Entry& entry : *l.overloads)
      if (entry.decl && entry.decl->kind == NodeKind::FunctionDecl && constDefinition(static_cast<FunctionDecl*>(entry.decl)))
        candidates.push_back(static_cast<FunctionDecl*>(entry.decl));
  }
  else if (l.decl && l.decl->kind == NodeKind::FunctionDecl && constDefinition(static_cast<FunctionDecl*>(l.decl)))
    candidates.push_back(static_cast<FunctionDecl*>(l.decl));
  // Of a const and a non-const overload alike (`operator[]`), the const one: the object is a value.
  std::vector<FunctionDecl*> consts;
  for (FunctionDecl* f : candidates)
    if (f->flags & DF_Const)
      consts.push_back(f);
  if (!consts.empty())
    candidates = consts;
  Subst subst = memberSubst(types.unqualified(object.type), l.foundIn, depth + 1);
  bool ambiguous = false;
  FunctionDecl* f = chooseAmong(candidates, argTypes, subst.empty() ? nullptr : &subst, types.unqualified(object.type), ambiguous);
  if (!f || ambiguous || !f->templateParams.empty())
    return std::nullopt;
  LocalStack params;
  for (const Subst::Pair& p : subst.pairs)
    params.push(static_cast<TemplateParamDecl*>(p.param)->name, p.param);
  TypeContext fctx{lexicalScopeOf(f), params.items.empty() ? nullptr : &params, subst.empty() ? nullptr : &subst,
                   f->file ? f->file->tokens : nullptr, f->file ? f->file->text : std::string_view()};
  std::optional<ConstValue> self = object;
  return constRun(f, values, fctx, depth, &self, &places, &categories);
}

auto GlobalScope::Impl::constRun(FunctionDecl* f, const std::vector<ConstValue>& args, TypeContext fctx, int depth, std::optional<ConstValue>* self, const std::vector<std::optional<ConstPlace>>* places, const std::vector<uint8_t>* categories, const ConstPlace* selfPlace, std::optional<ConstPlace>* returnPlace) -> std::optional<ConstValue>
{
  bool qualified = std::exchange(constQualifiedCall(), false); // (read first: no other run takes it)
  if (int which = intrinsicOf(f))
    return constIntrinsic(which, f, args, fctx.subst, depth);
  // A call of a function taking a Text the walk left as written (a template's own nodes): its
  // arguments from the Text parameter's position on packed here (design.md 14).
  if (size_t k = f->params.size() - 1; namesTextLast(f) && args.size() >= k && !(args.size() == k + 1 && isText(args[k].type)))
  {
    std::optional<ConstValue> text = constTextOf(args, places, k, depth);
    if (!text)
      return std::nullopt;
    std::vector<ConstValue> packed(args.begin(), args.begin() + k);
    packed.push_back(*text);
    std::vector<std::optional<ConstPlace>> packedPlaces;
    std::vector<uint8_t> packedCategories;
    for (size_t i = 0; i < k; ++i)
    {
      packedPlaces.push_back(places && i < places->size() ? (*places)[i] : std::nullopt);
      packedCategories.push_back(categories && i < categories->size() ? (*categories)[i] : 0);
    }
    packedPlaces.push_back(std::nullopt);
    packedCategories.push_back(0);
    constQualifiedCall() = qualified;
    return constRun(f, packed, fctx, depth, self, &packedPlaces, &packedCategories, selfPlace, returnPlace);
  }
  // A virtual function runs as the object's own class overrides it: the most-derived object's, which
  // a base part's place leads to (`const A* a = &b; a->f()` ran A::f at compile time, B::f at run
  // time: language/consteval_virtual_call_through_base_pointer), its overrider run on its part of
  // the class that declares it. Not one named with its class (`A::f()` in B's override of f ran B's
  // again, until calls nested 512 deep: language/consteval_qualified_call_is_no_virtual_call).
  if (!qualified && self && *self && selfPlace && (f->flags & (DF_Virtual | DF_Override)) && !(f->flags & DF_Destructor))
  {
    ConstPlace whole = constDynamicPlace(*selfPlace, depth);
    std::optional<ConstValue> object = whole.path.size() < constResolve(*selfPlace).path.size() ? constRead(whole) : std::nullopt;
    if (object && object->isObject() && object->type && !object->type->isBraceList())
      if (FunctionDecl* g = constOverrider(f, object->type, 0); g != f && constDefinition(g))
      {
        std::vector<size_t> path;
        const Ty* owner = g->parent ? constBaseDeclaring(object->type, g->parent, path, 0) : nullptr;
        if (!owner)
          return std::nullopt;
        ConstPlace at = whole;
        at.path.insert(at.path.end(), path.begin(), path.end());
        at.type = owner;
        std::optional<ConstValue> part = constRead(at);
        Subst subst = memberSubst(types.unqualified(object->type), g->parent->ownScope, 0);
        std::optional<ConstValue> r = part ? constCallOn(g, subst, args, &part, depth + 1, &at, places, categories, returnPlace) : std::nullopt;
        *self = constRead(*selfPlace); // (the object as the call left it)
        return r;
      }
  }
  if (!qualified && self && *self)
    if (FunctionDecl* g = constOverrider(f, (*self)->type, 0); g != f && constDefinition(g))
    {
      Subst subst = memberSubst(types.unqualified((*self)->type), g->parent ? g->parent->ownScope : nullptr, 0);
      return constCallOn(g, subst, args, self, depth + 1, selfPlace, places, categories, returnPlace);
    }
  struct Call
  {
    ~Call() { --constCalls(); }

    bool ok = ++constCalls() <= 512 && stackRoomLeft();
  } call;
  if (!call.ok)
  {
    if (constWhy().empty())
      constWhy() = "calls nested more than 512 deep";
    return std::nullopt;
  }
  // The body counts its own nesting from here: the calls that reached it are the call guard's to
  // bound, and a type written in it converts as deep as it is written, not as deep as the chain
  // (`sizeof(T)` in Limits<T>::max(), six calls into BoundingBox::emptyValue()).
  depth = 0;
  // A member function declared in the class and defined after it: the definition's body and
  // parameters, the declaration's default arguments.
  FunctionDecl* def = constDefinition(f);
  if (!def || def->params.size() != f->params.size())
    return std::nullopt;
  ranBody(def);
  def = constInstanceBody(def, fctx.subst); // a template's function: its instance's
  std::vector<ConstBinding> env;
  ConstFrame frame(&env);
  ConstRunning running(def->file);
  fctx.env = &env;
  for (size_t i = 0; i < def->params.size(); ++i)
  {
    auto* p = static_cast<ParamDecl*>(def->params[i]);
    Expr* defaultValue = p->defaultValue ? p->defaultValue : static_cast<ParamDecl*>(f->params[i])->defaultValue;
    if (p->isPack)
    {
      // `Args&&... args`: the arguments from here on, each converted to its element's type.
      const Ty* packTy = i + 1 == def->params.size() ? declaredTypeOf(p, fctx.subst, depth + 1) : nullptr;
      if (!packTy || packTy->kind != TyKind::Pack || packTy->args.size() != (args.size() > i ? args.size() - i : 0))
        return std::nullopt;
      std::vector<ConstValue> elements;
      std::vector<ConstPlace> elementPlaces;
      for (size_t k = i; k < args.size(); ++k)
      {
        std::optional<ConstValue> e = constConvertTo(args[k], packTy->args[k - i], depth + 1);
        if (!e)
          return std::nullopt;
        elements.push_back(*e);
        bool refers = packTy->args[k - i]->isReference() && places && k < places->size() && (*places)[k];
        elementPlaces.push_back(refers ? *(*places)[k] : ConstPlace{});
      }
      ConstBinding b{p->name, ConstValue::object(packTy, std::move(elements))};
      b.type = packTy;
      b.writable = false;
      b.pack = true;
      b.packPlaces = std::move(elementPlaces);
      env.push_back(std::move(b));
      break;
    }
    const Ty* type = p->type ? convertType(p->type, fctx, depth + 1) : nullptr;
    if (!constBindParameter(p, type, i, args, defaultValue, fctx, depth, places, categories, env))
      return std::nullopt;
  }
  size_t selfAt = env.size();
  if (self && *self && selfPlace)
  {
    // On an object the evaluation holds a place of: `this` refers to it there (what the
    // function changes is changed in it, `&this->value` is its address).
    ConstBinding alias{constThisSymbol, ConstValue::unset((*self)->type)};
    alias.type = (*self)->type;
    alias.writable = !(f->flags & DF_Const) && selfPlace->writable();
    alias.aliasEnv = selfPlace->env;
    alias.aliasIndex = selfPlace->index;
    alias.aliasPath = selfPlace->path;
    env.push_back(std::move(alias));
  }
  else if (self && *self)
  {
    env.push_back(constBinding(constThisSymbol, **self, (*self)->type));
    env.back().writable = !(f->flags & DF_Const);
  }
  const Ty* returns = f->returnType ? convertType(f->returnType, fctx, depth + 1) : nullptr;
  fctx.returnType = returns;
  fctx.returnPlace = returnPlace; // (a reference result asked for as a place: constCallPlace)
  fctx.namedReturn = returnPlace ? nullptr : constNamedReturn(def->body, returns, fctx, depth);
  bool isVoid = (returns && types.unqualified(returns)->kind == TyKind::Void) || (f->flags & DF_Destructor);
  std::optional<ConstValue> result;
  ConstFlow flow = constStatement(def->body, fctx, env, result, depth + 1);
  if (self && *self && selfAt < env.size() && env[selfAt].symbol == constThisSymbol)
    *self = selfPlace ? constRead(*selfPlace) : std::optional<ConstValue>(env[selfAt].value);
  if (!constEndLocals(env, 0, depth, flow != ConstFlow::Failed)) // the by-value parameters
    return std::nullopt;
  if (returnPlace)
  {
    if (flow != ConstFlow::Returned)
      return std::nullopt;
    if (*returnPlace)
      return ConstValue::integer(0, types.voidType());
    return result && returns ? constConvertTo(result, returns, depth + 1) : result; // a constant's value (the return statement's)
  }
  if (isVoid && (flow == ConstFlow::Next || flow == ConstFlow::Returned))
    return ConstValue::integer(0, types.voidType()); // a helper run for what it checks (asserts)
  if (flow != ConstFlow::Returned || !result)
    return std::nullopt;
  return returns ? constConvertTo(result, returns, depth + 1) : result;
}

auto GlobalScope::Impl::constBindParameter(ParamDecl* p, const Ty* type, size_t i, const std::vector<ConstValue>& args, Expr* defaultValue, const TypeContext& fctx, int depth,
                                            const std::vector<std::optional<ConstPlace>>* places, const std::vector<uint8_t>* categories, std::vector<ConstBinding>& env) -> bool
{
  // A reference refers to the argument's object when that is of its type (or of a class derived
  // from it: the base part); anything else converts, a class object it makes being a temporary.
  std::optional<std::vector<size_t>> basePart;
  if (type && type->isReference() && i < args.size() && type->inner)
  {
    const Ty* want = types.unqualified(type->inner);
    const Ty* have = places && i < places->size() && (*places)[i] && (*places)[i]->type ? types.unqualified(types.stripRef((*places)[i]->type))
                   : args[i].type                                                     ? types.unqualified(types.stripRef(args[i].type))
                                                                                      : nullptr;
    // (An array's const is its elements': `const int (&a)[3]` refers to an int[3] as it is.)
    if (have && (have == want || want->kind == TyKind::Void || (have->kind == TyKind::Array && unqualifiedDeep(have) == unqualifiedDeep(want))))
      basePart = std::vector<size_t>{};
    else if (have && have->kind == TyKind::Class && want->kind == TyKind::Class)
      basePart = constBasePath(have, want, depth + 1);
  }
  bool prvalue = categories && i < categories->size() && (*categories)[i] == 0;
  if (type && type->isReference() && i < args.size() && !args[i].isUnset() &&
      ((!basePart && types.unqualified(type->inner)->kind == TyKind::Class) ||
       (basePart && !(places && i < places->size() && (*places)[i]) && prvalue && args[i].isObject() && args[i].type && constDestroyRunsCode(args[i].type))))
  {
    // `const Text& t` given `Text("a")`, or a StringView where a `const String&` is wanted: a
    // temporary of the caller's full expression, which the function refers to (and may move
    // from), destroyed when that ends.
    std::optional<ConstValue> v = constConvertTo(args[i], types.unqualified(type->inner), depth + 1);
    std::optional<ConstPlace> at = v && v->isObject() && v->type && constDestroyRunsCode(v->type) ? constMaterialize(*v, type->inner) : std::nullopt;
    if (at)
    {
      ConstBinding alias{p->name, ConstValue::unset(type)};
      alias.type = at->type;
      alias.writable = !(type->inner && type->inner->isConst);
      alias.aliasEnv = at->env;
      alias.aliasIndex = at->index;
      env.push_back(std::move(alias));
      return true;
    }
  }
  if (type && type->isReference() && places && i < places->size() && (*places)[i] && basePart)
  {
    // `int& out`, `const Point& p` given a local: a reference to it (to its base part).
    const ConstPlace& at = *(*places)[i];
    ConstBinding alias{p->name, ConstValue::unset(type)};
    alias.type = basePart->empty() ? (*places)[i]->type : types.unqualified(type->inner);
    alias.writable = !isConstObject(type->inner) && !at.readOnly;
    alias.aliasEnv = at.env;
    alias.aliasIndex = at.index;
    alias.aliasPath = at.path;
    alias.aliasPath.insert(alias.aliasPath.end(), basePart->begin(), basePart->end());
    env.push_back(std::move(alias));
    return true;
  }
  std::optional<ConstValue> v;
  if (i < args.size() && type && !type->isReference() && categories && i < categories->size() && (*categories)[i] &&
      args[i].isObject() && args[i].type && types.unqualified(types.stripRef(args[i].type)) == types.unqualified(type) &&
      constCopyRunsCode(type))
  {
    // `Counted c` given an lvalue: copied by the class's copy constructor (an xvalue: moved).
    std::optional<ConstPlace> at = places && i < places->size() ? (*places)[i] : std::nullopt;
    v = constCopyConstruct(type, args[i], at, (*categories)[i] == 2, depth + 1);
  }
  else
  {
    // `path(path(a, b), c)` with a StringView parameter: the String the inner call made is
    // converted, and is a temporary of the caller's full expression, destroyed when that ends.
    bool held = places && i < places->size() && (*places)[i];
    if (i < args.size() && type && categories && i < categories->size() && (*categories)[i] == 0 && args[i].isObject() && args[i].type &&
        types.unqualified(types.stripRef(args[i].type)) != types.unqualified(types.stripRef(type)) && constDestroyRunsCode(args[i].type) && !held)
      constMaterialize(args[i], args[i].type);
    else if (held && type && !type->isReference() && categories && i < categories->size() && (*categories)[i] == 0 &&
             types.unqualified(types.stripRef(args[i].type)) == types.unqualified(types.stripRef(type)))
      (*places)[i]->binding().owns = false; // the prvalue's temporary is the parameter now: destroyed by the call
    v = i < args.size() ? constConvertTo(args[i], type, depth + 1)
                        : defaultValue ? constInitialize(type, defaultValue, fctx, depth + 1) : std::nullopt;
  }
  if (!v)
    return false;
  env.push_back(constBinding(p->name, *v, type));
  // A by-value parameter is the callee's to destroy, once the evaluator knows it made it (a copy,
  // a prvalue, a default).
  env.back().owns = type && !type->isReference() && (i >= args.size() || (categories && i < categories->size()));
  env.back().referenceToTemporary = type && type->isReference(); // (a value: of no place the run holds)
  if (type && type->kind == TyKind::RRef && !(type->inner && type->inner->isConst))
    env.back().writable = true; // (the temporary an rvalue reference binds: `take(3)` of `take(int&& x)`, which may change it)
  return true;
}

auto GlobalScope::Impl::constBinding(uint32_t symbol, const ConstValue& v, const Ty* declared) -> ConstBinding
{
  ConstBinding b{symbol, v};
  const Ty* t = declared ? types.unqualified(types.stripRef(declared)) : nullptr;
  // (A pointer's too: `int* p = nullptr` holds addresses later.)
  bool pointer = t && t->kind == TyKind::Pointer && t->isKnown() && !types.isDependent(t) && !containsAuto(t);
  b.type = t && (t->isArithmetic() || t->kind == TyKind::Enum || pointer) ? t : v.type;
  b.writable = !(declared && declared->isReference() && !(declared->inner && declared->inner->isConst)) &&
               !(declared && !declared->isReference() && declared->isConst);
  return b;
}

auto GlobalScope::Impl::constAssign(BinaryExpr* b, const TypeContext& ctx, int depth, std::optional<ConstPlace>* where) -> std::optional<ConstValue>
{
  // The right operand first, as C++17 orders it; one a call gives (`x = v[i]`) where it is.
  std::optional<ConstPlace> rightPlace;
  std::optional<std::optional<ConstValue>> placed = constPlacedOperand(b->rhs, ctx, depth, rightPlace);
  std::optional<ConstValue> r = placed ? *placed : constValue(b->rhs, ctx, depth + 1);
  std::optional<ConstPlace> place = r ? constPlaceOf(b->lhs, ctx, depth + 1, true) : std::nullopt;
  if (!place || !place->writable())
    return std::nullopt;
  std::optional<ConstValue> old = P(b->op) == P::Assign ? std::optional<ConstValue>(ConstValue{}) : constRead(*place);
  if (!old)
    return std::nullopt;
  P op = P(b->op);
  // An object's own operator (`optional = position`, `total += amount`): run on it.
  const Ty* target = place->type ? types.unqualified(types.stripRef(place->type)) : nullptr;
  if (target && target->kind == TyKind::Class)
  {
    // The right operand as overload resolution sees it: an lvalue as `T&` (argumentType), so that
    // `y = x` copies and only `y = move(x)` moves.
    const Ty* rightType = b->rhs->ty && b->rhs->ty->isKnown() && !types.isDependent(b->rhs->ty) ? b->rhs->ty : constTypeOfValue(*r);
    Subst subst;
    FunctionDecl* f = constClassOperator(target, op, {argumentType(b->rhs, rightType, types)}, subst, depth);
    if (f && !constDefaulted(f) && constDefinition(f))
    {
      // Run on the object where it is, the right operand referred to where it is: an lvalue's
      // place, a prvalue as a temporary of the full expression (`name = String(text)`), so that
      // `this == &other` compares and a move empties its source.
      Expr* source = nullptr;
      uint8_t category = constCategory(b->rhs, source);
      std::optional<ConstPlace> right = rightPlace;
      if (!right && category != 0 && constPure(source))
        right = constPlaceOf(source, ctx, depth + 1);
      else if (!right && category == 0 && r->isObject() && r->type && types.unqualified(types.stripRef(r->type))->kind == TyKind::Class)
        right = constMaterialize(*r, r->type);
      std::vector<std::optional<ConstPlace>> places{right};
      std::vector<uint8_t> categories{category};
      std::optional<ConstValue> self = constRead(*place);
      if (!self)
        return std::nullopt;
      std::optional<ConstValue> result = constCallOn(f, subst, {*r}, &self, depth + 1, &*place, &places, &categories);
      if (!result || !self)
        return std::nullopt;
      if (where)
        *where = place;
      return constRead(*place);
    }
    if (op != P::Assign)
      return std::nullopt; // `a += b` of a class without its operator+=
    if (f && (f->flags & DF_Delete))
      return std::nullopt;
    if (r->isObject() && r->type && types.unqualified(types.stripRef(r->type)) == target && constAssignRunsCode(target))
    {
      // An implicit or defaulted operator= (`y = x` of a struct holding a String): member by
      // member, the right operand's parts referred to where they are (a move empties them).
      Expr* source = nullptr;
      uint8_t category = constCategory(b->rhs, source);
      std::optional<ConstPlace> right = rightPlace;
      if (!right && category != 0 && constPure(source))
        right = constPlaceOf(source, ctx, depth + 1);
      else if (!right && category == 0)
        right = constMaterialize(*r, r->type);
      if (!constAssignObject(*place, target, *r, right, category, depth + 1))
        return std::nullopt;
      if (where)
        *where = place;
      return constRead(*place);
    }
  }
  if (target && target->kind == TyKind::Enum && op != P::Assign && b->call == OperatorCall::Function && b->callee && !b->calleeMember &&
      b->callee->templateParams.empty())
  {
    // `flags |= other` with an operator|=(Flags&, Flags) declared: that function, as the checker
    // chose it (Walker::enumOperator) and codegen calls it, given the left operand where it is
    // (language/enum_operand_calls_user_defined_operator).
    std::vector<std::optional<ConstPlace>> places{place, std::nullopt};
    std::vector<uint8_t> categories{1, 0};
    return constCallOn(b->callee, Subst{}, {*old, *r}, nullptr, depth + 1, nullptr, &places, &categories);
  }
  std::optional<ConstValue> v = r;
  if (op != P::Assign)
  {
    static const std::pair<P, P> compound[] = {{P::PlusAssign, P::Plus}, {P::MinusAssign, P::Minus}, {P::StarAssign, P::Star},
                                               {P::SlashAssign, P::Slash}, {P::PercentAssign, P::Percent}, {P::AmpAssign, P::Amp},
                                               {P::PipeAssign, P::Pipe}, {P::CaretAssign, P::Caret}, {P::ShlAssign, P::Shl},
                                               {P::ShrAssign, P::Shr}};
    std::optional<P> base;
    for (auto [assign, plain] : compound)
      if (assign == op)
        base = plain;
    if (!base)
      return std::nullopt;
    v = old->isPointer()   ? constPointerArith(*base, *old, *r)
      : old->isAddress() ? constAddressArith(*base, *old, *r, depth)
                         : constArith().binary(*base, *old, *r);
  }
  v = v ? constConvertTo(v, place->type, depth + 1) : std::nullopt;
  if (!v || !constWrite(*place, *v))
    return std::nullopt;
  if (where)
    *where = place;
  return v;
}

auto GlobalScope::Impl::constEnumUnary(UnaryExpr* u, P op, const TypeContext& ctx, int depth) -> std::optional<std::optional<ConstValue>>
{
  bool step = op == P::PlusPlus || op == P::MinusMinus;
  bool postfix = step && u->postfix;
  if (u->postfix && !step)
    return std::nullopt;
  const Ty* operand = argumentType(u->operand, u->operand->ty, types);
  FunctionDecl* f = u->call == OperatorCall::Function && !u->calleeMember ? u->callee : nullptr;
  if (verifyInterpreterDecisions())
  {
    // KPP_VERIFY_DECISIONS: this lookup's own, against the checker's.
    std::vector<const Ty*> argTypes{operand};
    if (postfix)
      argTypes.push_back(types.intType());
    FunctionDecl* own = constFreeFunction(opSymbols[uint32_t(op)], argTypes, ctx.scope);
    const Ty* ft = own ? declaredTypeOf(own, nullptr, 0) : nullptr;
    if (ft && (ft->kind != TyKind::Function || ft->args.size() != argTypes.size() ||
               types.unqualified(types.stripRef(ft->args[0])) != types.unqualified(types.stripRef(operand))))
      own = nullptr;
    if (own != f && (!own || !f || constDefinition(own) != constDefinition(f)))
      fprintf(stderr, "%s: error: decision check (interpreter): it calls %s, the checker chose %s\n",
              ctx.function && ctx.function->file ? locationOf(ctx.function->file, u->token).c_str() : "<interpreter>",
              own ? debugName(own->name.last().symbol).c_str() : "the built-in operator", f ? debugName(f->name.last().symbol).c_str() : "the built-in operator");
    f = own;
  }
  if (!f || !f->templateParams.empty())
    return std::nullopt; // the built-in operator
  // `!flags`, `~flags`, `++d`, `d++`: the operator function declared for the enumeration that the
  // checker chose (Walker::enumUnaryOperator, design.md 24), as codegen calls it, its operand where it
  // is for a reference parameter; postfix given 0 too (language/enum_operand_calls_user_defined_operator).
  std::optional<ConstPlace> at = constPure(u->operand) ? constPlaceOf(u->operand, ctx, depth + 1, true) : std::nullopt;
  std::optional<ConstValue> v = at ? constRead(*at) : constValue(u->operand, ctx, depth + 1);
  if (!v)
    return std::optional<ConstValue>();
  std::vector<ConstValue> args{*v};
  std::vector<std::optional<ConstPlace>> places{at};
  std::vector<uint8_t> categories{uint8_t(at ? 1 : 0)};
  if (postfix)
  {
    args.push_back(ConstValue::integer(0, types.intType()));
    places.push_back(std::nullopt);
    categories.push_back(0);
  }
  return constCallOn(f, Subst{}, args, nullptr, depth + 1, nullptr, &places, &categories);
}

auto GlobalScope::Impl::constIncrement(UnaryExpr* u, bool increment, const TypeContext& ctx, int depth, std::optional<ConstPlace>* where) -> std::optional<ConstValue>
{
  std::optional<ConstPlace> place = constPlaceOf(u->operand, ctx, depth + 1, true);
  if (!place || !place->writable())
    return std::nullopt;
  std::optional<ConstValue> old = constRead(*place);
  if (!old)
    return std::nullopt;
  if (old->isObject() && old->type && !old->type->isBraceList() && types.unqualified(types.stripRef(old->type))->kind == TyKind::Class)
  {
    // `++it` of an object: its class's operator++ (postfix: operator++(int), given 0) run on it where
    // it is, as codegen calls it; the prefix one's value is the object after it.
    P op = increment ? P::PlusPlus : P::MinusMinus;
    Subst subst;
    std::vector<const Ty*> argTypes;
    std::vector<ConstValue> args;
    if (u->postfix)
    {
      argTypes.push_back(types.intType());
      args.push_back(ConstValue::integer(0, types.intType()));
    }
    FunctionDecl* f = constClassOperator(old->type, op, argTypes, subst, depth);
    if (!f || !constDefinition(f))
    {
      if (constWhy().empty())
        constWhy() = std::string(u->postfix ? "a postfix " : "a prefix ") + std::string(punctuatorSpelling(op)) + " on " +
                     types.toString(types.unqualified(types.stripRef(old->type)), &symbols) + " without an operator function the evaluator can run";
      return std::nullopt;
    }
    std::optional<ConstValue> self = old;
    std::optional<ConstValue> r = constCallOn(f, subst, args, &self, depth + 1, &*place);
    if (!r)
      return std::nullopt;
    if (where && !u->postfix)
      *where = place;
    return u->postfix ? r : constRead(*place);
  }
  ConstValue one = ConstValue::integer(1, types.intType());
  std::optional<ConstValue> v = old->isPointer()   ? constPointerArith(increment ? P::Plus : P::Minus, *old, one)
                                : old->isAddress() ? constAddressMove(*old, increment ? 1 : -1, depth)
                                                   : constArith().binary(increment ? P::Plus : P::Minus, *old, one);
  v = v ? constConvertTo(v, place->type, depth + 1) : std::nullopt;
  if (!v || !constWrite(*place, *v))
    return std::nullopt;
  if (where && !u->postfix)
    *where = place;
  return u->postfix ? *old : *v;
}

auto GlobalScope::Impl::isAssignmentOperator(P op) -> bool
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

auto GlobalScope::Impl::hasNestedLabel(Stmt* s, int depth) -> bool
{
  if (!s || depth > 64)
    return false;
  switch (s->kind)
  {
    case NodeKind::CaseStmt: case NodeKind::DefaultStmt: return depth > 1; // the body's own statements are at depth 1
    case NodeKind::BlockStmt:
      for (Stmt* inner : static_cast<BlockStmt*>(s)->stmts)
        if (hasNestedLabel(inner, depth + 1))
          return true;
      return false;
    case NodeKind::IfStmt: return hasNestedLabel(static_cast<IfStmt*>(s)->then, depth + 1) || hasNestedLabel(static_cast<IfStmt*>(s)->else_, depth + 1);
    case NodeKind::ForStmt: return hasNestedLabel(static_cast<ForStmt*>(s)->body, depth + 1);
    case NodeKind::WhileStmt: return hasNestedLabel(static_cast<WhileStmt*>(s)->body, depth + 1);
    case NodeKind::DoStmt: return hasNestedLabel(static_cast<DoStmt*>(s)->body, depth + 1);
    default: return false; // a nested switch has labels of its own
  }
}

auto GlobalScope::Impl::constRangeForIterators(RangeForStmt* r, const TypeContext& ctx, std::vector<ConstBinding>& env, std::optional<ConstValue>& result, int depth, const ConstValue& begin, const ConstValue& end, const Ty* declared, uint32_t name) -> ConstFlow
{
  size_t at = env.size();
  env.push_back(constBinding(0, begin, begin.type));
  env.back().owns = true;
  env.push_back(constBinding(0, end, end.type));
  env.back().owns = true;
  ConstPlace it{&env, at, {}, begin.type}, last{&env, at + 1, {}, end.type};
  const Ty* iterator = types.unqualified(begin.type);
  Subst derefSubst, stepSubst;
  FunctionDecl* deref = constClassOperator(iterator, P::Star, {}, derefSubst, depth);
  FunctionDecl* step = constClassOperator(iterator, P::PlusPlus, {}, stepSubst, depth);
  if (!deref || !step || !constDefinition(deref) || !constDefinition(step))
  {
    if (constWhy().empty())
      constWhy() = "a range-for over iterators of " + types.toString(iterator, &symbols) + " without operator* and prefix operator++ the evaluator can run";
    return ConstFlow::Failed;
  }
  for (;;)
  {
    if (++constSteps() > 1000000)
      return ConstFlow::Failed;
    ConstOperand left{it, 1}, right{last, 1};
    std::optional<ConstValue> a = constRead(it), b = constRead(last);
    std::optional<ConstValue> more = a && b ? constBinaryValues(P::NotEq, *a, *b, ctx, depth + 1, &left, &right) : std::nullopt;
    std::optional<bool> go = more ? constTruth(*more, depth + 1) : std::nullopt;
    if (!go)
      return ConstFlow::Failed;
    if (!*go)
      break;
    size_t bodyMark = env.size();
    std::optional<ConstPlace> element = constCallPlace(deref, derefSubst, {}, it, depth + 1); // `*it` refers to the element
    if (declared && declared->isReference() && element)
    {
      ConstPlace resolved = constResolve(*element);
      ConstBinding alias{name, ConstValue::unset(declared)};
      alias.type = element->type;
      alias.writable = !(declared->inner && declared->inner->isConst) && !resolved.readOnly;
      alias.aliasEnv = resolved.env;
      alias.aliasIndex = resolved.index;
      alias.aliasPath = resolved.path;
      env.push_back(std::move(alias));
    }
    else
    {
      std::optional<ConstValue> value;
      if (element)
        value = constRead(*element);
      else
      {
        std::optional<ConstValue> self = constRead(it);
        value = self ? constCallOn(deref, derefSubst, {}, &self, depth + 1, &it) : std::nullopt; // `*it` by value
      }
      if (!value || value->isUnset())
        return ConstFlow::Failed;
      const Ty* own = declared && !containsAuto(declared) ? declared : value->type;
      if (own && !own->isReference() && value->isObject() && value->type && types.unqualified(types.stripRef(value->type)) == types.unqualified(own) &&
          constCopyRunsCode(own))
        value = constCopyConstruct(own, *value, element, false, depth + 1);
      else if (declared && !containsAuto(declared))
        value = constConvertTo(value, declared, depth + 1);
      if (!value)
        return ConstFlow::Failed;
      env.push_back(constBinding(name, *value, declared));
      env.back().owns = !(declared && declared->isReference());
      if (declared && declared->isReference())
        env.back().writable = false;
    }
    if (name == constHiddenSymbol && !constBindNames(static_cast<VariableDecl*>(r->var), env, env.size() - 1, ctx, depth))
      return ConstFlow::Failed; // `for (const auto& [key, value] : map)`: the names in the element
    ConstFlow flow = constStatement(r->body, ctx, env, result, depth + 1);
    if (!constEndLocals(env, bodyMark, depth, flow != ConstFlow::Failed))
      return ConstFlow::Failed;
    if (flow == ConstFlow::Returned || flow == ConstFlow::Failed)
      return flow;
    if (flow == ConstFlow::Break)
      break;
    std::optional<ConstValue> self = constRead(it);
    if (!self || !constCallOn(step, stepSubst, {}, &self, depth + 1, &it)) // `++it`, where it is
      return ConstFlow::Failed;
  }
  return ConstFlow::Next;
}

auto GlobalScope::Impl::constRangeFor(RangeForStmt* r, const TypeContext& ctx, std::vector<ConstBinding>& env, std::optional<ConstValue>& result, int depth) -> ConstFlow
{
  if (!r->var || r->var->kind != NodeKind::VariableDecl || !r->range || !r->body)
    return ConstFlow::Failed;
  auto* v = static_cast<VariableDecl*>(r->var);
  bool structured = !v->structuredBindings.empty(); // `for (const auto& [key, value] : map)`: names in each element
  if (!structured && v->name.parts.size() != 1)
    return ConstFlow::Failed;
  uint32_t name = structured ? constHiddenSymbol : v->name.parts[0].symbol;
  std::optional<ConstPlace> range = constPlaceOf(r->range, ctx, depth + 1, true);
  if (!range)
  {
    std::optional<ConstValue> value = constValue(r->range, ctx, depth + 1);
    if (!value || !value->isObject())
      return ConstFlow::Failed;
    env.push_back(constBinding(constRangeSymbol, *value, value->type)); // the temporary the loop holds
    Expr* source = nullptr;
    env.back().owns = constCategory(r->range, source) == 0;
    range = ConstPlace{&env, env.size() - 1, {}, value->type};
  }
  std::optional<ConstValue> held = constRead(*range);
  if (!held || !held->isObject())
    return ConstFlow::Failed;
  const Ty* ht = held->type ? types.unqualified(held->type) : nullptr;
  std::shared_ptr<const std::vector<ConstValue>> pointed; // the array begin() and end() point into
  std::optional<ConstValue> first;                        // or the element of the evaluation's own begin() points at
  size_t from = 0, to = 0;
  bool places = ht && ht->kind == TyKind::Array;
  const Ty* declared = v->type ? convertType(v->type, ctx, depth + 1) : nullptr;
  if (places)
    to = held->parts->size();
  else if (!ht || ht->isBraceList())
  {
    pointed = held->parts;
    to = pointed->size();
  }
  else if (ht->kind == TyKind::Class)
  {
    bool write = declared && declared->isReference() && declared->inner && !declared->inner->isConst;
    std::optional<ConstValue> b = constRangeEnd(*held, symBegin, depth + 1, &*range, write);
    std::optional<ConstValue> e = constRangeEnd(*held, symEnd, depth + 1, &*range, write);
    bool empty = b && e && b->kind == ConstValue::Kind::Int && e->kind == ConstValue::Kind::Int && b->i == 0 && e->i == 0; // two nulls
    if (b && e && b->isObject() && e->isObject() && b->type && types.unqualified(b->type)->kind == TyKind::Class)
      return constRangeForIterators(r, ctx, env, result, depth, *b, *e, declared, name); // a Deque's, a Map's iterators
    if (b && e && b->isAddress() && e->isAddress() && b->target->storage)
    {
      // Into an array of the evaluation's own (a local Array's): the elements there, which a
      // reference loop variable refers to.
      const ConstTarget &bt = *b->target, &et = *e->target;
      if (bt.storage != et.storage || bt.path.empty() || bt.path.size() != et.path.size() ||
          !std::equal(bt.path.begin(), bt.path.end() - 1, et.path.begin()) || bt.path.back() > et.path.back())
        return ConstFlow::Failed;
      first = b;
      from = bt.path.back();
      to = et.path.back();
    }
    else if (!empty && (!b || !e || !b->isPointer() || !e->isPointer() || b->parts != e->parts || b->i > e->i))
      return ConstFlow::Failed;
    else if (!empty)
    {
      pointed = b->parts;
      from = size_t(b->i);
      to = size_t(e->i);
    }
  }
  else
    return ConstFlow::Failed;
  for (size_t i = from; i < to; ++i)
  {
    size_t bodyMark = env.size();
    ConstPlace at = *range;
    if (places)
    {
      at.path.push_back(i);
      at.type = ht->inner;
      at.bits = 0;
    }
    if (first)
    {
      std::optional<ConstValue> moved = constAddressMove(*first, int64_t(i - from), depth);
      std::optional<ConstPlace> element = moved ? constStoragePlace(*moved, "a use of", depth) : std::nullopt;
      if (!element)
        return ConstFlow::Failed;
      at = *element;
    }
    if (declared && declared->isReference() && (places || first))
    {
      ConstPlace resolved = constResolve(at);
      ConstBinding alias{name, ConstValue::unset(declared)};
      alias.type = first ? at.type : ht->inner;
      alias.writable = !(declared->inner && declared->inner->isConst) && !resolved.readOnly;
      alias.aliasEnv = at.env;
      alias.aliasIndex = at.index;
      alias.aliasPath = at.path;
      env.push_back(std::move(alias));
    }
    else
    {
      std::optional<ConstValue> element = places || first ? constRead(at) : std::optional<ConstValue>((*pointed)[i]);
      if (!element || element->isUnset())
        return ConstFlow::Failed;
      const Ty* own = declared && !containsAuto(declared) ? declared : element->type;
      std::optional<ConstValue> value;
      if (own && !own->isReference() && element->isObject() && element->type &&
          types.unqualified(types.stripRef(element->type)) == types.unqualified(own) && constCopyRunsCode(own))
        value = constCopyConstruct(own, *element, places || first ? std::optional<ConstPlace>(at) : std::nullopt, false, depth + 1); // `for (String s : names)`: copied
      else
        value = declared && !containsAuto(declared) ? constConvertTo(element, declared, depth + 1) : element;
      if (!value)
        return ConstFlow::Failed;
      env.push_back(constBinding(name, *value, declared));
      env.back().owns = !(declared && declared->isReference());
      if (declared && declared->isReference())
        env.back().writable = false; // what begin() points at: a copy, which may only be read
    }
    if (structured && !constBindNames(v, env, env.size() - 1, ctx, depth))
      return ConstFlow::Failed;
    ConstFlow flow = constStatement(r->body, ctx, env, result, depth + 1);
    if (!constEndLocals(env, bodyMark, depth, flow != ConstFlow::Failed))
      return ConstFlow::Failed;
    if (flow == ConstFlow::Returned || flow == ConstFlow::Failed)
      return flow;
    if (!places && pointed && ht && ht->kind == TyKind::Class)
    {
      std::optional<ConstValue> now = constRead(*range);
      if (!now || now->parts != held->parts)
        return ConstFlow::Failed; // the object iterated was changed: its pointers read the old array
    }
    if (flow == ConstFlow::Break)
      break;
  }
  return ConstFlow::Next;
}

auto GlobalScope::Impl::constDeclare(VariableDecl* v, const TypeContext& ctx, std::vector<ConstBinding>& env, int depth) -> ConstFlow
{
  bool structured = !v->structuredBindings.empty();
  if ((!structured && v->name.parts.size() != 1) || (v->flags & (DF_Static | DF_Typedef)))
    return ConstFlow::Failed;
  // `auto [a, b] = e;`: the hidden object bound as a variable is, then its names (constBindNames).
  uint32_t symbol = structured ? constHiddenSymbol : v->name.parts[0].symbol;
  auto named = [&](size_t at) {
    return !structured || constBindNames(v, env, at, ctx, depth) ? ConstFlow::Next : ConstFlow::Failed;
  };
  const Ty* type = v->type ? constDeducedType(v, convertType(v->type, ctx, depth + 1)) : nullptr;
  if (structured && type && !type->isReference() && v->init && v->init->ty && types.unqualified(types.stripRef(v->init->ty))->kind == TyKind::Array)
  {
    // `auto [a, b] = arr;`: the hidden object a copy of the array ([dcl.struct.bind] 1), its
    // elements copied by their copy constructors (moved from an xvalue), cv from the declaration;
    // as a value the array decayed to its first element's address, no object to bind names into.
    Expr* source = nullptr;
    uint8_t category = constCategory(v->init, source);
    std::optional<ConstPlace> at = constPlaceOf(category == 2 && source ? source : v->init, ctx, depth + 1);
    std::optional<ConstValue> whole = at ? constRead(*at) : std::nullopt;
    const Ty* array = whole && whole->isObject() && whole->type && types.unqualified(whole->type)->kind == TyKind::Array
                    ? types.qualified(types.unqualified(whole->type), type->isConst, type->isVolatile) : nullptr;
    std::optional<ConstValue> copy = array ? constCopyConstruct(array, *whole, at, category == 2, depth + 1) : std::nullopt;
    if (!copy)
      return ConstFlow::Failed;
    env.push_back(constBinding(symbol, *copy, array));
    env.back().owns = true;
    return named(env.size() - 1);
  }
  if (type && type->isReference())
  {
    // `T& r = x;`: a reference to x's place; `const T& r = f();` and `T&& r = f();` keep the
    // temporary.
    std::optional<ConstValue> ran;
    constCallPlaceRan = false;
    std::optional<ConstPlace> at = v->init ? constPlaceOf(v->init, ctx, depth + 1, true, &ran) : std::nullopt;
    if (!at && ran)
    {
      // A call's reference result to a constant (`const int& r = next(c);`, next returning an
      // element of a global): its value, the call run once.
      std::optional<ConstValue> value = constConvertTo(ran, type, depth + 1);
      if (!value)
        return ConstFlow::Failed;
      env.push_back(constBinding(symbol, *value, type));
      return named(env.size() - 1);
    }
    if (!at && constCallPlaceRan)
    {
      if (constWhy().empty())
        constWhy() = "a reference to the result of a call that refers to no object the evaluation holds";
      return ConstFlow::Failed; // it ran: never twice
    }
    if (at)
    {
      ConstBinding alias{symbol, ConstValue::unset(type)};
      alias.type = at->type;
      alias.writable = !(type->inner && type->inner->isConst) && !at->readOnly;
      alias.aliasEnv = at->env;
      alias.aliasIndex = at->index;
      alias.aliasPath = at->path;
      env.push_back(std::move(alias));
      return named(env.size() - 1);
    }
    if (!((type->inner && type->inner->isConst) || type->kind == TyKind::RRef) || !v->init)
      return ConstFlow::Failed; // a reference to no place the run holds
    std::optional<ConstValue> value = constInitialize(type, v->init, ctx, depth + 1);
    if (!value)
      return ConstFlow::Failed;
    env.push_back(constBinding(symbol, *value, type));
    Expr* source = nullptr;
    env.back().owns = constCategory(v->init, source) == 0; // `const T& r = make();`: the temporary lives as r
    if (env.back().owns && type->kind == TyKind::RRef && !(type->inner && type->inner->isConst))
      env.back().writable = true; // `String&& t = String("de");`: the temporary is t's to change (`move(t)` empties it), as a parameter's is
    return named(env.size() - 1);
  }
  if (type && types.unqualified(type)->kind == TyKind::Array && !types.unqualified(type)->count && !v->init)
    return ConstFlow::Failed;
  if (type && types.unqualified(type)->kind == TyKind::Class)
  {
    // `List<int> l;`, `List<int> l{1, 2}`, `List<int> b = a;`: made where it lives (the target of
    // the constructor that makes it), so that a constructor pointing the object at itself (List's
    // sentinel) points at the local, which outlives the constructor. (Pushed before its initializer
    // is evaluated: its name is the local from its declarator on, as C++ has it.)
    env.push_back(constBinding(symbol, ConstValue::unset(types.unqualified(type)), type));
    size_t at = env.size() - 1;
    bool writable = env[at].writable;
    env[at].writable = true; // (a const one's constructor makes it too)
    env[at].owns = true;     // (the local's object: `this` addressable while the constructor runs)
    ConstPlace target{&env, at, {}, types.unqualified(type)};
    std::optional<ConstValue> made = v->init ? constInitialize(type, v->init, ctx, depth + 1, &target) : constEmpty(type, false, depth + 1, &target);
    if (!made)
      return ConstFlow::Failed;
    env[at].value = *made;
    env[at].writable = writable;
    return named(at);
  }
  // `T x;`: default-initialized (a scalar unset until assigned, a class from its defaults).
  std::optional<ConstValue> value = v->init ? constInitialize(type, v->init, ctx, depth + 1) : type ? constEmpty(type, false, depth + 1) : std::nullopt;
  if (!value)
    return ConstFlow::Failed;
  env.push_back(constBinding(symbol, *value, type));
  env.back().owns = true;
  return named(env.size() - 1);
}

auto GlobalScope::Impl::constBindNames(VariableDecl* v, std::vector<ConstBinding>& env, size_t hidden, const TypeContext& ctx, int depth) -> bool
{
  if (hidden >= env.size())
    return false;
  env[hidden].symbol = constHiddenSymbol;
  env[hidden].hiddenOf = v;
  ConstPlace object = constResolve(ConstPlace{&env, hidden, {}, env[hidden].type});
  bool readOnly = object.readOnly || !env[hidden].writable;
  std::optional<ConstValue> held = constRead(object);
  if (!held || !held->isObject() || !held->type || held->type->isBraceList())
    return false;
  const Ty* whole = types.unqualified(types.stripRef(held->type));
  // A class's names are its data members in declaration order, those of the class declaring them
  // (the object's, or the one base that does: Walker::bindingType's rule).
  std::vector<Decl*> members;
  if (whole->kind == TyKind::Class)
    if (const Ty* holder = bindingMembersClass(whole))
      if (ClassDecl* def = classDefinition(holder))
        for (Decl* m : membersRead(def))
          if (m->kind == NodeKind::VariableDecl && !(m->flags & (DF_Static | DF_Typedef)))
            members.push_back(m);
  for (size_t i = 0; i < v->structuredBindings.size(); ++i)
  {
    auto* b = static_cast<NameExpr*>(v->structuredBindings[i]);
    if (b->name.parts.size() != 1)
      return false;
    if (Decl* own = b->resolved; own && own->kind == NodeKind::VariableDecl && static_cast<VariableDecl*>(own)->tupleBinding)
    {
      // A tuple-like class's: `auto&& name = get<i>(object)`.
      if (constDeclare(static_cast<VariableDecl*>(own), ctx, env, depth) != ConstFlow::Next)
        return false;
      continue;
    }
    std::vector<size_t> path;
    const Ty* part = nullptr;
    if (whole->kind == TyKind::Array)
    {
      if (i >= whole->count)
        return false;
      path = {i};
      part = whole->inner;
    }
    else
    {
      std::optional<std::vector<size_t>> at = i < members.size() ? constFieldPath(whole, members[i], depth + 1) : std::nullopt;
      if (!at)
        return false;
      path = *at;
      part = constPartType(whole, path, path.size(), depth + 1);
    }
    if (!part)
      return false;
    ConstPlace member = object;
    member.path.insert(member.path.end(), path.begin(), path.end());
    member.type = part;
    member.bits = 0;
    bool referenceMember = part->isReference();
    std::optional<ConstPlace> named = constThroughReference(member, depth); // (a reference member: the object it refers to)
    part = types.stripRef(part);
    if (!named)
    {
      // A reference member referring to a global: its value, read only.
      std::optional<ConstValue> held = constRead(member);
      std::optional<ConstValue> value = held && held->isAddress() ? constReadTarget(*held, depth + 1) : std::nullopt;
      if (!value)
        return false;
      env.push_back(constBinding(b->name.parts[0].symbol, *value, part));
      env.back().writable = false;
      continue;
    }
    ConstPlace at = constResolve(*named);
    ConstBinding alias{b->name.parts[0].symbol, ConstValue::unset(part)};
    alias.type = part;
    alias.writable = !(readOnly && !referenceMember) && !at.readOnly && !part->isConst;
    alias.aliasEnv = at.env;
    alias.aliasIndex = at.index;
    alias.aliasPath = at.path;
    env.push_back(std::move(alias));
  }
  return true;
}

auto GlobalScope::Impl::constStatement(Stmt* s, const TypeContext& ctx, std::vector<ConstBinding>& env, std::optional<ConstValue>& result, int depth) -> ConstFlow
{
  if (!s)
    return ConstFlow::Next;
  // A statement of its own scope ends its locals; one of expressions (and a range-for, whose
  // range lives through the loop) ends its full expression's temporaries.
  bool scope = s->kind == NodeKind::BlockStmt || s->kind == NodeKind::IfStmt || s->kind == NodeKind::ForStmt || s->kind == NodeKind::WhileStmt ||
               s->kind == NodeKind::DoStmt || s->kind == NodeKind::SwitchStmt || s->kind == NodeKind::RangeForStmt;
  size_t mark = env.size();
  std::optional<ConstTemps> temps;
  if (!scope || s->kind == NodeKind::RangeForStmt)
    temps.emplace();
  ConstFlow flow = constStatementIn(s, ctx, env, result, depth);
  if (scope && !constEndLocals(env, mark, depth, flow != ConstFlow::Failed))
    flow = ConstFlow::Failed;
  if (temps && !constEndTemps(*temps, depth, flow != ConstFlow::Failed))
    flow = ConstFlow::Failed;
  if (flow == ConstFlow::Failed && !constFailedAt().file)
    constFailedAt() = ConstWhere{constRunningFile(), s->token}; // the innermost statement that failed
  return flow;
}

auto GlobalScope::Impl::constStatementIn(Stmt* s, const TypeContext& ctx, std::vector<ConstBinding>& env, std::optional<ConstValue>& result, int depth) -> ConstFlow
{
  if (depth > 512 || (depth > 64 && !stackRoomLeft()))
    return ConstFlow::Failed;
  if (++constSteps() > 1000000)
  {
    if (constWhy().empty())
      constWhy() = "an evaluation of more than a million steps";
    return ConstFlow::Failed;
  }
  // (The locals a statement declares end with its scope: constStatement.)
  auto condition = [&](Node* c) -> std::optional<bool> {
    if (!c || uint8_t(c->kind) < uint8_t(NodeKind::NameExpr))
      return std::nullopt; // a declaration as the condition
    ConstTemps temps; // a full expression of its own
    std::optional<ConstValue> v = constValue(static_cast<Expr*>(c), ctx, depth + 1);
    std::optional<bool> truth = v ? constTruth(*v, depth + 1) : std::nullopt;
    return constEndTemps(temps, depth, truth.has_value()) ? truth : std::nullopt;
  };
  switch (s->kind)
  {
    case NodeKind::BlockStmt:
    {
      for (Stmt* inner : static_cast<BlockStmt*>(s)->stmts)
        if (ConstFlow flow = constStatement(inner, ctx, env, result, depth + 1); flow != ConstFlow::Next)
          return flow;
      return ConstFlow::Next;
    }
    case NodeKind::ReturnStmt:
      if (!static_cast<ReturnStmt*>(s)->value)
      {
        result = ConstValue::integer(0, types.voidType()); // `return;` from a void function
        return ConstFlow::Returned;
      }
      if (ctx.returnPlace)
      {
        // A reference result asked for as a place (`result[i] = x`): what the expression
        // designates, which must outlive the call (a part of the object it runs on).
        Expr* value = static_cast<ReturnStmt*>(s)->value;
        std::optional<ConstValue> ran;
        std::optional<ConstPlace> at = constPlaceOf(value, ctx, depth + 1, true, &ran);
        if (!at)
        {
          // No place the run holds: a constant's (a global's, a part of one), which no evaluation
          // changes, given as its value, which the caller takes so rather than run the call again
          // (`return table[c];` after `++c` ran twice: language/consteval_reference_call_runs_once).
          if (!ran && constPure(value) && constDesignatesConstant(value, ctx, depth + 1))
            ran = constValue(value, ctx, depth + 1);
          if (!ran)
            return ConstFlow::Failed;
          result = ran;
          return ConstFlow::Returned;
        }
        ConstPlace resolved = constResolve(*at);
        resolved.type = at->type;
        resolved.bits = at->bits;
        if (resolved.env == ctx.env)
        {
          // A reference parameter holding its argument's value (`max(1, 2)`'s): that value, which
          // the caller's full expression holds; a local of the call ends with it.
          if (resolved.index >= ctx.env->size() || !(*ctx.env)[resolved.index].referenceToTemporary)
            return ConstFlow::Failed;
          result = constRead(resolved);
          return result ? ConstFlow::Returned : ConstFlow::Failed;
        }
        *ctx.returnPlace = resolved;
        return ConstFlow::Returned;
      }
      if (ctx.namedReturn && constOwnLocal(static_cast<ReturnStmt*>(s)->value, ctx))
      {
        // `return v;` of the named return variable (NRVO, guaranteed: kovarex, 2026-11-04): v is the
        // result, made where it is, with no copy or move constructor run (a class without them
        // returns so too), and not destroyed here: its destructor runs with the result's. An address
        // of v the evaluation took (its elements' while it was filled) still names v's binding,
        // which ends here: a use of it is no constant (constLiveStorage, constAddressArith), where
        // the program's would be the result.
        Expr* named = static_cast<ReturnStmt*>(s)->value;
        while (named->kind == NodeKind::ParenExpr)
          named = static_cast<ParenExpr*>(named)->inner;
        auto* n = static_cast<NameExpr*>(named);
        if (n->resolved == ctx.namedReturn)
          for (size_t i = ctx.env->size(); i-- > 0;)
            if ((*ctx.env)[i].symbol == n->name.parts[0].symbol)
            {
              ConstBinding& b = (*ctx.env)[i];
              result = b.value;
              b.owns = false;
              return ConstFlow::Returned;
            }
      }
      if (ctx.returnType && !ctx.returnType->isReference() && constOwnLocal(static_cast<ReturnStmt*>(s)->value, ctx))
        if (std::optional<std::optional<ConstValue>> moved = constCopyInit(static_cast<ReturnStmt*>(s)->value, ctx.returnType, ctx, depth + 1, true))
        {
          // `return c;` of a by-value parameter, or of a local that is no named return variable,
          // moves it ([class.copy.elision]/3).
          result = *moved;
          return result ? ConstFlow::Returned : ConstFlow::Failed;
        }
      result = ctx.returnType ? constInitialize(ctx.returnType, static_cast<ReturnStmt*>(s)->value, ctx, depth + 1)
                              : constValue(static_cast<ReturnStmt*>(s)->value, ctx, depth + 1);
      return result ? ConstFlow::Returned : ConstFlow::Failed;
    case NodeKind::ExprStmt:
    {
      // `total += i;`, `++count;`, `check(x);`: run for what it changes; an object it makes is a
      // temporary, destroyed with the statement.
      Expr* x = static_cast<ExprStmt*>(s)->expr;
      if (!x)
        return ConstFlow::Next;
      std::optional<ConstValue> v = constValue(x, ctx, depth + 1);
      if (!v)
        return ConstFlow::Failed;
      Expr* source = nullptr;
      if (v->isObject() && v->type && constDestroyRunsCode(v->type) && constCategory(x, source) == 0)
        constMaterialize(*v, v->type);
      return ConstFlow::Next;
    }
    case NodeKind::IfStmt:
    {
      auto* i = static_cast<IfStmt*>(s);
      if (i->ifConsteval) // `if consteval`: a compile-time evaluation's branch
        return constStatement(i->ifConsteval == 1 ? i->then : i->else_, ctx, env, result, depth + 1);
      if (i->init || (i->cond && i->cond->kind == NodeKind::DeclStmt))
      {
        // `if (init; cond)` and `if (T x = e)`: their variables live through both branches.
        size_t mark = env.size();
        if (i->init && constStatement(i->init, ctx, env, result, depth + 1) != ConstFlow::Next)
          return ConstFlow::Failed;
        std::optional<bool> cond;
        if (i->cond && i->cond->kind == NodeKind::DeclStmt)
        {
          size_t before = env.size();
          if (constStatement(static_cast<DeclStmt*>(i->cond), ctx, env, result, depth + 1) != ConstFlow::Next || env.size() != before + 1)
            return ConstFlow::Failed;
          const ConstBinding& b = env.back();
          if (b.aliasEnv)
            return ConstFlow::Failed; // (a reference declared as the condition)
          cond = constTruth(b.value, depth + 1);
        }
        else
          cond = condition(i->cond);
        if (!cond)
          return ConstFlow::Failed;
        ConstFlow flow = constStatement(*cond ? i->then : i->else_, ctx, env, result, depth + 1);
        if (flow == ConstFlow::Failed || !constEndLocals(env, mark, depth))
          return ConstFlow::Failed;
        return flow;
      }
      std::optional<bool> cond = condition(i->cond);
      if (!cond)
        return ConstFlow::Failed;
      return constStatement(*cond ? i->then : i->else_, ctx, env, result, depth + 1);
    }
    case NodeKind::ForStmt:
    case NodeKind::WhileStmt:
    case NodeKind::DoStmt:
    {
      Stmt* body = nullptr;
      Node* cond = nullptr;
      Expr* step = nullptr;
      bool testFirst = true;
      if (s->kind == NodeKind::ForStmt)
      {
        auto* f = static_cast<ForStmt*>(s);
        if (f->init && constStatement(f->init, ctx, env, result, depth + 1) != ConstFlow::Next)
          return ConstFlow::Failed;
        body = f->body;
        cond = f->cond;
        step = f->step;
      }
      else if (s->kind == NodeKind::WhileStmt)
      {
        body = static_cast<WhileStmt*>(s)->body;
        cond = static_cast<WhileStmt*>(s)->cond;
      }
      else
      {
        body = static_cast<DoStmt*>(s)->body;
        cond = static_cast<DoStmt*>(s)->cond;
        testFirst = false;
      }
      for (bool first = true;; first = false)
      {
        if (cond && (testFirst || !first))
        {
          std::optional<bool> go = condition(cond);
          if (!go)
            return ConstFlow::Failed;
          if (!*go)
            break;
        }
        size_t bodyMark = env.size();
        ConstFlow flow = constStatement(body, ctx, env, result, depth + 1);
        if (!constEndLocals(env, bodyMark, depth, flow != ConstFlow::Failed))
          return ConstFlow::Failed;
        if (flow == ConstFlow::Break)
          break;
        if (flow == ConstFlow::Returned || flow == ConstFlow::Failed)
          return flow;
        if (step)
        {
          ConstTemps temps;
          if (!constValue(step, ctx, depth + 1) || !constEndTemps(temps, depth))
            return ConstFlow::Failed;
        }
        if (!cond && !testFirst)
          break; // `do ... while` with no condition: not C++
      }
      return ConstFlow::Next;
    }
    case NodeKind::SwitchStmt:
    {
      // The statements from the label the value picks (default when none does) to a break, the
      // labels on the way falling through.
      auto* sw = static_cast<SwitchStmt*>(s);
      if (sw->init && constStatement(sw->init, ctx, env, result, depth + 1) != ConstFlow::Next)
        return ConstFlow::Failed;
      if (!sw->cond || uint8_t(sw->cond->kind) < uint8_t(NodeKind::NameExpr) || !sw->body || sw->body->kind != NodeKind::BlockStmt ||
          hasNestedLabel(sw->body))
        return ConstFlow::Failed;
      std::optional<ConstValue> value;
      {
        ConstTemps temps;
        value = constValue(static_cast<Expr*>(sw->cond), ctx, depth + 1);
        if (!constEndTemps(temps, depth, value.has_value()))
          return ConstFlow::Failed;
      }
      if (!value || value->isFloat())
        return ConstFlow::Failed;
      Span<Stmt*> stmts = static_cast<BlockStmt*>(sw->body)->stmts;
      size_t start = stmts.size(), fallback = stmts.size();
      for (size_t i = 0; i < stmts.size() && start == stmts.size(); ++i)
      {
        if (stmts[i] && stmts[i]->kind == NodeKind::DefaultStmt)
          fallback = i;
        if (!stmts[i] || stmts[i]->kind != NodeKind::CaseStmt)
          continue;
        auto* label = static_cast<CaseStmt*>(stmts[i]);
        if (label->rangeEnd)
          return ConstFlow::Failed; // `case 1 ... 3:`
        std::optional<ConstValue> v = constValue(label->value, ctx, depth + 1);
        std::optional<ConstValue> same = v ? constArith().binary(P::EqEq, *value, *v) : std::nullopt;
        if (!same)
          return ConstFlow::Failed;
        if (same->truthy())
          start = i;
      }
      if (start == stmts.size())
        start = fallback;
      for (size_t i = start; i < stmts.size(); ++i)
      {
        ConstFlow flow = constStatement(stmts[i], ctx, env, result, depth + 1);
        if (flow == ConstFlow::Break)
          break;
        if (flow != ConstFlow::Next)
          return flow; // a return, a failure, or a continue of the loop around the switch
      }
      return ConstFlow::Next;
    }
    case NodeKind::RangeForStmt:
    {
      return constRangeFor(static_cast<RangeForStmt*>(s), ctx, env, result, depth);
    }
    case NodeKind::BreakStmt: return ConstFlow::Break;
    case NodeKind::ContinueStmt: return ConstFlow::Continue;
    case NodeKind::CaseStmt:
    case NodeKind::DefaultStmt:
    case NodeKind::FallthroughStmt:
      return ConstFlow::Next; // labels, met while the statements after one run
    case NodeKind::DeclStmt:
      for (Decl* d : static_cast<DeclStmt*>(s)->decls)
      {
        if (d->kind == NodeKind::StaticAssertDecl || d->kind == NodeKind::UsingAliasDecl || d->kind == NodeKind::EmptyDecl ||
            d->kind == NodeKind::UsingDirectiveDecl || d->kind == NodeKind::UsingNameDecl) // (names the walk bound where written)
          continue;
        if (d->kind != NodeKind::VariableDecl || constDeclare(static_cast<VariableDecl*>(d), ctx, env, depth) != ConstFlow::Next)
          return ConstFlow::Failed;
      }
      return ConstFlow::Next;
    case NodeKind::AssertStmt:
    {
      // A failing assert stops a constant evaluation, as C++'s does; where config::debug is false an
      // assert is nothing (a release_assert and a test's check stay).
      if (auto* a = static_cast<AssertStmt*>(s); !a->release && !a->testKind && !debug)
        return ConstFlow::Next;
      std::optional<ConstValue> cond = constValue(static_cast<AssertStmt*>(s)->cond, ctx, depth + 1);
      std::optional<bool> holds = cond ? constTruth(*cond, depth + 1) : std::nullopt;
      return holds && *holds ? ConstFlow::Next : ConstFlow::Failed;
    }
    case NodeKind::EmptyStmt:
      return ConstFlow::Next;
    default:
      return ConstFlow::Failed;
  }
}
