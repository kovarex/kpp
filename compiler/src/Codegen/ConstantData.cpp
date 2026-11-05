// struct Emitter's members: constant data and globals; their declarations in ConstantData.hpp, part
// of the struct's body in Codegen/Emitter.hpp. A translation unit of its own since 2026-11-05
// (codegen.cpp split, stage 2), moved without a change.
#include "Emitter.hpp"

auto codegen::Emitter::valueMembers(ClassDecl* c) -> std::vector<Decl*>
{
  std::vector<Decl*> members;
  for (Decl* m : c->members)
    if (m->kind == NodeKind::VariableDecl && !(m->flags & (DF_Static | DF_Typedef | DF_Friend)))
      members.push_back(m);
  return members;
}

auto codegen::Emitter::constantText(const ConstValue& v, const Ty* t, Node* at, int depth) -> std::optional<std::string>
{
  if (!t || depth > 64)
    return std::nullopt;
  t = types.unqualified(t);
  bool unset = v.kind == ConstValue::Kind::Unset; // static storage starts zero
  switch (t->kind)
  {
    case TyKind::Bool:
      if (unset)
        return std::string("false");
      return v.kind == ConstValue::Kind::Int ? std::optional<std::string>(v.i ? "true" : "false") : std::nullopt;
    case TyKind::Char: case TyKind::Int: case TyKind::Enum:
      if (unset)
        return std::string("0");
      return v.kind == ConstValue::Kind::Int ? std::optional<std::string>(std::to_string(v.i)) : std::nullopt;
    case TyKind::Float:
      if (unset)
        return floatConstant(0.0, t->bits);
      if (v.kind == ConstValue::Kind::Float)
        return floatConstant(v.f, t->bits);
      return v.kind == ConstValue::Kind::Int ? std::optional<std::string>(floatConstant(double(v.i), t->bits)) : std::nullopt;
    case TyKind::Pointer:
      // A null pointer only when no initializer set it: the interpreter's Int 0 of a pointer type
      // (`FOLD = fold` with fold = nullptr, a table's `{NULL, NULL}`) is built at startup. (A
      // function template argument is its function's Address since d61e6a56, so the Int 0 would be
      // a null too; but lowered, Lua's `loadedlibs` table, whose other entries name C functions of
      // other modules, left their modules undeclared: B's, 2026-11-05.) (`T* p = nullptr;` itself
      // is a null constant before constant data is asked for.)
      if (unset)
        return std::string("null");
      if (v.kind == ConstValue::Kind::Pointer)
        return evaluatedArrayElement(v, t, at, depth);
      if (v.kind == ConstValue::Kind::Address && v.target)
        return addressConstant(*v.target, at);
      return std::nullopt;
    case TyKind::MemberPointer:
    {
      // A pointer to a data member: the member's offset within the class (that of the pointer's
      // type, the member maybe a base's), -1 for null, as one is made at run time (Expressions.cpp,
      // Values.cpp's convert). One to a member function is built at startup.
      if (t->inner && t->inner->kind == TyKind::Function)
        return std::nullopt;
      if (unset)
        return std::string("-1");
      const Ty* cls = !t->args.empty() && isClass(t->args[0]) ? t->args[0] : v.target ? v.target->owner : nullptr;
      if (v.kind != ConstValue::Kind::MemberPointer || !v.target || !v.target->decl || v.target->decl->kind != NodeKind::VariableDecl || !cls ||
          !v.target->path.empty())
        return std::nullopt;
      std::optional<uint64_t> offset = memberOffset(cls, v.target->decl, at);
      return offset ? std::optional<std::string>(std::to_string(*offset)) : std::nullopt;
    }
    case TyKind::Array:
    {
      if (unset)
        return std::string("zeroinitializer");
      if (v.kind != ConstValue::Kind::Object || !v.parts || v.parts->size() != t->count)
        return std::nullopt;
      std::string element = llType(t->inner, at), out = "[";
      bool allZero = true;
      for (size_t i = 0; i < v.parts->size(); ++i)
      {
        std::optional<std::string> e = constantText((*v.parts)[i], t->inner, at, depth + 1);
        if (!e)
          return std::nullopt;
        allZero = allZero && zeroText(*e);
        out += (i ? ", " : "") + element + " " + *e;
      }
      return allZero ? std::string("zeroinitializer") : out + "]";
    }
    case TyKind::Class:
      return classConstant(v, t, at, depth);
    default:
      return std::nullopt;
  }
}

auto codegen::Emitter::classConstant(const ConstValue& v, const Ty* t, Node* at, int depth) -> std::optional<std::string>
{
  StructInfo& info = structFor(t, at);
  ClassDecl* c = classDef(t->decl, at);
  if (info.isUnion || c->isTypedUnion || info.polymorphic || !info.anonymous.empty())
    return std::nullopt;
  if (v.kind == ConstValue::Kind::Unset)
    return std::string("zeroinitializer");
  std::vector<Decl*> members = valueMembers(c);
  if (v.kind != ConstValue::Kind::Object || !v.parts || v.parts->size() != info.bases.size() + members.size())
    return std::nullopt;
  std::vector<const Ty*> fieldTypes = info.fieldTypes;
  std::vector<bool> zeroSize = info.zeroSize;
  std::vector<std::string> fields(fieldTypes.size());
  std::vector<uint64_t> units(fieldTypes.size(), 0); // bit fields' storage units
  std::vector<bool> unit(fieldTypes.size(), false);
  for (size_t k = 0; k < info.bases.size(); ++k)
  {
    uint32_t index = info.bases[k].second;
    if (zeroSize[index])
      continue; // an empty base: no storage of its own
    std::optional<std::string> b = constantText((*v.parts)[k], fieldTypes[index], at, depth + 1);
    if (!b)
      return std::nullopt;
    fields[index] = *b;
  }
  for (size_t k = 0; k < members.size(); ++k)
  {
    const ConstValue& part = (*v.parts)[info.bases.size() + k];
    auto it = info.fieldIndex.find(members[k]);
    if (it == info.fieldIndex.end())
      return std::nullopt;
    uint32_t index = it->second;
    if (auto bits = info.bitfields.find(members[k]); bits != info.bitfields.end())
    {
      if (part.kind != ConstValue::Kind::Int && part.kind != ConstValue::Kind::Unset)
        return std::nullopt;
      uint32_t shift = bits->second.shift, width = bits->second.width;
      uint64_t mask = width >= 64 ? ~0ull : (1ull << width) - 1;
      units[index] |= ((part.kind == ConstValue::Kind::Int ? uint64_t(part.i) : 0) & mask) << shift;
      unit[index] = true;
      continue;
    }
    if (fieldTypes[index]->isReference())
      return std::nullopt; // the interpreter holds the referred value there, not an address
    std::optional<std::string> f = constantText(part, fieldTypes[index], at, depth + 1);
    if (!f)
      return std::nullopt;
    fields[index] = *f;
  }
  std::string out = info.packed ? "<{ " : "{ ";
  bool allZero = true;
  for (size_t i = 0; i < fieldTypes.size(); ++i)
  {
    std::string value = unit[i] ? std::to_string(int64_t(units[i])) : fields[i].empty() ? "zeroinitializer" : fields[i]; // padding and empty bases: zero
    allZero = allZero && zeroText(value);
    out += (i ? ", " : "") + (zeroSize[i] ? std::string("[0 x i8]") : llType(fieldTypes[i], at)) + " " + value;
  }
  if (info.empty)
    out += std::string(fieldTypes.empty() ? "" : ", ") + "i8 0"; // its byte of its own
  if (info.tailPadding)
    out += ", [" + std::to_string(info.tailPadding) + " x i8] zeroinitializer";
  out += info.packed ? " }>" : " }";
  return allZero ? std::string("zeroinitializer") : out;
}

auto codegen::Emitter::evaluatedArrayElement(const ConstValue& v, const Ty* pointerTy, Node* at, int depth) -> std::optional<std::string>
{
  const Ty* element = types.unqualified(pointerTy->inner);
  if (!v.parts || element->kind == TyKind::Void || !element->isKnown())
    return std::nullopt;
  const Ty* arrayTy = types.array(element, uint32_t(v.parts->size()));
  std::optional<std::string> body = constantText(ConstValue::object(arrayTy, *v.parts), arrayTy, at, depth + 1);
  if (!body)
    return std::nullopt;
  std::string contents = llType(arrayTy, at) + " " + *body;
  auto [name, isNew] = constantName("@.data.", contents);
  if (isNew)
    (emittingShared ? decls : moduleDecls[currentModule]) += literalDefinition(name, contents);
  return "getelementptr inbounds (" + llType(arrayTy, at) + ", ptr " + name + ", i64 0, i64 " + std::to_string(v.i) + ")";
}

auto codegen::Emitter::addressConstant(const ConstTarget& target, Node* at) -> std::optional<std::string>
{
  if (!target.decl)
    return std::nullopt;
  if (target.decl->kind == NodeKind::FunctionDecl)
  {
    auto* f = static_cast<FunctionDecl*>(target.decl);
    if (!target.path.empty() || (global.owningClass(f) && !(f->flags & DF_Static)))
      return std::nullopt; // a member function's address is a member pointer
    TemplateArgs args;
    for (auto& p : target.args)
      args.pairs.push_back(p);
    refuseCAddress(f, f);
    return useFunction(f, args, target.owner, at);
  }
  if (target.decl->kind != NodeKind::VariableDecl || (target.decl->parent && target.decl->parent->kind == NodeKind::FunctionDecl))
    return std::nullopt; // a static local is laid out where its function is emitted
  auto* var = static_cast<VariableDecl*>(target.decl);
  std::string name = globalFor(var, at, target.owner);
  if (target.path.empty())
    return name;
  const Ty* type = target.owner ? global.typeOfDeclIn(var, global.classArgsOf(target.owner)) : global.typeOfDecl(var);
  std::string gep = "getelementptr inbounds (" + llType(types.stripRef(type), at) + ", ptr " + name + ", i64 0";
  for (size_t step : target.path)
  {
    type = types.unqualified(types.stripRef(type));
    if (type->kind == TyKind::Array)
    {
      gep += ", i64 " + std::to_string(step);
      type = type->inner;
      continue;
    }
    if (!isClass(type))
      return std::nullopt;
    StructInfo& info = structFor(type, at);
    if (info.isUnion || !info.anonymous.empty())
      return std::nullopt;
    uint32_t index;
    if (step < info.bases.size())
      index = info.bases[step].second;
    else
    {
      std::vector<Decl*> members = valueMembers(classDef(type->decl, at));
      size_t k = step - info.bases.size();
      if (k >= members.size() || !info.fieldIndex.count(members[k]) || info.bitfields.count(members[k]))
        return std::nullopt;
      index = info.fieldIndex[members[k]];
    }
    gep += ", i32 " + std::to_string(index);
    type = info.fieldTypes[index];
  }
  return gep + ")";
}

auto codegen::Emitter::hasMutableMember(const Ty* t, Node* at, int depth) -> bool
{
  t = types.unqualified(t);
  while (t->kind == TyKind::Array)
    t = types.unqualified(t->inner);
  if (!isClass(t) || depth > 16)
    return false;
  StructInfo& info = structFor(t, at);
  for (Decl* m : info.fieldDecls)
    if (m->flags & DF_Mutable)
      return true;
  for (const Ty* ft : std::vector<const Ty*>(info.fieldTypes))
    if (!ft->isReference() && hasMutableMember(ft, at, depth + 1))
      return true;
  return false;
}

auto codegen::Emitter::constantData(const Ty* t, Expr* value, VariableDecl* def, const TemplateArgs& args, Node* at, bool* evaluated)
  -> std::optional<std::string>
{
  bool layoutFailed = false;
  std::optional<ConstValue> v;
  if (PreEvaluated* pre = preEvaluated; pre && pre->evaluated && pre->value == value && pre->type == t)
  {
    v = pre->data; // evaluated before the lock was taken (globalFor)
    notConstantWhy = pre->why;
    takePreEvaluated(*pre);
  }
  else
  {
    ConstContextScope context(*this, def);
    GlobalScope::clearConstantReason();
    v = global.evaluateInitializer(t, value, def->file, def, args, foldHooks(at, layoutFailed));
    notConstantWhy = v ? std::string() : GlobalScope::constantReason();
  }
  if (evaluated)
    *evaluated = v.has_value();
  if (!v)
    return std::nullopt;
  return constantText(*v, t, at);
}

auto codegen::Emitter::takePreEvaluated(PreEvaluated& pre) -> void
{
  if (pre.used)
    return;
  pre.used = true;
  errors.insert(errors.end(), pre.errors.begin(), pre.errors.end());
}

auto codegen::Emitter::preEvaluateGlobal(VariableDecl* v, Node* at, const Ty* classTy, PreEvaluated& pre) -> void
{
  TemplateArgs classArgs;
  if (classTy)
    classArgs = global.definitionArgs(variableDefinition(v, classTy), global.classArgsOf(classTy));
  ArgsScope argsScope(*this, classTy ? &classArgs : nullptr);
  struct ClassRestore { const Ty*& c; const Ty* saved; ~ClassRestore() { c = saved; } } classRestore{currentClassTy, currentClassTy};
  currentClassTy = classTy;
  struct SharedScope
  {
    bool& flag;
    bool saved;
    SharedScope(bool& f) : flag(f), saved(f) { flag = true; }
    ~SharedScope() { flag = saved; }
  } sharedScope(emittingShared);
  const Ty* t = classTy ? global.typeOfDeclIn(v, classArgs) : global.typeOfDecl(v);
  if (!t || !t->isKnown() || t->isReference() || v->builtin)
    return;
  VariableDecl* def = variableDefinition(v, classTy);
  if (def == v && (v->flags & DF_Extern) && !v->init && !classTy)
    return;
  Expr* value = classTy ? global.instanceInitializer(def, classArgs) : def->init;
  if (!value)
    return;
  const Ty* st = types.stripRef(t);
  struct FileRestore { ParsedFile*& f; ParsedFile* saved; ~FileRestore() { f = saved; } } fileRestore{currentFile, currentFile};
  currentFile = def->file;
  pre.value = value;
  pre.type = st;
  {
    ConstContextScope context(*this, def);
    pre.scalar = foldAs(foldConst(value), st);
    if (pre.scalar && !pre.scalar->isScalar())
      pre.scalar.reset();
    pre.folded = true;
  }
  const std::optional<ConstValue>& c = pre.scalar;
  bool scalarInit = c && (((st->kind == TyKind::Int || st->kind == TyKind::Char || st->kind == TyKind::Enum) && !c->isFloat()) ||
                          st->kind == TyKind::Float || st->kind == TyKind::Bool);
  bool literal = value->kind == NodeKind::LiteralExpr &&
                 ((st->kind == TyKind::Pointer && static_cast<LiteralExpr*>(value)->keyword == Kw::nullptr_) ||
                  (static_cast<LiteralExpr*>(value)->literalKind == TokenKind::String && st->kind == TyKind::Pointer));
  if (!scalarInit && !literal && (isClass(st) || st->kind == TyKind::Array || st->kind == TyKind::Pointer || st->kind == TyKind::MemberPointer || !c))
  {
    bool layoutFailed = false;
    ConstContextScope context(*this, def);
    GlobalScope::clearConstantReason();
    pre.data = global.evaluateInitializer(st, value, def->file, def, classArgs, foldHooks(at, layoutFailed));
    pre.why = pre.data ? std::string() : GlobalScope::constantReason();
    pre.evaluated = true;
  }
}

auto codegen::Emitter::globalFor(VariableDecl* v, Node* at, const Ty* classTy) -> std::string
{
  // A type name read as a value (`(HANDLE) -1` taken for `HANDLE - 1`, `(LPWSTR)&msg` for
  // `LPWSTR & msg`): no variable to define, never a global of that name.
  if (v->flags & DF_Typedef)
    unsupported(at, "the type " + (v->name.empty() ? std::string("?") : sym(v->name.last().symbol)) + " used as a value");
  if (!classTy && (v->flags & DF_Extern))
    v = variableDefinition(v); // `extern T x;` names the variable its namespace defines, if it does
  if (auto mine = globalCache.find({v, classTy}); mine != globalCache.end())
  {
    buildThreadLocal(mine->second);
    return mine->second;
  }
  std::string name;
  // Not yet defined (and not under the lock already): its initializer evaluated first, the lock
  // not held. What that noted and numbered is taken back unless globalForOnce takes the values.
  PreEvaluated pre;
  std::optional<GlobalClaim> claim; // (until it is defined: after the lock below)
  size_t inlinedBefore = inlinedNow.size();
  if (sharedDepth == 0)
  {
    bool known;
    {
      auto lock = sharedLock();
      known = sh.failedGlobals.count({v, classTy}) ||
              (classTy ? instanceGlobals.count("@\"" + irString(classTy) + "::" + sym(v->name.last().symbol) + "\"") != 0 : globals.count(v) != 0);
    }
    if (!known)
    {
      // Evaluated by another emitter now: waited for (no lock is held here, and an evaluation
      // before the lock waits for nothing), then taken as any later use takes it.
      std::unique_lock claims(sh.claimsMutex);
      if (sh.evaluating.count({v, classTy}))
      {
        sh.claimsChanged.wait(claims, [&] { return !sh.evaluating.count({v, classTy}); });
        known = true;
      }
      else
      {
        sh.evaluating.insert({v, classTy});
        claims.unlock();
        claim.emplace(sh, std::pair<Decl*, const Ty*>{v, classTy});
      }
    }
    if (!known)
    {
      size_t errorsBefore = errors.size();
      uint32_t tempBefore = temp;
      size_t fnBefore = fn.size(), allocasBefore = allocas.size();
      try
      {
        preEvaluateGlobal(v, at, classTy, pre);
      }
      catch (Unsupported&)
      {
        pre = PreEvaluated{}; // globalForOnce evaluates (and fails) as before
      }
      if (pre.value)
        pre.errors.assign(errors.begin() + errorsBefore, errors.end()); // (reported if globalForOnce takes the values)
      errors.resize(errorsBefore);
      if (fn.size() == fnBefore && allocas.size() == allocasBefore)
        temp = tempBefore;
    }
  }
  struct PreRestore { PreEvaluated*& p; PreEvaluated* saved; ~PreRestore() { p = saved; } } preRestore{preEvaluated, preEvaluated};
  preEvaluated = pre.value ? &pre : nullptr;
  {
    auto lock = sharedLock(); // a global is defined (and its startup function emitted) once
    std::pair<Decl*, const Ty*> key{v, classTy};
    if (auto failed = sh.failedGlobals.find(key); failed != sh.failedGlobals.end())
      fail(at, failed->second);
    size_t errorsBefore = errors.size();
    // Defined by whichever function uses it first, which depends on the threads' timing: the
    // temporaries it numbers (building a constant initializer) are not that function's, or its
    // IR would differ from run to run.
    uint32_t tempBefore = temp;
    size_t fnBefore = fn.size(), allocasBefore = allocas.size();
    try
    {
      name = globalForOnce(v, at, classTy);
      if (fn.size() == fnBefore && allocas.size() == allocasBefore)
        temp = tempBefore;
    }
    catch (Unsupported&)
    {
      // Every use, the first one like the others, reports the global unusable and why (the
      // same lines whichever use comes first).
      std::string why = "a use of the global " + qualifiedName(v) + ", whose definition failed (" + lastFailure + ")";
      sh.failedGlobals[key] = why;
      ++sh.failuresRemembered;
      errors.resize(errorsBefore);
      if (!pre.used && inlinedNow.size() > inlinedBefore)
        inlinedNow.resize(inlinedBefore);
      fail(at, why);
    }
  }
  claim.reset(); // defined (or failed): another emitter waiting for it goes on
  if (!pre.used && inlinedNow.size() > inlinedBefore && pre.value)
    inlinedNow.resize(inlinedBefore); // (another emitter defined it meanwhile: nothing of it is this function's)
  globalCache.emplace(std::pair<Decl*, const Ty*>{v, classTy}, name);
  buildThreadLocal(name);
  return name;
}

auto codegen::Emitter::buildThreadLocal(const std::string& name) -> void
{
  std::string init;
  if (auto mine = threadLocalCache.find(name); mine != threadLocalCache.end())
    init = mine->second;
  else
  {
    auto lock = sharedLock();
    if (auto it = threadLocalInits.find(name); it != threadLocalInits.end())
      init = it->second;
    if (mayPublish()) // (not while a global's definition is under way)
      threadLocalCache.emplace(name, init);
  }
  if (!init.empty() && !currentFunctionName.empty() && currentFunctionName != init)
    fn += "  call void " + init + "()\n";
}

auto codegen::Emitter::globalForOnce(VariableDecl* v, Node* at, const Ty* classTy) -> std::string
{
  TemplateArgs classArgs;
  std::string instanceName;
  if (classTy)
  {
    // A static member of a class template: one global per instantiation, its initializer
    // evaluated under the instantiation's arguments (also an out-of-class definition's own
    // declarations of the class's parameters).
    classArgs = global.definitionArgs(variableDefinition(v, classTy), global.classArgsOf(classTy));
    instanceName = "@\"" + irString(classTy) + "::" + sym(v->name.last().symbol) + "\"";
    auto found = instanceGlobals.find(instanceName);
    if (found != instanceGlobals.end())
      return found->second;
  }
  else
  {
    auto it = globals.find(v);
    if (it != globals.end())
      return it->second;
  }
  // The global's initializer under its own arguments and class, not those of the function that
  // happened to use it first.
  ArgsScope argsScope(*this, classTy ? &classArgs : nullptr);
  struct ClassRestore { const Ty*& c; const Ty* saved; ~ClassRestore() { c = saved; } } classRestore{currentClassTy, currentClassTy};
  currentClassTy = classTy;
  struct SharedScope
  {
    bool& flag;
    bool saved;
    SharedScope(bool& f) : flag(f), saved(f) { flag = true; }
    ~SharedScope() { flag = saved; }
  } sharedScope(emittingShared); // a global's initializer belongs to every module
  const Ty* t = classTy ? global.typeOfDeclIn(v, classArgs) : global.typeOfDecl(v);
  if (!t || !t->isKnown())
    unsupported(at, "a global of unknown type");
  // A variable of C linkage (declared inside extern "C") has C's name, whatever namespace or
  // library scope it is declared in (design.md 16): http's `extern "C" char** environ`. (A
  // private one keeps its file's prefix, should the parser ever allow it: design.md 15 refuses
  // it today, kpp/private_variable_in_extern_c_is_error.)
  bool cName = (v->flags & DF_ExternC) && !global.owningClass(v) && filePrivatePrefix(v).empty();
  std::string name = classTy ? instanceName
                     : cName ? "@\"" + sym(v->name.last().symbol) + "\""
                             : "@\"" + withSpecialisationArgs(qualifiedName(v), v) + "\"";
  if (classTy)
    instanceGlobals[instanceName] = name;
  else
    globals[v] = name;
  if (v->builtin == BUILTIN_EnumValues && v->parent)
  {
    // E::values: a constant array of the enumerators in declaration order.
    auto* e = static_cast<EnumDecl*>(v->parent);
    std::string element = llType(enumUnderlying(e), at), elements;
    for (Decl* d : e->enumerators)
      if (d->kind == NodeKind::EnumeratorDecl)
        elements += std::string(elements.empty() ? "" : ", ") + element + " " + std::to_string(enumeratorValue(static_cast<EnumeratorDecl*>(d)));
    decls += name + " = internal constant " + llType(t, at) + " [" + elements + "]\n";
    return name;
  }
  if (v->builtin == BUILTIN_EnumCount && v->parent && v->parent->kind == NodeKind::EnumDecl)
  {
    // E::count: a constant holding the number of enumerators, the value a read of it folds to;
    // never the zero a global without an initializer holds (`println(Direction::count)` read 0).
    size_t n = 0;
    for (Decl* d : static_cast<EnumDecl*>(v->parent)->enumerators)
      n += d->kind == NodeKind::EnumeratorDecl;
    decls += name + " = internal constant " + llType(types.stripRef(t), at) + " " + std::to_string(n) + "\n";
    return name;
  }
  if (v->builtin)
    unsupported(at, "the object of the generated member " + qualifiedName(v)); // its value is the compiler's, not an initializer's
  std::string init = "zeroinitializer";
  bool constructed = false;
  const Ty* st = types.stripRef(t);
  VariableDecl* def = variableDefinition(v, classTy); // `static double scale;` in the class, `double Gui::scale = 1.0;` outside
  if (def == v && (v->flags & DF_Extern) && !v->init && !classTy && !t->isReference())
  {
    // `extern int counter;` that no kpp source defines: a C library's variable (its header
    // declares it), defined and initialised there. A definition here would be a second one
    // (a duplicate in the link) or, where the library's is not linked, a zero read as its value.
    // Declared once, however many declarations name it.
    if (declaredNames.claim(name))
      decls += name + " = external global " + llType(st, at) + "\n";
    return name;
  }
  // `static constexpr T ValuesPerOne = T(1) << Bits`: the initializer typed for this instantiation.
  Expr* value = classTy ? global.instanceInitializer(def, classArgs) : def->init;
  ParsedFile* saved = currentFile;
  currentFile = def->file;
  // `thread_local IntrusiveList<CrashHookBase> CrashHooks::hooks;`: one per thread (TLS), built by
  // each thread at its first use (emitGlobalConstructor's perThread).
  bool threadLocal = ((v->flags | def->flags) & DF_ThreadLocal) != 0;
  if (t->isReference())
  {
    // `static const Style& defaultWidgetStyle = getStyleStatic();`: a pointer, bound at startup.
    currentFile = saved;
    if (threadLocal)
      unsupported(at, "a thread_local reference");
    decls += "$" + name.substr(1) + " = comdat any\n" + name + " = linkonce_odr global ptr null, comdat" + debugGlobal(name, def, t) + "\n";
    if (value)
      emitGlobalConstructor(v, name, t, value, at, false, def);
    return name;
  }
  if (value)
  {
    std::optional<ConstValue> c;
    if (PreEvaluated* pre = preEvaluated; pre && pre->folded && pre->value == value && pre->type == st)
    {
      c = pre->scalar; // evaluated before the lock was taken (globalFor)
      takePreEvaluated(*pre);
    }
    else
    {
      ConstContextScope context(*this, def); // traits in the initializer resolve their names in the declaration's context
      c = foldAs(foldConst(value), st);       // the value the emitted initialization would store
      if (c && !c->isScalar())
        c.reset(); // an object or a pointer: built at startup (constant data comes later)
    }
    bool evaluated = c.has_value(); // (a scalar's value; constantData says for the others)
    if (c && (st->kind == TyKind::Int || st->kind == TyKind::Char || st->kind == TyKind::Enum) && !c->isFloat())
      init = std::to_string(c->i);
    else if (c && st->kind == TyKind::Float)
      init = floatConstant(c->isFloat() ? c->f : double(c->i), st->bits);
    else if (c && st->kind == TyKind::Bool)
      init = c->truthy() ? "true" : "false";
    else if (st->kind == TyKind::Pointer && value->kind == NodeKind::LiteralExpr && static_cast<LiteralExpr*>(value)->keyword == Kw::nullptr_)
      init = "null";
    else if (value->kind == NodeKind::LiteralExpr && static_cast<LiteralExpr*>(value)->literalKind == TokenKind::String &&
             st->kind == TyKind::Pointer)
      init = stringConstant(static_cast<LiteralExpr*>(value));
    // (A scalar the fold gives no value of, `constexpr ID EMPTY{ID(-1)};`: as the initialization the
    // interpreter evaluates, which a fold of the braces as an expression is not.)
    else if (std::optional<std::string> data =
               isClass(st) || st->kind == TyKind::Array || st->kind == TyKind::Pointer || st->kind == TyKind::MemberPointer || !c
                 ? constantData(st, value, def, classArgs, at, &evaluated)
                 : std::nullopt)
      init = *data; // constant data: the object as the interpreter computes it
    else
    {
      // Built at startup, see emitGlobalConstructor: no value, or one that no LLVM constant
      // expresses yet (a typed union's), which design.md's step 4 builds at startup.
      constructed = true;
      // A constexpr variable needs its constant (design.md "A constexpr variable needs its
      // constant"): one the checker left to the code generator (its value needs exact layouts)
      // and that is no constant here either is an error, never a global built at startup, which
      // a global initialized before it reads as zero.
      if (!evaluated && ((v->flags | def->flags) & DF_Constexpr) && !global.checkerErrorWithin(def->file, def->token, def->token))
        error(def, "the constexpr variable " + sym(def->name.last().symbol) + " is no constant the compiler could evaluate" +
                     (notConstantWhy.empty() ? std::string() : ": " + notConstantWhy));
    }
  }
  else if (needsConstruction(st, at))
    constructed = true;
  currentFile = saved;
  // Defined in every module, merged by the linker (one instance per program). Two entries: the
  // shared text is sorted, and the definition (an `@` line) has to come after the struct types
  // (`%` lines) a constant initializer names; the comdat line (`$`) would sort first.
  decls += "$" + name.substr(1) + " = comdat any\n";
  decls += name + " = linkonce_odr " + (threadLocal ? "thread_local " : "") +
           (t->isConst && !constructed && !hasMutableMember(st, at) ? "constant " : "global ") + llType(st, at) + " " + init + ", comdat" +
           alignSuffix(st, variableAlignment(def, def, at), at) + debugGlobal(name, def, t) + "\n";
  if (constructed)
    emitGlobalConstructor(v, name, st, value, at, threadLocal, def);
  return name;
}
