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
  // (`static auto [a, b] = pair;`: its hidden object has no name, its token tells it apart.)
  return "@\"" + owner + "::" + (v->name.empty() ? std::string("[bindings]") : sym(v->name.last().symbol)) + "." + std::to_string(v->token) + "\"";
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
  if (isRef && !v->init)
    unsupported(v, "a reference without an initializer");
  if (isRef && (v->flags & DF_ThreadLocal) && !(isLvalueExpression(v->init) || (v->init->ty && v->init->ty->isReference())))
    unsupported(v, "a thread_local reference bound to a temporary");
  bool shared = nestedNamesOf(bodyNow).named.count(v) != 0;
  FunctionDecl* plain = shared ? plainWriter(v) : nullptr;
  std::string name = staticLocalName(plain ? functionName(plain, {}, nullptr) : currentFunctionName, v);
  // Constant initialization (C++'s [basic.start.static]): one whose initializer folds, as a global's
  // does (globalForOnce), const or not, has its value from the program's start, with no guard and
  // nothing done on the pass; a const one is a constant, any other a global the program writes.
  // FluidBuffer::getFlowRate's `static constexpr double MAX_FLOW_MULTIPLIER = 0.8` checked a guard
  // and loaded the double from memory on every call; until 2026-11-05 a non-const one
  // (`static int counter = 5;`) was zero until its declaration ran, so a goto or a case label past
  // it read 0 where C++ reads 5. (Not one a lambda or a local class names: its copies elsewhere are
  // zero-initialized and set by the guarded pass, staticLocalElsewhere.)
  // (One with a destructor has its value from the start too, and the guarded pass below registers
  // its destruction: `static const Cd c(1);`, destroyed only if its declaration was passed.)
  bool constantValue = false;
  if (!shared && !isRef && v->init && !(v->flags & DF_ThreadLocal))
    if (std::optional<std::string> init = staticConstant(v, t))
    {
      bool destroyed = needsDestruction(t, v);
      bool writable = !isConstValue(v) || hasMutableMember(t, v) || destroyed;
      addOwnedEntry(name + " = internal " + (writable ? "global " : "constant ") + llType(t, v) + " " + *init +
                    alignSuffix(t, variableAlignment(v, typingContext(), v), v) + "\n");
      {
        auto lock = sharedLock();
        globals[v] = name;
      }
      locals[v] = {name, t, false};
      if (!destroyed)
        return;
      constantValue = true;
    }
  // (A reference bound to an object of static storage the same: `static int& r = x;` past a case label.)
  if (!shared && isRef && !(v->flags & DF_ThreadLocal))
    if (std::optional<std::string> target = referenceConstant(t, v->init, v, currentArgs ? *currentArgs : TemplateArgs(), v))
    {
      addOwnedEntry(name + " = internal constant ptr " + *target + "\n");
      {
        auto lock = sharedLock();
        globals[v] = name;
      }
      locals[v] = {name, t, true};
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
  else if (!constantValue)
    addOwnedEntry(name + storage + llType(t, v) + " zeroinitializer" + alignSuffix(t, variableAlignment(v, typingContext(), v), v) + "\n");
  addOwnedEntry(guard + storage + "i8 0\n");
  {
    auto lock = sharedLock();
    globals[v] = name;
  }
  locals[v] = {name, t, isRef};
  // Built on the first pass whose initializer returns, once per program across threads ([stmt.dcl]
  // 4). The guard is 0 not built, 1 being built, 2 built: a pass that finds 2 goes on; one that claims
  // 0 for 1 (cmpxchg) builds it and releases 2, or, the initializer throwing, gives 0 back (an
  // unwind-only cleanup) for the next pass to try again; one that finds another thread building waits
  // for it. A thread_local one is its thread's alone: 0 or 1, set once the initializer returns. Until
  // 2026-11-05 the guard was set before the initializer ran: a constructor that threw left a
  // half-built object every later call used, and a second thread read the object before it was built.
  bool perThread = (v->flags & DF_ThreadLocal) != 0;
  std::string done = newLabel("static.done"), init = newLabel("static.init");
  std::string state = newTemp(), built = newTemp();
  fn += "  " + state + " = load atomic i8, ptr " + guard + " acquire, align 1\n";
  fn += "  " + built + " = icmp eq i8 " + state + ", " + (perThread ? "1" : "2") + "\n";
  if (perThread)
    fn += "  br i1 " + built + ", label %" + done + ", label %" + init + "\n";
  else
  {
    std::string claim = newLabel("static.claim"), wait = newLabel("static.wait"), spin = newLabel("static.spin");
    fn += "  br i1 " + built + ", label %" + done + ", label %" + claim + "\n";
    startBlock(claim);
    std::string pair = newTemp(), won = newTemp();
    fn += "  " + pair + " = cmpxchg ptr " + guard + ", i8 0, i8 1 acq_rel acquire, align 1\n";
    fn += "  " + won + " = extractvalue { i8, i1 } " + pair + ", 1\n";
    fn += "  br i1 " + won + ", label %" + init + ", label %" + wait + "\n";
    startBlock(wait);
    std::string seen = newTemp(), finished = newTemp();
    fn += "  " + seen + " = load atomic i8, ptr " + guard + " acquire, align 1\n";
    fn += "  " + finished + " = icmp eq i8 " + seen + ", 2\n";
    fn += "  br i1 " + finished + ", label %" + done + ", label %" + spin + "\n";
    startBlock(spin);
    // (another thread builds it, or gave it back: claim again; x86's pause, AArch64's yield)
    fn += std::string("  call void asm sideeffect \"") + (sh.target.arm64() ? "yield" : "pause") + "\", \"~{memory}\"()" + bundle() + "\n";
    fn += "  br label %" + claim + "\n";
  }
  startBlock(init);
  if (!perThread)
  {
    Object giveBack{guard, types.intType(8)}; // (a byte: no class, nothing that destroys it)
    giveBack.unwindOnly = true;
    giveBack.guardReset = guard;
    statementTemps.push_back(giveBack);
  }
  std::vector<std::pair<std::string, const Ty*>> outerTemporaries = std::move(staticTemporaries); // (of a static inlined into another's initializer)
  staticTemporaries.clear();
  if (constantValue)
    ; // (its value is there)
  else if (isRef)
    fn += "  store ptr " + staticReferenceTarget(v, name, t, v->init, v, perThread) + ", ptr " + name + "\n";
  else if (v->init && isClass(t))
    initializeExtending(name, t, v, v->init, name, (v->flags & DF_ThreadLocal) != 0);
  else if (v->init)
    initializeFrom(name, t, v->init, v);
  else if (isClass(t))
    defaultConstruct(name, t, v);
  else if (t->kind == TyKind::Array && needsConstruction(t, v))
    constructArray(name, t, v);
  destroyTemps(); // (the claim's cleanup with them: the initializer returned)
  if (perThread && !isRef)
    registerThreadExits(name, t, v); // destroyed when the thread ends ([basic.stc.thread] 2)
  else if (!perThread)
    registerProgramExits(name, isRef ? types.voidType() : t, v); // destroyed at the program's end, as a global ([basic.start.term] 3)
  staticTemporaries = std::move(outerTemporaries);
  fn += "  store atomic i8 " + std::string(perThread ? "1" : "2") + ", ptr " + guard + " release, align 1\n";
  emitBranch(done);
  startBlock(done);
}

auto codegen::Emitter::initializeExtending(const std::string& slot, const Ty* t, VariableDecl* v, Expr* init, const std::string& staticName, bool threadLocal) -> void
{
  // The elements of its brace lists (`Outer o{{T(3)}, 4}`), not those of a list inside an element
  // (a call's argument `f(Holder{T(2)})`, whose temporary dies with the statement).
  std::vector<const Expr*> elements;
  auto collect = [&](auto& self, Expr* e) -> void
  {
    while (e && e->kind == NodeKind::ParenExpr)
      e = static_cast<ParenExpr*>(e)->inner;
    if (!e)
      return;
    if (e->kind == NodeKind::BraceInitExpr && !static_cast<BraceInitExpr*>(e)->text)
      for (Expr* a : static_cast<BraceInitExpr*>(e)->args)
        self(self, a);
    else
      elements.push_back(e);
  };
  if (init->kind == NodeKind::BraceInitExpr)
    collect(collect, init);
  if (elements.empty())
  {
    initializeFrom(slot, t, init, v);
    return;
  }
  const std::vector<const Expr*>* outerElements = extendingElements;
  std::vector<std::string> outerExtended = std::move(extendedTemporaries);
  std::string outerStatic = std::move(extendingStatic);
  bool outerThreadLocal = extendingThreadLocal;
  uint32_t outerCount = extendingCount;
  extendedTemporaries.clear();
  extendingElements = &elements;
  extendingStatic = staticName;
  extendingThreadLocal = threadLocal;
  extendingCount = 0;
  initializeFrom(slot, t, init, v);
  extendingElements = outerElements;
  extendingStatic = std::move(outerStatic);
  extendingThreadLocal = outerThreadLocal;
  extendingCount = outerCount;
  // A local's: destroyed when its scope ends, after v (pushed after them).
  for (const std::string& addr : extendedTemporaries)
    for (size_t i = statementTemps.size(); i-- > 0;)
      if (statementTemps[i].addr == addr && !statementTemps[i].unwindOnly && !statementTemps[i].freeMemory && !statementTemps[i].exception)
      {
        scopes.back().push_back(statementTemps[i]);
        statementTemps.erase(statementTemps.begin() + i);
        break;
      }
  extendedTemporaries = std::move(outerExtended);
}

auto codegen::Emitter::staticTemporary(const Ty* type, Node* at) -> std::string
{
  std::string name = extendingStatic.substr(0, extendingStatic.size() - 1) + ".temp" + std::to_string(extendingCount++) + "\"";
  addOwnedEntry(name + (extendingThreadLocal ? " = internal thread_local global " : " = internal global ") + llType(type, at) + " zeroinitializer" +
                alignSuffix(type, 0, at) + "\n");
  if (needsDestruction(type, at))
    staticTemporaries.push_back({name, type}); // (destroyed at the program's or the thread's exit, after the variable)
  return name;
}

auto codegen::Emitter::referenceInitializer(VariableDecl* v, Expr* init) -> Expr*
{
  if (init->kind == NodeKind::FunctionalCastExpr && static_cast<FunctionalCastExpr*>(init)->type == v->type &&
      static_cast<FunctionalCastExpr*>(init)->args.size() == 1)
    init = static_cast<FunctionalCastExpr*>(init)->args[0];
  if (init->kind == NodeKind::BraceInitExpr && !static_cast<BraceInitExpr*>(init)->text && static_cast<BraceInitExpr*>(init)->args.size() == 1 &&
      static_cast<BraceInitExpr*>(init)->designators.empty())
    init = static_cast<BraceInitExpr*>(init)->args[0];
  return init;
}

auto codegen::Emitter::temporaryOfSubobject(Expr* init) const -> Expr*
{
  // `.member` (not `->`), a built-in subscript of an array (`make().items[1]`: an array of a
  // temporary is a part of it; a pointer's or an operator[]'s element is not) and a static_cast or
  // const_cast to a reference (`static_cast<T&&>(make().t)`) designate a part of their operand.
  // Until 2026-11-07 only `.` was followed: `const T& x = make().items[1];` destroyed the
  // temporary at the end of the statement and left x dangling.
  Expr* e = init;
  bool part = false;
  for (;;)
  {
    if (!e)
      return nullptr;
    if (e->kind == NodeKind::ParenExpr)
      e = static_cast<ParenExpr*>(e)->inner;
    else if (e->kind == NodeKind::MemberExpr && static_cast<MemberExpr*>(e)->op != P::Arrow && !static_cast<MemberExpr*>(e)->dynamicMember)
      part = true, e = static_cast<MemberExpr*>(e)->base;
    else if (Expr* b = e->kind == NodeKind::IndexExpr ? static_cast<IndexExpr*>(e)->base : nullptr;
             b && b->ty && types.unqualified(types.stripRef(b->ty))->kind == TyKind::Array)
      part = true, e = b;
    else if (e->kind == NodeKind::NamedCastExpr && e->ty && e->ty->isReference() &&
             (static_cast<NamedCastExpr*>(e)->castKind == Kw::static_cast_ || static_cast<NamedCastExpr*>(e)->castKind == Kw::const_cast_))
      e = static_cast<NamedCastExpr*>(e)->operand;
    else
      break;
  }
  return part && e->ty && !e->ty->isReference() && isClass(e->ty) && !isLvalueExpression(e) ? e : nullptr;
}

auto codegen::Emitter::staticReferenceTarget(VariableDecl* v, const std::string& name, const Ty* t, Expr* init, Node* at, bool perThread) -> std::string
{
  init = referenceInitializer(v, init);
  {
    // A member of a temporary (`const int& n = make().id;`) would keep the whole temporary.
    if (temporaryOfSubobject(init))
      unsupported(at, "a reference of static storage bound to a member of a temporary");
  }
  uint32_t temporaries = 0;
  auto temporary = [&](const Ty* type)
  {
    extendingStatic = name;
    extendingThreadLocal = perThread;
    extendingCount = temporaries++;
    std::string made = staticTemporary(type, at);
    extendingStatic.clear();
    return made;
  };
  const Ty* target = types.unqualified(types.stripRef(t));
  const Ty* given = init->ty && init->ty->isKnown() && !types.isDependent(init->ty) ? withCurrentArgs(types.unqualified(types.stripRef(init->ty))) : nullptr;
  bool named = isLvalueExpression(init) || (init->ty && init->ty->isReference());
  if (!isClass(target) && !(isLambda(target) && given && isClass(given)))
  {
    if (named || (given && isClass(given)) || target->kind == TyKind::Function)
      return scalarReferenceTarget(t, init, at, temporary);
    std::string made = temporary(target);
    initializeFrom(made, target, init, at);
    return made;
  }
  std::vector<std::pair<std::string, uint32_t>> path;
  bool same = given && isClass(given) && canonicalClass(given, at) == canonicalClass(target, at);
  bool derived = given && isClass(given) && !same && basePath(given, target, path, at);
  if (named && same)
    return rvalue(init).text;
  if (named && derived)
    return convert(rvalue(init), target, at).text;
  if (!same && !derived && given && isClass(given))
  {
    // An operator that gives a reference binds what it refers to; one that gives a value, as a
    // converting constructor, makes the temporary below.
    const Ty* owner = nullptr;
    TemplateArgs opArgs;
    if (FunctionDecl* op = conversionOperator(given, target, at, &owner, false, &opArgs))
      if (const Ty* returned = conversionReturns(op, owner); returned && returned->isReference())
        return convert(rvalue(init), target, at).text;
  }
  const Ty* made = derived ? given : target;
  std::string object = temporary(made);
  initializeFrom(object, made, init, at);
  return derived ? convert(Value{object, made}, target, at).text : object;
}

auto codegen::Emitter::debugAggregateBindings(VariableDecl* v) -> void
{
  if (!sh.fullDebugInfo || v->structuredBindings.empty())
    return;
  auto it = locals.find(v);
  if (it == locals.end())
    return;
  const Local hidden = it->second;
  const Ty* object = types.unqualified(types.stripRef(hidden.ty));
  bool through = hidden.isRef || hidden.ty->isReference(); // (`auto& [a, b] = x`: the slot holds the object's address)
  // A variable `offset` bytes into the hidden object. Through a reference, at a nonzero offset, a
  // slot of its own holds the member's address (CodeView describes a dereference, not one then an
  // offset: LLVM drops such a variable).
  bool constObject = types.stripRef(hidden.ty)->isConst; // (a const object's members are const: `auto& [x, y] = constPoint`)
  auto declare = [&](const std::string& name, NameExpr* b, const Ty* t, uint64_t offset)
  {
    if (constObject && !t->isConst)
      t = types.qualified(t, true, t->isVolatile);
    if (through && offset)
    {
      std::string object = newTemp(), member = newTemp(), slot = "%v" + std::to_string(temp++);
      allocas += "  " + slot + " = alloca ptr\n";
      fn += "  " + object + " = load ptr, ptr " + hidden.addr + "\n";
      fn += "  " + member + " = getelementptr inbounds i8, ptr " + object + ", i64 " + std::to_string(offset) + "\n";
      fn += "  store ptr " + member + ", ptr " + slot + "\n";
      debugVariable(name, b, slot, t, 0, false, false, true);
      return;
    }
    debugVariable(name, b, hidden.addr, t, 0, false, false, through);
    if (!debugVariables.empty() && debugVariables.back().name == name)
      debugVariables.back().offset = offset;
  };
  for (size_t i = 0; i < v->structuredBindings.size(); ++i)
  {
    auto* b = static_cast<NameExpr*>(v->structuredBindings[i]);
    Decl* own = b->resolved;
    if ((own && own->kind == NodeKind::VariableDecl && static_cast<VariableDecl*>(own)->tupleBinding) || b->name.empty())
      continue; // (a tuple's binding is a local of its own: declareTupleBindings)
    std::string name = sym(b->name.last().symbol);
    if (object->kind == TyKind::Array)
    {
      declare(name, b, object->inner, uint64_t(i) * sizeOf(object->inner, v));
      continue;
    }
    if (!isClass(object))
      return;
    // (the class with the members, as bindingAddress takes it: the object's, or its one base with members)
    const Ty* owner = object;
    uint64_t base = 0;
    for (int depth = 0; depth < 16 && structFor(owner, v).fieldDecls.empty(); ++depth)
    {
      const Ty* withMembers = nullptr;
      for (auto& [baseDef, baseIndex] : std::vector<std::pair<Decl*, uint32_t>>(structFor(owner, v).bases))
        if (const Ty* bt = structFor(owner, v).fieldTypes[baseIndex]; !structFor(bt, v).fieldDecls.empty() || !structFor(bt, v).bases.empty())
          withMembers = bt;
      if (!withMembers)
        break;
      std::optional<uint64_t> at = baseOffsetIn(owner, withMembers, v);
      if (!at)
        return;
      base += *at;
      owner = withMembers;
    }
    Decl* member = bindingMember(owner, i, v);
    if (!member || bitfieldIn(structFor(owner, v), member))
      continue;
    const Ty* mt = fieldTypeIn(owner, member, v);
    std::optional<uint64_t> offset = memberOffset(owner, member, v);
    if (!mt || mt->isReference() || !offset)
      continue;
    declare(name, b, mt, base + *offset);
  }
}

auto codegen::Emitter::declareTupleBindings(VariableDecl* v) -> void
{
  debugAggregateBindings(v);
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
    Expr* init = referenceInitializer(v, v->init);
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
    if (Expr* e = temporaryOfSubobject(init))
      temporaryBase = types.unqualified(e->ty);
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
    const Ty* initClass = init->ty ? types.unqualified(types.stripRef(init->ty)) : nullptr;
    if (isClass(types.stripRef(t)) || (isLambda(types.stripRef(t)) && isClass(initClass))) // (a class derived from a closure type binds its base subobject)
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
        if (!converts && isClass(target) && findConstructor(target, {init->ty}, v))
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
  // A constexpr local of a scalar type is its constant, the value a constant evaluation of its
  // initializer gives (`constexpr int c = which();` with `if consteval` in which: its compile-time
  // branch), never a call made at run time, which took the other branch (until 2026-11-06).
  std::optional<std::string> constant;
  if ((v->flags & DF_Constexpr) && v->init && !isClass(t) && t->kind != TyKind::Array && !t->isReference())
    constant = staticConstant(v, t);
  if (constant)
    fn += "  store " + llType(t, v) + " " + *constant + ", ptr " + slot + "\n";
  else if (v->init && isClass(t))
    initializeExtending(slot, t, v, v->init, "", false);
  else if (v->init)
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

auto codegen::Emitter::debugLineAt(uint32_t token) -> void
{
  Node end{NodeKind::BlockStmt, token};
  debugLine(&end);
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

// Whether a statement declares a variable in its head (`for (int i = 0; ...)`, `if (int c = f())`):
// the variable is the statement's, in a lexical block of its own, or the `i` of two loops of a
// function would be two variables of one scope to a debugger, a watch of `i` naming either.
static bool declaresInHead(Stmt* s)
{
  auto declares = [](Node* n) { return n && n->kind == NodeKind::DeclStmt; };
  switch (s->kind)
  {
    case NodeKind::ForStmt: return declares(static_cast<ForStmt*>(s)->init);
    case NodeKind::RangeForStmt: return true;
    case NodeKind::IfStmt: return declares(static_cast<IfStmt*>(s)->init) || declares(static_cast<IfStmt*>(s)->cond);
    case NodeKind::WhileStmt: return declares(static_cast<WhileStmt*>(s)->cond);
    case NodeKind::SwitchStmt: return declares(static_cast<SwitchStmt*>(s)->init) || declares(static_cast<SwitchStmt*>(s)->cond);
    default: return false;
  }
}

auto codegen::Emitter::endIteration(bool declares, size_t outside, const std::string& condLabel, const std::string& falseLabel, const std::string& endLabel) -> void
{
  if (falseLabel != endLabel)
  {
    if (!blockTerminated)
      leaveScopes(outside);
    emitBranch(condLabel);
    startBlock(falseLabel);
    leaveScopes(outside);
    emitBranch(endLabel);
    popScope(); // (its objects are destroyed on both ways out)
    return;
  }
  if (declares)
    popScope();
  emitBranch(condLabel);
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
  struct DepthGuard { ~DepthGuard() { --d; } int& d; } dg{recursionDepth};
  if (blockTerminated)
    startBlock(newLabel("dead"));
  struct DebugBlockGuard { ~DebugBlockGuard() { block = outer; } uint32_t& block; uint32_t outer; } debugBlockGuard{debugBlock, debugBlock};
  if (declaresInHead(s))
    openDebugBlock(s);
  debugLine(s);
  switch (s->kind)
  {
    case NodeKind::BlockStmt:
    {
      auto* block = static_cast<BlockStmt*>(s);
      uint32_t outer = openDebugBlock(s);
      pushScope();
      scopeEnds.back() = block->endToken;
      for (Stmt* inner : block->stmts)
        statement(inner);
      // A function's end has its closing brace's line whatever there is to destroy, for a breakpoint
      // there; what comes after the body (a destructor's members and bases, the return) is at it too.
      if (s == bodyNow && block->endToken && !blockTerminated)
        debugLineAt(block->endToken);
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
        if (jumpBypassesInitialization(scopeIds, scopeInits, place->second.ids, place->second.inits))
          fail(s, "the goto to " + sym(symbol) + " enters the scope of a variable past its initialization, which C++ makes ill-formed ([stmt.dcl] 3)");
        size_t common = 0;
        while (common < place->second.ids.size() && common < scopeIds.size() && place->second.ids[common] == scopeIds[common])
          ++common;
        leaveScopes(common, common > 0 && common - 1 < place->second.counts.size() ? place->second.counts[common - 1] : size_t(-1));
        fn += "  br label %" + labelBlocks[symbol] + "\n";
      }
      else
      {
        PendingGoto g{{}, scopeIds, scopeInits, s};
        leaveForward(g.stops);
        pendingGotos[symbol].push_back(std::move(g));
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
      // The forward gotos to it: each one left its scopes at the goto, down to every depth in turn
      // (leaveForward); the stop at the depth it shares with the label goes on to the label.
      if (auto pending = pendingGotos.find(symbol); pending != pendingGotos.end())
      {
        for (const PendingGoto& g : pending->second)
          if (jumpBypassesInitialization(g.ids, g.inits, scopeIds, scopeInits))
            fail(g.at, "the goto to " + sym(symbol) + " skips the initialization of a variable in scope at the label, which C++ makes ill-formed ([stmt.dcl] 3)");
        for (const PendingGoto& g : pending->second)
        {
          size_t common = 0;
          while (common < g.ids.size() && common < scopeIds.size() && g.ids[common] == scopeIds[common])
            ++common;
          for (const GotoStop& stop : g.stops)
          {
            startBlock(stop.stop);
            emitBranch(stop.low <= common && common <= stop.high ? labelBlocks[symbol] : stop.next);
          }
        }
        pendingGotos.erase(pending);
      }
      startBlock(labelBlocks[symbol]);
      LabelPlace place;
      place.ids = scopeIds;
      place.inits = scopeInits;
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
        if (d->flags & DF_Extern)
          continue; // `extern int x;` in a block: the namespace's x, its uses resolved to it
        if (d->kind == NodeKind::VariableDecl)
        {
          // A variable whose initialization a jump into its scope would skip (jumpBypassesInitialization):
          // an automatic one with an initializer, a reference, bindings, or of a class (or an array of
          // one) whose default construction does something; not one that is default-initialized
          // vacuously (`int k;`).
          if (auto* v = static_cast<VariableDecl*>(d); !(v->flags & (DF_Static | DF_ThreadLocal | DF_Extern)) && !scopeInits.empty())
          {
            const Ty* vt = declType(v);
            if (v->init || !v->structuredBindings.empty() || (vt && (vt->isReference() || needsConstruction(types.unqualified(vt), v))))
              ++scopeInits.back();
          }
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
      // The result, once built, is destroyed when a destructor the return runs throws, the last
      // built first ([except.ctor] 2, CWG 2176; g++'s order; clang's MSVC target leaks it, as kpp did
      // until 2026-11-07): listed with the statement's temporaries, which every cleanup pad destroys
      // first, while they and the scopes are destroyed. A named return joins them as its scope is
      // left (destroyOnExit).
      bool resultListed = false;
      if (returnsInMemory(currentReturn) && keptOnReturn.empty() && needsDestruction(currentReturn, s))
      {
        Object result{sretAddr, currentReturn};
        result.unwindOnly = true;
        statementTemps.insert(statementTemps.begin(), result);
        resultListed = true;
      }
      destroyTemps();
      if (resultListed)
      {
        Object result{sretAddr, currentReturn};
        result.unwindOnly = true;
        statementTemps.push_back(result);
      }
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
      statementTemps.clear();
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
      checkedCall = checkedCallOf(a);
      checkedArguments.clear();
      Value cond = condition(a->cond, s);
      checkedCall = nullptr;
      showCheckedArguments(cond.text, s); // (while the statement's temporaries live)
      destroyTemps();
      std::string okL = newLabel("assert.ok"), failL = newLabel("assert.fail");
      fn += "  br i1 " + cond.text + ", label %" + okL + ", label %" + failL + "\n";
      startBlock(failL);
      auto [line, column] = lineColumnOf(a->token);
      if (a->testKind)
      {
        // check, require and fail in a test (design.md 8.6, 8.7): the test runner records the
        // failure; a require's or a fail's call throws to end the test (compiler/src/tests.cpp).
        emitCall("", "void @kpp_check_failed(ptr " + cStringConstant(programPathOf(currentFile)) + ", i32 " + std::to_string(line) + ", i32 " +
                         std::to_string(column) + ", ptr " + cStringConstant(sourceText(a->startToken, a->endToken)) + ", i32 " +
                         std::to_string(a->testKind) + ")", a->testKind >= 2);
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
      // `while (T x = next())`: x lives for one iteration, the last one's ending with a false
      // condition ([stmt.while] 2).
      size_t outside = scopes.size();
      bool declares = w->cond->kind == NodeKind::DeclStmt;
      if (declares)
        pushScope();
      Value cond = condition(w->cond, s);
      destroyTemps();
      std::string fL = declares && !scopes.back().empty() ? newLabel("while.false") : eL;
      fn += "  br i1 " + cond.text + ", label %" + bL + ", label %" + fL + "\n";
      startBlock(bL);
      loops.push_back({eL, cL, outside});
      subStatement(w->body);
      loops.pop_back();
      endIteration(declares, outside, cL, fL, eL);
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
      // `for (; T x = next(); step)`: x lives for one iteration, the step included ([stmt.for] 1:
      // the step ends the body of the while it stands for), so continue stays in its scope.
      size_t outside = scopes.size();
      bool declares = f->cond && f->cond->kind == NodeKind::DeclStmt;
      if (declares)
        pushScope();
      std::string fL = eL;
      if (f->cond)
      {
        debugLine(f->cond);
        Value cond = condition(f->cond, s);
        destroyTemps();
        if (declares && !scopes.back().empty())
          fL = newLabel("for.false");
        fn += "  br i1 " + cond.text + ", label %" + bL + ", label %" + fL + "\n";
      }
      else
        fn += "  br label %" + bL + "\n";
      startBlock(bL);
      loops.push_back({eL, sL, outside, scopes.size()});
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
      endIteration(declares, outside, cL, fL, eL);
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
          leaveScopes(loops[i].continueDepth != size_t(-1) ? loops[i].continueDepth : loops[i].scopeDepth);
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
    if (!v->name.empty())
      debugVariable(sym(v->name.last().symbol), v, slot, vt); // the loop variable, each iteration's
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
    if (!v->name.empty())
      debugVariable(sym(v->name.last().symbol), v, slot, vt); // the loop variable, each iteration's
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
