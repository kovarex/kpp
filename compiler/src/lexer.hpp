#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

enum class TokenKind : uint8_t
{
  End,
  Identifier,
  Keyword,
  Number,
  String,
  Char,
  Punctuator,
  Directive, // A whole preprocessor line, from '#' to the end of line (continuations included).
  Error,
};

// C++ keywords, and `when` (design.md 19). kpp's other keywords (implicit, fallthrough, both_const,
// class_const, accumulate) are contextual and recognised by the parser, so C++ code using them as
// names still parses; C's code has `when` as a name (cKeywordsAsNames).
#define KPP_KEYWORDS(X) \
  X(alignas) X(alignof) X(and) X(and_eq) X(asm) X(auto) X(bitand) X(bitor) X(bool) X(break) \
  X(case) X(catch) X(char) X(char8_t) X(char16_t) X(char32_t) X(class) X(compl) X(concept) \
  X(const) X(consteval) X(constexpr) X(constinit) X(const_cast) X(continue) X(co_await) \
  X(co_return) X(co_yield) X(decltype) X(default) X(delete) X(do) X(double) X(dynamic_cast) \
  X(else) X(enum) X(explicit) X(export) X(extern) X(false) X(float) X(for) X(friend) X(goto) \
  X(if) X(inline) X(int) X(long) X(mutable) X(namespace) X(new) X(noexcept) X(not) X(not_eq) \
  X(nullptr) X(operator) X(or) X(or_eq) X(private) X(protected) X(public) X(register) \
  X(reinterpret_cast) X(requires) X(return) X(short) X(signed) X(sizeof) X(static) \
  X(static_assert) X(static_cast) X(struct) X(switch) X(template) X(this) X(thread_local) \
  X(throw) X(true) X(try) X(typedef) X(typeid) X(typename) X(union) X(unsigned) X(using) \
  X(virtual) X(void) X(volatile) X(wchar_t) X(when) X(while) X(xor) X(xor_eq)

enum class Kw : uint32_t
{
#define X(name) name##_,
  KPP_KEYWORDS(X)
#undef X
  Count
};

// Punctuators. Within one leading character, longer spellings must come first (max munch).
#define KPP_PUNCTUATORS(X) \
  X(Spaceship, "<=>") X(ShlAssign, "<<=") X(ShrAssign, ">>=") X(ArrowStar, "->*") X(Ellipsis, "...") \
  X(ColonColon, "::") X(Arrow, "->") X(PlusPlus, "++") X(MinusMinus, "--") X(Shl, "<<") X(Shr, ">>") \
  X(LessEq, "<=") X(GreaterEq, ">=") X(EqEq, "==") X(NotEq, "!=") X(AndAnd, "&&") X(OrOr, "||") \
  X(PlusAssign, "+=") X(MinusAssign, "-=") X(StarAssign, "*=") X(SlashAssign, "/=") \
  X(PercentAssign, "%=") X(AmpAssign, "&=") X(PipeAssign, "|=") X(CaretAssign, "^=") \
  X(HashHash, "##") X(DotStar, ".*") X(SafeArrow, "?->") X(Elvis, "?:") \
  X(LBrace, "{") X(RBrace, "}") X(LBracket, "[") X(RBracket, "]") X(LParen, "(") X(RParen, ")") \
  X(Semi, ";") X(Colon, ":") X(Question, "?") X(Dot, ".") X(Plus, "+") X(Minus, "-") X(Star, "*") \
  X(Slash, "/") X(Percent, "%") X(Caret, "^") X(Amp, "&") X(Pipe, "|") X(Tilde, "~") X(Bang, "!") \
  X(Assign, "=") X(Less, "<") X(Greater, ">") X(Comma, ",") X(Hash, "#")

enum class P : uint32_t
{
#define X(name, spelling) name,
  KPP_PUNCTUATORS(X)
#undef X
  Count
};

// Compact token: 12 bytes. Text is recovered from the file by offset+length.
struct Token
{
  uint32_t offset;
  uint32_t length : 24;
  uint32_t kind : 7;
  uint32_t spliced : 1; // a literal with a line splice in it: its spelling is tokenSpelling's
  // Identifier: interned symbol id. Keyword: Kw. Punctuator: P. Else 0.
  uint32_t id;

  TokenKind getKind() const { return TokenKind(kind); }
  bool is(Kw k) const { return getKind() == TokenKind::Keyword && id == uint32_t(k); }
  bool is(P p) const { return getKind() == TokenKind::Punctuator && id == uint32_t(p); }
};

struct LexResult
{
  std::vector<Token> tokens;
  uint32_t lineCount = 0;
  uint32_t errorCount = 0;
};

// Interns identifier spellings to dense ids, safe to call from many threads at once.
class SymbolTable
{
public:
  SymbolTable();
  ~SymbolTable();
  uint32_t intern(std::string_view name);
  std::string_view name(uint32_t id) const;
  size_t size() const;

private:
  struct Impl;
  Impl* impl;
};

std::string_view keywordSpelling(Kw id);
std::string_view punctuatorSpelling(P id);
// Returns the keyword id or UINT32_MAX.
uint32_t lookupKeyword(std::string_view spelling);

// text must be followed by a '\0' sentinel (std::string guarantees it).
LexResult lex(std::string_view text, SymbolTable& symbols);

const char* tokenKindName(TokenKind kind);

// A token's spelling as C++'s translation phase 2 leaves it: every line splice (a backslash that
// ends a line) taken out. Only a string or char literal has one (the lexer marks it, and a splice
// anywhere else in a token is an error); its spelling is then made in buffer.
std::string_view tokenSpelling(std::string_view text, const Token& t, std::string& buffer);

// A raw string literal's body (between `delimiter(` and `)delimiter`) as C++ reads it, appended to
// out: a CRLF line end of the source is one newline, as translation phase 1 maps it before the
// literal is read (a CRLF file's R"(a<CRLF>b)" is the bytes 97 10 98, never 97 13 10 98). Nothing
// else changes: a raw string takes no splice and no escape. Every decoder of a raw string's bytes
// takes them from here.
void appendRawStringBody(std::string& out, std::string_view body);
