#pragma once
// The syntax tree's statement nodes (Ast.hpp includes it, after Node.hpp).
#include "Node.hpp"

// --- Statements ---

struct Stmt : Node {};
struct BlockStmt : Stmt
{
  Span<Stmt*> stmts;
  uint32_t endToken = 0; // Its closing brace, where its objects are destroyed; 0 in a block the parser made up.
};
struct ExprStmt : Stmt { Expr* expr; };
struct DeclStmt : Stmt { Span<Decl*> decls; };
struct IfStmt : Stmt
{
  Stmt* init = nullptr; Node* cond; Stmt* then; Stmt* else_ = nullptr; bool isConstexpr = false; int8_t constexprKnown = -1; /* -1 = not evaluated, 0 = false branch, 1 = true branch */
  // `if consteval` (1) and `if !consteval` (2), C++23, no condition: the first branch runs in a
  // compile-time evaluation (the second for `!`), the other one in the program, the only one emitted.
  uint8_t ifConsteval = 0;
};
struct AssertStmt : Stmt { Expr* cond; uint32_t startToken, endToken; bool release = false; uint8_t testKind = 0; bool checksCall = false; }; // tokens of the expression text // assert(cond); release_assert(cond); testKind 1 check(cond), 2 require(cond) in a test
struct ForStmt : Stmt { Stmt* init = nullptr; Node* cond = nullptr; Expr* step = nullptr; Stmt* body; };
// begin, end: the functions the loop calls on a class range, as the checker chose them (design.md 24),
// members of its class (or a base) when rangeMembers, else free functions taking the range; null when
// the walk decided none (an array, a dependent range, no such function).
struct RangeForStmt : Stmt { Decl* var; Expr* range; Stmt* body; FunctionDecl* begin = nullptr; FunctionDecl* end = nullptr; bool rangeMembers = false; };
// `for constexpr (typename T : types<A, B>) body`, `for constexpr (int i : {1, 2, 4}) body`: var is a
// TemplateParamDecl (a type, or a value with valueType), list a `types<...>` NamedType, a brace list of
// constants or an expression naming a list; the body is instantiated once per element (design.md 8.2).
struct ForConstexprStmt : Stmt { Decl* var; Node* list; Stmt* body; };
struct WhileStmt : Stmt { Node* cond; Stmt* body; };
struct DoStmt : Stmt { Stmt* body; Expr* cond; };
struct SwitchStmt : Stmt { Stmt* init = nullptr; Node* cond; Stmt* body; };
struct CaseStmt : Stmt { Expr* value; Expr* rangeEnd = nullptr; };
struct DefaultStmt : Stmt {};
struct ReturnStmt : Stmt { Expr* value = nullptr; };
struct BreakStmt : Stmt {};
struct ContinueStmt : Stmt {};
struct FallthroughStmt : Stmt {};
struct GotoStmt : Stmt { uint32_t label; };
struct LabelStmt : Stmt { uint32_t label; };
// constructorTry: a constructor's function-try-block, whose try also holds the member initializers
// (codegen's tryBody) and whose handlers end in a rethrow (the parser's).
struct TryStmt : Stmt { struct Handler { Decl* param; Stmt* body; }; Stmt* body; Span<Handler> handlers; bool constructorTry = false; };
struct EmptyStmt : Stmt {};
struct AsmStmt : Stmt {};
struct ErrorStmt : Stmt { uint32_t endToken; };
struct MacroStmt : Stmt { uint32_t name; };
