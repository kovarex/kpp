// struct Emitter's members: debug information (--debug-info): metadata ids, files, types,
// variables, the classes' composites and the line tables; their declarations in DebugInfo.hpp, part
// of the struct's body in Codegen/Emitter.hpp. A translation unit of its own since 2026-11-05
// (codegen.cpp split, stage 2), moved without a change.
#include "Emitter.hpp"

auto codegen::Emitter::debugId(uint32_t local) -> std::string
{
  char digits[16];
  snprintf(digits, sizeof digits, "!%010u", local);
  return digits;
}

auto codegen::Emitter::programPathOf(const ParsedFile* file) -> const std::string&
{
  auto [it, added] = programPaths.try_emplace(file);
  if (added)
    it->second = programPath(file->path);
  return it->second;
}

auto codegen::Emitter::debugFileOf(const ParsedFile* file) -> const std::string&
{
  std::string& text = debugFiles[file];
  if (text.empty())
  {
    std::error_code ec;
    std::string path = std::filesystem::absolute(file->path, ec).string();
    if (ec)
      path = file->path;
    std::string escaped;
    for (char c : path)
      if (c == '\\' || c == '"' || (unsigned char)c < 0x20)
      {
        char hex[4];
        snprintf(hex, sizeof hex, "\\%02X", (unsigned char)c);
        escaped += hex;
      }
      else
        escaped += c;
    text = "!DIFile(filename: \"" + escaped + "\", directory: \"\")";
  }
  return text;
}

auto codegen::Emitter::metadataString(std::string_view s) -> std::string
{
  std::string out = "\"";
  for (char c : s)
    if (c == '\\' || c == '"' || (unsigned char)c < 0x20)
    {
      char hex[4];
      snprintf(hex, sizeof hex, "\\%02X", (unsigned char)c);
      out += hex;
    }
    else
      out += c;
  return out + "\"";
}

auto codegen::Emitter::debugTypePlaceholder(uint64_t key) -> std::string
{
  char text[24];
  snprintf(text, sizeof text, "!T%016llx", (unsigned long long)key);
  return text;
}

auto codegen::Emitter::debugAskedThisGeneration() -> void
{
  if (debugAskedGeneration == sh.debugGeneration)
    return;
  debugAskedGeneration = sh.debugGeneration;
  debugTypesAsked.clear();
  debugObjectPointersAsked.clear();
  debugFilesAsked.clear();
}

auto codegen::Emitter::debugType(const Ty* t) -> std::string
{
  debugAskedThisGeneration();
  if (auto asked = debugTypesAsked.find(t); asked != debugTypesAsked.end())
    return asked->second;
  std::string spelling = irString(t);
  uint64_t key = 14695981039346656037ull;
  for (unsigned char c : spelling)
    key = (key ^ c) * 1099511628211ull;
  std::string placeholder = debugTypePlaceholder(key);
  bool isClass = (t->kind == TyKind::Class || t->kind == TyKind::Lambda) && !t->isConst && !t->isVolatile;
  Shared::DebugTypeShard& shard = sh.debugTypeShard(key);
  {
    std::lock_guard lock(shard.mutex);
    auto [it, isNew] = shard.types.try_emplace(key);
    if (!isNew && it->second.generation == sh.debugGeneration)
      return debugTypesAsked[t] = placeholder; // (made, or being made: a type that names itself goes through a pointer)
    it->second.generation = sh.debugGeneration;
    if (isClass)
      shard.classesNamed.push_back(key);
  }
  std::string text = debugTypeText(t, spelling);
  std::lock_guard lock(shard.mutex);
  Shared::DebugTypeText& made = shard.types[key];
  made.text = std::move(text);
  made.type = isClass ? t : nullptr;
  return debugTypesAsked[t] = placeholder;
}

auto codegen::Emitter::debugSharedNode(const std::string& spelling, const std::function<std::string()>& make) -> std::string
{
  uint64_t key = 14695981039346656037ull;
  for (unsigned char c : spelling)
    key = (key ^ c) * 1099511628211ull;
  std::string placeholder = debugTypePlaceholder(key);
  Shared::DebugTypeShard& shard = sh.debugTypeShard(key);
  {
    std::lock_guard lock(shard.mutex);
    auto [it, isNew] = shard.types.try_emplace(key);
    if (!isNew && it->second.generation == sh.debugGeneration)
      return placeholder; // (made, or being made)
    it->second.generation = sh.debugGeneration;
  }
  std::string text = make();
  std::lock_guard lock(shard.mutex);
  Shared::DebugTypeText& made = shard.types[key];
  made.text = std::move(text);
  made.type = nullptr;
  return placeholder;
}

auto codegen::Emitter::debugFileNode(const ParsedFile* file) -> std::string
{
  const std::string& node = debugFileOf(file);
  if (!sh.fullDebugInfo)
    return node; // (a build of line tables alone appends no shared nodes)
  debugAskedThisGeneration();
  if (auto asked = debugFilesAsked.find(file); asked != debugFilesAsked.end())
    return asked->second;
  return debugFilesAsked[file] = debugSharedNode("file " + node, [&] { return debugId(0) + " = " + node + "\n"; });
}

auto codegen::Emitter::debugObjectPointer(const Ty* cls) -> std::string
{
  debugAskedThisGeneration();
  if (auto asked = debugObjectPointersAsked.find(cls); asked != debugObjectPointersAsked.end())
    return asked->second;
  std::string pointee = debugType(cls);
  return debugObjectPointersAsked[cls] = debugSharedNode("object pointer " + irString(cls), [&]
  { return debugId(0) + " = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: " + pointee + ", size: 64, flags: DIFlagArtificial | DIFlagObjectPointer)\n"; });
}

auto codegen::Emitter::debuggerTypeName(std::string name) -> std::string
{
  for (size_t i = name.find(">>"); i != std::string::npos; i = name.find(">>", i))
    name.insert(i + 1, " ");
  return name;
}

auto codegen::Emitter::debuggerClassName(const Ty* t) const -> std::string
{
  if (t && t->kind == TyKind::Class && !t->isConst && !t->isVolatile && !t->args.empty())
    if (std::string shown = types.toString(t, &symbols); !shown.empty() && shown.back() == '^')
      return debuggerTypeName(types.toString(t->args[0], &symbols, true)) + "^";
  std::string name = types.toString(t, &symbols, true);
  // A class declared in a class template carries the enclosing template's arguments (then its
  // own), which kpp's name writes after the innermost class, `Outer::Inner<A, B>`: here each
  // class's arguments follow it, `Outer<A>::Inner<B>`, as C++ and MSVC's PDBs name it (the game's
  // natvis rule `IntrusiveListImplementation<*,*,*,*>::List`).
  if (t && t->kind == TyKind::Class && t->decl && t->decl->kind == NodeKind::ClassDecl && !t->args.empty() && !(t->decl->flags & DF_Specialization))
  {
    std::vector<ClassDecl*> chain; // the classes, outermost first
    for (Decl* d = t->decl; d && d->kind == NodeKind::ClassDecl; d = d->parent)
      chain.insert(chain.begin(), static_cast<ClassDecl*>(d));
    size_t total = 0;
    bool outerTemplate = false;
    for (size_t i = 0; i < chain.size(); ++i)
    {
      total += chain[i]->templateParams.size();
      outerTemplate = outerTemplate || (i + 1 < chain.size() && !chain[i]->templateParams.empty());
    }
    size_t open = name.find('<');
    if (outerTemplate && total == t->args.size() && open != std::string::npos && name.back() == '>')
    {
      // The components of `ns::Outer::Inner`, the last chain.size() of them the classes'.
      std::string base = name.substr(0, open);
      std::vector<std::string> parts;
      for (size_t at = 0;;)
      {
        size_t next = base.find("::", at);
        parts.push_back(base.substr(at, next == std::string::npos ? std::string::npos : next - at));
        if (next == std::string::npos)
          break;
        at = next + 2;
      }
      if (parts.size() >= chain.size())
      {
        std::string out;
        size_t first = parts.size() - chain.size(), arg = 0;
        for (size_t i = 0; i < parts.size(); ++i)
        {
          out += (i ? "::" : "") + parts[i];
          if (i < first || chain[i - first]->templateParams.empty())
            continue;
          out += "<";
          for (size_t k = 0; k < chain[i - first]->templateParams.size(); ++k, ++arg)
            out += (k ? ", " : "") + types.toString(t->args[arg], &symbols, true);
          out += ">";
        }
        name = out;
      }
    }
  }
  return debuggerTypeName(name);
}

// A member function's `&` or `&&` in its DISubroutineType, which CodeView keeps on the `this` pointer's
// record (the other qualifiers are the class `this` points to).
static std::string debugRefQualifier(bool lvalue, bool rvalue)
{
  return lvalue ? "flags: DIFlagLValueReference, " : rvalue ? "flags: DIFlagRValueReference, " : "";
}

auto codegen::Emitter::debugTypeText(const Ty* t, const std::string& spelling) -> std::string
{
  auto basic = [](const char* name, uint64_t bits, const char* encoding)
  { return std::string("!DIBasicType(name: \"") + name + "\", size: " + std::to_string(bits) + ", encoding: " + encoding + ")"; };
  auto one = [](const std::string& node) { return debugId(0) + " = " + node + "\n"; };
  // (`T* const`'s qualifier is the pointer's: its unqualified type is the pointer.)
  if (const Ty* bare = types.unqualified(t); bare != t)
    return one(std::string("!DIDerivedType(tag: ") + (t->isConst ? "DW_TAG_const_type" : "DW_TAG_volatile_type") +
               ", baseType: " + debugType(t->isConst && t->isVolatile ? types.qualified(bare, false, true) : bare) + ")");
  switch (t->kind)
  {
    case TyKind::Bool: return one(basic("bool", 8, "DW_ATE_boolean"));
    case TyKind::Char:
      if (t->isWchar()) // (as clang names and encodes it: the debuggers' wchar_t)
        return one(basic("wchar_t", t->bits, t->isUnsigned ? "DW_ATE_unsigned" : "DW_ATE_signed"));
      if (t->bits == 8)
        return one(t->isUnsigned ? basic("char8_t", 8, "DW_ATE_UTF") : basic("char", 8, "DW_ATE_signed_char"));
      return one(basic(t->bits == 16 ? "char16_t" : "char32_t", t->bits, "DW_ATE_UTF"));
    case TyKind::Float: return one(basic(t->bits == 32 ? "float" : "double", t->bits, "DW_ATE_float"));
    case TyKind::Nullptr: return one("!DIBasicType(tag: DW_TAG_unspecified_type, name: \"decltype(nullptr)\")");
    case TyKind::Int:
    {
      if (t->isLong())
        return one(basic(t->isUnsigned ? "unsigned long" : "long", t->bits, t->isUnsigned ? "DW_ATE_unsigned" : "DW_ATE_signed"));
      static const char* const names[2][4] = {{"signed char", "short", "int", "long long"}, {"unsigned char", "unsigned short", "unsigned int", "unsigned long long"}};
      int size = t->bits == 8 ? 0 : t->bits == 16 ? 1 : t->bits == 32 ? 2 : 3;
      const char* encoding = t->isUnsigned ? (size == 0 ? "DW_ATE_unsigned_char" : "DW_ATE_unsigned") : (size == 0 ? "DW_ATE_signed_char" : "DW_ATE_signed");
      return debugId(0) + " = !DIDerivedType(tag: DW_TAG_typedef, name: " + metadataString(spelling) + ", baseType: " + debugId(1) + ")\n" + debugId(1) + " = " +
             basic(names[t->isUnsigned][size], t->bits, encoding) + "\n";
    }
    case TyKind::Pointer:
      return one("!DIDerivedType(tag: DW_TAG_pointer_type, baseType: " + (types.unqualified(t->inner)->kind == TyKind::Void ? std::string("null") : debugType(t->inner)) +
                 ", size: 64)");
    case TyKind::LRef:
    case TyKind::RRef:
      return one(std::string("!DIDerivedType(tag: ") + (t->kind == TyKind::LRef ? "DW_TAG_reference_type" : "DW_TAG_rvalue_reference_type") +
                 ", baseType: " + debugType(t->inner) + ", size: 64)");
    case TyKind::Array:
      if (t->count)
        return one("!DICompositeType(tag: DW_TAG_array_type, baseType: " + debugType(t->inner) + ", size: " + std::to_string(sizeOf(t) * 8) +
                   ", elements: " + debugId(1) + ")") +
               debugId(1) + " = !{ " + debugId(2) + "}\n" + debugId(2) + " = !DISubrange(count: " + std::to_string(t->count) + ")\n";
      break;
    case TyKind::Enum:
      if (t->decl && t->decl->kind == NodeKind::EnumDecl)
      {
        auto* e = static_cast<EnumDecl*>(t->decl);
        const Ty* underlying = enumUnderlying(t);
        std::string elements, enumerators;
        uint32_t next = 2;
        for (Decl* d : e->enumerators)
        {
          if (d->kind != NodeKind::EnumeratorDecl)
            continue;
          auto* en = static_cast<EnumeratorDecl*>(d);
          elements += (elements.empty() ? "" : ", ") + debugId(next);
          // An unsigned enumeration's value as its type holds it (`Top = ~0ull`, `None = -1`: LLVM
          // rejects an unsigned enumerator with a negative value).
          int64_t value = enumeratorValue(en, t);
          std::string valueText = std::to_string(value);
          if (underlying && underlying->isUnsigned)
          {
            uint64_t bits = uint64_t(value);
            if (underlying->bits && underlying->bits < 64)
              bits &= (uint64_t(1) << underlying->bits) - 1;
            valueText = std::to_string(bits);
          }
          enumerators += debugId(next++) + " = !DIEnumerator(name: " + metadataString(sym(en->name)) + ", value: " + valueText +
                         (underlying && underlying->isUnsigned ? ", isUnsigned: true" : "") + ")\n";
        }
        return one("!DICompositeType(tag: DW_TAG_enumeration_type, name: " + metadataString(debuggerTypeName(spelling)) +
                   (underlying ? ", baseType: " + debugType(underlying) : std::string()) + ", size: " + std::to_string(sizeOf(t) * 8) +
                   (e->isClass ? ", flags: DIFlagEnumClass" : "") + ", elements: " + debugId(1) + ", identifier: " + metadataString(spelling) + ")") +
               debugId(1) + " = !{ " + elements + "}\n" + enumerators; // (`!{ `: renumberDebugIds takes an id after a blank)
      }
      break;
    case TyKind::Class:
    case TyKind::Lambda:
    {
      // Its declaration, with the definition's tag (debugComposite): the definition is its home's
      // alone (homeDebugClasses), every other module's reference resolved by the identifier.
      auto* cd = t->decl && t->decl->kind == NodeKind::ClassDecl ? static_cast<ClassDecl*>(t->decl) : nullptr;
      const char* tag = cd && cd->keyword == Kw::union_ ? "DW_TAG_union_type" : cd && cd->keyword == Kw::class_ ? "DW_TAG_class_type" : "DW_TAG_structure_type";
      return one(std::string("!DICompositeType(tag: ") + tag + ", name: " + metadataString(debuggerClassName(t)) + ", flags: DIFlagFwdDecl, identifier: " +
                 metadataString(spelling) + ")");
    }
    case TyKind::Function:
    {
      // Its result and parameters, as clang's: null for a void result and for C's `...`. (noexcept
      // has no place in CodeView or in this node: clang's two types are one too.)
      std::string list = t->inner && t->inner->isKnown() && t->inner->kind != TyKind::Void && !types.isDependent(t->inner) ? debugType(t->inner) : std::string("null");
      for (const Ty* a : t->args)
        list += ", " + (a && a->isKnown() && !types.isDependent(a) ? debugType(a) : std::string("null"));
      if (t->variadic)
        list += ", null";
      return debugId(0) + " = !DISubroutineType(types: " + debugId(1) + ")\n" + debugId(1) + " = !{ " + list + " }\n";
    }
    case TyKind::MemberPointer:
      // A pointer to a member function: its class and representation (MSVC's single or multiple
      // inheritance model, memberFunctionPointerAdjusts), which give a debugger its size.
      if (t->inner && t->inner->kind == TyKind::Function && !t->args.empty() && isClass(t->args[0]) && !types.isDependent(t->args[0]))
      {
        bool adjusts = memberFunctionPointerAdjusts(t, nullptr);
        // Its function type as a member function's (clang's): the result, `this` (artificial), the
        // parameters, which CodeView writes as the class's LF_MFUNCTION. Its qualifiers are `this`'s.
        const Ty* ft = t->inner;
        uint8_t qualifiers = ft->memberQualifiers();
        std::string list = ft->inner && ft->inner->isKnown() && ft->inner->kind != TyKind::Void ? debugType(ft->inner) : std::string("null");
        list += ", " + debugId(3);
        for (const Ty* a : ft->args)
          list += ", " + (a && a->isKnown() ? debugType(a) : std::string("null"));
        if (ft->variadic)
          list += ", null";
        std::string cls = debugType(t->args[0]);
        std::string self = debugType(types.qualified(t->args[0], qualifiers & TypeTable::MemberConst, qualifiers & TypeTable::MemberVolatile));
        return debugId(0) + " = !DIDerivedType(tag: DW_TAG_ptr_to_member_type, baseType: " + debugId(1) + ", size: " + (adjusts ? "128" : "64") +
               ", flags: " + (adjusts ? "DIFlagMultipleInheritance" : "DIFlagSingleInheritance") + ", extraData: " + cls + ")\n" + debugId(1) +
               " = !DISubroutineType(" + debugRefQualifier(qualifiers & TypeTable::MemberLValue, qualifiers & TypeTable::MemberRValue) + "types: " + debugId(2) + ")\n" +
               debugId(2) + " = !{ " + list + " }\n" + debugId(3) +
               " = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: " + self + ", size: 64, flags: DIFlagArtificial | DIFlagObjectPointer)\n";
      }
      // A pointer to a data member: the member's type, its class, and its size (4 bytes in MSVC's ABI,
      // MSVC's single inheritance model), as clang describes one (until 2026-11-06 an unsigned
      // integer of 8 bytes).
      if (t->inner && t->inner->kind != TyKind::Function && t->inner->isKnown() && !types.isDependent(t->inner) && !t->args.empty() &&
          isClass(t->args[0]) && !types.isDependent(t->args[0]))
        return one("!DIDerivedType(tag: DW_TAG_ptr_to_member_type, baseType: " + debugType(t->inner) + ", size: " + std::to_string(sizeOf(t, nullptr) * 8) +
                   (sh.target.msvc() ? ", flags: DIFlagSingleInheritance" : "") + ", extraData: " + debugType(t->args[0]) + ")");
      break;
    default:
      break;
  }
  // What a debugger has no type for (a pointer to a member): an unsigned integer of its size.
  return one(basic("unsigned long long", 64, "DW_ATE_unsigned"));
}

auto codegen::Emitter::debugVariable(std::string name, Node* at, const std::string& address, const Ty* t, uint32_t arg, bool artificial, bool defined,
                                     bool indirect) -> void
{
  if (!sh.fullDebugInfo || !debugFile || debugFile != currentFile || !inlining.empty() || !t || !t->isKnown() || types.isDependent(t))
    return;
  debugVariables.push_back({address, std::move(name), debugType(t), at ? lineOf(at, currentFile) : 0, arg ? 0 : debugBlock, arg, artificial, defined, indirect});
}

auto codegen::Emitter::debugGlobal(const std::string& name, VariableDecl* def, const Ty* t) -> std::string
{
  if (!sh.fullDebugInfo || !def || !def->file || !t || !t->isKnown() || types.isDependent(t))
    return std::string();
  std::string bare = name.rfind("@\"", 0) == 0 ? name.substr(2, name.size() - 3) : name.substr(1);
  uint64_t key = 14695981039346656037ull;
  for (unsigned char c : bare)
    key = (key ^ c) * 1099511628211ull;
  char placeholder[24];
  snprintf(placeholder, sizeof placeholder, "!G%016llx", (unsigned long long)key);
  std::string text = debugId(0) + " = !DIGlobalVariableExpression(var: " + debugId(1) + ", expr: !DIExpression())\n" + debugId(1) +
                     " = distinct !DIGlobalVariable(name: " + metadataString(debuggerTypeName(bare)) + ", scope: !0, file: " + debugFileNode(def->file) +
                     ", line: " + std::to_string(lineOf(def, def->file)) + ", type: " + debugType(t) + ", isLocal: false, isDefinition: true)\n";
  auto lock = sharedLock();
  sh.debugGlobals[key] = std::move(text);
  return std::string(", !dbg ") + placeholder;
}

auto codegen::Emitter::debugName(Decl* d) -> std::string
{
  if (d && d->kind == NodeKind::VariableDecl && !static_cast<VariableDecl*>(d)->name.empty())
    return sym(static_cast<VariableDecl*>(d)->name.last().symbol);
  if (d && d->kind == NodeKind::ParamDecl && static_cast<ParamDecl*>(d)->name != UINT32_MAX)
    return sym(static_cast<ParamDecl*>(d)->name);
  return std::string();
}

auto codegen::Emitter::laidOutClass(const Ty* ct) -> const StructInfo*
{
  std::lock_guard lock(sh.structsMutex); // (the published layouts)
  const StructInfo* info = nullptr;
  if (auto it = sh.structsByType.find(ct); it != sh.structsByType.end())
    info = it->second;
  else if (auto s = sh.structs.find(ct); s != sh.structs.end())
    info = &s->second;
  return info && info->complete && !info->failed ? info : nullptr;
}

auto codegen::Emitter::staticConstant(VariableDecl* v, const Ty* t, const TemplateArgs& args) -> std::string
{
  // (`static constexpr int X = 8;`, and `static const int X = 8;`, which C++ makes a constant expression too)
  if (!((v->flags & DF_Constexpr) || (t->isConst && v->init)) || !(t->kind == TyKind::Int || t->kind == TyKind::Char || t->kind == TyKind::Enum))
    return {};
  VariableDecl* def = variableDefinition(v);
  Expr* init = def ? global.instanceInitializer(def, args) : nullptr;
  if (!init)
    return {};
  // No layouts, and no function run: a value that needs neither is the same in every build of the
  // class (an incremental build's too, which re-emits a class only when its own text changes).
  bool ran = false;
  ConstHooks hooks;
  hooks.ran = [&](const FunctionDecl*) { ran = true; };
  std::optional<ConstValue> value = global.evaluate(init, def->file, def, args, hooks);
  GlobalScope::clearConstantReason();
  uint64_t bits = sizeOf(t, nullptr) * 8;
  if (ran || !value || value->kind != ConstValue::Kind::Int || bits == 0 || bits > 64)
    return {};
  int64_t n = value->i;
  if (bits < 64)
    n = int64_t(uint64_t(n) << (64 - bits)) >> (64 - bits); // as the width's signed value, which LLVM reads
  return ", extraData: i" + std::to_string(bits) + " " + std::to_string(n);
}

auto codegen::Emitter::debugMethod(const Ty* cls, ClassDecl* c, FunctionDecl* m, size_t index, const TemplateArgs& args) -> std::string
{
  // (A class whose layout or vtable fails here is no member function the debugger gets: not the
  // build's error, which only debug information asked for.)
  size_t errorsBefore = errors.size();
  try
  {
    return debugMethodText(cls, c, m, index, args);
  }
  catch (Unsupported&)
  {
    errors.resize(errorsBefore);
    return {};
  }
}

auto codegen::Emitter::debugMethodText(const Ty* cls, ClassDecl* c, FunctionDecl* m, size_t index, const TemplateArgs& args) -> std::string
{
  if (!cls || !c || !m->templateParams.empty() || (m->flags & (DF_Constructor | DF_Destructor | DF_Friend)) || m->builtin || m->name.parts.size() != 1 ||
      m->name.last().special || m->name.last().symbol == UINT32_MAX)
    return {};
  const Ty* ft = global.typeOfDeclIn(m, args);
  if (!ft || ft->kind != TyKind::Function || !ft->isKnown() || types.isDependent(ft))
    return {};
  bool isStatic = (m->flags & DF_Static) != 0;
  bool isVirtual = !isStatic && (m->flags & (DF_Virtual | DF_Override)) != 0;
  int slot = isVirtual ? slotOf(cls, m, m) : -1;
  if (isVirtual && slot < 0)
    return {}; // (a virtual function without a slot in the primary vtable: no call the debugger could make right)
  // As clang describes one: the class that introduced the function (the deepest base whose primary
  // vtable has its key, first in declaration order), its slot there, and that base's offset in cls
  // (thisAdjustment): the debugger calls through that subobject's vtable pointer, which reaches the
  // object's override (kpp's secondary vtables mirror the base's). Introduced here when no base has it.
  const Ty* introducer = cls;
  uint64_t adjustment = 0;
  if (isVirtual)
  {
    std::string key = methodKey(m);
    // (a base's slot of another result is not this function's: a covariant override with a slot of its own)
    const Ty* result = structFor(cls, nullptr).vtables[0].slots[slot].result;
    std::function<const Ty*(const Ty*, int)> deepest = [&](const Ty* t, int depth) -> const Ty*
    {
      if (depth > 32)
        return nullptr;
      ClassDecl* tc = t->decl ? classDef(t->decl, nullptr) : nullptr;
      if (!tc)
        return nullptr;
      for (const Ty* b : basesOf(tc, global.classArgsOf(canonicalClass(t, nullptr)), nullptr))
        if (int inBase = b && isClass(b) && structFor(b, nullptr).polymorphic ? slotKeyed(structFor(b, nullptr), key) : -1;
            inBase >= 0 && structFor(b, nullptr).vtables[0].slots[inBase].result == result)
        {
          const Ty* below = deepest(b, depth + 1);
          return below ? below : b;
        }
      return nullptr;
    };
    if (const Ty* base = deepest(cls, 0))
      if (std::optional<uint64_t> offset = baseOffsetIn(cls, base, nullptr))
      {
        introducer = base;
        adjustment = *offset;
        slot = slotKeyed(structFor(base, nullptr), key);
      }
    // An override of a function a base at an offset introduced is not listed: Visual Studio calls
    // such a one (thisAdjustment) wrongly, clang's too (a garbage result); without it the debugger
    // takes the base's, which it calls right through that base's vtable pointer.
    if (slot < 0 || adjustment)
      return {};
  }
  std::string spelling = "method " + irString(cls) + "#" + std::to_string(index);
  uint64_t key = 14695981039346656037ull;
  for (unsigned char ch : spelling)
    key = (key ^ ch) * 1099511628211ull;
  std::string placeholder = debugTypePlaceholder(key);
  Shared::DebugTypeShard& shard = sh.debugTypeShard(key);
  {
    std::lock_guard lock(shard.mutex);
    auto [it, isNew] = shard.types.try_emplace(key);
    if (!isNew && it->second.generation == sh.debugGeneration)
      return placeholder;
    it->second.generation = sh.debugGeneration;
  }
  std::string classType, list;
  try
  {
    classType = debugType(cls);
    list = ft->inner && ft->inner->isKnown() && ft->inner->kind != TyKind::Void ? debugType(ft->inner) : std::string("null");
    if (!isStatic)
      list += ", " + debugObjectPointer(types.qualified(cls, m->flags & DF_Const, m->flags & DF_Volatile));
    for (const Ty* a : ft->args)
      list += ", " + (a && a->isKnown() && !types.isDependent(a) ? debugType(a) : std::string("null"));
  }
  catch (Unsupported&)
  {
    std::lock_guard lock(shard.mutex);
    shard.types[key].generation = UINT32_MAX; // (not made: debugMethod leaves the function out)
    throw;
  }
  std::string access = m->access == Access::Public ? " | DIFlagPublic" : m->access == Access::Protected ? " | DIFlagProtected" :
                       m->access == Access::Private ? " | DIFlagPrivate" : "";
  std::string text = debugId(0) + " = !DISubprogram(name: " + metadataString(sym(m->name.last().symbol)) + ", scope: " + classType +
                     ", type: " + debugId(1) + ", flags: DIFlagPrototyped" + access + (isStatic ? " | DIFlagStaticMember" : "") +
                     (isVirtual && introducer == cls ? " | DIFlagIntroducedVirtual" : "") + ", spFlags: " + (isVirtual ? "DISPFlagVirtual" : "0") +
                     (isVirtual ? ", virtualIndex: " + std::to_string(slot) + (adjustment ? ", thisAdjustment: " + std::to_string(adjustment) : std::string()) +
                                      ", containingType: " + classType : std::string()) + ")\n" +
                     debugId(1) + " = !DISubroutineType(" +
                     debugRefQualifier(m->refQualifier == FunctionDecl::RefQualifier::LValue, m->refQualifier == FunctionDecl::RefQualifier::RValue) +
                     "types: " + debugId(2) + ")\n" + debugId(2) + " = !{ " + list + " }\n";
  std::lock_guard lock(shard.mutex);
  Shared::DebugTypeText& made = shard.types[key];
  made.text = std::move(text);
  made.type = nullptr;
  return placeholder;
}

auto codegen::Emitter::debugComposite(const Ty* ct, const StructInfo& info, uint32_t self, const std::string* spelling, std::string& nodes, uint32_t& next, int depth) -> void
{
  auto* c = static_cast<ClassDecl*>(ct->decl);
  TemplateArgs args = global.classArgsOf(ct);
  // Each field's offset, as memberOffset computes it (an empty base's: no storage).
  std::vector<uint64_t> offsets(info.fieldTypes.size(), 0);
  uint64_t at = 0;
  for (size_t i = 0; i < info.fieldTypes.size() && !info.isUnion; ++i)
  {
    if (i < info.zeroSize.size() && info.zeroSize[i])
    {
      offsets[i] = at;
      continue;
    }
    uint64_t a = info.packed ? 1 : alignOf(info.fieldTypes[i], nullptr);
    at = (at + a - 1) / a * a;
    offsets[i] = at;
    at += sizeOf(info.fieldTypes[i], nullptr);
  }
  std::string elements;
  auto element = [&](uint32_t id) { elements += (elements.empty() ? "" : ", ") + debugId(id); };
  // A member's access as written (`public:` in a class, `private:` in a struct); none for the
  // default, which the tag gives (a class's members private, a struct's public).
  auto access = [](Access a) -> std::string
  {
    return a == Access::Public ? "DIFlagPublic" : a == Access::Protected ? "DIFlagProtected" : a == Access::Private ? "DIFlagPrivate" : "";
  };
  auto flagsOf = [](std::initializer_list<std::string> flags)
  {
    std::string text;
    for (const std::string& f : flags)
      if (!f.empty())
        text += (text.empty() ? ", flags: " : " | ") + f;
    return text;
  };
  auto member = [&](const std::string& name, const std::string& type, uint64_t bits, uint64_t offset, const std::string& flags, const std::string& extra = "")
  {
    uint32_t id = next++;
    nodes += debugId(id) + " = !DIDerivedType(tag: DW_TAG_member" + (name.empty() ? std::string() : ", name: " + metadataString(name)) +
             ", scope: " + debugId(self) + ", baseType: " + type + ", size: " + std::to_string(bits) + ", offset: " + std::to_string(offset) + flags + extra + ")\n";
    element(id);
  };
  if (info.polymorphic && !info.primaryBase && !info.isUnion && !info.fieldTypes.empty())
  {
    // Its own vtable pointer: a pointer to the vtable's shape (CodeView's LF_VTSHAPE, one entry a slot).
    uint64_t slots = info.vtables.empty() ? 1 : std::max<uint64_t>(1, info.vtables[0].slots.size());
    uint32_t shape = next++, pointer = next++;
    nodes += debugId(shape) + " = !DIDerivedType(tag: DW_TAG_pointer_type, name: \"__vtbl_ptr_type\", baseType: null, size: " + std::to_string(slots * 64) + ")\n";
    nodes += debugId(pointer) + " = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: " + debugId(shape) + ", size: 64)\n";
    member("_vptr$" + (c->name.empty() ? std::string("?") : sym(c->name.last().symbol)), debugId(pointer), 64, 0, flagsOf({"DIFlagArtificial"}));
  }
  // The bases in the order written, their access too where the layout's are the written ones.
  for (size_t b = 0; b < info.bases.size(); ++b)
    if (uint32_t index = info.bases[b].second; index < info.fieldTypes.size())
    {
      uint32_t id = next++;
      nodes += debugId(id) + " = !DIDerivedType(tag: DW_TAG_inheritance, scope: " + debugId(self) + ", baseType: " + debugType(info.fieldTypes[index]) +
               ", offset: " + std::to_string(offsets[index] * 8) + flagsOf({info.bases.size() == c->bases.size() ? access(c->bases[b].access) : ""}) +
               ", extraData: i32 0)\n";
      element(id);
    }
  for (Decl* m : info.fieldDecls)
  {
    auto found = info.fieldIndex.find(m);
    if (found == info.fieldIndex.end())
      continue;
    uint32_t index = found->second;
    uint64_t offset = info.isUnion || index >= offsets.size() ? 0 : offsets[index];
    if (m->kind == NodeKind::ClassDecl)
    {
      // An anonymous struct or union: its own composite here, with no name, an unnamed member.
      const Ty* inner = types.classType(m, ct->args);
      const StructInfo* innerInfo = depth < 8 ? laidOutClass(inner) : nullptr;
      if (!innerInfo)
        continue;
      uint32_t composite = next++;
      debugComposite(inner, *innerInfo, composite, nullptr, nodes, next, depth + 1);
      member("", debugId(composite), sizeOf(inner, nullptr) * 8, offset * 8, flagsOf({access(m->access)}));
      continue;
    }
    auto* v = static_cast<VariableDecl*>(m);
    std::string name = v->name.empty() ? std::string() : sym(v->name.last().symbol);
    if (info.bitfields.count(m))
    {
      // The first bit field of a storage unit stands for the unit in fieldDecls: each bit field of
      // the unit, in declaration order (until 2026-11-06 only the first: PropertyTree's flags after
      // its type were missing in the debugger).
      for (Decl* d : c->members)
      {
        auto bits = info.bitfields.find(d);
        if (bits == info.bitfields.end() || info.fieldIndex.at(d) != index)
          continue;
        auto* bv = static_cast<VariableDecl*>(d);
        const Ty* declared = global.typeOfDeclIn(d, args);
        if (declared && declared->isKnown() && !types.isDependent(declared))
          member(bv->name.empty() ? std::string() : sym(bv->name.last().symbol), debugType(declared), bits->second.width,
                 offset * 8 + bits->second.shift, flagsOf({"DIFlagBitField", access(d->access)}), ", extraData: i64 " + std::to_string(offset * 8));
      }
      continue;
    }
    const Ty* mt = info.isUnion ? global.typeOfDeclIn(m, args) : info.fieldTypes[index];
    if (!mt || !mt->isKnown() || types.isDependent(mt))
      continue;
    member(name, debugType(mt), sizeOf(mt, nullptr) * 8, offset * 8, flagsOf({access(m->access)}));
  }
  // Its static data members (CodeView's LF_STMEMBER): their storage is a global of the program,
  // named Class::member (debugGlobal), where the debugger reads their values.
  for (Decl* m : c->members)
  {
    if (m->kind != NodeKind::VariableDecl || !(m->flags & DF_Static) || (m->flags & (DF_Typedef | DF_Friend)) || m->builtin)
      continue;
    auto* v = static_cast<VariableDecl*>(m);
    const Ty* st = v->name.empty() ? nullptr : global.typeOfDeclIn(m, args);
    if (!st || !st->isKnown() || types.isDependent(st))
      continue;
    uint32_t id = next++;
    nodes += debugId(id) + " = !DIDerivedType(tag: DW_TAG_member, name: " + metadataString(sym(v->name.last().symbol)) + ", scope: " + debugId(self) +
             ", baseType: " + debugType(st) + flagsOf({"DIFlagStaticMember", access(m->access)}) + staticConstant(v, st, args) + ")\n";
    element(id);
  }
  // Its member functions (debugMethod), each a node shared with its definition's `declaration:`.
  if (spelling && sh.target.windows())
    for (size_t i = 0; i < c->members.size(); ++i)
      if (c->members[i]->kind == NodeKind::FunctionDecl)
        if (std::string method = debugMethod(ct, c, static_cast<FunctionDecl*>(c->members[i]), i, args); !method.empty())
          elements += (elements.empty() ? "" : ", ") + method;
  std::string tag = info.isUnion || c->keyword == Kw::union_ ? "DW_TAG_union_type" : c->keyword == Kw::class_ ? "DW_TAG_class_type" : "DW_TAG_structure_type";
  uint32_t list = next++;
  nodes += debugId(list) + " = !{ " + elements + "}\n";
  nodes += debugId(self) + " = distinct !DICompositeType(tag: " + tag + (spelling ? ", name: " + metadataString(debuggerClassName(ct)) : std::string()) +
           ", size: " + std::to_string(sizeOf(ct, nullptr) * 8) + ", elements: " + debugId(list) +
           (spelling && info.polymorphic && !info.primaryBase ? ", vtableHolder: " + debugId(self) : std::string()) +
           (spelling ? ", identifier: " + metadataString(*spelling) : std::string()) + ")\n";
}

auto codegen::Emitter::homeDebugClasses() -> void
{
  // Level by level: the classes not defined this generation yet, each level's in key order, until a
  // level has none (only a class's definition names new types).
  for (;;)
  {
    // This level's classes, each defined once a generation, on every emitter at once (Shared::
    // onEmitters; on one thread until 2026-11-07, 2.5 s of the game's debug build with every core
    // but one idle): a definition depends on its class alone (its nodes numbered from its own 0,
    // every other type by placeholder), so it is the same whichever emitter makes it and in
    // whatever order, and the types the definitions name are the next level's. The definitions are
    // taken in key order after, as one thread took them, and the first that failed is thrown there.
    // (The classes this generation named since the last level, each shard's classesNamed: those of every
    // entry that a scan of them all took.)
    std::vector<std::pair<uint64_t, Shared::DebugTypeText*>> classes;
    for (Shared::DebugTypeShard& shard : sh.debugTypeShards)
    {
      std::lock_guard lock(shard.mutex);
      for (uint64_t key : shard.classesNamed)
        if (auto it = shard.types.find(key); it != shard.types.end())
          if (Shared::DebugTypeText& entry = it->second;
              entry.type && entry.generation == sh.debugGeneration && entry.definitionGeneration != sh.debugGeneration)
            classes.push_back({key, &entry}); // (an element stays where it is as others are added)
      shard.classesNamed.clear();
    }
    if (classes.empty())
      break;
    std::sort(classes.begin(), classes.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    classes.erase(std::unique(classes.begin(), classes.end(), [](const auto& a, const auto& b) { return a.first == b.first; }), classes.end());
    for (auto& [key, entry] : classes)
      entry->definitionGeneration = sh.debugGeneration;
    struct Made
    {
      std::string definition;
      std::exception_ptr failed;
      std::vector<std::string> errors; // (what its emitter recorded as it failed)
    };
    std::vector<Made> made(classes.size());
    std::atomic<size_t> nextClass{0};
    auto define = [&](Emitter& em)
    {
      for (size_t i; (i = nextClass++) < classes.size();)
      {
        size_t errorsBefore = em.errors.size();
        try
        {
          const Ty* type = classes[i].second->type;
          if (const StructInfo* info = em.laidOutClass(type))
          {
            std::string spelling = em.irString(type);
            uint32_t next = 1;
            em.debugComposite(type, *info, 0, &spelling, made[i].definition, next); // (the types its members name join Shared::debugTypeShards)
          }
        }
        catch (...)
        {
          made[i].failed = std::current_exception();
          made[i].errors.assign(em.errors.begin() + errorsBefore, em.errors.end());
          em.errors.resize(errorsBefore);
        }
      }
    };
    if (sh.onEmitters && classes.size() >= 64) // (a few: not worth the threads)
      sh.onEmitters(define);
    else
      define(*this);
    for (size_t i = 0; i < classes.size(); ++i)
    {
      auto& [key, entry] = classes[i];
      if (made[i].failed)
      {
        errors.insert(errors.end(), made[i].errors.begin(), made[i].errors.end());
        std::rethrow_exception(made[i].failed);
      }
      // (A compile server's build, rebuildLLVM's generation, assembles the modules it changed: a
      // class's new definition is its home's change too.)
      if (sh.debugGeneration > 0 && made[i].definition != entry->definition && sh.moduleCount)
      {
        sh.unassembled.resize(sh.moduleCount);
        sh.unassembled[key % sh.moduleCount] = true;
      }
      entry->definition = std::move(made[i].definition);
      if (entry->definition.empty())
        sh.debugClassesDefined.erase(key);
      else
        sh.debugClassesDefined.insert(key);
    }
  }
  // Every class defined (this generation or kept from an earlier one), in key order.
  std::vector<std::pair<uint64_t, const Shared::DebugTypeText*>> defined;
  defined.reserve(sh.debugClassesDefined.size());
  for (uint64_t key : sh.debugClassesDefined)
    defined.push_back({key, sh.findDebugType(key)});
  std::sort(defined.begin(), defined.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  sh.debugHomes.assign(std::max(1u, sh.moduleCount), {});
  sh.debugClasses.clear();
  for (const auto& [key, entry] : defined)
  {
    sh.debugHomes[key % sh.debugHomes.size()].push_back(key);
    if (entry->type)
      sh.debugClasses.push_back({entry->type, debuggerClassName(entry->type)});
  }
  std::sort(sh.debugClasses.begin(), sh.debugClasses.end(), [](const DebugClass& a, const DebugClass& b) { return a.name < b.name; });
  sh.result.debugClasses = sh.debugClasses; // (a build from scratch's result; a compile server's rebuild takes them in rebuildLLVM)
}

auto codegen::Emitter::debugLines(std::string header, size_t declLine) -> std::string
{
  size_t name = header.find('@');
  size_t nameEnd = header[name + 1] == '"' ? header.find('"', name + 2) + 1 : header.find('(', name);
  std::string spName = header[name + 1] == '"' ? header.substr(name + 2, nameEnd - name - 3) : header.substr(name + 1, nameEnd - name - 1);
  header.insert(header.rfind("{\nentry:"), "!dbg " + debugId(0) + " ");
  std::string out = std::move(header);
  out.reserve(out.size() + (allocas.size() + fn.size() + ehBlocks.size()) * 5 / 4);
  bool variables = !debugBlocks.empty() || !debugVariables.empty();
  const uint32_t blocksBase = 1, variablesBase = 2 + uint32_t(debugBlocks.size()); // (1 unused: the file's node was there)
  const uint32_t firstLocation = variables ? variablesBase + uint32_t(debugVariables.size()) : 1;
  auto scope = [&](uint32_t block) { return debugId(block ? blocksBase + block : 0); };
  std::vector<std::pair<size_t, uint32_t>> locations; // id - firstLocation -> its line and block
  std::map<std::pair<size_t, uint32_t>, uint32_t> ids;
  std::pair<size_t, uint32_t> current{declLine, 0};
  std::string currentRef;
  auto idFor = [&](std::pair<size_t, uint32_t> location)
  {
    auto [it, isNew] = ids.emplace(location, firstLocation + uint32_t(ids.size()));
    if (isNew)
      locations.push_back(location);
    return it->second;
  };
  currentRef = ", !dbg " + debugId(idFor(current));
  // A variable's declaration, located at its own line and in its block: after the allocas, or for
  // one whose address an instruction of the body computes (a capture's field), before the
  // instruction after that one (a record goes before an instruction).
  auto declare = [&](size_t i)
  {
    const DebugVariable& v = debugVariables[i];
    std::string expression = v.indirect ? "DW_OP_deref" : "";
    if (v.offset)
      expression += (expression.empty() ? "" : ", ") + std::string("DW_OP_plus_uconst, ") + std::to_string(v.offset);
    return "    #dbg_declare(ptr " + v.address + ", " + debugId(variablesBase + uint32_t(i)) + ", !DIExpression(" + expression + "), " +
           debugId(idFor({v.line ? v.line : declLine, v.block})) + ")\n";
  };
  std::vector<size_t> defined, pending;
  for (size_t i = 0; i < debugVariables.size(); ++i)
    if (debugVariables[i].defined)
      defined.push_back(i);
  for (const std::string* part : {&allocas, &fn, &ehBlocks})
  {
    std::string_view text = *part;
    for (size_t pos = 0; pos < text.size();)
    {
      size_t end = text.find('\n', pos);
      if (end == std::string_view::npos)
        end = text.size();
      std::string_view l = text.substr(pos, end - pos);
      pos = end + 1;
      if (l.starts_with("  ;@"))
      {
        std::string marker(l.substr(4));
        char* rest = nullptr;
        std::pair<size_t, uint32_t> location{strtoull(marker.c_str(), &rest, 10), 0};
        if (rest && *rest == ':')
          location.second = uint32_t(strtoul(rest + 1, nullptr, 10));
        if (location != current)
        {
          current = location;
          currentRef = ", !dbg " + debugId(idFor(location));
        }
        continue;
      }
      bool instruction = l.starts_with("  ") && !l.starts_with("    ") && !l.starts_with("  ;");
      if (instruction && !pending.empty())
      {
        for (size_t i : pending)
          out += declare(i);
        pending.clear();
      }
      out.append(l);
      if (instruction && !l.ends_with("["))
        out += currentRef;
      out += '\n';
      if (instruction)
        for (size_t i : defined)
          if (l.size() > debugVariables[i].address.size() + 5 && l.substr(2, debugVariables[i].address.size()) == debugVariables[i].address &&
              l.substr(2 + debugVariables[i].address.size(), 3) == " = ")
            pending.push_back(i);
    }
    if (part == &allocas)
      for (size_t i = 0; i < debugVariables.size(); ++i)
        if (!debugVariables[i].defined)
          out += declare(i);
  }
  out += "}\n";
  const std::string file = debugFileNode(debugFile);
  // A member function (one with `this`, its artificial first parameter) is its class's, as clang
  // declares one for CodeView: its scope the class's composite, its name without the class's, its type
  // a DISubroutineType whose first parameter is the object pointer (DIFlagArtificial |
  // DIFlagObjectPointer). The PDB then has it as a member function (LF_MFUNC_ID, LF_MFUNCTION), and
  // Visual Studio evaluates `this` and `this->member` in it; as a free function it said "'this' may
  // only be used inside a nonstatic member function" (F's find, 2026-11-05). DWARF keeps the file.
  std::string spName0 = spName, spScope = file, spType = "!2", typeNodes, spDeclaration;
  const DebugVariable* self = nullptr;
  for (const DebugVariable& v : debugVariables)
    if (v.artificial && v.arg == 1 && v.name == "this")
      self = &v;
  if (self && sh.target.windows() && currentClassTy && currentClassTy->isKnown() && !types.isDependent(currentClassTy))
  {
    // (The function's name spells its class as the IR does, `Box<Box<int32>>::get()`; the PDB's
    // name is then the class's debugger name and the rest, `Box<Box<int32> >::get()`.)
    const Ty* cls = types.unqualified(currentClassTy);
    size_t prefix = 0;
    for (const std::string& owner : {irString(cls), debuggerClassName(cls)})
      if (!prefix && spName.size() > owner.size() + 2 && spName.compare(0, owner.size(), owner) == 0 && spName.compare(owner.size(), 2, "::") == 0)
        prefix = owner.size() + 2;
    if (prefix)
    {
      uint32_t extra = firstLocation + uint32_t(locations.size());
      std::string classType = debugType(cls);
      std::string list = currentReturn && currentReturn->isKnown() && currentReturn->kind != TyKind::Void ? debugType(currentReturn) : std::string("null");
      list += ", " + debugObjectPointer(cls);
      std::vector<const DebugVariable*> params;
      for (const DebugVariable& v : debugVariables)
        if (v.arg > 1)
          params.push_back(&v);
      std::stable_sort(params.begin(), params.end(), [](const DebugVariable* a, const DebugVariable* b) { return a->arg < b->arg; });
      for (const DebugVariable* p : params)
        list += ", " + p->type;
      typeNodes = debugId(extra) + " = !DISubroutineType(types: " + debugId(extra + 1) + ")\n" + debugId(extra + 1) + " = !{ " + list + "}\n";
      spName0 = spName.substr(prefix);
      spScope = classType;
      spType = debugId(extra);
      // The declaration in the class's debug information (debugMethod), which the debugger calls.
      if (currentFunction)
      {
        FunctionDecl* declared = global.declarationOf(currentFunction);
        if (!declared)
          declared = currentFunction;
        // (a class's: an enumeration's member function, `Direction::isVertical()` of a real enum,
        // has no class declaration to name, and its type no class layout to ask for)
        if (ClassDecl* c = isClass(cls) && cls->decl ? classDef(cls->decl, nullptr) : nullptr)
          for (size_t i = 0; i < c->members.size(); ++i)
            if (c->members[i] == declared)
              if (std::string method = debugMethod(cls, c, declared, i, global.classArgsOf(canonicalClass(cls, nullptr))); !method.empty())
              {
                // (named as the declaration, `area`, as clang's: the debugger finds the function
                // a watch calls by its class and name; `area() const` was no name it looks for)
                spDeclaration = ", declaration: " + method;
                spName0 = sym(declared->name.last().symbol);
              }
      }
    }
  }
  out += debugId(0) + " = distinct !DISubprogram(name: \"" + spName0 + "\", scope: " + spScope + ", file: " + file + ", line: " +
         std::to_string(declLine) + ", type: " + spType + ", scopeLine: " + std::to_string(declLine) + ", spFlags: DISPFlagDefinition, unit: !0" + spDeclaration +
         ")\n";
  out += typeNodes;
  if (variables)
  {
    for (uint32_t b = 1; b <= debugBlocks.size(); ++b)
      out += debugId(blocksBase + b) + " = distinct !DILexicalBlock(scope: " + scope(debugBlocks[b - 1].parent) + ", file: " + file +
             ", line: " + std::to_string(debugBlocks[b - 1].line ? debugBlocks[b - 1].line : declLine) + ")\n";
    for (size_t i = 0; i < debugVariables.size(); ++i)
    {
      const DebugVariable& v = debugVariables[i];
      out += debugId(variablesBase + uint32_t(i)) + " = !DILocalVariable(name: " + metadataString(v.name) +
             (v.arg ? ", arg: " + std::to_string(v.arg) : std::string()) + ", scope: " + scope(v.block) + ", file: " + file +
             ", line: " + std::to_string(v.line ? v.line : declLine) + ", type: " + v.type +
             (v.artificial ? ", flags: DIFlagArtificial | DIFlagObjectPointer" : "") + ")\n";
    }
  }
  for (size_t k = 0; k < locations.size(); ++k)
    out += debugId(firstLocation + uint32_t(k)) + " = !DILocation(line: " + std::to_string(locations[k].first) + ", scope: " + scope(locations[k].second) + ")\n";
  out += '\n';
  return out;
}
