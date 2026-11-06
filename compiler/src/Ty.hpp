#pragma once
// A canonical type (Ty) and its kinds. Every distinct type exists exactly once in the TypeTable, so type equality is
// pointer equality and two threads that build the same type (e.g. the same template
// instantiation) get the same object. Qualifiers are part of the type: `const int` and `int`
// are different Ty objects that share nothing but their kind.
#include "Ast/Ast.hpp"
#include <cstdint>
#include <string>

enum class TyKind : uint8_t
{
  Void,
  Bool,
  Char,    // bits = 8/16/32, isUnsigned; count = 1 marks wchar_t (TypeTable::wcharType): a type of its own beside
           // char16_t and char32_t, as wide and as signed as the target's C wchar_t
  Int,     // bits = 8/16/32/64, isUnsigned; count = 1 marks C's `long` (TypeTable::longType): a type of its
           // own beside int and long long, as wide as the target's C long
  Float,   // bits = 32/64
  Nullptr,
  Pointer, // inner
  LRef,    // inner
  RRef,    // inner
  Array,   // inner, count (0 = unknown bound)
  Function,// inner = result, args = parameters, variadic
  Class,   // decl = ClassDecl (definition if known), args = template arguments (empty for non-templates);
           // count = 1 marks a brace list argument (TypeTable::braceList)
  Enum,    // decl = EnumDecl
  Param,   // decl = TemplateParamDecl: dependent on a template parameter
  DependentMember, // inner = a dependent type, value = a member's symbol: `T::Unit`, resolved when T is substituted
  Lambda,  // inner = return type (null if unknown), args = parameters; decl = the LambdaExpr owner (unused)
  MemberPointer, // inner = member type, decl = owning class decl (may be null)
  Auto,    // Placeholder inside a declared type, replaced by deduction.
  Pack,    // args = the types a parameter pack was deduced to.
  Value,   // A non-type template argument: inner = its type, value = the constant.
  Member,  // A data member as a compile-time value (reflection, design.md 8.3): decl = its VariableDecl,
           // inner = its type in the class's instantiation, args[0] = the class type.
  Vector,  // SIMD vector: inner = element type (Float or Int), count = lane count (e.g. 4 for <4 x float>)
  Unknown, // Could not be determined; reason is tracked in the statistics, not in the type.
};

struct Ty
{
  bool isReference() const { return kind == TyKind::LRef || kind == TyKind::RRef; }
  bool isArithmetic() const { return kind == TyKind::Bool || kind == TyKind::Char || kind == TyKind::Int || kind == TyKind::Float; }
  bool isIntegral() const { return kind == TyKind::Bool || kind == TyKind::Char || kind == TyKind::Int || kind == TyKind::Enum; }
  bool isKnown() const { return kind != TyKind::Unknown; }
  bool isBraceList() const { return kind == TyKind::Class && count == 1; }
  bool isLong() const { return kind == TyKind::Int && count == 1; } // `long` or `unsigned long`
  bool isWchar() const { return kind == TyKind::Char && count == 1; } // wchar_t
  bool isCConvention() const { return kind == TyKind::Function && count == 1; } // TypeTable::function's cConvention
  bool isObjectValue() const { return kind == TyKind::Value && count == 1; }     // TypeTable::objectValue

  TyKind kind;
  uint8_t bits = 0;
  bool isUnsigned = false;
  bool isConst = false;
  bool isVolatile = false;
  bool variadic = false;
  uint32_t count = 0;
  int64_t value = 0; // Value kinds only.
  const Ty* inner = nullptr;
  Span<const Ty*> args;
  Decl* decl = nullptr;
};
