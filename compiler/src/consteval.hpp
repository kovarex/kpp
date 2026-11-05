#pragma once
// Compile-time values and what literals, operators and conversions make of them (design.md,
// "Compile-time evaluation: one interpreter"). The type checker's evaluator and the code
// generator's compute with these, so that the two cannot disagree on a constant.
#include "lexer.hpp"
#include "types.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// What an Address or a MemberPointer value names. Made once and shared, never changed.
struct ConstTarget
{
  bool operator==(const ConstTarget& o) const
  {
    return decl == o.decl && owner == o.owner && args == o.args && path == o.path && storage == o.storage;
  }

  // Address: a variable of static storage (a global, a static member, a static local) or a
  // function. MemberPointer: a non-static data member or member function.
  Decl* decl = nullptr;
  // Address: the class template instance decl is a member of (`Table<int>::rows`, a member
  // function of `Box<float>`), null for a member of a class that is no template and for anything
  // else. MemberPointer: the class it points into, always (`Point` of `&Point::y`).
  const Ty* owner = nullptr;
  // A function template's own arguments (`&convert<int>`), parameter to argument.
  std::vector<std::pair<Decl*, const Ty*>> args;
  // Address of a variable: the part of its value it points at, as an Object's parts are indexed
  // (bases, then data members, or elements), outermost first; empty for the whole variable. The
  // last index may be one past an array's end.
  std::vector<size_t> path;
  // Address: one of the evaluation's own objects (a local, a parameter, a temporary) by its serial,
  // decl then null. Only the interpreter reads one: an evaluation whose result holds it is no
  // constant.
  uint64_t storage = 0;
};

// A compile-time value. What the code generator may lower to constant data (globalForOnce; the
// contract agreed with the code generator thread on 2026-10-29):
// - Int: bool, char, integer and enumeration values, normalised to their type's width and
//   signedness; the Int 0 of a pointer type is the null pointer.
// - Float: float and double, as the host's IEEE double (a float's is exactly a float's value).
// - Object of a class type: one part per base, in declaration order, then one per non-static data
//   member, in declaration order, bit fields included (each its own part, its value already
//   fitted to its width: wrapped, a signed one sign-extended) and unnamed bit fields too (zero).
//   No part for a vtable pointer or padding. A reference member's part would be an Address; the
//   interpreter holds the referred-to value there for now, so an object with a reference member
//   is no constant data (any part of a kind its type cannot take means that).
// - Object of an array type: one part per element; the type has the length.
// - Pointer: into an array the evaluation made, which is no declaration's (a string literal's
//   chars with the terminating zero, the elements of a brace list given to an InitializerList):
//   parts are that array's elements, i the element pointed at (parts.size() is one past the end).
//   Pointers that share parts point into the same array, which is emitted once.
// - Address: of a declaration, or of a part of a variable's value (ConstTarget).
// - MemberPointer: of a data member (lowered to its offset) or a member function (ConstTarget).
// - Unset: no initializer gave a value; for static storage that is zero.
// A brace list's value before it meets its type (an Object whose type is TypeTable::braceList's,
// or of no type for `{}`) is never a global's value: GlobalScope::evaluateInitializer converts it.
struct ConstValue
{
  enum class Kind : uint8_t { Int, Float, Object, Pointer, Address, MemberPointer, Unset };

  static ConstValue integer(int64_t v, const Ty* type = nullptr)
  {
    ConstValue c;
    c.i = v;
    c.type = type;
    return c;
  }
  static ConstValue floating(double v, const Ty* type)
  {
    ConstValue c;
    c.kind = Kind::Float;
    c.f = v;
    c.type = type;
    return c;
  }
  static ConstValue object(const Ty* type, std::vector<ConstValue> parts)
  {
    ConstValue c;
    c.kind = Kind::Object;
    c.type = type;
    c.parts = std::make_shared<const std::vector<ConstValue>>(std::move(parts));
    return c;
  }
  static ConstValue pointer(const Ty* type, std::shared_ptr<const std::vector<ConstValue>> into, int64_t index)
  {
    ConstValue c;
    c.kind = Kind::Pointer;
    c.type = type;
    c.parts = std::move(into);
    c.i = index;
    return c;
  }
  static ConstValue unset(const Ty* type)
  {
    ConstValue c;
    c.kind = Kind::Unset;
    c.type = type;
    return c;
  }
  static ConstValue address(const Ty* type, ConstTarget target, Kind kind = Kind::Address)
  {
    ConstValue c;
    c.kind = kind;
    c.type = type;
    c.target = std::make_shared<const ConstTarget>(std::move(target));
    return c;
  }
  bool isFloat() const { return kind == Kind::Float; }
  bool isScalar() const { return kind == Kind::Int || kind == Kind::Float; }
  bool isObject() const { return kind == Kind::Object; }
  bool isPointer() const { return kind == Kind::Pointer; }
  bool isAddress() const { return kind == Kind::Address; }
  bool isMemberPointer() const { return kind == Kind::MemberPointer; }
  bool isUnset() const { return kind == Kind::Unset; }
  // A condition's value: a scalar's, or a pointer's (into an array, or a declaration's address:
  // never null).
  bool truthy() const { return isFloat() ? f != 0.0 : isPointer() || isAddress() || isMemberPointer() || i != 0; }

  Kind kind = Kind::Int;
  // Int: an integer, char, bool or enumeration type, the value normalised to its width and
  // signedness; null for a value of no known type, read as a 64-bit signed integer (a null
  // pointer is the Int 0 of a pointer type). Float: float or double. Object: a class or array
  // type. Pointer, Address: a pointer type. MemberPointer: a member pointer type. Unset: a field's
  // or element's type that no initializer set (reading it is no constant).
  const Ty* type = nullptr;
  int64_t i = 0; // Int; Pointer: the element it points at (the parts' size is one past the end)
  double f = 0.0;
  // Object: a class object's parts (its bases, then its non-static data members, in declaration
  // order) or an array's elements. Pointer: the array it points into. A copy shares them and a
  // write makes new ones, so that no value changes under another's feet.
  std::shared_ptr<const std::vector<ConstValue>> parts;
  // Address, MemberPointer: what it names.
  std::shared_ptr<const ConstTarget> target;
};

// The bytes a narrow string literal token stands for (escapes, raw strings, u8), its prefix
// and quotes left out; false for a wide one (L, u, U) or one it cannot read.
bool decodeNarrowString(std::string_view spelling, std::string& out);

enum class ConstMode : uint8_t
{
  // A context needs the constant (a template argument, an array bound, static_assert, if
  // constexpr): a signed overflow fails it, as in C++.
  Required,
  // The code generator folding a run-time expression: signed arithmetic wraps, as the emitted
  // code's does; what the emitted code leaves undefined (a division by zero, a shift by the
  // width) is no constant.
  Fold,
};

// What the code generator lends the checker's evaluator when it asks for a constant
// (GlobalScope::evaluate): its arithmetic mode (Fold: signed arithmetic wraps, as the emitted code
// does), the integer type it stores an enumeration in, and its layouts (exact: bit fields, empty
// bases), which are what `sizeof`, `alignof` and `offsetof` are there.
struct ConstHooks
{
  ConstMode mode = ConstMode::Fold;
  std::function<const Ty*(const Ty*)> underlying;
  std::function<std::optional<int64_t>(const Ty*, bool alignment)> layout;
  // The byte offset of data member m (a VariableDecl) in an object of class t, which may declare it
  // in a base; nullopt when t has no such member.
  std::function<std::optional<int64_t>(const Ty* t, Decl* m)> offset;
  // Each function body the evaluation runs (the definition it is written in): the code generator's
  // text then depends on that body, and an incremental build emits it again after an edit of it.
  std::function<void(const FunctionDecl*)> ran;
};

// The operator of a unary or binary expression (its op and isKeyword), `and`, `not` and `bitor`
// written as words included.
std::optional<P> constOperator(uint32_t op, bool isKeyword);
// Whether a binary operator computes a value of scalars (an assignment, a member pointer access
// does not).
bool isConstBinaryOperator(P op);

// The arithmetic of one evaluation. Every operation answers nullopt when its result is no
// constant, with the reason in *why (the first one is kept: what stopped the evaluation).
struct ConstArith
{
  // A number or character literal as written (digit separators, suffixes, floats, hex floats,
  // escapes).
  std::optional<ConstValue> literal(TokenKind kind, std::string_view spelling);
  std::optional<ConstValue> unary(P op, const ConstValue& v);
  std::optional<ConstValue> binary(P op, const ConstValue& l, const ConstValue& r);
  // An explicit or implicit conversion to an arithmetic, enumeration or bool type; nullopt for
  // any other target type.
  std::optional<ConstValue> convert(const ConstValue& v, const Ty* to);
private:
  struct IntType
  {
    uint8_t bits;
    bool isUnsigned;
    bool isLong = false; // C's long (Ty::isLong): its own rank, between int and long long
  };
  IntType intTypeOf(const Ty* t) const;
  IntType intTypeOf(const ConstValue& v) const;
  const Ty* tyOf(IntType it);
  IntType promoted(IntType it) const;
  IntType common(IntType a, IntType b) const;
  static int64_t normalise(int64_t v, IntType it);
  std::optional<ConstValue> fail(const std::string& reason);
  std::optional<ConstValue> fitted(__int128 exact, IntType it, const char* what);
public:
  TypeTable& types;
  ConstMode mode = ConstMode::Required;
  std::string* why = nullptr;
  // The underlying type an enumeration type was declared with, or null for one declared without
  // (its values then compute as int, or as a 64-bit integer when int cannot hold them).
  std::function<const Ty*(const Ty*)> underlying;
  // The range of values of an enumeration type declared without an underlying type, the smallest
  // bit field's that holds its enumerators ([dcl.enum]/8), or nullopt (one declared with, or one
  // whose enumerators are not known yet): a conversion out of it is no constant where one is
  // required, as clang has it ([expr.static.cast]/10: undefined).
  std::function<std::optional<std::pair<int64_t, int64_t>>(const Ty*)> enumRange;
};
