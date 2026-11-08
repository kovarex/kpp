// kpp format (format.hpp, docs/format.md): whitespace only, in kpp's default style.
//
// A file is lexed (the compiler's lexer) and parsed in the parser's format mode, which records the
// role of every operator and brace token (ParsedFile::formatRoles). A walk over the tokens then gives
// each one its place in the layout: the statement it belongs to, its indentation level, the kind of
// brace it is, the kind of colon. The output is written gap by gap (the whitespace and comments
// between two tokens), each gap decided by the rules of KPP_FORMAT_RULES, and compared with the
// input's: a gap that differs is a change, counted under the rule that decided it. The output is
// lexed again and must have the input's tokens and comments, else the file is refused.
#include "format.hpp"
#include "Ast/Ast.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "SymbolTable.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace
{
  // Each rule's slug and what it says, in docs/format.md's order (its numbers are these, from 1).
#define KPP_FORMAT_RULES(X) \
  X(TrailingSpace, "trailing-space", "no space or tab at a line's end") \
  X(FinalLineEnd, "final-line-end", "the file ends with one line end") \
  X(Tabs, "tabs", "indentation with spaces, never a tab") \
  X(LineEnds, "line-ends", "every line end is the file's (its first line end's, LF or CRLF)") \
  X(BlankMaxOne, "blank-lines-max-one", "never two empty lines in a row") \
  X(BlankStart, "blank-start-of-file", "no empty line or space before the file's first line") \
  X(BlankAfterOpen, "blank-after-open-brace", "no empty line after a body's {") \
  X(BlankBeforeClose, "blank-before-close-brace", "no empty line before a body's }") \
  X(BlankAroundDefinition, "blank-around-definition", "an empty line before and after an expanded definition (above its comment)") \
  X(BraceOwnLine, "brace-own-line", "an expanded body's { on a line of its own") \
  X(BraceContentsNextLine, "brace-contents-next-line", "nothing after an expanded body's { on its line") \
  X(CloseBraceOwnLine, "close-brace-own-line", "an expanded body's } first on its line") \
  X(CloseBraceElse, "close-brace-else", "else and catch on the line after a }") \
  X(CloseBraceJoin, "close-brace-join", "what ends a statement stays on its line: };  });  } while (x);  and an empty loop body's ;") \
  X(OneStatementPerLine, "one-statement-per-line", "each statement or declaration starts a line (a label's may follow it)") \
  X(BracelessBodyOwnLine, "braceless-body-own-line", "the body of an if, else, for or while without braces on a line of its own") \
  X(ElseIfOneLine, "else-if-one-line", "else if on one line") \
  X(AccessLabelOwnLine, "access-label-own-line", "public:, protected: and private: on a line of their own") \
  X(WrapJoin, "wrap-join", "a statement's lines are joined where they fit within the limit (the writer's breaks are not kept)") \
  X(WrapBreak, "wrap-break", "a statement over the limit is broken: after its loosest operators, else one argument per line after its (") \
  X(IndentBlock, "indent-block", "a block's statements one level in") \
  X(IndentNamespace, "indent-namespace", "a namespace's declarations one level in") \
  X(IndentClass, "indent-class", "a class's members one level in") \
  X(IndentCase, "indent-case", "case labels one level inside their switch, their statements one more") \
  X(IndentAccessLabel, "indent-access-label", "public:, protected: and private: at the class's column") \
  X(IndentBraceless, "indent-braceless-body", "a body without braces one level in") \
  X(IndentBrace, "indent-brace", "a body's { and } at the column of what it belongs to") \
  X(IndentContinuationParen, "indent-continuation-paren", "a continued line inside parentheses: under the start of its argument (a new argument under the first, after the ()") \
  X(IndentContinuation, "indent-continuation", "another continued line: under what it continues (the value after return or an assignment), else one level in (a method chain's .f())") \
  X(IndentInitializer, "indent-initializer", "a constructor's initializer list or a base clause continued one level in") \
  X(IndentList, "indent-list", "the lines of an initializer list or an enum one level in") \
  X(IndentComment, "indent-comment", "a comment on its own line indented as the code after it") \
  X(SpaceBinary, "space-binary-operator", "one space on each side of a binary operator, an assignment, ? and :") \
  X(SpaceUnary, "space-unary-operator", "no space after a unary operator") \
  X(SpacePointer, "space-pointer", "*, &, && and ^ with the type: T* p, const T& r") \
  X(SpaceTemplate, "space-template", "no space before or inside template brackets: Vector<i32>, template<typename T>") \
  X(SpaceBracedList, "space-braced-list", "no space inside an initializer list's braces: {1, 2}") \
  X(SpaceOneLiner, "space-one-liner", "one space inside a one-liner body's braces: { return x; }, and {} when empty") \
  X(SpaceBeforeBrace, "space-before-brace", "one space before a body's { on its line, none between a name and its initializer list") \
  X(SpaceKeywordParen, "space-keyword-paren", "one space between if, for, while, switch, catch, when, requires or return and (") \
  X(SpaceCallParen, "space-call-paren", "no space between a name and the ( of its call or parameters, or the [ of an index") \
  X(SpaceInsideParens, "space-inside-parens", "no space after ( or [ or before ) or ]") \
  X(SpaceComma, "space-comma", "no space before a comma, one after") \
  X(SpaceSemicolon, "space-semicolon", "no space before ;, one after inside for (...)") \
  X(SpaceMember, "space-member-access", "no space around ., ->, ?-> and ::") \
  X(SpaceTrailingComment, "space-trailing-comment", "one space before a comment at a line's end") \
  X(SpaceWords, "space-between-words", "one space between two names, keywords or literals") \
  X(SpaceLabelColon, "space-label-colon", "no space before the colon of a label, a case or an access label") \
  X(SpaceOther, "space-other", "the rest of the spacing rules (operator names, ..., lambda captures)")

  enum class Rule : uint8_t
  {
    None,
#define X(name, slug, text) name,
    KPP_FORMAT_RULES(X)
#undef X
    Count
  };

  const char* ruleSlug(Rule r)
  {
    static const char* slugs[] = {
      "none",
#define X(name, slug, text) slug,
      KPP_FORMAT_RULES(X)
#undef X
    };
    return slugs[size_t(r)];
  }

  const char* ruleText(Rule r)
  {
    static const char* texts[] = {
      "",
#define X(name, slug, text) text,
      KPP_FORMAT_RULES(X)
#undef X
    };
    return texts[size_t(r)];
  }

  enum class Brace : uint8_t { None, Block, Lambda, Class, Enum, Decl, List };
  enum class LineKind : uint8_t { Continuation, Statement, Label, Access, BraceLine, Item, Initializer, ListOpen };
  enum class Colon : uint8_t { None, Ternary, Label, Access, Range, Base, Init, Bitfield, Other };
  enum class Ctx : uint8_t { Namespace, Class, Block, Switch };

  int widthOf(std::string_view s);

  bool isBody(Brace b) { return b == Brace::Block || b == Brace::Lambda || b == Brace::Class || b == Brace::Enum || b == Brace::Decl; }

  // What the walk gives a token.
  struct Info
  {
    uint32_t match = UINT32_MAX; // the bracket that closes ( [ { or opens ) ] }
    uint32_t stmt = 0;           // where its statement starts (a list's {, for what is in the list)
    uint16_t frame = 0;          // whose base column its level counts from
    uint16_t level = 0;
    uint16_t opensFrame = 0;     // the frame whose base is this token's line's indentation (0: none)
    LineKind kind = LineKind::Continuation;
    Rule indentRule = Rule::IndentContinuation;
    Brace brace = Brace::None;
    Colon colon = Colon::None;
    bool oneLine = false;   // a { or } whose pair is on the same line of the input
    bool inOneLine = false; // inside a one-liner's braces, its } included
    bool stmtStart = false;
    bool braceless = false;
    bool attached = false;  // stays on the line of the token before it (the ; of `while (x);`)
    bool elseKw = false;
    bool catchKw = false;
    bool doWhile = false;
    bool elseIf = false;
    bool opName = false;    // part of an operator's name after `operator`
    bool capture = false;   // in a lambda's captures
    bool defStart = false;  // starts an expanded definition at namespace or class scope
    bool defEnd = false;    // ends one
    bool marked = false;
  };

  struct Frame
  {
    uint16_t parent = 0;
    int base = 0;
  };

  struct Item
  {
    uint32_t begin, end;
    int32_t tok;      // the token's index, -1 for a comment
    bool lineComment; // `//`
  };

  struct Paren
  {
    uint32_t open, close;
    int alignCol;
    int lineIndent;
    uint32_t outLine;
    bool content;
    int argCol = -1; // where the argument being written starts
  };

  class Formatter
  {
  public:
    Formatter(std::string_view text, const std::vector<Token>& toks, const std::vector<uint8_t>& roles, SymbolTable& symbols)
      : text(text), toks(toks), roles(roles), symbols(symbols)
    {}

    FormatOutcome run();

  private:
    // --- tokens ---
    uint32_t count() const { return uint32_t(toks.size() - 1); } // (the End token not counted)
    const Token& tok(uint32_t i) const { return toks[std::min<size_t>(i, toks.size() - 1)]; }
    bool isP(uint32_t i, P p) const { return i < count() && toks[i].is(p); }
    bool isKw(uint32_t i, Kw k) const { return i < count() && toks[i].is(k); }
    bool isIdent(uint32_t i, std::string_view name) const
    {
      return i < count() && toks[i].getKind() == TokenKind::Identifier && symbols.name(toks[i].id) == name;
    }
    std::string_view spelling(uint32_t i) const { return text.substr(toks[i].offset, toks[i].length); }
    uint8_t role(uint32_t i) const { return i < count() ? roles[toks[i].offset] : 0; }
    bool roleIs(uint32_t i, NodeKind k) const { return role(i) == uint8_t(uint8_t(k) + 1); }
    bool isWord(uint32_t i) const
    {
      TokenKind k = tok(i).getKind();
      return k == TokenKind::Identifier || k == TokenKind::Keyword || k == TokenKind::Number || k == TokenKind::String || k == TokenKind::Char;
    }
    bool templateOpen(uint32_t i) const { return isP(i, P::Less) && role(i) == FormatTemplateOpen; }
    bool templateClose(uint32_t i) const
    {
      return (isP(i, P::Greater) || isP(i, P::Shr) || isP(i, P::GreaterEq) || isP(i, P::ShrAssign)) && role(i) == FormatTemplateClose;
    }
    // How many template lists a `>` or `>>` closes (the parser split `>>` in place).
    int templateCloses(uint32_t i) const
    {
      if (!templateClose(i))
        return 0;
      return 1 + (toks[i].length >= 2 && roles[toks[i].offset + 1] == FormatTemplateClose ? 1 : 0);
    }
    uint32_t line(uint32_t offset) const { return uint32_t(std::upper_bound(lineStarts.begin(), lineStarts.end(), offset) - lineStarts.begin()); }
    uint32_t tokLine(uint32_t i) const { return line(toks[i].offset); }

    // --- the walk ---
    void mark(uint32_t i, uint16_t frame, uint16_t level, uint32_t stmt, LineKind kind, Rule indentRule = Rule::IndentContinuation)
    {
      if (i >= count())
        return;
      Info& in = info[i];
      in.frame = frame;
      in.level = level;
      in.stmt = stmt;
      in.kind = kind;
      in.indentRule = indentRule;
      in.marked = true;
    }
    uint16_t newFrame(uint16_t parent, uint32_t introducer)
    {
      frames.push_back({parent, 0});
      uint16_t f = uint16_t(frames.size() - 1);
      info[introducer].opensFrame = f;
      return f;
    }
    static Rule levelRule(Ctx ctx)
    {
      switch (ctx)
      {
        case Ctx::Namespace: return Rule::IndentNamespace;
        case Ctx::Class: return Rule::IndentClass;
        case Ctx::Switch: return Rule::IndentCase;
        default: return Rule::IndentBlock;
      }
    }
    void setOneLine(uint32_t open)
    {
      uint32_t close = info[open].match;
      for (uint32_t k = open + 1; k <= close && k < count(); ++k)
        info[k].inOneLine = true;
    }
    void markOperatorName(uint32_t i);
    void body(uint32_t from, uint32_t end, uint16_t frame, uint16_t level, Ctx ctx, Rule rule);
    uint32_t statement(uint32_t i, uint32_t end, uint16_t frame, uint16_t level, Ctx ctx, Rule rule, bool start = true, uint32_t stmt = UINT32_MAX);
    uint32_t sub(uint32_t i, uint32_t end, uint16_t frame, uint16_t level, Ctx ctx, uint32_t stmt);
    uint32_t parens(uint32_t open, uint16_t frame, uint16_t level, uint32_t stmt, bool forHeader = false);
    uint32_t simple(uint32_t i, uint32_t end, uint16_t frame, uint16_t level, Ctx ctx, uint32_t stmt);
    void inner(uint32_t from, uint32_t end, uint16_t frame, uint16_t level, uint32_t stmt, LineKind kind, bool forHeader, bool capture);
    void bodyBraces(uint32_t open, uint16_t frame, uint16_t level, Brace kind, Ctx innerCtx, uint16_t innerLevel, Rule innerRule);
    void list(uint32_t open, uint16_t frame, uint16_t level, uint32_t stmt);
    void lambdaBody(uint32_t open, uint16_t frame, uint16_t level, uint32_t stmt);
    void enumBody(uint32_t open, uint16_t frame, uint16_t level);
    bool requiresBrace(uint32_t open) const
    {
      if (role(open) != 0 || open == 0)
        return false;
      if (isKw(open - 1, Kw::requires_))
        return true;
      return isP(open - 1, P::RParen) && info[open - 1].match > 0 && isKw(info[open - 1].match - 1, Kw::requires_);
    }
    void requiresBody(uint32_t open, uint16_t frame, uint16_t level, uint32_t stmt);
    void markDefinition(uint32_t start, uint32_t open, uint32_t end, Ctx ctx);

    // --- writing ---
    struct Space { int n; Rule rule; };
    bool operandEnd(uint32_t i) const;
    bool unaryAt(uint32_t i) const;
    bool binaryAt(uint32_t i) const;
    bool typeOp(uint32_t i) const;
    bool postfixAt(uint32_t i) const;
    Space spaceBetween(uint32_t a, uint32_t b) const;
    int lineIndent(uint32_t i, Rule& rule) const;
    int commentIndent(size_t k, Rule& rule) const;
    struct Decision { bool brk; Rule rule; bool soft; };
    std::string_view gapBefore(size_t k) const
    {
      uint32_t begin = k == 0 ? bodyStart : items[k - 1].end;
      return text.substr(begin, items[k].begin - begin);
    }
    Decision decide(size_t k) const;
    Space spacesBefore(size_t k, bool& unclassified) const;
    // Whether tokens a and b written together lex as something else: a longer punctuator, a comment.
    bool joins(uint32_t a, uint32_t b) const
    {
      if (tok(a).getKind() != TokenKind::Punctuator || tok(b).getKind() != TokenKind::Punctuator)
        return false;
      std::string together = std::string(spelling(a)) + std::string(spelling(b));
      if (together.substr(0, 2) == "//" || together.substr(0, 2) == "/*")
        return true;
      size_t own = spelling(a).size();
      for (uint32_t p = 0; p < uint32_t(P::Count); ++p)
      {
        std::string_view ps = punctuatorSpelling(P(p));
        if (ps.size() > own && together.compare(0, ps.size(), ps) == 0)
          return true;
      }
      return false;
    }
    int breakPrecedence(uint32_t t) const;
    void planSegment(size_t k, int startCol);
    void wrap(size_t a, size_t b, int startCol, int contCol);

    std::string_view text;
    const std::vector<Token>& toks;
    const std::vector<uint8_t>& roles;
    SymbolTable& symbols;
    std::vector<uint32_t> lineStarts;
    std::vector<Info> info;
    std::vector<Frame> frames;
    std::vector<uint32_t> lambdaIntroducers;
    std::vector<Item> items;
    std::vector<Paren> parenStack;
    std::vector<int32_t> prevTokOf; // per item: the last token before it, -1 for none
    std::vector<int> exprCol;       // per statement: the column of the value after its `return` or assignment, -1 for none
    size_t lastCommaDepth = SIZE_MAX;
    bool assignment(uint32_t t) const
    {
      if (!binaryAt(t))
        return false;
      switch (P(tok(t).id))
      {
        case P::Assign: case P::PlusAssign: case P::MinusAssign: case P::StarAssign: case P::SlashAssign: case P::PercentAssign:
        case P::AmpAssign: case P::PipeAssign: case P::CaretAssign: case P::ShlAssign: case P::ShrAssign:
          return true;
        default:
          return false;
      }
    }
    // The wrapping plan (columns > 0): per item, 1 a break before it, 2 none, 0 not planned; the
    // segment planned, items [planFrom, planTo], and its items' flat widths and spaces.
    std::vector<uint8_t> plan;
    size_t planFrom = SIZE_MAX, planTo = 0;
    std::vector<int> segWidth, segSpace;
    std::vector<uint8_t> segSoft;
    uint32_t bodyStart = 0;
    int indentWidth = 2;
  public:
    int columns = 0;
  };

  // `operator==(`, `operator()(`, `operator new[](`, `operator T*()`: the tokens of the name up to
  // the ( of the parameters.
  void Formatter::markOperatorName(uint32_t i)
  {
    uint32_t j = i + 1;
    if (isP(j, P::LParen) && isP(j + 1, P::RParen))
    {
      info[j].opName = info[j + 1].opName = true;
      return;
    }
    if (isP(j, P::LBracket) && isP(j + 1, P::RBracket))
    {
      info[j].opName = info[j + 1].opName = true;
      return;
    }
    for (; j < count() && !isP(j, P::LParen) && !isP(j, P::Semi) && !isP(j, P::LBrace); ++j)
      info[j].opName = true;
  }

  void Formatter::body(uint32_t from, uint32_t end, uint16_t frame, uint16_t level, Ctx ctx, Rule rule)
  {
    uint32_t i = from;
    while (i < end)
    {
      uint32_t next;
      if (ctx == Ctx::Switch)
      {
        bool label = isKw(i, Kw::case_) || (isKw(i, Kw::default_) && isP(i + 1, P::Colon));
        // (a block right after a label: its braces at the label's column, its statements one in)
        bool labelBlock = isP(i, P::LBrace) && i > 0 && info[i - 1].colon == Colon::Label;
        next = statement(i, end, frame, label || labelBlock ? level : level + 1, ctx, Rule::IndentCase);
      }
      else
        next = statement(i, end, frame, level, ctx, rule);
      i = next > i ? next : i + 1;
    }
  }

  // The statement or declaration at i. start: it starts a line of its own (false after `else`, a
  // template header or a `when`, which the writer may break or not).
  uint32_t Formatter::statement(uint32_t i, uint32_t end, uint16_t frame, uint16_t level, Ctx ctx, Rule rule, bool start, uint32_t stmt)
  {
    if (i >= end)
      return end;
    if (stmt == UINT32_MAX)
      stmt = i;
    if (start)
      info[i].stmtStart = true;
    mark(i, frame, level, stmt, LineKind::Statement, rule);
    const Token& t = tok(i);
    // a block
    if (t.is(P::LBrace))
    {
      if (roleIs(i, NodeKind::BlockStmt) || ctx == Ctx::Block || ctx == Ctx::Switch)
      {
        bodyBraces(i, frame, level, Brace::Block, Ctx::Block, level + 1, Rule::IndentBlock);
        return info[i].match + 1;
      }
      return simple(i, end, frame, level, ctx, stmt);
    }
    if (t.is(P::Semi))
      return i + 1;
    // labels
    bool caseLabel = t.is(Kw::case_) || (t.is(Kw::default_) && isP(i + 1, P::Colon));
    bool accessLabel = ctx == Ctx::Class && (t.is(Kw::public_) || t.is(Kw::protected_) || t.is(Kw::private_)) && isP(i + 1, P::Colon);
    bool gotoLabel = (ctx == Ctx::Block || ctx == Ctx::Switch) && t.getKind() == TokenKind::Identifier && isP(i + 1, P::Colon);
    if (caseLabel || gotoLabel)
    {
      info[i].kind = LineKind::Label;
      int questions = 0;
      uint32_t j = i + 1;
      for (; j < end; ++j)
      {
        mark(j, frame, level, stmt, LineKind::Continuation);
        if (isP(j, P::LParen) || isP(j, P::LBracket))
        {
          j = parens(j, frame, level, stmt);
          continue;
        }
        if (isP(j, P::Question))
          ++questions;
        else if (isP(j, P::Colon))
        {
          if (questions == 0)
            break;
          info[j].colon = Colon::Ternary;
          --questions;
        }
      }
      if (j < end)
        info[j].colon = Colon::Label;
      return j + 1;
    }
    if (accessLabel)
    {
      mark(i, frame, uint16_t(level - 1), stmt, LineKind::Access, Rule::IndentAccessLabel);
      mark(i + 1, frame, uint16_t(level - 1), stmt, LineKind::Continuation);
      info[i + 1].colon = Colon::Access;
      return i + 2;
    }
    // control statements
    Ctx inner = ctx == Ctx::Switch ? Ctx::Block : ctx;
    if (t.is(Kw::if_))
    {
      uint32_t j = i + 1;
      if (isKw(j, Kw::constexpr_))
        mark(j++, frame, level, stmt, LineKind::Continuation);
      if (isP(j, P::Bang))
        mark(j++, frame, level, stmt, LineKind::Continuation);
      if (isKw(j, Kw::consteval_))
        mark(j++, frame, level, stmt, LineKind::Continuation);
      if (isP(j, P::LParen))
        j = parens(j, frame, level, stmt) + 1;
      j = sub(j, end, frame, level, inner, stmt);
      if (isKw(j, Kw::else_))
      {
        mark(j, frame, level, j, LineKind::Statement, rule);
        info[j].elseKw = true;
        if (isKw(j + 1, Kw::if_))
        {
          info[j + 1].elseIf = true;
          return statement(j + 1, end, frame, level, ctx, rule, false, j);
        }
        return sub(j + 1, end, frame, level, inner, j);
      }
      return j;
    }
    if (t.is(Kw::for_) || t.is(Kw::while_) || t.is(Kw::switch_))
    {
      uint32_t j = i + 1;
      if (isKw(j, Kw::constexpr_))
        mark(j++, frame, level, stmt, LineKind::Continuation);
      if (isP(j, P::LParen))
        j = parens(j, frame, level, stmt, t.is(Kw::for_)) + 1;
      if (t.is(Kw::switch_) && isP(j, P::LBrace))
      {
        mark(j, frame, level, stmt, LineKind::BraceLine, Rule::IndentBrace);
        bodyBraces(j, frame, level, Brace::Block, Ctx::Switch, level + 1, Rule::IndentCase);
        return info[j].match + 1;
      }
      return sub(j, end, frame, level, inner, stmt);
    }
    if (t.is(Kw::do_))
    {
      uint32_t j = sub(i + 1, end, frame, level, inner, stmt);
      if (isKw(j, Kw::while_))
      {
        mark(j, frame, level, j, LineKind::Statement, rule);
        info[j].doWhile = true;
        uint32_t k = j + 1;
        if (isP(k, P::LParen))
          k = parens(k, frame, level, j) + 1;
        if (isP(k, P::Semi))
        {
          mark(k, frame, level, j, LineKind::Continuation);
          ++k;
        }
        return k;
      }
      return j;
    }
    if (t.is(Kw::try_) && isP(i + 1, P::LBrace))
    {
      bodyBraces(i + 1, frame, level, Brace::Block, Ctx::Block, level + 1, Rule::IndentBlock);
      uint32_t j = info[i + 1].match + 1;
      while (isKw(j, Kw::catch_))
      {
        mark(j, frame, level, j, LineKind::Statement, rule);
        info[j].catchKw = true;
        uint32_t k = j + 1;
        if (isP(k, P::LParen))
          k = parens(k, frame, level, j) + 1;
        if (!isP(k, P::LBrace))
          return k;
        bodyBraces(k, frame, level, Brace::Block, Ctx::Block, level + 1, Rule::IndentBlock);
        j = info[k].match + 1;
      }
      return j;
    }
    // a template header, or a `when` before a declaration: the declaration is the writer's to break
    if (t.is(Kw::template_) && templateOpen(i + 1))
    {
      uint32_t j = i + 1;
      int depth = 0;
      for (; j < end; ++j)
      {
        mark(j, frame, level, stmt, LineKind::Continuation);
        if (isP(j, P::LParen) || isP(j, P::LBracket))
        {
          j = parens(j, frame, level, stmt);
          continue;
        }
        if (templateOpen(j))
          ++depth;
        depth -= templateCloses(j);
        if (depth <= 0 && j > i + 1)
          break;
      }
      if (isKw(j + 1, Kw::requires_))
      {
        // `template<typename T> requires C<T>`: the clause runs to the declaration's first name
        return simple(j + 1, end, frame, level, ctx, stmt);
      }
      return statement(j + 1, end, frame, level, ctx, rule, false, stmt);
    }
    if (t.is(Kw::when_) && isP(i + 1, P::LParen))
    {
      uint32_t j = parens(i + 1, frame, level, stmt) + 1;
      if (isP(j, P::LBrace) && !roleIs(j, NodeKind::BlockStmt) && !roleIs(j, NodeKind::BraceInitExpr))
      {
        bodyBraces(j, frame, level, Brace::Decl, ctx, level + 1, levelRule(ctx));
        markDefinition(stmt, j, info[j].match, ctx);
        return info[j].match + 1;
      }
      return statement(j, end, frame, level, ctx, rule, false, stmt);
    }
    return simple(i, end, frame, level, ctx, stmt);
  }

  // The body of an if, else, for, while or do at i: a block, or one statement one level in.
  uint32_t Formatter::sub(uint32_t i, uint32_t end, uint16_t frame, uint16_t level, Ctx ctx, uint32_t stmt)
  {
    if (i >= end)
      return end;
    if (isP(i, P::LBrace))
    {
      bool block = roleIs(i, NodeKind::BlockStmt) || ctx == Ctx::Block;
      bodyBraces(i, frame, level, block ? Brace::Block : Brace::Decl, block ? Ctx::Block : ctx, level + 1, block ? Rule::IndentBlock : levelRule(ctx));
      mark(i, frame, level, stmt, LineKind::BraceLine, Rule::IndentBrace);
      return info[i].match + 1;
    }
    if (isP(i, P::Semi))
    {
      // `while (x);`: the empty body stays on its line
      mark(i, frame, level, stmt, LineKind::Continuation);
      info[i].attached = true;
      return i + 1;
    }
    info[i].braceless = true;
    uint32_t next = statement(i, end, frame, uint16_t(level + 1), ctx, Rule::IndentBraceless);
    info[i].indentRule = Rule::IndentBraceless;
    return next;
  }

  // The brackets at open and what is in them; returns the closing bracket's index.
  uint32_t Formatter::parens(uint32_t open, uint16_t frame, uint16_t level, uint32_t stmt, bool forHeader)
  {
    uint32_t close = info[open].match;
    if (open != stmt || !info[open].marked) // (a statement that starts with it keeps its start's place)
      mark(open, frame, level, stmt, LineKind::Continuation);
    // (a lambda's [ has its closure class's role, made after the LambdaExpr at the same token)
    bool capture = isP(open, P::LBracket) && (roleIs(open, NodeKind::LambdaExpr) || roleIs(open, NodeKind::ClassDecl));
    if (capture)
      lambdaIntroducers.push_back(open);
    inner(open + 1, close, frame, level, stmt, LineKind::Continuation, forHeader, capture);
    mark(close, frame, level, stmt, LineKind::Continuation);
    return close;
  }

  void Formatter::inner(uint32_t from, uint32_t end, uint16_t frame, uint16_t level, uint32_t stmt, LineKind kind, bool forHeader, bool capture)
  {
    int questions = 0;
    for (uint32_t i = from; i < end; ++i)
    {
      mark(i, frame, level, stmt, kind);
      info[i].capture = capture;
      if (isP(i, P::LParen) || isP(i, P::LBracket))
      {
        i = parens(i, frame, level, stmt);
        continue;
      }
      if (isP(i, P::LBrace))
      {
        if (role(i) == FormatLambdaBody)
          lambdaBody(i, frame, level, stmt);
        else if (requiresBrace(i))
          requiresBody(i, frame, level, stmt);
        else
          list(i, frame, level, stmt);
        i = info[i].match;
        continue;
      }
      if (isKw(i, Kw::operator_))
        markOperatorName(i);
      else if (isP(i, P::Question))
        ++questions;
      else if (isP(i, P::Colon))
      {
        if (questions > 0)
        {
          info[i].colon = Colon::Ternary;
          --questions;
        }
        else
          info[i].colon = forHeader ? Colon::Range : Colon::Other;
      }
    }
  }

  // A declaration or an expression statement at i, to its `;` or, for a function, its body's }.
  uint32_t Formatter::simple(uint32_t i, uint32_t end, uint16_t frame, uint16_t level, Ctx ctx, uint32_t stmt)
  {
    uint32_t start = i;
    Brace head = Brace::None; // what a { with no role of its own opens
    bool sawParams = false;
    int questions = 0, templates = 0;
    enum { NoList, BaseList, InitList } listKind = NoList;
    for (; i < end; ++i)
    {
      if (i != start || !info[i].marked)
        mark(i, frame, level, stmt, LineKind::Continuation);
      const Token& t = tok(i);
      if (!sawParams && head == Brace::None)
      {
        if (t.is(Kw::enum_))
          head = Brace::Enum;
        else if (t.is(Kw::class_) || t.is(Kw::struct_) || t.is(Kw::union_))
          head = Brace::Class;
        else if (t.is(Kw::namespace_) || (t.is(Kw::extern_) && tok(i + 1).getKind() == TokenKind::String) ||
                 (i == start && isIdent(i, "suite") && tok(i + 1).getKind() == TokenKind::Identifier))
          head = Brace::Decl;
      }
      if (t.is(P::LParen) || t.is(P::LBracket))
      {
        if (t.is(P::LParen) && templates == 0)
          sawParams = true;
        i = parens(i, frame, level, stmt);
        continue;
      }
      if (templateOpen(i))
        ++templates;
      templates = std::max(0, templates - templateCloses(i));
      if (t.is(Kw::operator_))
      {
        markOperatorName(i);
        continue;
      }
      if (t.is(P::LBrace))
      {
        uint32_t close = info[i].match;
        if (roleIs(i, NodeKind::BraceInitExpr))
        {
          list(i, frame, level, stmt);
          i = close;
          continue;
        }
        if (role(i) == FormatLambdaBody)
        {
          lambdaBody(i, frame, level, stmt);
          i = close;
          continue;
        }
        if (roleIs(i, NodeKind::BlockStmt))
        {
          // a function's body (or a test's): the declaration ends with it
          bodyBraces(i, frame, level, Brace::Block, Ctx::Block, level + 1, Rule::IndentBlock);
          uint32_t last = close;
          if (isP(close + 1, P::Semi))
          {
            mark(close + 1, frame, level, stmt, LineKind::Continuation);
            last = close + 1;
          }
          markDefinition(stmt, i, last, ctx);
          return last + 1;
        }
        if (head == Brace::Class)
        {
          bodyBraces(i, frame, level, Brace::Class, Ctx::Class, level + 1, Rule::IndentClass);
          uint32_t last = close;
          while (last + 1 < end && !isP(last + 1, P::Semi) && !isP(last + 1, P::LBrace) && !isP(last + 1, P::RBrace))
            ++last;
          if (isP(last + 1, P::Semi))
            ++last;
          markDefinition(stmt, i, last, ctx);
          head = Brace::None;
          i = close;
          continue;
        }
        if (head == Brace::Enum)
        {
          enumBody(i, frame, level);
          uint32_t last = close;
          if (isP(close + 1, P::Semi))
            last = close + 1;
          markDefinition(stmt, i, last, ctx);
          head = Brace::None;
          i = close;
          continue;
        }
        if (head == Brace::Decl)
        {
          bodyBraces(i, frame, level, Brace::Decl, Ctx::Namespace, level + 1, Rule::IndentNamespace);
          uint32_t last = close;
          if (isP(close + 1, P::Semi))
          {
            mark(close + 1, frame, level, stmt, LineKind::Continuation);
            last = close + 1;
          }
          markDefinition(stmt, i, last, ctx);
          return last + 1;
        }
        // an expression's braces (`T{...}`, a member's initializer `x{1}`)
        if (requiresBrace(i))
          requiresBody(i, frame, level, stmt);
        else
          list(i, frame, level, stmt);
        i = close;
        continue;
      }
      if (t.is(P::Semi))
        return i + 1;
      if (t.is(P::Question))
        ++questions;
      else if (t.is(P::Colon) && templates == 0)
      {
        if (questions > 0)
        {
          info[i].colon = Colon::Ternary;
          --questions;
        }
        else if ((head == Brace::Class || head == Brace::Enum) && !sawParams)
        {
          info[i].colon = Colon::Base;
          listKind = BaseList;
        }
        else if (sawParams)
        {
          info[i].colon = Colon::Init;
          listKind = InitList;
        }
        else
          info[i].colon = ctx == Ctx::Class ? Colon::Bitfield : Colon::Other;
        if (listKind != NoList)
          mark(i, frame, level, stmt, LineKind::Initializer, Rule::IndentInitializer);
      }
      else if (t.is(P::Comma) && templates == 0 && listKind != NoList)
        mark(i, frame, level, stmt, LineKind::Initializer, Rule::IndentInitializer);
    }
    return i;
  }

  void Formatter::bodyBraces(uint32_t open, uint16_t frame, uint16_t level, Brace kind, Ctx innerCtx, uint16_t innerLevel, Rule innerRule)
  {
    uint32_t close = info[open].match;
    info[open].brace = info[close].brace = kind;
    bool oneLine = tokLine(open) == tokLine(close);
    info[open].oneLine = info[close].oneLine = oneLine;
    uint32_t stmt = info[open].marked ? info[open].stmt : open;
    mark(open, frame, level, stmt, LineKind::BraceLine, Rule::IndentBrace);
    mark(close, frame, level, stmt, LineKind::BraceLine, Rule::IndentBrace);
    if (oneLine)
      setOneLine(open);
    body(open + 1, close, frame, innerLevel, innerCtx, innerRule);
  }

  // An initializer list (or an expression's braces): on one line, or one frame whose items are one
  // level in from the line of its {, the writer's breaks kept.
  void Formatter::list(uint32_t open, uint16_t frame, uint16_t level, uint32_t stmt)
  {
    uint32_t close = info[open].match;
    info[open].brace = info[close].brace = Brace::List;
    bool oneLine = tokLine(open) == tokLine(close);
    info[open].oneLine = info[close].oneLine = oneLine;
    mark(open, frame, level, stmt, LineKind::ListOpen);
    if (oneLine)
    {
      setOneLine(open);
      inner(open + 1, close, frame, level, stmt, LineKind::Continuation, false, false);
      mark(close, frame, level, stmt, LineKind::Continuation);
      return;
    }
    uint16_t f = newFrame(frame, open);
    inner(open + 1, close, f, 1, open, LineKind::Item, false, false);
    for (uint32_t k = open + 1; k < close; ++k)
      if (info[k].frame == f && info[k].kind == LineKind::Item)
        info[k].indentRule = Rule::IndentList;
    mark(close, f, 0, open, LineKind::BraceLine, Rule::IndentList);
  }

  // A lambda's body: its statements one level in from the line its [ is on.
  void Formatter::lambdaBody(uint32_t open, uint16_t frame, uint16_t level, uint32_t stmt)
  {
    uint32_t close = info[open].match;
    uint32_t introducer = open;
    if (!lambdaIntroducers.empty())
    {
      introducer = lambdaIntroducers.back();
      lambdaIntroducers.pop_back();
    }
    info[open].brace = info[close].brace = Brace::Lambda;
    bool oneLine = tokLine(open) == tokLine(close);
    info[open].oneLine = info[close].oneLine = oneLine;
    if (oneLine)
    {
      mark(open, frame, level, stmt, LineKind::Continuation);
      mark(close, frame, level, stmt, LineKind::Continuation);
      setOneLine(open);
      body(open + 1, close, frame, uint16_t(level + 1), Ctx::Block, Rule::IndentBlock);
      return;
    }
    uint16_t f = newFrame(frame, introducer);
    mark(open, f, 0, open, LineKind::BraceLine, Rule::IndentBrace);
    mark(close, f, 0, open, LineKind::BraceLine, Rule::IndentBrace);
    body(open + 1, close, f, 1, Ctx::Block, Rule::IndentBlock);
  }

  // A requires expression's requirements: `requires { typename C::key_type; }`, laid out as a body.
  void Formatter::requiresBody(uint32_t open, uint16_t frame, uint16_t level, uint32_t stmt)
  {
    uint32_t close = info[open].match;
    info[open].brace = info[close].brace = Brace::Block;
    bool oneLine = tokLine(open) == tokLine(close);
    info[open].oneLine = info[close].oneLine = oneLine;
    if (oneLine)
    {
      mark(open, frame, level, stmt, LineKind::Continuation);
      mark(close, frame, level, stmt, LineKind::Continuation);
      setOneLine(open);
      inner(open + 1, close, frame, level, stmt, LineKind::Continuation, false, false);
      return;
    }
    mark(open, frame, level, stmt, LineKind::BraceLine, Rule::IndentBrace);
    mark(close, frame, level, stmt, LineKind::BraceLine, Rule::IndentBrace);
    body(open + 1, close, frame, uint16_t(level + 1), Ctx::Block, Rule::IndentBlock);
  }

  void Formatter::enumBody(uint32_t open, uint16_t frame, uint16_t level)
  {
    uint32_t close = info[open].match;
    info[open].brace = info[close].brace = Brace::Enum;
    bool oneLine = tokLine(open) == tokLine(close);
    info[open].oneLine = info[close].oneLine = oneLine;
    uint32_t stmt = info[open].stmt;
    mark(open, frame, level, stmt, LineKind::BraceLine, Rule::IndentBrace);
    mark(close, frame, level, stmt, LineKind::BraceLine, Rule::IndentBrace);
    if (oneLine)
      setOneLine(open);
    inner(open + 1, close, frame, uint16_t(level + 1), open, LineKind::Item, false, false);
    for (uint32_t k = open + 1; k < close; ++k)
      if (info[k].kind == LineKind::Item)
        info[k].indentRule = Rule::IndentList;
  }

  // A definition written over several lines at namespace or class scope has an empty line around it.
  void Formatter::markDefinition(uint32_t start, uint32_t open, uint32_t end, Ctx ctx)
  {
    if ((ctx != Ctx::Namespace && ctx != Ctx::Class) || info[open].oneLine || end >= count())
      return;
    info[start].defStart = true;
    info[end].defEnd = true;
  }

  // --- spacing ---

  bool Formatter::operandEnd(uint32_t i) const
  {
    const Token& t = tok(i);
    TokenKind k = t.getKind();
    if (k == TokenKind::Identifier || k == TokenKind::Number || k == TokenKind::String || k == TokenKind::Char)
      return true;
    if (t.is(Kw::this_) || t.is(Kw::true_) || t.is(Kw::false_) || t.is(Kw::nullptr_))
      return true;
    if (t.is(P::RParen) || t.is(P::RBracket) || templateClose(i))
      return true;
    return (t.is(P::PlusPlus) || t.is(P::MinusMinus)) && postfixAt(i);
  }

  bool Formatter::postfixAt(uint32_t i) const
  {
    return (isP(i, P::PlusPlus) || isP(i, P::MinusMinus)) && i > 0 && !info[i].stmtStart && operandEnd(i - 1);
  }

  bool Formatter::typeOp(uint32_t i) const
  {
    if (!(isP(i, P::Star) || isP(i, P::Amp) || isP(i, P::AndAnd) || isP(i, P::Caret)))
      return false;
    if (info[i].opName)
      return false;
    // what follows only a type's: `T*)`, `T*,`, `Vector<T*>`, `T*()` (a template argument read both
    // as a type and as an expression has the expression's role)
    {
      const Token& next = tok(i + 1);
      bool afterOperand = i > 0 && operandEnd(i - 1) && !isP(i - 1, P::RParen);
      if (afterOperand && !isP(i, P::Caret) && (next.is(P::RParen) || next.is(P::Comma) || next.is(P::Greater) || templateClose(i + 1) ||
                                                (next.is(P::LParen) && isP(i + 2, P::RParen))))
        return true;
    }
    if (roleIs(i, NodeKind::PointerType) || roleIs(i, NodeKind::LRefType) || roleIs(i, NodeKind::RRefType) ||
        (isP(i, P::Caret) && roleIs(i, NodeKind::NamedType)))
      return true;
    if (role(i) != 0 || isP(i, P::Caret))
      return false;
    // no role (a function type in a template argument, `Function<void(const String&)>`): a type's when
    // what follows cannot follow a binary operator, or a type keyword comes before
    const Token& next = tok(i + 1);
    if (next.is(P::RParen) || next.is(P::Comma) || next.is(P::Ellipsis) || templateClose(i + 1) || next.is(P::Greater))
      return true;
    const Token& before = tok(i - 1);
    return i > 0 && before.getKind() == TokenKind::Keyword && (before.is(Kw::const_) || before.is(Kw::void_) || before.is(Kw::char_) ||
           before.is(Kw::bool_) || before.is(Kw::int_) || before.is(Kw::auto_) || before.is(Kw::float_) || before.is(Kw::double_));
  }

  bool Formatter::unaryAt(uint32_t i) const
  {
    const Token& t = tok(i);
    if (t.getKind() != TokenKind::Punctuator)
      return false;
    P p = P(t.id);
    bool candidate = p == P::Plus || p == P::Minus || p == P::Bang || p == P::Tilde || p == P::Star || p == P::Amp || p == P::AndAnd ||
                     p == P::PlusPlus || p == P::MinusMinus;
    if (!candidate)
      return false;
    if ((p == P::PlusPlus || p == P::MinusMinus) && postfixAt(i))
      return false;
    if (roleIs(i, NodeKind::UnaryExpr))
      return true;
    if (typeOp(i) || roleIs(i, NodeKind::BinaryExpr) || roleIs(i, NodeKind::FoldExpr))
      return false;
    if (info[i].stmtStart || p == P::PlusPlus || p == P::MinusMinus)
      return true; // (a statement starts with no binary operator; a postfix one was answered above)
    if (p == P::Bang || p == P::Tilde)
      return true;
    // (another node's role: a statement's or declaration's that starts here, `--this->count;`)
    if (role(i) != 0 || p == P::PlusPlus || p == P::MinusMinus || p == P::Plus || p == P::Minus)
      return !(i > 0 && operandEnd(i - 1));
    return false;
  }

  bool Formatter::binaryAt(uint32_t i) const
  {
    const Token& t = tok(i);
    if (t.getKind() != TokenKind::Punctuator)
      return false;
    P p = P(t.id);
    if (p == P::Comma || p == P::Semi)
      return false;
    if (p == P::Elvis || p == P::Spaceship)
      return true;
    if (roleIs(i, NodeKind::BinaryExpr) || roleIs(i, NodeKind::ConditionalExpr) || roleIs(i, NodeKind::FoldExpr))
      return true;
    Colon c = info[i].colon;
    if (c == Colon::Ternary || c == Colon::Range || c == Colon::Base || c == Colon::Init || c == Colon::Bitfield || c == Colon::Other)
      return true;
    if (info[i].opName || typeOp(i) || roleIs(i, NodeKind::UnaryExpr) || role(i) >= FormatTemplateOpen)
      return false;
    // No role, or another node's made at the same token (check(a == b) calls __kppEqual there): by the
    // token and what comes before it.
    bool other = role(i) != 0;
    switch (p)
    {
      case P::Assign:
        return !(info[i].capture && i > 0 && (isP(i - 1, P::LBracket) || isP(i - 1, P::Comma)));
      case P::PlusAssign: case P::MinusAssign: case P::StarAssign: case P::SlashAssign: case P::PercentAssign: case P::AmpAssign:
      case P::PipeAssign: case P::CaretAssign: case P::ShlAssign: case P::ShrAssign: case P::EqEq: case P::NotEq: case P::LessEq:
      case P::GreaterEq: case P::OrOr: case P::Spaceship: case P::Elvis: case P::Question:
        return true;
      case P::Plus: case P::Minus: case P::Slash: case P::Percent: case P::Pipe:
        return i > 0 && operandEnd(i - 1);
      case P::Less: case P::Greater: case P::Shl: case P::Shr: case P::AndAnd: case P::Star: case P::Amp: case P::Caret:
        return other && i > 0 && operandEnd(i - 1);
      default:
        return false;
    }
  }

  Formatter::Space Formatter::spaceBetween(uint32_t a, uint32_t b) const
  {
    const Token& A = tok(a);
    const Token& B = tok(b);
    const Info& ia = info[a];
    const Info& ib = info[b];
    // operator names: `operator==(`, `operator()(`, `operator new`
    if (A.is(Kw::operator_))
      return {isWord(b) ? 1 : 0, Rule::SpaceOther};
    if (ia.opName && ib.opName)
      return {isWord(a) && isWord(b) ? 1 : 0, Rule::SpaceOther};
    if (ia.opName && B.is(P::LParen))
      return {0, Rule::SpaceCallParen};
    if (A.is(P::LParen) || A.is(P::LBracket))
      return {0, Rule::SpaceInsideParens};
    if (B.is(P::RParen) || B.is(P::RBracket))
      return {0, Rule::SpaceInsideParens};
    if (B.is(P::Comma))
      return {0, Rule::SpaceComma};
    if (B.is(P::Semi))
      return {0, Rule::SpaceSemicolon};
    if (A.is(P::Comma))
      return {1, Rule::SpaceComma};
    if (A.is(P::Semi))
      return {1, Rule::SpaceSemicolon};
    if (ib.colon == Colon::Label || ib.colon == Colon::Access)
      return {0, Rule::SpaceLabelColon};
    if (ia.colon == Colon::Label || ia.colon == Colon::Access)
      return {1, Rule::SpaceLabelColon};
    // braces
    if (B.is(P::LBrace))
    {
      if (isBody(ib.brace))
        return {1, Rule::SpaceBeforeBrace};
      if (ib.brace == Brace::List)
      {
        if (A.getKind() == TokenKind::Identifier || templateClose(a))
          return {0, Rule::SpaceBeforeBrace};
        if (A.getKind() == TokenKind::Keyword)
          return {1, Rule::SpaceBeforeBrace};
        if (A.is(P::LBrace) && ia.brace == Brace::List)
          return {0, Rule::SpaceBracedList};
      }
    }
    if (A.is(P::LBrace))
    {
      if (ia.brace == Brace::List)
        return {0, Rule::SpaceBracedList};
      if (isBody(ia.brace))
        return {B.is(P::RBrace) ? 0 : 1, Rule::SpaceOneLiner};
    }
    if (B.is(P::RBrace))
    {
      if (ib.brace == Brace::List)
        return {0, Rule::SpaceBracedList};
      if (isBody(ib.brace))
        return {1, Rule::SpaceOneLiner};
    }
    // scope and member access
    if (B.is(P::ColonColon))
    {
      if (A.getKind() == TokenKind::Keyword || binaryAt(a))
        return {1, Rule::SpaceMember};
      return {0, Rule::SpaceMember};
    }
    if (A.is(P::ColonColon))
      return {0, Rule::SpaceMember};
    auto member = [&](const Token& t) { return t.is(P::Dot) || t.is(P::Arrow) || t.is(P::SafeArrow) || t.is(P::DotStar) || t.is(P::ArrowStar); };
    if (member(B))
    {
      if (role(b) == FormatTrailingArrow)
        return {1, Rule::SpaceBinary};
      return {0, Rule::SpaceMember};
    }
    if (member(A))
    {
      if (role(a) == FormatTrailingArrow)
        return {1, Rule::SpaceBinary};
      return {0, Rule::SpaceMember};
    }
    // `~Name`: a destructor's name, or bitwise not of a name
    if (A.is(P::Tilde))
      return {0, Rule::SpaceUnary};
    // `(i32)x`: nothing after a cast's )
    if (A.is(P::RParen) && roleIs(ia.match, NodeKind::CastExpr))
      return {0, Rule::SpaceUnary};
    if (B.is(P::Ellipsis))
      return {binaryAt(a) ? 1 : 0, Rule::SpaceOther};
    if (A.is(P::Ellipsis))
      return {isWord(b) || binaryAt(b) ? 1 : 0, Rule::SpaceOther};
    // template brackets
    if (templateOpen(a) || templateOpen(b) || templateClose(b))
      return {0, Rule::SpaceTemplate};
    if (templateClose(a))
      return {isWord(b) || binaryAt(b) ? 1 : 0, Rule::SpaceTemplate};
    // pointers and references with the type
    if (typeOp(b))
      return {0, Rule::SpacePointer};
    // a declarator in parentheses: `T (*)(int)`, `const char* (*f)(void*)`
    if (B.is(P::LParen) && typeOp(b + 1) && (A.getKind() == TokenKind::Identifier || A.getKind() == TokenKind::Keyword || templateClose(a) || typeOp(a)))
      return {1, Rule::SpacePointer};
    if (typeOp(a))
    {
      if (a > 0 && (isP(a - 1, P::LParen) || isP(a - 1, P::ColonColon)))
        return {0, Rule::SpacePointer};
      return {isWord(b) || binaryAt(b) ? 1 : 0, Rule::SpacePointer};
    }
    // lambda captures: [=], [&], [&x], [&, x]
    if (ia.capture && (A.is(P::Amp) || (A.is(P::Assign) && a > 0 && (isP(a - 1, P::LBracket) || isP(a - 1, P::Comma)))))
      return {0, Rule::SpaceOther};
    // unary and binary operators
    if (postfixAt(b))
      return {0, Rule::SpaceUnary};
    if (unaryAt(a))
    {
      // (never `- -x` into `--x`)
      if ((A.is(P::Minus) && (B.is(P::Minus) || B.is(P::MinusMinus))) || (A.is(P::Plus) && (B.is(P::Plus) || B.is(P::PlusPlus))))
        return {1, Rule::SpaceUnary};
      return {0, Rule::SpaceUnary};
    }
    if (binaryAt(a) || binaryAt(b))
      return {1, Rule::SpaceBinary};
    // parentheses and brackets
    if (B.is(P::LParen))
    {
      if (A.getKind() == TokenKind::Identifier || A.is(P::RParen) || A.is(P::RBracket) || A.is(P::RBrace) || templateClose(a))
        return {0, Rule::SpaceCallParen};
      if (A.getKind() == TokenKind::Keyword)
      {
        switch (Kw(A.id))
        {
          case Kw::if_: case Kw::for_: case Kw::while_: case Kw::switch_: case Kw::catch_: case Kw::when_: case Kw::constexpr_:
          case Kw::return_: case Kw::throw_: case Kw::new_: case Kw::delete_: case Kw::co_await_: case Kw::requires_: case Kw::and_:
          case Kw::or_: case Kw::not_: case Kw::consteval_: case Kw::else_: case Kw::do_: case Kw::case_:
            return {1, Rule::SpaceKeywordParen};
          case Kw::sizeof_: case Kw::alignof_: case Kw::decltype_: case Kw::noexcept_: case Kw::typeid_: case Kw::static_assert_:
          case Kw::alignas_: case Kw::explicit_: case Kw::static_cast_: case Kw::const_cast_: case Kw::reinterpret_cast_:
          case Kw::dynamic_cast_: case Kw::this_: case Kw::asm_: case Kw::bool_: case Kw::char_:
          case Kw::char8_t_: case Kw::char16_t_: case Kw::char32_t_: case Kw::wchar_t_: case Kw::int_: case Kw::short_: case Kw::long_:
          case Kw::float_: case Kw::double_: case Kw::void_: case Kw::unsigned_: case Kw::signed_: case Kw::auto_: case Kw::operator_:
            return {0, Rule::SpaceCallParen};
          default:
            return {-1, Rule::None};
        }
      }
      return {-1, Rule::None};
    }
    if (B.is(P::LBracket))
    {
      if (isP(b + 1, P::LBracket))
        return {-1, Rule::None}; // an attribute `[[...]]`
      if (A.getKind() == TokenKind::Identifier || A.getKind() == TokenKind::String || A.is(P::RParen) || A.is(P::RBracket) || templateClose(a))
        return {0, Rule::SpaceCallParen};
      if (A.getKind() == TokenKind::Keyword && !A.is(Kw::delete_) && !A.is(Kw::new_))
        return {1, Rule::SpaceWords};
      return {-1, Rule::None};
    }
    if (isWord(a) && isWord(b))
      return {1, Rule::SpaceWords};
    if ((A.is(P::RParen) || A.is(P::RBracket) || A.is(P::RBrace)) && isWord(b))
      return {1, Rule::SpaceWords};
    if (unaryAt(b) && isWord(a))
      return {1, Rule::SpaceUnary};
    if (B.is(P::Tilde) && isWord(a))
      return {1, Rule::SpaceWords};
    return {-1, Rule::None};
  }

  // --- indentation ---

  int Formatter::lineIndent(uint32_t i, Rule& rule) const
  {
    const Info& in = info[i];
    int base = frames[in.frame].base;
    int statementBase = base + in.level * indentWidth;
    rule = in.indentRule;
    switch (in.kind)
    {
      case LineKind::Item:
        // an item of a list whose { has content after it on its line: under that content
        for (size_t k = parenStack.size(); k-- > 0;)
          if (parenStack[k].open == in.stmt)
          {
            if (parenStack[k].content)
            {
              rule = Rule::IndentList;
              return parenStack[k].alignCol;
            }
            break;
          }
        return statementBase;
      case LineKind::Statement: case LineKind::Label: case LineKind::Access: case LineKind::BraceLine:
        return statementBase;
      case LineKind::Initializer:
        rule = Rule::IndentInitializer;
        return statementBase + indentWidth;
      case LineKind::Continuation: case LineKind::ListOpen:
        break;
    }
    for (size_t k = parenStack.size(); k-- > 0;)
    {
      const Paren& p = parenStack[k];
      if (p.open < in.stmt)
        break;
      rule = Rule::IndentContinuationParen;
      if (i == p.close)
        return p.lineIndent;
      // inside an argument: under its start (what it continues); a new argument: under the first
      if (p.argCol >= 0 && !isP(i - 1, P::Comma) && uint32_t(i - 1) != p.open)
        return p.argCol;
      if (p.content)
        return p.alignCol;
      rule = Rule::IndentContinuation;
      return p.lineIndent + indentWidth;
    }
    rule = Rule::IndentContinuation;
    if (in.kind == LineKind::ListOpen)
      return statementBase;
    // under what it continues: the value after `return` or an assignment, else one level in (a
    // method chain's `.f()` too)
    if (exprCol[in.stmt] >= 0 && !isP(i, P::Dot) && !isP(i, P::Arrow) && !isP(i, P::SafeArrow))
      return exprCol[in.stmt];
    return statementBase + indentWidth;
  }

  // A comment on its own line: indented as the code after it (inside the body when that is a }).
  int Formatter::commentIndent(size_t k, Rule& rule) const
  {
    size_t next = k + 1;
    while (next < items.size() && items[next].tok < 0)
      ++next;
    rule = Rule::IndentComment;
    if (next >= items.size())
      return 0;
    uint32_t t = uint32_t(items[next].tok);
    Rule ignored;
    int indent = lineIndent(t, ignored);
    if (isP(t, P::RBrace) && info[t].kind == LineKind::BraceLine)
      indent += indentWidth;
    return indent;
  }

  // The break before item k, by the rules alone: soft when no rule decides it (a line break inside a
  // statement, the writer's in step 2, the wrapping's with a limit).
  Formatter::Decision Formatter::decide(size_t k) const
  {
    if (k == 0)
      return {false, Rule::BlankStart, false};
    const Item& it = items[k];
    const Item& prev = items[k - 1];
    bool inputBreak = gapBefore(k).find('\n') != std::string_view::npos;
    if (prev.tok < 0 && prev.lineComment)
      return {true, Rule::None, false};
    if (prev.tok < 0 || it.tok < 0)
      return {inputBreak, Rule::None, false}; // a comment's place is kept, and the code after a block comment
    int32_t prevTok = prevTokOf[k];
    if (prevTok < 0)
      return {inputBreak, Rule::None, false};
    uint32_t i = uint32_t(it.tok), pt = uint32_t(prevTok);
    const Info& in = info[i];
    const Info& pin = info[pt];
    bool bodyOpen = isP(i, P::LBrace) && isBody(in.brace) && !in.oneLine;
    bool afterBodyOpen = isP(pt, P::LBrace) && isBody(pin.brace) && !pin.oneLine;
    bool bodyClose = isP(i, P::RBrace) && isBody(in.brace) && !in.oneLine;
    bool afterBodyClose = isP(pt, P::RBrace) && isBody(pin.brace) && !pin.oneLine;
    if (in.inOneLine)
      return {false, Rule::None, false};
    if (bodyOpen)
      return {true, Rule::BraceOwnLine, false};
    if (afterBodyOpen)
      return {true, Rule::BraceContentsNextLine, false};
    if (bodyClose)
      return {true, Rule::CloseBraceOwnLine, false};
    if ((in.elseKw || in.catchKw) && isP(pt, P::RBrace) && pin.oneLine)
      return {inputBreak, Rule::None, false};
    if (in.elseKw || in.catchKw)
      return {true, afterBodyClose ? Rule::CloseBraceElse : Rule::BracelessBodyOwnLine, false};
    if (in.elseIf)
      return {false, Rule::ElseIfOneLine, false};
    if (in.doWhile && afterBodyClose)
      return {false, Rule::CloseBraceJoin, false};
    if (pin.colon == Colon::Access)
      return {true, Rule::AccessLabelOwnLine, false};
    if (in.stmtStart)
    {
      if (in.braceless)
        return {true, Rule::BracelessBodyOwnLine, false};
      if (pin.colon == Colon::Label && !inputBreak)
        return {false, Rule::None, false};
      return {true, Rule::OneStatementPerLine, false};
    }
    if (afterBodyClose || in.attached)
      return {false, Rule::CloseBraceJoin, false};
    return {inputBreak, Rule::None, in.kind == LineKind::Continuation};
  }

  // The spaces before item k on its line (n < 0: no rule covers them, the writer's are kept).
  Formatter::Space Formatter::spacesBefore(size_t k, bool& unclassified) const
  {
    unclassified = false;
    const Item& it = items[k];
    std::string_view oldGap = gapBefore(k);
    if (it.tok < 0)
    {
      if (it.lineComment)
        return {1, Rule::SpaceTrailingComment};
      return {oldGap.empty() ? 0 : 1, Rule::SpaceOther};
    }
    if (items[k - 1].tok < 0)
      return {oldGap.empty() ? 0 : 1, Rule::SpaceOther};
    Space s = spaceBetween(uint32_t(items[k - 1].tok), uint32_t(it.tok));
    if (s.n == 0 && joins(uint32_t(items[k - 1].tok), uint32_t(it.tok)))
      s.n = 1; // never two tokens into one (`* =`, `- -`, `> >`)
    if (s.n >= 0)
      return s;
    unclassified = true;
    bool newline = oldGap.find('\n') != std::string_view::npos;
    int n = newline ? 1 : int(oldGap.size());
    if (!newline && n > 1)
      return {1, Rule::SpaceOther};
    return {n, Rule::None};
  }

  // How loosely an operator binds, where a long statement breaks (0 loosest), -1 for no break point.
  int Formatter::breakPrecedence(uint32_t t) const
  {
    if (tok(t).getKind() == TokenKind::String && t + 1 < count() && tok(t + 1).getKind() == TokenKind::String)
      return 13;
    if (isP(t, P::Comma) || isP(t, P::Semi))
      return 0;
    Colon c = info[t].colon;
    if (c == Colon::Range)
      return 1;
    if (c == Colon::Ternary || isP(t, P::Question) || isP(t, P::Elvis))
      return 2;
    if (!binaryAt(t))
      return -1;
    switch (P(tok(t).id))
    {
      case P::Assign: case P::PlusAssign: case P::MinusAssign: case P::StarAssign: case P::SlashAssign: case P::PercentAssign:
      case P::AmpAssign: case P::PipeAssign: case P::CaretAssign: case P::ShlAssign: case P::ShrAssign:
        return 1;
      case P::OrOr: return 3;
      case P::AndAnd: return 4;
      case P::Pipe: return 5;
      case P::Caret: return 6;
      case P::Amp: return 7;
      case P::EqEq: case P::NotEq: return 8;
      case P::Less: case P::Greater: case P::LessEq: case P::GreaterEq: case P::Spaceship: return 9;
      case P::Shl: case P::Shr: return 10;
      case P::Plus: case P::Minus: return 11;
      case P::Star: case P::Slash: case P::Percent: return 12;
      default: return -1;
    }
  }

  // Plans the line that starts at item k (at startCol): the items up to the next break no wrapping can
  // remove, joined where they fit, else broken (wrap).
  void Formatter::planSegment(size_t k, int startCol)
  {
    size_t e = k;
    while (e + 1 < items.size())
    {
      Decision d = decide(e + 1);
      if (d.brk && !d.soft)
        break;
      ++e;
    }
    planFrom = k;
    planTo = e;
    segWidth.assign(e - k + 1, 0);
    segSpace.assign(e - k + 1, 0);
    segSoft.assign(e - k + 1, 0);
    bool multiline = false;
    for (size_t j = k; j <= e; ++j)
    {
      std::string_view body = text.substr(items[j].begin, items[j].end - items[j].begin);
      if (body.find('\n') != std::string_view::npos)
        multiline = true;
      segWidth[j - k] = items[j].tok < 0 && items[j].lineComment ? 0 : widthOf(body); // (a trailing comment may pass the limit)
      if (j > k)
      {
        Decision d = decide(j);
        bool unclassified;
        segSpace[j - k] = d.brk && !d.soft ? 1 : spacesBefore(j, unclassified).n;
        segSoft[j - k] = d.soft;
        plan[j] = d.soft ? 2 : 0;
      }
    }
    if (multiline)
    {
      // a raw string or comment over several lines: the writer's breaks stay
      for (size_t j = k + 1; j <= e; ++j)
        if (segSoft[j - k])
          plan[j] = gapBefore(j).find('\n') != std::string_view::npos ? 1 : 2;
      return;
    }
    // continued under the value after `return` or the first assignment, as the emitter does
    int contCol = startCol + indentWidth, col = startCol, depth = 0;
    for (size_t j = k; j <= e; ++j)
    {
      if (j > k)
        col += segSpace[j - k];
      if (items[j].tok >= 0)
      {
        uint32_t t = uint32_t(items[j].tok);
        if (isP(t, P::RParen) || isP(t, P::RBracket) || isP(t, P::RBrace))
          --depth;
        if (depth == 0 && j < e && items[j + 1].tok >= 0 && (isKw(t, Kw::return_) || isKw(t, Kw::throw_) || assignment(t)))
        {
          contCol = col + segWidth[j - k] + segSpace[j + 1 - k];
          break;
        }
        if (isP(t, P::LParen) || isP(t, P::LBracket) || isP(t, P::LBrace))
          ++depth;
      }
      col += segWidth[j - k];
    }
    wrap(k, e, startCol, contCol);
  }

  // Breaks items [a, b] (laid out from startCol, continued at contCol) where they pass the limit: after
  // each of their loosest operators outside brackets; else the arguments of the bracket that passes
  // the limit one to a line, after its (.
  void Formatter::wrap(size_t a, size_t b, int startCol, int contCol)
  {
    if (a > b)
      return;
    size_t base = planFrom;
    auto soft = [&](size_t j) { return j > base && j <= planTo && segSoft[j - base]; };
    auto opens = [&](size_t j)
    {
      if (items[j].tok < 0)
        return 0;
      uint32_t t = uint32_t(items[j].tok);
      return (isP(t, P::LParen) || isP(t, P::LBracket) || isP(t, P::LBrace) || templateOpen(t)) ? 1 : 0;
    };
    auto closes = [&](size_t j)
    {
      if (items[j].tok < 0)
        return 0;
      uint32_t t = uint32_t(items[j].tok);
      return (isP(t, P::RParen) || isP(t, P::RBracket) || isP(t, P::RBrace)) ? 1 : templateCloses(t);
    };
    // flat columns
    std::vector<int> after(b - a + 1);
    int col = startCol;
    size_t crossing = SIZE_MAX;
    for (size_t j = a; j <= b; ++j)
    {
      if (j > a)
        col += segSpace[j - base];
      col += segWidth[j - base];
      after[j - a] = col;
      if (col > columns && crossing == SIZE_MAX)
        crossing = j;
    }
    if (crossing == SIZE_MAX)
      return;
    // the loosest operators outside brackets; an assignment (or a range for's colon) last, after
    // what its value offers: `x = a ?` stays on the first line, the rest under the value
    int lowest = INT32_MAX, depth = 0;
    bool assignments = false;
    for (size_t j = a; j <= b; ++j)
    {
      depth -= closes(j);
      if (depth == 0 && j < b && soft(j + 1) && items[j].tok >= 0)
      {
        int p = breakPrecedence(uint32_t(items[j].tok));
        if (p == 1)
          assignments = true;
        else if (p >= 0)
          lowest = std::min(lowest, p);
      }
      depth += opens(j);
    }
    auto breakAt = [&](int precedence)
    {
      size_t start = a;
      int pieceCol = startCol;
      depth = 0;
      for (size_t j = a; j <= b; ++j)
      {
        depth -= closes(j);
        if (depth == 0 && j < b && soft(j + 1) && items[j].tok >= 0 && breakPrecedence(uint32_t(items[j].tok)) == precedence)
        {
          plan[j + 1] = 1;
          wrap(start, j, pieceCol, contCol);
          start = j + 1;
          pieceCol = contCol;
        }
        depth += opens(j);
      }
      wrap(start, b, pieceCol, contCol);
    };
    if (lowest != INT32_MAX)
    {
      breakAt(lowest);
      return;
    }
    // the bracket that passes the limit (else the last one)
    size_t open = SIZE_MAX, close = SIZE_MAX, lastOpen = SIZE_MAX, lastClose = SIZE_MAX;
    depth = 0;
    for (size_t j = a; j <= b; ++j)
    {
      depth -= closes(j);
      if (depth == 0 && items[j].tok >= 0 && (isP(uint32_t(items[j].tok), P::LParen) || isP(uint32_t(items[j].tok), P::LBracket)))
      {
        uint32_t m = info[items[j].tok].match;
        size_t mj = j;
        while (mj <= b && items[mj].tok != int32_t(m))
          ++mj;
        if (mj <= b && mj > j + 1)
        {
          lastOpen = j, lastClose = mj;
          if (crossing > j && crossing <= mj && open == SIZE_MAX)
            open = j, close = mj;
        }
      }
      depth += opens(j);
    }
    if (open == SIZE_MAX)
      open = lastOpen, close = lastClose;
    if (open == SIZE_MAX)
    {
      if (assignments)
        breakAt(1);
      return;
    }
    int alignCol = after[open - a];
    std::vector<size_t> starts{open + 1};
    depth = 0;
    for (size_t j = open + 1; j < close; ++j)
    {
      depth -= closes(j);
      if (depth == 0 && items[j].tok >= 0 && isP(uint32_t(items[j].tok), P::Comma) && j + 1 < close)
        starts.push_back(j + 1);
      depth += opens(j);
    }
    if (starts.size() >= 2)
    {
      for (size_t s = 1; s < starts.size(); ++s)
        if (soft(starts[s]))
          plan[starts[s]] = 1;
      for (size_t s = 0; s < starts.size(); ++s)
        wrap(starts[s], s + 1 < starts.size() ? starts[s + 1] - 1 : close - 1, alignCol, alignCol);
    }
    else
      wrap(open + 1, close - 1, alignCol, alignCol);
    if (close < b)
    {
      size_t last = starts.back();
      int tail = alignCol;
      for (size_t j = last; j <= close; ++j)
        tail += (j > last ? segSpace[j - base] : 0) + segWidth[j - base];
      wrap(close + 1, b, tail + segSpace[close + 1 - base], contCol);
    }
  }

  struct GapShape
  {
    int newlines = 0;
    bool trailing = false; // a space or tab before a line end
    bool tab = false;      // a tab in the last line's indentation
    bool otherEnd = false; // a line end other than the file's
  };

  GapShape shapeOf(std::string_view gap, bool crlf)
  {
    GapShape s;
    for (size_t i = 0; i < gap.size(); ++i)
    {
      char c = gap[i];
      if (c == '\n')
      {
        ++s.newlines;
        bool cr = i > 0 && gap[i - 1] == '\r';
        if (cr != crlf)
          s.otherEnd = true;
        size_t j = cr ? i - 1 : i;
        if (j > 0 && (gap[j - 1] == ' ' || gap[j - 1] == '\t'))
          s.trailing = true;
      }
    }
    size_t last = gap.rfind('\n');
    std::string_view indent = last == std::string_view::npos ? gap : gap.substr(last + 1);
    s.tab = indent.find('\t') != std::string_view::npos;
    return s;
  }

  // Code points, the width a limit counts.
  int widthOf(std::string_view s)
  {
    int w = 0;
    for (unsigned char c : s)
      if ((c & 0xC0) != 0x80)
        ++w;
    return w;
  }

  // Comments of a text in order, each block comment's lines after the first without their indentation.
  bool collectComments(std::string_view text, const std::vector<Token>& toks, std::vector<std::string>& out)
  {
    uint32_t from = 0;
    for (size_t t = 0; t <= toks.size(); ++t)
    {
      uint32_t to = t < toks.size() && toks[t].getKind() != TokenKind::End ? toks[t].offset : uint32_t(text.size());
      for (uint32_t p = from; p < to;)
      {
        if (text[p] == '/' && p + 1 < to && text[p + 1] == '/')
        {
          uint32_t e = p;
          while (e < to && text[e] != '\n')
            ++e;
          uint32_t last = e;
          while (last > p + 2 && (text[last - 1] == '\r' || text[last - 1] == ' ' || text[last - 1] == '\t'))
            --last;
          out.emplace_back(text.substr(p, last - p));
          p = e;
        }
        else if (text[p] == '/' && p + 1 < to && text[p + 1] == '*')
        {
          size_t e = text.find("*/", p + 2);
          e = e == std::string_view::npos ? to : std::min<size_t>(e + 2, to);
          std::string c;
          bool lineStart = false;
          for (size_t q = p; q < e; ++q)
          {
            char ch = text[q];
            if (ch == '\n')
            {
              while (!c.empty() && c.back() == '\r')
                c.pop_back();
              lineStart = true;
              c += '\n';
              continue;
            }
            if (lineStart && (ch == ' ' || ch == '\t'))
              continue;
            lineStart = false;
            c += ch;
          }
          out.push_back(c);
          p = uint32_t(e);
        }
        else
          ++p;
      }
      if (t >= toks.size() || toks[t].getKind() == TokenKind::End)
        break;
      from = toks[t].offset + toks[t].length;
    }
    return true;
  }

  FormatOutcome Formatter::run()
  {
    FormatOutcome outcome;
    bodyStart = text.substr(0, 3) == "\xEF\xBB\xBF" ? 3 : 0;
    lineStarts.push_back(0);
    for (size_t i = 0; i < text.size(); ++i)
      if (text[i] == '\n')
        lineStarts.push_back(uint32_t(i + 1));
    size_t firstEnd = text.find('\n');
    bool crlf = firstEnd != std::string_view::npos && firstEnd > 0 && text[firstEnd - 1] == '\r';
    std::string lineEnd = crlf ? "\r\n" : "\n";

    // items: the tokens and the comments between them
    uint32_t n = count();
    uint32_t from = uint32_t(bodyStart);
    for (uint32_t t = 0; t <= n; ++t)
    {
      uint32_t to = t < n ? toks[t].offset : uint32_t(text.size());
      for (uint32_t p = from; p < to;)
      {
        char c = text[p];
        if (c == '/' && p + 1 < to && text[p + 1] == '/')
        {
          uint32_t e = p;
          while (e < to && text[e] != '\n')
            ++e;
          uint32_t last = e;
          while (last > p && text[last - 1] == '\r')
            --last;
          if (last > p && text[last - 1] == '\\')
          {
            outcome.error = "a // comment continued by a line splice";
            return outcome;
          }
          while (last > p + 2 && (text[last - 1] == ' ' || text[last - 1] == '\t'))
            --last;
          items.push_back({p, last, -1, true});
          p = last;
        }
        else if (c == '/' && p + 1 < to && text[p + 1] == '*')
        {
          size_t e = text.find("*/", p + 2);
          if (e == std::string_view::npos || e + 2 > to)
          {
            outcome.error = "an unterminated comment";
            return outcome;
          }
          items.push_back({p, uint32_t(e + 2), -1, false});
          p = uint32_t(e + 2);
        }
        else if (c == '\\')
        {
          outcome.error = "a line splice between tokens";
          return outcome;
        }
        else if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v')
          ++p;
        else
        {
          outcome.error = "an unexpected character between tokens";
          return outcome;
        }
      }
      if (t < n)
      {
        items.push_back({toks[t].offset, toks[t].offset + toks[t].length, int32_t(t), false});
        from = toks[t].offset + toks[t].length;
      }
    }

    prevTokOf.assign(items.size(), -1);
    for (size_t k = 1; k < items.size(); ++k)
      prevTokOf[k] = items[k - 1].tok >= 0 ? items[k - 1].tok : prevTokOf[k - 1];
    plan.assign(items.size(), 0);
    exprCol.assign(n + 1, -1);

    // brackets
    info.assign(n, Info());
    std::vector<uint32_t> stack;
    for (uint32_t t = 0; t < n; ++t)
    {
      if (isP(t, P::LParen) || isP(t, P::LBracket) || isP(t, P::LBrace))
        stack.push_back(t);
      else if (isP(t, P::RParen) || isP(t, P::RBracket) || isP(t, P::RBrace))
      {
        if (stack.empty())
        {
          outcome.error = "unbalanced brackets";
          return outcome;
        }
        info[t].match = stack.back();
        info[stack.back()].match = t;
        stack.pop_back();
      }
    }
    if (!stack.empty())
    {
      outcome.error = "unbalanced brackets";
      return outcome;
    }

    // the walk
    frames.push_back({0, 0});
    body(0, n, 0, 0, Ctx::Namespace, Rule::IndentNamespace);
    for (uint32_t t = 0; t < n; ++t)
      if (!info[t].marked)
      {
        outcome.error = "internal: token " + std::to_string(t) + " (" + std::string(spelling(t)) + ", line " + std::to_string(tokLine(t)) +
                        ") is not laid out";
        return outcome;
      }
    // statements at namespace scope in the file itself are not indented (a namespace's are)
    for (uint32_t t = 0; t < n; ++t)
      if (info[t].frame == 0 && info[t].level == 0 && info[t].indentRule == Rule::IndentNamespace)
        info[t].indentRule = Rule::IndentBlock;

    // the definitions' empty lines: before the comments right above one, and after it
    std::vector<uint8_t> blankBefore(items.size() + 1, 0);
    auto breakBefore = [&](size_t k) { return k == 0 || std::string_view(text.data() + items[k - 1].end, items[k].begin - items[k - 1].end).find('\n') != std::string_view::npos; };
    auto blankGap = [&](size_t k)
    {
      std::string_view g(text.data() + items[k - 1].end, items[k].begin - items[k - 1].end);
      return std::count(g.begin(), g.end(), '\n') >= 2;
    };
    for (size_t k = 0; k < items.size(); ++k)
    {
      if (items[k].tok < 0)
        continue;
      const Info& in = info[items[k].tok];
      if (in.defStart)
      {
        size_t s = k;
        while (s > 0 && items[s - 1].tok < 0 && breakBefore(s - 1) && !blankGap(s))
          --s;
        blankBefore[s] = 1;
      }
      if (in.defEnd)
      {
        size_t e = k + 1;
        while (e < items.size() && items[e].tok < 0 && !breakBefore(e))
          ++e;
        if (e < items.size())
          blankBefore[e] = 1;
      }
    }

    // writing
    std::string out;
    out.reserve(text.size() + text.size() / 16);
    out.append(text.substr(0, bodyStart));
    uint32_t outLine = 1;
    int outCol = 0, curIndent = 0;
    int32_t prevTok = -1;
    auto change = [&](Rule rule, size_t k)
    {
      outcome.changes.push_back({uint8_t(rule), line(items[k].begin), outLine});
    };
    if (columns > 0 && !items.empty())
      planSegment(0, 0);
    for (size_t k = 0; k < items.size(); ++k)
    {
      const Item& it = items[k];
      uint32_t gapBegin = k == 0 ? uint32_t(bodyStart) : items[k - 1].end;
      std::string_view oldGap = text.substr(gapBegin, it.begin - gapBegin);
      GapShape old = shapeOf(oldGap, crlf);
      std::string gap;
      Rule breakRule = Rule::None, blankRule = Rule::None, indentRule = Rule::None, spaceRule = Rule::None;
      Decision d = decide(k);
      bool brk = d.brk;
      breakRule = d.rule;
      bool planned = columns > 0 && d.soft && k > planFrom && k <= planTo && plan[k] != 0;
      if (planned)
      {
        brk = plan[k] == 1;
        bool inputBreak = old.newlines > 0;
        breakRule = brk == inputBreak ? Rule::None : brk ? Rule::WrapBreak : Rule::WrapJoin;
      }
      if (brk)
      {
        int blank = old.newlines >= 2 ? 1 : 0;
        if (old.newlines >= 3)
          blankRule = Rule::BlankMaxOne;
        if (it.tok >= 0 && isP(uint32_t(it.tok), P::RBrace) && isBody(info[it.tok].brace) && !info[it.tok].oneLine)
        {
          if (blank)
            blankRule = Rule::BlankBeforeClose;
          blank = 0;
        }
        else if (prevTok >= 0 && items[k - 1].tok == prevTok && isP(uint32_t(prevTok), P::LBrace) && isBody(info[prevTok].brace) && !info[prevTok].oneLine)
        {
          if (blank)
            blankRule = Rule::BlankAfterOpen;
          blank = 0;
        }
        else if (blankBefore[k] && !blank)
        {
          // not first in its body, nor after an access label
          bool first = prevTok < 0 || (isP(uint32_t(prevTok), P::LBrace) && items[k - 1].tok == prevTok) || info[prevTok].colon == Colon::Access ||
                       (items[k].tok >= 0 && isP(uint32_t(items[k].tok), P::RBrace));
          if (!first)
            blank = 1, blankRule = Rule::BlankAroundDefinition;
        }
        int indent;
        if (it.tok < 0)
          indent = commentIndent(k, indentRule);
        else
          indent = lineIndent(uint32_t(it.tok), indentRule);
        gap = lineEnd;
        if (blank)
          gap += lineEnd;
        gap.append(size_t(std::max(indent, 0)), ' ');
        outLine += 1 + blank;
        outCol = curIndent = std::max(indent, 0);
        if (columns > 0 && !planned)
          planSegment(k, outCol);
      }
      else if (k > 0)
      {
        bool unclassified;
        Space sp = spacesBefore(k, unclassified);
        spaceRule = sp.rule;
        if (unclassified)
        {
          ++outcome.unclassified;
          if (outcome.unclassifiedLines.size() < 50)
          {
            outcome.unclassifiedLines.push_back(line(it.begin));
            outcome.unclassifiedPairs.push_back("`" + std::string(spelling(uint32_t(items[k - 1].tok))) + "` `" + std::string(spelling(uint32_t(it.tok))) + "`");
          }
        }
        gap.append(size_t(sp.n), ' ');
        outCol += sp.n;
      }
      // the change, by the rule that decided it
      if (gap != oldGap)
      {
        bool newBreak = gap.find('\n') != std::string::npos;
        if (newBreak != (old.newlines > 0))
          change(breakRule != Rule::None ? breakRule : Rule::SpaceOther, k);
        else if (newBreak)
        {
          bool any = false;
          if (old.trailing)
            change(Rule::TrailingSpace, k), any = true;
          if (old.otherEnd)
            change(Rule::LineEnds, k), any = true;
          int oldBlank = old.newlines >= 2 ? 1 : 0, newBlank = gap.find('\n') != gap.rfind('\n') ? 1 : 0;
          if (oldBlank != newBlank || old.newlines >= 3)
            change(blankRule != Rule::None ? blankRule : Rule::BlankMaxOne, k), any = true;
          size_t oldLast = oldGap.rfind('\n'), newLast = gap.rfind('\n');
          if (oldGap.substr(oldLast + 1) != std::string_view(gap).substr(newLast + 1))
            change(old.tab ? Rule::Tabs : indentRule != Rule::None ? indentRule : Rule::IndentBlock, k), any = true;
          if (!any)
            change(Rule::SpaceOther, k);
        }
        else
          change(k == 0 ? Rule::BlankStart : spaceRule != Rule::None ? spaceRule : Rule::SpaceOther, k);
      }
      out += gap;
      // the item
      std::string_view body = text.substr(it.begin, it.end - it.begin);
      if (it.tok >= 0)
      {
        uint32_t t = uint32_t(it.tok);
        // where an argument starts, and the value after `return` or an assignment
        if (!parenStack.empty() && prevTok >= 0 &&
            (uint32_t(prevTok) == parenStack.back().open || (isP(uint32_t(prevTok), P::Comma) && lastCommaDepth == parenStack.size())))
          parenStack.back().argCol = outCol;
        if (prevTok >= 0 && !brk && exprCol[info[t].stmt] < 0 && (isKw(uint32_t(prevTok), Kw::return_) || isKw(uint32_t(prevTok), Kw::throw_) ||
            (assignment(uint32_t(prevTok)) && info[prevTok].stmt == info[t].stmt)))
        {
          bool inParens = !parenStack.empty() && parenStack.back().open >= info[t].stmt;
          if (!inParens)
            exprCol[info[t].stmt] = outCol;
        }
        if (isP(t, P::Comma))
          lastCommaDepth = parenStack.size();
        // a ( content: a token after an opener on its line
        if (!parenStack.empty() && !parenStack.back().content && parenStack.back().outLine == outLine && parenStack.back().open != t)
          parenStack.back().content = true;
        if (info[t].opensFrame)
          frames[info[t].opensFrame].base = curIndent;
        if (!parenStack.empty() && parenStack.back().close == t)
          parenStack.pop_back();
        out.append(body);
        size_t nl = body.rfind('\n');
        if (nl == std::string_view::npos)
          outCol += widthOf(body);
        else
        {
          outLine += uint32_t(std::count(body.begin(), body.end(), '\n'));
          outCol = widthOf(body.substr(nl + 1));
        }
        if (isP(t, P::LParen) || isP(t, P::LBracket) || (isP(t, P::LBrace) && info[t].brace == Brace::List && !info[t].oneLine))
          parenStack.push_back({t, info[t].match, outCol, curIndent, outLine, false});
        prevTok = int32_t(t);
      }
      else
      {
        // a block comment's later lines move with its first, when that can be undone
        size_t nl = body.find('\n');
        if (nl == std::string_view::npos)
        {
          out.append(body);
          outCol += widthOf(body);
        }
        else
        {
          uint32_t oldCol = it.begin - lineStarts[line(it.begin) - 1];
          int delta = int(outCol) - int(oldCol);
          bool movable = true;
          for (size_t q = nl; q != std::string_view::npos; q = body.find('\n', q + 1))
          {
            size_t s = q + 1;
            size_t e = s;
            while (e < body.size() && body[e] == ' ')
              ++e;
            if (e < body.size() && body[e] != '\n' && body[e] != '\r' && e - s < oldCol)
              movable = false;
            if (e < body.size() && body[e] == '\t')
              movable = false;
          }
          if (!movable || delta == 0)
            out.append(body);
          else
          {
            std::string moved;
            size_t q = 0;
            moved.append(body.substr(0, nl + 1));
            q = nl + 1;
            while (q <= body.size())
            {
              size_t e = body.find('\n', q);
              std::string_view lineText = body.substr(q, e == std::string_view::npos ? std::string_view::npos : e - q + 1);
              size_t spaces = 0;
              while (spaces < lineText.size() && lineText[spaces] == ' ')
                ++spaces;
              bool blankLine = spaces == lineText.size() || lineText[spaces] == '\n' || lineText[spaces] == '\r';
              if (!blankLine)
                moved.append(size_t(int(spaces) + delta), ' ');
              moved.append(lineText.substr(blankLine ? 0 : spaces));
              if (e == std::string_view::npos)
                break;
              q = e + 1;
            }
            out += moved;
          }
          outLine += uint32_t(std::count(body.begin(), body.end(), '\n'));
          size_t last = out.rfind('\n');
          outCol = widthOf(std::string_view(out).substr(last + 1));
        }
      }
    }
    // the end: one line end
    {
      uint32_t gapBegin = items.empty() ? uint32_t(bodyStart) : items.back().end;
      std::string_view oldGap = text.substr(gapBegin);
      std::string gap = items.empty() ? std::string() : lineEnd;
      if (gap != oldGap)
      {
        GapShape old = shapeOf(oldGap, crlf);
        if (old.trailing)
          outcome.changes.push_back({uint8_t(Rule::TrailingSpace), line(gapBegin), outLine});
        else
          outcome.changes.push_back({uint8_t(Rule::FinalLineEnd), line(gapBegin), outLine});
      }
      out += gap;
    }

    // the check: the same tokens and comments
    {
      std::vector<Token> again = lex(out, symbols).tokens;
      bool same = true;
      size_t differs = 0;
      for (size_t i = 0; same && i + 1 < std::min(toks.size(), again.size()); ++i)
      {
        same = again[i].getKind() == toks[i].getKind() && std::string_view(out).substr(again[i].offset, again[i].length) == spelling(uint32_t(i));
        differs = i;
      }
      same = same && again.size() == toks.size();
      std::vector<std::string> before, after;
      collectComments(text, toks, before);
      collectComments(out, again, after);
      if (!same || before != after)
      {
        outcome.error = std::string("internal: the formatted text has other ") + (!same ? "tokens" : "comments") + " than the input";
        if (!same && differs + 1 < toks.size())
          outcome.error += " (from line " + std::to_string(tokLine(uint32_t(differs))) + ", `" + std::string(spelling(uint32_t(differs))) + "`)";
        return outcome;
      }
    }
    outcome.text = std::move(out);
    outcome.formatted = true;
    return outcome;
  }
}

FormatOutcome formatText(std::string_view path, const std::string& text, int columns)
{
  FormatOutcome outcome;
  SymbolTable symbols;
  LexResult lexed = lex(text, symbols);
  if (lexed.errorCount)
  {
    outcome.error = "the lexer reports an error";
    return outcome;
  }
  std::vector<uint8_t> roles(text.size() + 1, 0);
  {
    std::vector<Token> parsedTokens = lexed.tokens;
    ParsedFile parsed;
    parsed.path = std::string(path);
    parsed.formatRoles = &roles;
    parseFile(parsedTokens, text, symbols, parsed, Dialect::KppSource, nullptr);
    if (!parsed.diagnostics.empty())
    {
      const Diagnostic& d = parsed.diagnostics.front();
      uint32_t offset = d.token < parsedTokens.size() ? parsedTokens[d.token].offset : 0;
      uint32_t lineNumber = 1 + uint32_t(std::count(text.begin(), text.begin() + std::min<size_t>(offset, text.size()), '\n'));
      outcome.error = "line " + std::to_string(lineNumber) + ": " + d.message;
      return outcome;
    }
  }
  Formatter formatter(text, lexed.tokens, roles, symbols);
  formatter.columns = columns;
  return formatter.run();
}

namespace
{
  int formatUsage()
  {
    fprintf(stderr,
            "usage: kpp format [--check | --stdout | --stats [--examples N]] [--columns N] [--threads N] PATH...\n"
            "  lays out the .kpp files (a directory: every .kpp file under it) in kpp's style (docs/format.md)\n"
            "  --check     changes nothing; lists each file not in the style, exit 1 if there is one\n"
            "  --stdout    writes the one file's formatted text to stdout\n"
            "  --stats     changes nothing; counts the changes by rule (files, lines, gaps)\n"
            "  --examples N  with --stats: N changed lines of each rule, before and after\n"
            "  --columns N   the limit lines are wrapped to (130; 0: no wrapping, the writer's breaks stay)\n");
    return 2;
  }

  std::vector<std::string> splitLines(std::string_view s)
  {
    std::vector<std::string> lines;
    size_t p = 0;
    while (p <= s.size())
    {
      size_t e = s.find('\n', p);
      std::string_view l = s.substr(p, e == std::string_view::npos ? std::string_view::npos : e - p);
      if (!l.empty() && l.back() == '\r')
        l.remove_suffix(1);
      lines.emplace_back(l);
      if (e == std::string_view::npos)
        break;
      p = e + 1;
    }
    return lines;
  }
}

int runFormat(int argc, char** argv)
{
  bool check = false, stats = false, toStdout = false;
  int examples = 0, columns = 130;
  unsigned threads = std::max(1u, std::thread::hardware_concurrency());
  std::vector<std::filesystem::path> roots;
  for (int i = 2; i < argc; ++i)
  {
    std::string a = argv[i];
    if (a == "--check")
      check = true;
    else if (a == "--stats")
      stats = true;
    else if (a == "--stdout")
      toStdout = true;
    else if (a == "--examples" && i + 1 < argc)
      examples = atoi(argv[++i]);
    else if (a == "--columns" && i + 1 < argc)
      columns = atoi(argv[++i]);
    else if (a == "--threads" && i + 1 < argc)
      threads = unsigned(std::max(1, atoi(argv[++i])));
    else if (a.size() > 1 && a[0] == '-')
    {
      fprintf(stderr, "kpp format: unknown option %s\n", a.c_str());
      return formatUsage();
    }
    else
      roots.emplace_back(a);
  }
  if (roots.empty())
    return formatUsage();
  std::vector<std::filesystem::path> files;
  for (const auto& root : roots)
  {
    std::error_code ec;
    if (std::filesystem::is_directory(root, ec))
    {
      for (auto it = std::filesystem::recursive_directory_iterator(root, ec); it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
      {
        if (ec)
          break;
        std::string name = it->path().filename().string();
        if (it->is_directory() && (name == "build" || name == "bin" || (!name.empty() && name[0] == '.')))
        {
          it.disable_recursion_pending();
          continue;
        }
        if (it->is_regular_file() && it->path().extension() == ".kpp")
          files.push_back(it->path());
      }
    }
    else if (std::filesystem::is_regular_file(root, ec))
      files.push_back(root);
    else
    {
      fprintf(stderr, "kpp format: %s: no such file or directory\n", root.string().c_str());
      return 2;
    }
  }
  std::sort(files.begin(), files.end());
  if (toStdout && files.size() != 1)
  {
    fprintf(stderr, "kpp format: --stdout takes one file\n");
    return 2;
  }

  struct Result
  {
    FormatOutcome outcome;
    std::string original;
    bool read = false;
  };
  std::vector<Result> results(files.size());
  std::atomic<size_t> next{0};
  auto worker = [&]
  {
    for (size_t i; (i = next++) < files.size();)
    {
      std::ifstream in(files[i], std::ios::binary);
      if (!in)
        continue;
      std::stringstream buffer;
      buffer << in.rdbuf();
      results[i].original = buffer.str();
      results[i].read = true;
      results[i].outcome = formatText(files[i].string(), results[i].original, columns);
    }
  };
  std::vector<std::thread> pool;
  for (unsigned t = 0; t < std::min<size_t>(threads, files.size()); ++t)
    pool.emplace_back(worker);
  for (auto& t : pool)
    t.join();

  size_t refused = 0, changed = 0;
  for (size_t i = 0; i < files.size(); ++i)
  {
    const Result& r = results[i];
    if (!r.read || !r.outcome.formatted)
    {
      ++refused;
      fprintf(stderr, "%s: not formatted: %s\n", files[i].string().c_str(), r.read ? r.outcome.error.c_str() : "cannot read it");
      continue;
    }
    if (r.outcome.text != r.original)
      ++changed;
  }
  if (toStdout)
  {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY); // (the text's own line ends, never CRLF for LF)
#endif
    if (results[0].outcome.formatted)
      fwrite(results[0].outcome.text.data(), 1, results[0].outcome.text.size(), stdout);
    return results[0].outcome.formatted ? 0 : 1;
  }
  if (stats)
  {
    const std::vector<std::filesystem::path>& filePaths = files;
    size_t ruleCount = size_t(Rule::Count);
    std::vector<size_t> ruleFiles(ruleCount), lines(ruleCount), gaps(ruleCount);
    std::vector<std::vector<std::string>> samples(ruleCount);
    size_t totalLines = 0, totalGaps = 0, unclassified = 0, inputLines = 0;
    for (size_t i = 0; i < results.size(); ++i)
    {
      const Result& r = results[i];
      if (!r.read || !r.outcome.formatted)
        continue;
      inputLines += size_t(std::count(r.original.begin(), r.original.end(), '\n'));
      unclassified += r.outcome.unclassified;
      std::vector<std::vector<uint32_t>> byRule(ruleCount);
      std::vector<uint32_t> all;
      for (const FormatChange& c : r.outcome.changes)
      {
        byRule[c.rule].push_back(c.line);
        all.push_back(c.line);
        ++gaps[c.rule];
        ++totalGaps;
      }
      std::sort(all.begin(), all.end());
      totalLines += size_t(std::unique(all.begin(), all.end()) - all.begin());
      std::vector<std::string> before, after;
      bool split = false;
      for (size_t rule = 1; rule < ruleCount; ++rule)
      {
        auto& v = byRule[rule];
        if (v.empty())
          continue;
        ++ruleFiles[rule];
        std::sort(v.begin(), v.end());
        lines[rule] += size_t(std::unique(v.begin(), v.end()) - v.begin());
        if (int(samples[rule].size()) < examples)
        {
          if (!split)
          {
            before = splitLines(r.original);
            after = splitLines(r.outcome.text);
            split = true;
          }
          for (const FormatChange& c : r.outcome.changes)
            if (c.rule == rule && int(samples[rule].size()) < examples)
            {
              std::string sample = "    " + filePaths[i].string() + ":" + std::to_string(c.line) + "\n";
              if (c.line >= 1 && c.line <= before.size())
                sample += "      - " + before[c.line - 1] + "\n";
              if (c.outputLine >= 1 && c.outputLine <= after.size())
                sample += "      + " + after[c.outputLine - 1] + "\n";
              samples[rule].push_back(sample);
            }
        }
      }
    }
    printf("files: %zu, formatted %zu, refused %zu, changed %zu; input lines %zu, lines with a change %zu, changed gaps %zu; gaps kept as written (no rule) %zu\n",
           results.size(), results.size() - refused, refused, changed, inputLines, totalLines, totalGaps, unclassified);
    printf("%4s  %-28s %8s %10s %10s\n", "#", "rule", "files", "lines", "gaps");
    for (size_t rule = 1; rule < ruleCount; ++rule)
      printf("%4zu  %-28s %8zu %10zu %10zu\n", rule, ruleSlug(Rule(rule)), ruleFiles[rule], lines[rule], gaps[rule]);
    if (examples > 0)
    {
      // where no rule covers the spacing (the writer's is kept): the token pairs, most frequent first
      std::map<std::string, std::pair<size_t, std::string>> pairs;
      for (size_t i = 0; i < results.size(); ++i)
      {
        const Result& r = results[i];
        if (!r.read || !r.outcome.formatted)
          continue;
        for (size_t q = 0; q < r.outcome.unclassifiedPairs.size(); ++q)
        {
          auto& entry = pairs[r.outcome.unclassifiedPairs[q]];
          if (entry.first++ == 0)
            entry.second = filePaths[i].string() + ":" + std::to_string(r.outcome.unclassifiedLines[q]);
        }
      }
      std::vector<std::pair<size_t, std::string>> sorted;
      for (const auto& [pair, entry] : pairs)
        sorted.push_back({entry.first, pair + "  (" + entry.second + ")"});
      std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
      printf("\ngaps kept as written, by the tokens around them (up to 50 a file counted):\n");
      for (size_t q = 0; q < sorted.size() && q < size_t(examples) * 4; ++q)
        printf("  %6zu  %s\n", sorted[q].first, sorted[q].second.c_str());
    }
    if (examples > 0)
      for (size_t rule = 1; rule < ruleCount; ++rule)
        if (!samples[rule].empty())
        {
          printf("\n%zu %s: %s\n", rule, ruleSlug(Rule(rule)), ruleText(Rule(rule)));
          for (const std::string& s : samples[rule])
            fputs(s.c_str(), stdout);
        }
    return refused ? 1 : 0;
  }
  if (check)
  {
    for (size_t i = 0; i < results.size(); ++i)
    {
      const Result& r = results[i];
      if (!r.read || !r.outcome.formatted || r.outcome.text == r.original)
        continue;
      uint32_t first = UINT32_MAX;
      for (const FormatChange& c : r.outcome.changes)
        first = std::min(first, c.line);
      printf("%s:%u: not in kpp's style\n", files[i].string().c_str(), first);
    }
    printf("kpp format --check: %zu files, %zu not in the style, %zu not formatted\n", results.size(), changed, refused);
    return changed || refused ? 1 : 0;
  }
  for (size_t i = 0; i < results.size(); ++i)
  {
    const Result& r = results[i];
    if (!r.read || !r.outcome.formatted || r.outcome.text == r.original)
      continue;
    std::ofstream o(files[i], std::ios::binary);
    o.write(r.outcome.text.data(), std::streamsize(r.outcome.text.size()));
  }
  printf("kpp format: %zu files, %zu changed, %zu not formatted\n", results.size(), changed, refused);
  return refused ? 1 : 0;
}
