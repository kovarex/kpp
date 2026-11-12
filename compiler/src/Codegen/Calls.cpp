// struct Emitter's members: instantiation requests, the functions a call uses, inlining and calls;
// their declarations in Calls.hpp, part of the struct's body in Codegen/Emitter.hpp. A translation
// unit of its own since 2026-11-05 (codegen.cpp split, stage 2), moved without a change.
#include <set>
#include "Emitter.hpp"

auto codegen::Emitter::noteRequest(const std::string& name) -> void
{
  auto [it, first] = levelRequests.try_emplace(name, Request{currentFunctionName, currentDepth + 1, currentContext});
  if (!first && currentFunctionName < it->second.by)
    it->second = {currentFunctionName, currentDepth + 1, currentContext};
}

auto codegen::Emitter::claimInstantiation(const std::string& name) -> bool
{
  noteRequest(name);
  lastUseInstantiates = true;
  return requested.claim(name);
}

auto codegen::Emitter::reportTooDeep(const Instantiation& item) -> void
{
  auto shortened = [](const std::string& s) { return s.size() <= 300 ? s : s.substr(0, 300) + "..."; };
  currentFile = item.f->file;
  error(item.f, "template instantiations nested more than " + std::to_string(maxInstantiationDepth) + " deep: " + shortened(item.name) +
                    " requested by " + shortened(item.requestedBy));
}

auto codegen::Emitter::noteRequestDeclaration(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy, const std::string& name) -> void
{
  if (!recordingRequests())
    return;
  const TemplateArgs* savedArgs = currentArgs;
  const Ty* savedClassTy = currentClassTy;
  ParsedFile* savedFile = currentFile;
  std::string declaration;
  withoutFailing([&]
  {
    currentFile = f->file;
    currentArgs = args.empty() ? nullptr : &args;
    currentClassTy = classTy;
    if (!currentClassTy)
      if (Decl* cls = global.owningClass(f))
        currentClassTy = cls->kind == NodeKind::EnumDecl ? types.enumType(cls) : canonicalClass(types.classType(cls, {}), f);
    declaration = declarationText(signatureOf(f), name);
  });
  currentArgs = savedArgs;
  currentClassTy = savedClassTy;
  currentFile = savedFile;
  if (!declaration.empty())
    recordRequestDeclaration(name, std::move(declaration));
}

auto codegen::Emitter::queueInstantiation(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy, const std::string& name) -> void
{
  noteRequestDeclaration(f, args, classTy, name);
  // In the module its name hashes to: spread evenly, and the same module whoever asks first (the
  // first requester's module piled most of the library's instantiations into one module that
  // clang then took 7 s at -O0 and 224 s at -O2 on, alone).
  unsigned module = moduleCount <= 1 ? 0 : sh.emissionModule(std::hash<std::string>()(name));
  worklist.push_back({f, args, classTy, module, currentFunctionName, name, currentDepth + 1, currentContext});
}

auto codegen::Emitter::notAnInstance(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy) -> std::string
{
  std::function<bool(const Ty*, int)> unknownIn = [&](const Ty* t, int depth) {
    if (!t || depth > 16)
      return false;
    if (!t->isKnown() || (t->kind == TyKind::Class && t->decl && (t->decl->attributes & ATTR_Draft)))
      return true;
    if (unknownIn(t->inner, depth + 1))
      return true;
    for (const Ty* a : t->args)
      if (unknownIn(a, depth + 1))
        return true;
    return false;
  };
  // (A constant argument is its value: `ZeroHandling::KeepZeros`, whose enum is a class template's
  // own, names the instance as 0.)
  auto usable = [&](const Ty* t) {
    return t && (t->kind == TyKind::Value ? t->inner && !unknownIn(t->inner, 0) : !types.isDependent(t) && !unknownIn(t, 0));
  };
  auto boundTo = [&](Decl* p) -> const Ty* {
    for (auto& [d, t] : args.pairs)
      if (d == p)
        return t;
    return nullptr;
  };
  for (Decl* p : f->templateParams)
    if (!static_cast<TemplateParamDecl*>(p)->isPack && !usable(boundTo(p)))
      return "the function template " + qualifiedName(f) + " without its argument " + sym(static_cast<TemplateParamDecl*>(p)->name) +
             (boundTo(p) ? " (" + types.toString(boundTo(p), &symbols) + ")" : std::string());
  if (classTy && !usable(classTy))
    return qualifiedName(f) + " of " + types.toString(classTy, &symbols) + ", a class not instantiated";
  if (!classTy)
    if (Decl* owner = global.owningClass(f); owner && owner->kind == NodeKind::ClassDecl && !(owner->flags & DF_Specialization))
      for (Decl* p : owner->templateParams)
        if (!usable(boundTo(p)))
          return qualifiedName(f) + " of the class template " + qualifiedName(owner) + " without its argument " + sym(static_cast<TemplateParamDecl*>(p)->name);
  return {};
}

// Whether a C declaration's type is C's long double or holds it (through a pointer, a reference, an
// array, const, a typedef, a kpp alias or a function pointer's signature): kpp's long double is double,
// so off Windows, where C's is x87's 80 bits in 16 bytes, the two are different types.
static bool namesCLongDouble(const Type* t, int depth = 0)
{
  if (!t || depth > 32)
    return false;
  switch (t->kind)
  {
    case NodeKind::BuiltinType:
      return static_cast<const BuiltinType*>(t)->keyword == Kw::double_ && static_cast<const BuiltinType*>(t)->longCount > 0;
    case NodeKind::PointerType:
      return namesCLongDouble(static_cast<const PointerType*>(t)->pointee, depth + 1);
    case NodeKind::LRefType:
      return namesCLongDouble(static_cast<const LRefType*>(t)->referenced, depth + 1);
    case NodeKind::RRefType:
      return namesCLongDouble(static_cast<const RRefType*>(t)->referenced, depth + 1);
    case NodeKind::ConstType:
      return namesCLongDouble(static_cast<const ConstType*>(t)->inner, depth + 1);
    case NodeKind::ArrayType:
      return namesCLongDouble(static_cast<const ArrayType*>(t)->element, depth + 1);
    case NodeKind::NamedType:
    {
      const Decl* d = static_cast<const NamedType*>(t)->resolved;
      if (d && d->kind == NodeKind::UsingAliasDecl) // (`using real = long double;` in a kpp declaration)
        return d->templateParams.empty() && namesCLongDouble(static_cast<const UsingAliasDecl*>(d)->type, depth + 1);
      return d && d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef) && namesCLongDouble(static_cast<const VariableDecl*>(d)->type, depth + 1);
    }
    case NodeKind::FunctionType:
    {
      auto* ft = static_cast<const FunctionType*>(t);
      if (namesCLongDouble(ft->result, depth + 1))
        return true;
      for (const Decl* p : ft->params)
        if (p && p->kind == NodeKind::ParamDecl && namesCLongDouble(static_cast<const ParamDecl*>(p)->type, depth + 1))
          return true;
      return false;
    }
    default:
      return false;
  }
}

auto codegen::Emitter::useFunction(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy, Node* at) -> std::string
{
  // A method of an enumeration of a class template is the instance's (`S<long long>::Dir::str`): its
  // type, with the arguments, is the class type a call passes.
  if (f->parent && f->parent->kind == NodeKind::EnumDecl && !enumInstance(classTy))
    for (Decl* o = f->parent; o && (o->kind == NodeKind::ClassDecl || o->kind == NodeKind::EnumDecl); o = o->parent)
      if (!o->templateParams.empty())
        unsupported(at ? at : f, "a method of an enumeration of a class template, called without its instance");
  if (f->builtin == BUILTIN_EnumFromStr)
    return useFunctionOnce(f, args, classTy);
  // A C function that takes or returns C's long double, or a pointer to one, off Windows: an imported
  // one, or one a kpp declaration names (`extern "C" long double fabsl(long double x);`) and no kpp
  // body defines, whose definition is C's too. kpp's long double is double, C's there x87's 80 bits
  // (design.md, "Targets and hosts", the type table), so a call would pass and take double's bits
  // where the function has x87's: an error, as its address is (until 2026-11-05 the call was made as
  // double(double), silently: FreeBSD's floorl(2.5) printed garbage; until 2026-11-09 a kpp
  // declaration's too: fabsl(-2.5) gave -2.5 on Linux, the library's strtold 0 for "0.125").
  // (Apple's arm64 has long double as double, as Windows does: nothing to refuse there)
  if (!sh.target.windows() && !(sh.target.mac() && sh.target.arm64()) && isCFunction(f))
  {
    bool longDouble = namesCLongDouble(f->returnType);
    for (Decl* p : f->params)
      longDouble = longDouble || (p && p->kind == NodeKind::ParamDecl && namesCLongDouble(static_cast<ParamDecl*>(p)->type));
    // (an `extern "C"` function that a kpp body defines is kpp's own, of kpp's long double, whichever
    // of its declarations the use found)
    bool cDefinition = f->file && f->file->isImported;
    if (longDouble && !cDefinition)
    {
      Decl* d = global.definitionOf(f, args);
      cDefinition = !d || d->kind != NodeKind::FunctionDecl || !static_cast<FunctionDecl*>(d)->body || (d->file && d->file->isImported);
    }
    if (longDouble && cDefinition)
      fail(at ? at : f, "a C function of long double (" + sym(f->name.last().symbol) + "): C's long double is " + (sh.target.arm64() ? "128 bits" : "x87's 80 bits") + " on " + sh.target.name + ", kpp's is double (design.md, \"Targets and hosts\")");
  }
  std::string key = functionName(f, args, classTy);
  if (auto mine = usedFunctions.find(key); mine != usedFunctions.end())
  {
    if (mine->second.instantiation)
      noteRequest(mine->second.name);
    return mine->second.name;
  }
  // Never a symbol for a template's function that is no instantiation: an error at the use,
  // which the headline counts, not an undefined name at the link.
  if (std::string why = notAnInstance(f, args, classTy); !why.empty())
    unsupported(at ? at : f, "emitting " + why);
  lastUseInstantiates = false;
  std::string name = useFunctionOnce(f, args, classTy);
  usedFunctions.emplace(std::move(key), UsedFunction{name, lastUseInstantiates});
  return name;
}

auto codegen::Emitter::definitionArguments(FunctionDecl* f, FunctionDecl* def, const TemplateArgs& args) -> TemplateArgs
{
  TemplateArgs defArgs = args;
  if (def == f)
    return defArgs;
  Decl* owner = global.owningClass(f);
  Span<Decl*> classParams; // a nested class's are the enclosing template's (`Outer<K, V>::Inner::load`)
  for (Decl* o = owner; o && (o->kind == NodeKind::ClassDecl || o->kind == NodeKind::EnumDecl) && classParams.empty(); o = o->parent)
    classParams = o->templateParams;
  size_t classCount = def->templateParams.size() > f->templateParams.size() ? def->templateParams.size() - f->templateParams.size() : 0;
  if (classCount == classParams.size() && classCount + f->templateParams.size() <= def->templateParams.size())
  {
    std::vector<std::pair<Decl*, const Ty*>> added;
    for (auto& [d, t] : defArgs.pairs)
    {
      for (size_t i = 0; i < classCount; ++i)
        if (d == classParams[i])
          added.push_back({def->templateParams[i], t});
      for (size_t i = 0; i < f->templateParams.size(); ++i)
        if (d == f->templateParams[i])
          d = def->templateParams[classCount + i];
    }
    for (auto& p : added)
      defArgs.pairs.push_back(p);
  }
  return defArgs;
}

auto codegen::Emitter::useFunctionOnce(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy) -> std::string
{
  if (f->builtin == BUILTIN_EnumFromStr)
  {
    // The generated `E::fromStr(text)` of a kpp enum: the library's enumFromStr<E>(text),
    // which has the same signature (an Optional<E> result slot, the text).
    Decl* lib = global.enumFromStrDecl();
    if (!lib || lib->templateParams.size() != 1 || !f->parent || f->parent->kind != NodeKind::EnumDecl)
      unsupported(f, "E::fromStr without the library's enumFromStr<E>");
    TemplateArgs libArgs;
    libArgs.pairs.push_back({lib->templateParams[0], enumInstance(classTy) ? enumInstance(classTy) : types.enumType(f->parent)});
    return useFunction(static_cast<FunctionDecl*>(lib), libArgs, nullptr);
  }
  std::string name = functionName(f, args, classTy);
  Decl* defDecl = global.definitionOf(f, args);
  FunctionDecl* def = defDecl && defDecl->kind == NodeKind::FunctionDecl ? static_cast<FunctionDecl*>(defDecl) : f;
  bool templated = !args.empty();
  if (def->file && def->file->isImported)
    def = f, templated = false; // inline C functions are declared, the library provides them
  if (def != f && (def->flags & DF_Default) && !def->body && !(f->flags & DF_Default))
    f->flags |= DF_Default; // `Flow::~Flow() = default;` out of line: the declaration is defaulted too (settled before emitting)
  if ((f->flags & DF_Default) && !f->body)
  {
    // `= default`: the compiler's memberwise function, defined where first needed (a vtable
    // slot or a function pointer wants a real function; calls inline the body).
    if (claimInstantiation(name))
      queueInstantiation(f, args, classTy, name);
    return name;
  }
  if (def->body && templated)
  {
    if (claimInstantiation(name))
    {
      queueInstantiation(def, definitionArguments(f, def, args), classTy, name);
    }
    return name;
  }
  if (def->body && !(def->file && def->file->isImported))
  {
    // Defined by the main pass, except a local class's member, which is emitted when used.
    if (Decl* owner = global.owningClass(def); owner && declaredInFunction(owner) && claimInstantiation(name))
      queueInstantiation(def, args, classTy, name);
    return name;
  }
  if (def->body && def->file && def->file->isImported && isCFunction(def) && !(def->flags & DF_Variadic))
  {
    // A C header's inline function (corecrt_math.h's `__inline long double floorl(long double)`),
    // which no library need export: its inline definition, under a name of the program's own,
    // which C lets a call use in place of the external one (C11 6.7.4/7), so that it never
    // clashes with a library's (legacy_stdio_definitions' vsnprintf). A variadic one (the
    // printf family, written with the CRT's own va_start) is the library's.
    std::string own = "@\"kpp.c-inline." + sym(def->name.last().symbol) + "\"";
    if (claimInstantiation(own))
      queueInstantiation(def, args, classTy, own);
    return own;
  }
  if (!declaredNames.claim(name))
    return name;
  // A C declaration of one of the runtime's functions (the library's `extern "C" void kpp_throw(...)`):
  // the runtime's headers declare it to every module that calls it and the shared text defines it,
  // so a declare line of its own would be a second definition where both land in one module.
  static const std::unordered_set<std::string> runtimeNames = []
  {
    std::unordered_set<std::string> names;
    for (const std::string& header : runtimeHeaders())
    {
      std::string runtimeName;
      sharedDeclarationOf(header, &runtimeName);
      names.insert(runtimeName);
    }
    return names;
  }();
  if (global.isExternC(f) && runtimeNames.count(name))
    return name;
  {
    static bool debugCalls = getenv("KPP_DEBUG_CALLS") != nullptr;
    if (debugCalls)
      fprintf(stderr, "declare %s: def %s, def flags %x, def body %d, f flags %x, f body %d\n", name.c_str(), def == f ? "self" : "other", def->flags,
              def->body != nullptr, f->flags, f->body != nullptr);
  }
  if (f->builtin == BUILTIN_EnumStr || f->builtin == BUILTIN_EnumEnumeratorName)
  {
    emitEnumStr(f, name, classTy);
    return name;
  }
  // A function of the program's own (not C's) with no definition found: only declared, it would
  // be missing from the link with no error. Reported once generation is done (generateLLVM).
  if (!global.isExternC(f) && !(f->file && f->file->isImported) && !(f->flags & (DF_Delete | DF_Pure)))
  {
    auto lock = sharedLock();
    sh.undefinedUses.emplace(name, f);
  }
  ArgsScope scope(*this, &args);
  Signature s = signatureOf(f);
  std::string params, returnType = s.returnType;
  if (isCFunction(f))
    std::tie(returnType, params) = cDeclaration(f, s); // (its structs by value as the target's C passes them)
  else
  {
    for (size_t i = 0; i < s.paramTypes.size(); ++i)
      params += (i ? ", " : "") + s.paramTypes[i];
    if (s.variadic)
      params += params.empty() ? "..." : ", ...";
  }
  // (a narrow result extended by the callee, a C function's as the target's C says and every kpp
  // definition's: narrowExtension; the declaration as the definition has it)
  decls += "declare" + std::string(s.resultExtension) + " " + returnType + " " + name + "(" + params + ")\n";
  return name;
}

auto codegen::Emitter::enumeratorCount(Decl* e) -> size_t
{
  size_t n = 0;
  for (Decl* d : static_cast<EnumDecl*>(e)->enumerators)
    n += d->kind == NodeKind::EnumeratorDecl;
  return n;
}

auto codegen::Emitter::enumIntrinsic(int which, CallExpr* c) -> Value
{
  const Ty* charPointer = types.pointer(types.qualified(types.charType(8, false), true, false));
  if (which == 2)
  {
    const Ty* e = c->templateArgCount ? withCurrentArgs(c->templateArgs[0]) : nullptr;
    e = e ? types.unqualified(types.stripRef(e)) : nullptr;
    if (!e || e->kind != TyKind::Enum)
      unsupported(c, "kppEnumTypeName of no enumeration");
    return {cStringConstant(types.toString(e, &symbols)), charPointer};
  }
  if (c->args.size() != 1)
    unsupported(c, "kppEnumeratorName with no value");
  Value v = rvalue(c->args[0]);
  const Ty* e = v.ty ? types.unqualified(types.stripRef(withCurrentArgs(v.ty))) : nullptr;
  if (!e || e->kind != TyKind::Enum || !e->decl)
    unsupported(c, "kppEnumeratorName of no enumeration");
  Decl* def = global.definitionOf(e->decl);
  auto* en = static_cast<EnumDecl*>(def ? def : e->decl);
  std::string k = llType(enumUnderlying(e), c);
  // (An enumeration of a class template's: each instance has its values, and a function of its own.)
  std::string name = "@\"kpp.enumName." + qualifiedName(en) + (e->args.empty() ? "" : "." + types.toString(e, &symbols)) + "\"";
  if (declaredNames.claim(name))
  {
    emittingShared = true;
    std::string cases, blocks;
    int i = 0;
    std::set<std::string> seen; // (of enumerators of one value, the first names it)
    for (Decl* d : en->enumerators)
      if (d->kind == NodeKind::EnumeratorDecl)
      {
        auto* x = static_cast<EnumeratorDecl*>(d);
        std::string value = std::to_string(enumeratorValue(x, e));
        if (!seen.insert(value).second)
          continue;
        std::string l = "e" + std::to_string(i++);
        cases += "    " + k + " " + value + ", label %" + l + "\n";
        blocks += l + ":\n  ret ptr " + cStringConstant(sym(x->name)) + "\n";
      }
    shared += "$" + name.substr(1) + " = comdat any\n"
              "define linkonce_odr ptr " + name + "(" + k + " %v) comdat {\nentry:\n  switch " + k + " %v, label %other [\n" + cases +
              "  ]\n" + blocks + "other:\n  ret ptr null\n}\n";
    emittingShared = false;
  }
  std::string r = newTemp();
  emitCall(r + " = ", "ptr " + name + "(" + k + " " + v.text + ")", false);
  return {r, charPointer};
}

auto codegen::Emitter::emitEnumStr(FunctionDecl* f, const std::string& name, const Ty* classTy) -> void
{
  emittingShared = true;
  auto* e = static_cast<EnumDecl*>(f->parent);
  const Ty* et = enumInstance(classTy) ? enumInstance(classTy) : types.enumType(e); // (of a class template's enumeration, the instance's values)
  std::string k = llType(enumUnderlying(et), f), cases, blocks;
  int i = 0;
  std::set<std::string> seen; // (of enumerators of one value, the first names it)
  for (Decl* d : e->enumerators)
    if (d->kind == NodeKind::EnumeratorDecl)
    {
      auto* en = static_cast<EnumeratorDecl*>(d);
      std::string value = std::to_string(enumeratorValue(en, et));
      if (!seen.insert(value).second)
        continue;
      std::string l = "e" + std::to_string(i++);
      cases += "    " + k + " " + value + ", label %" + l + "\n";
      blocks += l + ":\n  ret ptr " + cStringConstant(sym(en->name)) + "\n";
    }
  shared += "$" + name.substr(1) + " = comdat any\n"
            "define linkonce_odr ptr " + name + "(ptr %this) comdat {\nentry:\n  %v = load " + k + ", ptr %this\n  switch " + k +
            " %v, label %other [\n" + cases + "  ]\n" + blocks + "other:\n  ret ptr " +
            (f->builtin == BUILTIN_EnumEnumeratorName ? std::string("null") : cStringConstant("?")) + "\n}\n"; // (enumeratorName(): null for a value that is none)
  emittingShared = false;
}

auto codegen::Emitter::friendClass(FunctionDecl* f, const std::vector<const Ty*>& argTypes, Node* at) -> const Ty*
{
  if (!(f->flags & DF_Friend) || !(f->flags & DF_Static))
    return nullptr;
  Decl* owner = global.owningClass(f);
  bool templated = false; // (its class's, or a class around it: `Union<T>::Point`'s friend)
  for (Decl* o = owner; o && (o->kind == NodeKind::ClassDecl || o->kind == NodeKind::EnumDecl); o = o->parent)
    templated = templated || !o->templateParams.empty();
  if (!owner || !templated)
    return nullptr;
  const Ty* cls = global.hiddenFriendClass(f, argTypes);
  if (!cls)
    unsupported(at, "a call of " + qualifiedName(f) + ", a friend function of a class template, whose instance the arguments do not name");
  return canonicalClass(cls, at);
}

auto codegen::Emitter::calleeArgs(FunctionDecl* f, const Ty* classTy, CallExpr* call, Node* at) -> TemplateArgs
{
  TemplateArgs args;
  if (const Ty* e = enumInstance(classTy))
    args = global.classArgsOf(e); // (a method of S<long long>::Dir: T is long long)
  else if (classTy)
    args = global.classArgsOf(canonicalClass(classTy, at));
  if (!f->templateParams.empty())
  {
    if (!call || call->templateArgCount != f->templateParams.size())
      unsupported(at, "a call to " + qualifiedName(f) + " whose template arguments could not be deduced");
    bool hasUnknown = false;
    for (size_t i = 0; i < f->templateParams.size(); ++i)
    {
      const Ty* ta = call->templateArgs[i];
      args.pairs.push_back({f->templateParams[i], ta});
      if (!ta || !ta->isKnown())
        hasUnknown = true;
      else if (ta->kind == TyKind::Pack)
        for (const Ty* e : ta->args)
          if (!e || !e->isKnown())
          { hasUnknown = true; break; }
    }
    if (hasUnknown)
    {
      // The type checker left some template args unresolved (e.g. a pack forwarded into
      // another template). Remove them so deduceArgs can re-add with concrete types.
      auto isUnresolved = [](const Ty* t) -> bool
      {
        if (!t || !t->isKnown()) return true;
        if (t->kind == TyKind::Pack)
          for (const Ty* e : t->args)
            if (!e || !e->isKnown()) return true;
        return false;
      };
      args.pairs.erase(
        std::remove_if(args.pairs.begin(), args.pairs.end(),
          [&](const auto& p) { return isUnresolved(p.second); }),
        args.pairs.end());
      std::vector<const Ty*> argTypes;
      for (Expr* a : call->args)
      {
        if (isPackExpansion(a))
        {
          Expr* pattern = static_cast<UnaryExpr*>(a)->operand;
          forEachPackElement(pattern, at, [&](size_t)
          {
            argTypes.push_back(pattern->ty ? argumentType(pattern, pattern->ty, types) : nullptr);
          });
        }
        else
          argTypes.push_back(a ? argumentType(a, a->ty, types) : nullptr);
      }
      global.deduceArgs(f, argTypes, args);
    }
  }
  return args;
}

auto codegen::Emitter::atomicBuiltin(FunctionDecl* f, Span<Expr*> callArgs, Node* at) -> std::optional<Value>
{
  std::string name = sym(f->name.last().symbol);
  if (name.rfind("kpp_atomic_", 0) != 0)
    return std::nullopt;
  // The operation, then the width in bits: kpp_atomic_load8, kpp_atomic_cas64.
  std::string op = name.substr(11);
  size_t digits = op.find_first_of("0123456789");
  std::string width = digits == std::string::npos ? "" : op.substr(digits);
  op = op.substr(0, digits);
  if (width != "8" && width != "16" && width != "32" && width != "64")
    unsupported(at, "the atomic builtin " + name + " (widths are 8, 16, 32 and 64)");
  int bits = std::stoi(width);
  std::string ty = "i" + width;
  std::string align = std::to_string(bits / 8);
  if (callArgs.empty())
    unsupported(at, "an atomic builtin without its pointer");
  std::string p = convert(rvalue(callArgs[0]), types.pointer(types.voidType()), at, true).text; // (the place, whatever its type)
  const Ty* valueTy = types.intType(bits, false);
  auto arg = [&](size_t i) { return convert(rvalue(callArgs[i]), valueTy, at).text; };
  std::string r = newTemp();
  if (op == "load")
  {
    fn += "  " + r + " = load atomic " + ty + ", ptr " + p + " seq_cst, align " + align + "\n";
    return Value{r, valueTy};
  }
  if (op == "store")
  {
    fn += "  store atomic " + ty + " " + arg(1) + ", ptr " + p + " seq_cst, align " + align + "\n";
    return Value{"", types.voidType()};
  }
  if (op == "add" || op == "exchange")
  {
    fn += "  " + r + " = atomicrmw " + (op == "add" ? "add" : "xchg") + " ptr " + p + ", " + ty + " " + arg(1) + " seq_cst, align " + align + "\n";
    return Value{r, valueTy};
  }
  if (op == "cas")
  {
    std::string exchange = arg(1), comparand = arg(2), pair = newTemp();
    fn += "  " + pair + " = cmpxchg ptr " + p + ", " + ty + " " + comparand + ", " + ty + " " + exchange + " seq_cst seq_cst, align " + align + "\n";
    fn += "  " + r + " = extractvalue { " + ty + ", i1 } " + pair + ", 0\n";
    return Value{r, valueTy};
  }
  unsupported(at, "the atomic builtin " + name);
}

auto codegen::Emitter::inlinedIdentity(FunctionDecl* f, const TemplateArgs& args, Span<Expr*> callArgs, Node* at) -> std::optional<Value>
{
  // (the library's move, forward and asConst, by declaration: GlobalScope::callIntrinsicOf)
  if (callArgs.size() != 1 || !callArgs[0] || isPackExpansion(callArgs[0]) || global.callIntrinsicOf(f) == GlobalScope::CallIntrinsic::None)
    return std::nullopt;
  std::string_view name = symbols.name(f->name.last().symbol);
  Signature s;
  const Ty* ret;
  {
    ArgsScope scope(*this, &args);
    s = signatureOf(f);
    ret = declType(f)->inner;
  }
  if (s.hasThis || s.hasSret || s.paramTys.size() != 1 || !s.paramTys[0] || !s.paramTys[0]->isReference() || !ret || !ret->isReference())
    return std::nullopt;
  std::string text = argumentText(s.paramTys[0], callArgs[0], at);
  if (text.rfind("ptr ", 0) != 0)
    unsupported(at, "an inlined " + std::string(name) + " given no address (" + text + ")"); // (a reference argument is always one)
  return Value{text.substr(4), ret, false};
}

auto codegen::Emitter::inlineScalar(const Ty* t) -> bool
{
  if (!t || !t->isKnown() || types.isDependent(t))
    return false;
  if (t->isReference())
    return types.stripRef(t)->isKnown();
  return t->isArithmetic() || t->kind == TyKind::Enum || t->kind == TyKind::Pointer || t->kind == TyKind::Nullptr;
}

auto codegen::Emitter::trivialExpression(Expr* e, FunctionDecl* def, int depth) -> bool
{
  if (!e || depth > 16 || !e->ty || !e->ty->isKnown() || types.isDependent(e->ty))
    return false;
  auto scalar = [&](Expr* x) { return x && x->ty && inlineScalar(types.unqualified(types.stripRef(x->ty))); };
  auto parameter = [&](Expr* x) {
    while (x && x->kind == NodeKind::ParenExpr)
      x = static_cast<ParenExpr*>(x)->inner;
    return x && x->kind == NodeKind::NameExpr && static_cast<NameExpr*>(x)->resolved &&
           static_cast<NameExpr*>(x)->resolved->kind == NodeKind::ParamDecl;
  };
  switch (e->kind)
  {
    case NodeKind::ParenExpr:
      return trivialExpression(static_cast<ParenExpr*>(e)->inner, def, depth + 1);
    case NodeKind::LiteralExpr:
    case NodeKind::ThisExpr:
      return true;
    case NodeKind::NameExpr:
    {
      Decl* d = static_cast<NameExpr*>(e)->resolved;
      if (!d)
        return false;
      if (d->kind == NodeKind::ParamDecl)
        return std::find(def->params.begin(), def->params.end(), d) != def->params.end() && !static_cast<ParamDecl*>(d)->isPack;
      if (d->kind == NodeKind::EnumeratorDecl)
        return true;
      return d->kind == NodeKind::VariableDecl && !(d->flags & (DF_Static | DF_Typedef)) && global.owningClass(d) &&
             static_cast<VariableDecl*>(d)->structuredBindings.empty(); // `items`: this->items
    }
    case NodeKind::MemberExpr:
    {
      auto* m = static_cast<MemberExpr*>(e);
      if (m->dynamicMember || !m->resolved || m->resolved->kind != NodeKind::VariableDecl || (m->resolved->flags & (DF_Static | DF_Typedef)) ||
          m->member.parts.size() != 1)
        return false;
      if ((m->op == P::Arrow || m->op == P::SafeArrow) &&
          (m->op == P::SafeArrow || !m->base->ty || types.unqualified(types.stripRef(m->base->ty))->kind != TyKind::Pointer))
        return false; // `->` of a class (its operator->), or `?->`
      return trivialExpression(m->base, def, depth + 1);
    }
    case NodeKind::UnaryExpr:
    {
      auto* u = static_cast<UnaryExpr*>(e);
      if (u->isKeyword || !scalar(u->operand))
        return false;
      P op = P(u->op);
      if (op == P::PlusPlus || op == P::MinusMinus)
        return !parameter(u->operand) && trivialExpression(u->operand, def, depth + 1); // `++this->count`
      if (op == P::Amp)
        return !parameter(u->operand) && trivialExpression(u->operand, def, depth + 1); // `&this->items[i]`
      if (op != P::Bang && op != P::Minus && op != P::Plus && op != P::Tilde && op != P::Star)
        return false;
      return trivialExpression(u->operand, def, depth + 1);
    }
    case NodeKind::BinaryExpr:
    {
      auto* b = static_cast<BinaryExpr*>(e);
      if (b->isKeyword || !scalar(b->lhs) || !scalar(b->rhs))
        return false;
      P op = P(b->op);
      if (op == P::DotStar || op == P::ArrowStar || op == P::Comma)
        return false;
      if (isAssignment(op) && parameter(b->lhs))
        return false; // a parameter is only read
      return trivialExpression(b->lhs, def, depth + 1) && trivialExpression(b->rhs, def, depth + 1);
    }
    case NodeKind::IndexExpr:
    {
      auto* x = static_cast<IndexExpr*>(e);
      if (x->args.size() != 1 || !x->base->ty || !scalar(x->args[0]))
        return false;
      const Ty* bt = types.unqualified(types.stripRef(x->base->ty));
      if (bt->kind != TyKind::Pointer && bt->kind != TyKind::Array)
        return false; // a class's operator[]
      return trivialExpression(x->base, def, depth + 1) && trivialExpression(x->args[0], def, depth + 1);
    }
    case NodeKind::ConditionalExpr:
    {
      auto* c = static_cast<ConditionalExpr*>(e);
      return c->then && scalar(c->cond) && scalar(c->then) && scalar(c->else_) && trivialExpression(c->cond, def, depth + 1) &&
             trivialExpression(c->then, def, depth + 1) && trivialExpression(c->else_, def, depth + 1);
    }
    case NodeKind::CastExpr:
    case NodeKind::NamedCastExpr:
    {
      Expr* operand = e->kind == NodeKind::CastExpr ? static_cast<CastExpr*>(e)->operand : static_cast<NamedCastExpr*>(e)->operand;
      if (e->kind == NodeKind::NamedCastExpr && static_cast<NamedCastExpr*>(e)->castKind == Kw::dynamic_cast_)
        return false;
      return inlineScalar(types.unqualified(e->ty)) && scalar(operand) && trivialExpression(operand, def, depth + 1);
    }
    default:
      return false;
  }
}

auto codegen::Emitter::trivialDefinition(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy, FunctionDecl** written) -> FunctionDecl*
{
  if ((f->flags & (DF_Constructor | DF_Destructor | DF_Variadic | DF_Default | DF_Delete)) || (f->attributes & ATTR_NoInline) ||
      f->builtin || global.isExternC(f))
    return nullptr;
  // The definition the call would emit (useFunctionOnce): an explicit specialisation for these
  // arguments (`template<> const char* name<bool>()`), an out-of-line definition, the instance
  // of either under its own parameters.
  Decl* defined = global.definitionOf(f, args);
  auto* chosen = defined && defined->kind == NodeKind::FunctionDecl ? static_cast<FunctionDecl*>(defined) : f;
  if (written)
    *written = chosen;
  if (!chosen->body || (chosen->file && chosen->file->isImported))
    return nullptr;
  FunctionDecl* def = args.empty() ? chosen : global.instanceBody(chosen, definitionArguments(f, chosen, args), classTy);
  if (!def || !def->body || def->body->kind != NodeKind::BlockStmt || inlining.count(def) || retypedPerElement(def))
    return nullptr;
  if (auto it = trivialDefinitions.find(def); it != trivialDefinitions.end())
    return it->second;
  FunctionDecl* result = nullptr;
  auto* block = static_cast<BlockStmt*>(def->body);
  Expr* e = nullptr;
  if (block->stmts.size() == 1 && block->stmts[0]->kind == NodeKind::ReturnStmt)
    e = static_cast<ReturnStmt*>(block->stmts[0])->value;
  else if (block->stmts.size() == 1 && block->stmts[0]->kind == NodeKind::ExprStmt)
    e = static_cast<ExprStmt*>(block->stmts[0])->expr;
  if (e && def->params.size() == f->params.size() && trivialExpression(e, def))
  {
    ArgsScope scope(*this, &args);
    Signature s = signatureOf(f);
    const Ty* ret = declType(f)->inner;
    bool ok = !s.hasSret && !s.variadic && ret && (ret->kind == TyKind::Void || inlineScalar(ret));
    if (ok && block->stmts[0]->kind == NodeKind::ReturnStmt && ret->kind != TyKind::Void && ret->isReference())
    {
      // A reference result: the lvalue E is, of the referred type itself (no base adjustment).
      const Ty* want = types.unqualified(types.stripRef(ret));
      const Ty* got = types.unqualified(types.stripRef(e->ty));
      ok = want == got || (isClass(want) && isClass(got) && classKey(want) == classKey(got));
    }
    for (size_t i = 0, slot = (s.hasThis ? 1 : 0); ok && i < f->params.size(); ++i, ++slot)
      ok = slot < s.paramTys.size() && inlineScalar(s.paramTys[slot]) && !static_cast<ParamDecl*>(def->params[i])->isPack;
    if (ok)
      result = def;
  }
  trivialDefinitions.emplace(def, result);
  return result;
}

auto codegen::Emitter::trivialConstructor(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy, Node* at, FunctionDecl** written) -> FunctionDecl*
{
  if (!(f->flags & DF_Constructor) || (f->flags & (DF_Variadic | DF_Default | DF_Delete)) || (f->attributes & ATTR_NoInline) || f->builtin ||
      !classTy || !isClass(classTy))
    return nullptr;
  Decl* owner = global.owningClass(f);
  ClassDecl* cls = classDef(classTy->decl, at);
  if (!owner || global.definitionOf(owner) != cls || cls->isTypedUnion)
    return nullptr;
  Decl* defined = global.definitionOf(f, args);
  auto* chosen = defined && defined->kind == NodeKind::FunctionDecl ? static_cast<FunctionDecl*>(defined) : f;
  if (written)
    *written = chosen;
  if (!chosen->body || (chosen->file && chosen->file->isImported))
    return nullptr;
  FunctionDecl* def = args.empty() ? chosen : global.instanceBody(chosen, definitionArguments(f, chosen, args), classTy);
  if (!def || !def->body || def->body->kind != NodeKind::BlockStmt || !static_cast<BlockStmt*>(def->body)->stmts.empty() || inlining.count(def) ||
      retypedPerElement(def) || def->params.size() != f->params.size())
    return nullptr;
  if (auto it = trivialDefinitions.find(def); it != trivialDefinitions.end())
    return it->second;
  const StructInfo& info = structFor(classTy, at);
  bool ok = info.bases.empty() && !info.polymorphic && !info.isUnion && info.anonymous.empty() && info.bitfields.empty();
  auto initialized = [&](VariableDecl* v)
  {
    for (const FunctionDecl::CtorInit& init : def->ctorInits)
      if (!init.member.empty() && init.member.last().symbol == v->name.last().symbol)
        return true;
    return false;
  };
  TemplateArgs classArgs = global.classArgsOf(canonicalClass(classTy, at));
  for (Decl* d : info.fieldDecls)
  {
    if (!ok)
      break;
    auto* v = d->kind == NodeKind::VariableDecl ? static_cast<VariableDecl*>(d) : nullptr;
    const Ty* declared = v ? global.typeOfDecl(v) : nullptr;
    const Ty* ft = info.fieldTypes[fieldIndexOf(info, d)];
    ok = v && !v->name.empty() && !ft->isReference() && !(declared && declared->isReference()) && inlineScalar(ft);
    if (ok && v->init && !initialized(v))
    {
      Expr* init = global.instanceInitializer(v, classArgs);
      ok = init && trivialExpression(init, def);
    }
  }
  for (const FunctionDecl::CtorInit& init : def->ctorInits)
  {
    if (!ok)
      break;
    bool member = false;
    if (init.member.parts.size() == 1 && !init.member.last().hasTemplateArgs && !init.isPack)
      for (Decl* d : info.fieldDecls)
        member = member || (d->kind == NodeKind::VariableDecl && !static_cast<VariableDecl*>(d)->name.empty() &&
                            static_cast<VariableDecl*>(d)->name.last().symbol == init.member.last().symbol);
    ok = member && init.args.size() <= 1 && (init.args.empty() || trivialExpression(init.args[0], def));
  }
  if (ok)
  {
    ArgsScope scope(*this, &args);
    Signature s = signatureOf(f);
    ok = s.hasThis && !s.hasSret && !s.variadic;
    for (size_t i = 0, slot = 1; ok && i < f->params.size(); ++i, ++slot)
      ok = slot < s.paramTys.size() && inlineScalar(s.paramTys[slot]) && !static_cast<ParamDecl*>(def->params[i])->isPack;
  }
  FunctionDecl* result = ok ? def : nullptr;
  trivialDefinitions.emplace(def, result);
  return result;
}

auto codegen::Emitter::inlineSlot(const std::string& key, const std::string& type) -> std::string
{
  std::string& slot = inlineSlots[key + " " + type];
  if (slot.empty())
  {
    slot = "%inl" + std::to_string(temp++);
    allocas += "  " + slot + " = alloca " + type + "\n";
  }
  return slot;
}

auto codegen::Emitter::inlinedBody(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy, const std::string& thisArg, const std::string& sret, Span<Expr*> callArgs, Node* at) -> std::optional<Value>
{
  if (!sret.empty() || callArgs.size() != f->params.size())
    return std::nullopt;
  for (Expr* a : callArgs)
    if (!a || isPackExpansion(a) || a->kind == NodeKind::BraceInitExpr)
      return std::nullopt;
  FunctionDecl* written = nullptr;
  FunctionDecl* def = trivialDefinition(f, args, classTy, &written);
  if (!def)
    return std::nullopt;
  Signature s;
  const Ty* ret;
  {
    ArgsScope scope(*this, &args);
    s = signatureOf(f);
    ret = declType(f)->inner;
  }
  if (s.hasThis && thisArg.empty())
    return std::nullopt;
  Value result{"", types.voidType(), false};
  inlineCall(def, written, args, classTy, s, thisArg, callArgs, at, [&]
  {
    auto* statement = static_cast<BlockStmt*>(def->body)->stmts[0];
    if (statement->kind == NodeKind::ExprStmt)
      rvalue(static_cast<ExprStmt*>(statement)->expr);
    else
    {
      Expr* e = static_cast<ReturnStmt*>(statement)->value;
      if (ret->kind == TyKind::Void)
        rvalue(e);
      else if (ret->isReference())
      {
        checkRvalueReferenceResult(ret, e); // (`template<class T> T&& g(T&& x) { return x; }`, inlined: never emitted itself)
        result = {isAggregate(types.stripRef(ret)) ? rvalue(e).text : lvalue(e), ret, false};
      }
      else
        result = {convert(rvalue(e), ret, e, false, ConversionContext::Copy).text, ret, false}; // (a return copy-initializes its result)
    }
  });
  return result;
}

auto codegen::Emitter::inlinedConstructor(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy, const std::string& thisArg, const std::string& sret, Span<Expr*> callArgs, Node* at) -> std::optional<Value>
{
  if (!(f->flags & DF_Constructor) || thisArg.empty() || !sret.empty() || callArgs.size() != f->params.size())
    return std::nullopt;
  for (Expr* a : callArgs)
    if (!a || isPackExpansion(a) || a->kind == NodeKind::BraceInitExpr)
      return std::nullopt;
  FunctionDecl* written = nullptr;
  FunctionDecl* def = trivialConstructor(f, args, classTy, at, &written);
  if (!def)
    return std::nullopt;
  Signature s;
  {
    ArgsScope scope(*this, &args);
    s = signatureOf(f);
  }
  inlineCall(def, written, args, classTy, s, thisArg, callArgs, at, [&]
  {
    // The member initializers make no temporary (trivialConstructor), so nothing is destroyed
    // after them as the constructor itself destroys its own: the caller's live on.
    size_t temps = statementTemps.size();
    initializeMembers(thisArg, classTy, def->ctorInits, def);
    if (statementTemps.size() != temps)
      unsupported(def, "an inlined constructor whose member initializers made a temporary");
  });
  return Value{"", types.voidType(), false};
}

// A call in test code of a test function (design.md 8.7): the runner keeps this call's location for
// the frame the callee's body opens, its "called from" line.
auto codegen::Emitter::noteTestCall(FunctionDecl* f, const TemplateArgs& args, Node* at) -> void
{
  Decl* writtenIn = lambdaContext ? lambdaContext->parent : nullptr;
  const FunctionDecl* caller = writtenIn && writtenIn->kind == NodeKind::FunctionDecl ? static_cast<FunctionDecl*>(writtenIn) : currentFunction;
  if (!caller || !caller->testCode)
    return;
  Decl* definition = global.definitionOf(f, args); // (the frame is the body's, which may be out of the class)
  if (!definition || definition->kind != NodeKind::FunctionDecl || static_cast<FunctionDecl*>(definition)->testCode != 2)
    return;
  auto [line, column] = lineColumnOf(at->kind == NodeKind::CallExpr ? static_cast<CallExpr*>(at)->callee->token : at->token);
  emitCall("", "void @kpp_test_call(ptr " + cStringConstant(programPathOf(currentFile)) + ", i32 " + lineOperand(line) + ", i32 " +
                   std::to_string(column) + ")", false);
}

// What runs between a noted call and the frame that takes it (design.md 8.7), so that a noted site
// never reaches another function's frame. A constructor's member initializers run first and may
// throw: its site waits in a slot of its own until they are done.
auto codegen::Emitter::holdTestCall(const FunctionDecl* f) -> std::string
{
  if (f->testCode != 2)
    return {};
  std::string held = newTemp();
  allocas += "  " + held + " = alloca { ptr, i32, i32 }\n";
  emitCall("", "void @kpp_test_hold(ptr " + held + ")", false);
  return held;
}

auto codegen::Emitter::releaseTestCall(const std::string& held) -> void
{
  if (!held.empty())
    emitCall("", "void @kpp_test_release(ptr " + held + ")", false);
}

// A virtual call goes by the function it names: the overrider it reaches may open no frame.
auto codegen::Emitter::forgetTestCall(FunctionDecl* f) -> void
{
  if (f->testCode == 2 || (f->flags & (DF_Static | DF_Constructor)) || !currentClassTy || !isClass(currentClassTy))
    return;
  const StructInfo& info = structFor(currentClassTy, f);
  if (info.vtables.empty() || std::none_of(info.vtables[0].slots.begin(), info.vtables[0].slots.end(), [](const VtableSlot& s) { return s.overridesTest; }))
    return;
  if (int slot = slotOf(currentClassTy, f, f); slot >= 0 && info.vtables[0].slots[slot].overridesTest)
    emitCall("", "void @kpp_test_forget()", false);
}

// A failed check of a call shows the call's arguments, as the function took them (design.md 8.7): the
// values are already there when it fails, so a check that passes runs nothing for them. But for a
// class by value, which the callee owns and may change: a copy of its bytes, or, when the callee
// destroys it, no value.
auto codegen::Emitter::checkedCallOf(const AssertStmt* a) -> Node*
{
  if (!a->checksCall)
    return nullptr;
  Expr* e = a->cond;
  if (e->kind == NodeKind::BinaryExpr)
    e = static_cast<BinaryExpr*>(e)->lhs; // (`cond || __kppMessage(...)`: the parser's)
  return e->kind == NodeKind::UnaryExpr ? static_cast<UnaryExpr*>(e)->operand : e;
}

// (at the call itself: a conversion of one of its arguments goes by the same node)
auto codegen::Emitter::isCheckedCall(Span<Expr*> callArgs, Node* at) const -> bool
{
  return at == checkedCall && callArgs.begin() == static_cast<CallExpr*>(checkedCall)->args.begin();
}

auto codegen::Emitter::keepCheckedArguments(const std::vector<std::pair<const Ty*, std::string>>& list, size_t leading, Node* at) -> void
{
  for (size_t i = leading; i < list.size(); ++i)
  {
    const auto& [t, text] = list[i];
    size_t space = text.rfind(' ');
    CheckedArgument kept;
    if (t && space != std::string::npos && !isInitializerListType(types.unqualified(types.stripRef(t)))) // (a list has no text)
    {
      kept.type = types.unqualified(types.stripRef(t));
      kept.value = text.substr(space + 1);
      std::string passedAs = text.substr(0, space);
      if (!t->isReference() && isClass(kept.type) && passedAs == "ptr")
      {
        if (needsDestruction(kept.type, at))
          kept.type = nullptr;
        else
        {
          std::string copy = newObjectSlot(kept.type, at);
          copyBytes(copy, kept.value, kept.type, at);
          kept.value = copy;
        }
      }
      else if (!t->isReference())
        kept.scalar = std::move(passedAs);
    }
    checkedArguments.push_back(std::move(kept));
  }
}

auto codegen::Emitter::showCheckedArguments(const std::string& cond, Node* at) -> void
{
  if (checkedArguments.empty())
    return;
  std::string showL = newLabel("check.values"), goL = newLabel("check.go");
  fn += "  br i1 " + cond + ", label %" + goL + ", label %" + showL + "\n";
  startBlock(showL);
  for (const CheckedArgument& kept : checkedArguments)
  {
    FunctionDecl* show = kept.type ? global.findFreeFunction("__kppCallValue", {kept.type}, nullptr) : global.findFreeFunction("__kppCallValue", {}, nullptr);
    if (!show)
      unsupported(at, "a check of a call outside a test build");
    TemplateArgs args;
    if (!show->templateParams.empty())
      args.pairs.push_back({show->templateParams[0], kept.type});
    std::string value = kept.value;
    if (!kept.scalar.empty())
    {
      value = newTemp();
      allocas += "  " + value + " = alloca " + kept.scalar + "\n";
      fn += "  store " + kept.scalar + " " + kept.value + ", ptr " + value + "\n";
    }
    emitCall("", "void " + useFunction(show, args, nullptr, at) + "(" + (kept.type ? "ptr " + value : std::string()) + ")", true);
  }
  checkedArguments.clear();
  emitBranch(goL);
  startBlock(goL);
}

auto codegen::Emitter::callFunction(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy, const std::string& thisArg, const std::string& sret, Span<Expr*> callArgs, Node* at,
                                    const std::string& indirect, const std::vector<std::string>& leading, std::vector<std::string>* emitted) -> Value
{
  // A closure's call operator (the resolver's closureCallOperator) is called as its lambda
  // (memberCall: `o(41)` through a class derived from closures); any other call of it (`o.operator()(41)`)
  // is not followed yet, never a call of a function of its name.
  if (global.closureOfCallOperator(f))
    unsupported(at, "a call of a lambda's call operator by its name, through a class derived from its closure");
  if ((f->flags & DF_Delete) && global.definitionOf(f, args) == f) // a deleted template's explicit specialisation is no deleted function
    unsupported(at, "a call to the deleted function " + qualifiedName(f));
  if (global.isExternC(f))
    if (std::optional<Value> v = atomicBuiltin(f, callArgs, at))
      return *v;
  if (f->flags & DF_Constructor) // (access: the checker does not choose constructors)
    if (std::string denied = global.checkConstructorAccess(f, implicitConstructionOf ? implicitConstructionOf : typingContext(), implicitConstructionOf ? implicitConstructionOf : at);
        !denied.empty())
      error(at, denied); // (at the construction: an implicit one's names the class whose constructor it is)
  if (f->flags & DF_Default)
    return defaultedCall(f, classTy, thisArg, callArgs, at);
  if (!conditionHolds(f))
    return {"", types.voidType(), false}; // conditional(false): no call, arguments not evaluated
  // More arguments than parameters (no C `...`, no pack) is never a call that drops some: the checker's error, and this one
  // where it let one through (until 2026-11-10 a trivial function's inlined body took the first ones, silently).
  if (!(f->flags & DF_Variadic) && leading.size() + callArgs.size() > f->params.size() &&
      std::none_of(f->params.begin(), f->params.end(), [](Decl* p) { return static_cast<ParamDecl*>(p)->isPack; }) &&
      std::none_of(callArgs.begin(), callArgs.end(), [&](Expr* e) { return isPackExpansion(e); }))
    unsupported(at, "a call to " + qualifiedName(f) + " with " + std::to_string(leading.size() + callArgs.size()) + " arguments, which takes " +
                      std::to_string(f->params.size()) + " (and the overload resolution let it through)");
  if (indirect.empty() && sh.inlineTrivial && leading.empty() && !emitted && !isCheckedCall(callArgs, at))
  {
    if (std::optional<Value> v = inlinedIdentity(f, args, callArgs, at))
      return *v;
    if (std::optional<Value> v = inlinedBody(f, args, classTy, thisArg, sret, callArgs, at))
      return *v;
    if (std::optional<Value> v = inlinedConstructor(f, args, classTy, thisArg, sret, callArgs, at))
      return *v;
  }
  std::string name = indirect.empty() ? useFunction(f, args, classTy, at) : indirect;
  Signature s;
  const Ty* ret;
  {
    ArgsScope scope(*this, &args);
    s = signatureOf(f);
    ret = declType(f)->inner;
  }
  std::string argText;
  size_t slot = 0;
  if (s.hasThis)
  {
    if (thisArg.empty())
      unsupported(at, "a method call without an object");
    argText += "ptr " + thisArg;
    ++slot;
  }
  std::string resultAddr = sret;
  bool resultTemp = false; // a temporary for the result, owned by the statement once the call returned it
  // A C function's struct result that comes back in registers (cPassing): no slot argument; the
  // registers are stored into the slot after the call.
  bool cFunction = isCFunction(f);
  CPassing cResult;
  if (s.hasSret && cFunction)
    cResult = cPassing(ret, at);
  if (s.hasSret)
  {
    if (resultAddr.empty())
    {
      resultAddr = newObjectSlot(ret, at);
      resultTemp = true;
    }
    if (cResult.inMemory)
      argText += std::string(argText.empty() ? "" : ", ") + cSlotArgument(cFunction ? ret : nullptr, at) + resultAddr;
    ++slot;
  }
  // The arguments with their types (a variadic one's its own), each as argumentText gives it; a C
  // function's structs by value lowered after (lowerCArguments).
  std::vector<std::pair<const Ty*, std::string>> argList;
  size_t position = slot;
  for (const std::string& text : leading)
  {
    argList.push_back({position < s.paramTys.size() ? s.paramTys[position] : nullptr, text});
    ++position;
  }
  for (Expr* arg : callArgs)
  {
    if (isPackExpansion(arg))
    {
      Expr* pattern = static_cast<UnaryExpr*>(arg)->operand;
      forEachPackElement(pattern, at, [&](size_t)
      {
        const Ty* pt = position < s.paramTys.size() ? s.paramTys[position] : nullptr;
        std::string text = argumentText(pt, pattern, at);
        argList.push_back({pt, std::move(text)});
        ++position;
      });
      continue;
    }
    const Ty* pt = position < s.paramTys.size() ? s.paramTys[position] : nullptr;
    std::string text = argumentText(pt, arg, at);
    if (emitted)
      emitted->push_back(text);
    argList.push_back({pt ? pt : arg->ty ? withCurrentArgs(types.unqualified(types.stripRef(arg->ty))) : nullptr, std::move(text)});
    ++position;
  }
  if (isCheckedCall(callArgs, at))
    keepCheckedArguments(argList, leading.size(), at);
  if (cFunction)
    lowerCArguments(argList, s.hasSret && cResult.inMemory, at);
  for (auto& [t, text] : argList)
    argText += std::string(argText.empty() ? "" : ", ") + text;
  size_t provided = position - slot; // argument values emitted (pack expansions counted element by element)
  if (provided < f->params.size())
  {
    // Parameters the call left out take their default arguments, evaluated at the call site
    // (the expression's literals belong to the callee's file; a `caller` location to this one).
    // A template's are the ones of the callee's own instantiation, typed under its arguments
    // (`ScrollBar(const Style* s = &Policy::defaultStyle)` with Policy = VerticalPolicy).
    ParsedFile* callerFile = currentFile;
    FunctionDecl* instance = nullptr;
    for (size_t i = provided; i < f->params.size(); ++i)
    {
      auto* pd = static_cast<ParamDecl*>(f->params[i]);
      if (pd->isPack)
        break;
      if (!pd->defaultValue)
        unsupported(at, "a call to " + qualifiedName(f) + " without an argument for its parameter " +
                          (pd->name != UINT32_MAX ? sym(pd->name) : std::to_string(i + 1)) + " (and the overload resolution let it through)");
      if (!instance)
        instance = args.empty() ? f : global.instanceBody(f, args, classTy);
      Expr* value = i < instance->params.size() ? static_cast<ParamDecl*>(instance->params[i])->defaultValue : nullptr;
      if (!value)
        unsupported(at, "the default argument of " + qualifiedName(f) + " missing from its instantiation");
      const Ty* pt = position < s.paramTys.size() ? s.paramTys[position] : nullptr;
      std::string text;
      if (value->kind == NodeKind::CallerExpr)
        text = "ptr " + callerLocation(pt, at);
      else
      {
        // Emitted from the callee's declaration: its own node (and file) for what it reports,
        // not the caller's token read in the callee's file.
        Restore<ParsedFile*> fileRestore(currentFile, callerFile);
        currentFile = f->file;
        // Names in it are the callee's (`Limits<ModifierValueType>::max()` with the class's
        // `using ModifierValueType = double;`), not those of the caller's scope.
        Restore<Decl*> contextRestore(initializerOf);
        initializerOf = instance;
        ArgsScope calleeArgs(*this, args.empty() ? currentArgs : &args); // CharacterModifier<double>'s ModifierValueType
        // A bare static member is the callee's class instance's (`int scaled(int n = scale)` in Box<T>
        // called as Box<int>().scaled(): Box<int>::scale), not the caller's class's, nor the
        // template's own object (until 2026-11-10 a call from main read a global made for Box itself,
        // holding the initializer: E's battery a2).
        Restore<const Ty*> classRestore(currentClassTy);
        if (classTy)
          currentClassTy = classTy;
        text = argumentText(pt, value, value);
      }
      argText += std::string(argText.empty() ? "" : ", ") + text;
      ++position;
    }
  }
  std::string sig = (s.variadic || !indirect.empty()) ? signatureText(s) + " " : s.returnType + " ";
  if (cFunction)
  {
    auto [returnType, params] = cDeclaration(f, s);
    sig = (s.variadic || !indirect.empty()) ? returnType + " (" + withoutParameterAttributes(params) + ") " : returnType + " ";
    if (!cResult.inMemory)
    {
      // The result in registers, stored into the slot part by part.
      std::string r = newTemp();
      emitCall(r + " = ", sig + name + "(" + argText + ")", false);
      for (size_t i = 0; i < cResult.parts.size(); ++i)
      {
        std::string part = r;
        if (cResult.parts.size() > 1)
        {
          part = newTemp();
          fn += "  " + part + " = extractvalue " + returnType + " " + r + ", " + std::to_string(i) + "\n";
        }
        // (each part's type covers exactly the struct's bytes it holds: i32 for the last four; AAPCS64's
        // wider ones are stored narrowed)
        storeCPart(resultAddr, cResult.offsets[i], cResult.parts[i], part, cResult.size - cResult.offsets[i], true);
      }
      if (resultTemp)
        statementTemps.push_back({resultAddr, ret});
      return {resultAddr, ret, sret.empty()};
    }
  }
  noteTestCall(f, args, at);
  bool mayThrow = !global.isExternC(f); // C functions do not throw kpp exceptions
  if (s.returnType == "void")
  {
    emitCall("", sig + name + "(" + argText + ")", mayThrow);
    if (resultTemp)
      statementTemps.push_back({resultAddr, ret}); // not before: a call that throws built no result to destroy
    if (s.hasSret)
      return {resultAddr, ret, sret.empty()};
    return {"", types.voidType(), false};
  }
  std::string r = newTemp();
  emitCall(r + " = ", sig + name + "(" + argText + ")", mayThrow);
  return {r, ret, false};
}

auto codegen::Emitter::defaultedCall(FunctionDecl* f, const Ty* classTy, const std::string& thisArg, Span<Expr*> args, Node* at) -> Value
{
  if (!classTy || thisArg.empty())
    unsupported(at, "a defaulted function without its object");
  const Ty* t = types.unqualified(types.stripRef(classTy));
  std::string src;
  if (!args.empty())
  {
    if (args.size() != 1)
      unsupported(at, "a defaulted function with " + std::to_string(args.size()) + " arguments");
    // A value of another type, neither t nor a class derived from it, for an assignment or a
    // comparison (`n = "abc"`, `m = String("def")` through Name's implicit constructors,
    // `fixedPoint == 0`): made a temporary of t first, the value evaluated once.
    const Ty* argTy = args[0]->ty ? withCurrentArgs(types.unqualified(types.stripRef(args[0]->ty))) : nullptr;
    std::vector<std::pair<std::string, uint32_t>> path;
    bool related = argTy && isClass(argTy) && !types.isDependent(argTy) &&
                   (canonicalClass(argTy, at) == canonicalClass(t, at) || basePath(argTy, t, path, at));
    // (`data.previewIcons = {Signal(4), Signal(5)}` through a defaulted `operator=(Icons&&)`:
    // the list makes the temporary, as list-initialization of t.)
    bool list = args[0]->kind == NodeKind::BraceInitExpr && !static_cast<BraceInitExpr*>(args[0])->text; // (a Text's packed arguments are no list)
    if (((argTy && argTy->isKnown() && !related) || list) && !(f->flags & (DF_Constructor | DF_Destructor)) && isClass(t))
    {
      std::string tmp = newObjectSlot(t, at);
      Expr* one = args[0];
      constructInto(tmp, t, Span<Expr*>{&one, 1}, at);
      if (needsDestruction(t, at))
        statementTemps.push_back({tmp, t}); // destroyed with the statement's other temporaries
      return defaultedOperation(f, t, thisArg, tmp, at);
    }
    Value v = rvalue(args[0]);
    if (!isClass(v.ty))
    {
      // A vector the checker could not type (an SSE operation clang's intrin.h has as a macro,
      // _mm_shuffle_ps) given to a defaulted copy or move of a class that wraps it (float32x4
      // wrapping __m128): the class made of it by its constructor, as a typed one is above
      // (constructFromVector), the object itself or, for an assignment, a temporary assigned from.
      if (v.ty && v.ty->kind == TyKind::Vector && isClass(t))
      {
        if (f->flags & DF_Constructor)
        {
          constructFromVector(thisArg, t, v, false, at);
          return {"", types.voidType(), false};
        }
        std::string tmp = newObjectSlot(t, at);
        constructFromVector(tmp, t, v, true, at);
        if (needsDestruction(t, at))
          statementTemps.push_back({tmp, t}); // destroyed with the statement's other temporaries
        return defaultedOperation(f, t, thisArg, tmp, at);
      }
      // For defaulted comparisons (operator==, operator<=>), convert the scalar to
      // a temporary of the class type through its converting constructor, e.g. `fixedPoint == 0`.
      if (!(f->flags & (DF_Constructor | DF_Destructor)) && isClass(t))
      {
        std::string tmp = newObjectSlot(t, at);
        Expr* one = args[0];
        constructInto(tmp, t, Span<Expr*>{&one, 1}, at);
        src = tmp;
      }
      else
        unsupported(at, "a defaulted function given a non-class");
    }
    if (src.empty())
    {
      src = v.text;
      if (classKey(v.ty) != classKey(t) && !baseAddress(v.ty, t, v.text, src, at))
        unsupported(at, "a defaulted function given an unrelated class");
    }
  }
  return defaultedOperation(f, t, thisArg, src, at);
}

auto codegen::Emitter::defaultedOperation(FunctionDecl* f, const Ty* t, const std::string& thisArg, const std::string& src, Node* at) -> Value
{
  bool move = false;
  if (!f->params.empty())
  {
    TemplateArgs cargs = global.classArgsOf(canonicalClass(t, at));
    ArgsScope scope(*this, &cargs);
    const Ty* pt = declType(f->params[0]);
    move = pt && pt->kind == TyKind::RRef;
  }
  if (f->flags & DF_Constructor)
  {
    if (src.empty())
    {
      storeZero(t, thisArg, at);
      initializeMembers(thisArg, t, {}, at);
      return {"", types.voidType(), false};
    }
    memberwiseConstruct(thisArg, src, t, at, move);
    return {"", types.voidType(), false};
  }
  if (f->flags & DF_Destructor)
  {
    destroyMembers(thisArg, t, at);
    return {"", types.voidType(), false};
  }
  if (!f->name.empty() && f->name.last().special == 2 && !src.empty())
  {
    P op = P(f->name.last().opToken);
    if (op == P::Assign)
    {
      memberwiseAssign(thisArg, src, t, at, move);
      return {thisArg, types.lref(t), false};
    }
    if (op == P::EqEq || op == P::NotEq)
    {
      Value eq = memberwiseEqual(thisArg, src, t, at);
      if (op == P::EqEq)
        return eq;
      std::string r = newTemp();
      fn += "  " + r + " = xor i1 " + eq.text + ", true\n";
      return {r, types.boolType(), false};
    }
    if (op == P::Spaceship)
    {
      // `operator<=>(const T&) const = default`: the order of the bases and members as the
      // value of `a <=> b` (inlined at a call; emitFunction moves it to the result slot).
      return orderingObject(memberwiseOrder(thisArg, src, t, at), at);
    }
  }
  unsupported(at, "the defaulted function " + qualifiedName(f));
}

auto codegen::Emitter::memberwiseEqual(const std::string& a, const std::string& b, const Ty* t, Node* at) -> Value
{
  const StructInfo& info = structFor(t, at);
  std::string result = "true";
  auto combine = [&](const std::string& v)
  {
    if (result == "true")
    {
      result = v;
      return;
    }
    std::string r = newTemp();
    fn += "  " + r + " = and i1 " + result + ", " + v + "\n";
    result = r;
  };
  for (auto& [baseDef, index] : info.bases)
    combine(objectsEqual(gep(info.name, a, index), gep(info.name, b, index), info.fieldTypes[index], at).text);
  for (Decl* d : info.fieldDecls)
  {
    uint32_t index = fieldIndexOf(info, d);
    const Ty* ft = info.fieldTypes[index];
    std::string fa = gep(info.name, a, index), fb = gep(info.name, b, index);
    if (d->kind == NodeKind::VariableDecl && static_cast<VariableDecl*>(d)->bitfield)
    {
      for (auto [field, bits, fieldTy] : bitfieldsOfUnit(t, d, at)) // (never the unit's other bits)
        combine(compare(P::EqEq, loadBitfield(fa, *bits, fieldTy, at), loadBitfield(fb, *bits, fieldTy, at), at).text);
      continue;
    }
    combine(partsEqual(fa, fb, ft, at).text);
  }
  return {result, types.boolType(), false};
}

auto codegen::Emitter::bitfieldsOfUnit(const Ty* t, Decl* first, Node* at) -> std::vector<UnitField>
{
  const StructInfo& info = structFor(t, at);
  // (each field's declared type, as the layout reads it: the unit's own is no field's, a byte array
  // off Windows, `uint8[2]`, and on Windows the first field's, an int beside `unsigned b : 4`)
  TemplateArgs classArgs = global.classArgsOf(canonicalClass(t, at));
  std::vector<UnitField> out;
  for (Decl* m : unitMembers(info, classDef(t->decl, at), first))
  {
    const BitfieldInfo* bits = bitfieldIn(info, m);
    if (!bits || static_cast<VariableDecl*>(m)->name.empty())
      continue;
    const Ty* declared = global.typeOfDeclIn(m, classArgs);
    if (!declared || !declared->isKnown())
      unsupported(at, "a bit field of unknown type in a defaulted comparison");
    out.push_back({m, bits, types.unqualified(types.stripRef(declared))});
  }
  return out;
}

auto codegen::Emitter::partsEqual(const std::string& a, const std::string& b, const Ty* t, Node* at) -> Value
{
  if (isClass(t))
    return objectsEqual(a, b, t, at);
  if (t->kind == TyKind::Array)
    return arraysEqual(a, b, t, at);
  return compare(P::EqEq, load(a, t, at), load(b, t, at), at);
}

auto codegen::Emitter::arraysEqual(const std::string& a, const std::string& b, const Ty* t, Node* at) -> Value
{
  if (!t->count || !t->inner)
    unsupported(at, "a defaulted comparison of an array member without its length");
  std::string slot = "%equal" + std::to_string(temp++), index = "%index" + std::to_string(temp++);
  allocas += "  " + slot + " = alloca i1\n  " + index + " = alloca i64\n";
  fn += "  store i1 true, ptr " + slot + "\n  store i64 0, ptr " + index + "\n";
  std::string arrayType = llType(t, at);
  std::string cond = newLabel("arrayequal.cond"), body = newLabel("arrayequal.body"), next = newLabel("arrayequal.next"), done = newLabel("arrayequal.done");
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
  Value same = partsEqual(ea, eb, t->inner, at);
  fn += "  store i1 " + same.text + ", ptr " + slot + "\n";
  fn += "  br i1 " + same.text + ", label %" + next + ", label %" + done + "\n";
  blockTerminated = true;
  startBlock(next);
  std::string i2 = newTemp();
  fn += "  " + i2 + " = add i64 " + i + ", 1\n  store i64 " + i2 + ", ptr " + index + "\n";
  emitBranch(cond);
  startBlock(done);
  return load(slot, types.boolType(), at);
}

auto codegen::Emitter::comparedAt(P op, const std::string& a, const std::string& b, const Ty* t, Node* at) -> Value
{
  VariableDecl& va = comparedVariables.emplace_back();
  VariableDecl& vb = comparedVariables.emplace_back();
  NameExpr& na = comparedNames.emplace_back();
  NameExpr& nb = comparedNames.emplace_back();
  BinaryExpr& cmp = comparisons.emplace_back();
  const Ty* operand = types.lref(types.qualified(t, true, false));
  for (VariableDecl* v : {&va, &vb})
  {
    v->kind = NodeKind::VariableDecl;
    v->token = at ? at->token : 0;
    v->file = currentFile;
  }
  for (auto [n, v] : {std::pair{&na, &va}, std::pair{&nb, &vb}})
  {
    n->kind = NodeKind::NameExpr;
    n->token = at ? at->token : 0;
    n->resolved = v;
    n->ty = operand;
  }
  cmp.kind = NodeKind::BinaryExpr;
  cmp.token = at ? at->token : 0;
  cmp.lhs = &na;
  cmp.rhs = &nb;
  cmp.op = uint32_t(op);
  cmp.isKeyword = false;
  cmp.ty = op == P::Spaceship ? orderingType() : types.boolType();
  locals[&va] = Local{a, t, false};
  locals[&vb] = Local{b, t, false};
  struct Forget
  {
    ~Forget()
    {
      locals.erase(x);
      locals.erase(y);
    }

    std::unordered_map<Decl*, Local>& locals;
    Decl* x;
    Decl* y;
  } forget{locals, &va, &vb};
  return rvalue(&cmp);
}

auto codegen::Emitter::objectsEqual(const std::string& a, const std::string& b, const Ty* t, Node* at) -> Value
{
  const Ty* foundIn = nullptr;
  std::vector<const Ty*> argTypes{types.lref(types.qualified(t, true, false))};
  FunctionDecl* op = findMemberFunction(t, "operator==", argTypes, t, at, &foundIn);
  if (op && !(op->flags & DF_Default))
  {
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
    std::string r = newTemp();
    emitCall(r + " = ", s.returnType + " " + name + "(ptr " + thisArg + ", ptr " + b + ")");
    if (s.returnType == "i1")
      return {r, types.boolType(), false};
    return convert(Value{r, types.intType(32), false}, types.boolType(), at);
  }
  if (!op)
    if (const Ty* operand = types.lref(types.qualified(t, true, false)); global.findFreeOperator(uint32_t(P::EqEq), operand, operand, currentFunction))
      return convert(comparedAt(P::EqEq, a, b, t, at), types.boolType(), at, false, ConversionContext::Direct); // SHA1Digest's `operator==(const SHA1Digest&, const SHA1Digest&)`
  if (ClassDecl* u = typedUnionOf(t, at))
    return typedUnionEqual(u, a, b, t, at); // the alternative held, then its values (design.md 8.5)
  return memberwiseEqual(a, b, t, at);
}

auto codegen::Emitter::braceScalar(BraceInitExpr* list, const Ty* t, Node* at) -> Value
{
  t = types.unqualified(types.stripRef(t));
  if (!t->isKnown() || isAggregate(t))
    unsupported(at, "a brace list as a value of " + types.toString(t, &symbols));
  if (list->designators.empty() && enumConstructs(t, list->args))
    return constructedEnum(t, list->args, at); // `return {};`, `f({})`, `d = {}`: a kpp enum's constructor (kovarex, 2026-11-11)
  if (list->args.empty())
  {
    if (t->kind == TyKind::Pointer || t->kind == TyKind::Nullptr)
      return {"null", t};
    if (t->kind == TyKind::MemberPointer)
      return convert(Value{"null", types.nullptrType()}, t, at);
    return {t->kind == TyKind::Float ? floatConstant(0, t->bits) : t->kind == TyKind::Vector ? "zeroinitializer" : "0", t};
  }
  if (list->args.size() == 1 && list->designators.empty())
    return convert(rvalue(list->args[0]), t, at);
  unsupported(at, "a brace list of " + std::to_string(list->args.size()) + " values as a value of " + types.toString(t, &symbols));
}

auto codegen::Emitter::nullPointerConstant(Expr* e) -> bool
{
  while (e && e->kind == NodeKind::ParenExpr)
    e = static_cast<ParenExpr*>(e)->inner;
  if (!e || e->kind != NodeKind::LiteralExpr || static_cast<LiteralExpr*>(e)->literalKind != TokenKind::Number)
    return false;
  auto* l = static_cast<LiteralExpr*>(e);
  if (!l->file || !l->file->tokens || l->token >= l->file->tokens->size())
    return false;
  const Token& tok = (*l->file->tokens)[l->token];
  std::string_view text = l->file->text.substr(tok.offset, tok.length);
  return isZeroLiteral(text);
}

auto codegen::Emitter::argumentText(const Ty* pt, Expr* arg, Node* at) -> std::string
{
  if (arg->kind == NodeKind::BraceInitExpr && static_cast<BraceInitExpr*>(arg)->text)
    return "ptr " + textTemporary(static_cast<BraceInitExpr*>(arg), at); // a Text parameter's packed arguments (design.md 14)
  if (pt && pt->isReference())
  {
    // A temporary binds a const lvalue reference or an rvalue reference, never a `T&` ([dcl.init.ref]
    // 5): the function would change the temporary and leave the argument as it was. (Until
    // 2026-11-05 made all the same: `bump(box)` with `bump(Holder&)` bumped a Holder made of box.)
    const Ty* element = types.stripRef(pt);
    while (element && element->kind == TyKind::Array)
      element = element->inner; // (`const int (&)[2]` refers to const: an array's const is its elements')
    bool takesTemporary = pt->kind == TyKind::RRef || types.stripRef(pt)->isConst || (element && element->isConst);
    // An rvalue reference binds no lvalue of its type (C++'s [dcl.init.ref] 5.4.4: `move(a)` says so;
    // one of another type converts to a temporary, which it binds). Until 2026-11-05 it bound the
    // variable itself: `take(a)` with `take(S&&)` left a moved from.
    if (pt->kind == TyKind::RRef && isLvalueExpression(arg) && arg->ty && arg->ty->isKnown() && !types.isDependent(arg->ty))
    {
      const Ty* given = types.unqualified(types.stripRef(arg->ty)), *bound = types.unqualified(types.stripRef(pt));
      std::vector<std::pair<std::string, uint32_t>> path; // (a derived class's lvalue no more than the class's own)
      if (given->kind != TyKind::Function && // (a function lvalue it binds: 5.3.1)
          (given == bound || (isClass(given) && isClass(bound) && (classKey(given) == classKey(bound) || basePath(given, bound, path, at)))))
        fail(at, "a " + types.toString(pt, &symbols) + " parameter given an lvalue of its type: an rvalue reference binds only a temporary or what move() gives, never the object itself");
    }
    auto refuseTemporary = [&](const char* made)
    {
      if (!takesTemporary)
        fail(at, "a " + types.toString(pt, &symbols) + " parameter given " + made +
                     ": a non-const lvalue reference binds only an object of its type, never a temporary made for it, which the function would change instead");
    };
    // `f("abc")` with `f(const char (&)[4])`, or `const T&` with T = char[4]: the literal's own
    // chars (the checker types it as the pointer it decays to, which is their address).
    if (types.unqualified(types.stripRef(pt))->kind == TyKind::Array && stringLiteralArray(arg, arg->ty, types))
      return "ptr " + rvalue(arg).text;
    // `sum(Three{1, 2, 3})` (`using Three = const int[3]`), `sum({1, 2, 3})`, with `sum(const int (&)[3])`:
    // a temporary array the list initializes, the elements it leaves out value-initialized
    // ([dcl.init.list] 3.10), destroyed with the statement's temporaries; a non-const reference binds
    // none. (Until 2026-11-05 "does not support the expression kind BraceInitExpr", once D's 94b4c0f7
    // made overload resolution choose such a candidate; the named form until 2026-11-08.)
    if (const Ty* array = types.unqualified(types.stripRef(pt)); array->kind == TyKind::Array && isArrayBraces(arg))
    {
      refuseTemporary(arg->kind == NodeKind::BraceInitExpr ? "a braced list" : "a temporary array");
      return "ptr " + arrayTemporary(arg, array, at);
    }
    if (isAggregate(types.stripRef(pt)))
    {
      const Ty* cls = types.stripRef(pt);
      if (arg->kind == NodeKind::BraceInitExpr && isInitializerListType(cls))
      {
        refuseTemporary("a braced list");
        return "ptr " + initializerListTemporary(cls, static_cast<BraceInitExpr*>(arg), at); // `max({a, b, c})`
      }
      if (arg->kind == NodeKind::BraceInitExpr && isClass(cls))
      {
        refuseTemporary("a braced list");
        return "ptr " + convertedTemporary(cls, arg, at); // `f({a, b, c})` with `f(const Rect&)`
      }
      const Ty* argTy = arg->ty && arg->ty->isKnown() && types.stripRef(arg->ty)->isKnown() && !types.isDependent(arg->ty) ? types.stripRef(arg->ty) : nullptr;
      if (argTy && isClass(cls) && !isClass(argTy))
      {
        refuseTemporary(("an argument of type " + types.toString(argTy, &symbols)).c_str());
        return "ptr " + convertedTemporary(cls, arg, at); // `f("text")` with `f(const String&)`, a lambda with `f(const Function<void()>&)`
      }
      if (argTy && isClass(cls) && isClass(argTy) && classKey(argTy) != classKey(cls) && !baseInstantiation(argTy, classDef(cls->decl, at), at))
      {
        if (typedUnionOf(cls, at))
        {
          refuseTemporary(("an argument of type " + types.toString(argTy, &symbols)).c_str());
          return "ptr " + convertedTemporary(cls, arg, at); // `report(Patrolling{3})` with `report(const AIState&)`
        }
        if (conversionOperator(argTy, types.unqualified(cls), at))
          return "ptr " + convert(rvalue(arg), types.unqualified(cls), at).text; // `f(lowercase)` through `operator const String&()`
        // (the argument as overload resolution sees it: an lvalue binds a constructor template's `T&`;
        // a constructor inherited with `using Base::Base;` converts too, as the resolver ranks it)
        if (constructorFor(cls, Span<Expr*>{&arg, 1}, at) || global.converts(argumentType(arg, arg->ty, types), types.unqualified(cls)))
        {
          refuseTemporary(("an argument of type " + types.toString(argTy, &symbols)).c_str());
          return "ptr " + convertedTemporary(cls, arg, at); // `f(view)` with `f(const String&)`
        }
      }
      Value o = rvalue(arg);
      if (o.ty && o.ty->kind == TyKind::Vector && isClass(cls) && (cls->isConst || pt->kind == TyKind::RRef))
      {
        // A vector the checker could not type (an SSE operation clang's intrin.h has as a macro)
        // for a `const T&` or `T&&` of a class that wraps it: a temporary made of it by the
        // class's constructor (constructFromVector), destroyed with the statement's.
        const Ty* made = types.unqualified(cls);
        std::string tmp = newObjectSlot(made, at);
        constructFromVector(tmp, made, o, true, at);
        if (needsDestruction(made, at))
          statementTemps.push_back({tmp, made});
        return "ptr " + tmp;
      }
      if (!isAggregate(o.ty))
        unsupported(at, "binding a non-class to a class reference");
      std::string addr = o.text;
      if (isClass(o.ty) && classKey(o.ty) != classKey(pt))
      {
        if (baseAddress(o.ty, types.stripRef(pt), o.text, addr, at))
          return "ptr " + addr;
        // Fallback: try implicit constructor of the target class (e.g. Optional(const T&)
        // when passing T where const Optional<T>& is expected).
        if (constructorFor(cls, Span<Expr*>{&arg, 1}, at))
        {
          refuseTemporary(("an argument of type " + types.toString(o.ty, &symbols)).c_str());
          return "ptr " + convertedTemporary(cls, arg, at);
        }
        if (conversionOperator(o.ty, types.unqualified(cls), at))
          return "ptr " + convert(o, types.unqualified(cls), at).text;
        unsupported(at, "binding an unrelated class to a reference (" + types.toString(o.ty, &symbols) + " to " + types.toString(pt, &symbols) + ")");
      }
      requireSameClosure(o.ty, cls, true, at); // (`take(l2)` with `take(const decltype(l1)&)`)
      return "ptr " + addr;
    }
    const Ty* referenced = types.stripRef(pt);
    if (referenced->kind == TyKind::Pointer && referenced->inner && referenced->inner->kind == TyKind::Function && arg->ty &&
        isLambda(types.stripRef(arg->ty)))
    {
      // `Pair(key, lambda)` with `Pair(const K&, const Fn*&)`: the function pointer, in a slot
      // the reference can bind to.
      Value fp = convert(rvalue(arg), referenced, at);
      std::string slot = "%v" + std::to_string(temp++);
      allocas += "  " + slot + " = alloca ptr\n";
      fn += "  store ptr " + fp.text + ", ptr " + slot + "\n";
      return "ptr " + slot;
    }
    // `const double&` given an int (`Constant(const double&)` built from 2): the reference
    // binds a temporary of its own type holding the converted value, never the int itself.
    // A bit-field (UnitTest's `const Actual&` given TreeData's `progress : 4`) the same: it has no
    // address of its own, and its storage unit's address read the neighbouring fields with it.
    const Ty* argTy = arg->ty && arg->ty->isKnown() && !types.isDependent(arg->ty) ? types.unqualified(types.stripRef(arg->ty)) : nullptr;
    if (argTy && argTy->isKnown() && (referenced->isConst || pt->kind == TyKind::RRef) && types.unqualified(referenced)->isKnown() &&
        !types.isDependent(referenced) && (argTy != types.unqualified(referenced) || bindsBitField(arg)) && arg->kind != NodeKind::BraceInitExpr)
    {
      const Ty* to = types.unqualified(referenced);
      Value v = convert(rvalue(arg), to, at);
      std::string slot = "%v" + std::to_string(temp++);
      allocas += "  " + slot + " = alloca " + llType(to, at) + "\n";
      fn += "  store " + llType(to, at) + " " + v.text + ", ptr " + slot + "\n";
      return "ptr " + slot;
    }
    // A class whose conversion operator gives an lvalue of T (ValueComparePointer's `operator T*&()`
    // for an `Image*&`): that lvalue, as C++ binds it ([dcl.init.ref] 5.1.2). Until 2026-11-05 the
    // object's own address was passed, right only while the member it returns was its first.
    if (!takesTemporary && argTy && isClass(argTy))
    {
      const Ty* owner = nullptr;
      TemplateArgs opArgs;
      // (of the operators giving a non-const lvalue, the argument as it is: `f(b)` for an f(int&) takes `operator int&()`
      // beside `operator int() &`)
      const Ty* given = argumentType(arg, arg->ty, types);
      if (FunctionDecl* op = conversionOf(given && given->isReference() ? given : given ? types.rref(given) : argTy, types.unqualified(referenced),
                                          GlobalScope::ConversionKind::ToLvalue, false, at, &owner, &opArgs))
      {
        const Ty* returned = conversionReturns(op, owner);
        if (returned && returned->kind == TyKind::LRef && !types.stripRef(returned)->isConst &&
            types.unqualified(types.stripRef(returned)) == types.unqualified(referenced))
          return "ptr " + callConversion(op, argTy, owner, lvalue(arg), at, &opArgs, true).text;
      }
    }
    if (argTy && argTy->kind == TyKind::Function && types.unqualified(referenced)->kind == TyKind::Function)
      return "ptr " + convert(rvalue(arg), types.pointer(types.unqualified(referenced)), at).text; // (`void (&)()` given a noexcept one)
    // A `T&` given another type's object (an int for a `double&`), or a bit-field: no object of T to
    // bind (C++ rejects it). Until 2026-11-05 its address was passed: a double written into an int.
    if (!takesTemporary && argTy && argTy->isKnown() && types.unqualified(referenced)->isKnown() && !types.isDependent(referenced) &&
        arg->kind != NodeKind::BraceInitExpr && (argTy != types.unqualified(referenced) || bindsBitField(arg)))
      fail(at, "a " + types.toString(pt, &symbols) + " parameter given " + (bindsBitField(arg) ? std::string("a bit-field") : "an argument of type " + types.toString(argTy, &symbols)) +
                   ": a non-const lvalue reference binds only an object of its type");
    return "ptr " + lvalue(arg);
  }
  if (pt && isAggregate(pt))
  {
    if (arg->kind == NodeKind::BraceInitExpr && isInitializerListType(pt))
      return "ptr " + initializerListTemporary(pt, static_cast<BraceInitExpr*>(arg), at); // `max({a, b, c})`
    if (arg->kind == NodeKind::BraceInitExpr && isClass(pt))
    {
      // `f({a, b, c})`; the callee destroys it. The list as a whole, as a variable's: an
      // InitializerList constructor first (`take({Caps::Color, Caps::Light})`), then the fields.
      std::string copy = newObjectSlot(pt, at);
      Expr* one = arg;
      constructInto(copy, pt, Span<Expr*>{&one, 1}, at);
      return "ptr " + copy;
    }
    const Ty* argTy = arg->ty && arg->ty->isKnown() && !types.isDependent(arg->ty) ? types.stripRef(arg->ty) : nullptr;
    if (argTy && isClass(pt) && (!isClass(argTy) || (classKey(argTy) != classKey(pt) &&
                                                     !baseInstantiation(argTy, classDef(pt->decl, at), at) &&
                                                     !conversionOperator(argTy, types.unqualified(pt), at))))
    {
      // Through a converting constructor (a typed union: from an alternative's value); the
      // callee destroys the parameter.
      std::string copy = newObjectSlot(pt, at);
      Expr* one = arg;
      if (typedUnionOf(pt, at))
        initializeFrom(copy, pt, arg, at);
      else
        constructInto(copy, pt, Span<Expr*>{&one, 1}, at, false, true); // (an argument copy-initializes its parameter)
      return "ptr " + copy;
    }
    Value v = rvalue(arg);
    if (!isAggregate(v.ty))
    {
      // A vector the checker could not type (an SSE operation clang's intrin.h has as a macro,
      // _mm_shuffle_ps) for a class parameter that wraps it (float32x4 taking __m128): made into
      // the class by its constructor (constructFromVector), as a typed one is above; the callee
      // destroys the parameter. (Until 2026-11-05 this loaded the vector from its value as if
      // that were an address, invalid IR that clang rejected, and stored it into the class's
      // first field.)
      if (v.ty && v.ty->kind == TyKind::Vector && isClass(pt))
      {
        std::string copy = newObjectSlot(pt, at);
        constructFromVector(copy, pt, v, true, at);
        return "ptr " + copy;
      }
      unsupported(at, "passing a non-class to a class parameter");
    }
    if (isClass(v.ty) && isClass(pt) && classKey(v.ty) != classKey(pt))
      v = convert(v, types.unqualified(pt), at); // a base, or a conversion operator
    if (isLambda(pt))
      requireSameClosure(v.ty, pt, false, at); // (`take(l2)` with `take(decltype(l1))`: an error, never l2's bytes as l1's)
    if (isLambda(pt) && v.isTemporary)
    {
      // The closure the lambda expression built is the parameter (the callee destroys it).
      for (size_t i = statementTemps.size(); i-- > 0;)
        if (statementTemps[i].addr == v.text)
        {
          statementTemps.erase(statementTemps.begin() + i);
          break;
        }
      return "ptr " + v.text;
    }
    std::string copy = newObjectSlot(pt, at);
    if (isLambda(pt))
      copyClosure(copy, v.text, pt, at, movable(arg));
    else if (v.isTemporary && isClass(v.ty) && classKey(v.ty) == classKey(pt) && !isLambda(pt))
    {
      // `byValue(Tracker(5))`, `f(makeT())`: the temporary is the parameter, not moved (C++17
      // builds the prvalue in the parameter); the callee destroys it.
      for (size_t i = statementTemps.size(); i-- > 0;)
        if (statementTemps[i].addr == v.text)
        {
          statementTemps.erase(statementTemps.begin() + i);
          break;
        }
      return "ptr " + v.text;
    }
    else if (v.isTemporary)
      moveTemporary(copy, v, pt, at);
    else if (movable(arg))
      moveConstruct(copy, v.text, pt, at); // `f(move(ptr))` with `f(UniquePtr<T> ptr)`
    else
      copyConstruct(copy, v.text, pt, at);
    return "ptr " + copy;
  }
  if (arg->kind == NodeKind::BraceInitExpr && pt)
  {
    Value a = braceScalar(static_cast<BraceInitExpr*>(arg), pt, at); // `deduceSelectedEntity(surface, position, allow, {}, {})`
    return llType(a.ty, at) + " " + a.text;
  }
  if (pt && pt->kind == TyKind::Pointer && nullPointerConstant(arg))
    return "ptr null"; // `Entity* secondaryEntity = 0`
  // `first(Three{1, 2, 3})` with `first(const int*)` (`using Three = const int[3]`): the temporary array
  // the braces make (the array-to-pointer conversion materializes it, [conv.array]), its first
  // element's address passed, the array destroyed with the statement's temporaries ([class.temporary]
  // 4). (Until 2026-11-08 "a temporary array ... other than for a reference to an array".)
  Value a = pt && types.unqualified(pt)->kind == TyKind::Pointer && arg->kind == NodeKind::FunctionalCastExpr && isArrayBraces(arg)
              ? load(arrayTemporary(arg, types.stripRef(arg->ty), at), types.stripRef(arg->ty), at)
              : rvalue(arg);
  // (an argument copy-initializes its parameter: no explicit operator bool, `f(object)` for an f(bool) takes `operator long()`)
  if (isClass(a.ty) && pt && !isAggregate(pt) && conversionOperator(conversionObject(a), types.unqualified(pt), at, nullptr, false, nullptr, ConversionContext::Copy))
    a = convert(a, types.unqualified(pt), at, false, ConversionContext::Copy); // `draw(image)` with `operator const Image*()`
  if (isLambda(a.ty) && pt && pt->kind == TyKind::Pointer && pt->inner && pt->inner->kind == TyKind::Function)
    a = convert(a, pt, at); // a lambda without captures as a pointer to function
  if (isAggregate(a.ty) && !pt)
  {
    // Variadic C argument with no declared type: try the class's implicit conversion operator
    // to a scalar (e.g. String's operator const char*() for lua_pushstring(L, str)).
    FunctionDecl* conv = conversionOperator(conversionObject(a), types.intType(32), at);
    if (conv)
    {
      const Ty* ft = declType(conv);
      const Ty* ret = types.unqualified(types.stripRef(ft->inner));
      a = convert(a, ret, at);
    }
  }
  if (isAggregate(a.ty) && !pt && isClass(a.ty))
  {
    // A class through C's `...` with no conversion to a scalar, as MSVC passes one: a copy, of 1,
    // 2, 4 or 8 bytes as an integer of its size, of any other size by its address
    // (PlatformBackdropCloud's `ssprintf("\"%s\" is unknown ...", input)` with a StringView:
    // a wrong argument on the game's error path, which MSVC compiles).
    std::string copy = newObjectSlot(a.ty, at);
    copyConstruct(copy, a.text, a.ty, at);
    if (needsDestruction(a.ty, at))
      statementTemps.push_back({copy, a.ty});
    // (Off Windows the copy goes to lowerCArguments, which passes it as the target's C does.)
    uint64_t size = sizeOf(a.ty, at);
    if (sh.target.windows() && (size == 1 || size == 2 || size == 4 || size == 8))
    {
      std::string bits = newTemp(), ty = "i" + std::to_string(size * 8);
      fn += "  " + bits + " = load " + ty + ", ptr " + copy + "\n";
      return ty + " " + bits;
    }
    return "ptr " + copy;
  }
  if (isAggregate(a.ty))
    unsupported(at, "passing a class (" + types.toString(a.ty, &symbols) + ") where a scalar is expected");
  if (pt && a.ty->kind == TyKind::Pointer && (pt->kind == TyKind::Int || pt->kind == TyKind::Char || pt->kind == TyKind::Enum))
    unsupported(at, "passing a pointer (" + types.toString(a.ty, &symbols) + ") where " + types.toString(pt, &symbols) + " is expected");
  if (pt && pt->kind == TyKind::Pointer && (a.ty->kind == TyKind::Int || a.ty->kind == TyKind::Char || a.ty->kind == TyKind::Enum))
    unsupported(at, "passing " + types.toString(a.ty, &symbols) + " where a pointer is expected");
  if (pt)
    a = convert(a, pt, at);
  else if (a.ty->kind == TyKind::Float && a.ty->bits == 32)
    a = convert(a, types.floatType(64), at);
  else if ((a.ty->kind == TyKind::Int || a.ty->kind == TyKind::Char) && a.ty->bits < 32)
    a = convert(a, types.intType(32), at);
  else if (a.ty->kind == TyKind::Bool)
    a = convert(a, types.intType(32), at);
  return llType(a.ty, at) + " " + a.text;
}

auto codegen::Emitter::bitFieldDesignator(Expr* e) -> Expr*
{
  for (;;) // as GlobalScope's designatedBitField: `(x.f)`, `++x.f`, `x.f = v`, `(a, x.f)` (built-in operators)
    if (e && e->kind == NodeKind::ParenExpr)
      e = static_cast<ParenExpr*>(e)->inner;
    else if (auto* u = e && e->kind == NodeKind::UnaryExpr ? static_cast<UnaryExpr*>(e) : nullptr;
             u && !u->isKeyword && !u->postfix && u->call != OperatorCall::Function && (P(u->op) == P::PlusPlus || P(u->op) == P::MinusMinus))
      e = u->operand;
    else if (auto* b = e && e->kind == NodeKind::BinaryExpr ? static_cast<BinaryExpr*>(e) : nullptr;
             b && !b->isKeyword && b->call != OperatorCall::Function && (isAssignment(P(b->op)) || P(b->op) == P::Comma))
      e = P(b->op) == P::Comma ? b->rhs : b->lhs;
    else
      break;
  Decl* d = !e ? nullptr : e->kind == NodeKind::MemberExpr ? static_cast<MemberExpr*>(e)->resolved
                         : e->kind == NodeKind::NameExpr    ? static_cast<NameExpr*>(e)->resolved
                                                            : nullptr;
  return d && d->kind == NodeKind::VariableDecl && static_cast<VariableDecl*>(d)->bitfield ? e : nullptr;
}

auto codegen::Emitter::bindingDesignator(Expr* e) -> Expr*
{
  for (;;) // as bitFieldDesignator walks
    if (e && e->kind == NodeKind::ParenExpr)
      e = static_cast<ParenExpr*>(e)->inner;
    else if (auto* u = e && e->kind == NodeKind::UnaryExpr ? static_cast<UnaryExpr*>(e) : nullptr;
             u && !u->isKeyword && !u->postfix && u->call != OperatorCall::Function && (P(u->op) == P::PlusPlus || P(u->op) == P::MinusMinus))
      e = u->operand;
    else if (auto* b = e && e->kind == NodeKind::BinaryExpr ? static_cast<BinaryExpr*>(e) : nullptr;
             b && !b->isKeyword && b->call != OperatorCall::Function && (isAssignment(P(b->op)) || P(b->op) == P::Comma))
      e = P(b->op) == P::Comma ? b->rhs : b->lhs;
    else
      break;
  return e && e->kind == NodeKind::NameExpr ? e : nullptr;
}

auto codegen::Emitter::namesBitField(Expr* e) -> bool
{
  for (;;) // `(x.f)`; `++x.f` and `x.f = v`, lvalues naming the field too
    if (e && e->kind == NodeKind::ParenExpr)
      e = static_cast<ParenExpr*>(e)->inner;
    else if (e && e->kind == NodeKind::UnaryExpr && !static_cast<UnaryExpr*>(e)->isKeyword && !static_cast<UnaryExpr*>(e)->postfix &&
             (P(static_cast<UnaryExpr*>(e)->op) == P::PlusPlus || P(static_cast<UnaryExpr*>(e)->op) == P::MinusMinus))
      e = static_cast<UnaryExpr*>(e)->operand;
    else if (e && e->kind == NodeKind::BinaryExpr && !static_cast<BinaryExpr*>(e)->isKeyword && isAssignment(P(static_cast<BinaryExpr*>(e)->op)))
      e = static_cast<BinaryExpr*>(e)->lhs;
    else
      break;
  Decl* d = !e ? nullptr : e->kind == NodeKind::MemberExpr ? static_cast<MemberExpr*>(e)->resolved
                         : e->kind == NodeKind::NameExpr    ? static_cast<NameExpr*>(e)->resolved
                                                            : nullptr;
  return d && d->kind == NodeKind::VariableDecl && static_cast<VariableDecl*>(d)->bitfield;
}

auto codegen::Emitter::conversionReturns(FunctionDecl* op, const Ty* owner) -> const Ty*
{
  TemplateArgs classArgs = global.classArgsOf(owner);
  ArgsScope scope(*this, &classArgs);
  const Ty* fnTy = declType(op);
  return fnTy ? fnTy->inner : nullptr;
}

auto codegen::Emitter::lvalueByConversion(Expr* value, const Ty* have, const Ty* referenced, Node* at, bool explicitCast) -> std::optional<std::string>
{
  const Ty* owner = nullptr;
  TemplateArgs opArgs;
  const Ty* want = types.unqualified(referenced);
  const Ty* given = argumentType(value, value->ty, types);
  // (a non-const reference binds only what an operator giving a non-const lvalue gives: `int& r(b);` takes `operator int&()`
  // beside `operator int()`)
  FunctionDecl* op = conversionOf(given && given->isReference() ? given : given ? types.rref(given) : have, want,
                                  referenced->isConst ? GlobalScope::ConversionKind::ToType : GlobalScope::ConversionKind::ToLvalue, explicitCast, at, &owner, &opArgs);
  const Ty* returned = op ? conversionReturns(op, owner) : nullptr;
  if (!returned || returned->kind != TyKind::LRef || (types.stripRef(returned)->isConst && !referenced->isConst) ||
      types.unqualified(types.stripRef(returned)) != want)
    return std::nullopt;
  return callConversion(op, have, owner, rvalue(value).text, at, &opArgs, true).text;
}

auto codegen::Emitter::scalarReferenceTarget(const Ty* refTy, Expr* value, Node* at, const std::function<std::string(const Ty*)>& makeSlot, bool listInit,
                                             bool directList) -> std::string
{
  auto scalar = [](const Ty* t)
  {
    return t->kind == TyKind::Bool || t->kind == TyKind::Char || t->kind == TyKind::Int || t->kind == TyKind::Float ||
           t->kind == TyKind::Enum || t->kind == TyKind::Pointer || t->kind == TyKind::Nullptr;
  };
  const Ty* referenced = types.stripRef(refTy);
  const Ty* want = types.unqualified(referenced);
  const Ty* have = value->ty && value->ty->isKnown() && !types.isDependent(value->ty) ? types.unqualified(types.stripRef(value->ty)) : nullptr;
  requireSameClosure(have, want, true, at); // (`const decltype(l1)& r = l2;`: an error, never l2 bound as l1)
  // (the object as it is: a const one calls a const conversion function only, `const int& r(k);`
  // never k's non-const `operator int&()`; until 2026-11-08 the object was taken as non-const)
  const Ty* object = have && isClass(have) ? types.stripRef(value->ty) : have;
  // (and its value category, which a ref-qualified one tells apart: `int&& r(b);` of an lvalue b never calls `operator double() &&`)
  const Ty* chooser = object;
  if (have && isClass(have))
    if (const Ty* a = argumentType(value, value->ty, types))
      chooser = a->isReference() ? a : types.rref(a);
  // A temporary of the referenced type holding the value, converted (`const double& r{object};` by an explicit
  // `operator double()` too, as clang does).
  bool explicitToo = directList && have && isClass(have);
  auto temporary = [&]
  {
    Value converted = convert(rvalue(value), want, at, explicitToo);
    std::string slot = makeSlot ? makeSlot(want) : newObjectSlot(want, at);
    fn += "  store " + llType(want, at) + " " + converted.text + ", ptr " + slot + "\n";
    return slot;
  };
  // `const uint8_t& bound = data.progress;`: a temporary holding the bit-field's value, as C++
  // binds it (the field has no address; its storage unit's holds its neighbours too).
  if (bindsBitField(value) && want->isKnown() && !types.isDependent(want) && scalar(want))
    return temporary();
  // `int& r = object;`, `const int& r = object;`: the lvalue its conversion operator gives, else a
  // temporary of what it gives by value. (Until 2026-11-06 the object's own address, read as an
  // int: its first member's value, or the bytes of a double.)
  if (have && isClass(have) && want->isKnown() && !types.isDependent(want))
  {
    // An rvalue reference binds no lvalue a conversion function gives: of a class with `operator
    // int&()` and `operator float()`, `int&& r(object);` binds a temporary of the float made an int,
    // as clang does (until 2026-11-08 the int& one's object).
    // A list's element initializes a temporary as `int t = object;` would, by any conversion function.
    if (refTy->kind == TyKind::RRef && scalar(want))
    {
      const Ty* owner = nullptr;
      TemplateArgs opArgs;
      if (FunctionDecl* op = conversionOf(chooser, want, listInit ? GlobalScope::ConversionKind::ToType : GlobalScope::ConversionKind::ToValue, explicitToo, at, &owner,
                                          &opArgs))
      {
        Value converted = convert(callConversion(op, object, owner, rvalue(value).text, at, &opArgs), want, at);
        std::string slot = makeSlot ? makeSlot(want) : newObjectSlot(want, at);
        fn += "  store " + llType(want, at) + " " + converted.text + ", ptr " + slot + "\n";
        return slot;
      }
    }
    if (std::optional<std::string> bound = lvalueByConversion(value, object, referenced, at, explicitToo))
      return *bound;
    if (scalar(want) && (refTy->kind == TyKind::RRef || referenced->isConst))
      return temporary();
  }
  // `int (&r)(double) = twice;`: the function a pointer to it would be given. A reference to a noexcept
  // function binds only a noexcept one, as such a pointer takes only one.
  if (have && want->kind == TyKind::Function && have->kind == TyKind::Function)
    return convert(rvalue(value), types.pointer(want), at).text;
  // `int* const& p = array;`, `int (*const& f)(int) = g;`: the array or function decays, and a
  // temporary holds the pointer. (Until 2026-11-06 the reference bound the array or the function
  // itself, and a read through it took their first bytes for the pointer.)
  if (have && want->kind == TyKind::Pointer && (have->kind == TyKind::Array || have->kind == TyKind::Function))
  {
    if (refTy->kind == TyKind::LRef && !referenced->isConst)
      fail(at, "the reference of type " + types.toString(refTy, &symbols) + " given a value of type " + types.toString(have, &symbols) +
                   ": its pointer is a temporary, which only a const or an rvalue reference binds");
    return temporary();
  }
  // A class object no conversion function binds a reference to a scalar through is no such object (`int& r(object);`
  // with no `operator int&()` it takes): an error, never the reference bound to the object itself (until 2026-11-08
  // `int& x(a);` with an explicit `operator int&()` bound x to a, the bytes of its first member read as the int).
  if (have && isClass(have) && want->isKnown() && !types.isDependent(want) && scalar(want))
    fail(at, "the reference of type " + types.toString(refTy, &symbols) + " given an object of " + types.toString(have, &symbols) +
                 ": none of its conversion functions gives what the reference binds");
  if (!have || have == want || !have->isKnown() || !want->isKnown() || types.isDependent(want) || !scalar(want) || !scalar(have))
    return lvalue(value);
  if (refTy->kind == TyKind::LRef && !referenced->isConst)
  {
    // (C++ binds no other type to a non-const lvalue reference; `long&` to an int is one
    // representation.)
    if (llType(have, at) != llType(want, at))
      unsupported(at, "binding " + types.toString(refTy, &symbols) + " to a value of type " + types.toString(have, &symbols));
    return lvalue(value);
  }
  return temporary();
}

auto codegen::Emitter::convertedTemporary(const Ty* cls, Expr* arg, Node* at) -> std::string
{
  const Ty* source = arg->ty ? types.stripRef(arg->ty) : nullptr;
  if (arg->kind != NodeKind::BraceInitExpr && (!source || !source->isKnown()))
    unsupported(at, "converting a value of unknown type to " + types.toString(cls, &symbols)); // (not the copy constructor, again and again)
  Expr* one = arg;
  // The resolver already enforces explicit-ness during overload resolution; codegen trusts it.
  std::string tmp = newObjectSlot(cls, at);
  if (typedUnionOf(cls, at) && arg->kind != NodeKind::BraceInitExpr)
    initializeFrom(tmp, cls, arg, at); // a typed union from an alternative's value (design.md 8.5)
  else
    constructInto(tmp, cls, Span<Expr*>{&one, 1}, at, false, arg->kind != NodeKind::BraceInitExpr); // (a copy-initialization: `f(2.5)` for an f(C&&)
  // takes `implicit C(int)` over `C(double)`; until 2026-11-08 the best of every constructor)
  if (needsDestruction(cls, at))
    statementTemps.push_back({tmp, cls});
  return tmp;
}
