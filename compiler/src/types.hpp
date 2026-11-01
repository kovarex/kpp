#pragma once
// Canonical types. Every distinct type exists exactly once in the TypeTable, so type equality is
// pointer equality and two threads that build the same type (e.g. the same template
// instantiation) get the same object. Qualifiers are part of the type: `const int` and `int`
// are different Ty objects that share nothing but their kind.
#include "ast.hpp"
#include <cstdint>
#include <string>

enum class TyKind : uint8_t
{
  Void,
  Bool,
  Char,    // bits = 8/16/32, isUnsigned
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

  bool isReference() const { return kind == TyKind::LRef || kind == TyKind::RRef; }
  bool isArithmetic() const { return kind == TyKind::Bool || kind == TyKind::Char || kind == TyKind::Int || kind == TyKind::Float; }
  bool isIntegral() const { return kind == TyKind::Bool || kind == TyKind::Char || kind == TyKind::Int || kind == TyKind::Enum; }
  bool isKnown() const { return kind != TyKind::Unknown; }
  bool isBraceList() const { return kind == TyKind::Class && count == 1; }
  bool isLong() const { return kind == TyKind::Int && count == 1; } // `long` or `unsigned long`
};

class TypeTable
{
public:
  TypeTable();
  ~TypeTable();

  const Ty* voidType() const { return void_; }
  const Ty* boolType() const { return bool_; }
  const Ty* nullptrType() const { return nullptr_; }
  const Ty* unknown() const { return unknown_; }
  const Ty* autoType() const { return auto_; }
  // `decltype(auto)`: a placeholder whose deduced type is the expression's decltype as it is
  // (references kept), where `auto` decays it. Kind Auto, count = 1.
  const Ty* decltypeAutoType() const { return decltypeAuto_; }
  const Ty* intType(uint8_t bits = 32, bool isUnsigned = false);
  const Ty* charType(uint8_t bits = 8, bool isUnsigned = false);
  const Ty* floatType(uint8_t bits = 64);
  // C's `long` and `unsigned long` (design.md, "Types"): distinct from int and long long as in
  // C++, and as wide as the target's C long (32 bits on Windows, 64 on LP64 targets).
  const Ty* longType(bool isUnsigned = false);
  uint8_t longBits = 32;
  // The integer conversion rank ([conv.rank]): long long above long above int above short above
  // the 8-bit types; 0 for a type that is no integer.
  static int integerRank(const Ty* t);
  // The usual arithmetic conversions ([expr.arith.conv]) of two promoted arithmetic types.
  const Ty* usualArithmetic(const Ty* a, const Ty* b);
  const Ty* sizeType() { return intType(64, true); }
  const Ty* ptrdiffType() { return intType(64, false); }

  const Ty* pointer(const Ty* pointee);
  const Ty* lref(const Ty* t);
  const Ty* rref(const Ty* t);
  const Ty* array(const Ty* element, uint32_t count);
  // `T (&array)[N]` in a template: an array whose bound is the constant parameter N (decl), until
  // N is known; dependent.
  const Ty* dependentArray(const Ty* element, Decl* boundParam);
  const Ty* function(const Ty* result, Span<const Ty*> params, bool variadic);
  const Ty* classType(Decl* decl, Span<const Ty*> args);
  // A brace list argument `{a, b}` as overload resolution sees it: InitializerList<element>, but
  // a type of its own, since it deduces only an InitializerList<P> parameter (C++: any other
  // parameter is a non-deduced context) where an InitializerList object deduces any. inner: a
  // pack of each element's type (a class the list initializes takes them one by one), or null
  // when one is not known.
  const Ty* braceList(Decl* initializerList, const Ty* element, Span<const Ty*> elements = {});
  // `{}`: a brace list known to have no elements (an empty pack of them, its element type unknown:
  // it deduces nothing).
  const Ty* emptyBraceList(Decl* initializerList);
  // args: for an enum declared in a class template, the enclosing instance's arguments
  // (`Flag<A>::Enum` is not `Flag<B>::Enum`); none otherwise.
  const Ty* enumType(Decl* decl, Span<const Ty*> args = {});
  const Ty* param(Decl* decl);
  const Ty* param(Decl* decl, Span<const Ty*> args); // `Container<T>`: a template template parameter applied to arguments
  const Ty* dependentMember(const Ty* of, uint32_t symbol); // `T::Unit`
  const Ty* valueType(const Ty* of, int64_t value); // a non-type template argument
  const Ty* functionValue(const Ty* of, Decl* function); // a function as one (`template<float (*func)(float, float)>`): decl = the function
  const Ty* member(const Ty* memberType, Decl* decl, const Ty* cls); // a data member as a value (`for constexpr (auto m : members(T))`)
  // instance: a hash of the enclosing instantiation's arguments, so that a lambda written in a
  // template is a different type (with its own closure and function) in each instantiation.
  const Ty* lambda(const Ty* result, Span<const Ty*> params, Decl* closure = nullptr, int64_t instance = 0);
  const Ty* memberPointer(const Ty* member, const Ty* cls); // args[0] = the class (may be a parameter), decl = its declaration
  const Ty* pack(Span<const Ty*> elements);
  const Ty* vectorType(const Ty* element, uint32_t count); // SIMD vector: <count x element>
  // Same type with qualifiers added (references and functions ignore them).
  const Ty* qualified(const Ty* t, bool isConst, bool isVolatile);
  const Ty* unqualified(const Ty* t);

  // Reference and top-level qualifier removal, as for `auto` deduction.
  const Ty* decay(const Ty* t);
  // Reference removal only.
  const Ty* stripRef(const Ty* t) const { return t && t->isReference() ? t->inner : t; }

  bool isDependent(const Ty* t) const;
  // qualified: classes and enums with the namespaces and classes they are declared in, and a
  // local class with the place it is declared at (DebugInfoRenderer::LineInfo and
  // DebugVisualizationRenderer::LineInfo are two classes): what the code generator's IR names
  // are made of, which must tell every two types apart.
  std::string toString(const Ty* t, const SymbolTable* symbols, bool qualified = false) const;
  size_t size() const;
  // Every class type with template arguments made so far, from the from-th on (in the order they
  // were made: an index into a list kept as they are interned, not a walk of every type).
  std::vector<const Ty*> classInstances(size_t from = 0);

private:
  const Ty* intern(const Ty& t);
  struct Impl;
  Impl* impl;
  const Ty* void_;
  const Ty* bool_;
  const Ty* nullptr_;
  const Ty* unknown_;
  const Ty* auto_;
  const Ty* decltypeAuto_;
};
