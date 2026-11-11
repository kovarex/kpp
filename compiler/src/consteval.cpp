#include "consteval.hpp"
#include "lexer.hpp"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

bool decodeNarrowString(std::string_view s, std::string& out)
{
  size_t q = s.find('"');
  if (q == std::string_view::npos || s.size() < q + 2 || s.back() != '"')
    return false;
  std::string_view prefix = s.substr(0, q);
  bool raw = !prefix.empty() && prefix.back() == 'R';
  if (raw)
    prefix.remove_suffix(1);
  if (!prefix.empty() && prefix != "u8")
    return false; // L"", u"", U"": wide characters
  if (raw)
  {
    // R"delim(text)delim"
    size_t open = s.find('(', q);
    if (open == std::string_view::npos)
      return false;
    size_t delim = open - q - 1, closing = 1 + delim + 1;
    if (s.size() < open + 1 + closing)
      return false;
    appendRawStringBody(out, s.substr(open + 1, s.size() - open - 1 - closing));
    return true;
  }
  auto utf8 = [&](uint32_t c) {
    if (c < 0x80)
      out += char(c);
    else if (c < 0x800)
      out += char(0xC0 | (c >> 6)), out += char(0x80 | (c & 0x3F));
    else if (c < 0x10000)
      out += char(0xE0 | (c >> 12)), out += char(0x80 | ((c >> 6) & 0x3F)), out += char(0x80 | (c & 0x3F));
    else
      out += char(0xF0 | (c >> 18)), out += char(0x80 | ((c >> 12) & 0x3F)), out += char(0x80 | ((c >> 6) & 0x3F)), out += char(0x80 | (c & 0x3F));
  };
  auto digit = [](char c, int base) {
    int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99;
    return d < base ? d : -1;
  };
  size_t end = s.size() - 1; // the closing quote
  for (size_t i = q + 1; i < end; ++i)
  {
    char c = s[i];
    if (c != '\\')
    {
      out += c;
      continue;
    }
    if (++i >= end)
      return false;
    char e = s[i];
    switch (e)
    {
      case 'n': out += '\n'; break;
      case 't': out += '\t'; break;
      case 'r': out += '\r'; break;
      case 'a': out += '\a'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'v': out += '\v'; break;
      case '\\': case '\'': case '"': case '?': out += e; break;
      case 'x':
      {
        uint32_t v = 0;
        int n = 0;
        for (; i + 1 < end && digit(s[i + 1], 16) >= 0; ++n)
          v = v * 16 + uint32_t(digit(s[++i], 16));
        if (n == 0)
          return false;
        out += char(v & 0xFF);
        break;
      }
      case 'u':
      case 'U':
      {
        int want = e == 'u' ? 4 : 8;
        uint32_t v = 0;
        for (int n = 0; n < want; ++n)
        {
          if (i + 1 >= end || digit(s[i + 1], 16) < 0)
            return false;
          v = v * 16 + uint32_t(digit(s[++i], 16));
        }
        utf8(v);
        break;
      }
      default:
      {
        if (digit(e, 8) < 0)
          return false;
        uint32_t v = uint32_t(digit(e, 8));
        for (int n = 1; n < 3 && i + 1 < end && digit(s[i + 1], 8) >= 0; ++n)
          v = v * 8 + uint32_t(digit(s[++i], 8));
        out += char(v & 0xFF);
        break;
      }
    }
  }
  return true;
}

std::optional<P> constOperator(uint32_t op, bool isKeyword)
{
  if (!isKeyword)
    return P(op);
  switch (Kw(op))
  {
    case Kw::or_: return P::OrOr;
    case Kw::and_: return P::AndAnd;
    case Kw::bitor_: return P::Pipe;
    case Kw::xor_: return P::Caret;
    case Kw::bitand_: return P::Amp;
    case Kw::not_eq_: return P::NotEq;
    case Kw::not_: return P::Bang;
    case Kw::compl_: return P::Tilde;
    default: return std::nullopt;
  }
}

bool isConstBinaryOperator(P op)
{
  switch (op)
  {
    case P::Plus: case P::Minus: case P::Star: case P::Slash: case P::Percent: case P::Shl: case P::Shr:
    case P::Amp: case P::Pipe: case P::Caret: case P::Less: case P::Greater: case P::LessEq: case P::GreaterEq:
    case P::EqEq: case P::NotEq: case P::AndAnd: case P::OrOr: case P::Comma:
      return true;
    default:
      return false;
  }
}

// --- integer types ---

ConstArith::IntType ConstArith::intTypeOf(const Ty* t) const
{
  if (!t)
    return {64, false}; // a value of no known type: what the evaluators computed with before
  t = types.unqualified(types.stripRef(t));
  switch (t->kind)
  {
    case TyKind::Bool: return {1, true};
    case TyKind::Char:
    case TyKind::Int: return {t->bits ? t->bits : uint8_t(32), t->isUnsigned, t->isLong()};
    case TyKind::Enum:
      if (const Ty* u = underlying ? underlying(t) : nullptr; u && u->kind != TyKind::Enum)
        return intTypeOf(u);
      return {32, false};
    default: return {64, false};
  }
}

// A value's integer type; an enumeration declared without an underlying type computes as int,
// or as a 64-bit integer for a value int cannot hold.
ConstArith::IntType ConstArith::intTypeOf(const ConstValue& v) const
{
  const Ty* t = v.type ? types.unqualified(types.stripRef(v.type)) : nullptr;
  if (t && t->kind == TyKind::Enum && !(underlying && underlying(t)) && (v.i < INT32_MIN || v.i > INT32_MAX))
    return {64, false};
  return intTypeOf(t);
}

const Ty* ConstArith::tyOf(IntType it)
{
  if (it.bits == 1)
    return types.boolType();
  return it.isLong ? types.longType(it.isUnsigned) : types.intType(it.bits, it.isUnsigned);
}

// Integral promotion: anything narrower than int (bool, char, short) computes as int.
ConstArith::IntType ConstArith::promoted(IntType it) const
{
  return it.bits < 32 ? IntType{32, false} : it;
}

// The usual arithmetic conversions between two promoted integer types.
ConstArith::IntType ConstArith::common(IntType a, IntType b) const
{
  auto rank = [](IntType t) { return t.isLong ? 5 : t.bits <= 8 ? 2 : t.bits <= 16 ? 3 : t.bits <= 32 ? 4 : 6; }; // as TypeTable::integerRank
  if (a.isUnsigned == b.isUnsigned)
    return rank(a) >= rank(b) ? a : b;
  IntType u = a.isUnsigned ? a : b, s = a.isUnsigned ? b : a;
  if (rank(u) >= rank(s))
    return u;
  return s.bits > u.bits ? s : IntType{s.bits, true, s.isLong}; // a wider signed type holds every value of the unsigned one
}

int64_t ConstArith::normalise(int64_t v, IntType it)
{
  if (it.bits == 1)
    return v != 0;
  if (it.bits >= 64)
    return v;
  uint64_t mask = (uint64_t(1) << it.bits) - 1, x = uint64_t(v) & mask;
  if (!it.isUnsigned && (x >> (it.bits - 1)) & 1)
    x |= ~mask;
  return int64_t(x);
}

namespace
{
  // A normalised value as the exact number it stands for.
  __int128 exactOf(int64_t v, bool isUnsigned) { return isUnsigned ? __int128(uint64_t(v)) : __int128(v); }

  const char* typeName(uint8_t bits, bool isUnsigned)
  {
    switch (bits)
    {
      case 1: return "bool";
      case 8: return isUnsigned ? "uint8" : "int8";
      case 16: return isUnsigned ? "uint16" : "int16";
      case 32: return isUnsigned ? "uint32" : "int32";
      default: return isUnsigned ? "uint64" : "int64";
    }
  }
}

std::optional<ConstValue> ConstArith::fail(const std::string& reason)
{
  if (why && why->empty())
    *why = reason;
  return std::nullopt;
}

// The exact result of an operation in type it: unsigned arithmetic wraps (C++ defines it so);
// signed arithmetic out of range is an overflow, an error where a constant is required and
// wrap-around where the code generator folds.
std::optional<ConstValue> ConstArith::fitted(__int128 exact, IntType it, const char* what)
{
  if (!it.isUnsigned)
  {
    __int128 max = (__int128(1) << (it.bits - 1)) - 1, min = -max - 1;
    if ((exact < min || exact > max) && mode == ConstMode::Required)
      return fail(std::string("signed overflow in ") + typeName(it.bits, false) + " (" + what + ")");
  }
  return ConstValue::integer(normalise(int64_t(uint64_t(exact)), it), tyOf(it));
}

// --- literals ---

bool multiCharacterLiteral(std::string_view spelling, std::vector<uint8_t>* bytes)
{
  // Only an ordinary literal: a prefixed one holds one character, a suffix is the program's own.
  if (spelling.size() < 4 || spelling.front() != '\'' || spelling.back() != '\'')
    return false;
  std::string_view body = spelling.substr(1, spelling.size() - 2);
  size_t count = 0;
  for (size_t i = 0; i < body.size(); ++count)
  {
    uint8_t value = uint8_t(body[i++]);
    if (value == '\\')
    {
      if (i >= body.size())
        return false;
      char e = body[i++];
      static const char names[] = "n\nt\tr\ra\ab\bf\fv\v\\\\''\"\"??";
      if (e == 'x')
      {
        size_t start = i;
        unsigned digits = 0;
        for (; i < body.size() && isxdigit(uint8_t(body[i])); ++i)
          digits = digits * 16 + unsigned(isdigit(uint8_t(body[i])) ? body[i] - '0' : (body[i] | 32) - 'a' + 10);
        if (i == start)
          return false;
        value = uint8_t(digits);
      }
      else if (e >= '0' && e <= '7')
      {
        unsigned digits = unsigned(e - '0');
        for (int more = 0; more < 2 && i < body.size() && body[i] >= '0' && body[i] <= '7'; ++more)
          digits = digits * 8 + unsigned(body[i++] - '0');
        value = uint8_t(digits);
      }
      else
      {
        const char* found = nullptr;
        for (const char* n = names; *n; n += 2)
          if (*n == e)
            found = n;
        if (!found)
          return false; // (`\u`: a code point, no byte of a narrow literal)
        value = uint8_t(found[1]);
      }
    }
    if (bytes)
      bytes->push_back(value);
  }
  return count > 1;
}

std::optional<ConstValue> ConstArith::literal(TokenKind kind, std::string_view spelling)
{
  if (kind == TokenKind::Char)
  {
    // 'a', '\n', '\x41', '\101', '\0', u8'a', L'a', U'\U0001F600': the character's value.
    size_t q = spelling.find('\'');
    if (q == std::string_view::npos || spelling.size() < q + 3 || spelling.back() != '\'')
      return fail("a character literal it cannot read");
    std::string_view prefix = spelling.substr(0, q), body = spelling.substr(q + 1, spelling.size() - q - 2);
    const Ty* type = prefix.empty() ? types.charType(8, false) : prefix == "L" ? types.wcharType() : prefix == "u" ? types.charType(16, true)
                   : prefix == "U" ? types.charType(32, true) : types.charType(8, true);
    int64_t v = 0;
    if (body.empty())
      return fail("an empty character literal");
    // `'ABCD'`: an int, its first character the highest byte (clang's and g++'s; conditionally
    // supported, [lex.ccon] 2). More than an int holds is refused, where they keep the last four.
    if (std::vector<uint8_t> bytes; multiCharacterLiteral(spelling, &bytes))
    {
      if (bytes.size() > 4)
        return fail("a character literal of more than four characters (" + std::string(spelling) + ")");
      uint32_t packed = 0;
      for (uint8_t b : bytes)
        packed = packed << 8 | b;
      return ConstValue::integer(int32_t(packed), types.intType(32));
    }
    if (body[0] != '\\')
    {
      // One character of the (UTF-8) source: in a wide literal its code point (`L'é'`), in a narrow
      // one a single byte (more is a multicharacter literal).
      uint32_t c = static_cast<unsigned char>(body[0]);
      size_t n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
      bool wide = prefix == "L" || prefix == "u" || prefix == "U";
      if (body.size() != 1 && !(wide && n > 1 && body.size() == n))
        return fail("a character literal of more than one character (" + std::string(spelling) + ")");
      if (n > 1 && body.size() == n)
        c &= 0x7F >> n;
      for (size_t k = 1; k < body.size(); ++k)
        c = (c << 6) | (static_cast<unsigned char>(body[k]) & 0x3F);
      v = c;
    }
    else
    {
      if (body.size() < 2)
        return fail("a character literal it cannot read");
      char e = body[1];
      std::string rest(body.substr(2));
      auto digitsOf = [](int base, std::string_view text) {
        std::string s(text);
        char* end = nullptr;
        long long n = std::strtoll(s.c_str(), &end, base);
        return !s.empty() && end && *end == 0 ? std::optional<int64_t>(n) : std::nullopt;
      };
      std::optional<int64_t> n;
      switch (e)
      {
        case 'n': n = '\n'; break;
        case 't': n = '\t'; break;
        case 'r': n = '\r'; break;
        case 'a': n = '\a'; break;
        case 'b': n = '\b'; break;
        case 'f': n = '\f'; break;
        case 'v': n = '\v'; break;
        case '\\': case '\'': case '"': case '?':
          if (rest.empty())
            n = e;
          break;
        case 'x': n = digitsOf(16, rest); break;
        case 'u': n = rest.size() == 4 ? digitsOf(16, rest) : std::nullopt; break;
        case 'U': n = rest.size() == 8 ? digitsOf(16, rest) : std::nullopt; break;
        default:
          if (e >= '0' && e <= '7' && body.size() <= 4) // up to three octal digits
            n = digitsOf(8, body.substr(1));
          break;
      }
      if (!n)
        return fail("a character literal it cannot read (" + std::string(spelling) + ")");
      v = *n;
    }
    return ConstValue::integer(normalise(v, intTypeOf(type)), type);
  }
  if (kind != TokenKind::Number)
    return fail("a literal that is no number");
  std::string digits;
  for (char c : spelling)
    if (c != '\'')
      digits += c;
  bool hex = digits.size() > 1 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X');
  bool binary = digits.size() > 1 && digits[0] == '0' && (digits[1] == 'b' || digits[1] == 'B');
  bool isFloat = hex ? digits.find_first_of("pP") != std::string::npos
                     : !binary && (digits.find('.') != std::string::npos || digits.find_first_of("eE") != std::string::npos);
  if (isFloat)
  {
    // 0.25, 1e-9, 2.f, 1.5L, 0x1.8p3: double unless an f suffix makes it float.
    bool single = !digits.empty() && (digits.back() == 'f' || digits.back() == 'F');
    while (!digits.empty() && strchr("fFlL", digits.back()))
      digits.pop_back();
    char* end = nullptr;
    double v = std::strtod(digits.c_str(), &end);
    if (!end || *end)
      return fail("a floating literal it cannot read (" + std::string(spelling) + ")");
    const Ty* type = types.floatType(single ? 32 : 64);
    return ConstValue::floating(single ? double(float(v)) : v, type);
  }
  // The suffix: u, l, ll, z in any case and order, or MSVC's i8 ... i64 and ui8 ... ui64.
  size_t end = digits.size();
  bool msvc64 = false;
  for (const char* s : {"i64", "i32", "i16", "i8"})
    if (end > strlen(s) && digits.compare(end - strlen(s), strlen(s), s) == 0)
    {
      msvc64 = s[1] == '6';
      end -= strlen(s);
      break;
    }
  size_t numberEnd = end;
  while (numberEnd > 0 && strchr("uUlLzZ", digits[numberEnd - 1]))
    --numberEnd;
  std::string suffix = digits.substr(numberEnd, end - numberEnd), number = digits.substr(0, numberEnd);
  int base = hex ? 16 : binary ? 2 : number.size() > 1 && number[0] == '0' ? 8 : 10;
  std::string_view body = number;
  if (hex || binary)
    body.remove_prefix(2);
  if (body.empty())
    return fail("a number it cannot read (" + std::string(spelling) + ")");
  unsigned __int128 value = 0;
  for (char c : body)
  {
    int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99;
    if (d >= base)
      return fail("a number it cannot read (" + std::string(spelling) + ")");
    value = value * base + d;
    if (value > std::numeric_limits<uint64_t>::max())
      return fail("an integer literal too large for 64 bits (" + std::string(spelling) + ")");
  }
  bool u = suffix.find_first_of("uU") != std::string::npos, z = suffix.find_first_of("zZ") != std::string::npos;
  int longs = 0;
  for (char c : suffix)
    longs += c == 'l' || c == 'L';
  // The first type that holds the value, as C++ chooses it (LLP64: long is 32 bits wide): a
  // literal without u stays signed when it can, a hex, octal or binary one may become unsigned,
  // and a decimal one too large for every signed type is unsigned (as clang and MSVC take it).
  auto fits = [&](IntType t) {
    return t.isUnsigned ? (t.bits >= 64 || value < (unsigned __int128)(uint64_t(1) << t.bits))
                        : value <= (unsigned __int128)((uint64_t(1) << (t.bits - 1)) - 1);
  };
  // ([lex.icon] 3: int, long, long long in order, each unsigned too for a literal that is not
  // decimal; l starts at long, ll at long long, u takes the unsigned ones.)
  IntType i{32, false}, ui{32, true}, l{types.longBits(), false, true}, ul{types.longBits(), true, true}, ll{64, false}, ull{64, true};
  std::vector<IntType> candidates;
  if (z)
    candidates = {IntType{64, u}}; // C++23: size_t, or its signed counterpart
  else if (u)
    candidates = longs >= 2 || msvc64 ? std::vector<IntType>{ull} : longs == 1 ? std::vector<IntType>{ul, ull} : std::vector<IntType>{ui, ul, ull};
  else if (longs >= 2 || msvc64)
    candidates = {ll, ull};
  else if (longs == 1)
    candidates = base == 10 ? std::vector<IntType>{l, ll, ull} : std::vector<IntType>{l, ul, ll, ull};
  else
    candidates = base == 10 ? std::vector<IntType>{i, l, ll, ull} : std::vector<IntType>{i, ui, l, ul, ll, ull};
  for (IntType t : candidates)
    if (fits(t))
      return ConstValue::integer(normalise(int64_t(uint64_t(value)), t), tyOf(t));
  return fail("an integer literal too large for its type (" + std::string(spelling) + ")");
}

// --- operators ---

std::optional<ConstValue> ConstArith::unary(P op, const ConstValue& v)
{
  if (op == P::Bang && (v.isScalar() || v.isPointer() || v.isAddress() || v.isMemberPointer()))
    return ConstValue::integer(!v.truthy(), types.boolType());
  if (!v.isScalar())
    return fail("an operator on an object or a pointer"); // the evaluator's own (pointer arithmetic, class operators)
  if (v.isFloat())
  {
    if (op == P::Minus)
      return ConstValue::floating(-v.f, v.type);
    if (op == P::Plus)
      return v;
    return fail("this operator on a floating value");
  }
  IntType it = promoted(intTypeOf(v));
  __int128 x = exactOf(normalise(v.i, it), it.isUnsigned);
  switch (op)
  {
    case P::Plus: return ConstValue::integer(normalise(v.i, it), tyOf(it));
    case P::Minus: return fitted(-x, it, "negation");
    case P::Tilde: return ConstValue::integer(normalise(~normalise(v.i, it), it), tyOf(it));
    default: return fail("this unary operator in a constant");
  }
}

std::optional<ConstValue> ConstArith::binary(P op, const ConstValue& l, const ConstValue& r)
{
  bool comparison = op == P::Less || op == P::Greater || op == P::LessEq || op == P::GreaterEq || op == P::EqEq || op == P::NotEq;
  auto truth = [](const ConstValue& v) { return v.isScalar() || v.isPointer() || v.isAddress() || v.isMemberPointer(); };
  if ((op == P::AndAnd || op == P::OrOr) && truth(l) && truth(r))
    return ConstValue::integer(op == P::AndAnd ? l.truthy() && r.truthy() : l.truthy() || r.truthy(), types.boolType());
  if (!l.isScalar() || !r.isScalar())
    return fail("an operator on an object or a pointer");
  if (op == P::AndAnd || op == P::OrOr)
    return ConstValue::integer(op == P::AndAnd ? l.truthy() && r.truthy() : l.truthy() || r.truthy(), types.boolType());
  if (l.isFloat() || r.isFloat())
  {
    // The usual arithmetic conversions: double if either is double, else float.
    bool isDouble = (l.isFloat() && (!l.type || l.type->bits != 32)) || (r.isFloat() && (!r.type || r.type->bits != 32));
    auto asDouble = [this](const ConstValue& v) {
      return v.isFloat() ? v.f : intTypeOf(v).isUnsigned ? double(uint64_t(v.i)) : double(v.i);
    };
    double a = asDouble(l), b = asDouble(r);
    if (!isDouble)
    {
      a = double(float(a));
      b = double(float(b));
    }
    if (comparison)
    {
      bool c = op == P::Less ? a < b : op == P::Greater ? a > b : op == P::LessEq ? a <= b : op == P::GreaterEq ? a >= b
             : op == P::EqEq ? a == b : a != b;
      return ConstValue::integer(c, types.boolType());
    }
    double x;
    switch (op)
    {
      case P::Plus: x = a + b; break;
      case P::Minus: x = a - b; break;
      case P::Star: x = a * b; break;
      case P::Slash:
        // A division by zero is undefined in C++ ([expr.mul]/4) and no constant where one is required,
        // as clang has it; folded, it is IEEE's infinity or NaN, as at run time.
        if (b == 0 && mode == ConstMode::Required)
          return fail("a division by zero");
        x = a / b;
        break;
      default: return fail("this operator on a floating value");
    }
    // A result that is a NaN is no number ([expr.pre]/4): no constant where one is required, as
    // clang's rule (an infinity from an overflow is one).
    if (x != x && mode == ConstMode::Required)
      return fail(std::string("a floating result that is a NaN (") + (op == P::Plus ? "addition" : op == P::Minus ? "subtraction" : op == P::Star ? "multiplication" : "division") + ")");
    return ConstValue::floating(isDouble ? x : double(float(x)), types.floatType(isDouble ? 64 : 32));
  }
  if (op == P::Shl || op == P::Shr)
  {
    // The promoted left operand's type; the count is taken modulo its width, as kpp's run-time shift
    // takes it (kovarex, 2026-11-11; C++ leaves a count outside [0, width) undefined): `1 << 33` is 2.
    IntType it = promoted(intTypeOf(l));
    int count = int(uint64_t(r.i) & uint64_t(it.bits - 1));
    int64_t a = normalise(l.i, it);
    if (op == P::Shl)
      return ConstValue::integer(normalise(int64_t(uint64_t(a) << count), it), tyOf(it)); // C++20: modulo 2^width
    if (it.isUnsigned)
      return ConstValue::integer(normalise(int64_t(uint64_t(a) >> count), it), tyOf(it));
    return ConstValue::integer(normalise(a >> count, it), tyOf(it));
  }
  IntType it = common(promoted(intTypeOf(l)), promoted(intTypeOf(r)));
  __int128 a = exactOf(normalise(l.i, it), it.isUnsigned), b = exactOf(normalise(r.i, it), it.isUnsigned);
  if (comparison)
  {
    bool c = op == P::Less ? a < b : op == P::Greater ? a > b : op == P::LessEq ? a <= b : op == P::GreaterEq ? a >= b
           : op == P::EqEq ? a == b : a != b;
    return ConstValue::integer(c, types.boolType());
  }
  switch (op)
  {
    case P::Plus: return fitted(a + b, it, "addition");
    case P::Minus: return fitted(a - b, it, "subtraction");
    case P::Star:
      if (it.isUnsigned) // modulo 2^width, without the exact product (which can exceed 128 bits)
        return ConstValue::integer(normalise(int64_t(uint64_t(a) * uint64_t(b)), it), tyOf(it));
      return fitted(a * b, it, "multiplication");
    case P::Slash:
    case P::Percent:
    {
      if (b == 0)
        return fail("a division by zero");
      if (!it.isUnsigned && b == -1 && a == -(__int128(1) << (it.bits - 1)))
      {
        if (mode == ConstMode::Required)
          return fail(std::string("signed overflow in ") + typeName(it.bits, false) + " (division)");
        return ConstValue::integer(op == P::Slash ? normalise(int64_t(uint64_t(a)), it) : 0, tyOf(it)); // wraps
      }
      // In 64 bits (both operands fit them; 128-bit division needs a runtime library MSVC lacks).
      int64_t q = it.isUnsigned ? int64_t(op == P::Slash ? uint64_t(a) / uint64_t(b) : uint64_t(a) % uint64_t(b))
                                : op == P::Slash ? int64_t(a) / int64_t(b) : int64_t(a) % int64_t(b);
      return ConstValue::integer(normalise(q, it), tyOf(it));
    }
    case P::Amp: return ConstValue::integer(normalise(int64_t(a & b), it), tyOf(it));
    case P::Pipe: return ConstValue::integer(normalise(int64_t(a | b), it), tyOf(it));
    case P::Caret: return ConstValue::integer(normalise(int64_t(a ^ b), it), tyOf(it));
    default: return fail("this binary operator in a constant");
  }
}

// --- conversions ---

std::optional<ConstValue> ConstArith::convert(const ConstValue& v, const Ty* to)
{
  if (!to)
    return std::nullopt;
  const Ty* t = types.unqualified(types.stripRef(to));
  if ((v.isPointer() || v.isAddress() || v.isMemberPointer()) && t->kind == TyKind::Bool)
    return ConstValue::integer(1, types.boolType()); // a pointer into an array, an address: never null
  if (!v.isScalar())
    return fail("a conversion of an object or a pointer to a number");
  switch (t->kind)
  {
    case TyKind::Bool: return ConstValue::integer(v.truthy(), types.boolType());
    case TyKind::Float:
    {
      double x = v.isFloat() ? v.f : intTypeOf(v).isUnsigned ? double(uint64_t(v.i)) : double(v.i);
      return ConstValue::floating(t->bits == 32 ? double(float(x)) : x, t);
    }
    case TyKind::Enum:
    {
      if (mode == ConstMode::Required && enumRange)
        if (std::optional<std::pair<int64_t, int64_t>> range = enumRange(t))
        {
          bool inRange;
          int64_t shown;
          if (v.isFloat())
            inRange = v.f > double(range->first) - 1.0 && v.f < double(range->second) + 1.0, shown = int64_t(v.f);
          else if (intTypeOf(v).isUnsigned && v.i < 0)
            inRange = false, shown = v.i; // (beyond int64's range: no enumeration of int's reaches it)
          else
            inRange = v.i >= range->first && v.i <= range->second, shown = v.i;
          if (!inRange)
            return fail("a value of " + (intTypeOf(v).isUnsigned && !v.isFloat() ? std::to_string(uint64_t(shown)) : std::to_string(shown)) +
                        " out of its enumeration's range of values [" + std::to_string(range->first) + ", " + std::to_string(range->second) + "]");
        }
      // To the underlying type the enumeration was declared with; one declared without keeps
      // the value (every value of its enumerators fits it).
      if (const Ty* u = underlying ? underlying(t) : nullptr; u && u->kind != TyKind::Enum)
      {
        std::optional<ConstValue> c = convert(v, u);
        if (c)
          c->type = t;
        return c;
      }
      if (v.isFloat())
        return convert(ConstValue::integer(int64_t(v.f)), t);
      return ConstValue::integer(v.i, t);
    }
    case TyKind::Char:
    case TyKind::Int:
    {
      IntType it = intTypeOf(t);
      if (v.isFloat())
      {
        // Truncation toward zero; a value the type cannot hold is undefined, so no constant.
        double whole = std::trunc(v.f), lo = it.isUnsigned ? 0.0 : -std::ldexp(1.0, it.bits - 1), hi = std::ldexp(1.0, it.isUnsigned ? it.bits : it.bits - 1);
        if (!(whole >= lo && whole < hi))
          return fail(std::string("a floating value out of the range of ") + typeName(it.bits, it.isUnsigned));
        int64_t x = it.isUnsigned && v.f >= 9223372036854775808.0 ? int64_t(uint64_t(v.f)) : int64_t(v.f);
        return ConstValue::integer(normalise(x, it), t);
      }
      // Between integer types the value is taken modulo 2^width (C++20), never an error.
      return ConstValue::integer(normalise(v.i, it), t);
    }
    default:
      return std::nullopt;
  }
}
