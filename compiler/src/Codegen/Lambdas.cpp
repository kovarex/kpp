// struct Emitter's members: lambdas and their closures; their declarations in Lambdas.hpp, part of
// the struct's body in Codegen/Emitter.hpp. A translation unit of its own since 2026-11-05
// (codegen.cpp split, stage 2), moved without a change.
#include "Emitter.hpp"

auto codegen::Emitter::localNamed(Decl* d, uint32_t symbol) -> bool
{
  if (d && d->kind == NodeKind::VariableDecl && !static_cast<VariableDecl*>(d)->name.empty())
    return static_cast<VariableDecl*>(d)->name.last().symbol == symbol;
  return d && d->kind == NodeKind::ParamDecl && static_cast<ParamDecl*>(d)->name == symbol;
}

auto codegen::Emitter::functionLocal(const Decl* d) -> bool
{
  return d && (d->kind == NodeKind::ParamDecl ||
               (d->kind == NodeKind::VariableDecl && !(d->flags & DF_Static) && d->parent && d->parent->kind == NodeKind::FunctionDecl));
}

auto codegen::Emitter::captureNames(Node* n, std::unordered_set<Decl*>& inner, std::vector<Decl*>& used, bool& usesThis, std::unordered_map<Decl*, const Ty*>* useTypes, int depth, bool keptBranches) -> void
{
  if (!n || depth > 96)
    return;
  auto visit = [&](Node* c) { captureNames(c, inner, used, usesThis, useTypes, depth + 1, keptBranches); };
  auto visitExprs = [&](Span<Expr*> es) { for (Expr* e : es) visit(e); };
  switch (n->kind)
  {
    case NodeKind::NameExpr:
    {
      Decl* d = static_cast<NameExpr*>(n)->resolved;
      if (!d)
        break;
      if (functionLocal(d))
      {
        if (!inner.count(d) && std::find(used.begin(), used.end(), d) == used.end())
        {
          used.push_back(d);
          if (useTypes)
            (*useTypes)[d] = static_cast<NameExpr*>(n)->ty;
        }
      }
      else if ((d->kind == NodeKind::VariableDecl || d->kind == NodeKind::FunctionDecl) && global.owningClass(d) && !(d->flags & DF_Static))
        usesThis = true; // a member named without `this->`
      break;
    }
    case NodeKind::VariableDecl: // a condition that declares (`if (auto* p = f())`)
      inner.insert(static_cast<Decl*>(n));
      visit(static_cast<VariableDecl*>(n)->init);
      break;
    case NodeKind::DeclStmt:
      for (Decl* d : static_cast<DeclStmt*>(n)->decls)
        if (d->kind == NodeKind::VariableDecl && !(keptBranches && d->whenKnown == 0)) // (a `when` its instance decided false names nothing)
        {
          inner.insert(d);
          visit(static_cast<VariableDecl*>(d)->init);
        }
      break;
    case NodeKind::ThisExpr: usesThis = true; break;
    case NodeKind::CallExpr: visit(static_cast<CallExpr*>(n)->callee); visitExprs(static_cast<CallExpr*>(n)->args); break;
    case NodeKind::IndexExpr: visit(static_cast<IndexExpr*>(n)->base); visitExprs(static_cast<IndexExpr*>(n)->args); break;
    case NodeKind::MemberExpr: visit(static_cast<MemberExpr*>(n)->base); visit(static_cast<MemberExpr*>(n)->dynamicMember); break;
    case NodeKind::UnaryExpr: visit(static_cast<UnaryExpr*>(n)->operand); break;
    case NodeKind::BinaryExpr: visit(static_cast<BinaryExpr*>(n)->lhs); visit(static_cast<BinaryExpr*>(n)->rhs); break;
    case NodeKind::ConditionalExpr:
      visit(static_cast<ConditionalExpr*>(n)->cond);
      visit(static_cast<ConditionalExpr*>(n)->then);
      visit(static_cast<ConditionalExpr*>(n)->else_);
      break;
    case NodeKind::CastExpr: visit(static_cast<CastExpr*>(n)->operand); break;
    case NodeKind::NamedCastExpr: visit(static_cast<NamedCastExpr*>(n)->operand); break;
    case NodeKind::FunctionalCastExpr: visitExprs(static_cast<FunctionalCastExpr*>(n)->args); break;
    case NodeKind::SizeofExpr: visit(static_cast<SizeofExpr*>(n)->operand); break;
    case NodeKind::NewExpr: visitExprs(static_cast<NewExpr*>(n)->placement); visitExprs(static_cast<NewExpr*>(n)->args); break;
    case NodeKind::DeleteExpr: visit(static_cast<DeleteExpr*>(n)->operand); break;
    case NodeKind::BraceInitExpr: visitExprs(static_cast<BraceInitExpr*>(n)->args); break;
    case NodeKind::ThrowExpr: visit(static_cast<ThrowExpr*>(n)->operand); break;
    case NodeKind::ParenExpr: visit(static_cast<ParenExpr*>(n)->inner); break;
    case NodeKind::FoldExpr: visit(static_cast<FoldExpr*>(n)->lhs); visit(static_cast<FoldExpr*>(n)->rhs); break; // (`(show(ts), ...)`)
    case NodeKind::NameofExpr: visit(static_cast<NameofExpr*>(n)->operand); break;
    case NodeKind::NoexceptExpr: break; // (its operand is not evaluated)
    case NodeKind::LambdaExpr:
    {
      auto* l = static_cast<LambdaExpr*>(n);
      for (const LambdaCapture& c : l->captures)
      {
        visit(c.init);
        if (c.decl)
          inner.insert(c.decl);
      }
      for (Decl* p : l->params)
        inner.insert(p);
      visit(l->body);
      break;
    }
    case NodeKind::BlockStmt: for (Stmt* s : static_cast<BlockStmt*>(n)->stmts) visit(s); break;
    case NodeKind::ExprStmt: visit(static_cast<ExprStmt*>(n)->expr); break;
    case NodeKind::IfStmt:
    {
      auto* i = static_cast<IfStmt*>(n);
      visit(i->init);
      if (i->ifConsteval)
      {
        visit(i->ifConsteval == 1 ? i->else_ : i->then); // `if consteval`: the program's branch alone
        break;
      }
      if (keptBranches && i->isConstexpr && i->constexprKnown >= 0)
      {
        // An instance's `if constexpr`: only the branch it keeps, as C++ captures what the
        // instantiated body uses (FluidBox::findOtherUndergroundEnd<BlueprintSurface>'s lambda
        // names `surface` only in the branch it discards, where nothing types it).
        visit(i->constexprKnown ? i->then : i->else_);
        break;
      }
      visit(i->cond);
      visit(i->then);
      visit(i->else_);
      break;
    }
    case NodeKind::ForStmt:
      visit(static_cast<ForStmt*>(n)->init);
      visit(static_cast<ForStmt*>(n)->cond);
      visit(static_cast<ForStmt*>(n)->step);
      visit(static_cast<ForStmt*>(n)->body);
      break;
    case NodeKind::RangeForStmt:
      visit(static_cast<RangeForStmt*>(n)->range);
      if (Decl* v = static_cast<RangeForStmt*>(n)->var)
        inner.insert(v);
      visit(static_cast<RangeForStmt*>(n)->body);
      break;
    case NodeKind::ForConstexprStmt: visit(static_cast<ForConstexprStmt*>(n)->body); break;
    case NodeKind::WhileStmt: visit(static_cast<WhileStmt*>(n)->cond); visit(static_cast<WhileStmt*>(n)->body); break;
    case NodeKind::DoStmt: visit(static_cast<DoStmt*>(n)->body); visit(static_cast<DoStmt*>(n)->cond); break;
    case NodeKind::SwitchStmt:
      visit(static_cast<SwitchStmt*>(n)->init);
      visit(static_cast<SwitchStmt*>(n)->cond);
      visit(static_cast<SwitchStmt*>(n)->body);
      break;
    case NodeKind::CaseStmt: visit(static_cast<CaseStmt*>(n)->value); break;
    case NodeKind::ReturnStmt: visit(static_cast<ReturnStmt*>(n)->value); break;
    case NodeKind::AssertStmt: visit(static_cast<AssertStmt*>(n)->cond); break;
    case NodeKind::TryStmt:
    {
      auto* t = static_cast<TryStmt*>(n);
      visit(t->body);
      for (const TryStmt::Handler& h : t->handlers)
      {
        if (h.param)
          inner.insert(h.param);
        visit(h.body);
      }
      break;
    }
    default: break;
  }
}

auto codegen::Emitter::captureList(LambdaExpr* l, std::unordered_map<Decl*, const Ty*>* useTypes) -> std::vector<CaptureSlot>
{
  std::unordered_set<Decl*> inner;
  for (Decl* p : l->params)
    inner.insert(p);
  for (const LambdaCapture& cap : l->captures)
    if (cap.decl)
      inner.insert(cap.decl);
  std::vector<Decl*> used;
  bool usesThis = false;
  captureNames(l->body, inner, used, usesThis, useTypes, 0, l->templateParams.empty());
  std::vector<CaptureSlot> out;
  bool thisCaptured = false;
  for (const LambdaCapture& cap : l->captures)
  {
    if (cap.init)
    {
      // `[x = expr]`: a field of the closure, initialised from expr when the closure is made;
      // `[&x = expr]`: one pointing at what expr names.
      if (!cap.decl)
        unsupported(l, "an init-capture without its variable");
      out.push_back({cap.decl, cap.byRef, cap.init});
      continue;
    }
    if (cap.isThis)
    {
      thisCaptured = true;
      continue;
    }
    Decl* d = nullptr;
    for (Decl* u : used)
      if (localNamed(u, cap.symbol) || bindsName(u, cap.symbol))
        d = d ? d : u;
    if (!d)
      d = unusedCapture(l, cap.symbol);
    // `[mixingConnection]` of a structured binding: the object it names a part of, captured
    // the same way (the body reaches the part through it, as outside the lambda); once for
    // several of its bindings.
    if (std::none_of(out.begin(), out.end(), [&](const CaptureSlot& s) { return s.decl == d && !s.init; }))
      out.push_back({d, cap.byRef, nullptr});
  }
  if (l->defaultCapture != 0)
  {
    for (Decl* d : used)
      if (std::none_of(out.begin(), out.end(), [&](const CaptureSlot& s) { return s.decl == d; }))
        out.push_back({d, l->defaultCapture == 2, nullptr});
    thisCaptured = thisCaptured || usesThis;
  }
  if (thisCaptured)
    out.push_back({nullptr, true, nullptr});
  // A parameter pack of the function around: a slot per element, in order. Its elements are the
  // parameter's type under the instantiation's arguments, as the walk of the copy that declares the
  // closure had them (typeInInstanceOf: whichever function lays the closure out first, a caller
  // that holds a closure an instance returned too), or as the function's emission has them
  // (currentArgs, in its body or in a lambda's in it); the parameter's own type is the template's
  // (`Ts`), and the body's names of it are typed as the pack or as one element. (A generic lambda's
  // call operator that expands it is emitted from a private copy retyped per element: emitLambda.)
  std::vector<CaptureSlot> expanded;
  for (const CaptureSlot& s : out)
  {
    auto* pd = s.decl && !s.init && s.decl->kind == NodeKind::ParamDecl ? static_cast<ParamDecl*>(s.decl) : nullptr;
    if (!pd || !pd->isPack)
    {
      expanded.push_back(s);
      continue;
    }
    const Ty* pack = l->closure ? global.typeInInstanceOf(pd, l->closure) : nullptr;
    if (!pack || pack->kind != TyKind::Pack)
    {
      Decl* writer = l->closure ? l->closure->parent : nullptr;
      while (writer && writer->kind != NodeKind::FunctionDecl)
        writer = writer->parent;
      bool inWriter = writer && (writer == static_cast<const Decl*>(checkerChecked) || writer == currentFunction);
      pack = inWriter && currentArgs ? declType(pd) : nullptr;
    }
    if (!pack || pack->kind != TyKind::Pack)
      unsupported(l, "a capture of a parameter pack whose elements are not known");
    for (size_t k = 0; k < pack->args.size(); ++k)
      expanded.push_back({s.decl, s.byRef, nullptr, int(k), types.stripRef(pack->args[k])});
  }
  return expanded;
}

auto codegen::Emitter::genericLambdaRetypedPerElement(LambdaExpr* l) -> bool
{
  auto hasPack = [](Decl* d)
  {
    for (Decl* tp : d->templateParams)
      if (tp->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(tp)->isPack)
        return true;
    return false;
  };
  for (Decl* tp : l->templateParams)
    if (tp->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(tp)->isPack)
      return true;
  for (Decl* d = l->closure ? l->closure->parent : nullptr; d; d = d->parent)
    if (hasPack(d))
      return true;
  return containsForConstexpr(l->body);
}

auto codegen::Emitter::bindsName(Decl* d, uint32_t symbol) -> bool
{
  if (!d || d->kind != NodeKind::VariableDecl)
    return false;
  for (Node* b : static_cast<VariableDecl*>(d)->structuredBindings)
    if (b && b->kind == NodeKind::NameExpr && !static_cast<NameExpr*>(b)->name.empty() && static_cast<NameExpr*>(b)->name.last().symbol == symbol)
      return true;
  return false;
}

auto codegen::Emitter::unusedCapture(LambdaExpr* l, uint32_t symbol) -> Decl*
{
  Decl* f = l->closure ? l->closure->parent : nullptr;
  while (f && f->kind != NodeKind::FunctionDecl)
    f = f->parent;
  if (!f)
    unsupported(l, "a capture that is not a local variable (" + sym(symbol) + ")");
  auto* fn = static_cast<FunctionDecl*>(f);
  std::unordered_set<Decl*> declared(fn->params.begin(), fn->params.end());
  std::vector<Decl*> ignored;
  bool ignoredThis = false;
  captureNames(fn->body, declared, ignored, ignoredThis);
  Decl* only = nullptr;
  for (Decl* d : declared)
    if (localNamed(d, symbol) && d->token < l->token)
    {
      if (only)
        unsupported(l, "a capture of " + sym(symbol) + " that the lambda does not use, with several locals of that name in the function");
      only = d;
    }
  if (!only)
    unsupported(l, "a capture that is not a local variable (" + sym(symbol) + ")");
  return only;
}

auto codegen::Emitter::mentionsAuto(const Ty* t, int depth) -> bool
{
  if (!t || depth > 16)
    return false;
  if (t->kind == TyKind::Auto || mentionsAuto(t->inner, depth + 1))
    return true;
  for (const Ty* a : t->args)
    if (mentionsAuto(a, depth + 1))
      return true;
  return false;
}

auto codegen::Emitter::captureType(Decl* d, Expr* init, const std::unordered_map<Decl*, const Ty*>& useTypes, LambdaExpr* l) -> const Ty*
{
  auto exact = [&](const Ty* t) { return t && t->isKnown() && !types.isDependent(t) && !mentionsAuto(t); };
  if (const Ty* cached = d->cachedType.load(std::memory_order_acquire); exact(cached))
    return types.stripRef(cached); // as the instantiation's walk typed its own copy of the local
  const Ty* t = global.typeOfDecl(d);
  if (exact(t))
    return types.stripRef(t);
  if (init && init->ty && init->ty->isKnown() && !types.isDependent(init->ty))
    return types.decay(types.stripRef(init->ty)); // `[x = expr]`: as auto deduces it
  if (auto it = useTypes.find(d); it != useTypes.end() && it->second && it->second->isKnown() && !types.isDependent(it->second))
    return types.stripRef(it->second);
  unsupported(l, "a capture of " + (d->kind == NodeKind::VariableDecl ? sym(static_cast<VariableDecl*>(d)->name.last().symbol) : std::string("a parameter")) +
                     " whose type the code generator could not work out");
}

auto codegen::Emitter::closureStructName(const Ty* t) -> std::string
{
  t = types.unqualified(t); // `const auto f = [..]{..}`: the const variable holds the same closure
  if (auto mine = closureNames.find(t); mine != closureNames.end())
    return mine->second;
  auto lock = sharedLock();
  ClosureInfo& info = closures[t];
  if (info.structName.empty())
    info.structName = irTypeName("%closure.", t);
  closureNames.emplace(t, info.structName);
  return info.structName;
}

auto codegen::Emitter::closureFor(const Ty* t, Node* at, bool) -> ClosureInfo&
{
  t = types.unqualified(t);
  if (auto mine = closureCache.find(t); mine != closureCache.end())
    return *mine->second;
  if (ClosureInfo* known; sh.finalClosures.find(t, known))
    return *closureCache.emplace(t, known).first->second;
  auto lock = sharedLock(); // a laid-out closure does not change, so its reference may be read after
  auto it = closures.find(t);
  if (it != closures.end() && it->second.laidOut)
  {
    closureCache.emplace(t, &it->second);
    if (mayPublish())
      sh.finalClosures.publish(t, &it->second);
    return it->second;
  }
  if (it != closures.end() && !it->second.failure.empty())
    fail(at, "the closure of " + irString(t) + " could not be laid out (" + it->second.failure + ")");
  size_t errorsBefore = errors.size();
  try
  {
    return layOutClosure(t, at);
  }
  catch (Unsupported&)
  {
    // Every use, the first one like the others, reports the closure and why: the same lines
    // whichever function asks first.
    std::string why = lastFailure;
    closures[t].failure = why;
    ++sh.failuresRemembered;
    errors.resize(errorsBefore);
    fail(at, "the closure of " + irString(t) + " could not be laid out (" + why + ")");
  }
}

auto codegen::Emitter::layOutClosure(const Ty* t, Node* at) -> ClosureInfo&
{
  auto* c = static_cast<ClassDecl*>(t->decl);
  LambdaExpr* l = c->lambda; // the instantiation's own lambda: the same whoever lays it out
  if (!l)
    unsupported(at, "a lambda without its expression");
  // Laid out aside and published whole: a capture that fails (an unsupported one, a field whose
  // class cannot be laid out) leaves the closure unbuilt for every later use, not half built.
  ClosureInfo info;
  info.structName = closureStructName(t);
  std::unordered_map<Decl*, const Ty*> useTypes;
  for (const CaptureSlot& s : captureList(l, &useTypes))
  {
    info.captured.push_back(s.decl);
    info.byRef.push_back(s.byRef);
    info.inits.push_back(s.init);
    // `this`: a pointer field; the class it points to is the writer's (enclosingClass).
    // An element of a parameter pack: its own type (`Ts&... ts`: the object an element refers to).
    info.fieldTypes.push_back(s.element >= 0 ? s.elementType : s.decl ? captureType(s.decl, s.init, useTypes, l) : types.pointer(types.voidType()));
  }
  std::string line = info.structName + " = type { ";
  for (size_t i = 0; i < info.fieldTypes.size(); ++i)
    line += (i ? ", " : "") + (info.byRef[i] ? std::string("ptr") : llType(info.fieldTypes[i], at));
  line += info.fieldTypes.empty() ? "i8 }\n" : " }\n";
  const Ty* ft = t;
  std::string params;
  for (size_t i = 0; i < ft->args.size(); ++i)
    params += (i ? ", " : "") + irString(ft->args[i]);
  info.functionName = "@\"" + irString(t) + "::operator()(" + params + ")\"";
  info.laidOut = true;
  decls += line;
  ClosureInfo& published = closures[t];
  published = std::move(info);
  return published;
}

auto codegen::Emitter::layOutClosuresIn(Node* n) -> void
{
  if (!n)
    return;
  switch (n->kind)
  {
    case NodeKind::LambdaExpr:
    {
      auto* l = static_cast<LambdaExpr*>(n);
      for (const LambdaCapture& c : l->captures)
        layOutClosuresIn(c.init);
      if (isLambda(l->ty))
        closureFor(l->ty, l, true);
      return;
    }
    case NodeKind::CallExpr:
      layOutClosuresIn(static_cast<CallExpr*>(n)->callee);
      for (Expr* a : static_cast<CallExpr*>(n)->args)
        layOutClosuresIn(a);
      return;
    case NodeKind::MemberExpr: layOutClosuresIn(static_cast<MemberExpr*>(n)->base); return;
    case NodeKind::IndexExpr:
      layOutClosuresIn(static_cast<IndexExpr*>(n)->base);
      for (Expr* a : static_cast<IndexExpr*>(n)->args)
        layOutClosuresIn(a);
      return;
    case NodeKind::UnaryExpr: layOutClosuresIn(static_cast<UnaryExpr*>(n)->operand); return;
    case NodeKind::BinaryExpr:
      layOutClosuresIn(static_cast<BinaryExpr*>(n)->lhs);
      layOutClosuresIn(static_cast<BinaryExpr*>(n)->rhs);
      return;
    case NodeKind::ConditionalExpr:
      layOutClosuresIn(static_cast<ConditionalExpr*>(n)->cond);
      layOutClosuresIn(static_cast<ConditionalExpr*>(n)->then);
      layOutClosuresIn(static_cast<ConditionalExpr*>(n)->else_);
      return;
    case NodeKind::CastExpr: layOutClosuresIn(static_cast<CastExpr*>(n)->operand); return;
    case NodeKind::NamedCastExpr: layOutClosuresIn(static_cast<NamedCastExpr*>(n)->operand); return;
    case NodeKind::FunctionalCastExpr:
      for (Expr* a : static_cast<FunctionalCastExpr*>(n)->args)
        layOutClosuresIn(a);
      return;
    case NodeKind::NewExpr:
      for (Expr* a : static_cast<NewExpr*>(n)->placement)
        layOutClosuresIn(a);
      for (Expr* a : static_cast<NewExpr*>(n)->args)
        layOutClosuresIn(a);
      return;
    case NodeKind::BraceInitExpr:
      for (Expr* a : static_cast<BraceInitExpr*>(n)->args)
        layOutClosuresIn(a);
      return;
    case NodeKind::ParenExpr: layOutClosuresIn(static_cast<ParenExpr*>(n)->inner); return;
    case NodeKind::ThrowExpr: layOutClosuresIn(static_cast<ThrowExpr*>(n)->operand); return;
    default: return;
  }
}

auto codegen::Emitter::closureValueFields(const Ty* t, Node* at) -> std::vector<std::pair<uint32_t, const Ty*>>
{
  ClosureInfo& info = closureFor(t, at);
  std::vector<std::pair<uint32_t, const Ty*>> fields;
  for (size_t i = 0; i < info.captured.size(); ++i)
    if (info.captured[i] && !info.byRef[i])
      fields.push_back({uint32_t(i), info.fieldTypes[i]});
  return fields;
}

auto codegen::Emitter::copyClosure(const std::string& dst, const std::string& src, const Ty* t, Node* at, bool move) -> void
{
  ClosureInfo& info = closureFor(t, at);
  for (size_t i = 0; i < info.captured.size(); ++i)
  {
    std::string to = gep(info.structName, dst, uint32_t(i)), from = gep(info.structName, src, uint32_t(i));
    const Ty* ft = info.fieldTypes[i];
    bool byValue = info.captured[i] && !info.byRef[i];
    if (byValue && isClass(ft))
    {
      if (move)
        moveConstruct(to, from, ft, at);
      else
        copyConstruct(to, from, ft, at);
      continue;
    }
    if (byValue && isLambda(ft))
    {
      copyClosure(to, from, ft, at, move);
      continue;
    }
    if (byValue && ft->kind == TyKind::Array)
    {
      transferArray(to, from, ft, at, move, false);
      continue;
    }
    if (byValue)
    {
      copyBytes(to, from, ft, at); // a scalar
      continue;
    }
    std::string v = newTemp(); // a pointer: by reference, `this`
    fn += "  " + v + " = load ptr, ptr " + from + "\n";
    fn += "  store ptr " + v + ", ptr " + to + "\n";
  }
}

auto codegen::Emitter::initializeClosure(const std::string& addr, const Ty* t, Expr* init, Node* at) -> void
{
  Expr* inner = init;
  while (inner && inner->kind == NodeKind::ParenExpr)
    inner = static_cast<ParenExpr*>(inner)->inner;
  if (inner && inner->kind == NodeKind::LambdaExpr && types.unqualified(inner->ty) == types.unqualified(t))
  {
    makeClosure(static_cast<LambdaExpr*>(inner), addr);
    return;
  }
  Value v = rvalue(init);
  if (!isLambda(v.ty))
    unsupported(at, "initialising a lambda variable from something else");
  if (v.isTemporary && relocatesBitwise(t, at))
  {
    // Its bytes are the closure now: the statement no longer destroys the temporary.
    for (size_t i = statementTemps.size(); i-- > 0;)
      if (statementTemps[i].addr == v.text)
        statementTemps.erase(statementTemps.begin() + i);
    copyBytes(addr, v.text, t, at);
    return;
  }
  copyClosure(addr, v.text, t, at, v.isTemporary || movable(init));
}

auto codegen::Emitter::makeClosure(LambdaExpr* l, const std::string& into) -> Value
{
  const Ty* t = l->ty;
  if (!isLambda(t))
    unsupported(l, "a lambda whose type is unknown");
  ClosureInfo info = closureFor(t, l, true);
  {
    // The function the lambda is written in: its class is what `this` in the body points to,
    // its name the requester the operator's diagnostics give.
    auto lock = sharedLock();
    ClosureWriter& writer = sh.closureWriters[types.unqualified(t)];
    if (writer.writtenIn.empty())
    {
      writer.writtenIn = currentFunctionName;
      writer.writtenInContext = currentContext;
      writer.enclosingClass = currentClassTy;
    }
  }
  // This lambda's own captures, field by field (in a private copy of an instantiation they name
  // the copy's locals; the layout was read from the instantiation's).
  std::vector<CaptureSlot> slots = captureList(l);
  if (slots.size() != info.captured.size())
    unsupported(l, "a lambda whose captures differ from its closure's");
  std::string slot = into.empty() ? newObjectSlot(t, l) : into;
  for (size_t i = 0; i < info.captured.size(); ++i)
  {
    std::string field = gep(info.structName, slot, uint32_t(i));
    Decl* d = slots[i].decl;
    if (!d)
    {
      fn += "  store ptr " + thisPointer(l) + ", ptr " + field + "\n";
      continue;
    }
    if (slots[i].init && slots[i].byRef)
    {
      // `[&entities = this->entities]`: the field points at what the initializer names.
      Expr* init = slots[i].init;
      std::string target = isClass(types.stripRef(init->ty)) ? rvalue(init).text : lvalue(init);
      fn += "  store ptr " + target + ", ptr " + field + "\n";
      continue;
    }
    if (slots[i].init)
    {
      initializeFrom(field, info.fieldTypes[i], slots[i].init, l); // `[x = expr]`
      continue;
    }
    Local* found = nullptr;
    if (slots[i].element >= 0)
    {
      // An element of a parameter pack: the function's own (or, in a lambda, its closure's field).
      auto pack = packLocals.find(d);
      if (pack == packLocals.end() || size_t(slots[i].element) >= pack->second.size())
        unsupported(l, "a capture of a parameter pack the function being emitted does not have");
      found = &pack->second[slots[i].element];
    }
    else
    {
      if (!locals.count(d))
        unsupported(l, "a capture of a variable the function being emitted does not have");
      found = &locals[d];
    }
    Local& local = *found;
    std::string addr = local.addr;
    if (local.isRef)
    {
      addr = newTemp();
      fn += "  " + addr + " = load ptr, ptr " + local.addr + "\n";
    }
    if (info.byRef[i])
      fn += "  store ptr " + addr + ", ptr " + field + "\n";
    else if (isAggregate(info.fieldTypes[i]))
    {
      if (isClass(info.fieldTypes[i]))
        copyConstruct(field, addr, info.fieldTypes[i], l);
      else
        copyClosure(field, addr, info.fieldTypes[i], l, false); // `[f]` of a closure
    }
    else
      store(load(addr, info.fieldTypes[i], l), field, info.fieldTypes[i], l);
  }
  bool first = false; // this emitter emits the operator (a generic lambda's is emitted per call, once its types are known)
  {
    auto lock = sharedLock();
    if (!isGenericLambda(t))
      first = sh.closuresEmitted.insert(t).second;
  }
  if (first)
  {
    // (Several functions write one closure: a default member initializer's lambda each constructor,
    // a default argument's each call site: lambdaHome is the lambda's, whoever writes it.)
    NameHome home(*this, lambdaHome(l, info.functionName));
    emitLambda(l, t, types.function(t->inner ? t->inner : types.voidType(), t->args, false), info.functionName, nullptr);
  }
  if (!into.empty())
    return {slot, t, false};
  if (needsDestruction(t, l))
    statementTemps.push_back({slot, t});
  return {slot, t, true};
}

auto codegen::Emitter::resetFunctionState() -> void
{
  static_cast<FunctionBody&>(*this) = FunctionBody{};
  recursionDepth = 0;
}

auto codegen::Emitter::functionText(std::string header) -> std::string
{
  if (usesEH)
    header.insert(header.rfind("{\nentry:"), "personality ptr " + personality() + " ");
  if (size_t line = debugFile ? lineOf(debugAt, debugFile) : 0)
    return debugLines(header, line);
  return header + allocas + fn + ehBlocks + "}\n\n";
}

auto codegen::Emitter::closureWriterOf(const Ty* t) -> ClosureWriter
{
  auto lock = sharedLock();
  auto it = sh.closureWriters.find(types.unqualified(t));
  return it == sh.closureWriters.end() ? ClosureWriter{} : it->second;
}

auto codegen::Emitter::emitLambda(LambdaExpr* l, const Ty* t, const Ty* callTy, const std::string& name, const TemplateArgs* args) -> void
{
  ClosureInfo info = closureFor(t, l);
  ClosureWriter writer = closureWriterOf(t);
  FunctionState saved = saveState();
  const TemplateArgs* savedArgs = currentArgs;
  ParsedFile* savedFile = currentFile;
  std::string savedName = currentFunctionName;
  struct NameRestore { std::string& name; std::string saved; ~NameRestore() { name = saved; } } nameRestore{currentFunctionName, savedName};
  currentFunctionName = name; // what the operator's own statics and filters are named after
  // Its diagnostics name the function it is written in as the requester, not the function that
  // happened to emit it first.
  NameRestore requesterRestore{currentRequestedBy, currentRequestedBy};
  currentRequestedBy = writer.writtenIn;
  // As an error says it: the function it is written in and its line, which a lambda's closure
  // tag would not tell a reader (A, 2026-11-04).
  NameRestore contextRestore1{currentContext, currentContext};
  NameRestore contextRestore2{currentRequestedByContext, currentRequestedByContext};
  {
    size_t line = lineOf(l, l->closure && l->closure->file ? l->closure->file : currentFile);
    currentContext = "a lambda" + (writer.writtenInContext.empty() ? std::string() : " in " + writer.writtenInContext) +
                     (line ? ", line " + std::to_string(line) : std::string());
    currentRequestedByContext.clear();
  }
  struct ContextRestore
  {
    Decl*& lambda;
    Decl*& initializer;
    Decl* savedLambda;
    Decl* savedInitializer;
    ~ContextRestore()
    {
      lambda = savedLambda;
      initializer = savedInitializer;
    }
  } contextRestore{lambdaContext, initializerOf, lambdaContext, initializerOf};
  // One written in a function body (whose declarations the closure's context reaches); one in
  // a constructor's initializer list or a member's initializer is typed as before, from the
  // function or the initializer being emitted.
  if (l->closure && l->closure->parent && l->closure->parent->kind == NodeKind::FunctionDecl)
  {
    lambdaContext = l->closure;
    initializerOf = nullptr;
  }
  resetFunctionState();
  if (args)
  {
    // A generic lambda's call operator that expands a pack, or has a `for constexpr`, is emitted
    // from a private copy of its instance's body, retyped per element (retypeForElement) where no
    // other thread reads it (a caller reads the instance's own copy: its default arguments). One
    // emitted on its own (a generic lambda's, one on demand) retypes nothing of its caller's.
    retypedLambda = nullptr;
    retypedLambdaTy = nullptr;
    if (!l->templateParams.empty() && genericLambdaRetypedPerElement(l))
    {
      LambdaExpr* own = global.privateLambdaInstance(t, l, *args);
      if (!own)
        unsupported(l, "a generic lambda whose environment is not known");
      l = own;
      retypedLambda = own;
      retypedLambdaTy = t;
    }
  }
  bodyNow = l->body;
  // The function it is written in is the current function while its body is emitted (not the
  // one that happened to emit it first, a generic lambda's first caller): its operators and free
  // functions are looked up from there, `this` is that function's, and its nodes are that
  // function's to retype (a pack expansion, a for constexpr). Restored with the saved state.
  FunctionDecl* emitting = currentFunction;
  if (l->closure && l->closure->parent && l->closure->parent->kind == NodeKind::FunctionDecl)
  {
    currentFunction = static_cast<FunctionDecl*>(l->closure->parent);
    // Written in an instantiation emitted from a private copy (emitFunction: one retyped per
    // element), the lambda is the copy's and its closure the instantiation's, whose own copy other
    // threads read: what a pack expansion or a for constexpr in the body retypes is the copy.
    if (currentFunction == checkerChecked && emitting && emitting != currentFunction)
      currentFunction = emitting;
  }
  if (args)
  {
    currentArgs = args;
    // A generic lambda's call operator from its instance's own copy of the body (lambdaInstance's,
    // typed for these arguments by a walk of its own, which recorded its choices: design.md 24), or
    // from a private copy (typed by privateLambdaInstance's walk, then per element by
    // retypeLambdaFor's). For a lambda emitted on demand codegen chooses.
    choicesRecorded = !l->templateParams.empty();
  }
  if (l->closure && l->closure->file)
    currentFile = l->closure->file;
  if (sh.debugInfo)
  {
    debugAt = l;
    debugFile = currentFile;
  }
  currentReturn = callTy->inner ? callTy->inner : types.voidType();
  if (!currentReturn->isKnown())
    currentReturn = types.pointer(types.voidType());
  sretAddr.clear();
  currentClassTy = writer.enclosingClass;
  try
  {
    Signature s = lambdaSignatureOf(callTy, l);
    std::string params = "ptr %closure";
    bool hasSret = s.hasSret;
    if (hasSret)
    {
      params += ", ptr %sret";
      sretAddr = "%sret";
    }
    std::vector<std::string> paramNames;
    for (size_t i = 0; i < callTy->args.size(); ++i)
    {
      std::string pn = "%p" + std::to_string(i);
      paramNames.push_back(pn);
      params += ", " + s.paramTypes[i + (hasSret ? 2 : 1)] + " " + pn;
    }
    std::string retType = s.returnType;
    std::string header = "define " + retType + " " + name + "(" + params + ") {\nentry:\n";
    pushScope();
    // Captures become locals: by-value captures live in the closure, references point out.
    // The body's own names for them, field by field (captureList of the lambda emitted).
    std::vector<CaptureSlot> slots = captureList(l);
    if (slots.size() != info.captured.size())
      unsupported(l, "a lambda whose captures differ from its closure's");
    for (size_t i = 0; i < info.captured.size(); ++i)
    {
      std::string field = gep(info.structName, "%closure", uint32_t(i));
      Decl* d = slots[i].decl;
      if (!d)
      {
        std::string thisSlot = "%v" + std::to_string(temp++);
        allocas += "  " + thisSlot + " = alloca ptr\n";
        std::string loaded = newTemp();
        fn += "  " + loaded + " = load ptr, ptr " + field + "\n";
        fn += "  store ptr " + loaded + ", ptr " + thisSlot + "\n";
        locals[nullptr] = {thisSlot, nullptr, false};
        if (currentClassTy)
          debugVariable("this", l, thisSlot, types.pointer(currentClassTy));
        continue;
      }
      // (by value the capture is its field; by reference the field holds its address: a reference)
      const Ty* capturedTy = info.byRef[i] ? types.lref(info.fieldTypes[i]) : info.fieldTypes[i];
      if (slots[i].element >= 0)
      {
        // An element of a parameter pack of the function around: what the body's expansions of
        // the pack read (the function's own elements are other IR names).
        std::vector<Local>& elements = packLocals[d];
        if (elements.size() <= size_t(slots[i].element))
          elements.resize(size_t(slots[i].element) + 1);
        elements[slots[i].element] = {field, capturedTy, info.byRef[i]};
        if (std::string name = debugName(d); !name.empty())
          debugVariable(name + "#" + std::to_string(slots[i].element), d, field, capturedTy, 0, false, true);
        continue;
      }
      locals[d] = {field, capturedTy, info.byRef[i]};
      if (std::string name = debugName(d); !name.empty())
        debugVariable(name, d, field, capturedTy, 0, false, true);
    }
    for (size_t i = 0; i < l->params.size() && i < callTy->args.size(); ++i)
    {
      const Ty* pt = callTy->args[i];
      auto* pd = l->params[i]->kind == NodeKind::ParamDecl ? static_cast<ParamDecl*>(l->params[i]) : nullptr;
      if (isAggregate(pt))
      {
        locals[l->params[i]] = {paramNames[i], pt, false};
        if (needsDestruction(pt, l))
          scopes.back().push_back({paramNames[i], pt});
        if (pd && pd->name != UINT32_MAX)
          debugVariable(sym(pd->name), pd, paramNames[i], pt, uint32_t(i + 1));
        continue;
      }
      bool isRef = pt->isReference();
      std::string ps = "%v" + std::to_string(temp++);
      allocas += "  " + ps + " = alloca " + (isRef ? std::string("ptr") : llType(pt, l)) + "\n";
      fn += "  store " + (isRef ? std::string("ptr") : llType(pt, l)) + " " + paramNames[i] + ", ptr " + ps + "\n";
      locals[l->params[i]] = {ps, pt, isRef};
      if (pd && pd->name != UINT32_MAX)
        debugVariable(sym(pd->name), pd, ps, pt, uint32_t(i + 1));
    }
    namedReturn = builtInResult(global.namedReturnVariable(l->body, currentReturn, nullptr, currentArgs), l);
    statement(l->body);
    if (!blockTerminated)
    {
      popScope();
      if (currentReturn->kind == TyKind::Void || hasSret)
        fn += "  ret void\n";
      else
        fn += "  ret " + (currentReturn->isReference() ? std::string("ptr null") : llType(currentReturn, l) + " zeroinitializer") + "\n";
    }
    addFunction(name, functionText(header));
  }
  catch (Unsupported&)
  {
    restoreState(saved);
    currentArgs = savedArgs;
    currentFile = savedFile;
    throw;
  }
  restoreState(saved);
  currentArgs = savedArgs;
  currentFile = savedFile;
}

auto codegen::Emitter::lambdaWriter(LambdaExpr* l) -> FunctionDecl*
{
  if (!l || !l->closure)
    return nullptr;
  Decl* writer = l->closure->parent;
  while (writer && writer->kind != NodeKind::FunctionDecl)
  {
    if (!writer->templateParams.empty())
      return nullptr;
    writer = writer->parent;
  }
  if (!writer || !writer->templateParams.empty())
    return nullptr;
  for (Decl* o = global.owningClass(writer); o; o = o->parent)
    if (!o->templateParams.empty() || (o->kind == NodeKind::FunctionDecl && o != writer))
      return nullptr;
  return static_cast<FunctionDecl*>(writer);
}

auto codegen::Emitter::lambdaHome(LambdaExpr* l, const std::string& name) -> unsigned
{
  if (FunctionDecl* writer = lambdaWriter(l))
    if (auto it = sh.fileModule.find(writer->file); it != sh.fileModule.end())
      return it->second;
  return moduleCount > 1 ? sh.emissionModule(std::hash<std::string>()(name)) : currentModule;
}

auto codegen::Emitter::emitCallOperatorOnDemand(const Ty* t, const Ty* callTy, const std::string& name) -> void
{
  LambdaExpr* l = lambdaOf(t);
  FunctionDecl* writer = lambdaWriter(l);
  if (!writer)
    return;
  Decl* ownerClass = global.owningClass(writer);
  {
    auto lock = sharedLock();
    if (!sh.closuresEmitted.insert(t).second)
      return;
    ClosureWriter& recorded = sh.closureWriters[types.unqualified(t)];
    if (!recorded.enclosingClass && ownerClass && ownerClass->kind == NodeKind::ClassDecl)
      recorded.enclosingClass = types.classType(ownerClass, {}); // what `this` in the body points to
  }
  // Placed and owned as if its writer had made it, whichever caller needs it first (callers of one
  // level are emitted at once): where its closure's first writer puts it too (lambdaHome: the two
  // share closuresEmitted), belonging to the writer. An edit of the writer drops it, and rebuildLLVM
  // emits it again on its own (Shared::Definition::closure); what it inlines is its own, not its
  // first caller's.
  auto* writerFunction = static_cast<FunctionDecl*>(writer);
  struct Restore
  {
    Emitter& e;
    unsigned module;
    std::string owner;
    std::vector<const FunctionDecl*> inlined;
    OnDemand onDemand;
    ~Restore()
    {
      e.currentModule = module;
      e.owner = std::move(owner);
      e.inlinedNow = std::move(inlined);
      e.onDemandNow = std::move(onDemand);
    }
  } restore{*this, currentModule, owner, std::move(inlinedNow), onDemandNow};
  inlinedNow.clear();
  if (moduleCount > 1)
    currentModule = lambdaHome(l, name);
  owner = functionName(writerFunction, {}, nullptr);
  onDemandNow = {name, t, callTy, writerFunction};
  TemplateArgs none;
  emitLambda(l, t, callTy, name, &none);
}

auto codegen::Emitter::lambdaThunk(const Ty* t, const Ty* fnTy, Node* at) -> std::string
{
  t = types.unqualified(t);
  LambdaExpr* l = lambdaOf(t);
  ClosureInfo info = closureFor(t, at);
  if (!info.captured.empty())
    unsupported(at, "a lambda with captures used as a pointer to function");
  // The operator the pointer calls: the lambda's, or for a generic one (`[](const auto&
  // parameters, MapDeserialiser& input) -> ElemType*` as InstanceLoader's LoadFunction) the
  // instance whose parameters the pointer's give, each `auto` the pointer's type there.
  const Ty* callTy = types.function(t->inner ? t->inner : types.voidType(), t->args, false);
  std::string functionName = info.functionName;
  std::string suffix;
  if (l && !l->templateParams.empty())
  {
    if (fnTy->args.size() != t->args.size())
      unsupported(at, "a lambda used as a pointer to a function of another signature");
    TemplateArgs own;
    for (Decl* p : l->templateParams)
    {
      const Ty* found = nullptr;
      for (size_t i = 0; i < t->args.size() && !found; ++i)
        if (const Ty* pt = types.unqualified(types.stripRef(t->args[i])); pt->kind == TyKind::Param && pt->decl == p)
          found = types.unqualified(types.stripRef(fnTy->args[i]));
      if (!found)
        unsupported(at, "a generic lambda used as a pointer to function, its parameters not deduced from the pointer's");
      own.pairs.push_back({p, found});
    }
    TemplateArgs instanceArgs;
    LambdaExpr* genericBody = nullptr;
    callTy = global.lambdaInstance(t, own, &instanceArgs, &genericBody);
    if (!callTy || !genericBody)
      unsupported(at, "a generic lambda whose body could not be typed for the pointer's parameters");
    std::string params;
    for (size_t i = 0; i < callTy->args.size(); ++i)
      params += (i ? ", " : "") + irString(callTy->args[i]);
    functionName = genericOperatorName(t, own, callTy);
    suffix = "(" + params + ")";
    bool firstUse;
    {
      auto lock = sharedLock();
      firstUse = lambdaInstances.insert(functionName).second;
    }
    if (firstUse)
    {
      NameHome home(*this, functionName);
      emitLambda(genericBody, t, callTy, functionName, &instanceArgs);
    }
  }
  const Ty* ret = fnTy->inner;
  const Ty* lambdaRet = callTy->inner ? callTy->inner : types.voidType();
  if (types.unqualified(types.stripRef(ret)) != types.unqualified(types.stripRef(lambdaRet)) || fnTy->args.size() != callTy->args.size())
    unsupported(at, "a lambda used as a pointer to a function of another signature");
  for (size_t i = 0; i < fnTy->args.size(); ++i)
  {
    const Ty* pt = fnTy->args[i];
    if (types.unqualified(types.stripRef(pt)) != types.unqualified(types.stripRef(callTy->args[i])))
      unsupported(at, "a lambda used as a pointer to a function of another signature");
    if (isClass(pt) && needsDestruction(pt, at))
      unsupported(at, "a lambda used as a pointer to function taking an object with a destructor by value");
  }
  std::string name = "@\"" + irString(t) + "::thunk" + suffix + "\"";
  {
    auto lock = sharedLock();
    if (!lambdaInstances.insert(name).second)
      return name;
  }
  bool hasSret = returnsInMemory(ret);
  std::string retType = hasSret ? "void" : (!ret || !ret->isKnown()) ? "ptr" : ret->isReference() ? "ptr" : llType(ret, at);
  std::string params, args = "ptr null";
  if (hasSret)
  {
    params = "ptr %sret";
    args += ", ptr %sret";
  }
  for (size_t i = 0; i < fnTy->args.size(); ++i)
  {
    const Ty* pt = fnTy->args[i];
    std::string ty = isAggregate(pt) || pt->isReference() || !pt->isKnown() ? "ptr" : llType(pt, at);
    params += (params.empty() ? "" : ", ") + ty + " %a" + std::to_string(i);
    args += ", " + ty + " %a" + std::to_string(i);
  }
  std::string text = "define " + retType + " " + name + "(" + params + ") {\nentry:\n";
  if (hasSret || ret->kind == TyKind::Void)
    text += "  call void " + functionName + "(" + args + ")\n  ret void\n";
  else
    text += "  %r = call " + retType + " " + functionName + "(" + args + ")\n  ret " + retType + " %r\n";
  text += "}\n\n";
  NameHome home(*this, name);
  addFunction(name, text, true); // emitted once, at its first use: its own, not the user's
  return name;
}

auto codegen::Emitter::genericOperatorName(const Ty* t, const TemplateArgs& own, const Ty* callTy) -> std::string
{
  std::string args, params;
  for (size_t i = 0; i < own.pairs.size(); ++i)
    args += (i ? ", " : "") + irString(own.pairs[i].second);
  for (size_t i = 0; i < callTy->args.size(); ++i)
    params += (i ? ", " : "") + irString(callTy->args[i]);
  return "@\"" + irString(t) + "::operator()<" + args + ">(" + params + ")\"";
}

auto codegen::Emitter::callClosure(Value closure, Span<Expr*> callArgs, const std::string& sret, Node* at) -> Value
{
  const Ty* t = types.unqualified(closure.ty); // a const lambda variable calls the same operator
  ClosureInfo info = closureFor(t, at);
  const Ty* callTy = types.function(t->inner ? t->inner : types.voidType(), t->args, false);
  std::string functionName = info.functionName;
  TemplateArgs instanceArgs;
  LambdaExpr* genericBody = nullptr; // a generic lambda's copy of its parameters and body for this call's arguments
  LambdaExpr* genericLambda = lambdaOf(t);
  if (genericLambda && genericLambda->templateParams.empty())
    genericLambda = nullptr;
  if (LambdaExpr* l = genericLambda)
  {
    // A generic lambda: the call's deduced arguments (recorded by the type checker, as for a
    // function template) pick the instantiation of its call operator, emitted at first use.
    CallExpr* call = at && at->kind == NodeKind::CallExpr ? static_cast<CallExpr*>(at) : nullptr;
    if (!call || call->templateArgCount != l->templateParams.size())
      unsupported(at, "a call to a generic lambda whose parameter types were not deduced");
    TemplateArgs own;
    for (size_t i = 0; i < l->templateParams.size(); ++i)
      own.pairs.push_back({l->templateParams[i], call->templateArgs[i]});
    callTy = global.lambdaInstance(t, own, &instanceArgs, &genericBody);
    if (!callTy || !genericBody)
      unsupported(at, "a generic lambda whose body could not be typed for this call");
    functionName = genericOperatorName(t, own, callTy);
  }
  else
  {
    // Emitted with the function the lambda is written in: checked once generation is done.
    {
      auto lock = sharedLock();
      sh.closureCalls.emplace(functionName, t);
    }
    // Declared by the call, for a program piece whose function calls an operator its writer (an
    // instance) emits at a later level (Shared::requestDeclarations).
    if (recordingRequests())
      withoutFailing([&] { recordRequestDeclaration(functionName, declarationText(lambdaSignatureOf(callTy, at), functionName)); });
    emitCallOperatorOnDemand(t, callTy, functionName);
  }
  const Ty* ret = callTy->inner ? callTy->inner : types.voidType();
  std::string argText = "ptr " + closure.text;
  std::string resultAddr = sret;
  bool hasSret = returnsInMemory(ret);
  bool resultTemp = false; // owned by the statement once the call returned it
  if (hasSret)
  {
    if (resultAddr.empty())
    {
      resultAddr = newObjectSlot(ret, at);
      resultTemp = true;
    }
    argText += ", ptr " + resultAddr;
  }
  std::string sig = (hasSret ? "void" : ret->isReference() ? "ptr" : llType(ret, at)) + std::string(" (ptr") + (hasSret ? ", ptr" : "");
  size_t position = 0;
  for (Expr* arg : callArgs)
  {
    if (isPackExpansion(arg))
    {
      Expr* pattern = static_cast<UnaryExpr*>(arg)->operand;
      forEachPackElement(pattern, at, [&](size_t)
      {
        const Ty* pt = position < callTy->args.size() ? callTy->args[position] : nullptr;
        if (!pt)
          unsupported(at, "too many arguments to a lambda");
        sig += ", " + std::string(isAggregate(pt) || pt->isReference() ? "ptr" : llType(pt, at));
        argText += ", " + argumentText(pt, pattern, at);
        ++position;
      });
      continue;
    }
    const Ty* pt = position < callTy->args.size() ? callTy->args[position] : nullptr;
    if (!pt)
      unsupported(at, "too many arguments to a lambda");
    sig += ", " + std::string(isAggregate(pt) || pt->isReference() ? "ptr" : llType(pt, at));
    argText += ", " + argumentText(pt, arg, at);
    ++position;
  }
  // Parameters the call left out take their default arguments: GuiTargetItemBase's loader calls
  // `[&](auto& value, OpenGuiType initIfType, bool load = true)` with two, and the operator read
  // whatever its third register held (no targeter was read, and the map loading lost its place).
  LambdaExpr* declared = genericBody ? genericBody : lambdaOf(t);
  for (; position < callTy->args.size(); ++position)
  {
    auto* pd = declared && position < declared->params.size() ? static_cast<ParamDecl*>(declared->params[position]) : nullptr;
    if (!pd || pd->isPack)
      break;
    if (!pd->defaultValue)
      unsupported(at, "a call to a lambda without an argument for its parameter " + (pd->name != UINT32_MAX ? sym(pd->name) : std::to_string(position + 1)));
    const Ty* pt = callTy->args[position];
    sig += ", " + std::string(isAggregate(pt) || pt->isReference() ? "ptr" : llType(pt, at));
    if (pd->defaultValue->kind == NodeKind::CallerExpr)
      argText += ", ptr " + callerLocation(pt, at); // `CallerLocation here = caller`: this call's location, as for a function
    else
      argText += ", " + argumentText(pt, pd->defaultValue, pd->defaultValue);
  }
  sig += ")";
  // The instantiation's operator is emitted after the arguments: a lambda argument's closure
  // (its capture list) is built here, in the function that writes it, before the operator's
  // body calls it.
  bool firstUse = false;
  if (genericLambda)
  {
    auto lock = sharedLock();
    firstUse = lambdaInstances.insert(functionName).second;
  }
  if (firstUse)
  {
    NameHome home(*this, functionName);
    emitLambda(genericBody, t, callTy, functionName, &instanceArgs);
  }
  if (hasSret || ret->kind == TyKind::Void)
  {
    emitCall("", sig + " " + functionName + "(" + argText + ")");
    if (resultTemp)
      statementTemps.push_back({resultAddr, ret}); // not before: a call that throws built no result to destroy
    return hasSret ? Value{resultAddr, ret, sret.empty()} : Value{"", types.voidType(), false};
  }
  std::string r = newTemp();
  emitCall(r + " = ", sig + " " + functionName + "(" + argText + ")");
  return {r, ret, false};
}
