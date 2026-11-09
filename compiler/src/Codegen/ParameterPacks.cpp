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
    case NodeKind::BraceInitExpr: // (`Box{args}`)
      for (Expr* a : static_cast<BraceInitExpr*>(n)->args)
        packsIn(a, out, depth + 1);
      return;
    case NodeKind::ConditionalExpr:
      packsIn(static_cast<ConditionalExpr*>(n)->cond, out, depth + 1);
      packsIn(static_cast<ConditionalExpr*>(n)->then, out, depth + 1);
      packsIn(static_cast<ConditionalExpr*>(n)->else_, out, depth + 1);
      return;
    case NodeKind::SizeofExpr:
    {
      // `sizeof(a)` of one element (`(0 + ... + (sizeof(a) == 4 ? 1 : 0))`; not sizeof..., which
      // counts the pack): its operand, or the name the parser read as a type.
      auto* s = static_cast<SizeofExpr*>(n);
      if (s->isPack)
        return;
      packsIn(s->operand, out, depth + 1);
      if (s->type && s->type->kind == NodeKind::NamedType && static_cast<NamedType*>(s->type)->name.parts.size() == 1)
      {
        Decl* d = static_cast<NamedType*>(s->type)->resolved;
        if (d && packLocals.count(d) && std::find(out.begin(), out.end(), d) == out.end())
          out.push_back(d);
      }
      return;
    }
    default:
      return;
  }
}

auto codegen::Emitter::templatePacksOf(Decl* param) -> std::vector<Decl*>
{
  std::vector<Decl*> packs;
  if (param && param->kind == NodeKind::VariableDecl)
  {
    // An init-capture pack (`[...w = v * 2]`): the template packs of the parameter packs its
    // pattern expands, which an element of it is typed by (the walk types w's element from v's).
    Expr* init = static_cast<VariableDecl*>(param)->init;
    if (init && init->kind == NodeKind::UnaryExpr)
      for (Decl* p : parameterPacksNamedIn(static_cast<UnaryExpr*>(init)->operand))
        for (Decl* tp : templatePacksOf(p))
          if (std::find(packs.begin(), packs.end(), tp) == packs.end())
            packs.push_back(tp);
    return packs;
  }
  collectPacks(global.typeOfDecl(param), packs, 0);
  return packs;
}

auto codegen::Emitter::parameterPacksNamedIn(Node* n, int depth) -> std::vector<Decl*>
{
  std::vector<Decl*> out;
  if (!n || depth > 32)
    return out;
  auto add = [&](Node* x) {
    for (Decl* d : parameterPacksNamedIn(x, depth + 1))
      if (std::find(out.begin(), out.end(), d) == out.end())
        out.push_back(d);
  };
  switch (n->kind)
  {
    case NodeKind::NameExpr:
      if (Decl* d = static_cast<NameExpr*>(n)->resolved; d && d->kind == NodeKind::ParamDecl && static_cast<ParamDecl*>(d)->isPack)
        out.push_back(d);
      break;
    case NodeKind::ParenExpr: add(static_cast<ParenExpr*>(n)->inner); break;
    case NodeKind::UnaryExpr: add(static_cast<UnaryExpr*>(n)->operand); break;
    case NodeKind::CastExpr: add(static_cast<CastExpr*>(n)->operand); break;
    case NodeKind::NamedCastExpr: add(static_cast<NamedCastExpr*>(n)->operand); break;
    case NodeKind::MemberExpr: add(static_cast<MemberExpr*>(n)->base); break;
    case NodeKind::BinaryExpr:
      add(static_cast<BinaryExpr*>(n)->lhs);
      add(static_cast<BinaryExpr*>(n)->rhs);
      break;
    case NodeKind::CallExpr:
      add(static_cast<CallExpr*>(n)->callee);
      for (Expr* a : static_cast<CallExpr*>(n)->args)
        add(a);
      break;
    case NodeKind::FunctionalCastExpr:
      for (Expr* a : static_cast<FunctionalCastExpr*>(n)->args)
        add(a);
      break;
    default:
      break;
  }
  return out;
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
  std::vector<Decl*> all;
  typePacksIn(n, all, depth);
  return all.empty() ? nullptr : all[0];
}

auto codegen::Emitter::addNamedPacks(Node* pattern, std::vector<Decl*>& packs, size_t count) -> bool
{
  std::vector<Decl*> named;
  typePacksIn(pattern, named);
  bool added = false;
  for (Decl* d : named)
    if (std::find(packs.begin(), packs.end(), d) == packs.end())
      if (const Ty* bound = boundPack(d); bound && bound->args.size() == count)
      {
        packs.push_back(d);
        added = true;
      }
  return added;
}

auto codegen::Emitter::typePacksIn(Node* n, std::vector<Decl*>& out, int depth) -> void
{
  if (!n || depth > 32 || !currentArgs)
    return;
  auto inName = [&](const QualifiedName& name)
  {
    for (const NamePart& part : name.parts)
      for (Node* a : part.templateArgs)
        typePacksIn(a, out, depth + 1);
  };
  auto named = [&](Decl* d)
  {
    if (d && d->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(d)->isPack)
      for (auto& [p, a] : currentArgs->pairs)
        if (p == d && a && a->kind == TyKind::Pack)
        {
          if (std::find(out.begin(), out.end(), d) == out.end())
            out.push_back(d);
          return true;
        }
    return false;
  };
  auto visit = [&](Node* x) { typePacksIn(x, out, depth + 1); };
  switch (n->kind)
  {
    case NodeKind::NamedType:
    {
      auto* t = static_cast<NamedType*>(n);
      if (!named(t->resolved))
        inName(t->name);
      return;
    }
    case NodeKind::PointerType: visit(static_cast<PointerType*>(n)->pointee); return;
    case NodeKind::LRefType: visit(static_cast<LRefType*>(n)->referenced); return;
    case NodeKind::RRefType: visit(static_cast<RRefType*>(n)->referenced); return;
    case NodeKind::ConstType: visit(static_cast<ConstType*>(n)->inner); return;
    case NodeKind::NameExpr:
      // (`Args()` parsed as a call of the name Args)
      if (!named(static_cast<NameExpr*>(n)->resolved))
        inName(static_cast<NameExpr*>(n)->name);
      return;
    case NodeKind::ParenExpr: visit(static_cast<ParenExpr*>(n)->inner); return;
    case NodeKind::UnaryExpr:
      if (!isPackExpansion(static_cast<UnaryExpr*>(n))) // (an expansion inside the pattern expands its own packs, whole)
        visit(static_cast<UnaryExpr*>(n)->operand);
      return;
    case NodeKind::MemberExpr:
      // (`this->Leaf<A>::value` of a pack of bases: the member's qualifier names the pack too)
      visit(static_cast<MemberExpr*>(n)->base);
      inName(static_cast<MemberExpr*>(n)->member);
      return;
    case NodeKind::IndexExpr:
      visit(static_cast<IndexExpr*>(n)->base);
      for (Expr* a : static_cast<IndexExpr*>(n)->args)
        visit(a);
      return;
    case NodeKind::CastExpr:
      visit(static_cast<CastExpr*>(n)->type);
      visit(static_cast<CastExpr*>(n)->operand);
      return;
    case NodeKind::NamedCastExpr:
      visit(static_cast<NamedCastExpr*>(n)->type);
      visit(static_cast<NamedCastExpr*>(n)->operand);
      return;
    case NodeKind::BinaryExpr:
      visit(static_cast<BinaryExpr*>(n)->lhs);
      visit(static_cast<BinaryExpr*>(n)->rhs);
      return;
    case NodeKind::ConditionalExpr:
      visit(static_cast<ConditionalExpr*>(n)->cond);
      visit(static_cast<ConditionalExpr*>(n)->then);
      visit(static_cast<ConditionalExpr*>(n)->else_);
      return;
    case NodeKind::SizeofExpr:
      if (auto* s = static_cast<SizeofExpr*>(n); !s->isPack) // (`sizeof...(Args)` counts the pack, it expands nothing)
      {
        visit(s->type);
        visit(s->operand);
      }
      return;
    case NodeKind::FunctionalCastExpr:
      visit(static_cast<FunctionalCastExpr*>(n)->type);
      for (Expr* a : static_cast<FunctionalCastExpr*>(n)->args)
        visit(a);
      return;
    case NodeKind::BraceInitExpr:
      for (Expr* a : static_cast<BraceInitExpr*>(n)->args)
        visit(a);
      return;
    case NodeKind::CallExpr:
      visit(static_cast<CallExpr*>(n)->callee);
      for (Expr* a : static_cast<CallExpr*>(n)->args)
        visit(a);
      return;
    default:
      return;
  }
}

auto codegen::Emitter::isPackExpansion(Expr* e) -> bool
{
  return e && e->kind == NodeKind::UnaryExpr && !static_cast<UnaryExpr*>(e)->isKeyword &&
         P(static_cast<UnaryExpr*>(e)->op) == P::Ellipsis && static_cast<UnaryExpr*>(e)->postfix;
}
