#pragma once
// The table of canonical types (Ty.hpp): every type made once and shared, so that type equality is
// pointer equality, from any number of threads.
#include "Ty.hpp"
#include <cstdint>
#include <string>
#include <vector>

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
  // The target's C data model: long and wchar_t 64 and 32 bits wide (lp64: Linux, FreeBSD, macOS),
  // else 32 and 16 (Windows).
  void setDataModel(bool lp64)
  {
    this->cLongBits = lp64 ? 64 : 32;
    this->cWcharBits = lp64 ? 32 : 16;
  }
  uint8_t longBits() const { return this->cLongBits; }
  uint8_t wcharBits() const { return this->cWcharBits; }
  // wchar_t: a type of its own (count = 1), never char16_t or char32_t, which have its size and on
  // Windows its sign too: as wide as the target's C wchar_t, unsigned where that is 16 bits.
  const Ty* wcharType();
  // The integer conversion rank ([conv.rank]): long long above long above int above short above
  // the 8-bit types; 0 for a type that is no integer.
  static int integerRank(const Ty* t);
  // The type an arithmetic type t (unqualified) promotes to ([conv.prom]): bool, the character types
  // and the integers narrower than int to int, but char32_t to unsigned int, as int cannot hold its
  // values; any other type (a wider integer, a floating one) is itself. Not an enumeration's: the
  // caller promotes its underlying type.
  const Ty* promoted(const Ty* t);
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
  // cConvention: the function passes a class by value as its target's C does, not as kpp does (a
  // pointer to a copy): a C function's type that takes or returns one, and such a function type a C
  // header spells (GlobalScope::Impl::withCConvention). Another type than kpp's of the same signature
  // (count = 1), so that no pointer of the one converts to the other, where a call through it would
  // pass the class as the function does not take it. It prints as `extern C int32(S8)` (no quotes: a
  // type's printed form is part of LLVM's quoted names).
  // noexcept: part of the type (C++17): `void() noexcept` is another type than `void()`, printed so, which
  // keeps two instances and overloads apart in LLVM's names. noexceptParam: the `N` of a pattern's
  // `noexcept(N)`, dependent until N is bound.
  const Ty* function(const Ty* result, Span<const Ty*> params, bool variadic, bool cConvention = false, bool isNoexcept = false, Decl* noexceptParam = nullptr);
  // Another function type with what `like` says besides its result and parameters.
  const Ty* functionLike(const Ty* like, const Ty* result, Span<const Ty*> params);
  // Function type t, noexcept or not: `void()` and `void() noexcept` from each other.
  const Ty* withNoexcept(const Ty* t, bool isNoexcept);
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
  // A class object as one (`scaled<Fixed{3, 14}>`, C++20's class-type parameter): inner the class,
  // args each part's value as ConstValue's Object has them (its bases, then its data members, each a
  // value, a part of a class type an object value too); count 1 (isObjectValue).
  const Ty* objectValue(const Ty* of, Span<const Ty*> parts);
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
  // The types made that mention no unknown one: the report's count, the same at every thread count.
  // (A lookup another thread has not finished yet answers unknown for a moment, and a type made of
  // that answer, `IntrusiveListImplementation<<unknown>, ...>` while UnitGroup's bases are resolved,
  // was made in some runs only: the count of all of them differed between -j 8 and -j 32.)
  size_t sizeWithoutUnknown() const;
  // Every class type with template arguments made so far, from the from-th on (in the order they
  // were made: an index into a list kept as they are interned, not a walk of every type).
  std::vector<const Ty*> classInstances(size_t from = 0);
private:
  const Ty* intern(const Ty& t);
  struct Impl;
  Impl* impl;
  uint8_t cLongBits = 32;
  // wchar_t, and the units of L"..." and L'x': the target's C wchar_t (unsigned 16 bits, UTF-16, on
  // Windows; a signed 32-bit int, UTF-32, as glibc, FreeBSD and macOS have it: docs/design.md,
  // "Targets and hosts").
  uint8_t cWcharBits = 16;
  const Ty* void_;
  const Ty* bool_;
  const Ty* nullptr_;
  const Ty* unknown_;
  const Ty* auto_;
  const Ty* decltypeAuto_;
};
