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

struct ConstValue
{
  enum class Kind : uint8_t { Int, Float, Object, Pointer, Unset };
  Kind kind = Kind::Int;
  // Int: an integer, char, bool or enumeration type, the value normalised to its width and
  // signedness; null for a value of no known type, read as a 64-bit signed integer (a null
  // pointer is the Int 0 of a pointer type). Float: float or double. Object: a class or array
  // type. Pointer: a pointer type. Unset: a field's or element's type that no initializer set
  // (reading it is no constant).
  const Ty* type = nullptr;
  int64_t i = 0; // Int; Pointer: the element it points at (the parts' size is one past the end)
  double f = 0.0;
  // Object: a class object's parts (its bases, then its non-static data members, in declaration
  // order) or an array's elements. Pointer: the array it points into. A copy shares them and a
  // write makes new ones, so that no value changes under another's feet.
  std::shared_ptr<const std::vector<ConstValue>> parts;

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
  bool isFloat() const { return kind == Kind::Float; }
  bool isScalar() const { return kind == Kind::Int || kind == Kind::Float; }
  bool isObject() const { return kind == Kind::Object; }
  bool isPointer() const { return kind == Kind::Pointer; }
  bool isUnset() const { return kind == Kind::Unset; }
  // A condition's value: a scalar's, or a pointer's (into an array: never null).
  bool truthy() const { return isFloat() ? f != 0.0 : isPointer() || i != 0; }
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
// bases), which are what `sizeof` and `alignof` are there.
struct ConstHooks
{
  ConstMode mode = ConstMode::Fold;
  std::function<const Ty*(const Ty*)> underlying;
  std::function<std::optional<int64_t>(const Ty*, bool alignment)> layout;
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
  TypeTable& types;
  ConstMode mode = ConstMode::Required;
  std::string* why = nullptr;
  // The underlying type an enumeration type was declared with, or null for one declared without
  // (its values then compute as int, or as a 64-bit integer when int cannot hold them).
  std::function<const Ty*(const Ty*)> underlying;

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
  };
  IntType intTypeOf(const Ty* t) const;
  IntType intTypeOf(const ConstValue& v) const;
  const Ty* tyOf(IntType it);
  IntType promoted(IntType it) const;
  IntType common(IntType a, IntType b) const;
  static int64_t normalise(int64_t v, IntType it);
  std::optional<ConstValue> fail(const std::string& reason);
  std::optional<ConstValue> fitted(__int128 exact, IntType it, const char* what);
};
