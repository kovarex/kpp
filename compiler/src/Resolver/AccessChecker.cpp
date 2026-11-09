// GlobalScope::Impl, access checking (design.md, "struct and class") (its declarations in AccessChecker.hpp, part of GlobalScopeImpl.hpp): a translation unit
// of its own since 2026-11-05 (resolver.cpp split, stage 2), moved without a change.
#include "GlobalScopeImpl.hpp"

auto GlobalScope::Impl::accessContext(Decl* d, std::vector<Scope*>& out) -> void
{
  for (int depth = 0; d && depth < 64; ++depth)
  {
    if (d->kind == NodeKind::ClassDecl && d->ownScope)
    {
      // With the classes its scope is nested in (`struct Button::Helper {...}` defined outside
      // Button, whose declaration the index moved into it).
      for (Scope* s = d->ownScope; s && s->kind == ScopeKind::Class; s = s->parent)
        out.push_back(s);
    }
    Decl* owner = nullptr;
    if (d->kind == NodeKind::FunctionDecl)
      owner = owningClassOf(d);
    else if (d->kind == NodeKind::VariableDecl || d->kind == NodeKind::ClassDecl)
    {
      owner = d->kind == NodeKind::VariableDecl ? owningClassOf(d) : nullptr;
      const QualifiedName& name = d->kind == NodeKind::VariableDecl ? static_cast<VariableDecl*>(d)->name : static_cast<ClassDecl*>(d)->name;
      if (!owner && name.parts.size() > 1)
      {
        // `int Cache::limit = Cache::defaultLimit();`: a static data member's definition is the
        // class's; `struct Button::Helper {...}` is nested in Button where it is defined.
        QualifiedName qualifier = name;
        qualifier.parts.count -= 1;
        Lookup l = lookupQualified(qualifier, lexicalScopeOf(d), nullptr, 0);
        if (l.scope && l.scope->kind == ScopeKind::Class)
          owner = l.scope->owner;
      }
    }
    d = owner ? owner : d->parent;
  }
}

auto GlobalScope::Impl::sameNamePart(const NamePart& a, const NamePart& b) -> bool
{
  return a.symbol == b.symbol && a.special == b.special && (a.special != 2 || a.opToken == b.opToken);
}

auto GlobalScope::Impl::grantingFriend(Decl* owner, FunctionDecl* fn, const std::vector<Scope*>& context) -> Decl*
{
  for (Decl* m : membersRead(static_cast<ClassDecl*>(owner)))
  {
    if (!m || !(m->flags & DF_Friend))
      continue;
    if (m->kind == NodeKind::ClassDecl)
    {
      const QualifiedName& n = static_cast<ClassDecl*>(m)->name;
      if (!n.empty())
        for (Scope* s : context)
          if (s->owner && s->owner->kind == NodeKind::ClassDecl && !static_cast<ClassDecl*>(s->owner)->name.empty() &&
              static_cast<ClassDecl*>(s->owner)->name.last().symbol == n.last().symbol)
            return m;
    }
    else if (m->kind == NodeKind::FunctionDecl && fn && !fn->name.empty())
    {
      const QualifiedName& n = static_cast<FunctionDecl*>(m)->name;
      if (n.empty() || !sameNamePart(n.last(), fn->name.last()))
        continue;
      // (`friend Data LightningUtil::getData(...)`: a qualifier that is no class names a namespace.)
      Decl* cls = owningClassOf(fn);
      if (n.parts.size() < 2 || !cls
            ? !cls
            : cls->kind == NodeKind::ClassDecl && !static_cast<ClassDecl*>(cls)->name.empty() &&
                static_cast<ClassDecl*>(cls)->name.last().symbol == n.parts[n.parts.size() - 2].symbol)
        return m;
    }
  }
  return nullptr;
}

auto GlobalScope::Impl::namePartText(const NamePart& p) const -> std::string
{
  if (p.special == 1)
    return "~" + debugName(p.symbol);
  if (p.special == 2)
    return "operator";
  if (p.special == 3)
    return "a conversion operator";
  return debugName(p.symbol);
}

auto GlobalScope::Impl::accessName(Decl* d) const -> std::string
{
  std::string name;
  switch (d->kind)
  {
    case NodeKind::ClassDecl: name = static_cast<ClassDecl*>(d)->name.empty() ? "(anonymous)" : namePartText(static_cast<ClassDecl*>(d)->name.last()); break;
    case NodeKind::FunctionDecl: name = static_cast<FunctionDecl*>(d)->name.empty() ? "?" : namePartText(static_cast<FunctionDecl*>(d)->name.last()); break;
    case NodeKind::VariableDecl: name = static_cast<VariableDecl*>(d)->name.empty() ? "?" : namePartText(static_cast<VariableDecl*>(d)->name.last()); break;
    case NodeKind::EnumDecl: name = static_cast<EnumDecl*>(d)->name.empty() ? "(anonymous enum)" : namePartText(static_cast<EnumDecl*>(d)->name.last()); break;
    case NodeKind::UsingAliasDecl: name = debugName(static_cast<UsingAliasDecl*>(d)->name); break;
    case NodeKind::EnumeratorDecl: name = debugName(static_cast<EnumeratorDecl*>(d)->name); break;
    default: name = "?"; break;
  }
  for (Decl* p = d->parent; p && p->kind == NodeKind::ClassDecl; p = p->parent)
    name = (static_cast<ClassDecl*>(p)->name.empty() ? std::string("(anonymous)") : namePartText(static_cast<ClassDecl*>(p)->name.last())) + "::" + name;
  return name;
}

auto GlobalScope::Impl::accessGroup(const ParsedFile* file) -> const char*
{
  if (!file)
    return "game";
  if (file->isLibrary)
    return "stdlib";
  std::string path = file->path;
  std::transform(path.begin(), path.end(), path.begin(), [](char c) { return c == '/' ? '\\' : char(std::tolower(uint8_t(c))); });
  return path.find("\\tests\\") != std::string::npos || path.find("\\unittest++\\") != std::string::npos ? "tests" : "game";
}

auto GlobalScope::Impl::accessOf(Decl* used, FunctionDecl* fn, const std::vector<Scope*>& context, Scope* naming, const Ty* self) -> AccessDecision
{
  AccessDecision a;
  Decl* member = used;
  if (member->kind == NodeKind::EnumeratorDecl && member->parent && member->parent->kind == NodeKind::EnumDecl)
    member = member->parent; // an enumerator is as accessible as its enum
  if ((member->access != Access::Private && member->access != Access::Protected) || (member->flags & DF_Friend))
    return a;
  Decl* owner = owningClassOf(member);
  if (!owner || owner->kind != NodeKind::ClassDecl || !owner->ownScope || static_cast<ClassDecl*>(owner)->isClosure)
    return a;
  if (member != used && !static_cast<EnumDecl*>(member)->name.empty())
    for (Decl* m : membersRead(static_cast<ClassDecl*>(owner)))
      if (m && m->kind == NodeKind::UsingNameDecl && static_cast<UsingNameDecl*>(m)->isEnum && !static_cast<UsingNameDecl*>(m)->name.empty() &&
          static_cast<UsingNameDecl*>(m)->name.last().symbol == static_cast<EnumDecl*>(member)->name.last().symbol && m->access == Access::Public)
        return a; // `enum class Enum {...}; public: using enum Enum;`: the enumerators are public members
  for (Scope* s : context)
    if (s == owner->ownScope)
      return a;
  bool isProtected = member->access == Access::Protected;
  if (isProtected)
    for (Scope* s : context)
    {
      if (isBaseScope(owner->ownScope, s))
        return a;
      // A base reached through a class template's instance, which the scopes do not link
      // (`class ConditionDragPane : public DragPaneWidget<...>` naming Widget's members): as types.
      if (s->owner && s->owner->kind == NodeKind::ClassDecl && s->owner->templateParams.empty() && owner->templateParams.empty() &&
          isBaseClass(types.classType(owner, {}), types.classType(s->owner, {})))
        return a;
    }
  // In an instance of a class template whose base its arguments choose (`class DragPaneWidget :
  // public WidgetType` naming agui::Layout's members as an instance with WidgetType a Table).
  if (isProtected && self && !types.isDependent(self) && isBaseClass(types.classType(owner, {}), self))
    return a;
  a.member = member;
  a.owner = owner;
  a.isProtected = isProtected;
  a.grantedBy = grantingFriend(owner, fn, context);
  if (!a.grantedBy && isProtected && naming && naming != owner->ownScope && naming->owner && naming->owner->kind == NodeKind::ClassDecl)
    a.grantedBy = grantingFriend(naming->owner, fn, context);
  a.allowed = a.grantedBy != nullptr;
  return a;
}

auto GlobalScope::Impl::checkAccessUse(Decl* used, Node* at, Decl* d, FunctionDecl* fn, const std::vector<Scope*>& context, Scope* naming, const Ty* self) -> std::string
{
  AccessDecision a = accessOf(used, fn, context, naming, self);
  if (!a.member)
    return {}; // (allowed without a friend)
  Decl* member = a.member;
  bool isProtected = a.isProtected;
  std::string where = locationOf(d->file, at->token);
  std::string name = accessName(member);
  Decl* f = a.grantedBy;
  // The code's name: a function with its class, also when defined outside the class.
  std::string in = fn ? accessName(fn) : accessName(d);
  if (Decl* cls = fn ? owningClassOf(fn) : nullptr; cls && fn->parent != cls && !fn->name.empty())
    in = accessName(cls) + "::" + namePartText(fn->name.last());
  else if (fn && !cls)
    in = "the function " + in;
  std::string use = name + " is " + (isProtected ? "protected" : "private") + ", named in " + in;
  if (f)
  {
    std::lock_guard lock(accessMutex);
    accessFriendUses.insert(where + ": note: " + use + ", which a friend " + (f->kind == NodeKind::ClassDecl ? "class" : "function") +
                            " declaration allows");
    accessFriendsUsed.insert(f);
    return {};
  }
  if (accessErrors)
    return use;
  const char* kind = member->kind == NodeKind::VariableDecl     ? ((member->flags & DF_Static) ? "static data member" : "data member")
                     : member->kind == NodeKind::FunctionDecl   ? "function"
                     : member->kind == NodeKind::EnumDecl && used != member ? "enumerator"
                                                                : "type";
  std::string message = where + ": warning: " + use;
  std::lock_guard lock(accessMutex);
  accessViolations.emplace(std::move(message), AccessViolation{accessGroup(d->file), isProtected, kind});
  return use;
}

auto GlobalScope::Impl::arrowObject(const Ty* t, int depth) -> const Ty*
{
  t = t ? types.unqualified(types.stripRef(t)) : nullptr;
  if (!t || depth > 4)
    return nullptr;
  if (t->kind == TyKind::Pointer)
    return t->inner;
  Scope* cls = t->kind == TyKind::Class && !types.isDependent(t) ? classScopeOf(t) : nullptr;
  Lookup arrow;
  if (!cls || !lookupInScope(cls, opSymbols[uint32_t(P::Arrow)], arrow, 0) || !arrow.decl || arrow.decl->kind != NodeKind::FunctionDecl)
    return nullptr;
  Subst subst = substFor(t);
  const Ty* ft = declaredTypeOf(arrow.decl, subst.empty() ? nullptr : &subst, 0);
  if (!ft || ft->kind != TyKind::Function || !ft->inner || !ft->inner->isKnown() || containsAuto(ft->inner))
    return nullptr;
  return arrowObject(ft->inner, depth + 1);
}

auto GlobalScope::Impl::usedAt(Node* n, Scope*& naming) -> Decl*
{
  naming = nullptr;
  if (n->kind == NodeKind::NameExpr)
    return static_cast<NameExpr*>(n)->resolved;
  if (n->kind == NodeKind::NamedType)
    return static_cast<NamedType*>(n)->resolved;
  if (n->kind != NodeKind::MemberExpr)
    return nullptr;
  auto* m = static_cast<MemberExpr*>(n);
  if (const Ty* object = m->resolved && m->base && m->base->ty && m->base->ty->isKnown() ? types.stripRef(m->base->ty) : nullptr)
  {
    if (m->op == P::Arrow)
      object = arrowObject(object);
    object = object ? types.unqualified(types.stripRef(object)) : nullptr;
    if (object && object->kind == TyKind::Class && !types.isDependent(object))
      naming = classScopeOf(object);
  }
  return m->resolved;
}

auto GlobalScope::Impl::namesOnlyAccessible(Node* n, FunctionDecl* fn, const std::vector<Scope*>& context, const Ty* self) -> bool
{
  bool accessible = true;
  forEachNode(n, [&](Node* node)
  {
    Scope* naming;
    if (Decl* used = accessible ? usedAt(node, naming) : nullptr)
      accessible = accessOf(used, fn, context, naming, self).allowed;
  });
  return accessible;
}

auto GlobalScope::Impl::checkAccessIn(Decl* d, const Ty* self) -> void
{
  FunctionDecl* fn = d->kind == NodeKind::FunctionDecl ? static_cast<FunctionDecl*>(d) : nullptr;
  std::vector<Scope*> context;
  accessContext(d, context);
  std::unordered_set<const Node*> local; // the nodes of local classes: their members are checked as theirs
  forEachNode(d, [&](Node* n)
  {
    if (local.count(n))
      return;
    if (n != d && n->kind == NodeKind::ClassDecl && !static_cast<ClassDecl*>(n)->isClosure)
    {
      forEachNode(n, [&](Node* inner) { local.insert(inner); });
      return;
    }
    Scope* naming;
    if (Decl* used = usedAt(n, naming))
      if (std::string denied = checkAccessUse(used, n, d, fn, context, naming, self); !denied.empty() && accessErrors)
        reportCheckerError(d->file, n->token, denied); // (the walk's unit's: a walk again takes it back)
  });
}

auto GlobalScope::Impl::declaresVirtual(ClassDecl* c, bool record) -> bool
{
  for (Decl* m : record ? membersRead(c) : c->members)
    if (m && m->kind == NodeKind::FunctionDecl && (m->flags & (DF_Virtual | DF_Override | DF_Final | DF_Pure)))
      return true;
  return false;
}

auto GlobalScope::Impl::polymorphicBaseOf(ClassDecl* c, int depth, bool record) -> ClassDecl*
{
  if (depth > 16)
    return nullptr;
  for (const ClassDecl::Base& b : c->bases)
  {
    Type* named = b.type && b.type->kind == NodeKind::PackType ? static_cast<PackType*>(b.type)->pattern : b.type; // (`Leaf<A>...`: Leaf)
    Decl* d = named && named->kind == NodeKind::NamedType ? static_cast<NamedType*>(named)->resolved : nullptr;
    if (!d || d->kind != NodeKind::ClassDecl)
      continue;
    if (Scope* s = scopeOfEntry(Entry{d, nullptr}, 0); s && s->owner && s->owner->kind == NodeKind::ClassDecl)
      d = s->owner;
    if (declaresVirtual(static_cast<ClassDecl*>(d), record) || polymorphicBaseOf(static_cast<ClassDecl*>(d), depth + 1, record))
      return static_cast<ClassDecl*>(d);
  }
  return nullptr;
}

auto GlobalScope::Impl::structRule(Span<Decl*> decls, std::vector<std::string>& out, StructCounts& counts) -> void
{
  for (Decl* d : decls)
  {
    if (!d)
      continue;
    if (d->kind == NodeKind::NamespaceDecl)
    {
      structRule(static_cast<NamespaceDecl*>(d)->decls, out, counts);
      continue;
    }
    if (d->kind != NodeKind::ClassDecl)
      continue;
    auto* c = static_cast<ClassDecl*>(d);
    if (!c->isDefinition || c->isClosure || (c->flags & DF_Friend) || (c->file && c->file->isImported) || c->keyword == Kw::union_)
      continue;
    for (Decl* m : membersRead(c))
      if (m && (m->flags & DF_Friend))
      {
        ++counts.friends;
        // The friend, for comparing the warnings of a conversion with the C++ it read.
        std::string named = "?";
        if (m->kind == NodeKind::ClassDecl && !static_cast<ClassDecl*>(m)->name.empty())
          named = "class " + namePartText(static_cast<ClassDecl*>(m)->name.last());
        else if (m->kind == NodeKind::FunctionDecl && !static_cast<FunctionDecl*>(m)->name.empty())
        {
          const QualifiedName& n = static_cast<FunctionDecl*>(m)->name;
          named = "function ";
          for (size_t i = 0; i < n.parts.size(); ++i)
            named += (i ? "::" : "") + namePartText(n.parts[i]);
        }
        out.push_back(locationOf(m->file, m->token) + ": note: " + accessName(c) + " has the friend " + named);
      }
    std::array<size_t, 6>& n = counts.byGroup[accessGroup(c->file)];
    if (c->keyword == Kw::class_)
      ++n[1];
    else
    {
      ++n[0];
      bool privateMember = false, privateBase = false;
      for (Decl* m : membersRead(c))
        privateMember = privateMember || (m && !(m->flags & DF_Friend) && (m->access == Access::Private || m->access == Access::Protected));
      for (const ClassDecl::Base& b : c->bases)
        privateBase = privateBase || b.access == Access::Private || b.access == Access::Protected;
      bool isVirtual = declaresVirtual(c);
      bool byBase = !isVirtual && polymorphicBase(c, 0);
      n[2] += privateMember;
      n[3] += privateBase;
      n[4] += isVirtual;
      n[5] += byBase;
      std::vector<const char*> why;
      if (privateMember)
        why.push_back("a private or protected member");
      if (privateBase)
        why.push_back("a base that is not public");
      if (isVirtual)
        why.push_back("a virtual function");
      if (!why.empty())
      {
        std::string text = why[0];
        for (size_t i = 1; i < why.size(); ++i)
          text += std::string(i + 1 == why.size() ? " and " : ", ") + why[i];
        out.push_back(locationOf(c->file, c->token) + ": warning: struct " + accessName(c) + " has " + text + " (a struct is a public value type: make it a class)");
      }
    }
    structRule(c->members, out, counts);
  }
}

auto GlobalScope::Impl::checkStaticAssert(StaticAssertDecl* s, const TypeContext& ctx, const std::string& instance, const FunctionDecl* inFunction) -> void
{
  if (!s->cond)
    return;
  TokenScope tokenScope(ctx.tokens, ctx.text);
  // An instance whose arguments the checker does not know (`EnumType = ?`) decides nothing.
  bool knownInstance = true;
  if (ctx.subst)
    for (const Subst::Pair& p : ctx.subst->pairs)
      knownInstance = knownInstance && p.type && !mentionsUnknown(p.type);
  bool outerExact = exactLayoutOnly, outerNeeded = layoutNeeded; // (an assert met while another is evaluated)
  exactLayoutOnly = true;
  layoutNeeded = false;
  constWhy().clear();
  std::optional<bool> v = knownInstance ? constCondition(s->cond, ctx, 0) : std::nullopt;
  std::string why = v ? std::string() : constWhy();
  bool needed = layoutNeeded;
  exactLayoutOnly = outerExact;
  layoutNeeded = outerNeeded;
  if (!v && needed)
  {
    std::lock_guard lock(checkerErrorsMutex);
    LayoutAssert& a = layoutAsserts.emplace_back();
    a.decl = s;
    if (ctx.subst)
      a.subst = *ctx.subst;
    a.instance = instance;
    a.inFunction = inFunction;
    a.unit = reportingUnit;
    return;
  }
  if (v && *v)
    return;
  // A condition that is no constant in C++ (a signed overflow, a division by zero) says why; one
  // the checker cannot evaluate yet is its gap.
  std::string message = v               ? "static assertion failed"
                      : !why.empty()    ? "static assertion whose condition is no constant: " + why
                                        : "static assertion whose condition the compiler cannot evaluate yet";
  if (s->message && s->message->kind == NodeKind::LiteralExpr && s->file && s->file->tokens && !s->file->tokens->empty())
  {
    const Token& t = (*s->file->tokens)[std::min<size_t>(s->message->token, s->file->tokens->size() - 1)];
    std::string spliced;
    message += ": " + std::string(tokenSpelling(s->file->text, t, spliced));
  }
  // A failure names its instance; a condition the checker cannot evaluate is its gap at that
  // place, one error however many instances meet it.
  if (v && !instance.empty())
    message += " (in " + instance + ")";
  if (inFunction)
  {
    std::lock_guard lock(checkerErrorsMutex);
    instanceAssertErrors[inFunction].push_back(CheckerError{s->file, s->token, message});
    return;
  }
  reportCheckerError(s->file, s->token, message);
}

auto GlobalScope::Impl::checkConstexprVariable(VariableDecl* v, const Ty* t, const TypeContext& ctx, const FunctionDecl* inFunction, const ParsedFile* in) -> void
{
  const ParsedFile* where = v->file ? v->file : in;
  if (!t || !t->isKnown() || types.isDependent(t) || mentionsUnknown(t) || containsAuto(t) || v->name.empty())
    return;
  if (ctx.subst)
    for (const Subst::Pair& p : ctx.subst->pairs)
      if (!p.type || mentionsUnknown(p.type))
        return; // an instance whose arguments the checker does not know decides nothing
  TokenScope tokenScope(ctx.tokens, ctx.text);
  bool outerExact = exactLayoutOnly, outerNeeded = layoutNeeded;
  exactLayoutOnly = true;
  layoutNeeded = false;
  constWhy().clear();
  std::optional<ConstValue> value = constInitialize(t, v->init, ctx, 0);
  std::string why = value ? std::string() : constWhy();
  bool needed = layoutNeeded;
  exactLayoutOnly = outerExact;
  layoutNeeded = outerNeeded;
  if (value || needed)
    return;
  std::string message = "the constexpr variable " + std::string(symbols.name(v->name.last().symbol)) + " is no constant";
  if (!why.empty())
    message += ": " + why;
  if (inFunction)
  {
    std::lock_guard lock(checkerErrorsMutex);
    instanceAssertErrors[inFunction].push_back(CheckerError{where, v->token, message});
    return;
  }
  reportCheckerError(where, v->token, message);
}

auto GlobalScope::Impl::constOfDecl(Decl* d, const TypeContext& ctx, int depth, const Subst* classSubst) -> std::optional<int64_t>
{
  std::optional<ConstValue> v = constValueOfDecl(d, ctx, depth, classSubst);
  if (!v || v->isFloat())
    return std::nullopt;
  return v->i;
}

auto GlobalScope::Impl::constDeducedType(VariableDecl* v, const Ty* written) -> const Ty*
{
  const Ty* cached = v->cachedType.load(std::memory_order_acquire);
  auto concrete = [&](const Ty* t) { return t && t->isKnown() && !types.isDependent(t) && !mentionsUnknown(t); };
  // `auto b = a;` of a class: the class the walk deduced (else the initializer's), so that b is made
  // as `T b = a;` makes it, by T's copy constructor; as `auto` it was a copy of a's value with the
  // constructor skipped (a counting copy constructor counted 0 at compile time, 1 at run time, D
  // 2026-11-06). Not a brace list's (`auto x = {1, 2}`), a closure's or a reference's.
  if (written && !written->isReference() && containsAuto(written) && v->init && v->init->kind != NodeKind::BraceInitExpr)
  {
    const Ty* deduced = concrete(cached) ? cached : concrete(v->init->ty) ? types.qualified(types.unqualified(types.stripRef(v->init->ty)), written->isConst, written->isVolatile) : nullptr;
    if (deduced && !deduced->isReference() && types.unqualified(deduced)->kind == TyKind::Class && !types.unqualified(deduced)->isBraceList())
      return deduced;
  }
  if (written && written->isKnown() && !isUndeducedClassTemplate(types.unqualified(types.stripRef(written))))
    return written;
  return concrete(cached) ? cached : written;
}

auto GlobalScope::Impl::constValueOfDecl(Decl* d, const TypeContext& ctx, int depth, const Subst* classSubst) -> std::optional<ConstValue>
{
  if (!d || depth > 96)
    return std::nullopt;
  if (d->kind == NodeKind::TemplateParamDecl)
  {
    if (ctx.subst)
      if (const Ty* t = elementOf(d, ctx.subst->find(d), ctx); t && t->kind == TyKind::Value)
      {
        if (t->isObjectValue())
          return argumentObject(t); // `template<Fixed F>`: the object given, by its parts
        if (!t->decl && t->inner && types.unqualified(t->inner)->kind == TyKind::Float)
          return ConstValue::floating(TypeTable::floatOfBits(t->value, types.unqualified(t->inner)->bits), t->inner); // (`template<double D>`: its bits)
        if (!t->decl)
          return ConstValue::integer(t->value, t->inner);
        // `Expr<add, 3>`'s fold: the address of the function (or variable) given, never the null
        // pointer its value field would read as; one of a class template's members, whose
        // instance the argument does not record, is no constant.
        if (t->decl->parent && t->decl->parent->kind == NodeKind::ClassDecl && !effectiveParams(t->decl->parent).empty())
          return std::nullopt;
        ConstTarget target;
        target.decl = t->decl;
        const Ty* pt = t->inner ? types.unqualified(t->inner) : nullptr;
        if (pt && pt->kind == TyKind::MemberPointer && t->decl->kind == NodeKind::VariableDecl && !pt->args.empty())
        {
          // `template<int Pos::*Field>` given &Pos::x: the member pointer, as `&Pos::x` makes it.
          target.owner = pt->args[0];
          return ConstValue::address(pt, std::move(target), ConstValue::Kind::MemberPointer);
        }
        if (!pt || pt->kind != TyKind::Pointer)
          return std::nullopt;
        return ConstValue::address(pt, std::move(target));
      }
    return std::nullopt;
  }
  if (d->kind == NodeKind::VariableDecl)
  {
    auto* v = static_cast<VariableDecl*>(d);
    if (v->builtin == BUILTIN_EnumCount && v->parent && v->parent->kind == NodeKind::EnumDecl)
      return ConstValue::integer(int64_t(enumeratorCount(v->parent)), types.intType()); // a kpp enum's count
    if (v->parent && v->parent->kind == NodeKind::ClassDecl && !(v->flags & DF_Static))
      return std::nullopt; // a data member: its value is an object's, not its initializer
    // A member of a class template: under the instantiation's arguments, the ones it was named
    // through or the context's when they bind the class's parameters (`count` read in another
    // member's initializer).
    bool member = v->parent && v->parent->kind == NodeKind::ClassDecl;
    Span<Decl*> classParams = member ? effectiveParams(v->parent) : Span<Decl*>{};
    const Subst* memberArgs = classSubst;
    if (!memberArgs && ctx.subst)
      for (Decl* p : classParams)
        if (ctx.subst->find(p))
        {
          memberArgs = ctx.subst;
          break;
        }
    // `static const int LIMIT;` in the class, `const int Gui::LIMIT = 21;` outside; of an instance
    // with an explicit specialisation of the member, that one.
    VariableDecl* def = v->init ? v : variableDefinitionFor(v, member ? memberArgs : nullptr);
    if (!v->type)
      return std::nullopt;
    TypeContext vctx{lexicalScopeOf(v), nullptr, nullptr, v->file ? v->file->tokens : nullptr, v->file ? v->file->text : std::string_view()};
    const Ty* t = convertType(v->type, vctx, depth + 1);
    if (!(v->flags & DF_Constexpr) && !(t && t->isConst))
      return std::nullopt;
    if (!def->init)
    {
      // `const InPlaceSecond inPlaceSecond;` (the library's tag, which Map's and HashMap's operator[] pass): a const
      // object of a class defined at namespace scope without an initializer is zero-initialized and
      // default-initialized, as the program's static storage is: its members from their default member
      // initializers, its default constructor run (constEmpty). Not one declared extern or by a C import, whose
      // definition is elsewhere, a class's static member or a function's local.
      const Ty* u = t ? types.unqualified(t) : nullptr;
      if (!u || u->kind != TyKind::Class || member || (v->flags & (DF_Extern | DF_ExternC)) || (v->file && v->file->isImported) ||
          (v->parent && v->parent->kind == NodeKind::FunctionDecl))
        return std::nullopt;
      return constEmpty(u, true, depth + 1);
    }
    // The definition's initializer, read from its file, evaluated where the declaration is (the class).
    // A constant local to a function body (`constexpr bool sign = isSigned<T>;`): its initializer
    // under the context it is used in, which has the instantiation's arguments.
    if (v->parent && v->parent->kind == NodeKind::FunctionDecl && v->name.parts.size() == 1)
      return constInitialize(constDeducedType(v, convertType(v->type, ctx, depth + 1)), def->init, ctx, depth + 1);
    LocalStack params;
    for (Decl* p : classParams)
      params.push(static_cast<TemplateParamDecl*>(p)->name, p);
    TypeContext ictx{lexicalScopeOf(v), classParams.empty() ? nullptr : &params, memberArgs, def->file ? def->file->tokens : nullptr,
                     def->file ? def->file->text : std::string_view()};
    Subst ofClass; // (the record's key: the class's arguments alone)
    if (memberArgs)
      for (Decl* p : classParams)
        if (const Ty* t = memberArgs->find(p))
          ofClass.pairs.push_back({p, t});
    std::pair<Decl*, std::vector<const Ty*>> key{def, {}};
    for (const Subst::Pair& p : ofClass.pairs)
      key.second.push_back(p.type);
    bool evaluating = constEvaluating() > 0; // (outside an evaluation nothing is kept)
    if (evaluating)
      if (auto known = constGlobalValues().find(key); known != constGlobalValues().end())
        return known->second;
    std::optional<ConstValue> value = constInitialize(constDeducedType(v, convertType(v->type, ictx, depth + 1)),
                                                      constInitializerOf(def, ofClass.empty() ? nullptr : &ofClass), ictx, depth + 1);
    if (evaluating && value)
      constGlobalValues().emplace(std::move(key), *value);
    return value;
  }
  if (d->kind == NodeKind::EnumeratorDecl)
  {
    // Enumerators count up from zero, or from the last written value, converted to the
    // enumeration's underlying type when it was declared with one.
    auto* e = static_cast<EnumDecl*>(d->parent);
    if (!e || e->kind != NodeKind::EnumDecl)
      return std::nullopt;
    // In the enum's own scope: `GENERIC = NOTHING` names an earlier enumerator of an enum class.
    // Of the instance it is named through (`Flag<A>::Enum`, `Tm<3>::value`, its value written from
    // the template's parameters: `enum { value = N * 2 };`); the enum alone where none is bound.
    const Subst* instance = classSubst ? classSubst : ctx.subst;
    const Ty* enumType = enumTypeIn(e, instance);
    if (types.isDependent(enumType))
    {
      enumType = types.enumType(e);
      instance = nullptr;
    }
    LocalStack enumParams;
    for (Decl* p : effectiveParams(e))
      if (p->kind == NodeKind::TemplateParamDecl)
        enumParams.push(static_cast<TemplateParamDecl*>(p)->name, p);
    if (instance && effectiveParams(e).empty())
      for (const Subst::Pair& p : instance->pairs) // (in a function's instance: `enum { a = sizeof(T), b = a + 1 };`)
        if (p.param->kind == NodeKind::TemplateParamDecl)
          enumParams.push(static_cast<TemplateParamDecl*>(p.param)->name, p.param);
    TypeContext ectx{e->ownScope ? e->ownScope : lexicalScopeOf(e), &enumParams, instance, e->file ? e->file->tokens : nullptr,
                     e->file ? e->file->text : std::string_view()};
    // An enumeration with no instance's arguments to depend on: all its enumerators' values, kept
    // once computed (keptEnumerators), where each one asked was computed again from the first.
    const bool plain = !instance || instance->pairs.empty();
    if (plain)
      if (std::shared_ptr<const EnumeratorTable> kept = keptEnumerators(enumType))
        if (auto it = kept->find(d); it != kept->end())
          return it->second;
    // An enumerator written from the one before (`F1 = 2 * F0`) asks for it while this loop
    // computes it: the values found so far answer, so that a chain is evaluated once, not again
    // for each link (2^n steps).
    for (const EnumeratorValues* f = enumeratorValues; f; f = f->outer)
      if (f->e == e && f->type == enumType)
        for (const auto& [at, value] : f->values)
          if (at == d)
            return value;
    EnumeratorValues found{e, enumType, {}, enumeratorValues};
    enumeratorValues = &found;
    struct Pop
    {
      ~Pop() { enumeratorValues = f.outer; }

      EnumeratorValues& f;
    } pop{found};
    ConstValue next = ConstValue::integer(0, enumType);
    // (a plain enumeration's loop goes on past d to its last enumerator, and keeps them all; one after
    // d that does not evaluate leaves d's value as before, its reason as it was at d, nothing kept)
    std::optional<ConstValue> asked;
    std::string whyAtAsked;
    for (Decl* m : e->enumerators)
    {
      if (m->kind != NodeKind::EnumeratorDecl)
        continue;
      auto* cur = static_cast<EnumeratorDecl*>(m);
      if (cur->value)
      {
        // (its value as a walk of it typed it: constWalkedCopy)
        std::optional<ConstValue> written = constValue(constWalkedCopy(cur, cur->value, e->file, ectx.scope, e, instance), ectx, depth + 1);
        // (Off Windows an enumeration without a fixed type is as wide as its enumerators, which are
        // what is being found out: declaredUnderlying. `big = 0x100000000` stays, on Windows it is int.)
        bool holdsItsValue = written && written->kind == ConstValue::Kind::Int && !e->underlying && !e->isClass && types.longBits() == 64;
        std::optional<ConstValue> v = holdsItsValue ? ConstValue::integer(written->i, enumType) : constConverted(written, enumType);
        if (!v)
        {
          if (asked)
            constWhy() = whyAtAsked;
          return asked;
        }
        next = *v;
      }
      if (cur == d)
      {
        if (!plain)
          return next;
        asked = next;
        whyAtAsked = constWhy();
      }
      found.values.push_back({cur, next});
      ++next.i;
    }
    if (asked)
    {
      auto table = std::make_shared<EnumeratorTable>();
      for (const auto& [at, value] : found.values)
        table->emplace(at, value);
      {
        std::lock_guard lock(enumeratorTablesMutex);
        enumeratorTables.emplace(enumType, std::move(table));
      }
      constWhy() = whyAtAsked; // (as the evaluation left it at d, where it returned before)
      return asked;
    }
  }
  return std::nullopt;
}

auto GlobalScope::Impl::constDeclaredAlign(ClassDecl* cls, Expr* alignment, const Subst* sp, int depth) -> std::optional<int64_t>
{
  if (!alignment)
    return 1;
  DeclContext dc;
  declContextFor(cls, dc);
  TypeContext ctx2{dc.scope, &dc.params, sp, cls->file ? cls->file->tokens : nullptr, cls->file ? cls->file->text : std::string_view()};
  std::optional<ConstValue> v = constValue(alignment, ctx2, depth + 1);
  if (!v || v->kind != ConstValue::Kind::Int || v->i <= 0)
    return std::nullopt;
  return v->i;
}

auto GlobalScope::Impl::classBasesOf(const Ty* cls, ClassDecl*& def, int depth) -> std::vector<const Ty*>
{
  std::vector<const Ty*> out;
  def = nullptr;
  if (!cls || cls->kind != TyKind::Class || !cls->decl || depth > 64 || types.isDependent(cls))
    return out;
  Decl* d = cls->decl;
  if (!d->ownScope)
  {
    Entry e{d, nullptr};
    if (Scope* s = scopeOfEntry(e, 0))
      d = s->owner;
  }
  if (!d || d->kind != NodeKind::ClassDecl || !static_cast<ClassDecl*>(d)->isDefinition)
    return out;
  def = static_cast<ClassDecl*>(d);
  Subst subst = substFor(cls);
  const Subst* sp = subst.empty() ? nullptr : &subst;
  for (const ClassDecl::Base& b : def->bases)
  {
    DeclContext dc;
    declContextFor(def, dc);
    TypeContext ctx2{dc.scope, &dc.params, sp, def->file ? def->file->tokens : nullptr, def->file ? def->file->text : std::string_view()};
    const Ty* bt = convertType(b.type, ctx2, depth + 1);
    if (bt && bt->kind == TyKind::Pack)
      for (const Ty* element : bt->args)
        out.push_back(element);
    else
      out.push_back(bt);
  }
  return out;
}

auto GlobalScope::Impl::classDeclaredPolymorphic(const Ty* cls, int depth) -> bool
{
  ClassDecl* def = nullptr;
  std::vector<const Ty*> bases = classBasesOf(cls, def, depth);
  if (!def)
    return false;
  for (Decl* m : membersRead(def))
    if (m->kind == NodeKind::FunctionDecl && (m->flags & (DF_Virtual | DF_Override)) && !(m->flags & (DF_Static | DF_Constructor)) &&
        m->templateParams.empty())
      return true;
  for (const Ty* b : bases)
    if (classDeclaredPolymorphic(b, depth + 1))
      return true;
  return false;
}

auto GlobalScope::Impl::classDeclaredEmpty(const Ty* cls, int depth) -> bool
{
  ClassDecl* def = nullptr;
  std::vector<const Ty*> bases = classBasesOf(cls, def, depth);
  if (!def || classDeclaredPolymorphic(cls, depth))
    return false;
  for (Decl* m : membersRead(def))
    if ((m->kind == NodeKind::VariableDecl && !(m->flags & (DF_Static | DF_Typedef | DF_Friend))) ||
        (m->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(m)->name.parts.empty() && static_cast<ClassDecl*>(m)->isDefinition))
      return false; // (a data member, or an anonymous struct's or union's)
  for (const Ty* b : bases)
    if (!classDeclaredEmpty(b, depth + 1))
      return false;
  return true;
}

auto GlobalScope::Impl::classHasBaseAtOffset(const Ty* cls, int depth) -> bool
{
  ClassDecl* def = nullptr;
  std::vector<const Ty*> bases = classBasesOf(cls, def, depth);
  if (!def)
    return false;
  bool anyPolymorphic = false;
  for (const Ty* b : bases)
    anyPolymorphic = anyPolymorphic || classDeclaredPolymorphic(b, depth + 1);
  // Something at offset 0 that takes bytes (the class's own vtable pointer, then each base in the
  // layout's order that is not empty): a base after it is at a non-zero offset.
  bool taken = !anyPolymorphic && classDeclaredPolymorphic(cls, depth);
  for (bool polymorphicPass : {true, false})
    for (const Ty* b : bases)
      if (b && classDeclaredPolymorphic(b, depth + 1) == polymorphicPass)
      {
        if (taken || classHasBaseAtOffset(b, depth + 1))
          return true;
        if (!classDeclaredEmpty(b, depth + 1))
          taken = true;
      }
  return false;
}

bool GlobalScope::hasBaseAtOffset(const Ty* cls) { return impl->classHasBaseAtOffset(cls, 0); }

auto GlobalScope::Impl::classStartsWithBase(const Ty* cls, const Ty* base, int depth) -> bool
{
  if (cls == base)
    return true;
  ClassDecl* def = nullptr;
  ClassDecl* baseDef = nullptr;
  std::vector<const Ty*> bases = classBasesOf(cls, def, depth);
  classBasesOf(base, baseDef, depth);
  if (!def)
    return false;
  if (def == baseDef && cls->args.size() == base->args.size() && std::equal(cls->args.begin(), cls->args.end(), base->args.begin()))
    return true; // (a forward declaration's type and the definition's)
  bool anyPolymorphic = false;
  for (const Ty* b : bases)
    anyPolymorphic = anyPolymorphic || classDeclaredPolymorphic(b, depth + 1);
  if (!anyPolymorphic && classDeclaredPolymorphic(cls, depth))
    return false; // (its own vtable pointer is at the start)
  for (const Ty* b : bases) // (the first one the layout places: the first polymorphic one, or the first)
    if (b && (!anyPolymorphic || classDeclaredPolymorphic(b, depth + 1)))
      return classStartsWithBase(b, base, depth + 1);
  return false;
}

bool GlobalScope::classStartsWith(const Ty* cls, const Ty* base) { return impl->classStartsWithBase(cls, base, 0); }

bool GlobalScope::classDerivesFrom(const Ty* cls, const Ty* base) { return impl->derivesFrom(cls, base); }

auto GlobalScope::Impl::constSizeOf(const Ty* t, int depth) -> std::optional<int64_t>
{
  if (!t || depth > 24)
    return std::nullopt;
  if (const ConstHooks* h = constHooks(); h && h->layout)
    return h->layout(t, false); // the code generator's layout, the exact one
  t = types.unqualified(t);
  if (t->kind == TyKind::Class && exactLayoutOnly)
  {
    layoutNeeded = true;
    return std::nullopt;
  }
  switch (t->kind)
  {
    case TyKind::Bool: return 1;
    case TyKind::Char: case TyKind::Int: case TyKind::Float:
      return t->bits / 8;
    case TyKind::Nullptr: case TyKind::Pointer: case TyKind::LRef: case TyKind::RRef:
    case TyKind::Function:
      return 8;
    case TyKind::MemberPointer:
      // A data member's offset (4 bytes in MSVC's ABI, 8 in Itanium's: setMsvcAbi); a member function,
      // with the adjustment of `this` where its class has a base at an offset (hasBaseAtOffset, the
      // code generator's model).
      if (!t->inner || t->inner->kind != TyKind::Function)
        return msvcAbi ? 4 : 8;
      return !t->args.empty() && classHasBaseAtOffset(t->args[0], depth + 1) ? 16 : 8;
    case TyKind::Array:
    {
      auto inner = constSizeOf(t->inner, depth + 1);
      if (!inner) return std::nullopt;
      return int64_t(t->count) * *inner;
    }
    case TyKind::Enum:
    {
      // (Of the instance, and off Windows as wide as its enumerators: declaredUnderlying.)
      const Ty* u = declaredUnderlying(types.unqualified(t));
      if (u && u->isKnown())
        return constSizeOf(u, depth + 1);
      return 4; // default: int-sized
    }
    case TyKind::Class:
    {
      Decl* def = t->decl;
      if (def && !def->ownScope)
      {
        Entry e{def, nullptr};
        if (Scope* s = scopeOfEntry(e, 0))
          def = s->owner;
      }
      if (!def || def->kind != NodeKind::ClassDecl || !static_cast<ClassDecl*>(def)->isDefinition)
        return std::nullopt;
      auto* cls = static_cast<ClassDecl*>(def);
      Subst subst = substFor(t);
      const Subst* sp = subst.empty() ? nullptr : &subst;
      bool isUnion = cls->keyword == Kw::union_;
      int64_t size = 0, align = 1;
      // Base classes contribute their fields before own members.
      if (!isUnion)
        for (const ClassDecl::Base& b : cls->bases)
        {
          DeclContext dc;
          declContextFor(cls, dc);
          TypeContext ctx2{dc.scope, &dc.params, sp, cls->file ? cls->file->tokens : nullptr,
                          cls->file ? cls->file->text : std::string_view()};
          for (const Ty* bt : baseInstances(b.type, ctx2, depth + 1)) // (a pack of bases: each element)
          {
            if (!bt || !bt->isKnown() || types.isDependent(bt))
              return std::nullopt;
            auto bs = constSizeOf(bt, depth + 1);
            auto ba = constAlignOf(bt, depth + 1);
            if (!bs || !ba) return std::nullopt;
            size = (size + *ba - 1) / *ba * *ba + *bs;
            align = std::max(align, *ba);
          }
        }
      for (Decl* m : membersRead(cls))
      {
        const Ty* ft = nullptr;
        if (m->kind == NodeKind::VariableDecl && !(m->flags & (DF_Static | DF_Typedef | DF_Friend)))
          ft = declaredTypeOf(m, sp, depth + 1);
        else if (m->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(m)->name.parts.empty() &&
                 static_cast<ClassDecl*>(m)->isDefinition)
          ft = types.classType(m, {});
        else
          continue;
        if (!ft || !ft->isKnown() || types.isDependent(ft))
          return std::nullopt;
        auto fs = constSizeOf(ft, depth + 1);
        auto fa = constAlignOf(ft, depth + 1);
        if (!fs || !fa) return std::nullopt;
        if (m->kind == NodeKind::VariableDecl) // `alignas(8) unsigned char raw[4];`
        {
          auto own = constDeclaredAlign(cls, static_cast<VariableDecl*>(m)->alignment, sp, depth);
          if (!own) return std::nullopt;
          fa = std::max(*fa, *own);
        }
        if (isUnion)
        {
          size = std::max(size, *fs);
          align = std::max(align, *fa);
        }
        else
        {
          size = (size + *fa - 1) / *fa * *fa + *fs;
          align = std::max(align, *fa);
        }
      }
      auto declared = constDeclaredAlign(cls, cls->alignment, sp, depth); // `struct alignas(16) Wide`
      if (!declared) return std::nullopt;
      align = std::max(align, *declared);
      size = (size + align - 1) / align * align;
      return std::max<int64_t>(size, 1);
    }
    case TyKind::Vector:
    {
      auto inner = constSizeOf(t->inner, depth + 1);
      if (!inner) return std::nullopt;
      return int64_t(t->count) * *inner;
    }
    default:
      return std::nullopt;
  }
}

auto GlobalScope::Impl::constAlignOf(const Ty* t, int depth) -> std::optional<int64_t>
{
  if (!t || depth > 24) return std::nullopt;
  if (const ConstHooks* h = constHooks(); h && h->layout)
    return h->layout(t, true);
  t = types.unqualified(t);
  if (t->kind == TyKind::Array)
    return constAlignOf(t->inner, depth + 1);
  if (t->kind == TyKind::Class && exactLayoutOnly)
  {
    layoutNeeded = true; // only the code generator's layout is exact (bit fields, empty bases)
    return std::nullopt;
  }
  if (t->kind == TyKind::Class)
  {
    Decl* def = t->decl;
    if (def && !def->ownScope)
    {
      Entry e{def, nullptr};
      if (Scope* s = scopeOfEntry(e, 0))
        def = s->owner;
    }
    if (!def || def->kind != NodeKind::ClassDecl || !static_cast<ClassDecl*>(def)->isDefinition)
      return std::nullopt;
    auto* cls = static_cast<ClassDecl*>(def);
    Subst subst = substFor(t);
    const Subst* sp = subst.empty() ? nullptr : &subst;
    int64_t align = 1;
    for (const ClassDecl::Base& b : cls->bases)
    {
      DeclContext dc;
      declContextFor(cls, dc);
      TypeContext ctx2{dc.scope, &dc.params, sp, cls->file ? cls->file->tokens : nullptr,
                      cls->file ? cls->file->text : std::string_view()};
      for (const Ty* bt : baseInstances(b.type, ctx2, depth + 1)) // (a pack of bases: each element)
      {
        if (!bt || !bt->isKnown()) return std::nullopt;
        auto ba = constAlignOf(bt, depth + 1);
        if (!ba) return std::nullopt;
        align = std::max(align, *ba);
      }
    }
    for (Decl* m : membersRead(cls))
    {
      const Ty* ft = nullptr;
      if (m->kind == NodeKind::VariableDecl && !(m->flags & (DF_Static | DF_Typedef | DF_Friend)))
        ft = declaredTypeOf(m, sp, depth + 1);
      else if (m->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(m)->name.parts.empty() &&
               static_cast<ClassDecl*>(m)->isDefinition)
        ft = types.classType(m, {});
      else
        continue;
      if (!ft || !ft->isKnown()) return std::nullopt;
      auto fa = constAlignOf(ft, depth + 1);
      if (!fa) return std::nullopt;
      align = std::max(align, *fa);
      if (m->kind == NodeKind::VariableDecl)
      {
        auto own = constDeclaredAlign(cls, static_cast<VariableDecl*>(m)->alignment, sp, depth);
        if (!own) return std::nullopt;
        align = std::max(align, *own);
      }
    }
    auto declared = constDeclaredAlign(cls, cls->alignment, sp, depth);
    if (!declared) return std::nullopt;
    return std::max(align, *declared);
  }
  auto s = constSizeOf(t, depth);
  if (!s) return std::nullopt;
  if (t->kind == TyKind::MemberPointer)
    return std::min<int64_t>(*s, 8); // (a { function, adjustment } of 16 bytes is aligned as its pointer, as the code generator lays it out)
  return std::max<int64_t>(1, *s);
}

auto GlobalScope::Impl::constFieldOffset(const Ty* t, Decl* m, int depth) -> std::optional<int64_t>
{
  if (!t || !m || depth > 24)
    return std::nullopt;
  t = types.unqualified(types.stripRef(t));
  if (t->kind != TyKind::Class)
    return std::nullopt;
  if (const ConstHooks* h = constHooks(); h && h->offset)
    return h->offset(t, m);
  if (exactLayoutOnly)
  {
    layoutNeeded = true;
    return std::nullopt;
  }
  Decl* def = t->decl;
  if (def && !def->ownScope)
  {
    Entry e{def, nullptr};
    if (Scope* s = scopeOfEntry(e, 0))
      def = s->owner;
  }
  if (!def || def->kind != NodeKind::ClassDecl || !static_cast<ClassDecl*>(def)->isDefinition)
    return std::nullopt;
  auto* cls = static_cast<ClassDecl*>(def);
  Subst subst = substFor(t);
  const Subst* sp = subst.empty() ? nullptr : &subst;
  bool isUnion = cls->keyword == Kw::union_;
  int64_t offset = 0;
  if (!isUnion)
    for (const ClassDecl::Base& b : cls->bases)
    {
      DeclContext dc;
      declContextFor(cls, dc);
      TypeContext ctx2{dc.scope, &dc.params, sp, cls->file ? cls->file->tokens : nullptr, cls->file ? cls->file->text : std::string_view()};
      for (const Ty* bt : baseInstances(b.type, ctx2, depth + 1)) // (a pack of bases: each element)
      {
        if (!bt || !bt->isKnown() || types.isDependent(bt))
          return std::nullopt;
        auto bs = constSizeOf(bt, depth + 1);
        auto ba = constAlignOf(bt, depth + 1);
        if (!bs || !ba)
          return std::nullopt;
        offset = (offset + *ba - 1) / *ba * *ba;
        if (std::optional<int64_t> inner = constFieldOffset(bt, m, depth + 1))
          return offset + *inner;
        offset += *bs;
      }
    }
  for (Decl* f : membersRead(cls))
  {
    if (f->kind != NodeKind::VariableDecl || (f->flags & (DF_Static | DF_Typedef | DF_Friend)))
      continue; // (an anonymous struct or union member: not walked into here)
    const Ty* ft = declaredTypeOf(f, sp, depth + 1);
    if (!ft || !ft->isKnown() || types.isDependent(ft))
      return std::nullopt;
    auto fs = constSizeOf(ft, depth + 1);
    auto fa = constAlignOf(ft, depth + 1);
    auto own = constDeclaredAlign(cls, static_cast<VariableDecl*>(f)->alignment, sp, depth);
    if (!fs || !fa || !own)
      return std::nullopt;
    int64_t a = std::max(*fa, *own);
    if (isUnion)
    {
      if (f == m)
        return 0;
      continue;
    }
    offset = (offset + a - 1) / a * a;
    if (f == m)
      return offset;
    offset += *fs;
  }
  return std::nullopt;
}

auto GlobalScope::Impl::constMemberOffset(Expr* e, const TypeContext& ctx, int depth) -> std::optional<int64_t>
{
  std::vector<Expr*> steps; // the members and subscripts, outermost first
  Expr* p = e;
  while (p && (p->kind == NodeKind::MemberExpr || p->kind == NodeKind::IndexExpr) && steps.size() < 32)
  {
    steps.push_back(p);
    p = p->kind == NodeKind::MemberExpr ? static_cast<MemberExpr*>(p)->base : static_cast<IndexExpr*>(p)->base;
  }
  if (steps.empty() || !p || p->kind != NodeKind::CastExpr)
    return std::nullopt;
  auto* root = static_cast<CastExpr*>(p);
  if (!root->operand || root->operand->kind != NodeKind::LiteralExpr || static_cast<LiteralExpr*>(root->operand)->keyword != Kw::nullptr_)
    return std::nullopt;
  const Ty* pt = types.unqualified(convertType(root->type, ctx, depth + 1));
  if (!pt || pt->kind != TyKind::Pointer || !pt->inner)
    return std::nullopt;
  const Ty* current = types.unqualified(pt->inner);
  int64_t offset = 0;
  for (size_t i = steps.size(); i-- > 0;)
  {
    if (steps[i]->kind == NodeKind::IndexExpr)
    {
      auto* index = static_cast<IndexExpr*>(steps[i]);
      if (current->kind != TyKind::Array || index->args.size() != 1)
        return std::nullopt;
      std::optional<int64_t> k = constEval(index->args[0], ctx, depth + 1);
      std::optional<int64_t> size = constSizeOf(current->inner, depth + 1);
      if (!k || !size)
        return std::nullopt;
      offset += *k * *size;
      current = types.unqualified(current->inner);
      continue;
    }
    auto* m = static_cast<MemberExpr*>(steps[i]);
    if (current->kind != TyKind::Class || m->member.parts.size() != 1)
      return std::nullopt;
    Decl* field = m->resolved;
    if (!field || field->kind != NodeKind::VariableDecl)
    {
      Lookup l;
      Scope* s = classScopeOf(current);
      if (!s || !lookupInScope(s, m->member.parts[0].symbol, l, depth + 1) || !l.decl || l.decl->kind != NodeKind::VariableDecl)
        return std::nullopt;
      field = l.decl;
    }
    if (field->flags & (DF_Static | DF_Typedef))
      return std::nullopt;
    std::optional<int64_t> at = constFieldOffset(current, field, depth + 1);
    if (!at)
      return std::nullopt;
    offset += *at;
    Scope* in = field->parent && field->parent->kind == NodeKind::ClassDecl ? field->parent->ownScope : nullptr;
    Subst s = memberSubst(current, in, depth + 1); // a base's member: that base's arguments
    const Ty* ft = declaredTypeOf(field, s.empty() ? nullptr : &s, depth + 1);
    if (!ft || !ft->isKnown())
      return std::nullopt;
    current = types.unqualified(types.stripRef(ft));
  }
  return offset;
}

auto GlobalScope::Impl::constCastValue(Type* type, Expr* operand, const TypeContext& ctx, int depth) -> std::optional<ConstValue>
{
  if (operand && operand->kind == NodeKind::UnaryExpr && !static_cast<UnaryExpr*>(operand)->isKeyword && !static_cast<UnaryExpr*>(operand)->postfix &&
      P(static_cast<UnaryExpr*>(operand)->op) == P::Amp && type)
    if (const Ty* t = convertType(type, ctx, depth + 1); t && types.unqualified(t)->kind == TyKind::Int)
      if (std::optional<int64_t> offset = constMemberOffset(static_cast<UnaryExpr*>(operand)->operand, ctx, depth + 1))
        return constConverted(ConstValue::integer(*offset, types.intType(64, true)), t); // offsetof(T, member)
  std::optional<ConstValue> v = constValue(operand, ctx, depth + 1);
  if (!v || !type)
    return v;
  const Ty* t = convertType(type, ctx, depth + 1);
  if (t && t->isReference() && v->type && !v->isScalar())
  {
    // `(T&&)value` (forward, move): the same object, another value category; an array of the
    // element type the target names without its length (`const char(&)[]`), or an object of a
    // class derived from the target's, stays what it is.
    const Ty* want = types.unqualified(types.stripRef(t));
    const Ty* have = types.unqualified(types.stripRef(v->type));
    bool same = want == have || (want->kind == TyKind::Array && have->kind == TyKind::Array && (!want->count || want->count == have->count) &&
                                 want->inner && have->inner && types.unqualified(want->inner) == types.unqualified(have->inner));
    if (same || (want->kind == TyKind::Class && have->kind == TyKind::Class && constBasePath(have, want, 0)))
      return v;
  }
  return constConverted(v, t);
}

auto GlobalScope::Impl::stackRoomLeft() -> bool
{
#if defined(_WIN32)
  static thread_local uintptr_t low = 0;
  if (!low)
  {
    unsigned long long l = 0, h = 0;
    GetCurrentThreadStackLimits(&l, &h);
    low = uintptr_t(l);
  }
  char here;
  return uintptr_t(&here) - low > (uintptr_t(1) << 20);
#else
  return true;
#endif
}

auto GlobalScope::Impl::evaluationTrip(const char* why) -> void
{
  if (constWhy().empty())
    constWhy() = why;
  ++evaluationTrips;
}

namespace
{
  const char* const namesTooDeep = "constants nested more than 1024 deep (each name's value needing the next one's, as a template recursion does)";
  const char* const callsTooDeep = "calls nested more than 512 deep";
  const char* const stackTooDeep = "an evaluation nested deeper than the compiler's stack";
  const char* const cachedTooDeep = "declarations' types, class templates' specialisations, layouts, traits and static members nested more than 4096 deep";
}

auto GlobalScope::Impl::levelsTooDeep() -> const char* { return cachedTooDeep; }

auto GlobalScope::Impl::enterName() -> bool
{
  deepestNames = std::max(deepestNames, ++nameEvaluations);
  if (nameEvaluations > maxNestedNames)
    return evaluationTrip(namesTooDeep), false;
  if (!stackRoomLeft())
    return evaluationTrip(stackTooDeep), false;
  return true;
}

auto GlobalScope::Impl::enterCall() -> bool
{
  deepestCalls = std::max(deepestCalls, ++constCalls());
  if (constCalls() > maxNestedCalls)
    return evaluationTrip(callsTooDeep), false;
  if (!stackRoomLeft())
    return evaluationTrip(stackTooDeep), false;
  return true;
}

auto GlobalScope::Impl::cachedNestingFits(Nesting needed) -> bool
{
  int names = nameEvaluations + needed.names, calls = constCalls() + needed.calls, levels = cachedLevels + needed.levels;
  deepestNames = std::max(deepestNames, names);
  deepestCalls = std::max(deepestCalls, calls);
  deepestLevels = std::max(deepestLevels, levels);
  if (names > maxNestedNames)
    return evaluationTrip(namesTooDeep), false;
  if (calls > maxNestedCalls)
    return evaluationTrip(callsTooDeep), false;
  if (levels > maxNestedLevels)
    return evaluationTrip(cachedTooDeep), false;
  return true;
}

auto GlobalScope::Impl::memberConstant(Decl* d, const TypeContext& ctx, const Subst& classSubst) -> std::optional<ConstValue>
{
  if (exactLayoutOnly)
    return constValueOfDecl(d, ctx, 0, &classSubst);
  std::vector<const void*> key{d};
  for (const Subst::Pair& p : classSubst.pairs)
  {
    key.push_back(p.param);
    key.push_back(p.type);
  }
  {
    std::lock_guard lock(memberConstantsMutex);
    if (auto it = memberConstants.find(key); it != memberConstants.end())
    {
      replayUses(it->second.uses);
      if (!cachedNestingFits(it->second.needed))
        return std::nullopt;
      return it->second.value;
    }
  }
  CollectUses collected;
  CachedEvaluation evaluation;
  if (!evaluation.ok)
    return std::nullopt;
  std::optional<ConstValue> v = constValueOfDecl(d, ctx, 0, &classSubst);
  if (evaluation.tripped())
    return std::nullopt;
  if (v)
  {
    std::lock_guard lock(memberConstantsMutex);
    memberConstants.emplace(std::move(key), MemberConstant{*v, collected.uses, evaluation.needed()});
  }
  return v;
}
