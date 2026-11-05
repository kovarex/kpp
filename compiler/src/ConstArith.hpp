#pragma once
// Included by consteval.hpp, where it was.

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
