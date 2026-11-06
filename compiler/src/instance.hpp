#pragma once
// A copy of the nodes a function owns (its parameters, body, constructor initializers, default
// arguments, and the type and declaration nodes inside them) for one instantiation of it: the copy
// is typed in place and kept as that instantiation's body, while the template's own nodes stay as
// the parser and the generic pass left them. See docs/design.md, "Instantiations own their typed
// bodies".
#include "Ast/Ast.hpp"
#include <unordered_map>

class InstanceCopier
{
public:
  explicit InstanceCopier(Arena& arena) : arena(arena) {}

  // The function with its own parts copied: parameters, body, constructor initializers, the
  // requires clause and conditions. What it refers to outside itself (its class, the names it
  // calls, its template parameters) stays shared. The copy's parent is the original's.
  FunctionDecl* function(const FunctionDecl* f);

  // One expression on its own (a default member initializer, a static member's initializer), the
  // references inside it to its own declarations (a lambda's parameters) pointed at the copies.
  Expr* expression(const Expr* e)
  {
    Expr* copy = expr(e);
    fixReferences(copy);
    return copy;
  }

  // One type on its own (a requires-expression's type requirement, probed on its copy).
  Type* typeAlone(const Type* t)
  {
    Type* copy = type(t);
    fixReferences(copy);
    return copy;
  }

  // A generic lambda's parameters, return type and body for one instantiation of its call
  // operator. The captures and the closure stay the lambda's own, so that the copy's body names
  // the variables the closure was laid out with.
  LambdaExpr* lambdaInstance(const LambdaExpr* l);

  // The copy of a declaration copied so far (a parameter, a local), or the original when it was not
  // part of the copy.
  Decl* copyOf(Decl* d) const
  {
    auto it = decls.find(d);
    return it == decls.end() ? d : it->second;
  }

  size_t nodesCopied() const { return nodes; }
private:
  template<typename T>
  T* raw(const T* n);
  Node* node(const Node* n);
  Type* type(const Type* t);
  Expr* expr(const Expr* e);
  Stmt* stmt(const Stmt* s);
  Decl* decl(const Decl* d);
  QualifiedName name(const QualifiedName& q);
  template<typename T, typename F>
  Span<T> span(Span<T> s, F&& each);
  Span<Expr*> exprs(Span<Expr*> s);
  Span<Decl*> declList(Span<Decl*> s);
  void fix(Decl*& d) const;
  void fixReferences(Node* n);
public:
  // The copy's lambdas keep the closures (and generic lambdas their template parameters) of the
  // nodes copied: a private copy of an instantiation made for re-typing it in place, or the copy
  // of a function that is no template typed for its `auto` return; its lambdas are the same types
  // as the nodes copied.
  bool keepClosures = false;
private:
  Arena& arena;
  std::unordered_map<Decl*, Decl*> decls; // declaration inside the copied part -> its copy
  std::unordered_map<const ParenExpr*, ParenExpr*> holders; // ParenExpr::holds -> its copy, for the copied HeldExprs
  const Decl* root = nullptr;             // the function being copied
  size_t nodes = 0;
};
