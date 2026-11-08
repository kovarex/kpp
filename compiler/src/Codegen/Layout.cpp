// struct Emitter's members: sizes, alignment, class layouts, vtables and fields; their declarations
// in Layout.hpp, part of the struct's body in Codegen/Emitter.hpp. A translation unit of its own
// since 2026-11-05 (codegen.cpp split, stage 2), moved without a change.
#include "Emitter.hpp"

auto codegen::Emitter::sizeOf(const Ty* t, Node* at) -> uint64_t
{
  switch (t->kind)
  {
    case TyKind::Bool: return 1;
    case TyKind::Char: case TyKind::Int: case TyKind::Float: return t->bits / 8;
    case TyKind::Nullptr: case TyKind::Pointer: case TyKind::LRef: case TyKind::RRef: case TyKind::Function:
      return 8;
    case TyKind::MemberPointer:
      // A data member's offset (4 bytes in MSVC's ABI, as MSVC and clang for Windows have it, 8 in
      // Itanium's), a function, or { function, adjustment }.
      if (!t->inner || t->inner->kind != TyKind::Function)
        return sh.target.msvc() ? 4 : 8;
      return memberFunctionPointerAdjusts(t, at) ? 16 : 8;
    case TyKind::Lambda:
    {
      // The closure struct's own size: its fields where LLVM puts them (a reference capture is
      // a pointer; fieldTypes holds what it points to), rounded to the largest alignment; an
      // empty closure is one byte.
      if (!t->decl)
        return 8;
      const ClosureInfo& info = closureFor(t, at);
      uint64_t size = 0, align = 1;
      for (size_t i = 0; i < info.fieldTypes.size(); ++i)
      {
        uint64_t s = info.byRef[i] ? 8 : sizeOf(info.fieldTypes[i], at), a = info.byRef[i] ? 8 : llvmAlignOf(info.fieldTypes[i], at);
        size = (size + a - 1) / a * a + s;
        align = std::max(align, a);
      }
      return std::max<uint64_t>((size + align - 1) / align * align, 1);
    }
    case TyKind::Array: return t->count * sizeOf(t->inner, at);
    case TyKind::Vector: return t->count * sizeOf(t->inner, at);
    case TyKind::Enum: return sizeOf(enumUnderlying(t), at);
    case TyKind::Class: return classSizeAndAlign(t, at).first;
    case TyKind::Value:
      // `sizeof(V)` of a value template parameter: its type's (until 2026-11-08 "sizeof 3").
      if (t->inner && t->inner->kind != TyKind::Value)
        return sizeOf(types.stripRef(t->inner), at);
      unsupported(at, "sizeof " + types.toString(t, &symbols));
    default:
      unsupported(at, "sizeof " + types.toString(t, &symbols));
  }
}

auto codegen::Emitter::classSizeAndAlign(const Ty* t, Node* at) -> std::pair<uint64_t, uint64_t>
{
  noteLayout(t);
  if (auto mine = sizeCache.find(t); mine != sizeCache.end())
    return mine->second;
  if (std::pair<uint64_t, uint64_t> known; sh.finalSizes.find(t, known))
    return sizeCache.emplace(t, known).first->second;
  StructInfo& info = structFor(t, at);
  if (info.complete)
  {
    sizeCache.emplace(t, std::pair{info.size, info.align});
    sh.finalSizes.publish(t, {info.size, info.align});
    return {info.size, info.align};
  }
  if (info.align)
    return {info.size, info.align}; // (alignPadding's, final before the layout is)
  return fieldsSize(info, t, at); // a class being laid out: from the fields it has so far
}

auto codegen::Emitter::fieldsSize(const StructInfo& info, const Ty* t, Node* at) -> std::pair<uint64_t, uint64_t>
{
  if (!sizingNow.insert(&info).second)
    unsupported(at, "the class " + types.toString(t, &symbols) + ", which contains itself (a member or base of its own type)");
  struct Sizing
  {
    ~Sizing() { now.erase(info); }

    std::unordered_set<const StructInfo*>& now;
    const StructInfo* info;
  } sizing{sizingNow, &info};
  uint64_t size = 0, align = 1;
  for (size_t i = 0; i < info.fieldTypes.size(); ++i)
  {
    if (i < info.zeroSize.size() && info.zeroSize[i])
      continue; // an empty base: no storage of its own
    const Ty* f = info.fieldTypes[i];
    uint64_t a = info.packed ? 1 : alignOf(f, at);
    size = (size + a - 1) / a * a + sizeOf(f, at);
    align = std::max(align, a);
  }
  align = std::max(align, declaredAlignment(t, at)); // `struct alignas(16) X`
  size = std::max<uint64_t>((size + align - 1) / align * align, 1);
  return {size, align};
}

auto codegen::Emitter::alignOf(const Ty* t, Node* at) -> uint64_t
{
  switch (t->kind)
  {
    case TyKind::Array: return alignOf(t->inner, at);
    case TyKind::MemberPointer: return !t->inner || t->inner->kind != TyKind::Function ? (sh.target.msvc() ? 4 : 8) : 8; // (sizeOf's)
    case TyKind::Vector: return t->count * sizeOf(t->inner, at); // SSE vectors are 16-byte aligned
    case TyKind::Class: return classSizeAndAlign(t, at).second;
    case TyKind::Lambda:
    {
      if (!t->decl)
        return 8;
      const ClosureInfo& info = closureFor(t, at);
      uint64_t align = 1;
      for (size_t i = 0; i < info.fieldTypes.size(); ++i)
        align = std::max(align, info.byRef[i] ? uint64_t(8) : alignOf(info.fieldTypes[i], at));
      return align;
    }
    default: return std::max<uint64_t>(1, sizeOf(t, at));
  }
}

auto codegen::Emitter::declaredAlignment(const Ty* t, Node* at) -> uint64_t
{
  ClassDecl* c = classDef(t->decl, at);
  if (!c->alignment)
    return 1;
  TemplateArgs args = global.classArgsOf(canonicalClass(t, at));
  return alignmentValue(c->alignment, c, args, types.toString(t, &symbols), at);
}

auto codegen::Emitter::alignmentValue(Expr* alignment, Decl* owner, const TemplateArgs& args, const std::string& what, Node* at) -> uint64_t
{
  // `alignas(T)` naming a type (a template's type parameter, a class, an alias): its alignment, as
  // C++'s alignas(alignof(T)) ([dcl.align] 3). The parser cannot tell such a name from a constant's
  // (`alignas(N)`), so the declaration it resolves to does.
  Expr* named = alignment;
  while (named && named->kind == NodeKind::ParenExpr)
    named = static_cast<ParenExpr*>(named)->inner;
  if (Decl* d = named && named->kind == NodeKind::NameExpr && !static_cast<NameExpr*>(named)->name.last().hasTemplateArgs
                  ? static_cast<NameExpr*>(named)->resolved
                  : nullptr)
  {
    const Ty* type = nullptr;
    if (d->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(d)->isType) // (`alignas(N)` of a value parameter: its value, below)
    {
      for (auto& [p, t] : args.pairs)
        if (p == d)
          type = t;
      if (!type && currentArgs)
        for (auto& [p, t] : currentArgs->pairs)
          if (p == d)
            type = t;
    }
    else if (d->kind == NodeKind::ClassDecl || d->kind == NodeKind::EnumDecl || d->kind == NodeKind::UsingAliasDecl ||
             (d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef)))
      type = global.typeOfDecl(d);
    if (type && type->kind != TyKind::Pack && (d->kind != NodeKind::TemplateParamDecl || type->isKnown()))
    {
      if (!type->isKnown() || types.isDependent(type))
        unsupported(at, "the alignment of " + what + ", alignas of a type not known here");
      return alignOf(type, at);
    }
  }
  ArgsScope scope(*this, &args);
  ConstContextScope context(*this, owner);
  ParsedFile* saved = currentFile;
  currentFile = owner && owner->file ? owner->file : currentFile;
  std::optional<int64_t> v = evalConst(alignment);
  currentFile = saved;
  if (!v || *v <= 0 || (*v & (*v - 1)))
    unsupported(at, "the alignment of " + what + ", which is not a constant power of two");
  return uint64_t(*v);
}

auto codegen::Emitter::llvmAlignOf(const Ty* t, Node* at) -> uint64_t
{
  switch (t->kind)
  {
    case TyKind::Array: return llvmAlignOf(t->inner, at);
    case TyKind::Class: return structFor(t, at).llvmAlign;
    default: return alignOf(t, at);
  }
}

auto codegen::Emitter::alignSuffix(const Ty* t, uint64_t declared, Node* at) -> std::string
{
  t = types.unqualified(types.stripRef(t));
  if (!declared && !overAlignedType(t, at))
    return "";
  uint64_t want = std::max(declared, alignOf(t, at));
  return want > llvmAlignOf(t, at) ? ", align " + std::to_string(want) : std::string();
}

auto codegen::Emitter::overAlignedType(const Ty* t, Node* at) -> bool
{
  t = types.unqualified(t);
  while (t->kind == TyKind::Array)
    t = types.unqualified(t->inner);
  return t->kind == TyKind::Class && structFor(t, at).overAligned;
}

auto codegen::Emitter::alignPadding(StructInfo& info, ClassDecl* c, const Ty* key, const TemplateArgs& args, Node* at) -> uint64_t
{
  std::vector<uint64_t> declaredAt(info.fieldTypes.size(), 0); // a member's own alignas
  for (Decl* m : c->members)
    if (m->kind == NodeKind::VariableDecl && static_cast<VariableDecl*>(m)->alignment && info.fieldIndex.count(m))
    {
      uint32_t i = info.fieldIndex[m];
      declaredAt[i] = std::max(declaredAt[i], alignmentValue(static_cast<VariableDecl*>(m)->alignment, c, args,
                                                             "the member " + sym(static_cast<VariableDecl*>(m)->name.last().symbol), at));
    }
  // A C struct under #pragma pack(n): a member's alignment is at most n, its padding then written
  // out in a packed LLVM struct. A required alignment (__declspec(align), an SSE vector) stays on
  // MSVC's layout, and gives way to n on Itanium's (clang's two record layout builders). Where no
  // member is aligned to more than n, the pack changes nothing and the layout stays the plain one
  // (most of windows.h is under pack(8)).
  auto alignmentUnderPack = [&](size_t i)
  {
    uint64_t own = std::max(alignOf(info.fieldTypes[i], at), declaredAt[i]);
    uint64_t required = sh.target.msvc() ? std::max(requiredAlignment(info.fieldTypes[i], at), declaredAt[i]) : 1;
    return std::max(std::min<uint64_t>(own, c->pack), required);
  };
  if (c->pack)
  {
    for (size_t i = 0; i < info.fieldTypes.size(); ++i)
      if (!info.zeroSize[i] && alignmentUnderPack(i) < std::max(alignOf(info.fieldTypes[i], at), declaredAt[i]))
        info.packedTo = c->pack;
    if (!sh.target.windows() && !info.bitfields.empty() && (info.packedTo || info.bitFieldAlign > c->pack))
      fail(at, "the layout of " + types.toString(key, &symbols) + ": bit fields under #pragma pack off Windows are not supported");
    info.packed = info.packed || info.packedTo;
  }
  uint64_t natural = 1;
  for (size_t i = 0; i < info.fieldTypes.size(); ++i)
    if (!info.zeroSize[i] && !info.packed)
      natural = std::max(natural, llvmAlignOf(info.fieldTypes[i], at));
  info.llvmAlign = natural;
  if (info.packed && !info.packedTo)
    return 0;
  bool needed = info.packedTo != 0;
  for (size_t i = 0; i < info.fieldTypes.size(); ++i)
    needed = needed || declaredAt[i] > llvmAlignOf(info.fieldTypes[i], at);
  // The field after a zero-width bit field at its boundary (MSVC's: the layout's zeroWidthAligned).
  for (auto [index, boundary] : info.zeroWidthAligned)
    if (index < declaredAt.size())
    {
      declaredAt[index] = std::max(declaredAt[index], boundary);
      needed = needed || boundary > llvmAlignOf(info.fieldTypes[index], at);
    }
  for (size_t i = 0; i < info.fieldTypes.size() && !needed; ++i)
    needed = !info.zeroSize[i] && overAlignedType(info.fieldTypes[i], at);
  // MSVC's layout of a class with a vtable pointer of its own: its bases and members are pushed back
  // by the pointer's size rounded up to their largest alignment, not merely laid after it
  // (clang's MicrosoftRecordLayoutBuilder::injectVFPtr): with `char c; alignas(16) double d;` c is at
  // 16 and d at 32, where the pointer's 8 bytes put c at 8 (F's battery f9, 2026-11-06). The
  // first part after the pointer starts there (each later one keeps its place relative to it).
  // Itanium's (the other targets, windows-gnu's too) lays them after the pointer.
  uint64_t vptrShift = 0;
  size_t firstAfterVptr = info.fieldTypes.size();
  if (sh.target.msvc() && info.polymorphic && !info.primaryBase && !info.fieldTypes.empty())
  {
    uint64_t largest = 1;
    size_t vptrField = 0; // (after any [[no_unique_address]] members, which take none of its bytes)
    while (info.zeroSize[vptrField])
      ++vptrField;
    for (size_t i = vptrField + 1; i < info.fieldTypes.size(); ++i)
      if (!info.zeroSize[i])
      {
        largest = std::max({largest, alignOf(info.fieldTypes[i], at), declaredAt[i]});
        firstAfterVptr = std::min(firstAfterVptr, i);
      }
    uint64_t pointer = sizeOf(info.fieldTypes[vptrField], at);
    vptrShift = (pointer + largest - 1) / largest * largest;
    if (vptrShift > pointer && firstAfterVptr < info.fieldTypes.size())
      needed = true;
    else
      vptrShift = 0;
  }
  uint64_t declaredClass = std::max<uint64_t>(c->alignment ? declaredAlignment(key, at) : 1, info.bitFieldAlign);
  if (!needed && declaredClass <= natural)
    return 0;
  std::vector<const Ty*> fields;
  std::vector<bool> zero;
  std::vector<uint32_t> newIndex(info.fieldTypes.size());
  uint64_t offset = 0, align = 1;
  for (size_t i = 0; i < info.fieldTypes.size(); ++i)
  {
    bool none = info.zeroSize[i];
    uint64_t want = none ? 1 : info.packedTo ? alignmentUnderPack(i) : std::max(alignOf(info.fieldTypes[i], at), declaredAt[i]);
    uint64_t llvmWant = none || info.packedTo ? 1 : llvmAlignOf(info.fieldTypes[i], at);
    uint64_t place = (offset + want - 1) / want * want;
    if (i == firstAfterVptr && vptrShift)
      place = std::max(place, vptrShift); // (MSVC's push back after its own vtable pointer, above)
    if (place != (offset + llvmWant - 1) / llvmWant * llvmWant)
    {
      fields.push_back(types.array(types.intType(8), uint32_t(place - offset)));
      zero.push_back(false);
    }
    newIndex[i] = uint32_t(fields.size());
    fields.push_back(info.fieldTypes[i]);
    zero.push_back(none);
    offset = place + (none ? 0 : sizeOf(info.fieldTypes[i], at));
    align = std::max(align, want);
  }
  if (info.empty)
    offset = std::max<uint64_t>(offset, 1); // its byte of its own
  align = std::max(align, declaredClass);
  uint64_t size = std::max<uint64_t>((offset + align - 1) / align * align, 1);
  info.fieldTypes = std::move(fields);
  info.zeroSize = std::move(zero);
  for (auto& [d, index] : info.fieldIndex)
    index = newIndex[index];
  for (auto& [d, index] : info.bases)
    index = newIndex[index];
  for (auto& [d, index] : info.anonymous)
    index = newIndex[index];
  info.size = size;
  info.align = align;
  info.overAligned = align > natural;
  uint64_t llvmSize = (offset + natural - 1) / natural * natural;
  return size > llvmSize ? size - offset : 0;
}

auto codegen::Emitter::variableAlignment(VariableDecl* v, Decl* context, Node* at) -> uint64_t
{
  if (!v || !v->alignment)
    return 0;
  return alignmentValue(v->alignment, context, currentArgs ? *currentArgs : TemplateArgs{}, sym(v->name.last().symbol), at);
}

auto codegen::Emitter::declaredOnly(const Ty* t) -> bool
{
  t = classKey(t);
  if (!t || !t->decl || isLambda(t))
    return false; // (a closure type is defined by its lambda: its synthetic class is declared only)
  Decl* def = global.definitionOf(t->decl);
  return !def || def->kind != NodeKind::ClassDecl;
}

auto codegen::Emitter::classDef(Decl* decl, Node* at) -> ClassDecl*
{
  Decl* def = global.definitionOf(decl);
  if (!def || def->kind != NodeKind::ClassDecl)
    fail(at, qualifiedName(decl) + " is only declared (no definition in the program), and this use needs its layout");
  auto* c = static_cast<ClassDecl*>(def);
  // An imported C struct the C parser could not read in full: its layout without what it skipped
  // would silently be another than C's.
  if (c->importError)
    fail(at, qualifiedName(decl) + ": the C import could not read its definition (" + std::string(c->importError) +
                 (c->importErrorAt != UINT32_MAX ? " at `" + sym(c->importErrorAt) + "`" : std::string()) + "), and this use needs its layout");
  return c;
}

auto codegen::Emitter::canonicalClass(const Ty* t, Node* at) -> const Ty*
{
  t = classKey(t);
  if (isLambda(t))
    return t; // (a closure is its own: its class is synthetic, declared only)
  ClassDecl* def = classDef(t->decl, at);
  std::vector<const Ty*> args;
  bool changed = def != t->decl;
  for (const Ty* a : t->args)
  {
    args.push_back(definitionsIn(a));
    changed = changed || args.back() != a;
  }
  return changed ? types.classType(def, Span<const Ty*>{args.data(), uint32_t(args.size())}) : t;
}

auto codegen::Emitter::definitionsIn(const Ty* t, int depth) -> const Ty*
{
  if (!t || depth > 32)
    return t;
  if (auto it = definitionsCache.find(t); it != definitionsCache.end())
    return it->second;
  const Ty* u = types.unqualified(t);
  const Ty* result = u;
  switch (u->kind)
  {
    case TyKind::Pointer: result = types.pointer(definitionsIn(u->inner, depth + 1)); break;
    case TyKind::LRef: result = types.lref(definitionsIn(u->inner, depth + 1)); break;
    case TyKind::RRef: result = types.rref(definitionsIn(u->inner, depth + 1)); break;
    case TyKind::Array: result = u->decl ? u : types.array(definitionsIn(u->inner, depth + 1), u->count); break;
    case TyKind::Class:
      if (!u->isBraceList())
      {
        Decl* def = u->decl ? global.definitionOf(u->decl) : nullptr;
        std::vector<const Ty*> args;
        bool changed = def && def != u->decl && def->kind == NodeKind::ClassDecl;
        for (const Ty* a : u->args)
        {
          args.push_back(definitionsIn(a, depth + 1));
          changed = changed || args.back() != a;
        }
        if (changed)
          result = types.classType(def && def->kind == NodeKind::ClassDecl ? def : u->decl, Span<const Ty*>{args.data(), uint32_t(args.size())});
      }
      break;
    default: break;
  }
  if (result != u && (t->isConst || t->isVolatile))
    result = types.qualified(result, t->isConst, t->isVolatile);
  else if (result == u)
    result = t;
  definitionsCache.emplace(t, result);
  return result;
}

auto codegen::Emitter::structFor(const Ty* classTy, Node* at) -> StructInfo&
{
  if (auto mine = layoutCache.find(classTy); mine != layoutCache.end())
    return checkedLayout(*mine->second, classTy, at);
  if (StructInfo* known; sh.finalLayouts.find(classTy, known))
    return checkedLayout(*layoutCache.emplace(classTy, known).first->second, classTy, at);
  StructInfo* info = nullptr;
  {
    std::lock_guard lock(sh.structsMutex);
    if (auto known = structsByType.find(classTy); known != structsByType.end())
      info = known->second;
  }
  if (!info)
  {
    const Ty* key = canonicalClass(classTy, at);
    if (auto own = privateLayouts.find(key); own != privateLayouts.end())
      return checkedLayout(own->second, classTy, at); // (this emitter's copy, being built)
    {
      std::lock_guard lock(sh.structsMutex);
      if (auto it = structs.find(key); it != structs.end())
      {
        structsByType.emplace(classTy, &it->second);
        info = &it->second;
      }
    }
    if (!info)
      info = &buildAndPublish(key, classTy, at);
  }
  layoutCache.emplace(classTy, info);
  sh.finalLayouts.publish(classTy, info);
  return checkedLayout(*info, classTy, at);
}

auto codegen::Emitter::buildAndPublish(const Ty* key, const Ty* classTy, Node* at) -> StructInfo&
{
  StructInfo& mine = privateLayouts.try_emplace(key).first->second;
  std::string typeLine;
  size_t errorsBefore = errors.size();
  try
  {
    buildStruct(key, at, mine, typeLine);
  }
  catch (Unsupported&)
  {
    mine.failed = true;
    mine.failedWhy = lastFailure;
    mine.failedShown = lastFailureShown;
    errors.resize(errorsBefore);
  }
  catch (...)
  {
    privateLayouts.erase(key); // (no half-built copy left for a later use to find)
    throw;
  }
  auto node = privateLayouts.extract(key);
  StructInfo* published = nullptr;
  bool first = false;
  {
    std::lock_guard lock(sh.structsMutex);
    auto inserted = structs.insert(std::move(node));
    first = inserted.inserted;
    published = &inserted.position->second;
    structsByType.emplace(classTy, published);
    if (first && !typeLine.empty())
      ++result.structs;
    if (!first)
      droppedLayouts.push_back(std::move(inserted.node));
  }
  if (first && !typeLine.empty())
    decls += typeLine;
  if (first && published->failed)
    ++sh.failuresRemembered;
  return *published;
}

auto codegen::Emitter::checkedLayout(StructInfo& info, const Ty* classTy, Node* at) -> StructInfo&
{
  noteLayout(classTy);
  if (info.failed)
    fail(at, info.failedWhy.empty() ? "codegen does not support the class " + types.toString(classKey(classTy), &symbols) + ", whose layout failed yet" : info.failedWhy, info.failedShown);
  return info;
}

auto codegen::Emitter::draftHeld(const Ty* t) -> const Ty*
{
  for (; t; t = t->kind == TyKind::Array ? t->inner : nullptr)
    if (t->kind == TyKind::Class && t->decl && (t->decl->attributes & ATTR_Draft))
      return t;
  return nullptr;
}

auto codegen::Emitter::basesOf(ClassDecl* c, const TemplateArgs& args, Node* at, std::vector<bool>* isPublic) -> std::vector<const Ty*>
{
  std::vector<const Ty*> out;
  for (const ClassDecl::Base& b : c->bases)
  {
    const Ty* bt = global.typeOfAstIn(b.type, c, args);
    bool baseIsPublic = b.access == Access::Public || (b.access == Access::Default && c->keyword != Kw::class_);
    if (!b.isPack)
    {
      out.push_back(bt);
      if (isPublic)
        isPublic->push_back(baseIsPublic);
      continue;
    }
    // `struct overloaded : Ts...` (the visitor idiom): a base per element of the pack, in order.
    if (!bt || bt->kind != TyKind::Pack)
      unsupported(at, "a pack of base classes whose pack is not known here (" + (bt ? types.toString(bt, &symbols) : std::string("?")) + ")");
    for (const Ty* element : bt->args)
    {
      out.push_back(element);
      if (isPublic)
        isPublic->push_back(baseIsPublic);
    }
  }
  return out;
}

auto codegen::Emitter::buildStruct(const Ty* key, Node* at, StructInfo& info, std::string& typeLine) -> void
{
  if (isLambda(key))
  {
    // A closure as a class, the base of one (the overload-set idiom's `struct Overload : Ts...` given
    // lambdas): its fields its captures as the closure's own struct type has them (a reference capture
    // a pointer), no base, no vtable, and its struct type the closure's, which closureFor declares (so
    // no type line here). A closure without captures is empty: no storage as a base.
    const ClosureInfo& closure = closureFor(key, at);
    info.name = closure.structName;
    for (size_t i = 0; i < closure.fieldTypes.size(); ++i)
      info.fieldTypes.push_back(closure.byRef[i] ? types.pointer(closure.fieldTypes[i]) : closure.fieldTypes[i]);
    info.empty = closure.fieldTypes.empty();
    info.size = sizeOf(key, at);
    info.align = alignOf(key, at);
    info.llvmAlign = info.align;
    info.complete = true;
    return;
  }
  ClassDecl* c = static_cast<ClassDecl*>(key->decl);
  auto layoutError = [&](const std::string& why) { unsupported(at, "the layout of " + types.toString(key, &symbols) + ": " + why); };
  // A member as written (`Tick lastCheck`), for the error naming what has no known type.
  auto memberName = [&](Decl* m)
  {
    std::string name = m->kind == NodeKind::VariableDecl && !static_cast<VariableDecl*>(m)->name.empty() ? sym(static_cast<VariableDecl*>(m)->name.last().symbol) : std::string("?");
    if (!m->file || !m->file->tokens || m->endToken <= m->token || m->endToken > m->file->tokens->size())
      return name;
    const std::vector<Token>& toks = *m->file->tokens;
    size_t from = toks[m->token].offset, to = toks[m->endToken - 1].offset + toks[m->endToken - 1].length;
    std::string written;
    for (char ch : m->file->text.substr(from, std::min<size_t>(to - from, 160)))
      if (ch != '\n' && ch != '\r' && !(ch == ' ' && !written.empty() && written.back() == ' '))
        written += ch == '\t' ? ' ' : ch;
    return name + " (`" + written + "`)";
  };
  // A type name declared nowhere is the resolver's placeholder class (a draft, so that checking
  // goes on): an error wherever a layout needs it, never an empty class that compiles.
  // (Not shown again when the checker reported the name where it is written.)
  if (c->attributes & ATTR_Draft)
    fail(at, "the type '" + global.draftName(c) + "' is not declared", !global.undeclaredReported(global.draftName(c)));
  auto undeclared = [&](const std::string& what, const Ty* draft)
  {
    fail(at, "the layout of " + types.toString(key, &symbols) + ": " + what + " names '" + global.draftName(draft->decl) + "', which is not declared",
         !global.undeclaredReported(global.draftName(draft->decl)));
  };
  // `Imm<strlen("x")>`: an argument the checker could not work out (a constant it could not
  // evaluate) is no argument to lay out with; taken for 0, `char data[N]` was an empty array
  // that writes ran past.
  for (size_t i = 0; i < key->args.size(); ++i)
    if (!key->args[i] || !key->args[i]->isKnown())
    {
      Span<Decl*> params = key->decl->templateParams;
      auto* p = i < params.size() ? static_cast<TemplateParamDecl*>(params[i]) : nullptr;
      std::string which = p && p->name != UINT32_MAX ? "the argument for " + sym(p->name) : "argument " + std::to_string(i + 1);
      fail(at, "the layout of " + types.toString(key, &symbols) + ": " + which +
                   (p && p->isType ? " is a type the compiler could not resolve" : " is a constant the compiler could not evaluate"));
    }
  TemplateArgs args = global.classArgsOf(key);
  if (c->keyword == Kw::union_)
  {
    // A union is one block of storage; every member lives at its start.
    info.name = irTypeName("%struct.", key);
    info.isUnion = true;
    uint64_t size = 1, align = 1;
    for (Decl* m : c->members)
    {
      const Ty* ft = nullptr;
      if (m->kind == NodeKind::VariableDecl && !(m->flags & (DF_Static | DF_Typedef | DF_Friend)))
      {
        if (!whenMemberHolds(m, c, args, key, at))
          continue; // (a `when` member this instance does not have)
        ft = global.typeOfDeclIn(m, args);
      }
      else if (m->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(m)->name.empty() && static_cast<ClassDecl*>(m)->isDefinition)
        ft = types.classType(m, key->args); // an anonymous struct takes the enclosing instantiation's arguments
      else
        continue;
      info.fieldIndex[m] = 0;
      info.fieldDecls.push_back(m);
      if (!ft || !ft->isKnown() || types.isDependent(ft))
        layoutError("the member " + memberName(m) + " has an unknown type");
      if (const Ty* draft = draftHeld(ft))
        undeclared("the member " + memberName(m), draft);
      for (const Ty* a = types.unqualified(ft); a->kind == TyKind::Array; a = types.unqualified(a->inner))
        if (a->count == 0 && !(c->file && c->file->isImported))
          fail(at, "the layout of " + types.toString(key, &symbols) + ": the member " + memberName(m) + " is an array without a length in a union (no flexible array member there)");
      if (m->kind == NodeKind::ClassDecl)
        info.anonymous.push_back({m, 0});
      size = std::max(size, sizeOf(ft, at));
      align = std::max(align, alignOf(ft, at));
      info.requiredAlign = std::max(info.requiredAlign, requiredAlignment(ft, at));
      if (m->kind == NodeKind::VariableDecl && static_cast<VariableDecl*>(m)->alignment)
      {
        uint64_t declared = alignmentValue(static_cast<VariableDecl*>(m)->alignment, c, args, "the member " + memberName(m), at);
        align = std::max(align, declared);
        info.requiredAlign = std::max(info.requiredAlign, declared);
      }
    }
    info.requiredAlign = std::max(info.requiredAlign, c->alignment ? declaredAlignment(key, at) : 1);
    // #pragma pack(n): as in a struct (alignPadding).
    if (c->pack)
      align = std::max(std::min<uint64_t>(align, c->pack), sh.target.msvc() ? info.requiredAlign : 1);
    // A [[packed]] union (ImmutableString's, in a packed class: MSVC's #pragma pack(1)) is as
    // large as its largest member, aligned to 1: no padding up to a member's alignment.
    if (c->attributes & ATTR_Packed)
      align = 1;
    // The storage: integers of the union's alignment, up to i128 (LLVM's most aligned integer);
    // a member aligned to more (OptionalOwningStorage of an alignas(64) class) makes the union
    // over-aligned, its objects then aligned with `align`.
    uint64_t unit = std::min<uint64_t>(align, 16), total = (size + align - 1) / align * align;
    info.llvmAlign = unit;
    info.fieldTypes.push_back(types.array(types.intType(uint8_t(unit * 8)), uint32_t(total / unit)));
    if (align > unit)
    {
      info.size = total;
      info.align = align;
      info.overAligned = true;
    }
    typeLine = info.name + " = type { " + llType(info.fieldTypes[0], c) + " }\n";
    if (!info.align) // (an over-aligned union's are set above)
      std::tie(info.size, info.align) = fieldsSize(info, key, at);
    info.complete = true;
    return;
  }
  info.name = irTypeName("%struct.", key);
  // Bases first (resolved before deciding on the vtable pointer).
  std::vector<const Ty*> baseKeys;
  for (const Ty* bt : basesOf(c, args, at, &info.basePublic))
  {
    // (a closure as a base, the overload-set idiom's `struct Overload : Ts...` given lambdas: laid
    // out as buildStruct lays a closure out)
    if (!bt || (bt->kind != TyKind::Class && !isLambda(bt)) || types.isDependent(bt))
      layoutError("a base class whose type is unknown (" + (bt ? types.toString(bt, &symbols) : std::string("?")) + ")");
    if (const Ty* draft = draftHeld(bt))
      undeclared("a base class", draft);
    const Ty* baseKey = canonicalClass(bt, at);
    if (structFor(baseKey, at).flexible)
      fail(at, "the layout of " + types.toString(key, &symbols) + ": the base " + types.toString(baseKey, &symbols) +
                   " has a flexible array member, whose elements this class's members would overlap");
    baseKeys.push_back(baseKey);
  }
  // The primary base is the first polymorphic one: at offset 0, its vtable pointer the class's
  // own. A polymorphic class without a polymorphic base has a vtable pointer of its own there.
  // The polymorphic bases are laid out first, then the others (MSVC's layout, design.md section
  // 9); info.bases keeps declaration order, the order bases are built in.
  int primary = -1;
  for (size_t i = 0; i < baseKeys.size() && primary < 0; ++i)
    if (structFor(baseKeys[i], at).polymorphic)
      primary = int(i);
  bool ownVptr = false;
  if (primary < 0)
    for (Decl* m : c->members)
      if (m->kind == NodeKind::FunctionDecl && (m->flags & (DF_Virtual | DF_Override)) && !(m->flags & (DF_Static | DF_Constructor)) &&
          m->templateParams.empty())
        ownVptr = true;
  info.polymorphic = primary >= 0 || ownVptr;
  info.primaryBase = primary >= 0 ? baseKeys[primary] : nullptr;
  // [[no_unique_address]] members of an empty class take no storage, as an empty base (design.md 9):
  // zero-size fields first, so at offset 0, inside the object, where Itanium's layout puts them too.
  // One whose class another part of this class holds keeps its byte: two objects of one type have
  // two addresses. So does one aligned to more than a byte, which a field of no size could not align
  // the class to. Their fields come first, their places in fieldDecls stay the declaration's.
  std::unordered_set<Decl*> overlapping = overlappingMembers(c, key, args, baseKeys, at);
  for (Decl* m : c->members)
    if (overlapping.count(m))
    {
      info.fieldIndex[m] = uint32_t(info.fieldTypes.size());
      info.fieldTypes.push_back(global.typeOfDeclIn(m, args));
    }
  const size_t overlappingFields = info.fieldTypes.size();
  // The class of the first data member that takes storage, or null (an empty base of that class
  // keeps its byte).
  auto firstMemberClass = [&]() -> const Ty*
  {
    for (Decl* m : info.fieldDecls)
      if (!overlapping.count(m))
        return classKey(info.fieldTypes[fieldIndexOf(info, m)]);
    return nullptr;
  };
  if (ownVptr)
    info.fieldTypes.push_back(types.pointer(types.voidType()));
  std::vector<uint32_t> baseIndex(baseKeys.size());
  for (bool polymorphicPass : {true, false})
    for (size_t i = 0; i < baseKeys.size(); ++i)
      if (structFor(baseKeys[i], at).polymorphic == polymorphicPass)
      {
        baseIndex[i] = uint32_t(info.fieldTypes.size());
        info.fieldTypes.push_back(baseKeys[i]);
      }
  for (size_t i = 0; i < baseKeys.size(); ++i)
    info.bases.push_back({baseKeys[i]->decl, baseIndex[i]});
  int bitUnitIndex = -1;    // the storage field the current run of bit fields shares
  uint32_t bitUnitBits = 0; // its size in bits
  uint32_t bitUsed = 0;
  // Off Windows bit fields are laid out by SysV's rules (the target's C: design.md's targets
  // table; 2026-11-05, MSVC's until then): sysvBits. Its state: the next free bit while a run of
  // bit fields goes on (sysvRun), the storage unit open (sysvUnit: its field, its first and end
  // byte) and its bit fields with their first bits.
  bool sysvBits = !sh.target.windows() && !(c->attributes & ATTR_Packed);
  bool sysvRun = false;
  uint64_t sysvNextBit = 0;
  int sysvUnit = -1;
  uint64_t sysvUnitStart = 0, sysvUnitEnd = 0;
  std::vector<std::pair<Decl*, uint64_t>> sysvUnitFields;
  // Where the layout's fields end, as LLVM places them (an empty base takes no byte unless the
  // first data member is of its class).
  auto layoutEnd = [&]() -> uint64_t
  {
    const Ty* firstMember = firstMemberClass();
    uint64_t end = 0;
    for (size_t i = overlappingFields; i < info.fieldTypes.size(); ++i)
    {
      const Ty* f = info.fieldTypes[i];
      bool base = false;
      for (auto& [baseDef, index] : info.bases)
        base = base || index == i;
      if (base && structFor(f, at).empty && classKey(f) != firstMember)
        continue; // (zero size, as below)
      uint64_t a = llvmAlignOf(f, at);
      end = (end + a - 1) / a * a + sizeOf(f, at);
    }
    return end;
  };
  auto padTo = [&](uint64_t byte)
  {
    uint64_t end = layoutEnd();
    if (byte > end)
      info.fieldTypes.push_back(types.array(types.intType(8, true), uint32_t(byte - end)));
  };
  // The open storage unit closed: its byte array, and each of its bit fields read as an integer of
  // the unit's bytes from its first byte.
  auto closeUnit = [&]()
  {
    if (sysvUnit < 0)
      return;
    uint32_t bytes = uint32_t(sysvUnitEnd - sysvUnitStart);
    info.fieldTypes[sysvUnit] = types.array(types.intType(8, true), bytes);
    for (auto& [f, bit] : sysvUnitFields)
    {
      BitfieldInfo& b = info.bitfields[f];
      b.shift = uint32_t(bit - sysvUnitStart * 8);
      b.unit = types.intType(uint8_t(bytes * 8), true);
      b.bytes = true;
    }
    sysvUnit = -1;
    sysvUnitFields.clear();
  };
  // A field that is no bit field after a run of them: it starts at the byte after their last bit
  // (or the boundary a zero-width one padded to).
  auto endRun = [&]()
  {
    if (!sysvRun)
      return;
    closeUnit();
    padTo((sysvNextBit + 7) / 8);
    sysvRun = false;
  };
  for (Decl* m : c->members)
  {
    if (m->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(m)->name.empty() && static_cast<ClassDecl*>(m)->isDefinition)
    {
      // Anonymous struct/union member: a field whose own members are reachable by name, laid
      // out under the enclosing instantiation's arguments (ImmutableStringTemplate<N>'s union
      // holding a LongStr<N>).
      const Ty* ft = types.classType(m, key->args);
      structFor(ft, at);
      endRun();
      info.anonymous.push_back({m, uint32_t(info.fieldTypes.size())});
      info.fieldIndex[m] = uint32_t(info.fieldTypes.size());
      info.fieldDecls.push_back(m);
      info.fieldTypes.push_back(ft);
      bitUnitIndex = -1;
      continue;
    }
    if (m->kind != NodeKind::VariableDecl || (m->flags & (DF_Static | DF_Typedef | DF_Friend)))
      continue;
    // A `when` member on the template's parameters is in this instance's layout only where its
    // condition holds (design.md 19); two of one name that both hold are a clash.
    if (!whenMemberHolds(m, c, args, key, at))
      continue;
    if (!static_cast<VariableDecl*>(m)->name.empty())
      for (Decl* f : info.fieldDecls)
        if ((m->when || f->when) && f->kind == NodeKind::VariableDecl && !static_cast<VariableDecl*>(f)->name.empty() &&
            static_cast<VariableDecl*>(f)->name.last().symbol == static_cast<VariableDecl*>(m)->name.last().symbol)
          fail(at, "the layout of " + types.toString(key, &symbols) + ": two `when` declarations of the member " + memberName(m) +
                       " exist at once, a clash (design.md 19)");
    if (overlapping.count(m))
    {
      info.fieldDecls.push_back(m); // (its field is among the first)
      continue;
    }
    const Ty* ft = global.typeOfDeclIn(m, args);
    if (!ft || !ft->isKnown() || types.isDependent(ft))
      layoutError("the member " + memberName(m) + " has an unknown type");
    if (const Ty* draft = draftHeld(ft))
      undeclared("the member " + memberName(m), draft);
    // `char data[N]` with N no constant the checker could evaluate has the length 0; only a
    // struct's last data member may be such an array, written without a length (`char data[];`,
    // a flexible array member), after another data member (C's rules; not in a union, above).
    for (const Ty* a = types.unqualified(ft); a && a->kind == TyKind::Array; a = types.unqualified(a->inner))
      if (a->count == 0 && !(c->file && c->file->isImported))
      {
        auto* declared = static_cast<VariableDecl*>(m)->type;
        bool unbounded = a == types.unqualified(ft) && declared && declared->kind == NodeKind::ArrayType && !static_cast<ArrayType*>(declared)->size;
        if (!unbounded)
          fail(at, "the layout of " + types.toString(key, &symbols) + ": the member " + memberName(m) +
                       " is an array whose length is no constant the compiler could evaluate");
        bool last = true;
        for (auto after = std::find(c->members.begin(), c->members.end(), m) + 1; after != c->members.end(); ++after)
          last = last && !((*after)->kind == NodeKind::VariableDecl && !((*after)->flags & (DF_Static | DF_Typedef | DF_Friend))) &&
                 !((*after)->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(*after)->name.empty() && static_cast<ClassDecl*>(*after)->isDefinition);
        if (!last || info.fieldDecls.empty() || static_cast<VariableDecl*>(m)->bitfield)
          fail(at, "the layout of " + types.toString(key, &symbols) + ": the member " + memberName(m) + " is a flexible array member " +
                      (!last ? "before another data member" : info.fieldDecls.empty() ? "with no data member before it" : "with a bit width") +
                      " (a flexible array member is the last data member of a struct with others)");
        info.flexible = true;
      }
    // A lambda held by value (FunctionImpl<F>'s `F f`, TransformView's): its closure, laid out
    // from the lambda's AST whoever asks first; one that cannot be laid out fails this class.
    for (const Ty* held = types.unqualified(ft); held; held = held->kind == TyKind::Array ? types.unqualified(held->inner) : nullptr)
      if (isLambda(held))
        closureFor(held, at);
    if (Expr* widthExpr = static_cast<VariableDecl*>(m)->bitfield)
    {
      // `unsigned level : 3;`: consecutive bit fields share a storage unit while their types
      // have one size and their widths fit (MSVC: `uint8_t x : 5; bool on : 1;` share a byte,
      // `int a : 3; unsigned b : 4;` four bytes); each remembers its shift and width.
      // The width under the class's arguments, in its context: not the arguments of whichever
      // function asked for the layout first.
      ParsedFile* savedFile = currentFile;
      currentFile = c->file ? c->file : currentFile;
      std::optional<int64_t> w;
      {
        ArgsScope widthArgs(*this, &args);
        ConstContextScope widthContext(*this, c);
        w = evalConst(widthExpr);
      }
      currentFile = savedFile;
      if (!w)
        unsupported(m, "a bit field whose width is not a constant");
      if (*w < 0)
        fail(m, "the layout of " + types.toString(key, &symbols) + ": the bit field " + memberName(m) + " has a negative width (" + std::to_string(*w) + ")");
      const Ty* unit = types.unqualified(ft);
      if (unit->kind == TyKind::Enum)
        unit = enumUnderlying(unit);
      if (unit->kind == TyKind::Bool)
        unit = types.intType(8, true);
      if (sysvBits)
      {
        // SysV: the next free bit, unless the field would straddle a boundary of its type's size
        // (or is of width 0: it pads to its type's alignment, and holds nothing); bit fields
        // sharing a byte share a storage unit; a named one gives the class its type's alignment.
        uint64_t typeBits = sizeOf(unit, m) * 8, alignBits = alignOf(unit, m) * 8;
        uint64_t bit = sysvRun ? sysvNextBit : layoutEnd() * 8;
        uint64_t width = uint64_t(*w);
        if (width > typeBits)
          fail(m, "the layout of " + types.toString(key, &symbols) + ": the bit field " + memberName(m) + " is wider (" + std::to_string(width) +
                      " bits) than its type");
        if (width == 0 || (bit % alignBits) + width > typeBits)
          bit = (bit + alignBits - 1) / alignBits * alignBits;
        sysvRun = true;
        if (width == 0)
        {
          closeUnit();
          sysvNextBit = bit;
          continue;
        }
        uint64_t first = bit / 8, end = (bit + width + 7) / 8;
        if (sysvUnit >= 0 && first < sysvUnitEnd)
          sysvUnitEnd = std::max(sysvUnitEnd, end);
        else
        {
          closeUnit();
          padTo(first);
          sysvUnit = int(info.fieldTypes.size());
          sysvUnitStart = first;
          sysvUnitEnd = end;
          info.fieldTypes.push_back(types.array(types.intType(8, true), uint32_t(end - first)));
          info.fieldDecls.push_back(m); // the first field of the unit stands for the unit in memberwise work
        }
        info.fieldIndex[m] = uint32_t(sysvUnit);
        info.bitfields[m] = {0, uint32_t(width), unit, true};
        sysvUnitFields.push_back({m, bit});
        sysvNextBit = bit + width;
        if (!static_cast<VariableDecl*>(m)->name.empty())
          info.bitFieldAlign = std::max(info.bitFieldAlign, alignOf(unit, m));
        continue;
      }
      if (*w == 0)
      {
        // `unsigned : 0;` (unnamed: C++ has no other bit field of width 0), as MSVC lays it out:
        // right after a bit field it ends that field's storage unit, the next field starts at a
        // boundary of its type's alignment, and the class takes that alignment (alignPadding pads
        // for them); anywhere else it is nothing. It holds no value: no field of the layout.
        // (Until 2026-11-05: "a bit field whose width is not a constant".)
        if (bitUnitIndex >= 0)
        {
          uint64_t boundary = alignOf(unit, m);
          info.zeroWidthAligned.push_back({uint32_t(info.fieldTypes.size()), boundary});
          info.bitFieldAlign = std::max(info.bitFieldAlign, boundary);
        }
        bitUnitIndex = -1;
        continue;
      }
      uint32_t unitBits = uint32_t(sizeOf(unit, m) * 8), width = uint32_t(*w);
      if (bitUnitIndex < 0 || bitUnitBits != unitBits || bitUsed + width > unitBits)
      {
        bitUnitIndex = int(info.fieldTypes.size());
        bitUnitBits = unitBits;
        bitUsed = 0;
        info.fieldTypes.push_back(unit);
        info.fieldDecls.push_back(m); // the first field of the unit stands for the unit in memberwise work
      }
      info.fieldIndex[m] = uint32_t(bitUnitIndex);
      info.bitfields[m] = {bitUsed, width, unit};
      bitUsed += width;
      continue;
    }
    bitUnitIndex = -1;
    endRun();
    info.fieldIndex[m] = uint32_t(info.fieldTypes.size());
    info.fieldDecls.push_back(m);
    info.fieldTypes.push_back(ft);
  }
  endRun(); // (a zero-width bit field last pads the class to its boundary)
  // Empty bases take no storage, unless the first member is of the same class (two objects of
  // one type at one address: C++ gives the base a byte then).
  info.zeroSize.assign(info.fieldTypes.size(), false);
  std::fill_n(info.zeroSize.begin(), overlappingFields, true);
  const Ty* firstMember = firstMemberClass();
  bool allBasesEmpty = true;
  for (auto& [baseDef, index] : info.bases)
  {
    bool emptyBase = structFor(info.fieldTypes[index], at).empty;
    allBasesEmpty = allBasesEmpty && emptyBase;
    info.zeroSize[index] = emptyBase && classKey(info.fieldTypes[index]) != firstMember;
  }
  info.empty = !info.polymorphic && info.fieldDecls.size() == overlapping.size() && allBasesEmpty;
  info.packed = (c->attributes & ATTR_Packed) != 0;
  uint64_t tailPadding = alignPadding(info, c, key, args, at);
  info.tailPadding = tailPadding;
  info.requiredAlign = c->alignment ? declaredAlignment(key, at) : 1;
  for (const Ty* field : info.fieldTypes)
    info.requiredAlign = std::max(info.requiredAlign, requiredAlignment(field, at));
  std::string line = info.name + " = type " + (info.packed ? "<{ " : "{ ");
  for (size_t i = 0; i < info.fieldTypes.size(); ++i)
    line += (i ? ", " : "") + (info.zeroSize[i] ? std::string("[0 x i8]") : llType(info.fieldTypes[i], c));
  for (Decl* m : info.fieldDecls)
  {
    const Ty* held = types.unqualified(info.fieldTypes[fieldIndexOf(info, m)]);
    while (held->kind == TyKind::Array)
      held = types.unqualified(held->inner);
    if (isClass(held) && structFor(held, c).flexible) // (laid out already, for its field above)
      fail(at, "the layout of " + types.toString(key, &symbols) + ": the member " + memberName(m) + " holds " + types.toString(held, &symbols) +
                   ", which has a flexible array member: only a pointer may refer to such an object inside another");
  }
  if (info.empty)
    line += info.fieldTypes.empty() ? "i8" : ", i8"; // one byte of its own as an object
  if (tailPadding)
    line += ", [" + std::to_string(tailPadding) + " x i8]";
  line += info.packed ? " }>\n" : " }\n";
  typeLine = line;
  // Its size and alignment, final with the layout (classSizeAndAlign reads them; alignPadding
  // sets them where alignas asks for padding).
  if (!info.align)
    std::tie(info.size, info.align) = fieldsSize(info, key, at);
  info.complete = true;
  if (info.polymorphic)
  {
    // The base fields are final now (alignPadding may have moved them).
    std::vector<uint32_t> finalIndex;
    for (auto& [baseDef, index] : info.bases)
      finalIndex.push_back(index);
    info.vtables = collectVtables(key, c, args, baseKeys, finalIndex, primary, at);
    emitVtables(key, info, at);
  }
}

auto codegen::Emitter::requiredAlignment(const Ty* t, Node* at) -> uint64_t
{
  t = types.unqualified(t);
  while (t->kind == TyKind::Array)
    t = types.unqualified(t->inner);
  if (t->kind == TyKind::Vector)
    return alignOf(t, at);
  return isClass(t) ? structFor(t, at).requiredAlign : 1;
}

auto codegen::Emitter::holdsClass(const Ty* t, const Ty* wanted, Node* at) -> bool
{
  t = types.unqualified(t);
  while (t->kind == TyKind::Array)
    t = types.unqualified(t->inner);
  if (!isClass(t) && !isLambda(t))
    return false;
  const Ty* held = canonicalClass(t, at);
  if (held == wanted)
    return true;
  for (const Ty* field : structFor(held, at).fieldTypes)
    if (holdsClass(field, wanted, at))
      return true;
  return false;
}

auto codegen::Emitter::overlappingMembers(ClassDecl* c, const Ty* key, const TemplateArgs& args, const std::vector<const Ty*>& baseKeys, Node* at) -> std::unordered_set<Decl*>
{
  std::unordered_set<Decl*> overlapping;
  std::vector<Decl*> members; // the data members of this instance
  bool any = false;
  for (Decl* m : c->members)
    if (m->kind == NodeKind::VariableDecl && !(m->flags & (DF_Static | DF_Typedef | DF_Friend)))
    {
      members.push_back(m);
      any = any || (m->attributes & ATTR_NoUniqueAddress);
    }
  if (!any)
    return overlapping;
  auto known = [&](const Ty* t) { return t && t->isKnown() && !types.isDependent(t); };
  std::erase_if(members, [&](Decl* m) { return !whenMemberHolds(m, c, args, key, at) || !known(global.typeOfDeclIn(m, args)); });
  for (Decl* m : members)
  {
    const Ty* held = types.unqualified(global.typeOfDeclIn(m, args));
    if (!(m->attributes & ATTR_NoUniqueAddress) || static_cast<VariableDecl*>(m)->alignment || (!isClass(held) && !isLambda(held)))
      continue;
    if (isLambda(held))
      closureFor(held, at);
    const Ty* cls = canonicalClass(held, at);
    if (!structFor(cls, at).empty || alignOf(cls, at) > 1)
      continue;
    bool shared = false;
    for (const Ty* base : baseKeys)
      shared = shared || holdsClass(base, cls, at);
    for (Decl* other : members)
      shared = shared || (other != m && holdsClass(global.typeOfDeclIn(other, args), cls, at));
    if (!shared)
      overlapping.insert(m);
  }
  return overlapping;
}

auto codegen::Emitter::collectVtables(const Ty* key, ClassDecl* c, const TemplateArgs& args, const std::vector<const Ty*>& baseKeys, const std::vector<uint32_t>& baseIndex, int primary, Node* at) -> std::vector<Vtable>
{
  std::vector<Vtable> tables(1);
  for (size_t i = 0; i < baseKeys.size(); ++i)
  {
    std::vector<Vtable> inherited = structFor(baseKeys[i], at).vtables;
    for (size_t j = 0; j < inherited.size(); ++j)
    {
      Vtable& t = inherited[j];
      for (VtableSlot& s : t.slots)
        s.ownerPath.insert(s.ownerPath.begin(), baseIndex[i]);
      if (int(i) == primary && j == 0)
      {
        tables[0].slots = std::move(t.slots);
        continue;
      }
      t.path.insert(t.path.begin(), baseIndex[i]);
      t.name.clear();
      t.vptrValue.clear();
      tables.push_back(std::move(t));
    }
  }
  auto addToPrimary = [&](const VtableSlot& slot)
  {
    const VtableSlot* last = nullptr;
    for (const VtableSlot& s : tables[0].slots)
      if (s.key == slot.key)
        last = &s;
    // (a covariant override's result that does not start with the class the slot gives back: its
    // callers here want the whole result, those of the slot a base of it, at another address)
    if (!last || (slot.result && last->result && !global.classStartsWith(slot.result, last->result)))
      tables[0].slots.push_back(slot);
  };
  bool declaresDestructor = false;
  {
    ArgsScope scope(*this, &args);
    for (Decl* m : c->members)
    {
      if (m->kind != NodeKind::FunctionDecl || (m->flags & (DF_Static | DF_Constructor)) || !m->templateParams.empty())
        continue;
      auto* f = static_cast<FunctionDecl*>(m);
      declaresDestructor = declaresDestructor || (f->flags & DF_Destructor);
      VtableSlot own{methodKey(f), f, key, {}};
      bool overrides = false;
      for (const Vtable& t : tables)
        for (const VtableSlot& s : t.slots)
          if (s.key == own.key)
            own.overridesTest = own.overridesTest || s.overridesTest || (s.function && s.function->testCode);
      for (Vtable& t : tables)
        for (VtableSlot& s : t.slots)
          if (s.key == own.key)
          {
            const Ty* callersExpect = s.result;
            s = own;
            s.result = callersExpect;
            overrides = true;
          }
      if (overrides || (f->flags & (DF_Virtual | DF_Override)))
      {
        bool pointer = false;
        own.result = resultClassOf(f, key, pointer);
        // (Its layout would wait for this one, or this one for it, by which is asked for first. C++
        // cannot declare it: the result must be complete at the override.)
        if (own.result && global.classDerivesFrom(own.result, key) && !global.classStartsWith(key, own.result)) // (not the class itself)
          for (const Vtable& t : tables)
            for (const VtableSlot& s : t.slots)
              if (s.key == own.key && s.result && !(global.classStartsWith(s.result, own.result) && global.classStartsWith(own.result, s.result)))
                fail(f, "the override " + qualifiedName(f) + " returns " + types.toString(own.result, &symbols) + ", which derives from " + types.toString(key, &symbols));
        addToPrimary(own);
      }
    }
  }
  if (!declaresDestructor)
  {
    // A class that declares no destructor has an implicit one, virtual when a base's is: it
    // destroys this class's members too, so it takes the destructor slots.
    VtableSlot implicit{"~", nullptr, key, {}};
    bool virtualDestructor = false;
    for (Vtable& t : tables)
      for (VtableSlot& s : t.slots)
        if (s.key == "~")
        {
          s = implicit;
          virtualDestructor = true;
        }
    if (virtualDestructor)
      addToPrimary(implicit);
  }
  // An instance's destructor against its bases' ([except.spec] 4; a class of no template: the checker's).
  if (!args.empty())
    if (const Ty* base = global.destructorLoosens(key))
      fail(c, "the destructor of " + types.toString(key, &symbols) + " may throw and overrides the noexcept destructor of " + types.toString(base, &symbols));
  return tables;
}

auto codegen::Emitter::subobjectAtNull(const Ty* cls, const std::vector<uint32_t>& path, Node* at) -> std::string
{
  std::string p = "null";
  const Ty* t = cls;
  for (uint32_t index : path)
  {
    StructInfo& info = structFor(t, at);
    p = "getelementptr (" + info.name + ", ptr " + p + ", i32 0, i32 " + std::to_string(index) + ")";
    t = info.fieldTypes[index];
  }
  return p;
}

auto codegen::Emitter::samePlace(std::vector<uint32_t> a, std::vector<uint32_t> b) -> bool
{
  while (!a.empty() && a.back() == 0)
    a.pop_back();
  while (!b.empty() && b.back() == 0)
    b.pop_back();
  return a == b;
}

auto codegen::Emitter::classIrName(const Ty* key, const StructInfo& info) -> std::string
{
  std::string name = irString(key);
  const size_t hashed = std::string("%struct.").size() + 16; // "%struct." and 16 hex digits
  if (info.name.size() > hashed && info.name[hashed] == '.')
    name += "#" + info.name.substr(hashed + 1);
  return name;
}

auto codegen::Emitter::emitVtables(const Ty* key, StructInfo& info, Node* at) -> void
{
  std::string ti = descriptorFor(key, at);
  std::vector<Vtable> tables = info.vtables;
  std::string text, thunks;
  // The vtable asks for its functions, not whichever function happened to need the layout first
  // (the requester the diagnostics name, the same in every run).
  Restore<std::string> restore(currentFunctionName);
  currentFunctionName = "the vtable of " + types.toString(key, &symbols);
  Restore<std::string> contextRestore(currentContext);
  Restore<std::string> requesterContextRestore(currentRequestedByContext);
  currentContext = currentFunctionName;
  currentRequestedByContext.clear();
  std::string deleter = deleterFor(key, classIrName(key, info), thunks, at);
  for (size_t t = 0; t < tables.size(); ++t)
  {
    Vtable& table = tables[t];
    table.name = "@\"vtable." + classIrName(key, info) + (t ? "." + std::to_string(t) : std::string()) + "\"";
    std::string entries = "ptr " + deleter + ", ptr " + subobjectAtNull(key, table.path, at) + ", ptr " + ti;
    for (size_t s = 0; s < table.slots.size(); ++s)
    {
      const VtableSlot& slot = table.slots[s];
      if (slot.function && (slot.function->flags & DF_Pure))
      {
        entries += ", ptr null";
        continue;
      }
      std::string target = slot.function ? useFunction(slot.function, global.classArgsOf(slot.owner), slot.owner)
                                         : implicitDestructor(slot.owner, at); // a class declaring none still destroys its own members
      // (a covariant override's result moved to the base its callers here expect: until 2026-11-05
      // `Second* s = &both; s->self()` gave the whole object's address back as the Second)
      ResultMove move = covariantMove(slot, at);
      if (move.bytes && t == 0 && slotKeyed(info, slot.key) == int(s))
        unsupported(at, "the covariant override " + qualifiedName(slot.function) + " in " + types.toString(key, &symbols) + ": its slot gives a base of its result, where calls through the class want all of it");
      if (!samePlace(slot.ownerPath, table.path) || move.bytes)
      {
        std::string thunk = "@\"thunk." + classIrName(key, info) + "." + std::to_string(t) + "." + std::to_string(s) + "\"";
        thunks += adjustingThunk(thunk, target, slot, subobjectAtNull(key, table.path, at), subobjectAtNull(key, slot.ownerPath, at), at, move);
        target = thunk;
      }
      entries += ", ptr " + target;
    }
    std::string tableType = "[" + std::to_string(table.slots.size() + 3) + " x ptr]";
    // On Windows, the name MSVC gives the table at its address point (`??_7Square@@6B@`, one of
    // several `??_7Square@@6BShape@@@`, "const Square::`vftable'{for `Shape'}"): Visual Studio reads
    // an object's dynamic type from the symbol its vtable pointer points at (a Shape& shows
    // [Square]). Not for a class MSVC would name with template arguments or a local one (msvcClassName).
    // As clang's: the table private in a comdat named after that symbol, which is an alias at its
    // address point and the vtable pointer's value (an object's constructor stores it), so that the
    // linker keeps one copy by the comdat's key and nothing names the private copies it drops. (A
    // second external name in the comdat, the table's own, was defined twice: LNK2005.)
    std::string symbol;
    if (sh.target.windows() && sh.fullDebugInfo) // (a debug build's, which the debugger reads: ThinLTO left an imported table's alias undefined)
    {
      std::string self = msvcClassName(key);
      const Ty* holder = table.path.empty() && info.primaryBase ? info.primaryBase : key; // (the primary table is its primary base's)
      for (uint32_t index : table.path)
        holder = structFor(holder, at).fieldTypes[index];
      std::string forBase = tables.size() == 1 ? std::string() : msvcClassName(holder);
      symbol = self.empty() || (tables.size() > 1 && forBase.empty()) ? std::string() : "??_7" + self + "6B" + forBase + "@";
    }
    table.vptrValue = "getelementptr inbounds (" + tableType + ", ptr " + table.name + ", i64 0, i64 3)";
    if (!symbol.empty() && msvcTableNames.insert(symbol).second)
    {
      text += "$\"" + symbol + "\" = comdat any\n" + table.name + " = private unnamed_addr constant " + tableType + " [" + entries + "], comdat($\"" + symbol +
              "\")\n@\"" + symbol + "\" = unnamed_addr alias i8, " + table.vptrValue + "\n";
      table.vptrValue = "@\"" + symbol + "\"";
    }
    else
      text += "$" + table.name.substr(1) + " = comdat any\n" + table.name + " = linkonce_odr constant " + tableType + " [" + entries + "], comdat\n";
  }
  msvcTableNames.clear();
  info.vtables = std::move(tables);
  info.vtableName = info.vtables[0].name;
  info.vptrValue = info.vtables[0].vptrValue;
  info.vtableText = text + thunks;
}

auto codegen::Emitter::msvcClassName(const Ty* cls) -> std::string
{
  if (!cls || cls->kind != TyKind::Class || !cls->decl || cls->decl->kind != NodeKind::ClassDecl || !cls->args.empty())
    return {};
  auto* c = static_cast<ClassDecl*>(cls->decl);
  if (c->name.parts.empty() || c->lambda || !filePrivatePrefix(c).empty() || (c->flags & DF_Specialization) || !c->templateParams.empty() ||
      c->name.last().hasTemplateArgs)
    return {}; // (a file's private class: two files may have one of a name, design.md 15; a specialization's
    // name has its arguments)
  std::vector<uint32_t> parts; // innermost first
  for (size_t i = c->name.parts.size(); i-- > 0;)
    parts.push_back(c->name.parts[i].symbol);
  for (Decl* p = c->parent; p; p = p->parent)
    if (p->kind == NodeKind::NamespaceDecl)
    {
      auto* n = static_cast<NamespaceDecl*>(p);
      if (n->name == UINT32_MAX || n->isInline)
        return {}; // (an anonymous namespace: MSVC's name has a hash; an inline one's name is in MSVC's)
      if (n->nested.empty())
        parts.push_back(n->name);
      for (size_t i = n->nested.parts.size(); i-- > 0;) // (`namespace a::b`: its parts, a first, the name)
        parts.push_back(n->nested.parts[i].symbol);
    }
    else if (p->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(p)->templateParams.empty() && !static_cast<ClassDecl*>(p)->name.parts.empty())
      parts.push_back(static_cast<ClassDecl*>(p)->name.last().symbol);
    else
      return {}; // (a class template's member, a local class)
  // Not std's classes nor type_info: the C runtime defines the tables of its own of those names
  // (`??_7exception@std@@6B@`, `??_7type_info@@6B@`, linked in with it), and the linker would keep
  // one table of the two for both, of another layout.
  if (sym(parts.back()) == "std" || (parts.size() == 1 && sym(parts[0]) == "type_info"))
    return {};
  std::string out;
  for (uint32_t symbol : parts)
  {
    if (symbol == UINT32_MAX)
      return {};
    std::string name = sym(symbol);
    for (char ch : name)
      if (!(isalnum((unsigned char)ch) || ch == '_'))
        return {};
    out += name + "@";
  }
  return out + "@";
}

auto codegen::Emitter::resultClassOf(FunctionDecl* f, const Ty* owner, bool& pointer) -> const Ty*
{
  TemplateArgs args = global.classArgsOf(owner);
  ArgsScope scope(*this, &args);
  const Ty* ft = declType(f);
  if (!ft || ft->kind != TyKind::Function || !ft->inner || !ft->inner->isKnown())
    return nullptr;
  const Ty* r = ft->inner;
  const Ty* object = nullptr;
  if (r->isReference())
    object = types.stripRef(r);
  else if (types.unqualified(r)->kind == TyKind::Pointer)
  {
    object = types.unqualified(r)->inner;
    pointer = true;
  }
  object = object ? types.unqualified(object) : nullptr;
  return object && isClass(object) && !types.isDependent(object) ? object : nullptr;
}

auto codegen::Emitter::hasBaseAtOffset(const Ty* cls, Node* at, int) -> bool
{
  if (!cls || cls->kind != TyKind::Class || !cls->decl || types.isDependent(cls))
    return false;
  const Ty* key = canonicalClass(cls, at);
  if (auto found = baseAtOffsetCache.find(key); found != baseAtOffsetCache.end())
    return found->second;
  bool result = global.hasBaseAtOffset(key); // (the resolver's, which sizeof at compile time reads too)
  baseAtOffsetCache[key] = result;
  return result;
}

auto codegen::Emitter::memberFunctionPointerAdjusts(const Ty* mp, Node* at) -> bool
{
  if (!mp || mp->kind != TyKind::MemberPointer || !mp->inner || mp->inner->kind != TyKind::Function || mp->args.empty())
    return false;
  const Ty* cls = mp->args[0];
  if (cls && types.isDependent(cls))
    cls = withCurrentArgs(cls);
  return cls && isClass(cls) && !types.isDependent(cls) && hasBaseAtOffset(cls, at);
}

auto codegen::Emitter::memberFunctionParts(const std::string& value) -> std::pair<std::string, std::string>
{
  if (value == "zeroinitializer")
    return {"null", "0"};
  // `{ ptr @"f(int32, int32)", i32 16 }`: the function's name may hold commas, the adjustment not.
  if (value.rfind("{ ptr ", 0) == 0)
    if (size_t comma = value.rfind(", i32 "); comma != std::string::npos && value.size() > 2 && value.compare(value.size() - 2, 2, " }") == 0)
      return {value.substr(6, comma - 6), value.substr(comma + 6, value.size() - 2 - comma - 6)};
  std::string function = newTemp(), adjustment = newTemp();
  fn += "  " + function + " = extractvalue { ptr, i32 } " + value + ", 0\n";
  fn += "  " + adjustment + " = extractvalue { ptr, i32 } " + value + ", 1\n";
  return {function, adjustment};
}

auto codegen::Emitter::memberFunctionValue(const std::string& function, const std::string& adjustment) -> std::string
{
  auto constantNumber = [](const std::string& s) { return !s.empty() && s.find_first_not_of("-0123456789") == std::string::npos; };
  if ((function == "null" || (!function.empty() && function[0] == '@')) && constantNumber(adjustment))
    return function == "null" && adjustment == "0" ? std::string("zeroinitializer") : "{ ptr " + function + ", i32 " + adjustment + " }";
  std::string first = newTemp(), whole = newTemp();
  fn += "  " + first + " = insertvalue { ptr, i32 } poison, ptr " + function + ", 0\n";
  fn += "  " + whole + " = insertvalue { ptr, i32 } " + first + ", i32 " + adjustment + ", 1\n";
  return whole;
}

auto codegen::Emitter::baseOffsetIn(const Ty* derived, const Ty* base, Node* at) -> std::optional<uint64_t>
{
  std::vector<std::pair<std::string, uint32_t>> path;
  if (!basePath(derived, base, path, at))
    return std::nullopt;
  uint64_t offset = 0;
  const Ty* t = derived;
  for (auto& step : path)
  {
    classSizeAndAlign(t, at);
    StructInfo& info = structFor(t, at);
    offset += fieldStart(info, step.second, at);
    t = info.fieldTypes[step.second];
  }
  return offset;
}

auto codegen::Emitter::covariantMove(const VtableSlot& slot, Node* at) -> ResultMove
{
  bool pointer = false;
  const Ty* result = slot.function && slot.result ? resultClassOf(slot.function, slot.owner, pointer) : nullptr;
  if (!result || result == slot.result)
    return {};
  // (either may be a forward declaration's type, which has no bases)
  const Ty* whole = canonicalClass(result, at);
  const Ty* part = canonicalClass(slot.result, at);
  if (whole == part)
    return {};
  std::optional<uint64_t> offset = baseOffsetIn(whole, part, at);
  if (!offset)
    unsupported(at, "the override " + qualifiedName(slot.function) + " returns " + types.toString(result, &symbols) + ", which has no base " + types.toString(slot.result, &symbols) + " to give back to the function's callers");
  return *offset ? ResultMove{*offset, pointer} : ResultMove{};
}

auto codegen::Emitter::adjustingThunk(const std::string& name, const std::string& target, const VtableSlot& slot, const std::string& from, const std::string& to, Node* at,
                                      ResultMove move) -> std::string
{
  Signature s;
  if (slot.function)
  {
    TemplateArgs args = global.classArgsOf(slot.owner);
    ArgsScope scope(*this, &args);
    s = signatureOf(slot.function);
  }
  else
  {
    s.returnType = "void";
    s.paramTypes = {"ptr"};
    s.hasThis = true;
  }
  if (s.variadic)
    unsupported(at, "the variadic virtual function " + qualifiedName(slot.function) + " reached through another base of " +
                        types.toString(slot.owner, &symbols) + " (its thunk cannot pass the arguments on)");
  std::string params, args;
  for (size_t i = 1; i < s.paramTypes.size(); ++i)
  {
    params += ", " + s.paramTypes[i] + " %p" + std::to_string(i);
    args += ", " + s.paramTypes[i] + " %p" + std::to_string(i);
  }
  std::string body = "  %to = ptrtoint ptr " + to + " to i64\n  %from = ptrtoint ptr " + from + " to i64\n  %d = sub i64 %to, %from\n"
                     "  %this1 = getelementptr i8, ptr %this, i64 %d\n";
  if (s.returnType == "void")
    body += "  tail call void " + target + "(ptr %this1" + args + ")\n  ret void\n";
  else if (!move.bytes)
    body += "  %r = tail call " + s.returnType + " " + target + "(ptr %this1" + args + ")\n  ret " + s.returnType + " %r\n";
  else
  {
    // The override's result moved to the base its callers here expect; a null pointer stays null.
    body += "  %r = call ptr " + target + "(ptr %this1" + args + ")\n  %moved = getelementptr i8, ptr %r, i64 " + std::to_string(move.bytes) + "\n";
    body += move.pointer ? "  %null = icmp eq ptr %r, null\n  %result = select i1 %null, ptr null, ptr %moved\n  ret ptr %result\n" : "  ret ptr %moved\n";
  }
  return "$" + name.substr(1) + " = comdat any\ndefine linkonce_odr " + s.returnType + " " + name + "(ptr %this" + params + ") comdat {\nentry:\n" + body + "}\n";
}

auto codegen::Emitter::memberFunctionThunk(const Ty* cls, const Ty* owner, const std::vector<std::pair<std::string, uint32_t>>& path, FunctionDecl* f, int slot, Node* at) -> std::string
{
  Signature s;
  std::string key;
  {
    TemplateArgs args = global.classArgsOf(owner);
    ArgsScope scope(*this, &args);
    s = signatureOf(f);
    key = methodKey(f);
  }
  if (s.variadic)
    unsupported(at, "a pointer to the variadic member function " + qualifiedName(f) + " (its thunk cannot pass the arguments on)");
  std::string target = slot < 0 ? useFunction(f, global.classArgsOf(owner), owner) : std::string();
  return sharedThunk("@\"pmf." + irString(cls) + "." + irString(owner) + "::" + key + "\"", s, pathAtNull(path), target, slot);
}

auto codegen::Emitter::pathAtNull(const std::vector<std::pair<std::string, uint32_t>>& path) -> std::string
{
  std::string offset = "null";
  for (auto& [structName, index] : path)
    offset = "getelementptr (" + structName + ", ptr " + offset + ", i32 0, i32 " + std::to_string(index) + ")";
  return offset;
}

auto codegen::Emitter::baseSubobjects(const Ty* cls, Node* at) -> std::vector<BaseSubobject>
{
  std::vector<BaseSubobject> found;
  std::vector<std::pair<std::string, uint32_t>> path;
  std::function<void(const Ty*, bool)> walk = [&](const Ty* t, bool isPublic)
  {
    const StructInfo& info = structFor(t, at);
    std::string structName = info.name;
    std::vector<std::pair<Decl*, uint32_t>> bases = info.bases;
    std::vector<bool> basePublic = info.basePublic;
    std::vector<const Ty*> fieldTypes = info.fieldTypes;
    for (size_t i = 0; i < bases.size(); ++i)
    {
      path.push_back({structName, bases[i].second});
      const Ty* base = canonicalClass(fieldTypes[bases[i].second], at);
      bool basePath = isPublic && basePublic[i];
      found.push_back({base, basePath, "ptrtoint (ptr " + pathAtNull(path) + " to i64)"});
      walk(base, basePath);
      path.pop_back();
    }
  };
  walk(canonicalClass(cls, at), true);
  return found;
}

auto codegen::Emitter::handlerBases(const Ty* cls, Node* at) -> std::vector<BaseSubobject>
{
  std::vector<BaseSubobject> all = baseSubobjects(cls, at), out;
  for (const BaseSubobject& one : all)
    if (one.isPublic && std::count_if(all.begin(), all.end(), [&](const BaseSubobject& other) { return other.cls == one.cls; }) == 1)
      out.push_back(one);
  return out;
}

auto codegen::Emitter::descriptorFor(const Ty* cls, Node* at) -> std::string
{
  StructInfo& info = structFor(cls, at);
  const Ty* key = canonicalClass(cls, at);
  auto building = privateLayouts.find(key);
  bool own = building != privateLayouts.end() && &building->second == &info;
  {
    std::unique_lock lock(sh.descriptorsMutex, std::defer_lock);
    if (!own)
      lock.lock();
    if (!info.typeInfoName.empty())
      return info.typeInfoName;
  }
  std::string name = "@\"typeinfo." + classIrName(key, info) + "\"";
  bool savedShared = emittingShared;
  emittingShared = true; // the descriptor goes into every module that uses it
  std::string nameConstant = cStringConstant(types.toString(key, &symbols));
  emittingShared = savedShared;
  std::string structName = info.name, list;
  for (size_t i = 0; i < info.bases.size(); ++i)
  {
    uint32_t index = info.bases[i].second;
    list += (list.empty() ? "" : ", ") + std::string("{ ptr, i64, i64 } { ptr ") + descriptorFor(info.fieldTypes[index], at) +
            ", i64 ptrtoint (ptr getelementptr (" + structName + ", ptr null, i32 0, i32 " + std::to_string(index) + ") to i64), i64 " +
            (info.basePublic[i] ? "1" : "0") + " }";
  }
  std::vector<BaseSubobject> subobjects = baseSubobjects(key, at);
  std::unordered_set<const Ty*> distinct;
  bool plain = true;
  for (const BaseSubobject& one : subobjects)
    plain = plain && one.isPublic && distinct.insert(one.cls).second;
  std::string arrayType = "[" + std::to_string(info.bases.size()) + " x { ptr, i64, i64 }]";
  std::string text = "$" + name.substr(1) + " = comdat any\n" + name + " = linkonce_odr constant { ptr, i64, i64, " + arrayType + " } { ptr " +
                     nameConstant + ", i64 " + std::to_string(info.bases.size()) + ", i64 " + (plain ? "1" : "0") + ", " + arrayType + " " +
                     (info.bases.empty() ? std::string("zeroinitializer") : "[" + list + "]") + " }, comdat\n";
  std::unique_lock lock(sh.descriptorsMutex, std::defer_lock);
  if (!own)
    lock.lock();
  if (info.typeInfoName.empty())
  {
    info.typeInfoName = name;
    info.descriptorText = std::move(text);
  }
  return name;
}

auto codegen::Emitter::useDescriptor(const StructInfo& info) -> void
{
  std::string name, text;
  {
    std::lock_guard descriptors(sh.descriptorsMutex);
    name = info.typeInfoName;
    text = info.descriptorText;
  }
  auto lock = sharedLock(); // (vtablesIn)
  if (text.empty() || !vtablesIn[currentModule].insert(name).second)
    return;
  moduleDecls[currentModule] += text;
  for (auto& [baseDef, index] : std::vector<std::pair<Decl*, uint32_t>>(info.bases))
    useDescriptor(structFor(info.fieldTypes[index], nullptr));
}

auto codegen::Emitter::methodKey(FunctionDecl* f) -> std::string
{
  if (f->flags & DF_Destructor)
    return "~";
  std::string full = qualifiedName(f);
  size_t pos = full.rfind("::");
  std::string k = pos == std::string::npos ? full : full.substr(pos + 2);
  const Ty* ft = declType(f);
  k += "(";
  for (size_t i = 0; ft && ft->kind == TyKind::Function && i < ft->args.size(); ++i)
    k += (i ? ", " : "") + types.toString(ft->args[i], &symbols);
  k += ")";
  if (f->flags & DF_Const)
    k += " const";
  return k;
}

auto codegen::Emitter::slotOf(const Ty* classTy, FunctionDecl* f, Node* at) -> int
{
  StructInfo& info = structFor(classTy, at);
  if (!info.polymorphic)
    return -1;
  TemplateArgs args = global.classArgsOf(canonicalClass(classTy, at));
  ArgsScope scope(*this, &args);
  return slotKeyed(structFor(classTy, at), methodKey(f));
}

auto codegen::Emitter::slotKeyed(const StructInfo& info, const std::string& key) -> int
{
  if (info.vtables.empty())
    return -1;
  for (size_t i = info.vtables[0].slots.size(); i-- > 0;) // (the last of a key: the one calls through this class use)
    if (info.vtables[0].slots[i].key == key)
      return int(i);
  return -1;
}

auto codegen::Emitter::loadSlot(const std::string& addr, int slot) -> std::string
{
  std::string vptr = newTemp();
  fn += "  " + vptr + " = load ptr, ptr " + addr + "\n";
  std::string entry = newTemp();
  fn += "  " + entry + " = getelementptr inbounds ptr, ptr " + vptr + ", i64 " + std::to_string(slot) + "\n";
  std::string fp = newTemp();
  fn += "  " + fp + " = load ptr, ptr " + entry + "\n";
  return fp;
}

auto codegen::Emitter::gep(const std::string& structName, const std::string& addr, uint32_t index) -> std::string
{
  std::string r = newTemp();
  fn += "  " + r + " = getelementptr inbounds " + structName + ", ptr " + addr + ", i32 0, i32 " + std::to_string(index) + "\n";
  return r;
}

auto codegen::Emitter::fieldTypeIn(const Ty* classTy, Decl* member, Node* at, int depth) -> const Ty*
{
  if (!isClass(classTy) || depth > 16)
    return nullptr;
  StructInfo& info = structFor(classTy, at);
  if (auto it = info.fieldIndex.find(member); it != info.fieldIndex.end())
    return info.isUnion ? nullptr : info.fieldTypes[it->second];
  for (auto& [base, index] : std::vector<std::pair<Decl*, uint32_t>>(info.bases))
    if (const Ty* t = fieldTypeIn(structFor(classTy, at).fieldTypes[index], member, at, depth + 1))
      return t;
  for (auto& [anon, index] : std::vector<std::pair<Decl*, uint32_t>>(info.anonymous))
    if (const Ty* t = fieldTypeIn(structFor(classTy, at).fieldTypes[index], member, at, depth + 1))
      return t;
  return nullptr;
}

auto codegen::Emitter::fieldAddress(const Ty* classTy, Decl* member, const std::string& objectAddr, std::string& out, Node* at, const BitfieldInfo** bits) -> bool
{
  StructInfo& info = structFor(classTy, at);
  auto it = info.fieldIndex.find(member);
  if (it != info.fieldIndex.end())
  {
    out = info.isUnion ? objectAddr : gep(info.name, objectAddr, it->second);
    if (bits)
      *bits = bitfieldIn(info, member);
    return true;
  }
  // Fallback: the resolver may have a different Decl* for the same field (e.g. an out-of-line
  // definition or a using-declaration). Match by name when the pointer lookup fails.
  if (member->kind == NodeKind::VariableDecl)
  {
    auto* vd = static_cast<VariableDecl*>(member);
    if (!vd->name.empty())
    {
      uint32_t nameSymbol = vd->name.last().symbol;
      for (auto& [d, idx] : info.fieldIndex)
      {
        if (d->kind == NodeKind::VariableDecl)
        {
          auto* fd = static_cast<VariableDecl*>(d);
          if (!fd->name.empty() && fd->name.last().symbol == nameSymbol)
          {
            out = info.isUnion ? objectAddr : gep(info.name, objectAddr, idx);
            if (bits)
              *bits = bitfieldIn(info, d);
            return true;
          }
        }
      }
    }
  }
  std::vector<std::pair<Decl*, uint32_t>> anonymous = info.anonymous;
  std::vector<std::pair<Decl*, uint32_t>> bases = info.bases;
  std::vector<const Ty*> fieldTypes = info.fieldTypes;
  bool isUnion = info.isUnion;
  std::string name = info.name;
  Span<const Ty*> classArgs = canonicalClass(classTy, at)->args; // an anonymous member's: its class's arguments (buildStruct)
  for (auto& [anon, index] : anonymous)
  {
    std::string addr = isUnion ? objectAddr : gep(name, objectAddr, index);
    if (fieldAddress(types.classType(anon, classArgs), member, addr, out, at, bits))
      return true;
  }
  for (auto& [baseDef, index] : bases)
  {
    std::string baseAddr = gep(name, objectAddr, index);
    if (fieldAddress(fieldTypes[index], member, baseAddr, out, at, bits))
      return true;
  }
  return false;
}

auto codegen::Emitter::findFieldByName(const Ty* classTy, uint32_t nameSymbol, Node* at, int depth) -> Decl*
{
  if (depth > 16 || !classTy || classTy->kind != TyKind::Class || !classTy->decl)
    return nullptr;
  Decl* def = global.definitionOf(classTy->decl);
  if (!def || def->kind != NodeKind::ClassDecl)
    return nullptr;
  auto* c = static_cast<ClassDecl*>(def);
  TemplateArgs args = global.classArgsOf(classTy);
  Decl* absent = nullptr; // a `when` member of the name this instance does not have
  for (Decl* m : c->members)
  {
    if (m->kind == NodeKind::VariableDecl && !(m->flags & (DF_Static | DF_Typedef | DF_Friend)))
    {
      auto* vd = static_cast<VariableDecl*>(m);
      if (!vd->name.empty() && vd->name.last().symbol == nameSymbol)
      {
        // (several `when` members may declare one name: the one this instance has)
        if (!whenMemberHolds(m, c, args, classTy, at))
        {
          absent = m;
          continue;
        }
        return m;
      }
    }
    // Search anonymous struct/union members.
    if (m->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(m)->name.empty() && static_cast<ClassDecl*>(m)->isDefinition)
      if (Decl* found = findFieldByName(types.classType(m, classTy->args), nameSymbol, at, depth + 1))
        return found;
  }
  // Search base classes.
  for (const Ty* bt : basesOf(c, args, at))
    if (bt && bt->kind == TyKind::Class)
      if (Decl* found = findFieldByName(bt, nameSymbol, at, depth + 1))
        return found;
  if (absent)
    fail(at, "'" + sym(nameSymbol) + "' does not exist in " + types.toString(classTy, &symbols) +
                 ": its `when` condition does not hold there (design.md 19)");
  return nullptr;
}

auto codegen::Emitter::whenMemberHolds(Decl* m, ClassDecl* c, const TemplateArgs& args, const Ty* key, Node* at) -> bool
{
  if (!m->when || m->whenKnown >= 0)
    return m->whenKnown != 0;
  ParsedFile* savedFile = currentFile;
  currentFile = c->file ? c->file : currentFile;
  std::optional<bool> holds;
  {
    ArgsScope whenArgs(*this, &args);
    ConstContextScope whenContext(*this, c);
    holds = evalCondition(m->when);
  }
  currentFile = savedFile;
  if (!holds)
    fail(at, "the `when` condition of " + types.toString(key, &symbols) + "'s member " +
                 (static_cast<VariableDecl*>(m)->name.empty() ? std::string("?") : sym(static_cast<VariableDecl*>(m)->name.last().symbol)) +
                 " is no constant the compiler could evaluate (design.md 19)");
  return *holds;
}
