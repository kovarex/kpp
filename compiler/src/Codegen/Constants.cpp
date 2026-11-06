// struct Emitter's members: constants; their declarations in Constants.hpp, part of the struct's
// body in Codegen/Emitter.hpp. A translation unit of its own since 2026-11-05 (codegen.cpp split,
// stage 2), moved without a change.
#include "Emitter.hpp"

auto codegen::Emitter::spelling(uint32_t token) const -> std::string_view
{
  const std::vector<Token>& toks = *currentFile->tokens;
  const Token& t = toks[std::min<size_t>(token, toks.size() - 1)];
  return currentFile->text.substr(t.offset, t.length);
}

auto codegen::Emitter::literalSpelling(const LiteralExpr* l, uint32_t token, std::string& buffer) const -> std::string_view
{
  const ParsedFile* f = literalFile(l);
  const std::vector<Token>& toks = *f->tokens;
  const Token& t = toks[std::min<size_t>(token, toks.size() - 1)];
  return tokenSpelling(f->text, t, buffer);
}

auto codegen::Emitter::isParenthesizedTypeName(Expr* e) -> bool
{
  if (!e || e->kind != NodeKind::ParenExpr)
    return false;
  Expr* inner = static_cast<ParenExpr*>(e)->inner;
  if (!inner || inner->kind != NodeKind::NameExpr)
    return false;
  Decl* d = static_cast<NameExpr*>(inner)->resolved;
  return d && (d->kind == NodeKind::ClassDecl || d->kind == NodeKind::EnumDecl || d->kind == NodeKind::UsingAliasDecl ||
               (d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef)));
}

auto codegen::Emitter::valueParamType(Decl* param) -> const Ty*
{
  auto* p = param && param->kind == NodeKind::TemplateParamDecl ? static_cast<TemplateParamDecl*>(param) : nullptr;
  if (p && !p->isType && p->valueType)
    if (const Ty* t = global.typeOfAst(p->valueType, p); t && t->isKnown() && !types.isDependent(t))
      return types.unqualified(t);
  return types.intType();
}

auto codegen::Emitter::variableTemplateArgs(NameExpr* n, VariableDecl* v, Node* at) -> TemplateArgs
{
  TemplateArgs args, none;
  Decl* context = constContext ? constContext : typingContext();
  const NamePart& last = n->name.last();
  std::vector<const Ty*> packElements;
  size_t count = v->templateParams.size();
  auto* lastParam = count ? static_cast<TemplateParamDecl*>(v->templateParams[count - 1]) : nullptr;
  for (size_t i = 0; i < last.templateArgs.size(); ++i)
  {
    Node* arg = last.templateArgs[i];
    const Ty* t = nullptr;
    if (uint8_t(arg->kind) <= uint8_t(NodeKind::PackType))
      t = global.typeOfAstIn(static_cast<Type*>(arg), context, currentArgs ? *currentArgs : none);
    else if (std::optional<int64_t> c = evalConst(static_cast<Expr*>(arg)))
      t = types.valueType(valueParamType(i < count ? v->templateParams[i] : nullptr), *c);
    if (!t || !t->isKnown())
      unsupported(at, "an argument of " + qualifiedName(v) + " that is not a type or a constant");
    if (lastParam && lastParam->isPack && i + 1 >= count)
      packElements.push_back(t);
    else if (i < count)
      args.pairs.push_back({v->templateParams[i], t});
    else
      unsupported(at, "more arguments than " + qualifiedName(v) + " has parameters");
  }
  if (lastParam && lastParam->isPack)
    args.pairs.push_back({lastParam, types.pack(Span<const Ty*>{packElements.data(), uint32_t(packElements.size())})});
  // Fill in default arguments for parameters not covered by the written arguments.
  for (size_t i = args.pairs.size(); i < count; ++i)
  {
    auto* param = static_cast<TemplateParamDecl*>(v->templateParams[i]);
    if (param->defaultArg)
    {
      const Ty* t = nullptr;
      if (param->isType && uint8_t(param->defaultArg->kind) <= uint8_t(NodeKind::PackType))
        t = global.typeOfAstIn(static_cast<Type*>(param->defaultArg), v, args); // `class U = T`: the parameters before it
      else
      {
        ParsedFile* saved = currentFile;
        ConstContextScope inTemplate(*this, v);
        ArgsScope withArgs(*this, &args);
        currentFile = v->file ? v->file : currentFile;
        std::optional<int64_t> c = evalConst(static_cast<Expr*>(param->defaultArg));
        currentFile = saved;
        if (c)
          t = types.valueType(valueParamType(param), *c);
      }
      if (t && t->isKnown())
      {
        args.pairs.push_back({param, t});
        continue;
      }
    }
    unsupported(at, "the variable template " + qualifiedName(v) + " used without all its arguments");
  }
  return args;
}

auto codegen::Emitter::foldArith() -> ConstArith
{
  return ConstArith{types, ConstMode::Fold, nullptr, [this](const Ty* e) { return e->decl ? enumUnderlying(e) : nullptr; }};
}

auto codegen::Emitter::foldHooks(Node* at, bool& layoutFailed) -> ConstHooks
{
  ConstHooks hooks;
  hooks.mode = ConstMode::Fold;
  hooks.underlying = [this](const Ty* e) { return e->decl ? enumUnderlying(e) : nullptr; };
  hooks.layout = [this, at, &layoutFailed](const Ty* t, bool alignment) -> std::optional<int64_t>
  {
    try
    {
      t = types.stripRef(t);
      return int64_t(alignment ? alignOf(t, at) : sizeOf(t, at));
    }
    catch (Unsupported&)
    {
      layoutFailed = true; // not thrown through the checker's evaluator
      return std::nullopt;
    }
  };
  hooks.offset = [this, at, &layoutFailed](const Ty* t, Decl* m) -> std::optional<int64_t>
  {
    try
    {
      std::optional<uint64_t> o = memberOffset(t, m, at);
      return o ? std::optional<int64_t>(int64_t(*o)) : std::nullopt;
    }
    catch (Unsupported&)
    {
      layoutFailed = true;
      return std::nullopt;
    }
  };
  hooks.ran = [this](const FunctionDecl* f) { noteInlined(f); }; // its body is in the folded value
  return hooks;
}

auto codegen::Emitter::memberOffset(const Ty* t, Decl* m, Node* at, int depth) -> std::optional<uint64_t>
{
  t = types.unqualified(types.stripRef(t));
  if (!isClass(t) || depth > 16)
    return std::nullopt;
  classSizeAndAlign(t, at); // laid out, the fields' sizes known
  auto lock = sharedLock();
  StructInfo& info = structFor(t, at);
  auto start = [&](uint32_t index) { return fieldStart(info, index, at); };
  if (auto it = info.fieldIndex.find(m); it != info.fieldIndex.end())
    return info.isUnion ? 0 : start(it->second);
  for (const auto& [base, index] : info.bases)
    if (index < info.fieldTypes.size())
      if (std::optional<uint64_t> inner = memberOffset(info.fieldTypes[index], m, at, depth + 1))
        return start(index) + *inner;
  return std::nullopt;
}

auto codegen::Emitter::evalConst(Expr* e, int) -> std::optional<int64_t>
{
  std::optional<ConstValue> v = foldConst(e);
  if (!v || v->kind != ConstValue::Kind::Int)
    return std::nullopt; // a floating value, an object, a pointer: no integer constant
  return v->i;
}

auto codegen::Emitter::evalCondition(Expr* e) -> std::optional<bool>
{
  std::optional<ConstValue> v = foldConst(e);
  return v && (v->isScalar() || v->isPointer()) ? std::optional<bool>(v->truthy()) : std::nullopt;
}

auto codegen::Emitter::foldAs(std::optional<ConstValue> v, const Ty* t) -> std::optional<ConstValue>
{
  if (!v || !t || !t->isKnown() || types.isDependent(t) || !v->isScalar())
    return v;
  t = types.unqualified(types.stripRef(t));
  return t->isArithmetic() || t->kind == TyKind::Enum ? foldArith().convert(*v, t) : v;
}

auto codegen::Emitter::foldConst(Expr* e) -> std::optional<ConstValue>
{
  if (!e)
    return std::nullopt;
  if (e->kind == NodeKind::LiteralExpr)
  {
    auto* l = static_cast<LiteralExpr*>(e);
    std::string spliced;
    if (l->literalKind == TokenKind::Number || l->literalKind == TokenKind::Char)
      return foldArith().literal(l->literalKind, literalSpelling(l, l->token, spliced)); // from its own file's tokens
  }
  TemplateArgs none;
  bool layoutFailed = false;
  std::optional<ConstValue> v =
    global.evaluate(e, currentFile, constContext ? constContext : typingContext(), currentArgs ? *currentArgs : none, foldHooks(e, layoutFailed));
  if (!v && layoutFailed)
    throw Unsupported{}; // the failed layout reported itself; what needs the constant fails with it
  return v;
}

auto codegen::Emitter::enumeratorNamed(EnumeratorDecl* en, Expr* e) -> int64_t
{
  for (Decl* p = en->parent; p; p = p->parent)
    if (!p->templateParams.empty())
    {
      if (std::optional<int64_t> v = evalConst(e))
        return *v;
      break;
    }
  return enumeratorValue(en, e->ty);
}

auto codegen::Emitter::enumeratorValue(EnumeratorDecl* en, const Ty* type) -> int64_t
{
  // Of an instance (`Tm<3>::value`): as the interpreter computes it from the instance's arguments.
  const Ty* instance = type ? withCurrentArgs(types.unqualified(types.stripRef(type))) : nullptr;
  if (instance && instance->kind == TyKind::Enum && !instance->args.empty() && !types.isDependent(instance))
  {
    std::pair<Decl*, const Ty*> key{en, instance};
    if (auto known = instanceEnumerators.find(key); known != instanceEnumerators.end())
      return known->second;
    bool layoutFailed = false;
    ConstContextScope context(*this, en->parent);
    std::optional<ConstValue> v = global.evaluateEnumerator(en, instance, foldHooks(en, layoutFailed));
    if (!v || v->kind != ConstValue::Kind::Int)
      unsupported(en, "the enumerator " + sym(en->name) + " of " + types.toString(instance, &symbols) + ", no constant the compiler could evaluate");
    instanceEnumerators[key] = v->i;
    return v->i;
  }
  auto it = enumeratorValues.find(en);
  if (it != enumeratorValues.end())
    return it->second;
  auto* e = static_cast<EnumDecl*>(en->parent);
  int64_t next = 0;
  ParsedFile* saved = currentFile;
  if (e && e->file)
    currentFile = e->file;
  ConstContextScope context(*this, e); // `B = A + 1` names the enumeration's own A
  if (e)
    for (Decl* d : e->enumerators)
    {
      if (d->kind != NodeKind::EnumeratorDecl)
        continue;
      auto* cur = static_cast<EnumeratorDecl*>(d);
      if (cur->value)
      {
        std::optional<int64_t> v = evalConst(cur->value);
        if (!v)
          unsupported(cur, "the enumerator " + sym(cur->name) + ", no constant the compiler could evaluate"); // (never the one before plus one)
        next = *v;
      }
      enumeratorValues[cur] = next;
      ++next;
    }
  currentFile = saved;
  it = enumeratorValues.find(en);
  return it == enumeratorValues.end() ? 0 : it->second;
}

auto codegen::Emitter::decodeString(std::string_view s, bool wide) -> std::string
{
  size_t q = s.find_first_of("\"'");
  if (q == std::string_view::npos)
    return {};
  char quote = s[q];
  bool raw = q > 0 && s[q - 1] == 'R';
  std::string out;
  auto utf8 = [&](uint32_t cp)
  {
    if (cp < 0x80)
      out += char(cp);
    else if (cp < 0x800)
    {
      out += char(0xC0 | (cp >> 6));
      out += char(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000)
    {
      out += char(0xE0 | (cp >> 12));
      out += char(0x80 | ((cp >> 6) & 0x3F));
      out += char(0x80 | (cp & 0x3F));
    }
    else
    {
      out += char(0xF0 | (cp >> 18));
      out += char(0x80 | ((cp >> 12) & 0x3F));
      out += char(0x80 | ((cp >> 6) & 0x3F));
      out += char(0x80 | (cp & 0x3F));
    }
  };
  auto unit = [&](uint32_t v)
  {
    if (wide)
      utf8(v);
    else
      out += char(v);
  };
  auto hexValue = [](char c) { return uint32_t(isdigit((unsigned char)c) ? c - '0' : tolower((unsigned char)c) - 'a' + 10); };
  if (raw)
  {
    size_t open = s.find('(', q);
    size_t close = s.rfind(')');
    if (open != std::string_view::npos && close != std::string_view::npos && close > open)
      appendRawStringBody(out, s.substr(open + 1, close - open - 1));
    return out;
  }
  for (size_t i = q + 1; i < s.size() && s[i] != quote; ++i)
  {
    char c = s[i];
    if (c != '\\')
    {
      out += c;
      continue;
    }
    if (++i >= s.size())
      break;
    char e = s[i];
    switch (e)
    {
      case 'n': out += '\n'; break;
      case 't': out += '\t'; break;
      case 'r': out += '\r'; break;
      case 'a': out += '\a'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'v': out += '\v'; break;
      case 'x':
      {
        uint32_t v = 0;
        while (i + 1 < s.size() && isxdigit((unsigned char)s[i + 1]))
          v = v * 16 + hexValue(s[++i]);
        unit(v);
        break;
      }
      case 'u':
      case 'U':
      {
        uint32_t v = 0;
        for (int digits = e == 'u' ? 4 : 8; digits > 0 && i + 1 < s.size() && isxdigit((unsigned char)s[i + 1]); --digits)
          v = v * 16 + hexValue(s[++i]);
        utf8(v);
        break;
      }
      default:
        if (e >= '0' && e <= '7')
        {
          uint32_t v = uint32_t(e - '0');
          for (int digits = 2; digits > 0 && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '7'; --digits)
            v = v * 8 + uint32_t(s[++i] - '0');
          unit(v);
        }
        else
          out += e;
        break;
    }
  }
  return out;
}

auto codegen::Emitter::stringConstant(LiteralExpr* l) -> std::string
{
  std::string spliced;
  std::string_view first = literalSpelling(l, l->token, spliced);
  // (L"..." is in the target's wchar_t: UTF-16 on Windows, UTF-32 elsewhere)
  bool wideL = first.starts_with("L");
  bool wide16 = (wideL && types.wcharBits() == 16) || (first.starts_with("u") && !first.starts_with("u8"));
  bool wide32 = first.starts_with("U") || (wideL && types.wcharBits() == 32);
  std::string bytes;
  const std::vector<Token>& toks = *literalFile(l)->tokens;
  for (size_t i = l->token; i < toks.size() && toks[i].getKind() == TokenKind::String; ++i)
    bytes += decodeString(literalSpelling(l, uint32_t(i), spliced), wide16 || wide32);
  if (wide16 || wide32)
    return wideStringConstant(bytes, wide16 ? 16 : 32);
  return cStringConstant(bytes);
}

auto codegen::Emitter::stringLiteralUnits(LiteralExpr* l, uint32_t& unitBytes) -> size_t
{
  std::string spliced;
  std::string_view first = literalSpelling(l, l->token, spliced);
  bool wideL = first.starts_with("L");
  bool wide16 = (wideL && types.wcharBits() == 16) || (first.starts_with("u") && !first.starts_with("u8"));
  unitBytes = wide16 ? 2 : first.starts_with("U") || wideL ? 4 : 1;
  std::string bytes;
  const std::vector<Token>& toks = *literalFile(l)->tokens;
  for (size_t i = l->token; i < toks.size() && toks[i].getKind() == TokenKind::String; ++i)
    bytes += decodeString(literalSpelling(l, uint32_t(i), spliced), unitBytes != 1);
  if (unitBytes == 1)
    return bytes.size();
  size_t units = 0;
  for (size_t i = 0; i < bytes.size();)
  {
    unsigned char c = bytes[i];
    size_t n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
    units += unitBytes == 2 && n == 4 ? 2 : 1; // a surrogate pair in UTF-16
    i += n;
  }
  return units;
}

auto codegen::Emitter::wideStringConstant(const std::string& utf8, int bits) -> std::string
{
  std::vector<uint32_t> points;
  for (size_t i = 0; i < utf8.size();)
  {
    unsigned char c = utf8[i];
    uint32_t cp;
    size_t n;
    if (c < 0x80) { cp = c; n = 1; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
    else { cp = c & 0x07; n = 4; }
    for (size_t k = 1; k < n && i + k < utf8.size(); ++k)
      cp = (cp << 6) | (utf8[i + k] & 0x3F);
    points.push_back(cp);
    i += n;
  }
  std::vector<uint32_t> units;
  for (uint32_t cp : points)
  {
    if (bits == 16 && cp >= 0x10000)
    {
      cp -= 0x10000;
      units.push_back(0xD800 + (cp >> 10));
      units.push_back(0xDC00 + (cp & 0x3FF));
    }
    else
      units.push_back(cp);
  }
  units.push_back(0);
  std::string unit = bits == 16 ? "i16" : "i32", elements;
  for (size_t i = 0; i < units.size(); ++i)
    elements += std::string(i ? ", " : "") + unit + " " + std::to_string(units[i]);
  std::string contents = "[" + std::to_string(units.size()) + " x " + unit + "] [" + elements + "]";
  auto [name, isNew] = constantName("@.wstr.", contents);
  if (isNew)
    (emittingShared ? decls : moduleDecls[currentModule]) += literalDefinition(name, contents);
  return name;
}

auto codegen::Emitter::constantName(const char* prefix, const std::string& contents) -> std::pair<std::string, bool>
{
  std::string key = (emittingShared ? std::string("*") : std::to_string(currentModule)) + "|" + prefix + "|" + contents;
  if (auto mine = constantCache.find(key); mine != constantCache.end())
    return {mine->second, false};
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : contents)
    h = (h ^ c) * 1099511628211ull;
  char hex[17];
  snprintf(hex, sizeof hex, "%016llx", (unsigned long long)h);
  std::string base = std::string(prefix) + hex, name = base;
  std::lock_guard lock(sh.constantsMutex);
  for (int n = 2;; ++n)
  {
    auto [it, inserted] = sh.literalContents.emplace(name, contents);
    if (inserted || it->second == contents)
      break;
    name = base + "." + std::to_string(n); // two contents with one hash (which takes the suffix is the emission order's)
  }
  bool isNew = emittingShared ? sh.sharedLiterals.insert(name).second
                              : !sh.sharedLiterals.count(name) && sh.moduleLiterals[currentModule].insert(name).second;
  constantCache.emplace(std::move(key), name);
  return {name, isNew};
}

auto codegen::Emitter::literalDefinition(const std::string& name, const std::string& contents) -> std::string
{
  return "$" + name.substr(1) + " = comdat any\n" + name + " = linkonce_odr unnamed_addr constant " + contents + ", comdat\n";
}

auto codegen::Emitter::irTypeName(const char* prefix, const Ty* t) -> std::string
{
  std::string printed = irString(t);
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : printed)
    h = (h ^ c) * 1099511628211ull;
  char hex[17];
  snprintf(hex, sizeof hex, "%016llx", (unsigned long long)h);
  std::string base = std::string(prefix) + hex, name = base;
  std::lock_guard lock(sh.irTypeNamesMutex); // (the names alone: no layout is read here)
  for (int n = 2;; ++n)
  {
    auto [it, inserted] = irTypeNames.emplace(name, t);
    if (inserted || it->second == t)
      return name;
    name = base + "." + std::to_string(n);
  }
}

auto codegen::Emitter::cStringConstant(std::string bytes) -> std::string
{
  bytes += '\0';
  std::string escaped;
  for (unsigned char c : bytes)
  {
    if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\')
      escaped += char(c);
    else
    {
      char buf[8];
      snprintf(buf, sizeof buf, "\\%02X", c);
      escaped += buf;
    }
  }
  std::string contents = "[" + std::to_string(bytes.size()) + " x i8] c\"" + escaped + "\"";
  auto [name, isNew] = constantName("@.str.", contents);
  if (isNew)
    (emittingShared ? decls : moduleDecls[currentModule]) += literalDefinition(name, contents);
  return name;
}

auto codegen::Emitter::useVtable(const StructInfo& info) -> void
{
  auto lock = sharedLock();
  if (info.vtableText.empty() || !vtablesIn[currentModule].insert(info.vtableName).second)
    return;
  moduleDecls[currentModule] += info.vtableText;
  useDescriptor(info);
}

auto codegen::Emitter::descriptorName(const std::string& descriptor) -> std::string
{
  std::string n = newTemp();
  fn += "  " + n + " = load ptr, ptr " + descriptor + "\n";
  return n;
}

auto codegen::Emitter::dynamicCast(NamedCastExpr* c, Expr* e) -> std::optional<Value>
{
  const Ty* target = types.stripRef(e->ty);
  if (!target || !target->isKnown())
    return std::nullopt;
  bool pointer = target->kind == TyKind::Pointer;
  const Ty* tcls = pointer ? target->inner : target;
  if (pointer && tcls && tcls->kind == TyKind::Void)
  {
    // `dynamic_cast<void*>(p)` ([expr.dynamic.cast] 7): the most derived object's address, the
    // subobject's own less its offset in that object (its vtable's offset to the top), null for
    // null. A conversion of the pointer alone gave a base's address that is not the first: `pb`
    // itself for a B* into `class C : A, B` (thread A's survey, 2026-11-06).
    Value p = rvalue(c->operand);
    const Ty* pointee = p.ty && p.ty->kind == TyKind::Pointer ? p.ty->inner : nullptr;
    if (!pointee || !isClass(pointee) || !structFor(pointee, e).polymorphic)
      return explicitConvert(p, e->ty, e); // (a pointer to a class that is not polymorphic: C++'s error, the checker's)
    auto lock = sharedLock();
    if (declaredNames.claim("@kpp.mostderived"))
    {
      decls += "$kpp.mostderived = comdat any\n"
               "define linkonce_odr ptr @kpp.mostderived(ptr %obj) comdat {\n"
               "entry:\n  %isnull = icmp eq ptr %obj, null\n  br i1 %isnull, label %null, label %start\n"
               "start:\n  %vt = load ptr, ptr %obj\n  %tp = getelementptr inbounds ptr, ptr %vt, i64 -2\n  %top = load ptr, ptr %tp\n"
               "  %topi = ptrtoint ptr %top to i64\n  %neg = sub i64 0, %topi\n  %r = getelementptr i8, ptr %obj, i64 %neg\n  ret ptr %r\n"
               "null:\n  ret ptr null\n}\n";
    }
    std::string r = newTemp();
    fn += "  " + r + " = call ptr @kpp.mostderived(ptr " + p.text + ")" + bundle() + "\n";
    return Value{r, target, false};
  }
  if (!isClass(tcls))
    return std::nullopt;
  Value o = rvalue(c->operand);
  const Ty* scls = o.ty && o.ty->kind == TyKind::Pointer ? o.ty->inner : types.stripRef(o.ty);
  // The static conversion of the value made (the same class or a base of it, or a class that is not
  // polymorphic, the checker's error), never nullopt: the caller's conversion would emit the
  // operand again, and `dynamic_cast<Base*>(next())` called next() twice (until 2026-11-06).
  if (!isClass(scls) || !structFor(scls, e).polymorphic)
    return explicitConvert(o, e->ty, e);
  std::vector<std::pair<std::string, uint32_t>> path;
  if (classKey(scls) == classKey(tcls) || basePath(scls, tcls, path, e))
    return explicitConvert(o, e->ty, e);
  std::string descriptor = descriptorFor(tcls, e), source = descriptorFor(scls, e);
  useDescriptor(structFor(tcls, e));
  useDescriptor(structFor(scls, e));
  auto lock = sharedLock();
  if (!dyncastEmitted)
  {
    dyncastEmitted = true;
    decls += dynamicCastFunctions();
  }
  std::string r = newTemp();
  fn += "  " + r + " = call ptr @kpp.dyncast(ptr " + o.text + ", ptr " + source + ", ptr " + descriptor + ")" + bundle() + "\n";
  if (pointer)
    return Value{r, e->ty, false};
  // `dynamic_cast<const T&>(x)`: the T object itself (its members are called on it); an x that
  // is no T throws BadCast.
  std::string bad = newTemp(), okL = newLabel("dyncast.ok"), badL = newLabel("dyncast.bad");
  fn += "  " + bad + " = icmp eq ptr " + r + ", null\n";
  fn += "  br i1 " + bad + ", label %" + badL + ", label %" + okL + "\n";
  startBlock(badL);
  throwFromLibrary("throwBadCast", e, "dynamic_cast to a reference of an object that is none");
  blockTerminated = true;
  startBlock(okL);
  return Value{r, target, false};
}

auto codegen::Emitter::dynamicCastFunctions() -> std::string
{
  // [expr.dynamic.cast] 8. A descriptor of a class with every base public and none twice (plain,
  // nearly all) needs a search for the target alone (@kpp.basesearch, first hit). Else
  // @kpp.dyncast.walk visits every subobject of the whole object and fills %st: { the target
  // subobject the operand is a public base of, or -1; the number of target subobjects; the last
  // one's place; whether it is public; whether the operand is a public base of the whole object }.
  return "$kpp.dyncast = comdat any\n"
         "define linkonce_odr ptr @kpp.dyncast(ptr %obj, ptr %source, ptr %target) comdat {\n"
         "entry:\n  %st = alloca [5 x i64]\n  %isnull = icmp eq ptr %obj, null\n  br i1 %isnull, label %fail, label %start\n"
         "start:\n  %vt = load ptr, ptr %obj\n  %tp = getelementptr inbounds ptr, ptr %vt, i64 -2\n  %top = load ptr, ptr %tp\n"
         "  %topi = ptrtoint ptr %top to i64\n  %dp = getelementptr inbounds ptr, ptr %vt, i64 -1\n  %d = load ptr, ptr %dp\n"
         "  %plainp = getelementptr inbounds i8, ptr %d, i64 16\n  %plain = load i64, ptr %plainp\n  %isPlain = icmp ne i64 %plain, 0\n"
         "  br i1 %isPlain, label %search, label %walk\n"
         "search:\n  %off = call i64 @kpp.basesearch(ptr %d, ptr %target)\n  %found = icmp sge i64 %off, 0\n  br i1 %found, label %ok, label %fail\n"
         "walk:\n  store i64 -1, ptr %st\n  %count = getelementptr inbounds i8, ptr %st, i64 8\n  store i64 0, ptr %count\n"
         "  %place = getelementptr inbounds i8, ptr %st, i64 16\n  %public = getelementptr inbounds i8, ptr %st, i64 24\n  store i64 0, ptr %public\n"
         "  %sourcePublic = getelementptr inbounds i8, ptr %st, i64 32\n  store i64 0, ptr %sourcePublic\n"
         "  %whole = call i1 @kpp.dyncast.walk(ptr %d, i64 0, i1 true, ptr %source, i64 %topi, ptr %target, ptr %st)\n"
         "  %down = load i64, ptr %st\n  %isDown = icmp sge i64 %down, 0\n  br i1 %isDown, label %ok, label %cross\n"
         "cross:\n  %n = load i64, ptr %count\n  %one = icmp eq i64 %n, 1\n  %p = load i64, ptr %public\n  %isPublic = icmp ne i64 %p, 0\n"
         "  %s = load i64, ptr %sourcePublic\n  %fromPublic = icmp ne i64 %s, 0\n  %both = and i1 %isPublic, %fromPublic\n  %may = and i1 %one, %both\n"
         "  %at = load i64, ptr %place\n  br i1 %may, label %ok, label %fail\n"
         "ok:\n  %result = phi i64 [ %off, %search ], [ %down, %walk ], [ %at, %cross ]\n  %delta = sub i64 %result, %topi\n"
         "  %r = getelementptr i8, ptr %obj, i64 %delta\n  ret ptr %r\n"
         "fail:\n  ret ptr null\n}\n"
         "$kpp.dyncast.walk = comdat any\n"
         "define linkonce_odr i1 @kpp.dyncast.walk(ptr %d, i64 %at, i1 %pub, ptr %source, i64 %sourceAt, ptr %target, ptr %st) comdat {\n"
         "entry:\n  %isClass = icmp eq ptr %d, %source\n  %isPlace = icmp eq i64 %at, %sourceAt\n  %isSource = and i1 %isClass, %isPlace\n"
         "  %np = getelementptr inbounds i8, ptr %d, i64 8\n  %n = load i64, ptr %np\n  br label %loop\n"
         "loop:\n  %i = phi i64 [ 0, %entry ], [ %i1, %body ]\n  %has = phi i1 [ %isSource, %entry ], [ %has1, %body ]\n"
         "  %more = icmp slt i64 %i, %n\n  br i1 %more, label %body, label %done\n"
         "body:\n  %skip = mul i64 %i, 24\n  %first = getelementptr inbounds i8, ptr %d, i64 24\n  %e = getelementptr inbounds i8, ptr %first, i64 %skip\n"
         "  %b = load ptr, ptr %e\n  %op = getelementptr inbounds i8, ptr %e, i64 8\n  %o = load i64, ptr %op\n"
         "  %fp = getelementptr inbounds i8, ptr %e, i64 16\n  %f = load i64, ptr %fp\n  %isPublic = icmp ne i64 %f, 0\n"
         "  %sub = add i64 %at, %o\n  %subPublic = and i1 %pub, %isPublic\n"
         "  %inside = call i1 @kpp.dyncast.walk(ptr %b, i64 %sub, i1 %subPublic, ptr %source, i64 %sourceAt, ptr %target, ptr %st)\n"
         "  %kept = and i1 %inside, %isPublic\n  %has1 = or i1 %has, %kept\n  %i1 = add i64 %i, 1\n  br label %loop\n"
         "done:\n  %wholeSource = and i1 %isSource, %pub\n  br i1 %wholeSource, label %noteSource, label %check\n"
         "noteSource:\n  %sp = getelementptr inbounds i8, ptr %st, i64 32\n  store i64 1, ptr %sp\n  br label %check\n"
         "check:\n  %isTarget = icmp eq ptr %d, %target\n  br i1 %isTarget, label %noteTarget, label %end\n"
         "noteTarget:\n  %cp = getelementptr inbounds i8, ptr %st, i64 8\n  %c = load i64, ptr %cp\n  %c1 = add i64 %c, 1\n  store i64 %c1, ptr %cp\n"
         "  %lp = getelementptr inbounds i8, ptr %st, i64 16\n  store i64 %at, ptr %lp\n  %pp = getelementptr inbounds i8, ptr %st, i64 24\n"
         "  %pz = zext i1 %pub to i64\n  store i64 %pz, ptr %pp\n  br i1 %has, label %noteDown, label %end\n"
         "noteDown:\n  store i64 %at, ptr %st\n  br label %end\n"
         "end:\n  ret i1 %has\n}\n"
         "$kpp.basesearch = comdat any\n"
         "define linkonce_odr i64 @kpp.basesearch(ptr %d, ptr %target) comdat {\n"
         "entry:\n  %same = icmp eq ptr %d, %target\n  br i1 %same, label %yes, label %scan\n"
         "scan:\n  %np = getelementptr inbounds i8, ptr %d, i64 8\n  %n = load i64, ptr %np\n  br label %loop\n"
         "loop:\n  %i = phi i64 [ 0, %scan ], [ %i1, %next ]\n  %more = icmp slt i64 %i, %n\n  br i1 %more, label %body, label %no\n"
         "body:\n  %at = mul i64 %i, 24\n  %at1 = add i64 %at, 24\n  %e = getelementptr inbounds i8, ptr %d, i64 %at1\n  %b = load ptr, ptr %e\n"
         "  %op = getelementptr inbounds i8, ptr %e, i64 8\n  %o = load i64, ptr %op\n  %r = call i64 @kpp.basesearch(ptr %b, ptr %target)\n"
         "  %hit = icmp sge i64 %r, 0\n  br i1 %hit, label %found, label %next\n"
         "found:\n  %sum = add i64 %r, %o\n  ret i64 %sum\n"
         "next:\n  %i1 = add i64 %i, 1\n  br label %loop\n"
         "yes:\n  ret i64 0\nno:\n  ret i64 -1\n}\n";
}

auto codegen::Emitter::baseSearchFunction(const std::string& name, const std::string& nullDescriptor) -> std::string
{
  // What a thrown type's descriptor lists is all it converts to (typeinfoFor): no search below it.
  std::string markers;
  if (!nullDescriptor.empty())
    markers = "entry:\n  %tn = getelementptr inbounds i8, ptr %target, i64 8\n  %tcount = load i64, ptr %tn\n"
              "  %marked = icmp slt i64 %tcount, 0\n  br i1 %marked, label %marker, label %plain\n"
              "marker:\n  %rp = getelementptr inbounds i8, ptr %target, i64 16\n  %real = load ptr, ptr %rp\n"
              "  %isExact = icmp eq i64 %tcount, -1\n  br i1 %isExact, label %exact, label %convert\n"
              "exact:\n  %exactSame = icmp eq ptr %d, %real\n  %exactResult = select i1 %exactSame, i64 0, i64 -1\n  ret i64 %exactResult\n"
              "convert:\n  %isNull = icmp eq ptr %d, " + nullDescriptor + "\n  br i1 %isNull, label %yes, label %convertSearch\n"
              "convertSearch:\n  %c = call i64 " + name + "(ptr %d, ptr %real)\n  ret i64 %c\n"
              "plain:\n";
  return "$" + name.substr(1) + " = comdat any\n" + baseSearchHeader(name) + "\n" + (markers.empty() ? "entry:\n" : markers) +
         "  %same = icmp eq ptr %d, %target\n  br i1 %same, label %yes, label %scan\n"
         "scan:\n  %np = getelementptr inbounds i8, ptr %d, i64 8\n  %n = load i64, ptr %np\n  br label %loop\n"
         "loop:\n  %i = phi i64 [ 0, %scan ], [ %i1, %next ]\n  %more = icmp slt i64 %i, %n\n  br i1 %more, label %body, label %no\n"
         "body:\n  %at = mul i64 %i, 16\n  %at1 = add i64 %at, 16\n  %e = getelementptr inbounds i8, ptr %d, i64 %at1\n  %b = load ptr, ptr %e\n"
         "  %op = getelementptr inbounds i8, ptr %e, i64 8\n  %o = load i64, ptr %op\n"
         "  %hit = icmp eq ptr %b, %target\n  br i1 %hit, label %found, label %next\n"
         "found:\n  ret i64 %o\n"
         "next:\n  %i1 = add i64 %i, 1\n  br label %loop\n"
         "yes:\n  ret i64 0\nno:\n  ret i64 -1\n}\n";
}

auto codegen::Emitter::floatConstant(double value, uint8_t bits) -> std::string
{
  double exact = bits == 32 ? double(float(value)) : value;
  uint64_t raw;
  memcpy(&raw, &exact, sizeof raw);
  char buf[32];
  snprintf(buf, sizeof buf, "0x%016llX", (unsigned long long)raw);
  return buf;
}

auto codegen::Emitter::lineColumnOf(uint32_t token) -> std::pair<uint32_t, uint32_t>
{
  const std::vector<Token>& toks = *currentFile->tokens;
  const Token& t = toks[std::min<size_t>(token, toks.size() - 1)];
  uint32_t line = 1, lineStart = 0;
  for (uint32_t i = 0; i < t.offset && i < currentFile->text.size(); ++i)
    if (currentFile->text[i] == '\n')
    {
      ++line;
      lineStart = i + 1;
    }
  return {line, t.offset - lineStart + 1};
}

auto codegen::Emitter::sourceText(uint32_t fromToken, uint32_t toToken) -> std::string
{
  const std::vector<Token>& toks = *currentFile->tokens;
  if (fromToken >= toks.size() || toToken <= fromToken)
    return "";
  const Token& first = toks[fromToken];
  const Token& last = toks[std::min<size_t>(toToken - 1, toks.size() - 1)];
  return std::string(currentFile->text.substr(first.offset, last.offset + last.length - first.offset));
}

auto codegen::Emitter::callerLocation(const Ty* pt, Node* at) -> std::string
{
  const Ty* t = types.stripRef(pt);
  if (!t || !isClass(t))
    unsupported(at, "a caller location parameter whose type is not a class");
  StructInfo& info = structFor(t, at);
  if (info.fieldTypes.size() < 4)
    unsupported(at, "a caller location class without its four fields (file, line, column, function)");
  std::string addr = newObjectSlot(t, at);
  uint32_t token = at->kind == NodeKind::CallExpr ? static_cast<CallExpr*>(at)->callee->token : at->token; // the callee's name
  auto [line, column] = lineColumnOf(token);
  // In a lambda written in a function body: that function, not whichever function emitted the
  // lambda first (which varies with the threads' order).
  Decl* writtenIn = lambdaContext ? lambdaContext->parent : nullptr;
  FunctionDecl* named = writtenIn && writtenIn->kind == NodeKind::FunctionDecl ? static_cast<FunctionDecl*>(writtenIn) : currentFunction;
  std::string function = named ? qualifiedName(named) : std::string();
  fn += "  store ptr " + cStringConstant(programPathOf(currentFile)) + ", ptr " + gep(info.name, addr, 0) + "\n";
  fn += "  store i32 " + std::to_string(line) + ", ptr " + gep(info.name, addr, 1) + "\n";
  fn += "  store i32 " + std::to_string(column) + ", ptr " + gep(info.name, addr, 2) + "\n";
  fn += "  store ptr " + cStringConstant(function) + ", ptr " + gep(info.name, addr, 3) + "\n";
  return addr;
}

auto codegen::Emitter::conditionHolds(FunctionDecl* f) -> bool
{
  if (!f->condition)
    return true;
  ParsedFile* saved = currentFile;
  currentFile = f->file;
  ConstContextScope context(*this, f);
  std::optional<int64_t> v = evalConst(f->condition);
  currentFile = saved;
  if (!v)
    unsupported(f, "a conditional(...) whose condition is not a compile-time constant");
  return *v != 0;
}

auto codegen::Emitter::attributeText(Decl* d) -> std::string
{
  std::string s;
  if (d->attributes & ATTR_NoInline)
    s += "noinline ";
  if (d->attributes & ATTR_ForceInline)
    s += "alwaysinline ";
  return s;
}

auto codegen::Emitter::addDefinition(const std::string& name, const std::string& text, bool ownItself, bool counted) -> void
{
  if (moduleCount <= 1)
  {
    body += text;
    std::lock_guard lock(sh.definitionsMutex);
    definedIn[name] = 0; // (what generateLLVM's check of the functions used without a definition reads)
    return;
  }
  std::string declare = declareFromHeader(text);
  Shared::Definition definition = Shared::definitionOf(text);
  definition.name = name;
  definition.owner = ownItself || owner.empty() ? name : owner;
  definition.counted = counted;
  bool onDemand = !onDemandNow.name.empty() && name == onDemandNow.name;
  if (onDemand)
  {
    definition.closure = onDemandNow.closure;
    definition.callType = onDemandNow.callType;
  }
  if (!ownItself && (name == owner || onDemand) && !layoutsNow.empty())
  {
    // The layouts it and its lambdas used (a call operator emitted on demand: what its
    // emission used, its writer's so far included; emitted again with its writer).
    std::sort(layoutsNow.begin(), layoutsNow.end());
    layoutsNow.erase(std::unique(layoutsNow.begin(), layoutsNow.end()), layoutsNow.end());
    definition.layouts = std::move(layoutsNow);
    layoutsNow.clear();
  }
  if (!ownItself && (name == owner || onDemand) && !inlinedNow.empty())
  {
    // The function's own definition, its emission done: what it and its lambdas inlined. (What
    // an operator emitted on demand inlined emits its writer again, and the operator with it.)
    std::sort(inlinedNow.begin(), inlinedNow.end());
    inlinedNow.erase(std::unique(inlinedNow.begin(), inlinedNow.end()), inlinedNow.end());
    definition.inlined = std::move(inlinedNow);
    inlinedNow.clear();
    definition.emitted = std::make_unique<Instantiation>(
      onDemand ? Instantiation{onDemandNow.writer, {}, nullptr, currentModule, "", "", 0} : emittingNow);
  }
  else if (!ownItself && name == owner && !onDemand && (!emittingNow.args.empty() || emittingNow.classTy || !definition.layouts.empty()))
    definition.emitted = std::make_unique<Instantiation>(emittingNow); // an instance, or a user of layouts: emitted again when they change
  else if (onDemand && !definition.layouts.empty())
    definition.emitted = std::make_unique<Instantiation>(Instantiation{onDemandNow.writer, {}, nullptr, currentModule, "", "", 0});
  std::lock_guard lock(sh.definitionsMutex);
  sh.definitions[currentModule].push_back(std::move(definition));
  definedIn[name] = currentModule;
  declareLines[name] = std::move(declare);
}

auto codegen::Emitter::forgetLayoutAnswers() -> void
{
  layoutCache.clear();
  sizeCache.clear();
  destructionCache.clear();
  destructionCallCounts.clear();
  throwingDestructionCache.clear();
  bitwiseRelocation.clear();
  bitwiseAssignment.clear();
  lvalueBitfield = {nullptr, nullptr}; // (a place in a layout that may go)
  droppedLayouts.clear();              // (nothing points into them once the answers above go)
}

auto codegen::Emitter::noteLayout(const Ty* classTy) -> void
{
  if (moduleCount > 1 && !owner.empty())
    layoutsNow.push_back(classTy);
}

auto codegen::Emitter::addOwnedEntry(const std::string& text) -> void
{
  if (moduleCount <= 1)
  {
    moduleDecls[currentModule] += text;
    return;
  }
  auto lock = sharedLock();
  sh.ownedEntries[currentModule].push_back({owner.empty() ? currentFunctionName : owner, text});
}

auto codegen::Emitter::sharedDeclarationOf(const std::string& header, std::string* name) -> std::string
{
  std::string declaration = declareFromHeader(header);
  if (declaration.starts_with("declare linkonce_odr "))
    declaration.erase(8, 13);
  if (name)
  {
    size_t at = header.find(" @");
    size_t end = header[at + 2] == '"' ? header.find('"', at + 3) + 1 : header.find('(', at);
    *name = header.substr(at + 1, end - at - 1);
  }
  return declaration;
}

auto codegen::Emitter::runtimeHeaders() -> std::vector<std::string>
{
  return {kppThrowHeader,     kppEhRethrowHeader,    kppEhPushHeader,       kppEhBeginHeader, kppEhEndHeader,
          kppEhFilterAnyHeader, kppEhFilterNoexceptHeader, kppEhUnhandledHeader, kppAssertFailedHeader, kppMissingHeader, baseSearchHeader("@kpp_eh_offset"),
          kppEhEnterHeader,   kppEhTerminateHeader, kppThreadAtExitHeader, kppAtExitHeader, kppCurrentExceptionHeader};
}

auto codegen::Emitter::declareFromHeader(const std::string& text) -> std::string
{
  size_t at = text.find(" @");
  size_t nameEnd = text[at + 2] == '"' ? text.find('"', at + 3) + 1 : text.find('(', at);
  size_t open = text.find('(', nameEnd);
  size_t close = text.find(") ", open);
  if (close == std::string::npos || text.find('{', open) < close)
    close = text.find(")", open);
  std::string params;
  size_t p = open + 1;
  while (p < close)
  {
    size_t comma = text.find(", ", p);
    if (comma == std::string::npos || comma > close)
      comma = close;
    std::string one = text.substr(p, comma - p);
    size_t pct = one.rfind(" %");
    if (pct != std::string::npos)
      one = one.substr(0, pct);
    params += (params.empty() ? "" : ", ") + one;
    p = comma + 2;
  }
  std::string head = text.substr(7, nameEnd - 7);
  if (head.starts_with("weak_odr "))
    head.erase(0, 9); // a C header's inline function (a declaration has no linkage)
  return "declare " + head + "(" + params + ")";
}

auto codegen::Emitter::stubDefinition(const std::string& name, const std::string& ret, const std::string& params) -> std::string
{
  auto it = sh.stubMessages.find(name);
  return stubText(name, ret, params, it != sh.stubMessages.end() ? it->second : name);
}

auto codegen::Emitter::finishStubRuntime() -> void
{
  if (!sh.stubMissing || !declaredNames.claim("@kpp_missing"))
    return;
  emittingShared = true;
  std::string format = cStringConstant("kppc left this function out (it did not compile; see the compile's errors): %s\n");
  emittingShared = false;
  if (declaredNames.claim("@fprintf"))
    decls += "declare i32 @fprintf(ptr, ptr, ...)\n";
  std::string err = stderrStream();
  if (declaredNames.claim("@fflush"))
    decls += "declare i32 @fflush(ptr)\n";
  if (declaredNames.claim("@abort"))
    decls += "declare void @abort()\n";
  shared += sharedComdat("@kpp_missing") +
            std::string(kppMissingHeader) + "\nentry:\n"
            "  %f = call i32 @fflush(ptr null)\n" + // (every stream first: the program's own output comes before the message)
            err + "  %r = call i32 (ptr, ptr, ...) @fprintf(ptr %err, ptr " + format + ", ptr %what)\n"
            "  %f2 = call i32 @fflush(ptr %err)\n  call void @abort()\n  unreachable\n}\n";
}

auto codegen::Emitter::declsFor(unsigned k, bool types, const std::unordered_set<std::string_view>* programNames) -> std::string
{
  std::string out;
  const std::string& decls = this->decls.text;
  size_t pos = 0;
  while (pos < decls.size())
  {
    size_t end = decls.find('\n', pos);
    if (end == std::string::npos)
      end = decls.size();
    std::string_view line(decls.data() + pos, end - pos);
    bool skip = !types && isTypeLine(line);
    if (line.rfind("declare ", 0) == 0)
    {
      size_t at = line.find('@');
      if (at != std::string_view::npos)
      {
        size_t nameEnd = line[at + 1] == '"' ? line.find('"', at + 2) + 1 : line.find('(', at);
        if (nameEnd != std::string_view::npos)
        {
          std::string name(line.substr(at, nameEnd - at));
          const unsigned* in = sh.moduleDefining(name);
          skip = in && *in == k && !(programNames && programNames->count(name));
          // --stub-missing: a function of the program used without a definition is defined
          // here as a stub (in every module: linkonce_odr), from its declaration.
          if (!skip && sh.stubMissing && sh.undefinedUses.count(name) && !definedIn.count(name))
          {
            out += stubDefinition(name, std::string(line.substr(8, at - 8)), std::string(line.substr(nameEnd)));
            skip = true;
          }
        }
      }
    }
    if (!skip)
    {
      out.append(line);
      out += '\n';
    }
    pos = end + 1;
  }
  return out;
}

auto codegen::Emitter::declaredIn(std::string_view text, std::unordered_set<std::string>& out) -> void
{
  for (size_t pos = 0; pos < text.size();)
  {
    size_t end = text.find('\n', pos);
    if (end == std::string_view::npos)
      end = text.size();
    std::string_view line = text.substr(pos, end - pos);
    if (line.rfind("declare ", 0) == 0)
      if (size_t at = line.find('@'); at != std::string_view::npos)
      {
        size_t nameEnd = line[at + 1] == '"' ? line.find('"', at + 2) + 1 : line.find('(', at);
        if (nameEnd != std::string_view::npos && nameEnd > at)
          out.insert(std::string(line.substr(at, nameEnd - at)));
      }
    pos = end + 1;
  }
}

auto codegen::Emitter::typeLengthIn(std::string_view text) -> size_t
{
  if (text.empty())
    return 0;
  if (text[0] != '[' && text[0] != '{' && text[0] != '<')
    return std::min(text.find(' '), text.size());
  int open = 0;
  for (size_t i = 0; i < text.size(); ++i)
  {
    char c = text[i];
    if (c == '[' || c == '{' || c == '<')
      ++open;
    else if (c == ']' || c == '}' || c == '>')
      if (--open == 0)
        return i + 1;
  }
  return text.size();
}

auto codegen::Emitter::externalDeclarationOf(std::string_view line, std::string_view& name) -> std::string
{
  static constexpr std::string_view mark = " = linkonce_odr ";
  if (size_t debug = line.find(", !dbg !G"); debug != std::string_view::npos)
    line = line.substr(0, debug); // (--debug-info's attachment is the definition's: Emitter::debugGlobal)
  size_t eq = line.find(mark);
  if (eq == std::string_view::npos || line.empty() || line[0] != '@' || line.find(", comdat") == std::string_view::npos)
    return {};
  name = line.substr(0, eq);
  std::string_view rest = line.substr(eq + mark.size());
  if (rest.starts_with("unnamed_addr "))
    rest.remove_prefix(13);
  bool tls = rest.starts_with("thread_local ");
  if (tls)
    rest.remove_prefix(13);
  std::string_view kind = rest.starts_with("global ") ? "global" : rest.starts_with("constant ") ? "constant" : "";
  if (kind.empty())
    return {};
  rest.remove_prefix(kind.size() + 1);
  size_t typeLength = typeLengthIn(rest);
  std::string declaration = std::string(name) + " = external " + (tls ? "thread_local " : "") + std::string(kind) + " " + std::string(rest.substr(0, typeLength));
  if (size_t align = line.rfind(", align "); align != std::string_view::npos && align > eq &&
      line.find_first_not_of("0123456789", align + 8) == std::string_view::npos)
    declaration += line.substr(align);
  return declaration;
}

auto codegen::Emitter::homeOf(std::string_view name, unsigned modules) -> unsigned
{
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : name)
    h = (h ^ c) * 1099511628211ull;
  return unsigned(h % modules);
}

auto codegen::Emitter::definedBy(std::string_view line) -> std::string_view
{
  size_t at = line.starts_with("declare ") || line.starts_with("define ") ? line.find('@') : line.starts_with("@") || line.starts_with("$") ? 0 : std::string_view::npos;
  if (at == std::string_view::npos || at + 1 >= line.size())
    return {};
  size_t end = line[at + 1] == '"' ? line.find('"', at + 2) + 1 : line.find_first_of(" (", at + 1);
  return end == std::string_view::npos || end <= at + 1 ? std::string_view() : line.substr(at + 1, end - at - 1);
}

auto codegen::Emitter::localLine(std::string_view line) -> bool
{
  return (line[0] == '@' && (line.find(" = private ") != std::string_view::npos || line.find(" = internal ") != std::string_view::npos)) ||
         line.starts_with("define private ") || line.starts_with("define internal ");
}

auto codegen::Emitter::prepareModuleTexts() -> void
{
  declaredIn(decls.text, sharedDeclared);
  // The program functions the shared declarations name (a function's address in a shared global's
  // constant data), which every module but the defining one declares (moduleText): a kpp function's
  // quoted name and a C function's bare one alike. Until 2026-11-05 only the quoted names were
  // looked for, so `@"regs" = weak_odr global [...] [... ptr @specAddOne ...]` named an extern "C"
  // function no other module declared ("use of undefined value '@specAddOne'", thread D's find).
  const std::string& shared = decls.text;
  irNamesIn(shared, [&](size_t at, size_t length)
  {
    if (length < 2 || shared[at + 1] == '.')
      return; // (`@.str`, `@.data.`: the constants, never a function)
    std::string name = shared.substr(at, length);
    if (definedIn.count(name) && !sharedDeclared.count(name))
      sh.sharedReferences.insert(name);
  });
  std::string_view text = decls.text;
  for (size_t pos = 0; pos < text.size();)
  {
    size_t end = text.find('\n', pos);
    if (end == std::string_view::npos)
      end = text.size();
    std::string_view line = text.substr(pos, end - pos);
    if (isTypeLine(line))
    {
      sh.typeIndex.emplace(line.substr(0, line.find(" = type ")), uint32_t(sh.typeLines.size()));
      sh.typeLines.push_back({line, {}});
    }
    pos = end + 1;
  }
  for (Shared::TypeLine& t : sh.typeLines)
    typeNamesIn(t.line.substr(t.line.find(" = type ")), [&](std::string_view name)
    {
      if (auto it = sh.typeIndex.find(name); it != sh.typeIndex.end())
        t.uses.push_back(it->second);
    });
  // A debug build's homed globals (Shared::homeGlobals), with debug info too: its line tables
  // (DICompileUnit's LineTablesOnly) put no metadata on a global's line. (A release build's ThinLTO
  // imports constants into the modules that read them: not changed here.)
  sh.homeGlobals = moduleCount > 1 && sh.importableModules == 0;
  // (Again after a server's layout edit rewrote type lines in decls: the views below are into it.)
  sh.homedGlobals.clear();
  sh.homedComdats.clear();
  sh.namedByShared.clear();
  sh.homeNames.clear();
  if (sh.fullDebugInfo)
    homeDebugClasses();
  if (!sh.homeGlobals)
    return;
  sh.homeNames.assign(moduleCount, {});
  for (size_t pos = 0; pos < text.size();)
  {
    size_t end = text.find('\n', pos);
    if (end == std::string_view::npos)
      end = text.size();
    std::string_view line = text.substr(pos, end - pos);
    pos = end + 1;
    std::string_view name;
    if (std::string declaration = externalDeclarationOf(line, name); !declaration.empty())
    {
      unsigned home = homeOf(name, moduleCount);
      sh.homedGlobals.emplace(name, Shared::HomedGlobal{home, std::move(declaration)});
      sh.homedComdats.emplace(name.substr(1), home); // (its `$` line names it without the `@`)
      std::string_view initializer = line.substr(name.size());
      irNamesIn(initializer, [&](size_t at, size_t length) { sh.homeNames[home].push_back(initializer.substr(at, length)); });
    }
    else if (!line.empty() && line[0] != '$' && line[0] != '%')
      irNamesIn(line, [&](size_t at, size_t length) { sh.namedByShared.push_back(line.substr(at, length)); });
  }
}

auto codegen::Emitter::homedDeclsFor(unsigned k, const std::string& decls, const std::unordered_set<std::string>& seen) -> std::string
{
  std::unordered_set<std::string_view> needed;
  auto need = [&](std::string_view name)
  {
    if (auto it = sh.homedGlobals.find(name); it != sh.homedGlobals.end() && it->second.home != k)
      needed.insert(it->first);
  };
  for (const std::string& name : seen)
    need(name);
  for (std::string_view name : sh.homeNames[k])
    need(name);
  for (std::string_view name : sh.namedByShared)
    need(name);
  std::string out;
  out.reserve(decls.size());
  for (size_t pos = 0; pos < decls.size();)
  {
    size_t end = decls.find('\n', pos);
    if (end == std::string::npos)
      end = decls.size();
    std::string_view line(decls.data() + pos, end - pos);
    pos = end + 1;
    if (!line.empty() && line[0] == '$')
    {
      // `$name = comdat any`: the homed global's comdat only where it is defined
      size_t eq = line.find(" = comdat ");
      if (eq != std::string_view::npos)
        if (auto it = sh.homedComdats.find(line.substr(1, eq - 1)); it != sh.homedComdats.end() && it->second != k)
          continue;
    }
    else if (!line.empty() && line[0] == '@')
    {
      size_t eq = line.find(" = ");
      if (eq != std::string_view::npos)
        if (auto it = sh.homedGlobals.find(line.substr(0, eq)); it != sh.homedGlobals.end())
        {
          if (it->second.home == k)
          {
            // The one definition: weak_odr, not linkonce_odr, which an optimizer drops where its
            // own module does not read it (a debug build with --optimize: the other modules'
            // declarations were then left undefined), as the destructor helpers' homes are.
            static constexpr std::string_view linkonce = " = linkonce_odr ";
            out.append(line.substr(0, eq));
            out += " = weak_odr ";
            out.append(line.substr(eq + linkonce.size()));
          }
          else if (needed.count(it->first))
            out += it->second.declaration;
          else
            continue;
          out += '\n';
          continue;
        }
    }
    out.append(line);
    out += '\n';
  }
  return out;
}

auto codegen::Emitter::placeShortDefinitions(unsigned threads) -> void
{
  for (unsigned k = 0; k < moduleCount; ++k)
  {
    sh.placedHere[k].clear();
    sh.placedDecls[k].clear();
    sh.movedOut[k].clear();
  }
  sh.placedIn.clear();
  sh.placedAway.clear();
  unsigned importable = sh.importableModules;
  if (!importable)
    return;
  // f(k) for every module, on the threads (what each module's part writes is its own; the parts
  // are joined in module order after).
  auto forModules = [&](auto&& f)
  {
    std::atomic<unsigned> next{0};
    auto run = [&]()
    {
      for (unsigned k; (k = next++) < moduleCount;)
        f(k);
    };
    std::vector<std::thread> pool;
    for (unsigned i = 1; i < std::min(std::max(threads, 1u), moduleCount); ++i)
      pool.emplace_back(run);
    run();
    for (std::thread& t : pool)
      t.join();
  };
  // Every module's declarations by the name each line is for (a global and its comdat).
  std::vector<std::unordered_map<std::string_view, std::vector<std::string_view>>> linesFor(moduleCount);
  forModules([&](unsigned k)
  {
    forEachLine(moduleDecls[k].text, [&](std::string_view line)
    {
      if (std::string_view name = definedBy(line); !name.empty())
        linesFor[k][name].push_back(line);
    });
  });
  // The units that may move: per module after the importable ones, its short definitions
  // joined with the local names they share (each unit moves whole or not at all).
  struct Unit
  {
    unsigned module;
    std::vector<uint32_t> definitions;           // indices in the module's definitions
    std::vector<std::string_view> lines, locals; // what they name from the module's declarations: copied, moved
  };
  std::vector<std::vector<Unit>> unitsIn(moduleCount); // per module, then all of them in module order
  std::vector<std::vector<std::pair<std::string_view, uint32_t>>> unitNamesIn(moduleCount);
  std::vector<std::vector<std::string_view>> definedName(moduleCount);
  forModules([&](unsigned k)
  {
    const std::vector<Shared::Definition>& defs = sh.definitions[k];
    definedName[k].resize(defs.size());
    for (size_t i = 0; i < defs.size(); ++i)
      definedName[k][i] = definedBy(std::string_view(defs[i].text).substr(0, defs[i].text.find('\n')));
    if (k < importable)
      return;
    // A local name moves with every definition that names it, and the local names its line
    // names with it: the definitions and local names joined that way move together or stay
    // together. A group stays when one of its definitions is long, or a line that stays (one
    // not local) names one of its local names.
    std::unordered_map<std::string_view, uint32_t> localNode;
    for (auto& [name, lines] : linesFor[k])
      for (std::string_view line : lines)
        if (localLine(line))
          localNode.emplace(name, 0);
    for (size_t i = 0; i < defs.size(); ++i)
      if (!definedName[k][i].empty() && localLine(std::string_view(defs[i].text).substr(0, defs[i].text.find('\n'))))
        localNode.emplace(definedName[k][i], 0);
    uint32_t nodes = uint32_t(defs.size());
    for (auto& [name, node] : localNode)
      node = nodes++;
    std::vector<uint32_t> parent(nodes);
    std::iota(parent.begin(), parent.end(), 0u);
    auto find = [&](uint32_t x)
    {
      while (parent[x] != x)
        x = parent[x] = parent[parent[x]];
      return x;
    };
    auto join = [&](uint32_t a, uint32_t b) { parent[find(a)] = find(b); };
    std::vector<char> stays(nodes, 0);
    for (size_t i = 0; i < defs.size(); ++i)
    {
      if (auto own = localNode.find(definedName[k][i]); own != localNode.end())
        join(uint32_t(i), own->second);
      for (auto [at, length] : defs[i].names)
        if (auto it = localNode.find(std::string_view(defs[i].text).substr(at + 1, length - 1)); it != localNode.end())
          join(uint32_t(i), it->second);
    }
    for (auto& [name, lines] : linesFor[k])
      for (std::string_view line : lines)
      {
        auto self = localNode.find(name);
        bool isLocal = self != localNode.end() && localLine(line);
        irNamesIn(line, [&](size_t at, size_t length)
        {
          if (auto it = localNode.find(line.substr(at + 1, length - 1)); it != localNode.end())
          {
            if (isLocal)
              join(self->second, it->second);
            else
              stays[it->second] = 1;
          }
        });
      }
    for (uint32_t x = 0; x < nodes; ++x)
      if (stays[x])
        stays[find(x)] = 1;
    for (size_t i = 0; i < defs.size(); ++i)
      if (definedName[k][i].empty() || size_t(std::count(defs[i].text.begin(), defs[i].text.end(), '\n')) > shortLines)
        stays[find(uint32_t(i))] = 1;
    // What each definition names from the module's declarations, and what those name in turn:
    // copied with it, or (a local name of its group) moved with it. A local name of another
    // group reached through a copied line keeps the definition's group where it is.
    std::vector<std::vector<std::string_view>> lines(defs.size()), locals(defs.size());
    for (size_t i = 0; i < defs.size(); ++i)
    {
      uint32_t group = find(uint32_t(i));
      if (stays[group])
        continue;
      std::vector<std::string_view> work;
      std::unordered_set<std::string_view> seen;
      for (auto [at, length] : defs[i].names)
        work.push_back(std::string_view(defs[i].text).substr(at + 1, length - 1));
      while (!work.empty())
      {
        std::string_view n = work.back();
        work.pop_back();
        if (!seen.insert(n).second)
          continue;
        if (auto it = localNode.find(n); it != localNode.end() && find(it->second) != group)
        {
          stays[group] = 1;
          break;
        }
        if (auto it = linesFor[k].find(n); it != linesFor[k].end())
          for (std::string_view line : it->second)
          {
            (localLine(line) ? locals[i] : lines[i]).push_back(line);
            irNamesIn(line, [&](size_t at, size_t length) { work.push_back(line.substr(at + 1, length - 1)); });
          }
      }
    }
    std::unordered_map<uint32_t, uint32_t> unitOfGroup;
    for (size_t i = 0; i < defs.size(); ++i)
    {
      uint32_t group = find(uint32_t(i));
      if (stays[group])
        continue;
      auto [it, added] = unitOfGroup.emplace(group, uint32_t(unitsIn[k].size()));
      if (added)
        unitsIn[k].push_back({k, {}, {}, {}});
      Unit& u = unitsIn[k][it->second];
      u.definitions.push_back(uint32_t(i));
      u.lines.insert(u.lines.end(), lines[i].begin(), lines[i].end());
      u.locals.insert(u.locals.end(), locals[i].begin(), locals[i].end());
      unitNamesIn[k].push_back({definedName[k][i], it->second});
    }
  });
  std::vector<Unit> units;
  std::unordered_map<std::string_view, uint32_t> unitOf; // a unit's definition's name -> the unit
  for (unsigned k = 0; k < moduleCount; ++k)
  {
    uint32_t first = uint32_t(units.size());
    for (Unit& u : unitsIn[k])
      units.push_back(std::move(u));
    for (auto [name, local] : unitNamesIn[k])
      unitOf.emplace(name, first + local);
  }
  // The modules each unit's definitions are called from: those of the definitions that stay
  // (or the module's declarations' thunks) naming one directly, and those a unit naming one is
  // called from. A bit per module.
  size_t words = (moduleCount + 63) / 64;
  std::vector<uint64_t> callers(units.size() * words, 0);
  std::vector<std::vector<uint32_t>> callees(units.size());
  std::vector<std::vector<uint32_t>> calledFrom(moduleCount);                 // per module: the units it calls
  std::vector<std::vector<std::pair<uint32_t, uint32_t>>> unitCalls(moduleCount); // per module: unit -> unit it calls
  forModules([&](unsigned k)
  {
    const std::vector<Shared::Definition>& defs = sh.definitions[k];
    for (size_t i = 0; i < defs.size(); ++i)
    {
      auto self = unitOf.find(definedName[k][i]);
      for (auto [at, length] : defs[i].names)
        if (auto it = unitOf.find(std::string_view(defs[i].text).substr(at + 1, length - 1)); it != unitOf.end())
        {
          if (self == unitOf.end())
            calledFrom[k].push_back(it->second);
          else if (self->second != it->second)
            unitCalls[k].push_back({self->second, it->second});
        }
    }
    // (a thunk calls; a vtable or another constant names: ThinLTO imports a read-only constant and
    // then the functions it names)
    for (auto& [name, lines] : linesFor[k])
      for (std::string_view line : lines)
        irNamesIn(line, [&](size_t at, size_t length)
        {
          if (auto it = unitOf.find(line.substr(at + 1, length - 1)); it != unitOf.end() && it->first != name)
            calledFrom[k].push_back(it->second);
        });
  });
  for (unsigned k = 0; k < moduleCount; ++k)
  {
    for (uint32_t u : calledFrom[k])
      callers[u * words + k / 64] |= uint64_t(1) << (k % 64);
    for (auto [from, to] : unitCalls[k])
      callees[from].push_back(to);
  }
  // What a unit's lines name (a vtable it takes with it) is called from wherever the unit is.
  for (uint32_t u = 0; u < units.size(); ++u)
    for (std::string_view line : units[u].lines)
      irNamesIn(line, [&](size_t at, size_t length)
      {
        if (auto it = unitOf.find(line.substr(at + 1, length - 1)); it != unitOf.end() && it->second != u)
          callees[u].push_back(it->second);
      });
  for (std::vector<uint32_t>& c : callees)
  {
    std::sort(c.begin(), c.end());
    c.erase(std::unique(c.begin(), c.end()), c.end());
  }
  std::vector<uint32_t> work;
  std::vector<char> queued(units.size(), 0);
  auto propagate = [&]()
  {
    while (!work.empty())
    {
      uint32_t u = work.back();
      work.pop_back();
      queued[u] = 0;
      for (uint32_t c : callees[u])
      {
        bool grew = false;
        for (size_t w = 0; w < words; ++w)
        {
          uint64_t now = callers[c * words + w] | callers[u * words + w];
          grew = grew || now != callers[c * words + w];
          callers[c * words + w] = now;
        }
        if (grew && !queued[c])
        {
          queued[c] = 1;
          work.push_back(c);
        }
      }
    }
  };
  for (uint32_t u = 0; u < units.size(); ++u)
  {
    work.push_back(u);
    queued[u] = 1;
  }
  propagate();
  // A unit nothing calls (a virtual function, one whose address is taken) stays where it was
  // emitted, and calls from there.
  for (uint32_t u = 0; u < units.size(); ++u)
    if (std::all_of(&callers[u * words], &callers[u * words] + words, [](uint64_t w) { return w == 0; }))
    {
      callers[u * words + units[u].module / 64] |= uint64_t(1) << (units[u].module % 64);
      work.push_back(u);
      queued[u] = 1;
    }
  propagate();
  // Where each unit goes, then what it takes with it.
  std::vector<std::unordered_set<std::string_view>> own(moduleCount); // what each module's declarations define or declare
  forModules([&](unsigned k)
  {
    for (auto& [name, lines] : linesFor[k])
      own[k].insert(name);
  });
  std::vector<std::map<std::string, std::string>> needed(moduleCount); // per module: sigil and name -> the line placed in it
  for (const Unit& u : units)
  {
    const uint64_t* bits = &callers[(&u - units.data()) * words];
    unsigned count = 0, only = 0;
    for (size_t w = 0; w < words; ++w)
    {
      count += unsigned(std::popcount(bits[w]));
      if (bits[w])
        only = unsigned(w * 64 + std::countr_zero(bits[w]));
    }
    // (several: by its name, not by who calls it, as the importable modules are imported by
    // nearly every module either way, there being thousands of different sets of callers; and
    // a function does not move between them when a caller is added)
    unsigned to = count == 0 ? u.module
                  : count == 1 ? only
                               : unsigned(std::hash<std::string_view>()(definedName[u.module][u.definitions.front()]) % importable);
    if (to == u.module)
      continue;
    bool clash = false; // (a local name the module it would go to has a line of its own for)
    for (std::string_view line : u.locals)
      clash = clash || own[to].count(definedBy(line));
    if (clash)
      continue;
    for (uint32_t i : u.definitions)
    {
      const Shared::Definition& d = sh.definitions[u.module][i];
      sh.placedHere[to].push_back(&d);
      sh.placedAway.insert(&d);
      sh.placedIn["@" + std::string(definedName[u.module][i])] = to;
    }
    for (std::string_view line : u.locals)
    {
      needed[to][std::string(line.substr(0, 1)) + std::string(definedBy(line))] = std::string(line) + "\n";
      sh.movedOut[u.module].push_back(line); // (its module has it no longer)
    }
    for (std::string_view line : u.lines)
      if (!own[to].count(definedBy(line)))
      {
        std::string& text = needed[to][std::string(line.substr(0, 1)) + std::string(definedBy(line))];
        if (text.empty())
          text = std::string(line) + "\n";
      }
  }
  for (unsigned k = 0; k < moduleCount; ++k)
  {
    std::sort(sh.movedOut[k].begin(), sh.movedOut[k].end(), [](std::string_view a, std::string_view b) { return a.data() < b.data(); });
    sh.movedOut[k].erase(std::unique(sh.movedOut[k].begin(), sh.movedOut[k].end(),
                                     [](std::string_view a, std::string_view b) { return a.data() == b.data(); }),
                         sh.movedOut[k].end());
    for (auto& [key, text] : needed[k])
    {
      // (not a declaration of what is now defined here)
      const unsigned* in = sh.moduleDefining("@" + key.substr(1));
      if (text.starts_with("declare ") && in && *in == k)
        continue;
      sh.placedDecls[k] += text;
    }
  }
}

auto codegen::Emitter::moduleText(unsigned k, const std::string& prefix, bool instancePiece, std::string* errors) -> std::string
{
  std::unordered_set<std::string_view> programNames, programPrivate; // (an instance piece's: what its program piece defines)
  if (instancePiece)
  {
    for (const Shared::Definition& d : sh.definitions[k])
      if (sh.programOwned(d.owner))
        programNames.insert(d.name);
    for (const Shared::OwnedEntry& o : sh.ownedEntries[k])
      if (sh.programOwned(o.owner))
        forEachLine(o.text, [&](std::string_view line)
        {
          if (!definedBy(line).empty() && localLine(line))
            programPrivate.insert(line.substr(0, line.find(" = ")));
        });
  }
  std::string ctors = ctorsText(k); // module 0 runs the startup functions of every module
  std::string decls = declsFor(k, false, instancePiece ? &programNames : nullptr);
  // The module's own declarations, without the lines placed in another module.
  std::string_view ownDecls = moduleDecls[k].text;
  std::string kept;
  if (!sh.movedOut[k].empty())
  {
    const std::vector<std::string_view>& cut = sh.movedOut[k]; // (sorted by where they are)
    const char* at = ownDecls.data();
    const char* end = ownDecls.data() + ownDecls.size();
    for (std::string_view line : cut)
    {
      kept.append(at, line.data());
      at = std::min(end, line.data() + line.size() + 1);
    }
    kept.append(at, end);
    ownDecls = kept;
  }
  // An instance piece: only the module's own declarations its texts reach (its definitions, the
  // shared definitions and declarations it carries, the startup list), closed over what those name;
  // the program piece has copies of those its own definitions name. Each unit (a line, a thunk's
  // definition) where it was.
  std::string reached;
  if (instancePiece)
  {
    std::unordered_map<std::string_view, std::vector<std::string_view>> units;
    forEachLine(ownDecls, [&](std::string_view unit)
    {
      if (std::string_view name = definedBy(unit); !name.empty())
        units[name].push_back(unit);
    });
    std::vector<std::string_view> work; // names, without the sigil
    auto uses = [&](std::string_view text) { irNamesIn(text, [&](size_t at, size_t length) { work.push_back(text.substr(at + 1, length - 1)); }); };
    for (const Shared::Definition& d : sh.definitions[k])
      if (!sh.placedAway.count(&d) && !sh.programOwned(d.owner))
        for (auto [at, length] : d.names)
          work.push_back(std::string_view(d.text).substr(at + 1, length - 1));
    uses(shared.text);
    uses(ctors);
    if (!sh.homeNames.empty())
      for (std::string_view name : sh.homeNames[k])
        work.push_back(name.substr(1));
    for (std::string_view name : sh.namedByShared)
      work.push_back(name.substr(1));
    std::unordered_set<std::string_view> seen;
    std::unordered_set<const char*> keep; // the units reached, by where they start
    while (!work.empty())
    {
      std::string_view name = work.back();
      work.pop_back();
      if (!seen.insert(name).second)
        continue;
      if (auto it = units.find(name); it != units.end())
        for (std::string_view unit : it->second)
          if (keep.insert(unit.data()).second)
            uses(unit);
    }
    reached.reserve(ownDecls.size());
    forEachLine(ownDecls, [&](std::string_view unit)
    {
      if (keep.count(unit.data()))
      {
        reached.append(unit);
        reached += '\n';
      }
    });
    ownDecls = reached;
  }
  std::unordered_set<std::string> already; // declarations the module carries (shared, or its own)
  declaredIn(ownDecls, already);
  declaredIn(sh.placedDecls[k], already);
  std::string declares;
  std::unordered_set<std::string> seen;
  auto declare = [&](const std::string& name)
  {
    auto line = declareLines.find(name);
    if (line != declareLines.end())
      declares += line->second + "\n";
  };
  // The names the module's texts use, in the order they appear (its declarations, its
  // definitions as sorted, the shared definitions, the startup list): each function's names were
  // found when it was emitted; the other texts are scanned here.
  auto consider = [&](std::string_view nameView)
  {
    std::string name(nameView);
    if (!seen.insert(name).second)
      return;
    const unsigned* in = sh.moduleDefining(name);
    if (in && (*in != k || programNames.count(name)) && !sharedDeclared.count(name) && !already.count(name))
      declare(name); // not when a declaration is in the module's text already
    if (errors && programPrivate.count(name))
      *errors += "codegen: module " + std::to_string(k) + "'s instance piece names " + name + ", private to its program piece (step 4's pieces)\n";
  };
  auto scan = [&](std::string_view text) { irNamesIn(text, [&](size_t at, size_t length) { consider(text.substr(at, length)); }); };
  // Its definitions: those emitted in it and not placed in another, then those placed in it.
  std::vector<const Shared::Definition*> definitions;
  for (const Shared::Definition& d : sh.definitions[k])
    if (!sh.placedAway.count(&d) && !(instancePiece && sh.programOwned(d.owner)))
      definitions.push_back(&d);
  definitions.insert(definitions.end(), sh.placedHere[k].begin(), sh.placedHere[k].end());
  scan(ownDecls);
  scan(sh.placedDecls[k]);
  for (const Shared::Definition* d : definitions)
    for (auto [at, length] : d->names)
      consider(std::string_view(d->text).substr(at, length));
  scan(shared.text);
  scan(ctors);
  // The functions the shared declarations name: every module but the defining one declares them.
  for (const std::string& name : sh.sharedReferences)
    if (const unsigned* in = sh.moduleDefining(name); in && (*in != k || programNames.count(name)) && !already.count(name) && seen.insert(name).second)
      declare(name);
  if (sh.homeGlobals)
    decls = homedDeclsFor(k, decls, seen);
  // The type definitions the module names, and those they name in turn (every module had all of
  // the game's 24 k), in the shared declarations' order.
  std::vector<char> used(sh.typeLines.size(), 0);
  std::vector<uint32_t> work;
  auto mark = [&](uint32_t t)
  {
    if (!used[t])
    {
      used[t] = 1;
      work.push_back(t);
    }
  };
  auto markName = [&](std::string_view name)
  {
    if (auto it = sh.typeIndex.find(name); it != sh.typeIndex.end())
      mark(it->second);
  };
  typeNamesIn(ownDecls, markName);
  typeNamesIn(sh.placedDecls[k], markName);
  for (const Shared::Definition* d : definitions)
    for (auto [at, length] : d->types)
      markName(std::string_view(d->text).substr(at, length));
  typeNamesIn(shared.text, markName);
  typeNamesIn(ctors, markName);
  typeNamesIn(decls, markName);
  typeNamesIn(declares, markName);
  while (!work.empty())
  {
    uint32_t t = work.back();
    work.pop_back();
    for (uint32_t u : sh.typeLines[t].uses)
      mark(u);
  }
  std::string types;
  for (size_t t = 0; t < sh.typeLines.size(); ++t)
    if (used[t])
    {
      types.append(sh.typeLines[t].line);
      types += '\n';
    }
  size_t size = prefix.size() + types.size() + decls.size() + ownDecls.size() + sh.placedDecls[k].size() + ctors.size() + 1 +
                shared.text.size() + declares.size() + 1;
  for (const Shared::Definition* d : definitions)
    size += d->text.size();
  std::string out;
  out.reserve(size);
  out += prefix;
  out += types;
  out += decls;
  out += ownDecls;
  out += sh.placedDecls[k];
  out += ctors;
  out += '\n';
  out += shared.text;
  out += declares;
  out += '\n';
  uint32_t nextDebugId = debugFirstId;
  size_t definitionsAt = out.size();
  for (const Shared::Definition* d : definitions)
  {
    size_t at = out.size();
    out += d->text;
    if (sh.debugInfo)
      nextDebugId += renumberDebugIds(out.data() + at, d->text.size(), nextDebugId);
  }
  DebugLists lists;
  if (sh.fullDebugInfo)
    lists = appendDebugTypes(sh, out, definitionsAt, nextDebugId, (!sh.splitPieces || instancePiece) && k < sh.debugHomes.size() ? &sh.debugHomes[k] : nullptr);
  if (sh.debugInfo)
    out += debugModuleTailFor(sh.target, sh.fullDebugInfo, lists);
  return out;
}

auto codegen::Emitter::stderrStream() -> std::string
{
  if (sh.target.windows())
  {
    if (declaredNames.claim("@__acrt_iob_func"))
      decls += "declare ptr @__acrt_iob_func(i32)\n";
    return "  %err = call ptr @__acrt_iob_func(i32 2)\n";
  }
  // (quoted, as a C variable the program declares is named: one declaration for both)
  std::string stream = sh.target.os == Target::Os::FreeBSD ? "@\"__stderrp\"" : "@\"stderr\"";
  if (declaredNames.claim(stream))
    decls += stream + " = external global ptr\n";
  return "  %err = load ptr, ptr " + stream + "\n";
}

auto codegen::Emitter::staticExits(const std::string& addr, const Ty* t, Node* at) -> std::vector<std::pair<std::string, std::string>>
{
  std::vector<std::pair<std::string, const Ty*>> objects;
  if (needsDestruction(t, at))
    objects.push_back({addr, t});
  for (size_t i = staticTemporaries.size(); i-- > 0;)
    objects.push_back(staticTemporaries[i]); // (built before the variable: destroyed after it)
  staticTemporaries.clear();
  // Registered in reverse, so that the variable, registered last, is destroyed first.
  std::vector<std::pair<std::string, std::string>> exits;
  for (size_t i = objects.size(); i-- > 0;)
    if (std::string dtor = destructorThunk(objects[i].second, at); dtor != "null")
      exits.push_back({dtor, objects[i].first});
  return exits;
}

auto codegen::Emitter::registerThreadExits(const std::string& addr, const Ty* t, Node* at) -> void
{
  for (const auto& [dtor, object] : staticExits(addr, t, at))
  {
    usesThreadExit = true;
    fn += "  call void @kpp_thread_atexit(ptr " + dtor + ", ptr " + object + ")" + bundle() + "\n";
  }
}

auto codegen::Emitter::registerProgramExits(const std::string& addr, const Ty* t, Node* at) -> void
{
  for (const auto& [dtor, object] : staticExits(addr, t, at))
  {
    usesProgramExit = true;
    fn += "  call void @kpp_atexit(ptr " + dtor + ", ptr " + object + ")" + bundle() + "\n";
  }
}

auto codegen::Emitter::finishProgramExitRuntime() -> void
{
  if (!usesProgramExit || !declaredNames.claim("@kpp_atexit"))
    return;
  emittingShared = true;
  if (!sh.target.windows())
  {
    if (declaredNames.claim("@__cxa_atexit"))
      decls += "declare i32 @__cxa_atexit(ptr, ptr, ptr)\n";
    if (declaredNames.claim("@__dso_handle"))
      decls += "@__dso_handle = external hidden global i8\n";
    shared += sharedComdat("@kpp_atexit") + std::string(kppAtExitHeader) + "\nentry:\n  %r = call i32 @__cxa_atexit(ptr %dtor, ptr %object, ptr @__dso_handle)\n  ret void\n}\n";
    emittingShared = false;
    return;
  }
  ensureMalloc();
  if (declaredNames.claim("@atexit"))
    decls += "declare i32 @atexit(ptr)\n";
  shared += sharedComdat("@kpp.exit.list") + "@kpp.exit.list = linkonce_odr global ptr null, comdat\n";
  shared += sharedComdat("@kpp_exit_one") +
            "define linkonce_odr void @kpp_exit_one() comdat personality ptr " + personality() + " {\nentry:\n  br label %pop\n"
            "pop:\n  %node = load atomic ptr, ptr @kpp.exit.list acquire, align 8\n  %none = icmp eq ptr %node, null\n  br i1 %none, label %end, label %take\n"
            "take:\n  %rest = load ptr, ptr %node\n  %pair = cmpxchg ptr @kpp.exit.list, ptr %node, ptr %rest acq_rel acquire, align 8\n"
            "  %won = extractvalue { ptr, i1 } %pair, 1\n  br i1 %won, label %run, label %pop\n"
            "run:\n  %dp = getelementptr inbounds i8, ptr %node, i64 8\n  %dtor = load ptr, ptr %dp\n"
            "  %op = getelementptr inbounds i8, ptr %node, i64 16\n  %object = load ptr, ptr %op\n"
            "  call void @free(ptr %node)\n  invoke void %dtor(ptr %object)\n    to label %end unwind label %dispatch\n"
            "end:\n  ret void\n"
            // (a destructor that throws ends the program, as out of a noexcept function; the C library has no frame that would)
            "dispatch:\n  %cs = catchswitch within none [label %handler] unwind to caller\n"
            "handler:\n  %pad = catchpad within %cs [ptr @kpp.eh.filter.noexcept]\n  unreachable\n}\n";
  usesNoexceptFilter = true;
  shared += sharedComdat("@kpp_atexit") + std::string(kppAtExitHeader) +
            "\nentry:\n  %node = call ptr @malloc(i64 24)\n  %dp = getelementptr inbounds i8, ptr %node, i64 8\n  store ptr %dtor, ptr %dp\n"
            "  %op = getelementptr inbounds i8, ptr %node, i64 16\n  store ptr %object, ptr %op\n  br label %push\n"
            "push:\n  %head = load atomic ptr, ptr @kpp.exit.list acquire, align 8\n  store ptr %head, ptr %node\n"
            "  %pair = cmpxchg ptr @kpp.exit.list, ptr %head, ptr %node acq_rel acquire, align 8\n"
            "  %won = extractvalue { ptr, i1 } %pair, 1\n  br i1 %won, label %done, label %push\n"
            "done:\n  %r = call i32 @atexit(ptr @kpp_exit_one)\n  ret void\n}\n";
  emittingShared = false;
}

auto codegen::Emitter::finishThreadExitRuntime() -> void
{
  if (!usesThreadExit || !declaredNames.claim("@kpp_thread_atexit"))
    return;
  emittingShared = true;
  if (!sh.target.windows())
  {
    // The C library's: glibc's __cxa_thread_atexit_impl, FreeBSD's __cxa_thread_atexit, given the
    // program's __dso_handle.
    std::string impl = sh.target.os == Target::Os::FreeBSD ? "@__cxa_thread_atexit" : "@__cxa_thread_atexit_impl";
    if (declaredNames.claim(impl))
      decls += "declare i32 " + impl + "(ptr, ptr, ptr)\n";
    if (declaredNames.claim("@__dso_handle"))
      decls += "@__dso_handle = external hidden global i8\n"; // (crtbegin's: glibc finds the program's link map by it; null crashed a static program)
    shared += sharedComdat("@kpp_thread_atexit") + std::string(kppThreadAtExitHeader) + "\nentry:\n  %r = call i32 " + impl +
              "(ptr %dtor, ptr %object, ptr @__dso_handle)\n  ret void\n}\n";
    emittingShared = false;
    return;
  }
  // Windows: this thread's list of what to destroy (a node: next, dtor, object), and one fiber-local
  // slot whose callback runs the list when a thread that set the slot ends.
  ensureMalloc();
  if (sh.target.msvc())
  {
    // The Visual C++ runtime's list of a thread's destructors, which exit() runs first of all, as
    // C++ needs ([basic.start.term] 2), and a thread's end too. A fiber-local slot's callback ran
    // on the main thread only after the statics were gone.
    if (declaredNames.claim("@__tlregdtor"))
      decls += "declare i32 @__tlregdtor(ptr)\n";
    shared += sharedComdat("@kpp.thread.exits") + "@kpp.thread.exits = linkonce_odr thread_local global ptr null, comdat\n";
    shared += sharedComdat("@kpp_thread_exit") +
              "define linkonce_odr void @kpp_thread_exit() comdat {\nentry:\n  br label %next\n"
              "next:\n  %node = load ptr, ptr @kpp.thread.exits\n  %done = icmp eq ptr %node, null\n  br i1 %done, label %end, label %run\n"
              "run:\n  %rest = load ptr, ptr %node\n  store ptr %rest, ptr @kpp.thread.exits\n"
              "  %dp = getelementptr inbounds i8, ptr %node, i64 8\n  %dtor = load ptr, ptr %dp\n"
              "  %op = getelementptr inbounds i8, ptr %node, i64 16\n  %object = load ptr, ptr %op\n"
              "  call void @free(ptr %node)\n  call void %dtor(ptr %object)\n  br label %next\n"
              "end:\n  ret void\n}\n";
    shared += sharedComdat("@kpp_thread_atexit") + std::string(kppThreadAtExitHeader) +
              "\nentry:\n  %head = load ptr, ptr @kpp.thread.exits\n  %first = icmp eq ptr %head, null\n  br i1 %first, label %register, label %push\n"
              "register:\n  %r = call i32 @__tlregdtor(ptr @kpp_thread_exit)\n  br label %push\n"
              "push:\n  %node = call ptr @malloc(i64 24)\n  store ptr %head, ptr %node\n"
              "  %dp = getelementptr inbounds i8, ptr %node, i64 8\n  store ptr %dtor, ptr %dp\n"
              "  %op = getelementptr inbounds i8, ptr %node, i64 16\n  store ptr %object, ptr %op\n"
              "  store ptr %node, ptr @kpp.thread.exits\n  ret void\n}\n";
    emittingShared = false;
    return;
  }
  if (declaredNames.claim("@FlsAlloc"))
    decls += "declare dllimport i32 @FlsAlloc(ptr)\n";
  if (declaredNames.claim("@FlsFree"))
    decls += "declare dllimport i32 @FlsFree(i32)\n";
  if (declaredNames.claim("@FlsSetValue"))
    decls += "declare dllimport i32 @FlsSetValue(i32, ptr)\n";
  shared += sharedComdat("@kpp.thread.slot") + "@kpp.thread.slot = linkonce_odr global i32 -1, comdat\n";
  shared += sharedComdat("@kpp.thread.exits") + "@kpp.thread.exits = linkonce_odr thread_local global ptr null, comdat\n";
  shared += sharedComdat("@kpp_thread_exit") +
            "define linkonce_odr void @kpp_thread_exit(ptr %unused) comdat {\nentry:\n  br label %next\n"
            "next:\n  %node = load ptr, ptr @kpp.thread.exits\n  %done = icmp eq ptr %node, null\n  br i1 %done, label %end, label %run\n"
            "run:\n  %rest = load ptr, ptr %node\n  store ptr %rest, ptr @kpp.thread.exits\n"
            "  %dp = getelementptr inbounds i8, ptr %node, i64 8\n  %dtor = load ptr, ptr %dp\n"
            "  %op = getelementptr inbounds i8, ptr %node, i64 16\n  %object = load ptr, ptr %op\n"
            "  call void @free(ptr %node)\n  call void %dtor(ptr %object)\n  br label %next\n"
            "end:\n  ret void\n}\n";
  shared += sharedComdat("@kpp_thread_atexit") + std::string(kppThreadAtExitHeader) +
            "\nentry:\n  %slot = load atomic i32, ptr @kpp.thread.slot acquire, align 4\n  %none = icmp eq i32 %slot, -1\n"
            "  br i1 %none, label %alloc, label %push\n"
            "alloc:\n  %new = call i32 @FlsAlloc(ptr @kpp_thread_exit)\n"
            "  %pair = cmpxchg ptr @kpp.thread.slot, i32 -1, i32 %new acq_rel acquire, align 4\n"
            "  %won = extractvalue { i32, i1 } %pair, 1\n  %other = extractvalue { i32, i1 } %pair, 0\n"
            "  br i1 %won, label %push, label %lost\n"
            "lost:\n  %freed = call i32 @FlsFree(i32 %new)\n  br label %push\n"
            "push:\n  %index = phi i32 [ %slot, %entry ], [ %new, %alloc ], [ %other, %lost ]\n"
            "  %node = call ptr @malloc(i64 24)\n  %head = load ptr, ptr @kpp.thread.exits\n  store ptr %head, ptr %node\n"
            "  %dp = getelementptr inbounds i8, ptr %node, i64 8\n  store ptr %dtor, ptr %dp\n"
            "  %op = getelementptr inbounds i8, ptr %node, i64 16\n  store ptr %object, ptr %op\n"
            "  store ptr %node, ptr @kpp.thread.exits\n"
            "  %set = call i32 @FlsSetValue(i32 %index, ptr inttoptr (i64 1 to ptr))\n  ret void\n}\n";
  emittingShared = false;
}

auto codegen::Emitter::finishAssertRuntime() -> void
{
  if (!usesAssert || !declaredNames.claim("@kpp_assert_failed"))
    return;
  emittingShared = true;
  std::string format = cStringConstant("%s:%u:%u: assertion failed: %s\n");
  if (declaredNames.claim("@fprintf"))
    decls += "declare i32 @fprintf(ptr, ptr, ...)\n";
  std::string err = stderrStream();
  if (declaredNames.claim("@abort"))
    decls += "declare void @abort()\n";
  shared += "$kpp_assert_failed = comdat any\n" + std::string(kppAssertFailedHeader) + "\nentry:\n" +
          err +
          "  %0 = call i32 (ptr, ptr, ...) @fprintf(ptr %err, ptr " + format + ", ptr %file, i32 %line, i32 %column, ptr %expression)\n"
          "  call void @abort()\n  unreachable\n}\n";
  emittingShared = false;
}
