#pragma once
// The syntax tree's base: Node, its kinds, qualified names and the arena the nodes live in (Ast.hpp includes the rest).
// AST for the kpp subset (tolerant of C++ so the Factorio corpus can be used as a benchmark).
// Nodes live in a per-file arena and are never freed individually.
#include "lexer.hpp"
#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "Arena.hpp"

template<typename T>
struct Span
{
  T* begin() const { return data; }
  T* end() const { return data + count; }
  size_t size() const { return count; }
  bool empty() const { return count == 0; }
  T& operator[](size_t i) const { return data[i]; }

  T* data = nullptr;
  uint32_t count = 0;
};

#define KPP_NODE_KINDS(X) \
  /* types */ \
  X(NamedType) X(PointerType) X(LRefType) X(RRefType) X(ConstType) X(ArrayType) X(FunctionType) \
  X(DecltypeType) X(AutoType) X(BuiltinType) X(MemberPointerType) X(PackType) \
  /* declarations */ \
  X(NamespaceDecl) X(ClassDecl) X(EnumDecl) X(EnumeratorDecl) X(FunctionDecl) X(ParamDecl) \
  X(VariableDecl) X(UsingAliasDecl) X(UsingDirectiveDecl) X(UsingNameDecl) X(TemplateParamDecl) \
  X(StaticAssertDecl) X(MacroDecl) X(ErrorDecl) X(EmptyDecl) X(AsmDecl) X(FriendDecl) X(DeductionGuideDecl) X(ImportDecl) \
  /* statements */ \
  X(BlockStmt) X(ExprStmt) X(DeclStmt) X(IfStmt) X(AssertStmt) X(ForStmt) X(RangeForStmt) X(ForConstexprStmt) X(WhileStmt) \
  X(DoStmt) X(SwitchStmt) X(CaseStmt) X(DefaultStmt) X(ReturnStmt) X(BreakStmt) X(ContinueStmt) \
  X(GotoStmt) X(LabelStmt) X(TryStmt) X(EmptyStmt) X(ErrorStmt) X(FallthroughStmt) X(AsmStmt) X(MacroStmt) \
  /* expressions */ \
  X(NameExpr) X(LiteralExpr) X(ThisExpr) X(CallExpr) X(IndexExpr) X(MemberExpr) X(UnaryExpr) \
  X(BinaryExpr) X(ConditionalExpr) X(CastExpr) X(NamedCastExpr) X(SizeofExpr) X(CallerExpr) X(NameofExpr) X(NoexceptExpr) X(NewExpr) \
  X(DeleteExpr) X(LambdaExpr) X(BraceInitExpr) X(ThrowExpr) X(FunctionalCastExpr) X(TypeidExpr) \
  X(FoldExpr) X(RequiresExpr) X(ErrorExpr) X(ParenExpr) X(HeldExpr) X(DefaultValueExpr)

enum class NodeKind : uint8_t
{
#define X(name) name,
  KPP_NODE_KINDS(X)
#undef X
};
const char* nodeKindName(NodeKind kind);

struct Node
{
  NodeKind kind;
  uint32_t token; // Index of the first token, for diagnostics.
};

struct Type;
struct Expr;
struct Stmt;
struct Decl;

// One component of a qualified name: `name<args>`.
struct NamePart
{
  uint32_t symbol = UINT32_MAX; // UINT32_MAX for the leading `::` component or destructor/operator parts.
  Span<Node*> templateArgs;     // Type* or Expr*.
  bool hasTemplateArgs = false;
  // Special names:
  uint8_t special = 0; // 0 none, 1 destructor (~symbol), 2 operator (opToken = punctuator/keyword id), 3 conversion operator (convType)
  uint32_t opToken = 0;
  Type* convType = nullptr;
};

struct QualifiedName
{
  bool empty() const { return parts.empty(); }
  const NamePart& last() const { return parts[parts.size() - 1]; }

  Span<NamePart> parts;
  bool global = false; // Leading `::`.
  // Required `this.` (design.md 23c, a NameExpr's name, markRequiredThis): this bare name is never a member. Not in the
  // stdlib or imported C, nor in the types in declarations where `this` is no value (a parameter's, a data member's, a
  // leading return type); the walk and the evaluator read this one mark, wherever they meet the name first.
  bool requiresThis = false;
  Type* typeQualifier = nullptr; // `decltype(x)::member`: the parts are looked up in the class this type names.
};
