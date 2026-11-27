// struct Emitter's members: C calling conventions; their declarations in CCallingConvention.hpp,
// part of the struct's body in Codegen/Emitter.hpp. A translation unit of its own since 2026-11-05
// (codegen.cpp split, stage 2), moved without a change.
#include "Emitter.hpp"

auto codegen::Emitter::cScalars(const Ty* t, uint64_t base, std::vector<std::pair<uint64_t, const Ty*>>& out, Node* at) -> void
{
  t = types.unqualified(t);
  if (t->kind == TyKind::Enum)
    return cScalars(enumUnderlying(t), base, out, at);
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
  p.size = size;
  if (sh.target.windows())
  {
    if (size == 1 || size == 2 || size == 4 || size == 8)
      p.inMemory = false, p.parts = {"i" + std::to_string(size * 8)}, p.offsets = {0};
    return p;
  }
  if (sh.target.arm64())
  {
    if (size == 0)
      return p;
    std::vector<std::pair<uint64_t, const Ty*>> scalars;
    cScalars(t, 0, scalars, at);
    // A homogeneous floating-point aggregate: every scalar a float of one size, at the places of an
    // array of 1 to 4 of them (a union's members may share one).
    uint64_t element = 0;
    bool homogeneous = !scalars.empty();
    for (auto& [offset, s] : scalars)
    {
      if (s->kind == TyKind::Vector)
        unsupported(at, "a struct holding a vector (" + irString(t) + ") passed by value to or from a C function on " + sh.target.triple);
      uint64_t ssize = sizeOf(s, at);
      if (s->kind != TyKind::Float || ssize > 8 || (element && ssize != element) || offset % ssize != 0)
        homogeneous = false;
      element = ssize;
    }
    if (homogeneous && size % element == 0 && size / element <= 4)
    {
      std::vector<bool> filled(size / element, false);
      for (auto& [offset, s] : scalars)
        filled[offset / element] = true;
      if (std::find(filled.begin(), filled.end(), false) == filled.end())
      {
        p.parts = {"[" + std::to_string(size / element) + " x " + (element == 4 ? "float" : "double") + "]"};
        p.offsets = {0};
        p.sses = unsigned(size / element);
        p.inMemory = false;
        return p;
      }
    }
    if (size > 16)
      return p; // through a pointer to a copy (a result through the slot)
    p.parts = {size <= 8 ? "i64" : alignOf(t, at) == 16 ? "i128" : "[2 x i64]"};
    p.offsets = {0};
    p.ints = size <= 8 ? 1 : 2;
    p.inMemory = false;
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
  bool ranOut = false;
  if (!sh.target.windows() && !sh.target.arm64() && !p.inMemory && (p.ints > left.ints || p.sses > left.sses))
    p.inMemory = ranOut = true; // not enough registers left: all of it on the stack, as C does
  if (!p.inMemory)
  {
    left.ints -= std::min(left.ints, p.ints), left.sses -= std::min(left.sses, p.sses);
    // (Linux's AAPCS64 gives a float aggregate on the stack an 8-byte slot, as clang says; Apple's packs)
    if (sh.target.arm64() && !sh.target.mac() && p.sses)
      return {p.parts[0] + " alignstack(8)"};
    return p.parts;
  }
  if (sh.target.windows() || sh.target.arm64())
    return {"ptr"};
  uint64_t align = std::max<uint64_t>(8, alignOf(t, at));
  // Through kpp's C++ back end (--via-cpp) the C++ compiler decides again whether the struct's registers ran
  // out, by what its eightbytes hold: the copy's type says so (floats for an SSE eightbyte, bytes for an
  // INTEGER one), where bytes alone would be passed in the integer registers still free (two doubles after
  // seven, spec interop/c_structs_by_value).
  if (ranOut && sh.target.cpp)
  {
    uint64_t size = sizeOf(t, at);
    std::string members;
    for (size_t i = 0; i < p.parts.size(); ++i)
    {
      uint64_t bytes = (i + 1 < p.offsets.size() ? p.offsets[i + 1] : size) - p.offsets[i];
      const std::string& part = p.parts[i];
      members += (members.empty() ? "" : ", ") +
                 (part == "double" ? std::string("double")
                  : part == "<2 x float>" || (part == "float" && bytes == 8) ? std::string("float, float")
                  : part == "float" ? std::string("float") : "[" + std::to_string(bytes) + " x i8]");
    }
    return {"ptr byval(<{ " + members + " }>) align " + std::to_string(align)};
  }
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
      std::string v = loadCPart(addr, p.offsets[i], p.parts[i], p.size - p.offsets[i]);
      text += (i ? ", " : "") + passed[i] + " " + v;
    }
  }
}

auto codegen::Emitter::loadCPart(const std::string& addr, uint64_t offset, const std::string& type, uint64_t bytes, uint64_t align) -> std::string
{
  if (align)
  {
    std::string from = addr;
    if (offset)
    {
      from = newTemp();
      fn += "  " + from + " = getelementptr inbounds i8, ptr " + addr + ", i64 " + std::to_string(offset) + "\n";
    }
    std::string v = newTemp();
    fn += "  " + v + " = load " + type + ", ptr " + from + ", align " + std::to_string(align) + "\n";
    return v;
  }
  if (type == "[2 x i64]" && bytes < 16)
  {
    std::string low = loadCPart(addr, offset, "i64", 8), high = loadCPart(addr, offset + 8, "i64", bytes - 8);
    std::string half = newTemp(), whole = newTemp();
    fn += "  " + half + " = insertvalue [2 x i64] undef, i64 " + low + ", 0\n";
    fn += "  " + whole + " = insertvalue [2 x i64] " + half + ", i64 " + high + ", 1\n";
    return whole;
  }
  std::string from = addr;
  if (offset)
  {
    from = newTemp();
    fn += "  " + from + " = getelementptr inbounds i8, ptr " + addr + ", i64 " + std::to_string(offset) + "\n";
  }
  std::string v = newTemp();
  if (type == "i64" && bytes < 8)
  {
    std::string narrow = "i" + std::to_string(bytes * 8), n = newTemp();
    fn += "  " + n + " = load " + narrow + ", ptr " + from + "\n";
    fn += "  " + v + " = zext " + narrow + " " + n + " to i64\n";
    return v;
  }
  fn += "  " + v + " = load " + type + ", ptr " + from + "\n";
  return v;
}

auto codegen::Emitter::storeCPart(const std::string& addr, uint64_t offset, const std::string& type, const std::string& value, uint64_t bytes, bool gepAtZero,
                                  uint64_t align) -> void
{
  if (align)
  {
    std::string to = addr;
    if (offset)
    {
      to = newTemp();
      fn += "  " + to + " = getelementptr inbounds i8, ptr " + addr + ", i64 " + std::to_string(offset) + "\n";
    }
    fn += "  store " + type + " " + value + ", ptr " + to + ", align " + std::to_string(align) + "\n";
    return;
  }
  if (type == "[2 x i64]" && bytes < 16)
  {
    std::string low = newTemp(), high = newTemp();
    fn += "  " + low + " = extractvalue [2 x i64] " + value + ", 0\n";
    fn += "  " + high + " = extractvalue [2 x i64] " + value + ", 1\n";
    storeCPart(addr, offset, "i64", low, 8, gepAtZero);
    storeCPart(addr, offset + 8, "i64", high, bytes - 8, gepAtZero);
    return;
  }
  std::string to = addr;
  if (offset || gepAtZero) // (a call's result: every part's address made, as it always was)
  {
    to = newTemp();
    fn += "  " + to + " = getelementptr inbounds i8, ptr " + addr + ", i64 " + std::to_string(offset) + "\n";
  }
  if (type == "i64" && bytes < 8)
  {
    std::string narrow = "i" + std::to_string(bytes * 8), n = newTemp();
    fn += "  " + n + " = trunc i64 " + value + " to " + narrow + "\n";
    fn += "  store " + narrow + " " + n + ", ptr " + to + "\n";
    return;
  }
  fn += "  store " + type + " " + value + ", ptr " + to + "\n";
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
      params = cSlotArgument(ret, f), params.pop_back(), left.ints -= 1;
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
      texts = {s.paramTypes[i] + narrowExtension(pt)};
    }
    for (const std::string& t : texts)
      params += (params.empty() ? "" : ", ") + t;
  }
  if (s.variadic)
    params += params.empty() ? "..." : ", ...";
  return {returnType, params};
}

auto codegen::Emitter::narrowExtension(const Ty* t) -> const char*
{
  if (!sh.target.extendsNarrowIntegers() || !t || t->isReference())
    return "";
  t = types.unqualified(withCurrentArgs(t)); // (an instance's T)
  if (t->kind == TyKind::Enum)
    t = types.unqualified(enumUnderlying(t));
  if (t->kind == TyKind::Bool)
    return " zeroext";
  if ((t->kind == TyKind::Int || t->kind == TyKind::Char) && t->bits < 32)
    return t->isUnsigned ? " zeroext" : " signext";
  return "";
}

auto codegen::Emitter::withNarrowExtension(const Ty* t, const std::string& text) -> std::string
{
  const char* extension = narrowExtension(t);
  size_t space = text.find(' ');
  if (!*extension || space == std::string::npos)
    return text;
  return text.substr(0, space) + extension + text.substr(space);
}

auto codegen::Emitter::cSlotArgument(const Ty* ret, Node* at) -> std::string
{
  if (!ret || sh.target.windows())
    return "ptr ";
  return "ptr sret(" + llType(ret, at) + ") ";
}

auto codegen::Emitter::withoutParameterAttributes(std::string params) -> std::string
{
  for (const char* attribute : {" byval(", " sret(", " alignstack("})
    for (size_t at = params.find(attribute); at != std::string::npos; at = params.find(attribute, at))
    {
      size_t end = params.find(')', at);
      size_t align = params.compare(end + 1, 7, " align ") == 0 ? params.find_first_not_of("0123456789", end + 8) : end + 1;
      params.erase(at, (align == std::string::npos ? params.size() : align) - at);
    }
  for (const char* attribute : {" signext", " zeroext"})
    for (size_t at = params.find(attribute); at != std::string::npos; at = params.find(attribute, at))
      params.erase(at, strlen(attribute));
  return params;
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
    uint64_t offset = cReturnOffsets[i];
    uint64_t align = cReturnAlign && offset ? std::min<uint64_t>(cReturnAlign, offset & (~offset + 1)) : cReturnAlign;
    std::string part = loadCPart(sretAddr, offset, cReturnParts[i], cReturnSize - offset, align);
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
    s.paramTypes.push_back(parameterType(pt, c));
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
  std::vector<std::string> passed; // (handOverArguments: before the lowering)
  for (auto& [t, text] : argList)
    passed.push_back(text);
  handOverArguments(passed);
  lowerCArguments(argList, slot, c);
  for (auto& [t, text] : argList) // (a char, short or bool extended, as the C callee reads it)
    if (t && !isAggregate(t))
      text = withNarrowExtension(t, text);
  std::string argText = slot ? cSlotArgument(ret, c) + resultAddr : std::string();
  for (auto& [t, text] : argList)
    argText += std::string(argText.empty() ? "" : ", ") + text;
  auto [returnType, params] = cDeclarationOf(ret, s, c);
  // (the call's function type takes no parameter attributes: a byval copy's are its argument's)
  params = withoutParameterAttributes(params);
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
      storeCPart(resultAddr, cResult.offsets[i], cResult.parts[i], part, cResult.size - cResult.offsets[i], true);
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
  // (under the register convention, its lowered parameters and result passed on as they are)
  std::vector<std::string> lowered = llvmParams(s);
  std::string returnType = llvmReturn(s), cc = ccText(s);
  std::string params, args;
  for (size_t i = 1; i < lowered.size(); ++i)
  {
    params += ", " + lowered[i] + " %p" + std::to_string(i);
    args += ", " + lowered[i] + " %p" + std::to_string(i);
  }
  std::string body = "  %off = ptrtoint ptr " + at + " to i64\n  %self = getelementptr i8, ptr %this, i64 %off\n";
  std::string target = callee;
  if (slot >= 0)
  {
    body += "  %vt = load ptr, ptr %self" + vptrAccess(sh.sanitizeThread) + "\n  %sp = getelementptr inbounds ptr, ptr %vt, i64 " + std::to_string(slot) + "\n  %fp = load ptr, ptr %sp\n";
    target = "%fp";
  }
  if (returnType == "void")
    body += "  tail call" + cc + " void " + target + "(ptr %self" + args + ")\n  ret void\n";
  else
    body += "  %r = tail call" + cc + " " + returnType + " " + target + "(ptr %self" + args + ")\n  ret " + returnType + " %r\n";
  shared += sharedComdat(name) + "define linkonce_odr" + cc + " " + returnType + " " + name + "(ptr %this" + params + ") comdat {\nentry:\n" + body + "}\n";
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
  {
    s.returnType = !ret || !ret->isKnown() || ret->isReference() ? "ptr" : llType(ret, at);
    s.resultExtension = ret && ret->isKnown() ? narrowExtension(ret) : "";
  }
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
    s.paramTypes.push_back(parameterType(pt, at));
    s.paramTys.push_back(pt);
  }
  s.variadic = ft->variadic;
  if (!ft->isCConvention())
    withRegisters(s, ret, at);
  return s;
}

auto codegen::Emitter::functionTypeSignature(const Ty* ft, Node* at) -> Signature
{
  Signature s;
  const Ty* ret = ft->inner;
  if (returnsInMemory(ret))
  {
    s.hasSret = true;
    s.returnType = "void";
    s.paramTypes.push_back("ptr");
    s.paramTys.push_back(nullptr);
  }
  else
    s.returnType = !ret || !ret->isKnown() || ret->isReference() ? "ptr" : llType(ret, at);
  for (const Ty* pt : ft->args)
  {
    s.paramTypes.push_back(parameterType(pt, at));
    s.paramTys.push_back(pt);
  }
  s.variadic = ft->variadic;
  if (!ft->isCConvention())
    withRegisters(s, ret, at);
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
  {
    s.returnType = ret->isReference() ? "ptr" : llType(ret, f);
    s.resultExtension = narrowExtension(ret);
  }
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
        s.paramTypes.push_back(parameterType(e, f));
        s.paramTys.push_back(e);
      }
      continue;
    }
    s.paramTypes.push_back(parameterType(pt, f));
    s.paramTys.push_back(pt);
  }
  s.variadic = ft->variadic;
  if (!isCFunction(f) && !global.isExternC(f) && !global.closureOfCallOperator(f))
    withRegisters(s, ret, f);
  return s;
}

auto codegen::Emitter::signatureText(const Signature& s) -> std::string
{
  std::vector<std::string> params = llvmParams(s);
  std::string t = llvmReturn(s) + " (";
  for (size_t i = 0; i < params.size(); ++i)
    t += (i ? ", " : "") + params[i];
  if (s.variadic)
    t += params.empty() ? "..." : ", ...";
  t += ")";
  return t;
}

auto codegen::Emitter::registerAbi() -> RegisterAbi
{
  static const RegisterAbi mode = []
  {
    // PROTOTYPE BRANCH: on (vectorcall) when unset, so that CI runs every suite with it; KPP_REGISTER_ABI=off is
    // kpp as on master. Off by default before anything of it lands.
    const char* v = getenv("KPP_REGISTER_ABI");
    if (!v || !*v)
      return RegisterAbi::Vectorcall;
    if (!strcmp(v, "off"))
      return RegisterAbi::Off;
    if (!strcmp(v, "vectorcall"))
      return RegisterAbi::Vectorcall;
    if (!strcmp(v, "sysv"))
      return RegisterAbi::Sysv;
    if (!strcmp(v, "c"))
      return RegisterAbi::C;
    fprintf(stderr, "kpp: KPP_REGISTER_ABI is off, vectorcall, sysv or c, not %s\n", v);
    exit(2);
  }();
  return mode;
}

auto codegen::Emitter::registerPassing(const Ty* t, bool result, Node* at) -> RegisterParts
{
  RegisterParts none;
  t = types.unqualified(t);
  if (!isClass(t) || !t->isKnown())
    return none;
  auto& cache = registerPassingCache[result ? 1 : 0];
  if (auto it = cache.find(t); it != cache.end())
    return it->second;
  RegisterParts p = registerPassingOf(t, result, at);
  cache.emplace(t, p);
  return p;
}

auto codegen::Emitter::registerPassingOf(const Ty* t, bool result, Node* at) -> RegisterParts
{
  RegisterParts none;
  classSizeAndAlign(t, at); // (laid out, the fields' sizes known)
  uint64_t size = sizeOf(t, at);
  if (size > (result ? 16u : 32u))
    return none;
  {
    auto lock = sharedLock();
    StructInfo& info = structFor(t, at);
    if (info.polymorphic || info.packed)
      return none;
  }
  if (needsDestruction(t, at) || !relocatesBitwise(t, at))
    return none;
  std::vector<std::pair<uint64_t, const Ty*>> scalars;
  cScalars(t, 0, scalars, at);
  // Each eightbyte's class, as SysV's: INTEGER where an integer or pointer lies, SSE where only floats
  // do (a float, two, a double), a 16-byte vector two eightbytes of its own; padding alone none.
  enum Kind : uint8_t { None, Integer, Sse, VectorStart, VectorRest };
  size_t words = size_t((size + 7) / 8);
  std::vector<Kind> kind(words, None);
  std::vector<std::string> vectorType(words);
  std::vector<bool> isDouble(words, false), upperFloat(words, false);
  for (auto& [offset, s] : scalars)
  {
    const Ty* u = types.unqualified(s);
    uint64_t ssize = sizeOf(u, at);
    if (ssize == 0)
      continue;
    size_t w = size_t(offset / 8);
    if (u->kind == TyKind::Vector)
    {
      if ((ssize != 8 && ssize != 16) || offset % ssize != 0 || kind[w] != None || (ssize == 16 && kind[w + 1] != None))
        return none;
      kind[w] = VectorStart;
      if (ssize == 16)
        kind[w + 1] = VectorRest;
      vectorType[w] = llType(u, at);
      continue;
    }
    if (offset % 8 + ssize > 8 || offset % std::max<uint64_t>(1, alignOf(u, at)) != 0 || kind[w] == VectorStart || kind[w] == VectorRest)
      return none; // (unaligned; a union of a vector and something else)
    bool fp = u->kind == TyKind::Float;
    if (fp && ssize != 4 && ssize != 8)
      return none; // (a half, x87's long double)
    if (!fp)
      kind[w] = Integer;
    else if (kind[w] != Integer)
    {
      kind[w] = Sse;
      isDouble[w] = isDouble[w] || ssize == 8;
      upperFloat[w] = upperFloat[w] || (ssize == 4 && offset % 8 >= 4);
    }
  }
  RegisterParts p;
  for (size_t w = 0; w < words; ++w)
  {
    std::string part;
    switch (kind[w])
    {
      case None:
      case VectorRest:
        continue;
      case VectorStart:
        part = vectorType[w];
        break;
      case Sse:
        part = isDouble[w] ? "double" : upperFloat[w] ? "<2 x float>" : "float";
        break;
      case Integer:
        part = "i" + std::to_string(std::min<uint64_t>(8, size - w * 8) * 8);
        break;
    }
    p.parts.push_back(part);
    p.offsets.push_back(w * 8);
  }
  if (p.parts.size() > (result ? 2u : 4u))
    return none;
  p.inRegisters = true;
  p.size = size;
  p.align = std::max<uint64_t>(1, alignOf(t, at));
  return p;
}

auto codegen::Emitter::withRegisters(Signature& s, const Ty* ret, Node* at) -> void
{
  RegisterAbi mode = registerAbi();
  if (mode == RegisterAbi::Off || s.variadic || sh.target.cpp)
    return;
  std::vector<RegisterParts> lowered(s.paramTypes.size());
  bool any = false;
  for (size_t i = 0; i < s.paramTypes.size(); ++i)
    if (const Ty* pt = s.paramTys[i]; pt && !pt->isReference() && isClass(types.unqualified(pt)))
    {
      lowered[i] = registerPassing(pt, false, at);
      any = any || lowered[i].inRegisters;
    }
  RegisterParts result;
  if (s.hasSret && ret && !ret->isReference() && isClass(types.unqualified(ret)))
    result = registerPassing(ret, true, at);
  if (!any && !result.inRegisters)
    return;
  s.registers = true;
  s.lowered = std::move(lowered);
  s.result = std::move(result);
  // (MS x64's C convention has four positional registers and passes a 16-byte vector by pointer; vectorcall
  // has MS x64's callee-saved registers, six vector registers and vectors in them; SysV's has more registers,
  // but LLVM's funclets of such a function keep only SysV's callee-saved registers, while Windows' exception
  // runtime calls them as MS x64 functions: a catch block that uses RSI, RDI or XMM6-15 corrupts its caller)
  if (sh.target.windows() && !sh.target.arm64())
    s.cc = mode == RegisterAbi::Sysv ? "x86_64_sysvcc" : mode == RegisterAbi::Vectorcall ? "x86_vectorcallcc" : "";
}

auto codegen::Emitter::emitKppCall(const Signature& s, const std::string& fnText, const std::vector<std::string>& args, bool mayThrow, bool typed) -> std::string
{
  std::string argText, slotAddr;
  size_t slot = s.hasThis ? 1 : 0;
  for (size_t i = 0; i < args.size(); ++i)
  {
    const std::string& a = args[i];
    if (s.registers && s.hasSret && i == slot)
    {
      slotAddr = a.substr(a.find(' ') + 1);
      if (s.result.inRegisters)
        continue;
    }
    if (s.registers && i < s.lowered.size() && s.lowered[i].inRegisters)
    {
      if (a.compare(0, 4, "ptr ") != 0)
        unsupported(currentFunction, "a class argument of the register convention that is no copy in memory (" + a + ")");
      std::string addr = a.substr(4);
      const RegisterParts& p = s.lowered[i];
      for (size_t k = 0; k < p.parts.size(); ++k)
      {
        std::string v = loadCPart(addr, p.offsets[k], p.parts[k], p.size - p.offsets[k], partAlign(p, k));
        argText += (argText.empty() ? "" : ", ") + p.parts[k] + " " + v;
      }
      continue;
    }
    argText += (argText.empty() ? "" : ", ") + a;
  }
  std::string ret = llvmReturn(s);
  std::string callee = (s.cc.empty() ? std::string() : s.cc + " ") + (typed ? signatureText(s) : ret) + " " + fnText + "(" + argText + ")";
  if (ret == "void")
  {
    emitCall("", callee, mayThrow);
    return "";
  }
  std::string r = newTemp();
  emitCall(r + " = ", callee, mayThrow);
  if (!s.registers || !s.result.inRegisters)
    return r;
  const RegisterParts& p = s.result;
  for (size_t k = 0; k < p.parts.size(); ++k)
  {
    std::string part = r;
    if (p.parts.size() > 1)
    {
      part = newTemp();
      fn += "  " + part + " = extractvalue " + ret + " " + r + ", " + std::to_string(k) + "\n";
    }
    storeCPart(slotAddr, p.offsets[k], p.parts[k], part, p.size - p.offsets[k], false, partAlign(p, k));
  }
  return "";
}
