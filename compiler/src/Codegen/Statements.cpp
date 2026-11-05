// struct Emitter's members: local variables and statements; their declarations in Statements.hpp,
// part of the struct's body in Codegen/Emitter.hpp. A translation unit of its own since 2026-11-05
// (codegen.cpp split, stage 2), moved without a change.
#include "Emitter.hpp"

auto codegen::Emitter::isStaticLocal(const VariableDecl* v) -> bool
{
  return v->parent && v->parent->kind == NodeKind::FunctionDecl && (v->flags & (DF_Static | DF_ThreadLocal)) &&
         !(v->flags & (DF_Extern | DF_Typedef));
}

auto codegen::Emitter::isAutomaticLocal(const Decl* d) -> bool
{
  return d->kind == NodeKind::ParamDecl || (d->kind == NodeKind::VariableDecl && d->parent && d->parent->kind == NodeKind::FunctionDecl &&
                                            !(d->flags & (DF_Static | DF_ThreadLocal | DF_Extern | DF_Typedef)));
}

auto codegen::Emitter::refuseOutsideItsFunction(Decl* d, Node* at, bool notConstant) -> void
{
  // A member function of a local class names its function's variable (C++'s [class.local]: no
  // odr-use of it), or a lambda one it does not capture. Until 2026-11-05 a variable was built as a
  // global from its initializer: `int value = 3; value += 1;` read 3 there, and a local class's
  // member hidden by it (the resolver searched the function's locals first) read the local.
  std::string name = d->kind == NodeKind::ParamDecl ? sym(static_cast<ParamDecl*>(d)->name) : sym(static_cast<VariableDecl*>(d)->name.last().symbol);
  std::string function = d->parent && d->parent->kind == NodeKind::FunctionDecl ? qualifiedName(d->parent) : std::string("its function");
  fail(at, name + ", a " + (d->kind == NodeKind::ParamDecl ? "parameter" : "local") + " of " + function +
               ", is used outside its function (a member function of a local class, or a lambda that does not capture it), which reads only a constant of it" +
               (notConstant ? ": its value is no constant" : ""));
}

auto codegen::Emitter::nestedNamesOf(Node* body) -> const NestedNames&
{
  auto [it, isNew] = nestedNamesByBody.try_emplace(body);
  if (!isNew || !body)
    return it->second;
  NestedNames& found = it->second;
  std::unordered_set<const Node*> nested;
  forEachNode(body, [&](Node* n)
  {
    if (n->kind != NodeKind::LambdaExpr && !(n->kind == NodeKind::ClassDecl && !static_cast<ClassDecl*>(n)->isClosure))
      return;
    forEachNode(n, [&](Node* m)
    {
      nested.insert(m);
      if (m->kind == NodeKind::NameExpr && static_cast<NameExpr*>(m)->resolved)
        found.named.insert(static_cast<NameExpr*>(m)->resolved);
    });
  });
  forEachNode(body, [&](Node* n)
  {
    if (n->kind == NodeKind::VariableDecl && !nested.count(n))
      found.direct.insert(static_cast<Decl*>(n));
  });
  return found;
}

auto codegen::Emitter::plainWriter(VariableDecl* v) -> FunctionDecl*
{
  Decl* w = v->parent;
  if (!w || w->kind != NodeKind::FunctionDecl || !static_cast<FunctionDecl*>(w)->body)
    return nullptr;
  for (Decl* p = w; p; p = p->parent)
    if (!p->templateParams.empty() || (p != w && p->kind == NodeKind::FunctionDecl))
      return nullptr; // a template's, or a local class's member's
  auto* f = static_cast<FunctionDecl*>(w);
  return nestedNamesOf(f->body).direct.count(v) ? f : nullptr;
}

auto codegen::Emitter::localType(VariableDecl* v) -> const Ty*
{
  const Ty* t = declType(v);
  if ((!t || !t->isKnown() || types.isDependent(t)) && v->init && v->init->ty)
  {
    // Fall back to the initializer's type when the declared type is unknown (e.g. auto).
    const Ty* initTy = types.stripRef(v->init->ty);
    if (initTy && initTy->isKnown() && !types.isDependent(initTy))
      t = initTy;
  }
  return t;
}

auto codegen::Emitter::staticLocalName(const std::string& function, VariableDecl* v) -> std::string
{
  std::string owner = function;
  if (owner.rfind("@\"", 0) == 0)
    owner = owner.substr(2, owner.size() - 3);
  else if (!owner.empty() && owner[0] == '@')
    owner = owner.substr(1);
  return "@\"" + owner + "::" + sym(v->name.last().symbol) + "." + std::to_string(v->token) + "\"";
}

auto codegen::Emitter::sharedStaticText(const std::string& name, VariableDecl* v, const Ty* t, uint64_t alignment) -> std::string
{
  std::string storage = (v->flags & DF_ThreadLocal) ? " = linkonce_odr thread_local global " : " = linkonce_odr global ";
  std::string value = t->isReference() ? "ptr null, comdat" : llType(t, v) + " zeroinitializer, comdat" + alignSuffix(t, alignment, v);
  return "$" + name.substr(1) + " = comdat any\n" + name + storage + value + "\n";
}

auto codegen::Emitter::addSharedStatic(const std::string& name, const std::string& text) -> void
{
  auto lock = sharedLock();
  if (moduleCount <= 1)
  {
    if (sh.sharedStaticsDefined.insert(name).second)
      moduleDecls[currentModule] += text;
    return;
  }
  const std::string& by = owner.empty() ? currentFunctionName : owner;
  std::vector<Shared::OwnedEntry>& here = sh.ownedEntries[currentModule];
  for (const Shared::OwnedEntry& o : here)
    if (o.owner == by && o.text == text)
      return;
  here.push_back({by, text});
}

auto codegen::Emitter::staticLocalElsewhere(VariableDecl* v, Node* at) -> std::string
{
  std::string name, text;
  if (FunctionDecl* plain = plainWriter(v))
  {
    if (!nestedNamesOf(plain->body).named.count(v))
      fail(at, "the static local " + sym(v->name.last().symbol) + " of " + qualifiedName(plain) +
                   " is named outside the function's emission, not from a lambda or a local class of it");
    // In the function's terms (staticLocal there): no template arguments, the function the context.
    ArgsScope noArgs(*this, nullptr);
    const Ty* t = localType(v);
    if (!t || !t->isKnown() || types.isDependent(t))
      unsupported(at, "the static local " + sym(v->name.last().symbol) + " of unknown type");
    name = staticLocalName(functionName(plain, {}, nullptr), v);
    text = sharedStaticText(name, v, t, variableAlignment(v, plain, at));
  }
  else
  {
    auto lock = sharedLock();
    auto it = globals.find(v);
    if (it == globals.end())
      unsupported(at, "the static local " + sym(v->name.last().symbol) + " of a template's instance or of a lambda, named before its function is emitted");
    name = it->second;
    if (auto shared = sh.sharedStatics.find(name); shared != sh.sharedStatics.end())
      text = shared->second;
  }
  if (!text.empty())
    addSharedStatic(name, text);
  return name;
}

auto codegen::Emitter::staticConstant(VariableDecl* v, const Ty* t) -> std::optional<std::string>
{
  const Ty* st = types.unqualified(t);
  ConstContextScope context(*this, v); // traits in the initializer resolve their names in the declaration's context
  if (std::optional<ConstValue> c = foldAs(foldConst(v->init), st); c && c->isScalar())
  {
    if ((st->kind == TyKind::Int || st->kind == TyKind::Char || st->kind == TyKind::Enum) && !c->isFloat())
      return std::to_string(c->i);
    if (st->kind == TyKind::Float)
      return floatConstant(c->isFloat() ? c->f : double(c->i), st->bits);
    if (st->kind == TyKind::Bool)
      return std::string(c->truthy() ? "true" : "false");
  }
  if (st->kind == TyKind::Pointer && v->init->kind == NodeKind::LiteralExpr && static_cast<LiteralExpr*>(v->init)->keyword == Kw::nullptr_)
    return std::string("null");
  if (st->kind == TyKind::Pointer && v->init->kind == NodeKind::LiteralExpr && static_cast<LiteralExpr*>(v->init)->literalKind == TokenKind::String)
    return stringConstant(static_cast<LiteralExpr*>(v->init));
  if (isClass(st) || st->kind == TyKind::Array || st->kind == TyKind::Pointer)
    return constantData(st, v->init, v, currentArgs ? *currentArgs : TemplateArgs(), v);
  return std::nullopt;
}

auto codegen::Emitter::staticLocal(VariableDecl* v, const Ty* t) -> void
{
  // `static const Map<String, SDL_Scancode>& keyStrings = generate();`: a pointer, bound on the
  // first pass to what the initializer names (an lvalue: a reference bound to a temporary would
  // need the temporary kept for the program).
  bool isRef = t->isReference();
  if (isRef && (!v->init || !(isLvalueExpression(v->init) || (v->init->ty && v->init->ty->isReference()))))
    unsupported(v, "a static local reference bound to a temporary");
  bool shared = nestedNamesOf(bodyNow).named.count(v) != 0;
  FunctionDecl* plain = shared ? plainWriter(v) : nullptr;
  std::string name = staticLocalName(plain ? functionName(plain, {}, nullptr) : currentFunctionName, v);
  // Constant initialization (C++'s [basic.start.static]): a const or constexpr one whose initializer
  // folds, as a global's does (globalForOnce), is a constant of the program, with no guard and nothing
  // done on the pass. FluidBuffer::getFlowRate's `static constexpr double MAX_FLOW_MULTIPLIER = 0.8`
  // checked a guard and loaded the double from memory on every call. (Not one a lambda or a local
  // class names: its copies elsewhere are zero-initialized and set by the guarded pass,
  // staticLocalElsewhere.)
  if (!shared && !isRef && v->init && !(v->flags & DF_ThreadLocal) && isConstValue(v))
    if (std::optional<std::string> init = staticConstant(v, t))
    {
      addOwnedEntry(name + " = internal " + (hasMutableMember(t, v) ? "global " : "constant ") + llType(t, v) + " " + *init +
                    alignSuffix(t, variableAlignment(v, typingContext(), v), v) + "\n");
      {
        auto lock = sharedLock();
        globals[v] = name;
      }
      locals[v] = {name, t, false};
      return;
    }
  std::string guard = name.substr(0, name.size() - 1) + ".guard\"";
  std::string storage = (v->flags & DF_ThreadLocal) ? " = internal thread_local global " : " = internal global ";
  if (shared)
  {
    std::string text = sharedStaticText(name, v, t, variableAlignment(v, typingContext(), v));
    {
      auto lock = sharedLock();
      sh.sharedStatics[name] = text;
    }
    addSharedStatic(name, text);
  }
  else if (isRef)
    addOwnedEntry(name + storage + "ptr null\n");
  else
    addOwnedEntry(name + storage + llType(t, v) + " zeroinitializer" + alignSuffix(t, variableAlignment(v, typingContext(), v), v) + "\n");
  addOwnedEntry(guard + storage + "i1 false\n");
  {
    auto lock = sharedLock();
    globals[v] = name;
  }
  locals[v] = {name, t, isRef};
  std::string done = newLabel("static.done"), init = newLabel("static.init");
  std::string flag = newTemp();
  fn += "  " + flag + " = load i1, ptr " + guard + "\n";
  fn += "  br i1 " + flag + ", label %" + done + ", label %" + init + "\n";
  startBlock(init);
  fn += "  store i1 true, ptr " + guard + "\n";
  if (isRef)
    fn += "  store ptr " + lvalue(v->init) + ", ptr " + name + "\n";
  else if (v->init)
    initializeFrom(name, t, v->init, v);
  else if (isClass(t))
    defaultConstruct(name, t, v);
  else if (t->kind == TyKind::Array && needsConstruction(t, v))
    constructArray(name, t, v);
  destroyTemps();
  emitBranch(done);
  startBlock(done);
}

auto codegen::Emitter::declareTupleBindings(VariableDecl* v) -> void
{
  for (Expr* b : v->structuredBindings)
    if (Decl* own = static_cast<NameExpr*>(b)->resolved; own && own->kind == NodeKind::VariableDecl && static_cast<VariableDecl*>(own)->tupleBinding)
    {
      declareLocal(static_cast<VariableDecl*>(own));
      destroyTemps();
    }
}

auto codegen::Emitter::declareLocal(VariableDecl* v) -> void
{
  const Ty* t = localType(v);
  if ((!t || !t->isKnown() || types.isDependent(t)) && v->init && v->init->kind == NodeKind::NameExpr)
    if (auto local = locals.find(static_cast<NameExpr*>(v->init)->resolved); local != locals.end() && local->second.ty && local->second.ty->kind == TyKind::Vector)
      t = local->second.ty; // `auto xmm3 = xmm6;` of an SSE `auto` local, which the code generator typed
  if ((!t || !t->isKnown() || types.isDependent(t)) && v->init && v->init->kind == NodeKind::CallExpr && v->type &&
      v->type->kind == NodeKind::AutoType && !(v->flags & (DF_Static | DF_ThreadLocal)))
  {
    // `auto xmm0 = _mm_loadu_si128(...)`: an SSE operation, which the code generator types (the
    // type checker does not know them): the local has the vector type of its value.
    auto* call = static_cast<CallExpr*>(v->init);
    Decl* callee = call->callee && call->callee->kind == NodeKind::NameExpr ? static_cast<NameExpr*>(call->callee)->resolved : nullptr;
    if (call->callee && call->callee->kind == NodeKind::NameExpr && (!callee || (callee->kind == NodeKind::FunctionDecl && callee->file && callee->file->isImported)))
      if (std::optional<Value> value = sseBuiltin(static_cast<NameExpr*>(call->callee), call); value && value->ty && value->ty->kind == TyKind::Vector)
      {
        std::string slot = "%v" + std::to_string(temp++);
        allocas += "  " + slot + " = alloca " + llType(value->ty, v) + "\n";
        fn += "  store " + llType(value->ty, v) + " " + value->text + ", ptr " + slot + "\n";
        locals[v] = {slot, value->ty, false};
        return;
      }
  }
  if (!t || !t->isKnown() || types.isDependent(t))
    unsupported(v, "a local of unknown type (" + (v->name.empty() ? std::string("[bindings]") : sym(v->name.last().symbol)) + ")");
  if (types.unqualified(t)->kind == TyKind::Void)
    fail(v, "the variable " + (v->name.empty() ? std::string("[bindings]") : sym(v->name.last().symbol)) + " is of type void, which holds no value");
  // `char buffer[N]` with N no constant the checker could evaluate has the length 0 (writes
  // would run past it).
  for (const Ty* a = types.unqualified(t); a && a->kind == TyKind::Array; a = types.unqualified(a->inner))
    if (a->count == 0)
      fail(v, "the local " + (v->name.empty() ? std::string("[bindings]") : sym(v->name.last().symbol)) +
                  " is an array whose length is no constant the compiler could evaluate");
  // (`thread_local SignalsVector signals;` is static too: the function returns a reference to it.)
  if ((v->flags & (DF_Static | DF_ThreadLocal)) && !(v->flags & DF_Typedef))
  {
    staticLocal(v, t);
    return;
  }
  bool isRef = t->isReference();
  std::string slot;
  if (v == namedReturn && !isRef)
    slot = sretAddr; // built in the caller's result: every return returns it (builtInResult)
  else
  {
    slot = "%v" + std::to_string(temp++);
    allocas += "  " + slot + " = alloca " + (isRef ? std::string("ptr") : llType(t, v) + alignSuffix(t, variableAlignment(v, typingContext(), v), v)) + "\n";
  }
  locals[v] = {slot, t, isRef};
  if (!v->name.empty())
    debugVariable(sym(v->name.last().symbol), v, slot, t);
  if (isRef)
  {
    if (!v->init)
      unsupported(v, "a reference without an initializer");
    // `const Sprite& s(expr);`, `T& r{expr};`: a reference initialised directly binds to the one
    // value, as `= expr` does (a copy into a temporary needs a copy constructor, Sprite's is
    // deleted).
    Expr* init = v->init;
    if (init->kind == NodeKind::FunctionalCastExpr && static_cast<FunctionalCastExpr*>(init)->type == v->type &&
        static_cast<FunctionalCastExpr*>(init)->args.size() == 1)
      init = static_cast<FunctionalCastExpr*>(init)->args[0];
    if (init->kind == NodeKind::BraceInitExpr && !static_cast<BraceInitExpr*>(init)->text && static_cast<BraceInitExpr*>(init)->args.size() == 1 &&
        static_cast<BraceInitExpr*>(init)->designators.empty())
      init = static_cast<BraceInitExpr*>(init)->args[0];
    // An rvalue reference binds no lvalue of its type (C++'s [dcl.init.ref] 5.4.4, as for a parameter:
    // argumentText). Until 2026-11-05 `S&& r = a;` bound a itself, and a move from r moved a.
    // (`auto&& value = this->state;` is an lvalue reference since D's cee07bff, as in C++.)
    if (t->kind == TyKind::RRef && isLvalueExpression(init) && init->ty && init->ty->isKnown() && !types.isDependent(init->ty))
    {
      const Ty* given = types.unqualified(types.stripRef(init->ty)), *bound = types.unqualified(types.stripRef(t));
      std::vector<std::pair<std::string, uint32_t>> path;
      if (given->kind != TyKind::Function &&
          (given == bound || (isClass(given) && isClass(bound) && (classKey(given) == classKey(bound) || basePath(given, bound, path, v)))))
        fail(v, "the " + types.toString(t, &symbols) +
                    " variable given an lvalue of its type: an rvalue reference binds only a temporary or what move() gives, never the object itself");
    }
    // A member of a temporary (`const M& m = wrap(5).inner;`): the whole temporary lives as long as
    // the reference ([class.temporary] 6), never only to the end of the statement, which left the
    // reference dangling (until 2026-11-05). The temporary the member chain starts from is the last
    // one the initializer made: a call's result is a temporary of the statement once the call is
    // done, and a member access makes none.
    const Ty* temporaryBase = nullptr;
    size_t tempsBefore = statementTemps.size();
    {
      Expr* e = init;
      bool member = false;
      while (e && (e->kind == NodeKind::ParenExpr || (e->kind == NodeKind::MemberExpr && static_cast<MemberExpr*>(e)->op != P::Arrow)))
      {
        member = member || e->kind == NodeKind::MemberExpr;
        e = e->kind == NodeKind::ParenExpr ? static_cast<ParenExpr*>(e)->inner : static_cast<MemberExpr*>(e)->base;
      }
      if (member && e && e->ty && !e->ty->isReference() && isClass(e->ty) && !isLvalueExpression(e))
        temporaryBase = types.unqualified(e->ty);
    }
    auto extendTemporaryBase = [&]
    {
      if (!temporaryBase || statementTemps.size() <= tempsBefore)
        return;
      Object made = statementTemps.back();
      if (made.unwindOnly || made.freeMemory || made.exception || !isClass(made.ty) || canonicalClass(made.ty, v) != canonicalClass(temporaryBase, v))
        return;
      statementTemps.pop_back();
      scopes.back().push_back(made);
    };
    std::string addr;
    if (isClass(types.stripRef(t)))
    {
      // `const Sorted& r = path;`, `const String& s = "text";`: a value of another type that is
      // neither of a derived class nor converts by an operator binds to a temporary the class's
      // converting constructor makes, which lives as long as the reference.
      const Ty* target = types.unqualified(types.stripRef(t));
      const Ty* initTy = init->ty ? withCurrentArgs(types.unqualified(types.stripRef(init->ty))) : nullptr;
      if (initTy && initTy->isKnown() && !types.isDependent(initTy) && (!isClass(initTy) || canonicalClass(initTy, v) != canonicalClass(target, v)))
      {
        std::vector<std::pair<std::string, uint32_t>> path;
        const Ty* owner = nullptr;
        TemplateArgs opArgs;
        bool converts = isClass(initTy) && (basePath(initTy, target, path, v) || conversionOperator(initTy, target, v, &owner, false, &opArgs));
        if (!converts && findConstructor(target, {init->ty}, v))
        {
          std::string tmp = newObjectSlot(target, v);
          Expr* one = init;
          constructInto(tmp, target, Span<Expr*>{&one, 1}, v);
          if (needsDestruction(target, v))
            scopes.back().push_back({tmp, target});
          fn += "  store ptr " + tmp + ", ptr " + slot + "\n";
          return;
        }
      }
      Value o = rvalue(init);
      // `const Label& l = wrapper;`: an object of another class is converted, to its base or
      // through its conversion operator (reference_wrapper's `operator T&()`), never bound as
      // if it were the referenced class.
      if (const Ty* target = types.unqualified(types.stripRef(t)); isClass(o.ty) && !types.isDependent(o.ty) && canonicalClass(o.ty, v) != canonicalClass(target, v))
        o = convert(o, target, v);
      addr = o.text;
      if (o.isTemporary)
      {
        for (size_t i = statementTemps.size(); i-- > 0;)
          if (statementTemps[i].addr == addr)
            statementTemps.erase(statementTemps.begin() + i);
        scopes.back().push_back({addr, types.stripRef(t)});
      }
    }
    else
      addr = scalarReferenceTarget(t, init, v);
    extendTemporaryBase();
    fn += "  store ptr " + addr + ", ptr " + slot + "\n";
    return;
  }
  if (v->init)
    initializeFrom(slot, t, v->init, v);
  else if (isClass(t))
    defaultConstruct(slot, t, v);
  else if (t->kind == TyKind::Array && needsConstruction(t, v))
    constructArray(slot, t, v); // `StringView text[2];`
  else
    storeZero(t, slot, v);
  if (needsDestruction(t, v))
    scopes.back().push_back({slot, t});
}

auto codegen::Emitter::debugLine(Node* at) -> void
{
  if (debugFile && debugFile == currentFile && inlining.empty())
    if (size_t line = lineOf(at, currentFile))
      fn += "  ;@" + std::to_string(line) + (debugBlock ? ":" + std::to_string(debugBlock) : std::string()) + "\n";
}

auto codegen::Emitter::openDebugBlock(Node* at) -> uint32_t
{
  uint32_t outer = debugBlock;
  if (sh.fullDebugInfo && debugFile && debugFile == currentFile && inlining.empty() && at != bodyNow)
  {
    debugBlocks.push_back({outer, lineOf(at, currentFile)});
    debugBlock = uint32_t(debugBlocks.size());
  }
  return outer;
}

auto codegen::Emitter::statement(Stmt* s) -> void
{
  if (!s)
    return;
  if (++recursionDepth > 128)
  {
    recursionDepth = 0;
    fail(s, "statement nested too deeply");
  }
  struct DepthGuard { int& d; ~DepthGuard() { --d; } } dg{recursionDepth};
  if (blockTerminated)
    startBlock(newLabel("dead"));
  debugLine(s);
  switch (s->kind)
  {
    case NodeKind::BlockStmt:
    {
      uint32_t outer = openDebugBlock(s);
      pushScope();
      for (Stmt* inner : static_cast<BlockStmt*>(s)->stmts)
        statement(inner);
      popScope();
      debugBlock = outer;
      break;
    }
    case NodeKind::ExprStmt:
      if (static_cast<ExprStmt*>(s)->expr)
      {
        layOutClosuresIn(static_cast<ExprStmt*>(s)->expr);
        rvalue(static_cast<ExprStmt*>(s)->expr);
      }
      destroyTemps();
      break;
    case NodeKind::GotoStmt:
    {
      // A jump to a label of this function destroys what it leaves ([stmt.jump] 2): the objects of
      // the scopes it leaves and, jumping back, those its label's scope built after the label. Until
      // 2026-11-05 none (`{ M m(n); goto again; }` destroyed no m: its comment took Factorio's labels
      // for jumps past nothing to destroy).
      uint32_t symbol = static_cast<GotoStmt*>(s)->label;
      if (!labelBlocks.count(symbol))
        labelBlocks[symbol] = newLabel("goto");
      if (auto place = labelPlaces.find(symbol); place != labelPlaces.end())
      {
        size_t common = 0;
        while (common < place->second.ids.size() && common < scopeIds.size() && place->second.ids[common] == scopeIds[common])
          ++common;
        unwindRange(common, scopes.size());
        if (common > 0 && common - 1 < place->second.counts.size())
        {
          std::vector<Object>& shared = scopes[common - 1];
          for (size_t i = shared.size(); i-- > place->second.counts[common - 1];)
            destroyOnExit(shared[i]); // (they stay listed: this path ends in the jump)
        }
        fn += "  br label %" + labelBlocks[symbol] + "\n";
      }
      else
      {
        std::string trampoline = newLabel("goto.leave");
        pendingGotos[symbol].push_back({trampoline, scopeIds, scopes});
        fn += "  br label %" + trampoline + "\n";
      }
      blockTerminated = true;
      break;
    }
    case NodeKind::LabelStmt:
    {
      uint32_t symbol = static_cast<LabelStmt*>(s)->label;
      if (!labelBlocks.count(symbol))
        labelBlocks[symbol] = newLabel("goto");
      emitBranch(labelBlocks[symbol]);
      // The forward gotos to it: each one's trampoline destroys the objects of the scopes its jump
      // leaves (those beyond the scopes it shares with the label), the innermost first.
      if (auto pending = pendingGotos.find(symbol); pending != pendingGotos.end())
      {
        for (const PendingGoto& g : pending->second)
        {
          startBlock(g.trampoline);
          size_t common = 0;
          while (common < g.ids.size() && common < scopeIds.size() && g.ids[common] == scopeIds[common])
            ++common;
          for (size_t scope = g.objects.size(); scope-- > common;)
            for (size_t i = g.objects[scope].size(); i-- > 0;)
              destroyOnExit(g.objects[scope][i]);
          emitBranch(labelBlocks[symbol]);
        }
        pendingGotos.erase(pending);
      }
      startBlock(labelBlocks[symbol]);
      LabelPlace place;
      place.ids = scopeIds;
      for (const std::vector<Object>& scope : scopes)
        place.counts.push_back(scope.size());
      labelPlaces[symbol] = place;
      break;
    }
    case NodeKind::DeclStmt:
      for (Decl* d : static_cast<DeclStmt*>(s)->decls)
      {
        if (d->when && d->whenKnown < 0)
          unsupported(d, "a when declaration the checker did not decide");
        if (d->whenKnown == 0)
          continue; // `when (false)` (design.md 19): no storage, no constructor, no destructor
        if (d->kind == NodeKind::VariableDecl)
        {
          layOutClosuresIn(static_cast<VariableDecl*>(d)->init); // the variable's slot may hold one
          declareLocal(static_cast<VariableDecl*>(d));
          destroyTemps();
          declareTupleBindings(static_cast<VariableDecl*>(d));
        }
        else if (d->kind == NodeKind::ClassDecl || d->kind == NodeKind::EnumDecl || d->kind == NodeKind::UsingAliasDecl ||
                 d->kind == NodeKind::UsingDirectiveDecl || d->kind == NodeKind::UsingNameDecl ||
                 d->kind == NodeKind::EmptyDecl || d->kind == NodeKind::StaticAssertDecl)
          continue; // names only (`using namespace gui;` in a body): resolved, nothing to emit
        else
          unsupported(d, "this local declaration");
      }
      break;
    case NodeKind::EmptyStmt:
      break;
    case NodeKind::ReturnStmt:
    {
      auto* r = static_cast<ReturnStmt*>(s);
      layOutClosuresIn(r->value);
      std::string retText;
      if (currentReturn->kind == TyKind::Void)
      {
        if (r->value)
          rvalue(r->value);
      }
      else if (returnsInMemory(currentReturn))
      {
        if (!r->value)
          unsupported(s, "return without a value");
        if (returnsNamedReturn(r->value))
        {
          // Built in the result already: the scopes left keep it (design.md 23). A declaration
          // namedReturnVariable took for one that is built elsewhere would return the result
          // unwritten: an error, never that.
          auto local = locals.find(namedReturn);
          if (local == locals.end() || local->second.addr != sretAddr)
            fail(s, "the named return " + (namedReturn->name.empty() ? std::string("?") : sym(namedReturn->name.last().symbol)) +
                        " was not built in the result (namedReturnVariable took a declaration codegen builds elsewhere)");
          keptOnReturn = sretAddr;
        }
        else if (returnedLocalObject(r->value))
          moveConstruct(sretAddr, lvalue(r->value), currentReturn, s); // `return result;`: the local is moved, as in C++
        else if (const Ty* local = localObjectOfOtherClass(r->value); local && findConstructor(currentReturn, {types.rref(local)}, s))
        {
          // `return obj;` with obj a local of another class (UniquePtr<Derived> returned as
          // UniquePtr<Base>): C++ takes the local as an rvalue first, for the converting
          // constructor that takes one.
          NamedCastExpr moved{};
          moved.kind = NodeKind::NamedCastExpr;
          moved.token = r->value->token;
          moved.castKind = Kw::static_cast_;
          moved.operand = r->value;
          moved.ty = types.rref(local);
          initializeFrom(sretAddr, currentReturn, &moved, s);
        }
        else
          initializeFrom(sretAddr, currentReturn, r->value, s);
      }
      else if (currentReturn->isReference())
      {
        if (!r->value)
          unsupported(s, "return without a value in a function returning a reference");
        if (isAggregate(types.stripRef(currentReturn)))
        {
          Value v = rvalue(r->value);
          if (v.text.empty() || !v.ty || v.ty->kind == TyKind::Void)
            fail(r->value, "a function returning " + types.toString(currentReturn, &symbols) + " returns an expression without a value (of type void)");
          // A derived object returned as a base's reference is that base's subobject:
          // `PendingRobotsSetHook& asPendingRobotsHook() { return *this; }` returned the robot's
          // start, and unlinking that "hook" walked the vtable as its tree.
          const Ty* want = types.unqualified(types.stripRef(currentReturn));
          if (isClass(v.ty) && isClass(want) && classKey(v.ty) != classKey(want))
            v = convert(v, want, s);
          retText = v.text;
        }
        else
        {
          // A reference result of another scalar type than the value returned (`const double& f(const
          // int& i) { return i; }`): an error, where another type's would be a temporary that dies
          // with the return (and a non-const reference binds none). Until 2026-11-05 the int's own
          // bytes were returned as the double.
          const Ty* want = types.unqualified(types.stripRef(currentReturn));
          const Ty* have = r->value->ty && r->value->ty->isKnown() && !types.isDependent(r->value->ty) ? types.unqualified(types.stripRef(r->value->ty)) : nullptr;
          if (have && want->isKnown() && !types.isDependent(want) && !isAggregate(have) && have != want && llType(have, s) != llType(want, s))
            fail(r->value, "a function returning " + types.toString(currentReturn, &symbols) + " returns a value of type " + types.toString(have, &symbols) +
                               ": a reference result refers only to an object of its type (another type's would be a temporary, which dies with the return)");
          retText = lvalue(r->value);
        }
      }
      else
      {
        if (!r->value)
          unsupported(s, "return without a value in a non-void function");
        if (r->value->kind == NodeKind::BraceInitExpr)
          retText = braceScalar(static_cast<BraceInitExpr*>(r->value), currentReturn, s).text; // `return {};` of an enum
        else
          retText = convert(rvalue(r->value), currentReturn, s).text;
      }
      destroyTemps();
      std::string retLl = currentReturn->kind == TyKind::Void || returnsInMemory(currentReturn) ? std::string()
                          : currentReturn->isReference()                                ? std::string("ptr")
                                                                                         : llType(currentReturn, s);
      if (!retLl.empty() && !currentFunclet().empty())
      {
        // Leaving a catch handler: the value crosses a funclet boundary through memory.
        std::string slot = "%v" + std::to_string(temp++);
        allocas += "  " + slot + " = alloca " + retLl + "\n";
        fn += "  store " + retLl + " " + retText + ", ptr " + slot + "\n";
        leaveScopes(0);
        retText = newTemp();
        fn += "  " + retText + " = load " + retLl + ", ptr " + slot + "\n";
      }
      else
        leaveScopes(0);
      keptOnReturn.clear();
      if (retLl.empty() && !returnLabel.empty())
        fn += "  br label %" + returnLabel + "\n"; // a destructor's: its members and bases are destroyed there
      else if (retLl.empty())
        emitReturnVoid(); // (a C-callable function's struct result in registers: from its slot)
      else
        fn += "  ret " + retLl + " " + retText + "\n";
      blockTerminated = true;
      break;
    }
    case NodeKind::IfStmt:
    {
      auto* i = static_cast<IfStmt*>(s);
      if (i->ifConsteval)
      {
        // `if consteval`: the program's branch alone (the other runs only at compile time).
        subStatement(i->ifConsteval == 1 ? i->else_ : i->then);
        break;
      }
      if (i->isConstexpr && i->constexprKnown >= 0)
      {
        // The resolver already folded this if constexpr during instantiation.
        subStatement(i->constexprKnown ? i->then : i->else_);
        break;
      }
      if (i->isConstexpr && !i->init && i->cond && i->cond->kind != NodeKind::VariableDecl && i->cond->kind != NodeKind::DeclStmt)
        if (std::optional<bool> known = evalCondition(static_cast<Expr*>(i->cond)))
        {
          // if constexpr with a known condition: only the taken branch exists.
          subStatement(*known ? i->then : i->else_);
          break;
        }
      pushScope();
      if (i->init)
        statement(i->init);
      Value cond = condition(i->cond, s);
      destroyTemps();
      std::string tL = newLabel("if.then"), eL = newLabel("if.else"), jL = newLabel("if.end");
      fn += "  br i1 " + cond.text + ", label %" + tL + ", label %" + (i->else_ ? eL : jL) + "\n";
      startBlock(tL);
      subStatement(i->then);
      emitBranch(jL);
      if (i->else_)
      {
        startBlock(eL);
        subStatement(i->else_);
        emitBranch(jL);
      }
      startBlock(jL);
      popScope();
      break;
    }
    case NodeKind::AssertStmt:
    {
      auto* a = static_cast<AssertStmt*>(s);
      if (!a->release && !a->testKind && !debug)
        break; // assert compiles to nothing outside debug builds
      Value cond = condition(a->cond, s);
      destroyTemps();
      std::string okL = newLabel("assert.ok"), failL = newLabel("assert.fail");
      fn += "  br i1 " + cond.text + ", label %" + okL + ", label %" + failL + "\n";
      startBlock(failL);
      auto [line, column] = lineColumnOf(a->token);
      if (a->testKind)
      {
        // check and require in a test (design.md 8.6): the test runner records the failure; a
        // require's call throws to end the test (compiler/src/tests.cpp).
        emitCall("", "void @kpp_check_failed(ptr " + cStringConstant(programPathOf(currentFile)) + ", i32 " + std::to_string(line) + ", i32 " +
                         std::to_string(column) + ", ptr " + cStringConstant(sourceText(a->startToken, a->endToken)) + ", i32 " +
                         std::to_string(a->testKind) + ")", a->testKind == 2);
        emitBranch(okL);
        startBlock(okL);
        break;
      }
      emitCall("", "void @kpp_assert_failed(ptr " + cStringConstant(programPathOf(currentFile)) + ", i32 " + std::to_string(line) + ", i32 " +
                       std::to_string(column) + ", ptr " + cStringConstant(sourceText(a->startToken, a->endToken)) + ")", false);
      fn += "  unreachable\n";
      usesAssert = true;
      startBlock(okL);
      break;
    }
    case NodeKind::WhileStmt:
    {
      auto* w = static_cast<WhileStmt*>(s);
      std::string cL = newLabel("while.cond"), bL = newLabel("while.body"), eL = newLabel("while.end");
      emitBranch(cL);
      startBlock(cL);
      Value cond = condition(w->cond, s);
      destroyTemps();
      fn += "  br i1 " + cond.text + ", label %" + bL + ", label %" + eL + "\n";
      startBlock(bL);
      loops.push_back({eL, cL, scopes.size()});
      subStatement(w->body);
      loops.pop_back();
      emitBranch(cL);
      startBlock(eL);
      break;
    }
    case NodeKind::DoStmt:
    {
      auto* d = static_cast<DoStmt*>(s);
      std::string bL = newLabel("do.body"), cL = newLabel("do.cond"), eL = newLabel("do.end");
      emitBranch(bL);
      startBlock(bL);
      loops.push_back({eL, cL, scopes.size()});
      subStatement(d->body);
      loops.pop_back();
      emitBranch(cL);
      startBlock(cL);
      debugLine(d->cond);
      Value cond = convert(rvalue(d->cond), types.boolType(), s);
      destroyTemps();
      fn += "  br i1 " + cond.text + ", label %" + bL + ", label %" + eL + "\n";
      startBlock(eL);
      break;
    }
    case NodeKind::ForStmt:
    {
      auto* f = static_cast<ForStmt*>(s);
      pushScope();
      if (f->init)
        statement(f->init);
      std::string cL = newLabel("for.cond"), bL = newLabel("for.body"), sL = newLabel("for.step"), eL = newLabel("for.end");
      emitBranch(cL);
      startBlock(cL);
      if (f->cond)
      {
        debugLine(f->cond);
        Value cond = condition(f->cond, s);
        destroyTemps();
        fn += "  br i1 " + cond.text + ", label %" + bL + ", label %" + eL + "\n";
      }
      else
        fn += "  br label %" + bL + "\n";
      startBlock(bL);
      loops.push_back({eL, sL, scopes.size()});
      subStatement(f->body);
      loops.pop_back();
      emitBranch(sL);
      startBlock(sL);
      if (f->step)
      {
        debugLine(f->step);
        rvalue(f->step);
        destroyTemps();
      }
      emitBranch(cL);
      startBlock(eL);
      popScope();
      break;
    }
    case NodeKind::BreakStmt:
      if (loops.empty())
        unsupported(s, "break outside a loop or switch");
      leaveScopes(loops.back().scopeDepth);
      emitBranch(loops.back().breakLabel);
      break;
    case NodeKind::ContinueStmt:
    {
      for (size_t i = loops.size(); i-- > 0;)
        if (!loops[i].continueLabel.empty())
        {
          leaveScopes(loops[i].scopeDepth);
          emitBranch(loops[i].continueLabel);
          return;
        }
      unsupported(s, "continue outside a loop");
    }
    case NodeKind::SwitchStmt:
      switchStatement(static_cast<SwitchStmt*>(s));
      break;
    case NodeKind::TryStmt:
      tryStatement(static_cast<TryStmt*>(s));
      break;
    case NodeKind::RangeForStmt:
      rangeFor(static_cast<RangeForStmt*>(s));
      break;
    case NodeKind::ForConstexprStmt:
      forConstexpr(static_cast<ForConstexprStmt*>(s));
      break;
    case NodeKind::CaseStmt:
    case NodeKind::DefaultStmt:
      unsupported(s, "a case label outside a switch body");
    case NodeKind::FallthroughStmt:
      break;
    default:
      unsupported(s, std::string("the statement kind ") + nodeKindName(s->kind));
  }
}

auto codegen::Emitter::findMember(const Ty* classTy, const std::string& name, Node* at, const Ty** foundIn) -> FunctionDecl*
{
  return findMemberFunction(classTy, name, {}, classTy, at, foundIn);
}

auto codegen::Emitter::iteratorLoop(RangeForStmt* f, Value begin, Value end) -> void
{
  auto own = [&](const Value& v)
  {
    for (size_t i = statementTemps.size(); i-- > 0;)
      if (statementTemps[i].addr == v.text)
        statementTemps.erase(statementTemps.begin() + i);
    if (needsDestruction(v.ty, f))
      scopes.back().push_back({v.text, v.ty});
  };
  own(begin);
  own(end);
  destroyTemps();
  // `it != end`, or `!(it == end)`; end may be of another type than the iterator (a sentinel:
  // CircleIterator's `nullptr_t end()` and `bool operator!=(nullptr_t)`).
  const Ty* in = nullptr;
  std::vector<const Ty*> argTypes{isClass(end.ty) ? types.lref(types.qualified(end.ty, true, false)) : end.ty};
  bool negate = false;
  FunctionDecl* compare = findMemberFunction(begin.ty, "operator!=", argTypes, begin.ty, f, &in);
  if (!compare)
  {
    compare = findMemberFunction(begin.ty, "operator==", argTypes, begin.ty, f, &in);
    negate = true;
  }
  if (!compare)
    unsupported(f, "an iterator without operator!= or operator==");
  TemplateArgs compareArgs = global.classArgsOf(in);
  std::string compareName = useFunction(compare, compareArgs, in);
  Signature compareSig;
  {
    ArgsScope scope(*this, &compareArgs);
    compareSig = signatureOf(compare);
  }
  std::string cL = newLabel("iter.cond"), bL = newLabel("iter.body"), sL = newLabel("iter.step"), eL = newLabel("iter.end");
  emitBranch(cL);
  startBlock(cL);
  std::string thisArg = begin.text;
  if (canonicalClass(begin.ty, f) != in)
    baseAddress(begin.ty, in, begin.text, thisArg, f);
  std::string endArg = "ptr " + end.text;
  if (!isClass(end.ty) && compareSig.paramTys.size() > 1 && compareSig.paramTys[1])
  {
    const Ty* pt = compareSig.paramTys[1];
    if (pt->isReference())
    {
      std::string slot = newObjectSlot(end.ty, f);
      store(end, slot, end.ty, f);
      endArg = "ptr " + slot;
    }
    else
      endArg = compareSig.paramTypes[1] + " " + convert(end, pt, f).text;
  }
  std::string cond = newTemp();
  emitCall(cond + " = ", compareSig.returnType + " " + compareName + "(ptr " + thisArg + ", " + endArg + ")");
  if (compareSig.returnType != "i1")
    cond = convert(Value{cond, types.intType(32)}, types.boolType(), f).text;
  if (negate)
  {
    std::string flipped = newTemp();
    fn += "  " + flipped + " = xor i1 " + cond + ", true\n";
    cond = flipped;
  }
  fn += "  br i1 " + cond + ", label %" + bL + ", label %" + eL + "\n";
  startBlock(bL);
  pushScope();
  // The loop variable from `*it`: a reference binds to the element, a value copies it.
  auto* v = static_cast<VariableDecl*>(f->var);
  const Ty* vt = declType(v);
  if (!vt || !vt->isKnown() || types.isDependent(vt))
    unsupported(v, "a loop variable of unknown type");
  Value element = operatorCall(begin, "operator*", {}, f, true);
  std::string slot = "%v" + std::to_string(temp++);
  if (vt->isReference())
  {
    std::string addr = element.text;
    if (!element.ty->isReference() && !isClass(element.ty))
    {
      addr = newObjectSlot(element.ty, v);
      fn += "  store " + llType(element.ty, v) + " " + element.text + ", ptr " + addr + "\n";
    }
    if (elementMadeInto(element.ty, vt))
    {
      // `for (const String& name : names)` over a Vector<const char*>: a String made from the
      // element, the reference's (it lives for the iteration).
      const Ty* cls = types.unqualified(types.stripRef(vt));
      std::string made = newObjectSlot(cls, v);
      makeFromElement(made, cls, addr, element.ty, v);
      if (needsDestruction(cls, v))
        scopes.back().push_back({made, cls});
      addr = made;
    }
    else
      addr = elementAs(addr, element.ty, vt, v);
    allocas += "  " + slot + " = alloca ptr\n";
    fn += "  store ptr " + addr + ", ptr " + slot + "\n";
    locals[v] = {slot, vt, true};
  }
  else
  {
    if (v == namedReturn)
      slot = sretAddr; // built in the caller's result, each iteration (builtInResult)
    else
      allocas += "  " + slot + " = alloca " + llType(vt, v) + "\n";
    if (elementMadeInto(element.ty, vt))
    {
      // `for (String name : names)` over a Vector<const char*>: made from the element.
      std::string addr = resultAddress(element, v);
      makeFromElement(slot, vt, addr, element.ty, v);
    }
    else if (isClass(vt) && elementOfOtherClass(element.ty, vt, v))
      copyConstruct(slot, elementAs(element.text, element.ty, vt, v), vt, v); // an element of another class, converted
    else if (isClass(vt))
    {
      if (element.ty->isReference() || !element.isTemporary)
        copyConstruct(slot, element.text, vt, v);
      else
        moveTemporary(slot, element, vt, v);
    }
    else
      store(callRvalue(element, v), slot, vt, v);
    locals[v] = {slot, vt, false};
    if (needsDestruction(vt, v))
      scopes.back().push_back({slot, vt});
  }
  destroyTemps();
  declareTupleBindings(v); // `for (auto& [a, b] : tuples)`: each element's get<i>
  loops.push_back({eL, sL, scopes.size()});
  subStatement(f->body);
  loops.pop_back();
  popScope();
  emitBranch(sL);
  startBlock(sL);
  operatorCall(begin, "operator++", {}, f, true);
  destroyTemps();
  emitBranch(cL);
  startBlock(eL);
}
