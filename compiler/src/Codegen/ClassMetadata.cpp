// struct Emitter's members: constructors, destructors, member initialisation and copies; their
// declarations in ClassMetadata.hpp, part of the struct's body in Codegen/Emitter.hpp. A
// translation unit of its own since 2026-11-05 (codegen.cpp split, stage 2), moved without a
// change.
#include "Emitter.hpp"

auto codegen::Emitter::constructorFor(const Ty* classTy, Span<Expr*> args, Node* at, bool implicitOnly) -> FunctionDecl*
{
  std::vector<bool> nulls;
  bool anyNull = false;
  for (Expr* a : args)
  {
    if (a && isPackExpansion(a))
      return findConstructor(classTy, argTypesOf(args), at, {}, implicitOnly); // (the elements are no literals)
    nulls.push_back(nullPointerConstant(a));
    anyNull = anyNull || nulls.back();
  }
  return findConstructor(classTy, argTypesOf(args), at, anyNull ? nulls : std::vector<bool>{}, implicitOnly);
}

auto codegen::Emitter::findConstructor(const Ty* classTy, const std::vector<const Ty*>& argTypes, Node* at, const std::vector<bool>& nulls,
                                       bool implicitOnly) -> FunctionDecl*
{
  const Ty* canonical = canonicalClass(classTy, at);
  ConstructorKey key{canonical, argTypes, nulls, implicitOnly};
  if (auto it = constructors.find(key); it != constructors.end())
    return it->second;
  // The one rule (design.md 24, GlobalScope::chooseConstructor), for the interpreter and the checker
  // to follow as well: the class's own constructors and those it inherits by `using
  // Base::Base;`. When an inherited one wins, null: constructInto builds that base from the
  // arguments (`MoveOnly direct("abc")` takes Text(const char*), not the move constructor through
  // a conversion by it).
  const Ty* inheritedFrom = nullptr;
  FunctionDecl* chosen = global.chooseConstructor(canonical, argTypes, nulls.empty() ? nullptr : &nulls, &inheritedFrom, implicitOnly);
  if (inheritedFrom)
    chosen = nullptr;
  constructors.emplace(std::move(key), chosen);
  return chosen;
}

auto codegen::Emitter::argTypesOf(Span<Expr*> args) -> std::vector<const Ty*>
{
  std::vector<const Ty*> out;
  for (Expr* a : args)
  {
    if (a && isPackExpansion(a))
    {
      // `args...`: one argument per element of the pack, under the current instantiation: the
      // parameter's own element types, or for a longer pattern the elements of its first pack.
      Expr* pattern = static_cast<UnaryExpr*>(a)->operand;
      Decl* param = packIn(a);
      if (param && pattern->kind == NodeKind::NameExpr && static_cast<NameExpr*>(pattern)->resolved == param)
      {
        for (const Local& element : packLocals[param])
          out.push_back(argumentType(pattern, element.ty, types));
        continue;
      }
      std::vector<Decl*> packs = param ? templatePacksOf(param) : std::vector<Decl*>{};
      const Ty* pack = nullptr;
      if (!packs.empty() && currentArgs)
        for (auto& [d, t] : currentArgs->pairs)
          if (d == packs[0])
            pack = t;
      if (pack && pack->kind == TyKind::Pack)
      {
        for (const Ty* element : pack->args)
          out.push_back(argumentType(a, element, types));
        continue;
      }
      // `values[I]...` over a template parameter pack: the pattern's type, once per element.
      if (Decl* valuePack = !param && currentArgs ? typePackIn(pattern) : nullptr)
        if (const Ty* bound = boundPack(valuePack); bound && bound->kind == TyKind::Pack)
        {
          for (size_t i = 0; i < bound->args.size(); ++i)
            out.push_back(argumentType(pattern, pattern->ty, types));
          continue;
        }
    }
    if (a && a->kind == NodeKind::BraceInitExpr)
    {
      out.push_back(braceListType(a, global.initializerListDecl(), types));
      continue;
    }
    out.push_back(a ? argumentType(a, a->ty, types) : nullptr);
  }
  return out;
}

auto codegen::Emitter::isInitializerListType(const Ty* t) -> bool
{
  Decl* d = global.initializerListDecl();
  return d && t && t->kind == TyKind::Class && t->decl == d;
}

auto codegen::Emitter::initializerListTemporary(const Ty* listTy, BraceInitExpr* b, Node* at, const std::string& variable) -> std::string
{
  if (listTy->args.empty())
    unsupported(at, "an InitializerList without its element type");
  const Ty* element = listTy->args[0];
  // A pack expansion among the elements (`{args...}`) gives one element per element of the pack.
  uint32_t n = 0;
  for (Expr* a : b->args)
    n += isPackExpansion(a) ? uint32_t(packElementCount(static_cast<UnaryExpr*>(a)->operand, at)) : 1;
  const Ty* arrayTy = types.array(element, n);
  bool staticArray = !variable.empty() && variable[0] == '@';
  std::string array;
  if (staticArray)
  {
    if (needsDestruction(arrayTy, at))
      unsupported(at, "an InitializerList in static storage of elements that need destruction");
    std::string owner = variable.substr(0, variable.size() - (variable.back() == '"' ? 1 : 0));
    array = owner + ".list." + std::to_string(b->token) + (variable.back() == '"' ? "\"" : "");
    addOwnedEntry(array + " = internal global " + llType(arrayTy, at) + " zeroinitializer\n");
  }
  else
    array = newObjectSlot(arrayTy, at);
  uint32_t i = 0;
  auto initializeElement = [&](Expr* value)
  {
    std::string slot = newTemp();
    fn += "  " + slot + " = getelementptr inbounds " + llType(arrayTy, at) + ", ptr " + array + ", i64 0, i64 " + std::to_string(i++) + "\n";
    initializeFrom(slot, element, value, at);
  };
  for (Expr* a : b->args)
  {
    if (isPackExpansion(a))
    {
      Expr* pattern = static_cast<UnaryExpr*>(a)->operand;
      forEachPackElement(pattern, at, [&](size_t) { initializeElement(pattern); });
    }
    else
      initializeElement(a);
  }
  if (!variable.empty() && !staticArray && needsDestruction(arrayTy, at))
    scopes.back().push_back({array, arrayTy}); // destroyed with the variable's scope
  else if (variable.empty() && needsDestruction(arrayTy, at))
    statementTemps.push_back({array, arrayTy});
  std::string list = variable.empty() ? newObjectSlot(listTy, at) : variable;
  const StructInfo& info = structFor(listTy, at);
  if (info.fieldDecls.size() < 2)
    unsupported(at, "an InitializerList class without its two fields");
  fn += "  store ptr " + array + ", ptr " + gep(info.name, list, fieldIndexOf(info, info.fieldDecls[0])) + "\n";
  // The count in its field's own type (the library's ContainerSize).
  uint32_t countField = fieldIndexOf(info, info.fieldDecls[1]);
  fn += "  store " + llType(info.fieldTypes[countField], at) + " " + std::to_string(n) + ", ptr " + gep(info.name, list, countField) + "\n";
  return list;
}

auto codegen::Emitter::isArrayBraces(Expr* e) -> bool
{
  if (e->kind == NodeKind::BraceInitExpr)
    return !static_cast<BraceInitExpr*>(e)->text; // (a Text's packed arguments are no list)
  if (e->kind != NodeKind::FunctionalCastExpr || !e->ty || types.stripRef(e->ty)->kind != TyKind::Array)
    return false;
  auto* c = static_cast<FunctionalCastExpr*>(e);
  return c->braced && c->args.size() == 1 && c->args[0]->kind == NodeKind::BraceInitExpr && !static_cast<BraceInitExpr*>(c->args[0])->text;
}

auto codegen::Emitter::arrayTemporary(Expr* braces, const Ty* array, Node* at, const std::string& into) -> std::string
{
  Expr* list = braces;
  if (braces->kind == NodeKind::FunctionalCastExpr)
  {
    // The type the braces name, the one the reference refers to (the checker matched them).
    const Ty* named = types.stripRef(braces->ty);
    if (named->count != array->count || llType(named, at) != llType(array, at))
      unsupported(at, "a " + types.toString(named, &symbols) + " bound to a reference to " + types.toString(array, &symbols));
    list = static_cast<FunctionalCastExpr*>(braces)->args[0];
  }
  if (array->count == 0)
    unsupported(at, "a temporary array of no length the compiler could evaluate (" + types.toString(array, &symbols) + ")");
  std::string addr = into.empty() ? newObjectSlot(array, at) : into;
  initializeFrom(addr, array, list, at);
  if (into.empty() && needsDestruction(array, at))
    statementTemps.push_back({addr, array});
  return addr;
}

auto codegen::Emitter::textTemporary(BraceInitExpr* b, Node* at) -> std::string
{
  const Ty* textTy = b->ty ? types.unqualified(types.stripRef(b->ty)) : nullptr;
  if (!textTy || !isClass(textTy))
    unsupported(at, "a Text without its type");
  const StructInfo& info = structFor(textTy, at);
  if (info.fieldDecls.size() < 2)
    unsupported(at, "a Text class without its two fields");
  uint32_t piecesField = fieldIndexOf(info, info.fieldDecls[0]);
  uint32_t countField = fieldIndexOf(info, info.fieldDecls[1]);
  const Ty* piecesTy = info.fieldTypes[piecesField];
  const Ty* pieceTy = piecesTy && piecesTy->kind == TyKind::Pointer ? types.unqualified(piecesTy->inner) : nullptr;
  if (!pieceTy || !isClass(pieceTy))
    unsupported(at, "a Text whose first field is no pointer to its pieces");
  const StructInfo& pieceInfo = structFor(pieceTy, at);
  if (pieceInfo.fieldDecls.size() < 2)
    unsupported(at, "a TextPiece class without its two fields");
  uint32_t n = 0;
  for (Expr* a : b->args)
    n += isPackExpansion(a) ? uint32_t(packElementCount(static_cast<UnaryExpr*>(a)->operand, at)) : 1;
  const Ty* arrayTy = types.array(pieceTy, n);
  std::string array = newObjectSlot(arrayTy, at);
  uint32_t i = 0;
  auto piece = [&](Expr* value)
  {
    const Ty* t = value->ty ? withCurrentArgs(value->ty) : nullptr;
    TextPieceKind kind = global.textPieceOf(value, t);
    if (!kind.thunk)
      unsupported(value, kind.error.empty() ? "a piece of a Text of no known type" : kind.error);
    // Its address: a string literal's chars, an lvalue's own object, a temporary otherwise.
    const Ty* object = kind.type ? types.qualified(kind.type, true, false) : nullptr;
    if (!object)
    {
      object = stringLiteralArray(value, t, types);
      if (!object)
        object = types.unqualified(types.stripRef(t)); // an array of chars
    }
    std::string address = argumentText(types.lref(object), value, at).substr(4); // ("ptr " ...)
    TemplateArgs fargs;
    if (kind.type && !kind.thunk->templateParams.empty())
      fargs.pairs.push_back({kind.thunk->templateParams[0], kind.type});
    std::string function = useFunction(kind.thunk, fargs, nullptr, at);
    std::string slot = newTemp();
    fn += "  " + slot + " = getelementptr inbounds " + llType(arrayTy, at) + ", ptr " + array + ", i64 0, i64 " + std::to_string(i++) + "\n";
    fn += "  store ptr " + address + ", ptr " + gep(pieceInfo.name, slot, fieldIndexOf(pieceInfo, pieceInfo.fieldDecls[0])) + "\n";
    fn += "  store ptr " + function + ", ptr " + gep(pieceInfo.name, slot, fieldIndexOf(pieceInfo, pieceInfo.fieldDecls[1])) + "\n";
  };
  for (Expr* a : b->args)
  {
    if (isPackExpansion(a))
    {
      Expr* pattern = static_cast<UnaryExpr*>(a)->operand;
      forEachPackElement(pattern, at, [&](size_t) { piece(pattern); });
    }
    else
      piece(a);
  }
  std::string text = newObjectSlot(textTy, at);
  fn += "  store ptr " + array + ", ptr " + gep(info.name, text, piecesField) + "\n";
  fn += "  store " + llType(info.fieldTypes[countField], at) + " " + std::to_string(n) + ", ptr " + gep(info.name, text, countField) + "\n";
  return text;
}

auto codegen::Emitter::InlineParts::built(const std::string& addr, const Ty* t) -> void
{
  if (!e.needsDestruction(t, at))
    return;
  Object o{addr, t};
  o.unwindOnly = true;
  add(o);
}

auto codegen::Emitter::InlineParts::add(const Object& o) -> void
{
  size_t at = start == SIZE_MAX ? e.statementTemps.size() : std::min(start + parts.size(), e.statementTemps.size());
  e.statementTemps.insert(e.statementTemps.begin() + ptrdiff_t(at), o);
  parts.push_back(o.addr);
}

auto codegen::Emitter::InlineParts::builtCounted(const std::string& addr, const Ty* arrayTy, const std::string& countSlot) -> void
{
  if (!e.needsDestruction(arrayTy, at))
    return;
  Object o{addr, arrayTy};
  o.unwindOnly = true;
  o.countSlot = countSlot;
  add(o);
}

codegen::Emitter::InlineParts::~InlineParts()
{
  if (parts.empty())
    return;
  e.statementTemps.erase(std::remove_if(e.statementTemps.begin(), e.statementTemps.end(),
                                        [&](const Object& o) { return o.unwindOnly && std::find(parts.begin(), parts.end(), o.addr) != parts.end(); }),
                         e.statementTemps.end());
}

auto codegen::Emitter::constructArray(const std::string& addr, const Ty* t, Node* at) -> void
{
  InlineParts parts(*this, at);
  auto constructAt = [&](const std::string& index)
  {
    std::string element = newTemp();
    fn += "  " + element + " = getelementptr inbounds " + llType(t, at) + ", ptr " + addr + ", i64 0, i64 " + index + "\n";
    if (isClass(t->inner))
      defaultConstruct(element, t->inner, at);
    else if (t->inner->kind == TyKind::Array)
      constructArray(element, t->inner, at);
    else
      storeZero(t->inner, element, at);
    return element;
  };
  if (t->count <= 8)
  {
    for (uint32_t i = 0; i < t->count; ++i)
      parts.built(constructAt(std::to_string(i)), t->inner);
    return;
  }
  std::string slot = "%v" + std::to_string(temp++);
  allocas += "  " + slot + " = alloca i64\n";
  fn += "  store i64 0, ptr " + slot + "\n";
  parts.builtCounted(addr, t, slot); // (the loop's count: the elements built before the one under way)
  std::string loopL = newLabel("array.loop"), bodyL = newLabel("array.body"), endL = newLabel("array.end");
  emitBranch(loopL);
  startBlock(loopL);
  std::string index = newTemp(), done = newTemp();
  fn += "  " + index + " = load i64, ptr " + slot + "\n";
  fn += "  " + done + " = icmp eq i64 " + index + ", " + std::to_string(t->count) + "\n";
  fn += "  br i1 " + done + ", label %" + endL + ", label %" + bodyL + "\n";
  startBlock(bodyL);
  constructAt(index);
  std::string next = newTemp();
  fn += "  " + next + " = add i64 " + index + ", 1\n";
  fn += "  store i64 " + next + ", ptr " + slot + "\n";
  emitBranch(loopL);
  startBlock(endL);
}

auto codegen::Emitter::findDestructor(ClassDecl* c) -> FunctionDecl*
{
  for (Decl* m : c->members)
    if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Destructor) && !(m->flags & DF_Default))
      return static_cast<FunctionDecl*>(m);
  return nullptr;
}

auto codegen::Emitter::hasUserConstructors(ClassDecl* c) -> bool
{
  for (Decl* m : c->members)
    if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Constructor) && !(m->flags & DF_Default))
      return true;
  return false;
}

auto codegen::Emitter::needsConstruction(const Ty* t, Node* at, int depth) -> bool
{
  t = types.stripRef(t);
  if (t->kind == TyKind::Array)
    return needsConstruction(t->inner, at, depth);
  if (!isClass(t) || !t->decl || depth > 16)
    return false;
  if (hasUserConstructors(classDef(t->decl, at)))
    return true;
  const StructInfo& info = structFor(t, at);
  if (info.polymorphic)
    return true;
  for (Decl* m : info.fieldDecls)
  {
    if (m->kind == NodeKind::VariableDecl && static_cast<VariableDecl*>(m)->init)
      return true;
    // (a storage unit of bit fields: any of its fields' default initializers, `unsigned b : 5 = 6;`
    // after `unsigned a : 3;`, which a global left zeroed)
    if (m->kind == NodeKind::VariableDecl && bitfieldIn(info, m))
      for (Decl* f : unitMembers(info, classDef(t->decl, at), m))
        if (f->kind == NodeKind::VariableDecl && static_cast<VariableDecl*>(f)->init)
          return true;
  }
  for (const Ty* ft : info.fieldTypes)
    if (needsConstruction(ft, at, depth + 1))
      return true;
  return false;
}

auto codegen::Emitter::needsDestruction(const Ty* t, Node* at) -> bool
{
  t = types.stripRef(t);
  if (t->kind == TyKind::Array)
    return needsDestruction(t->inner, at);
  if (isLambda(t))
  {
    for (auto& [index, ft] : closureValueFields(t, at))
      if (needsDestruction(ft, at))
        return true;
    return false;
  }
  if (!isClass(t))
    return false;
  noteLayout(t);
  if (auto mine = destructionCache.find(t); mine != destructionCache.end())
    return mine->second;
  if (bool known; sh.finalDestruction.find(t, known))
    return destructionCache.emplace(t, known).first->second;
  // Computed by this emitter without the shared lock: the answer is the class's destructor's and
  // its complete layout's fields' (a typed union's alternatives'), the same whichever emitter
  // computes it, so two may compute it at once and either publishes it. (Computed under the shared
  // lock, the emitters' first answers queued there at codegen's start: codegen-speed.md section 5.)
  const StructInfo& info = structFor(t, at); // (laid out first: structFor takes the locks it needs)
  if (!destructionPending.insert(t).second)
  {
    destructionGuessed = true; // reached again inside its own answer: false for now, and nothing final until the outermost answer
    return false;
  }
  struct Pending
  {
    ~Pending()
    {
      e.destructionPending.erase(t);
      if (e.destructionPending.empty())
        e.destructionGuessed = false;
    }

    Emitter& e;
    const Ty* t;
  } pending{*this, t};
  ClassDecl* c = classDef(t->decl, at);
  bool result = false;
  if (c->isTypedUnion)
  {
    // A typed union destroys whichever alternative it holds.
    for (size_t i = 0; i < c->alternatives.size() && !result; ++i)
      result = needsDestruction(alternativeType(c, t, i, at), at);
  }
  else
  {
    result = findDestructor(c) != nullptr;
    for (size_t i = 0; i < info.fieldTypes.size() && !result; ++i)
      result = !info.fieldTypes[i]->isReference() && needsDestruction(info.fieldTypes[i], at); // a reference member owns nothing
  }
  // Final when the layout is complete (this emitter's own copy still being built may not be yet)
  // and no class was reached again inside its own answer.
  if (info.complete && !destructionGuessed)
  {
    destructionCache.emplace(t, result);
    sh.finalDestruction.publish(t, result);
  }
  return result;
}

auto codegen::Emitter::destroy(const std::string& addr, const Ty* t, Node* at, bool dynamic) -> void
{
  t = types.stripRef(t);
  if (t->kind == TyKind::Array)
  {
    if (!needsDestruction(t->inner, at))
      return;
    if (destructionMayThrow(t->inner, at))
    {
      // (an element's destructor that throws leaves the elements before it to be destroyed)
      pushScope();
      for (uint32_t i = 0; i < t->count; ++i)
      {
        std::string e = newTemp();
        fn += "  " + e + " = getelementptr inbounds " + llType(t, at) + ", ptr " + addr + ", i64 0, i64 " + std::to_string(i) + "\n";
        scopes.back().push_back({e, t->inner});
      }
      popScope();
      return;
    }
    for (uint32_t i = t->count; i-- > 0;)
    {
      std::string e = newTemp();
      fn += "  " + e + " = getelementptr inbounds " + llType(t, at) + ", ptr " + addr + ", i64 0, i64 " + std::to_string(i) + "\n";
      destroy(e, t->inner, at);
    }
    return;
  }
  if (isLambda(t))
  {
    std::vector<std::pair<uint32_t, const Ty*>> fields = closureValueFields(t, at);
    std::string structName = closureFor(t, at).structName;
    if (destructionMayThrow(t, at))
    {
      pushScope();
      for (size_t i = 0; i < fields.size(); ++i)
        if (needsDestruction(fields[i].second, at))
          scopes.back().push_back({gep(structName, addr, fields[i].first), fields[i].second});
      popScope();
      return;
    }
    for (size_t i = fields.size(); i-- > 0;)
      if (needsDestruction(fields[i].second, at))
        destroy(gep(structName, addr, fields[i].first), fields[i].second, at);
    return;
  }
  if (!isClass(t))
    return;
  if (dynamic)
  {
    // A virtual destructor: the dynamic class's, also when this class has nothing to destroy
    // (`virtual ~Shape() = default`) and the derived one does.
    if (int slot = slotKeyed(structFor(t, at), "~"); slot >= 0)
    {
      std::string fp = loadSlot(addr, slot);
      callDestructor(fp, addr, destructionMayThrow(t, at));
      return;
    }
  }
  if (!needsDestruction(t, at))
    return;
  ClassDecl* c = classDef(t->decl, at);
  if (c->isTypedUnion)
  {
    destroyAlternative(c, t, addr, at);
    return;
  }
  if (FunctionDecl* dtor = findDestructor(c))
  {
    TemplateArgs args = global.classArgsOf(canonicalClass(t, at));
    callDestructor(useFunction(dtor, args, canonicalClass(t, at)), addr, destructionMayThrow(t, at));
    return;
  }
  // A class whose destruction spells out more than a few calls is destroyed through its
  // destructor helper (one definition, `@kpp.dtor.<class>`), not inline wherever one is
  // destroyed: a constructor's cleanups destroy every member built so far, and inline each
  // member's members (ControlSettings' constructor held 100,648 String destructor calls, 25.9 MB
  // of IR, 70% of the game's largest module). The helper's own body spells them out.
  if (t != destructorHelperFor && destructionCalls(t, at) > 8)
  {
    callDestructor(destructorThunk(t, at), addr, destructionMayThrow(t, at));
    return;
  }
  destroyMembers(addr, t, at);
}

auto codegen::Emitter::destructionMayThrow(const Ty* t, Node* at, int depth) -> bool
{
  t = types.stripRef(t);
  if (t->kind == TyKind::Array)
    return destructionMayThrow(t->inner, at, depth + 1);
  if (!(isClass(t) || isLambda(t)))
    return false;
  if (auto it = throwingDestructionCache.find(t); it != throwingDestructionCache.end())
    return it->second;
  bool result = false;
  if (isLambda(t))
  {
    for (const auto& [index, ft] : closureValueFields(t, at))
      result = result || destructionMayThrow(ft, at, depth + 1);
  }
  else
  {
    noteLayout(t);
    ClassDecl* c = classDef(t->decl, at);
    // (the resolver's rule, [except.spec] 8; it leaves to this walk a class with a closure among its parts)
    if (std::optional<bool> known = global.destructorMayThrow(canonicalClass(t, at)))
      result = *known;
    else if (depth > 64)
      fail(at, "the destruction of " + types.toString(t, &symbols) + ", nested too deeply to say whether it may throw");
    else if (c->isTypedUnion)
    {
      for (size_t i = 0; i < c->alternatives.size() && !result; ++i)
        result = destructionMayThrow(alternativeType(c, t, i, at), at, depth + 1);
    }
    else
      for (const Ty* ft : std::vector<const Ty*>(structFor(t, at).fieldTypes))
        result = result || (!ft->isReference() && destructionMayThrow(ft, at, depth + 1));
  }
  throwingDestructionCache.emplace(t, result);
  return result;
}

auto codegen::Emitter::callDestructor(const std::string& dtor, const std::string& object, bool mayThrow) -> void
{
  if (!mayThrow || funcletOverride.empty())
  {
    emitCall("", "void " + dtor + "(ptr " + object + ")", mayThrow);
    return;
  }
  markEH();
  if (declaredNames.claim("@abort"))
    decls += "declare void @abort()\n";
  {
    auto lock = sharedLock();
    if (!emittedShared["@kpp_destroy_unwinding"])
    {
      emittedShared["@kpp_destroy_unwinding"] = true;
      shared += sharedComdat("@kpp_destroy_unwinding") +
                "define linkonce_odr void @kpp_destroy_unwinding(ptr %dtor, ptr %object) comdat personality ptr @__C_specific_handler {\nentry:\n"
                "  invoke void %dtor(ptr %object) to label %done unwind label %terminate\n"
                "done:\n  call void asm sideeffect \"nop\", \"\"()\n  ret void\n"
                "terminate:\n  %pad = cleanuppad within none []\n  call void @abort() [ \"funclet\"(token %pad) ]\n  unreachable\n}\n";
    }
  }
  fn += "  call void @kpp_destroy_unwinding(ptr " + dtor + ", ptr " + object + ")" + bundle() + "\n";
}

auto codegen::Emitter::destructionCalls(const Ty* t, Node* at, int depth) -> size_t
{
  t = types.stripRef(t);
  noteLayout(t);
  if (auto it = destructionCallCounts.find(t); it != destructionCallCounts.end())
    return it->second;
  size_t n = 0;
  if (depth > 16)
    n = 1000;
  else if (t->kind == TyKind::Array)
    n = needsDestruction(t->inner, at) ? std::min<size_t>(1000, size_t(t->count) * destructionCalls(t->inner, at, depth + 1)) : 0;
  else if (isLambda(t))
  {
    for (const auto& [index, ft] : closureValueFields(t, at))
      if (needsDestruction(ft, at))
        n += destructionCalls(ft, at, depth + 1);
  }
  else if (isClass(t) && needsDestruction(t, at))
  {
    ClassDecl* c = classDef(t->decl, at);
    if (c->isTypedUnion || findDestructor(c))
      n = 1;
    else
    {
      const StructInfo& info = structFor(t, at);
      for (const Ty* ft : info.fieldTypes)
        if (!ft->isReference() && needsDestruction(ft, at))
          n += destructionCalls(ft, at, depth + 1);
    }
  }
  n = std::min<size_t>(n, 1000);
  destructionCallCounts.emplace(t, n);
  return n;
}

auto codegen::Emitter::destroyMembers(const std::string& addr, const Ty* t, Node* at) -> void
{
  StructInfo& info = structFor(t, at);
  std::string name = info.name;
  std::vector<Decl*> fieldDecls = info.fieldDecls;
  std::vector<const Ty*> fieldTypes = info.fieldTypes;
  std::unordered_map<Decl*, uint32_t> fieldIndex = info.fieldIndex;
  std::vector<std::pair<Decl*, uint32_t>> bases = info.bases;
  if (std::any_of(fieldTypes.begin(), fieldTypes.end(), [&](const Ty* ft) { return !ft->isReference() && destructionMayThrow(ft, at); }))
  {
    // `~S() noexcept = default;` over a member's that may throw: the program ends there, so the members
    // go in a noexcept frame of their own, the helper's, as a function of no body has none.
    if (needsDestruction(t, at) && !destructionMayThrow(t, at) && !emittingNoexcept())
    {
      callDestructor(destructorThunk(t, at), addr, false);
      return;
    }
    // A member's or base's destructor may throw: they are a scope of their own, built in order
    // (bases, then members), so that one throwing leaves those built before it to be destroyed.
    pushScope();
    for (auto& [base, index] : bases)
      if (needsDestruction(fieldTypes[index], at))
        scopes.back().push_back({gep(name, addr, index), fieldTypes[index]});
    for (Decl* d : fieldDecls)
      if (const Ty* ft = fieldTypes[fieldIndex[d]]; !ft->isReference() && needsDestruction(ft, at))
        scopes.back().push_back({gep(name, addr, fieldIndex[d]), ft});
    popScope();
    return;
  }
  for (size_t i = fieldDecls.size(); i-- > 0;)
  {
    const Ty* ft = fieldTypes[fieldIndex[fieldDecls[i]]];
    if (!ft->isReference() && needsDestruction(ft, at))
      destroy(gep(name, addr, fieldIndex[fieldDecls[i]]), ft, at);
  }
  for (size_t i = bases.size(); i-- > 0;)
  {
    const Ty* bt = fieldTypes[bases[i].second];
    if (needsDestruction(bt, at))
      destroy(gep(name, addr, bases[i].second), bt, at);
  }
}

auto codegen::Emitter::defaultConstruct(const std::string& addr, const Ty* t, Node* at) -> void
{
  ClassDecl* c = classDef(t->decl, at);
  if (c->isTypedUnion)
  {
    // `Property p;`: the first alternative, default-constructed.
    storeZero(t, addr, at);
    if (!c->alternatives.empty())
      constructAlternative(c, t, addr, 0, nullptr, at);
    return;
  }
  if (hasUserConstructors(c))
  {
    FunctionDecl* ctor = findConstructor(t, {}, at);
    if (!ctor && !inheritedConstructorBase(c))
      unsupported(at, "default construction of " + qualifiedName(c) + ", which has no default constructor");
    if (ctor && !(ctor->flags & DF_Default))
    {
      // Through callFunction: `Style(const Style* parent = nullptr)` wants its default argument.
      TemplateArgs args = global.classArgsOf(canonicalClass(t, at));
      if (!ctor->templateParams.empty())
        global.deduceArgs(ctor, {}, args); // `template<typename... Args> OnHeap(Args&&...)`: Args is empty
      callFunction(ctor, args, canonicalClass(t, at), addr, "", {}, at);
      return;
    }
    // `T() = default;` next to other constructors: the memberwise default construction below.
  }
  storeZero(t, addr, at);
  initializeMembers(addr, t, {}, at);
}

auto codegen::Emitter::initializerClass(const Ty* t, const FunctionDecl::CtorInit& init, Node* at, const TemplateArgs* args) -> const Ty*
{
  if (init.member.empty())
    return nullptr;
  const Ty* cls = canonicalClass(t, at);
  ClassDecl* c = classDef(cls->decl, at);
  NamedType named{};
  named.kind = NodeKind::NamedType;
  named.token = at ? at->token : 0;
  named.name = init.member;
  // Looked up from inside the class: from the constructor when it is the one being emitted.
  Decl* context = currentFunction && global.owningClass(currentFunction) && classDef(global.owningClass(currentFunction), at) == c ? static_cast<Decl*>(currentFunction) : c;
  const Ty* nt = global.typeOfAstIn(&named, context, args ? *args : global.classArgsOf(cls));
  return nt && (isClass(nt) || isLambda(nt)) && !types.isDependent(nt) ? canonicalClass(nt, at) : nullptr; // (a closure: `F(f)` with the base F a lambda's type)
}

auto codegen::Emitter::initializerNamesOther(const Ty* t, const FunctionDecl::CtorInit& init, const Ty* cls, Node* at) -> bool
{
  // Only a name with template arguments or a qualified one can name another class than its
  // last part spells: `Chain<Tail...>(...)`, `UnitTest::TestRunner(reporter)` in a TestRunner.
  if (init.member.empty() || (!init.member.last().hasTemplateArgs && init.member.parts.size() == 1))
    return false;
  const Ty* named = initializerClass(t, init, at);
  return named && named != canonicalClass(cls, at);
}

auto codegen::Emitter::initializerNamingBase(const Ty* t, const Ty* baseTy, Span<FunctionDecl::CtorInit> inits, Node* at) -> const FunctionDecl::CtorInit*
{
  const Ty* base = canonicalClass(baseTy, at);
  for (const FunctionDecl::CtorInit& init : inits)
    if (!init.isPack && initializerClass(t, init, at) == base) // (a pack expansion's elements: packInitializerOf)
      return &init;
  return nullptr;
}

auto codegen::Emitter::packInitializerOf(const Ty* t, const Ty* baseTy, Span<FunctionDecl::CtorInit> inits, size_t& element, Node* at) -> const FunctionDecl::CtorInit*
{
  const Ty* base = canonicalClass(baseTy, at);
  TemplateArgs args = global.classArgsOf(canonicalClass(t, at));
  size_t count = 0;
  for (auto& [d, bound] : args.pairs)
    if (bound && bound->kind == TyKind::Pack)
      count = std::max(count, size_t(bound->args.size()));
  for (const FunctionDecl::CtorInit& init : inits)
    if (init.isPack)
      for (size_t k = 0; k < count; ++k)
      {
        // The pattern's element k: each pack of the class bound to its element k (as the resolver
        // converts a pack expansion's pattern, convertType).
        TemplateArgs one = args;
        for (auto& [d, bound] : one.pairs)
          if (bound && bound->kind == TyKind::Pack && k < bound->args.size())
            bound = bound->args[k];
        if (initializerClass(t, init, at, &one) == base)
        {
          element = k;
          return &init;
        }
      }
  return nullptr;
}

auto codegen::Emitter::bindReferenceMember(const std::string& fieldAddr, const Ty* refTy, Expr* source, Node* at, bool listElement) -> void
{
  withSingleElement(source, at, [&](Expr* one)
  {
    std::string target;
    // A list of one element binds that element itself when it is of the referenced class or one derived
    // from it, or an array of the referenced array's type ([dcl.init.list] 3.9: `P p{{point}}`, `A
    // a{{arr}}`), never a copy; a scalar reference's list is its one value. Any other list makes the
    // temporary below.
    if (const Ty* bound = refTy ? types.stripRef(refTy) : nullptr; bound && one->kind == NodeKind::BraceInitExpr && !static_cast<BraceInitExpr*>(one)->text &&
                                                                   static_cast<BraceInitExpr*>(one)->args.size() == 1 && static_cast<BraceInitExpr*>(one)->designators.empty())
    {
      Expr* only = static_cast<BraceInitExpr*>(one)->args[0];
      const Ty* given = only->kind != NodeKind::BraceInitExpr && only->ty && only->ty->isKnown() ? withCurrentArgs(types.stripRef(only->ty)) : nullptr;
      if (given && types.isDependent(given))
        given = nullptr;
      const Ty* referenced = types.unqualified(bound);
      std::vector<std::pair<std::string, uint32_t>> path;
      if (bound->kind == TyKind::Array ? given && given->kind == TyKind::Array && given->count == bound->count && llType(given, at) == llType(bound, at)
          : isClass(referenced) ? given && isClass(types.unqualified(given)) &&
                                    (canonicalClass(types.unqualified(given), at) == canonicalClass(referenced, at) || basePath(types.unqualified(given), referenced, path, at))
                                : only->kind != NodeKind::BraceInitExpr)
        one = only;
    }
    // An element of a variable's brace list that makes a temporary: it lives as long as the
    // variable (initializeExtending), one of static storage's in static storage of its own.
    bool listed = extendingElements && std::find(extendingElements->begin(), extendingElements->end(), one) != extendingElements->end();
    bool extended = listed && !isLvalueExpression(one) && !(one->ty && one->ty->isReference());
    // A temporary made for the member: by braces (`Holder h{{1, 2}}` with `const Point& p`, `Holder
    // h{Two{T(1), T(2)}}` and `{T(1), T(2)}` with `const T (&r)[2]`: the list initializes it,
    // [dcl.init.list] 3.10, an array as for a reference to an array, arrayTemporary) or of the
    // referenced class from a value of no class (`Named n{5}` with `const Name& name`: copy-initialized
    // from it, [dcl.init.ref] 5.4); kept as long as the variable when it is an element of the
    // variable's lists, an object of static storage of its own for a static's; else (a temporary
    // Holder's: `show(Holder{Two{...}})`) destroyed with the statement. Only an aggregate's braces bind
    // one here. (Until 2026-11-08 "a temporary array ... other than for a reference to an array" and
    // "the expression kind BraceInitExpr", and a value of no class was bound itself, its bytes taken
    // for the class's, the constructor never run.)
    const Ty* valueTy = one->kind != NodeKind::BraceInitExpr && one->ty && one->ty->isKnown() ? withCurrentArgs(types.unqualified(types.stripRef(one->ty))) : nullptr;
    if (valueTy && types.isDependent(valueTy))
      valueTy = nullptr;
    bool braces = one->kind == NodeKind::BraceInitExpr && !static_cast<BraceInitExpr*>(one)->text;
    if (const Ty* bound = refTy ? types.stripRef(refTy) : nullptr;
        bound && (bound->kind == TyKind::Array ? isArrayBraces(one)
                                               : isClass(types.unqualified(bound)) && !types.isDependent(bound) &&
                                                   (braces || (valueTy && !isClass(valueTy) && !isLambda(valueTy) && valueTy->kind != TyKind::Function))))
    {
      std::string what = bound->kind == TyKind::Array ? "a temporary array"
                         : braces                     ? "a temporary made by braces"
                                                      : "a temporary " + types.toString(types.unqualified(bound), &symbols) + " made from a value of type " + types.toString(valueTy, &symbols);
      if (!listElement)
        fail(at, "the reference member of type " + types.toString(refTy, &symbols) + " bound to " + what +
                     " outside an aggregate's braces: in a constructor's member initializer C++ rejects it ([class.base.init] 11), and a default member initializer's temporary is not kept as long as the aggregate yet");
      const Ty* element = bound;
      while (element->kind == TyKind::Array)
        element = element->inner;
      if (refTy->kind != TyKind::RRef && !bound->isConst && !element->isConst)
        fail(at, "the reference member of type " + types.toString(refTy, &symbols) + " given " + what +
                     ": a non-const lvalue reference binds only an object, never a temporary made for it");
      const Ty* made = bound->kind == TyKind::Array ? bound : withCurrentArgs(types.unqualified(bound));
      if (listed && !extendingStatic.empty())
      {
        target = staticTemporary(made, at);
        if (made->kind == TyKind::Array)
          arrayTemporary(one, made, at, target);
        else
          initializeFrom(target, made, one, at, braces ? ConversionContext::Unknown : ConversionContext::Copy);
      }
      else if (made->kind == TyKind::Array)
        target = arrayTemporary(one, made, at);
      else
      {
        target = newObjectSlot(made, at);
        initializeFrom(target, made, one, at, braces ? ConversionContext::Unknown : ConversionContext::Copy);
        if (needsDestruction(made, at))
          statementTemps.push_back({target, made});
      }
      if (listed && extendingStatic.empty())
        extendedTemporaries.push_back(target);
      fn += "  store ptr " + target + ", ptr " + fieldAddr + "\n";
      return;
    }
    if (isClass(types.stripRef(one->ty)))
    {
      Value object;
      if (extended && !extendingStatic.empty())
      {
        const Ty* made = withCurrentArgs(types.unqualified(types.stripRef(one->ty)));
        std::string kept = staticTemporary(made, at);
        initializeFrom(kept, made, one, at);
        object = {kept, made};
      }
      else
        object = rvalue(one);
      target = object.text;
      if (extended && object.isTemporary)
        extendedTemporaries.push_back(object.text);
      const Ty* referenced = refTy ? types.unqualified(types.stripRef(refTy)) : nullptr;
      if (referenced && isClass(referenced) && !types.isDependent(referenced) && isClass(object.ty) &&
          canonicalClass(object.ty, at) != canonicalClass(referenced, at))
        target = convert(object, referenced, at).text; // to the base, or through a conversion operator
    }
    else
    {
      // A reference member of another scalar type than its initializer (`const double& r` given an
      // int): an error, as in C++, where another type's would be a temporary that dies with the
      // constructor ([class.base.init] 8); and an rvalue reference member given an lvalue of its
      // type. Until 2026-11-05 it bound the int's own bytes.
      const Ty* want = refTy ? types.unqualified(types.stripRef(refTy)) : nullptr;
      const Ty* have = one->ty && one->ty->isKnown() && !types.isDependent(one->ty) ? types.unqualified(types.stripRef(one->ty)) : nullptr;
      if (want && have && want->isKnown() && !types.isDependent(want) && !isAggregate(want) && !isAggregate(have) && have != want &&
          llType(have, at) != llType(want, at))
        fail(at, "the reference member of type " + types.toString(refTy, &symbols) + " given a value of type " + types.toString(have, &symbols) +
                     ": a reference member binds only an object of its type (another type's would be a temporary, which dies with the constructor)");
      if (refTy && refTy->kind == TyKind::RRef && have == want && isLvalueExpression(one))
        fail(at, "the reference member of type " + types.toString(refTy, &symbols) +
                     " given an lvalue of its type: an rvalue reference binds only a temporary or what move() gives, never the object itself");
      requireSameClosure(have, want, true, at); // (a `const decltype(l1)&` member given l2: an error, never l2 bound as l1)
      if (extended && !extendingStatic.empty() && want && !isAggregate(want))
      {
        // (a local's scalar temporary is the function's: it outlives the variable)
        target = staticTemporary(want, at);
        initializeFrom(target, want, one, at);
      }
      else
        target = lvalue(one);
    }
    fn += "  store ptr " + target + ", ptr " + fieldAddr + "\n";
  });
}

auto codegen::Emitter::storeVptrs(const std::string& addr, const StructInfo& info) -> void
{
  if (!info.polymorphic)
    return;
  useVtable(info);
  std::vector<std::string> where = vptrAddresses(addr, info);
  for (size_t i = 0; i < where.size(); ++i)
    fn += "  store ptr " + info.vtables[i].vptrValue + ", ptr " + where[i] + "\n";
}

auto codegen::Emitter::loadVptrs(const std::string& addr, const StructInfo& info) -> std::vector<std::string>
{
  std::vector<std::string> saved; // pairs: the value, then where it goes back
  if (!info.polymorphic)
    return saved;
  for (const std::string& at : vptrAddresses(addr, info))
  {
    saved.push_back(newTemp());
    fn += "  " + saved.back() + " = load ptr, ptr " + at + "\n";
    saved.push_back(at);
  }
  return saved;
}

auto codegen::Emitter::restoreVptrs(const std::vector<std::string>& saved) -> void
{
  for (size_t i = 0; i + 1 < saved.size(); i += 2)
    fn += "  store ptr " + saved[i] + ", ptr " + saved[i + 1] + "\n";
}

auto codegen::Emitter::vptrAddresses(const std::string& addr, const StructInfo& info) -> std::vector<std::string>
{
  std::vector<std::string> out;
  for (const Vtable& t : info.vtables)
  {
    std::string a = addr;
    const StructInfo* s = &info;
    for (uint32_t index : t.path)
    {
      a = gep(s->name, a, index);
      s = &structFor(s->fieldTypes[index], nullptr);
    }
    out.push_back(a);
  }
  return out;
}

auto codegen::Emitter::initializeMembers(const std::string& addr, const Ty* t, Span<FunctionDecl::CtorInit> inits, Node* at, bool built) -> void
{
  const StructInfo& info = structFor(t, at);
  struct ImplicitScope
  {
    ~ImplicitScope() { e.implicitConstructionOf = saved; }

    Emitter& e;
    Decl* saved;
  } implicitScope{*this, implicitConstructionOf};
  if (isClass(t) && t->decl)
    if (Decl* own = currentFunction && (currentFunction->flags & DF_Constructor) ? global.owningClass(currentFunction) : nullptr;
        !own || classDef(own, at) != classDef(t->decl, at))
      implicitConstructionOf = classDef(t->decl, at);
  // Each base and member made is destroyed if a later one's initializer throws ([except.ctor] 3):
  // an unwind-only cleanup. In a constructor's own body (built) its scope ends with the function.
  // Done inline (an implicit or defaulted constructor's work, no function of its own) the cleanups go
  // with the statement's temporaries, so that unwinding destroys them before a new-expression frees
  // the memory, and are dropped once the object is whole, which its owner destroys then. Until
  // 2026-11-05 the inline parts had none: `class Implicit { M a{3}; M b{fail(1)}; };` left a
  // undestroyed when b's initializer threw.
  InlineParts inlineParts(*this, at);
  auto builtPart = [&](const std::string& part, const Ty* pt)
  {
    if (!built)
      inlineParts.built(part, pt);
    else if (needsDestruction(pt, at))
    {
      Object o{part, pt};
      o.unwindOnly = true;
      scopes.back().push_back(o);
    }
  };
  auto initFor = [&](uint32_t symbol) -> const FunctionDecl::CtorInit*
  {
    for (const FunctionDecl::CtorInit& init : inits)
      if (!init.member.empty() && init.member.last().symbol == symbol)
        return &init;
    return nullptr;
  };
  if (isClass(t) && t->decl)
  {
    // `Frame(x) : Frame(x, "") {}`: a delegating constructor builds the whole object (not
    // `Chain<Tail...>(tail...)` in Chain<Head, Tail...>, which initializes its base).
    const QualifiedName& own = classDef(t->decl, at)->name;
    if (const FunctionDecl::CtorInit* self = own.empty() ? nullptr : initFor(own.last().symbol); self && !initializerNamesOther(t, *self, t, at))
    {
      constructInto(addr, t, self->args, at);
      builtPart(addr, t); // (whole now: if the body throws, its destructor runs, [except.ctor] 4)
      return;
    }
  }
  // Each base's and member's initializer is a full-expression of its own ([class.base.init] 7):
  // the temporaries it made die when it is done, not when every member is (ControlSettings'
  // constructor kept 980 temporaries alive to its end, and each member's cleanup destroyed all
  // the earlier ones again: 97,512 destructor calls in 96,730 cleanup pads).
  for (auto& [baseDef, index] : info.bases)
  {
    size_t partTemps = statementTemps.size();
    const Ty* bt = info.fieldTypes[index];
    std::string baseAddr = gep(info.name, addr, index);
    const QualifiedName& bn = static_cast<ClassDecl*>(baseDef)->name;
    const FunctionDecl::CtorInit* init = bn.empty() ? nullptr : initFor(bn.last().symbol);
    if (init && (init->isPack || initializerNamesOther(t, *init, bt, at)))
      init = nullptr; // `Holder<0, H>(a)` met first for the base Holder<1, T>: by the type each names
    if (!init)
      init = initializerNamingBase(t, bt, inits, at);
    size_t element = 0;
    const FunctionDecl::CtorInit* packInit = init ? nullptr : packInitializerOf(t, bt, inits, element, at);
    if (packInit)
    {
      // `Many(A... a) : Leaf<A>(a)... {}`: the base of element k from the pattern with element k of
      // the packs it names (until 2026-11-07 a pack of bases was laid out only for `Ts...`).
      withPackElement(packInit->args, element, at, [&]
      {
        if (isLambda(bt) && packInit->args.size() == 1)
          initializeFrom(baseAddr, bt, packInit->args[0], at); // `Ts(ts)...` of closures
        else
          constructInto(baseAddr, bt, packInit->args, at);
      });
    }
    else if (init && isLambda(bt) && init->args.size() == 1)
      withSingleElement(init->args[0], at, [&](Expr* one) { initializeFrom(baseAddr, bt, one, at); }); // `F(f)`: the closure's values, as a member's
    else if (init)
      constructInto(baseAddr, bt, init->args, at);
    else if (hasUserConstructors(static_cast<ClassDecl*>(baseDef)))
      defaultConstruct(baseAddr, bt, at);
    else
      initializeMembers(baseAddr, bt, {}, at);
    destroyTempsFrom(partTemps);
    builtPart(baseAddr, bt);
  }
  storeVptrs(addr, info);
  if (info.isUnion)
  {
    // A union with no member named by a constructor initializer: its member with a default
    // member initializer is the one initialized, as in C++ (`union { int64_t raw = 0; ... }`).
    bool named = false;
    for (Decl* d : info.fieldDecls)
      if (d->kind == NodeKind::VariableDecl && !static_cast<VariableDecl*>(d)->name.empty())
        named = named || initFor(static_cast<VariableDecl*>(d)->name.last().symbol);
    // The member a constructor initializer names, at the union's start (UpgradableEntity's
    // `WrappedValue(EntityWithHealth& wrapped) : wrappedEntityWithHealth(&wrapped) {}`).
    if (named)
      for (Decl* d : info.fieldDecls)
        if (d->kind == NodeKind::VariableDecl && !static_cast<VariableDecl*>(d)->name.empty())
          if (const FunctionDecl::CtorInit* init = initFor(static_cast<VariableDecl*>(d)->name.last().symbol))
          {
            const Ty* ft = unionMemberType(t, d, at);
            if (isClass(ft))
              constructInto(addr, ft, init->args, at);
            else if (init->args.empty())
              storeZero(ft, addr, at); // `: raw()`: value-initialized
            else if (init->args.size() == 1)
              withSingleElement(init->args[0], at, [&](Expr* one) { initializeFrom(addr, ft, one, at, scalarContext(ft, true)); });
            else
              unsupported(at, "a union member initializer of several values");
            break;
          }
    if (!named)
      for (Decl* d : info.fieldDecls)
        if (d->kind == NodeKind::VariableDecl && static_cast<VariableDecl*>(d)->init && !bitfieldIn(info, d))
        {
          auto* v = static_cast<VariableDecl*>(d);
          const Ty* ft = unionMemberType(t, d, at);
          if (const Ty* declared = global.typeOfDecl(v); !ft->isReference() && !(declared && declared->isReference()))
            withMemberInitializer(addr, t, v, [&](Expr* value) { initializeFrom(addr, ft, value, v, scalarContext(ft, v->bracedInit)); });
          break;
        }
    return;
  }
  std::set<size_t> zeroedUnits; // the storage units of bit fields, each zeroed at its first member
  for (Decl* d : info.fieldDecls)
  {
    if (d->kind == NodeKind::ClassDecl)
    {
      // An anonymous struct or union member (`union { SharedString sharedStr; LongStr longStr;
      // ... }` with `: longStr{nullptr, 0}`): its members are initialised as the class's own,
      // by the same constructor initializers.
      initializeAnonymous(gep(info.name, addr, fieldIndexOf(info, d)), info.fieldTypes[fieldIndexOf(info, d)], static_cast<ClassDecl*>(d),
                          inits, at);
      continue;
    }
    if (d->kind != NodeKind::VariableDecl)
      continue;
    auto* v = static_cast<VariableDecl*>(d);
    size_t partTemps = statementTemps.size();
    const Ty* ft = info.fieldTypes[fieldIndexOf(info, d)];
    std::string fieldAddr = gep(info.name, addr, fieldIndexOf(info, d));
    const FunctionDecl::CtorInit* init = v->name.empty() ? nullptr : initFor(v->name.last().symbol);
    if (bitfieldIn(info, d))
    {
      // The unit's first bit field stands for the unit: each bit field in it is initialised.
      for (Decl* m : unitMembers(info, classDef(t->decl, at), d))
      {
        auto* bv = static_cast<VariableDecl*>(m);
        const FunctionDecl::CtorInit* bitInit = bv->name.empty() ? nullptr : initFor(bv->name.last().symbol);
        if (bitInit && bitInit->args.size() > 1)
          unsupported(at, "this member initializer");
        initializeBitfield(addr, t, info, bv, *bitfieldIn(info, m), bitInit != nullptr, bitInit && bitInit->args.size() == 1 ? bitInit->args[0] : nullptr,
                           zeroedUnits, at);
      }
      continue;
    }
    if (const Ty* declared = global.typeOfDecl(v); ft->isReference() || (declared && declared->isReference()))
    {
      // A reference member: the field holds the address of what it refers to.
      if (!init && v->init)
      {
        withMemberInitializer(addr, t, v, [&](Expr* source) { bindReferenceMember(fieldAddr, ft->isReference() ? ft : declared, source, at); });
        continue;
      }
      Expr* source = init && init->args.size() == 1 ? init->args[0] : nullptr;
      if (!source)
        unsupported(at, "a reference member without an initializer");
      bindReferenceMember(fieldAddr, ft->isReference() ? ft : declared, source, at);
      continue;
    }
    if (init)
    {
      if (isClass(ft))
        constructInto(fieldAddr, ft, init->args, at);
      else if (isLambda(ft) && init->args.size() == 1)
        withSingleElement(init->args[0], at, [&](Expr* one) { initializeFrom(fieldAddr, ft, one, at); }); // `f(f)`: the closure's bytes, not its address
      else if (size_t values = expandedCount(init->args, at); values == 0)
        storeZero(ft, fieldAddr, at); // `value()`, `value(forward<Args>(args)...)` with Args empty: value-initialized
      else if (ft->kind == TyKind::Array)
      {
        // `v{m...}`, and `values(4, 5)` (C++20: each value one element, the checker refused another),
        // the rest value-initialized.
        storeZero(ft, fieldAddr, at); // (what the values leave out)
        initializeArrayElements(fieldAddr, ft, init->args, at);
      }
      else if (values == 1)
        forEachExpanded(init->args, at, [&](Expr* one)
        {
          // A member initializer is direct-initialization: a class value converts through an
          // explicit conversion operator too (Pair's `second(forward<U2>(second))` with
          // ParsedNoiseExpression's `explicit operator const NoiseExpression*()`).
          Value value = rvalue(one);
          if (isClass(value.ty) && !isClass(ft) && ft->kind != TyKind::Lambda && !ft->isReference())
            value = convert(value, ft, at, true);
          store(value, fieldAddr, ft, at);
        }); // `value(first, args...)`, args empty
      else
        unsupported(at, "this member initializer");
    }
    else if (v->init)
      withMemberInitializer(addr, t, v, [&](Expr* value) { initializeFrom(fieldAddr, ft, value, v, scalarContext(ft, v->bracedInit)); });
    else if (isClass(ft) && hasUserConstructors(classDef(ft->decl, at)))
      defaultConstruct(fieldAddr, ft, at);
    else if (isClass(ft))
      initializeMembers(fieldAddr, ft, {}, at);
    else if (ft->kind == TyKind::Array && (needsDestruction(ft, at) || needsConstruction(ft, at))) // elements with constructors or default member initializers
      constructArray(fieldAddr, ft, at);
    else
      storeZero(ft, fieldAddr, at);
    destroyTempsFrom(partTemps);
    builtPart(fieldAddr, ft);
  }
}

auto codegen::Emitter::namesMemberOf(ClassDecl* c, Span<FunctionDecl::CtorInit> inits) -> bool
{
  for (Decl* m : c->members)
  {
    if (m->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(m)->name.empty() && static_cast<ClassDecl*>(m)->isDefinition)
    {
      if (namesMemberOf(static_cast<ClassDecl*>(m), inits))
        return true;
      continue;
    }
    if (m->kind != NodeKind::VariableDecl || (m->flags & (DF_Static | DF_Typedef)) || static_cast<VariableDecl*>(m)->name.empty())
      continue;
    for (const FunctionDecl::CtorInit& init : inits)
      if (!init.member.empty() && init.member.last().symbol == static_cast<VariableDecl*>(m)->name.last().symbol)
        return true;
  }
  return false;
}

auto codegen::Emitter::initializeAnonymous(const std::string& addr, const Ty* t, ClassDecl* c, Span<FunctionDecl::CtorInit> inits, Node* at) -> void
{
  if (!structFor(t, at).isUnion)
  {
    initializeMembers(addr, t, inits, at);
    return;
  }
  storeZero(t, addr, at);
  Decl* chosen = nullptr;
  for (Decl* m : c->members)
    if (((m->kind == NodeKind::VariableDecl && !(m->flags & (DF_Static | DF_Typedef))) ||
         (m->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(m)->name.empty() && static_cast<ClassDecl*>(m)->isDefinition)) &&
        !chosen)
    {
      if (m->kind == NodeKind::ClassDecl ? namesMemberOf(static_cast<ClassDecl*>(m), inits) : false)
        chosen = m;
      else if (m->kind == NodeKind::VariableDecl && !static_cast<VariableDecl*>(m)->name.empty())
        for (const FunctionDecl::CtorInit& init : inits)
          if (!init.member.empty() && init.member.last().symbol == static_cast<VariableDecl*>(m)->name.last().symbol)
            chosen = m;
    }
  if (!chosen)
    for (Decl* m : c->members)
      if (m->kind == NodeKind::VariableDecl && !(m->flags & (DF_Static | DF_Typedef)) && static_cast<VariableDecl*>(m)->init && !chosen)
        chosen = m;
  if (!chosen)
    return;
  const Ty* mt = unionMemberType(t, chosen, at);
  if (chosen->kind == NodeKind::ClassDecl)
  {
    initializeAnonymous(addr, mt, static_cast<ClassDecl*>(chosen), inits, at); // a struct inside the union, at its start
    return;
  }
  auto* v = static_cast<VariableDecl*>(chosen);
  if (v->bitfield) // (a union's layout has no bit-field places: it would be its whole member)
    unsupported(at, "a bit field of an anonymous union initialised by a constructor");
  const FunctionDecl::CtorInit* init = nullptr;
  for (const FunctionDecl::CtorInit& i : inits)
    if (!i.member.empty() && i.member.last().symbol == v->name.last().symbol)
      init = &i;
  if (const Ty* declared = global.typeOfDecl(v); mt->isReference() || (declared && declared->isReference()))
    unsupported(at, "a reference member of an anonymous union");
  if (!init)
  {
    withMemberInitializer(addr, t, v, [&](Expr* value) { initializeFrom(addr, mt, value, v, scalarContext(mt, v->bracedInit)); });
    return;
  }
  if (isClass(mt))
    constructInto(addr, mt, init->args, at);
  else if (size_t values = expandedCount(init->args, at); values == 0)
    storeZero(mt, addr, at);
  else if (values == 1)
    forEachExpanded(init->args, at, [&](Expr* one) { initializeFrom(addr, mt, one, at, scalarContext(mt, true)); });
  else
    unsupported(at, "this member initializer");
}

auto codegen::Emitter::unionMemberType(const Ty* t, Decl* m, Node* at) -> const Ty*
{
  const Ty* key = canonicalClass(t, at);
  if (m->kind == NodeKind::VariableDecl)
    return global.typeOfDeclIn(m, global.classArgsOf(key));
  return types.classType(m, key->args);
}

auto codegen::Emitter::listConvertsTo(FunctionDecl* ctor, BraceInitExpr* list, const Ty* t, Node* at) -> bool
{
  TemplateArgs classArgs = global.classArgsOf(canonicalClass(t, at));
  ArgsScope scope(*this, &classArgs);
  const Ty* pt = declType(ctor->params[0]);
  pt = pt ? types.unqualified(types.stripRef(pt)) : nullptr;
  if (!pt || pt->args.empty())
    return true;
  const Ty* element = pt->args[0];
  for (Expr* a : list->args)
  {
    // A list as an element (`Vector<ID> ids{{ID(1), ID(2), ID(3)}}`): one only when the element
    // type is list-initialized from it; otherwise the list is the InitializerList itself, the
    // argument of the second phase.
    if (a && a->kind == NodeKind::BraceInitExpr && !types.isDependent(element))
      if (const Ty* inner = braceListType(a, global.initializerListDecl(), types); inner && !global.converts(inner, element))
        return false;
    if (!a || a->kind == NodeKind::BraceInitExpr || isPackExpansion(a) || !a->ty || !a->ty->isKnown())
      continue;
    const Ty* at = withCurrentArgs(a->ty);
    if (types.isDependent(at) || types.isDependent(element))
      continue;
    if (!global.converts(argumentType(a, at, types), element))
      return false;
  }
  return true;
}

auto codegen::Emitter::initializerListConstructor(ClassDecl* c, const Ty* t, Node* at) -> FunctionDecl*
{
  TemplateArgs classArgs = global.classArgsOf(canonicalClass(t, at));
  ArgsScope scope(*this, &classArgs);
  for (Decl* m : c->members)
  {
    if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Constructor) || (m->flags & DF_Delete))
      continue;
    auto* f = static_cast<FunctionDecl*>(m);
    if (f->params.empty() || !f->templateParams.empty())
      continue;
    bool othersDefaulted = true;
    for (size_t i = 1; i < f->params.size(); ++i)
      othersDefaulted = othersDefaulted && static_cast<ParamDecl*>(f->params[i])->defaultValue != nullptr;
    if (!othersDefaulted)
      continue;
    const Ty* pt = declType(f->params[0]);
    if (pt && isInitializerListType(types.stripRef(pt)))
      return f;
  }
  return nullptr;
}

auto codegen::Emitter::directInitialize(const std::string& addr, const Ty* t, Span<Expr*> args, bool braced, Node* at, bool listElements) -> void
{
  if (braced && isInitializerListType(types.unqualified(t)))
  {
    // `InitializerList<TestCase>{{...}, {...}}` (a range-for's range in the tests): the list
    // over its elements' array, which lives as long as the object made here does.
    if (!listElements && args.size() == 1 && args[0] && args[0]->kind == NodeKind::BraceInitExpr && !static_cast<BraceInitExpr*>(args[0])->text)
    {
      initializerListTemporary(types.unqualified(t), static_cast<BraceInitExpr*>(args[0]), at, addr);
      return;
    }
    BraceInitExpr list{};
    list.kind = NodeKind::BraceInitExpr;
    list.token = at ? at->token : 0;
    list.args = args;
    initializerListTemporary(types.unqualified(t), &list, at, addr);
    return;
  }
  Expr* arg = args.size() == 1 && !braced ? args[0] : nullptr;
  const Ty* argTy = arg && arg->ty && arg->ty->isKnown() ? withCurrentArgs(types.unqualified(types.stripRef(arg->ty))) : nullptr;
  if (argTy && isClass(argTy) && !types.isDependent(argTy) && classKey(argTy) != classKey(t) && !baseInstantiation(argTy, classDef(t->decl, at), at) &&
      !constructorFor(t, args, at) && !global.converts(argumentType(arg, arg->ty, types), types.unqualified(t)))
  {
    // The operator gives the class (`operator AchievementId()`) or a reference to it (`operator
    // const Constant&()`): either way the copy or move constructor's argument.
    const Ty* value = types.unqualified(t);
    const Ty* reference = types.lref(types.qualified(value, true, false));
    const Ty* target = conversionOperator(argTy, value, at, nullptr, true) ? value : conversionOperator(argTy, reference, at, nullptr, true) ? reference : nullptr;
    // (one giving the class itself, a prvalue, builds the object where it is: constructInto, C++17's guaranteed
    // elision; until 2026-11-08 `A x(kb);` with `explicit operator A() const` moved a temporary into x)
    if (target && target == value && returnsInMemory(value))
    {
      constructInto(addr, t, args, at, listElements);
      return;
    }
    if (target)
    {
      Value converted = convert(rvalue(arg), target, at, true);
      if (converted.isTemporary)
        moveTemporary(addr, converted, value, at);
      else
        copyConstruct(addr, converted.text, value, at);
      return;
    }
  }
  constructInto(addr, t, args, at, listElements);
}

auto codegen::Emitter::constructInto(const std::string& addr, const Ty* t, Span<Expr*> args, Node* at, bool listElements, bool copyInit) -> void
{
  ClassDecl* c = classDef(t->decl, at);
  if (args.empty())
  {
    defaultConstruct(addr, t, at);
    return;
  }
  std::optional<ConstructingScope> guard;
  if (args.size() == 1)
  {
    const Ty* key = canonicalClass(t, at);
    for (auto& [k, a] : constructing)
      if (k == key && a == args[0])
        unsupported(at, "converting " + (args[0]->ty ? types.toString(args[0]->ty, &symbols) : std::string("a value")) + " to " +
                            types.toString(key, &symbols) + ": the constructor chosen needs the same conversion for its own parameter");
    guard.emplace(constructing, key, args[0]);
  }
  // (`return {{a, b}};` for an Optional<Pair<int, int>>: the inner list, one element of the
  // outer one, is an argument of a constructor, not the class's own list again; nor is the
  // one value of `{.effectTexture = {bitmap}}`, which is the designated member's; nor is an
  // aggregate's one element (`H h{{{2}, {3}}}`: its first member's list, as C++ has it; D,
  // 2026-11-05: it was H's own list again, `{3}` went to H's second member, and `H a{{{2}}}`
  // was unwrapped down to `2`).)
  if (args.size() == 1 && args[0]->kind == NodeKind::BraceInitExpr && !static_cast<BraceInitExpr*>(args[0])->text && !(listElements && hasUserConstructors(c)) &&
      !(listElements && !pendingDesignators.empty()) && !(listElements && !hasUserConstructors(c) && !c->isTypedUnion))
  {
    // `T x{y}` with y a T, or of a class derived from T: a copy or a move of y, before any
    // initializer-list constructor (C++ DR 1467): `[names{::move(names)}]` moves the Vector.
    auto* list = static_cast<BraceInitExpr*>(args[0]);
    if (list->args.size() == 1 && list->designators.empty() && list->args[0]->kind != NodeKind::BraceInitExpr && list->args[0]->ty)
    {
      const Ty* et = withCurrentArgs(types.stripRef(list->args[0]->ty));
      std::vector<std::pair<std::string, uint32_t>> path;
      if (et && isClass(et) && !types.isDependent(et) &&
          (canonicalClass(et, at) == canonicalClass(t, at) || basePath(et, t, path, at)))
      {
        constructInto(addr, t, list->args, at);
        return;
      }
    }
    // `Vector<Item> items = {}` of a class with a default constructor: value-initialisation (C++
    // [dcl.init.list] 3.5), not the InitializerList constructor, whose body copies elements (a
    // Vector of a move-only class: CommandProcessor's `Vector<SavedSpecialItemReference> ... = {}`).
    bool valueInitialised = list->args.empty() && findConstructor(t, {}, at);
    if (FunctionDecl* ctor = valueInitialised ? nullptr : initializerListConstructor(c, t, at); ctor && listConvertsTo(ctor, list, t, at))
    {
      // `Vector<int> v = {1, 2, 3}`, `Poly p({1, 2, 3, 4})`: the list as one InitializerList argument.
      TemplateArgs classArgs = global.classArgsOf(canonicalClass(t, at));
      callFunction(ctor, classArgs, canonicalClass(t, at), addr, "", args, at);
      return;
    }
    pendingDesignators = static_cast<BraceInitExpr*>(args[0])->designators;
    constructInto(addr, t, static_cast<BraceInitExpr*>(args[0])->args, at, true);
    return;
  }
  Span<uint32_t> designators = pendingDesignators;
  pendingDesignators = {};
  // `T a(T{})`, `T b(make())`, `new T(T(5))`, a member initializer `t(T())`, `T c{T{}}`: a prvalue of
  // the class itself makes the object in place, with no copy or move (C++17 [dcl.init] 17.6.1), as
  // `T d = T{};` does (initializeFrom builds a T(...) and a call's result in the object itself).
  // Until 2026-11-05 the object was moved from a temporary, a silent wrong value where the move
  // constructor does more than move (thread D's spec language/direct_initialization_from_prvalue_is_in_place).
  if (args.size() == 1 && args[0] && args[0]->kind != NodeKind::BraceInitExpr && !isPackExpansion(args[0]) && args[0]->ty &&
      !args[0]->ty->isReference() && !isLvalueExpression(args[0]))
    if (const Ty* from = withCurrentArgs(types.unqualified(args[0]->ty));
        from && isClass(from) && !types.isDependent(from) && canonicalClass(from, at) == canonicalClass(t, at))
    {
      initializeFrom(addr, t, args[0], at);
      return;
    }
  // `U(U::Text, x)`, `U u(U::Where, 1, 2)`, `return U(U::Comment, s)`: a tag first names the
  // alternative, the rest build it (its value, or its constructor's arguments), whichever others
  // share its type, and nothing else is built first (design.md 8.5, decided by kovarex 2026-10-31).
  if (c->isTypedUnion && !args.empty() && args[0]->ty && types.unqualified(types.stripRef(args[0]->ty)) == tagType(c))
  {
    std::optional<int64_t> index = constantTag(c, args[0]);
    if (!index || *index < 0 || size_t(*index) >= c->alternatives.size())
      fail(args[0], "the tag constructing the typed union " + qualifiedName(c) + " is no constant one of its alternatives");
    storeZero(t, addr, at);
    if (args.size() <= 2)
      constructAlternative(c, t, addr, size_t(*index), args.size() == 2 ? args[1] : nullptr, at);
    else
    {
      setTag(c, t, addr, c->alternatives.size(), at); // (holding none meanwhile: constructAlternative)
      constructInto(alternativeAddress(c, t, addr, size_t(*index), at), alternativeType(c, t, size_t(*index), at),
                    Span<Expr*>{args.data + 1, uint32_t(args.size() - 1)}, at);
      setTag(c, t, addr, size_t(*index), at);
    }
    return;
  }
  if (c->isTypedUnion && args.size() == 1 && args[0]->ty && args[0]->ty->isKnown() && !isPackExpansion(args[0])) // an expansion: its element, below
  {
    // `Shape a(s)`, `Shape(7)`, a member initializer `shape(s)`, `new (slot) Shape(x)`: as
    // copy-initialization, a value of the union itself copies or moves it, any other the
    // alternative of its type (design.md 8.5).
    const Ty* argTy = types.stripRef(args[0]->ty);
    if (!(isClass(argTy) && classKey(argTy) == classKey(t)))
    {
      storeZero(t, addr, at);
      constructAlternative(c, t, addr, alternativeForValue(c, t, args[0]->ty, at), args[0], at);
      return;
    }
    initializeFrom(addr, t, args[0], at);
    return;
  }
  // `using Base::Base;`: the base's constructors, when none of the class's own takes the
  // arguments (a copy stays a copy).
  bool copyLike = args.size() == 1 && args[0]->ty && isClass(types.stripRef(args[0]->ty)) && classKey(types.stripRef(args[0]->ty)) == classKey(t);
  // (A value whose class converts to t itself takes that conversion: `return connector;` for a
  // SignalID through SignalIDConnector's `operator SignalID()`, not a base's constructors.)
  // (the value as it is, an lvalue or an rvalue: `operator T() &&` converts a temporary only)
  auto valueObject = [&](Expr* e) {
    const Ty* a = argumentType(e, e->ty, types);
    return a && !a->isReference() && isClass(a) ? types.rref(a) : a;
  };
  bool converts = args.size() == 1 && !copyLike && args[0]->ty && isClass(types.stripRef(args[0]->ty)) &&
                  !types.isDependent(types.stripRef(args[0]->ty)) && conversionOperator(valueObject(args[0]), t, at, nullptr, !copyInit);
  // (a direct-initialization's an explicit one too, `A x(b);` with `explicit operator A()`, as clang and g++ take it;
  // a list's as well, as clang does, where g++ takes none for `A x = {b};`)
  if (!copyLike && !converts && (!hasUserConstructors(c) || !constructorFor(t, args, at, copyInit)))
    if (std::optional<QualifiedName> base = inheritedConstructorBase(c))
    {
      // Inherited constructors: the arguments build that base, everything else is default.
      FunctionDecl::CtorInit init{};
      init.member = *base;
      init.args = args;
      initializeMembers(addr, t, Span<FunctionDecl::CtorInit>{&init, 1}, at);
      return;
    }
  if (args.size() == 1 && isPackExpansion(args[0]))
  {
    // `T(forward<Args>(args)...)`: the elements are the arguments. With one element the
    // constructor choice (copy, move, converting) sees that element's type; several elements
    // of a class without constructors fill its fields in order; otherwise the constructor
    // call below expands the pack itself.
    Expr* pattern = static_cast<UnaryExpr*>(args[0])->operand;
    size_t count = packElementCount(pattern, at);
    if (count == 0)
    {
      defaultConstruct(addr, t, at);
      return;
    }
    if (count == 1)
    {
      forEachPackElement(pattern, at, [&](size_t)
      {
        Expr* one = pattern;
        constructInto(addr, t, Span<Expr*>{&one, 1}, at);
      });
      return;
    }
    if (!hasUserConstructors(c))
    {
      // An aggregate: the elements go to its parts as a brace list's values do, the bases first,
      // then the members, each named bit field on its own (aggregateMembers), a reference member
      // bound; the parts left over are value-initialized, as from `{}`. Until 2026-11-06 each
      // field of the layout took one element: `S{v...}` over `unsigned a : 3; unsigned b : 5; int
      // c;` put 2 into c where C++ puts it into b (one storage unit is one field), a member left
      // over stayed zero (its default member initializer and constructor never ran), and a
      // reference member held the value instead of referring to it (B's find).
      const StructInfo& info = structFor(t, at);
      if (info.isUnion)
        fail(at, "a pack of " + std::to_string(count) + " values for the union " + qualifiedName(c) + ", which holds one");
      std::vector<Decl*> members = aggregateMembers(info, c);
      if (count > info.bases.size() + members.size())
        unsupported(at, "more constructor arguments than " + qualifiedName(c) + " has fields");
      storeZero(t, addr, at);
      storeVptrs(addr, info);
      InlineParts parts(*this, at);
      std::set<size_t> zeroedUnits; // the storage units of bit fields, each zeroed at its first member
      size_t next = 0;
      // (A part whose braces C++ would elide, a number for an aggregate member, takes several of
      // the elements: not done here, an error.)
      auto whole = [&](const Ty* partTy, Expr* value)
      {
        if (value && !initializesWhole(partTy, value, at))
          unsupported(at, "the elements of a pack with the braces of a part of " + qualifiedName(c) + " elided (" +
                              types.toString(types.stripRef(partTy), &symbols) + " from a value of another type)");
      };
      auto initializePart = [&](size_t i, Expr* value)
      {
        if (i < info.bases.size())
        {
          auto& [baseDef, baseFieldIdx] = info.bases[i];
          std::string baseAddr = gep(info.name, addr, baseFieldIdx);
          const Ty* bt = info.fieldTypes[baseFieldIdx];
          whole(bt, value);
          if (value)
            initializeFrom(baseAddr, bt, value, at);
          else if (needsConstruction(bt, at))
            defaultConstruct(baseAddr, bt, at);
          else
            return;
          parts.built(baseAddr, bt);
          return;
        }
        if (i == info.bases.size() && !info.bases.empty())
          storeVptrs(addr, info); // the bases built pointed it at theirs
        whole(aggregateMemberType(t, info, members[i - info.bases.size()], at), value);
        initializeAggregateMember(addr, t, info, members[i - info.bases.size()], value, zeroedUnits, parts, at);
      };
      forEachPackElement(pattern, at, [&](size_t) { initializePart(next++, pattern); });
      for (; next < info.bases.size() + members.size(); ++next)
        initializePart(next, nullptr);
      if (members.empty() && !info.bases.empty())
        storeVptrs(addr, info);
      return;
    }
  }
  const Ty* firstTy = args.size() == 1 ? args[0]->ty : nullptr;
  // (A copy-initialization's: no explicit one, `B4 x = 11LL;` takes `implicit B4(int)` over `B4(bool)`;
  // until 2026-11-08 it took the best of all, which the checker had refused.)
  FunctionDecl* ctor = constructorFor(t, args, at, copyInit);
  if (ctor && firstTy && isClass(types.stripRef(firstTy)) && classKey(firstTy) == classKey(t) && !ctor->params.empty())
  {
    // The same class: a copy, unless the chosen constructor is a copy constructor itself (a
    // converting one, deleted or not, reached through a conversion of the class, loses to
    // the implicit copy: `Ref(const int&&) = delete` beside Ref's `operator const int&()`;
    // `ConvertingElement(const Tree&, uint32_t index = 0)` beside Tree's converting template).
    TemplateArgs classArgs = global.classArgsOf(canonicalClass(t, at));
    if (!ctor->templateParams.empty())
      global.deduceArgs(ctor, argTypesOf(args), classArgs); // `template<typename... Args> Str(Args&&...)` as Str(Str&): as deduced
    const Ty* pt;
    {
      ArgsScope scope(*this, &classArgs);
      pt = declType(ctor->params[0]);
    }
    if (pt && pt->kind == TyKind::Pack)
      pt = pt->args.empty() ? nullptr : pt->args[0];
    if (!pt || !isClass(types.stripRef(pt)) || classKey(pt) != classKey(t))
      ctor = nullptr; // a converting constructor of another class: not for a copy
    else if (!ctor->templateParams.empty() && implicitCopyOrMoveTakes(c, t, pt, at))
      ctor = nullptr; // as good as the implicit copy or move constructor, which wins as no template
  }
  if (ctor && (ctor->flags & DF_Delete))
    unsupported(at, "constructing " + types.toString(t, &symbols) + " with a deleted constructor (copying a move-only object?)");
  if (firstTy && isClass(types.stripRef(firstTy)) && classKey(firstTy) != classKey(t) && ((ctor && !ctor->params.empty()) || converts))
  {
    const Ty* pt = nullptr;
    if (ctor && !ctor->params.empty())
    {
      TemplateArgs classArgs = global.classArgsOf(canonicalClass(t, at));
      ArgsScope scope(*this, &classArgs);
      pt = declType(ctor->params[0]);
    }
    // A copy-initialization's converting constructor takes no user-defined conversion of its
    // argument ([over.best.ics] 4): `A x = b;` with `A(int)` beside b's `operator int()` and
    // `operator A()` is the conversion function's, never b made an int (until 2026-11-08 two
    // conversions, the constructor's). A direct- or list-initialization takes the conversion function
    // as well over a constructor that needs a user-defined conversion of b (CWG2327, as clang and g++
    // do: the function's object an exact match): `A x(b);`, `A x{b};`, `A x = {b};` (until 2026-11-08
    // A(char) called through b's `operator char()`; g++ calls `A x = {b};` ambiguous).
    bool secondConversion = false;
    if (converts)
    {
      const Ty* p = pt ? types.unqualified(types.stripRef(pt)) : nullptr;
      const Ty* a = withCurrentArgs(types.unqualified(types.stripRef(firstTy)));
      std::vector<std::pair<std::string, uint32_t>> through;
      secondConversion = !types.isDependent(a) && !(p && isClass(p) && (classKey(p) == classKey(a) || basePath(a, p, through, at)));
    }
    if ((pt && ctor->params.size() == 1 && isClass(types.stripRef(pt)) && classKey(pt) == classKey(t)) || secondConversion)
    {
      // The copy constructor, chosen for a value of another class: that class converts to t
      // first (a base, or its conversion operator), the constructor is not called with it.
      // A conversion function giving t itself, a prvalue, builds the object where it is
      // (`A x = b;`, `A x{b}` with `operator A()`: C++17's guaranteed elision; until 2026-11-08 a
      // temporary moved or copied from).
      std::vector<std::pair<std::string, uint32_t>> path;
      const Ty* fromTy = withCurrentArgs(types.stripRef(firstTy));
      const Ty* owner = nullptr;
      TemplateArgs opArgs;
      if (!types.isDependent(fromTy) && !basePath(types.unqualified(fromTy), t, path, at))
      {
        Value object = rvalue(args[0]);
        if (FunctionDecl* op = conversionOperator(conversionObject(object), types.unqualified(t), at, &owner, !copyInit, &opArgs); op && returnsInMemory(types.unqualified(t)))
        {
          const Ty* result;
          {
            TemplateArgs ownerArgs = opArgs.pairs.empty() || op->templateParams.empty() ? global.classArgsOf(owner) : opArgs;
            ArgsScope scope(*this, &ownerArgs);
            result = declType(op) ? declType(op)->inner : nullptr;
          }
          if (result && !result->isReference() && isClass(result) && classKey(result) == classKey(t))
          {
            Value made = callConversion(op, object.ty, owner, object.text, at, &opArgs, false, addr);
            if (made.text != addr)
              unsupported(at, "a conversion function's " + types.toString(t, &symbols) + " result built somewhere else than the object it initializes");
            return;
          }
        }
        Value src = convert(object, types.unqualified(t), at, !copyInit);
        if (src.isTemporary)
          moveTemporary(addr, src, t, at);
        else if (movable(args[0]))
          moveConstruct(addr, src.text, t, at);
        else
          copyConstruct(addr, src.text, t, at);
        return;
      }
      Value src = convert(rvalue(args[0]), types.unqualified(t), at);
      if (src.isTemporary)
        moveTemporary(addr, src, t, at);
      else if (movable(args[0]))
        moveConstruct(addr, src.text, t, at);
      else
        copyConstruct(addr, src.text, t, at);
      return;
    }
  }
  if (ctor)
  {
    TemplateArgs ctorArgs = global.classArgsOf(canonicalClass(t, at));
    if (!ctor->templateParams.empty())
      global.deduceArgs(ctor, argTypesOf(args), ctorArgs); // `template<typename F> Function(F)`: F from the argument
    callFunction(ctor, ctorArgs, canonicalClass(t, at), addr, "", args, at);
    return;
  }
  if (args.size() == 1 && firstTy && isClass(types.stripRef(firstTy)) && classKey(firstTy) != classKey(t))
  {
    // A value of a class derived from t, no constructor of t taking it as written (t's copy
    // and move constructors are implicit, or t is an aggregate): its t part, copied or moved
    // (`Name(const Name& n) : Text(n)`, `: Plain(d)`).
    std::vector<std::pair<std::string, uint32_t>> path;
    if (const Ty* from = withCurrentArgs(types.unqualified(types.stripRef(firstTy))); !types.isDependent(from) && basePath(from, t, path, at))
    {
      Value src = convert(rvalue(args[0]), types.unqualified(t), at);
      if (movable(args[0]))
        moveConstruct(addr, src.text, t, at);
      else
        copyConstruct(addr, src.text, t, at);
      return;
    }
  }
  if (args.size() == 1 && firstTy && isClass(types.stripRef(firstTy)) && classKey(firstTy) != classKey(t) &&
      conversionOperator(types.stripRef(firstTy), t, at))
  {
    // No constructor of t takes the value, its class converts to t: `ProtoID copy = connector;`
    // and `h.chosen = connector;` through IDConnector's `implicit operator IDType()`.
    Value src = convert(rvalue(args[0]), types.unqualified(t), at);
    if (src.isTemporary)
      moveTemporary(addr, src, t, at);
    else
      copyConstruct(addr, src.text, t, at);
    return;
  }
  if (args.size() == 1 && firstTy && isClass(types.stripRef(firstTy)) && classKey(firstTy) == classKey(t))
  {
    if (args[0]->kind == NodeKind::CallExpr && !firstTy->isReference())
    {
      // `: simulations(loadSimulations(input))`: the result built in the object itself (C++17),
      // as `T x = f();` is; a Map moved bitwise afterwards kept its header pointing at the dead
      // temporary.
      Value v = callValue(static_cast<CallExpr*>(args[0]), addr);
      if (!isClass(v.ty) || v.text == addr)
        return;
      if (v.isTemporary)
        moveTemporary(addr, v, t, at);
      else
        copyConstruct(addr, v.text, t, at);
      return;
    }
    Value src = rvalue(args[0]);
    if (src.isTemporary)
      moveTemporary(addr, src, t, at);
    else if (movable(args[0]))
      moveConstruct(addr, src.text, t, at);
    else
      copyConstruct(addr, src.text, t, at);
    return;
  }
  if (!hasUserConstructors(c))
  {
    // An aggregate: the values fill the fields in order; the fields left over get their
    // default member initializers (or default construction), as in C++.
    const StructInfo& info = structFor(t, at);
    // `Item {load, ifNoPrototype, ::forward<Args>(args)...}` with Args empty: the expansion of an
    // empty pack is no value (InstanceLoader::registerLoader).
    std::vector<Expr*> values;
    bool expanded = false;
    for (Expr* a : args)
    {
      if (!isPackExpansion(a))
      {
        values.push_back(a);
        continue;
      }
      expanded = true;
      if (expandedCount(Span<Expr*>{&a, 1}, at) != 0)
        unsupported(at, "a pack expanded into the values of an aggregate");
    }
    if (expanded)
      args = Span<Expr*>{values.data(), uint32_t(values.size())};
    storeZero(t, addr, at);
    storeVptrs(addr, info);
    if (info.isUnion && designators.empty() && args.size() == 1 && !info.fieldDecls.empty())
    {
      // `{nullptr}` for a union: its first member, at the union's start (Lua's TValue's Value).
      initializeFrom(addr, unionMemberType(t, info.fieldDecls[0], at), args[0], at);
      return;
    }
    // C++17 aggregate init: base sub-objects consume the first args, then data members; each part
    // made is destroyed if a later one's initializer throws (InlineParts).
    InlineParts parts(*this, at);
    size_t argIdx = 0;
    if (!info.bases.empty() && designators.empty())
    {
      for (auto& [baseDef, baseFieldIdx] : info.bases)
      {
        std::string baseAddr = gep(info.name, addr, baseFieldIdx);
        const Ty* bt = info.fieldTypes[baseFieldIdx];
        if (argIdx < args.size() && !initializesWhole(bt, args[argIdx], at))
        {
          // (a base's braces elided: `Der2 d{1, 2, 3}` over `struct Base2 { int u; int v; }` and
          // its own `int w` gives the base 1 and 2, as C++ does)
          size_t take = std::max<size_t>(1, elidedValues(bt, args, argIdx, at));
          initializeFromElided(baseAddr, bt, Span<Expr*>{args.begin() + argIdx, uint32_t(take)}, at);
          argIdx += take;
        }
        else if (argIdx < args.size())
          initializeFrom(baseAddr, bt, args[argIdx++], at);
        else if (needsConstruction(bt, at))
          defaultConstruct(baseAddr, bt, at); // a base no value is for: value-initialized, as from `{}` (D, 2026-11-05: it stayed zero)
        else
          continue;
        parts.built(baseAddr, bt);
      }
      storeVptrs(addr, info); // the bases built pointed it at theirs
    }
    // The members the values go to (aggregateMembers).
    std::vector<Decl*> members = aggregateMembers(info, c);
    // Which value goes to which member: in order, or by name for `.x = 1` designators.
    std::vector<Expr*> valueOf(members.size(), nullptr);
    std::vector<std::vector<Expr*>> elided(members.size()); // a member's values without their braces (an array's, an aggregate's)
    size_t next = 0;
    for (size_t k = argIdx; k < args.size(); ++k)
    {
      uint32_t designator = k < designators.size() ? designators[k] : UINT32_MAX;
      if (designator != UINT32_MAX)
      {
        size_t found = members.size();
        for (size_t j = 0; j < members.size(); ++j)
          if (members[j]->kind == NodeKind::VariableDecl && !static_cast<VariableDecl*>(members[j])->name.empty() &&
              static_cast<VariableDecl*>(members[j])->name.last().symbol == designator)
            found = j;
        if (found == members.size())
          unsupported(at, "a designator naming no field of " + qualifiedName(c) + " (" + sym(designator) + ")");
        valueOf[found] = args[k];
        next = found + 1;
        continue;
      }
      if (next >= valueOf.size())
      {
        if (valueOf.empty())
          break; // anonymous/incomplete class with no known fields — already zeroed
        unsupported(at, "more values than " + qualifiedName(c) + " has fields");
      }
      // Brace elision ([dcl.init.aggr] 16): a member that is an array or an aggregate takes its
      // elements' values from the list itself when the value there does not initialize it whole
      // (initializesWhole): `GUID g = { 0x429b8c22, 0x9188, 0x4b0c, 0x87, ... }` fills Data4[8]
      // from the last eight, and `Out o{1, 2}` over `In in; int c;` gives in both (D, 2026-11-06:
      // an aggregate member took one value, in {1, 0} and c 2).
      if (const Ty* mt = aggregateMemberType(t, info, members[next], at); !initializesWhole(mt, args[k], at))
      {
        size_t take = std::max<size_t>(1, elidedValues(mt, args, k, at));
        elided[next] = std::vector<Expr*>(args.begin() + k, args.begin() + k + take);
        k += take - 1;
        ++next;
        continue;
      }
      valueOf[next++] = args[k];
    }
    if (info.flexible && !valueOf.empty() && valueOf.back())
      fail(at, "a value for " + qualifiedName(c) + "::" + sym(static_cast<VariableDecl*>(members.back())->name.last().symbol) + ", a flexible array member, which has no elements of its own to initialize");
    size_t i = 0;
    std::set<size_t> zeroedUnits; // the storage units of bit fields, each zeroed at its first member
    // A union holds one member: the one a value is for (`{.effectTexture = {bitmap}}`), else
    // the one with a default member initializer, else its first (C++'s rule), each at the
    // union's start with its own type.
    Decl* unionMember = nullptr;
    if (info.isUnion)
    {
      for (size_t j = 0; j < members.size() && !unionMember; ++j)
        if (valueOf[j] || !elided[j].empty())
          unionMember = members[j];
      for (size_t j = 0; j < members.size() && !unionMember; ++j)
        if (members[j]->kind == NodeKind::VariableDecl && static_cast<VariableDecl*>(members[j])->init)
          unionMember = members[j];
      if (!unionMember && !members.empty())
        unionMember = members[0];
    }
    for (Decl* d : members)
    {
      if (info.isUnion && d != unionMember)
      {
        ++i;
        continue;
      }
      if (!elided[i].empty())
      {
        // The values its braces were elided from, then what they leave out value-initialized, as
        // from `{}` (initializeFromElided).
        const Ty* ft = types.unqualified(aggregateMemberType(t, info, d, at));
        std::string fieldAddr = info.isUnion ? addr : gep(info.name, addr, fieldIndexOf(info, d));
        initializeFromElided(fieldAddr, ft, Span<Expr*>{elided[i].data(), uint32_t(elided[i].size())}, at);
        if (!info.isUnion)
          parts.built(fieldAddr, ft);
        ++i;
        continue;
      }
      initializeAggregateMember(addr, t, info, d, valueOf[i++], zeroedUnits, parts, at);
    }
    return;
  }
  unsupported(at, "a constructor of " + qualifiedName(c) + " taking " + std::to_string(args.size()) + " arguments");
}

auto codegen::Emitter::aggregateMembers(const StructInfo& info, ClassDecl* c) -> std::vector<Decl*>
{
  std::vector<Decl*> members;
  for (Decl* d : info.fieldDecls)
    if (d->kind == NodeKind::VariableDecl && bitfieldIn(info, d))
    {
      for (Decl* m : unitMembers(info, c, d))
        if (!static_cast<VariableDecl*>(m)->name.empty())
          members.push_back(m);
    }
    else
      members.push_back(d);
  return members;
}

auto codegen::Emitter::initializeAggregateMember(const std::string& addr, const Ty* t, const StructInfo& info, Decl* d, Expr* value,
                                                 std::set<size_t>& zeroedUnits, InlineParts& parts, Node* at) -> void
{
  if (d->kind == NodeKind::VariableDecl)
    if (const BitfieldInfo* bits = bitfieldIn(info, d))
    {
      initializeBitfield(addr, t, info, static_cast<VariableDecl*>(d), *bits, value != nullptr, value, zeroedUnits, at);
      return;
    }
  std::string fieldAddr = info.isUnion ? addr : gep(info.name, addr, fieldIndexOf(info, d));
  const Ty* ft = info.isUnion ? unionMemberType(t, d, at) : info.fieldTypes[fieldIndexOf(info, d)];
  const Ty* declared = d->kind == NodeKind::VariableDecl ? global.typeOfDecl(d) : nullptr;
  bool reference = ft->isReference() || (declared && declared->isReference());
  if (value)
  {
    if (reference)
      bindReferenceMember(fieldAddr, ft->isReference() ? ft : declared, value, at, true); // `Ref r{x}`: the member refers to x
    else
    {
      initializeFrom(fieldAddr, ft, value, at);
      if (!info.isUnion)
        parts.built(fieldAddr, ft);
    }
    return;
  }
  if (d->kind != NodeKind::VariableDecl)
    return;
  auto* v = static_cast<VariableDecl*>(d);
  if (v->init && reference)
    withMemberInitializer(addr, t, v, [&](Expr* value) { bindReferenceMember(fieldAddr, ft->isReference() ? ft : declared, value, v); });
  else if (v->init)
    withMemberInitializer(addr, t, v, [&](Expr* value) { initializeFrom(fieldAddr, ft, value, v, scalarContext(ft, v->bracedInit)); });
  else if (isClass(ft))
    defaultConstruct(fieldAddr, ft, at);
  else if (ft->kind == TyKind::Array && (needsDestruction(ft, at) || needsConstruction(ft, at))) // elements with constructors or default member initializers
    constructArray(fieldAddr, ft, at);
  if (!reference && !info.isUnion)
    parts.built(fieldAddr, ft);
}

auto codegen::Emitter::aggregateMemberType(const Ty* t, const StructInfo& info, Decl* d, Node* at) -> const Ty*
{
  const Ty* declared = d->kind == NodeKind::VariableDecl ? global.typeOfDecl(d) : nullptr;
  if (d->kind == NodeKind::VariableDecl && bitfieldIn(info, d))
    return declared ? declared : types.intType(32); // (a scalar: one value)
  if (declared && declared->isReference())
    return declared;
  return info.isUnion ? unionMemberType(t, d, at) : info.fieldTypes[fieldIndexOf(info, d)];
}

auto codegen::Emitter::initializesWhole(const Ty* et, Expr* value, Node* at) -> bool
{
  if (!value || !et || et->isReference() || value->kind == NodeKind::BraceInitExpr)
    return true;
  et = types.unqualified(et);
  Expr* inner = value;
  while (inner->kind == NodeKind::ParenExpr)
    inner = static_cast<ParenExpr*>(inner)->inner;
  const Ty* vt = value->ty && value->ty->isKnown() ? types.unqualified(types.stripRef(withCurrentArgs(value->ty))) : nullptr;
  if (vt && types.isDependent(vt))
    vt = nullptr;
  if (et->kind == TyKind::Array)
  {
    // An array is initialized whole by an array of its own type, and one of characters by a
    // string literal (typed as a pointer to its characters); a string literal for an array of
    // pointers is its first element's (`Array<const char*, 3> a = {"a", "b", "c"}`): as the
    // checker's bracesElided has it.
    const Ty* element = et->inner ? types.unqualified(et->inner) : nullptr;
    bool characters = element && (element->kind == TyKind::Char || element->kind == TyKind::Int);
    bool literal = inner->kind == NodeKind::LiteralExpr && static_cast<LiteralExpr*>(inner)->literalKind == TokenKind::String;
    if (et->count == 0 || (characters && literal) || !vt)
      return true;
    return vt->kind == TyKind::Array && vt == et;
  }
  // Only an aggregate class has braces to elide: no constructor declared (`= default` either, C++20;
  // none inherited), no virtual functions, no typed union, no InitializerList: as the checker's
  // bracesElided has it, which counts an array's elements from its values.
  if (!isClass(et) || !et->decl || isInitializerListType(et) || typedUnionOf(et, at))
    return true;
  ClassDecl* c = classDef(et->decl, at);
  if (!c || inheritedConstructorBase(c) || structFor(et, at).polymorphic)
    return true;
  for (Decl* m : c->members)
    if (m->kind == NodeKind::FunctionDecl && (m->flags & (DF_Constructor | DF_Virtual)))
      return true;
  if (!vt)
    return true; // (a value of a type the checker left open: taken as it is)
  if (!isClass(vt))
    return false; // a number, a pointer, an enumerator: no aggregate converts from one
  if (canonicalClass(vt, at) == canonicalClass(et, at))
    return true;
  std::vector<std::pair<std::string, uint32_t>> path;
  return basePath(vt, et, path, at) || conversionOperator(vt, et, at);
}

auto codegen::Emitter::elidedValues(const Ty* t, Span<Expr*> values, size_t start, Node* at, int depth) -> size_t
{
  t = types.unqualified(types.stripRef(t));
  size_t next = start;
  // One element of type et: the value there, or the values its own elements take.
  auto element = [&](const Ty* et) -> bool
  {
    if (next >= values.size())
      return false;
    if (depth > 32 || initializesWhole(et, values[next], at))
      ++next;
    else
      next += std::max<size_t>(1, elidedValues(et, values, next, at, depth + 1));
    return true;
  };
  if (t->kind == TyKind::Array)
  {
    for (uint32_t e = 0; e < t->count && element(t->inner); ++e)
    {
    }
    return next - start;
  }
  if (!isClass(t) || !t->decl)
    return start < values.size() ? 1 : 0;
  const StructInfo& info = structFor(t, at);
  for (auto& [baseDef, index] : info.bases)
    if (!element(info.fieldTypes[index]))
      return next - start;
  for (Decl* d : aggregateMembers(info, classDef(t->decl, at)))
    if (!element(aggregateMemberType(t, info, d, at)) || info.isUnion)
      break; // (a union: its first member alone)
  return next - start;
}

auto codegen::Emitter::initializeFromElided(const std::string& addr, const Ty* et, Span<Expr*> values, Node* at) -> void
{
  et = types.unqualified(types.stripRef(et));
  if (et->kind == TyKind::Array)
    initializeArrayElements(addr, et, values, at);
  else
    constructInto(addr, et, values, at, true);
}

auto codegen::Emitter::initializeArrayElements(const std::string& addr, const Ty* t, Span<Expr*> values, Node* at) -> void
{
  t = types.unqualified(types.stripRef(t));
  const Ty* et = t->inner;
  InlineParts parts(*this, at, true);
  size_t next = 0;
  uint32_t e = 0;
  if (std::any_of(values.begin(), values.end(), [&](Expr* v) { return isPackExpansion(v); }))
  {
    // `int a[] = {0, v..., 100}`, `{N * N...}`: an element for each value the expansions give, in
    // order, each initializing its element whole (braces elided around a pack's elements: an
    // error). They were taken as one value each ("taking the address of v").
    if (size_t total = expandedCount(values, at); total > t->count)
      fail(at, "more values than an array of " + std::to_string(t->count) + " " + types.toString(et, &symbols) + " takes (" +
                   std::to_string(total) + ")");
    forEachExpanded(values, at, [&](Expr* one)
    {
      if (!initializesWhole(et, one, at))
        unsupported(at, "an element of an array initialized from a pack expansion with its braces elided");
      std::string element = newTemp();
      fn += "  " + element + " = getelementptr inbounds " + llType(t, at) + ", ptr " + addr + ", i64 0, i64 " + std::to_string(e) + "\n";
      initializeFrom(element, et, one, at);
      parts.built(element, et);
      ++e;
    });
    next = values.size();
  }
  for (; e < t->count && next < values.size(); ++e)
  {
    std::string element = newTemp();
    fn += "  " + element + " = getelementptr inbounds " + llType(t, at) + ", ptr " + addr + ", i64 0, i64 " + std::to_string(e) + "\n";
    if (initializesWhole(et, values[next], at))
      initializeFrom(element, et, values[next++], at);
    else
    {
      size_t take = std::max<size_t>(1, elidedValues(et, values, next, at));
      initializeFromElided(element, et, Span<Expr*>{values.begin() + next, uint32_t(take)}, at);
      next += take;
    }
    parts.built(element, et);
  }
  if (next < values.size())
    fail(at, "more values than an array of " + std::to_string(t->count) + " " + types.toString(et, &symbols) + " takes (" +
                 std::to_string(values.size()) + " where it takes " + std::to_string(next) + ")");
  if (e < t->count && needsConstruction(et, at))
  {
    // The elements the values leave out, value-initialized as from `{}`: their default constructor
    // or default member initializers (D, 2026-11-05: they stayed zero), in one loop for many.
    std::string rest = newTemp();
    fn += "  " + rest + " = getelementptr inbounds " + llType(t, at) + ", ptr " + addr + ", i64 0, i64 " + std::to_string(e) + "\n";
    constructArray(rest, types.array(et, t->count - e), at);
  }
}

auto codegen::Emitter::bindingAddress(VariableDecl* v, Node* e, std::string& out) -> bool
{
  if (v->structuredBindings.empty() || !e || e->kind != NodeKind::NameExpr || static_cast<NameExpr*>(e)->name.empty())
    return false;
  uint32_t symbol = static_cast<NameExpr*>(e)->name.last().symbol;
  size_t index = v->structuredBindings.size();
  for (size_t i = 0; i < v->structuredBindings.size(); ++i)
    if (static_cast<NameExpr*>(v->structuredBindings[i])->name.last().symbol == symbol)
      index = i;
  if (index == v->structuredBindings.size())
    return false;
  auto it = locals.find(v);
  if (it == locals.end())
    unsupported(e, "a structured binding outside its function");
  std::string base = it->second.addr;
  if (it->second.isRef)
  {
    std::string r = newTemp();
    fn += "  " + r + " = load ptr, ptr " + base + "\n";
    base = r;
  }
  const Ty* object = types.unqualified(types.stripRef(it->second.ty));
  if (object->kind == TyKind::Array)
  {
    std::string r = newTemp();
    fn += "  " + r + " = getelementptr inbounds " + llType(object->inner, e) + ", ptr " + base + ", i64 " + std::to_string(index) + "\n";
    out = r;
    return true;
  }
  if (!isClass(object))
    unsupported(e, "a structured binding of something that is neither a class nor an array");
  // The class whose direct members the bindings name: the object's own class, or, when that
  // declares none, the one base that has them (C++'s rule: Vector2D's x and y are its CRTP
  // base Vector2's).
  const Ty* owner = object;
  for (int depth = 0; depth < 16 && structFor(owner, e).fieldDecls.empty(); ++depth)
  {
    const Ty* withMembers = nullptr;
    for (auto& [baseDef, baseIndex] : std::vector<std::pair<Decl*, uint32_t>>(structFor(owner, e).bases))
    {
      const Ty* bt = structFor(owner, e).fieldTypes[baseIndex];
      if (!structFor(bt, e).fieldDecls.empty() || !structFor(bt, e).bases.empty())
      {
        if (withMembers)
          unsupported(e, "a structured binding of a class whose data members are in several bases");
        withMembers = bt;
      }
    }
    if (!withMembers)
      break;
    std::string part;
    if (!baseAddress(owner, withMembers, base, part, e))
      unsupported(e, "a structured binding through a base the layout does not have");
    base = part;
    owner = withMembers;
  }
  Decl* member = bindingMember(owner, index, e);
  const BitfieldInfo* bits = nullptr;
  if (!member || !fieldAddress(owner, member, base, out, e, &bits))
    unsupported(e, "a structured binding of a class without that many data members");
  // A binding of a bit-field is that field, its own bits of the storage unit (lvalueBitfield, as a
  // member's lvalue records it): `auto [a, b] = s;` with `unsigned u3 : 3; unsigned rest : 29;`
  // read a as the whole unit, 17 for u3 1 and rest 2.
  lvalueBitfield = {static_cast<const Expr*>(e), bits};
  // A reference member's binding names the object the member refers to, as `pair.second` does
  // (TestBuildabilityChecks' `[testedLocation, surface]` over Pair<Locations, Surface&> read the
  // member's own storage as the Surface).
  if (const Ty* ft = fieldTypeIn(owner, member, e); ft && ft->isReference())
  {
    std::string referenced = newTemp();
    fn += "  " + referenced + " = load ptr, ptr " + out + "\n";
    out = referenced;
  }
  return true;
}

auto codegen::Emitter::bindingBitfield(Expr* e) -> const BitfieldInfo*
{
  while (e && e->kind == NodeKind::ParenExpr)
    e = static_cast<ParenExpr*>(e)->inner;
  Decl* d = e && e->kind == NodeKind::NameExpr && !static_cast<NameExpr*>(e)->name.empty() ? static_cast<NameExpr*>(e)->resolved : nullptr;
  auto* v = d && d->kind == NodeKind::VariableDecl ? static_cast<VariableDecl*>(d) : nullptr;
  if (!v || v->structuredBindings.empty())
    return nullptr;
  uint32_t symbol = static_cast<NameExpr*>(e)->name.last().symbol;
  size_t index = v->structuredBindings.size();
  for (size_t i = 0; i < v->structuredBindings.size(); ++i)
    if (static_cast<NameExpr*>(v->structuredBindings[i])->name.last().symbol == symbol)
      index = i;
  auto it = locals.find(v);
  if (index == v->structuredBindings.size() || it == locals.end())
    return nullptr;
  const Ty* owner = types.unqualified(types.stripRef(it->second.ty));
  if (!isClass(owner))
    return nullptr;
  // (the class bindingAddress takes the member from: the object's, or the one base with members)
  for (int depth = 0; depth < 16 && structFor(owner, e).fieldDecls.empty(); ++depth)
  {
    const Ty* withMembers = nullptr;
    for (auto& [baseDef, baseIndex] : std::vector<std::pair<Decl*, uint32_t>>(structFor(owner, e).bases))
      if (const Ty* bt = structFor(owner, e).fieldTypes[baseIndex]; !structFor(bt, e).fieldDecls.empty() || !structFor(bt, e).bases.empty())
        withMembers = bt;
    if (!withMembers)
      break;
    owner = withMembers;
  }
  Decl* member = bindingMember(owner, index, e);
  return member ? bitfieldIn(structFor(owner, e), member) : nullptr;
}

auto codegen::Emitter::bindingMember(const Ty* owner, size_t index, Node* at) -> Decl*
{
  const StructInfo& info = structFor(owner, at);
  const Ty* key = canonicalClass(owner, at);
  auto* c = key && key->decl && key->decl->kind == NodeKind::ClassDecl ? static_cast<ClassDecl*>(key->decl) : nullptr;
  if (!c)
    return nullptr;
  size_t seen = 0;
  for (Decl* m : c->members)
    if (m->kind == NodeKind::VariableDecl && !(m->flags & (DF_Static | DF_Typedef | DF_Friend)) && !static_cast<VariableDecl*>(m)->name.empty() &&
        info.fieldIndex.count(m) && seen++ == index)
      return m;
  return nullptr;
}

auto codegen::Emitter::elementOfOtherClass(const Ty* elementTy, const Ty* vt, Node* at) -> bool
{
  const Ty* target = types.unqualified(types.stripRef(vt));
  const Ty* et = elementTy ? types.unqualified(types.stripRef(elementTy)) : nullptr;
  return et && isClass(target) && isClass(et) && !types.isDependent(et) && canonicalClass(et, at) != canonicalClass(target, at);
}

auto codegen::Emitter::elementAs(const std::string& addr, const Ty* elementTy, const Ty* vt, Node* at) -> std::string
{
  // A reference loop variable of another scalar type than the elements (`for (const double& d :
  // ints)`): a temporary holding the converted element, which a const reference (or an rvalue one)
  // binds as in C++; a non-const lvalue reference binds none ([dcl.init.ref] 5). Until 2026-11-05 it
  // bound the element's own bytes: an int read as a double.
  const Ty* target = types.unqualified(types.stripRef(vt));
  const Ty* et = elementTy ? types.unqualified(types.stripRef(elementTy)) : nullptr;
  if (vt->isReference() && et && target->isKnown() && et->isKnown() && !types.isDependent(target) && !types.isDependent(et) &&
      !isAggregate(target) && !isAggregate(et) && et != target)
  {
    if (vt->kind == TyKind::LRef && !types.stripRef(vt)->isConst)
    {
      if (llType(et, at) != llType(target, at))
        fail(at, "the loop variable of type " + types.toString(vt, &symbols) + " over elements of type " + types.toString(et, &symbols) +
                     ": a non-const lvalue reference binds only an element of its type");
      return addr; // (`long&` over ints where both are 32 bits: one representation)
    }
    Value converted = convert(load(addr, et, at), target, at);
    std::string slot = newObjectSlot(target, at);
    fn += "  store " + llType(target, at) + " " + converted.text + ", ptr " + slot + "\n";
    return slot;
  }
  if (!elementOfOtherClass(elementTy, vt, at))
    return addr;
  return convert(Value{addr, types.unqualified(types.stripRef(elementTy)), false}, types.unqualified(types.stripRef(vt)), at).text;
}

auto codegen::Emitter::elementMadeInto(const Ty* elementTy, const Ty* vt) -> bool
{
  return isClass(types.unqualified(types.stripRef(vt))) && elementTy && !isAggregate(types.unqualified(types.stripRef(elementTy)));
}

auto codegen::Emitter::makeFromElement(const std::string& slot, const Ty* cls, const std::string& elementAddr, const Ty* elementTy, Node* at) -> void
{
  VariableDecl element{};
  element.kind = NodeKind::VariableDecl;
  element.token = at->token;
  NameExpr name{};
  name.kind = NodeKind::NameExpr;
  name.token = at->token;
  name.resolved = &element;
  name.ty = types.stripRef(elementTy);
  locals[&element] = {elementAddr, types.stripRef(elementTy), false};
  struct Forget { ~Forget() { locals.erase(d); } std::unordered_map<Decl*, Local>& locals; Decl* d; } forget{locals, &element};
  initializeFrom(slot, cls, &name, at);
}

auto codegen::Emitter::constructFromVector(const std::string& addr, const Ty* cls, Value v, bool copyInit, Node* at) -> void
{
  cls = types.unqualified(types.stripRef(cls));
  FunctionDecl* ctor = findConstructor(cls, {v.ty}, at);
  if (!ctor || (ctor->flags & DF_Delete))
    unsupported(at, "making " + types.toString(cls, &symbols) + " of a vector the checker could not type (" + types.toString(v.ty, &symbols) +
                        "): no constructor of it takes one");
  if (copyInit && global.isExplicit(ctor, global.classArgsOf(canonicalClass(cls, at))))
    fail(at, "copy-initialising " + types.toString(cls, &symbols) + " from a vector (" + types.toString(v.ty, &symbols) + ") by its explicit constructor");
  std::string slot = newObjectSlot(v.ty, at);
  fn += "  store " + llType(v.ty, at) + " " + v.text + ", ptr " + slot + "\n";
  VariableDecl vector{};
  vector.kind = NodeKind::VariableDecl;
  vector.token = at->token;
  NameExpr name{};
  name.kind = NodeKind::NameExpr;
  name.token = at->token;
  name.resolved = &vector;
  name.ty = v.ty;
  locals[&vector] = {slot, v.ty, false};
  struct Forget { ~Forget() { locals.erase(d); } std::unordered_map<Decl*, Local>& locals; Decl* d; } forget{locals, &vector};
  Expr* one = &name;
  constructInto(addr, cls, Span<Expr*>{&one, 1}, at);
}

auto codegen::Emitter::implicitlyMovableLocal(Expr* e, const Ty*& cls) -> Decl*
{
  while (e && e->kind == NodeKind::ParenExpr)
    e = static_cast<ParenExpr*>(e)->inner; // `return (result);` (until 2026-11-08 a copy)
  if (!e || e->kind != NodeKind::NameExpr)
    return nullptr;
  Decl* d = static_cast<NameExpr*>(e)->resolved;
  if (!d || (d->kind != NodeKind::VariableDecl && d->kind != NodeKind::ParamDecl) || (d->flags & (DF_Static | DF_ThreadLocal | DF_Extern)))
    return nullptr;
  if (d->kind == NodeKind::VariableDecl && !static_cast<VariableDecl*>(d)->structuredBindings.empty())
    return nullptr;
  if (capturedLocals.count(d))
    return nullptr; // `[m]() { return m; }`: the closure's member, copied
  auto it = locals.find(d);
  if (it == locals.end() || !isClass(types.stripRef(it->second.ty)))
    return nullptr;
  // An rvalue reference is moved from as its object is (`M f(M&& m) { return m; }`; until 2026-11-08
  // a copy, where C++20 moves), an lvalue reference never.
  const Ty* declared = declType(d);
  if (it->second.isRef && (!declared || declared->kind != TyKind::RRef))
    return nullptr;
  // A const one is copied (`const T t; return t;`: the implicit move's `const T&&` binds the copy
  // constructor, never the move constructor).
  if (declared && types.stripRef(declared)->isConst)
    return nullptr;
  cls = types.unqualified(types.stripRef(it->second.ty));
  return d;
}

auto codegen::Emitter::localObjectOfOtherClass(Expr* e) -> const Ty*
{
  const Ty* cls = nullptr;
  if (!implicitlyMovableLocal(e, cls) || classKey(cls) == classKey(currentReturn))
    return nullptr;
  return cls;
}

auto codegen::Emitter::returnedLocalObject(Expr* e) -> Decl*
{
  const Ty* cls = nullptr;
  Decl* d = implicitlyMovableLocal(e, cls);
  if (!d || classKey(cls) != classKey(currentReturn))
    return nullptr;
  return d;
}

auto codegen::Emitter::builtInResult(VariableDecl* v, Node* at) -> VariableDecl*
{
  if (!v || sretAddr.empty())
    return nullptr;
  const Ty* t = localType(v);
  if (!t || !t->isKnown() || types.isDependent(t) || t->isReference() || !isClass(types.unqualified(t)) || !currentReturn ||
      classKey(types.unqualified(t)) != classKey(types.unqualified(currentReturn)))
  {
    fail(at, "the named return " + (v->name.empty() ? std::string("?") : sym(v->name.last().symbol)) + " is a " +
                 (t ? types.toString(t, &symbols) : std::string("?")) + ", not the returned " +
                 (currentReturn ? types.toString(currentReturn, &symbols) : std::string("?")) +
                 " (namedReturnVariable and codegen disagree)");
    return nullptr;
  }
  return v;
}

auto codegen::Emitter::rvalueReferenceGivenLvalue(const Ty* t, Expr* init, Node* at) -> bool
{
  if (!t || t->kind != TyKind::RRef || !isLvalueExpression(init) || !init->ty || !init->ty->isKnown() || types.isDependent(init->ty))
    return false;
  const Ty* given = types.unqualified(types.stripRef(init->ty)), *bound = types.unqualified(types.stripRef(t));
  std::vector<std::pair<std::string, uint32_t>> path;
  return given->kind != TyKind::Function &&
         (given == bound || (isClass(given) && isClass(bound) && (classKey(given) == classKey(bound) || basePath(given, bound, path, at))));
}

auto codegen::Emitter::checkRvalueReferenceResult(const Ty* ret, Expr* value) -> void
{
  // An rvalue reference result binds no lvalue of its type, as a variable does not: `int&& f(int&& x)
  // { return x; }` is an error, x being an lvalue (C++20's [dcl.init.ref] 5.4.4; C++23's P2266 would
  // make it an xvalue). Until 2026-11-08 x itself was returned.
  if (rvalueReferenceGivenLvalue(ret, value, value))
    fail(value, "a function returning " + types.toString(ret, &symbols) +
                    " returns an lvalue of its type: an rvalue reference binds only a temporary or what move() gives, never the object itself");
}

auto codegen::Emitter::returnsNamedReturn(Expr* e) -> bool
{
  while (e && e->kind == NodeKind::ParenExpr)
    e = static_cast<ParenExpr*>(e)->inner;
  return namedReturn && e && e->kind == NodeKind::NameExpr && static_cast<NameExpr*>(e)->resolved == namedReturn;
}

auto codegen::Emitter::loadBitfield(const std::string& addr, const BitfieldInfo& bits, const Ty* memberTy, Node* at) -> Value
{
  std::string unitLl = llType(bits.unit, at);
  uint32_t unitBits = uint32_t(sizeOf(bits.unit, at) * 8);
  std::string unit = newTemp(), r;
  fn += "  " + unit + " = load " + unitLl + ", ptr " + addr + (bits.bytes ? ", align 1" : "") + "\n";
  const Ty* valueTy = types.unqualified(memberTy);
  bool isSigned = (valueTy->kind == TyKind::Int || valueTy->kind == TyKind::Char) && !valueTy->isUnsigned;
  if (valueTy->kind == TyKind::Enum && valueTy->decl)
  {
    // An enumeration's field is as signed as its underlying type: a fixed one's (`enum class Color :
    // signed char` stored -2 and read 6 back, zero-extended), else the target's choice: int on
    // Windows (MSVC sign-extends every such field), and on the others int only when an enumerator
    // is negative, unsigned otherwise (the Itanium ABI's, as GCC and clang read them).
    Decl* def = global.definitionOf(valueTy->decl);
    auto* e = static_cast<EnumDecl*>(def ? def : valueTy->decl);
    if (e->underlying)
    {
      const Ty* u = types.unqualified(enumUnderlying(valueTy));
      isSigned = (u->kind == TyKind::Int || u->kind == TyKind::Char) && !u->isUnsigned;
    }
    else if (sh.target.windows())
      isSigned = true;
    else
    {
      isSigned = false;
      for (Decl* en : e->enumerators)
        if (en && en->kind == NodeKind::EnumeratorDecl && enumeratorValue(static_cast<EnumeratorDecl*>(en), valueTy) < 0)
          isSigned = true;
    }
  }
  if (isSigned)
  {
    std::string left = newTemp();
    fn += "  " + left + " = shl " + unitLl + " " + unit + ", " + std::to_string(unitBits - bits.shift - bits.width) + "\n";
    r = newTemp();
    fn += "  " + r + " = ashr " + unitLl + " " + left + ", " + std::to_string(unitBits - bits.width) + "\n";
  }
  else
  {
    std::string shifted = newTemp();
    fn += "  " + shifted + " = lshr " + unitLl + " " + unit + ", " + std::to_string(bits.shift) + "\n";
    r = newTemp();
    uint64_t mask = bits.width >= 64 ? ~0ull : ((1ull << bits.width) - 1);
    fn += "  " + r + " = and " + unitLl + " " + shifted + ", " + std::to_string(mask) + "\n";
  }
  // (a byte array's unit is no integer type of the member's: the value converts with the
  // member's own signedness, sign-extended by the shifts above)
  Value asUnit{r, bits.bytes ? types.intType(uint8_t(unitBits), !isSigned) : bits.unit};
  if (valueTy->kind == TyKind::Bool)
  {
    std::string b = newTemp();
    fn += "  " + b + " = icmp ne " + unitLl + " " + r + ", 0\n";
    return {b, valueTy};
  }
  // (as a cast: the unit's bits are the field's value, an enumeration's too; the program's own
  // conversions are checked where it makes them, `s.level = 1;` an error)
  return convert(asUnit, valueTy, at, true);
}

auto codegen::Emitter::storeBitfield(Value v, const std::string& addr, const BitfieldInfo& bits, Node* at) -> void
{
  std::string unitLl = llType(bits.unit, at);
  Value asUnit = convert(v, bits.unit, at, true); // (the field's value, already of its type: an enum class one's too)
  uint64_t mask = bits.width >= 64 ? ~0ull : ((1ull << bits.width) - 1);
  std::string old = newTemp(), cleared = newTemp(), masked = newTemp(), shifted = newTemp(), merged = newTemp();
  fn += "  " + old + " = load " + unitLl + ", ptr " + addr + (bits.bytes ? ", align 1" : "") + "\n";
  fn += "  " + cleared + " = and " + unitLl + " " + old + ", " + std::to_string(~(mask << bits.shift)) + "\n";
  fn += "  " + masked + " = and " + unitLl + " " + asUnit.text + ", " + std::to_string(mask) + "\n";
  fn += "  " + shifted + " = shl " + unitLl + " " + masked + ", " + std::to_string(bits.shift) + "\n";
  fn += "  " + merged + " = or " + unitLl + " " + cleared + ", " + shifted + "\n";
  fn += "  store " + unitLl + " " + merged + ", ptr " + addr + (bits.bytes ? ", align 1" : "") + "\n";
}

auto codegen::Emitter::unitMembers(const StructInfo& info, ClassDecl* c, Decl* first) -> std::vector<Decl*>
{
  std::vector<Decl*> out;
  uint32_t index = fieldIndexOf(info, first);
  if (c)
    for (Decl* m : c->members)
      if (info.bitfields.count(m) && fieldIndexOf(info, m) == index)
        out.push_back(m);
  if (out.empty())
    out.push_back(first);
  return out;
}

auto codegen::Emitter::initializeBitfield(const std::string& addr, const Ty* t, const StructInfo& info, VariableDecl* v, const BitfieldInfo& bits, bool explicitInit, Expr* value, std::set<size_t>& zeroed, Node* at) -> void
{
  size_t index = fieldIndexOf(info, v);
  std::string fieldAddr = gep(info.name, addr, index);
  if (zeroed.insert(index).second)
    fn += "  store " + llType(info.fieldTypes[index], at) + (info.fieldTypes[index]->kind == TyKind::Array ? " zeroinitializer" : " 0") + ", ptr " + fieldAddr + "\n";
  const Ty* memberTy = types.unqualified(types.stripRef(declType(v)));
  auto storeInto = [&](Expr* e, Node* where)
  {
    if (e && e->kind == NodeKind::BraceInitExpr)
    {
      auto* list = static_cast<BraceInitExpr*>(e);
      if (list->args.empty())
        return; // `{}`: zero, as the unit already is
      if (list->args.size() != 1)
        unsupported(where, "a bit field initialised with a list of several values");
      e = list->args[0];
    }
    storeBitfield(convert(rvalue(e), memberTy, where), fieldAddr, bits, where);
  };
  if (value)
    withSingleElement(value, at, [&](Expr* one) { storeInto(one, at); });
  else if (explicitInit)
    storeBitfield(Value{"0", bits.unit, false}, fieldAddr, bits, at);
  else if (v->init)
    withMemberInitializer(addr, t, v, [&](Expr* init) { storeInto(init, v); });
}

auto codegen::Emitter::addressOfDecl(Decl* d, Node* e) -> std::string
{
  if (d->kind == NodeKind::VariableDecl)
    if (std::string bound; bindingAddress(static_cast<VariableDecl*>(d), e, bound))
      return bound;
  auto it = locals.find(d);
  if (it != locals.end())
  {
    if (it->second.isRef)
    {
      std::string r = newTemp();
      fn += "  " + r + " = load ptr, ptr " + it->second.addr + "\n";
      return r;
    }
    return it->second.addr;
  }
  if (d->kind == NodeKind::VariableDecl)
  {
    if (Decl* cls = global.owningClass(d); cls && !(d->flags & DF_Static))
    {
      if (!currentClassTy)
        unsupported(e, "a member used outside a method");
      return memberObject(currentClassTy, d, thisPointer(e), e);
    }
    return globalAddress(static_cast<VariableDecl*>(d), e, qualifierClass(e, e));
  }
  if (d->kind == NodeKind::ParamDecl && !packLocals.count(d))
    refuseOutsideItsFunction(d, e); // (its own function's are its locals)
  unsupported(e, "taking the address of " + qualifiedName(d) + " (" + nodeKindName(d->kind) + (packLocals.count(d) ? ", a parameter pack outside its expansion" : "") + ")");
}

auto codegen::Emitter::memberObject(const Ty* classTy, Decl* d, const std::string& object, Node* e, const BitfieldInfo** bits) -> std::string
{
  std::string addr;
  if (!fieldAddress(classTy, d, object, addr, e, bits))
  {
    // A `when` member on a class template's parameters that this instance does not have (design.md
    // 19): a use of it is an error in the instance being built.
    if (d->when && d->whenKnown < 0 && d->kind == NodeKind::VariableDecl && !static_cast<VariableDecl*>(d)->name.empty())
      fail(e, "'" + sym(static_cast<VariableDecl*>(d)->name.last().symbol) + "' does not exist in " +
                  types.toString(types.unqualified(types.stripRef(classTy)), &symbols) + ": its `when` condition does not hold there (design.md 19)");
    unsupported(e, "a member that is not a field of the object's class");
  }
  const Ty* ft = fieldTypeIn(classTy, d, e);
  if (!ft)
    ft = d->kind == NodeKind::VariableDecl ? declType(d) : nullptr;
  if (ft && ft->isReference())
  {
    std::string referenced = newTemp();
    fn += "  " + referenced + " = load ptr, ptr " + addr + "\n";
    return referenced;
  }
  return addr;
}

auto codegen::Emitter::implicitCopyOrMoveTakes(ClassDecl* c, const Ty* t, const Ty* pt, Node* at) -> bool
{
  if (!pt || !pt->isReference() || !isClass(types.stripRef(pt)) || classKey(types.stripRef(pt)) != classKey(t))
    return false;
  bool copyCtor = false, moveCtor = false, assignment = false, destructor = false;
  TemplateArgs args = global.classArgsOf(canonicalClass(t, at));
  ArgsScope scope(*this, &args);
  for (Decl* m : c->members)
  {
    if (m->kind != NodeKind::FunctionDecl)
      continue;
    auto* f = static_cast<FunctionDecl*>(m);
    if (f->flags & DF_Destructor)
      destructor = true;
    else if (!f->name.empty() && f->name.last().special == 2 && P(f->name.last().opToken) == P::Assign)
    {
      // Only a copy or move assignment takes the implicit move away, not `operator=(int)`.
      const Ty* q = f->params.size() == 1 ? declType(f->params[0]) : nullptr;
      assignment = assignment || (q && isClass(types.stripRef(q)) && classKey(types.stripRef(q)) == classKey(t));
    }
    else if ((f->flags & DF_Constructor) && f->templateParams.empty() && f->params.size() == 1)
    {
      const Ty* q = declType(f->params[0]);
      if (q && q->isReference() && isClass(types.stripRef(q)) && classKey(types.stripRef(q)) == classKey(t))
        (q->kind == TyKind::RRef ? moveCtor : copyCtor) = true;
    }
  }
  if (pt->kind == TyKind::RRef)
    return !copyCtor && !moveCtor && !assignment && !destructor;
  return !copyCtor && types.stripRef(pt)->isConst; // `const T&`; a non-const T& binds better than the copy's
}

auto codegen::Emitter::moveConstruct(const std::string& dst, const std::string& src, const Ty* t, Node* at) -> void
{
  ClassDecl* c = classDef(t->decl, at);
  TemplateArgs args = global.classArgsOf(canonicalClass(t, at));
  FunctionDecl* move = nullptr;
  bool defaultedMove = false;
  {
    ArgsScope scope(*this, &args);
    for (Decl* m : c->members)
    {
      if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Constructor))
        continue;
      auto* f = static_cast<FunctionDecl*>(m);
      if (f->params.size() != 1)
        continue;
      const Ty* pt = declType(f->params[0]);
      bool ownMove = pt && pt->kind == TyKind::RRef && isClass(pt->inner) && classKey(pt->inner) == classKey(t);
      if (f->flags & DF_Default)
      {
        defaultedMove = defaultedMove || ownMove; // `T(T&&) = default`: the memberwise move below
        continue;
      }
      if (ownMove)
        move = f;
    }
  }
  if (move && !(move->flags & DF_Delete))
  {
    emitCall("", "void " + useFunction(move, args, canonicalClass(t, at)) + "(ptr " + dst + ", ptr " + src + ")");
    return;
  }
  if (!move && !defaultedMove && !implicitCopyOrMoveTakes(c, t, types.rref(types.unqualified(t)), at))
  {
    // No move constructor, not even an implicit one: a constructor template taking the rvalue
    // as a T&& is the one C++ chooses, over a copy constructor's const T& (`template<typename...
    // Args> Str(Args&&...)` beside an explicit copy constructor: ImmutableStringTemplate's moves
    // were copies).
    const Ty* rvalueTy = types.rref(types.unqualified(t));
    FunctionDecl* chosen = findConstructor(t, {rvalueTy}, at);
    if (chosen && !chosen->templateParams.empty() && !(chosen->flags & DF_Delete) && !global.isExplicit(chosen, args))
    {
      TemplateArgs ctorArgs = args;
      global.deduceArgs(chosen, {rvalueTy}, ctorArgs);
      const Ty* pt;
      {
        ArgsScope scope(*this, &ctorArgs);
        pt = declType(chosen->params[0]);
      }
      if (pt && pt->kind == TyKind::Pack)
        pt = pt->args.size() == 1 ? pt->args[0] : nullptr;
      if (pt && pt->kind == TyKind::RRef && isClass(pt->inner) && classKey(pt->inner) == classKey(t))
      {
        emitCall("", "void " + useFunction(chosen, ctorArgs, canonicalClass(t, at)) + "(ptr " + dst + ", ptr " + src + ")");
        return;
      }
    }
  }
  if (defaultedMove && !c->isTypedUnion)
  {
    // `explicit Animation(Animation&&) = default` beside a deleted copy constructor: the
    // members moved, the copy constructor not looked at (a move is no copy).
    memberwiseConstruct(dst, src, t, at, true);
    return;
  }
  // The user copy constructor if any, else memberwise: moves where the class has its implicit move
  // constructor, copies where it has none, its source left as it is (a destructor, copy
  // constructor or copy assignment of its own, [class.copy.ctor] 8).
  copyConstruct(dst, src, t, at, c->isTypedUnion || implicitCopyOrMoveTakes(c, t, types.rref(types.unqualified(t)), at));
}

auto codegen::Emitter::copyConstruct(const std::string& dst, const std::string& src, const Ty* t, Node* at, bool move) -> void
{
  ClassDecl* c = classDef(t->decl, at);
  if (c->isTypedUnion)
  {
    copyAlternative(c, t, dst, src, move, at);
    return;
  }
  TemplateArgs args = global.classArgsOf(canonicalClass(t, at));
  FunctionDecl* copy = nullptr;
  bool hasMove = false, defaultedCopy = false;
  {
    ArgsScope scope(*this, &args);
    for (Decl* m : c->members)
    {
      if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Constructor))
        continue;
      auto* f = static_cast<FunctionDecl*>(m);
      if (f->params.size() != 1)
        continue;
      const Ty* pt = declType(f->params[0]);
      if (!pt || !pt->isReference() || !isClass(pt->inner) || classKey(pt->inner) != classKey(t))
        continue;
      if (f->flags & DF_Default)
      {
        // `T(const T&) = default`: the memberwise copy below, whatever else the class declares
        // (BuildResult: a move constructor and a destructor of its own besides).
        defaultedCopy = defaultedCopy || pt->kind == TyKind::LRef;
        continue;
      }
      if (pt->kind == TyKind::LRef)
        copy = f; // `T(const T&)`; the move constructor `T(T&&)` would empty the source
      else
        hasMove = true;
    }
  }
  if (copy && (copy->flags & DF_Delete))
    fail(at, "copying " + types.toString(types.unqualified(t), &symbols) + ", whose copy constructor is deleted (move it instead)"); // (the program's mistake, no gap: as errors print it, `Item^`)
  if (!copy && hasMove && !defaultedCopy)
    fail(at, "copying " + types.toString(types.unqualified(t), &symbols) + ", which has a move constructor but no copy constructor");
  if (copy)
  {
    emitCall("", "void " + useFunction(copy, args, canonicalClass(t, at)) + "(ptr " + dst + ", ptr " + src + ")");
    return;
  }
  // A destructor of its own leaves the class its implicit copy constructor (C++ only deprecates
  // it): the members copied (Achievement's `new Achievement(*this)`, InputAction's ActionData).
  // A declared move assignment deletes it; and a union with a destructor of its own manages its
  // member's lifetime (an Optional's storage), whose bytes a copy would share.
  if (!defaultedCopy && declaresMoveMember(c, t, true, at))
    unsupported(at, "copying " + qualifiedName(c) + ", which declares a move assignment but no copy constructor");
  if (!defaultedCopy && c->keyword == Kw::union_ && findDestructor(c))
    unsupported(at, "copying " + qualifiedName(c) + ", a union with a destructor but no copy constructor");
  // A base's or member's copy constructor deleted deletes this class's implicit one (or a defaulted
  // one) as C++ does: no copy of its bytes (until 2026-11-05 `U b = a;` copied a member T whose
  // `T(const T&) = delete` refused a copy of T alone).
  if (!move)
    if (std::string why = implicitCopyDeletedBy(t, at); !why.empty())
      fail(at, "copying " + types.toString(types.unqualified(t), &symbols) + ", whose implicit copy constructor is deleted: " + why);
  memberwiseConstruct(dst, src, t, at, move);
}

auto codegen::Emitter::implicitCopyDeletedBy(const Ty* t, Node* at, int depth) -> std::string
{
  t = types.unqualified(types.stripRef(t));
  if (!isClass(t) || isLambda(t) || depth > 16)
    return "";
  const StructInfo& info = structFor(t, at);
  auto ofPart = [&](const Ty* pt, const std::string& what) -> std::string
  {
    if (pt->isReference())
      return ""; // (a reference member copies the reference, never the object it refers to)
    pt = types.unqualified(pt);
    while (pt->kind == TyKind::Array && pt->inner)
      pt = types.unqualified(pt->inner);
    if (!isClass(pt) || isLambda(pt))
      return "";
    ClassDecl* pc = classDef(pt->decl, at);
    if (pc->isTypedUnion)
      return "";
    TemplateArgs args = global.classArgsOf(canonicalClass(pt, at));
    ArgsScope scope(*this, &args);
    bool written = false;
    for (Decl* m : pc->members)
    {
      if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Constructor) || !m->templateParams.empty() || static_cast<FunctionDecl*>(m)->params.size() != 1)
        continue;
      const Ty* param = declType(static_cast<FunctionDecl*>(m)->params[0]);
      if (!param || param->kind != TyKind::LRef || !isClass(param->inner) || classKey(param->inner) != classKey(pt))
        continue;
      if (m->flags & DF_Delete)
        return what + " of type " + types.toString(pt, &symbols) + ", whose copy constructor is deleted";
      if (!(m->flags & DF_Default))
        written = true;
    }
    if (written)
      return "";
    std::string inner = implicitCopyDeletedBy(pt, at, depth + 1);
    return inner.empty() ? "" : what + " of type " + types.toString(pt, &symbols) + ", " + inner;
  };
  for (auto& [baseDef, index] : info.bases)
    if (std::string why = ofPart(info.fieldTypes[index], "its base"); !why.empty())
      return why;
  if (info.isUnion)
    return "";
  for (Decl* d : info.fieldDecls)
    if (d->kind == NodeKind::VariableDecl && !(global.typeOfDecl(d) && global.typeOfDecl(d)->isReference()))
      if (std::string why = ofPart(info.fieldTypes[fieldIndexOf(info, d)], "its member " + (static_cast<VariableDecl*>(d)->name.empty() ? std::string("?") : sym(static_cast<VariableDecl*>(d)->name.last().symbol)));
          !why.empty())
        return why;
  return "";
}

auto codegen::Emitter::declaresMoveMember(ClassDecl* c, const Ty* t, bool assignment, Node* at) -> bool
{
  TemplateArgs args = global.classArgsOf(canonicalClass(t, at));
  ArgsScope scope(*this, &args);
  for (Decl* m : c->members)
  {
    if (m->kind != NodeKind::FunctionDecl)
      continue;
    auto* f = static_cast<FunctionDecl*>(m);
    bool isAssignment = !f->name.empty() && f->name.last().special == 2 && P(f->name.last().opToken) == P::Assign;
    if ((assignment ? !isAssignment : !(f->flags & DF_Constructor)) || f->params.size() != 1)
      continue;
    const Ty* pt = declType(f->params[0]);
    if (pt && pt->kind == TyKind::RRef && isClass(pt->inner) && classKey(pt->inner) == classKey(t))
      return true;
  }
  return false;
}

auto codegen::Emitter::memberwiseConstruct(const std::string& dst, const std::string& src, const Ty* t, Node* at, bool move) -> void
{
  // Bitwise when nothing needs destruction and no base or member has a copy or move constructor
  // of its own (agui::GenericTargetable's copy targets nothing: a widget copied bitwise shared
  // the original's targeters).
  if (!needsDestruction(t, at) && relocatesBitwise(t, at))
  {
    if (isClass(t) && structFor(t, at).empty)
      return; // no bytes to copy (and as a base, none of its own: its byte is the next member's)
    copyBytes(dst, src, t, at);
    if (isClass(t))
      storeVptrs(dst, structFor(t, at)); // the source may be of a derived class (a slicing copy)
    return;
  }
  // Memberwise: bases and members in order, each with its own copy semantics, each destroyed if a
  // later one's copy throws.
  const StructInfo& info = structFor(t, at);
  InlineParts parts(*this, at);
  for (auto& [baseDef, index] : info.bases)
  {
    if (move)
      moveConstruct(gep(info.name, dst, index), gep(info.name, src, index), info.fieldTypes[index], at);
    else
      copyConstruct(gep(info.name, dst, index), gep(info.name, src, index), info.fieldTypes[index], at);
    parts.built(gep(info.name, dst, index), info.fieldTypes[index]);
  }
  storeVptrs(dst, info); // after the bases, whose constructors pointed it at theirs
  for (Decl* d : info.fieldDecls)
  {
    uint32_t index = fieldIndexOf(info, d);
    const Ty* ft = info.fieldTypes[index];
    std::string to = gep(info.name, dst, index), from = gep(info.name, src, index);
    if (isClass(ft) && move)
      moveConstruct(to, from, ft, at);
    else if (isClass(ft))
      copyConstruct(to, from, ft, at);
    else if (isLambda(ft))
      copyClosure(to, from, ft, at, move); // FunctionImpl<F>'s `F f`
    else if (ft->kind == TyKind::Array)
      transferArray(to, from, ft, at, move, false);
    else
    {
      copyBytes(to, from, ft, at);
    }
    if (!info.isUnion)
      parts.built(to, ft);
  }
}

auto codegen::Emitter::transferArray(const std::string& dst, const std::string& src, const Ty* t, Node* at, bool move, bool assign) -> void
{
  t = types.unqualified(t);
  if ((!needsDestruction(t, at) && (assign ? assignsBitwise(t, at) : relocatesBitwise(t, at))) || t->count == 0)
  {
    copyBytes(dst, src, t, at);
    return;
  }
  InlineParts parts(*this, at); // (constructing, not assigning: the elements built so far)
  auto transferAt = [&](const std::string& index)
  {
    std::string to = newTemp(), from = newTemp();
    fn += "  " + to + " = getelementptr inbounds " + llType(t, at) + ", ptr " + dst + ", i64 0, i64 " + index + "\n";
    fn += "  " + from + " = getelementptr inbounds " + llType(t, at) + ", ptr " + src + ", i64 0, i64 " + index + "\n";
    const Ty* element = types.unqualified(t->inner);
    if (element->kind == TyKind::Array)
      transferArray(to, from, element, at, move, assign);
    else if (assign)
      assignObject(to, from, element, at, move);
    else if (move)
      moveConstruct(to, from, element, at);
    else
      copyConstruct(to, from, element, at);
    return to;
  };
  if (t->count <= 8)
  {
    for (uint32_t i = 0; i < t->count; ++i)
    {
      std::string to = transferAt(std::to_string(i));
      if (!assign)
        parts.built(to, t->inner);
    }
    return;
  }
  std::string slot = "%v" + std::to_string(temp++);
  allocas += "  " + slot + " = alloca i64\n";
  fn += "  store i64 0, ptr " + slot + "\n";
  if (!assign)
    parts.builtCounted(dst, t, slot);
  std::string loopL = newLabel("array.loop"), bodyL = newLabel("array.body"), endL = newLabel("array.end");
  emitBranch(loopL);
  startBlock(loopL);
  std::string index = newTemp(), done = newTemp();
  fn += "  " + index + " = load i64, ptr " + slot + "\n";
  fn += "  " + done + " = icmp eq i64 " + index + ", " + std::to_string(t->count) + "\n";
  fn += "  br i1 " + done + ", label %" + endL + ", label %" + bodyL + "\n";
  startBlock(bodyL);
  transferAt(index);
  std::string next = newTemp();
  fn += "  " + next + " = add i64 " + index + ", 1\n";
  fn += "  store i64 " + next + ", ptr " + slot + "\n";
  emitBranch(loopL);
  startBlock(endL);
}

auto codegen::Emitter::assignObject(const std::string& dst, const std::string& src, const Ty* t, Node* at, bool move) -> void
{
  if (ClassDecl* c = typedUnionOf(t, at))
  {
    // One alternative in both: its operator=, as std::variant. Else the alternative held goes, the
    // source's alternative and tag come. Nothing for `u = u`, which copied from the alternative it
    // had destroyed (until 2026-11-06: a String's freed text).
    std::string same = newTemp(), assignL = newLabel("union.assign"), heldL = newLabel("union.same"), otherL = newLabel("union.other"),
                doneL = newLabel("union.assigned");
    fn += "  " + same + " = icmp eq ptr " + dst + ", " + src + "\n";
    fn += "  br i1 " + same + ", label %" + doneL + ", label %" + assignL + "\n";
    startBlock(assignL);
    std::string sameTag = newTemp(), toTag = loadTag(c, t, dst, at), fromTag = loadTag(c, t, src, at);
    fn += "  " + sameTag + " = icmp eq " + llType(tagType(c), at) + " " + toTag + ", " + fromTag + "\n";
    fn += "  br i1 " + sameTag + ", label %" + heldL + ", label %" + otherL + "\n";
    startBlock(heldL);
    assignHeldAlternative(c, t, dst, src, move, at);
    emitBranch(doneL);
    startBlock(otherL);
    if (move)
    {
      destroyAlternative(c, t, dst, at);
      copyAlternative(c, t, dst, src, true, at);
    }
    else
      copyOverAlternative(c, t, dst, src, at);
    emitBranch(doneL);
    startBlock(doneL);
    return;
  }
  const Ty* foundIn = nullptr;
  // From an lvalue: operator=(const T&). From a temporary or `move(x)`: operator=(T&&) if there is one.
  std::vector<const Ty*> argTypes{move ? types.rref(types.unqualified(t)) : types.lref(types.qualified(t, true, false))};
  FunctionDecl* op = declaresAssignment(t, at) ? findMemberFunction(t, "operator=", argTypes, t, at, &foundIn) : nullptr;
  // A base's operator=, the class's own taking no T (`Mixed& operator=(int)`): the class's implicit
  // assignment, which hides the base's (C++ [class.copy.assign]) and assigns the class's own
  // members too, unless a using-declaration brought the base's in.
  if (op && foundIn && foundIn != canonicalClass(t, at) && !declaresAssignment(t, at, true))
    op = nullptr;
  bool defaulted = op && (op->flags & DF_Default);
  // Without a move assignment, declared or implicit, a move is a copy, its source left as it is
  // ([class.copy.assign] 4); `operator=(const T&) = default` is the copy assignment.
  if (defaulted)
  {
    TemplateArgs args = global.classArgsOf(foundIn);
    ArgsScope scope(*this, &args);
    const Ty* pt = op->params.size() == 1 ? declType(op->params[0]) : nullptr;
    move = move && pt && pt->kind == TyKind::RRef;
    op = nullptr; // the memberwise assignment below
  }
  else if (move && !op && !implicitCopyOrMoveTakes(classDef(t->decl, at), t, types.rref(types.unqualified(t)), at))
    move = false;
  if (op)
  {
    if (op->flags & DF_Delete)
      unsupported(at, "assignment to " + qualifiedName(t->decl) + " through its deleted operator=");
    std::string thisArg = dst;
    if (canonicalClass(t, at) != foundIn)
      baseAddress(t, foundIn, dst, thisArg, at);
    std::string name = useFunction(op, global.classArgsOf(foundIn), foundIn);
    Signature s;
    {
      TemplateArgs args = global.classArgsOf(foundIn);
      ArgsScope scope(*this, &args);
      s = signatureOf(op);
    }
    if (s.returnType == "void")
      emitCall("", "void " + name + "(ptr " + thisArg + ", ptr " + src + ")");
    else
      emitCall(newTemp() + " = ", s.returnType + " " + name + "(ptr " + thisArg + ", ptr " + src + ")");
    return;
  }
  if (!needsDestruction(t, at) && assignsBitwise(t, at))
  {
    if (isClass(t) && structFor(t, at).empty)
      return; // no state, and as a base maybe no bytes of its own (the next member's)
    std::vector<std::string> vptrs = loadVptrs(dst, structFor(t, at));
    copyBytes(dst, src, t, at);
    restoreVptrs(vptrs);
    return;
  }
  // An implicit move assignment C++ defines as deleted, as a base's or a member's move assignment is
  // (`CopyOnly& operator=(CopyOnly&&) = delete`), is no candidate ([over.match.funcs] 8, [class.copy.assign]
  // 7): the implicit copy assignment takes the rvalue, base by base and member by member, every one copied.
  // Until 2026-11-08 the member was moved through its deleted operator=, an error (D's spec
  // classes/implicit_move_assignment_deleted_copies_memberwise). After the bitwise case, which is the same
  // either way.
  if (move && !defaulted && partAssignmentDeleted(t, true, at))
    move = false;
  // A destructor of its own leaves the class its implicit copy assignment (C++ only deprecates
  // it), a defaulted one is memberwise anyway (Trigger); a declared move constructor deletes the
  // implicit copy assignment.
  if (!defaulted && !move && declaresMoveMember(classDef(t->decl, at), t, false, at))
    unsupported(at, "assignment to " + qualifiedName(t->decl) + ", which declares a move constructor but no operator=");
  if (!defaulted && classDef(t->decl, at)->keyword == Kw::union_ && findDestructor(classDef(t->decl, at)))
    unsupported(at, "assignment to " + qualifiedName(t->decl) + ", a union with a destructor but no operator=");
  memberwiseAssign(dst, src, t, at, move);
}

auto codegen::Emitter::declaresAssignment(const Ty* t, Node* at, bool usingOnly) -> bool
{
  ClassDecl* c = classDef(t->decl, at);
  for (Decl* m : c->members)
  {
    if (m->kind == NodeKind::FunctionDecl)
    {
      if (usingOnly)
        continue;
      auto* f = static_cast<FunctionDecl*>(m);
      if (!f->name.empty() && f->name.last().special == 2 && P(f->name.last().opToken) == P::Assign)
        return true;
    }
    else if (m->kind == NodeKind::UsingNameDecl)
    {
      const QualifiedName& name = static_cast<UsingNameDecl*>(m)->name;
      if (!name.empty() && name.last().special == 2 && P(name.last().opToken) == P::Assign)
        return true;
    }
  }
  return false;
}

auto codegen::Emitter::assignmentDeleted(const Ty* t, bool move, Node* at, int depth) -> bool
{
  t = types.unqualified(types.stripRef(t));
  while (t->kind == TyKind::Array && t->inner)
    t = types.unqualified(t->inner);
  if (!isClass(t) || isLambda(t) || depth > 16 || typedUnionOf(t, at))
    return false;
  const Ty* foundIn = nullptr;
  std::vector<const Ty*> argTypes{move ? types.rref(t) : types.lref(types.qualified(t, true, false))};
  FunctionDecl* op = declaresAssignment(t, at) ? findMemberFunction(t, "operator=", argTypes, t, at, &foundIn) : nullptr;
  if (op && foundIn && foundIn != canonicalClass(t, at) && !declaresAssignment(t, at, true))
    op = nullptr; // (a base's, which the implicit one hides: as assignObject)
  if (op && !(op->flags & DF_Default))
    return (op->flags & DF_Delete) != 0; // its own: deleted, or one that assigns as it is written
  ClassDecl* c = classDef(t->decl, at);
  bool movesParts = false; // the implicit or defaulted one that takes the value is a move assignment
  if (op)
  {
    TemplateArgs args = global.classArgsOf(foundIn);
    ArgsScope scope(*this, &args);
    const Ty* pt = op->params.size() == 1 ? declType(op->params[0]) : nullptr;
    movesParts = pt && pt->kind == TyKind::RRef;
  }
  else if (move && implicitCopyOrMoveTakes(c, t, types.rref(t), at))
    movesParts = true;
  else if (declaresMoveMember(c, t, false, at) || declaresMoveMember(c, t, true, at))
    return true; // (a declared move constructor or move assignment deletes the implicit copy assignment)
  if (!movesParts)
    return partAssignmentDeleted(t, false, at, depth);
  return partAssignmentDeleted(t, true, at, depth) && assignmentDeleted(t, false, at, depth + 1);
}

auto codegen::Emitter::partAssignmentDeleted(const Ty* t, bool move, Node* at, int depth) -> bool
{
  const StructInfo& info = structFor(t, at);
  for (auto& [baseDef, index] : info.bases)
    if (assignmentDeleted(info.fieldTypes[index], move, at, depth + 1))
      return true;
  if (info.isUnion)
    return false;
  for (Decl* d : info.fieldDecls)
    if (d->kind == NodeKind::VariableDecl && !(global.typeOfDecl(d) && global.typeOfDecl(d)->isReference()) &&
        assignmentDeleted(info.fieldTypes[fieldIndexOf(info, d)], move, at, depth + 1))
      return true;
  return false;
}

auto codegen::Emitter::memberwiseAssign(const std::string& dst, const std::string& src, const Ty* t, Node* at, bool move) -> void
{
  const StructInfo& info = structFor(t, at);
  for (auto& [baseDef, index] : info.bases)
    assignObject(gep(info.name, dst, index), gep(info.name, src, index), info.fieldTypes[index], at, move);
  for (Decl* d : info.fieldDecls)
  {
    uint32_t index = fieldIndexOf(info, d);
    const Ty* ft = info.fieldTypes[index];
    std::string to = gep(info.name, dst, index), from = gep(info.name, src, index);
    if (isClass(ft))
      assignObject(to, from, ft, at, move);
    else if (ft->kind == TyKind::Array)
      transferArray(to, from, ft, at, move, true);
    else
    {
      copyBytes(to, from, ft, at);
    }
  }
}

auto codegen::Emitter::conditionalOfPrvalues(ConditionalExpr* c, const Ty* t, Node* at) -> bool
{
  if (!c->then || !c->else_)
    return false;
  if (c->else_->kind == NodeKind::DefaultValueExpr)
    return true; // (`T a = p?->member;`: the copy of the member, or the default, is the object)
  bool prvalue = false;
  for (Expr* branch : {c->then, c->else_})
  {
    const Ty* bt = branch->ty ? withCurrentArgs(branch->ty) : nullptr;
    const Ty* object = bt ? types.stripRef(bt) : nullptr;
    if (!object || !isClass(object) || types.isDependent(object) || canonicalClass(types.unqualified(object), at) != canonicalClass(types.unqualified(t), at))
      return false;
    prvalue = prvalue || (!bt->isReference() && !isLvalueExpression(branch));
  }
  return prvalue;
}

auto codegen::Emitter::builtinComma(Expr* e) const -> BinaryExpr*
{
  while (e && e->kind == NodeKind::ParenExpr)
    e = static_cast<ParenExpr*>(e)->inner;
  auto* b = e && e->kind == NodeKind::BinaryExpr ? static_cast<BinaryExpr*>(e) : nullptr;
  return b && !b->isKeyword && P(b->op) == P::Comma && (verifyDecisions() || !recordedOperator(b)) ? b : nullptr;
}

auto codegen::Emitter::initializeFrom(const std::string& addr, const Ty* t, Expr* init, Node* at, ConversionContext context) -> void
{
  bool copyInit = context == ConversionContext::Copy;
  t = types.stripRef(t);
  if (init && init->kind == NodeKind::DefaultValueExpr)
  {
    // `a?->b` when a is not set: `T()`, zeroed and then constructed.
    if (isClass(t))
      if (ClassDecl* c = classDef(t->decl, at); hasUserConstructors(c) && !findConstructor(t, {}, at) && !inheritedConstructorBase(c))
        fail(at, "`?->` gives a default value when its object is not set, and " + qualifiedName(c) + " has no default constructor");
    storeZero(t, addr, at);
    if (isClass(t))
      constructInto(addr, t, {}, at);
    return;
  }
  // `[callback{::move(callback)}]`, `int x{5}`: one value in braces initialises what is not a
  // class as the value itself would.
  // A direct-list-initialization's (`float x{object};`, not a copy-initialization) takes an explicit conversion
  // function to t too, as a direct-initialization does ([over.match.conv] 1.1; until 2026-11-08 only the implicit ones).
  bool directList = false;
  if (init && init->kind == NodeKind::BraceInitExpr && !isClass(t) && t->kind != TyKind::Array)
    if (auto* list = static_cast<BraceInitExpr*>(init); list->args.size() == 1 && list->designators.empty() &&
                                                        list->args[0]->kind != NodeKind::BraceInitExpr)
    {
      init = list->args[0];
      directList = context == ConversionContext::Direct && init->ty && init->ty->isKnown() && isClass(types.stripRef(init->ty));
    }
  // A literal 0 for a pointer (an error in kpp, design.md 4; a warning under --zero-null-warnings),
  // here where the walk does not see it: a brace list's member (`Heap hh = { 0, 0, 0 };`). Reported
  // at the 0 itself, the null stored as C++ would.
  if (init && t->kind == TyKind::Pointer && nullPointerConstant(init))
  {
    zeroIsNoPointer(init);
    storeZero(t, addr, at);
    return;
  }
  if (init && t->kind == TyKind::MemberPointer && nullPointerConstant(init))
  {
    // (The same for a member pointer, whose null is no zero: -1 for a data member, as MSVC has it.)
    if (!zeroIsNoPointer(init))
      store({t->inner && t->inner->kind == TyKind::Function ? (memberFunctionPointerAdjusts(t, at) ? "zeroinitializer" : "null") : "-1", t}, addr, t, at);
    return;
  }
  if (BinaryExpr* comma = isClass(t) || isLambda(t) ? builtinComma(init) : nullptr)
  {
    // `T a = (x, T(4));`, `return (side, make());`: the comma is its right operand once the left one
    // ran, so a prvalue there builds the object itself ([expr.comma] 1, guaranteed elision), as a
    // class that cannot move needs. Until 2026-11-07 it was built in a temporary and moved.
    rvalue(comma->lhs); // (its temporaries live to the end of the full-expression)
    initializeFrom(addr, t, comma->rhs, at, context);
    return;
  }
  if (isLambda(t))
  {
    initializeClosure(addr, t, init, at);
    return;
  }
  if (isClass(t) && init && init->kind == NodeKind::BraceInitExpr && isInitializerListType(t))
  {
    // `InitializerList<int> numbers = {1, 2, 3}`, `const auto entityNames = {"pipe", "tank"}`:
    // the list over an array that lives as long as the variable.
    initializerListTemporary(t, static_cast<BraceInitExpr*>(init), at, addr);
    return;
  }
  if (isClass(t) && init && init->kind == NodeKind::ConditionalExpr && conditionalOfPrvalues(static_cast<ConditionalExpr*>(init), t, at))
  {
    // `T a = c ? T(1) : make(2);`: the chosen branch builds the object itself, no temporary
    // moved from (guaranteed elision, [expr.cond]/7 and [dcl.init]/17.6.1); `c ? make() : b`
    // copies b into it, the prvalue's conversion of the lvalue branch.
    auto* c = static_cast<ConditionalExpr*>(init);
    Value cond = convert(rvalue(c->cond), types.boolType(), at, false, ConversionContext::Direct);
    std::string tL = newLabel("cond.then"), eL = newLabel("cond.else"), jL = newLabel("cond.end");
    fn += "  br i1 " + cond.text + ", label %" + tL + ", label %" + eL + "\n";
    for (int branch = 0; branch < 2; ++branch)
    {
      startBlock(branch == 0 ? tL : eL);
      size_t temps = statementTemps.size();
      initializeFrom(addr, t, branch == 0 ? c->then : c->else_, at, context);
      keepOperandTemps(temps); // (a branch's own temporaries, built only when it ran)
      emitBranch(jL);
    }
    startBlock(jL);
    return;
  }
  if (isClass(t))
  {
    // `T a = T(x)` and `T a = makeT()` build the object in place; an initializer of another
    // class (`Target a = make();` returning a Source) goes through the converting constructor below.
    const Ty* initTy = withCurrentArgs(init->ty);
    bool initIsClass = initTy && isClass(types.stripRef(initTy));
    // A template parameter the type checker left in (`shared_from_this()` of a base class
    // template, typed SharedPtr<T>): the value's own class decides once it is emitted.
    bool dependent = initIsClass && types.isDependent(initTy);
    bool sameClass = initIsClass && !dependent && canonicalClass(initTy, at) == canonicalClass(t, at);
    if (ClassDecl* u = typedUnionOf(t, at); u && !sameClass && !dependent && init->kind != NodeKind::BraceInitExpr && initTy && initTy->isKnown())
    {
      // `AIState s = Patrolling{1};`, `return Attacking{n};`: the alternative of the value's
      // type, constructed from it (design.md 8.5).
      size_t index = alternativeForValue(u, t, initTy, at);
      storeZero(t, addr, at);
      constructAlternative(u, t, addr, index, init, at);
      return;
    }
    if (init->kind == NodeKind::FunctionalCastExpr && sameClass)
    {
      // `T(list)`: the list is a constructor's argument; `T{...}` has its list as the one argument.
      directInitialize(addr, t, static_cast<FunctionalCastExpr*>(init)->args, static_cast<FunctionalCastExpr*>(init)->braced, at,
                       !static_cast<FunctionalCastExpr*>(init)->braced);
      return;
    }
    if (init->kind == NodeKind::NamedCastExpr && sameClass && static_cast<NamedCastExpr*>(init)->castKind == Kw::static_cast_ && !init->ty->isReference())
    {
      // `static_cast<T>(e)`, a prvalue of T: the object itself direct-initialized from e, as `T(e)`
      // is (C++17's guaranteed elision), never a temporary moved into it.
      Expr* operand = static_cast<NamedCastExpr*>(init)->operand;
      directInitialize(addr, t, Span<Expr*>{&operand, 1}, false, at, true);
      return;
    }
    if (init->kind == NodeKind::BraceInitExpr)
    {
      Expr* one = init; // constructInto sees the whole list: an InitializerList constructor, designators, or the fields
      constructInto(addr, t, Span<Expr*>{&one, 1}, at);
      return;
    }
    if (init->kind == NodeKind::CallExpr && sameClass && !init->ty->isReference())
    {
      // The result is built in place; a call that put it anywhere else would leave the
      // variable unset (`Chunk c = maker(5)` through operator() once did).
      Value v = callValue(static_cast<CallExpr*>(init), addr);
      if (isClass(v.ty) && v.text != addr)
        unsupported(at, "a call that built its " + types.toString(t, &symbols) + " result somewhere else than the object it initialises");
      return;
    }
    if (init->kind == NodeKind::CallExpr && dependent && !init->ty->isReference())
    {
      Value v = callValue(static_cast<CallExpr*>(init), "");
      if (!isClass(v.ty) || !v.isTemporary || canonicalClass(v.ty, at) != canonicalClass(t, at))
        unsupported(at, "initialising a " + types.toString(t, &symbols) + " from a call returning " + (v.ty ? types.toString(v.ty, &symbols) : std::string("?")));
      moveTemporary(addr, v, t, at);
      return;
    }
    if (init->kind == NodeKind::ParenExpr)
    {
      initializeFrom(addr, t, static_cast<ParenExpr*>(init)->inner, at, context);
      return;
    }
    // A call the checker could not type, evaluated once: an SSE operation clang's intrin.h has as a
    // macro (_mm_shuffle_ps) initialising a class that wraps its vector (float32x4) is made into it
    // by the class's constructor (constructFromVector); any other value is taken below.
    std::optional<Value> evaluated;
    if (init->kind == NodeKind::CallExpr && (!init->ty || !init->ty->isKnown()))
    {
      evaluated = rvalue(init);
      if (evaluated->ty && evaluated->ty->kind == TyKind::Vector)
      {
        constructFromVector(addr, t, *evaluated, true, at);
        return;
      }
    }
    if (!evaluated && init->ty && init->ty->isKnown() && !isClass(types.stripRef(init->ty)) && isClass(t))
    {
      // A non-class value: through a converting constructor (`String s = "text"`).
      Expr* one = init;
      constructInto(addr, t, Span<Expr*>{&one, 1}, at, false, copyInit);
      return;
    }
    if (!evaluated && initIsClass && !dependent && !sameClass)
    {
      // Another class: through a converting constructor, a base, or its conversion operator
      // (`String text = vec;` with `operator String()`), never by copying it as a t.
      Expr* one = init;
      constructInto(addr, t, Span<Expr*>{&one, 1}, at, false, copyInit);
      return;
    }
    // `T a = b + c;` and `T a = -b;`, an operator function's prvalue, builds the object itself, as a
    // call does (guaranteed elision): no temporary moved from, which a class with a copy or
    // destructor of its own sees, or a copy or move constructor of its own (`Outer m = o + n;` with a user
    // move constructor moved the result into m until 2026-11-09, found beside `This`). (Other classes keep
    // the call the inliner can take.)
    bool operatorInPlace = !evaluated && sameClass && (init->kind == NodeKind::BinaryExpr || init->kind == NodeKind::UnaryExpr) && !init->ty->isReference() &&
                           !isLvalueExpression(init) && (needsDestruction(t, at) || !assignsBitwise(t, at) || !relocatesBitwise(t, at));
    const Node* outerOperator = inPlaceOperator;
    std::string outerSlot = inPlaceSlot;
    if (operatorInPlace)
    {
      inPlaceOperator = init;
      inPlaceSlot = addr;
    }
    Value src = evaluated ? *evaluated : rvalue(init);
    if (operatorInPlace)
    {
      inPlaceOperator = outerOperator;
      inPlaceSlot = outerSlot;
      if (src.text == addr)
        return;
    }
    if (!isClass(src.ty))
      unsupported(at, "initialising a class from a non-class value");
    if (canonicalClass(src.ty, at) != canonicalClass(t, at))
      unsupported(at, "initialising a " + types.toString(t, &symbols) + " from a " + types.toString(src.ty, &symbols) + " whose type the checker left open");
    if (src.isTemporary)
      moveTemporary(addr, src, t, at);
    else if (movable(init))
      moveConstruct(addr, src.text, t, at); // `T tmp = move(a);`
    else
      copyConstruct(addr, src.text, t, at);
    return;
  }
  // `char text[16] = "edit me"` (also in braces or parentheses), `wchar_t name[8] = L"x"`: the
  // literal's characters and its terminating zero, the rest of the array zero.
  Expr* literal = init;
  while (literal && literal->kind == NodeKind::ParenExpr)
    literal = static_cast<ParenExpr*>(literal)->inner;
  if (literal && literal->kind == NodeKind::BraceInitExpr && static_cast<BraceInitExpr*>(literal)->args.size() == 1)
    literal = static_cast<BraceInitExpr*>(literal)->args[0];
  if (t->kind == TyKind::Array && (types.unqualified(t->inner)->kind == TyKind::Char || types.unqualified(t->inner)->kind == TyKind::Int) &&
      literal && literal->kind == NodeKind::LiteralExpr && static_cast<LiteralExpr*>(literal)->literalKind == TokenKind::String)
  {
    auto* l = static_cast<LiteralExpr*>(literal);
    uint32_t unitBytes = 1;
    size_t units = stringLiteralUnits(l, unitBytes);
    if (sizeOf(t->inner, at) != unitBytes)
      fail(at, "an array of " + types.toString(t->inner, &symbols) + " initialised from a string literal of " + std::to_string(unitBytes * 8) + "-bit characters");
    if (units > t->count)
      fail(at, "a string literal of " + std::to_string(units) + " characters initialises an array of " + std::to_string(t->count));
    storeZero(t, addr, at);
    if (declaredNames.claim("@llvm.memcpy.p0.p0.i64"))
      decls += "declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)\n";
    fn += "  call void @llvm.memcpy.p0.p0.i64(ptr " + addr + ", ptr " + stringConstant(l) + ", i64 " +
          std::to_string(std::min<uint64_t>(units + 1, t->count) * unitBytes) + ", i1 false)\n";
    return;
  }
  if (t->kind == TyKind::Array && init->kind == NodeKind::FunctionalCastExpr && !static_cast<FunctionalCastExpr*>(init)->braced &&
      !static_cast<FunctionalCastExpr*>(init)->args.empty())
  {
    // `int a[3](1, 2)` (C++20, [dcl.init.general] 16.5): each value initializes an element whole (the
    // checker refused one that is no element: parentheses elide no braces), the rest value-initialized.
    storeZero(t, addr, at);
    initializeArrayElements(addr, t, static_cast<FunctionalCastExpr*>(init)->args, init); // (at the values: a failure the checker said is not said again)
    return;
  }
  if (t->kind == TyKind::Array && init->kind != NodeKind::BraceInitExpr)
  {
    // `auto [a, b, c] = arr;`: a structured binding's hidden object, an array copied from the one
    // it is given, element by element by their copy constructors (moved from an xvalue, `auto [a,
    // b] = move(arr)`), as [dcl.struct.bind] 1 has it (D, 2026-11-05: the hidden object was a
    // decayed pointer until then). Of anything else an array is no copy: an error, never the
    // value stored over it as a scalar's.
    const Ty* from = init->ty ? types.unqualified(types.stripRef(withCurrentArgs(init->ty))) : nullptr;
    if (!from || from->kind != TyKind::Array || from->count != t->count || types.unqualified(from->inner) != types.unqualified(t->inner))
      unsupported(at, "an array of " + types.toString(t, &symbols) + " initialised from " + (from ? types.toString(from, &symbols) : std::string("an expression of unknown type")));
    transferArray(addr, lvalue(init), t, at, movable(init), false);
    return;
  }
  if (init->kind == NodeKind::BraceInitExpr)
  {
    auto* bi = static_cast<BraceInitExpr*>(init);
    if (bi->args.empty())
    {
      storeZero(t, addr, at);
      // `Q arr[2] = {}`, `R arr[2]{}`: each element value-initialized, its default constructor run
      // or its default member initializers applied (D, 2026-11-05: zero alone left them out).
      if (t->kind == TyKind::Array && needsConstruction(t, at))
        constructArray(addr, t, at);
      return;
    }
    if (t->kind == TyKind::Array)
    {
      // Element by element, the braces of an element elided where it takes several values (`In
      // arr[2] = {1, 2, 3, 4}`), the elements the list leaves out value-initialized, as from `{}`
      // (initializeArrayElements); the rest of a scalar array zero.
      storeZero(t, addr, at);
      initializeArrayElements(addr, t, bi->args, at);
      return;
    }
    if (bi->args.size() != 1)
      unsupported(at, "this brace initializer");
    init = bi->args[0];
  }
  if (directList)
  {
    store(convert(rvalue(init), t, at, true, ConversionContext::Direct), addr, t, at);
    return;
  }
  Value value = rvalue(init);
  if (context != ConversionContext::Unknown && isClass(value.ty))
    value = convert(value, t, at, false, context); // (`bool b = x;` takes no explicit operator bool, `bool b{x}` does)
  store(value, addr, t, at);
}

auto codegen::Emitter::withCurrentArgs(const Ty* t, int depth) -> const Ty*
{
  if (!t || !currentArgs || depth > 16 || !t->isKnown() || !types.isDependent(t))
    return t;
  switch (t->kind)
  {
    case TyKind::Param:
      for (auto& [d, a] : currentArgs->pairs)
        if (d == t->decl && a)
        {
          const Ty* bound = a;
          // (of one element's expansion: the pattern's mention of the pack, that element)
          if (a->kind == TyKind::Pack && currentArgs->elementIndex < a->args.size() &&
              std::find(currentArgs->elementPacks.begin(), currentArgs->elementPacks.end(), d) != currentArgs->elementPacks.end())
            bound = a->args[currentArgs->elementIndex];
          return types.qualified(bound, t->isConst || bound->isConst, t->isVolatile || bound->isVolatile);
        }
      return t;
    case TyKind::Pointer: return types.qualified(types.pointer(withCurrentArgs(t->inner, depth + 1)), t->isConst, t->isVolatile);
    case TyKind::LRef: return types.lref(withCurrentArgs(t->inner, depth + 1));
    case TyKind::RRef: return types.rref(withCurrentArgs(t->inner, depth + 1));
    case TyKind::Class:
    {
      std::vector<const Ty*> args;
      for (const Ty* a : t->args)
        args.push_back(withCurrentArgs(a, depth + 1));
      return types.qualified(types.classType(t->decl, Span<const Ty*>{args.data(), uint32_t(args.size())}), t->isConst, t->isVolatile);
    }
    case TyKind::Enum: // (an enumeration of a class template, named in its own code)
    {
      std::vector<const Ty*> args;
      for (const Ty* a : t->args)
        args.push_back(withCurrentArgs(a, depth + 1));
      return types.qualified(types.enumType(t->decl, Span<const Ty*>{args.data(), uint32_t(args.size())}), t->isConst, t->isVolatile);
    }
    default: return t;
  }
}

auto codegen::Emitter::enumInstance(const Ty* t) -> const Ty*
{
  t = t ? withCurrentArgs(types.unqualified(types.stripRef(t))) : nullptr;
  return t && t->kind == TyKind::Enum && !t->args.empty() && !types.isDependent(t) ? t : nullptr;
}

auto codegen::Emitter::relocatesBitwise(const Ty* t, Node* at, int depth) -> bool
{
  t = types.unqualified(types.stripRef(t));
  while (t->kind == TyKind::Array && t->inner)
    t = types.unqualified(t->inner);
  if (isLambda(t) && depth <= 32)
  {
    for (auto& [index, ft] : closureValueFields(t, at))
      if (!relocatesBitwise(ft, at, depth + 1))
        return false;
    return true;
  }
  if (!isClass(t) || depth > 32)
    return true;
  const Ty* key = canonicalClass(t, at);
  noteLayout(key);
  if (auto it = bitwiseRelocation.find(key); it != bitwiseRelocation.end())
    return it->second;
  bitwiseRelocation[key] = true; // (a member pointing back at its class is no member by value)
  ClassDecl* c = classDef(key->decl, at);
  bool bitwise = !c->isTypedUnion;
  if (bitwise)
  {
    TemplateArgs args = global.classArgsOf(key);
    ArgsScope scope(*this, &args);
    for (Decl* m : c->members)
    {
      if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Constructor) || (m->flags & (DF_Default | DF_Delete)) ||
          !m->templateParams.empty() || static_cast<FunctionDecl*>(m)->params.size() != 1)
        continue;
      const Ty* pt = declType(static_cast<FunctionDecl*>(m)->params[0]);
      if (pt && pt->isReference() && isClass(pt->inner) && classKey(pt->inner) == classKey(key))
        bitwise = false; // `IDConnector(IDConnector&& other)`, `T(const T&)` of its own
    }
  }
  if (bitwise)
  {
    const StructInfo& info = structFor(key, at);
    std::vector<const Ty*> fields = info.fieldTypes;
    for (const Ty* f : fields)
      if (!relocatesBitwise(f, at, depth + 1))
        bitwise = false;
  }
  bitwiseRelocation[key] = bitwise;
  return bitwise;
}

auto codegen::Emitter::assignsBitwise(const Ty* t, Node* at, int depth) -> bool
{
  t = types.unqualified(types.stripRef(t));
  while (t->kind == TyKind::Array && t->inner)
    t = types.unqualified(t->inner);
  if (!isClass(t) || depth > 32)
    return true;
  const Ty* key = canonicalClass(t, at);
  noteLayout(key);
  if (auto it = bitwiseAssignment.find(key); it != bitwiseAssignment.end())
    return it->second;
  bitwiseAssignment[key] = true; // (a member pointing back at its class is no member by value)
  ClassDecl* c = classDef(key->decl, at);
  bool bitwise = !c->isTypedUnion;
  if (bitwise)
  {
    TemplateArgs args = global.classArgsOf(key);
    ArgsScope scope(*this, &args);
    for (Decl* m : c->members)
    {
      if (m->kind != NodeKind::FunctionDecl || (m->flags & (DF_Default | DF_Delete)) || !m->templateParams.empty())
        continue;
      auto* f = static_cast<FunctionDecl*>(m);
      bool isAssignment = !f->name.empty() && f->name.last().special == 2 && P(f->name.last().opToken) == P::Assign;
      if (!isAssignment || f->params.size() != 1)
        continue;
      const Ty* pt = declType(f->params[0]);
      if (pt && pt->isReference() && isClass(pt->inner) && classKey(pt->inner) == classKey(key))
        bitwise = false;
    }
  }
  if (bitwise)
  {
    const StructInfo& info = structFor(key, at);
    std::vector<const Ty*> fields = info.fieldTypes;
    for (const Ty* f : fields)
      if (!assignsBitwise(f, at, depth + 1))
        bitwise = false;
  }
  bitwiseAssignment[key] = bitwise;
  return bitwise;
}

auto codegen::Emitter::movableByConstructor(const Ty* t, Node* at) -> bool
{
  ClassDecl* c = classDef(t->decl, at);
  TemplateArgs args = global.classArgsOf(canonicalClass(t, at));
  ArgsScope scope(*this, &args);
  bool declared = false, usable = false;
  for (Decl* m : c->members)
  {
    if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Constructor) || !m->templateParams.empty() ||
        static_cast<FunctionDecl*>(m)->params.size() != 1)
      continue;
    const Ty* pt = declType(static_cast<FunctionDecl*>(m)->params[0]);
    if (!pt || !pt->isReference() || !isClass(pt->inner) || classKey(pt->inner) != classKey(t))
      continue;
    declared = true;
    usable = usable || !(m->flags & DF_Delete);
  }
  return !declared || usable;
}

auto codegen::Emitter::moveTemporary(const std::string& dst, Value src, const Ty* t, Node* at) -> void
{
  for (size_t i = statementTemps.size(); i-- > 0;)
    if (statementTemps[i].addr == src.text)
    {
      statementTemps.erase(statementTemps.begin() + i);
      break;
    }
  if (isClass(t) && structFor(t, at).empty)
    return; // no state, and as a base maybe no bytes of its own (the next member's)
  if (isClass(t) && !relocatesBitwise(t, at) && !(src.ty && classKey(types.stripRef(src.ty)) != classKey(t)) && movableByConstructor(t, at))
  {
    // (Where the object could be built in place, it is: a call's result, an argument. Here it was
    // built elsewhere first; a class that may know its own address is moved by its constructor.)
    moveConstruct(dst, src.text, t, at);
    destroy(src.text, t, at);
    return;
  }
  copyBytes(dst, src.text, t, at);
  if (isClass(t) && src.ty && classKey(types.stripRef(src.ty)) != classKey(t))
    storeVptrs(dst, structFor(t, at)); // a temporary of a derived class, sliced
}

auto codegen::Emitter::newObjectSlot(const Ty* t, Node* at) -> std::string
{
  std::string slot = "%o" + std::to_string(temp++);
  allocas += "  " + slot + " = alloca " + llType(t, at) + alignSuffix(t, 0, at) + "\n";
  return slot;
}
