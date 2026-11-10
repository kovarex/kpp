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

auto codegen::Emitter::partMembers(ClassDecl* c) -> std::vector<Decl*>
{
  std::vector<Decl*> members;
  for (Decl* m : c->members)
    if ((m->kind == NodeKind::VariableDecl && !(m->flags & (DF_Static | DF_Typedef | DF_Friend))) ||
        (m->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(m)->name.empty() && static_cast<ClassDecl*>(m)->isDefinition))
      members.push_back(m);
  return members;
}

auto codegen::Emitter::partField(const StructInfo& info, Decl* m, uint32_t& index) -> bool
{
  if (m->kind == NodeKind::VariableDecl)
  {
    auto it = info.fieldIndex.find(m);
    if (it == info.fieldIndex.end())
      return false;
    index = it->second;
    return true;
  }
  for (const auto& [anonymous, field] : info.anonymous)
    if (anonymous == m)
    {
      index = field;
      return true;
    }
  return false;
}

auto codegen::Emitter::fieldStart(StructInfo& info, uint32_t index, Node* at) -> uint64_t
{
  uint64_t offset = 0;
  for (uint32_t i = 0; i < index && i < info.fieldTypes.size(); ++i)
  {
    if (i < info.zeroSize.size() && info.zeroSize[i])
      continue; // an empty base: no storage
    uint64_t a = info.packed ? 1 : alignOf(info.fieldTypes[i], at);
    offset = (offset + a - 1) / a * a + sizeOf(info.fieldTypes[i], at);
  }
  if (index < info.fieldTypes.size() && !(index < info.zeroSize.size() && info.zeroSize[index]))
  {
    uint64_t a = info.packed ? 1 : alignOf(info.fieldTypes[index], at);
    offset = (offset + a - 1) / a * a;
  }
  return offset;
}

auto codegen::Emitter::constantBytes(const ConstValue& v, const Ty* t, std::vector<uint8_t>& out, uint64_t offset, Node* at, int depth) -> bool
{
  if (!t || depth > maxClassNesting)
    return false;
  t = types.unqualified(t);
  if (v.kind == ConstValue::Kind::Unset)
    return true; // static storage starts zero
  uint64_t size = sizeOf(t, at);
  if (offset + size > out.size())
    return false;
  auto integer = [&](uint64_t bits, uint64_t bytes, bool negative)
  {
    for (uint64_t b = 0; b < bytes; ++b)
      out[offset + b] = b < 8 ? uint8_t(bits >> (8 * b)) : negative ? 0xFF : 0x00;
  };
  switch (t->kind)
  {
    case TyKind::Bool:
      if (v.kind != ConstValue::Kind::Int)
        return false;
      out[offset] = v.i ? 1 : 0;
      return true;
    case TyKind::Char: case TyKind::Int: case TyKind::Enum:
      if (v.kind != ConstValue::Kind::Int)
        return false;
      integer(uint64_t(v.i), size, v.i < 0);
      return true;
    case TyKind::Float:
    {
      double d;
      if (v.kind == ConstValue::Kind::Float)
        d = v.f;
      else if (v.kind == ConstValue::Kind::Int)
        d = double(v.i);
      else
        return false;
      if (t->bits == 32)
      {
        float f = float(d);
        uint32_t bits;
        memcpy(&bits, &f, sizeof bits);
        integer(bits, 4, false);
      }
      else if (t->bits == 64)
      {
        uint64_t bits;
        memcpy(&bits, &d, sizeof bits);
        integer(bits, 8, false);
      }
      else
        return false;
      return true;
    }
    case TyKind::Pointer:
      return v.kind == ConstValue::Kind::Int && v.i == 0; // a null pointer; an address is no bytes the compiler knows
    case TyKind::Array:
    {
      if (v.kind != ConstValue::Kind::Object || !v.parts || v.parts->size() != t->count)
        return false;
      uint64_t element = sizeOf(t->inner, at);
      for (size_t i = 0; i < v.parts->size(); ++i)
        if (!constantBytes((*v.parts)[i], t->inner, out, offset + i * element, at, depth + 1))
          return false;
      return true;
    }
    case TyKind::Class:
    {
      if (v.kind != ConstValue::Kind::Object || !v.parts)
        return false;
      StructInfo& info = structFor(t, at);
      ClassDecl* c = classDef(t->decl, at);
      if (info.polymorphic)
        return false; // its vtable pointer: an address
      std::vector<Decl*> members = partMembers(c);
      const Ty* canonical = canonicalClass(t, at);
      if (info.isUnion)
      {
        // Its active member at its start: the one part that holds a value (an inactive member's
        // part is unset, or an object of unset parts; none holding one: zero).
        if (v.parts->size() != members.size())
          return false;
        std::function<bool(const ConstValue&)> unsetDeeply = [&](const ConstValue& p)
        {
          if (p.kind == ConstValue::Kind::Unset)
            return true;
          if (p.kind != ConstValue::Kind::Object || !p.parts)
            return false;
          for (const ConstValue& q : *p.parts)
            if (!unsetDeeply(q))
              return false;
          return true;
        };
        for (size_t k = 0; k < members.size(); ++k)
        {
          const ConstValue& part = (*v.parts)[k];
          if (unsetDeeply(part))
            continue;
          for (size_t other = k + 1; other < members.size(); ++other)
            if (!unsetDeeply((*v.parts)[other]))
              return false; // (two members alive: none the interpreter makes)
          if (!info.fieldIndex.count(members[k]) && members[k]->kind == NodeKind::VariableDecl)
            return false; // (a `when` member this instance does not have)
          const Ty* mt = members[k]->kind == NodeKind::VariableDecl ? global.typeOfDeclIn(members[k], global.classArgsOf(canonical))
                                                                     : types.classType(members[k], canonical->args);
          if (!mt || !mt->isKnown() || types.isDependent(mt) || types.unqualified(mt)->isReference())
            return false;
          return constantBytes(part, mt, out, offset, at, depth + 1);
        }
        return true;
      }
      if (v.parts->size() != info.bases.size() + members.size())
        return false;
      for (size_t k = 0; k < info.bases.size(); ++k)
      {
        uint32_t index = info.bases[k].second;
        if (index < info.zeroSize.size() && info.zeroSize[index])
          continue; // an empty base: no storage of its own
        if (!constantBytes((*v.parts)[k], info.fieldTypes[index], out, offset + fieldStart(info, index, at), at, depth + 1))
          return false;
      }
      for (size_t k = 0; k < members.size(); ++k)
      {
        const ConstValue& part = (*v.parts)[info.bases.size() + k];
        uint32_t index;
        if (!partField(info, members[k], index) || index >= info.fieldTypes.size())
          return false;
        uint64_t at0 = offset + fieldStart(info, index, at);
        if (auto bits = info.bitfields.find(members[k]); bits != info.bitfields.end())
        {
          // Its bits of the storage unit it shares (little-endian, as LLVM's integer is).
          if (part.kind == ConstValue::Kind::Unset)
            continue;
          if (part.kind != ConstValue::Kind::Int)
            return false;
          uint64_t unitBytes = sizeOf(info.fieldTypes[index], at);
          uint32_t shift = bits->second.shift, width = bits->second.width;
          for (uint32_t b = 0; b < width && b < 64; ++b)
            if ((uint64_t(part.i) >> b) & 1)
            {
              uint64_t bit = uint64_t(shift) + b;
              if (bit / 8 >= unitBytes)
                return false;
              out[at0 + bit / 8] |= uint8_t(1u << (bit % 8));
            }
          continue;
        }
        if (info.fieldTypes[index]->isReference())
          return false;
        if (!constantBytes(part, info.fieldTypes[index], out, at0, at, depth + 1))
          return false;
      }
      return true;
    }
    default:
      return false;
  }
}

auto codegen::Emitter::unionConstant(const ConstValue& v, const Ty* t, StructInfo& info, Node* at, int depth) -> std::optional<std::string>
{
  if (v.kind == ConstValue::Kind::Unset)
    return std::string("zeroinitializer");
  const Ty* storage = info.fieldTypes.empty() ? nullptr : info.fieldTypes[0];
  if (!storage || storage->kind != TyKind::Array || !storage->inner || storage->inner->kind != TyKind::Int)
    return std::nullopt;
  uint64_t unit = sizeOf(storage->inner, at);
  std::vector<uint8_t> bytes(size_t(unit * storage->count), 0);
  if (!constantBytes(v, t, bytes, 0, at, depth + 1))
    return std::nullopt;
  bool allZero = true;
  for (uint8_t b : bytes)
    allZero = allZero && b == 0;
  if (allZero)
    return std::string("zeroinitializer");
  std::string element = llType(storage->inner, at), out = "{ " + llType(storage, at) + " [";
  for (uint32_t i = 0; i < storage->count; ++i)
  {
    std::string digits;
    if (unit <= 8)
    {
      uint64_t value = 0;
      for (uint64_t b = unit; b-- > 0;)
        value = (value << 8) | bytes[size_t(i * unit + b)];
      if (unit < 8 && (value >> (unit * 8 - 1)) & 1)
        value |= ~0ull << (unit * 8); // sign-extended: LLVM reads a decimal as the type's signed value
      digits = std::to_string(int64_t(value));
    }
    else
    {
      // (i128, a 16-byte aligned union's: LLVM's unsigned hexadecimal, most significant byte first)
      static const char hex[] = "0123456789abcdef";
      digits = "u0x";
      for (uint64_t b = unit; b-- > 0;)
      {
        uint8_t byte = bytes[size_t(i * unit + b)];
        digits += hex[byte >> 4];
        digits += hex[byte & 15];
      }
    }
    out += (i ? ", " : "") + element + " " + digits;
  }
  return out + "] }";
}

auto codegen::Emitter::constantText(const ConstValue& v, const Ty* t, Node* at, int depth) -> std::optional<std::string>
{
  if (!t || depth > maxClassNesting)
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
      // The interpreter's Int 0 of a pointer type is a null pointer (`FOLD = fold` with fold =
      // nullptr, a table's `{NULL, NULL}`): a function template argument is its function's Address
      // since d61e6a56, never an Int 0, and a table's C functions are declared in its module since
      // 5857310f (Lua's `loadedlibs`). (`T* p = nullptr;` itself is a null constant before constant
      // data is asked for.)
      if (unset || (v.kind == ConstValue::Kind::Int && v.i == 0))
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
  if (info.polymorphic)
  {
    constantHasVptr = true;
    return std::nullopt;
  }
  // A union, the storage of a typed union too: its active member's bytes as the storage's integers.
  // (Until 2026-11-05 a union, a typed union and a class with an anonymous member were built at
  // startup, which a global initialized before them in their file read as zero: thread D's note on
  // FreeControllerNavigation's DEFAULT_NAVIGATOR_CONFIGURATION.)
  if (info.isUnion)
    return unionConstant(v, t, info, at, depth);
  if (v.kind == ConstValue::Kind::Unset)
    return std::string("zeroinitializer");
  std::vector<Decl*> members = partMembers(c); // (an anonymous member: one part, its class's object)
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
    uint32_t index;
    if (!partField(info, members[k], index))
      return std::nullopt;
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
    if (unit[i] && fieldTypes[i]->kind == TyKind::Array)
    {
      // A byte array's unit (SysV's, off Windows): its bytes, little-endian.
      value = "[";
      for (uint32_t b = 0; b < fieldTypes[i]->count; ++b)
        value += (b ? ", i8 " : "i8 ") + std::to_string(int((units[i] >> (8 * b)) & 0xff));
      value += "]";
    }
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
  if (!isClass(t) || depth > maxClassNesting)
    return false;
  StructInfo& info = structFor(t, at);
  for (Decl* m : info.fieldDecls)
  {
    if (m->flags & DF_Mutable)
      return true;
    // (a storage unit of bit fields: each of its fields, `mutable unsigned b : 5;` after `unsigned a : 3;`)
    if (m->kind == NodeKind::VariableDecl && bitfieldIn(info, m))
      for (Decl* f : unitMembers(info, classDef(t->decl, at), m))
        if (f->flags & DF_Mutable)
          return true;
  }
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
    if (v && pre->text)
    {
      if (evaluated)
        *evaluated = true;
      return pre->text; // (and its text made then: numbers alone, pureData)
    }
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

auto codegen::Emitter::referenceConstant(const Ty* t, Expr* value, VariableDecl* def, const TemplateArgs& args, Node* at) -> std::optional<std::string>
{
  Expr* init = referenceInitializer(def, value, t);
  const Ty* target = types.unqualified(types.stripRef(t));
  const Ty* given = init->ty && init->ty->isKnown() && !types.isDependent(init->ty) ? withCurrentArgs(types.unqualified(types.stripRef(init->ty))) : nullptr;
  std::vector<std::pair<std::string, uint32_t>> path;
  if (!given || !isLvalueExpression(init) || bindsBitField(init) ||
      (given != target && !(isClass(given) && isClass(target) && (canonicalClass(given, at) == canonicalClass(target, at) || basePath(given, target, path, at)))))
    return std::nullopt; // (a temporary, a conversion: bound at run time)
  UnaryExpr address{};
  address.kind = NodeKind::UnaryExpr;
  address.token = init->token;
  address.operand = init;
  address.op = uint32_t(P::Amp);
  address.isKeyword = false;
  address.postfix = false;
  bool layoutFailed = false;
  const Ty* pointer = types.pointer(types.stripRef(t));
  ConstContextScope context(*this, def);
  std::optional<ConstValue> v = global.evaluateInitializer(pointer, &address, def->file, def, args, foldHooks(at, layoutFailed));
  if (!v || v->kind != ConstValue::Kind::Address || !v->target || !v->target->decl || (v->target->decl->flags & DF_ThreadLocal))
    return std::nullopt; // (a thread_local has no address before a thread runs)
  return addressConstant(*v->target, at);
}

auto codegen::Emitter::takePreEvaluated(PreEvaluated& pre) -> void
{
  if (pre.used)
    return;
  pre.used = true;
  errors.insert(errors.end(), pre.errors.begin(), pre.errors.end());
}

auto codegen::Emitter::integerAddress(Expr* value) -> std::optional<int64_t>
{
  // The casts to a pointer around an integer constant: the integer, converted as the cast converts it
  // (an int32 sign-extended, an unsigned one zero-extended: the value folds as its own type's).
  Expr* e = value;
  bool cast = false;
  for (int depth = 0; e && depth < maxExpressionNesting; ++depth)
  {
    if (e->kind == NodeKind::ParenExpr)
      e = static_cast<ParenExpr*>(e)->inner;
    else if (e->kind == NodeKind::CastExpr && e->ty && types.stripRef(e->ty)->kind == TyKind::Pointer)
      e = static_cast<CastExpr*>(e)->operand, cast = true;
    else if (e->kind == NodeKind::NamedCastExpr && e->ty && types.stripRef(e->ty)->kind == TyKind::Pointer &&
             (static_cast<NamedCastExpr*>(e)->castKind == Kw::reinterpret_cast_ || static_cast<NamedCastExpr*>(e)->castKind == Kw::static_cast_))
      e = static_cast<NamedCastExpr*>(e)->operand, cast = true;
    else
      break;
  }
  if (!cast || !e || !e->ty)
    return std::nullopt;
  const Ty* from = types.unqualified(types.stripRef(e->ty));
  if (from->kind != TyKind::Int && from->kind != TyKind::Char && from->kind != TyKind::Enum)
    return std::nullopt;
  std::optional<int64_t> v = evalConst(e);
  if (!v)
    return std::nullopt;
  if (from->isUnsigned && from->bits < 64)
    return int64_t(uint64_t(*v) & ((1ull << from->bits) - 1));
  return v;
}

auto codegen::Emitter::preEvaluateGlobal(VariableDecl* v, Node* at, const Ty* classTy, PreEvaluated& pre) -> void
{
  TemplateArgs classArgs;
  if (classTy)
    classArgs = global.definitionArgs(variableDefinition(v, classTy), global.classArgsOf(classTy));
  ArgsScope argsScope(*this, classTy ? &classArgs : nullptr);
  Restore<const Ty*> classRestore(currentClassTy);
  currentClassTy = classTy;
  struct SharedScope
  {
    SharedScope(bool& f) : flag(f), saved(f) { flag = true; }
    ~SharedScope() { flag = saved; }

    bool& flag;
    bool saved;
  } sharedScope(emittingShared);
  const Ty* t = classTy ? global.typeOfDeclIn(v, classArgs) : global.typeOfDecl(v);
  if (!t || !t->isKnown() || t->isReference() || v->builtin)
    return;
  VariableDecl* def = variableDefinition(v, classTy);
  if (def == v && (v->flags & DF_Extern) && !v->init && !classTy)
    return;
  Expr* value = classTy ? global.instanceInitializer(def, classArgs) : def->init;
  const Ty* st = types.stripRef(t);
  Restore<ParsedFile*> fileRestore(currentFile);
  currentFile = def->file;
  Restore<bool> requiredRestore(requiredConstant);
  requiredConstant = constantRequired(v, def, t);
  if (!value)
  {
    // No initializer: a class's default initialization, as globalForOnce asks constantData for it
    // (a class that needs no construction is zero there, and asks for nothing).
    if (!isClass(st) && st->kind != TyKind::Array)
      return;
    pre.value = nullptr;
    pre.type = st;
    bool layoutFailed = false;
    ConstContextScope context(*this, def);
    GlobalScope::clearConstantReason();
    pre.data = global.evaluateInitializer(st, nullptr, def->file, def, classArgs, foldHooks(at, layoutFailed));
    pre.why = pre.data ? std::string() : GlobalScope::constantReason();
    pre.evaluated = true;
    if (pre.data && pureData(st, at))
      pre.text = constantText(*pre.data, st, at);
    return;
  }
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
    if (pre.data && pureData(st, at))
      pre.text = constantText(*pre.data, st, at);
  }
}

auto codegen::Emitter::pureData(const Ty* t, Node* at, int depth) -> bool
{
  if (!t || depth > maxClassNesting)
    return false;
  t = types.unqualified(t);
  switch (t->kind)
  {
    case TyKind::Bool: case TyKind::Char: case TyKind::Int: case TyKind::Enum: case TyKind::Float:
      return true;
    case TyKind::Array:
      return pureData(t->inner, at, depth + 1);
    case TyKind::Class:
    {
      if (!isClass(t) || types.isDependent(t))
        return false;
      const StructInfo& info = structFor(t, at);
      if (info.polymorphic || info.isUnion)
        return false;
      for (const Ty* field : info.fieldTypes)
        if (!pureData(field, at, depth + 1))
          return false;
      return true;
    }
    default:
      return false;
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
  // Defined for good by another emitter (or by this one): its published answer, without the shared
  // lock, which a global's definition holds (the emitters' first uses of each global queued behind
  // the startup globals' definitions: codegen-speed.md section 5).
  if (GlobalAnswer known; sh.finalGlobals.find({v, classTy}, known))
  {
    threadLocalCache.emplace(known.name, known.threadLocalInit);
    const std::string& mine = globalCache.emplace(std::pair<Decl*, const Ty*>{v, classTy}, std::move(known.name)).first->second;
    buildThreadLocal(mine);
    return mine;
  }
  std::string name;
  // The bodies its initializer's evaluation runs, before the lock and under it, those of a global it
  // reads too (added to the one evaluated around it on the way out): a constant's record
  // (Shared::constantGlobals, globalForOnce).
  std::vector<const FunctionDecl*> ran;
  struct RanScope
  {
    ~RanScope()
    {
      slot = around;
      if (around)
        around->insert(around->end(), ran.begin(), ran.end());
    }

    std::vector<const FunctionDecl*>*& slot;
    std::vector<const FunctionDecl*>* around;
    std::vector<const FunctionDecl*>& ran;
  } ranScope{globalRan, globalRan, ran};
  globalRan = &ran;
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
      if (pre.value || pre.evaluated)
        pre.errors.assign(errors.begin() + errorsBefore, errors.end()); // (reported if globalForOnce takes the values)
      errors.resize(errorsBefore);
      if (fn.size() == fnBefore && allocas.size() == allocasBefore)
        temp = tempBefore;
    }
  }
  Restore<PreEvaluated*> preRestore(preEvaluated);
  // (a global without an initializer too: its default initialization's value, constantData's; until
  // 2026-11-09 evaluated before the lock and then again under it, the game's 65,536-entry tables of
  // Tile with them)
  preEvaluated = pre.value || pre.evaluated ? &pre : nullptr;
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
      // same lines whichever use comes first). (A constant local of another function: why it is
      // none, refuseOutsideItsFunction's.)
      std::string why = isAutomaticLocal(v) ? lastFailure : "a use of the global " + qualifiedName(v) + ", whose definition failed (" + lastFailure + ")";
      sh.failedGlobals[key] = why;
      ++sh.failuresRemembered;
      errors.resize(errorsBefore);
      if (!pre.used && inlinedNow.size() > inlinedBefore)
        inlinedNow.resize(inlinedBefore);
      fail(at, why);
    }
    if (mayPublish()) // (not while another global's definition is under way: one used inside its own is not built yet)
    {
      auto init = threadLocalInits.find(name);
      GlobalAnswer answer{name, init == threadLocalInits.end() ? std::string() : init->second};
      threadLocalCache.emplace(name, answer.threadLocalInit);
      sh.finalGlobals.publish(key, answer);
    }
  }
  claim.reset(); // defined (or failed): another emitter waiting for it goes on
  if (!pre.used && inlinedNow.size() > inlinedBefore && (pre.value || pre.evaluated))
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
    fn += "  call void " + init + "()" + bundle() + "\n";
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
  Restore<const Ty*> classRestore(currentClassTy);
  currentClassTy = classTy;
  struct SharedScope
  {
    SharedScope(bool& f) : flag(f), saved(f) { flag = true; }
    ~SharedScope() { flag = saved; }

    bool& flag;
    bool saved;
  } sharedScope(emittingShared); // a global's initializer belongs to every module
  const Ty* t = classTy ? global.typeOfDeclIn(v, classArgs) : global.typeOfDecl(v);
  if (!t || !t->isKnown())
    unsupported(at, "a global of unknown type");
  // (`const auto NOTHING = ((void)0);`: no `@"NOTHING" = external global void`, which clang rejects.)
  if (types.unqualified(t)->kind == TyKind::Void)
    fail(at, "the variable " + qualifiedName(v) + " is of type void, which holds no value");
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
    const Ty* enumeration = classTy ? classTy : types.enumType(e); // (an instance's: staticMemberClass)
    std::string element = llType(enumUnderlying(enumeration), at), elements;
    for (Decl* d : e->enumerators)
      if (d->kind == NodeKind::EnumeratorDecl)
        elements += std::string(elements.empty() ? "" : ", ") + element + " " + std::to_string(enumeratorValue(static_cast<EnumeratorDecl*>(d), enumeration));
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
  Restore<bool> requiredRestore(requiredConstant);
  requiredConstant = constantRequired(v, def, t);
  // `thread_local IntrusiveList<CrashHookBase> CrashHooks::hooks;`: one per thread (TLS), built by
  // each thread at its first use (emitGlobalConstructor's perThread).
  bool threadLocal = ((v->flags | def->flags) & DF_ThreadLocal) != 0;
  if (t->isReference())
  {
    // `static const Style& defaultWidgetStyle = getStyleStatic();`: a pointer, bound at startup.
    currentFile = saved;
    if (isAutomaticLocal(v))
      refuseOutsideItsFunction(v, at); // (globalAddress refuses it first)
    if (threadLocal)
      unsupported(at, "a thread_local reference");
    // Bound to an object of static storage: constant initialization ([basic.start.static] 2), bound
    // from the program's start for whatever reads it first. (Until 2026-11-06 at startup: a global
    // initialized before it read through a null pointer.)
    if (value)
    {
      Restore<ParsedFile*> file(currentFile);
      currentFile = def->file;
      if (std::optional<std::string> target = referenceConstant(t, value, def, classArgs, at))
      {
        decls += "$" + name.substr(1) + " = comdat any\n" + name + " = linkonce_odr constant ptr " + *target + ", comdat" + debugGlobal(name, def, t) + "\n";
        return name;
      }
    }
    decls += "$" + name.substr(1) + " = comdat any\n" + name + " = linkonce_odr global ptr null, comdat" + debugGlobal(name, def, t) + "\n";
    if (value)
      emitGlobalConstructor(v, name, t, value, at, false, def);
    return name;
  }
  // A constant whose bytes no LLVM constant gives, for its vtable pointers: constant initialization
  // all the same ([basic.start.static] 2, `const Logger logger;` of a constexpr constructor), so
  // built before every other startup function, which may read it whatever their order. Its
  // constructor, one the interpreter ran, reads no global built at startup. (Until 2026-11-06 in its
  // place: a global initialized before it read zeros, and called through a null vtable pointer.)
  Restore<bool> vptrRestore(constantHasVptr);
  constantHasVptr = false;
  bool builtFirst = false;
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
    // An integer cast to the pointer (`reinterpret_cast<Targetable*>(254)`, `(void*)(long long)-1`,
    // INVALID_HANDLE_VALUE): that address, as clang and g++ emit it. (Until 2026-11-05 built at
    // startup, which a global initialized before it in its file read as null: `0 261` for `261 261`.)
    else if (std::optional<int64_t> address = st->kind == TyKind::Pointer ? [&] { ConstContextScope context(*this, def); return integerAddress(value); }() : std::nullopt)
      init = *address == 0 ? std::string("null") : "inttoptr (i64 " + std::to_string(*address) + " to ptr)";
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
      // expresses yet (a typed union's), which design.md's step 4 builds at startup. Never a
      // const local of another function, read from a local class's member function: its value is
      // its function's, at run time.
      if (isAutomaticLocal(v))
        refuseOutsideItsFunction(v, at, true);
      constructed = true;
      builtFirst = evaluated && constantHasVptr;
      // A constexpr variable needs its constant (design.md "A constexpr variable needs its
      // constant"): one the checker left to the code generator (its value needs exact layouts)
      // and that is no constant here either is an error, never a global built at startup, which
      // a global initialized before it reads as zero.
      if (!evaluated && requiredConstant && !global.checkerErrorWithin(def->file, def->token, def->token))
        error(def, std::string(((v->flags | def->flags) & DF_Constexpr) ? "the constexpr variable " : "the static member ") + sym(def->name.last().symbol) +
                     (classTy ? " of " + types.toString(classTy, &symbols) : std::string()) +
                     (((v->flags | def->flags) & DF_Constexpr) ? std::string() : std::string(", whose initializer in its class is a constant expression,")) +
                     " is no constant the compiler could evaluate" + (notConstantWhy.empty() ? std::string() : ": " + notConstantWhy));
    }
  }
  else if (needsConstruction(st, at))
  {
    // No initializer, a class with a constructor or default member initializers (`constexpr P p;`):
    // its default initialization as the interpreter computes it (GlobalScope::evaluateInitializer
    // with none), constant data as an initializer's value is, else built at startup. (Until
    // 2026-11-05 always built at startup, which a global initialized before it in its file read as
    // zero: thread D's spec language/constexpr_global_without_initializer_is_constant_data.)
    bool evaluated = false;
    if (std::optional<std::string> data = constantData(st, nullptr, def, classArgs, at, &evaluated))
      init = *data;
    else
    {
      if (isAutomaticLocal(v))
        refuseOutsideItsFunction(v, at, true); // (as one with an initializer)
      constructed = true;
      builtFirst = evaluated && constantHasVptr;
    }
  }
  currentFile = saved;
  // A thread_local that needs destroying is built by each thread at its first use, constant or not:
  // the destruction at the thread's exit is registered then (registerThreadExits).
  if (threadLocal && !constructed && needsDestruction(st, at))
  {
    constructed = true;
    init = "zeroinitializer";
  }
  // Constant data: what its evaluation ran is kept with it, for a server's build that changes one of
  // those bodies (Shared::constantGlobals).
  if (!constructed && globalRan)
  {
    std::vector<const FunctionDecl*> bodies = *globalRan;
    std::sort(bodies.begin(), bodies.end());
    bodies.erase(std::unique(bodies.begin(), bodies.end()), bodies.end());
    sh.constantGlobals[name] = Shared::ConstantGlobal{v, classTy, std::move(bodies)};
  }
  // Defined in every module, merged by the linker (one instance per program). Two entries: the
  // shared text is sorted, and the definition (an `@` line) has to come after the struct types
  // (`%` lines) a constant initializer names; the comdat line (`$`) would sort first.
  decls += "$" + name.substr(1) + " = comdat any\n";
  // (A destructor may write: `const Lock lock;`'s, a const object's as any other's.)
  bool destroyed = !threadLocal && needsDestruction(st, at);
  decls += name + " = linkonce_odr " + (threadLocal ? "thread_local " : "") +
           (t->isConst && !constructed && !destroyed && !hasMutableMember(st, at) ? "constant " : "global ") + llType(st, at) + " " + init + ", comdat" +
           alignSuffix(st, variableAlignment(def, def, at), at) + debugGlobal(name, def, t) + "\n";
  if (constructed)
  {
    Restore<std::optional<Shared::Startup>> startupRestore(startupAt);
    bool first = builtFirst && !threadLocal && !startupAt;
    if (first)
      startupAt = Shared::Startup{0, 0, 0, std::string()};
    emitGlobalConstructor(v, name, st, value, at, threadLocal, def, first ? Shared::GlobalBuild::BuildOnly : Shared::GlobalBuild::BuildAndExit);
    if (first)
      startupAt.reset();
    if (first && destroyed)
      emitGlobalConstructor(v, name, st, value, at, false, def, Shared::GlobalBuild::ExitOnly);
  }
  else if (destroyed)
    emitGlobalConstructor(v, name, st, value, at, false, def, Shared::GlobalBuild::ExitOnly); // constant data, destroyed all the same, in its place
  return name;
}
