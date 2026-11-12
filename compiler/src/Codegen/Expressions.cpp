// struct Emitter's members: expressions: rvalues, unary and binary operators, three-way comparison;
// their declarations in Expressions.hpp, part of the struct's body in Codegen/Emitter.hpp. A
// translation unit of its own since 2026-11-05 (codegen.cpp split, stage 2), moved without a
// change.
#include "Emitter.hpp"

auto codegen::Emitter::arrowOperand(Value p, Node* at) -> Value
{
  for (int depth = 0; depth < maxArrowChain && isClass(p.ty); ++depth)
  {
    const Ty* foundIn = nullptr;
    if (!findMemberFunction(p.ty, "operator->", {}, p.ty, at, &foundIn))
      break;
    p = callRvalue(operatorCall(p, "operator->", {}, at, true), at);
  }
  return p;
}

auto codegen::Emitter::baseInstantiation(const Ty* derived, Decl* baseDecl, Node* at) -> const Ty*
{
  ClassDecl* target = classDef(baseDecl, at);
  StructInfo& info = structFor(derived, at);
  std::vector<const Ty*> candidates;
  for (auto& [baseDef, index] : info.bases)
    candidates.push_back(info.fieldTypes[index]);
  for (const Ty* b : candidates)
  {
    if (static_cast<ClassDecl*>(canonicalClass(b, at)->decl) == target)
      return canonicalClass(b, at);
    if (const Ty* deeper = baseInstantiation(b, baseDecl, at))
      return deeper;
  }
  return nullptr;
}

auto codegen::Emitter::addressedTemplateArgs(FunctionDecl* f, NameExpr* n, Expr* e, TemplateArgs& fargs) -> void
{
  const NamePart& last = n->name.last();
  size_t given = 0;
  // A trailing pack takes the rest (`getOrCreate<Expression, const NoiseExpression&, const
  // NoiseExpression&>`), and is empty when nothing is left for it.
  bool packLast = !f->templateParams.empty() && static_cast<TemplateParamDecl*>(f->templateParams[f->templateParams.size() - 1])->isPack;
  std::vector<const Ty*> packElements;
  for (size_t i = 0; i < last.templateArgs.size(); ++i)
  {
    Node* arg = last.templateArgs[i];
    TemplateArgs none;
    const Ty* t = nullptr;
    if (uint8_t(arg->kind) > uint8_t(NodeKind::PackType))
    {
      // `checkArray<16>`: a constant argument, its parameter's type.
      Decl* param = i < f->templateParams.size() ? f->templateParams[i] : nullptr;
      std::optional<int64_t> v = evalConst(static_cast<Expr*>(arg));
      if (!v)
        unsupported(e, "a constant argument the compiler could not evaluate in the address of a function template");
      t = types.valueType(valueParamType(param), *v);
    }
    else
      t = global.typeOfAstIn(static_cast<Type*>(arg), typingContext(), currentArgs ? *currentArgs : none);
    if (packLast && i + 1 >= f->templateParams.size())
    {
      if (t && t->kind == TyKind::Pack)
        packElements.insert(packElements.end(), t->args.begin(), t->args.end()); // `Args...` of the enclosing instance
      else
        packElements.push_back(t);
      continue;
    }
    if (i >= f->templateParams.size())
      break;
    fargs.pairs.push_back({f->templateParams[i], t});
    ++given;
  }
  // The checker typed the name as the instance a function pointer of a known type wants
  // (`construct<Neg>` for a `const Expr& (*)(const Expr&)`, the trailing pack deduced
  // from its parameters): the arguments not written, deduced the same way.
  const Ty* typed = e->ty ? withCurrentArgs(types.unqualified(types.stripRef(e->ty))) : nullptr;
  if (typed && (typed->kind == TyKind::Pointer || typed->kind == TyKind::MemberPointer))
    typed = typed->inner;
  if (given < f->templateParams.size() && packElements.empty() && typed && typed->kind == TyKind::Function && typed->isKnown() &&
      !types.isDependent(typed))
  {
    std::vector<const Ty*> paramTypes(typed->args.begin(), typed->args.end());
    global.deduceArgs(f, paramTypes, fargs);
    given = 0;
    for (Decl* p : f->templateParams)
      for (auto& [d, t] : fargs.pairs)
        if (d == p)
        {
          ++given;
          break;
        }
  }
  if (packLast && given + 1 == f->templateParams.size())
  {
    fargs.pairs.push_back({f->templateParams[f->templateParams.size() - 1], types.pack(Span<const Ty*>{packElements.data(), uint32_t(packElements.size())})});
    ++given;
  }
  if (given < f->templateParams.size())
    unsupported(e, "taking the address of a function template without all its arguments");
}

auto codegen::Emitter::callRvalue(Value v, Node* at) -> Value
{
  if (operatorWatch && at == operatorWatch->step)
  {
    operatorWatch->seen = true;
    operatorWatch->returned = v;
  }
  if (v.ty && v.ty->isReference())
  {
    if (isClass(types.stripRef(v.ty)))
      return {v.text, types.unqualified(types.stripRef(v.ty)), false};
    return load(v.text, v.ty, at);
  }
  return v;
}

auto codegen::Emitter::rvalue(Expr* e) -> Value
{
  Value v = rvalueOf(e);
  // (a const or volatile object as the checker typed it: the conversion operator a const or volatile object takes)
  if (v.ty && (!v.ty->isConst || !v.ty->isVolatile) && isClass(v.ty) && e->ty && e->ty->isKnown() && !types.isDependent(e->ty))
    if (const Ty* declared = types.stripRef(e->ty); ((declared->isConst && !v.ty->isConst) || (declared->isVolatile && !v.ty->isVolatile)) && isClass(declared) &&
                                                    classKey(declared) == classKey(v.ty))
      v.ty = types.qualified(v.ty, v.ty->isConst || declared->isConst, v.ty->isVolatile || declared->isVolatile);
  // (an lvalue: a name, a member, a call or cast giving T&; an rvalue: a temporary, `move(x)`, a call giving T or T&&)
  if (isClass(v.ty) && e->ty && e->ty->isKnown() && !types.isDependent(e->ty))
    v.category = isLvalueExpression(e) || e->ty->kind == TyKind::LRef ? Value::Category::Lvalue : Value::Category::Rvalue;
  return v;
}

// A fold of assignments: `((E1 op E2) op ...) op En` and `E1 op (E2 op (... op En))` ([temp.variadic] 10). An assignment
// evaluates its right operand before its left one ([expr.ass] 1), so the operands from the last one back: a left fold's right
// operands all before E1 and any of its assignments, a right fold's each assignment as its left operand is reached. Each step
// is `a op b` as binary() emits it, a class's or an enumeration's own operator as in a plain assignment. Its result is E1, the
// object (foldAssignedPlace, for lvalue), a fold of one operand that operand.
auto codegen::Emitter::foldOfAssignments(FoldExpr* f, Expr* pattern, Expr* init, bool right, Decl* typePack) -> Value
{
  std::deque<PlaceName> places;
  std::deque<NamedCastExpr> casts;
  std::deque<BinaryExpr> steps;
  const Ty* bound = typePack ? boundPack(typePack) : nullptr;
  if (typePack && !bound)
    unsupported(f, "a fold of assignments over a pack that is not bound here");
  size_t operands = (typePack ? bound->args.size() : packElementCount(pattern, f)) + (init ? 1 : 0);
  if (operands == 0)
    unsupported(f, "a fold over an empty pack without an initial value");
  auto fromTheLast = [&](auto&& visit)
  {
    if (init && right)
      visit(init);
    if (typePack)
      forEachTypePackElement(typePack, [&](size_t) { visit(pattern); }, pattern, true);
    else
      forEachPackElement(pattern, f, [&](size_t) { visit(pattern); }, true);
    if (init && !right)
      visit(init);
  };
  auto place = [&](Expr* x) -> Expr*
  {
    if (bitfieldNamed(x))
      unsupported(f, "a fold of assignments to a bit field");
    foldAssignedPlace = lvalue(x);
    foldAssignedAt = f;
    return &places.emplace_back(*this, foldAssignedPlace, types.unqualified(types.stripRef(x->ty)), f->token).name;
  };
  // A value standing as an operand: a scalar in a slot of its own, a class's object as itself, a temporary as an xvalue.
  auto standIn = [&](const Value& v) -> Expr*
  {
    const Ty* t = types.unqualified(types.stripRef(v.ty));
    std::string at = v.text;
    if (!isClass(t))
    {
      at = newObjectSlot(t, f);
      store(v, at, t, f);
    }
    PlaceName& named = places.emplace_back(*this, at, t, f->token);
    if (isClass(t) && !v.isTemporary)
      return &named.name;
    NamedCastExpr& cast = casts.emplace_back();
    cast.kind = NodeKind::NamedCastExpr;
    cast.token = f->token;
    cast.castKind = Kw::static_cast_;
    cast.operand = &named.name;
    cast.ty = isClass(t) ? types.rref(t) : t;
    return &cast;
  };
  auto assign = [&](Expr* target, const Value& value)
  {
    if (value.ty && types.stripRef(value.ty)->kind == TyKind::Void)
      unsupported(f, "a fold of assignments over an operator that gives nothing");
    BinaryExpr& step = steps.emplace_back();
    step.kind = NodeKind::BinaryExpr;
    step.token = f->token;
    step.op = f->op;
    step.lhs = target;
    step.rhs = standIn(value);
    return binary(&step);
  };
  size_t seen = 0;
  Value result{"", types.voidType()};
  if (operands == 1 && (!f->ty || f->ty->kind != TyKind::LRef)) // (one operand, a value: `(1 += ... += none)`)
  {
    fromTheLast([&](Expr* x) { result = rvalue(x); });
    return result;
  }
  if (right)
  {
    fromTheLast([&](Expr* x)
    {
      if (++seen == 1 && operands > 1)
        result = rvalue(x);
      else if (Expr* target = place(x); seen == 1)
        result = rvalue(target); // (one operand: the fold is it)
      else
        result = assign(target, result);
    });
    return result;
  }
  std::vector<Value> assigned;
  Expr* target = nullptr;
  fromTheLast([&](Expr* x)
  {
    if (++seen < operands)
      assigned.push_back(rvalue(x));
    else
      target = place(x);
  });
  if (assigned.empty())
    return rvalue(target);
  for (size_t i = assigned.size(); i-- > 0;)
  {
    result = assign(target, assigned[i]);
    if (isClass(types.stripRef(target->ty)) && i > 0) // (what a class's own operator gave takes the next one)
      target = standIn(result);
  }
  return result;
}

// A fold with an operand of a class, a closure or an enumeration ([expr.prim.fold], [temp.variadic] 10): an operator
// function may be called at any step, so each step is `a op b` as binary() emits it, the operator its operands' types
// choose (the fold's one node records no choice for each element). `((init op E0) op E1) op E2` calls each operator
// before the next element is evaluated, as `<<` sequences them; `E0 op (E1 op (E2 op init))` evaluates every element,
// then calls the innermost operator first. Each operand is what it is in C++: a step takes the expression itself where
// it can (init, and the element whose pass it is), an lvalue or an xvalue stands as the place it designates, and a
// prvalue is kept as it was made, for a by-value parameter to take. Until 2026-11-09 every element was evaluated first
// and a scalar's value copied into a slot of its own: `(a << ... << forward<Args>(args))` called operator<<(Acc&, int&&)
// for an int&, a const int& and an int&& alike (3333 where C++ gives 1323), an int& parameter was "no operator<<", and
// the elements of a `<<` fold all ran before its first operator.
auto codegen::Emitter::foldCallsOperator(FoldExpr* f, P op, const std::vector<const Ty*>& operandTypes) -> bool
{
  for (const Ty* t : operandTypes)
  {
    if (!t || !t->isKnown() || !(isClass(t) || isLambda(t) || t->kind == TyKind::Enum))
      continue;
    // (two lvalues of the type: the operator found for them, no code emitted)
    PlaceName a(*this, "", t, f->token), b(*this, "", t, f->token);
    BinaryExpr probe{};
    probe.kind = NodeKind::BinaryExpr;
    probe.token = f->token;
    probe.op = uint32_t(op);
    probe.lhs = &a.name;
    probe.rhs = &b.name;
    if (userLogicalOperator(&probe, op) || userEnumLogicalOperator(&probe, op))
      return true;
  }
  return false;
}

auto codegen::Emitter::foldOverOperators(FoldExpr* f, Expr* pattern, Expr* init, bool right, Decl* typePack) -> Value
{
  std::deque<PlaceName> places;
  std::deque<NamedCastExpr> casts;
  std::deque<ParenExpr> holders;
  std::deque<HeldExpr> held;
  std::deque<BinaryExpr> steps;
  ErrorExpr noObject{}; // (what a held value's parenthesis holds: never an lvalue, whatever the pattern it came from)
  noObject.kind = NodeKind::ErrorExpr;
  struct Forget
  {
    ~Forget()
    {
      for (const ParenExpr& p : holders)
        heldValues.erase(&p);
    }

    std::unordered_map<const ParenExpr*, HeldValue>& heldValues;
    std::deque<ParenExpr>& holders;
  } forget{heldValues, holders};
  auto place = [&](const std::string& at, const Ty* t, bool xvalue) -> Expr*
  {
    PlaceName& named = places.emplace_back(*this, at, t, f->token);
    if (!xvalue)
      return &named.name;
    NamedCastExpr& cast = casts.emplace_back();
    cast.kind = NodeKind::NamedCastExpr;
    cast.token = f->token;
    cast.castKind = Kw::static_cast_;
    cast.operand = &named.name;
    cast.ty = types.rref(t);
    return &cast;
  };
  auto value = [&](const Value& v) -> Expr*
  {
    ParenExpr& holder = holders.emplace_back();
    holder.kind = NodeKind::ParenExpr;
    holder.token = f->token;
    holder.ty = v.ty;
    holder.inner = &noObject;
    heldValues[&holder] = HeldValue{v, std::string()};
    HeldExpr& one = held.emplace_back();
    one.kind = NodeKind::HeldExpr;
    one.token = f->token;
    one.ty = v.ty;
    one.holder = &holder;
    return &one;
  };
  // An operand evaluated now that a later step takes, in its element's pass, where the pattern means that element.
  auto operand = [&](Expr* x) -> Expr*
  {
    if (!x->ty || !x->ty->isKnown())
      unsupported(f, "a fold over an operator with an operand of unknown type");
    const Ty* t = types.stripRef(x->ty);
    if (bindsBitField(x))
    {
      // (no address of its own: its value in a place of its own, const so that no T& binds it)
      const Ty* plain = types.unqualified(t);
      std::string slot = newObjectSlot(plain, x);
      store(rvalue(x), slot, plain, x);
      return place(slot, types.qualified(plain, true, false), false);
    }
    if (isLvalueExpression(x) || x->ty->kind == TyKind::LRef)
      return place(lvalue(x), t, false);
    if (isXvalue(x))
      return place(lvalue(x), t, true);
    return value(rvalue(x));
  };
  // One operand and no step, where the fold initializes an object: the operand initializes it, as it would alone.
  auto alone = [&](Expr* x) -> std::optional<Value>
  {
    if (inPlaceOperator != f || !x->ty || !x->ty->isKnown())
      return std::nullopt;
    const Ty* t = types.unqualified(types.stripRef(f->ty));
    Restore restoreOperator(inPlaceOperator, inPlaceOperator);
    std::string slot = inPlaceSlot;
    inPlaceOperator = nullptr;
    initializeFrom(slot, t, x, f);
    return Value{slot, t};
  };
  Expr* last = nullptr; // what the next step takes: init, the first element, the result of the step before
  Value made;
  auto step = [&](Expr* lhs, Expr* rhs, bool lastStep)
  {
    BinaryExpr& b = steps.emplace_back();
    b.kind = NodeKind::BinaryExpr;
    b.token = f->token;
    b.op = f->op;
    b.lhs = lhs;
    b.rhs = rhs;
    OperatorWatch watch{&b};
    {
      Restore restoreWatch(operatorWatch);
      Restore restoreOperator(inPlaceOperator);
      operatorWatch = &watch;
      if (lastStep && inPlaceOperator == f)
        inPlaceOperator = &b; // (the fold's value: built in the object the fold initializes)
      made = binary(&b);
    }
    // (an operator function's reference is the object it refers to, its const and its category kept for the next step)
    const Ty* returned = watch.seen ? watch.returned.ty : nullptr;
    last = returned && returned->isReference() ? place(watch.returned.text, types.stripRef(returned), returned->kind == TyKind::RRef) : value(made);
  };
  auto eachElement = [&](auto&& visit)
  {
    if (typePack)
      forEachTypePackElement(typePack, [&](size_t) { visit(pattern); }, pattern);
    else
      forEachPackElement(pattern, f, [&](size_t) { visit(pattern); });
  };
  size_t count = typePack ? boundPack(typePack)->args.size() : packElementCount(pattern, f);
  if (count == 0)
  {
    if (!init)
      unsupported(f, "a fold over an empty pack without an initial value");
    if (std::optional<Value> whole = alone(init))
      return *whole;
    return rvalue(init);
  }
  if (count == 1 && !init)
  {
    eachElement([&](Expr* x)
    {
      std::optional<Value> whole = alone(x);
      made = whole ? *whole : rvalue(x);
    });
    return made;
  }
  if (!right)
  {
    size_t done = 0;
    last = init;
    eachElement([&](Expr* x)
    {
      if (last)
        step(last, x, ++done + (init ? 0 : 1) == count);
      else
        last = operand(x);
    });
  }
  else
  {
    std::vector<Expr*> lefts;
    eachElement([&](Expr* x)
    {
      if (lefts.size() + 1 < count)
        lefts.push_back(operand(x));
      else if (init)
        step(x, init, lefts.empty());
      else
      {
        Expr* lhs = lefts.back();
        lefts.pop_back();
        step(lhs, x, lefts.empty());
      }
    });
    for (; !lefts.empty(); lefts.pop_back())
      step(lefts.back(), last, lefts.size() == 1);
  }
  return made;
}

auto codegen::Emitter::rvalueOf(Expr* e) -> Value
{
  if (e->kind == NodeKind::BraceInitExpr && static_cast<BraceInitExpr*>(e)->text)
    return {textTemporary(static_cast<BraceInitExpr*>(e), e), types.unqualified(types.stripRef(e->ty)), false}; // (design.md 14)
  if (++recursionDepth > maxExpressionNesting)
  {
    recursionDepth = 0;
    fail(e, "expression nested too deeply");
  }
  struct DepthGuard { ~DepthGuard() { --d; } int& d; } dg{recursionDepth};
  switch (e->kind)
  {
    case NodeKind::LiteralExpr:
    {
      auto* l = static_cast<LiteralExpr*>(e);
      const Ty* t = e->ty;
      if (l->literalKind == TokenKind::String)
        return {stringConstant(l), t};
      if (l->literalKind == TokenKind::Keyword)
        return {l->keyword == Kw::nullptr_ ? "null" : l->keyword == Kw::true_ ? "true" : "false", t};
      if (t->kind == TyKind::Float)
      {
        std::string digits;
        std::string spliced;
        // The digit separators out and the suffix (f, F, l, L) at the end, as consteval's literal
        // reads it: a hexadecimal one's f is a digit (0x1.fp3 is 15.5; every f was dropped, 0x1p3,
        // and 0xABCDEF.1p4 lost a digit).
        for (char c : literalSpelling(l, l->token, spliced))
          if (c != '\'')
            digits += c;
        while (!digits.empty() && strchr("fFlL", digits.back()))
          digits.pop_back();
        double d = strtod(digits.c_str(), nullptr);
        // LLVM wants the exact value: the bits of the double (rounded through float first for f32).
        double exact = t->bits == 32 ? double(float(d)) : d;
        uint64_t bits;
        memcpy(&bits, &exact, sizeof bits);
        char buf[32];
        snprintf(buf, sizeof buf, "0x%016llX", (unsigned long long)bits);
        return {buf, t};
      }
      auto v = evalConst(e);
      if (!v)
        unsupported(e, "this literal");
      return {std::to_string(*v), t};
    }
    case NodeKind::NameExpr:
    {
      auto* n = static_cast<NameExpr*>(e);
      Decl* d = n->resolved;
      if (!d)
        if (std::optional<int64_t> c = evalConst(e)) // a builtin trait used as a value
          return {e->ty && e->ty->kind == TyKind::Bool ? std::string(*c ? "true" : "false") : std::to_string(*c), e->ty};
      if (d && d->kind == NodeKind::EnumeratorDecl)
        return {std::to_string(enumeratorNamed(static_cast<EnumeratorDecl*>(d), e)), e->ty};
      if (d && d->kind == NodeKind::TemplateParamDecl)
      {
        // A non-type template parameter is a constant of the instantiation; a function given
        // for `float (*func)(float, float)` is that function's address.
        if (std::string object = templateParameterObject(d, e); !object.empty())
          return {object, templateParameterClass(d)};
        if (currentArgs)
          for (auto& [p, bound] : currentArgs->pairs)
          {
            if (p == d && bound && bound->kind == TyKind::Value && bound->decl && bound->decl->kind == NodeKind::FunctionDecl && bound->inner &&
                types.unqualified(types.stripRef(bound->inner))->kind == TyKind::MemberPointer)
            {
              // `template<int (A::*M)()>` given &A::f: what `&A::f` is here, a thunk for a virtual one.
              NameExpr name{};
              name.kind = NodeKind::NameExpr;
              name.token = e->token;
              name.resolved = bound->decl;
              UnaryExpr address{};
              address.kind = NodeKind::UnaryExpr;
              address.token = e->token;
              address.op = uint32_t(P::Amp);
              address.operand = &name;
              address.ty = types.unqualified(types.stripRef(bound->inner));
              return unary(&address);
            }
            if (p == d && bound && bound->kind == TyKind::Value && bound->decl && bound->decl->kind == NodeKind::FunctionDecl)
            {
              refuseCAddress(static_cast<FunctionDecl*>(bound->decl), e);
              return {useFunction(static_cast<FunctionDecl*>(bound->decl), {}, nullptr, e), bound->inner};
            }
            // `template<int* P>` given &g: the variable's address (until 2026-11-08 "a template parameter whose value is
            // not known here", in the class's members and in a function template, deduced or not).
            if (p == d && bound && bound->kind == TyKind::Value && bound->decl && bound->decl->kind == NodeKind::VariableDecl && bound->inner &&
                types.unqualified(types.stripRef(bound->inner))->kind == TyKind::Pointer)
              return {addressOfDecl(bound->decl, e), types.unqualified(types.stripRef(bound->inner))};
            if (p == d && bound && bound->kind == TyKind::Value && bound->decl && bound->decl->kind == NodeKind::VariableDecl && bound->inner &&
                types.unqualified(types.stripRef(bound->inner))->kind == TyKind::MemberPointer)
            {
              // `template<int Pos::*Field>` given &Pos::x: the field's offset, as `&Pos::x` gives it.
              const Ty* mp = types.unqualified(types.stripRef(bound->inner));
              const Ty* cls = mp->args.empty() ? nullptr : withCurrentArgs(mp->args[0]);
              std::string addr;
              if (!cls || !isClass(cls) || !fieldAddress(cls, bound->decl, "null", addr, e))
                unsupported(e, "a pointer to a member given as a template argument that is no field of its class");
              std::string r = newTemp();
              fn += "  " + r + " = ptrtoint ptr " + addr + " to " + memberOffsetType() + "\n";
              return {r, mp};
            }
          }
        // `template<double D>` given 1.25 (C++20): the exact constant its bits are (TypeTable::floatBits); of a pack of
        // them (`(Ds + ... + 0.0)`), the element bound now (forEachPackElement).
        if (currentArgs)
          for (auto& [p, bound] : currentArgs->pairs)
          {
            const Ty* one = p == d ? bound : nullptr;
            if (one && one->kind == TyKind::Pack)
            {
              const std::vector<Decl*>& element = currentArgs->elementPacks;
              bool expanding = std::find(element.begin(), element.end(), d) != element.end();
              one = expanding && currentArgs->elementIndex < one->args.size() ? one->args[currentArgs->elementIndex] : nullptr;
            }
            if (one && one->kind == TyKind::Value && !one->decl && !one->isObjectValue() && one->inner && types.unqualified(one->inner)->kind == TyKind::Float)
            {
              const Ty* ft = types.unqualified(one->inner);
              return {floatConstant(TypeTable::floatOfBits(one->value, ft->bits), ft->bits), ft};
            }
          }
        std::optional<int64_t> v = evalConst(e);
        const Ty* t = e->ty ? types.stripRef(e->ty) : nullptr;
        if (t && t->kind == TyKind::Value && t->inner)
          t = t->inner;
        // (`{M...}` in a default member initializer: typed once as the pack, one element bound now)
        if (t && t->kind == TyKind::Pack && currentArgs)
        {
          for (auto& [p, bound] : currentArgs->pairs)
            if (p == d && bound && bound->kind == TyKind::Value && bound->inner)
              t = bound->inner;
          const std::vector<Decl*>& element = currentArgs->elementPacks;
          if (t->kind == TyKind::Pack && std::find(element.begin(), element.end(), d) != element.end() && currentArgs->elementIndex < t->args.size())
          {
            const Ty* one = t->args[currentArgs->elementIndex];
            if (one && one->kind == TyKind::Value && one->inner)
            {
              t = one->inner;
              v = one->value;
            }
          }
        }
        if (!v || !t || !t->isKnown())
          unsupported(e, "a template parameter whose value is not known here");
        if ((t->kind == TyKind::Pointer || t->kind == TyKind::Nullptr) && *v == 0)
          return {"null", t}; // `fold = nullptr` for a function pointer parameter; `template<auto V>` given nullptr
        return {t->kind == TyKind::Bool ? std::string(*v ? "true" : "false") : std::to_string(*v), t};
      }
      if (d && d->kind == NodeKind::FunctionDecl)
      {
        auto* f = static_cast<FunctionDecl*>(d);
        if (f->templateParams.empty() && !global.isTemplateContext(f))
        {
          refuseCAddress(f, e);
          return {useFunction(f, {}, nullptr, e), types.pointer(types.withNoexcept(declType(d), global.isNoexcept(f, {})))};
        }
        // `&FunctionImpl<R, Args...>::call<F>`: a member of an instantiation, with explicit arguments.
        TemplateArgs fargs;
        const Ty* classTy = nullptr;
        if (global.owningClass(f))
        {
          if (n->name.parts.size() > 1)
          {
            NamedType qualifier;
            qualifier.kind = NodeKind::NamedType;
            qualifier.token = n->token;
            qualifier.name = n->name;
            qualifier.name.parts.count -= 1;
            TemplateArgs none;
            const Ty* q = global.typeOfAstIn(&qualifier, typingContext(), currentArgs ? *currentArgs : none);
            if (q && isClass(q))
              classTy = canonicalClass(q, e);
          }
          else if (currentClassTy)
            classTy = canonicalClass(currentClassTy, e);
          // `Expression::computeConstant` with Expression deriving from the class template
          // instance that declares it (ComplexExpressionWithOperation<QuickMultioctaveNoise, 2,
          // 10>): that instance's member, with its arguments.
          if (classTy)
            if (const Ty* declaring = instantiationIn(classTy, global.owningClass(f), e); declaring && !declaring->args.empty())
              classTy = declaring;
          if (!classTy)
            unsupported(e, "taking the address of a member of an unknown instantiation");
          fargs = global.classArgsOf(classTy);
        }
        addressedTemplateArgs(f, n, e, fargs);
        std::string name = useFunction(f, fargs, classTy, e);
        ArgsScope scope(*this, &fargs);
        return {name, types.pointer(types.withNoexcept(declType(f), global.isNoexcept(f, fargs)))};
      }
      if (d && d->kind == NodeKind::VariableDecl && !locals.count(d))
      {
        const Ty* t = types.stripRef(declType(d));
        if (!d->templateParams.empty() && isConstValue(static_cast<VariableDecl*>(d))) // (another is an object, read as one: variableTemplateObject)
        {
          if (std::optional<Value> value = variableTemplateConstant(e, t))
            return *value;
          if (getenv("KPP_DEBUG_VT"))
            fprintf(stderr, "DEBUG vt file=%s ctx=%s args=%d constContext=%s\n", currentFile ? currentFile->path.c_str() : "null",
                    typingContext() ? nodeKindName(typingContext()->kind) : "null", currentArgs ? int(currentArgs->pairs.size()) : -1,
                    constContext ? nodeKindName(constContext->kind) : "null");
          unsupported(e, "a variable template whose value is not a constant");
        }
        if (t && t->isConst && (t->kind == TyKind::Int || t->kind == TyKind::Enum || t->kind == TyKind::Char || t->kind == TyKind::Bool))
          if (auto c = evalConst(e))
            return {t->kind == TyKind::Bool ? (*c ? "true" : "false") : std::to_string(*c), types.unqualified(t)};
      }
      std::string addr = lvalue(e);
      const Ty* type = e->ty;
      if ((!type || !type->isKnown()) && d)
        if (auto local = locals.find(d); local != locals.end() && local->second.ty && local->second.ty->isKnown())
          type = local->second.ty; // `auto xmm0 = _mm_loadu_si128(...)`: the type the code generator gave the local
      if (const BitfieldInfo* bits = bitfieldNamed(e)) // a member bit-field named without `this->`
        return loadBitfield(addr, *bits, types.stripRef(type ? type : declType(d)), e);
      return load(addr, type ? type : (d ? declType(d) : nullptr), e);
    }
    case NodeKind::ThisExpr:
    {
      std::string p = thisPointer(e);
      const Ty* t = types.stripRef(e->ty);
      if (t && t->kind == TyKind::Enum)
      {
        std::string r = newTemp(); // in an enum's methods `this` is the value
        fn += "  " + r + " = load " + llType(t, e) + ", ptr " + p + "\n";
        return {r, t};
      }
      return {p, e->ty};
    }
    case NodeKind::ParenExpr:
      return static_cast<ParenExpr*>(e)->holds ? holdValue(static_cast<ParenExpr*>(e)) : rvalue(static_cast<ParenExpr*>(e)->inner);
    case NodeKind::HeldExpr:
    {
      const HeldValue& held = heldValues.at(static_cast<HeldExpr*>(e)->holder);
      return held.address.empty() ? held.value : load(held.address, held.value.ty, e);
    }
    case NodeKind::DefaultValueExpr:
    {
      const Ty* t = types.stripRef(e->ty);
      if (!t || !t->isKnown())
        unsupported(e, "the default value of an unknown type");
      if (t->kind == TyKind::Void)
        return {"", t};
      std::string slot = newObjectSlot(t, e);
      initializeFrom(slot, t, e, e);
      if (!isClass(t))
        return load(slot, t, e);
      if (needsDestruction(t, e))
        statementTemps.push_back({slot, t});
      return {slot, t};
    }
    case NodeKind::MemberExpr:
    {
      auto* m = static_cast<MemberExpr*>(e);
      if (m->resolved && m->resolved->kind == NodeKind::EnumeratorDecl)
      {
        if (m->base->kind != NodeKind::NameExpr)
          rvalue(m->base); // `make().E` calls make, as `make().staticMember` does
        return {std::to_string(enumeratorNamed(static_cast<EnumeratorDecl*>(m->resolved), e)), e->ty};
      }
      if (const Ty* bt = m->base->ty ? types.stripRef(m->base->ty) : nullptr; bt && bt->kind == TyKind::Member && !m->dynamicMember)
      {
        // A reflection descriptor: m.name is the member's name, m.offset its offset in the class;
        // an alternative's (alternatives(U)) a.name is the alternative's, a.tag its tag value.
        auto* field = static_cast<VariableDecl*>(bt->decl);
        std::string which = m->member.empty() ? std::string() : sym(m->member.last().symbol);
        if (ClassDecl* u = bt->args.empty() ? nullptr : typedUnionOf(bt->args[0], e))
          if (int64_t index = descriptorAlternative(u, bt); index >= 0)
          {
            auto* en = static_cast<EnumeratorDecl*>(static_cast<EnumDecl*>(u->tagEnum)->enumerators[size_t(index)]);
            if (which == "name")
              return {cStringConstant(sym(en->name)), e->ty};
            if (which == "tag")
              return {std::to_string(enumeratorValue(en)), tagType(u)};
            unsupported(e, "an alternative descriptor has name, type and tag only");
          }
        if (which == "name")
          return {cStringConstant(field->name.empty() ? std::string() : sym(field->name.last().symbol)), e->ty};
        if (which == "offset")
        {
          const Ty* cls = bt->args.empty() ? nullptr : bt->args[0];
          std::string addr;
          if (!cls || !fieldAddress(cls, field, "null", addr, e))
            unsupported(e, "the offset of a member that is not a field");
          std::string r = newTemp();
          fn += "  " + r + " = ptrtoint ptr " + addr + " to i64\n";
          return {r, e->ty};
        }
        unsupported(e, "a member descriptor has name and offset only");
      }
      std::string addr = lvalue(e);
      // Use the field's declared type when the expression type is unknown.
      const Ty* memberTy = e->ty;
      const Ty* memberTyStripped = memberTy ? types.stripRef(memberTy) : nullptr;
      if ((!memberTyStripped || !memberTyStripped->isKnown()) && m->resolved && m->resolved->kind == NodeKind::VariableDecl)
        memberTy = declType(m->resolved);
      memberTyStripped = memberTy ? types.stripRef(memberTy) : nullptr;
      if ((!memberTyStripped || !memberTyStripped->isKnown()) && !m->member.empty())
      {
        // Try finding the field by name for its declared type.
        const Ty* bt = m->base->ty ? types.stripRef(m->base->ty) : nullptr;
        if (m->op == P::Arrow && bt && bt->kind == TyKind::Pointer)
          bt = types.stripRef(bt->inner);
        if (bt && isClass(bt))
          if (Decl* fd = findFieldByName(bt, m->member.last().symbol, e))
            memberTy = declType(fd);
      }
      if (const BitfieldInfo* bits = bitfieldNamed(e)) // (one whose place is not known is an error, never its whole unit)
        return loadBitfield(addr, *bits, types.stripRef(memberTy), e);
      return load(addr, memberTy, e);
    }
    case NodeKind::IndexExpr:
    {
      auto* i = static_cast<IndexExpr*>(e);
      Value written = i->indexFirst ? convert(rvalue(i->args[0]), types.intType(64), e) : Value{}; // (`2[a]`: from the left)
      Value base = withCheckedConst(rvalue(i->base), i->base);
      if (ClassDecl* c = typedUnionOf(base.ty, e))
      {
        std::string addr = alternativeLvalue(c, base, i, e, true);
        const Ty* altTy = types.stripRef(e->ty);
        return isClass(altTy) ? Value{addr, altTy, false} : load(addr, altTy, e);
      }
      if (isClass(base.ty))
        return callRvalue(operatorCall(base, operatorName(P::LBracket), i->args, e, true), e);
      Value index = i->indexFirst ? written : convert(rvalue(i->args[0]), types.intType(64), e);
      if (base.ty->kind != TyKind::Pointer)
        unsupported(e, "indexing a non-pointer");
      std::string r = newTemp();
      fn += "  " + r + " = getelementptr inbounds " + llType(base.ty->inner, e) + ", ptr " + base.text + ", i64 " + index.text + "\n";
      return load(r, base.ty->inner, e);
    }
    case NodeKind::CallExpr: return callRvalue(callValue(static_cast<CallExpr*>(e), ""), e);
    case NodeKind::TypeidExpr:
    {
      auto* t = static_cast<TypeidExpr*>(e);
      const Ty* infoTy = e->ty ? types.stripRef(e->ty) : nullptr;
      if (!isClass(infoTy))
        unsupported(e, "typeid without the library's TypeInfo class");
      std::string namePtr, identity = "null";
      const Ty* staticTy = nullptr;
      if (t->operandDecl)
      {
        // typeid(q): the parser read the variable as a type name; the resolver found it.
        const Ty* xt = t->operandType ? types.stripRef(t->operandType) : nullptr;
        if (isClass(xt) && structFor(xt, e).polymorphic)
        {
          identity = loadSlot(addressOfDecl(t->operandDecl, e), -1);
          namePtr = descriptorName(identity);
        }
        else
          staticTy = xt;
      }
      else if (t->operand && uint8_t(t->operand->kind) <= uint8_t(NodeKind::PackType))
        staticTy = t->operandType ? types.stripRef(t->operandType) : nullptr; // typeid(Widget): typed by the resolver
      else if (t->operand)
      {
        auto* x = static_cast<Expr*>(t->operand);
        Decl* named = x->kind == NodeKind::NameExpr ? static_cast<NameExpr*>(x)->resolved : nullptr;
        const Ty* xt = x->ty ? types.stripRef(x->ty) : nullptr;
        if (named && named->kind == NodeKind::ClassDecl)
          staticTy = canonicalClass(types.classType(named, {}), e); // typeid(Base): the parser read the type as a name
        else if (named && named->kind == NodeKind::EnumDecl)
          staticTy = types.enumType(named);
        else if (isClass(xt) && structFor(xt, e).polymorphic)
        {
          // The dynamic type: slot -1 of the object's vtable. `typeid(*p)` of a null p throws
          // bad_typeid ([expr.typeid] 3).
          std::string object = lvalue(x);
          Expr* inner = x;
          while (inner->kind == NodeKind::ParenExpr)
            inner = static_cast<ParenExpr*>(inner)->inner;
          if (inner->kind == NodeKind::UnaryExpr && !static_cast<UnaryExpr*>(inner)->isKeyword && P(static_cast<UnaryExpr*>(inner)->op) == P::Star)
          {
            std::string isNull = newTemp(), nullL = newLabel("typeid.null"), okL = newLabel("typeid.ok");
            fn += "  " + isNull + " = icmp eq ptr " + object + ", null\n";
            fn += "  br i1 " + isNull + ", label %" + nullL + ", label %" + okL + "\n";
            startBlock(nullL);
            throwFromLibrary("throwBadTypeid", e, "typeid of the object a null pointer points to");
            blockTerminated = true;
            startBlock(okL);
          }
          identity = loadSlot(object, -1);
          namePtr = descriptorName(identity);
        }
        else
          staticTy = xt;
      }
      if (namePtr.empty())
      {
        if (!staticTy || !staticTy->isKnown() || types.isDependent(staticTy))
          unsupported(e, "typeid of an unknown type (operand: " + std::string(t->operand ? nodeKindName(t->operand->kind) : "none") + ", " +
                             (staticTy ? types.toString(staticTy, &symbols) : std::string("no type")) + ")");
        namePtr = cStringConstant(types.toString(types.unqualified(staticTy), &symbols));
        if (const Ty* cls = types.unqualified(staticTy); isClass(cls) && !declaredOnly(cls))
        {
          identity = descriptorFor(cls, e); // (a class's name does not tell it from another's of the name)
          useDescriptor(structFor(cls, e));
        }
      }
      std::string slot = newObjectSlot(infoTy, e);
      const StructInfo& info = structFor(infoTy, e);
      if (info.fieldDecls.size() < 2)
        unsupported(e, "a TypeInfo class without its name and identity fields");
      fn += "  store ptr " + namePtr + ", ptr " + gep(info.name, slot, fieldIndexOf(info, info.fieldDecls[0])) + "\n";
      fn += "  store ptr " + identity + ", ptr " + gep(info.name, slot, fieldIndexOf(info, info.fieldDecls[1])) + "\n";
      return {slot, infoTy, true};
    }
    case NodeKind::NoexceptExpr:
    {
      // The walk's answer (GlobalScope::canThrow); its operand is not evaluated.
      int8_t value = static_cast<NoexceptExpr*>(e)->value;
      if (value < 0)
        unsupported(e, "a noexcept operator the checker did not decide");
      return {value > 0 ? "true" : "false", types.boolType()};
    }
    case NodeKind::NameofExpr:
    {
      Expr* o = static_cast<NameofExpr*>(e)->operand;
      std::string text;
      if (!o)
        text = sym(static_cast<NameofExpr*>(e)->text);
      else if (o->kind == NodeKind::NameExpr)
        text = sym(static_cast<NameExpr*>(o)->name.last().symbol);
      else if (o->kind == NodeKind::MemberExpr)
        text = sym(static_cast<MemberExpr*>(o)->member.last().symbol);
      else
        text = std::string(spelling(o->token));
      return {cStringConstant(text), e->ty};
    }
    case NodeKind::CastExpr:
      if (e->ty && e->ty->kind == TyKind::Void)
      {
        rvalue(static_cast<CastExpr*>(e)->operand); // `(void)x`: evaluated, result dropped
        return {"", types.voidType()};
      }
      return explicitConvert(rvalue(static_cast<CastExpr*>(e)->operand), e->ty, e);
    case NodeKind::NamedCastExpr:
    {
      auto* c = static_cast<NamedCastExpr*>(e);
      if (c->castKind == Kw::dynamic_cast_)
        if (std::optional<Value> v = dynamicCast(c, e))
          return *v;
      if (const Ty* t = e->ty ? types.stripRef(e->ty) : nullptr; c->castKind == Kw::static_cast_ && t && !e->ty->isReference() && isClass(t))
      {
        // `static_cast<T>(x)` to a class: a temporary T direct-initialized from x, by an explicit
        // constructor too ([expr.static.cast] 4), as `T(x)` is (a copy of an object of T itself, a
        // base's part of a derived one). (Until 2026-11-05 an int for an explicit constructor was
        // "a conversion from int32 to Strict", thread D's spec language/static_cast_by_explicit_constructor.)
        std::string addr = newObjectSlot(t, e);
        Expr* one = c->operand;
        directInitialize(addr, t, Span<Expr*>{&one, 1}, false, e, true);
        statementTemps.push_back({addr, t});
        return {addr, t, true};
      }
      if (c->castKind == Kw::reinterpret_cast_ && e->ty && e->ty->isReference())
      {
        // `reinterpret_cast<unsigned&>(f)`: f's bytes read as an unsigned, no conversion of its value.
        const Ty* t = types.stripRef(e->ty);
        std::string addr = lvalue(c->operand);
        return isAggregate(t) ? Value{addr, t} : load(addr, t, e);
      }
      if (c->castKind == Kw::reinterpret_cast_ && e->ty && e->ty->kind == TyKind::Pointer)
      {
        // The address as it is: `reinterpret_cast<Base*>(derived)` moves to no base.
        Value v = rvalue(c->operand);
        return v.ty && v.ty->kind == TyKind::Pointer ? Value{v.text, e->ty} : explicitConvert(v, e->ty, e);
      }
      return explicitConvert(rvalue(c->operand), e->ty, e);
    }
    case NodeKind::FunctionalCastExpr:
    {
      auto* c = static_cast<FunctionalCastExpr*>(e);
      const Ty* t = types.stripRef(e->ty);
      if (isClass(t))
      {
        std::string addr = newObjectSlot(t, e);
        directInitialize(addr, t, c->args, c->braced, e, !c->braced); // `T(list)`: the list is a constructor's argument
        statementTemps.push_back({addr, t});
        return {addr, t, true};
      }
      if (t->kind == TyKind::Array) // (arrayTemporary makes one for a reference to an array, an aggregate's reference member and a pointer parameter: isArrayBraces)
        unsupported(e, "a temporary array (`" + types.toString(t, &symbols) + "{...}`) other than for a reference to an array or a pointer parameter");
      // A kpp enum's constructor (kovarex, 2026-11-11): `Dir()`, `Dir{}`, `Dir(1, 2)`, `Dir d(input);`; `Dir(2)` stays its conversion.
      if (Span<Expr*> values = c->braced && c->args.size() == 1 && c->args[0]->kind == NodeKind::BraceInitExpr ? static_cast<BraceInitExpr*>(c->args[0])->args : c->args;
          enumConstructs(t, values))
        return constructedEnum(t, values, e);
      // `value_type{forward<Args>(args)...}` with a scalar value_type: as `T(...)` below.
      Span<Expr*> args = c->args;
      if (args.size() == 1 && args[0]->kind == NodeKind::BraceInitExpr && !static_cast<BraceInitExpr*>(args[0])->text && static_cast<BraceInitExpr*>(args[0])->args.size() == 1 &&
          isPackExpansion(static_cast<BraceInitExpr*>(args[0])->args[0]))
        args = static_cast<BraceInitExpr*>(args[0])->args;
      if (args.size() == 1 && isPackExpansion(args[0]))
      {
        // `T(forward<Args>(args)...)` with a scalar T: the pack's one element, re-typed for it
        // (so that `forward<Args>` means forward<that element>).
        Expr* pattern = static_cast<UnaryExpr*>(args[0])->operand;
        size_t count = packElementCount(pattern, e);
        if (count == 0)
          return {t->kind == TyKind::Float ? "0.0" : t->kind == TyKind::Pointer ? "null" : "0", t};
        if (count != 1)
          unsupported(e, "a scalar built from a pack of several elements");
        Value v;
        forEachPackElement(pattern, e, [&](size_t) { v = explicitConvert(rvalue(pattern), t, e); });
        return v;
      }
      if (c->args.size() == 1)
      {
        Expr* a = c->args[0];
        if (a->kind == NodeKind::BraceInitExpr && static_cast<BraceInitExpr*>(a)->args.empty())
          return {t->kind == TyKind::Float ? "0.0" : t->kind == TyKind::Pointer ? "null" : "0", t}; // `mapped_type{}`: zero
        if (a->kind == NodeKind::BraceInitExpr && static_cast<BraceInitExpr*>(a)->args.size() == 1)
          return convert(rvalue(static_cast<BraceInitExpr*>(a)->args[0]), t, e, true); // `T{x}`: direct-list-initialisation, no cast
        return explicitConvert(rvalue(a), t, e); // `T(x)` is the cast `(T)x`
      }
      if (c->args.empty())
        return {t->kind == TyKind::Float ? "0.0" : t->kind == TyKind::Pointer ? "null" : "0", t};
      unsupported(e, "this functional cast");
    }
    case NodeKind::RequiresExpr:
      return {static_cast<RequiresExpr*>(e)->satisfied ? "true" : "false", types.boolType()};
    case NodeKind::SizeofExpr:
    {
      auto* s = static_cast<SizeofExpr*>(e);
      if (s->isPack)
      {
        Decl* param = nullptr;
        if (s->type && s->type->kind == NodeKind::NamedType)
          param = static_cast<NamedType*>(s->type)->resolved;
        if (!param && s->operand)
          param = packIn(s->operand);
        if (!param && s->type && s->type->kind == NodeKind::NamedType && static_cast<NamedType*>(s->type)->name.parts.size() == 1)
        {
          // `sizeof...(xs)` of a generic lambda's `auto... xs`, parsed as a type name the walk did
          // not bind: the pack parameter of that name being emitted.
          uint32_t symbol = static_cast<NamedType*>(s->type)->name.parts[0].symbol;
          for (auto& [d, elements] : packLocals)
            if (d && d->kind == NodeKind::ParamDecl && static_cast<ParamDecl*>(d)->name == symbol)
              param = d;
        }
        if (param && param->kind == NodeKind::TemplateParamDecl && currentArgs)
          for (auto& [p, t] : currentArgs->pairs)
            if (p == param && t && t->kind == TyKind::Pack)
              return {std::to_string(t->args.size()), types.sizeType()}; // `sizeof...(Args)` of a template pack
        if (param && !packLocals.count(param) && param->kind == NodeKind::ParamDecl && static_cast<ParamDecl*>(param)->isPack)
        {
          // A pack of the function a lambda is written in, which the lambda captures nothing of
          // (`sizeof...(ts)` names no element): as many as the instantiation's walk typed it with.
          const Ty* pack = param->cachedType.load(std::memory_order_acquire);
          if (!pack || pack->kind != TyKind::Pack)
            pack = global.typeOfDecl(param);
          if (pack && pack->kind == TyKind::Pack)
            return {std::to_string(pack->args.size()), types.sizeType()};
        }
        if (!param || !packLocals.count(param))
          unsupported(e, "sizeof... of something that is not a parameter pack");
        return {std::to_string(packLocals[param].size()), types.sizeType()};
      }
      // `sizeof(a)` of a function parameter pack's element, bound while its expansion is emitted
      // (`(0 + ... + (sizeof(a) == 4 ? 1 : 0))`): that element's size (the pack's whole type has none).
      Decl* named = s->type && s->type->kind == NodeKind::NamedType ? static_cast<NamedType*>(s->type)->resolved
                  : s->operand && s->operand->kind == NodeKind::NameExpr ? static_cast<NameExpr*>(s->operand)->resolved : nullptr;
      if (named && named->kind == NodeKind::ParamDecl && static_cast<ParamDecl*>(named)->isPack && s->sizeofKind != Kw::alignof_)
        if (auto local = locals.find(named); local != locals.end() && local->second.ty && local->second.ty->isKnown())
          return {std::to_string(sizeOf(types.stripRef(local->second.ty), e)), types.sizeType()};
      auto v = evalConst(e);
      if (!v)
        unsupported(e, "this sizeof");
      return {std::to_string(*v), types.sizeType()};
    }
    case NodeKind::FoldExpr:
    {
      auto* f = static_cast<FoldExpr*>(e);
      P op = P(f->op);
      // `(args op ...)` and `(args op ... op init)` fold to the right: E1 op (E2 op (... op init));
      // `(... op args)` and `(init op ... op args)` to the left: ((init op E1) op E2) op ...
      bool right = f->lhs && (packIn(f->lhs) || typePackIn(f->lhs));
      Expr* pattern = right ? f->lhs : f->rhs;
      Expr* init = right ? f->rhs : f->lhs;
      // A function parameter pack (`(args + ...)`), or a type pack the pattern names in a type
      // (`(func(Args()), ...)`, `(... || isSame<T, Args>)`).
      Decl* typePack = pattern && !packIn(pattern) ? typePackIn(pattern) : nullptr;
      if (!pattern || (!packIn(pattern) && !typePack))
        unsupported(e, "a fold expression that expands no parameter pack");
      // Operands in evaluation order: the elements, with init last (right fold) or first (left).
      auto eachOperand = [&](auto&& visit)
      {
        if (init && !right)
          visit(init);
        if (typePack)
          forEachTypePackElement(typePack, [&](size_t) { visit(pattern); }, pattern);
        else
          forEachPackElement(pattern, e, [&](size_t) { visit(pattern); });
        if (init && right)
          visit(init);
      };
      // A class's, a closure's or an enumeration's own `,`, `&&` or `||`: the fold is the chain of its calls
      // ([temp.variadic] 10), each step through binary(), which calls the operator function where one applies
      // and takes the built-in operator where none does. Until 2026-11-10 these three were the built-in ones
      // for every operand: a class's operator was never called, silently where the class converts to bool or
      // the comma fold's value is not used.
      if (op == P::Comma || op == P::AndAnd || op == P::OrOr)
      {
        std::vector<const Ty*> operandTypes;
        eachOperand([&](Expr* x) { operandTypes.push_back(x->ty ? types.stripRef(x->ty) : nullptr); });
        if (foldCallsOperator(f, op, operandTypes))
          return foldOverOperators(f, pattern, init, right, typePack);
      }
      if (op == P::Comma)
      {
        // Its value is the last operand's, where the walk typed it so (operands of no class).
        Value last{"", types.voidType()};
        eachOperand([&](Expr* x) { last = rvalue(x); }); // (its temporaries live to the end of the full-expression)
        const Ty* t = e->ty ? types.stripRef(e->ty) : nullptr;
        if (!t || t->kind == TyKind::Void || !last.ty || types.stripRef(last.ty)->kind == TyKind::Void)
          return {"", types.voidType()};
        return isClass(t) || types.unqualified(types.stripRef(last.ty)) == types.unqualified(t) ? last : convert(last, t, e, false, ConversionContext::Copy);
      }
      if (op == P::AndAnd || op == P::OrOr)
      {
        // One operand, the only element or init beside an empty pack: the fold is that operand itself,
        // not its truth ([temp.variadic] 10: `(... && args)` of the int -10 is -10, `(init && ... && args)`
        // of none is init; until 2026-11-09 a bool, 1, as the walk now types it too).
        const Ty* bound = typePack ? boundPack(typePack) : nullptr;
        size_t elements = typePack ? (bound ? bound->args.size() : 2) : packElementCount(pattern, e);
        if (elements + (init ? 1 : 0) == 1)
        {
          Value only{"", types.voidType()};
          eachOperand([&](Expr* x) { only = rvalue(x); });
          const Ty* t = e->ty ? types.stripRef(e->ty) : nullptr;
          return !t || !t->isKnown() || isClass(t) || types.unqualified(types.stripRef(only.ty)) == types.unqualified(t)
                   ? only : convert(only, t, e, false, ConversionContext::Copy);
        }
        // Short-circuit: each operand that decides the result ends the fold (an empty pack
        // without init is true for &&, false for ||).
        bool isAnd = op == P::AndAnd;
        std::string endL = newLabel(isAnd ? "fold.and.end" : "fold.or.end");
        std::string phi;
        eachOperand([&](Expr* x)
        {
          size_t temps = statementTemps.size();
          Value v = convert(rvalue(x), types.boolType(), e, false, ConversionContext::Direct);
          keepOperandTemps(temps); // an operand runs only when the ones before did not decide
          std::string next = newLabel("fold.next");
          fn += "  br i1 " + v.text + ", label %" + (isAnd ? next : endL) + ", label %" + (isAnd ? endL : next) + "\n";
          phi += std::string(phi.empty() ? "" : ", ") + "[ " + (isAnd ? "false" : "true") + ", %" + currentBlock + " ]";
          startBlock(next);
        });
        phi += std::string(phi.empty() ? "" : ", ") + "[ " + (isAnd ? "true" : "false") + ", %" + currentBlock + " ]";
        emitBranch(endL);
        startBlock(endL);
        std::string r = newTemp();
        fn += "  " + r + " = phi i1 " + phi + "\n";
        return {r, types.boolType()};
      }
      if (isAssignment(op))
        return foldOfAssignments(f, pattern, init, right, typePack);
      // A class, a closure or an enumeration among the operands: a step may call an operator function
      // (foldOverOperators). Scalars alone: the built-in operator, step by step.
      bool operators = false;
      eachOperand([&](Expr* x)
      {
        const Ty* t = x->ty ? types.stripRef(x->ty) : nullptr;
        operators = operators || (t && (isClass(t) || isLambda(t) || t->kind == TyKind::Enum));
      });
      if (operators)
        return foldOverOperators(f, pattern, init, right, typePack);
      std::vector<Value> values;
      eachOperand([&](Expr* x) { values.push_back(rvalue(x)); });
      if (values.empty())
        unsupported(e, "a fold over an empty pack without an initial value");
      auto combine = [&](const Value& a, const Value& b)
      {
        if (isClass(a.ty) || isClass(b.ty))
          unsupported(e, "a fold over an operand of a class the checker did not type");
        if (op == P::EqEq || op == P::NotEq || op == P::Less || op == P::LessEq || op == P::Greater || op == P::GreaterEq)
          return compare(op, a, b, e);
        return arithmetic(op, a, b, e);
      };
      Value acc = right ? values.back() : values.front();
      if (right)
        for (size_t i = values.size() - 1; i-- > 0;)
          acc = combine(values[i], acc);
      else
        for (size_t i = 1; i < values.size(); ++i)
          acc = combine(acc, values[i]);
      return acc;
    }
    case NodeKind::UnaryExpr: return unary(static_cast<UnaryExpr*>(e));
    case NodeKind::BinaryExpr: return binary(static_cast<BinaryExpr*>(e));
    case NodeKind::ConditionalExpr:
    {
      auto* c = static_cast<ConditionalExpr*>(e);
      if (!c->then)
        unsupported(e, "the ?: elvis operator");
      const Ty* t = types.stripRef(e->ty);
      if (t && t->kind == TyKind::Array && t->inner)
        t = types.pointer(t->inner); // two arrays of one type: as a value, the chosen one decays (as load's)
      if (!t || !t->isKnown())
        return conditionalOfBranches(c);
      if (t && t->kind != TyKind::Void)
        if (Expr* beside = enterBranchBesideThrow(c))
        {
          Value v = rvalue(beside);
          return isClass(t) || isLambda(t) || v.ty == t ? v : convert(v, t, e, false, ConversionContext::Copy);
        }
      Value cond = convert(rvalue(c->cond), types.boolType(), e, false, ConversionContext::Direct);
      std::string tL = newLabel("cond.then"), eL = newLabel("cond.else"), jL = newLabel("cond.end");
      fn += "  br i1 " + cond.text + ", label %" + tL + ", label %" + eL + "\n";
      // A closure is an object as a class is (`c ? make(1) : make(2)` of one lambda's type): until
      // 2026-11-07 it took the scalars' path below, a phi of the closure's struct given the branches'
      // addresses, which clang rejected (thread J's lambda generator).
      bool object = isClass(t) || isLambda(t);
      const Ty* thenObject = object ? conditionalObject(c->then, t, e) : nullptr;
      const Ty* elseObject = object ? conditionalObject(c->else_, t, e) : nullptr;
      if (thenObject && elseObject)
      {
        // Both branches name objects (`cond ? a : b` with lvalues or references): the result
        // is one of them, not a copy.
        size_t temps = statementTemps.size();
        startBlock(tL);
        std::string a = conditionalAddress(c->then, thenObject, t, e);
        keepOperandTemps(temps);
        std::string aEnd = currentBlock;
        emitBranch(jL);
        startBlock(eL);
        std::string b = conditionalAddress(c->else_, elseObject, t, e);
        keepOperandTemps(temps);
        std::string bEnd = currentBlock;
        emitBranch(jL);
        startBlock(jL);
        std::string r = newTemp();
        fn += "  " + r + " = phi ptr [ " + a + ", %" + aEnd + " ], [ " + b + ", %" + bEnd + " ]\n";
        return {r, t, false};
      }
      if (object)
      {
        // Both branches build the one temporary (a copy of an lvalue, or the value itself).
        std::string slot = newObjectSlot(t, e);
        size_t temps = statementTemps.size();
        startBlock(tL);
        initializeFrom(slot, t, c->then, e);
        keepOperandTemps(temps);
        emitBranch(jL);
        startBlock(eL);
        initializeFrom(slot, t, c->else_, e);
        keepOperandTemps(temps);
        emitBranch(jL);
        startBlock(jL);
        if (needsDestruction(t, e))
          statementTemps.push_back({slot, t});
        return {slot, t, true};
      }
      size_t temps = statementTemps.size();
      startBlock(tL);
      Value a = t->kind == TyKind::Void ? rvalue(c->then) : convert(rvalue(c->then), t, e, false, ConversionContext::Copy); // (a branch converts implicitly, [expr.cond] 4)
      keepOperandTemps(temps); // `number(isNumber ? strtodouble(String(data, length)) : 0)`: the String exists only when this branch ran
      std::string aEnd = currentBlock;
      emitBranch(jL);
      startBlock(eL);
      Value b = t->kind == TyKind::Void ? rvalue(c->else_) : convert(rvalue(c->else_), t, e, false, ConversionContext::Copy);
      keepOperandTemps(temps);
      std::string bEnd = currentBlock;
      emitBranch(jL);
      startBlock(jL);
      if (t->kind == TyKind::Void)
        return {"", t};
      std::string r = newTemp();
      fn += "  " + r + " = phi " + llType(t, e) + " [ " + a.text + ", %" + aEnd + " ], [ " + b.text + ", %" + bEnd + " ]\n";
      return {r, t};
    }
    case NodeKind::NewExpr:
    {
      auto* n = static_cast<NewExpr*>(e);
      const Ty* t = e->ty && e->ty->kind == TyKind::Pointer ? e->ty->inner : nullptr;
      if (!t)
        unsupported(e, "new of an unknown type");
      if (n->type && n->type->kind == NodeKind::ArrayType && static_cast<ArrayType*>(n->type)->size)
      {
        if (!n->placement.empty() && !n->allocator)
          unsupported(e, "placement new of an array");
        return {arrayNew(n, t, static_cast<ArrayType*>(n->type)->size), e->ty};
      }
      std::string r;
      std::vector<std::string> placed; // the placement arguments as a class's operator new took them
      if (n->allocator)
      {
        // The class's own operator new, or a global one the program declares, with the size and
        // the placement arguments (the type checker's choice, Walker::chooseAllocation).
        const Ty* in = global.owningClass(n->allocator) ? declaringClass(t, n->allocator, e) : nullptr;
        if (global.owningClass(n->allocator) && !in)
          unsupported(e, "the operator new chosen for a class that does not declare it");
        r = callFunction(n->allocator, in ? global.classArgsOf(in) : TemplateArgs{}, in, "", "", n->placement, e, "", {"i64 " + std::to_string(sizeOf(t, e))}, &placed).text;
      }
      else if (n->placement.size() == 1)
      {
        Value place = rvalue(n->placement[0]); // any object's place, a const one too (design.md 11)
        r = convert(place, types.pointer(types.voidType()), e, place.ty && place.ty->kind == TyKind::Pointer).text;
      }
      else if (!n->placement.empty())
        unsupported(e, "placement new with several arguments");
      else
      {
        ensureMalloc();
        r = newTemp();
        emitCall(r + " = ", "ptr @malloc(i64 " + std::to_string(sizeOf(t, e)) + ")", false);
        checkAllocation(r, e);
      }
      // (a kpp enum's constructor makes its value as a class's makes an object: kovarex, 2026-11-11)
      Span<Expr*> enumValues = n->args.size() == 1 && n->args[0]->kind == NodeKind::BraceInitExpr ? static_cast<BraceInitExpr*>(n->args[0])->args : n->args;
      bool enumConstructor = !isClass(t) && enumConstructs(t, enumValues);
      if ((isClass(t) || enumConstructor) && (n->placement.empty() || n->deallocator))
      {
        // A constructor that throws gives the memory back, as C++'s new does: freed while the
        // constructor runs if an exception leaves it, and only then; by the class's operator
        // delete when the type checker found one for its operator new ([expr.new] 26), and by
        // none for a placement form without one.
        Object allocation{r, t};
        allocation.unwindOnly = true;
        allocation.freeMemory = true;
        if (n->deallocator)
          allocation.freeCall = deallocationCall(n->deallocator, n->sizedDeallocation && n->placement.empty() ? std::to_string(sizeOf(t, e)) : "", t, r, placed, e);
        else if (programAllocationFunctions())
          allocation.freeCall = globalFreeCall(r, std::to_string(sizeOf(t, e)), false, e);
        statementTemps.push_back(allocation);
        size_t at = statementTemps.size() - 1;
        if (enumConstructor)
          constructEnum(r, t, enumValues, e);
        else
          constructInto(r, t, n->args, e);
        statementTemps.erase(statementTemps.begin() + at);
      }
      else if (isClass(t))
        constructInto(r, t, n->args, e);
      else if (enumConstructor)
        constructEnum(r, t, enumValues, e); // `new (p) Dir(args...)`
      else if (isLambda(t) && n->args.size() == 1)
        initializeFrom(r, t, n->args[0], e); // `new F(callable)` with F a lambda type: the closure, copied
      else if (n->args.size() == 1 && isPackExpansion(n->args[0]))
      {
        // `new (p) T(forward<Args>(args)...)` with a scalar T: the pack's one element.
        Expr* pattern = static_cast<UnaryExpr*>(n->args[0])->operand;
        size_t count = packElementCount(pattern, e);
        if (count == 0)
          storeZero(t, r, e);
        else if (count != 1)
          unsupported(e, "a scalar built from a pack of several elements");
        else
          forEachPackElement(pattern, e, [&](size_t) { store(rvalue(pattern), r, t, e); });
      }
      else if (!n->args.empty())
      {
        Expr* a = n->args[0];
        size_t braced = a->kind == NodeKind::BraceInitExpr ? static_cast<BraceInitExpr*>(a)->args.size() : 1;
        if (braced > 1)
          fail(e, "new of a scalar given " + std::to_string(braced) + " values in braces (it takes one at most)");
        if (braced == 0)
          storeZero(t, r, e); // `new int{}`: value-initialized, as `new int()` is
        else
        {
          if (a->kind == NodeKind::BraceInitExpr)
            a = static_cast<BraceInitExpr*>(a)->args[0];
          store(rvalue(a), r, t, e);
        }
      }
      else
        storeZero(t, r, e);
      return {r, e->ty};
    }
    case NodeKind::LambdaExpr: return makeClosure(static_cast<LambdaExpr*>(e));
    case NodeKind::ThrowExpr:
      emitThrow(static_cast<ThrowExpr*>(e));
      return {"", types.voidType()};
    case NodeKind::DeleteExpr:
    {
      auto* d = static_cast<DeleteExpr*>(e);
      Value p = rvalue(d->operand);
      if (isClass(p.ty))
        if (std::optional<Value> pointer = toPointer(p, e))
          p = *pointer; // `delete image` of a ValueComparePointer: what its conversion to a pointer gives, as C++ has it
      if (p.ty->kind != TyKind::Pointer)
        unsupported(e, "delete of a non-pointer");
      ensureMalloc();
      if (d->isArray)
      {
        arrayDelete(p.text, p.ty->inner, e, d->deallocator, d->sizedDeallocation);
        return {"", types.voidType()};
      }
      if (isClass(p.ty->inner) && (needsDestruction(p.ty->inner, e) || structFor(p.ty->inner, e).polymorphic))
      {
        // `delete p` with p null does nothing (C++): no destructor runs on address 0.
        std::string isNull = newTemp(), objectL = newLabel("delete.object"), endL = newLabel("delete.end");
        fn += "  " + isNull + " = icmp eq ptr " + p.text + ", null\n";
        fn += "  br i1 " + isNull + ", label %" + endL + ", label %" + objectL + "\n";
        blockTerminated = true;
        startBlock(objectL);
        // Through a base with a virtual destructor the object may be of a class that has this
        // base at an offset (a second base): the block starts at the whole object, which slot -2
        // of the vtable gives (read before the destructor runs).
        std::string block = p.text;
        std::string deleter; // through a virtual destructor: the object's own class's operator delete, slot -3 (null: free)
        if (slotKeyed(structFor(p.ty->inner, e), "~") >= 0)
        {
          std::string vptr = newTemp(), slot = newTemp(), offset = newTemp(), bytes = newTemp(), back = newTemp(), deleterSlot = newTemp();
          block = newTemp();
          deleter = newTemp();
          fn += "  " + vptr + " = load ptr, ptr " + p.text + "\n";
          fn += "  " + slot + " = getelementptr inbounds ptr, ptr " + vptr + ", i64 -2\n";
          fn += "  " + offset + " = load ptr, ptr " + slot + "\n";
          fn += "  " + bytes + " = ptrtoint ptr " + offset + " to i64\n";
          fn += "  " + back + " = sub i64 0, " + bytes + "\n";
          fn += "  " + block + " = getelementptr i8, ptr " + p.text + ", i64 " + back + "\n";
          fn += "  " + deleterSlot + " = getelementptr inbounds ptr, ptr " + vptr + ", i64 -3\n";
          fn += "  " + deleter + " = load ptr, ptr " + deleterSlot + "\n";
        }
        destroy(p.text, p.ty->inner, e, true);
        if (!deleter.empty())
        {
          std::string isFree = newTemp(), freeL = newLabel("delete.free"), ownL = newLabel("delete.own");
          fn += "  " + isFree + " = icmp eq ptr " + deleter + ", null\n";
          fn += "  br i1 " + isFree + ", label %" + freeL + ", label %" + ownL + "\n";
          blockTerminated = true;
          startBlock(ownL);
          emitCall("", "void " + deleter + "(ptr " + block + ")", false);
          emitBranch(endL);
          startBlock(freeL);
          emitCall("", "void @free(ptr " + block + ")", false); // (deleterFor's null: no allocation function of the program's)
          emitBranch(endL);
          startBlock(endL);
          return {"", types.voidType()};
        }
        emitCall("", d->deallocator ? deallocationCall(d->deallocator, d->sizedDeallocation ? std::to_string(sizeOf(p.ty->inner, e)) : "", p.ty->inner, block, {}, e)
                                    : globalFreeCall(block, std::to_string(sizeOf(p.ty->inner, e)), false, e),
                 false);
        emitBranch(endL);
        startBlock(endL);
        return {"", types.voidType()};
      }
      if (isLambda(p.ty->inner) && needsDestruction(p.ty->inner, e))
      {
        // FunctionImpl's `delete (F*)target` of a closure: its captures by value are destroyed.
        std::string isNull = newTemp(), objectL = newLabel("delete.object"), endL = newLabel("delete.end");
        fn += "  " + isNull + " = icmp eq ptr " + p.text + ", null\n";
        fn += "  br i1 " + isNull + ", label %" + endL + ", label %" + objectL + "\n";
        blockTerminated = true;
        startBlock(objectL);
        destroy(p.text, p.ty->inner, e);
        emitCall("", "void @free(ptr " + p.text + ")", false);
        emitBranch(endL);
        startBlock(endL);
        return {"", types.voidType()};
      }
      if (d->deallocator && isClass(p.ty->inner))
      {
        // The class's own operator delete, not called for a null pointer (as clang has it).
        std::string isNull = newTemp(), objectL = newLabel("delete.object"), endL = newLabel("delete.end");
        fn += "  " + isNull + " = icmp eq ptr " + p.text + ", null\n";
        fn += "  br i1 " + isNull + ", label %" + endL + ", label %" + objectL + "\n";
        blockTerminated = true;
        startBlock(objectL);
        emitCall("", deallocationCall(d->deallocator, d->sizedDeallocation ? std::to_string(sizeOf(p.ty->inner, e)) : "", p.ty->inner, p.text, {}, e), false);
        emitBranch(endL);
        startBlock(endL);
        return {"", types.voidType()};
      }
      if (programAllocationFunctions())
      {
        // The global operator delete, not called for a null pointer (as clang has it).
        std::string isNull = newTemp(), objectL = newLabel("delete.object"), endL = newLabel("delete.end");
        fn += "  " + isNull + " = icmp eq ptr " + p.text + ", null\n";
        fn += "  br i1 " + isNull + ", label %" + endL + ", label %" + objectL + "\n";
        blockTerminated = true;
        startBlock(objectL);
        const Ty* pointee = types.unqualified(p.ty->inner);
        emitCall("", globalFreeCall(p.text, pointee->kind == TyKind::Void ? "" : std::to_string(sizeOf(pointee, e)), false, e), false);
        emitBranch(endL);
        startBlock(endL);
        return {"", types.voidType()};
      }
      emitCall("", "void @free(ptr " + p.text + ")", false); // free(null) does nothing
      return {"", types.voidType()};
    }
    default:
      unsupported(e, std::string("the expression kind ") + nodeKindName(e->kind));
  }
}

auto codegen::Emitter::deallocationCall(FunctionDecl* f, const std::string& size, const Ty* t, const std::string& block, const std::vector<std::string>& extra, Node* at) -> std::string
{
  const Ty* in = global.owningClass(f) ? declaringClass(t, f, at) : nullptr;
  if (global.owningClass(f) && !in)
    unsupported(at, "the operator delete chosen for a class that does not declare it");
  std::string text = "void " + useFunction(f, in ? global.classArgsOf(in) : TemplateArgs{}, in, at) + "(ptr " + block;
  if (!size.empty())
    text += ", i64 " + size;
  for (const std::string& x : extra)
    text += ", " + x;
  return text + ")";
}

auto codegen::Emitter::classDeallocation(const Ty* cls, const Ty*& owner, bool& sized, Node* at, int depth) -> FunctionDecl*
{
  if (!cls || !isClass(cls) || depth > maxClassNesting)
    return nullptr;
  const Ty* key = canonicalClass(cls, at);
  ClassDecl* c = classDef(key->decl, at);
  if (!c)
    return nullptr;
  FunctionDecl* unsized = nullptr;
  FunctionDecl* withSize = nullptr;
  bool declared = false;
  for (Decl* m : c->members)
  {
    if (m->kind != NodeKind::FunctionDecl)
      continue;
    auto* f = static_cast<FunctionDecl*>(m);
    if (f->name.empty() || f->name.last().special != 0 || sym(f->name.last().symbol) != "operator delete")
      continue;
    declared = true;
    if (f->params.size() == 1 && !unsized)
      unsized = f;
    else if (f->params.size() == 2 && !withSize)
      withSize = f;
  }
  if (declared)
  {
    if (!unsized && !withSize)
      fail(at, "the operator delete of " + types.toString(key, &symbols) + ", which a virtual destructor's delete calls, has no usual form, operator delete(void*) or operator delete(void*, size_t)");
    owner = key;
    sized = !unsized;
    return unsized ? unsized : withSize;
  }
  FunctionDecl* found = nullptr;
  for (const Ty* b : basesOf(c, global.classArgsOf(key), at))
  {
    const Ty* from = nullptr;
    bool fromSized = false;
    if (FunctionDecl* f = classDeallocation(b, from, fromSized, at, depth + 1))
    {
      if (found && f != found)
        fail(at, "the operator delete of " + types.toString(key, &symbols) + " is ambiguous: more than one of its bases declares one");
      found = f;
      owner = from;
      sized = fromSized;
    }
  }
  return found;
}

auto codegen::Emitter::deleterFor(const Ty* key, const std::string& irName, std::string& definitions, Node* at) -> std::string
{
  const Ty* owner = nullptr;
  bool sized = false;
  FunctionDecl* f = classDeallocation(key, owner, sized, at);
  if (!f && !programAllocationFunctions())
    return "null";
  std::string name = "@\"delete." + irName + "\"";
  std::string call = f ? deallocationCall(f, sized ? std::to_string(sizeOf(key, at)) : "", owner, "%block", {}, at)
                       : globalFreeCall("%block", std::to_string(sizeOf(key, at)), false, at);
  definitions += "$" + name.substr(1) + " = comdat any\ndefine linkonce_odr void " + name + "(ptr %block) comdat {\nentry:\n  call " + call + "\n  ret void\n}\n";
  return name;
}

auto codegen::Emitter::templateParameterObject(Decl* d, Node* at) -> std::string
{
  const Ty* bound = nullptr;
  if (currentArgs)
    for (auto& [p, b] : currentArgs->pairs)
      if (p == d)
        bound = b;
  if (!bound || !bound->isObjectValue())
    return "";
  const Ty* cls = types.unqualified(bound->inner);
  std::optional<ConstValue> value = global.argumentObject(bound);
  std::optional<std::string> text = value ? constantText(*value, cls, at) : std::nullopt;
  if (!text)
    unsupported(at, "the object " + types.toString(bound, &symbols) + " given as a template argument");
  std::string name = "@\"template.object." + types.toString(bound, &symbols) + "\"";
  if (declaredNames.claim(name))
    decls += "$" + name.substr(1) + " = comdat any\n" + name + " = linkonce_odr constant " + llType(cls, at) + " " + *text + ", comdat\n";
  return name;
}

auto codegen::Emitter::templateParameterClass(Decl* d) -> const Ty*
{
  if (currentArgs)
    for (auto& [p, b] : currentArgs->pairs)
      if (p == d && b && b->isObjectValue())
        return types.unqualified(b->inner);
  return nullptr;
}

auto codegen::Emitter::programAllocationFunctions() -> bool
{
  if (programAllocates < 0)
    programAllocates = global.programAllocationFunctions() ? 1 : 0;
  return programAllocates == 1;
}

auto codegen::Emitter::globalFreeCall(const std::string& block, const std::string& size, bool array, Node* at) -> std::string
{
  if (!programAllocationFunctions())
  {
    ensureMalloc();
    return "void @free(ptr " + block + ")";
  }
  FunctionDecl* f = size.empty() ? nullptr : global.globalDeallocation(array, true);
  std::string passed = f ? size : "";
  if (!f)
    f = global.globalDeallocation(array, false);
  if (!f)
    unsupported(at, std::string("no global ") + (array ? "operator delete[]" : "operator delete") + " to give the block back with");
  return deallocationCall(f, passed, nullptr, block, {}, at);
}

auto codegen::Emitter::bitfieldNamed(Expr* e) -> const BitfieldInfo*
{
  while (e && e->kind == NodeKind::ParenExpr)
    e = static_cast<ParenExpr*>(e)->inner;
  if (e && lvalueBitfield.first == e && lvalueBitfield.second)
    return lvalueBitfield.second;
  Decl* d = !e ? nullptr : e->kind == NodeKind::MemberExpr ? static_cast<MemberExpr*>(e)->resolved
                         : e->kind == NodeKind::NameExpr    ? static_cast<NameExpr*>(e)->resolved
                                                            : nullptr;
  if (d && d->kind == NodeKind::VariableDecl && static_cast<VariableDecl*>(d)->bitfield)
    unsupported(e, "a bit-field whose place in its class is not known"); // never its whole storage unit
  return nullptr;
}

auto codegen::Emitter::enterBranchBesideThrow(ConditionalExpr* c) -> Expr*
{
  // `x > 0 ? x : throw x`: a branch that is a throw-expression leaves, and the result is the other
  // branch's (C++'s [expr.cond] 2). The throw's block ends unreachable; the code goes on in the other's.
  Expr* beside = branchBesideThrow(c);
  if (!beside)
    return nullptr;
  Value cond = convert(rvalue(c->cond), types.boolType(), c, false, ConversionContext::Direct);
  std::string on = newLabel("cond.on"), leaves = newLabel("cond.throw");
  bool thenThrows = beside == c->else_;
  fn += "  br i1 " + cond.text + ", label %" + (thenThrows ? leaves : on) + ", label %" + (thenThrows ? on : leaves) + "\n";
  // (What the throw's operand made is not made on the way on, and what the condition made is still there: the
  // throw destroys the statement's temporaries on its way out (emitThrow's destroyTemps), which left the list empty,
  // and a resize grew it again with empty records, kpp's crash on `c ? 1 : throw 2` where c made an object with a
  // destructor, until 2026-11-08: thread J's expression generator.)
  std::vector<Object> before(statementTemps.begin(), statementTemps.end());
  startBlock(leaves);
  rvalue(thenThrows ? c->then : c->else_);
  statementTemps = std::move(before);
  fn += "  unreachable\n";
  startBlock(on);
  return beside;
}

auto codegen::Emitter::conditionalOfBranches(ConditionalExpr* c) -> Value
{
  Value cond = convert(rvalue(c->cond), types.boolType(), c, false, ConversionContext::Direct);
  std::string tL = newLabel("cond.then"), eL = newLabel("cond.else"), jL = newLabel("cond.end");
  fn += "  br i1 " + cond.text + ", label %" + tL + ", label %" + eL + "\n";
  size_t temps = statementTemps.size();
  // Each branch's value kept in a slot of its own type, the two met where they join, converted
  // there to their common type: which branch ran picks one (no conversion has side effects).
  auto branch = [&](Expr* x, const std::string& label, std::string& slot, std::string& end) -> Value
  {
    startBlock(label);
    Value v = bitfieldOperand(x, rvalue(x));
    keepOperandTemps(temps);
    const Ty* vt = v.ty ? types.unqualified(types.stripRef(v.ty)) : nullptr;
    if (!vt || !vt->isKnown() || isClass(vt) || vt->kind == TyKind::Array)
      unsupported(c, "a conditional whose type the checker could not tell, with a branch of " + (vt ? types.toString(vt, &symbols) : std::string("unknown type")));
    if (vt->kind != TyKind::Void)
    {
      slot = "%condbranch" + std::to_string(temp++);
      allocas += "  " + slot + " = alloca " + llType(vt, c) + "\n";
      fn += "  store " + llType(vt, c) + " " + v.text + ", ptr " + slot + "\n";
    }
    end = currentBlock;
    emitBranch(jL);
    return {v.text, vt};
  };
  std::string slotA, slotB, aEnd, bEnd;
  Value a = branch(c->then, tL, slotA, aEnd);
  Value b = branch(c->else_, eL, slotB, bEnd);
  startBlock(jL);
  if (a.ty->kind == TyKind::Void || b.ty->kind == TyKind::Void)
    return {"", types.voidType()};
  auto arithmeticOrEnum = [](const Ty* t) { return t->isArithmetic() || (t->kind == TyKind::Enum && !static_cast<EnumDecl*>(t->decl)->isClass); };
  const Ty* ct = nullptr;
  if (a.ty == b.ty)
    ct = a.ty;
  else if (arithmeticOrEnum(a.ty) && arithmeticOrEnum(b.ty))
    ct = commonArithmetic(a.ty, b.ty); // ([expr.cond] 7.2, the usual arithmetic conversions)
  else if (a.ty->kind == TyKind::Nullptr && b.ty->kind == TyKind::Pointer)
    ct = b.ty;
  else if (b.ty->kind == TyKind::Nullptr && a.ty->kind == TyKind::Pointer)
    ct = a.ty;
  else if (a.ty->kind == TyKind::Pointer && b.ty->kind == TyKind::Pointer)
    ct = impliedPointerConversion(b.ty, a.ty, c) ? a.ty : impliedPointerConversion(a.ty, b.ty, c) ? b.ty : nullptr;
  if (!ct)
    unsupported(c, "a conditional of " + types.toString(a.ty, &symbols) + " and " + types.toString(b.ty, &symbols) + " whose type the checker could not tell");
  std::string took = newTemp();
  fn += "  " + took + " = phi i1 [ true, %" + aEnd + " ], [ false, %" + bEnd + " ]\n";
  Value ca = convert(load(slotA, a.ty, c), ct, c), cb = convert(load(slotB, b.ty, c), ct, c);
  std::string r = newTemp();
  fn += "  " + r + " = select i1 " + took + ", " + llType(ct, c) + " " + ca.text + ", " + llType(ct, c) + " " + cb.text + "\n";
  return {r, ct};
}

auto codegen::Emitter::bitfieldOperand(Expr* e, Value v) -> Value
{
  const Ty* t = v.ty ? types.unqualified(types.stripRef(v.ty)) : nullptr;
  if (!t || (t->kind != TyKind::Int && t->kind != TyKind::Char) || t->bits < 32)
    return v; // (a narrower type's field promotes as its type does, an enumeration's and a bool's too)
  Expr* field = bitFieldDesignator(e);
  uint32_t width = 0;
  if (field)
  {
    if (lvalueBitfield.first != field || !lvalueBitfield.second)
      unsupported(e, "a bit-field operand whose width is not known here"); // (its value just read through its place)
    width = lvalueBitfield.second->width;
  }
  else if (const BitfieldInfo* bits = bindingBitfield(bindingDesignator(e)))
    width = bits->width; // (a structured binding of a bit field is that field: `auto [a] = s;`, a of `uint64_t w3 : 3`)
  else
    return v;
  if (width > 32)
    return v;
  return convert(v, width < 32 || !t->isUnsigned ? types.intType(32) : types.intType(32, true), e, true);
}

auto codegen::Emitter::ensureMalloc() -> void
{
  if (mallocKnown)
    return;
  mallocKnown = true;
  auto lock = sharedLock();
  if (mallocDeclared)
    return;
  mallocDeclared = true;
  if (declaredNames.claim("@malloc"))
    decls += "declare ptr @malloc(i64)\n";
  if (declaredNames.claim("@free"))
    decls += "declare void @free(ptr)\n";
}

auto codegen::Emitter::allocationFailure(Node* at, const std::string& stop) -> void
{
  throwFromLibrary("throwBadAlloc", at, stop);
}

auto codegen::Emitter::throwFromLibrary(const char* name, Node* at, const std::string& stop) -> void
{
  // The library's function (its names are reserved: no program function takes its place).
  FunctionDecl* thrower = global.libraryFunction(name);
  if (thrower && thrower->file && thrower->file->isLibrary && thrower->params.empty())
  {
    emitCall("", "void " + useFunction(thrower, {}, nullptr, at) + "()", true);
    fn += "  unreachable\n";
    return;
  }
  auto [line, column] = lineColumnOf(at->token);
  emitCall("", "void @kpp_assert_failed(ptr " + cStringConstant(programPathOf(currentFile)) + ", i32 " + lineOperand(line) + ", i32 " +
                   std::to_string(column) + ", ptr " + cStringConstant(stop) + ")", false);
  fn += "  unreachable\n";
  usesAssert = true;
}

auto codegen::Emitter::missingOperator(Node* at, const Ty* cls, const std::string& name) -> void
{
  std::string className = qualifiedName(cls->decl);
  if (name == "operator+=" && (global.isLibraryClass(cls, "String") || global.isLibraryClass(cls, "WString")))
    fail(at, className + " has no operator+=: `<<` appends to a " + className + " (docs/stdlib/text.md; design.md 14)");
  unsupported(at, className + " has no " + name);
}

auto codegen::Emitter::checkAllocation(const std::string& block, Node* at) -> void
{
  // (C++'s operator new throws std::bad_alloc when it cannot have the block, [basic.stc.dynamic.allocation]
  // 3; until 2026-11-06 the object was built at the null pointer malloc gave.)
  std::string isNull = newTemp(), okL = newLabel("new.ok"), failL = newLabel("new.fail");
  fn += "  " + isNull + " = icmp eq ptr " + block + ", null\n";
  fn += "  br i1 " + isNull + ", label %" + failL + ", label %" + okL + "\n";
  startBlock(failL);
  allocationFailure(at, "new could not have the block it needs: malloc gave none");
  startBlock(okL);
}

auto codegen::Emitter::arrayCookie(const Ty* t, Node* at, bool sizedDelete) -> uint64_t
{
  if (!needsDestruction(t, at) && !sizedDelete)
    return 0;
  const Ty* inner = t;
  while (inner->kind == TyKind::Array)
    inner = inner->inner;
  uint64_t align = isClass(inner) ? classSizeAndAlign(inner, at).second : sizeOf(inner, at);
  if (align > 16)
    unsupported(at, "new[] of " + types.toString(t, &symbols) + ", aligned to " + std::to_string(align) + " bytes (more than malloc's 16)");
  return align > 8 ? 16 : 8;
}

auto codegen::Emitter::arrayNew(NewExpr* n, const Ty* t, Expr* sizeExpr) -> std::string
{
  Span<Expr*> list;
  bool valueInit = n->parenthesized;
  if (n->braced)
  {
    list = static_cast<BraceInitExpr*>(n->args[0])->args;
    valueInit = true;
  }
  else if (!n->args.empty())
  {
    // `new int[3](8, 9)` (C++20): the values initialize the first elements, each one whole (the checker
    // refused one that is no element), the rest value-initialized.
    list = n->args;
    valueInit = true;
  }
  // The elements the list initializes: one for each value, or for each run of values an element
  // takes with its braces elided (`new In[2]{1, 2, 3}` of `struct In { int a; int b; }`: {1, 2} and
  // {3, 0}, [dcl.init.aggr] 16; D, 2026-11-06: it was refused as three initializers for two).
  std::vector<std::pair<size_t, size_t>> runs; // each element's first value and how many it takes
  for (size_t k = 0; k < list.size();)
  {
    size_t take = initializesWhole(t, list[k], n) ? 1 : std::max<size_t>(1, elidedValues(t, list, k, n));
    runs.push_back({k, take});
    k += take;
  }
  // A count known here that is negative or below the elements the list initializes is C++'s
  // error (`new int[2]{1, 2, 3}` wrote past its block until 2026-11-05); one equal to it leaves no
  // rest to build (`new T[3]{a, b, c}` of a class with no default constructor).
  std::optional<int64_t> known = evalConst(sizeExpr);
  if (known && *known < 0)
    fail(n, "new[] of a negative count (" + std::to_string(*known) + ")");
  if (known && uint64_t(*known) < runs.size())
    fail(n, "new[] of " + std::to_string(*known) + " elements given " + std::to_string(runs.size()) + " initializers");
  std::string count = convert(rvalue(sizeExpr), types.sizeType(), n).text;
  std::string stride = "ptrtoint (ptr getelementptr (" + llType(t, n) + ", ptr null, i32 1) to i64)"; // LLVM's own element size
  uint64_t cookie = arrayCookie(t, n, n->allocator && n->sizedDeallocation);
  // A constant count whose block's size would not fit (C++'s ill-formed new-expression, [expr.new]
  // 9), never a size that wrapped: `new int[0x4000000000000001]` asked for 4 bytes.
  if (known && uint64_t(*known) > (uint64_t(INT64_MAX) - cookie) / std::max<uint64_t>(sizeOf(t, n), 1))
    fail(n, "new[] of " + std::to_string(*known) + " elements of " + types.toString(t, &symbols) + ": more bytes than a block can have");
  if (!known)
  {
    // A count that is negative (a huge size_t), too large for the block's size to be computed, or
    // below the list's length: C++ throws bad_array_new_length, a bad_alloc, and kpp BadAlloc
    // (allocationFailure), never a size that wrapped and a block too small.
    uint64_t element = std::max<uint64_t>(sizeOf(t, n), 1);
    std::string bad = newTemp();
    fn += "  " + bad + " = icmp ugt i64 " + count + ", " + std::to_string((uint64_t(INT64_MAX) - cookie) / element) + "\n";
    if (!list.empty())
    {
      std::string fewer = newTemp(), either = newTemp();
      fn += "  " + fewer + " = icmp ult i64 " + count + ", " + std::to_string(runs.size()) + "\n";
      fn += "  " + either + " = or i1 " + bad + ", " + fewer + "\n";
      bad = either;
    }
    std::string okL = newLabel("newarray.ok"), failL = newLabel("newarray.fail");
    fn += "  br i1 " + bad + ", label %" + failL + ", label %" + okL + "\n";
    startBlock(failL);
    allocationFailure(n, list.empty() ? "new[]'s count is negative or too large" : "new[]'s count is negative, too large or below its initializer list's length");
    startBlock(okL);
  }
  ensureMalloc();
  std::string bytes = newTemp();
  fn += "  " + bytes + " = mul i64 " + count + ", " + stride + "\n";
  std::string total = bytes;
  if (cookie)
  {
    total = newTemp();
    fn += "  " + total + " = add i64 " + bytes + ", " + std::to_string(cookie) + "\n";
  }
  std::string block;
  std::vector<std::string> placed; // the placement arguments as the class's operator new[] took them
  if (n->allocator)
  {
    // The class's own operator new[], or a global one the program declares, with the size, the
    // cookie's included, and the placement arguments (the type checker's choice, Walker::chooseAllocation).
    const Ty* in = global.owningClass(n->allocator) ? declaringClass(t, n->allocator, n) : nullptr;
    if (global.owningClass(n->allocator) && !in)
      unsupported(n, "the operator new[] chosen for a class that does not declare it");
    block = callFunction(n->allocator, in ? global.classArgsOf(in) : TemplateArgs{}, in, "", "", n->placement, n, "", {"i64 " + total}, &placed).text;
  }
  else
  {
    block = newTemp();
    emitCall(block + " = ", "ptr @malloc(i64 " + total + ")", false);
    checkAllocation(block, n);
  }
  std::string base = block;
  if (cookie)
  {
    base = newTemp();
    fn += "  " + base + " = getelementptr inbounds i8, ptr " + block + ", i64 " + std::to_string(cookie) + "\n";
    std::string countAddr = newTemp();
    fn += "  " + countAddr + " = getelementptr inbounds i8, ptr " + base + ", i64 -8\n";
    fn += "  store i64 " + count + ", ptr " + countAddr + "\n";
  }
  // An element's constructor or an initializer of the list that throws leaves with the elements
  // built so far destroyed, the last first, and the block freed ([expr.new] 26), as one object's
  // new frees its memory: the elements built are counted in an i64 slot, which the loop over the
  // rest uses as its index (until 2026-11-05 they were never destroyed, the block never freed).
  size_t mark = statementTemps.size();
  Object allocation{block, t};
  allocation.unwindOnly = true;
  allocation.freeMemory = true;
  if (n->deallocator)
    allocation.freeCall = deallocationCall(n->deallocator, n->sizedDeallocation && n->placement.empty() ? total : "", t, block, placed, n);
  else if (programAllocationFunctions() && n->placement.empty())
    allocation.freeCall = globalFreeCall(block, cookie ? total : "", true, n);
  bool released = !n->allocator || n->deallocator || n->placement.empty(); // (a placement form without its operator delete[] gives nothing back)
  if (released)
    statementTemps.push_back(allocation);
  std::string builtSlot = "%v" + std::to_string(temp++);
  allocas += "  " + builtSlot + " = alloca i64\n";
  fn += "  store i64 0, ptr " + builtSlot + "\n";
  bool counted = needsDestruction(t, n);
  if (counted)
  {
    Object elements{base, types.array(t, 0)};
    elements.unwindOnly = true;
    elements.countSlot = builtSlot;
    statementTemps.push_back(elements);
  }
  bool scalar = !isClass(t) && t->kind != TyKind::Array && !global.enumWithConstructors(t); // (a kpp enum's elements: its default constructor)
  if (scalar && valueInit)
  {
    if (declaredNames.claim("@llvm.memset.p0.i64"))
      decls += "declare void @llvm.memset.p0.i64(ptr, i8, i64, i1)\n";
    fn += "  call void @llvm.memset.p0.i64(ptr " + base + ", i8 0, i64 " + bytes + ", i1 false)\n";
  }
  for (size_t i = 0; i < runs.size(); ++i)
  {
    std::string element = newTemp();
    fn += "  " + element + " = getelementptr inbounds " + llType(t, n) + ", ptr " + base + ", i64 " + std::to_string(i) + "\n";
    auto [first, take] = runs[i];
    if (take == 1 && initializesWhole(t, list[first], n))
      initializeFrom(element, t, list[first], n);
    else
    {
      if (!isClass(t) && t->kind != TyKind::Array)
        unsupported(n, "brace elision into a new[] element of " + types.toString(t, &symbols));
      storeZero(t, element, n);
      initializeFromElided(element, t, Span<Expr*>{list.begin() + first, uint32_t(take)}, n);
    }
    if (counted)
      fn += "  store i64 " + std::to_string(i + 1) + ", ptr " + builtSlot + "\n";
  }
  bool rest = !known || uint64_t(*known) > runs.size();
  if (rest && !scalar && (valueInit || needsConstruction(t, n)))
  {
    fn += "  store i64 " + std::to_string(runs.size()) + ", ptr " + builtSlot + "\n";
    std::string loopL = newLabel("array.loop"), bodyL = newLabel("array.body"), endL = newLabel("array.end");
    emitBranch(loopL);
    startBlock(loopL);
    std::string index = newTemp(), done = newTemp();
    fn += "  " + index + " = load i64, ptr " + builtSlot + "\n";
    fn += "  " + done + " = icmp uge i64 " + index + ", " + count + "\n";
    fn += "  br i1 " + done + ", label %" + endL + ", label %" + bodyL + "\n";
    startBlock(bodyL);
    std::string element = newTemp();
    fn += "  " + element + " = getelementptr inbounds " + llType(t, n) + ", ptr " + base + ", i64 " + index + "\n";
    if (isClass(t))
      defaultConstruct(element, t, n);
    else if (global.enumWithConstructors(t))
      constructEnum(element, t, {}, n);
    else
      constructArray(element, t, n);
    std::string next = newTemp();
    fn += "  " + next + " = add i64 " + index + ", 1\n";
    fn += "  store i64 " + next + ", ptr " + builtSlot + "\n";
    emitBranch(loopL);
    startBlock(endL);
  }
  statementTemps.erase(statementTemps.begin() + mark, statementTemps.begin() + mark + (counted ? 1 : 0) + (released ? 1 : 0));
  return base;
}

auto codegen::Emitter::arrayDelete(const std::string& p, const Ty* t, Node* at, FunctionDecl* dealloc, bool sized) -> void
{
  uint64_t cookie = arrayCookie(t, at, dealloc && sized);
  if (!cookie && !dealloc && !programAllocationFunctions())
  {
    emitCall("", "void @free(ptr " + p + ")", false); // free(null) does nothing
    return;
  }
  if (!cookie)
  {
    // The class's own operator delete[], not called for a null pointer (as clang has it).
    std::string isNull = newTemp(), callL = newLabel("delete.array"), endL = newLabel("delete.end");
    fn += "  " + isNull + " = icmp eq ptr " + p + ", null\n";
    fn += "  br i1 " + isNull + ", label %" + endL + ", label %" + callL + "\n";
    blockTerminated = true;
    startBlock(callL);
    emitCall("", dealloc ? deallocationCall(dealloc, "", t, p, {}, at) : globalFreeCall(p, "", true, at), false);
    emitBranch(endL);
    startBlock(endL);
    return;
  }
  std::string isNull = newTemp(), arrayL = newLabel("delete.array"), endL = newLabel("delete.end");
  fn += "  " + isNull + " = icmp eq ptr " + p + ", null\n";
  fn += "  br i1 " + isNull + ", label %" + endL + ", label %" + arrayL + "\n";
  blockTerminated = true;
  startBlock(arrayL);
  std::string countAddr = newTemp(), count = newTemp();
  fn += "  " + countAddr + " = getelementptr inbounds i8, ptr " + p + ", i64 -8\n";
  fn += "  " + count + " = load i64, ptr " + countAddr + "\n";
  forEachElement(p, t, "0", count, true, at, [&](const std::string& element) { destroy(element, t, at); });
  std::string block = newTemp();
  fn += "  " + block + " = getelementptr inbounds i8, ptr " + p + ", i64 -" + std::to_string(cookie) + "\n";
  if (dealloc)
  {
    std::string size;
    if (sized)
    {
      // The size operator new[] was asked for: the elements' and the cookie's.
      std::string bytes = newTemp();
      size = newTemp();
      fn += "  " + bytes + " = mul i64 " + count + ", " + std::to_string(sizeOf(t, at)) + "\n";
      fn += "  " + size + " = add i64 " + bytes + ", " + std::to_string(cookie) + "\n";
    }
    emitCall("", deallocationCall(dealloc, size, t, block, {}, at), false);
  }
  else if (programAllocationFunctions())
  {
    // The global operator delete[] with the size operator new[] was asked for.
    std::string bytes = newTemp(), size = newTemp();
    fn += "  " + bytes + " = mul i64 " + count + ", " + std::to_string(sizeOf(t, at)) + "\n";
    fn += "  " + size + " = add i64 " + bytes + ", " + std::to_string(cookie) + "\n";
    emitCall("", globalFreeCall(block, size, true, at), false);
  }
  else
    emitCall("", "void @free(ptr " + block + ")", false);
  emitBranch(endL);
  startBlock(endL);
}

auto codegen::Emitter::unary(UnaryExpr* u) -> Value
{
  if (u->isKeyword)
  {
    if (Kw(u->op) == Kw::not_)
    {
      Value v = convert(rvalue(u->operand), types.boolType(), u, false, ConversionContext::Direct);
      std::string r = newTemp();
      fn += "  " + r + " = xor i1 " + v.text + ", true\n";
      return {r, types.boolType()};
    }
    unsupported(u, "this keyword operator");
  }
  P op = P(u->op);
  if (op == P::Amp && u->ty && types.stripRef(u->ty)->kind == TyKind::MemberPointer)
  {
    // `&Style::field`: the field's offset within the class.
    auto* n = static_cast<NameExpr*>(u->operand);
    Decl* d = n->resolved;
    const Ty* mp = types.stripRef(u->ty);
    // (`&Proto::read` in a member of the class template: this instance's class, not the
    // template's own parameters the shared expression was typed with.)
    const Ty* cls = mp->args.empty() ? nullptr : withCurrentArgs(mp->args[0]);
    if (cls && types.isDependent(cls) && n->name.parts.size() > 1)
    {
      // `&T::luaNewIndex` in LuaBinder<T>: typed with the declaring template's own parameters
      // (LuaPrototypeTemplate<TemplateType, ...>), which this instance does not bind; the
      // qualifier under this instance's arguments names the class.
      QualifiedName qualifier = n->name;
      qualifier.parts.count -= 1;
      NamedType tmp{};
      tmp.kind = NodeKind::NamedType;
      tmp.token = n->token;
      tmp.name = qualifier;
      TemplateArgs none;
      if (const Ty* q = global.typeOfAstIn(&tmp, typingContext(), currentArgs ? *currentArgs : none); q && isClass(q) && !types.isDependent(q))
        cls = q;
    }
    if (d && d->kind == NodeKind::FunctionDecl && cls && isClass(cls))
    {
      // `&Calc::mul`: the function itself; a call through it passes the object first. A virtual
      // function, or one of a base at another place in the object, goes through a thunk that
      // calls the object's override, or moves `this` to that base.
      auto* f = static_cast<FunctionDecl*>(d);
      const Ty* key = canonicalClass(cls, u);
      const Ty* owner = key;
      if (Decl* ownerDecl = global.owningClass(f); ownerDecl && ownerDecl->kind == NodeKind::ClassDecl && classDef(ownerDecl, u) != classDef(key->decl, u))
        if (const Ty* base = baseInstantiation(key, ownerDecl, u))
          owner = base;
      std::vector<std::pair<std::string, uint32_t>> path;
      if (owner != key)
        basePath(key, owner, path, u);
      bool moved = false;
      for (auto& [structName, index] : path)
        moved = moved || index != 0;
      int slot = f->templateParams.empty() ? slotOf(owner, f, u) : -1;
      if (hasBaseAtOffset(key, u))
      {
        // The multiple model: the function as its own class's (a virtual one through that class's
        // thunk reading the object's vtable), `this` moved to that class by the adjustment.
        if (!f->templateParams.empty() && moved)
          unsupported(u, "the address of a member function template of a base at another place in the object");
        std::string function;
        if (slot >= 0)
          function = memberFunctionThunk(owner, owner, {}, f, slot, u);
        else
        {
          TemplateArgs fargs = global.classArgsOf(owner);
          if (!f->templateParams.empty())
            addressedTemplateArgs(f, n, u, fargs);
          function = useFunction(f, fargs, owner, u);
        }
        std::optional<uint64_t> offset = owner == key ? std::optional<uint64_t>(0) : baseOffsetIn(key, owner, u);
        if (!offset)
          unsupported(u, "a pointer to a member function of " + types.toString(owner, &symbols) + " as one of " + types.toString(key, &symbols));
        // (typed as key's: `&T::luaIndex` in LuaBinder<T> was typed with its declaring template's own
        // parameters, whose representation is not known)
        const Ty* typed = types.isDependent(mp) ? types.memberPointer(withCurrentArgs(mp->inner), key) : mp;
        return {memberFunctionValue(function, std::to_string(*offset)), typed};
      }
      if (slot < 0 && !moved)
      {
        // `&Bindable::debugChildren<Binder<T>>`: a member template's instance, its own
        // arguments with its class's.
        TemplateArgs fargs = global.classArgsOf(owner);
        if (!f->templateParams.empty())
          addressedTemplateArgs(f, n, u, fargs);
        return {useFunction(f, fargs, owner, u), mp};
      }
      if (!f->templateParams.empty())
        unsupported(u, "the address of a member function template of a base at another place in the object");
      return {memberFunctionThunk(key, owner, path, f, slot, u), mp};
    }
    if (!d || d->kind != NodeKind::VariableDecl || !cls || !isClass(cls))
      unsupported(u, "a pointer to a member that is neither a field nor a function");
    std::string addr;
    if (!fieldAddress(cls, d, "null", addr, u))
      unsupported(u, "a pointer to a member that is not a field");
    std::string r = newTemp();
    fn += "  " + r + " = ptrtoint ptr " + addr + " to " + memberOffsetType() + "\n";
    return {r, mp};
  }
  if (op == P::Amp && u->operand->kind == NodeKind::NameExpr && static_cast<NameExpr*>(u->operand)->resolved &&
      static_cast<NameExpr*>(u->operand)->resolved->kind == NodeKind::FunctionDecl)
  {
    // `&function`: the function itself is the pointer, noexcept as this instance of it is.
    Value function = rvalue(u->operand);
    bool known = function.ty && function.ty->kind == TyKind::Pointer && function.ty->inner && function.ty->inner->kind == TyKind::Function && !types.isDependent(function.ty);
    return {function.text, known ? function.ty : u->ty};
  }
  if (op == P::Amp)
  {
    // `&x` of a class with its own operator& (CComPtr's): the call, as the resolver typed it;
    // the built-in address where the class has none, whose result is a pointer to it.
    const Ty* ot = u->operand->ty ? types.stripRef(u->operand->ty) : nullptr;
    if (ot && isClass(ot))
    {
      const Ty* cls = types.unqualified(ot);
      const Ty* foundIn = nullptr;
      if (findMemberFunction(cls, operatorName(P::Amp), {}, cls, u, &foundIn, true))
        return callRvalue(operatorCall(rvalue(u->operand), operatorName(P::Amp), {}, u, true), u);
      const Ty* rt = u->ty ? types.stripRef(u->ty) : nullptr;
      if (!rt || rt->kind != TyKind::Pointer || !rt->inner || types.unqualified(rt->inner) != cls)
        unsupported(u, "a unary operator& that is no member function");
    }
    return {lvalue(u->operand), u->ty};
  }
  if (op == P::Star)
  {
    // `*g` with a free operator*(const G&), the one the checker chose.
    if (FunctionDecl* chosen = isClass(types.stripRef(u->operand->ty)) && !u->calleeMember ? recordedOperator(u) : nullptr)
    {
      Expr* operand = u->operand;
      return callChosenOperator(u, chosen, false, Span<Expr*>{&operand, 1});
    }
    Value p = rvalue(u->operand);
    if (isClass(p.ty))
    {
      // No operator* of its own: the class's conversion to a pointer, dereferenced
      // (`*this->undoFiller` through UndoItemFiller's `operator UndoItem*()`).
      const Ty* foundIn = nullptr;
      if (!findMemberFunction(p.ty, operatorName(P::Star), {}, p.ty, u, &foundIn, true))
        if (std::optional<Value> pointer = toPointer(p, u))
          p = *pointer;
      if (isClass(p.ty))
        return callRvalue(operatorCall(p, operatorName(P::Star), {}, u, true), u);
    }
    if (p.ty->kind != TyKind::Pointer)
      unsupported(u, "* on something that is not a pointer");
    return load(p.text, p.ty->inner, u);
  }
  if (op == P::PlusPlus || op == P::MinusMinus)
  {
    const Ty* ot = types.stripRef(u->operand->ty);
    // `++g`, `g++` with free operators, the ones the checker chose: as an enumeration's, below.
    bool freeOnClass = isClass(ot) && !u->calleeMember && recordedOperator(u);
    if (isClass(ot) && !freeOnClass)
    {
      Value o = rvalue(u->operand);
      std::string opName = operatorName(op);
      if (u->postfix)
      {
        const Ty* foundIn = nullptr;
        std::vector<const Ty*> intArg = {types.intType(32)};
        FunctionDecl* pf = findMemberFunction(o.ty, opName, intArg, conversionObject(o), u, &foundIn, true);
        // No postfix operator++(int): an error, as in C++. (Until 2026-11-05 the prefix one was
        // called, MSVC's extension: `C d = c++;` gave d the incremented value.)
        if (!pf)
          fail(u, types.toString(o.ty, &symbols) + " has no postfix " + opName + " (" + opName + "(int)), which x" +
                      (op == P::PlusPlus ? "++" : "--") + " calls: C++ never takes the prefix one for it");
        std::string thisArg = o.text;
        if (canonicalClass(o.ty, u) != foundIn)
        {
          std::string addr;
          if (baseAddress(o.ty, foundIn, o.text, addr, u))
            thisArg = addr;
        }
        TemplateArgs fnArgs = global.classArgsOf(foundIn);
        std::string fname = useFunction(pf, fnArgs, foundIn);
        Signature s;
        const Ty* ret;
        {
          ArgsScope scope(*this, &fnArgs);
          s = signatureOf(pf);
          ret = declType(pf)->inner;
        }
        std::string argText = "ptr " + thisArg;
        std::string resultAddr;
        if (s.hasSret)
        {
          resultAddr = newObjectSlot(ret, u);
          argText += ", ptr " + resultAddr;
        }
        argText += ", i32 0";
        std::string sig = s.returnType + " ";
        if (s.returnType == "void")
        {
          emitCall("", sig + fname + "(" + argText + ")");
          if (s.hasSret)
            statementTemps.push_back({resultAddr, ret}); // not before: a call that throws built no result to destroy
          return s.hasSret ? callRvalue({resultAddr, ret, true}, u) : Value{"", types.voidType(), false};
        }
        std::string r = newTemp();
        emitCall(r + " = ", sig + fname + "(" + argText + ")");
        return callRvalue({r, ret, false}, u);
      }
      return callRvalue(operatorCall(o, opName, {}, u, true), u);
    }
    // `++direction`, `direction++` with an operator function declared for the enumeration: the one
    // the checker chose (Walker::enumUnaryOperator), never the built-in increment, which C++ does
    // not even have for an enumeration (language/enum_operand_calls_user_defined_operator).
    if (ot && (ot->kind == TyKind::Enum || freeOnClass))
    {
      FunctionDecl* chosen = verifyDecisions() ? nullptr : recordedOperator(u);
      if (verifyDecisions() && !freeOnClass)
      {
        chosen = global.findFreeOperator(uint32_t(op), argumentType(u->operand, u->operand->ty, types), u->postfix ? types.intType(32) : nullptr, currentFunction);
        verifyOperatorCall(u, chosen ? OperatorCall::Function : OperatorCall::Builtin, chosen, false);
      }
      if (chosen && !u->calleeMember)
      {
        Expr* operand = u->operand;
        if (!u->postfix)
          return callChosenOperator(u, chosen, false, Span<Expr*>{&operand, 1});
        // `operator++(Direction&, int)`: the operand's address, and 0.
        const Ty* ft = declType(chosen);
        if (!ft || ft->kind != TyKind::Function || ft->args.size() != 2 || ft->args[0]->kind != TyKind::LRef || !chosen->templateParams.empty())
          unsupported(u, "a postfix " + std::string(punctuatorSpelling(op)) + " whose operator function " + qualifiedName(chosen) +
                           " takes no reference to the operand and an int");
        TemplateArgs none;
        std::string fname = useFunction(chosen, none, nullptr, u);
        Signature s = signatureOf(chosen);
        const Ty* ret = ft->inner;
        std::string argText, resultAddr;
        if (s.hasSret)
        {
          resultAddr = newObjectSlot(ret, u);
          argText = "ptr " + resultAddr + ", ";
        }
        argText += "ptr " + lvalue(u->operand) + ", " + llType(ft->args[1], u) + " 0";
        if (s.returnType == "void")
        {
          emitCall("", s.returnType + " " + fname + "(" + argText + ")");
          if (s.hasSret)
            statementTemps.push_back({resultAddr, ret}); // (after the call: one that throws built no result)
          return s.hasSret ? callRvalue({resultAddr, ret, true}, u) : Value{"", types.voidType(), false};
        }
        std::string r = newTemp();
        emitCall(r + " = ", s.returnType + " " + fname + "(" + argText + ")");
        return callRvalue({r, ret, false}, u);
      }
    }
    std::string addr = lvalue(u->operand);
    prefixTarget = addr;
    // A bit-field: its own bits of the storage unit, read and written back as an assignment
    // does them, a prefix one's value read back (wrapped to the field's width).
    const BitfieldInfo* bits = bitfieldNamed(u->operand);
    Value old = bits ? loadBitfield(addr, *bits, ot, u) : load(addr, ot, u);
    Value updated = arithmetic(op == P::PlusPlus ? P::Plus : P::Minus, old, Value{"1", types.intType(32)}, u);
    if (bits)
    {
      storeBitfield(convert(updated, ot, u), addr, *bits, u);
      return u->postfix ? old : loadBitfield(addr, *bits, ot, u);
    }
    store(updated, addr, ot, u);
    return u->postfix ? old : convert(updated, ot, u);
  }
  // A free `Coin operator-(const Coin&)`, a class's friend operator among them, when the class
  // has no member operator: called with the operand as written.
  bool operatorLooked = false, noOperator = false; // (the class's member and free operators, looked up here)
  // The operator function the checker chose (design.md 24); KPP_VERIFY_DECISIONS keeps codegen's
  // own choice below, to compare.
  if (FunctionDecl* chosen = verifyDecisions() ? nullptr : recordedOperator(u))
  {
    Expr* operand = u->operand;
    return callChosenOperator(u, chosen, u->calleeMember, Span<Expr*>{&operand, 1});
  }
  if (const Ty* ct = u->operand->ty && u->operand->ty->isKnown() ? types.unqualified(types.stripRef(u->operand->ty)) : nullptr; ct && isClass(ct))
  {
    const Ty* in = nullptr;
    const Ty* operand = argumentType(u->operand, u->operand->ty, types);
    operatorLooked = true;
    // On the object's const as the checker typed it (`~blankPosition` of a const one captured by
    // reference: `const BaseClass& operator~() const`).
    const Ty* object = constObject(u->operand) ? types.qualified(ct, true, ct->isVolatile) : ct;
    if (FunctionDecl* member = findMemberFunction(ct, operatorName(op), {}, object, u, &in))
      verifyOperatorCall(u, OperatorCall::Function, member, true);
    else
    {
      noOperator = true;
      if (FunctionDecl* f = global.findFreeOperator(uint32_t(op), operand, nullptr, currentFunction))
      {
        verifyOperatorCall(u, OperatorCall::Function, f, false);
        const Ty* classTy = friendClass(f, {operand}, u);
        TemplateArgs fnArgs = classTy ? global.classArgsOf(classTy) : TemplateArgs{};
        if (!f->templateParams.empty())
        {
          global.deduceArgs(f, {operand}, fnArgs);
          if (fnArgs.pairs.size() < f->templateParams.size() + (classTy ? classTy->args.size() : 0))
            unsupported(u, "a call to " + qualifiedName(f) + " whose template arguments could not be deduced");
        }
        Expr* arg = u->operand;
        return callRvalue(callFunction(f, fnArgs, classTy, "", operatorResult(u), Span<Expr*>{&arg, 1}, u), u);
      }
    }
  }
  if (const Ty* et = u->operand->ty && u->operand->ty->isKnown() ? types.unqualified(types.stripRef(u->operand->ty)) : nullptr;
      et && et->kind == TyKind::Enum && verifyDecisions())
  {
    // KPP_VERIFY_DECISIONS: codegen's own choice for an enumeration operand (`!flags`), compared
    // with the checker's (Walker::enumUnaryOperator).
    FunctionDecl* f = global.findFreeOperator(uint32_t(op), argumentType(u->operand, u->operand->ty, types), nullptr, currentFunction);
    verifyOperatorCall(u, f ? OperatorCall::Function : OperatorCall::Builtin, f, false);
    if (f)
    {
      Expr* operand = u->operand;
      return callChosenOperator(u, f, false, Span<Expr*>{&operand, 1});
    }
  }
  Value v = rvalue(u->operand);
  if (isClass(v.ty))
  {
    const Ty* in = nullptr;
    if (op == P::Bang && (operatorLooked ? noOperator : !findMemberFunction(v.ty, "operator!", {}, v.ty, u, &in)))
    {
      // No operator!: the class's conversion to bool, negated.
      verifyOperatorCall(u, OperatorCall::Builtin);
      Value b = convert(v, types.boolType(), u, false, ConversionContext::Direct);
      std::string r = newTemp();
      fn += "  " + r + " = xor i1 " + b.text + ", true\n";
      return {r, types.boolType()};
    }
    // No operator function: the built-in operator on the class's conversion to a scalar
    // (`-limit` with a Const<double>), as the resolver typed it.
    const Ty* scalar = nullptr;
    if (operatorLooked ? noOperator : !findMemberFunction(v.ty, operatorName(op), {}, v.ty, u, &in))
    {
      scalar = global.scalarConversionOf(v.ty);
      // (a class converting to several scalar types: the best built-in candidate's, as the resolver typed it)
      if (!scalar && (op == P::Minus || op == P::Plus || op == P::Tilde))
        global.builtinOperands(uint32_t(op), conversionObject(v), nullptr, &scalar, nullptr);
    }
    if (!scalar)
      return callRvalue(operatorCall(withCheckedConst(v, u->operand), operatorName(op), {}, u, true, operatorResult(u)), u);
    verifyOperatorCall(u, OperatorCall::Builtin);
    v = convert(v, scalar, u);
  }
  switch (op)
  {
    case P::Bang:
    {
      Value b = convert(v, types.boolType(), u, false, ConversionContext::Direct);
      std::string r = newTemp();
      fn += "  " + r + " = xor i1 " + b.text + ", true\n";
      return {r, types.boolType()};
    }
    case P::Minus:
    {
      Value c = convert(v, u->ty, u);
      std::string r = newTemp();
      if (c.ty->kind == TyKind::Float)
        fn += "  " + r + " = fneg " + llType(c.ty, u) + " " + c.text + "\n";
      else if (!c.ty->isUnsigned && c.ty->kind != TyKind::Bool && checksOverflow(u))
      {
        // (--sanitize overflow: -INT_MIN overflows)
        std::string t = llType(c.ty, u), intrinsic = "@llvm.ssub.with.overflow." + t, pair = newTemp(), over = newTemp();
        if (declaredNames.claim(intrinsic))
          decls += "declare {" + t + ", i1} " + intrinsic + "(" + t + ", " + t + ")\n";
        fn += "  " + pair + " = call {" + t + ", i1} " + intrinsic + "(" + t + " 0, " + t + " " + c.text + ")\n";
        fn += "  " + r + " = extractvalue {" + t + ", i1} " + pair + ", 0\n";
        fn += "  " + over + " = extractvalue {" + t + ", i1} " + pair + ", 1\n";
        undefinedCheck(over, "signed overflow in int" + std::to_string(sizeOf(c.ty, u) * 8) + " (negation)", u);
      }
      else
        fn += "  " + r + " = sub " + llType(c.ty, u) + " 0, " + c.text + "\n";
      return {r, c.ty};
    }
    case P::Plus: return convert(v, u->ty, u);
    case P::Tilde:
    {
      Value c = convert(v, u->ty, u);
      std::string r = newTemp();
      fn += "  " + r + " = xor " + llType(c.ty, u) + " " + c.text + ", -1\n";
      return {r, c.ty};
    }
    default:
      unsupported(u, "this unary operator");
  }
}

auto codegen::Emitter::userLogicalOperator(BinaryExpr* b, P op) -> bool
{
  const Ty* lt = b->lhs->ty ? types.stripRef(b->lhs->ty) : nullptr;
  const Ty* rt = b->rhs->ty ? types.stripRef(b->rhs->ty) : nullptr;
  if (!lt || !rt || !lt->isKnown() || !rt->isKnown() || (!isClass(lt) && !isClass(rt)))
    return false;
  Expr* rhsExpr = b->rhs;
  Span<Expr*> one{&rhsExpr, 1};
  const Ty* foundIn = nullptr;
  if (isClass(lt) && findMemberFunction(types.unqualified(lt), operatorName(op), argTypesOf(one), types.unqualified(lt), b, &foundIn, true))
    return true;
  return global.findFreeOperator(uint32_t(op), argumentType(b->lhs, b->lhs->ty, types), argumentType(b->rhs, b->rhs->ty, types), currentFunction) != nullptr;
}

auto codegen::Emitter::userEnumLogicalOperator(BinaryExpr* b, P op) -> bool
{
  const Ty* lt = b->lhs->ty ? types.stripRef(b->lhs->ty) : nullptr;
  const Ty* rt = b->rhs->ty ? types.stripRef(b->rhs->ty) : nullptr;
  if (!lt || !rt || !lt->isKnown() || !rt->isKnown() || isClass(lt) || isClass(rt) || (lt->kind != TyKind::Enum && rt->kind != TyKind::Enum))
    return false;
  return global.enumOperator(uint32_t(op), argumentType(b->lhs, b->lhs->ty, types), argumentType(b->rhs, b->rhs->ty, types), currentFunction) != nullptr;
}

auto codegen::Emitter::callChosenOperator(Expr* e, FunctionDecl* f, bool member, Span<Expr*> operands) -> Value
{
  if (member)
  {
    // A kpp enum's own operator (design.md 8.4), called as its methods are (callMember): one that is not const on the
    // operand's place (`a += b` changes a), a const one on a copy of its value.
    if (const Ty* et = operands[0]->ty ? types.unqualified(types.stripRef(operands[0]->ty)) : nullptr; et && et->kind == TyKind::Enum)
    {
      std::string thisArg;
      if (!(f->flags & DF_Const))
        thisArg = lvalue(operands[0]);
      else
      {
        Value o = rvalue(operands[0]);
        thisArg = "%v" + std::to_string(temp++);
        allocas += "  " + thisArg + " = alloca " + llType(et, e) + "\n";
        fn += "  store " + llType(et, e) + " " + o.text + ", ptr " + thisArg + "\n";
      }
      const Ty* objectTy = enumInstance(et);
      return callRvalue(callFunction(f, global.classArgsOf(objectTy), objectTy, thisArg, operatorResult(e),
                                     Span<Expr*>{operands.begin() + 1, uint32_t(operands.size() - 1)}, e),
                        e);
    }
    Value object = withCheckedConst(rvalue(operands[0]), operands[0]);
    const Ty* foundIn = isClass(object.ty) ? declaringClass(object.ty, f, e) : nullptr;
    if (!foundIn)
      unsupported(e, "a call of the operator " + qualifiedName(f) + " the checker chose, on an object of a class that does not declare it");
    return callRvalue(memberCall(object, f, foundIn, Span<Expr*>{operands.begin() + 1, uint32_t(operands.size() - 1)}, e, operatorResult(e)), e);
  }
  std::vector<const Ty*> argTypes;
  for (Expr* o : operands)
    argTypes.push_back(argumentType(o, o->ty, types));
  const Ty* classTy = friendClass(f, argTypes, e);
  TemplateArgs fnArgs = classTy ? global.classArgsOf(classTy) : TemplateArgs{};
  if (!f->templateParams.empty())
  {
    global.deduceArgs(f, argTypes, fnArgs);
    if (fnArgs.pairs.size() < f->templateParams.size() + (operands.size() == 1 && classTy ? classTy->args.size() : 0)) // (as unary() and binary() had it)
      unsupported(e, "a call to " + qualifiedName(f) + " whose template arguments could not be deduced");
  }
  return callRvalue(callFunction(f, fnArgs, classTy, "", operatorResult(e), operands, e), e);
}

codegen::Emitter::PlaceName::PlaceName(Emitter& emitter, const std::string& at, const Ty* t, uint32_t token) : e(emitter)
{
  variable.kind = NodeKind::VariableDecl;
  variable.token = token;
  name.kind = NodeKind::NameExpr;
  name.token = token;
  name.resolved = &variable;
  name.ty = t;
  e.locals[&variable] = {at, t, false};
}

codegen::Emitter::PlaceName::~PlaceName() { e.locals.erase(&variable); }

codegen::Emitter::EvaluatedOperand::EvaluatedOperand(Emitter& e, Expr* rhs, Node* at) : place(e, placeOf(e, rhs, at), rhs->ty ? e.types.stripRef(rhs->ty) : nullptr, rhs->token)
{
  // An lvalue stands as a variable at its place; a temporary of a class, or an xvalue, as
  // `static_cast<T&&>` of one (still an rvalue, for operator=(T&&)); a scalar prvalue as the
  // value stored, cast back to a prvalue.
  const Ty* rty = place.name.ty;
  rvalueCast.kind = NodeKind::NamedCastExpr;
  rvalueCast.token = rhs->token;
  rvalueCast.castKind = Kw::static_cast_;
  rvalueCast.operand = &place.name;
  rvalueCast.ty = e.isClass(rty) || e.isLambda(rty) ? e.types.rref(rty) : rty; // (a closure too: `fn = [q = move(p)]() {...}`)
  standIn = isLvalueExpression(rhs) || (rhs->ty && rhs->ty->kind == TyKind::LRef) ? static_cast<Expr*>(&place.name) : &rvalueCast; // (`sourceRef()` returning T&)
}

auto codegen::Emitter::EvaluatedOperand::placeOf(Emitter& e, Expr* rhs, Node* at) -> std::string
{
  const Ty* rty = rhs->ty ? e.types.stripRef(rhs->ty) : nullptr;
  if (!rty || !rty->isKnown())
    e.unsupported(at, "an assignment whose right operand, evaluated first (C++17), has no known type");
  if (isLvalueExpression(rhs) || (rhs->ty && rhs->ty->kind == TyKind::LRef) || isXvalue(rhs))
    return e.lvalue(rhs);
  if (e.isClass(rty) || e.isLambda(rty))
    return e.rvalue(rhs).text; // (a temporary, destroyed with the statement's)
  std::string slot = e.newObjectSlot(rty, at);
  e.store(e.rvalue(rhs), slot, rty, at);
  return slot;
}

auto codegen::Emitter::binary(BinaryExpr* b) -> Value
{
  if (b->isKeyword)
  {
    Kw k = Kw(b->op);
    if (k == Kw::and_ || k == Kw::or_)
      return logical(k == Kw::and_, b->lhs, b->rhs, b);
    unsupported(b, "this keyword operator");
  }
  P op = P(b->op);
  // A class's own operator&& or operator|| is called like any overloaded operator (below), both
  // operands evaluated (C++ [over.match.oper]): NamedBool's `guiCheck || GuiCheck::fromBool(x)`.
  // The built-in, short-circuit one is for operands no such function takes.
  FunctionDecl* chosen = verifyDecisions() ? nullptr : recordedOperator(b); // (design.md 24; below)
  if ((op == P::AndAnd || op == P::OrOr) && !chosen && !userLogicalOperator(b, op) && !userEnumLogicalOperator(b, op))
  {
    verifyOperatorCall(b, OperatorCall::Builtin);
    return logical(op == P::AndAnd, b->lhs, b->rhs, b);
  }
  // (a class's operator, the walk chose: called below; and one a fold's step finds, no walk having typed the step)
  if (op == P::Comma && !chosen && !(b->call == OperatorCall::Undecided && (userLogicalOperator(b, op) || userEnumLogicalOperator(b, op))))
  {
    rvalue(b->lhs);
    return rvalue(b->rhs);
  }
  // (a class's operator->*, `object->*k` with object no pointer: called below as any operator)
  if ((op == P::DotStar || op == P::ArrowStar) && !(op == P::ArrowStar && b->lhs->ty && isClass(types.stripRef(b->lhs->ty))))
  {
    std::string addr = memberPointerAddress(b);
    const Ty* mt = b->ty ? types.stripRef(b->ty) : nullptr;
    if (!mt || !mt->isKnown())
      unsupported(b, "a member pointer of unknown type");
    return load(addr, mt, b);
  }
  const Ty* lt = types.stripRef(b->lhs->ty);
  const Ty* rt = b->rhs && b->rhs->ty ? types.stripRef(b->rhs->ty) : nullptr;
  // C++17 evaluates an assignment's right operand, simple or compound, built-in or a class's
  // operator, before its left ([expr.ass] 1, [over.match.oper] 2; design.md, "Evaluation
  // order"). The scalar assignment below does; a class's operator takes its operands as a call
  // does, the object first, so where the order can show (the right operand may write, the left
  // one is no fixed place) the right one is evaluated first and stands for itself in the
  // assignment, as the place it designates.
  if (isAssignment(op) && (isClass(lt) || isClass(rt) || (op != P::Assign && lt && lt->kind == TyKind::Enum)) && b->rhs &&
      b->rhs->kind != NodeKind::BraceInitExpr && b->rhs != evaluatedFirst && mayWrite(b->rhs) && !fixedPlace(b->lhs))
  {
    EvaluatedOperand evaluated(*this, b->rhs, b);
    Restore<const Expr*> restore(evaluatedFirst);
    evaluatedFirst = evaluated.standIn;
    BinaryExpr assignment = *b;
    assignment.rhs = evaluated.standIn;
    Value assigned = binary(&assignment);
    if (assignedAt == &assignment)
      assignedAt = b;
    return assigned;
  }
  if (op == P::Assign && b->lhs->kind == NodeKind::IndexExpr && static_cast<IndexExpr*>(b->lhs)->base->ty &&
      typedUnionOf(types.stripRef(static_cast<IndexExpr*>(b->lhs)->base->ty), b))
    return assignAlternative(b); // `p[Integer] = 5`: the tag and the value
  if ((op == P::EqEq || op == P::NotEq) &&
      ((typedUnionOf(lt, b) && rt && rt->kind == TyKind::Enum) || (typedUnionOf(rt, b) && lt && lt->kind == TyKind::Enum)))
    return compareTag(b, op == P::NotEq);
  if (op == P::Assign && rt && rt->isKnown() && !types.isDependent(rt))
    if (ClassDecl* u = typedUnionOf(lt, b); u && !(isClass(rt) && canonicalClass(rt, b) == canonicalClass(lt, b)))
    {
      // `s = Attacking{7};`: as `s[Attacking] = ...`, the alternative of the value's type (design.md 8.5).
      std::string obj = lvalue(b->lhs);
      setAlternative(u, lt, obj, alternativeForValue(u, lt, rt, b), b->rhs, nullptr, b);
      return {obj, lt, false};
    }
  // The operator function the checker chose (design.md 24): a class's member or free operator, an
  // enumeration's own. KPP_VERIFY_DECISIONS keeps codegen's own choice below, to compare.
  if (chosen)
  {
    Expr* operands[2] = {b->lhs, b->rhs};
    return callChosenOperator(b, chosen, b->calleeMember, Span<Expr*>{operands, 2});
  }
  // `a | b` on an enum with its own `Flags operator|(Flags, Flags)`: the user's function, as the
  // resolver typed it (a scoped enum has no built-in one).
  if (op != P::Assign && !isClass(lt) && !isClass(rt) && ((lt && lt->kind == TyKind::Enum) || (rt && rt->kind == TyKind::Enum)) &&
      lt && lt->isKnown() && rt && rt->isKnown())
    if (FunctionDecl* f = global.enumOperator(uint32_t(op), argumentType(b->lhs, b->lhs->ty, types), argumentType(b->rhs, b->rhs->ty, types),
                                              currentFunction))
    {
      verifyOperatorCall(b, OperatorCall::Function, f, false);
      Expr* args[2] = {b->lhs, b->rhs};
      return callRvalue(callFunction(f, {}, nullptr, "", operatorResult(b), Span<Expr*>{args, 2}, b), b);
    }
  // `a == b`, `a < b` of two typed unions of one type with no operator of their own: as
  // std::variant's, by the alternative held, then its values (design.md 8.5).
  if (ClassDecl* u = lt && rt ? typedUnionOf(lt, b) : nullptr;
      u && typedUnionOf(rt, b) && canonicalClass(lt, b) == canonicalClass(rt, b) &&
      (op == P::EqEq || op == P::NotEq || op == P::Less || op == P::Greater || op == P::LessEq || op == P::GreaterEq || op == P::Spaceship))
  {
    Expr* rhsExpr = b->rhs;
    Span<Expr*> one{&rhsExpr, 1};
    auto own = [&](P which) {
      const Ty* foundIn = nullptr;
      return findMemberFunction(types.unqualified(lt), operatorName(which), argTypesOf(one), types.unqualified(lt), b, &foundIn, true) ||
             global.findFreeOperator(uint32_t(which), argumentType(b->lhs, b->lhs->ty, types), argumentType(b->rhs, b->rhs->ty, types), currentFunction);
    };
    bool equality = op == P::EqEq || op == P::NotEq;
    if (!own(op) && !(equality ? own(P::EqEq) : own(P::Spaceship)))
    {
      Value l = rvalue(b->lhs), r = rvalue(b->rhs);
      if (equality)
      {
        Value eq = typedUnionEqual(u, l.text, r.text, lt, b);
        if (op == P::EqEq)
          return eq;
        std::string ne = newTemp();
        fn += "  " + ne + " = xor i1 " + eq.text + ", true\n";
        return {ne, types.boolType(), false};
      }
      Value order = typedUnionOrder(u, l.text, r.text, lt, b);
      if (op == P::Spaceship)
        return orderingObject(order, b);
      const char* predicate = op == P::Less ? "slt" : op == P::Greater ? "sgt" : op == P::LessEq ? "sle" : "sge";
      std::string res = newTemp();
      fn += "  " + res + " = icmp " + predicate + " i8 " + order.text + ", 0\n";
      return {res, types.boolType(), false};
    }
  }
  // `area += limit` with a Const<float> and no function for it: the scalar assignment below, on
  // the right operand's conversion to a scalar; `recipeCopied = setupForCrafting(...)`, an
  // ActionResult converting to bool and to its Enum: the conversion to the left side's type.
  bool scalarAssignment = isAssignment(op) && lt && lt->isKnown() && !isClass(lt) && isClass(rt) &&
                          (global.scalarConversionOf(rt) || (op == P::Assign && conversionOperator(rt, lt, b, nullptr, false, nullptr, ConversionContext::Copy))) &&
                          !global.findFreeOperator(uint32_t(op), argumentType(b->lhs, b->lhs->ty, types), argumentType(b->rhs, b->rhs->ty, types), currentFunction);
  if ((isClass(lt) || isClass(rt)) && !scalarAssignment)
  {
    std::string name = operatorName(op);
    Expr* rhsExpr = b->rhs;
    Span<Expr*> one{&rhsExpr, 1};
    const Ty* foundIn = nullptr;
    // `a = move(b)` of a class declaring no operator=: its implicit one (memberwise, below), not
    // a base's, which it hides.
    bool implicitAssignment = op == P::Assign && isClass(lt) && !declaresAssignment(types.unqualified(lt), b);
    FunctionDecl* member = isClass(lt) && !implicitAssignment ? findMemberFunction(types.unqualified(lt), name, argTypesOf(one), types.unqualified(lt), b, &foundIn, true) : nullptr;
    // An operator= found in a base, with the class's own operator= taking none of these operands
    // (Optional's `operator=(const T&)` beside its implicit copy assignment): the class's implicit
    // one, which hides the base's (C++ [class.copy.assign]), unless a using-declaration brought it.
    if (member && op == P::Assign && foundIn && canonicalClass(foundIn, b) != canonicalClass(types.unqualified(lt), b) &&
        !declaresAssignment(types.unqualified(lt), b, true))
    {
      member = nullptr;
      implicitAssignment = true;
    }
    // An operator= that is no copy or move assignment, which needs a conversion for the right operand, against the class's
    // implicit copy or move assignment, an exact match: the implicit one, as the checker chooses (Impl::implicitAssignmentBeats;
    // SmallProgressValue's `c = a.progress` went through its `operator=(f32)` and `implicit operator f32()`).
    if (member && op == P::Assign && b->rhs && b->rhs->ty && b->rhs->ty->isKnown() &&
        global.implicitAssignmentBeats(member, global.classArgsOf(foundIn), types.unqualified(lt), argumentType(b->rhs, b->rhs->ty, types)))
    {
      member = nullptr;
      implicitAssignment = true;
    }
    // `one < 1.5` that the checker rewrote as `(one <=> 1.5) < 0`, `w != x` as `!(w == x)`, each a
    // better match than the class's own operator (Walker::rewrittenBeats): neither the member nor a
    // free operator of its own.
    bool rewrittenRelation = b->call == OperatorCall::Rewritten && (op == P::Less || op == P::Greater || op == P::LessEq || op == P::GreaterEq ||
                                                                    op == P::EqEq || op == P::NotEq);
    if (rewrittenRelation)
      member = nullptr;
    // A free operator taking the operands strictly better than the member is called: C++ ranks
    // them together, as the checker does (classes/operator_member_and_free_ranked_together).
    if (member && op != P::Assign && b->lhs->ty && b->rhs->ty && b->lhs->ty->isKnown() && b->rhs->ty->isKnown())
    {
      const Ty* la = argumentType(b->lhs, b->lhs->ty, types);
      const Ty* ra = argumentType(b->rhs, b->rhs->ty, types);
      if (FunctionDecl* f = global.findFreeOperator(uint32_t(op), la, ra, currentFunction))
      {
        const Ty* friendOf = friendClass(f, {la, ra}, b);
        if (global.freeOperatorBeatsMember(f, friendOf ? global.classArgsOf(friendOf) : TemplateArgs{}, member, global.classArgsOf(foundIn), la, ra))
          member = nullptr;
      }
    }
    if (member)
    {
      verifyOperatorCall(b, OperatorCall::Function, member, true);
      return callRvalue(operatorCall(rvalue(b->lhs), name, one, b, true, operatorResult(b)), b);
    }
    // A free `operator+(const char*, const String&)` and the like, templates among them
    // (`template<class B, class E> bool operator!=(const Opt<B, E>&, const Opt<B, E>&)`).
    if (!rewrittenRelation && b->lhs->ty && b->rhs && b->rhs->ty && b->lhs->ty->isKnown() && b->rhs->ty->isKnown())
      if (FunctionDecl* f = global.findFreeOperator(uint32_t(op), argumentType(b->lhs, b->lhs->ty, types), argumentType(b->rhs, b->rhs->ty, types), currentFunction))
      {
        verifyOperatorCall(b, OperatorCall::Function, f, false);
        const Ty* classTy = friendClass(f, {argumentType(b->lhs, b->lhs->ty, types), argumentType(b->rhs, b->rhs->ty, types)}, b);
        TemplateArgs fnArgs = classTy ? global.classArgsOf(classTy) : TemplateArgs{};
        if (!f->templateParams.empty())
        {
          global.deduceArgs(f, {argumentType(b->lhs, b->lhs->ty, types), argumentType(b->rhs, b->rhs->ty, types)}, fnArgs);
          if (fnArgs.pairs.size() < f->templateParams.size())
            unsupported(b, "a call to " + qualifiedName(f) + " whose template arguments could not be deduced");
        }
        Expr* args[2] = {b->lhs, b->rhs};
        return callRvalue(callFunction(f, fnArgs, classTy, "", operatorResult(b), Span<Expr*>{args, 2}, b), b);
      }
    if (std::optional<Value> rewritten = rewrittenComparison(b, op, lt, rt))
      return *rewritten;
    // No operator function: C++'s built-in operator on the operands' conversions to a scalar
    // (`area < smallBlueprintAreaSize` with a Const<float>), as the resolver typed it.
    if (!isAssignment(op))
    {
      const Ty* lc = isClass(lt) ? global.scalarConversionOf(lt) : lt;
      const Ty* rc = isClass(rt) ? global.scalarConversionOf(rt) : rt;
      // A class converting to several scalar types (`q + 0.25` of `operator int() &` and `operator
      // double() const`): the types the best built-in candidate takes, as the resolver typed it
      // (until 2026-11-08 "Q has no operator+").
      if ((!lc || !rc) && b->lhs->ty && b->rhs->ty && b->lhs->ty->isKnown() && b->rhs->ty->isKnown())
      {
        auto object = [&](Expr* e) {
          const Ty* a = argumentType(e, e->ty, types);
          return a && !a->isReference() && isClass(a) ? types.rref(a) : a;
        };
        if (!global.builtinOperands(uint32_t(op), object(b->lhs), object(b->rhs), &lc, &rc))
          lc = rc = nullptr;
      }
      if (lc && rc && lc->isKnown() && rc->isKnown())
      {
        verifyOperatorCall(b, OperatorCall::Builtin);
        Value l = convert(rvalue(b->lhs), lc, b);
        Value r = convert(rvalue(b->rhs), rc, b);
        switch (op)
        {
          case P::EqEq: case P::NotEq: case P::Less: case P::LessEq: case P::Greater: case P::GreaterEq:
            return compare(op, l, r, b);
          case P::Spaceship:
            return orderingObject(orderOfScalars(l, r, b), b);
          default:
            return arithmetic(op, l, r, b);
        }
      }
    }
    if (!isClass(lt))
      unsupported(b, "an operator with a class on the right that no function defines");
    Value l = rvalue(b->lhs);
    if (op == P::Assign)
    {
      verifyOperatorCall(b, OperatorCall::Builtin); // the implicit assignment
      const Ty* lt0 = types.unqualified(types.stripRef(l.ty));
      const Ty* rt0 = b->rhs->ty && b->rhs->ty->isKnown() ? types.stripRef(b->rhs->ty) : nullptr;
      if (rt0 && !isClass(rt0) && findConstructor(lt0, {argumentType(b->rhs, b->rhs->ty, types)}, b))
      {
        // `flag = InvertColors::True`: the converting constructor makes a temporary to assign from.
        std::string tmp = convertedTemporary(lt0, b->rhs, b);
        assignObject(l.text, tmp, l.ty, b, true);
        return l;
      }
      if (rt0 && isClass(rt0) && classKey(rt0) != classKey(lt0) && !baseInstantiation(rt0, classDef(lt0->decl, b), b) &&
          (conversionOperator(rt0, lt0, b) || findConstructor(lt0, {argumentType(b->rhs, b->rhs->ty, types)}, b) ||
           global.converts(argumentType(b->rhs, b->rhs->ty, types), lt0)))
      {
        // `view = string`: StringView's converting constructor makes the temporary to assign from;
        // also one inherited with `using Base::Base;` (`states[i] = createPipelineStateObject(spec)`
        // into a SpritePipelineState built from PipelineStateHandle's UniquePtr constructor).
        std::string tmp = convertedTemporary(lt0, b->rhs, b);
        assignObject(l.text, tmp, l.ty, b, true);
        return l;
      }
      if (b->rhs->kind == NodeKind::BraceInitExpr)
      {
        // `*this = {}`, `this->data = {a, b}`, `mode = {Mode::MinLinear, Mode::MagLinear}`: a
        // temporary of the left side's class, list-initialised as `T tmp = {...};` would be (an
        // InitializerList constructor first), moved from, and destroyed at the statement's end.
        std::string tmp = newObjectSlot(lt0, b);
        initializeFrom(tmp, lt0, b->rhs, b);
        if (needsDestruction(lt0, b))
          statementTemps.push_back({tmp, lt0});
        assignObject(l.text, tmp, lt0, b, true);
        return l;
      }
      Value r = rvalue(b->rhs);
      if (r.ty && r.ty->kind == TyKind::Vector)
      {
        // A vector the checker could not type (an SSE operation clang's intrin.h has as a macro,
        // _mm_shuffle_ps) assigned to a class that wraps it: a temporary made of it by the class's
        // constructor (constructFromVector), assigned from and destroyed at the statement's end.
        std::string tmp = newObjectSlot(lt0, b);
        constructFromVector(tmp, lt0, r, true, b);
        if (needsDestruction(lt0, b))
          statementTemps.push_back({tmp, lt0});
        assignObject(l.text, tmp, l.ty, b, true);
        return l;
      }
      if (!isClass(r.ty))
        unsupported(b, "assigning a non-class to a class");
      std::string src = r.text;
      if (classKey(r.ty) != classKey(l.ty))
        if (!baseAddress(r.ty, l.ty, r.text, src, b))
          unsupported(b, "assigning an unrelated class");
      assignObject(l.text, src, l.ty, b, r.isTemporary || movable(b->rhs));
      return l;
    }
    missingOperator(b, l.ty, name);
  }
  if (isAssignment(op))
  {
    const Ty* t = types.stripRef(b->lhs->ty);
    if ((!t || !t->isKnown()) && b->lhs->kind == NodeKind::MemberExpr)
    {
      auto* mx = static_cast<MemberExpr*>(b->lhs);
      if (mx->resolved && mx->resolved->kind == NodeKind::VariableDecl)
        t = types.stripRef(declType(mx->resolved));
      if ((!t || !t->isKnown()) && !mx->member.empty())
      {
        const Ty* bt = mx->base->ty ? types.stripRef(mx->base->ty) : nullptr;
        if (mx->op == P::Arrow && bt && bt->kind == TyKind::Pointer)
          bt = types.stripRef(bt->inner);
        if (bt && isClass(bt))
          if (Decl* fd = findFieldByName(bt, mx->member.last().symbol, b))
            t = types.stripRef(declType(fd));
      }
    }
    if ((!t || !t->isKnown()) && b->lhs->kind == NodeKind::NameExpr)
      if (auto local = locals.find(static_cast<NameExpr*>(b->lhs)->resolved); local != locals.end() && local->second.ty && local->second.ty->isKnown())
        t = types.stripRef(local->second.ty); // `xmm7 = _mm_shufflelo_epi16(...)` into an SSE `auto` local
    if (!t || !t->isKnown())
    {
      // Fall back to the RHS type when the LHS type is unknown.
      const Ty* rhsTy = b->rhs->ty ? types.stripRef(b->rhs->ty) : nullptr;
      if (rhsTy && rhsTy->isKnown())
        t = rhsTy;
      else
        unsupported(b, "an assignment to an expression of unknown type");
    }
    // The right operand first, then the left one's place (C++17 [expr.ass] 1).
    Value rhs = op == P::Assign && b->rhs->kind == NodeKind::BraceInitExpr ? braceScalar(static_cast<BraceInitExpr*>(b->rhs), t, b) // `mod->id.crc = {};`
                                                                            : rvalue(b->rhs);
    if (op != P::Assign)
      rhs = bitfieldOperand(b->rhs, rhs); // (`i /= u3`: an int division, u3 a bit-field)
    if (scalarAssignment)
      verifyOperatorCall(b, OperatorCall::Builtin);
    if (scalarAssignment)
      rhs = convert(rhs, global.scalarConversionOf(rhs.ty) ? global.scalarConversionOf(rhs.ty) : t, b, false, ConversionContext::Copy);
    std::string addr = lvalue(b->lhs);
    const BitfieldInfo* bits = bitfieldNamed(b->lhs);
    assignedAt = bits ? nullptr : b; // (a bit field has no place to refer to)
    assignedPlace = addr;
    if (op != P::Assign)
    {
      Value old = bits ? bitfieldOperand(b->lhs, loadBitfield(addr, *bits, t, b)) : load(addr, t, b); // (`u3 /= -2` divides ints)
      rhs = arithmetic(baseOp(op), old, rhs, b);
    }
    if (bits)
    {
      storeBitfield(convert(rhs, t, b), addr, *bits, b);
      return loadBitfield(addr, *bits, t, b); // the field's value, wrapped to its width
    }
    store(rhs, addr, t, b);
    return convert(rhs, t, b);
  }
  Value l = bitfieldOperand(b->lhs, rvalue(b->lhs));
  if (isClass(l.ty))
  {
    // Late resolution: the AST type was Unknown but codegen resolved a class.
    std::string name = operatorName(op);
    Expr* rhsExpr = b->rhs;
    Span<Expr*> one{&rhsExpr, 1};
    Value result = operatorCall(l, name, one, b, false, operatorResult(b));
    if (result.ty)
      return callRvalue(result, b);
    if (std::optional<Value> rewritten = rewrittenComparison(b, op, l.ty, nullptr))
      return *rewritten;
    missingOperator(b, l.ty, name);
  }
  Value r = bitfieldOperand(b->rhs, rvalue(b->rhs));
  switch (op)
  {
    case P::EqEq: case P::NotEq: case P::Less: case P::LessEq: case P::Greater: case P::GreaterEq:
      return compare(op, l, r, b);
    case P::Spaceship:
      return orderingObject(orderOfScalars(l, r, b), b);
    default:
      return arithmetic(op, l, r, b);
  }
}

auto codegen::Emitter::orderingType() -> const Ty*
{
  Decl* d = global.strongOrderingDecl();
  return d ? types.classType(d, {}) : nullptr;
}

auto codegen::Emitter::orderOfScalars(Value l, Value r, Node* at) -> Value
{
  Value less = compare(P::Less, l, r, at);
  Value greater = compare(P::Greater, l, r, at);
  std::string g = newTemp(), res = newTemp();
  // Neither less nor greater: equal, or for floating values, unordered when one is a NaN (C++
  // [expr.spaceship] 8: partial_ordering::unordered, the library's strong_ordering::unordered, 2,
  // which no comparison with 0 holds; until 2026-11-06 `nan <=> 1.0` was equal: E's battery).
  std::string otherwise = "0";
  if ((l.ty && types.unqualified(l.ty)->kind == TyKind::Float) || (r.ty && types.unqualified(r.ty)->kind == TyKind::Float))
  {
    Value equal = compare(P::EqEq, l, r, at);
    otherwise = newTemp();
    fn += "  " + otherwise + " = select i1 " + equal.text + ", i8 0, i8 2\n";
  }
  fn += "  " + g + " = select i1 " + greater.text + ", i8 1, i8 " + otherwise + "\n";
  fn += "  " + res + " = select i1 " + less.text + ", i8 -1, i8 " + g + "\n";
  return {res, types.intType(8)};
}

auto codegen::Emitter::orderHolds(P cmp, Value order, Node* at) -> Value
{
  Value plain = compare(cmp, order, Value{"0", types.intType(8)}, at);
  if (cmp != P::Greater && cmp != P::GreaterEq)
    return plain; // (unordered, 2, is neither < 0, <= 0 nor == 0, and is != 0)
  std::string ordered = newTemp(), r = newTemp();
  fn += "  " + ordered + " = icmp ne i8 " + order.text + ", 2\n";
  fn += "  " + r + " = and i1 " + plain.text + ", " + ordered + "\n";
  return {r, types.boolType()};
}

auto codegen::Emitter::orderingObject(Value order, Node* at) -> Value
{
  const Ty* ot = orderingType();
  if (!ot)
    return convert(order, types.intType(), at);
  std::string slot = newObjectSlot(ot, at);
  fn += "  store i8 " + order.text + ", ptr " + gep(structFor(ot, at).name, slot, 0) + "\n";
  return {slot, ot, true};
}

auto codegen::Emitter::orderOf(Value ordering, Node* at) -> Value
{
  const Ty* t = types.stripRef(ordering.ty);
  if (isClass(t))
    return load(gep(structFor(t, at).name, ordering.text, 0), types.intType(8), at);
  return convert(ordering, types.intType(8), at);
}

auto codegen::Emitter::orderOfObjects(const std::string& a, const std::string& b, const Ty* t, Node* at) -> Value
{
  const Ty* foundIn = nullptr;
  std::vector<const Ty*> argTypes{types.lref(types.qualified(t, true, false))};
  FunctionDecl* op = findMemberFunction(t, "operator<=>", argTypes, t, at, &foundIn);
  if (ClassDecl* u = typedUnionOf(t, at); u && (!op || (op->flags & DF_Default)))
    return typedUnionOrder(u, a, b, t, at); // the alternative held, then its values (design.md 8.5)
  if (!op)
  {
    const Ty* operand = types.lref(types.qualified(t, true, false));
    if (global.findFreeOperator(uint32_t(P::Spaceship), operand, operand, currentFunction))
      return orderOf(comparedAt(P::Spaceship, a, b, t, at), at);
    // C++'s synthesized three-way comparison (what a defaulted <=> declaring its order type does
    // with a member that has no <=>): equal by ==, else less by <, else greater; a Direction
    // compares by its conversion to its Enum.
    Value equal = convert(comparedAt(P::EqEq, a, b, t, at), types.boolType(), at, false, ConversionContext::Direct);
    Value less = convert(comparedAt(P::Less, a, b, t, at), types.boolType(), at, false, ConversionContext::Direct);
    std::string ordered = newTemp(), order = newTemp();
    fn += "  " + ordered + " = select i1 " + less.text + ", i8 -1, i8 1\n";
    fn += "  " + order + " = select i1 " + equal.text + ", i8 0, i8 " + ordered + "\n";
    return {order, types.intType(8)};
  }
  if (op->flags & DF_Default)
    return memberwiseOrder(a, b, t, at);
  TemplateArgs args = global.classArgsOf(foundIn);
  Signature s;
  {
    ArgsScope scope(*this, &args);
    s = signatureOf(op);
  }
  std::string name = useFunction(op, args, foundIn);
  std::string thisArg = a;
  if (canonicalClass(t, at) != foundIn)
    baseAddress(t, foundIn, a, thisArg, at);
  if (s.returnType == "void")
  {
    const Ty* ot = orderingType();
    if (!ot)
      unsupported(at, "operator<=> returning an object without the library's strong_ordering");
    std::string tmp = newObjectSlot(ot, at); // the result object: this, sret, other
    emitCall("", "void " + name + "(ptr " + thisArg + ", ptr " + tmp + ", ptr " + b + ")");
    return orderOf(Value{tmp, ot, true}, at);
  }
  std::string r = newTemp();
  emitCall(r + " = ", s.returnType + " " + name + "(ptr " + thisArg + ", ptr " + b + ")");
  return convert(Value{r, types.intType(32), false}, types.intType(8), at);
}

auto codegen::Emitter::memberwiseOrder(const std::string& a, const std::string& b, const Ty* t, Node* at) -> Value
{
  const StructInfo& info = structFor(t, at);
  std::string slot = "%order" + std::to_string(temp++);
  allocas += "  " + slot + " = alloca i8\n";
  fn += "  store i8 0, ptr " + slot + "\n";
  std::string done = newLabel("order.done");
  auto part = [&](Value order)
  {
    fn += "  store i8 " + order.text + ", ptr " + slot + "\n";
    std::string ne = newTemp(), next = newLabel("order.next");
    fn += "  " + ne + " = icmp ne i8 " + order.text + ", 0\n";
    fn += "  br i1 " + ne + ", label %" + done + ", label %" + next + "\n";
    blockTerminated = true;
    startBlock(next);
  };
  for (auto& [baseDef, index] : info.bases)
    part(orderOfObjects(gep(info.name, a, index), gep(info.name, b, index), info.fieldTypes[index], at));
  for (Decl* d : info.fieldDecls)
  {
    uint32_t index = fieldIndexOf(info, d);
    const Ty* ft = info.fieldTypes[index];
    std::string fa = gep(info.name, a, index), fb = gep(info.name, b, index);
    if (d->kind == NodeKind::VariableDecl && static_cast<VariableDecl*>(d)->bitfield)
    {
      // (each bit field in declaration order: the unit as one integer ordered by its highest bits)
      for (auto [field, bits, fieldTy] : bitfieldsOfUnit(t, d, at))
        part(orderOfScalars(loadBitfield(fa, *bits, fieldTy, at), loadBitfield(fb, *bits, fieldTy, at), at));
      continue;
    }
    part(orderOfParts(fa, fb, ft, at));
  }
  emitBranch(done);
  startBlock(done);
  return load(slot, types.intType(8), at);
}

auto codegen::Emitter::orderOfParts(const std::string& a, const std::string& b, const Ty* t, Node* at) -> Value
{
  if (isClass(t))
    return orderOfObjects(a, b, t, at);
  if (t->kind == TyKind::Array)
    return orderOfArrays(a, b, t, at);
  return orderOfScalars(load(a, t, at), load(b, t, at), at);
}

auto codegen::Emitter::orderOfArrays(const std::string& a, const std::string& b, const Ty* t, Node* at) -> Value
{
  if (!t->count || !t->inner)
    unsupported(at, "a defaulted comparison of an array member without its length");
  std::string slot = "%order" + std::to_string(temp++), index = "%index" + std::to_string(temp++);
  allocas += "  " + slot + " = alloca i8\n  " + index + " = alloca i64\n";
  fn += "  store i8 0, ptr " + slot + "\n  store i64 0, ptr " + index + "\n";
  std::string arrayType = llType(t, at);
  std::string cond = newLabel("arrayorder.cond"), body = newLabel("arrayorder.body"), next = newLabel("arrayorder.next"), done = newLabel("arrayorder.done");
  emitBranch(cond);
  startBlock(cond);
  std::string i = newTemp(), more = newTemp();
  fn += "  " + i + " = load i64, ptr " + index + "\n";
  fn += "  " + more + " = icmp ult i64 " + i + ", " + std::to_string(t->count) + "\n";
  fn += "  br i1 " + more + ", label %" + body + ", label %" + done + "\n";
  blockTerminated = true;
  startBlock(body);
  std::string ea = newTemp(), eb = newTemp();
  fn += "  " + ea + " = getelementptr inbounds " + arrayType + ", ptr " + a + ", i64 0, i64 " + i + "\n";
  fn += "  " + eb + " = getelementptr inbounds " + arrayType + ", ptr " + b + ", i64 0, i64 " + i + "\n";
  Value order = orderOfParts(ea, eb, t->inner, at);
  std::string differs = newTemp();
  fn += "  store i8 " + order.text + ", ptr " + slot + "\n";
  fn += "  " + differs + " = icmp ne i8 " + order.text + ", 0\n";
  fn += "  br i1 " + differs + ", label %" + done + ", label %" + next + "\n";
  blockTerminated = true;
  startBlock(next);
  std::string i2 = newTemp();
  fn += "  " + i2 + " = add i64 " + i + ", 1\n  store i64 " + i2 + ", ptr " + index + "\n";
  emitBranch(cond);
  startBlock(done);
  return load(slot, types.intType(8), at);
}

auto codegen::Emitter::rewrittenComparison(BinaryExpr* b, P op, const Ty* lt, const Ty* rt) -> std::optional<Value>
{
  Expr* rhsExpr = b->rhs;
  Span<Expr*> one{&rhsExpr, 1};
  auto reversed = [&]() -> std::optional<Value>
  {
    // `3 == id` through Id::operator==(int): the operands the other way round.
    Expr* lhsExpr = b->lhs;
    Span<Expr*> left{&lhsExpr, 1};
    if (rt && isClass(rt))
    {
      const Ty* foundIn = nullptr;
      if (findMemberFunction(types.unqualified(rt), "operator==", argTypesOf(left), types.unqualified(rt), b, &foundIn, true))
        return callRvalue(operatorCall(rvalue(b->rhs), "operator==", left, b, true), b);
    }
    // `a == b` through a free operator==(const B&, const A&), function templates among them: the
    // function given the operands the other way round (C++20's reversed candidate; until 2026-11-05
    // "A has no operator==").
    if (b->lhs->ty && b->rhs && b->rhs->ty && b->lhs->ty->isKnown() && b->rhs->ty->isKnown())
    {
      const Ty* la = argumentType(b->lhs, b->lhs->ty, types);
      const Ty* ra = argumentType(b->rhs, b->rhs->ty, types);
      if (FunctionDecl* f = global.findFreeOperator(uint32_t(P::EqEq), ra, la, currentFunction))
      {
        const Ty* classTy = friendClass(f, {ra, la}, b);
        TemplateArgs fnArgs = classTy ? global.classArgsOf(classTy) : TemplateArgs{};
        if (!f->templateParams.empty())
        {
          global.deduceArgs(f, {ra, la}, fnArgs);
          if (fnArgs.pairs.size() < f->templateParams.size())
            unsupported(b, "a call to " + qualifiedName(f) + " whose template arguments could not be deduced");
        }
        Expr* args[2] = {b->rhs, b->lhs};
        return callRvalue(callFunction(f, fnArgs, classTy, "", operatorResult(b), Span<Expr*>{args, 2}, b), b);
      }
    }
    return std::nullopt;
  };
  // A free operator of the two operands, in this order, a function template's under what they deduce.
  auto freeCall = [&](P which, Expr* first, Expr* second) -> std::optional<Value>
  {
    const Ty* fa = argumentType(first, first->ty, types);
    const Ty* sa = argumentType(second, second->ty, types);
    FunctionDecl* f = global.findFreeOperator(uint32_t(which), fa, sa, currentFunction);
    if (!f)
      return std::nullopt;
    const Ty* classTy = friendClass(f, {fa, sa}, b);
    TemplateArgs fnArgs = classTy ? global.classArgsOf(classTy) : TemplateArgs{};
    if (!f->templateParams.empty())
    {
      global.deduceArgs(f, {fa, sa}, fnArgs);
      if (fnArgs.pairs.size() < f->templateParams.size())
        unsupported(b, "a call to " + qualifiedName(f) + " whose template arguments could not be deduced");
    }
    Expr* args[2] = {first, second};
    return callRvalue(callFunction(f, fnArgs, classTy, "", operatorResult(b), Span<Expr*>{args, 2}, b), b);
  };
  auto call = [&](P which) -> std::optional<Value>
  {
    std::string name = operatorName(which);
    const Ty* foundIn = nullptr;
    if (isClass(lt) && findMemberFunction(types.unqualified(lt), name, argTypesOf(one), types.unqualified(lt), b, &foundIn, true))
      return callRvalue(operatorCall(rvalue(b->lhs), name, one, b, true, operatorResult(b)), b);
    if (b->lhs->ty && b->rhs && b->rhs->ty && b->lhs->ty->isKnown() && b->rhs->ty->isKnown())
      if (std::optional<Value> free = freeCall(which, b->lhs, b->rhs))
        return free;
    return std::nullopt;
  };
  auto threeWayDefaulted = [&]()
  {
    const Ty* foundIn = nullptr;
    FunctionDecl* f = isClass(lt) ? findMemberFunction(types.unqualified(lt), "operator<=>", argTypesOf(one), types.unqualified(lt), b, &foundIn, true) : nullptr;
    return f && (f->flags & DF_Default);
  };
  auto orderCompare = [&](P cmp) -> std::optional<Value>
  {
    std::optional<Value> ordering = call(P::Spaceship);
    if (!ordering)
      return std::nullopt;
    return orderHolds(cmp, orderOf(*ordering, b), b);
  };
  // `scale < ScreenshotRequest::MinScale` with a double on the left: the right operand's
  // `operator<=>(const double&)`, the operands the other way round (`0 < (b <=> a)`).
  auto reversedOrder = [&](P cmp) -> std::optional<Value>
  {
    // (Also with a class on the left whose own <=> does not take the right operand: C++20's
    // reversed candidate, `current > latest` with Optional's `operator<=>(const U&)`.)
    P swapped = cmp == P::Less ? P::Greater : cmp == P::Greater ? P::Less : cmp == P::LessEq ? P::GreaterEq : P::LessEq;
    Expr* lhsExpr = b->lhs;
    Span<Expr*> left{&lhsExpr, 1};
    const Ty* foundIn = nullptr;
    if (rt && isClass(rt) && findMemberFunction(types.unqualified(rt), "operator<=>", argTypesOf(left), types.unqualified(rt), b, &foundIn, true))
      return orderHolds(swapped, orderOf(callRvalue(operatorCall(rvalue(b->rhs), "operator<=>", left, b, true), b), b), b);
    // `2 < o` through a free operator<=>(const O&, int).
    if (b->lhs->ty && b->rhs && b->rhs->ty && b->lhs->ty->isKnown() && b->rhs->ty->isKnown())
      if (std::optional<Value> ordering = freeCall(P::Spaceship, b->rhs, b->lhs))
        return orderHolds(swapped, orderOf(*ordering, b), b);
    return std::nullopt;
  };
  // C++20: a class whose operator<=> is defaulted has a defaulted operator==(const T&) too, which
  // takes a value of another type through T's converting constructor (`alertType ==
  // AlertType::ALERTS_COUNT` beside AlertType's `operator<=>(const Enum&)`, which is no ==).
  // classOnRight: `this->value != MAX_TICK`, the scalar on the left converted to the MapTick on
  // the right (C++20's reversed candidate of the implicit ==).
  auto implicitEquality = [&](bool classOnRight) -> std::optional<Value>
  {
    Expr* classSide = classOnRight ? b->rhs : b->lhs;
    Expr* otherSide = classOnRight ? b->lhs : b->rhs;
    const Ty* classSideTy = classOnRight ? rt : lt;
    if (!isClass(classSideTy) || !otherSide->ty || !otherSide->ty->isKnown())
      return std::nullopt;
    const Ty* cls = canonicalClass(types.unqualified(classSideTy), b);
    bool defaulted = false;
    for (Decl* m : classDef(cls->decl, b)->members)
      if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Default) && !static_cast<FunctionDecl*>(m)->name.empty() &&
          static_cast<FunctionDecl*>(m)->name.last().special == 2 && P(static_cast<FunctionDecl*>(m)->name.last().opToken) == P::Spaceship)
        defaulted = true;
    if (!defaulted)
      return std::nullopt;
    const Ty* other = withCurrentArgs(types.unqualified(types.stripRef(otherSide->ty)));
    bool sameClass = isClass(other) && classKey(other) == classKey(cls);
    if (!sameClass && !findConstructor(cls, {argumentType(otherSide, otherSide->ty, types)}, b))
      return std::nullopt;
    auto otherValue = [&]() -> std::string
    {
      if (sameClass)
        return rvalue(otherSide).text;
      std::string r = newObjectSlot(cls, b);
      Expr* one = otherSide;
      constructInto(r, cls, Span<Expr*>{&one, 1}, b);
      if (needsDestruction(cls, b))
        statementTemps.push_back({r, cls});
      return r;
    };
    // The operands in the order they are written.
    if (classOnRight)
    {
      std::string l = otherValue();
      return objectsEqual(l, rvalue(classSide).text, cls, b);
    }
    Value l = rvalue(classSide);
    return objectsEqual(l.text, otherValue(), cls, b);
  };
  switch (op)
  {
    case P::NotEq:
    {
      std::optional<Value> eq = call(P::EqEq);
      if (!eq && threeWayDefaulted())
        return orderCompare(P::NotEq);
      if (!eq)
        eq = reversed();
      if (!eq)
        eq = implicitEquality(false);
      if (!eq)
        eq = implicitEquality(true);
      if (!eq)
        return std::nullopt;
      std::string r = newTemp();
      fn += "  " + r + " = xor i1 " + convert(*eq, types.boolType(), b, false, ConversionContext::Direct).text + ", true\n";
      return Value{r, types.boolType()};
    }
    case P::EqEq:
      if (threeWayDefaulted())
        return orderCompare(P::EqEq);
      if (std::optional<Value> eq = reversed())
        return eq;
      if (std::optional<Value> eq = implicitEquality(false))
        return eq;
      return implicitEquality(true);
    case P::Less: case P::Greater: case P::LessEq: case P::GreaterEq:
      if (std::optional<Value> ordered = orderCompare(op))
        return ordered;
      return reversedOrder(op);
    case P::Spaceship:
    {
      // `minimum <=> scale`: the reversed `0 <=> (scale <=> minimum)`, its order negated, of the
      // type that call gives (as the resolver types it).
      if (!rt || !isClass(rt) || (lt && isClass(lt)))
        return std::nullopt;
      Expr* lhsExpr = b->lhs;
      Span<Expr*> left{&lhsExpr, 1};
      const Ty* foundIn = nullptr;
      std::optional<Value> reversedCall;
      if (findMemberFunction(types.unqualified(rt), "operator<=>", argTypesOf(left), types.unqualified(rt), b, &foundIn, true))
        reversedCall = callRvalue(operatorCall(rvalue(b->rhs), "operator<=>", left, b, true), b);
      else if (b->lhs->ty && b->rhs->ty && b->lhs->ty->isKnown() && b->rhs->ty->isKnown())
        reversedCall = freeCall(P::Spaceship, b->rhs, b->lhs); // `2 <=> m` through a free operator<=>(const M&, int)
      if (!reversedCall)
        return std::nullopt;
      Value ordering = *reversedCall;
      // (unordered, 2, stays unordered: a NaN's is no less the other way round)
      std::string turned = newTemp(), unordered = newTemp(), negated = newTemp();
      std::string forward = orderOf(ordering, b).text;
      fn += "  " + turned + " = sub i8 0, " + forward + "\n";
      fn += "  " + unordered + " = icmp eq i8 " + forward + ", 2\n";
      fn += "  " + negated + " = select i1 " + unordered + ", i8 2, i8 " + turned + "\n";
      Value order{negated, types.intType(8)};
      const Ty* ot = types.unqualified(types.stripRef(ordering.ty));
      return isClass(ot) ? orderingObject(order, b) : convert(order, ot, b);
    }
    default:
      return std::nullopt;
  }
}

auto codegen::Emitter::memberPointerAddress(BinaryExpr* b) -> std::string
{
  Value object = rvalue(b->lhs);
  std::string base = object.text;
  if (P(b->op) == P::ArrowStar)
  {
    if (object.ty->kind != TyKind::Pointer)
      unsupported(b, "->* on something that is not a pointer");
  }
  else if (!isClass(object.ty))
    unsupported(b, ".* on something that is not an object");
  Value offset = rvalue(b->rhs);
  const Ty* mp = offset.ty ? types.stripRef(offset.ty) : nullptr;
  if (!mp || mp->kind != TyKind::MemberPointer)
    unsupported(b, "a member pointer of unknown type");
  // The offset is within the pointer's class: an object of a class derived from it moves to
  // that base first (`c.*p` with `int B::* p` and B the second base of C).
  const Ty* objectTy = P(b->op) == P::ArrowStar ? object.ty->inner : types.stripRef(object.ty);
  const Ty* memberOf = mp->args.empty() ? nullptr : withCurrentArgs(mp->args[0]);
  if (isClass(objectTy) && isClass(memberOf) && classKey(objectTy) != classKey(memberOf))
  {
    std::string moved;
    if (!baseAddress(objectTy, memberOf, base, moved, b))
      unsupported(b, "a pointer to a member of " + types.toString(memberOf, &symbols) + " used on " + types.toString(objectTy, &symbols) + ", which is not derived from it");
    base = moved;
  }
  std::string r = newTemp();
  fn += "  " + r + " = getelementptr inbounds i8, ptr " + base + ", " + memberOffsetType() + " " + offset.text + "\n"; // (an i32 index is sign-extended)
  return r;
}

auto codegen::Emitter::logical(bool isAnd, Expr* lhs, Expr* rhs, Node* at) -> Value
{
  Value l = convert(rvalue(lhs), types.boolType(), at, false, ConversionContext::Direct);
  std::string rL = newLabel(isAnd ? "and.rhs" : "or.rhs"), eL = newLabel(isAnd ? "and.end" : "or.end");
  std::string lEnd = currentBlock;
  if (isAnd)
    fn += "  br i1 " + l.text + ", label %" + rL + ", label %" + eL + "\n";
  else
    fn += "  br i1 " + l.text + ", label %" + eL + ", label %" + rL + "\n";
  startBlock(rL);
  size_t temps = statementTemps.size();
  Value r = convert(rvalue(rhs), types.boolType(), at, false, ConversionContext::Direct);
  keepOperandTemps(temps); // `flag && Noisy(2).value() > 5`: the Noisy exists only when this operand ran
  std::string rEnd = currentBlock;
  emitBranch(eL);
  startBlock(eL);
  std::string res = newTemp();
  fn += "  " + res + " = phi i1 [ " + std::string(isAnd ? "false" : "true") + ", %" + lEnd + " ], [ " + r.text + ", %" + rEnd + " ]\n";
  return {res, types.boolType()};
}
