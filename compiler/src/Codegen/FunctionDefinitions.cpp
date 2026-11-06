// struct Emitter's members: function definitions and startup globals; their declarations in
// FunctionDefinitions.hpp, part of the struct's body in Codegen/Emitter.hpp. A translation unit of
// its own since 2026-11-05 (codegen.cpp split, stage 2), moved without a change.
#include "Emitter.hpp"

// Whether a function type is one of the forms C++ gives main, which the program's entry calls
// (emitEntry): int() or int(int, char**), wmain's int(int, wchar_t**), const anywhere in the
// pointers (as C++ compilers take it). Another (main(int), the environment's main(int, char**,
// char**)) was called with no arguments, by register luck at -O0 and with none in a release build.
static bool entryForm(const Ty* ft, bool wide)
{
  auto isInt = [](const Ty* t) { return t && t->kind == TyKind::Int && t->bits == 32 && !t->isUnsigned && t->count == 0; };
  auto pointee = [](const Ty* t) -> const Ty* { return t && (t->kind == TyKind::Pointer || t->kind == TyKind::Array) ? t->inner : nullptr; };
  if (!isInt(ft->inner) || ft->variadic)
    return false;
  if (ft->args.size() == 0)
    return true;
  if (ft->args.size() != 2 || !isInt(ft->args[0]))
    return false;
  const Ty* c = pointee(pointee(ft->args[1]));
  return c && c->kind == TyKind::Char && (wide ? c->count == 1 : c->bits == 8 && c->count == 0 && !c->isUnsigned);
}

auto codegen::Emitter::retypedPerElement(FunctionDecl* f) -> bool
{
  for (Decl* p = f; p; p = p->parent)
    for (Decl* tp : p->templateParams)
      if (tp->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(tp)->isPack)
        return true;
  if (!f->body)
    return false;
  // In a lambda's body too for an instantiation's copy, which other threads read (a lambda's `for
  // constexpr` retypes the function it is written in); a plain function's own nodes are retyped in
  // place there, as they were (its private copy's walks, with no arguments, gave the lambda's
  // variable the type of the last element's walk: templates/for_constexpr_in_lambda_of_function).
  auto [it, added] = forConstexprIn.try_emplace(f->body, false); // (by body: a compile server's edit grafts a new one)
  if (added)
    it->second = containsForConstexpr(f->body, (f->attributes & ATTR_InstanceCopy) != 0);
  return it->second;
}

auto codegen::Emitter::containsForConstexpr(Node* n, bool intoLambdas, int depth) -> bool
{
  if (!n || depth > 256)
    return false;
  auto in = [&](Node* c) { return containsForConstexpr(c, intoLambdas, depth + 1); };
  auto any = [&](Span<Expr*> es)
  {
    for (Expr* e : es)
      if (in(e))
        return true;
    return false;
  };
  switch (n->kind)
  {
    case NodeKind::ForConstexprStmt: return true;
    case NodeKind::BlockStmt:
      for (Stmt* inner : static_cast<BlockStmt*>(n)->stmts)
        if (in(inner))
          return true;
      return false;
    case NodeKind::ExprStmt: return in(static_cast<ExprStmt*>(n)->expr);
    case NodeKind::DeclStmt:
      for (Decl* d : static_cast<DeclStmt*>(n)->decls)
        if (in(d))
          return true;
      return false;
    case NodeKind::VariableDecl: return in(static_cast<VariableDecl*>(n)->init); // (a condition that declares)
    case NodeKind::IfStmt:
    {
      auto* i = static_cast<IfStmt*>(n);
      return in(i->init) || in(i->cond) || in(i->then) || in(i->else_);
    }
    case NodeKind::ForStmt:
    {
      auto* f = static_cast<ForStmt*>(n);
      return in(f->init) || in(f->cond) || in(f->step) || in(f->body);
    }
    case NodeKind::RangeForStmt: return in(static_cast<RangeForStmt*>(n)->range) || in(static_cast<RangeForStmt*>(n)->body);
    case NodeKind::WhileStmt: return in(static_cast<WhileStmt*>(n)->cond) || in(static_cast<WhileStmt*>(n)->body);
    case NodeKind::DoStmt: return in(static_cast<DoStmt*>(n)->body) || in(static_cast<DoStmt*>(n)->cond);
    case NodeKind::SwitchStmt:
    {
      auto* s = static_cast<SwitchStmt*>(n);
      return in(s->init) || in(s->cond) || in(s->body);
    }
    case NodeKind::ReturnStmt: return in(static_cast<ReturnStmt*>(n)->value);
    case NodeKind::TryStmt:
    {
      auto* t = static_cast<TryStmt*>(n);
      if (in(t->body))
        return true;
      for (const TryStmt::Handler& h : t->handlers)
        if (in(h.body))
          return true;
      return false;
    }
    // A lambda's body is the function's nodes too (emitLambda retypes the function for it), and so
    // is whatever holds a lambda.
    case NodeKind::LambdaExpr:
    {
      if (!intoLambdas)
        return false;
      auto* l = static_cast<LambdaExpr*>(n);
      for (const LambdaCapture& c : l->captures)
        if (in(c.init))
          return true;
      return in(l->body);
    }
    case NodeKind::CallExpr: return in(static_cast<CallExpr*>(n)->callee) || any(static_cast<CallExpr*>(n)->args);
    case NodeKind::IndexExpr: return in(static_cast<IndexExpr*>(n)->base) || any(static_cast<IndexExpr*>(n)->args);
    case NodeKind::MemberExpr: return in(static_cast<MemberExpr*>(n)->base);
    case NodeKind::UnaryExpr: return in(static_cast<UnaryExpr*>(n)->operand);
    case NodeKind::BinaryExpr: return in(static_cast<BinaryExpr*>(n)->lhs) || in(static_cast<BinaryExpr*>(n)->rhs);
    case NodeKind::ConditionalExpr:
    {
      auto* c = static_cast<ConditionalExpr*>(n);
      return in(c->cond) || in(c->then) || in(c->else_);
    }
    case NodeKind::CastExpr: return in(static_cast<CastExpr*>(n)->operand);
    case NodeKind::NamedCastExpr: return in(static_cast<NamedCastExpr*>(n)->operand);
    case NodeKind::FunctionalCastExpr: return any(static_cast<FunctionalCastExpr*>(n)->args);
    case NodeKind::NewExpr: return any(static_cast<NewExpr*>(n)->placement) || any(static_cast<NewExpr*>(n)->args);
    case NodeKind::BraceInitExpr: return any(static_cast<BraceInitExpr*>(n)->args);
    case NodeKind::ThrowExpr: return in(static_cast<ThrowExpr*>(n)->operand);
    case NodeKind::ParenExpr: return in(static_cast<ParenExpr*>(n)->inner);
    case NodeKind::FoldExpr: return in(static_cast<FoldExpr*>(n)->lhs) || in(static_cast<FoldExpr*>(n)->rhs);
    default: return false;
  }
}

auto codegen::Emitter::emitFunction(FunctionDecl* original, const TemplateArgs& args, const Ty* classTy, const std::string& nameOverride) -> void
{
  // An instantiation is emitted from its own copy of the template's nodes, typed for args once
  // (GlobalScope::instanceBody); its IR name is the template's with the arguments. One that is
  // re-typed per pack element while it is emitted gets a copy of its own for that, so that
  // nobody reading the instantiation's copy meanwhile (a caller's default arguments, on another
  // thread) sees the element's types.
  FunctionDecl* f = args.empty() ? original : global.instanceBody(original, args, classTy);
  FunctionDecl* checked = f; // the body the checker typed, whose static_asserts count now (GlobalScope::functionEmitted)
  if (retypedPerElement(f))
    f = global.privateInstance(original, args, classTy);
  if (!conditionHolds(f))
    return; // conditional(false): the function does not exist in this build
  Restore<const FunctionDecl*> restore(checkerChecked);
  checkerChecked = checked;
  global.functionEmitted(checked);
  if (f != checked)
    global.functionEmitted(f);
  currentFile = f->file;
  currentArgs = args.empty() ? nullptr : &args;
  currentClassTy = classTy;
  if (!currentClassTy)
    if (Decl* cls = global.owningClass(f))
      currentClassTy = cls->kind == NodeKind::EnumDecl ? types.enumType(cls) : canonicalClass(types.classType(cls, {}), f);
  // The type callers see: the template's own declaration under args. An `auto` instantiation's
  // return type is the one its body (the copy emitted here) deduced; asking for the copy's type
  // would type a copy of the copy, whose lambdas are other closure types than the body's.
  const Ty* ft = declType(original);
  if (!ft || ft->kind != TyKind::Function)
  {
    error(f, "function type unknown, skipped");
    currentArgs = nullptr;
    return;
  }
  resetFunctionState();
  currentFunction = f;
  retypedLambda = nullptr;
  retypedLambdaTy = nullptr;
  // Its nodes were typed for this function alone (design.md 24): the checker's own copy by its walk,
  // or a private copy by privateInstance's walk and then, per element of a pack expansion or a `for
  // constexpr`, by retypeFor's (each a walk that records, of nodes no other emission reads).
  choicesRecorded = true;
  bodyNow = f->body;
  nestedNamesByBody.clear();
  if (sh.debugInfo)
  {
    debugAt = f;
    debugFile = currentFile;
  }
  currentReturn = ft->inner;
  // If the return type is unknown, pretend it's ptr so the function body can be emitted;
  // return statements will try to convert their value to a pointer (and throw if they can't).
  if (!currentReturn || !currentReturn->isKnown())
    currentReturn = types.pointer(types.voidType());
  sretAddr.clear();
  currentFunctionName = nameOverride.empty() ? functionName(original, args, classTy) : nameOverride;
  currentContext = displayFunctionName(original, args, classTy);
  struct OwnerScope
  {
    ~OwnerScope()
    {
      if (outermost)
        owner.clear();
    }

    std::string& owner;
    bool outermost;
  } ownerScope{owner, owner.empty()};
  if (ownerScope.outermost)
  {
    owner = currentFunctionName;
    emittingName = currentFunctionName;
    emittingNow = {original, args, classTy, currentModule, currentRequestedBy, nameOverride, currentDepth, currentRequestedByContext};
    inlinedNow.clear();
    layoutsNow.clear();
    // One of the program's own functions (a level-0 item): what it owns goes in its module's
    // program piece (Shared::splitPieces).
    if (sh.splitPieces && currentDepth == 0 && currentRequestedBy.empty() && args.empty() && !classTy && nameOverride.empty() &&
        isProgramFunction(original))
      programFunctionsNow.push_back(currentFunctionName);
  }
  try
  {
    if (bool wide = currentFunctionName == "@kpp.wmain"; (currentFunctionName == "@kpp.main" || (wide && sh.target.windows())) && !entryForm(ft, wide))
      error(f, wide ? "wmain is int wmain() or int wmain(int argc, wchar_t** argv)"
                    : "main is int main() or int main(int argc, char** argv)" + std::string(ft->args.size() == 3 ? "; getenv reads the environment" : ""));
    else if (currentFunctionName == "@main") // (C++ too: a program that declares main with C language linkage is ill-formed)
      error(f, "main is not extern \"C\": the program's entry calls it");
    Signature s = signatureOf(original);
    std::string params, returnType = s.returnType;
    std::vector<std::string> paramNames;
    if (isCFunction(original) && cStructsByValue(s, currentReturn, f))
    {
      // A C-callable function (extern "C", or a C header's inline one) with a struct by value
      // takes its parameters and returns its result as its target's C does, as a C caller passes
      // them and kpp's own calls of it do (cDeclaration): a struct in registers is stored into a slot
      // of its own, which the body has as kpp's pointer to a copy (a `byval` copy on the stack is
      // that pointer already); a result in registers is built in a slot of its own, returned from
      // there (emitReturnVoid); one in memory takes C's result slot, `sret`.
      CRegisters left;
      unsigned next = 0;
      auto parameter = [&](const std::string& type)
      {
        std::string pn = "%p" + std::to_string(next++);
        params += (params.empty() ? "" : ", ") + type + " " + pn;
        return pn;
      };
      for (size_t i = 0; i < s.paramTypes.size(); ++i)
      {
        const Ty* pt = s.paramTys[i];
        if (s.hasSret && i == 0)
        {
          const Ty* ret = currentReturn;
          CPassing r = cPassing(ret, f);
          if (r.inMemory)
          {
            left.ints -= 1;
            paramNames.push_back(parameter("ptr sret(" + llType(ret, f) + ")"));
            continue;
          }
          returnType = cReturnType(r);
          cReturnParts = r.parts;
          cReturnOffsets = r.offsets;
          paramNames.push_back(newObjectSlot(ret, f));
          continue;
        }
        if (!pt || !isAggregate(pt))
        {
          cScalarArgument(pt, left);
          paramNames.push_back(parameter(s.paramTypes[i]));
          continue;
        }
        CPassing p;
        std::vector<std::string> passed = cArgumentTypes(pt, left, p, f);
        if (p.inMemory)
        {
          paramNames.push_back(parameter(passed[0])); // (`ptr byval(...)`, or Windows' pointer to a copy)
          continue;
        }
        std::string copy = newObjectSlot(pt, f);
        for (size_t k = 0; k < p.parts.size(); ++k)
        {
          std::string pn = parameter(p.parts[k]), to = copy;
          if (p.offsets[k])
          {
            to = newTemp();
            fn += "  " + to + " = getelementptr inbounds i8, ptr " + copy + ", i64 " + std::to_string(p.offsets[k]) + "\n";
          }
          fn += "  store " + p.parts[k] + " " + pn + ", ptr " + to + "\n";
        }
        paramNames.push_back(copy);
      }
    }
    else
      for (size_t i = 0; i < s.paramTypes.size(); ++i)
      {
        std::string pn = "%p" + std::to_string(i);
        paramNames.push_back(pn);
        params += (i ? ", " : "") + s.paramTypes[i] + " " + pn;
      }
    if (s.variadic)
      params += params.empty() ? "..." : ", ...";
    std::string name = currentFunctionName;
    declaredNames.claim(name);
    // A C header's inline function (useFunctionOnce: @"kpp.c-inline.NAME") is weak_odr: its home
    // module's copy is the only one, and ThinLTO dropped a linkonce_odr one that only other
    // modules called (`undefined symbol: floorl`).
    bool headerInline = original->file && original->file->isImported;
    std::string header = std::string("define ") + (headerInline ? "weak_odr " : "") + returnType + " " + name + "(" + params + ") " +
                         attributeText(f) + "{\nentry:\n";
    pushScope();
    size_t slot = 0;
    if (s.hasThis)
    {
      std::string thisSlot = "%v" + std::to_string(temp++);
      allocas += "  " + thisSlot + " = alloca ptr\n";
      fn += "  store ptr " + paramNames[slot] + ", ptr " + thisSlot + "\n";
      locals[nullptr] = {thisSlot, nullptr, false};
      if (currentClassTy)
        debugVariable("this", f, thisSlot, types.pointer((f->flags & DF_Const) ? types.qualified(currentClassTy, true, false) : currentClassTy), 1, true); // (a const one's points to const, as clang's)
      ++slot;
    }
    if (s.hasSret)
      sretAddr = paramNames[slot++];
    packLocals.clear();
    for (size_t i = 0; i < f->params.size(); ++i)
    {
      auto* pd = static_cast<ParamDecl*>(f->params[i]);
      size_t count = 1;
      if (pd->isPack)
      {
        const Ty* packTy = declType(pd);
        if (!packTy || packTy->kind != TyKind::Pack)
          unsupported(f, "a parameter pack that was not deduced");
        count = packTy->args.size();
        packLocals[pd] = {};
      }
      for (size_t k = 0; k < count; ++k, ++slot)
      {
        if (slot >= s.paramTys.size())
          unsupported(f, "parameter " + std::to_string(i + 1) + " (" + types.toString(declType(pd), &symbols) +
                             ") beyond the parameters of the function's type " + types.toString(ft, &symbols));
        const Ty* pt = s.paramTys[slot];
        Local local;
        if (isAggregate(pt))
        {
          local = {paramNames[slot], pt, false};
          if (needsDestruction(pt, f))
            scopes.back().push_back({paramNames[slot], pt});
        }
        else
        {
          bool isRef = pt->isReference();
          std::string ps = "%v" + std::to_string(temp++);
          allocas += "  " + ps + " = alloca " + (isRef ? std::string("ptr") : llType(pt, f)) + "\n";
          fn += "  store " + (isRef ? std::string("ptr") : llType(pt, f)) + " " + paramNames[slot] + ", ptr " + ps + "\n";
          local = {ps, pt, isRef};
        }
        // (a pack's elements are the debugger's as one each: name#k)
        if (pd->name != UINT32_MAX)
        {
          // A class passed by pointer: the debugger's at a slot that keeps the pointer, read through it
          // (DW_OP_deref), as clang declares one; at the incoming pointer itself, which no slot kept
          // at -O0, Visual Studio showed "Variable is optimized away" (until 2026-11-05).
          std::string address = local.addr;
          bool indirect = false;
          if (isAggregate(pt) && sh.fullDebugInfo && debugFile && debugFile == currentFile && inlining.empty())
          {
            address = "%v" + std::to_string(temp++);
            allocas += "  " + address + " = alloca ptr\n";
            fn += "  store ptr " + paramNames[slot] + ", ptr " + address + "\n";
            indirect = true;
          }
          debugVariable(sym(pd->name) + (pd->isPack ? "#" + std::to_string(k) : std::string()), pd, address, pt,
                        uint32_t(slot + 1 - (s.hasSret ? 1 : 0)), false, false, indirect);
        }
        if (pd->isPack)
          packLocals[pd].push_back(local);
        else
          locals[pd] = local;
      }
    }
    forgetTestCall(f);
    if ((f->flags & DF_Default) && !f->body)
    {
      // `= default`: the memberwise body; the source object, if any, is the parameter after this.
      size_t sourceIndex = s.hasSret ? 2 : 1;
      std::string source = paramNames.size() > sourceIndex ? paramNames[sourceIndex] : std::string();
      Value r = defaultedOperation(f, types.unqualified(types.stripRef(currentClassTy)), thisPointer(f), source, f);
      destroyTemps();
      popScope();
      if (isClass(currentReturn))
      {
        // A class result (a defaulted operator<=>'s strong_ordering): into the result slot.
        if (!sretAddr.empty() && r.ty && isClass(r.ty))
          fn += "  store i8 " + orderOf(r, f).text + ", ptr " + gep(structFor(currentReturn, f).name, sretAddr, 0) + "\n";
        fn += "  ret void\n";
      }
      else if (currentReturn->kind == TyKind::Void)
        fn += "  ret void\n";
      else if (currentReturn->isReference())
        fn += "  ret ptr " + r.text + "\n";
      else
        fn += "  ret " + llType(currentReturn, f) + " " + r.text + "\n";
      addFunction(name, functionText(header));
      currentArgs = nullptr;
      currentClassTy = nullptr;
      return;
    }
    // (a constructor's function-try-block runs them inside its try: tryBody)
    if ((f->flags & DF_Constructor) && !constructorTryOf(f))
    {
      std::string held = holdTestCall(f);
      initializeMembers(thisPointer(f), currentClassTy, f->ctorInits, f, true);
      destroyTemps();
      releaseTestCall(held);
    }
    bool destructor = (f->flags & DF_Destructor) && isClass(currentClassTy);
    if (destructor)
    {
      storeVptrs(thisPointer(f), structFor(currentClassTy, f)); // virtual calls while it is destroyed reach this class's functions, as in C++
      // A destructor that may throw: its members and bases are destroyed when its body throws, too.
      if (destructionMayThrow(currentClassTy, f))
      {
        StructInfo& info = structFor(currentClassTy, f);
        std::string structName = info.name, self = thisPointer(f);
        std::vector<std::pair<Decl*, uint32_t>> bases = info.bases;
        std::vector<Decl*> fieldDecls = info.fieldDecls;
        std::vector<const Ty*> fieldTypes = info.fieldTypes;
        std::unordered_map<Decl*, uint32_t> fieldIndex = info.fieldIndex;
        auto member = [&](uint32_t index)
        {
          Object o{gep(structName, self, index), fieldTypes[index]};
          o.unwindOnly = true;
          scopes.back().push_back(o);
        };
        for (auto& [base, index] : bases)
          if (needsDestruction(fieldTypes[index], f))
            member(index);
        for (Decl* d : fieldDecls)
          if (!fieldTypes[fieldIndex[d]]->isReference() && needsDestruction(fieldTypes[fieldIndex[d]], f))
            member(fieldIndex[d]);
      }
      // The members and bases are destroyed after the body however it ends: falling off its end
      // or at a `return` (CargoBayResizeHelper's `if (!this->surface) return;` left its targeter
      // member in the hub's list, and destroying the hub faulted).
      returnLabel = newLabel("dtor.members");
    }
    namedReturn = builtInResult(global.namedReturnVariable(f->body, currentReturn, nullptr, currentArgs), f);
    statement(f->body);
    if (destructor)
    {
      popScope();
      emitBranch(returnLabel);
      startBlock(returnLabel);
      returnLabel.clear();
      destroyMembers(thisPointer(f), currentClassTy, f);
      fn += "  ret void\n";
    }
    else if (!blockTerminated)
    {
      popScope();
      if (currentReturn->kind == TyKind::Void || returnsInMemory(currentReturn))
        emitReturnVoid();
      else if (name == "@kpp.main" || name == "@kpp.wmain")
        fn += "  ret i32 0\n";
      else // falling off the end (after a throw, usually): a zero rather than `unreachable`, see emitThrow
        fn += "  ret " + (currentReturn->isReference() ? std::string("ptr null") : llType(currentReturn, f) + " zeroinitializer") + "\n";
    }
    addFunction(name, functionText(header));
  }
  catch (Unsupported&)
  {
    failedFunctions.push_back(currentFunctionName);
    failedAt[currentFunctionName] = qualifiedName(original) + " at " + programLocationOf(original, original->file); // what --stub-missing's stub says
  }
  currentArgs = nullptr;
  currentClassTy = nullptr;
}

auto codegen::Emitter::checkLayoutAssert(const GlobalScope::LayoutStaticAssert& a) -> void
{
  StaticAssertDecl* s = a.decl;
  if (!s || !s->cond)
    return;
  ArgsScope scope(*this, &a.args);
  ConstContextScope context(*this, s);
  currentFile = s->file;
  currentFunctionName.clear();
  currentRequestedBy.clear();
  std::optional<bool> v;
  GlobalScope::clearConstantReason();
  try
  {
    v = evalCondition(s->cond);
  }
  catch (Unsupported&)
  {
    return;
  }
  if (v && *v)
    return;
  // As the checker words it: a condition that is no constant says why (the interpreter's reason).
  std::string why = v ? std::string() : GlobalScope::constantReason();
  std::string message = v                ? "static assertion failed"
                      : !why.empty()     ? "static assertion whose condition is no constant: " + why
                                         : "static assertion whose condition the compiler cannot evaluate yet";
  if (s->message && s->message->kind == NodeKind::LiteralExpr && s->file && s->file->tokens && !s->file->tokens->empty())
  {
    const Token& t = (*s->file->tokens)[std::min<size_t>(s->message->token, s->file->tokens->size() - 1)];
    message += ": " + std::string(s->file->text.substr(t.offset, t.length));
  }
  if (v && !a.instance.empty())
    message += " (in " + a.instance + ")";
  error(s, message);
}

auto codegen::Emitter::settleDefinitionFlags(Span<Decl*> decls) -> void
{
  for (Decl* d : decls)
  {
    if (d->kind == NodeKind::NamespaceDecl)
      settleDefinitionFlags(static_cast<NamespaceDecl*>(d)->decls);
    if (d->kind == NodeKind::ClassDecl)
    {
      // `Flow(Flow&&) noexcept;` in the class and `Flow::Flow(Flow&&) noexcept = default;`
      // outside: the declaration is defaulted too. Found from the declaration, as useFunction
      // finds the definition (declarationOf finds no constructor or destructor), so that
      // useFunction never has to write the flag while other threads read it.
      for (Decl* m : static_cast<ClassDecl*>(d)->members)
        if (m->kind == NodeKind::FunctionDecl && !static_cast<FunctionDecl*>(m)->body && !(m->flags & DF_Default))
          if (Decl* def = global.definitionOf(m);
              def && def != m && def->kind == NodeKind::FunctionDecl && (def->flags & DF_Default) && !static_cast<FunctionDecl*>(def)->body)
            m->flags |= DF_Default;
      settleDefinitionFlags(static_cast<ClassDecl*>(d)->members); // nested classes
      continue;
    }
    if (d->kind != NodeKind::FunctionDecl || static_cast<FunctionDecl*>(d)->name.parts.size() <= 1)
      continue;
    auto* def = static_cast<FunctionDecl*>(d);
    FunctionDecl* declaration = global.declarationOf(def);
    if (declaration && (def->flags & DF_Default) && !def->body && !(declaration->flags & DF_Default))
      declaration->flags |= DF_Default;
    // `template<> void LuaDefines::setKey<const String&>(lua_State*, ...) {...}`: an explicit
    // specialisation of a member function template is static when the template is (`static` is
    // written in the class only, and the declaration lookup matches no template). Without it the
    // definition took a `this` its callers never pass, and read every argument one off.
    if (!declaration && def->isTemplate && def->templateParams.empty() && !(def->flags & DF_Static))
      if (Decl* owner = global.owningClass(def); owner && owner->kind == NodeKind::ClassDecl)
        if (ClassDecl* cls = classDef(owner, def))
        {
          const NamePart& name = def->name.last();
          bool anyTemplate = false, allStatic = true;
          for (Decl* m : cls->members)
            if (m->kind == NodeKind::FunctionDecl && !m->templateParams.empty() && !static_cast<FunctionDecl*>(m)->name.empty())
              if (const NamePart& mn = static_cast<FunctionDecl*>(m)->name.last(); mn.symbol == name.symbol && mn.special == name.special &&
                                                                                   mn.opToken == name.opToken)
              {
                anyTemplate = true;
                allStatic = allStatic && (m->flags & DF_Static);
              }
          if (anyTemplate && allStatic)
            def->flags |= DF_Static;
        }
  }
}

auto codegen::Emitter::collectStartupGlobals(Span<Decl*> decls, std::vector<VariableDecl*>& out) -> void
{
  for (Decl* d : decls)
  {
    if (d->kind == NodeKind::NamespaceDecl)
      collectStartupGlobals(static_cast<NamespaceDecl*>(d)->decls, out);
    else if (d->kind == NodeKind::ClassDecl)
    {
      auto* c = static_cast<ClassDecl*>(d);
      if (!c->templateParams.empty() || !c->isDefinition || c->isClosure || global.isTemplateContext(c))
        continue;
      for (Decl* m : c->members)
      {
        // `static inline T x = ...;` in the class, or `static bool registered;` defined outside
        // (`bool FontBank::loaderRegistered = ...;`, whose initializer considerStartupGlobal reads).
        if (m->kind == NodeKind::VariableDecl && (m->flags & DF_Static))
          considerStartupGlobal(static_cast<VariableDecl*>(m), out);
        else if (m->kind == NodeKind::ClassDecl)
          collectStartupGlobals(Span<Decl*>{&m, 1}, out); // a nested class
      }
    }
    else if (d->kind == NodeKind::VariableDecl && static_cast<VariableDecl*>(d)->name.parts.size() == 1)
      considerStartupGlobal(static_cast<VariableDecl*>(d), out); // (an out-of-line static member: through its class)
  }
}

auto codegen::Emitter::considerStartupGlobal(VariableDecl* v, std::vector<VariableDecl*>& out) -> void
{
  VariableDecl* def = variableDefinition(v);
  if ((v->flags & (DF_Typedef | DF_Constexpr | DF_ThreadLocal)) || (def->flags & (DF_Constexpr | DF_ThreadLocal)) || v->isTemplate ||
      !v->structuredBindings.empty() || (v->file && v->file->isImported) || global.isTemplateContext(v))
    return;
  if ((v->flags & DF_Extern) && !def->init && def == v)
    return; // a C library's variable
  if (def->init)
  {
    if (def->init->kind == NodeKind::LiteralExpr)
      return; // a constant: built by nobody
  }
  else
  {
    const Ty* t = global.typeOfDecl(v);
    const Ty* element = t;
    while (element && element->kind == TyKind::Array)
      element = element->inner;
    if (!element || !isClass(element))
      return; // zero
  }
  out.push_back(v);
}

auto codegen::Emitter::collectInstanceStartupGlobals(const Ty* cls, std::vector<std::pair<VariableDecl*, const Ty*>>& out) -> void
{
  Decl* d = global.definitionOf(cls->decl);
  if (!d || d->kind != NodeKind::ClassDecl)
    return;
  for (Decl* m : static_cast<ClassDecl*>(d)->members)
  {
    if (m->kind != NodeKind::VariableDecl || !(m->flags & DF_Static))
      continue;
    auto* v = static_cast<VariableDecl*>(m);
    VariableDecl* def = variableDefinition(v);
    if ((v->flags & (DF_Typedef | DF_Constexpr | DF_ThreadLocal)) || (def->flags & (DF_Constexpr | DF_ThreadLocal)) || v->isTemplate)
      continue;
    if (def->init ? def->init->kind == NodeKind::LiteralExpr : !needsConstruction(global.typeOfDeclIn(v, global.classArgsOf(cls)), v))
      continue;
    out.push_back({v, cls});
  }
}

auto codegen::Emitter::emitStartupGlobal(VariableDecl* v, const Ty* classTy, std::optional<Shared::Startup> at) -> void
{
  struct AtRestore { ~AtRestore() { a.reset(); } std::optional<Shared::Startup>& a; } atRestore{startupAt};
  startupAt = at;
  auto it = fileModule.find(v->file);
  currentModule = it == fileModule.end() ? 0 : it->second;
  currentFunctionName = "@\"kpp.init." + (classTy ? irString(classTy) + "::" + sym(v->name.last().symbol) : qualifiedName(v)) + "\"";
  currentRequestedBy.clear();
  currentContext = "the initializer of " + (classTy ? types.toString(classTy, &symbols) + "::" + sym(v->name.last().symbol) : qualifiedName(v));
  currentRequestedByContext.clear();
  currentFile = v->file; // its errors point at its declaration
  try
  {
    globalFor(v, v, classTy);
  }
  catch (Unsupported&)
  {
  }
  currentFunctionName.clear();
  currentFile = nullptr;
}

auto codegen::Emitter::collectFunctions(Span<Decl*> decls, std::vector<FunctionDecl*>& out) -> void
{
  for (Decl* d : decls)
  {
    switch (d->kind)
    {
      case NodeKind::NamespaceDecl: collectFunctions(static_cast<NamespaceDecl*>(d)->decls, out); break;
      case NodeKind::ClassDecl:
        if (static_cast<ClassDecl*>(d)->templateParams.empty())
          collectFunctions(static_cast<ClassDecl*>(d)->members, out);
        break;
      case NodeKind::EnumDecl: collectFunctions(static_cast<EnumDecl*>(d)->members, out); break;
      case NodeKind::FunctionDecl:
      {
        auto* f = static_cast<FunctionDecl*>(d);
        // (not the library's allocation function a program replaced: the program's is that function)
        if (f->body && f->templateParams.empty() && !global.isTemplateContext(f) && !(f->file && f->file->isImported) && !global.replacedAllocation(f))
          out.push_back(f);
        break;
      }
      default: break;
    }
  }
}

auto codegen::Emitter::isProgramFunction(FunctionDecl* f) -> bool
{
  if (!f->body || !f->templateParams.empty() || global.isTemplateContext(f) || (f->file && f->file->isImported))
    return false;
  for (Decl* p = f->parent; p; p = p->parent)
    if (p->kind == NodeKind::FunctionDecl || (p->kind == NodeKind::ClassDecl && !static_cast<ClassDecl*>(p)->templateParams.empty()))
      return false;
  return true;
}
