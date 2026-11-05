#pragma once
// Included by consteval.hpp, where it was.

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
