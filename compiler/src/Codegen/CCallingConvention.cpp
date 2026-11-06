// struct Emitter's members: C calling conventions; their declarations in CCallingConvention.hpp,
// part of the struct's body in Codegen/Emitter.hpp. A translation unit of its own since 2026-11-05
// (codegen.cpp split, stage 2), moved without a change.
#include "Emitter.hpp"

auto codegen::Emitter::cScalars(const Ty* t, uint64_t base, std::vector<std::pair<uint64_t, const Ty*>>& out, Node* at) -> void
{
  t = types.unqualified(t);
  if (t->kind == TyKind::Enum)
    return cScalars(enumUnderlying(t->decl), base, out, at);
  if (t->kind == TyKind::Array)
  {
    uint64_t size = sizeOf(t->inner, at);
    for (uint64_t i = 0; i < t->count; ++i)
      cScalars(t->inner, base + i * size, out, at);
    return;
  }
  if (!isClass(t))
  {
    out.push_back({base, t});
    return;
  }
  classSizeAndAlign(t, at); // laid out, the fields' sizes known
  std::vector<const Ty*> fields;
  std::vector<bool> zeroSize;
  std::vector<Decl*> unionMembers;
  bool packed;
  {
    auto lock = sharedLock();
    StructInfo& info = structFor(t, at);
    if (info.polymorphic)
      unsupported(at, "a class with virtual functions (" + irString(t) + ") passed by value to or from a C function");
    fields = info.fieldTypes, zeroSize = info.zeroSize, packed = info.packed;
    if (info.isUnion)
      unionMembers = info.fieldDecls;
  }
  // A union's storage is integers to the layout; to C it is its members, all at its start: one
  // with a float where every other leaves padding gives that eightbyte the SSE class.
  if (!unionMembers.empty())
  {
    for (Decl* m : unionMembers)
      cScalars(unionMemberType(t, m, at), base, out, at);
    return;
  }
  uint64_t offset = 0;
  for (size_t i = 0; i < fields.size(); ++i)
  {
    if (i < zeroSize.size() && zeroSize[i])
      continue; // an empty base: no storage
    uint64_t a = packed ? 1 : alignOf(fields[i], at);
    offset = (offset + a - 1) / a * a;
    cScalars(fields[i], base + offset, out, at);
    offset += sizeOf(fields[i], at);
  }
}

auto codegen::Emitter::cPassing(const Ty* t, Node* at) -> CPassing
{
  CPassing p;
  uint64_t size = sizeOf(t, at);
  if (sh.target.windows())
  {
    if (size == 1 || size == 2 || size == 4 || size == 8)
      p.inMemory = false, p.parts = {"i" + std::to_string(size * 8)}, p.offsets = {0};
    return p;
  }
  if (size == 0 || size > 16)
    return p;
  std::vector<std::pair<uint64_t, const Ty*>> scalars;
  cScalars(t, 0, scalars, at);
  enum { None, Integer, Sse };
  int classes[2] = {None, None};
  bool upperHalf[2] = {false, false}; // an SSE eightbyte with a float in its upper four bytes
  for (auto& [offset, s] : scalars)
  {
    uint64_t ssize = sizeOf(s, at);
    if (s->kind == TyKind::Vector || offset % std::max<uint64_t>(1, alignOf(s, at)) != 0 || (offset % 8) + ssize > 8)
      return p; // unaligned (packed), or a vector: in memory
    bool sse = s->kind == TyKind::Float;
    if (sse && ssize > 8)
      return p; // (x87's long double: in memory)
    int& c = classes[offset / 8];
    c = c == Integer || !sse ? Integer : Sse;
    if (sse && offset % 8 >= 4)
      upperHalf[offset / 8] = true;
  }
  for (unsigned i = 0; i * 8 < size; ++i)
  {
    uint64_t bytes = std::min<uint64_t>(8, size - i * 8);
    if (classes[i] == Sse)
    {
      bool isDouble = false;
      for (auto& [offset, s] : scalars)
        isDouble = isDouble || (offset == i * 8 && sizeOf(s, at) == 8);
      p.parts.push_back(isDouble ? "double" : upperHalf[i] ? "<2 x float>" : "float");
      ++p.sses;
    }
    else
    {
      p.parts.push_back("i" + std::to_string(bytes * 8)); // (padding alone: an integer too)
      ++p.ints;
    }
    p.offsets.push_back(i * 8);
  }
  p.inMemory = false;
  return p;
}

auto codegen::Emitter::cArgumentTypes(const Ty* t, CRegisters& left, CPassing& p, Node* at) -> std::vector<std::string>
{
  p = cPassing(t, at);
  if (!sh.target.windows() && !p.inMemory && (p.ints > left.ints || p.sses > left.sses))
    p.inMemory = true; // not enough registers left: all of it on the stack, as C does
  if (!p.inMemory)
  {
    left.ints -= p.ints, left.sses -= p.sses;
    return p.parts;
  }
  if (sh.target.windows())
    return {"ptr"};
  uint64_t align = std::max<uint64_t>(8, alignOf(t, at));
  return {"ptr byval([" + std::to_string(sizeOf(t, at)) + " x i8]) align " + std::to_string(align)};
}

auto codegen::Emitter::cScalarArgument(const Ty* t, CRegisters& left) -> void
{
  if (!t)
    return;
  const Ty* u = types.unqualified(t);
  if (u->kind == TyKind::Float)
    left.sses -= left.sses > 0;
  else
    left.ints -= left.ints > 0;
}

auto codegen::Emitter::lowerCArguments(std::vector<std::pair<const Ty*, std::string>>& args, bool sretSlot, Node* at) -> void
{
  CRegisters left;
  if (sretSlot)
    left.ints -= 1;
  for (auto& [t, text] : args)
  {
    if (!t || !isAggregate(t))
    {
      cScalarArgument(t, left);
      continue;
    }
    if (text.compare(0, 4, "ptr ") != 0)
    {
      if (sh.target.windows())
        continue; // a class through `...`, which argumentText passed as MSVC does already
      unsupported(at, "a struct argument of a C function that is no copy in memory (" + text + ")");
    }
    std::string addr = text.substr(4);
    CPassing p;
    std::vector<std::string> passed = cArgumentTypes(t, left, p, at);
    if (p.inMemory)
    {
      text = passed[0] + " " + addr;
      continue;
    }
    text.clear();
    for (size_t i = 0; i < p.parts.size(); ++i)
    {
      std::string from = addr;
      if (p.offsets[i])
      {
        from = newTemp();
        fn += "  " + from + " = getelementptr inbounds i8, ptr " + addr + ", i64 " + std::to_string(p.offsets[i]) + "\n";
      }
      std::string v = newTemp();
      fn += "  " + v + " = load " + p.parts[i] + ", ptr " + from + "\n";
      text += (i ? ", " : "") + p.parts[i] + " " + v;
    }
  }
}

auto codegen::Emitter::cReturnType(const CPassing& p) -> std::string
{
  if (p.parts.size() == 1)
    return p.parts[0];
  std::string t = "{ ";
  for (size_t i = 0; i < p.parts.size(); ++i)
    t += (i ? ", " : "") + p.parts[i];
  return t + " }";
}

auto codegen::Emitter::cDeclaration(FunctionDecl* f, const Signature& s) -> std::pair<std::string, std::string>
{
  return cDeclarationOf(declType(f)->inner, s, f);
}

auto codegen::Emitter::cDeclarationOf(const Ty* ret, const Signature& s, Node* f) -> std::pair<std::string, std::string>
{
  std::string returnType = s.returnType, params;
  CRegisters left;
  if (s.hasSret)
  {
    CPassing r = cPassing(ret, f);
    if (!r.inMemory)
      returnType = cReturnType(r); // (no slot parameter)
    else
      params = "ptr", left.ints -= 1;
  }
  for (size_t i = s.hasSret ? 1 : 0; i < s.paramTypes.size(); ++i)
  {
    const Ty* pt = s.paramTys[i];
    std::vector<std::string> texts;
    if (pt && isAggregate(pt))
    {
      CPassing p;
      texts = cArgumentTypes(pt, left, p, f);
    }
    else
    {
      cScalarArgument(pt, left);
      texts = {s.paramTypes[i]};
    }
    for (const std::string& t : texts)
      params += (params.empty() ? "" : ", ") + t;
  }
  if (s.variadic)
    params += params.empty() ? "..." : ", ...";
  return {returnType, params};
}

auto codegen::Emitter::cStructsByValue(const Signature& s, const Ty* ret, Node* at) -> bool
{
  for (size_t i = 0; i < s.paramTys.size(); ++i)
  {
    const Ty* t = s.hasSret && i == (s.hasThis ? 1u : 0u) ? ret : s.paramTys[i];
    if (t && isAggregate(t) && (!sh.target.windows() || !cPassing(t, at).inMemory))
      return true;
  }
  return false;
}

auto codegen::Emitter::emitReturnVoid() -> void
{
  if (cReturnParts.empty())
  {
    fn += "  ret void\n";
    return;
  }
  CPassing p;
  p.parts = cReturnParts;
  std::string type = cReturnType(p), value = "undef";
  for (size_t i = 0; i < cReturnParts.size(); ++i)
  {
    std::string from = sretAddr;
    if (cReturnOffsets[i])
    {
      from = newTemp();
      fn += "  " + from + " = getelementptr inbounds i8, ptr " + sretAddr + ", i64 " + std::to_string(cReturnOffsets[i]) + "\n";
    }
    std::string part = newTemp();
    fn += "  " + part + " = load " + cReturnParts[i] + ", ptr " + from + "\n";
    if (cReturnParts.size() == 1)
    {
      value = part;
      break;
    }
    std::string whole = newTemp();
    fn += "  " + whole + " = insertvalue " + type + " " + value + ", " + cReturnParts[i] + " " + part + ", " + std::to_string(i) + "\n";
    value = whole;
  }
  fn += "  ret " + type + " " + value + "\n";
}

auto codegen::Emitter::refuseCAddress(FunctionDecl* f, Node* at) -> void
{
  (void)f, (void)at;
}

auto codegen::Emitter::callIndirectC(const std::string& fnText, const Ty* ft, Span<Expr*> callArgs, const std::string& sret, Node* c) -> Value
{
  // The kpp convention's signature (a class result through a slot first, a class parameter as a
  // pointer to a copy), which cDeclarationOf lowers as a C function's declaration is lowered.
  const Ty* ret = ft->inner;
  Signature s;
  if (returnsInMemory(ret))
  {
    s.hasSret = true;
    s.returnType = "void";
    s.paramTypes.push_back("ptr");
    s.paramTys.push_back(nullptr);
  }
  else if (!ret || !ret->isKnown())
    unsupported(c, "a call through a pointer to a C function whose result type is not known");
  else
    s.returnType = ret->isReference() ? "ptr" : llType(ret, c);
  for (const Ty* pt : ft->args)
  {
    s.paramTypes.push_back(isAggregate(pt) || pt->isReference() ? "ptr" : llType(pt, c));
    s.paramTys.push_back(pt);
  }
  s.variadic = ft->variadic;
  CPassing cResult;
  if (s.hasSret)
    cResult = cPassing(ret, c);
  std::string resultAddr = sret;
  bool resultTemp = false;
  if (s.hasSret && resultAddr.empty())
  {
    resultAddr = newObjectSlot(ret, c);
    resultTemp = true;
  }
  // The arguments as argumentText gives them (a struct as a pointer to the call's copy), then their
  // structs lowered (lowerCArguments), as a direct C call's are.
  std::vector<std::pair<const Ty*, std::string>> argList;
  size_t position = 0;
  auto addArgument = [&](Expr* arg)
  {
    const Ty* pt = position < ft->args.size() ? ft->args[position] : nullptr;
    std::string text = argumentText(pt, arg, c);
    argList.push_back({pt ? pt : arg->ty ? withCurrentArgs(types.unqualified(types.stripRef(arg->ty))) : nullptr, std::move(text)});
    ++position;
  };
  for (Expr* arg : callArgs)
  {
    if (isPackExpansion(arg))
    {
      Expr* pattern = static_cast<UnaryExpr*>(arg)->operand;
      forEachPackElement(pattern, c, [&](size_t) { addArgument(pattern); });
    }
    else
      addArgument(arg);
  }
  bool slot = s.hasSret && cResult.inMemory;
  lowerCArguments(argList, slot, c);
  std::string argText = slot ? "ptr " + resultAddr : std::string();
  for (auto& [t, text] : argList)
    argText += std::string(argText.empty() ? "" : ", ") + text;
  auto [returnType, params] = cDeclarationOf(ret, s, c);
  // (the call's function type takes no parameter attributes: a byval copy's are its argument's)
  for (size_t at = params.find(" byval("); at != std::string::npos; at = params.find(" byval(", at))
  {
    size_t end = params.find(')', at);
    size_t align = params.compare(end + 1, 7, " align ") == 0 ? params.find_first_not_of("0123456789", end + 8) : end + 1;
    params.erase(at, (align == std::string::npos ? params.size() : align) - at);
  }
  std::string callee = returnType + " (" + params + ") " + fnText + "(" + argText + ")";
  if (s.hasSret && !cResult.inMemory)
  {
    // The result in registers, stored into the slot part by part.
    std::string r = newTemp();
    emitCall(r + " = ", callee, false);
    for (size_t i = 0; i < cResult.parts.size(); ++i)
    {
      std::string part = r;
      if (cResult.parts.size() > 1)
      {
        part = newTemp();
        fn += "  " + part + " = extractvalue " + returnType + " " + r + ", " + std::to_string(i) + "\n";
      }
      std::string at8 = newTemp();
      fn += "  " + at8 + " = getelementptr inbounds i8, ptr " + resultAddr + ", i64 " + std::to_string(cResult.offsets[i]) + "\n";
      fn += "  store " + cResult.parts[i] + " " + part + ", ptr " + at8 + "\n";
    }
    if (resultTemp)
      statementTemps.push_back({resultAddr, ret});
    return {resultAddr, ret, sret.empty()};
  }
  if (returnType == "void")
  {
    emitCall("", callee, false);
    if (resultTemp)
      statementTemps.push_back({resultAddr, ret});
    if (s.hasSret)
      return {resultAddr, ret, sret.empty()};
    return {"", types.voidType()};
  }
  std::string r = newTemp();
  emitCall(r + " = ", callee, false);
  return {r, ret, false};
}

auto codegen::Emitter::sharedThunk(const std::string& name, const Signature& s, const std::string& at, const std::string& callee, int slot) -> std::string
{
  auto lock = sharedLock();
  if (emittedShared[name])
    return name;
  emittedShared[name] = true;
  std::string params, args;
  for (size_t i = 1; i < s.paramTypes.size(); ++i)
  {
    params += ", " + s.paramTypes[i] + " %p" + std::to_string(i);
    args += ", " + s.paramTypes[i] + " %p" + std::to_string(i);
  }
  std::string body = "  %off = ptrtoint ptr " + at + " to i64\n  %self = getelementptr i8, ptr %this, i64 %off\n";
  std::string target = callee;
  if (slot >= 0)
  {
    body += "  %vt = load ptr, ptr %self\n  %sp = getelementptr inbounds ptr, ptr %vt, i64 " + std::to_string(slot) + "\n  %fp = load ptr, ptr %sp\n";
    target = "%fp";
  }
  if (s.returnType == "void")
    body += "  tail call void " + target + "(ptr %self" + args + ")\n  ret void\n";
  else
    body += "  %r = tail call " + s.returnType + " " + target + "(ptr %self" + args + ")\n  ret " + s.returnType + " %r\n";
  shared += sharedComdat(name) + "define linkonce_odr " + s.returnType + " " + name + "(ptr %this" + params + ") comdat {\nentry:\n" + body + "}\n";
  return name;
}

auto codegen::Emitter::methodSignature(const Ty* ft, Node* at) -> Signature
{
  Signature s;
  const Ty* ret = ft->inner;
  if (returnsInMemory(ret))
  {
    s.hasSret = true;
    s.returnType = "void";
  }
  else
    s.returnType = !ret || !ret->isKnown() || ret->isReference() ? "ptr" : llType(ret, at);
  s.hasThis = true;
  s.paramTypes.push_back("ptr");
  s.paramTys.push_back(nullptr);
  if (s.hasSret)
  {
    s.paramTypes.push_back("ptr");
    s.paramTys.push_back(nullptr);
  }
  for (const Ty* pt : ft->args)
  {
    s.paramTypes.push_back(isAggregate(pt) || pt->isReference() || !pt->isKnown() ? "ptr" : llType(pt, at));
    s.paramTys.push_back(pt);
  }
  s.variadic = ft->variadic;
  return s;
}

auto codegen::Emitter::signatureOf(FunctionDecl* f) -> Signature
{
  const Ty* ft = declType(f);
  if (!ft || ft->kind != TyKind::Function)
    unsupported(f, "a function whose type is unknown");
  Signature s;
  bool method = global.owningClass(f) && !(f->flags & DF_Static) && !global.isExternC(f);
  const Ty* ret = ft->inner;
  if (returnsInMemory(ret))
  {
    s.hasSret = true;
    s.returnType = "void";
  }
  else if (!ret || !ret->isKnown())
    s.returnType = "ptr"; // unknown return type: treat as opaque pointer
  else
    s.returnType = ret->isReference() ? "ptr" : llType(ret, f);
  if (method)
  {
    s.hasThis = true;
    s.paramTypes.push_back("ptr");
    s.paramTys.push_back(nullptr);
  }
  if (s.hasSret)
  {
    s.paramTypes.push_back("ptr");
    s.paramTys.push_back(nullptr);
  }
  for (size_t i = 0; i < ft->args.size(); ++i)
  {
    const Ty* pt = ft->args[i];
    if (pt->kind == TyKind::Pack)
    {
      // A deduced pack still in one piece (`Args&&...` of a member template): one parameter per element.
      for (const Ty* e : pt->args)
      {
        s.paramTypes.push_back(isAggregate(e) || e->isReference() || !e->isKnown() ? "ptr" : llType(e, f));
        s.paramTys.push_back(e);
      }
      continue;
    }
    s.paramTypes.push_back(isAggregate(pt) || pt->isReference() || !pt->isKnown() ? "ptr" : llType(pt, f));
    s.paramTys.push_back(pt);
  }
  s.variadic = ft->variadic;
  return s;
}

auto codegen::Emitter::signatureText(const Signature& s) -> std::string
{
  std::string t = s.returnType + " (";
  for (size_t i = 0; i < s.paramTypes.size(); ++i)
    t += (i ? ", " : "") + s.paramTypes[i];
  if (s.variadic)
    t += s.paramTypes.empty() ? "..." : ", ...";
  t += ")";
  return t;
}
