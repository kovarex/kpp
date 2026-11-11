#pragma once
// Compile-time values and what literals, operators and conversions make of them (design.md,
// "Compile-time evaluation: one interpreter"). The type checker's evaluator and the code
// generator's compute with these, so that the two cannot disagree on a constant.
#include "lexer.hpp"
#include "TypeTable.hpp"
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
  bool operator==(const ConstTarget&) const = default;

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

#include "ConstValue.hpp"

// The bytes a narrow string literal token stands for (escapes, raw strings, u8), its prefix
// and quotes left out; false for a wide one (L, u, U) or one it cannot read.
bool decodeNarrowString(std::string_view spelling, std::string& out);

enum class ConstMode : uint8_t
{
  // A context needs the constant (a template argument, an array bound, static_assert, if
  // constexpr): a signed overflow fails it, as in C++.
  Required,
  // The code generator folding a run-time expression: signed arithmetic wraps, as the emitted
  // code's does, and a shift count is taken modulo the width, as at run time; what the
  // emitted code leaves undefined (a division by zero) is no constant.
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

#include "ConstArith.hpp"
