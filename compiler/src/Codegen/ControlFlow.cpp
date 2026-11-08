// struct Emitter's members: for constexpr, range-for, conditions and switch; their declarations in
// ControlFlow.hpp, part of the struct's body in Codegen/Emitter.hpp. A translation unit of its own
// since 2026-11-05 (codegen.cpp split, stage 2), moved without a change.
#include "Emitter.hpp"

auto codegen::Emitter::isTypesList(const NamedType* n) -> bool
{
  return n->name.parts.size() == 1 && n->name.parts[0].hasTemplateArgs && sym(n->name.parts[0].symbol) == "types";
}

auto codegen::Emitter::reflectionSubject(Node* list, bool* alternatives) -> Expr*
{
  if (!list || list->kind != NodeKind::CallExpr)
    return nullptr;
  auto* c = static_cast<CallExpr*>(list);
  if (!c->callee || c->callee->kind != NodeKind::NameExpr || c->args.size() != 1)
    return nullptr;
  const QualifiedName& name = static_cast<NameExpr*>(c->callee)->name;
  if (name.parts.size() != 1 || name.parts[0].hasTemplateArgs)
    return nullptr;
  std::string which = sym(name.parts[0].symbol);
  if (which != "members" && which != "alternatives")
    return nullptr;
  if (alternatives)
    *alternatives = which == "alternatives";
  return c->args[0];
}

auto codegen::Emitter::collectMembers(const Ty* cls, std::vector<const Ty*>& out, Node* at, int depth) -> void
{
  if (depth > 16)
    unsupported(at, "a class nested too deep for members(T)");
  ClassDecl* def = classDef(cls->decl, at);
  if (!def)
    unsupported(at, "members(T) of a class without a definition");
  TemplateArgs classArgs = global.classArgsOf(cls);
  for (const Ty* written : basesOf(def, classArgs, at))
  {
    const Ty* bt = written ? types.unqualified(types.stripRef(written)) : nullptr;
    if (!isClass(bt))
      unsupported(at, "members(T) with a base class of unknown type");
    collectMembers(canonicalClass(bt, at), out, at, depth + 1);
  }
  for (Decl* d : def->members)
  {
    if (d->kind != NodeKind::VariableDecl || (d->flags & (DF_Static | DF_Typedef)))
      continue;
    auto* v = static_cast<VariableDecl*>(d);
    if (v->name.empty())
      continue;
    const Ty* t = global.typeOfDeclIn(v, classArgs);
    if (!t || !t->isKnown())
      unsupported(at, "members(T) with a member of unknown type (" + sym(v->name.last().symbol) + ")");
    out.push_back(types.member(t, v, cls));
  }
}

auto codegen::Emitter::forConstexpr(ForConstexprStmt* f) -> void
{
  forEachConstexprElement(f, [&](size_t)
  {
    pushScope();
    statement(f->body);
    popScope();
  });
}

auto codegen::Emitter::bodyStatements(Stmt* body) -> std::vector<Stmt*>
{
  if (!body)
    return {};
  if (body->kind == NodeKind::BlockStmt)
    return std::vector<Stmt*>(static_cast<BlockStmt*>(body)->stmts.begin(), static_cast<BlockStmt*>(body)->stmts.end());
  return {body};
}

auto codegen::Emitter::forEachConstexprElement(ForConstexprStmt* f, const std::function<void(size_t)>& body) -> void
{
  auto* var = static_cast<TemplateParamDecl*>(f->var);
  if (!currentFunction)
    unsupported(f, "for constexpr outside a function");
  TemplateArgs none;
  TemplateArgs original = currentArgs ? *currentArgs : none;
  std::vector<const Ty*> elements;
  Node* list = f->list;
  if (list && list->kind == NodeKind::NamedType && isTypesList(static_cast<NamedType*>(list)))
  {
    if (!var->isType)
      unsupported(f, "a list of types for a for constexpr value variable");
    for (Node* arg : static_cast<NamedType*>(list)->name.parts[0].templateArgs)
    {
      if (!arg || uint8_t(arg->kind) > uint8_t(NodeKind::PackType))
        unsupported(f, "an element of types<...> that is not a type");
      const Ty* t = global.typeOfAstIn(static_cast<Type*>(arg), currentFunction, original);
      if (!t || !t->isKnown())
        unsupported(f, "an element of types<...> whose type is unknown");
      if (arg->kind == NodeKind::PackType && t->kind == TyKind::Pack)
      {
        elements.insert(elements.end(), t->args.begin(), t->args.end()); // `types<Ts...>`: each type of the pack in turn
        continue;
      }
      elements.push_back(t);
    }
  }
  else if (list && list->kind == NodeKind::BraceInitExpr)
  {
    if (var->isType)
      unsupported(f, "a list of values for a for constexpr type variable");
    const Ty* elementTy = var->valueType ? global.typeOfAstIn(var->valueType, currentFunction, original) : nullptr;
    if (!elementTy || !elementTy->isKnown() || elementTy->kind == TyKind::Auto)
      unsupported(f, "a for constexpr value variable without a written type");
    for (Expr* e : static_cast<BraceInitExpr*>(list)->args)
    {
      std::optional<int64_t> v = evalConst(e);
      if (!v)
        unsupported(e, "a for constexpr element that is not a constant");
      elements.push_back(types.valueType(elementTy, *v));
    }
  }
  else if (bool alternatives = false; Expr* subject = reflectionSubject(list, &alternatives))
  {
    // `members(T)`: T's non-static data members, bases first, in declaration order;
    // `alternatives(U)`: a typed union's alternatives, in declaration order.
    if (var->isType)
      unsupported(f, "members(T) or alternatives(U) for a for constexpr type variable");
    const Ty* cls = nullptr;
    if (subject->kind == NodeKind::NameExpr)
    {
      NamedType tmp{};
      tmp.kind = NodeKind::NamedType;
      tmp.name = static_cast<NameExpr*>(subject)->name;
      cls = global.typeOfAstIn(&tmp, currentFunction, original);
    }
    cls = cls ? types.unqualified(types.stripRef(cls)) : nullptr;
    if (!isClass(cls))
      unsupported(f, alternatives ? "alternatives(U) where U is not a class" : "members(T) where T is not a class");
    cls = canonicalClass(cls, f);
    if (alternatives)
    {
      ClassDecl* u = typedUnionOf(cls, f);
      if (!u)
        unsupported(f, "alternatives(U) where U is not a typed union");
      for (size_t i = 0; i < u->alternatives.size(); ++i)
        elements.push_back(types.member(alternativeType(u, cls, i, f), u->alternatives[i], cls));
    }
    else
      collectMembers(cls, elements, f, 0);
  }
  else
    unsupported(f, "a for constexpr list that is neither types<...>, members(T), alternatives(U) nor a brace list of constants");
  const TemplateArgs* savedArgs = currentArgs;
  for (size_t index = 0; index < elements.size(); ++index)
  {
    TemplateArgs elementArgs = original;
    bool bound = false;
    for (auto& [d, t] : elementArgs.pairs)
      if (d == var)
        t = elements[index], bound = true;
    if (!bound)
      elementArgs.pairs.push_back({var, elements[index]});
    retypeForElement(elementArgs);
    currentArgs = &elementArgs;
    body(index);
    currentArgs = savedArgs;
  }
  if (!elements.empty())
    retypeForElement(original);
}

auto codegen::Emitter::rangeTemporaryBase(Expr* e) -> const Ty*
{
  bool member = false;
  for (;;)
  {
    while (e && e->kind == NodeKind::ParenExpr)
      e = static_cast<ParenExpr*>(e)->inner;
    if (e && e->kind == NodeKind::MemberExpr && static_cast<MemberExpr*>(e)->op == P::Dot)
    {
      Decl* d = static_cast<MemberExpr*>(e)->resolved;
      if (!d || d->kind != NodeKind::VariableDecl || (d->flags & DF_Static))
        return nullptr;
      e = static_cast<MemberExpr*>(e)->base;
      member = true;
      continue;
    }
    if (e && e->kind == NodeKind::IndexExpr && static_cast<IndexExpr*>(e)->base && static_cast<IndexExpr*>(e)->base->ty &&
        types.stripRef(static_cast<IndexExpr*>(e)->base->ty)->kind == TyKind::Array)
    {
      e = static_cast<IndexExpr*>(e)->base;
      member = true;
      continue;
    }
    break;
  }
  if (!member || !e || !e->ty || e->ty->isReference() || isLvalueExpression(e) || !isClass(e->ty))
    return nullptr;
  return types.unqualified(e->ty);
}

auto codegen::Emitter::extendRangeTemporary(const Ty* cls, size_t since, Node* at) -> void
{
  for (size_t i = statementTemps.size(); i-- > since;)
    if (statementTemps[i].ty && isClass(statementTemps[i].ty) && canonicalClass(statementTemps[i].ty, at) == canonicalClass(cls, at))
    {
      scopes.back().push_back(statementTemps[i]);
      statementTemps.erase(statementTemps.begin() + i);
      return;
    }
}

auto codegen::Emitter::rangeFor(RangeForStmt* f) -> void
{
  pushScope();
  const Ty* rt = types.stripRef(f->range->ty);
  const Ty* temporaryBase = rangeTemporaryBase(f->range);
  size_t tempsBefore = statementTemps.size();
  if (!rt || !rt->isKnown())
    unsupported(f, "a range of unknown type");
  std::string beginV, endV;
  const Ty* elementTy;
  if (rt->kind == TyKind::Array)
  {
    std::string base;
    if (f->range->kind == NodeKind::BraceInitExpr)
    {
      // `for (X* kids : {&a, &b})`: the elements in a temporary array. Objects are destroyed after the
      // loop, the last first, as C++'s initializer_list's array is ([dcl.init.list] 6), each kept by the
      // loop's scope once built (one that throws leaves those before it destroyed). Until 2026-11-08
      // they were never destroyed.
      auto* b = static_cast<BraceInitExpr*>(f->range);
      base = newObjectSlot(rt, f);
      bool destroyed = needsDestruction(rt->inner, f);
      for (size_t i = 0; i < b->args.size(); ++i)
      {
        std::string slot = newTemp();
        fn += "  " + slot + " = getelementptr inbounds " + llType(rt, f) + ", ptr " + base + ", i64 0, i64 " + std::to_string(i) + "\n";
        initializeFrom(slot, rt->inner, b->args[i], f);
        if (destroyed)
          scopes.back().push_back({slot, rt->inner});
      }
    }
    else if (isArrayBraces(f->range))
    {
      // `for (const Noisy& n : Two{Noisy(1), Noisy(2)})`: the temporary array lives for the loop, as
      // C++'s `auto&& __range` keeps it ([stmt.ranged] 1), never only to the end of the range's
      // expression.
      base = arrayTemporary(f->range, rt, f);
      for (size_t i = statementTemps.size(); i-- > tempsBefore;)
        if (statementTemps[i].addr == base)
        {
          scopes.back().push_back(statementTemps[i]);
          statementTemps.erase(statementTemps.begin() + i);
        }
    }
    else
      base = lvalue(f->range);
    if (temporaryBase)
      extendRangeTemporary(temporaryBase, tempsBefore, f);
    beginV = newTemp();
    fn += "  " + beginV + " = getelementptr inbounds " + llType(rt, f) + ", ptr " + base + ", i64 0, i64 0\n";
    endV = newTemp();
    fn += "  " + endV + " = getelementptr inbounds " + llType(rt, f) + ", ptr " + base + ", i64 0, i64 " + std::to_string(rt->count) + "\n";
    elementTy = rt->inner;
  }
  else if (isClass(rt))
  {
    Value range = rvalue(f->range);
    if (temporaryBase)
      extendRangeTemporary(temporaryBase, tempsBefore, f);
    if (range.isTemporary)
    {
      for (size_t i = statementTemps.size(); i-- > 0;)
        if (statementTemps[i].addr == range.text)
          statementTemps.erase(statementTemps.begin() + i);
      scopes.back().push_back({range.text, range.ty});
    }
    const Ty* beginIn = nullptr;
    const Ty* endIn = nullptr;
    // The begin and end the checker chose (design.md 24), in a body its walk typed for this
    // function; KPP_VERIFY_DECISIONS keeps codegen's own choice, compared with the recorded one.
    bool recorded = choicesRecorded && f->begin && f->end;
    FunctionDecl* begin = nullptr;
    FunctionDecl* end = nullptr;
    if (recorded && !verifyDecisions())
    {
      if (f->rangeMembers)
      {
        begin = f->begin;
        end = f->end;
        beginIn = declaringClass(range.ty, begin, f);
        endIn = declaringClass(range.ty, end, f);
        if (!beginIn || !endIn)
          unsupported(f, "a range's begin() and end() the checker chose, on a class that does not declare them");
      }
    }
    else
    {
      // On the range's const as the checker typed it: a const range's `begin() const`.
      const Ty* object = withCheckedConst(range, f->range).ty;
      begin = findMember(object, "begin", f, &beginIn);
      end = findMember(object, "end", f, &endIn);
    }
    Value b, e;
    if (recorded && verifyDecisions() && begin && end)
      verifyRangeFunctions(f, true, begin, end, range.ty);
    if (begin && end)
    {
      // On the base that declares them, where it is (`for (T* t : *this)` in TriggerList, a
      // Vector<T*> after a polymorphic base: not at the object's address).
      auto on = [&](const Ty* in)
      {
        std::string object = range.text;
        if (canonicalClass(range.ty, f) != in)
          baseAddress(range.ty, in, range.text, object, f);
        return object;
      };
      b = callFunction(begin, global.classArgsOf(beginIn), beginIn, on(beginIn), "", {}, f);
      e = callFunction(end, global.classArgsOf(endIn), endIn, on(endIn), "", {}, f);
    }
    else
    {
      // No such members: begin(__range) and end(__range), the free functions found for the
      // range's class, called with the range evaluated once (C++'s rule).
      // (its const kept, as the checker typed it: a const range's `begin(const Bag&)`)
      const Ty* rangeRef = types.lref(types.stripRef(withCheckedConst(range, f->range).ty));
      FunctionDecl* freeBegin = recorded && !verifyDecisions() ? f->begin : global.findFreeFunction("begin", {rangeRef}, currentFunction);
      FunctionDecl* freeEnd = recorded && !verifyDecisions() ? f->end : global.findFreeFunction("end", {rangeRef}, currentFunction);
      if (recorded && verifyDecisions())
        verifyRangeFunctions(f, false, freeBegin, freeEnd, range.ty);
      if (!freeBegin || !freeEnd)
        unsupported(f, "a range without begin()/end()");
      VariableDecl& var = rangeVariables.emplace_back();
      var.kind = NodeKind::VariableDecl;
      var.token = f->range->token;
      var.file = currentFile;
      std::string slot = "%v" + std::to_string(temp++);
      allocas += "  " + slot + " = alloca ptr\n";
      fn += "  store ptr " + range.text + ", ptr " + slot + "\n";
      locals[&var] = {slot, rangeRef, true};
      NameExpr& name = rangeNames.emplace_back();
      name.kind = NodeKind::NameExpr;
      name.token = f->range->token;
      name.resolved = &var;
      name.ty = types.stripRef(rangeRef);
      Expr* arg = &name;
      auto call = [&](FunctionDecl* callee) -> Value
      {
        TemplateArgs calleeArgs;
        if (!callee->templateParams.empty())
        {
          global.deduceArgs(callee, {rangeRef}, calleeArgs);
          if (calleeArgs.pairs.size() < callee->templateParams.size())
            unsupported(f, "a call to " + qualifiedName(callee) + " whose template arguments could not be deduced");
        }
        return callFunction(callee, calleeArgs, nullptr, "", "", Span<Expr*>{&arg, 1}, f);
      };
      b = call(freeBegin);
      e = call(freeEnd);
    }
    // `Iterator& begin()`, `const Iterator& end()`: the loop works on copies, C++'s `auto __begin
    // = range.begin()` (HeuristicEntitySearch hands out its own iterator member).
    auto byValue = [&](Value v) -> Value
    {
      if (!v.ty || !v.ty->isReference())
        return v;
      const Ty* t = types.unqualified(types.stripRef(v.ty));
      if (t->kind == TyKind::Pointer)
        return load(v.text, t, f);
      if (!isClass(t))
        return v;
      std::string copy = newObjectSlot(t, f);
      copyConstruct(copy, v.text, t, f);
      return {copy, t, true};
    };
    b = byValue(b);
    e = byValue(e);
    if (b.ty && isClass(b.ty))
    {
      iteratorLoop(f, b, e);
      popScope();
      return;
    }
    if (!b.ty || b.ty->kind != TyKind::Pointer)
      unsupported(f, "a range whose iterators are not pointers");
    beginV = b.text;
    endV = e.text;
    elementTy = b.ty->inner;
  }
  else
    unsupported(f, "this kind of range");
  std::string itSlot = "%v" + std::to_string(temp++);
  allocas += "  " + itSlot + " = alloca ptr\n";
  fn += "  store ptr " + beginV + ", ptr " + itSlot + "\n";
  destroyTemps();
  std::string cL = newLabel("range.cond"), bL = newLabel("range.body"), sL = newLabel("range.step"), eL = newLabel("range.end");
  emitBranch(cL);
  startBlock(cL);
  std::string it = newTemp();
  fn += "  " + it + " = load ptr, ptr " + itSlot + "\n";
  std::string done = newTemp();
  fn += "  " + done + " = icmp eq ptr " + it + ", " + endV + "\n";
  fn += "  br i1 " + done + ", label %" + eL + ", label %" + bL + "\n";
  startBlock(bL);
  pushScope();
  // The loop variable: a reference binds to the element, a value copies it.
  auto* v = static_cast<VariableDecl*>(f->var);
  const Ty* vt = declType(v);
  if (!vt || !vt->isKnown() || types.isDependent(vt))
    unsupported(v, "a loop variable of unknown type");
  std::string element = newTemp();
  fn += "  " + element + " = load ptr, ptr " + itSlot + "\n";
  std::string slot = "%v" + std::to_string(temp++);
  if (vt->isReference())
  {
    std::string target = elementAs(element, elementTy, vt, v);
    if (elementMadeInto(elementTy, vt))
    {
      // `for (const String& name : {"ref"})`: a String made from the element, the reference's
      // (it lives for the iteration).
      const Ty* cls = types.unqualified(types.stripRef(vt));
      target = newObjectSlot(cls, v);
      makeFromElement(target, cls, element, elementTy, v);
      if (needsDestruction(cls, v))
        scopes.back().push_back({target, cls});
    }
    allocas += "  " + slot + " = alloca ptr\n";
    fn += "  store ptr " + target + ", ptr " + slot + "\n";
    locals[v] = {slot, vt, true, scopes.size()};
    if (!v->name.empty())
      debugVariable(sym(v->name.last().symbol), v, slot, vt); // the loop variable, each iteration's
  }
  else
  {
    if (v == namedReturn)
      slot = sretAddr; // built in the caller's result, each iteration (builtInResult)
    else
      allocas += "  " + slot + " = alloca " + llType(vt, v) + "\n";
    if (elementMadeInto(elementTy, vt))
      makeFromElement(slot, vt, element, elementTy, v);
    else if (isClass(vt))
      copyConstruct(slot, elementAs(element, elementTy, vt, v), vt, v);
    else if (const Ty* array = types.unqualified(vt); array->kind == TyKind::Array)
    {
      // `for (auto [u, w] : pairs)` over arrays: the hidden object a copy of each element array
      // ([dcl.struct.bind] 1), element by element (its value as a scalar's stored nothing).
      const Ty* from = types.unqualified(types.stripRef(elementTy));
      if (from->kind != TyKind::Array || from->count != array->count || types.unqualified(from->inner) != types.unqualified(array->inner))
        unsupported(v, "a loop variable of " + types.toString(vt, &symbols) + " over elements of " + types.toString(elementTy, &symbols));
      transferArray(slot, element, array, v, false, false);
    }
    else
      store(load(element, elementTy, v), slot, vt, v);
    locals[v] = {slot, vt, false, scopes.size()};
    if (!v->name.empty())
      debugVariable(sym(v->name.last().symbol), v, slot, vt); // the loop variable, each iteration's
    if (needsDestruction(vt, v))
      scopes.back().push_back({slot, vt});
  }
  declareTupleBindings(v); // `for (auto& [a, b] : tuples)`: each element's get<i>
  loops.push_back({eL, sL, scopes.size()});
  subStatement(f->body);
  loops.pop_back();
  popScope();
  emitBranch(sL);
  startBlock(sL);
  debugLine(f);
  std::string cur = newTemp();
  fn += "  " + cur + " = load ptr, ptr " + itSlot + "\n";
  std::string next = newTemp();
  fn += "  " + next + " = getelementptr inbounds " + llType(elementTy, f) + ", ptr " + cur + ", i64 1\n";
  fn += "  store ptr " + next + ", ptr " + itSlot + "\n";
  emitBranch(cL);
  startBlock(eL);
  popScope();
}

auto codegen::Emitter::condition(Node* cond, Node* at) -> Value
{
  if (!cond)
    unsupported(at, "a missing condition");
  if (cond->kind == NodeKind::DeclStmt)
  {
    auto* d = static_cast<DeclStmt*>(cond);
    if (d->decls.size() != 1 || d->decls[0]->kind != NodeKind::VariableDecl)
      unsupported(at, "this condition declaration");
    auto* v = static_cast<VariableDecl*>(d->decls[0]);
    declareLocal(v);
    Local& l = locals[v];
    if (l.isRef || l.ty->isReference())
    {
      // `if (const IDConnector<ItemID>& spoilResultID = itemID->spoilResult)`: the object the
      // reference names is the condition (its operator bool), not the reference's address.
      std::string object = newTemp();
      fn += "  " + object + " = load ptr, ptr " + l.addr + "\n";
      const Ty* objectTy = types.stripRef(l.ty);
      return convert(isAggregate(objectTy) ? Value{object, objectTy, false, Value::Category::Lvalue} : load(object, objectTy, at), types.boolType(), at, false,
                     ConversionContext::Direct);
    }
    Value variable = load(l.addr, l.ty, at);
    if (isClass(variable.ty))
      variable.category = Value::Category::Lvalue; // (the variable: `if (Guard g = make())` calls an `operator bool() &`)
    return convert(variable, types.boolType(), at, false, ConversionContext::Direct);
  }
  return convert(rvalue(static_cast<Expr*>(cond)), types.boolType(), at, false, ConversionContext::Direct);
}

auto codegen::Emitter::switchStatement(SwitchStmt* sw) -> void
{
  pushScope();
  if (sw->init)
    statement(sw->init);
  if (!sw->cond)
    unsupported(sw, "a switch without a condition");
  Value cond;
  const Ty* switchedTy = nullptr;
  if (sw->cond->kind == NodeKind::DeclStmt)
  {
    // `switch (const int type = table.get<uint32_t>("type"))`: the variable, in the switch's
    // scope, and its value switched on.
    auto* d = static_cast<DeclStmt*>(sw->cond);
    if (d->decls.size() != 1 || d->decls[0]->kind != NodeKind::VariableDecl)
      unsupported(sw, "this switch declaration");
    auto* v = static_cast<VariableDecl*>(d->decls[0]);
    declareLocal(v);
    Local& l = locals[v];
    cond = load(l.addr, l.ty, sw);
    switchedTy = types.stripRef(l.ty);
  }
  else
  {
    cond = rvalue(static_cast<Expr*>(sw->cond));
    switchedTy = types.stripRef(static_cast<Expr*>(sw->cond)->ty); // the written type: a kpp enum's switch names every value
  }
  if (ClassDecl* c = typedUnionOf(cond.ty, sw))
  {
    // `switch (p)` on a typed union: its tag.
    cond = Value{loadTag(c, cond.ty, cond.text, sw), tagType(c), false};
    switchedTy = cond.ty;
  }
  else if (isClass(cond.ty))
  {
    // `switch (direction)` on an object: its conversion to an enum or integer (Direction's `operator Enum()`).
    const Ty* owner = nullptr;
    FunctionDecl* op = integralConversionOperator(cond.ty, sw, &owner);
    if (!op)
      unsupported(sw->cond, "a switch on a class without a conversion to an enum or an integer"); // (the checker's error, Walker::checkSwitchOnClass)
    cond = callConversion(op, cond.ty, owner, cond.text, sw);
    switchedTy = cond.ty;
  }
  const Ty* ct = cond.ty->kind == TyKind::Enum ? enumUnderlying(cond.ty) : cond.ty;
  if (ct->kind != TyKind::Int && ct->kind != TyKind::Char && ct->kind != TyKind::Bool)
    unsupported(sw, "a switch on a non-integer");
  cond = convert(cond, ct, sw, true); // (an enumeration's value as its underlying integer: the switch's own)
  destroyTemps();
  if (!sw->body || sw->body->kind != NodeKind::BlockStmt)
    unsupported(sw, "a switch whose body is not a block");
  auto* body = static_cast<BlockStmt*>(sw->body);
  std::string endL = newLabel("switch.end");
  std::string defaultL;
  std::map<std::pair<const Stmt*, size_t>, std::string> caseLabels; // (label statement, for constexpr element) -> block
  std::set<int64_t> covered;
  std::string cases;
  constexpr size_t topLevel = SIZE_MAX;
  auto addLabel = [&](Stmt* st, size_t element)
  {
    if (st->kind == NodeKind::CaseStmt)
    {
      auto* c = static_cast<CaseStmt*>(st);
      if (c->rangeEnd)
        unsupported(st, "case ranges");
      std::string l = newLabel("case");
      caseLabels[{st, element}] = l;
      // (`case A, B, C:`: each value a case of the one block; a value twice is the checker's error, Walker::checkCaseValues)
      auto addValue = [&](Expr* value)
      {
        auto v = evalConst(value);
        if (!v)
          unsupported(value, "a case label that is not a constant");
        if (!covered.insert(*v).second)
          fail(value, "the case value " + std::to_string(*v) + " is a label of this switch twice");
        cases += "    " + llType(ct, sw) + " " + std::to_string(*v) + ", label %" + l + "\n";
      };
      addValue(c->value);
      for (Expr* value : c->moreValues)
        addValue(value);
    }
    else if (st->kind == NodeKind::DefaultStmt)
    {
      defaultL = newLabel("default");
      caseLabels[{st, element}] = defaultL;
    }
  };
  for (Stmt* st : body->stmts)
  {
    if (st->kind == NodeKind::ForConstexprStmt)
    {
      // `for constexpr (typename T : kinds) case T::kind: ...`: one case per element.
      auto* fc = static_cast<ForConstexprStmt*>(st);
      forEachConstexprElement(fc, [&](size_t element)
      {
        for (Stmt* inner : bodyStatements(fc->body))
          addLabel(inner, element);
      });
      continue;
    }
    addLabel(st, topLevel);
  }
  std::string invalidL;
  const Ty* condTy = switchedTy; // before the conversion to the underlying type
  if (condTy && condTy->kind == TyKind::Enum && defaultL.empty())
  {
    Decl* def = global.definitionOf(condTy->decl);
    auto* e = def && def->kind == NodeKind::EnumDecl ? static_cast<EnumDecl*>(def) : nullptr;
    if (e && e->isKpp)
    {
      // A kpp enum: every value needs a case, and a value outside them aborts.
      std::string missing;
      for (Decl* d : e->enumerators)
        if (d->kind == NodeKind::EnumeratorDecl && !covered.count(enumeratorValue(static_cast<EnumeratorDecl*>(d), condTy)))
          missing += (missing.empty() ? "" : ", ") + sym(static_cast<EnumeratorDecl*>(d)->name);
      if (!missing.empty())
        error(sw, "switch on " + qualifiedName(e) + " does not handle " + missing);
      invalidL = newLabel("switch.invalid");
    }
  }
  fn += "  switch " + llType(ct, sw) + " " + cond.text + ", label %" + (invalidL.empty() ? (defaultL.empty() ? endL : defaultL) : invalidL) + " [\n" + cases + "  ]\n";
  blockTerminated = true;
  if (!invalidL.empty())
  {
    startBlock(invalidL);
    auto [line, column] = lineColumnOf(sw->token);
    emitCall("", "void @kpp_assert_failed(ptr " + cStringConstant(programPathOf(currentFile)) + ", i32 " + std::to_string(line) + ", i32 " +
                     std::to_string(column) + ", ptr " + cStringConstant("invalid value in switch on " + qualifiedName(condTy->decl)) + ")", false);
    fn += "  unreachable\n";
    usesAssert = true;
    blockTerminated = true;
  }
  loops.push_back({endL, "", scopes.size()});
  pushScope();
  size_t bodyScope = scopeInits.size() - 1;
  // --no-implicit-fallthrough (design.md "No implicit fallthrough", step 2): each label begins a group of its own, a
  // group whose end is reached breaks unless it ends with `fallthrough;`, and each top-level group is a scope of its
  // own (rule 4: its objects are destroyed at its end). `case A: case B: x();` runs x() for B alone: A's group is
  // empty and does nothing (kovarex, 2026-11-08; until then labels side by side shared one group).
  bool implicitBreaks = noImplicitFallthroughOn();
  enum class Last { Nothing, Label, Fallthrough, Statement } last = Last::Nothing;
  bool groupScope = false;
  auto emitLabelled = [&](Stmt* st, size_t element)
  {
    if (implicitBreaks && (st->kind == NodeKind::CaseStmt || st->kind == NodeKind::DefaultStmt))
    {
      if (groupScope)
        popScope(); // (the group before it ends: its objects with it)
      groupScope = false;
      if (last != Last::Nothing)
        emitBranch(last == Last::Fallthrough ? caseLabels[{st, element}] : endL); // (an empty group's too; nothing when its end is not reached)
    }
    if (st->kind == NodeKind::CaseStmt || st->kind == NodeKind::DefaultStmt)
    {
      // The switch jumps to it from before its body: past nothing initialized in the body's
      // scopes ([stmt.dcl] 3; until 2026-11-05 `case 0: Noisy n(1); break; case 1:` ran case 1 and
      // destroyed the n it never built).
      for (size_t d = bodyScope; d < scopeInits.size(); ++d)
        if (scopeInits[d] > 0)
          fail(st, std::string(st->kind == NodeKind::CaseStmt ? "the case label" : "the default label") +
                       " is reached past the initialization of a variable declared before it in the switch's body, which C++ makes ill-formed ([stmt.dcl] 3): put that case's declarations in a block");
      const std::string& l = caseLabels[{st, element}];
      emitBranch(l);
      startBlock(l);
      if (implicitBreaks && element == topLevel && !groupScope)
      {
        pushScope();
        groupScope = true;
      }
      last = Last::Label;
      return;
    }
    if (st->kind == NodeKind::FallthroughStmt)
    {
      last = Last::Fallthrough;
      return;
    }
    statement(st);
    if (st->kind != NodeKind::EmptyStmt)
      last = Last::Statement;
  };
  for (Stmt* st : body->stmts)
  {
    if (st->kind == NodeKind::ForConstexprStmt)
    {
      auto* fc = static_cast<ForConstexprStmt*>(st);
      // (its elements' labels begin groups of their own, each element its scope: the group before it ends here)
      if (implicitBreaks && groupScope)
        for (Stmt* inner : bodyStatements(fc->body))
          if (inner->kind == NodeKind::CaseStmt || inner->kind == NodeKind::DefaultStmt)
          {
            popScope();
            groupScope = false;
            break;
          }
      forEachConstexprElement(fc, [&](size_t element)
      {
        pushScope();
        for (Stmt* inner : bodyStatements(fc->body))
          emitLabelled(inner, element);
        popScope();
      });
      continue;
    }
    emitLabelled(st, topLevel);
  }
  if (groupScope)
    popScope(); // (the last group's)
  popScope();
  loops.pop_back();
  emitBranch(endL);
  startBlock(endL);
  popScope();
}
