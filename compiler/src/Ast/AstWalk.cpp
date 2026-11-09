#include "AstWalk.hpp"
#include <unordered_set>
#include <vector>

namespace
{
  struct Walk
  {
    bool first(Node* n) { return n && seen.insert(n).second; }

    void name(const QualifiedName& q)
    {
      for (const NamePart& part : q.parts)
      {
        for (Node* a : part.templateArgs)
          node(a);
        type(part.convType);
      }
      type(q.typeQualifier);
    }

    void decls(Span<Decl*> list)
    {
      for (Decl* d : list)
        decl(d);
    }

    void exprs(Span<Expr*> list)
    {
      for (Expr* e : list)
        expr(e);
    }

    void node(Node* n)
    {
      if (!n)
        return;
      if (n->kind <= NodeKind::PackType)
        type(static_cast<Type*>(n));
      else if (n->kind <= NodeKind::ImportDecl)
        decl(static_cast<Decl*>(n));
      else if (n->kind <= NodeKind::MacroStmt)
        stmt(static_cast<Stmt*>(n));
      else
        expr(static_cast<Expr*>(n));
    }

    void type(Type* t)
    {
      if (!first(t))
        return;
      each(t);
      switch (t->kind)
      {
        case NodeKind::NamedType: name(static_cast<NamedType*>(t)->name); break;
        case NodeKind::PointerType: type(static_cast<PointerType*>(t)->pointee); break;
        case NodeKind::LRefType: type(static_cast<LRefType*>(t)->referenced); break;
        case NodeKind::RRefType: type(static_cast<RRefType*>(t)->referenced); break;
        case NodeKind::ConstType: type(static_cast<ConstType*>(t)->inner); break;
        case NodeKind::ArrayType:
          type(static_cast<ArrayType*>(t)->element);
          expr(static_cast<ArrayType*>(t)->size);
          expr(static_cast<ArrayType*>(t)->asIndex);
          break;
        case NodeKind::FunctionType:
          type(static_cast<FunctionType*>(t)->result);
          decls(static_cast<FunctionType*>(t)->params);
          expr(static_cast<FunctionType*>(t)->asCall);
          break;
        case NodeKind::DecltypeType: expr(static_cast<DecltypeType*>(t)->expr); break;
        case NodeKind::MemberPointerType:
          type(static_cast<MemberPointerType*>(t)->classType);
          type(static_cast<MemberPointerType*>(t)->pointee);
          break;
        case NodeKind::PackType: type(static_cast<PackType*>(t)->pattern); break;
        default: break;
      }
    }

    void decl(Decl* d)
    {
      if (!first(d))
        return;
      each(d);
      decls(d->templateParams);
      expr(d->when);
      switch (d->kind)
      {
        case NodeKind::NamespaceDecl:
          name(static_cast<NamespaceDecl*>(d)->nested);
          decls(static_cast<NamespaceDecl*>(d)->decls);
          break;
        case NodeKind::ParamDecl:
          type(static_cast<ParamDecl*>(d)->type);
          expr(static_cast<ParamDecl*>(d)->defaultValue);
          break;
        case NodeKind::VariableDecl:
        {
          auto* v = static_cast<VariableDecl*>(d);
          type(v->type);
          name(v->name);
          expr(v->init);
          expr(v->bitfield);
          exprs(v->structuredBindings);
          break;
        }
        case NodeKind::TemplateParamDecl:
          type(static_cast<TemplateParamDecl*>(d)->valueType);
          node(static_cast<TemplateParamDecl*>(d)->defaultArg);
          decls(static_cast<TemplateParamDecl*>(d)->innerParams);
          break;
        case NodeKind::ClassDecl:
        {
          auto* c = static_cast<ClassDecl*>(d);
          name(c->name);
          for (const ClassDecl::Base& b : c->bases)
            type(b.type);
          decls(c->members);
          expr(c->alignment);
          break;
        }
        case NodeKind::EnumDecl:
        {
          auto* e = static_cast<EnumDecl*>(d);
          name(e->name);
          type(e->underlying);
          decls(e->enumerators);
          decls(e->members);
          break;
        }
        case NodeKind::EnumeratorDecl: expr(static_cast<EnumeratorDecl*>(d)->value); break;
        case NodeKind::FunctionDecl:
        {
          auto* f = static_cast<FunctionDecl*>(d);
          type(f->returnType);
          name(f->name);
          decls(f->params);
          type(f->trailingReturn);
          stmt(f->body);
          for (const FunctionDecl::CtorInit& init : f->ctorInits)
          {
            name(init.member);
            exprs(init.args);
          }
          expr(f->requiresClause);
          expr(f->condition);
          expr(f->explicitCondition);
          expr(f->noexceptCondition);
          break;
        }
        case NodeKind::UsingAliasDecl: type(static_cast<UsingAliasDecl*>(d)->type); break;
        case NodeKind::UsingDirectiveDecl: name(static_cast<UsingDirectiveDecl*>(d)->name); break;
        case NodeKind::UsingNameDecl: name(static_cast<UsingNameDecl*>(d)->name); break;
        case NodeKind::StaticAssertDecl:
          expr(static_cast<StaticAssertDecl*>(d)->cond);
          expr(static_cast<StaticAssertDecl*>(d)->message);
          break;
        case NodeKind::FriendDecl:
          type(static_cast<FriendDecl*>(d)->type);
          decl(static_cast<FriendDecl*>(d)->decl);
          break;
        default: break;
      }
    }

    void stmt(Stmt* s)
    {
      if (!first(s))
        return;
      each(s);
      switch (s->kind)
      {
        case NodeKind::BlockStmt:
          for (Stmt* x : static_cast<BlockStmt*>(s)->stmts)
            stmt(x);
          break;
        case NodeKind::ExprStmt: expr(static_cast<ExprStmt*>(s)->expr); break;
        case NodeKind::DeclStmt: decls(static_cast<DeclStmt*>(s)->decls); break;
        case NodeKind::IfStmt:
        {
          auto* c = static_cast<IfStmt*>(s);
          stmt(c->init);
          node(c->cond);
          stmt(c->then);
          stmt(c->else_);
          break;
        }
        case NodeKind::AssertStmt: expr(static_cast<AssertStmt*>(s)->cond); break;
        case NodeKind::ForStmt:
        {
          auto* c = static_cast<ForStmt*>(s);
          stmt(c->init);
          node(c->cond);
          expr(c->step);
          stmt(c->body);
          break;
        }
        case NodeKind::RangeForStmt:
        {
          auto* c = static_cast<RangeForStmt*>(s);
          decl(c->var);
          expr(c->range);
          stmt(c->body);
          break;
        }
        case NodeKind::ForConstexprStmt:
        {
          auto* c = static_cast<ForConstexprStmt*>(s);
          decl(c->var);
          node(c->list);
          stmt(c->body);
          break;
        }
        case NodeKind::WhileStmt:
          node(static_cast<WhileStmt*>(s)->cond);
          stmt(static_cast<WhileStmt*>(s)->body);
          break;
        case NodeKind::DoStmt:
          stmt(static_cast<DoStmt*>(s)->body);
          expr(static_cast<DoStmt*>(s)->cond);
          break;
        case NodeKind::SwitchStmt:
        {
          auto* c = static_cast<SwitchStmt*>(s);
          stmt(c->init);
          node(c->cond);
          stmt(c->body);
          break;
        }
        case NodeKind::CaseStmt:
          expr(static_cast<CaseStmt*>(s)->value);
          expr(static_cast<CaseStmt*>(s)->rangeEnd);
          for (Expr* v : static_cast<CaseStmt*>(s)->moreValues)
            expr(v);
          break;
        case NodeKind::ReturnStmt: expr(static_cast<ReturnStmt*>(s)->value); break;
        case NodeKind::TryStmt:
        {
          auto* c = static_cast<TryStmt*>(s);
          stmt(c->body);
          for (const TryStmt::Handler& h : c->handlers)
          {
            decl(h.param);
            stmt(h.body);
          }
          break;
        }
        default: break;
      }
    }

    void expr(Expr* e)
    {
      if (!first(e))
        return;
      each(e);
      switch (e->kind)
      {
        case NodeKind::NameExpr: name(static_cast<NameExpr*>(e)->name); break;
        case NodeKind::CallExpr:
          expr(static_cast<CallExpr*>(e)->callee);
          exprs(static_cast<CallExpr*>(e)->args);
          break;
        case NodeKind::IndexExpr:
          expr(static_cast<IndexExpr*>(e)->base);
          exprs(static_cast<IndexExpr*>(e)->args);
          break;
        case NodeKind::MemberExpr:
        {
          auto* c = static_cast<MemberExpr*>(e);
          expr(c->base);
          name(c->member);
          expr(c->dynamicMember);
          break;
        }
        case NodeKind::UnaryExpr: expr(static_cast<UnaryExpr*>(e)->operand); break;
        case NodeKind::BinaryExpr:
          expr(static_cast<BinaryExpr*>(e)->lhs);
          expr(static_cast<BinaryExpr*>(e)->rhs);
          break;
        case NodeKind::ConditionalExpr:
        {
          auto* c = static_cast<ConditionalExpr*>(e);
          expr(c->cond);
          expr(c->then);
          expr(c->else_);
          break;
        }
        case NodeKind::CastExpr:
          type(static_cast<CastExpr*>(e)->type);
          expr(static_cast<CastExpr*>(e)->operand);
          break;
        case NodeKind::NamedCastExpr:
          type(static_cast<NamedCastExpr*>(e)->type);
          expr(static_cast<NamedCastExpr*>(e)->operand);
          break;
        case NodeKind::SizeofExpr:
          type(static_cast<SizeofExpr*>(e)->type);
          expr(static_cast<SizeofExpr*>(e)->operand);
          break;
        case NodeKind::NameofExpr: expr(static_cast<NameofExpr*>(e)->operand); break;
        case NodeKind::NoexceptExpr: expr(static_cast<NoexceptExpr*>(e)->operand); break;
        case NodeKind::NewExpr:
        {
          auto* c = static_cast<NewExpr*>(e);
          type(c->type);
          exprs(c->placement);
          exprs(c->args);
          break;
        }
        case NodeKind::DeleteExpr: expr(static_cast<DeleteExpr*>(e)->operand); break;
        case NodeKind::LambdaExpr:
        {
          auto* c = static_cast<LambdaExpr*>(e);
          decls(c->templateParams);
          decls(c->params);
          type(c->returnType);
          for (const LambdaCapture& cap : c->captures)
          {
            decl(cap.decl);
            expr(cap.init);
          }
          stmt(c->body);
          decl(c->closure);
          break;
        }
        case NodeKind::BraceInitExpr: exprs(static_cast<BraceInitExpr*>(e)->args); break;
        case NodeKind::ThrowExpr: expr(static_cast<ThrowExpr*>(e)->operand); break;
        case NodeKind::FunctionalCastExpr:
          type(static_cast<FunctionalCastExpr*>(e)->type);
          exprs(static_cast<FunctionalCastExpr*>(e)->args);
          break;
        case NodeKind::TypeidExpr: node(static_cast<TypeidExpr*>(e)->operand); break;
        case NodeKind::FoldExpr:
          expr(static_cast<FoldExpr*>(e)->lhs);
          expr(static_cast<FoldExpr*>(e)->rhs);
          break;
        case NodeKind::RequiresExpr:
        {
          auto* c = static_cast<RequiresExpr*>(e);
          decls(c->params);
          exprs(c->requirements);
          for (Type* t : c->typeRequirements)
            type(t);
          for (Type* t : c->resultConstraints)
            if (t)
              type(t);
          exprs(c->nested);
          break;
        }
        case NodeKind::ParenExpr: expr(static_cast<ParenExpr*>(e)->inner); break;
        default: break;
      }
    }

    const std::function<void(Node*)>& each;
    std::unordered_set<const Node*> seen; // a node reached twice (an init-capture's expression) is visited once
  };
}

void forEachNode(Span<Decl*> decls, const std::function<void(Node*)>& each)
{
  Walk w{each, {}};
  w.decls(decls);
}

void forEachNode(Node* root, const std::function<void(Node*)>& each)
{
  Walk w{each, {}};
  w.node(root);
}

void markRequiredThis(Span<Decl*> decls)
{
  std::vector<Node*> declaredTypes;
  forEachNode(decls, [&](Node* n)
  {
    switch (n->kind)
    {
      case NodeKind::NameExpr:
      {
        QualifiedName& name = static_cast<NameExpr*>(n)->name;
        name.requiresThis = name.parts.size() == 1 && !name.global && !name.typeQualifier;
        break;
      }
      case NodeKind::ParamDecl:
        declaredTypes.push_back(static_cast<ParamDecl*>(n)->type);
        break;
      case NodeKind::FunctionDecl:
        declaredTypes.push_back(static_cast<FunctionDecl*>(n)->returnType); // (a trailing one is code: `this` is in scope there)
        break;
      case NodeKind::ClassDecl:
        for (Decl* member : static_cast<ClassDecl*>(n)->members)
          if (member->kind == NodeKind::VariableDecl)
            declaredTypes.push_back(static_cast<VariableDecl*>(member)->type);
        break;
      default:
        break;
    }
  });
  for (Node* type : declaredTypes)
    forEachNode(type, [](Node* n)
    {
      if (n->kind == NodeKind::NameExpr)
        static_cast<NameExpr*>(n)->name.requiresThis = false;
    });
}

void remapNodeTokens(Node* n, const std::function<uint32_t(uint32_t)>& remap)
{
  n->token = remap(n->token);
  if (n->kind > NodeKind::PackType && n->kind <= NodeKind::ImportDecl)
  {
    auto* d = static_cast<Decl*>(n);
    if (d->endToken)
      d->endToken = remap(d->endToken - 1) + 1; // one past the last
    if (n->kind == NodeKind::MacroDecl && static_cast<MacroDecl*>(n)->endToken)
      static_cast<MacroDecl*>(n)->endToken = remap(static_cast<MacroDecl*>(n)->endToken - 1) + 1;
    if (n->kind == NodeKind::ErrorDecl && static_cast<ErrorDecl*>(n)->endToken)
      static_cast<ErrorDecl*>(n)->endToken = remap(static_cast<ErrorDecl*>(n)->endToken - 1) + 1;
    if (n->kind == NodeKind::ImportDecl)
      static_cast<ImportDecl*>(n)->pathToken = remap(static_cast<ImportDecl*>(n)->pathToken);
  }
  else if (n->kind == NodeKind::AssertStmt)
  {
    auto* a = static_cast<AssertStmt*>(n);
    a->startToken = remap(a->startToken);
    if (a->endToken)
      a->endToken = remap(a->endToken - 1) + 1;
  }
  else if (n->kind == NodeKind::ErrorStmt && static_cast<ErrorStmt*>(n)->endToken)
    static_cast<ErrorStmt*>(n)->endToken = remap(static_cast<ErrorStmt*>(n)->endToken - 1) + 1;
}
