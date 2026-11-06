// struct Emitter's members: assignments, operator calls, arithmetic, comparison and call
// expressions; their declarations in Operators.hpp, part of the struct's body in
// Codegen/Emitter.hpp. A translation unit of its own since 2026-11-05 (codegen.cpp split, stage 2),
// moved without a change.
#include "Emitter.hpp"

auto codegen::Emitter::isAssignment(P op) -> bool
{
  switch (op)
  {
    case P::Assign: case P::PlusAssign: case P::MinusAssign: case P::StarAssign: case P::SlashAssign: case P::PercentAssign:
    case P::AmpAssign: case P::PipeAssign: case P::CaretAssign: case P::ShlAssign: case P::ShrAssign:
      return true;
    default:
      return false;
  }
}

auto codegen::Emitter::baseOp(P op) -> P
{
  switch (op)
  {
    case P::PlusAssign: return P::Plus;
    case P::MinusAssign: return P::Minus;
    case P::StarAssign: return P::Star;
    case P::SlashAssign: return P::Slash;
    case P::PercentAssign: return P::Percent;
    case P::AmpAssign: return P::Amp;
    case P::PipeAssign: return P::Pipe;
    case P::CaretAssign: return P::Caret;
    case P::ShlAssign: return P::Shl;
    case P::ShrAssign: return P::Shr;
    default: return op;
  }
}

auto codegen::Emitter::mayWrite(Expr* e) -> bool
{
  if (!e)
    return false;
  auto classOperand = [&](Expr* x) { return x && x->ty && isClass(types.unqualified(types.stripRef(x->ty))); };
  auto any = [&](Span<Expr*> list) {
    for (Expr* x : list)
      if (mayWrite(x))
        return true;
    return false;
  };
  switch (e->kind)
  {
    case NodeKind::CallExpr: case NodeKind::NewExpr: case NodeKind::DeleteExpr: case NodeKind::ThrowExpr: case NodeKind::FunctionalCastExpr:
      return true;
    case NodeKind::BraceInitExpr: return classOperand(e) || any(static_cast<BraceInitExpr*>(e)->args);
    case NodeKind::ParenExpr: return mayWrite(static_cast<ParenExpr*>(e)->inner);
    case NodeKind::CastExpr: return classOperand(e) || mayWrite(static_cast<CastExpr*>(e)->operand);
    case NodeKind::NamedCastExpr: return classOperand(e) || mayWrite(static_cast<NamedCastExpr*>(e)->operand);
    case NodeKind::MemberExpr:
    {
      auto* m = static_cast<MemberExpr*>(e);
      return (m->op == P::Arrow && classOperand(m->base)) || mayWrite(m->base) || mayWrite(m->dynamicMember); // (a class's operator->)
    }
    case NodeKind::IndexExpr:
    {
      auto* x = static_cast<IndexExpr*>(e);
      return classOperand(x->base) || mayWrite(x->base) || any(x->args); // (a class's operator[])
    }
    case NodeKind::UnaryExpr:
    {
      auto* u = static_cast<UnaryExpr*>(e);
      if (u->isKeyword)
        return mayWrite(u->operand);
      P op = P(u->op);
      return op == P::PlusPlus || op == P::MinusMinus || classOperand(u->operand) || mayWrite(u->operand);
    }
    case NodeKind::BinaryExpr:
    {
      auto* b = static_cast<BinaryExpr*>(e);
      return (!b->isKeyword && isAssignment(P(b->op))) || classOperand(b->lhs) || classOperand(b->rhs) || mayWrite(b->lhs) || mayWrite(b->rhs);
    }
    case NodeKind::ConditionalExpr:
    {
      auto* c = static_cast<ConditionalExpr*>(e);
      return mayWrite(c->cond) || mayWrite(c->then) || mayWrite(c->else_);
    }
    case NodeKind::LambdaExpr:
      for (const LambdaCapture& c : static_cast<LambdaExpr*>(e)->captures)
        if (mayWrite(c.init))
          return true;
      return false;
    default:
      return false;
  }
}

auto codegen::Emitter::fixedPlace(Expr* e) -> bool
{
  while (e && e->kind == NodeKind::ParenExpr)
    e = static_cast<ParenExpr*>(e)->inner;
  if (e && e->kind == NodeKind::IndexExpr)
  {
    auto* x = static_cast<IndexExpr*>(e);
    const Ty* bt = x->base && x->base->ty ? types.unqualified(types.stripRef(x->base->ty)) : nullptr;
    if (!bt || x->args.size() != 1 || !fixedPlace(x->base))
      return false;
    if (typedUnionOf(bt, e))
      return true;
    return bt->kind == TyKind::Array && !mayWrite(x->args[0]) && evalConst(x->args[0]).has_value();
  }
  return fixedName(e);
}

auto codegen::Emitter::fixedName(Expr* e) -> bool
{
  if (!e)
    return false;
  if (e->kind == NodeKind::NameExpr)
  {
    Decl* d = static_cast<NameExpr*>(e)->resolved;
    return d && (d->kind == NodeKind::VariableDecl || d->kind == NodeKind::ParamDecl); // (a member named alone: `this->` it)
  }
  if (e->kind == NodeKind::UnaryExpr)
  {
    auto* u = static_cast<UnaryExpr*>(e);
    return !u->isKeyword && P(u->op) == P::Star && u->operand && u->operand->kind == NodeKind::ThisExpr; // `*this`
  }
  if (e->kind != NodeKind::MemberExpr)
    return false;
  auto* m = static_cast<MemberExpr*>(e);
  if (!m->resolved || m->resolved->kind != NodeKind::VariableDecl || m->dynamicMember)
    return false;
  if (m->op == P::Arrow)
    return m->base && m->base->kind == NodeKind::ThisExpr;
  return fixedPlace(m->base);
}

auto codegen::Emitter::verifyDecisions() -> bool
{
  static const bool on = [] { const char* v = std::getenv("KPP_VERIFY_DECISIONS"); return v && *v && *v != '0'; }();
  return on;
}

auto codegen::Emitter::operatorCallName(OperatorCall c) -> const char*
{
  switch (c)
  {
    case OperatorCall::Undecided: return "undecided";
    case OperatorCall::Builtin: return "the built-in operator";
    case OperatorCall::Function: return "a function";
    case OperatorCall::Rewritten: return "a rewritten comparison";
  }
  return "?";
}

auto codegen::Emitter::verifyRangeFunctions(RangeForStmt* f, bool members, FunctionDecl* begin, FunctionDecl* end, const Ty* range) -> void
{
  if (members == f->rangeMembers && begin == f->begin && end == f->end)
    return;
  auto name = [&](FunctionDecl* g) { return g ? qualifiedName(g) + " (" + locationOf(g, g->file) + ")" : std::string("none"); };
  std::string where = currentFunctionName.empty() ? std::string() : " (in " + currentFunctionName + ")";
  error(f, "decision check: codegen's range-for over " + types.toString(range, &symbols) + " calls " + (members ? "the members " : "the free ") +
               name(begin) + " and " + name(end) + ", the checker chose " + (f->rangeMembers ? "the members " : "the free ") + name(f->begin) +
               " and " + name(f->end) + where);
}

auto codegen::Emitter::findMemberFunction(const Ty* classTy, const std::string& name, const std::vector<const Ty*>& argTypes, const Ty* objectTy, Node* at, const Ty** foundIn, bool templates) -> FunctionDecl*
{
  FunctionDecl* found = nullptr;
  std::function<void(const Ty*)> search = [&](const Ty* cls)
  {
    // A closure as a base (the overload-set idiom given lambdas): no member function of its own but
    // its call operator, which a call through the derived class does not reach yet.
    if (isLambda(cls))
    {
      if (name == "operator()")
        unsupported(at, "the call operator of a lambda's closure as a base class (" + types.toString(cls, &symbols) + ")");
      return;
    }
    ClassDecl* c = classDef(cls->decl, at);
    std::vector<FunctionDecl*> candidates;
    for (Decl* m : c->members)
    {
      if (m->kind != NodeKind::FunctionDecl || (!templates && !m->templateParams.empty()))
        continue;
      auto* f = static_cast<FunctionDecl*>(m);
      const std::string& q = qualifiedName(f); // ends with "::" + name
      if (q.size() > name.size() + 1 && q.ends_with(name) && q.compare(q.size() - name.size() - 2, 2, "::") == 0)
        candidates.push_back(f);
    }
    // A using-declaration of the name (`using A::operator(); using B::operator();`, `using
    // X::operator();` with X a parameter of the class, `using Ts::operator()...;`): the functions
    // of the bases it names are candidates too, as the checker has them; the first base's alone
    // called A's operator()(int) for o(3.0) (B's double), its result read as a double.
    std::unordered_map<FunctionDecl*, const Ty*> usedFrom;
    TemplateArgs classArgs = global.classArgsOf(cls);
    for (Decl* m : c->members)
    {
      if (m->kind != NodeKind::UsingNameDecl || static_cast<UsingNameDecl*>(m)->isEnum)
        continue;
      const QualifiedName& un = static_cast<UsingNameDecl*>(m)->name;
      if (un.parts.size() != 2 || un.parts[0].hasTemplateArgs)
        continue;
      const NamePart& last = un.last();
      std::string usedName = last.special == 2 && last.opToken < uint32_t(P::Count) ? operatorName(P(last.opToken))
                             : last.special == 0 ? sym(last.symbol) : std::string();
      if (usedName.empty() || usedName != name)
        continue;
      // The bases it names: one written by its name, or the argument (each element of a pack)
      // of the class's parameter of that name.
      std::vector<const Ty*> named;
      for (const auto& [param, arg] : classArgs.pairs)
        if (param && param->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(param)->name == un.parts[0].symbol && arg)
        {
          if (arg->kind == TyKind::Pack)
            named.insert(named.end(), arg->args.begin(), arg->args.end());
          else
            named.push_back(arg);
        }
      StructInfo& info = structFor(cls, at);
      for (auto& [baseDef, index] : info.bases)
      {
        const Ty* base = info.fieldTypes[index];
        bool chosen = false;
        for (const Ty* n : named)
          chosen = chosen || (isClass(n) && canonicalClass(n, at) == canonicalClass(base, at));
        if (named.empty())
          if (ClassDecl* bd = classDef(base->decl, at); bd && !bd->name.empty() && bd->name.last().symbol == un.parts[0].symbol)
            chosen = true;
        if (!chosen)
          continue;
        for (Decl* bm : classDef(base->decl, at)->members)
        {
          if (bm->kind != NodeKind::FunctionDecl || (!templates && !bm->templateParams.empty()))
            continue;
          auto* f = static_cast<FunctionDecl*>(bm);
          const std::string& q = qualifiedName(f);
          if (q.size() > name.size() + 1 && q.ends_with(name) && q.compare(q.size() - name.size() - 2, 2, "::") == 0)
          {
            candidates.push_back(f);
            usedFrom[f] = canonicalClass(base, at);
          }
        }
      }
    }
    if (!candidates.empty())
    {
      const Ty* obj = objectTy ? objectTy : cls;
      found = global.chooseOverload(candidates, argTypes, classArgs, obj);
      if (found)
      {
        if (foundIn)
          *foundIn = usedFrom.count(found) ? usedFrom[found] : cls;
        return;
      }
    }
    StructInfo& info = structFor(cls, at);
    std::vector<const Ty*> baseTypes;
    for (auto& [baseDef, index] : info.bases)
      baseTypes.push_back(info.fieldTypes[index]);
    for (const Ty* b : baseTypes)
      if (!found)
        search(b);
  };
  search(canonicalClass(classTy, at));
  return found;
}

auto codegen::Emitter::findOperator(const Ty* classTy, const std::string& name, size_t argCount, Node* at, const Ty** foundIn) -> FunctionDecl*
{
  std::vector<const Ty*> neutral(argCount, nullptr);
  return findMemberFunction(classTy, name, neutral, nullptr, at, foundIn);
}

auto codegen::Emitter::withCheckedConst(Value v, Expr* e) -> Value
{
  if (v.ty && !v.ty->isConst && isClass(v.ty) && constObject(e))
    v.ty = types.qualified(v.ty, true, v.ty->isVolatile);
  return v;
}

auto codegen::Emitter::constObject(Expr* e, int depth) -> bool
{
  if (!e || depth > 16)
    return false;
  if (e->ty && types.stripRef(e->ty)->isConst)
    return true;
  if (e->kind == NodeKind::ParenExpr)
    return constObject(static_cast<ParenExpr*>(e)->inner, depth + 1);
  if (e->kind != NodeKind::MemberExpr)
    return false;
  auto* m = static_cast<MemberExpr*>(e);
  if (!m->resolved || m->resolved->kind != NodeKind::VariableDecl || (m->resolved->flags & (DF_Mutable | DF_Static)))
    return false;
  // A reference member names the object it refers to, const only as that object's type is (the
  // checker's type, above): MapGenerator's `this->surface.hiddenTiles[position] = tile` in a
  // const member function, with `Surface& surface`, called the const operator[] and assigned
  // to its copy.
  if (const Ty* ft = declType(m->resolved); ft && ft->isReference())
    return false;
  if (m->op != P::Arrow)
    return constObject(m->base, depth + 1);
  if (m->base && m->base->kind == NodeKind::ThisExpr && currentFunction && (currentFunction->flags & DF_Const))
    return true;
  const Ty* bt = m->base && m->base->ty ? types.stripRef(m->base->ty) : nullptr;
  return bt && bt->kind == TyKind::Pointer && bt->inner && bt->inner->isConst;
}

auto codegen::Emitter::operatorCall(Value object, const std::string& name, Span<Expr*> args, Node* at, bool required, const std::string& sret) -> Value
{
  const Ty* foundIn = nullptr;
  FunctionDecl* f = findMemberFunction(object.ty, name, argTypesOf(args), object.ty, at, &foundIn, true);
  if (!f)
  {
    if (required)
      unsupported(at, qualifiedName(object.ty->decl) + " has no " + name);
    return {"", nullptr};
  }
  return memberCall(object, f, foundIn, args, at, sret);
}

auto codegen::Emitter::declaringClass(const Ty* objectTy, FunctionDecl* f, Node* at) -> const Ty*
{
  std::function<const Ty*(const Ty*)> search = [&](const Ty* cls) -> const Ty*
  {
    if (classDef(cls->decl, at) == f->parent)
      return cls;
    StructInfo& info = structFor(cls, at);
    std::vector<const Ty*> baseTypes;
    for (auto& [baseDef, index] : info.bases)
      baseTypes.push_back(info.fieldTypes[index]);
    for (const Ty* b : baseTypes)
      if (const Ty* found = search(b))
        return found;
    return nullptr;
  };
  return search(canonicalClass(types.unqualified(types.stripRef(objectTy)), at));
}

auto codegen::Emitter::memberCall(Value object, FunctionDecl* f, const Ty* foundIn, Span<Expr*> args, Node* at, const std::string& sret) -> Value
{
  std::string thisArg = object.text;
  if (canonicalClass(object.ty, at) != foundIn)
  {
    std::string addr;
    if (baseAddress(object.ty, foundIn, object.text, addr, at))
      thisArg = addr;
  }
  TemplateArgs fnArgs = global.classArgsOf(foundIn);
  if (!f->templateParams.empty())
    global.deduceArgs(f, argTypesOf(args), fnArgs); // `template<typename U> bool operator==(const U*) const`
  std::string indirect;
  if (isClass(foundIn) && f->templateParams.empty() && structFor(foundIn, at).polymorphic)
    if (int slot = slotOf(foundIn, f, at); slot >= 0)
      indirect = loadSlot(thisArg, slot); // `*a == *b` on images: the dynamic type's operator==, through the declaring class's vtable pointer
  return callFunction(f, fnArgs, foundIn, thisArg, sret, args, at, indirect);
}

auto codegen::Emitter::arithmetic(P op, Value l, Value r, Node* at) -> Value
{
  bool lp = l.ty->kind == TyKind::Pointer, rp = r.ty->kind == TyKind::Pointer;
  std::string res = newTemp();
  if (op == P::Plus || op == P::Minus)
  {
    if (lp && rp && op == P::Minus)
    {
      // Only pointers to one type subtract (C++'s [expr.add] 2): `&x - &y` of an int and a long
      // long, or `q - p` of a long long* and an int*, counted a difference in the left's elements
      // (an exact division by its size that is not exact is poison). Imported C keeps C's rule.
      const Ty* li = l.ty->inner ? types.unqualified(l.ty->inner) : nullptr, *ri = r.ty->inner ? types.unqualified(r.ty->inner) : nullptr;
      if (li && ri && li->isKnown() && ri->isKnown() && !types.isDependent(li) && !types.isDependent(ri) && !(currentFile && currentFile->isImported) &&
          li != ri && !(isClass(li) && isClass(ri) && canonicalClass(li, at) == canonicalClass(ri, at)))
        fail(at, "a difference of pointers to different types (" + types.toString(l.ty, &symbols) + " and " + types.toString(r.ty, &symbols) +
                     "): only pointers to one type subtract");
      std::string a = newTemp(), b = newTemp(), d = newTemp();
      fn += "  " + a + " = ptrtoint ptr " + l.text + " to i64\n";
      fn += "  " + b + " = ptrtoint ptr " + r.text + " to i64\n";
      fn += "  " + d + " = sub i64 " + a + ", " + b + "\n";
      fn += "  " + res + " = sdiv exact i64 " + d + ", " + std::to_string(std::max<uint64_t>(1, sizeOf(l.ty->inner, at))) + "\n";
      return {res, types.ptrdiffType()};
    }
    if (lp || rp)
    {
      if (rp)
        std::swap(l, r);
      // C++ has no arithmetic on a pointer to void or to a function (no element size): an error,
      // never a `getelementptr inbounds void` LLVM rejects.
      if (!l.ty->inner || l.ty->inner->kind == TyKind::Void || l.ty->inner->kind == TyKind::Function || !l.ty->inner->isKnown())
        unsupported(at, "arithmetic on a pointer to " + (l.ty->inner ? types.toString(l.ty->inner, &symbols) : std::string("?")));
      Value idx = convert(r, types.intType(64), at);
      std::string i = idx.text;
      if (op == P::Minus)
      {
        std::string neg = newTemp();
        fn += "  " + neg + " = sub i64 0, " + i + "\n";
        i = neg;
      }
      fn += "  " + res + " = getelementptr inbounds " + llType(l.ty->inner, at) + ", ptr " + l.text + ", i64 " + i + "\n";
      return {res, l.ty};
    }
  }
  // C++ has no other arithmetic on pointers (`(LPWSTR)&msg` with LPWSTR read as a variable is
  // `LPWSTR & msg`): an error, not an instruction LLVM rejects.
  if (lp || rp)
  {
    std::string spelling(punctuatorSpelling(op));
    unsupported(at, spelling + " on a pointer (" + types.toString(l.ty, &symbols) + " " + spelling + " " + types.toString(r.ty, &symbols) + ")");
  }
  // A shift has its left operand's promoted type, the count converted to it (LLVM shifts two
  // operands of one type), never a type common to both: `position.x >> sideBitShift` with an
  // int32 x and a uint32 count is an arithmetic shift of an int32 (ChunkPosition(TilePosition)).
  // An enum class has no arithmetic of its own (C++'s [expr.arith.conv]: no usual conversions for
  // it): only its overloaded operators, chosen before this.
  if (scopedEnum(l.ty) || scopedEnum(r.ty))
    fail(at, std::string(punctuatorSpelling(op)) + " on a value of an enum class (" + types.toString(l.ty, &symbols) + " " + std::string(punctuatorSpelling(op)) + " " +
                 types.toString(r.ty, &symbols) + "): an enum class converts to a number only by a cast");
  const Ty* ct = op == P::Shl || op == P::Shr ? promoted(l.ty) : commonArithmetic(l.ty, r.ty);
  {
    bool outer = operandConversion;
    operandConversion = true;
    l = convert(l, ct, at);
    r = convert(r, ct, at);
    operandConversion = outer;
  }
  bool isFloat = ct->kind == TyKind::Float;
  bool isUnsigned = ct->isUnsigned;
  const char* ins = nullptr;
  switch (op)
  {
    case P::Plus: ins = isFloat ? "fadd" : "add"; break;
    case P::Minus: ins = isFloat ? "fsub" : "sub"; break;
    case P::Star: ins = isFloat ? "fmul" : "mul"; break;
    case P::Slash: ins = isFloat ? "fdiv" : isUnsigned ? "udiv" : "sdiv"; break;
    case P::Percent: ins = isFloat ? "frem" : isUnsigned ? "urem" : "srem"; break;
    case P::Amp: ins = "and"; break;
    case P::Pipe: ins = "or"; break;
    case P::Caret: ins = "xor"; break;
    case P::Shl: ins = "shl"; break;
    case P::Shr: ins = isUnsigned ? "lshr" : "ashr"; break;
    default: unsupported(at, "this operator");
  }
  fn += "  " + res + " = " + ins + " " + llType(ct, at) + " " + l.text + ", " + r.text + "\n";
  return {res, ct};
}

auto codegen::Emitter::compare(P op, Value l, Value r, Node* at) -> Value
{
  std::string res = newTemp();
  // Pointers to members (== and != only): both as the member pointer's type (nullptr or the
  // literal 0 becomes its null), a data member's offset (-1 when null) or a function.
  if ((l.ty->kind == TyKind::MemberPointer || r.ty->kind == TyKind::MemberPointer) && (op == P::EqEq || op == P::NotEq))
  {
    const Ty* mt = l.ty->kind == TyKind::MemberPointer ? l.ty : r.ty;
    l = convert(l, mt, at);
    r = convert(r, mt, at);
    bool function = mt->inner && mt->inner->kind == TyKind::Function;
    fn += "  " + res + " = icmp " + (op == P::EqEq ? "eq " : "ne ") + (function ? "ptr " : "i64 ") + l.text + ", " + r.text + "\n";
    return {res, types.boolType()};
  }
  if (l.ty->kind == TyKind::Pointer || r.ty->kind == TyKind::Pointer || l.ty->kind == TyKind::Nullptr || r.ty->kind == TyKind::Nullptr)
  {
    const Ty* pt = l.ty->kind == TyKind::Pointer ? l.ty : r.ty->kind == TyKind::Pointer ? r.ty : types.pointer(types.voidType());
    // Both pointers: their composite pointer type ([expr.type]/4), the one the other converts to
    // (`this != &other`, a `C*` and a `const C*`, compare as `const C*`).
    if (l.ty->kind == TyKind::Pointer && r.ty->kind == TyKind::Pointer && l.ty != r.ty && l.ty->inner && r.ty->inner)
    {
      // (each side's pointee with the other's qualifiers too: `const Car*` against `Entity*` is
      // a `const Entity*`, the base)
      const Ty* a = l.ty->inner;
      const Ty* b = r.ty->inner;
      const Ty* lq = types.pointer(types.qualified(a, b->isConst, b->isVolatile));
      const Ty* rq = types.pointer(types.qualified(b, a->isConst, a->isVolatile));
      if (impliedPointerConversion(r.ty, lq, at))
        pt = lq;
      else if (impliedPointerConversion(l.ty, rq, at))
        pt = rq;
    }
    l = convert(l, pt, at);
    r = convert(r, pt, at);
    const char* pred = op == P::EqEq ? "eq" : op == P::NotEq ? "ne" : op == P::Less ? "ult" : op == P::LessEq ? "ule" : op == P::Greater ? "ugt" : "uge";
    fn += "  " + res + " = icmp " + pred + " ptr " + l.text + ", " + r.text + "\n";
    return {res, types.boolType()};
  }
  // An enum class compares with its own type only (C++: no usual conversions for it).
  if ((scopedEnum(l.ty) || scopedEnum(r.ty)) && !(l.ty->kind == TyKind::Enum && r.ty->kind == TyKind::Enum &&
                                                  types.unqualified(l.ty)->decl == types.unqualified(r.ty)->decl))
    fail(at, "a comparison of a value of an enum class with another type (" + types.toString(l.ty, &symbols) + " and " + types.toString(r.ty, &symbols) +
                 "): an enum class converts to a number only by a cast");
  const Ty* ct = (l.ty->kind == TyKind::Bool && r.ty->kind == TyKind::Bool) ? types.boolType() : commonArithmetic(l.ty, r.ty);
  {
    bool outer = operandConversion;
    operandConversion = true;
    l = convert(l, ct, at);
    r = convert(r, ct, at);
    operandConversion = outer;
  }
  if (ct->kind == TyKind::Float)
  {
    const char* pred = op == P::EqEq ? "oeq" : op == P::NotEq ? "une" : op == P::Less ? "olt" : op == P::LessEq ? "ole" : op == P::Greater ? "ogt" : "oge";
    fn += "  " + res + " = fcmp " + pred + " " + llType(ct, at) + " " + l.text + ", " + r.text + "\n";
  }
  else
  {
    bool u = ct->isUnsigned || ct->kind == TyKind::Bool;
    const char* pred = op == P::EqEq ? "eq" : op == P::NotEq ? "ne" : op == P::Less ? (u ? "ult" : "slt") : op == P::LessEq ? (u ? "ule" : "sle")
                     : op == P::Greater ? (u ? "ugt" : "sgt") : (u ? "uge" : "sge");
    fn += "  " + res + " = icmp " + pred + " " + llType(ct, at) + " " + l.text + ", " + r.text + "\n";
  }
  return {res, types.boolType()};
}

auto codegen::Emitter::emitBranch(const std::string& target) -> void
{
  if (!blockTerminated)
    fn += "  br label %" + target + "\n";
  blockTerminated = true;
}

auto codegen::Emitter::startBlock(const std::string& name) -> void
{
  fn += name + ":\n";
  blockTerminated = false;
  currentBlock = name;
}

auto codegen::Emitter::callValue(CallExpr* c, const std::string& sret) -> Value
{
  Expr* callee = c->callee;
  if (callee->kind == NodeKind::NameExpr)
  {
    auto* n = static_cast<NameExpr*>(callee);
    Decl* d = n->resolved;
    if (d && d->kind == NodeKind::FunctionDecl)
      if (int which = global.intrinsicOf(static_cast<FunctionDecl*>(d)))
        return enumIntrinsic(which, c);
    if (!d && n->name.parts.size() > 1 && n->name.last().special == 2 && P(n->name.last().opToken) == P::Assign && c->args.size() == 1)
    {
      // `super::operator=(other)` of a class declaring none: its implicit assignment, as the
      // walker typed it, on this object (`this->super::operator=(other)`).
      const Ty* q = qualifierOf(n);
      q = q ? types.unqualified(types.stripRef(q)) : nullptr;
      if (!q || !isClass(q) || !currentClassTy)
        unsupported(c, "operator= qualified with no class of this object");
      return implicitAssignment(thisPointer(c), currentClassTy, canonicalClass(q, c), c->args[0], c);
    }
    if (d && d->kind == NodeKind::FunctionDecl && d->file && d->file->isImported)
    {
      // An intrinsic declared by intrin.h (its static inline body is not emitted, or there is
      // none): the code generator's own IR.
      if (std::optional<Value> v = sseBuiltin(n, c))
        return *v;
      if (std::optional<Value> v = msvcBuiltin(n, c))
        return *v;
      if (std::optional<Value> v = gccBuiltin(n, c))
        return *v;
    }
    if (d && d->kind == NodeKind::FunctionDecl)
    {
      auto* f = static_cast<FunctionDecl*>(d);
      std::string thisArg;
      const Ty* classTy = nullptr;
      if (Decl* owner = global.owningClass(f); owner && !(f->flags & DF_Static))
      {
        if (!currentClassTy)
          unsupported(c, "a method call outside a method");
        thisArg = thisPointer(c);
        classTy = isClass(currentClassTy) ? currentClassTy : nullptr;
        if (owner->kind == NodeKind::ClassDecl && classTy && classDef(owner, c) != classDef(currentClassTy->decl, c))
        {
          // Inherited method: find the base subobject of the right class.
          const Ty* base = baseInstantiation(currentClassTy, owner, c);
          std::string addr;
          if (base && baseAddress(currentClassTy, base, thisArg, addr, c))
          {
            thisArg = addr;
            classTy = base;
          }
        }
        else if (owner->kind == NodeKind::ClassDecl && classTy && n->name.parts.size() > 1)
          if (const Ty* q = qualifierType(n, c); q && classDef(q->decl, c) == classDef(owner, c) && q != canonicalClass(currentClassTy, c))
          {
            // `Chain<Tail...>::count()` in Chain<Head, Tail...>: the member of the base the
            // qualifier names, an instance of the same template as this class.
            std::string addr;
            if (!baseAddress(currentClassTy, q, thisArg, addr, c))
              unsupported(c, "a call of " + qualifiedName(f) + " through " + types.toString(q, &symbols) + ", which is no base of " +
                                 types.toString(currentClassTy, &symbols));
            thisArg = addr;
            classTy = q;
          }
      }
      std::string indirect;
      if (classTy && n->name.parts.size() == 1 && !thisArg.empty())
        if (int slot = slotOf(classTy, f, c); slot >= 0)
          indirect = loadSlot(thisArg, slot);
      if (!classTy && (f->flags & DF_Static) && n->name.parts.size() == 1 && currentClassTy && isClass(currentClassTy))
        if (Decl* owner = global.owningClass(f); owner && owner->kind == NodeKind::ClassDecl)
        {
          // `listOptions()` inside Item<Mode>: a static member of the class being emitted (or
          // of a base), with that instantiation's arguments.
          if (classDef(owner, c) == classDef(currentClassTy->decl, c))
            classTy = currentClassTy;
          else if (const Ty* base = baseInstantiation(currentClassTy, owner, c))
            classTy = base;
        }
      if (!classTy && (f->flags & DF_Static) && n->name.parts.size() > 1 && global.owningClass(f))
      {
        // `Box<T>::get()`: the instantiation comes from the qualifier.
        QualifiedName qualifier = n->name;
        qualifier.parts.count -= 1;
        NamedType tmp{};
        tmp.kind = NodeKind::NamedType;
        tmp.token = n->token;
        tmp.name = qualifier;
        TemplateArgs none;
        const Ty* q = global.typeOfAstIn(&tmp, typingContext(), currentArgs ? *currentArgs : none);
        if (!q || !q->isKnown() || types.isDependent(q))
          if (const Ty* bound = boundParameter(qualifier))
            q = bound;
        // `T::computeHash(args...)` with T a class deriving from the template that declares it
        // (ComplexExpressionWithOperation<BasisNoise> from ComplexExpression<BasisNoise, 2, 6>):
        // the member of that base instance, with its arguments.
        const Ty* declaring = q && isClass(q) ? instantiationIn(q, global.owningClass(f), c) : nullptr;
        if (declaring && !declaring->args.empty())
          classTy = declaring;
        else if (q && isClass(q) && !q->args.empty())
          classTy = canonicalClass(q, c);
      }
      // move(x) and forward(x): identity casts (lref -> rref in the type system,
      // but both are just a pointer in LLVM IR).
      if (!classTy && f->params.size() == 1 && c->args.size() == 1 &&
          !f->templateParams.empty() && c->templateArgCount != f->templateParams.size())
      {
        std::string_view fname = f->name.empty() ? "" : symbols.name(f->name.last().symbol);
        if (fname == "move" || fname == "forward")
        {
          std::string addr = lvalue(c->args[0]);
          const Ty* argTy = c->args[0]->ty;
          if (argTy) argTy = types.stripRef(argTy);
          return {addr, argTy ? argTy : types.unknown(), false};
        }
      }
      if (!classTy && (f->flags & DF_Friend) && (f->flags & DF_Static))
        classTy = friendClass(f, argTypesOf(c->args), c); // `unwrap(holder)`: Holder<int>'s friend
      return callFunction(f, calleeArgs(f, classTy, c, c), classTy, thisArg, sret, c->args, c, indirect);
    }
    if (!d)
      if (std::optional<Value> v = vaBuiltin(n, c))
        return *v;
    if (!d)
      if (std::optional<Value> v = sseBuiltin(n, c))
        return *v;
    if (!d || d->kind == NodeKind::ClassDecl || d->kind == NodeKind::EnumDecl || d->kind == NodeKind::UsingAliasDecl ||
        (d->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(d)->isType) || // `func(a, b)` with a function as a constant parameter: a call
        (d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef)))
    {
      const Ty* t = c->ty;
      if (!t || !t->isKnown())
      {
        // Try to derive the type from the resolved declaration.
        if (d && d->kind == NodeKind::ClassDecl)
        {
          NamedType tmp{};
          tmp.kind = NodeKind::NamedType;
          tmp.name = n->name;
          TemplateArgs none;
          t = global.typeOfAstIn(&tmp, typingContext(), currentArgs ? *currentArgs : none);
          if (t) t = types.unqualified(types.stripRef(t));
          if (!t || !t->isKnown())
            t = types.classType(static_cast<ClassDecl*>(d), {});
        }
        else if (d && d->kind == NodeKind::EnumDecl)
          t = types.enumType(static_cast<ClassDecl*>(d));
        else if (d && (d->kind == NodeKind::UsingAliasDecl || d->kind == NodeKind::TemplateParamDecl))
        {
          t = declType(d);
          if (t) t = types.unqualified(types.stripRef(t));
        }
        if (!t || !t->isKnown())
          unsupported(c, "a call that did not resolve");
      }
      if (isClass(t))
      {
        std::string addr = sret.empty() ? newObjectSlot(t, c) : sret;
        directInitialize(addr, t, c->args, false, c, true); // `Widths({{120, 0}})`: the list is the constructor's argument
        if (sret.empty())
          statementTemps.push_back({addr, t});
        return {addr, t, sret.empty()};
      }
      if (c->args.size() == 1 && isPackExpansion(c->args[0]))
      {
        // `T(forward<Args>(args)...)` with a scalar T: the pack's one element, re-typed for it.
        Expr* pattern = static_cast<UnaryExpr*>(c->args[0])->operand;
        size_t count = packElementCount(pattern, c);
        if (count == 0)
          return {t->kind == TyKind::Float ? "0.0" : t->kind == TyKind::Pointer ? "null" : "0", t};
        if (count != 1)
          unsupported(c, "a scalar built from a pack of several elements");
        Value v;
        forEachPackElement(pattern, c, [&](size_t) { v = explicitConvert(rvalue(pattern), t, c); });
        return v;
      }
      if (c->args.size() == 1)
        return explicitConvert(rvalue(c->args[0]), t, c); // `uintptr_t(&x)` is the cast `(uintptr_t)&x`
      if (c->args.empty())
        return {t->kind == TyKind::Float ? "0.0" : t->kind == TyKind::Pointer ? "null" : "0", t};
      unsupported(c, "this functional cast");
    }
  }
  else if (callee->kind == NodeKind::MemberExpr)
  {
    auto* m = static_cast<MemberExpr*>(callee);
    if (!m->member.empty() && m->member.last().special == 1)
    {
      // Explicit destructor call.
      std::string addr;
      const Ty* objectTy;
      if (m->op == P::Arrow)
      {
        Value p = arrowOperand(rvalue(m->base), c);
        if (p.ty->kind != TyKind::Pointer)
          unsupported(c, "-> on something that is not a pointer");
        addr = p.text;
        objectTy = p.ty->inner;
      }
      else
      {
        Value o = rvalue(m->base);
        addr = o.text;
        objectTy = o.ty;
      }
      if (isClass(objectTy))
        destroy(addr, objectTy, c);
      return {"", types.voidType()};
    }
    if (m->member.parts.size() == 1 && m->member.last().special == 2 && P(m->member.last().opToken) == P::LParen && isLambda(m->ty))
    {
      // `runTest.operator()<CraftingMachine>(name)`: a call of the lambda, its template arguments
      // on the call as the type checker recorded them.
      Value closure;
      if (m->op == P::Arrow)
      {
        Value p = arrowOperand(rvalue(m->base), c);
        if (p.ty->kind != TyKind::Pointer)
          unsupported(c, "-> on something that is not a pointer");
        closure = {p.text, p.ty->inner};
      }
      else
        closure = rvalue(m->base);
      return callClosure(closure, c->args, sret, c);
    }
    if ((!m->resolved || m->resolved->kind != NodeKind::FunctionDecl) && !m->member.empty() && m->member.last().special == 2 &&
        P(m->member.last().opToken) == P::Assign && c->args.size() == 1)
    {
      // `this->CollisionMask::operator=(other)` of a class declaring none: its implicit
      // assignment, member by member, of the base the name is qualified with; the object.
      std::string addr;
      const Ty* objectTy;
      if (m->op == P::Arrow)
      {
        Value p = arrowOperand(rvalue(m->base), c);
        if (p.ty->kind != TyKind::Pointer)
          unsupported(c, "-> on something that is not a pointer");
        addr = p.text;
        objectTy = p.ty->inner;
      }
      else
      {
        Value o = rvalue(m->base);
        addr = o.text;
        objectTy = o.ty;
      }
      objectTy = types.unqualified(types.stripRef(objectTy));
      if (!isClass(objectTy))
        unsupported(c, "operator= on a non-class object");
      const Ty* target = objectTy;
      if (const Ty* q = memberQualifier(m, c))
        target = q;
      return implicitAssignment(addr, objectTy, target, c->args[0], c);
    }
    if (m->resolved && m->resolved->kind == NodeKind::FunctionDecl)
    {
      auto* f = static_cast<FunctionDecl*>(m->resolved);
      std::string thisArg;
      const Ty* objectTy = nullptr;
      if (!(f->flags & DF_Static))
      {
        if (m->op == P::Arrow)
        {
          Value p = arrowOperand(rvalue(m->base), c);
          if (p.ty->kind != TyKind::Pointer)
            unsupported(c, "-> on something that is not a pointer");
          thisArg = p.text;
          objectTy = p.ty->inner;
        }
        else
        {
          Value o = rvalue(m->base);
          if (isClass(o.ty))
          {
            thisArg = o.text;
            objectTy = o.ty;
          }
          else if (o.ty && types.stripRef(o.ty)->kind == TyKind::Enum)
          {
            // A method of an enum: `this` is the value, passed by its address.
            const Ty* et = types.stripRef(o.ty);
            std::string slot = "%v" + std::to_string(temp++);
            allocas += "  " + slot + " = alloca " + llType(et, c) + "\n";
            fn += "  store " + llType(et, c) + " " + o.text + ", ptr " + slot + "\n";
            thisArg = slot;
            objectTy = et;
          }
          else
            unsupported(c, "a method call on a non-class object");
        }
        if (isClass(objectTy))
        {
          objectTy = canonicalClass(objectTy, c);
          Decl* owner = global.owningClass(f);
          if (owner && classDef(owner, c) != static_cast<ClassDecl*>(objectTy->decl))
          {
            // `r.Hook<Robot, TagB>::getId()`: the instance of the method's class template that
            // the qualifier is or derives from, not the first instance among the bases.
            const Ty* qualifier = memberQualifier(m, c);
            const Ty* base = qualifier ? instantiationIn(qualifier, owner, c) : nullptr;
            if (!base)
              base = baseInstantiation(objectTy, owner, c);
            std::string addr;
            // (`b.Right::get()` with Base under both Left and Right: Right's Base, through Right.)
            if (base && qualifier && qualifier != objectTy && qualifier != base && baseAddress(objectTy, qualifier, thisArg, addr, c))
            {
              thisArg = addr;
              objectTy = qualifier;
            }
            if (base && baseAddress(objectTy, base, thisArg, addr, c))
            {
              thisArg = addr;
              objectTy = base;
            }
          }
        }
        else
          objectTy = nullptr; // an enum: no class arguments, no bases
      }
      else if (m->base)
      {
        const Ty* bt = types.stripRef(m->base->ty);
        if (bt && bt->kind == TyKind::Pointer)
          bt = bt->inner;
        if (isClass(bt))
        {
          objectTy = canonicalClass(bt, c);
          // `object.staticType()` with staticType a static member of a base class template
          // (ComplexExpression<WithOperation<7, 2, 0>, 2, 0>): that base instance's member,
          // as for a call through the derived class's name.
          Decl* owner = global.owningClass(f);
          if (owner && classDef(owner, c) != static_cast<ClassDecl*>(objectTy->decl))
            if (const Ty* base = baseInstantiation(objectTy, owner, c))
              objectTy = base;
        }
      }
      std::string indirect;
      if (objectTy && !thisArg.empty() && m->member.parts.size() == 1)
        if (int slot = slotOf(objectTy, f, c); slot >= 0)
          indirect = loadSlot(thisArg, slot);
      return callFunction(f, calleeArgs(f, objectTy, c, c), objectTy, thisArg, sret, c->args, c, indirect);
    }
  }
  // `(obj.*pmf)(args)` / `(p->*pmf)(args)`: a call through a pointer to a member function, the
  // object first.
  {
    Expr* inner = callee;
    while (inner && inner->kind == NodeKind::ParenExpr)
      inner = static_cast<ParenExpr*>(inner)->inner;
    if (inner && inner->kind == NodeKind::BinaryExpr && !static_cast<BinaryExpr*>(inner)->isKeyword &&
        (P(static_cast<BinaryExpr*>(inner)->op) == P::DotStar || P(static_cast<BinaryExpr*>(inner)->op) == P::ArrowStar))
    {
      auto* b = static_cast<BinaryExpr*>(inner);
      const Ty* mp = b->rhs && b->rhs->ty ? types.stripRef(b->rhs->ty) : nullptr;
      if (mp && mp->kind == TyKind::MemberPointer && mp->inner && mp->inner->kind == TyKind::Function)
      {
        Value o = P(b->op) == P::ArrowStar ? arrowOperand(rvalue(b->lhs), c) : rvalue(b->lhs);
        std::string object = o.text;
        // The object as the pointer's class: an object of a class derived from it moves to that base.
        const Ty* objectTy = P(b->op) == P::ArrowStar ? (o.ty && o.ty->kind == TyKind::Pointer ? o.ty->inner : nullptr) : types.stripRef(o.ty);
        const Ty* memberOf = mp->args.empty() ? nullptr : withCurrentArgs(mp->args[0]);
        if (isClass(objectTy) && isClass(memberOf) && classKey(objectTy) != classKey(memberOf))
          baseAddress(objectTy, memberOf, o.text, object, c);
        Value fp = rvalue(b->rhs);
        return callIndirect(fp.text, mp->inner, object, c->args, sret, c);
      }
    }
  }
  // Call through a closure or a function pointer.
  Value fnValue = rvalue(callee);
  if (isLambda(fnValue.ty))
    return callClosure(fnValue, c->args, sret, c);
  if (isClass(fnValue.ty))
    return operatorCall(fnValue, "operator()", c->args, c, true, sret); // a functor: object(args)
  const Ty* ft = fnValue.ty->kind == TyKind::Pointer ? fnValue.ty->inner : fnValue.ty;
  if (!ft || ft->kind != TyKind::Function)
    unsupported(c, "a call that did not resolve to a function (the callee has type " + types.toString(fnValue.ty, &symbols) + ")");
  return callIndirect(fnValue.text, ft, "", c->args, sret, c);
}

auto codegen::Emitter::callIndirect(const std::string& fnText, const Ty* ft, const std::string& thisArg, Span<Expr*> callArgs, const std::string& sret, Node* c) -> Value
{
  if (ft->isCConvention())
  {
    if (!thisArg.empty())
      unsupported(c, "a call of C's calling convention through a pointer to a member function");
    return callIndirectC(fnText, ft, callArgs, sret, c);
  }
  Signature s;
  const Ty* ret = ft->inner;
  if (returnsInMemory(ret))
  {
    s.hasSret = true;
    s.returnType = "void";
  }
  else if (!ret || !ret->isKnown())
    s.returnType = "ptr";
  else
    s.returnType = ret->isReference() ? "ptr" : llType(ret, c);
  std::string args, resultAddr;
  if (!thisArg.empty())
  {
    args = "ptr " + thisArg;
    s.paramTypes.push_back("ptr");
  }
  if (s.hasSret)
  {
    resultAddr = sret.empty() ? newObjectSlot(ret, c) : sret;
    args += (args.empty() ? "" : ", ") + std::string("ptr ") + resultAddr;
    s.paramTypes.push_back("ptr");
  }
  size_t position = 0;
  auto addArgument = [&](Expr* arg)
  {
    const Ty* pt = position < ft->args.size() ? ft->args[position] : nullptr;
    std::string text = argumentText(pt, arg, c);
    args += (args.empty() ? "" : ", ") + text;
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
  for (const Ty* pt : ft->args)
    s.paramTypes.push_back(isAggregate(pt) || pt->isReference() ? "ptr" : llType(pt, c));
  s.variadic = ft->variadic;
  std::string sig = signatureText(s);
  if (s.returnType == "void")
  {
    emitCall("", sig + " " + fnText + "(" + args + ")");
    if (s.hasSret)
      return {resultAddr, ret, sret.empty()};
    return {"", types.voidType()};
  }
  std::string r = newTemp();
  emitCall(r + " = ", sig + " " + fnText + "(" + args + ")");
  return {r, ret, false};
}

auto codegen::Emitter::vaBuiltin(NameExpr* n, CallExpr* c) -> std::optional<Value>
{
  if (n->name.parts.size() != 1)
    return std::nullopt;
  std::string name = sym(n->name.parts[0].symbol);
  // SysV's va_list (the imported headers' __kpp_va_list, main.cpp's vaListForSysV) is an array of
  // one register save area, which the intrinsics and va_arg take the address of: a local's own,
  // a parameter's (decayed to a pointer, as C passes one on) the pointer it holds.
  auto sysvList = [&](Expr* e)
  {
    const Ty* lt = e->ty ? types.unqualified(types.stripRef(e->ty)) : nullptr;
    if (lt && lt->kind == TyKind::Pointer)
      return rvalue(e).text;
    return lvalue(e);
  };
  if (name == "va_arg" && c->args.size() == 2 && c->args[1]->kind == NodeKind::SizeofExpr && c->ty && !sh.target.windows())
  {
    // `va_arg(list, T)` on SysV x86-64, lowered as clang lowers it, not by LLVM's va_arg
    // instruction: at -O0 that read the va_list's pointer back from a stack slot nothing wrote
    // when the va_list was a parameter (spec language/variadic_with_own_format crashed on Linux
    // and FreeBSD). The va_list is { i32 gp_offset, i32 fp_offset, ptr overflow_arg_area,
    // ptr reg_save_area }: a value of the INTEGER class (an integer, a pointer, an enumeration)
    // comes from the six general registers' save area while gp_offset leaves room for its
    // eightbytes, a floating one from the eight vector registers' (16 bytes each, fp_offset 48 to
    // 176), any other from the overflow area, an argument there in 8-byte steps. (AArch64's
    // va_list, stage 7, is another.)
    const Ty* t = withCurrentArgs(types.unqualified(types.stripRef(c->ty)));
    if (isAggregate(t) || t->kind == TyKind::Vector)
      unsupported(c, "va_arg of a struct or vector on " + sh.target.triple);
    std::string list = sysvList(c->args[0]);
    uint64_t size = sizeOf(t, c), align = alignOf(t, c);
    bool sse = t->kind == TyKind::Float;
    uint64_t eightbytes = (size + 7) / 8;
    uint64_t limit = sse ? 176 - 16 : 48 - 8 * eightbytes, step = sse ? 16 : 8 * eightbytes;
    std::string offsetAddr = newTemp(), offset = newTemp(), fits = newTemp();
    std::string regL = newLabel("vaarg.reg"), memL = newLabel("vaarg.mem"), endL = newLabel("vaarg.end");
    fn += "  " + offsetAddr + " = getelementptr inbounds i8, ptr " + list + ", i64 " + (sse ? "4" : "0") + "\n";
    fn += "  " + offset + " = load i32, ptr " + offsetAddr + "\n";
    fn += "  " + fits + " = icmp ule i32 " + offset + ", " + std::to_string(limit) + "\n";
    fn += "  br i1 " + fits + ", label %" + regL + ", label %" + memL + "\n";
    startBlock(regL);
    std::string saveAddr = newTemp(), save = newTemp(), offset64 = newTemp(), inRegister = newTemp(), nextOffset = newTemp();
    fn += "  " + saveAddr + " = getelementptr inbounds i8, ptr " + list + ", i64 16\n";
    fn += "  " + save + " = load ptr, ptr " + saveAddr + "\n";
    fn += "  " + offset64 + " = zext i32 " + offset + " to i64\n";
    fn += "  " + inRegister + " = getelementptr inbounds i8, ptr " + save + ", i64 " + offset64 + "\n";
    fn += "  " + nextOffset + " = add i32 " + offset + ", " + std::to_string(step) + "\n";
    fn += "  store i32 " + nextOffset + ", ptr " + offsetAddr + "\n";
    std::string regEnd = currentBlock;
    emitBranch(endL);
    startBlock(memL);
    std::string areaAddr = newTemp(), area = newTemp(), next = newTemp();
    fn += "  " + areaAddr + " = getelementptr inbounds i8, ptr " + list + ", i64 8\n";
    fn += "  " + area + " = load ptr, ptr " + areaAddr + "\n";
    std::string inMemory = area;
    if (align > 8)
    {
      // (one aligned to more than 8, an __int128, starts at its alignment there)
      std::string bits = newTemp(), up = newTemp(), aligned = newTemp();
      inMemory = newTemp();
      fn += "  " + bits + " = ptrtoint ptr " + area + " to i64\n";
      fn += "  " + up + " = add i64 " + bits + ", " + std::to_string(align - 1) + "\n";
      fn += "  " + aligned + " = and i64 " + up + ", " + std::to_string(-int64_t(align)) + "\n";
      fn += "  " + inMemory + " = inttoptr i64 " + aligned + " to ptr\n";
    }
    fn += "  " + next + " = getelementptr inbounds i8, ptr " + inMemory + ", i64 " + std::to_string(8 * eightbytes) + "\n";
    fn += "  store ptr " + next + ", ptr " + areaAddr + "\n";
    std::string memEnd = currentBlock;
    emitBranch(endL);
    startBlock(endL);
    std::string address = newTemp();
    fn += "  " + address + " = phi ptr [ " + inRegister + ", %" + regEnd + " ], [ " + inMemory + ", %" + memEnd + " ]\n";
    return load(address, t, c);
  }
  if (name == "va_arg" && c->args.size() == 2 && c->args[1]->kind == NodeKind::SizeofExpr && c->ty)
  {
    // `va_arg(list, T)` (Parser::parseVaArg): x64 Windows passes each variadic argument in an
    // 8-byte slot, one larger than 8 bytes or of another size than 1, 2, 4, 8 by its address.
    const Ty* t = withCurrentArgs(types.unqualified(types.stripRef(c->ty)));
    std::string list = lvalue(c->args[0]);
    std::string slot = newTemp(), next = newTemp();
    fn += "  " + slot + " = load ptr, ptr " + list + "\n";
    fn += "  " + next + " = getelementptr inbounds i8, ptr " + slot + ", i64 8\n";
    fn += "  store ptr " + next + ", ptr " + list + "\n";
    uint64_t size = sizeOf(t, c);
    if (size > 8 || (size != 1 && size != 2 && size != 4 && size != 8))
    {
      std::string address = newTemp();
      fn += "  " + address + " = load ptr, ptr " + slot + "\n";
      return load(address, t, c);
    }
    return load(slot, t, c);
  }
  if (name != "va_start" && name != "va_end" && name != "va_copy")
    return std::nullopt;
  if (c->args.size() < (name == "va_start" ? 2u : name == "va_copy" ? 2u : 1u))
    unsupported(c, name + " with too few arguments");
  std::string list = sh.target.windows() ? lvalue(c->args[0]) : sysvList(c->args[0]);
  std::string intrinsic = name == "va_start" ? "@llvm.va_start.p0" : name == "va_end" ? "@llvm.va_end.p0" : "@llvm.va_copy.p0";
  if (declaredNames.claim(intrinsic))
    decls += "declare void " + intrinsic + (name == "va_copy" ? "(ptr, ptr)\n" : "(ptr)\n");
  if (name == "va_copy")
    fn += "  call void " + intrinsic + "(ptr " + list + ", ptr " + (sh.target.windows() ? lvalue(c->args[1]) : sysvList(c->args[1])) + ")\n";
  else
    fn += "  call void " + intrinsic + "(ptr " + list + ")\n";
  return Value{"", types.voidType(), false};
}
