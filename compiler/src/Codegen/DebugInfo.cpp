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

auto codegen::Emitter::debugType(const Ty* t) -> std::string
{
  std::string spelling = irString(t);
  uint64_t key = 14695981039346656037ull;
  for (unsigned char c : spelling)
    key = (key ^ c) * 1099511628211ull;
  std::string placeholder = debugTypePlaceholder(key);
  {
    auto lock = sharedLock();
    auto [it, isNew] = sh.debugTypes.try_emplace(key);
    if (!isNew && it->second.generation == sh.debugGeneration)
      return placeholder; // (made, or being made: a type that names itself goes through a pointer)
    it->second.generation = sh.debugGeneration;
  }
  std::string text = debugTypeText(t, spelling);
  bool isClass = (t->kind == TyKind::Class || t->kind == TyKind::Lambda) && !t->isConst && !t->isVolatile;
  auto lock = sharedLock();
  Shared::DebugTypeText& made = sh.debugTypes[key];
  made.text = std::move(text);
  made.type = isClass ? t : nullptr;
  return placeholder;
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
  return debuggerTypeName(types.toString(t, &symbols, true));
}

auto codegen::Emitter::debugTypeText(const Ty* t, const std::string& spelling) -> std::string
{
  auto basic = [](const char* name, uint64_t bits, const char* encoding)
  { return std::string("!DIBasicType(name: \"") + name + "\", size: " + std::to_string(bits) + ", encoding: " + encoding + ")"; };
  auto one = [](const std::string& node) { return debugId(0) + " = " + node + "\n"; };
  // (`T* const`'s qualifier is the pointer's: its unqualified type is the pointer.)
  if (const Ty* bare = types.unqualified(t); bare != t)
    return one(std::string("!DIDerivedType(tag: ") + (t->isConst ? "DW_TAG_const_type" : "DW_TAG_volatile_type") + ", baseType: " + debugType(bare) + ")");
  switch (t->kind)
  {
    case TyKind::Bool: return one(basic("bool", 8, "DW_ATE_boolean"));
    case TyKind::Char:
      if (t->bits == 8)
        return one(t->isUnsigned ? basic("unsigned char", 8, "DW_ATE_unsigned_char") : basic("char", 8, "DW_ATE_signed_char"));
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
        const Ty* underlying = enumUnderlying(e);
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
          int64_t value = enumeratorValue(en);
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
      return one("!DISubroutineType(types: !{})");
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
                     " = distinct !DIGlobalVariable(name: " + metadataString(debuggerTypeName(bare)) + ", scope: !0, file: " + debugFileOf(def->file) +
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
  if (!(v->flags & DF_Constexpr) || !(t->kind == TyKind::Int || t->kind == TyKind::Char || t->kind == TyKind::Enum))
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
  std::set<uint64_t> done;
  for (bool more = true; more;)
  {
    more = false;
    std::vector<std::pair<uint64_t, Shared::DebugTypeText*>> entries;
    {
      auto lock = sharedLock();
      for (auto& [key, entry] : sh.debugTypes)
        if (!done.count(key))
          entries.push_back({key, &entry}); // (an element stays where it is as others are added)
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& [key, entry] : entries)
    {
      done.insert(key);
      more = true;
      if (!entry->type || entry->generation != sh.debugGeneration || entry->definitionGeneration == sh.debugGeneration)
        continue;
      entry->definitionGeneration = sh.debugGeneration;
      std::string definition;
      if (const StructInfo* info = laidOutClass(entry->type))
      {
        std::string spelling = irString(entry->type);
        uint32_t next = 1;
        debugComposite(entry->type, *info, 0, &spelling, definition, next); // (the types its members name join debugTypes)
      }
      // (A compile server's build, rebuildLLVM's generation, assembles the modules it changed: a
      // class's new definition is its home's change too.)
      if (sh.debugGeneration > 0 && definition != entry->definition && sh.moduleCount)
      {
        sh.unassembled.resize(sh.moduleCount);
        sh.unassembled[key % sh.moduleCount] = true;
      }
      entry->definition = std::move(definition);
    }
  }
  sh.debugHomes.assign(std::max(1u, sh.moduleCount), {});
  sh.debugClasses.clear();
  for (uint64_t key : done)
    if (auto it = sh.debugTypes.find(key); it != sh.debugTypes.end() && !it->second.definition.empty())
    {
      sh.debugHomes[key % sh.debugHomes.size()].push_back(key);
      if (it->second.type)
        sh.debugClasses.push_back({it->second.type, debuggerClassName(it->second.type)});
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
  const uint32_t fileId = 1, blocksBase = 1, variablesBase = 2 + uint32_t(debugBlocks.size());
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
    return "    #dbg_declare(ptr " + v.address + ", " + debugId(variablesBase + uint32_t(i)) + ", !DIExpression(" + (v.indirect ? "DW_OP_deref" : "") + "), " +
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
  const std::string& file = debugFileOf(debugFile);
  // A member function (one with `this`, its artificial first parameter) is its class's, as clang
  // declares one for CodeView: its scope the class's composite, its name without the class's, its type
  // a DISubroutineType whose first parameter is the object pointer (DIFlagArtificial |
  // DIFlagObjectPointer). The PDB then has it as a member function (LF_MFUNC_ID, LF_MFUNCTION), and
  // Visual Studio evaluates `this` and `this->member` in it; as a free function it said "'this' may
  // only be used inside a nonstatic member function" (F's find, 2026-11-05). DWARF keeps the file.
  std::string spName0 = spName, spScope = file, spType = "!2", typeNodes;
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
      list += ", " + debugId(extra + 2);
      std::vector<const DebugVariable*> params;
      for (const DebugVariable& v : debugVariables)
        if (v.arg > 1)
          params.push_back(&v);
      std::stable_sort(params.begin(), params.end(), [](const DebugVariable* a, const DebugVariable* b) { return a->arg < b->arg; });
      for (const DebugVariable* p : params)
        list += ", " + p->type;
      typeNodes = debugId(extra) + " = !DISubroutineType(types: " + debugId(extra + 1) + ")\n" + debugId(extra + 1) + " = !{ " + list + "}\n" +
                  debugId(extra + 2) + " = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: " + classType +
                  ", size: 64, flags: DIFlagArtificial | DIFlagObjectPointer)\n";
      spName0 = spName.substr(prefix);
      spScope = classType;
      spType = debugId(extra);
    }
  }
  out += debugId(0) + " = distinct !DISubprogram(name: \"" + spName0 + "\", scope: " + spScope + ", file: " + file + ", line: " +
         std::to_string(declLine) + ", type: " + spType + ", scopeLine: " + std::to_string(declLine) + ", spFlags: DISPFlagDefinition, unit: !0)\n";
  out += typeNodes;
  if (variables)
  {
    out += debugId(fileId) + " = " + file + "\n";
    for (uint32_t b = 1; b <= debugBlocks.size(); ++b)
      out += debugId(blocksBase + b) + " = distinct !DILexicalBlock(scope: " + scope(debugBlocks[b - 1].parent) + ", file: " + debugId(fileId) +
             ", line: " + std::to_string(debugBlocks[b - 1].line ? debugBlocks[b - 1].line : declLine) + ")\n";
    for (size_t i = 0; i < debugVariables.size(); ++i)
    {
      const DebugVariable& v = debugVariables[i];
      out += debugId(variablesBase + uint32_t(i)) + " = !DILocalVariable(name: " + metadataString(v.name) +
             (v.arg ? ", arg: " + std::to_string(v.arg) : std::string()) + ", scope: " + scope(v.block) + ", file: " + debugId(fileId) +
             ", line: " + std::to_string(v.line ? v.line : declLine) + ", type: " + v.type +
             (v.artificial ? ", flags: DIFlagArtificial | DIFlagObjectPointer" : "") + ")\n";
    }
  }
  for (size_t k = 0; k < locations.size(); ++k)
    out += debugId(firstLocation + uint32_t(k)) + " = !DILocation(line: " + std::to_string(locations[k].first) + ", scope: " + scope(locations[k].second) + ")\n";
  out += '\n';
  return out;
}
