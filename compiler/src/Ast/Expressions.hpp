#pragma once
// The syntax tree's expression nodes (Ast.hpp includes it, after Node.hpp).
#include "Node.hpp"

// --- Expressions ---

struct Expr : Node { const Ty* ty = nullptr; }; // ty: filled by the type checker.
struct NameExpr : Expr { QualifiedName name; Decl* resolved = nullptr; };
// keyword for true/false/nullptr; file: the one it was parsed from, whose tokens spell it (an
// expression may be evaluated in another file's context: a default argument, a template's)
struct LiteralExpr : Expr { TokenKind literalKind; Kw keyword; ParsedFile* file = nullptr; };
struct ThisExpr : Expr {};
struct CallExpr : Expr
{
  Expr* callee;
  Span<Expr*> args;
  // Deduced template arguments of the called function template, in parameter order (filled by
  // the type checker; at most 8, which is all code generation supports).
  const Ty* templateArgs[8] = {};
  uint8_t templateArgCount = 0;
};
struct IndexExpr : Expr { Expr* base; Span<Expr*> args; };
// dynamicMember: `object.[m]`, the member a reflection descriptor names (design.md 8.3); member is then empty.
struct MemberExpr : Expr { Expr* base; QualifiedName member; P op; Decl* resolved = nullptr; Expr* dynamicMember = nullptr; };
// What the checker decided an operator calls (design.md 24: decisions are made once), written by the
// walk that types the node, so per instance on an instance's own copy. Undecided: no walk decided it
// (a template's dependent operand, a node codegen made). Builtin: no operator function, the built-in
// operator (on the operands' scalar conversions for a class). Function: callee, a member of the left
// operand's class when calleeMember. Rewritten: a comparison C++ rewrites (`a != b` as `!(a == b)`,
// reversed operands, `<=>`), its function not recorded yet.
enum class OperatorCall : uint8_t { Undecided, Builtin, Function, Rewritten };
struct UnaryExpr : Expr { Expr* operand; uint32_t op; bool isKeyword; bool postfix; OperatorCall call = OperatorCall::Undecided; bool calleeMember = false; FunctionDecl* callee = nullptr; };
struct BinaryExpr : Expr { Expr* lhs; Expr* rhs; uint32_t op; bool isKeyword; OperatorCall call = OperatorCall::Undecided; bool calleeMember = false; FunctionDecl* callee = nullptr; };
struct ConditionalExpr : Expr { Expr* cond; Expr* then; Expr* else_; }; // then == null for Elvis.
struct CastExpr : Expr { Type* type; Expr* operand; };
struct NamedCastExpr : Expr { Kw castKind; Type* type; Expr* operand; };
struct SizeofExpr : Expr { Kw sizeofKind; Type* type = nullptr; Expr* operand = nullptr; bool isPack = false; };
struct CallerExpr : Expr {}; // `caller` as a default argument: the compiler fills in the call site.
struct NameofExpr : Expr { Expr* operand; }; // nameof(x): the last name of x as a string literal.
// noexcept(e), the operator: whether e cannot throw (its operand is not evaluated). value is the
// walk's answer (GlobalScope::canThrow): 1 true, 0 false, -1 not decided (a template's own walk).
struct NoexceptExpr : Expr { Expr* operand; int8_t value = -1; };
// parenthesized: `new T[n]()`, value-initialized (scalars zero), unlike `new T[n]`.
// allocator, deallocator: the class's own operator new the type checker chose (Walker::chooseAllocation),
// called with the size and the placement arguments, and the operator delete that gives the memory
// back when the constructor throws (null: the library's free, or nothing for a placement form
// without its operator delete); null allocator: malloc, or the placement at an address.
// sizedDeallocation: the deallocator takes the size after the pointer.
struct NewExpr : Expr
{
  Type* type;
  Span<Expr*> placement;
  Span<Expr*> args;
  bool braced = false;
  bool global = false;
  bool parenthesized = false;
  bool sizedDeallocation = false;
  FunctionDecl* allocator = nullptr;
  FunctionDecl* deallocator = nullptr;
};
// deallocator: the class's own operator delete the type checker chose (null: free).
struct DeleteExpr : Expr { Expr* operand; bool isArray; bool global; bool sizedDeallocation = false; FunctionDecl* deallocator = nullptr; };
struct LambdaCapture
{
  uint32_t symbol = UINT32_MAX; // Captured variable (UINT32_MAX for `this`).
  bool byRef = false;
  bool isThis = false;
  Expr* init = nullptr;         // Init-capture `[x = expr]`.
  Decl* decl = nullptr;         // Init-capture: the variable it declares (a VariableDecl of type auto).
  bool isPack = false;          // `[...w = v * 2]` (C++20): init, the pattern's expansion, is a pack of them.
};
struct LambdaExpr : Expr
{
  Span<Decl*> params;
  Stmt* body;
  Type* returnType = nullptr;
  Span<Decl*> templateParams;
  Span<LambdaCapture> captures;
  uint8_t defaultCapture = 0; // 0 none, 1 by copy (=), 2 by reference (&)
  Decl* closure = nullptr;    // Synthetic ClassDecl giving this lambda an identity and a name.
  bool isMutable = false;     // `mutable`: its captures by copy may change (else they are const in its body)
};
// designators: the member named per argument (`.x = 1`), UINT32_MAX for none; empty when no argument has one.
// text: the arguments a call gives a Text parameter (design.md 14), packed by the type checker, the Text built from them.
struct BraceInitExpr : Expr { Span<Expr*> args; Span<uint32_t> designators; bool text = false; };
struct ThrowExpr : Expr { Expr* operand; };
struct FunctionalCastExpr : Expr { Type* type; Span<Expr*> args; bool braced; };
// operandType and operandDecl are filled by the type checker: the type named, or the variable the
// parser read as a type name (`typeid(q)`).
struct TypeidExpr : Expr { Node* operand; const Ty* operandType = nullptr; Decl* operandDecl = nullptr; };
struct FoldExpr : Expr { Expr* lhs; Expr* rhs; uint32_t op; };
// requires(params) { requirements; }: true when every requirement is well-typed for the types at
// hand; the type checker sets satisfied where the expression is typed (again per instantiation).
// resultConstraints: one per requirement, the type-constraint of a compound one (`{ t.size() } ->
// Integral`), null for none. nested: the nested requirements (`requires sizeof(T) <= 2;`), each a
// constraint that must hold.
struct RequiresExpr : Expr
{
  Span<Decl*> params;
  Span<Expr*> requirements;
  Span<Type*> typeRequirements;
  Span<Type*> resultConstraints;
  Span<Expr*> nested;
  bool satisfied = false;
};
struct ParenExpr : Expr { Expr* inner; };
struct ErrorExpr : Expr {};
