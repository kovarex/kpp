#pragma once
// The syntax tree's type nodes (Ast.hpp includes it, after Node.hpp).
#include "Node.hpp"

// --- Types ---

struct Type : Node {};
struct NamedType : Type
{
  QualifiedName name;
  bool typename_ = false;
  bool elaborated = false; // written `class X` / `struct X` / `union X`: declares X where nothing named X is visible
  Decl* resolved = nullptr;
};
struct BuiltinType : Type { Kw keyword; uint8_t longCount = 0; bool isUnsigned = false, isSigned = false; };
struct PointerType : Type { Type* pointee; };
struct LRefType : Type { Type* referenced; };
struct RRefType : Type { Type* referenced; };
struct ConstType : Type { Type* inner; bool isVolatile = false; bool isConst = true; }; // `volatile T` alone: isConst false
// asIndex: in a template argument list, the same tokens read as an index (`Take<table[2]>`), for
// the resolver to take when the parameter is a constant (`Kind<T[N]>` is an array type).
struct ArrayType : Type { Type* element; Expr* size; Expr* asIndex = nullptr; };
// asCall: in a template argument list, the same tokens read as a call (`Frac<gcd(N, M)>`), for
// the resolver to take when the parameter is a constant.
struct FunctionType : Type { Type* result; Span<Decl*> params; bool isConst = false, isNoexcept = false, isVariadic = false; Expr* asCall = nullptr; };
// file: the file it is written in, whose tokens type its expression again under an instance's
// arguments where the context has none (a qualified name's template arguments, as a lookup reads them).
struct DecltypeType : Type { Expr* expr; bool isAuto = false; const struct ParsedFile* file = nullptr; };
struct AutoType : Type { bool isDecltypeAuto = false; };
struct MemberPointerType : Type { Type* classType; Type* pointee; };
struct PackType : Type { Type* pattern; };
