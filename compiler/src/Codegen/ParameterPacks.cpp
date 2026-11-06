// struct Emitter's members: parameter packs; their declarations in ParameterPacks.hpp, part of the
// struct's body in Codegen/Emitter.hpp. A translation unit of its own since 2026-11-05 (codegen.cpp
// split, stage 2), moved without a change.
#include "Emitter.hpp"

auto codegen::Emitter::packIn(Node* n, int depth) -> Decl*
{
  std::vector<Decl*> all;
  packsIn(n, all, depth);
  return all.empty() ? nullptr : all[0];
}

auto codegen::Emitter::packsIn(Node* n, std::vector<Decl*>& out, int depth) -> void
{
  if (!n || depth > 32)
    return;
  switch (n->kind)
  {
    case NodeKind::NameExpr:
    {
      Decl* d = static_cast<NameExpr*>(n)->resolved;
      if (d && packLocals.count(d) && std::find(out.begin(), out.end(), d) == out.end())
        out.push_back(d);
      return;
    }
    case NodeKind::ParenExpr: packsIn(static_cast<ParenExpr*>(n)->inner, out, depth + 1); return;
    case NodeKind::UnaryExpr: packsIn(static_cast<UnaryExpr*>(n)->operand, out, depth + 1); return;
    case NodeKind::CastExpr: packsIn(static_cast<CastExpr*>(n)->operand, out, depth + 1); return;
    case NodeKind::NamedCastExpr: packsIn(static_cast<NamedCastExpr*>(n)->operand, out, depth + 1); return;
    case NodeKind::MemberExpr: packsIn(static_cast<MemberExpr*>(n)->base, out, depth + 1); return;
    case NodeKind::BinaryExpr:
      packsIn(static_cast<BinaryExpr*>(n)->lhs, out, depth + 1);
      packsIn(static_cast<BinaryExpr*>(n)->rhs, out, depth + 1);
      return;
    case NodeKind::CallExpr:
    {
      auto* c = static_cast<CallExpr*>(n);
      packsIn(c->callee, out, depth + 1);
      for (Expr* a : c->args)
        packsIn(a, out, depth + 1);
      return;
    }
    case NodeKind::IndexExpr:
    {
      auto* i = static_cast<IndexExpr*>(n);
      packsIn(i->base, out, depth + 1);
      for (Expr* a : i->args)
        packsIn(a, out, depth + 1);
      return;
    }
    case NodeKind::FunctionalCastExpr:
      for (Expr* a : static_cast<FunctionalCastExpr*>(n)->args)
        packsIn(a, out, depth + 1);
      return;
    default:
      return;
  }
}

auto codegen::Emitter::templatePacksOf(Decl* param) -> std::vector<Decl*>
{
  std::vector<Decl*> packs;
  collectPacks(global.typeOfDecl(param), packs, 0);
  return packs;
}

auto codegen::Emitter::collectPacks(const Ty* t, std::vector<Decl*>& packs, int depth) -> void
{
  if (!t || depth > 16)
    return;
  if (t->kind == TyKind::Param && t->decl && t->decl->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(t->decl)->isPack)
  {
    if (std::find(packs.begin(), packs.end(), t->decl) == packs.end())
      packs.push_back(t->decl);
    return;
  }
  collectPacks(t->inner, packs, depth + 1);
  for (const Ty* a : t->args)
    collectPacks(a, packs, depth + 1);
}

auto codegen::Emitter::boundPacks(const std::vector<Decl*>& packs, size_t count, Node* at) -> std::vector<const Ty*>
{
  std::vector<const Ty*> bound;
  for (Decl* tp : packs)
  {
    const Ty* packTy = nullptr;
    if (currentArgs)
      for (auto& [d, t] : currentArgs->pairs)
        if (d == tp)
          packTy = t;
    if (!packTy || packTy->kind != TyKind::Pack || packTy->args.size() != count)
      unsupported(at, "a pack whose elements are unknown");
    bound.push_back(packTy);
  }
  return bound;
}

auto codegen::Emitter::expandedCount(Span<Expr*> args, Node* at) -> size_t
{
  size_t n = 0;
  for (Expr* a : args)
    n += isPackExpansion(a) ? packElementCount(static_cast<UnaryExpr*>(a)->operand, at) : 1;
  return n;
}

auto codegen::Emitter::packElementCount(Expr* pattern, Node* at) -> size_t
{
  Decl* param = packIn(pattern);
  if (!param)
    if (Decl* pack = currentArgs ? typePackIn(pattern) : nullptr)
      return boundPack(pack)->args.size(); // `values[I]...` over `int... I`
  if (!param)
    unsupported(at, "a pack expansion that does not name a parameter pack");
  return packLocals[param].size();
}

auto codegen::Emitter::boundPack(Decl* pack) -> const Ty*
{
  for (auto& [d, t] : currentArgs->pairs)
    if (d == pack)
      return t;
  return nullptr;
}

auto codegen::Emitter::typePackIn(Node* n, int depth) -> Decl*
{
  if (!n || depth > 32 || !currentArgs)
    return nullptr;
  auto inName = [&](const QualifiedName& name) -> Decl*
  {
    for (const NamePart& part : name.parts)
      for (Node* a : part.templateArgs)
        if (Decl* d = typePackIn(a, depth + 1))
          return d;
    return nullptr;
  };
  auto either = [&](Node* a, Node* b) -> Decl* { Decl* d = typePackIn(a, depth + 1); return d ? d : typePackIn(b, depth + 1); };
  switch (n->kind)
  {
    case NodeKind::NamedType:
    {
      auto* t = static_cast<NamedType*>(n);
      if (Decl* d = t->resolved; d && d->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(d)->isPack)
        for (auto& [p, a] : currentArgs->pairs)
          if (p == d && a && a->kind == TyKind::Pack)
            return d;
      return inName(t->name);
    }
    case NodeKind::PointerType: return typePackIn(static_cast<PointerType*>(n)->pointee, depth + 1);
    case NodeKind::LRefType: return typePackIn(static_cast<LRefType*>(n)->referenced, depth + 1);
    case NodeKind::RRefType: return typePackIn(static_cast<RRefType*>(n)->referenced, depth + 1);
    case NodeKind::ConstType: return typePackIn(static_cast<ConstType*>(n)->inner, depth + 1);
    case NodeKind::NameExpr:
    {
      // `Args()` parsed as a call of the name Args.
      if (Decl* d = static_cast<NameExpr*>(n)->resolved; d && d->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(d)->isPack)
        for (auto& [p, a] : currentArgs->pairs)
          if (p == d && a && a->kind == TyKind::Pack)
            return d;
      return inName(static_cast<NameExpr*>(n)->name);
    }
    case NodeKind::ParenExpr: return typePackIn(static_cast<ParenExpr*>(n)->inner, depth + 1);
    case NodeKind::UnaryExpr: return typePackIn(static_cast<UnaryExpr*>(n)->operand, depth + 1);
    case NodeKind::MemberExpr: return typePackIn(static_cast<MemberExpr*>(n)->base, depth + 1);
    case NodeKind::IndexExpr:
    {
      auto* i = static_cast<IndexExpr*>(n);
      if (Decl* d = typePackIn(i->base, depth + 1))
        return d;
      for (Expr* a : i->args)
        if (Decl* d = typePackIn(a, depth + 1))
          return d;
      return nullptr;
    }
    case NodeKind::CastExpr: return either(static_cast<CastExpr*>(n)->type, static_cast<CastExpr*>(n)->operand);
    case NodeKind::NamedCastExpr: return either(static_cast<NamedCastExpr*>(n)->type, static_cast<NamedCastExpr*>(n)->operand);
    case NodeKind::BinaryExpr: return either(static_cast<BinaryExpr*>(n)->lhs, static_cast<BinaryExpr*>(n)->rhs);
    case NodeKind::SizeofExpr:
    {
      auto* s = static_cast<SizeofExpr*>(n);
      return s->isPack ? nullptr : either(s->type, s->operand); // `sizeof...(Args)` counts the pack, it expands nothing
    }
    case NodeKind::FunctionalCastExpr:
    {
      auto* c = static_cast<FunctionalCastExpr*>(n);
      if (Decl* d = typePackIn(c->type, depth + 1))
        return d;
      for (Expr* a : c->args)
        if (Decl* d = typePackIn(a, depth + 1))
          return d;
      return nullptr;
    }
    case NodeKind::CallExpr:
    {
      auto* c = static_cast<CallExpr*>(n);
      if (Decl* d = typePackIn(c->callee, depth + 1))
        return d;
      for (Expr* a : c->args)
        if (Decl* d = typePackIn(a, depth + 1))
          return d;
      return nullptr;
    }
    default:
      return nullptr;
  }
}

auto codegen::Emitter::isPackExpansion(Expr* e) -> bool
{
  return e && e->kind == NodeKind::UnaryExpr && !static_cast<UnaryExpr*>(e)->isKeyword &&
         P(static_cast<UnaryExpr*>(e)->op) == P::Ellipsis && static_cast<UnaryExpr*>(e)->postfix;
}
