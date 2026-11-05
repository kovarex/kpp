// GlobalScope::Impl, the compile-time evaluator (its declarations in Interpreter.hpp, part of GlobalScopeImpl.hpp; design.md,
// "Compile-time evaluation: one interpreter"): a translation unit of its own since 2026-11-05 (resolver.cpp split, stage 2), moved without a change.
#include "GlobalScopeImpl.hpp"

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
  if (const ConstHooks* h = constHooks())
    return ConstArith{types, h->mode, &constWhy(), h->underlying ? h->underlying : [this](const Ty* e) { return declaredUnderlying(e); }};
  return ConstArith{types, ConstMode::Required, &constWhy(), [this](const Ty* e) { return declaredUnderlying(e); }};
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
    case TyKind::Function: case TyKind::Lambda: case TyKind::Void: case TyKind::Vector:
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
      if (m->dynamicMember || m->member.parts.size() != 1)
        return std::nullopt;
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
      if (noValue && t && t->kind == TyKind::Class)
      {
        if (std::optional<ConstValue> b = traitObjectValue(t, depth + 1))
          return b; // `HasSize<T>{}`: a trait object as bool
        return constEmpty(t, true, depth + 1);
      }
      if (t && t->kind == TyKind::Class)
      {
        // `Point(1, 2)`, `Point{1, 2}`: the constructor's object, or an aggregate's.
        if (f->braced || (f->args.size() == 1 && f->args[0] && f->args[0]->kind == NodeKind::BraceInitExpr))
        {
          BraceInitExpr list{};
          list.kind = NodeKind::BraceInitExpr;
          list.token = f->token;
          list.args = f->args.size() == 1 && f->args[0]->kind == NodeKind::BraceInitExpr ? static_cast<BraceInitExpr*>(f->args[0])->args : f->args;
          return constBraceInit(types.unqualified(t), &list, ctx, depth + 1);
        }
        std::vector<ConstValue> values;
        if (!constArguments(f->args, ctx, depth, values))
          return std::nullopt;
        return constConstruct(types.unqualified(t), values, false, depth + 1, values.size() == f->args.size() ? &f->args : nullptr);
      }
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
    return std::nullopt; // `int` read as a name (`v < int > ...`): no part to look up
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
    // `fill(v.emplace_back())`: a member function's non-const reference result run once for its
    // place, which a reference parameter then refers to (the new element, not a copy of it).
    std::optional<ConstPlace> called;
    Expr* bare = a;
    while (bare && bare->kind == NodeKind::ParenExpr)
      bare = static_cast<ParenExpr*>(bare)->inner;
    if (places && bare && bare->kind == NodeKind::CallExpr && bare->ty && bare->ty->kind == TyKind::LRef && bare->ty->inner &&
        !bare->ty->inner->isConst && static_cast<CallExpr*>(bare)->callee &&
        (static_cast<CallExpr*>(bare)->callee->kind == NodeKind::MemberExpr || static_cast<CallExpr*>(bare)->callee->kind == NodeKind::NameExpr))
    {
      constCallPlaceRan = false;
      called = constPlaceOf(bare, ctx, depth + 1, true);
      if (!called && constCallPlaceRan)
        return false; // it ran, and its result is no place the run holds: never run twice
    }
    std::optional<ConstValue> v = called ? constPartOrUnset(*called) : constValue(a, ctx, depth + 1);
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

auto GlobalScope::Impl::constBindTemplate(FunctionDecl* f, const std::vector<const Ty*>& given, CallExpr* c, const std::vector<ConstValue>& values, const TypeContext& ctx, int depth, Subst& bound, LocalStack& params) -> bool
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
    if (!t && param->isType && !param->isPack && param->defaultArg)
    {
      // Neither written nor deduced: its default (`template<typename Unused = void>`), which may
      // name the parameters before it.
      TypeContext dctx{lexicalScopeOf(f), &params, bound.empty() ? nullptr : &bound, f->file ? f->file->tokens : nullptr,
                       f->file ? f->file->text : std::string_view()};
      t = convertType(static_cast<Type*>(param->defaultArg), dctx, depth + 1);
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
    constCallPlaceRan = true;
  }
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
    return constCallLambda(static_cast<LambdaExpr*>(called), c, ctx, depth); // `[]() -> Shape { ... }()`
  if (c->callee->kind != NodeKind::NameExpr)
  {
    // `ops[0](5)`, `(*f)(5)`: through the function pointer the callee evaluates to.
    std::optional<ConstValue> fn = constValue(c->callee, ctx, depth + 1);
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
      std::vector<ConstValue> values;
      if (!constArguments(c->args, ctx, depth, values))
        return std::nullopt;
      return constConstruct(to, values, false, depth + 1, values.size() == c->args.size() ? &c->args : nullptr);
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
    std::vector<ConstValue> values;
    if (!constArguments(c->args, ctx, depth, values))
      return std::nullopt;
    return constConstruct(t, values, false, depth + 1, values.size() == c->args.size() ? &c->args : nullptr);
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
    if (std::string_view name = symbols.name(f->name.last().symbol); name == "memcpy" || name == "memmove" || name == "memset" || name == "memcmp")
    {
      std::vector<ConstValue> values;
      if (!constArguments(c->args, ctx, depth, values))
        return std::nullopt;
      return name == "memset"   ? constMemset(values[0], values[1], values[2], depth)
           : name == "memcmp"   ? constMemcmp(values[0], values[1], values[2], depth)
                                : constMemcpy(values[0], values[1], values[2], depth);
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
  if (!constBindTemplate(f, given, c, values, ctx, depth, bound, params))
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
  if (returnPlace)
    return object ? constRun(f, values, fctx, depth, &object, &places, &categories, &*self, returnPlace) : std::nullopt;
  std::optional<ConstValue> r = object ? constRun(f, values, fctx, depth, &object, &places, &categories) : std::nullopt;
  if (r && object && self->writable() && !(f->flags & DF_Const))
    constWrite(*self, *object);
  return r;
}

auto GlobalScope::Impl::constMemberCall(CallExpr* c, const TypeContext& ctx, int depth) -> std::optional<ConstValue>
{
  auto* m = static_cast<MemberExpr*>(c->callee);
  if (!m->base || m->dynamicMember || m->member.parts.size() != 1 || m->member.parts[0].hasTemplateArgs)
    return std::nullopt;
  if (m->member.parts[0].special == 1 && c->args.empty())
  {
    // `slots[i].~Text()`, `p->~T()`: the object destroyed where it is, its lifetime ended there
    // (what is left is no value: reading it is no constant).
    std::optional<ConstPlace> place;
    if (m->op == P::Dot)
      place = constPlaceOf(m->base, ctx, depth + 1);
    else if (std::optional<ConstValue> p = constValue(m->base, ctx, depth + 1))
      place = constStoragePlace(*p, "a destruction of", depth);
    std::optional<ConstValue> value = place ? constRead(*place) : std::nullopt;
    if (!value || !constDestroy(value->type ? value->type : place->type, *value, depth + 1) || !constWrite(*place, ConstValue::unset(value->type)))
      return std::nullopt;
    return ConstValue::integer(0, types.voidType());
  }
  const Ty* object = constObjectType(m->base, ctx, m->op == P::Arrow);
  Scope* s = object ? classScopeOf(object) : nullptr;
  Lookup l;
  if (!s || !lookupInScope(s, m->member.parts[0].symbol, l, depth + 1))
    return std::nullopt;
  if (l.decl && l.decl->kind == NodeKind::VariableDecl && !(l.decl->flags & (DF_Static | DF_Typedef)))
    return constCallObject(m, c, ctx, depth, l.decl); // `this->hasher(key)`: a member object's operator()
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
  Subst bound = memberSubst(object, l.foundIn, depth + 1);
  LocalStack params;
  for (const Subst::Pair& p : bound.pairs)
    params.push(static_cast<TemplateParamDecl*>(p.param)->name, p.param);
  for (Decl* p : effectiveParams(f->parent && f->parent->kind == NodeKind::ClassDecl ? f->parent : nullptr))
    if (!bound.find(p))
      return std::nullopt;
  if (!f->templateParams.empty() && !constBindTemplate(f, {}, c, values, ctx, depth, bound, params))
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
  if (!place && m->op == P::Arrow && constPointsIntoStorage(m->base))
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
  for (Scope* place : places)
    for (Scope* s = place; s; s = s->parent)
    {
      if (s->kind == ScopeKind::Class)
        continue; // members are no free functions, and do not hide them
      Lookup look;
      if (!lookupInScope(s, symbol, look, 0))
        continue;
      if (FunctionDecl* f = choose(friends.decl && look.decl && look.decl->kind == NodeKind::FunctionDecl ? mergeOverloads(look, friends) : look))
        return f;
      break;
    }
  return friends.decl ? choose(friends) : nullptr;
}

auto GlobalScope::Impl::constCallPlace(FunctionDecl* f, const Subst& subst, const std::vector<ConstValue>& args, const ConstPlace& self, int depth, const std::vector<std::optional<ConstPlace>>* places, const std::vector<uint8_t>* categories) -> std::optional<ConstPlace>
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
  if (!object || !constRun(f, args, fctx, depth, &object, places, categories, &self, &out) || !out)
    return std::nullopt;
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
  for (const Ty* cls = t; cls;)
  {
    Scope* s = classScopeOf(cls);
    if (!s || !s->owner || s->owner->kind != NodeKind::ClassDecl)
      return std::nullopt;
    Subst subst = substFor(cls);
    for (Decl* m : membersRead(static_cast<ClassDecl*>(s->owner)))
    {
      if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Conversion) || !constDefinition(static_cast<FunctionDecl*>(m)))
        continue;
      const Ty* ft = declaredTypeOf(m, subst.empty() ? nullptr : &subst, 0);
      const Ty* to = ft && ft->kind == TyKind::Function && ft->inner ? types.unqualified(types.stripRef(ft->inner)) : nullptr;
      if (!to || !(to->isArithmetic() || to->kind == TyKind::Enum))
        continue;
      std::optional<ConstValue> self = v;
      std::vector<size_t> base;
      if (cls != t)
      {
        std::optional<std::vector<size_t>> path = constBasePath(t, cls, depth);
        std::optional<ConstValue> part = path ? constPart(v, *path) : std::nullopt;
        if (!part)
          return std::nullopt;
        self = part;
      }
      std::optional<ConstValue> r = constCallOn(static_cast<FunctionDecl*>(m), subst, {}, &self, depth + 1);
      return r && r->isScalar() ? std::optional<bool>(r->truthy()) : std::nullopt;
    }
    ClassDecl* def = classDefinition(cls);
    std::vector<const Ty*> bases;
    if (!def || !directBases(cls, def, bases) || bases.size() != 1)
      return std::nullopt;
    cls = bases[0];
  }
  return std::nullopt;
}

auto GlobalScope::Impl::constClassOperator(const Ty* t, P op, const std::vector<const Ty*>& argTypes, Subst& subst, int depth) -> FunctionDecl*
{
  std::vector<FunctionDecl*> candidates = constMembersNamed(t, opSymbols[uint32_t(op)], subst, depth);
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
  if (castTo)
  {
    std::optional<ConstValue> r = constValue(chain.back()->rhs, ctx, depth + 1);
    value = r ? constConverted(constArith().unary(P(chain.back()->op), *r), castTo) : std::nullopt;
    chain.pop_back();
  }
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
    std::optional<ConstValue> r = constValue(chain[i]->rhs, ctx, depth + 1);
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
        left = constOperandOf(first, *value, ctx, depth);
      else if (returned)
        left.place = returned; // the previous operator's reference result: the object it refers to (`out << a << b`)
      else if (value->isObject() && value->type && types.unqualified(types.stripRef(value->type))->kind == TyKind::Class)
      {
        left.category = 0; // the previous operator's result
        left.place = constMaterialize(*value, value->type);
      }
      right = constOperandOf(chain[i]->rhs, *r, ctx, depth);
    }
    returned.reset();
    value = *op == P::Comma ? r : constBinaryValues(*op, *value, *r, ctx, depth, &left, &right, &returned);
    failed = !value;
  }
  if (place && !failed)
    *place = returned;
  return failed ? std::nullopt : value;
}

auto GlobalScope::Impl::constBinaryValues(P op, const ConstValue& l, const ConstValue& r, const TypeContext& ctx, int depth, const ConstOperand* lo, const ConstOperand* ro, std::optional<ConstPlace>* returned) -> std::optional<ConstValue>
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
  // `this->items + N`: an array an operator meets is a pointer to its first element.
  auto decayed = [this](const ConstValue& v) -> std::optional<ConstValue> {
    const Ty* t = v.isObject() && v.type ? types.unqualified(types.stripRef(v.type)) : nullptr;
    return t && t->kind == TyKind::Array && t->inner ? constPointerTo(v, types.pointer(t->inner)) : std::nullopt;
  };
  if (!objects && (op == P::Plus || op == P::Minus || op == P::EqEq || op == P::NotEq))
    if (std::optional<ConstValue> a = decayed(l), b = decayed(r); a || b)
      return constPointerArith(op, a ? *a : l, b ? *b : r);
  if (!l.isObject() && !r.isObject())
    return constArith().binary(op, l, r);
  auto boolean = [this](bool b) { return ConstValue::integer(b, types.boolType()); };
  bool comparison = op == P::EqEq || op == P::NotEq || op == P::Less || op == P::Greater || op == P::LessEq || op == P::GreaterEq;
  // The class's own operator, run on the left object; then a free one. With the operands' places
  // (ao, bo): run where they are.
  auto direct = [&](P o, const ConstValue& a, const ConstValue& b, const ConstOperand* ao = nullptr,
                    const ConstOperand* bo = nullptr) -> std::optional<std::optional<ConstValue>> {
    if (a.isObject() && a.type && types.unqualified(a.type)->kind == TyKind::Class)
    {
      Subst subst;
      if (FunctionDecl* f = constClassOperator(a.type, o, {constOperandType(b, bo)}, subst, depth))
      {
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
    if (FunctionDecl* f = constFreeFunction(opSymbols[uint32_t(o)], {constOperandType(a, ao), constOperandType(b, bo)}, ctx.scope))
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
  return ConstValue::object(types.array(types.qualified(ch, true, false), uint32_t(chars.size())), std::move(chars));
}

auto GlobalScope::Impl::constEmpty(const Ty* t, bool zero, int depth) -> std::optional<ConstValue>
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
  for (FunctionDecl* c : layout->constructors)
    if (c->params.empty() && !(c->flags & (DF_Default | DF_Delete)) && !c->builtin)
      return constRunConstructor(c, t, {}, depth + 1); // a user-provided default constructor (none visible: no constant)
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
  for (size_t i = 0; i < layout->fields.size(); ++i)
  {
    std::optional<ConstValue> part = constFieldDefault(*layout, i, zero, depth);
    if (!part)
      return std::nullopt;
    parts.push_back(*part);
  }
  constFitBitFields(*layout, parts);
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

auto GlobalScope::Impl::constCopyConstruct(const Ty* t, const ConstValue& v, const std::optional<ConstPlace>& at, bool moving, int depth) -> std::optional<ConstValue>
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
        part->path.push_back(i);
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
    return constRunConstructor(ctor, t, {v}, depth + 1, nullptr, nullptr, &places);
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
      part->path.push_back(i);
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

auto GlobalScope::Impl::constCopyInit(Expr* init, const Ty* t, const TypeContext& ctx, int depth, bool moving) -> std::optional<std::optional<ConstValue>>
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
    return std::nullopt; // (another class: its converting constructor, as before)
  std::optional<ConstPlace> at = constPlaceOf(source, ctx, depth + 1);
  return std::optional<std::optional<ConstValue>>(constCopyConstruct(t, *v, at, moving, depth + 1));
}

auto GlobalScope::Impl::constInitialize(const Ty* t, Expr* init, const TypeContext& ctx, int depth) -> std::optional<ConstValue>
{
  return constOutermost([&] { return constInitializeIn(t, init, ctx, depth); }, depth);
}

auto GlobalScope::Impl::constInitializeIn(const Ty* t, Expr* init, const TypeContext& ctx, int depth) -> std::optional<ConstValue>
{
  const Ty* u = t ? types.unqualified(types.stripRef(t)) : nullptr;
  if (!init || depth > 512)
    return std::nullopt;
  if (u && (u->kind == TyKind::Class || u->kind == TyKind::Array))
  {
    if (init->kind == NodeKind::BraceInitExpr)
    {
      auto* b = static_cast<BraceInitExpr*>(init);
      return u->kind == TyKind::Array ? constArrayInit(u, b->args, ctx, depth) : constBraceInit(u, b, ctx, depth);
    }
    if (!t->isReference())
      if (std::optional<std::optional<ConstValue>> copied = constCopyInit(init, u, ctx, depth))
        return *copied;
    if (std::optional<ConstValue> chars = constCharArrayFromLiteral(u, init, ctx, depth))
      return chars;
    return constConvertTo(constValue(init, ctx, depth + 1), u, depth);
  }
  return constAsDeclared(constInitValue(init, ctx, depth), t);
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

auto GlobalScope::Impl::constBraceInit(const Ty* t, BraceInitExpr* b, const TypeContext& ctx, int depth) -> std::optional<ConstValue>
{
  bool expands = false;
  for (Expr* a : b->args)
    expands = expands || constIsExpansion(a);
  if (expands)
  {
    // `Point{args...}`: the values the expansion gives, as a brace list's (no brace elision).
    std::optional<ConstValue> list = constBraceList(b, ctx, depth);
    return list ? constFromBraceList(*list, t, depth) : std::nullopt;
  }
  if (constIsAggregate(t, depth))
  {
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
  // constructor, or the constructor the elements choose.
  std::optional<ConstValue> list = constBraceList(b, ctx, depth);
  return list ? constFromBraceList(*list, t, depth, &b->args) : std::nullopt;
}

auto GlobalScope::Impl::constConvertTo(std::optional<ConstValue> v, const Ty* t, int depth) -> std::optional<ConstValue>
{
  if (!v || !t)
    return v;
  if (constIsBraceList(*v))
    return constFromBraceList(*v, t, depth);
  const Ty* u = types.unqualified(types.stripRef(t));
  if (u->kind != TyKind::Class)
    return constAsDeclared(v, t);
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

auto GlobalScope::Impl::constConstruct(const Ty* t, const std::vector<ConstValue>& args, bool implicitOnly, int depth, const Span<Expr*>* written) -> std::optional<ConstValue>
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
  std::vector<const Ty*> chosenTypes = argTypes; // the argument types the choice was made with
  bool ambiguous = false;
  const Ty* inheritedFrom = nullptr;
  FunctionDecl* ctor = chooseConstructor(t, argTypes, nullptr, &inheritedFrom, implicitOnly, &ambiguous);
  if (ambiguous && written && written->size() == args.size())
  {
    std::vector<const Ty*> typed = argTypes;
    for (size_t i = 0; i < typed.size(); ++i)
      if (Expr* a = (*written)[i]; a && a->ty && a->ty->isKnown() && !types.isDependent(a->ty) && !mentionsUnknown(a->ty))
        typed[i] = argumentType(a, a->ty, types);
      else if (a && a->kind == NodeKind::BraceInitExpr)
        if (const Ty* list = braceListType(a, initializerListDecl(), types))
          typed[i] = list;
    ctor = chooseConstructor(t, typed, nullptr, &inheritedFrom, implicitOnly, &ambiguous);
    chosenTypes = typed;
  }
  if (!ctor || ambiguous || (ctor->flags & DF_Delete))
    return std::nullopt; // (a deleted one: no constant; codegen's call of it is an error)
  if (!inheritedFrom)
  {
    if (ctor->templateParams.empty())
      return constRunConstructor(ctor, t, args, depth + 1);
    // `template<class T1, class T2> GamePair(T1&& key, T2&& value)`: its parameters as the
    // arguments deduce them, with the class's.
    Subst own = layout->subst;
    deduceCall(ctor, declaredTypeOf(ctor, layout->subst.empty() ? nullptr : &layout->subst, 0), chosenTypes, own);
    if (!fillDefaultTemplateArgs(ctor, own))
      return std::nullopt;
    for (Decl* p : ctor->templateParams)
      if (const Ty* bound = own.find(p); !bound || !bound->isKnown() || types.isDependent(bound))
        return std::nullopt;
    if (implicitOnly && ctor->explicitCondition && isExplicit(ctor, &own))
      return std::nullopt; // `explicit(!isConvertible<U, T>)` true for these arguments: no implicit conversion
    return constRunConstructor(ctor, t, args, depth + 1, &own);
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

auto GlobalScope::Impl::constBraceList(BraceInitExpr* b, const TypeContext& ctx, int depth) -> std::optional<ConstValue>
{
  if (!b->designators.empty() || !constMayMake(b->args.size()))
    return std::nullopt;
  std::vector<ConstValue> elements;
  if (!constArguments(b->args, ctx, depth, elements))
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

auto GlobalScope::Impl::constFromBraceList(const ConstValue& list, const Ty* t, int depth, const Span<Expr*>* written) -> std::optional<ConstValue>
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
    if (std::optional<ConstValue> v = constByInitializerList(u, elements, depth))
      return v;
    return constConstruct(u, elements, false, depth + 1, written);
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
    ConstValue backing = ConstValue::object(types.array(pt->inner, uint32_t(converted.size())), std::move(converted));
    items = ConstValue::pointer(pt, backing.parts, 0);
  }
  return ConstValue::object(u, {items, *count});
}

auto GlobalScope::Impl::constByInitializerList(const Ty* u, const std::vector<ConstValue>& elements, int depth) -> std::optional<ConstValue>
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
    return list ? constRunConstructor(c, u, {*list}, depth + 1) : std::nullopt;
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
    return std::nullopt;
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
    bool same = l.kind == r.kind && *l.target == *r.target;
    if (equality)
      return boolean(op == P::EqEq ? same : !same);
    const ConstTarget &a = *l.target, &b = *r.target;
    bool siblings = l.isAddress() && r.isAddress() && a.decl == b.decl && a.storage == b.storage && a.owner == b.owner && a.args == b.args && !a.path.empty() &&
                    a.path.size() == b.path.size() && std::equal(a.path.begin(), a.path.end() - 1, b.path.begin());
    if (!siblings)
      return std::nullopt; // addresses of different objects: no order
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
  return std::nullopt;
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
    bool ok = ++constCalls() <= 512 && stackRoomLeft();
    ~Call() { --constCalls(); }
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

auto GlobalScope::Impl::constRunConstructor(FunctionDecl* ctor, const Ty* t, const std::vector<ConstValue>& args, int depth, const Subst* own, FunctionDecl* declared, const std::vector<std::optional<ConstPlace>>* places) -> std::optional<ConstValue>
{
  struct Call
  {
    bool ok = ++constCalls() <= 512 && stackRoomLeft();
    ~Call() { --constCalls(); }
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
    return constRunConstructor(def, t, args, depth, own, ctor, places);
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
    const Ty* type = p->type ? convertType(p->type, fctx, depth + 1) : nullptr;
    if (type && type->isReference() && places && i < places->size() && (*places)[i])
    {
      // `Text(Text&& other)` given the source's place: a reference to it (a move empties it).
      const ConstPlace& at = *(*places)[i];
      ConstBinding alias{p->name, ConstValue::unset(type)};
      alias.type = at.type;
      alias.writable = !(type->inner && type->inner->isConst) && !at.readOnly;
      alias.aliasEnv = at.env;
      alias.aliasIndex = at.index;
      alias.aliasPath = at.path;
      env.push_back(std::move(alias));
      continue;
    }
    std::optional<ConstValue> v = i < args.size() ? constConvertTo(args[i], type, depth + 1)
                                : defaultValue ? constInitialize(type, defaultValue, fctx, depth + 1) : std::nullopt;
    if (!v)
      return std::nullopt;
    env.push_back(constBinding(p->name, *v, type));
  }
  // The members' initial values: unset, filled below in declaration order.
  size_t nb = layout->bases.size(), nf = layout->fields.size();
  std::vector<std::optional<ConstValue>> parts(nb + nf);
  for (const FunctionDecl::CtorInit& init : ctor->ctorInits)
  {
    if (absentMemberInit(layout->decl, init))
      continue; // (design.md 19)
    if (init.member.empty() || init.isPack)
      return std::nullopt;
    uint32_t symbol = init.member.last().symbol;
    // The values the initializer gives: one expression, a brace list, none (value-initialized).
    auto initialize = [&](const Ty* target) -> std::optional<ConstValue> {
      if (init.args.empty())
        return constEmpty(target, true, depth + 1);
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
        if (tu->kind == TyKind::Class)
          return values.size() == 1 && values[0].isObject() && values[0].type && types.unqualified(types.stripRef(values[0].type)) == tu
                   ? constCopyConstruct(tu, values[0], places[0], categories[0] != 1, depth + 1) // copied, or moved from where it is
                   : constConstruct(tu, values, false, depth + 1);
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
          if (std::optional<std::optional<ConstValue>> copied = constCopyInit(init.args[0], tu, fctx, depth + 1))
            return *copied;
          std::optional<ConstValue> v = constValue(init.args[0], fctx, depth + 1);
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
    bool found = false;
    for (size_t i = 0; i < nf && !found; ++i)
      if (layout->fields[i] && !layout->fields[i]->name.empty() && layout->fields[i]->name.last().symbol == symbol)
      {
        parts[nb + i] = initialize(layout->fieldTypes[i]);
        if (!parts[nb + i])
          return std::nullopt;
        found = true;
      }
    for (size_t b = 0; b < nb && !found; ++b)
      if (layout->bases[b]->decl && !static_cast<ClassDecl*>(layout->bases[b]->decl)->name.empty() &&
          static_cast<ClassDecl*>(layout->bases[b]->decl)->name.last().symbol == symbol)
      {
        parts[b] = initialize(layout->bases[b]);
        if (!parts[b])
          return std::nullopt;
        found = true;
      }
    if (!found && !layout->decl->name.empty() && symbol == layout->decl->name.last().symbol && ctor->ctorInits.size() == 1)
    {
      // `Frame(int x) : Frame(x, 0) {}`: the delegated-to constructor makes the object.
      std::optional<ConstValue> whole = initialize(t);
      if (!whole)
        return std::nullopt;
      return constRunBody(ctor, t, *whole, fctx, env, depth);
    }
    if (!found)
      return std::nullopt; // a base named through an alias, a member the layout does not hold
  }
  std::vector<ConstValue> object;
  if (!constMayMake(nb + nf))
    return std::nullopt;
  for (size_t b = 0; b < nb; ++b)
  {
    std::optional<ConstValue> part = parts[b] ? parts[b] : constEmpty(layout->bases[b], false, depth + 1);
    if (!part)
      return std::nullopt;
    object.push_back(*part);
  }
  for (size_t i = 0; i < nf; ++i)
  {
    std::optional<ConstValue> part = parts[nb + i] ? parts[nb + i] : constFieldDefault(*layout, i, false, depth);
    if (!part)
      return std::nullopt;
    object.push_back(*part);
  }
  constFitBitFields(*layout, object);
  return constRunBody(ctor, t, ConstValue::object(t, std::move(object)), fctx, env, depth);
}

auto GlobalScope::Impl::constRunBody(FunctionDecl* ctor, const Ty* t, const ConstValue& object, TypeContext& fctx, std::vector<ConstBinding>& env, int depth) -> std::optional<ConstValue>
{
  env.push_back(constBinding(constThisSymbol, object, t));
  size_t self = env.size() - 1;
  std::optional<ConstValue> result;
  ranBody(ctor);
  ConstFlow flow = constStatement(ctor->body, fctx, env, result, depth + 1);
  if (flow != ConstFlow::Next && flow != ConstFlow::Returned)
    return std::nullopt;
  if (self >= env.size() || env[self].symbol != constThisSymbol)
    return std::nullopt;
  return env[self].value;
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
  return s.symbol && s.symbol != constThisSymbol ? std::string(symbols.name(s.symbol)) : !s.symbol ? std::string("a temporary") : std::string("an object of the evaluation");
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
    bool typed = constHeap()[*index].type != nullptr;
    if (p.target->path != (typed ? std::vector<size_t>{0} : std::vector<size_t>{}))
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
    std::optional<ConstValue> e = a.zero ? constEmpty(t, true, depth + 1) : std::optional<ConstValue>(ConstValue::unset(t));
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

auto GlobalScope::Impl::constMemcmp(const ConstValue& a, const ConstValue& b, const ConstValue& count, int depth) -> std::optional<ConstValue>
{
  if (count.kind != ConstValue::Kind::Int || count.i < 0)
    return std::nullopt;
  auto pointer = [&](const ConstValue& p) -> std::optional<ConstValue> {
    if (p.isObject() && p.type && types.unqualified(p.type)->kind == TyKind::Array && types.unqualified(p.type)->inner)
      return ConstValue::pointer(types.pointer(types.unqualified(p.type)->inner), p.parts, 0); // an array given as it is
    return p.isPointer() || p.isAddress() ? std::optional<ConstValue>(p) : std::nullopt;
  };
  std::optional<ConstValue> pa = pointer(a), pb = pointer(b);
  if (!pa || !pb)
    return std::nullopt;
  for (int64_t i = 0; i < count.i; ++i)
  {
    std::optional<ConstValue> x = pa->isAddress() ? constAddressMove(*pa, i, depth) : constPointerArith(P::Plus, *pa, ConstValue::integer(i, types.intType()));
    std::optional<ConstValue> y = pb->isAddress() ? constAddressMove(*pb, i, depth) : constPointerArith(P::Plus, *pb, ConstValue::integer(i, types.intType()));
    std::optional<ConstValue> cx = x ? (x->isAddress() ? constReadTarget(*x, depth + 1) : constDeref(*x)) : std::nullopt;
    std::optional<ConstValue> cy = y ? (y->isAddress() ? constReadTarget(*y, depth + 1) : constDeref(*y)) : std::nullopt;
    if (!cx || !cy || cx->kind != ConstValue::Kind::Int || cy->kind != ConstValue::Kind::Int)
      return std::nullopt;
    std::optional<int64_t> wx = cx->type ? constSizeOf(cx->type, depth) : std::nullopt, wy = cy->type ? constSizeOf(cy->type, depth) : std::nullopt;
    if (!wx || !wy || *wx != 1 || *wy != 1)
    {
      if (constWhy().empty())
        constWhy() = "a memcmp of elements wider than a byte";
      return std::nullopt;
    }
    int ux = int(uint8_t(cx->i)), uy = int(uint8_t(cy->i));
    if (ux != uy)
      return ConstValue::integer(ux < uy ? -1 : 1, types.intType());
  }
  return ConstValue::integer(0, types.intType());
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

auto GlobalScope::Impl::constDestroy(const Ty* t, const ConstValue& v, int depth) -> bool
{
  t = t ? types.unqualified(types.stripRef(t)) : nullptr;
  if (!constDestroyRunsCode(t))
    return true;
  auto cannot = [&](const char* what) {
    if (constWhy().empty())
      constWhy() = std::string(what) + " " + types.toString(t, &symbols);
    return false;
  };
  if (!triviallyDestructible(t) || depth > 64 || !v.isObject() || !v.parts)
    return cannot("a destruction the evaluator cannot run: of");
  if (t->kind == TyKind::Array)
  {
    for (size_t i = v.parts->size(); i-- > 0;)
      if (!constDestroy(t->inner, (*v.parts)[i], depth + 1))
        return false;
    return true;
  }
  ClassDecl* def = t->kind == TyKind::Class ? classDefinition(t) : nullptr;
  const ConstClassLayout* layout = def ? constLayoutOf(t, depth) : nullptr;
  if (!def || !layout)
    return cannot("a destruction the evaluator cannot run: of");
  if (def->isTypedUnion)
    return cannot("the destruction of a typed union in a constant evaluation is not supported yet:");
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
    if (!constCallOn(dtor, layout->subst, {}, &self, depth + 1) || !self || !self->isObject())
      return false;
    object = *self;
  }
  if (def->keyword == Kw::union_)
    return true; // its destructor's body chose what to destroy
  size_t nb = layout->bases.size(), nf = layout->fields.size();
  if (object.parts->size() != nb + nf)
    return cannot("a destruction the evaluator cannot run: of");
  for (size_t i = nf; i-- > 0;)
    if (!constDestroy(layout->fieldTypes[i], (*object.parts)[nb + i], depth + 1))
      return false;
  for (size_t b = nb; b-- > 0;)
    if (!constDestroy(layout->bases[b], (*object.parts)[b], depth + 1))
      return false;
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
      ok = v.isUnset() || constDestroy(t, v, depth + 1);
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
               : constConstruct(u, values, false, depth + 1);
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
    if (std::optional<std::optional<ConstValue>> copied = constCopyInit(n->args[0], u, ctx, depth))
      return *copied;
    std::optional<ConstValue> v = constValue(n->args[0], ctx, depth + 1);
    if (!v)
      return std::nullopt;
    if (v->isObject() && v->type && types.unqualified(types.stripRef(v->type)) == u)
      return constConvertTo(v, u, depth + 1);
    return constConstruct(u, {*v}, false, depth + 1);
  }
  std::vector<ConstValue> values;
  if (!constArguments(n->args, ctx, depth, values))
    return std::nullopt;
  return constConstruct(u, values, false, depth + 1);
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
  if (a.array != d->isArray || p->target->path != (a.array ? std::vector<size_t>{0} : std::vector<size_t>{}))
    return fail(std::string(d->isArray ? "delete[]" : "delete") + " of memory " + (a.array ? "new[]" : "new") + " allocated at " +
                constWhereText(a.allocated) + (p->target->path.size() > (a.array ? 1u : 0u) ? ", through a pointer into it" : ""));
  const Ty* pointee = p->type && types.unqualified(types.stripRef(p->type))->kind == TyKind::Pointer ? types.unqualified(types.unqualified(types.stripRef(p->type))->inner) : nullptr;
  const Ty* object = constHeap()[index].type;
  if (!pointee || types.unqualified(a.array ? object->inner : object) != pointee)
    return fail("a delete through a pointer of another type than the object's is not supported in a constant evaluation yet");
  ConstValue value = constHeap()[index].value;
  if (!value.isUnset() && !constDestroy(object, value, depth + 1))
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
  std::optional<ConstPlace> place = constPure(e) ? constPlaceOf(e, ctx, depth + 1) : std::nullopt;
  if (!place)
    place = constMaterialize(v, v.type);
  std::optional<ConstValue> self = place ? constRead(*place) : std::optional<ConstValue>(v);
  return constCallOn(arrow, subst, {}, &self, depth + 1, place ? &*place : nullptr);
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
  if (!self || !self->isObject() || !self->type || self->type->isBraceList() || types.unqualified(types.stripRef(self->type))->kind != TyKind::Class)
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
  FunctionDecl* call = constClassOperator(self->type, P::LParen, argTypes, subst, depth);
  if (!call || !constDefinition(call))
    return std::nullopt;
  return constCallOn(call, subst, values, &self, depth + 1, place ? &*place : nullptr, &places, &categories);
}

auto GlobalScope::Impl::constPlaceOf(Expr* e, const TypeContext& ctx, int depth, bool calls) -> std::optional<ConstPlace>
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
        return self;
      }
    }
    return std::nullopt;
  }
  if (e->kind == NodeKind::UnaryExpr && P(static_cast<UnaryExpr*>(e)->op) == P::Star && !static_cast<UnaryExpr*>(e)->isKeyword &&
      static_cast<UnaryExpr*>(e)->operand && static_cast<UnaryExpr*>(e)->operand->kind == NodeKind::ThisExpr)
    return constThisPlace(ctx); // `*this`
  if (e->kind == NodeKind::UnaryExpr && P(static_cast<UnaryExpr*>(e)->op) == P::Star && !static_cast<UnaryExpr*>(e)->isKeyword &&
      !static_cast<UnaryExpr*>(e)->postfix && static_cast<UnaryExpr*>(e)->operand &&
      (constPointsIntoStorage(static_cast<UnaryExpr*>(e)->operand) || (calls && !constFrames().empty())))
  {
    // (`*this->slotAt(i)` where a place is needed (a reference result): the call evaluated once.)
    // `*p` with p pointing at an object of the evaluation.
    std::optional<ConstValue> p = constValue(static_cast<UnaryExpr*>(e)->operand, ctx, depth + 1);
    if (p && p->isAddress() && p->target->storage)
      return constStoragePlace(*p, "a use of", depth);
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
        else if (std::optional<ConstValue> q = constArrowOf(m->base, *p, ctx, depth); q && q->isAddress() && q->target->storage)
          base = constStoragePlace(*q, "a use of", depth); // `at->second`: through the iterator's operator->
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
    return base;
  }
  if (e->kind == NodeKind::IndexExpr)
  {
    auto* x = static_cast<IndexExpr*>(e);
    if (x->args.size() != 1)
      return std::nullopt;
    if (constPointsIntoStorage(x->base))
    {
      // `p[i]`, p pointing into an array of the evaluation.
      std::optional<ConstValue> p = constValue(x->base, ctx, depth + 1);
      if (p && p->isAddress() && p->target->storage)
      {
        std::optional<ConstValue> index = constValue(x->args[0], ctx, depth + 1);
        if (!index || index->kind != ConstValue::Kind::Int)
          return std::nullopt;
        std::optional<ConstValue> moved = constAddressMove(*p, index->i, depth);
        return moved ? constStoragePlace(*moved, "a use of", depth) : std::nullopt;
      }
    }
    std::optional<ConstPlace> base = constPlaceOf(x->base, ctx, depth + 1, calls);
    std::optional<ConstValue> index = base ? constValue(x->args[0], ctx, depth + 1) : std::nullopt;
    if (!base || !index || index->kind != ConstValue::Kind::Int)
      return std::nullopt;
    std::optional<ConstValue> array = constRead(*base);
    const Ty* at = array && array->type ? types.unqualified(array->type) : nullptr;
    if (calls && array && array->isObject() && at && at->kind == TyKind::Class && !at->isBraceList())
    {
      // `result[i]` of an Array: the element its operator[] refers to.
      Subst subst;
      std::vector<const Ty*> indexType{x->args[0]->ty ? argumentType(x->args[0], x->args[0]->ty, types) : nullptr};
      FunctionDecl* f = constPlaceMember(at, opSymbols[uint32_t(P::LBracket)], 1, base->writable(), subst, depth + 1, nullptr, &indexType);
      return f && f->templateParams.empty() ? constCallPlace(f, subst, {*index}, *base, depth + 1) : std::nullopt;
    }
    if (!array || !array->isObject() || !at || at->kind != TyKind::Array || index->i < 0 || uint64_t(index->i) >= array->parts->size())
      return std::nullopt;
    base->path.push_back(size_t(index->i));
    base->type = at->inner;
    base->bits = 0;
    return base;
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
  if (e->kind == NodeKind::CallExpr && calls && e->ty && e->ty->kind == TyKind::LRef && e->ty->inner && static_cast<CallExpr*>(e)->callee &&
      static_cast<CallExpr*>(e)->callee->kind == NodeKind::NameExpr)
  {
    // `add(uses, name)`: what a named function's reference result refers to (a part of an object
    // its arguments refer to), the function run once for it.
    std::optional<ConstPlace> out;
    if (!constCall(static_cast<CallExpr*>(e), ctx, depth + 1, &out) || !out)
      return std::nullopt;
    out->type = e->ty->inner;
    out->readOnly = out->readOnly || e->ty->inner->isConst;
    return out;
  }
  if (e->kind == NodeKind::CallExpr && calls)
  {
    // `result.front()`, `this->back()`: the part of the object a member function's reference
    // result refers to.
    auto* c = static_cast<CallExpr*>(e);
    auto* m = c->callee && c->callee->kind == NodeKind::MemberExpr ? static_cast<MemberExpr*>(c->callee) : nullptr;
    if (!m || !m->base || m->dynamicMember || m->member.parts.size() != 1 || m->member.parts[0].hasTemplateArgs)
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
    if (!constArguments(c->args, ctx, depth, args, &places, &categories))
      return std::nullopt;
    LocalStack own;
    if (!f->templateParams.empty() && !constBindTemplate(f, {}, c, args, ctx, depth, subst, own))
      return std::nullopt; // `all.emplace_back().roots`: a member function template's own parameters
    return constCallPlace(f, subst, args, *base, depth + 1, &places, &categories);
  }
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
      return std::nullopt; // pointers into different arrays: no order, no difference
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
  return path ? constPart(*object, *path) : std::nullopt;
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
  std::vector<ConstValue> values;
  std::vector<const Ty*> argTypes;
  for (Expr* a : args)
  {
    std::optional<ConstValue> v = constValue(a, ctx, depth + 1);
    if (!v)
      return std::nullopt;
    values.push_back(*v);
    argTypes.push_back(constTypeOfValue(*v));
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
  return constRun(f, values, fctx, depth, &self);
}

auto GlobalScope::Impl::constRun(FunctionDecl* f, const std::vector<ConstValue>& args, TypeContext fctx, int depth, std::optional<ConstValue>* self, const std::vector<std::optional<ConstPlace>>* places, const std::vector<uint8_t>* categories, const ConstPlace* selfPlace, std::optional<ConstPlace>* returnPlace) -> std::optional<ConstValue>
{
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
    return constRun(f, packed, fctx, depth, self, &packedPlaces, &packedCategories, selfPlace, returnPlace);
  }
  // A virtual function runs as the object's own class overrides it.
  if (self && *self)
    if (FunctionDecl* g = constOverrider(f, (*self)->type, 0); g != f && constDefinition(g))
    {
      Subst subst = memberSubst(types.unqualified((*self)->type), g->parent ? g->parent->ownScope : nullptr, 0);
      return constCallOn(g, subst, args, self, depth + 1, selfPlace, places, categories, returnPlace);
    }
  struct Call
  {
    bool ok = ++constCalls() <= 512 && stackRoomLeft();
    ~Call() { --constCalls(); }
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
    // A reference refers to the argument's object when that is of its type (or of a class derived
    // from it: the base part); anything else converts, a class object it makes being a temporary.
    std::optional<std::vector<size_t>> basePart;
    if (type && type->isReference() && i < args.size() && type->inner)
    {
      const Ty* want = types.unqualified(type->inner);
      const Ty* have = places && i < places->size() && (*places)[i] && (*places)[i]->type ? types.unqualified(types.stripRef((*places)[i]->type))
                     : args[i].type                                                     ? types.unqualified(types.stripRef(args[i].type))
                                                                                        : nullptr;
      if (have && (have == want || want->kind == TyKind::Void))
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
        continue;
      }
    }
    if (type && type->isReference() && places && i < places->size() && (*places)[i] && basePart)
    {
      // `int& out`, `const Point& p` given a local: a reference to it (to its base part).
      const ConstPlace& at = *(*places)[i];
      ConstBinding alias{p->name, ConstValue::unset(type)};
      alias.type = basePart->empty() ? (*places)[i]->type : types.unqualified(type->inner);
      alias.writable = !(type->inner && type->inner->isConst) && !at.readOnly;
      alias.aliasEnv = at.env;
      alias.aliasIndex = at.index;
      alias.aliasPath = at.path;
      alias.aliasPath.insert(alias.aliasPath.end(), basePart->begin(), basePart->end());
      env.push_back(std::move(alias));
      continue;
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
      return std::nullopt;
    env.push_back(constBinding(p->name, *v, type));
    // A by-value parameter is the function's to destroy, once the evaluator knows it made it (a
    // copy, a prvalue, a default).
    env.back().owns = type && !type->isReference() && (i >= args.size() || (categories && i < categories->size()));
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
    return flow == ConstFlow::Returned && *returnPlace ? std::optional<ConstValue>(ConstValue::integer(0, types.voidType())) : std::nullopt;
  if (isVoid && (flow == ConstFlow::Next || flow == ConstFlow::Returned))
    return ConstValue::integer(0, types.voidType()); // a helper run for what it checks (asserts)
  if (flow != ConstFlow::Returned || !result)
    return std::nullopt;
  return returns ? constConvertTo(result, returns, depth + 1) : result;
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

auto GlobalScope::Impl::constAssign(BinaryExpr* b, const TypeContext& ctx, int depth) -> std::optional<ConstValue>
{
  std::optional<ConstValue> r = constValue(b->rhs, ctx, depth + 1); // the right operand first, as C++17 orders it
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
      std::optional<ConstPlace> right;
      if (category != 0 && constPure(source))
        right = constPlaceOf(source, ctx, depth + 1);
      else if (category == 0 && r->isObject() && r->type && types.unqualified(types.stripRef(r->type))->kind == TyKind::Class)
        right = constMaterialize(*r, r->type);
      std::vector<std::optional<ConstPlace>> places{right};
      std::vector<uint8_t> categories{category};
      std::optional<ConstValue> self = constRead(*place);
      if (!self)
        return std::nullopt;
      std::optional<ConstValue> result = constCallOn(f, subst, {*r}, &self, depth + 1, &*place, &places, &categories);
      if (!result || !self)
        return std::nullopt;
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
      std::optional<ConstPlace> right;
      if (category != 0 && constPure(source))
        right = constPlaceOf(source, ctx, depth + 1);
      else if (category == 0)
        right = constMaterialize(*r, r->type);
      if (!constAssignObject(*place, target, *r, right, category, depth + 1))
        return std::nullopt;
      return constRead(*place);
    }
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
  return v;
}

auto GlobalScope::Impl::constIncrement(UnaryExpr* u, bool increment, const TypeContext& ctx, int depth) -> std::optional<ConstValue>
{
  std::optional<ConstPlace> place = constPlaceOf(u->operand, ctx, depth + 1, true);
  if (!place || !place->writable())
    return std::nullopt;
  std::optional<ConstValue> old = constRead(*place);
  if (!old)
    return std::nullopt;
  ConstValue one = ConstValue::integer(1, types.intType());
  std::optional<ConstValue> v = old->isPointer()   ? constPointerArith(increment ? P::Plus : P::Minus, *old, one)
                                : old->isAddress() ? constAddressMove(*old, increment ? 1 : -1, depth)
                                                   : constArith().binary(increment ? P::Plus : P::Minus, *old, one);
  v = v ? constConvertTo(v, place->type, depth + 1) : std::nullopt;
  if (!v || !constWrite(*place, *v))
    return std::nullopt;
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
  if (v->name.parts.size() != 1 || !v->structuredBindings.empty())
    return ConstFlow::Failed;
  uint32_t name = v->name.parts[0].symbol;
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
        std::optional<ConstPlace> at = constPlaceOf(static_cast<ReturnStmt*>(s)->value, ctx, depth + 1, true);
        if (!at)
          return ConstFlow::Failed;
        ConstPlace resolved = constResolve(*at);
        resolved.type = at->type;
        resolved.bits = at->bits;
        if (resolved.env == ctx.env)
          return ConstFlow::Failed; // a reference to the call's own local
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
        if (d->kind != NodeKind::VariableDecl)
          return ConstFlow::Failed;
        auto* v = static_cast<VariableDecl*>(d);
        if (v->name.parts.size() != 1 || (v->flags & (DF_Static | DF_Typedef)) || !v->structuredBindings.empty())
          return ConstFlow::Failed;
        const Ty* type = v->type ? constDeducedType(v, convertType(v->type, ctx, depth + 1)) : nullptr;
        if (type && type->isReference())
        {
          // `T& r = x;`: a reference to x's place; `const T& r = f();` keeps the temporary.
          std::optional<ConstPlace> at = v->init ? constPlaceOf(v->init, ctx, depth + 1, true) : std::nullopt;
          if (at)
          {
            ConstBinding alias{v->name.parts[0].symbol, ConstValue::unset(type)};
            alias.type = at->type;
            alias.writable = !(type->inner && type->inner->isConst) && !at->readOnly;
            alias.aliasEnv = at->env;
            alias.aliasIndex = at->index;
            alias.aliasPath = at->path;
            env.push_back(std::move(alias));
            continue;
          }
          if (!(type->inner && type->inner->isConst) || !v->init)
            return ConstFlow::Failed; // a reference to no place the run holds
          std::optional<ConstValue> value = constInitialize(type, v->init, ctx, depth + 1);
          if (!value)
            return ConstFlow::Failed;
          env.push_back(constBinding(v->name.parts[0].symbol, *value, type));
          Expr* source = nullptr;
          env.back().owns = constCategory(v->init, source) == 0; // `const T& r = make();`: the temporary lives as r
          continue;
        }
        if (type && types.unqualified(type)->kind == TyKind::Array && !types.unqualified(type)->count && !v->init)
          return ConstFlow::Failed;
        // `T x;`: default-initialized (a scalar unset until assigned, a class from its defaults).
        std::optional<ConstValue> value = v->init ? constInitialize(type, v->init, ctx, depth + 1) : type ? constEmpty(type, false, depth + 1) : std::nullopt;
        if (!value)
          return ConstFlow::Failed;
        env.push_back(constBinding(v->name.parts[0].symbol, *value, type));
        env.back().owns = true;
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
