// struct Emitter's members: typed unions; their declarations in TypedUnions.hpp, part of the
// struct's body in Codegen/Emitter.hpp. A translation unit of its own since 2026-11-05 (codegen.cpp
// split, stage 2), moved without a change.
#include "Emitter.hpp"

auto codegen::Emitter::typedUnionOf(const Ty* t, Node* at) -> ClassDecl*
{
  t = t ? types.unqualified(types.stripRef(t)) : nullptr;
  if (!isClass(t))
    return nullptr;
  ClassDecl* c = classDef(t->decl, at);
  return c && c->isTypedUnion ? c : nullptr;
}

auto codegen::Emitter::tagAddress(ClassDecl* c, const Ty* t, const std::string& obj, Node* at) -> std::string
{
  std::string addr;
  if (!fieldAddress(types.unqualified(types.stripRef(t)), c->tagField, obj, addr, at))
    unsupported(at, "a typed union without its tag");
  return addr;
}

auto codegen::Emitter::loadTag(ClassDecl* c, const Ty* t, const std::string& obj, Node* at) -> std::string
{
  return load(tagAddress(c, t, obj, at), tagType(c), at).text;
}

auto codegen::Emitter::alternativeType(ClassDecl* c, const Ty* t, size_t index, Node* at) -> const Ty*
{
  const Ty* cls = canonicalClass(types.unqualified(types.stripRef(t)), at);
  const Ty* alt = global.typeOfDeclIn(c->alternatives[index], global.classArgsOf(cls));
  if (!alt || !alt->isKnown())
    unsupported(at, "an alternative of a typed union whose type is unknown");
  return alt;
}

auto codegen::Emitter::alternativeAddress(ClassDecl* c, const Ty* t, const std::string& obj, size_t index, Node* at) -> std::string
{
  std::string addr;
  if (!fieldAddress(types.unqualified(types.stripRef(t)), c->alternatives[index], obj, addr, at))
    unsupported(at, "a typed union without its storage");
  return addr;
}

auto codegen::Emitter::alternativeForValue(ClassDecl* c, const Ty* t, const Ty* valueTy, Node* at) -> size_t
{
  const Ty* v = types.unqualified(types.stripRef(valueTy));
  std::vector<size_t> found;
  for (size_t i = 0; i < c->alternatives.size(); ++i)
    if (types.unqualified(alternativeType(c, t, i, at)) == v)
      found.push_back(i);
  auto name = [&](size_t i) { return sym(static_cast<EnumeratorDecl*>(static_cast<EnumDecl*>(c->tagEnum)->enumerators[i])->name); };
  if (found.empty())
    fail(at, "the typed union " + qualifiedName(c) + " has no alternative of type " + types.toString(v, &symbols));
  if (found.size() > 1)
    fail(at, "a " + types.toString(v, &symbols) + " is ambiguous for the typed union " + qualifiedName(c) + ": its alternatives " +
                 name(found[0]) + " and " + name(found[1]) + " both have that type (name one: " + qualifiedName(c) + "(" + qualifiedName(c) +
                 "::" + name(found[0]) + ", value), or u[" + name(found[0]) + "] = value)");
  return found[0];
}

auto codegen::Emitter::constantTag(ClassDecl* c, Expr* e) -> std::optional<int64_t>
{
  Expr* x = e;
  while (x && x->kind == NodeKind::ParenExpr)
    x = static_cast<ParenExpr*>(x)->inner;
  if (x && x->kind == NodeKind::MemberExpr)
  {
    auto* m = static_cast<MemberExpr*>(x);
    const Ty* bt = m->base && m->base->ty ? types.stripRef(m->base->ty) : nullptr;
    if (bt && bt->kind == TyKind::Member && !m->member.empty() && sym(m->member.last().symbol) == "tag")
      if (int64_t index = descriptorAlternative(c, bt); index >= 0)
        return index;
  }
  return evalConst(e);
}

auto codegen::Emitter::descriptorAlternative(ClassDecl* c, const Ty* descriptor) -> int64_t
{
  if (!descriptor || descriptor->kind != TyKind::Member)
    return -1;
  for (size_t i = 0; i < c->alternatives.size(); ++i)
    if (c->alternatives[i] == descriptor->decl)
      return int64_t(i);
  return -1;
}

auto codegen::Emitter::alternativeIndex(ClassDecl* c, IndexExpr* i, Node* at) -> size_t
{
  if (i->args.size() != 1)
    unsupported(at, "a typed union indexed with more than one alternative");
  const Ty* indexTy = i->args[0]->ty ? types.stripRef(i->args[0]->ty) : nullptr;
  std::optional<int64_t> v = indexTy && indexTy->kind == TyKind::Member ? descriptorAlternative(c, indexTy) : evalConst(i->args[0]);
  if (!v || *v < 0 || size_t(*v) >= c->alternatives.size())
    unsupported(at, "a typed union indexed with something other than one of its alternatives");
  return size_t(*v);
}

auto codegen::Emitter::alternativeLvalue(ClassDecl* c, Value base, IndexExpr* i, Node* at, bool check) -> std::string
{
  size_t index = alternativeIndex(c, i, at);
  if (check)
  {
    std::string tag = loadTag(c, base.ty, base.text, at);
    std::string same = newTemp();
    fn += "  " + same + " = icmp eq " + llType(tagType(c), at) + " " + tag + ", " + std::to_string(index) + "\n";
    std::string okL = newLabel("alt.ok"), failL = newLabel("alt.fail");
    fn += "  br i1 " + same + ", label %" + okL + ", label %" + failL + "\n";
    startBlock(failL);
    auto [line, column] = lineColumnOf(at->token);
    emitCall("", "void @kpp_assert_failed(ptr " + cStringConstant(programPathOf(currentFile)) + ", i32 " + std::to_string(line) + ", i32 " +
                     std::to_string(column) + ", ptr " + cStringConstant("the typed union holds another alternative") + ")", false);
    fn += "  unreachable\n";
    usesAssert = true;
    startBlock(okL);
  }
  return alternativeAddress(c, base.ty, base.text, index, at);
}

auto codegen::Emitter::setTag(ClassDecl* c, const Ty* t, const std::string& obj, size_t index, Node* at) -> void
{
  fn += "  store " + llType(tagType(c), at) + " " + std::to_string(index) + ", ptr " + tagAddress(c, t, obj, at) + "\n";
}

auto codegen::Emitter::constructAlternative(ClassDecl* c, const Ty* t, const std::string& obj, size_t index, Expr* init, Node* at) -> void
{
  // Holding none while the alternative is built (valueless: the tag Tag::count, which no switch on
  // it takes), its own tag once it is: a construction that throws leaves a union nothing destroys
  // again, as std::variant's valueless_by_exception (until 2026-11-06 the tag came first, and the
  // union destroyed the alternative never built: E's battery, kpp/typed_union_alternative_that_throws_is_not_destroyed).
  setTag(c, t, obj, c->alternatives.size(), at);
  const Ty* altTy = alternativeType(c, t, index, at);
  std::string addr = alternativeAddress(c, t, obj, index, at);
  if (init)
    initializeFrom(addr, altTy, init, at);
  else if (isClass(altTy))
    defaultConstruct(addr, altTy, at);
  else if (altTy->kind == TyKind::Array && needsDestruction(altTy, at))
    constructArray(addr, altTy, at);
  else
    storeZero(altTy, addr, at);
  setTag(c, t, obj, index, at);
}

auto codegen::Emitter::destroyAlternative(ClassDecl* c, const Ty* t, const std::string& obj, Node* at) -> void
{
  forHeldAlternative(c, t, obj, at,
                     [&](size_t i) { return needsDestruction(alternativeType(c, t, i, at), at); },
                     [&](size_t i) { destroy(alternativeAddress(c, t, obj, i, at), alternativeType(c, t, i, at), at); });
}

auto codegen::Emitter::copyAlternative(ClassDecl* c, const Ty* t, const std::string& dst, const std::string& src, bool move, Node* at) -> void
{
  // (dst holds none, valueless, until the copy is built: a copy that throws leaves dst nothing to
  // destroy, as constructAlternative; a valueless src copies as valueless.)
  std::string tag = loadTag(c, t, src, at);
  setTag(c, t, dst, c->alternatives.size(), at);
  forHeldAlternative(c, t, src, at, [&](size_t) { return true; }, [&](size_t i)
  {
    const Ty* altTy = alternativeType(c, t, i, at);
    std::string from = alternativeAddress(c, t, src, i, at), to = alternativeAddress(c, t, dst, i, at);
    if (isClass(altTy))
    {
      if (move)
        moveConstruct(to, from, altTy, at);
      else
        copyConstruct(to, from, altTy, at);
    }
    else
    {
      copyBytes(to, from, altTy, at);
    }
  });
  fn += "  store " + llType(tagType(c), at) + " " + tag + ", ptr " + tagAddress(c, t, dst, at) + "\n";
}

auto codegen::Emitter::assignAlternative(IndexExpr* i, Expr* rhs, Node* at) -> Value
{
  Value base = rvalue(i->base);
  ClassDecl* c = typedUnionOf(base.ty, at);
  size_t index = alternativeIndex(c, i, at);
  const Ty* altTy = alternativeType(c, base.ty, index, at);
  std::string addr = alternativeAddress(c, base.ty, base.text, index, at);
  // A value that is no class first: it may read what the union holds (`p[Size] = p[Text].size()`).
  if (!isClass(altTy) && altTy->kind != TyKind::Array)
  {
    std::string value = newObjectSlot(altTy, at);
    initializeFrom(value, altTy, rhs, at);
    destroyAlternative(c, base.ty, base.text, at);
    copyBytes(addr, value, altTy, at);
    setTag(c, base.ty, base.text, index, at);
    return load(addr, altTy, at);
  }
  // No operator= to call (a class with a move constructor and none, which std::variant refuses;
  // an array): the alternative held goes, the new one is built where it stays.
  if (!alternativeAssignable(altTy, at))
  {
    destroyAlternative(c, base.ty, base.text, at);
    constructAlternative(c, base.ty, base.text, index, rhs, at);
    return isClass(altTy) ? Value{addr, altTy, false} : load(addr, altTy, at);
  }
  std::string same = newTemp(), assignL = newLabel("alternative.assign"), otherL = newLabel("alternative.other"), doneL = newLabel("alternative.done");
  auto compareTag = [&] { fn += "  " + same + " = icmp eq " + llType(tagType(c), at) + " " + loadTag(c, base.ty, base.text, at) + ", " + std::to_string(index) + "\n"; };
  const Ty* rhsTy = rhs->ty && rhs->ty->isKnown() ? types.stripRef(rhs->ty) : nullptr;
  if (rhsTy && isClass(rhsTy) && classKey(rhsTy) == classKey(altTy) && (isLvalueExpression(rhs) || isXvalue(rhs)))
  {
    // An object: the alternative held is assigned from it, as std::variant; another goes for a
    // copy of it.
    Value from = rvalue(rhs);
    bool move = movable(rhs);
    compareTag();
    fn += "  br i1 " + same + ", label %" + assignL + ", label %" + otherL + "\n";
    startBlock(assignL);
    assignObject(addr, from.text, altTy, at, move);
    emitBranch(doneL);
    startBlock(otherL);
    destroyAlternative(c, base.ty, base.text, at);
    setTag(c, base.ty, base.text, c->alternatives.size(), at);
    if (move)
      moveConstruct(addr, from.text, altTy, at);
    else
      copyConstruct(addr, from.text, altTy, at);
    setTag(c, base.ty, base.text, index, at);
    emitBranch(doneL);
    startBlock(doneL);
    return Value{addr, altTy, false};
  }
  // A new value, written once, into the one place it goes: where the alternative stays when the
  // union holds another (built in place, no move: `target[a] = a.type(input)`, the game's loading,
  // kpp/typed_union_alternative_built_in_place), a temporary to assign from when it holds this one.
  std::string built = newLabel("alternative.build"), value = newObjectSlot(altTy, at), place = newTemp();
  compareTag();
  fn += "  br i1 " + same + ", label %" + built + ", label %" + otherL + "\n";
  startBlock(otherL);
  destroyAlternative(c, base.ty, base.text, at);
  setTag(c, base.ty, base.text, c->alternatives.size(), at);
  emitBranch(built);
  startBlock(built);
  fn += "  " + place + " = select i1 " + same + ", ptr " + value + ", ptr " + addr + "\n";
  initializeFrom(place, altTy, rhs, at);
  std::string setL = newLabel("alternative.set");
  fn += "  br i1 " + same + ", label %" + assignL + ", label %" + setL + "\n";
  startBlock(assignL);
  assignObject(addr, value, altTy, at, true);
  if (needsDestruction(altTy, at))
    destroy(value, altTy, at);
  emitBranch(doneL);
  startBlock(setL);
  setTag(c, base.ty, base.text, index, at);
  emitBranch(doneL);
  startBlock(doneL);
  return Value{addr, altTy, false};
}

auto codegen::Emitter::alternativeAssignable(const Ty* altTy, Node* at) -> bool
{
  return isClass(altTy) && (declaresAssignment(altTy, at) || !declaresMoveMember(classDef(altTy->decl, at), altTy, false, at));
}

auto codegen::Emitter::assignHeldAlternative(ClassDecl* c, const Ty* t, const std::string& dst, const std::string& src, bool move, Node* at) -> void
{
  forHeldAlternative(c, t, dst, at, [&](size_t) { return true; }, [&](size_t i)
  {
    const Ty* altTy = alternativeType(c, t, i, at);
    std::string from = alternativeAddress(c, t, src, i, at), to = alternativeAddress(c, t, dst, i, at);
    if (alternativeAssignable(altTy, at))
      assignObject(to, from, altTy, at, move);
    else if (isClass(altTy))
    {
      // (no operator= to call: as for another alternative, the union holding none meanwhile)
      std::string tag = loadTag(c, t, dst, at);
      destroy(to, altTy, at);
      setTag(c, t, dst, c->alternatives.size(), at);
      if (move)
        moveConstruct(to, from, altTy, at);
      else
        copyConstruct(to, from, altTy, at);
      fn += "  store " + llType(tagType(c), at) + " " + tag + ", ptr " + tagAddress(c, t, dst, at) + "\n";
    }
    else if (altTy->kind == TyKind::Array && needsDestruction(altTy, at))
      transferArray(to, from, altTy, at, move, true);
    else
      copyBytes(to, from, altTy, at);
  });
}

auto codegen::Emitter::alternativeTags(ClassDecl* c) -> std::vector<int64_t>
{
  std::vector<int64_t> tags;
  if (auto* e = static_cast<EnumDecl*>(c->tagEnum))
    for (Decl* d : e->enumerators)
      if (d->kind == NodeKind::EnumeratorDecl)
        tags.push_back(enumeratorValue(static_cast<EnumeratorDecl*>(d)));
  return tags;
}

auto codegen::Emitter::typedUnionEqual(ClassDecl* c, const std::string& a, const std::string& b, const Ty* t, Node* at) -> Value
{
  const Ty* underlying = enumUnderlying(c->tagEnum);
  Value ta = convert(Value{loadTag(c, t, a, at), tagType(c), false}, underlying, at);
  Value tb = convert(Value{loadTag(c, t, b, at), tagType(c), false}, underlying, at);
  std::string slot = "%equal" + std::to_string(temp++);
  allocas += "  " + slot + " = alloca i1\n";
  fn += "  store i1 false, ptr " + slot + "\n";
  std::string same = newTemp(), held = newLabel("unionequal.held"), done = newLabel("unionequal.done");
  fn += "  " + same + " = icmp eq " + llType(underlying, at) + " " + ta.text + ", " + tb.text + "\n";
  fn += "  br i1 " + same + ", label %" + held + ", label %" + done + "\n";
  blockTerminated = true;
  startBlock(held);
  fn += "  store i1 true, ptr " + slot + "\n"; // (two valueless unions are equal, as std::variant's)
  forEachHeldAlternative(c, ta, done, at, [&](size_t k) {
    Value e = partsEqual(alternativeAddress(c, t, a, k, at), alternativeAddress(c, t, b, k, at), alternativeType(c, t, k, at), at);
    fn += "  store i1 " + e.text + ", ptr " + slot + "\n";
  });
  startBlock(done);
  return load(slot, types.boolType(), at);
}

auto codegen::Emitter::typedUnionOrder(ClassDecl* c, const std::string& a, const std::string& b, const Ty* t, Node* at) -> Value
{
  const Ty* underlying = enumUnderlying(c->tagEnum);
  Value ta = convert(Value{loadTag(c, t, a, at), tagType(c), false}, underlying, at);
  Value tb = convert(Value{loadTag(c, t, b, at), tagType(c), false}, underlying, at);
  std::string slot = "%order" + std::to_string(temp++);
  allocas += "  " + slot + " = alloca i8\n";
  std::string type = llType(underlying, at);
  std::string less = newTemp(), byTag = newTemp(), same = newTemp();
  // A valueless union (the tag Tag::count, constructAlternative) is less than any other, as
  // std::variant's valueless_by_exception; else the alternatives' order.
  std::string none = std::to_string(c->alternatives.size()), aNone = newTemp(), bNone = newTemp(), bSome = newTemp(), tagLess = newTemp(),
              someLess = newTemp();
  fn += "  " + aNone + " = icmp eq " + type + " " + ta.text + ", " + none + "\n";
  fn += "  " + bNone + " = icmp eq " + type + " " + tb.text + ", " + none + "\n";
  fn += "  " + bSome + " = xor i1 " + bNone + ", true\n";
  fn += "  " + tagLess + " = icmp ult " + type + " " + ta.text + ", " + tb.text + "\n";
  fn += "  " + someLess + " = select i1 " + bNone + ", i1 false, i1 " + tagLess + "\n";
  fn += "  " + less + " = select i1 " + aNone + ", i1 " + bSome + ", i1 " + someLess + "\n";
  fn += "  " + byTag + " = select i1 " + less + ", i8 -1, i8 1\n";
  fn += "  store i8 " + byTag + ", ptr " + slot + "\n";
  std::string held = newLabel("unionorder.held"), done = newLabel("unionorder.done");
  fn += "  " + same + " = icmp eq " + type + " " + ta.text + ", " + tb.text + "\n";
  fn += "  br i1 " + same + ", label %" + held + ", label %" + done + "\n";
  blockTerminated = true;
  startBlock(held);
  fn += "  store i8 0, ptr " + slot + "\n";
  forEachHeldAlternative(c, ta, done, at, [&](size_t k) {
    Value o = orderOfParts(alternativeAddress(c, t, a, k, at), alternativeAddress(c, t, b, k, at), alternativeType(c, t, k, at), at);
    fn += "  store i8 " + o.text + ", ptr " + slot + "\n";
  });
  startBlock(done);
  return load(slot, types.intType(8), at);
}

auto codegen::Emitter::compareTag(BinaryExpr* b, bool negate) -> Value
{
  bool unionLeft = typedUnionOf(types.stripRef(b->lhs->ty), b) != nullptr;
  Expr* unionExpr = unionLeft ? b->lhs : b->rhs;
  Expr* tagExpr = unionLeft ? b->rhs : b->lhs;
  Value obj = rvalue(unionExpr);
  ClassDecl* c = typedUnionOf(obj.ty, b);
  const Ty* underlying = enumUnderlying(c->tagEnum);
  Value tag = convert(Value{loadTag(c, obj.ty, obj.text, b), tagType(c), false}, underlying, b);
  Value other = convert(rvalue(tagExpr), underlying, b);
  std::string r = newTemp();
  fn += "  " + r + " = icmp " + (negate ? "ne " : "eq ") + llType(underlying, b) + " " + tag.text + ", " + other.text + "\n";
  return {r, types.boolType(), false};
}
