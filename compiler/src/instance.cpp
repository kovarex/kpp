#include "instance.hpp"
#include <cstring>
#include <vector>

// Nodes are copied byte for byte first (every scalar field, and the pointers to what stays shared),
// then the pointers to the parts the copy owns are replaced by copies of their own. Declarations
// copied are remembered, so that references inside the copy to them (a name naming a parameter or
// a local, a type naming a local alias, a closure and its lambda) point to the copies.

template<typename T>
T* InstanceCopier::raw(const T* n)
{
  auto* copy = static_cast<T*>(arena.allocate(sizeof(T), alignof(T)));
  std::memcpy(static_cast<void*>(copy), static_cast<const void*>(n), sizeof(T));
  ++nodes;
  return copy;
}

template<typename T, typename F>
Span<T> InstanceCopier::span(Span<T> s, F&& each)
{
  if (s.empty())
    return s;
  Span<T> out;
  out.data = arena.array(s.data, s.count);
  out.count = s.count;
  for (uint32_t i = 0; i < out.count; ++i)
    each(out.data[i]);
  return out;
}

Span<Expr*> InstanceCopier::exprs(Span<Expr*> s)
{
  return span(s, [&](Expr*& e) { e = expr(e); });
}

Span<Decl*> InstanceCopier::declList(Span<Decl*> s)
{
  return span(s, [&](Decl*& d) { d = decl(d); });
}

QualifiedName InstanceCopier::name(const QualifiedName& q)
{
  QualifiedName out = q;
  out.parts = span(q.parts, [&](NamePart& part)
  {
    part.templateArgs = span(part.templateArgs, [&](Node*& a) { a = node(a); });
    part.convType = type(part.convType);
  });
  return out;
}

Node* InstanceCopier::node(const Node* n)
{
  if (!n)
    return nullptr;
  NodeKind k = n->kind;
  if (k <= NodeKind::PackType)
    return type(static_cast<const Type*>(n));
  if (k <= NodeKind::ImportDecl)
    return decl(static_cast<const Decl*>(n));
  if (k <= NodeKind::MacroStmt)
    return stmt(static_cast<const Stmt*>(n));
  return expr(static_cast<const Expr*>(n));
}

Type* InstanceCopier::type(const Type* t)
{
  if (!t)
    return nullptr;
  switch (t->kind)
  {
    case NodeKind::NamedType:
    {
      auto* c = raw(static_cast<const NamedType*>(t));
      c->name = name(c->name);
      return c;
    }
    case NodeKind::BuiltinType: return raw(static_cast<const BuiltinType*>(t));
    case NodeKind::AutoType: return raw(static_cast<const AutoType*>(t));
    case NodeKind::PointerType:
    {
      auto* c = raw(static_cast<const PointerType*>(t));
      c->pointee = type(c->pointee);
      return c;
    }
    case NodeKind::LRefType:
    {
      auto* c = raw(static_cast<const LRefType*>(t));
      c->referenced = type(c->referenced);
      return c;
    }
    case NodeKind::RRefType:
    {
      auto* c = raw(static_cast<const RRefType*>(t));
      c->referenced = type(c->referenced);
      return c;
    }
    case NodeKind::ConstType:
    {
      auto* c = raw(static_cast<const ConstType*>(t));
      c->inner = type(c->inner);
      return c;
    }
    case NodeKind::ArrayType:
    {
      auto* c = raw(static_cast<const ArrayType*>(t));
      c->element = type(c->element);
      c->size = expr(c->size);
      return c;
    }
    case NodeKind::FunctionType:
    {
      auto* c = raw(static_cast<const FunctionType*>(t));
      c->result = type(c->result);
      c->params = declList(c->params);
      c->asCall = expr(c->asCall);
      return c;
    }
    case NodeKind::DecltypeType:
    {
      auto* c = raw(static_cast<const DecltypeType*>(t));
      c->expr = expr(c->expr);
      return c;
    }
    case NodeKind::MemberPointerType:
    {
      auto* c = raw(static_cast<const MemberPointerType*>(t));
      c->classType = type(c->classType);
      c->pointee = type(c->pointee);
      return c;
    }
    case NodeKind::PackType:
    {
      auto* c = raw(static_cast<const PackType*>(t));
      c->pattern = type(c->pattern);
      return c;
    }
    default: return const_cast<Type*>(t);
  }
}

Decl* InstanceCopier::decl(const Decl* d)
{
  if (!d)
    return nullptr;
  if (auto it = decls.find(const_cast<Decl*>(d)); it != decls.end())
    return it->second; // reached twice (a declaration listed in two places)
  Decl* copy = nullptr;
  switch (d->kind)
  {
    case NodeKind::ParamDecl:
    {
      auto* c = raw(static_cast<const ParamDecl*>(d));
      decls[const_cast<Decl*>(d)] = c;
      c->type = type(c->type);
      c->defaultValue = expr(c->defaultValue);
      copy = c;
      break;
    }
    case NodeKind::VariableDecl:
    {
      auto* c = raw(static_cast<const VariableDecl*>(d));
      decls[const_cast<Decl*>(d)] = c;
      c->type = type(c->type);
      c->name = name(c->name);
      c->init = expr(c->init);
      c->bitfield = expr(c->bitfield);
      c->structuredBindings = exprs(c->structuredBindings);
      copy = c;
      break;
    }
    case NodeKind::TemplateParamDecl:
    {
      auto* c = raw(static_cast<const TemplateParamDecl*>(d));
      decls[const_cast<Decl*>(d)] = c;
      c->valueType = type(c->valueType);
      c->defaultArg = node(c->defaultArg);
      c->innerParams = declList(c->innerParams);
      copy = c;
      break;
    }
    case NodeKind::ClassDecl:
    {
      // A class written inside the body (a local class, a lambda's closure): a class of this
      // instantiation, with members of its own.
      auto* c = raw(static_cast<const ClassDecl*>(d));
      decls[const_cast<Decl*>(d)] = c;
      c->ownScope = nullptr;
      c->templateParams = declList(c->templateParams);
      c->name = name(c->name);
      c->bases = span(c->bases, [&](ClassDecl::Base& b) { b.type = type(b.type); });
      c->members = declList(c->members);
      c->alignment = expr(c->alignment);
      copy = c;
      break;
    }
    case NodeKind::EnumDecl:
    {
      auto* c = raw(static_cast<const EnumDecl*>(d));
      decls[const_cast<Decl*>(d)] = c;
      c->ownScope = nullptr;
      c->name = name(c->name);
      c->underlying = type(c->underlying);
      c->enumerators = declList(c->enumerators);
      c->members = declList(c->members);
      copy = c;
      break;
    }
    case NodeKind::EnumeratorDecl:
    {
      auto* c = raw(static_cast<const EnumeratorDecl*>(d));
      decls[const_cast<Decl*>(d)] = c;
      c->value = expr(c->value);
      copy = c;
      break;
    }
    case NodeKind::FunctionDecl:
    {
      auto* c = raw(static_cast<const FunctionDecl*>(d));
      decls[const_cast<Decl*>(d)] = c;
      if (d != root) // the instantiated function's own parameters are what its arguments bind: kept
        c->templateParams = declList(c->templateParams);
      c->returnType = type(c->returnType);
      c->name = name(c->name);
      c->params = declList(c->params);
      c->trailingReturn = type(c->trailingReturn);
      c->body = stmt(c->body);
      c->ctorInits = span(c->ctorInits, [&](FunctionDecl::CtorInit& init)
      {
        init.member = name(init.member);
        init.args = exprs(init.args);
      });
      c->requiresClause = expr(c->requiresClause);
      c->condition = expr(c->condition);
      c->explicitCondition = expr(c->explicitCondition);
      copy = c;
      break;
    }
    case NodeKind::UsingAliasDecl:
    {
      auto* c = raw(static_cast<const UsingAliasDecl*>(d));
      decls[const_cast<Decl*>(d)] = c;
      c->type = type(c->type);
      copy = c;
      break;
    }
    case NodeKind::UsingDirectiveDecl:
    {
      auto* c = raw(static_cast<const UsingDirectiveDecl*>(d));
      decls[const_cast<Decl*>(d)] = c;
      c->name = name(c->name);
      copy = c;
      break;
    }
    case NodeKind::UsingNameDecl:
    {
      auto* c = raw(static_cast<const UsingNameDecl*>(d));
      decls[const_cast<Decl*>(d)] = c;
      c->name = name(c->name);
      copy = c;
      break;
    }
    case NodeKind::StaticAssertDecl:
    {
      auto* c = raw(static_cast<const StaticAssertDecl*>(d));
      decls[const_cast<Decl*>(d)] = c;
      c->cond = expr(c->cond);
      copy = c;
      break;
    }
    case NodeKind::FriendDecl:
    {
      auto* c = raw(static_cast<const FriendDecl*>(d));
      decls[const_cast<Decl*>(d)] = c;
      c->type = type(c->type);
      copy = c;
      break;
    }
    case NodeKind::MacroDecl: copy = raw(static_cast<const MacroDecl*>(d)); break;
    case NodeKind::ErrorDecl: copy = raw(static_cast<const ErrorDecl*>(d)); break;
    case NodeKind::EmptyDecl: copy = raw(static_cast<const EmptyDecl*>(d)); break;
    case NodeKind::AsmDecl: copy = raw(static_cast<const AsmDecl*>(d)); break;
    default: return const_cast<Decl*>(d); // a namespace or an import cannot appear in a body
  }
  copy->cachedType.store(nullptr, std::memory_order_relaxed); // typed again, for this instantiation
  if (!decls.count(const_cast<Decl*>(d)))
    decls[const_cast<Decl*>(d)] = copy;
  return copy;
}

Stmt* InstanceCopier::stmt(const Stmt* s)
{
  if (!s)
    return nullptr;
  switch (s->kind)
  {
    case NodeKind::BlockStmt:
    {
      auto* c = raw(static_cast<const BlockStmt*>(s));
      c->stmts = span(c->stmts, [&](Stmt*& x) { x = stmt(x); });
      return c;
    }
    case NodeKind::ExprStmt:
    {
      auto* c = raw(static_cast<const ExprStmt*>(s));
      c->expr = expr(c->expr);
      return c;
    }
    case NodeKind::DeclStmt:
    {
      auto* c = raw(static_cast<const DeclStmt*>(s));
      c->decls = declList(c->decls);
      return c;
    }
    case NodeKind::IfStmt:
    {
      auto* c = raw(static_cast<const IfStmt*>(s));
      c->init = stmt(c->init);
      c->cond = node(c->cond);
      c->then = stmt(c->then);
      c->else_ = stmt(c->else_);
      c->constexprKnown = -1;
      return c;
    }
    case NodeKind::AssertStmt:
    {
      auto* c = raw(static_cast<const AssertStmt*>(s));
      c->cond = expr(c->cond);
      return c;
    }
    case NodeKind::ForStmt:
    {
      auto* c = raw(static_cast<const ForStmt*>(s));
      c->init = stmt(c->init);
      c->cond = node(c->cond);
      c->step = expr(c->step);
      c->body = stmt(c->body);
      return c;
    }
    case NodeKind::RangeForStmt:
    {
      auto* c = raw(static_cast<const RangeForStmt*>(s));
      c->var = decl(c->var);
      c->range = expr(c->range);
      c->body = stmt(c->body);
      return c;
    }
    case NodeKind::ForConstexprStmt:
    {
      auto* c = raw(static_cast<const ForConstexprStmt*>(s));
      c->var = decl(c->var);
      c->list = node(c->list);
      c->body = stmt(c->body);
      return c;
    }
    case NodeKind::WhileStmt:
    {
      auto* c = raw(static_cast<const WhileStmt*>(s));
      c->cond = node(c->cond);
      c->body = stmt(c->body);
      return c;
    }
    case NodeKind::DoStmt:
    {
      auto* c = raw(static_cast<const DoStmt*>(s));
      c->body = stmt(c->body);
      c->cond = expr(c->cond);
      return c;
    }
    case NodeKind::SwitchStmt:
    {
      auto* c = raw(static_cast<const SwitchStmt*>(s));
      c->init = stmt(c->init);
      c->cond = node(c->cond);
      c->body = stmt(c->body);
      return c;
    }
    case NodeKind::CaseStmt:
    {
      auto* c = raw(static_cast<const CaseStmt*>(s));
      c->value = expr(c->value);
      c->rangeEnd = expr(c->rangeEnd);
      return c;
    }
    case NodeKind::ReturnStmt:
    {
      auto* c = raw(static_cast<const ReturnStmt*>(s));
      c->value = expr(c->value);
      return c;
    }
    case NodeKind::TryStmt:
    {
      auto* c = raw(static_cast<const TryStmt*>(s));
      c->body = stmt(c->body);
      c->handlers = span(c->handlers, [&](TryStmt::Handler& h)
      {
        h.param = decl(h.param);
        h.body = stmt(h.body);
      });
      return c;
    }
    case NodeKind::DefaultStmt: return raw(static_cast<const DefaultStmt*>(s));
    case NodeKind::BreakStmt: return raw(static_cast<const BreakStmt*>(s));
    case NodeKind::ContinueStmt: return raw(static_cast<const ContinueStmt*>(s));
    case NodeKind::FallthroughStmt: return raw(static_cast<const FallthroughStmt*>(s));
    case NodeKind::GotoStmt: return raw(static_cast<const GotoStmt*>(s));
    case NodeKind::LabelStmt: return raw(static_cast<const LabelStmt*>(s));
    case NodeKind::EmptyStmt: return raw(static_cast<const EmptyStmt*>(s));
    case NodeKind::AsmStmt: return raw(static_cast<const AsmStmt*>(s));
    case NodeKind::ErrorStmt: return raw(static_cast<const ErrorStmt*>(s));
    case NodeKind::MacroStmt: return raw(static_cast<const MacroStmt*>(s));
    default: return const_cast<Stmt*>(s);
  }
}

Expr* InstanceCopier::expr(const Expr* e)
{
  if (!e)
    return nullptr;
  Expr* out = nullptr;
  switch (e->kind)
  {
    case NodeKind::NameExpr:
    {
      auto* c = raw(static_cast<const NameExpr*>(e));
      c->name = name(c->name);
      out = c;
      break;
    }
    case NodeKind::LiteralExpr: out = raw(static_cast<const LiteralExpr*>(e)); break;
    case NodeKind::ThisExpr: out = raw(static_cast<const ThisExpr*>(e)); break;
    case NodeKind::CallerExpr: out = raw(static_cast<const CallerExpr*>(e)); break;
    case NodeKind::ErrorExpr: out = raw(static_cast<const ErrorExpr*>(e)); break;
    case NodeKind::CallExpr:
    {
      auto* c = raw(static_cast<const CallExpr*>(e));
      c->callee = expr(c->callee);
      c->args = exprs(c->args);
      c->templateArgCount = 0;
      out = c;
      break;
    }
    case NodeKind::IndexExpr:
    {
      auto* c = raw(static_cast<const IndexExpr*>(e));
      c->base = expr(c->base);
      c->args = exprs(c->args);
      out = c;
      break;
    }
    case NodeKind::MemberExpr:
    {
      auto* c = raw(static_cast<const MemberExpr*>(e));
      c->base = expr(c->base);
      c->member = name(c->member);
      c->dynamicMember = expr(c->dynamicMember);
      out = c;
      break;
    }
    case NodeKind::UnaryExpr:
    {
      auto* c = raw(static_cast<const UnaryExpr*>(e));
      c->operand = expr(c->operand);
      out = c;
      break;
    }
    case NodeKind::BinaryExpr:
    {
      auto* c = raw(static_cast<const BinaryExpr*>(e));
      c->lhs = expr(c->lhs);
      c->rhs = expr(c->rhs);
      out = c;
      break;
    }
    case NodeKind::ConditionalExpr:
    {
      auto* c = raw(static_cast<const ConditionalExpr*>(e));
      c->cond = expr(c->cond);
      c->then = expr(c->then);
      c->else_ = expr(c->else_);
      out = c;
      break;
    }
    case NodeKind::CastExpr:
    {
      auto* c = raw(static_cast<const CastExpr*>(e));
      c->type = type(c->type);
      c->operand = expr(c->operand);
      out = c;
      break;
    }
    case NodeKind::NamedCastExpr:
    {
      auto* c = raw(static_cast<const NamedCastExpr*>(e));
      c->type = type(c->type);
      c->operand = expr(c->operand);
      out = c;
      break;
    }
    case NodeKind::SizeofExpr:
    {
      auto* c = raw(static_cast<const SizeofExpr*>(e));
      c->type = type(c->type);
      c->operand = expr(c->operand);
      out = c;
      break;
    }
    case NodeKind::NameofExpr:
    {
      auto* c = raw(static_cast<const NameofExpr*>(e));
      c->operand = expr(c->operand);
      out = c;
      break;
    }
    case NodeKind::NewExpr:
    {
      auto* c = raw(static_cast<const NewExpr*>(e));
      c->type = type(c->type);
      c->placement = exprs(c->placement);
      c->args = exprs(c->args);
      out = c;
      break;
    }
    case NodeKind::DeleteExpr:
    {
      auto* c = raw(static_cast<const DeleteExpr*>(e));
      c->operand = expr(c->operand);
      out = c;
      break;
    }
    case NodeKind::LambdaExpr:
    {
      // The closure class is part of the lambda: one closure type per instantiation.
      auto* c = raw(static_cast<const LambdaExpr*>(e));
      c->templateParams = declList(c->templateParams);
      c->params = declList(c->params);
      c->returnType = type(c->returnType);
      c->captures = span(c->captures, [&](LambdaCapture& cap)
      {
        Expr* init = cap.init;
        Decl* var = cap.decl;
        cap.decl = decl(var);
        // The parser gives an init-capture's variable the capture's own expression: one node, one copy.
        if (var && var->kind == NodeKind::VariableDecl && static_cast<VariableDecl*>(var)->init == init)
          cap.init = static_cast<VariableDecl*>(cap.decl)->init;
        else
          cap.init = expr(init);
      });
      c->body = stmt(c->body);
      if (!keepClosures)
      {
        c->closure = decl(c->closure);
        if (c->closure && c->closure->kind == NodeKind::ClassDecl)
          static_cast<ClassDecl*>(c->closure)->lambda = c;
      }
      out = c;
      break;
    }
    case NodeKind::BraceInitExpr:
    {
      auto* c = raw(static_cast<const BraceInitExpr*>(e));
      c->args = exprs(c->args);
      out = c;
      break;
    }
    case NodeKind::ThrowExpr:
    {
      auto* c = raw(static_cast<const ThrowExpr*>(e));
      c->operand = expr(c->operand);
      out = c;
      break;
    }
    case NodeKind::FunctionalCastExpr:
    {
      auto* c = raw(static_cast<const FunctionalCastExpr*>(e));
      c->type = type(c->type);
      c->args = exprs(c->args);
      out = c;
      break;
    }
    case NodeKind::TypeidExpr:
    {
      auto* c = raw(static_cast<const TypeidExpr*>(e));
      c->operand = node(c->operand);
      out = c;
      break;
    }
    case NodeKind::FoldExpr:
    {
      auto* c = raw(static_cast<const FoldExpr*>(e));
      c->lhs = expr(c->lhs);
      c->rhs = expr(c->rhs);
      out = c;
      break;
    }
    case NodeKind::RequiresExpr:
    {
      auto* c = raw(static_cast<const RequiresExpr*>(e));
      c->params = declList(c->params);
      c->requirements = exprs(c->requirements);
      c->typeRequirements = span(c->typeRequirements, [&](Type*& t) { t = type(t); });
      out = c;
      break;
    }
    case NodeKind::ParenExpr:
    {
      auto* c = raw(static_cast<const ParenExpr*>(e));
      c->inner = expr(c->inner);
      out = c;
      break;
    }
    default: return const_cast<Expr*>(e);
  }
  out->ty = nullptr; // typed again, for this instantiation
  return out;
}

void InstanceCopier::fix(Decl*& d) const
{
  if (d)
    if (auto it = decls.find(d); it != decls.end())
      d = it->second;
}

LambdaExpr* InstanceCopier::lambdaInstance(const LambdaExpr* l)
{
  for (const LambdaCapture& cap : l->captures)
    if (cap.decl)
      decls[cap.decl] = cap.decl; // kept: the closure holds these
  if (l->closure)
    decls[l->closure] = l->closure;
  auto* c = raw(l);
  c->params = declList(c->params);
  c->returnType = type(c->returnType);
  c->body = stmt(c->body);
  fixReferences(c);
  return c;
}

FunctionDecl* InstanceCopier::function(const FunctionDecl* f)
{
  root = f;
  auto* copy = static_cast<FunctionDecl*>(decl(f));
  copy->cachedType.store(nullptr, std::memory_order_relaxed);
  copy->parent = f->parent;
  fixReferences(copy);
  return copy;
}

// Points the references inside the copy at the copies of the declarations they name: what a
// resolved name, member or type refers to, a declaration's parent, a closure's lambda.
void InstanceCopier::fixReferences(Node* root)
{
  std::vector<Node*> todo{root};
  while (!todo.empty())
  {
    Node* n = todo.back();
    todo.pop_back();
    if (!n)
      continue;
    auto push = [&](Node* c) { if (c) todo.push_back(c); };
    auto pushName = [&](const QualifiedName& q)
    {
      for (const NamePart& p : q.parts)
      {
        for (Node* a : p.templateArgs)
          push(a);
        push(p.convType);
      }
    };
    switch (n->kind)
    {
      case NodeKind::NamedType: fix(static_cast<NamedType*>(n)->resolved); pushName(static_cast<NamedType*>(n)->name); break;
      case NodeKind::PointerType: push(static_cast<PointerType*>(n)->pointee); break;
      case NodeKind::LRefType: push(static_cast<LRefType*>(n)->referenced); break;
      case NodeKind::RRefType: push(static_cast<RRefType*>(n)->referenced); break;
      case NodeKind::ConstType: push(static_cast<ConstType*>(n)->inner); break;
      case NodeKind::ArrayType: push(static_cast<ArrayType*>(n)->element); push(static_cast<ArrayType*>(n)->size); break;
      case NodeKind::FunctionType:
        push(static_cast<FunctionType*>(n)->result);
        for (Decl* p : static_cast<FunctionType*>(n)->params)
          push(p);
        break;
      case NodeKind::DecltypeType: push(static_cast<DecltypeType*>(n)->expr); break;
      case NodeKind::MemberPointerType: push(static_cast<MemberPointerType*>(n)->classType); push(static_cast<MemberPointerType*>(n)->pointee); break;
      case NodeKind::PackType: push(static_cast<PackType*>(n)->pattern); break;
      case NodeKind::ParamDecl: case NodeKind::VariableDecl: case NodeKind::TemplateParamDecl: case NodeKind::ClassDecl: case NodeKind::EnumDecl:
      case NodeKind::EnumeratorDecl: case NodeKind::FunctionDecl: case NodeKind::UsingAliasDecl: case NodeKind::StaticAssertDecl: case NodeKind::FriendDecl:
      {
        auto* d = static_cast<Decl*>(n);
        fix(d->parent);
        for (Decl* tp : d->templateParams)
          push(tp);
        switch (n->kind)
        {
          case NodeKind::ParamDecl: push(static_cast<ParamDecl*>(n)->type); push(static_cast<ParamDecl*>(n)->defaultValue); break;
          case NodeKind::VariableDecl:
          {
            auto* v = static_cast<VariableDecl*>(n);
            push(v->type);
            push(v->init);
            push(v->bitfield);
            for (Expr* b : v->structuredBindings)
              push(b);
            break;
          }
          case NodeKind::TemplateParamDecl:
            push(static_cast<TemplateParamDecl*>(n)->valueType);
            push(static_cast<TemplateParamDecl*>(n)->defaultArg);
            for (Decl* p : static_cast<TemplateParamDecl*>(n)->innerParams)
              push(p);
            break;
          case NodeKind::ClassDecl:
          {
            auto* c = static_cast<ClassDecl*>(n);
            for (const ClassDecl::Base& b : c->bases)
              push(b.type);
            for (Decl* m : c->members)
              push(m);
            push(c->alignment);
            fix(c->tagEnum);
            fix(c->tagField);
            for (Decl*& a : c->alternatives)
              fix(a);
            break;
          }
          case NodeKind::EnumDecl:
            push(static_cast<EnumDecl*>(n)->underlying);
            for (Decl* x : static_cast<EnumDecl*>(n)->enumerators)
              push(x);
            for (Decl* x : static_cast<EnumDecl*>(n)->members)
              push(x);
            break;
          case NodeKind::EnumeratorDecl: push(static_cast<EnumeratorDecl*>(n)->value); break;
          case NodeKind::FunctionDecl:
          {
            auto* f = static_cast<FunctionDecl*>(n);
            push(f->returnType);
            for (Decl* p : f->params)
              push(p);
            push(f->trailingReturn);
            push(f->body);
            for (const FunctionDecl::CtorInit& init : f->ctorInits)
            {
              pushName(init.member);
              for (Expr* a : init.args)
                push(a);
            }
            push(f->requiresClause);
            push(f->condition);
            push(f->explicitCondition);
            break;
          }
          case NodeKind::UsingAliasDecl: push(static_cast<UsingAliasDecl*>(n)->type); break;
          case NodeKind::StaticAssertDecl: push(static_cast<StaticAssertDecl*>(n)->cond); break;
          case NodeKind::FriendDecl: push(static_cast<FriendDecl*>(n)->type); fix(static_cast<FriendDecl*>(n)->decl); break;
          default: break;
        }
        break;
      }
      case NodeKind::BlockStmt:
        for (Stmt* s : static_cast<BlockStmt*>(n)->stmts)
          push(s);
        break;
      case NodeKind::ExprStmt: push(static_cast<ExprStmt*>(n)->expr); break;
      case NodeKind::DeclStmt:
        for (Decl* d : static_cast<DeclStmt*>(n)->decls)
          push(d);
        break;
      case NodeKind::IfStmt:
      {
        auto* s = static_cast<IfStmt*>(n);
        push(s->init);
        push(s->cond);
        push(s->then);
        push(s->else_);
        break;
      }
      case NodeKind::AssertStmt: push(static_cast<AssertStmt*>(n)->cond); break;
      case NodeKind::ForStmt:
      {
        auto* s = static_cast<ForStmt*>(n);
        push(s->init);
        push(s->cond);
        push(s->step);
        push(s->body);
        break;
      }
      case NodeKind::RangeForStmt: push(static_cast<RangeForStmt*>(n)->var); push(static_cast<RangeForStmt*>(n)->range); push(static_cast<RangeForStmt*>(n)->body); break;
      case NodeKind::ForConstexprStmt: push(static_cast<ForConstexprStmt*>(n)->var); push(static_cast<ForConstexprStmt*>(n)->list); push(static_cast<ForConstexprStmt*>(n)->body); break;
      case NodeKind::WhileStmt: push(static_cast<WhileStmt*>(n)->cond); push(static_cast<WhileStmt*>(n)->body); break;
      case NodeKind::DoStmt: push(static_cast<DoStmt*>(n)->body); push(static_cast<DoStmt*>(n)->cond); break;
      case NodeKind::SwitchStmt: push(static_cast<SwitchStmt*>(n)->init); push(static_cast<SwitchStmt*>(n)->cond); push(static_cast<SwitchStmt*>(n)->body); break;
      case NodeKind::CaseStmt: push(static_cast<CaseStmt*>(n)->value); push(static_cast<CaseStmt*>(n)->rangeEnd); break;
      case NodeKind::ReturnStmt: push(static_cast<ReturnStmt*>(n)->value); break;
      case NodeKind::TryStmt:
        push(static_cast<TryStmt*>(n)->body);
        for (const TryStmt::Handler& h : static_cast<TryStmt*>(n)->handlers)
        {
          push(h.param);
          push(h.body);
        }
        break;
      case NodeKind::NameExpr: fix(static_cast<NameExpr*>(n)->resolved); pushName(static_cast<NameExpr*>(n)->name); break;
      case NodeKind::CallExpr:
        push(static_cast<CallExpr*>(n)->callee);
        for (Expr* a : static_cast<CallExpr*>(n)->args)
          push(a);
        break;
      case NodeKind::IndexExpr:
        push(static_cast<IndexExpr*>(n)->base);
        for (Expr* a : static_cast<IndexExpr*>(n)->args)
          push(a);
        break;
      case NodeKind::MemberExpr:
      {
        auto* m = static_cast<MemberExpr*>(n);
        fix(m->resolved);
        push(m->base);
        pushName(m->member);
        push(m->dynamicMember);
        break;
      }
      case NodeKind::UnaryExpr: push(static_cast<UnaryExpr*>(n)->operand); break;
      case NodeKind::BinaryExpr: push(static_cast<BinaryExpr*>(n)->lhs); push(static_cast<BinaryExpr*>(n)->rhs); break;
      case NodeKind::ConditionalExpr: push(static_cast<ConditionalExpr*>(n)->cond); push(static_cast<ConditionalExpr*>(n)->then); push(static_cast<ConditionalExpr*>(n)->else_); break;
      case NodeKind::CastExpr: push(static_cast<CastExpr*>(n)->type); push(static_cast<CastExpr*>(n)->operand); break;
      case NodeKind::NamedCastExpr: push(static_cast<NamedCastExpr*>(n)->type); push(static_cast<NamedCastExpr*>(n)->operand); break;
      case NodeKind::SizeofExpr: push(static_cast<SizeofExpr*>(n)->type); push(static_cast<SizeofExpr*>(n)->operand); break;
      case NodeKind::NameofExpr: push(static_cast<NameofExpr*>(n)->operand); break;
      case NodeKind::NewExpr:
        push(static_cast<NewExpr*>(n)->type);
        for (Expr* a : static_cast<NewExpr*>(n)->placement)
          push(a);
        for (Expr* a : static_cast<NewExpr*>(n)->args)
          push(a);
        break;
      case NodeKind::DeleteExpr: push(static_cast<DeleteExpr*>(n)->operand); break;
      case NodeKind::LambdaExpr:
      {
        auto* l = static_cast<LambdaExpr*>(n);
        for (Decl* p : l->templateParams)
          push(p);
        for (Decl* p : l->params)
          push(p);
        push(l->returnType);
        for (LambdaCapture& cap : l->captures)
        {
          push(cap.init);
          push(cap.decl);
        }
        push(l->body);
        push(l->closure);
        break;
      }
      case NodeKind::BraceInitExpr:
        for (Expr* a : static_cast<BraceInitExpr*>(n)->args)
          push(a);
        break;
      case NodeKind::ThrowExpr: push(static_cast<ThrowExpr*>(n)->operand); break;
      case NodeKind::FunctionalCastExpr:
        push(static_cast<FunctionalCastExpr*>(n)->type);
        for (Expr* a : static_cast<FunctionalCastExpr*>(n)->args)
          push(a);
        break;
      case NodeKind::TypeidExpr: fix(static_cast<TypeidExpr*>(n)->operandDecl); push(static_cast<TypeidExpr*>(n)->operand); break;
      case NodeKind::FoldExpr: push(static_cast<FoldExpr*>(n)->lhs); push(static_cast<FoldExpr*>(n)->rhs); break;
      case NodeKind::RequiresExpr:
        for (Decl* p : static_cast<RequiresExpr*>(n)->params)
          push(p);
        for (Expr* r : static_cast<RequiresExpr*>(n)->requirements)
          push(r);
        for (Type* t : static_cast<RequiresExpr*>(n)->typeRequirements)
          push(t);
        break;
      case NodeKind::ParenExpr: push(static_cast<ParenExpr*>(n)->inner); break;
      default: break;
    }
  }
}
