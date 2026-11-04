// Recursive-descent parser for the kpp subset, tolerant of C++ so that real C++ code bases can
// be used as benchmark input. Where C++ needs semantic knowledge to disambiguate (declaration vs.
// expression, template-id vs. less-than), this parser uses purely syntactic heuristics; the
// resolver later checks the guesses. Unknown macros are recognised by their ALL_CAPS spelling.
#include "parser.hpp"
#include "config.hpp"
#include "tests.hpp"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_set>

namespace
{
  struct Declarator
  {
    Type* type = nullptr;
    QualifiedName name;
    bool isFunction = false;
    Span<Decl*> params;
    uint32_t flags = 0;
    Expr* noexceptCondition = nullptr; // noexcept(expr)
    Type* trailingReturn = nullptr;
    Expr* bitfield = nullptr;
    Expr* requiresClause = nullptr;
    Span<Expr*> structuredBindings;
    Span<Expr*> objectArgs; // FunctionDecl::objectArgs
  };

  class Parser
  {
  public:
    Parser(std::vector<Token>& tokens, std::string_view text, SymbolTable& symbols, ParsedFile& out)
      : toks(tokens), text(text.data()), symbols(symbols), out(out), arena(out.arena)
    {
      kwImport = symbols.intern("import");
      kwSuite = symbols.intern("SUITE");
      kwAssert = symbols.intern("assert");
      kwReleaseAssert = symbols.intern("release_assert");
      kwCaller = symbols.intern("caller");
      kwConditional = symbols.intern("conditional");
      kwUnderscore = symbols.intern("_");
      kwConfig = symbols.intern("config");
      kwNameof = symbols.intern("nameof");
      kwCount = symbols.intern("count");
      kwValues = symbols.intern("values");
      kwStr = symbols.intern("str");
      kwTypes = symbols.intern("types");
      kwFromStr = symbols.intern("fromStr");
      kwOptional = symbols.intern("Optional");
      kwUniquePtr = symbols.intern("UniquePtr");
      kwText = symbols.intern("text");
      kwTag = symbols.intern("Tag");
      kwTagField = symbols.intern("__tag");
      kwTagMethod = symbols.intern("tag");
      kwTypeMember = symbols.intern("type");
      kwImplicit = symbols.intern("implicit");
      kwFallthrough = symbols.intern("fallthrough");
      kwBothConst = symbols.intern("both_const");
      kwClassConst = symbols.intern("class_const");
      kwAccumulate = symbols.intern("accumulate");
      kwTest = symbols.intern("test");
      kwCheck = symbols.intern("check");
      kwRequire = symbols.intern("require");
      kwMain = symbols.intern("main");
      kwCBool = symbols.intern("_Bool");
    }
    uint32_t kwTest, kwCheck, kwRequire, kwMain, kwCBool;
    bool inTest = false;      // In a test's body: check(...) and require(...) are statements.
    uint32_t testCount = 0;   // Tests parsed so far in this file.
    uint32_t kwImplicit, kwFallthrough, kwBothConst, kwClassConst, kwAccumulate, kwImport, kwAssert, kwReleaseAssert, kwCaller,
             kwConditional, kwUnderscore, kwConfig, kwNameof, kwCount, kwValues, kwStr, kwTypes, kwFromStr, kwOptional, kwUniquePtr, kwText, kwTag, kwTagField,
             kwTagMethod, kwTypeMember, kwSuite;
    uint32_t lambdaCount = 0;        // Lambdas parsed so far in this file.
    uint32_t autoCount = 0;          // Type parameters invented for `auto` parameters so far in this file.
    uint8_t pendingAttributes = 0;   // [[noinline]] and friends seen before the current declaration.
    bool sawVectorSize = false;      // C: `__attribute__((__vector_size__(N)))` seen since the last declaration (pushDecl).
    bool pendingConst = false;       // `const static int x`: the const read among the specifiers, applied to the type that follows.
    Expr* pendingAlignment = nullptr; // alignas(N) seen before the current class.
    Expr* pendingCondition = nullptr; // conditional(expr) seen in the current specifiers.
    Expr* pendingExplicitCondition = nullptr; // explicit(expr) seen in the current specifiers.

    void parseTranslationUnit()
    {
      size_t mark = scratch.size();
      if (config && at(Kw::requires_))
      {
        // `requires config::platform == Platform::Windows;` first in a file: the file exists only
        // when the condition holds.
        advance();
        Expr* cond = parseExpression();
        expect(P::Semi, "expected ';' after the file's requires condition");
        std::optional<ConfigValue> value = configValue(cond);
        if (!value || !value->isNumber)
          error("a file's requires condition needs config constants and literals");
        else if (value->number == 0)
        {
          out.decls = makeSpan<Decl*>(mark);
          return;
        }
      }
      while (!at(TokenKind::End))
      {
        if (at(P::RBrace))
        {
          error("unexpected '}'");
          ++pos;
          continue;
        }
        parseDeclaration(Scope::Namespace);
      }
      // A --tests build's main is the test runner's (compiler/src/tests.cpp): the program's is renamed.
      if (testsEnabled() && out.path != testRunnerPath)
        for (size_t i = mark; i < scratch.size(); ++i)
          if (scratch[i]->kind == NodeKind::FunctionDecl)
          {
            auto* f = static_cast<FunctionDecl*>(scratch[i]);
            if (f->name.parts.size() == 1 && f->name.parts[0].symbol == kwMain && !f->name.global)
              f->name.parts[0].symbol = symbols.intern("__kpp_program_main");
          }
      out.decls = makeSpan<Decl*>(mark);
    }

  private:
    enum class Scope { Namespace, Class, Enum, Local };

    std::vector<Token>& toks;
    const char* text;
    SymbolTable& symbols;
    ParsedFile& out;
    Arena& arena;
    uint32_t pos = 0;
    std::vector<Node*> scratch;
    Decl* currentParent = nullptr;
    uint32_t currentClassName = UINT32_MAX;
    Access currentAccess = Access::Default;
    bool inRecovery = false;
    // Every error() as it was called, a recovery's own too (diagnostics keeps the first of each): a
    // C struct's body that had one is one the import could not read in full (the safety net in
    // parseClassOrElaborated).
    std::vector<Diagnostic> allErrors;
    int depth = 0;
  public:
    bool externC = false; // Inside extern "C".
    bool cMode = false;   // Preprocessed C text (imported headers): no macro heuristics.
    bool kppSource = false; // A .kpp file: no C macro idioms (the converter's C++ input has them).
    const ConfigValues* config = nullptr; // Build configuration, for declaration-scope if constexpr.
  private:
    std::unordered_set<uint32_t> cTypedefNames; // in cMode: the typedef names declared so far (what `(Name)` casts to)
    size_t classMembersMark = 0; // where the members of the class being parsed start in scratch
    bool conditionMode = false; // Declarations inside `if (...)` end at ')' instead of ';'.
    bool inRequires = false;    // Inside a requires-clause names are always template-ids.
    std::vector<std::pair<uint32_t, Token>> splits; // '>>' tokens split in place, for undo on backtrack.

    // Backtracks to a saved position, undoing any token splits made after it.
    void restore(uint32_t save)
    {
      pos = save;
      while (!splits.empty() && splits.back().first >= save)
      {
        uint32_t at = splits.back().first;
        toks[at] = splits.back().second;
        toks.erase(toks.begin() + at + 1);
        splits.pop_back();
      }
    }

    // --- token access ---

    const Token& tok(int ahead = 0) const
    {
      size_t i = pos + size_t(ahead);
      return i < toks.size() ? toks[i] : toks.back();
    }
    bool at(TokenKind kind, int ahead = 0) const { return tok(ahead).getKind() == kind; }
    bool at(P p, int ahead = 0) const { return tok(ahead).is(p); }
    bool at(Kw k, int ahead = 0) const { return tok(ahead).is(k); }
    bool atIdent(int ahead = 0) const { return at(TokenKind::Identifier, ahead); }
    bool atIdent(uint32_t symbol, int ahead = 0) const { return atIdent(ahead) && tok(ahead).id == symbol; }
    std::string_view spelling(const Token& t) const { return std::string_view(text + t.offset, t.length); }
    std::string_view spelling(int ahead = 0) const { return spelling(tok(ahead)); }
    void advance()
    {
      if (pos < toks.size() - 1)
        ++pos;
    }
    bool accept(P p)
    {
      if (!at(p))
        return false;
      advance();
      return true;
    }
    bool accept(Kw k)
    {
      if (!at(k))
        return false;
      advance();
      return true;
    }
    bool expect(P p, const char* what)
    {
      if (accept(p))
        return true;
      error(what);
      return false;
    }
    // Closing '>' of a template argument list, splitting '>>', '>=' and '>>=' when needed.
    bool acceptTemplateClose()
    {
      if (accept(P::Greater))
        return true;
      Token t = toks[pos];
      if (t.is(P::Shr) || t.is(P::ShrAssign))
      {
        // Split into '>' and the rest, as C++ does for `>>` (never for `>=`: `x < 0 || x >= 5` is a
        // comparison). Inserting keeps earlier token indices valid, so speculative parses that
        // backtrack over this point re-parse the same token sequence.
        Token rest = t;
        rest.offset += 1;
        rest.length -= 1;
        std::string_view restSpelling = spelling(rest);
        rest.id = uint32_t(restSpelling == ">" ? P::Greater : P::GreaterEq);
        splits.push_back({pos, t});
        toks[pos].length = 1;
        toks[pos].id = uint32_t(P::Greater);
        toks.insert(toks.begin() + pos + 1, rest);
        advance();
        return true;
      }
      return false;
    }

    bool isAllCaps(std::string_view s) const
    {
      if (s.size() < 2)
        return false;
      bool letter = false;
      for (char c : s)
      {
        if (c >= 'a' && c <= 'z')
          return false;
        if (c >= 'A' && c <= 'Z')
          letter = true;
      }
      return letter;
    }
    bool atMacroName(int ahead = 0) const { return atIdent(ahead) && isAllCaps(spelling(ahead)); }

    static bool isDeclStartKeyword(Kw k)
    {
      switch (k)
      {
        case Kw::const_: case Kw::volatile_: case Kw::static_: case Kw::virtual_: case Kw::inline_:
        case Kw::constexpr_: case Kw::consteval_: case Kw::constinit_: case Kw::explicit_: case Kw::friend_:
        case Kw::extern_: case Kw::mutable_: case Kw::typedef_: case Kw::class_: case Kw::struct_:
        case Kw::union_: case Kw::enum_: case Kw::auto_: case Kw::typename_: case Kw::template_:
        case Kw::using_: case Kw::operator_: case Kw::namespace_: case Kw::thread_local_: case Kw::decltype_:
        case Kw::void_: case Kw::bool_: case Kw::char_: case Kw::char8_t_: case Kw::char16_t_: case Kw::char32_t_:
        case Kw::wchar_t_: case Kw::short_: case Kw::int_: case Kw::long_: case Kw::signed_: case Kw::unsigned_:
        case Kw::float_: case Kw::double_: case Kw::when_:
          return true;
        default:
          return false;
      }
    }

    // --- diagnostics and recovery ---

    // A string the parser reads itself (a language name, an import path, a test's name): a line
    // splice in it is an error (an expression's literal is read through tokenSpelling).
    void noSplice()
    {
      if (tok().spliced)
        error("a line splice in this string is not supported");
    }

    // In the body of a struct written in kpp (design.md "struct and class": a public value type);
    // C's structs (imported headers) are C's.
    bool inStruct() const
    {
      return !cMode && currentParent && currentParent->kind == NodeKind::ClassDecl && static_cast<const ClassDecl*>(currentParent)->keyword == Kw::struct_ &&
             !static_cast<const ClassDecl*>(currentParent)->isTypedUnion;
    }

    void error(const char* message)
    {
      allErrors.push_back({pos, message});
      if (inRecovery)
        return;
      inRecovery = true;
      // (the lexer's error token for a line splice inside a token: that is the cause, whatever was expected)
      if (at(TokenKind::Error) && spelling().starts_with("\\"))
        message = "a line splice inside a token (C++ joins the two lines into one token; kppc does not read it so yet)";
      out.diagnostics.push_back({pos, message});
    }

    // Skips a balanced bracket group starting at the current opening token.
    void skipBalanced()
    {
      P open = P(tok().id);
      P close = open == P::LParen ? P::RParen : open == P::LBracket ? P::RBracket : open == P::LBrace ? P::RBrace : P::Greater;
      int level = 0;
      for (;;)
      {
        if (at(TokenKind::End))
          return;
        if (at(open))
          ++level;
        else if (at(close))
        {
          --level;
          if (level == 0)
          {
            advance();
            return;
          }
        }
        advance();
      }
    }

    // Skips to the end of the current declaration or statement: past the next ';' at nesting
    // level 0, past a balanced '{...}', or up to (not past) a closing '}'.
    void recover()
    {
      int level = 0;
      for (;;)
      {
        if (at(TokenKind::End))
          break;
        if (at(P::LParen) || at(P::LBracket))
          ++level;
        else if (at(P::RParen) || at(P::RBracket))
        {
          if (level > 0)
            --level;
        }
        else if (level == 0 && at(P::Semi))
        {
          advance();
          break;
        }
        else if (level == 0 && at(P::LBrace))
        {
          skipBalanced();
          accept(P::Semi);
          break;
        }
        else if (level == 0 && at(P::RBrace))
          break;
        advance();
      }
      inRecovery = false;
    }

    // --- arena helpers ---

    template<typename T>
    T* node(uint32_t token)
    {
      T* n = arena.make<T>();
      n->kind = nodeKindOf<T>();
      n->token = token;
      if constexpr (std::is_same_v<T, LiteralExpr>)
        n->file = &out;
      ++out.nodeCount;
      return n;
    }
    template<typename T>
    T* decl(uint32_t token)
    {
      T* d = node<T>(token);
      d->parent = currentParent;
      d->access = currentAccess;
      d->file = &out;
      if (externC)
        d->flags |= DF_ExternC;
      ++out.declCount;
      return d;
    }
    template<typename T>
    static constexpr NodeKind nodeKindOf();

    template<typename T>
    Span<T> makeSpan(size_t mark)
    {
      Span<T> span;
      span.count = uint32_t(scratch.size() - mark);
      span.data = arena.array(reinterpret_cast<T*>(scratch.data() + mark), span.count);
      scratch.resize(mark);
      return span;
    }
    template<typename T>
    Span<T> makeSpanOf(const std::vector<T>& items)
    {
      Span<T> span;
      span.count = uint32_t(items.size());
      span.data = arena.array(items.data(), items.size());
      return span;
    }

    struct DepthGuard
    {
      Parser& p;
      DepthGuard(Parser& p) : p(p) { ++p.depth; }
      ~DepthGuard() { --p.depth; }
    };

    // --- attributes and other things we skip ---

    void skipAttributes()
    {
      for (;;)
      {
        if (at(P::LBracket) && at(P::LBracket, 1))
        {
          advance();
          advance();
          if (atIdent())
          {
            std::string_view a = spelling();
            if (a == "noinline")
              pendingAttributes |= ATTR_NoInline;
            else if (a == "forceinline")
              pendingAttributes |= ATTR_ForceInline;
            else if (a == "vectorcall")
              pendingAttributes |= ATTR_VectorCall;
            else if (a == "packed")
              pendingAttributes |= ATTR_Packed;
          }
          int level = 2;
          while (level > 0 && !at(TokenKind::End))
          {
            if (at(P::LBracket))
              ++level;
            else if (at(P::RBracket))
              --level;
            advance();
          }
        }
        else if (at(Kw::alignas_) && at(P::LParen, 1))
        {
          // `alignas(16)`: kept for the class declared next (ignored elsewhere for now).
          advance();
          advance();
          pendingAlignment = parseExpression();
          expect(P::RParen, "expected ')' after alignas");
        }
        else if (atIdent() && spelling() == "__declspec" && at(P::LParen, 1) && atIdent(2) && spelling(2) == "align" && at(P::LParen, 3))
        {
          // `__declspec(align(16))`, winnt.h's DECLSPEC_ALIGN (CONTEXT, M128A, XSAVE_FORMAT): the
          // alignment of the class or variable declared next, as alignas. RtlCaptureContext stores
          // into a CONTEXT with aligned SSE moves; one aligned to 8 crashed StackWalker.
          advance();
          advance();
          advance();
          advance();
          pendingAlignment = parseExpression();
          expect(P::RParen, "expected ')' after __declspec(align(...)");
          expect(P::RParen, "expected ')' after __declspec(align(...)");
        }
        else if (atIdent() && spelling() == "__attribute__" && at(P::LParen, 1) && at(P::LParen, 2) && atIdent(3) &&
                 (spelling(3) == "aligned" || spelling(3) == "__aligned__" || spelling(3) == "align") && at(P::LParen, 4))
        {
          // `__attribute__((aligned(16)))`, MinGW-w64's DECLSPEC_ALIGN (and its `__declspec(align(16))`,
          // which it spells `__attribute__((align(16)))`): as __declspec(align(16)) above.
          for (int i = 0; i < 5; ++i)
            advance();
          pendingAlignment = parseExpression();
          expect(P::RParen, "expected ')' after __attribute__((aligned(...)))");
          expect(P::RParen, "expected ')' after __attribute__((aligned(...)))");
          expect(P::RParen, "expected ')' after __attribute__((aligned(...)))");
        }
        else if (atIdent() && (spelling() == "__declspec" || spelling() == "__attribute__" || spelling() == "__pragma" ||
                               spelling() == "alignas") && at(P::LParen, 1))
        {
          bool gnu = spelling() == "__attribute__";
          advance();
          uint32_t from = pos;
          skipBalanced();
          if (gnu && cMode)
            for (uint32_t i = from; i < pos && i < toks.size(); ++i)
              if (toks[i].getKind() == TokenKind::Identifier && (spelling(toks[i]) == "__vector_size__" || spelling(toks[i]) == "vector_size"))
                sawVectorSize = true;
        }
        else if (atIdent() && (spelling() == "__forceinline" || spelling() == "__inline" || spelling() == "__inline__" || spelling() == "__cdecl" ||
                               spelling() == "__stdcall" || spelling() == "__fastcall" || spelling() == "__vectorcall" ||
                               spelling() == "__restrict" || spelling() == "__unaligned" || spelling() == "__thiscall" ||
                               spelling() == "__ptr64" || spelling() == "__ptr32" || spelling() == "__sptr" ||
                               spelling() == "__uptr" || spelling() == "__clrcall" || spelling() == "__w64"))
          advance();
        else if (at(TokenKind::Directive))
          advance();
        else
          return;
      }
    }

    // ALL_CAPS macro used as a specifier before a declaration, e.g. `FORCE_INLINE int f()` or
    // `KPP_API(dll) class X`. Returns true if something was skipped.
    bool skipMacroSpecifier()
    {
      if (cMode)
        return false; // preprocessed C: there are no macros left to guess at
      if (!atMacroName())
        return false;
      // MACRO(...) followed by something that starts a declaration.
      if (at(P::LParen, 1))
      {
        uint32_t save = pos;
        advance();
        skipBalanced();
        if (atIdent() || (at(TokenKind::Keyword) && isDeclStartKeyword(Kw(tok().id))) || at(P::Tilde) || at(P::ColonColon))
          return true;
        restore(save);
        return false;
      }
      // MACRO followed by a keyword, or by an identifier that is itself followed by a type-like
      // continuation (`MACRO Foo x`, `MACRO Foo* x`). `ETA getETA(` and `ID id;` are types, not macros.
      // A name before `const` or `volatile` is a type's east const (`U32 const x`, `Box const* p`)
      // unless a type follows the qualifier (`MACRO const Foo* x`, `MACRO const int x`).
      bool qualifier = at(Kw::const_, 1) || at(Kw::volatile_, 1);
      bool typeAfterQualifier = (atIdent(2) && (atIdent(3) || at(P::Star, 3) || at(P::Amp, 3) || at(P::AndAnd, 3) || at(P::ColonColon, 3) || at(P::Less, 3))) ||
                                (at(TokenKind::Keyword, 2) && isBuiltinTypeKeyword(Kw(tok(2).id)));
      if (at(TokenKind::Keyword, 1) && isDeclStartKeyword(Kw(tok(1).id)) && (!qualifier || typeAfterQualifier))
      {
        advance();
        return true;
      }
      if (atIdent(1) && (atIdent(2) || at(P::Star, 2) || at(P::Amp, 2) || at(P::AndAnd, 2) || at(P::Less, 2)))
      {
        advance();
        return true;
      }
      return false;
    }

    // --- names ---

    // Parses `a::b<args>::c`. In type context template arguments are always attempted; in
    // expression context they are only accepted when the token after '>' makes a template-id
    // plausible.
    std::vector<NamePart> partScratch; // Reused by parseQualifiedName (which may recurse).

    bool parseQualifiedName(QualifiedName& name, bool typeContext, bool allowSpecial = true)
    {
      size_t partMark = partScratch.size();
      std::vector<NamePart>& parts = partScratch;
      name.global = accept(P::ColonColon);
      for (;;)
      {
        skipAttributes();
        NamePart part;
        if (accept(Kw::template_))
        {
        }
        if (atIdent())
        {
          part.symbol = tok().id;
          advance();
        }
        else if (allowSpecial && at(P::Tilde) && atIdent(1))
        {
          part.special = 1;
          advance();
          part.symbol = tok().id;
          advance();
        }
        else if (allowSpecial && at(Kw::operator_))
        {
          advance();
          if (!parseOperatorName(part))
          {
            parts.resize(partMark);
            return false;
          }
        }
        else if (at(Kw::decltype_) && at(P::LParen, 1) && !parts.empty())
        {
          // `A::decltype(x)::b` is weird but legal; treat as unresolved component.
          advance();
          skipBalanced();
          part.symbol = UINT32_MAX;
        }
        else
        {
          parts.resize(partMark);
          return false;
        }
        // `~Vector<int>`: the arguments name the class, the destructor needs none; `runTest.operator()<CraftingMachine>(name)`:
        // an operator function template's own (a conversion operator's type took any already).
        if (part.special <= 2 && at(P::Less))
          tryParseTemplateArgs(part, typeContext);
        parts.push_back(part);
        if (at(P::ColonColon) && !at(P::Star, 1) && (atIdent(1) || at(P::Tilde, 1) || at(Kw::operator_, 1) || at(Kw::template_, 1)))
        {
          advance();
          continue;
        }
        break;
      }
      name.parts.count = uint32_t(parts.size() - partMark);
      name.parts.data = arena.array(parts.data() + partMark, name.parts.count);
      parts.resize(partMark);
      return true;
    }

    bool parseOperatorName(NamePart& part)
    {
      part.special = 2;
      if (at(TokenKind::Punctuator))
      {
        P p = P(tok().id);
        if (p == P::LParen && at(P::RParen, 1))
        {
          part.opToken = uint32_t(P::LParen);
          advance();
          advance();
          return true;
        }
        if (p == P::LBracket && at(P::RBracket, 1))
        {
          part.opToken = uint32_t(P::LBracket);
          advance();
          advance();
          return true;
        }
        part.opToken = tok().id;
        advance();
        return true;
      }
      if (at(Kw::new_) || at(Kw::delete_))
      {
        part.opToken = uint32_t(P::Count) + tok().id; // Keyword operators live above the punctuator ids.
        advance();
        if (at(P::LBracket) && at(P::RBracket, 1))
        {
          advance();
          advance();
        }
        return true;
      }
      if (at(TokenKind::String))
      {
        // operator"" _suffix
        advance();
        if (atIdent())
          advance();
        part.opToken = 0;
        return true;
      }
      // Conversion operator: operator Type
      part.special = 3;
      part.convType = parseType(false);
      if (!part.convType)
      {
        error("expected operator name");
        return false;
      }
      return true;
    }

    // Template argument heuristics: see parseQualifiedName.
    // A template argument as parsed that no expression is written as: a type keyword, `const`, a
    // pointer, reference, array or function type, `decltype(...)`, `typename X` (a pack of one).
    // kpp's names of the number types (design.md 13), built-in names the parser knows as types:
    // `(usize)-1` is a cast and `pi_v<f32> * x` a template's id, as with `size_t` and `float`.
    static bool isKppNumberName(std::string_view s)
    {
      if (s == "usize")
        return true;
      if (s.size() < 2 || s.size() > 3 || (s[0] != 'u' && s[0] != 'i' && s[0] != 'f'))
        return false;
      std::string_view bits = s.substr(1);
      return s[0] == 'f' ? bits == "32" || bits == "64" : bits == "8" || bits == "16" || bits == "32" || bits == "64";
    }

    static bool typeOnlyArgument(Node* n)
    {
      if (!n)
        return false;
      switch (n->kind)
      {
        case NodeKind::BuiltinType: case NodeKind::PointerType: case NodeKind::LRefType: case NodeKind::RRefType:
        case NodeKind::ConstType: case NodeKind::ArrayType: case NodeKind::DecltypeType: case NodeKind::MemberPointerType:
          return true;
        case NodeKind::FunctionType: return !static_cast<FunctionType*>(n)->asCall; // `gcd(N, M)` may be a call
        case NodeKind::NamedType: return static_cast<NamedType*>(n)->typename_ || static_cast<NamedType*>(n)->elaborated;
        case NodeKind::PackType: return typeOnlyArgument(static_cast<PackType*>(n)->pattern);
        default: return false;
      }
    }

    void tryParseTemplateArgs(NamePart& part, bool typeContext)
    {
      uint32_t save = pos;
      size_t mark = scratch.size();
      bool savedRecovery = inRecovery;
      size_t savedDiagnostics = out.diagnostics.size();
      advance(); // '<'
      bool ok = true;
      bool sawLogical = false;
      if (!at(P::Greater) && !at(P::Shr))
      {
        for (;;)
        {
          Node* arg = parseTemplateArgument(sawLogical);
          if (!arg)
          {
            ok = false;
            break;
          }
          scratch.push_back(arg);
          if (accept(P::Ellipsis) && uint8_t(arg->kind) <= uint8_t(NodeKind::PackType) && arg->kind != NodeKind::PackType)
          {
            // `isSame<Args...>`: the argument is the pack's expansion.
            PackType* pack = node<PackType>(arg->token);
            pack->pattern = static_cast<Type*>(arg);
            scratch.back() = pack;
          }
          if (accept(P::Comma))
            continue;
          break;
        }
      }
      if (ok && !acceptTemplateClose())
        ok = false;
      if (ok && !typeContext)
      {
        // In expression context, `a < b > c` style ambiguities are resolved towards comparison
        // unless the continuation only makes sense for a template-id.
        // (`Binary{make<E, Args...>}`, `c ? f<A> : g<B>`: no comparison ends before `}` or `:`.)
        bool plausible = at(P::LParen) || at(P::ColonColon) || at(P::LBrace) || at(P::RParen) || at(P::Comma) ||
                         at(P::Greater) || at(P::Semi) || at(P::RBracket) || at(P::Dot) || at(P::Arrow) ||
                         at(P::Ellipsis) || at(P::Shr) || at(P::RBrace) || at(P::Colon) || at(TokenKind::End);
        bool binaryOnly = at(P::EqEq) || at(P::NotEq) || at(P::AndAnd) || at(P::OrOr) || at(P::Question) ||
                          at(P::Slash) || at(P::Percent) || at(P::Pipe) || at(P::Caret) || at(P::Assign) ||
                          at(P::Less) || at(P::LessEq) || at(P::Shl) || at(P::Spaceship) || at(P::Elvis);
        if (inRequires)
          plausible = true, sawLogical = false;
        // An argument that can only be a type (`pi_v<float> * x`, `v<const T*> - 1`): `<` cannot be
        // a comparison, so the arguments are a template-id's whatever follows.
        bool typeOnly = false;
        for (size_t i = mark; i < scratch.size(); ++i)
          typeOnly = typeOnly || typeOnlyArgument(scratch[i]) || scratch[i]->kind == NodeKind::PackType || // nor is `b...` compared
                     (scratch[i] && scratch[i]->kind == NodeKind::NamedType && static_cast<NamedType*>(scratch[i])->name.parts.size() == 1 &&
                      !static_cast<NamedType*>(scratch[i])->name.parts[0].hasTemplateArgs && isKppNumberName(spelling(toks[scratch[i]->token])));
        if (binaryOnly || typeOnly)
          ok = true;
        else if (!plausible || sawLogical)
          ok = false;
      }
      if (!ok)
      {
        restore(save);
        scratch.resize(mark);
        inRecovery = savedRecovery;
        out.diagnostics.resize(savedDiagnostics);
        return;
      }
      part.hasTemplateArgs = true;
      part.templateArgs = makeSpan<Node*>(mark);
    }

    Node* parseTemplateArgument(bool& sawLogical)
    {
      // Try a type first; if what follows is not a valid continuation, parse an expression.
      uint32_t save = pos;
      size_t mark = scratch.size();
      size_t savedDiagnosticsForArg = out.diagnostics.size();
      if (Type* t = parseType())
      {
        if (at(P::Comma) || at(P::Greater) || at(P::Shr) || at(P::GreaterEq) || at(P::ShrAssign) || at(P::Ellipsis))
          return t;
        if (at(P::LParen))
        {
          // Function type `R(Args...)`.
          Declarator d;
          parseFunctionSuffix(d);
          if (!inRecovery && (at(P::Comma) || at(P::Greater) || at(P::Shr) || at(P::GreaterEq) || at(P::ShrAssign)))
          {
            FunctionType* f = node<FunctionType>(t->token);
            f->result = t;
            f->params = d.params;
            f->isConst = (d.flags & DF_Const) != 0;
            f->isNoexcept = (d.flags & DF_Noexcept) != 0;
            f->isVariadic = (d.flags & DF_Variadic) != 0;
            // `Frac<gcd(P1::num, P2::num)>`: also read as a call, which the resolver takes when the
            // parameter is a constant (the parser cannot tell a function from a type).
            uint32_t after = pos;
            size_t savedDiagnostics = out.diagnostics.size();
            restore(save);
            bool logical = false;
            Expr* call = parseExpressionNoGreater(logical);
            if (!inRecovery && pos == after && call && call->kind == NodeKind::CallExpr)
              f->asCall = call;
            inRecovery = false;
            out.diagnostics.resize(savedDiagnostics);
            restore(after);
            return f;
          }
          inRecovery = false;
          out.diagnostics.resize(savedDiagnosticsForArg);
        }
        // `T::value + 1` or `sizeof(T)` etc.: fall back to expression.
      }
      restore(save);
      scratch.resize(mark);
      // Expressions inside template arguments stop at '>' (no relational operators at top level).
      Expr* e = parseExpressionNoGreater(sawLogical);
      return e;
    }

    // --- types ---

    static bool isBuiltinTypeKeyword(Kw k)
    {
      switch (k)
      {
        case Kw::void_: case Kw::bool_: case Kw::char_: case Kw::char8_t_: case Kw::char16_t_: case Kw::char32_t_:
        case Kw::wchar_t_: case Kw::short_: case Kw::int_: case Kw::long_: case Kw::signed_: case Kw::unsigned_:
        case Kw::float_: case Kw::double_:
          return true;
        default:
          return false;
      }
    }

    // A per-file number for synthetic names: a hash of the text, computed once.
    uint64_t fileTag()
    {
      if (!tag)
      {
        uint64_t h = 1469598103934665603ull;
        for (unsigned char ch : out.text)
          h = (h ^ ch) * 1099511628211ull;
        tag = h % 1000000007ull + 1;
      }
      return tag;
    }
    uint64_t tag = 0;

    bool atBuiltinTypeIdent() const
    {
      if (!atIdent())
        return false;
      std::string_view s = spelling();
      return s == "__int64" || s == "__int32" || s == "__int16" || s == "__int8" || s == "size_t" || s == "__m128" ||
             s == "__m128i" || s == "__m256" || s == "__m256i" || s == "__m128d" || s == "__m256d";
    }

    // Parses a type (specifier + abstract pointer/reference suffixes, but not declarator names
    // or function/array suffixes). Returns null and restores position if no type starts here.
    Type* parseType(bool allowMacroSkip = true)
    {
      DepthGuard guard(*this);
      if (depth > 400)
      {
        error("nesting too deep");
        return nullptr;
      }
      uint32_t start = pos;
      bool isConst = false, isVolatile = false;
      skipAttributes();
      for (;;)
      {
        if (accept(Kw::const_))
          isConst = true;
        else if (accept(Kw::volatile_))
          isVolatile = true;
        else if (atIdent() && spelling() == "__restrict")
          advance();
        else
          break;
      }
      Type* type = nullptr;
      uint32_t typeToken = pos;
      if (at(TokenKind::Keyword) && isBuiltinTypeKeyword(Kw(tok().id)))
      {
        BuiltinType* b = node<BuiltinType>(pos);
        b->keyword = Kw::int_;
        bool any = false;
        while (at(TokenKind::Keyword) && isBuiltinTypeKeyword(Kw(tok().id)))
        {
          Kw k = Kw(tok().id);
          if (k == Kw::long_)
            ++b->longCount;
          else if (k == Kw::unsigned_)
            b->isUnsigned = true;
          else if (k == Kw::signed_)
            b->isSigned = true;
          else if (k != Kw::int_)
            b->keyword = k; // (`int` is the default: `short int` and `int short` are short)
          any = true;
          advance();
        }
        (void)any;
        if (atIdent())
        {
          // MSVC's `unsigned __int64` and friends fold into the builtin type.
          std::string_view s = spelling();
          if (s == "__int64" || s == "__int32" || s == "__int16" || s == "__int8")
          {
            b->keyword = s == "__int64" ? Kw::long_ : s == "__int32" ? Kw::int_ : s == "__int16" ? Kw::short_ : Kw::char_;
            b->longCount = s == "__int64" ? 2 : 0; // (MSVC's __int8 is char, as `unsigned __int8` is unsigned char)
            advance();
          }
        }
        type = b;
      }
      else if (atDescriptorType())
      {
        // `a.type` for a for constexpr descriptor a: read as `decltype(a.type)`, the type checker
        // giving a descriptor's `type` member the alternative's (or member's) type.
        DecltypeType* d = node<DecltypeType>(typeToken);
        NameExpr* base = node<NameExpr>(pos);
        NamePart basePart;
        basePart.symbol = tok().id;
        base->name.parts = makeSpanOf(std::vector<NamePart>{basePart});
        advance();
        MemberExpr* m = node<MemberExpr>(pos);
        m->base = base;
        m->op = P::Dot;
        advance();
        NamePart typePart;
        typePart.symbol = kwTypeMember;
        m->member.parts = makeSpanOf(std::vector<NamePart>{typePart});
        advance();
        d->expr = m;
        type = d;
      }
      else if (atBuiltinTypeIdent())
      {
        NamedType* n = node<NamedType>(pos);
        NamePart part;
        part.symbol = tok().id;
        advance();
        n->name.parts = makeSpanOf(std::vector<NamePart>{part});
        type = n;
      }
      else if (at(Kw::auto_))
      {
        AutoType* a = node<AutoType>(pos);
        advance();
        type = a;
      }
      else if (at(Kw::decltype_) && at(P::LParen, 1))
      {
        advance();
        advance();
        if (at(Kw::auto_) && at(P::RParen, 1))
        {
          advance();
          advance();
          AutoType* a = node<AutoType>(typeToken);
          a->isDecltypeAuto = true;
          type = a;
        }
        else
        {
          DecltypeType* d = node<DecltypeType>(typeToken);
          d->expr = parseExpression();
          if (!expect(P::RParen, "expected ')' after decltype"))
            return nullptr;
          type = d;
          // decltype(x)::member: the rest of the name inside the class decltype gives.
          if (at(P::ColonColon) && atIdent(1))
          {
            NamedType* n = node<NamedType>(typeToken);
            QualifiedName rest;
            advance();
            if (!parseQualifiedName(rest, true, false))
              return nullptr;
            n->name = rest;
            n->name.typeQualifier = d;
            type = n;
          }
        }
      }
      else
      {
        bool typename_ = accept(Kw::typename_);
        bool elaborated = false;
        if (at(Kw::class_) || at(Kw::struct_) || at(Kw::union_) || at(Kw::enum_))
        {
          // Elaborated type specifier `struct X`.
          elaborated = !at(Kw::enum_);
          advance();
          accept(Kw::class_); // enum class
          accept(Kw::struct_);
          skipAttributes();
        }
        if (allowMacroSkip)
          skipMacroSpecifier();
        if (!atIdent() && !at(P::ColonColon))
        {
          pos = start;
          return nullptr;
        }
        NamedType* n = node<NamedType>(pos);
        n->typename_ = typename_;
        n->elaborated = elaborated;
        if (!parseQualifiedName(n->name, true, false))
        {
          pos = start;
          return nullptr;
        }
        type = n;
      }
      // Trailing cv-qualifiers.
      for (;;)
      {
        if (accept(Kw::const_))
          isConst = true;
        else if (accept(Kw::volatile_))
          isVolatile = true;
        else
          break;
      }
      if (isConst || isVolatile)
      {
        ConstType* c = node<ConstType>(typeToken);
        c->inner = type;
        c->isVolatile = isVolatile;
        c->isConst = isConst;
        type = c;
      }
      return parseTypeSuffixes(type);
    }

    // Whether the `^` at pos ends a type (`T^`, design.md "T^") rather than being exclusive or: what
    // follows it cannot start an operand. An identifier may follow (`Item^ item;`, a declarator's
    // name): where a type is only tried (a template argument, a cast's parentheses), `a ^ b` is then
    // no type and is read again as an expression. A parenthesis may not (`a ^ (b)` is exclusive or:
    // no function type returns an owner written `T^(...)`), nor a literal or a unary operator.
    bool caretDeclarator() const
    {
      const Token& next = tok(1);
      if (next.getKind() == TokenKind::Number || next.getKind() == TokenKind::String || next.getKind() == TokenKind::Char)
        return false;
      return !(next.is(P::LParen) || next.is(P::Minus) || next.is(P::Plus) || next.is(P::Bang) || next.is(P::Tilde) ||
               next.is(P::Caret) || next.is(P::PlusPlus) || next.is(P::MinusMinus) || next.is(Kw::sizeof_) || next.is(Kw::new_) ||
               next.is(Kw::true_) || next.is(Kw::false_) || next.is(Kw::nullptr_) || next.is(Kw::this_));
    }

    // Pointer, reference, member-pointer and pack suffixes with their cv-qualifiers.
    Type* parseTypeSuffixes(Type* type)
    {
      for (;;)
      {
        skipAttributes();
        if (at(P::Star))
        {
          PointerType* p = node<PointerType>(pos);
          advance();
          p->pointee = type;
          type = p;
        }
        else if (at(P::Amp))
        {
          LRefType* r = node<LRefType>(pos);
          advance();
          r->referenced = type;
          type = r;
        }
        else if (at(P::AndAnd))
        {
          RRefType* r = node<RRefType>(pos);
          advance();
          r->referenced = type;
          type = r;
        }
        else if (at(P::Caret) && !cMode && caretDeclarator())
        {
          // `T^`: the library's UniquePtr<T> written short, `::UniquePtr<T>` and nothing else (sugar,
          // design.md "T^"; printed back as `T^`, types.cpp).
          NamedType* owner = node<NamedType>(pos);
          advance();
          NamePart part;
          part.symbol = kwUniquePtr;
          part.hasTemplateArgs = true;
          part.templateArgs = makeSpanOf(std::vector<Node*>{type});
          owner->name.parts = makeSpanOf(std::vector<NamePart>{part});
          owner->name.global = true;
          type = owner;
        }
        else if (at(Kw::const_) || at(Kw::volatile_))
        {
          ConstType* c = node<ConstType>(pos);
          c->isVolatile = at(Kw::volatile_);
          c->isConst = !c->isVolatile;
          advance();
          c->inner = type;
          type = c;
        }
        else if (atIdent() && spelling() == "__restrict")
          advance();
        else if (at(P::ColonColon) && at(P::Star, 1))
        {
          // T C::* : the qualified name was already consumed into `type`; treat as member pointer.
          advance();
          advance();
          MemberPointerType* m = node<MemberPointerType>(pos);
          m->classType = type;
          m->pointee = nullptr;
          type = m;
        }
        else if (at(P::LParen) && at(P::Star, 1) && at(P::RParen, 2) && at(P::LParen, 3))
        {
          // Abstract pointer to function: R (*)(Args)
          uint32_t start = pos;
          advance();
          advance();
          advance();
          Declarator d;
          parseFunctionSuffix(d);
          FunctionType* f = node<FunctionType>(start);
          f->result = type;
          f->params = d.params;
          f->isVariadic = (d.flags & DF_Variadic) != 0;
          PointerType* p = node<PointerType>(start);
          p->pointee = f;
          type = p;
        }
        else if (at(P::LParen) && atIdent(1) && at(P::ColonColon, 2) && memberFunctionPointerAhead())
        {
          uint32_t start = pos;
          advance();
          NamedType* cls = node<NamedType>(pos);
          parseQualifiedName(cls->name, true, false);
          advance(); // ::
          advance(); // *
          expect(P::RParen, "expected ')'");
          Declarator d;
          parseFunctionSuffix(d);
          FunctionType* f = node<FunctionType>(start);
          f->result = type;
          f->params = d.params;
          f->isConst = (d.flags & DF_Const) != 0;
          f->isVariadic = (d.flags & DF_Variadic) != 0;
          MemberPointerType* m = node<MemberPointerType>(start);
          m->classType = cls;
          m->pointee = f;
          type = m;
        }
        else if (atIdent() && memberPointerAhead())
        {
          // T C::* (abstract) or T C::* name: consume `C::*` here, the name is left to the declarator.
          MemberPointerType* m = node<MemberPointerType>(pos);
          NamedType* cls = node<NamedType>(pos);
          parseQualifiedName(cls->name, true, false);
          advance(); // ::
          advance(); // *
          m->classType = cls;
          m->pointee = type;
          type = m;
        }
        else
          return type;
      }
    }

    // At `(`: is the balanced parameter list followed by `->` (a deduction guide)?
    bool deductionGuideAhead() const
    {
      int depth = 0;
      for (uint32_t i = pos; i < toks.size(); ++i)
      {
        if (toks[i].is(P::LParen))
          ++depth;
        else if (toks[i].is(P::RParen) && --depth == 0)
          return i + 1 < toks.size() && toks[i + 1].is(P::Arrow);
        else if (toks[i].is(P::Semi) || toks[i].is(P::LBrace) || toks[i].getKind() == TokenKind::End)
          return false;
      }
      return false;
    }

    // Lookahead for `( Name::Name::* ) (` without allocating.
    bool memberFunctionPointerAhead() const
    {
      int i = 1;
      while (atIdent(i) && at(P::ColonColon, i + 1))
      {
        if (at(P::Star, i + 2))
          return at(P::RParen, i + 3) && at(P::LParen, i + 4);
        i += 2;
      }
      return false;
    }

    // Lookahead for `Name::Name::*` without allocating.
    bool memberPointerAhead() const
    {
      int i = 0;
      while (atIdent(i) && at(P::ColonColon, i + 1))
      {
        if (at(P::Star, i + 2))
          return true;
        i += 2;
      }
      return false;
    }

    // --- declarations ---

    template<typename T>
    void pushDecl(T* d)
    {
      d->endToken = pos;
      // A C typedef of a SIMD vector (`typedef float __m128 __attribute__((__vector_size__(16)))`,
      // clang's intrinsics headers) is left out: the importer would make it a plain float. So
      // __m128, __m128i and __m128d stay the compiler's own vector types, which the code generator's
      // SSE operations work on, and a use of another one (__v4sf) is an unresolved name.
      if (cMode && sawVectorSize)
      {
        sawVectorSize = false;
        if constexpr (std::is_same_v<T, VariableDecl>)
          if (d->flags & DF_Typedef)
            return;
      }
      // A C typedef named _Bool, C's keyword, is not declared (namesCBool).
      if constexpr (std::is_same_v<T, VariableDecl>)
        if ((d->flags & DF_Typedef) && namesCBool(d->name))
          return;
      // `alignas(16) float a[4], b[4];`: every variable of the declaration (it is cleared when the
      // next declaration starts).
      if constexpr (std::is_same_v<T, VariableDecl>)
        if (pendingAlignment && !(d->flags & DF_Typedef))
          d->alignment = pendingAlignment;
      scratch.push_back(d);
      // C declares a type before using it: the typedef names seen so far make `(HANDLE)(LONG_PTR)-1`
      // two casts (windows.h's INVALID_HANDLE_VALUE), not a call.
      if (cMode)
        if constexpr (std::is_same_v<T, VariableDecl>)
          if ((d->flags & DF_Typedef) && d->name.parts.size() == 1)
            cTypedefNames.insert(d->name.parts[0].symbol);
    }

    // C's _Bool keyword as a declared name. A header that typedefs bool after stdbool.h's `#define bool
    // _Bool` (curl's curl_setup.h without HAVE_BOOL_T: `typedef enum { bool_false, bool_true } bool;`)
    // does not compile as C, but the import reads it, and its _Bool would be another type than bool in
    // every declaration of the import (SDL's `const _Bool*`): so nothing of that name is declared.
    bool namesCBool(const QualifiedName& name) const
    {
      return cMode && name.parts.size() == 1 && name.parts[0].symbol == kwCBool;
    }

    // MS C's anonymous member of a named struct type, `MONITORINFO;` in winuser.h's MONITORINFOEXW
    // (its C++ version is `struct tagMONITORINFOEXW : public tagMONITORINFO`), whose members are
    // the enclosing struct's. As the first data member it is what C++ makes it, a base at offset 0:
    // the same layout, and its members found as a base's. Anywhere else a member of a name of its
    // own: the layout stays right, and a use of its members by their names is an error.
    void anonymousMemberOfNamedType(ClassDecl* c, uint32_t start)
    {
      NamedType* type = node<NamedType>(start);
      NamePart part;
      part.symbol = tok().id;
      type->name.parts = makeSpanOf(std::vector<NamePart>{part});
      advance();
      advance(); // ;
      bool first = true;
      for (size_t i = classMembersMark; i < scratch.size() && first; ++i)
      {
        Decl* m = static_cast<Decl*>(scratch[i]);
        if (m->kind == NodeKind::VariableDecl && !(m->flags & (DF_Static | DF_Typedef)))
          first = false;
        else if (m->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(m)->name.empty() && static_cast<ClassDecl*>(m)->isDefinition)
          first = false;
      }
      if (first && c->bases.empty())
      {
        ClassDecl::Base base{};
        base.type = type;
        base.access = Access::Public;
        c->bases = makeSpanOf(std::vector<ClassDecl::Base>{base});
        return;
      }
      VariableDecl* v = decl<VariableDecl>(start);
      v->type = type;
      NamePart name;
      name.symbol = symbols.intern("__anonymous" + std::to_string(start));
      v->name.parts = makeSpanOf(std::vector<NamePart>{name});
      pushDecl(v);
    }

    Decl* lastPushedDecl(size_t mark) const
    {
      return scratch.size() > mark ? static_cast<Decl*>(scratch.back()) : nullptr;
    }

    // Parses one declaration (possibly producing several Decl nodes, e.g. `int a, b;`) and pushes
    // the results onto scratch.
    // Generated members of a kpp enum: `static const int count`, `static const E values[]`,
    // `const char* str() const` and `static Optional<E> fromStr(const char* text)` (unless written
    // by hand). Their bodies and initializers come from the compiler (Decl::builtin), not from the
    // AST.
    void addEnumBuiltins(EnumDecl* e, uint32_t start, size_t mark)
    {
      bool hasStr = false, hasFromStr = false;
      for (size_t i = mark; i < scratch.size(); ++i)
      {
        Node* n = scratch[i];
        if (n->kind == NodeKind::FunctionDecl)
        {
          auto* f = static_cast<FunctionDecl*>(n);
          if (f->name.parts.size() == 1 && f->name.parts[0].symbol == kwStr)
            hasStr = true;
          if (f->name.parts.size() == 1 && f->name.parts[0].symbol == kwFromStr)
            hasFromStr = true;
        }
      }
      NamePart part;
      VariableDecl* count = decl<VariableDecl>(start);
      count->flags |= DF_Static | DF_Const;
      count->builtin = BUILTIN_EnumCount;
      BuiltinType* intType = node<BuiltinType>(start);
      intType->keyword = Kw::int_;
      ConstType* constInt = node<ConstType>(start);
      constInt->inner = intType;
      count->type = constInt;
      part.symbol = kwCount;
      count->name.parts = makeSpanOf(std::vector<NamePart>{part});
      pushDecl(count);
      VariableDecl* values = decl<VariableDecl>(start);
      values->flags |= DF_Static | DF_Const;
      values->builtin = BUILTIN_EnumValues;
      NamedType* self = node<NamedType>(start);
      self->name = e->name;
      ArrayType* array = node<ArrayType>(start);
      array->element = self;
      array->size = nullptr;
      values->type = array;
      part.symbol = kwValues;
      values->name.parts = makeSpanOf(std::vector<NamePart>{part});
      pushDecl(values);
      if (!hasStr)
      {
        FunctionDecl* str = decl<FunctionDecl>(start);
        str->flags |= DF_Const;
        str->builtin = BUILTIN_EnumStr;
        BuiltinType* ch = node<BuiltinType>(start);
        ch->keyword = Kw::char_;
        ConstType* constChar = node<ConstType>(start);
        constChar->inner = ch;
        PointerType* pointer = node<PointerType>(start);
        pointer->pointee = constChar;
        str->returnType = pointer;
        part.symbol = kwStr;
        str->name.parts = makeSpanOf(std::vector<NamePart>{part});
        pushDecl(str);
      }
      if (!hasFromStr)
      {
        // `static Optional<E> fromStr(const char* text)`: the code generator lowers it to the
        // library's enumFromStr<E>(text) (design.md 8.4).
        FunctionDecl* from = decl<FunctionDecl>(start);
        from->flags |= DF_Static;
        from->builtin = BUILTIN_EnumFromStr;
        NamedType* selfArg = node<NamedType>(start);
        selfArg->name = e->name;
        NamePart optional;
        optional.symbol = kwOptional;
        optional.hasTemplateArgs = true;
        optional.templateArgs = makeSpanOf(std::vector<Node*>{selfArg});
        NamedType* result = node<NamedType>(start);
        result->name.parts = makeSpanOf(std::vector<NamePart>{optional});
        from->returnType = result;
        BuiltinType* ch = node<BuiltinType>(start);
        ch->keyword = Kw::char_;
        ConstType* constChar = node<ConstType>(start);
        constChar->inner = ch;
        PointerType* pointer = node<PointerType>(start);
        pointer->pointee = constChar;
        ParamDecl* text = decl<ParamDecl>(start);
        text->type = pointer;
        text->name = kwText;
        text->parent = from;
        from->params = makeSpanOf(std::vector<Decl*>{text});
        part.symbol = kwFromStr;
        from->name.parts = makeSpanOf(std::vector<NamePart>{part});
        pushDecl(from);
      }
    }

    // --- build configuration ---

    // Evaluates a condition made of literals, config constants (`config::name`) and the operators
    // ! && || == != < > <= >= + - on them. An enum-valued config constant compares equal to a
    // qualified name whose last part spells its value: `config::platform == Platform::Windows`.
    std::optional<ConfigValue> configValue(Expr* e, int depth = 0)
    {
      if (!e || depth > 32)
        return std::nullopt;
      switch (e->kind)
      {
        case NodeKind::ParenExpr: return configValue(static_cast<ParenExpr*>(e)->inner, depth + 1);
        case NodeKind::LiteralExpr:
        {
          auto* l = static_cast<LiteralExpr*>(e);
          ConfigValue v;
          if (l->literalKind == TokenKind::Keyword)
          {
            v.number = l->keyword == Kw::true_ ? 1 : 0;
            return v;
          }
          if (l->literalKind != TokenKind::Number)
            return std::nullopt;
          const Token& t = toks[l->token];
          std::string digits;
          for (char c : std::string_view(text + t.offset, t.length))
            if (c != '\'')
              digits += c;
          while (!digits.empty() && strchr("uUlLzZ", digits.back()))
            digits.pop_back();
          try
          {
            v.number = std::stoll(digits, nullptr, 0);
          }
          catch (...)
          {
            return std::nullopt;
          }
          return v;
        }
        case NodeKind::NameExpr:
        {
          auto* n = static_cast<NameExpr*>(e);
          if (n->name.parts.size() >= 2 && n->name.parts[0].symbol == kwConfig)
          {
            if (!config)
              return std::nullopt;
            // config::NAME, or a library's config::LIB::NAME (--config LIB::NAME=VALUE)
            std::string name;
            for (size_t k = 1; k < n->name.parts.size(); ++k)
              name += (k > 1 ? "::" : "") + std::string(symbols.name(n->name.parts[k].symbol));
            auto it = config->find(name);
            return it == config->end() ? std::nullopt : std::optional<ConfigValue>(it->second);
          }
          ConfigValue v;
          v.isNumber = false;
          v.text = std::string(symbols.name(n->name.last().symbol));
          return v;
        }
        case NodeKind::UnaryExpr:
        {
          auto* u = static_cast<UnaryExpr*>(e);
          auto v = configValue(u->operand, depth + 1);
          if (!v || !v->isNumber || u->isKeyword)
            return std::nullopt;
          if (P(u->op) == P::Bang)
            v->number = !v->number;
          else if (P(u->op) == P::Minus)
            v->number = -v->number;
          else
            return std::nullopt;
          return v;
        }
        case NodeKind::BinaryExpr:
        {
          auto* b = static_cast<BinaryExpr*>(e);
          auto l = configValue(b->lhs, depth + 1), r = configValue(b->rhs, depth + 1);
          if (!l || !r || b->isKeyword)
            return std::nullopt;
          ConfigValue v;
          P op = P(b->op);
          if (!l->isNumber || !r->isNumber)
          {
            if (op != P::EqEq && op != P::NotEq)
              return std::nullopt;
            bool equal = l->isNumber == r->isNumber && (l->isNumber ? l->number == r->number : l->text == r->text);
            v.number = op == P::EqEq ? equal : !equal;
            return v;
          }
          switch (op)
          {
            case P::AndAnd: v.number = l->number && r->number; break;
            case P::OrOr: v.number = l->number || r->number; break;
            case P::EqEq: v.number = l->number == r->number; break;
            case P::NotEq: v.number = l->number != r->number; break;
            case P::Less: v.number = l->number < r->number; break;
            case P::Greater: v.number = l->number > r->number; break;
            case P::LessEq: v.number = l->number <= r->number; break;
            case P::GreaterEq: v.number = l->number >= r->number; break;
            case P::Plus: v.number = l->number + r->number; break;
            case P::Minus: v.number = l->number - r->number; break;
            default: return std::nullopt;
          }
          return v;
        }
        default: return std::nullopt;
      }
    }

    // `when (condition) declaration;` (design.md 19): the declaration, which exists only while the
    // constant condition holds; each declaration it makes gets the condition (`when (c) int a, b;`).
    // A local variable's the checker decides (Decl::whenKnown), per instance. At namespace and class
    // scope it is decided here, from config constants and literals, as declaration-scope if
    // constexpr is: true, the declarations are kept (whenKnown 1); false, they are dropped from the
    // program but kept as names a lookup stops at (ClassDecl::absentMembers, ParsedFile::absentDecls),
    // so a misplaced use is an error, never a use of an outer name; a dropped data member's constructor
    // initializers are dropped with it. In a class template, a data member's condition on its
    // parameters is decided per instance: kept undecided (whenKnown -1), the instance's layout has the
    // member where it holds (codegen's whenMemberHolds); another declaration's is not supported yet.
    void parseWhen(Scope scope)
    {
      uint32_t start = pos;
      advance();
      expect(P::LParen, "expected '(' after when");
      Expr* cond = parseExpression();
      expect(P::RParen, "expected ')' after the when condition");
      if (inRecovery)
        return;
      if (at(Kw::when_))
      {
        out.diagnostics.push_back({pos, "one when per declaration: join the conditions with &&"});
        inRecovery = true;
        return;
      }
      if (scope == Scope::Enum)
      {
        out.diagnostics.push_back({start, "when declares a variable, a member or a namespace's declaration (design.md 19), not an enumerator"});
        inRecovery = true;
        return;
      }
      std::optional<bool> holds;
      bool perInstance = false; // a class template's member, decided per instance
      if (scope != Scope::Local)
      {
        std::optional<ConfigValue> value = configValue(cond);
        if (value && value->isNumber)
          holds = value->number != 0;
        else if (scope == Scope::Class && inClassTemplate())
          perInstance = true;
        else
        {
          out.diagnostics.push_back({cond->token, "a when at namespace scope or in a class that is no template needs a condition made of config "
                                                  "constants and literals (design.md 19)"});
          inRecovery = true;
          return;
        }
      }
      size_t mark = scratch.size();
      parseDeclarationInner(scope, {});
      if (inRecovery)
        return;
      for (size_t i = mark; i < scratch.size(); ++i)
      {
        Decl* d = static_cast<Decl*>(scratch[i]);
        if (scope == Scope::Local && d->kind != NodeKind::VariableDecl && d->kind != NodeKind::UsingAliasDecl && d->kind != NodeKind::ClassDecl &&
            d->kind != NodeKind::EnumDecl)
        {
          out.diagnostics.push_back({d->token, "when in a block declares a local variable, alias, class or enumeration (design.md 19), not a function or a using"});
          continue;
        }
        if (d->kind == NodeKind::NamespaceDecl || d->kind == NodeKind::ImportDecl)
        {
          out.diagnostics.push_back({d->token, "when declares a declaration, not a namespace or an import (design.md 19)"});
          continue;
        }
        if (perInstance && (d->kind != NodeKind::VariableDecl || (d->flags & (DF_Static | DF_Typedef | DF_Friend))))
        {
          out.diagnostics.push_back({d->token, "a when on a class template's parameters declares a data member, decided per instance (design.md 19): "
                                               "a member function, static member or type is not supported yet"});
          continue;
        }
        d->when = cond;
        if (holds)
          d->whenKnown = *holds ? 1 : 0;
      }
      if (holds && !*holds)
      {
        // The declarations do not exist in this configuration; their names stay, for a lookup to stop
        // at (a misplaced use is an error, never a use of an outer name of the same spelling).
        out.droppedWhen = true;
        out.droppedTokens.push_back({start, pos});
        if (scope == Scope::Class && currentParent && currentParent->kind == NodeKind::ClassDecl)
        {
          auto* c = static_cast<ClassDecl*>(currentParent);
          std::vector<Decl*> absent(c->absentMembers.begin(), c->absentMembers.end());
          for (size_t i = mark; i < scratch.size(); ++i)
            absent.push_back(static_cast<Decl*>(scratch[i]));
          c->absentMembers = makeSpanOf(absent);
        }
        else if (scope == Scope::Namespace)
          for (size_t i = mark; i < scratch.size(); ++i)
            out.absentDecls.push_back(static_cast<Decl*>(scratch[i]));
        scratch.resize(mark);
      }
    }

    // Inside a class template's body, or a class's nested in one: a member's `when` may be on its
    // parameters (parseWhen).
    bool inClassTemplate() const
    {
      for (const Decl* p = currentParent; p; p = p->parent)
        if (p->kind == NodeKind::ClassDecl && !p->templateParams.empty())
          return true;
      return false;
    }

    // Two data members of one name that both exist, one of them a `when`'s (design.md 19: several
    // `when`s may declare one name; two existing at once in a configuration are a clash). One decided
    // per class template instance is checked in each instance's layout (codegen's buildStruct).
    void checkWhenClash(ClassDecl* c)
    {
      if (c->absentMembers.empty() && std::none_of(c->members.begin(), c->members.end(), [](const Decl* m) { return m->when != nullptr; }))
        return;
      for (size_t i = 0; i < c->members.size(); ++i)
      {
        auto* a = c->members[i]->kind == NodeKind::VariableDecl ? static_cast<VariableDecl*>(c->members[i]) : nullptr;
        if (!a || a->name.empty())
          continue;
        for (size_t j = i + 1; j < c->members.size(); ++j)
        {
          auto* b = c->members[j]->kind == NodeKind::VariableDecl ? static_cast<VariableDecl*>(c->members[j]) : nullptr;
          if (b && (a->when || b->when) && !(a->when && a->whenKnown < 0) && !(b->when && b->whenKnown < 0) && !b->name.empty() &&
              b->name.last().symbol == a->name.last().symbol)
            out.diagnostics.push_back({b->token, "two when declarations of this member exist at once in this configuration, a clash (design.md 19)"});
        }
      }
    }

    // `if constexpr (condition) { declarations } else { declarations }` at namespace or class
    // scope. The condition is evaluated here, from config constants and literals, and only the
    // declarations of the taken branch are kept; the others are skipped without being parsed.
    void parseStaticIf(Scope scope, bool alreadyTaken)
    {
      advance();
      advance();
      expect(P::LParen, "expected '(' after if constexpr");
      Expr* cond = parseExpression();
      expect(P::RParen, "expected ')'");
      std::optional<ConfigValue> value = configValue(cond);
      if (!value || !value->isNumber)
        error("if constexpr at declaration scope needs a condition made of config constants and literals");
      bool take = !alreadyTaken && value && value->isNumber && value->number != 0;
      parseStaticIfBranch(scope, take);
      if (accept(Kw::else_))
      {
        if (at(Kw::if_) && at(Kw::constexpr_, 1))
          parseStaticIf(scope, alreadyTaken || take);
        else
          parseStaticIfBranch(scope, !alreadyTaken && !take);
      }
    }

    void parseStaticIfBranch(Scope scope, bool keep)
    {
      if (!at(P::LBrace))
      {
        error("expected '{' after the if constexpr condition");
        return;
      }
      if (!keep)
      {
        uint32_t from = pos;
        skipBalanced();
        out.droppedTokens.push_back({from, pos});
        return;
      }
      advance();
      while (!at(P::RBrace) && !at(TokenKind::End))
        parseDeclaration(scope);
      expect(P::RBrace, "expected '}' closing if constexpr");
    }

    void parseDeclaration(Scope scope)
    {
      DepthGuard guard(*this);
      if (depth > 400)
      {
        error("nesting too deep");
        recover();
        return;
      }
      size_t mark = scratch.size();
      uint32_t start = pos;
      pendingAlignment = nullptr; // an alignas belongs to the declaration it is written in, never the next one
      // `private int helper();` at file scope: the declaration's names are its file's own (design.md 15).
      bool filePrivate = false;
      if (scope == Scope::Namespace && at(Kw::private_) && !at(P::Colon, 1))
      {
        if (currentParent || cMode)
          out.diagnostics.push_back({pos, "private is for a file's top-level declarations, not a namespace's"});
        filePrivate = !currentParent && !cMode;
        advance();
      }
      // `config TYPE NAME = DEFAULT;` (design.md, "config:: defaults in source") is read before parsing
      // and its tokens dropped (dropConfigDeclarations) where it may stand: first on its line at the
      // file's top level. One the parser meets stands elsewhere.
      if (kppSource && tok().getKind() == TokenKind::Identifier && tok().id == kwConfig &&
          (at(TokenKind::Identifier, 1) || at(TokenKind::Keyword, 1)))
      {
        error("a config declaration (config TYPE NAME = DEFAULT;) stands at a file's top level, first on its line");
        recover();
        return;
      }
      parseDeclarationInner(scope, {});
      if (filePrivate && !inRecovery)
        for (size_t i = mark; i < scratch.size(); ++i)
        {
          Decl* d = static_cast<Decl*>(scratch[i]);
          if (d->kind == NodeKind::NamespaceDecl || d->kind == NodeKind::ImportDecl || (d->flags & DF_ExternC))
            out.diagnostics.push_back({d->token, "a private declaration declares a name of its file: no namespace, import or extern \"C\""});
          d->filePrivate = true;
        }
      if (inRecovery)
      {
        scratch.resize(mark);
        if (pos == start)
          advance();
        recover();
        ErrorDecl* e = decl<ErrorDecl>(start);
        e->endToken = pos;
        pushDecl(e);
      }
    }

    struct TemplateHeader
    {
      Span<Decl*> params;
      bool present = false;
      Expr* requiresClause = nullptr;
    };
    std::vector<uint32_t> templateTypeNames; // type parameters of the templates being parsed: known type names
    // The value variables of the for constexpr loops being parsed (`for constexpr (auto a :
    // alternatives(U))`): `a.type` names a type (design.md 8.5).
    std::vector<uint32_t> descriptorNames;

    bool atDescriptorType()
    {
      return atIdent() && at(P::Dot, 1) && atIdent(kwTypeMember, 2) &&
             std::find(descriptorNames.begin(), descriptorNames.end(), tok().id) != descriptorNames.end();
    }

    bool isTemplateTypeName(Type* t) const
    {
      if (!t || t->kind != NodeKind::NamedType)
        return false;
      const QualifiedName& name = static_cast<NamedType*>(t)->name;
      if (name.parts.size() != 1 || name.parts[0].hasTemplateArgs || name.parts[0].special != 0)
        return false;
      for (uint32_t symbol : templateTypeNames)
        if (symbol == name.parts[0].symbol)
          return true;
      return false;
    }

    void parseDeclarationInner(Scope scope, TemplateHeader templ)
    {
      pendingAttributes = 0;
      skipAttributes();
      uint32_t start = pos;
      if (at(TokenKind::End))
        return;
      if (cMode && scope == Scope::Class && atIdent() && at(P::Semi, 1) && cTypedefNames.count(tok().id) &&
          currentParent && currentParent->kind == NodeKind::ClassDecl)
      {
        anonymousMemberOfNamedType(static_cast<ClassDecl*>(currentParent), start);
        return;
      }
      if (accept(P::Semi))
      {
        pushDecl(decl<EmptyDecl>(start));
        return;
      }
      if (at(Kw::when_))
      {
        parseWhen(scope);
        return;
      }
      if (kppSource && atIdent(kwTest) && at(TokenKind::String, 1) && at(P::LBrace, 2) && (scope == Scope::Namespace || scope == Scope::Class))
      {
        parseTest(scope, start);
        return;
      }
      if (atMacroName() && !at(P::LParen, 1) && !at(P::ColonColon, 1) && !at(P::Less, 1) &&
          (at(TokenKind::End, 1) || at(P::LBracket, 1) || at(P::RBrace, 1) || at(TokenKind::Directive, 1) ||
           (at(TokenKind::Keyword, 1) && !isDeclStartKeyword(Kw(tok(1).id)))))
      {
        MacroDecl* m = decl<MacroDecl>(start);
        m->name = tok().id;
        advance();
        m->endToken = pos;
        pushDecl(m);
        return;
      }
      if (at(Kw::if_) && at(Kw::constexpr_, 1) && scope != Scope::Local)
      {
        parseStaticIf(scope, false);
        return;
      }
      if (at(Kw::template_))
      {
        parseTemplate(scope);
        return;
      }
      if (at(Kw::namespace_))
      {
        parseNamespace();
        return;
      }
      if (at(Kw::using_))
      {
        parseUsing(templ);
        return;
      }
      if (at(Kw::static_assert_))
      {
        StaticAssertDecl* s = decl<StaticAssertDecl>(start);
        advance();
        expect(P::LParen, "expected '('");
        s->cond = parseAssignment();
        if (accept(P::Comma))
          s->message = parseAssignment();
        expect(P::RParen, "expected ')'");
        expect(P::Semi, "expected ';'");
        pushDecl(s);
        return;
      }
      if (at(Kw::asm_) || (atIdent() && (spelling() == "__asm" || spelling() == "__asm__")))
      {
        advance();
        if (at(P::LParen))
          skipBalanced();
        else if (at(P::LBrace))
          skipBalanced();
        accept(P::Semi);
        pushDecl(decl<AsmDecl>(start));
        return;
      }
      if (at(Kw::extern_) && at(TokenKind::String, 1))
      {
        // extern "C" { ... } or extern "C" decl
        advance();
        bool savedExternC = externC;
        noSplice();
        externC = spelling() == "\"C\"";
        advance();
        if (atIdent(kwImport) && at(TokenKind::String, 1))
        {
          ImportDecl* imp = decl<ImportDecl>(start);
          advance();
          imp->pathToken = pos;
          noSplice();
          advance();
          expect(P::Semi, "expected ';' after import");
          externC = savedExternC;
          pushDecl(imp);
          return;
        }
        if (accept(P::LBrace))
        {
          while (!at(P::RBrace) && !at(TokenKind::End))
            parseDeclaration(scope);
          expect(P::RBrace, "expected '}'");
          externC = savedExternC;
          return;
        }
        size_t mark = scratch.size();
        parseDeclarationInner(scope, templ);
        externC = savedExternC;
        // `extern "C" const IID IID_X;` declares a variable defined elsewhere, as `extern` does
        // (inside `extern "C" { ... }` one without the word is a definition).
        for (size_t i = mark; i < scratch.size(); ++i)
          if (auto* d = static_cast<Decl*>(scratch[i]); d && d->kind == NodeKind::VariableDecl && !static_cast<VariableDecl*>(d)->init)
            d->flags |= DF_Extern;
        return;
      }
      if (at(Kw::public_) || at(Kw::private_) || at(Kw::protected_))
      {
        if (at(P::Colon, 1))
        {
          if (!at(Kw::public_) && inStruct())
            out.structRule.push_back({pos, "a struct's members are all public (a struct is a public value type, design.md \"struct and class\"): make it a class"});
          currentAccess = at(Kw::public_) ? Access::Public : at(Kw::private_) ? Access::Private : Access::Protected;
          advance();
          advance();
          return;
        }
      }
      if (at(Kw::concept_))
      {
        // concept Name = constraint-expr;
        advance();
        VariableDecl* v = decl<VariableDecl>(start);
        v->flags |= DF_Constexpr;
        if (!parseQualifiedName(v->name, false, false))
          error("expected concept name");
        expect(P::Assign, "expected '='");
        v->templateParams = templ.params;
        v->isTemplate = templ.present;
        // The parameters of a requires-expression in it (`requires(const T& value)`) belong to the
        // concept: their types name its template parameters.
        Decl* savedParent = currentParent;
        currentParent = v;
        v->init = parseAssignment();
        currentParent = savedParent;
        expect(P::Semi, "expected ';'");
        pushDecl(v);
        return;
      }

      // Specifiers.
      uint32_t flags = 0;
      parseDeclSpecifiers(flags);
      if ((flags & DF_Friend) && (atIdent() || at(P::ColonColon)))
      {
        // `friend Description;`: a friend class named without the keyword, the same declaration
        // as `friend class Description;` (and as little of a member as that one).
        uint32_t save = pos;
        ClassDecl* c = decl<ClassDecl>(pos);
        c->keyword = Kw::class_;
        c->flags |= flags;
        if (parseQualifiedName(c->name, true, false) && at(P::Semi))
        {
          advance();
          pushDecl(c);
          return;
        }
        restore(save);
      }
      if (at(Kw::using_))
      {
        parseUsing(templ);
        return;
      }
      if (at(Kw::template_))
      {
        parseTemplate(scope, flags);
        return;
      }
      if (at(Kw::namespace_))
      {
        parseNamespace();
        return;
      }
      if (at(Kw::static_assert_) || (at(Kw::extern_) && at(TokenKind::String, 1)))
      {
        parseDeclarationInner(scope, templ);
        return;
      }

      // Class / enum definitions (which may be followed by declarators). `const struct { ... }
      // passes[] {...};`: the const is the declarators' (or, when the class is only named, the
      // type's: `const struct Point* p`).
      if (at(Kw::const_) && (at(Kw::class_, 1) || at(Kw::struct_, 1) || at(Kw::union_, 1) || at(Kw::enum_, 1)))
      {
        advance();
        pendingConst = true;
      }
      if (at(Kw::class_) || at(Kw::struct_) || at(Kw::union_))
      {
        if (parseClassOrElaborated(scope, flags, templ))
          return;
      }
      else if (at(Kw::enum_))
      {
        if (parseEnumOrElaborated(scope, flags, templ))
          return;
      }

      // UnitTest++'s `SUITE(Name) { ... }` in the C++ tests the converter reads: the namespace
      // SuiteName it expands to, its declarations parsed as any namespace's (so that the
      // converter's typed rules reach the tests, and `SuiteName::x` resolves), not skipped as
      // the macro invocation below.
      if (scope == Scope::Namespace && atIdent(kwSuite) && at(P::LParen, 1) && atIdent(2) && at(P::RParen, 3) && at(P::LBrace, 4))
      {
        uint32_t start = pos;
        NamespaceDecl* ns = decl<NamespaceDecl>(start);
        ns->name = symbols.intern("Suite" + std::string(symbols.name(tok(2).id)));
        NamePart part;
        part.symbol = ns->name;
        ns->nested.parts.data = arena.array(&part, 1);
        ns->nested.parts.count = 1;
        for (int k = 0; k < 4; ++k)
          advance();
        expect(P::LBrace, "expected '{' after SUITE(...)");
        Decl* savedParent = currentParent;
        Access savedAccess = currentAccess;
        currentParent = ns;
        currentAccess = Access::Default;
        size_t mark = scratch.size();
        while (!at(P::RBrace) && !at(TokenKind::End))
          parseDeclaration(Scope::Namespace);
        ns->decls = makeSpan<Decl*>(mark);
        currentParent = savedParent;
        currentAccess = savedAccess;
        expect(P::RBrace, "expected '}' to close SUITE(...)");
        pushDecl(ns);
        return;
      }
      // A test, `TEST(Name) { ... }`, `TEST_FIXTURE(Fixture, Name) { ... }`: its body parsed as
      // UnitTest++ makes it, so that the converter's typed rules reach the statements (the
      // converter writes the test out by text, as before). A fixture's test is the member
      // `void Run()` of the class FixtureNameHelper derived from the fixture, whose members it
      // uses; a TEST (the empty NullFixture's) the function `void TestBody_Name()`.
      if (scope == Scope::Namespace && atMacroName() && at(P::LParen, 1) &&
          (spelling(0) == "TEST" || spelling(0) == "TEST_FIXTURE" || spelling(0) == "TEST_PARAMETRISED_FIXTURE"))
      {
        uint32_t save = pos;
        uint32_t start = pos;
        bool fixture = spelling(0) != "TEST";
        advance();
        uint32_t fixtureToken = pos + 1;
        skipBalanced();
        uint32_t nameToken = pos - 2;
        if (fixture && at(P::LBrace) && toks[nameToken].getKind() == TokenKind::Identifier)
        {
          // The fixture's type, from the macro's first argument.
          uint32_t afterArguments = pos;
          pos = fixtureToken;
          NamedType* base = node<NamedType>(pos);
          bool parsed = parseQualifiedName(base->name, true, false) && at(P::Comma);
          pos = afterArguments;
          if (parsed)
          {
            ClassDecl* c = decl<ClassDecl>(start);
            c->keyword = Kw::class_;
            NamePart classPart;
            classPart.symbol = symbols.intern(std::string(symbols.name(base->name.last().symbol)) + std::string(symbols.name(toks[nameToken].id)) + "Helper");
            c->name.parts = makeSpanOf(std::vector<NamePart>{classPart});
            c->isDefinition = true;
            ClassDecl::Base b{base, Access::Public, false, false};
            c->bases = makeSpanOf(std::vector<ClassDecl::Base>{b});
            Decl* savedParent = currentParent;
            Access savedAccess = currentAccess;
            uint32_t savedClassName = currentClassName;
            currentParent = c;
            currentAccess = Access::Public;
            currentClassName = classPart.symbol;
            size_t mark = scratch.size();
            FunctionDecl* run = decl<FunctionDecl>(start);
            BuiltinType* voidType = node<BuiltinType>(start);
            voidType->keyword = Kw::void_;
            run->returnType = voidType;
            NamePart runPart;
            runPart.symbol = symbols.intern("Run");
            run->name.parts = makeSpanOf(std::vector<NamePart>{runPart});
            run->body = parseBlock();
            pushDecl(run);
            c->members = makeSpan<Decl*>(mark);
            checkWhenClash(c);
            currentParent = savedParent;
            currentAccess = savedAccess;
            currentClassName = savedClassName;
            pushDecl(c);
            return;
          }
        }
        if (at(P::LBrace) && toks[nameToken].getKind() == TokenKind::Identifier)
        {
          FunctionDecl* f = decl<FunctionDecl>(start);
          BuiltinType* voidType = node<BuiltinType>(start);
          voidType->keyword = Kw::void_;
          f->returnType = voidType;
          NamePart part;
          part.symbol = symbols.intern("TestBody_" + std::string(symbols.name(toks[pos - 2].id)));
          f->name.parts = makeSpanOf(std::vector<NamePart>{part});
          f->body = parseBlock();
          pushDecl(f);
          return;
        }
        restore(save);
      }

      // Macro invocation used as a declaration: MACRO(...) ; or MACRO(...) { ... }
      if (atMacroName() && at(P::LParen, 1) && !(scope == Scope::Class && tok().id == currentClassName))
      {
        uint32_t save = pos;
        uint32_t name = tok().id;
        advance();
        skipBalanced();
        if (at(P::Semi) || at(P::RBrace) || atMacroName() || at(TokenKind::Keyword) || at(TokenKind::End) ||
            (atIdent() && !at(P::LParen, 1) && !at(P::ColonColon, 1) && !at(P::Less, 1)) || at(P::LBrace) ||
            (at(P::LBracket) && at(P::LBracket, 1)))
        {
          if (at(P::LBrace))
            skipBalanced();
          accept(P::Semi);
          MacroDecl* m = decl<MacroDecl>(start);
          m->name = name;
          m->endToken = pos;
          pushDecl(m);
          return;
        }
        restore(save);
      }
      // Bare ALL_CAPS macro statement at class/namespace scope: `MACRO;` or `MACRO` on its own line.
      if (atMacroName() && at(P::Semi, 1))
      {
        MacroDecl* m = decl<MacroDecl>(start);
        m->name = tok().id;
        advance();
        advance();
        m->endToken = pos;
        pushDecl(m);
        return;
      }

      // Constructor / destructor / conversion operator: a name followed by '(' with no type.
      Type* type = nullptr;
      {
        uint32_t save = pos;
        QualifiedName name;
        size_t mark = scratch.size();
        if (parseQualifiedName(name, true) && at(P::LParen))
        {
          if (isConstructorLikeName(name, scope))
          {
            Declarator d;
            d.name = name;
            parseFunctionSuffix(d);
            finishFunction(start, nullptr, d, flags, templ, scope);
            return;
          }
          if ((templ.present || scope == Scope::Namespace) && deductionGuideAhead())
          {
            // `template<typename... Ts> Overload(Ts...) -> Overload<Ts...>;`: a deduction guide,
            // which class template argument deduction tries beside the constructors.
            DeductionGuideDecl* g = decl<DeductionGuideDecl>(start);
            g->name = name;
            g->templateParams = templ.params;
            g->isTemplate = templ.present;
            g->flags |= flags & DF_Explicit;
            Declarator d;
            d.name = name;
            parseFunctionSuffix(d);
            g->params = d.params;
            g->target = d.trailingReturn;
            if (!g->target)
              error("expected the class type the deduction guide gives");
            expect(P::Semi, "expected ';' after the deduction guide");
            pushDecl(g);
            return;
          }
        }
        restore(save);
        scratch.resize(mark);
      }

      type = parseType();
      if (!type)
      {
        error("expected declaration");
        return;
      }
      parseDeclSpecifiers(flags); // C++ lets them follow the type: `float static f()`, `void inline g()`, `const static int x[]`
      if (pendingConst)
      {
        // (the base type's, under the pointer: `const static char* name` is a `const char*`)
        Type* base = stripSuffixes(type);
        ConstType* c = node<ConstType>(start);
        c->inner = base;
        type = rebase(type, base, c);
        pendingConst = false;
      }
      // Declarators.
      Type* baseType = stripSuffixes(type);
      bool first = true;
      for (;;)
      {
        Declarator d;
        d.type = first ? type : baseType;
        parseDeclarator(d, scope);
        if (inRecovery)
          return;
        // `Buf;`, `const Buf*;`: a type and no name declares nothing, which C++ makes ill-formed
        // ([dcl.dcl]); an unnamed bitfield (`int : 3;`) and a structured binding are declarations.
        if (first && d.name.empty() && !d.isFunction && !d.bitfield && d.structuredBindings.empty() && scope != Scope::Local &&
            !conditionMode && !cMode)
          out.diagnostics.push_back({start, "the declaration declares nothing"});
        first = false;
        if (d.isFunction && (flags & DF_Typedef))
          functionTypeOfDeclarator(d, start); // `typedef void WriteFunc(void* context, int size);`: an alias of the function type
        if (d.isFunction)
        {
          finishFunction(start, d.type, d, flags, templ, scope);
          return;
        }
        VariableDecl* v = decl<VariableDecl>(start);
        v->type = d.type;
        v->name = d.name;
        if (scope == Scope::Local && v->name.parts.size() == 1 && v->name.parts[0].symbol == kwUnderscore)
        {
          // `_`: an unnamed local that lives to the end of the scope; every `_` is a distinct object.
          NamePart part;
          part.symbol = symbols.intern("_" + std::to_string(start));
          v->name.parts = makeSpanOf(std::vector<NamePart>{part});
        }
        v->flags |= flags | d.flags;
        v->bitfield = d.bitfield;
        v->structuredBindings = d.structuredBindings;
        v->templateParams = templ.params;
        v->isTemplate = templ.present;
        // A variable template's initializer belongs to it, as a concept's does: the parameters of
        // a requires-expression in it (`requires (T& t) { t.begin(); }`) name its parameters.
        Decl* savedParent = currentParent;
        if (templ.present && !templ.params.empty())
          currentParent = v;
        parseInitializer(v);
        currentParent = savedParent;
        pushDecl(v);
        if (accept(P::Comma))
          continue;
        if (conditionMode && at(P::RParen))
          break;
        if (!expect(P::Semi, "expected ';' after declaration"))
          return;
        break;
      }
    }

    Type* stripSuffixes(Type* t)
    {
      for (;;)
      {
        if (t->kind == NodeKind::PointerType)
          t = static_cast<PointerType*>(t)->pointee;
        else if (t->kind == NodeKind::LRefType)
          t = static_cast<LRefType*>(t)->referenced;
        else if (t->kind == NodeKind::RRefType)
          t = static_cast<RRefType*>(t)->referenced;
        else if (t->kind == NodeKind::ConstType && static_cast<ConstType*>(t)->inner->kind == NodeKind::PointerType)
          t = static_cast<ConstType*>(t)->inner;
        else
          return t;
      }
    }

    bool isConstructorLikeName(const QualifiedName& name, Scope scope) const
    {
      const NamePart& last = name.last();
      if (last.special == 1 || last.special == 3)
        return true;
      if (last.special == 2)
        return false;
      if (name.parts.size() >= 2 && name.parts[name.parts.size() - 2].symbol == last.symbol)
        return true;
      if (scope == Scope::Class && name.parts.size() == 1 && last.symbol == currentClassName)
        return true;
      return false;
    }

    void parseDeclSpecifiers(uint32_t& flags)
    {
      for (;;)
      {
        skipAttributes();
        if (at(Kw::const_) && (at(Kw::static_, 1) || at(Kw::inline_, 1) || at(Kw::constexpr_, 1) || at(Kw::extern_, 1) ||
                              at(Kw::thread_local_, 1) || at(Kw::mutable_, 1)))
        {
          advance(); // `const static int x`: the const belongs to the type, read after the specifiers
          pendingConst = true;
        }
        else if (accept(Kw::static_)) flags |= DF_Static;
        else if (accept(Kw::virtual_)) flags |= DF_Virtual;
        else if (accept(Kw::inline_)) flags |= DF_Inline;
        else if (accept(Kw::constexpr_)) flags |= DF_Constexpr;
        else if (accept(Kw::consteval_)) flags |= DF_Consteval;
        else if (accept(Kw::constinit_)) flags |= DF_Constexpr;
        else if (accept(Kw::friend_)) flags |= DF_Friend;
        else if (accept(Kw::extern_)) flags |= DF_Extern;
        else if (accept(Kw::mutable_)) flags |= DF_Mutable;
        else if (accept(Kw::thread_local_)) flags |= DF_ThreadLocal;
        else if (accept(Kw::typedef_)) flags |= DF_Typedef;
        else if (accept(Kw::register_)) flags |= DF_Register;
        else if (accept(Kw::export_)) flags |= DF_Export;
        else if (atIdent(kwImplicit) && !at(P::LParen, 1)) { advance(); flags |= DF_Implicit; }
        else if (atIdent(kwBothConst) && !at(P::LParen, 1)) { advance(); flags |= DF_BothConst; }
        else if (atIdent(kwAccumulate) && !at(P::LParen, 1) && !at(P::ColonColon, 1)) { advance(); flags |= DF_Accumulate; }
        else if (atIdent(kwConditional) && at(P::LParen, 1))
        {
          // conditional(expr): the function exists only while expr, a config constant, is true.
          advance();
          advance();
          pendingCondition = parseExpression();
          expect(P::RParen, "expected ')' after the condition");
        }
        else if (at(Kw::explicit_))
        {
          advance();
          flags |= DF_Explicit;
          if (at(P::LParen))
          {
            // explicit(condition): explicit only while the condition, a template parameter usually, is true.
            advance();
            pendingExplicitCondition = parseExpression();
            expect(P::RParen, "expected ')' after the explicit condition");
          }
        }
        else if (skipMacroSpecifier())
        {
        }
        else
          return;
      }
    }

    // --- declarators ---

    // `auto`, possibly under `const` and `&`/`&&`: the type a structured binding is declared with.
    static bool autoUnderRefs(Type* t)
    {
      while (t)
      {
        if (t->kind == NodeKind::LRefType)
          t = static_cast<LRefType*>(t)->referenced;
        else if (t->kind == NodeKind::RRefType)
          t = static_cast<RRefType*>(t)->referenced;
        else if (t->kind == NodeKind::ConstType)
          t = static_cast<ConstType*>(t)->inner;
        else
          break;
      }
      return t && t->kind == NodeKind::AutoType;
    }

    void parseDeclarator(Declarator& d, Scope scope)
    {
      d.type = parseTypeSuffixes(d.type);
      skipAttributes();
      // C headers parenthesise function names to suppress macro expansion:
      //   int (lua_gettop) (lua_State *L);
      // Strip the wrapping parens so the name is parsed normally.
      if (cMode && at(P::LParen) && atIdent(1) && at(P::RParen, 2))
      {
        advance();           // skip '('
        parseQualifiedName(d.name, true); // consume the function name
        advance();           // skip ')'
        skipAttributes();
        if (at(P::LParen) && !d.name.empty())
        {
          if (looksLikeParameterList(scope))
          {
            d.isFunction = true;
            parseFunctionSuffix(d);
          }
        }
        return;
      }
      // Function pointer / reference-to-array: ( * name )(params) or ( & name )[N]
      if (at(P::LParen) && (((at(P::Star, 1) || at(P::Amp, 1) || at(P::AndAnd, 1)) &&
                             (atIdent(2) || at(Kw::const_, 2) || at(P::Star, 2) || atIdent(1) && false)) ||
                            (atIdent(1) && at(P::ColonColon, 2) && at(P::Star, 3))))
      {
        advance();
        Type* inner = parseTypeSuffixes(d.type);
        if (atIdent(0) && at(P::ColonColon, 1))
        {
          // Pointer to member: ( C::* name )
          QualifiedName q;
          parseQualifiedName(q, true, false);
          if (accept(P::Star))
          {
            MemberPointerType* m = node<MemberPointerType>(pos);
            NamedType* cls = node<NamedType>(pos);
            cls->name = q;
            m->classType = cls;
            m->pointee = inner;
            inner = m;
          }
          inner = parseTypeSuffixes(inner);
        }
        if (atIdent())
          parseQualifiedName(d.name, true);
        // Array suffix inside the parens.
        inner = parseArraySuffixes(inner);
        if (!expect(P::RParen, "expected ')' in declarator"))
          return;
        if (at(P::LParen))
        {
          // Pointer to function: the declared type is a pointer to FunctionType.
          FunctionType* f = node<FunctionType>(pos);
          f->result = d.type;
          Declarator tmp;
          parseFunctionSuffix(tmp);
          f->params = tmp.params;
          f->isVariadic = (tmp.flags & DF_Variadic) != 0;
          f->isConst = (tmp.flags & DF_Const) != 0;
          f->isNoexcept = (tmp.flags & DF_Noexcept) != 0;
          if (inner && inner->kind == NodeKind::MemberPointerType)
          {
            // Pointer to member function: `R (C::*name)(params)`.
            static_cast<MemberPointerType*>(inner)->pointee = f;
            d.type = inner;
          }
          else
          {
            PointerType* p = node<PointerType>(pos);
            p->pointee = f;
            d.type = p;
          }
        }
        else if (at(P::LBracket) && inner != d.type)
        {
          // `const T (&array)[N]`, `int (*rows)[4]`: the bound belongs to the element type, which
          // the reference or pointer inside the parentheses then wraps (not an array of them).
          Type* arrayed = parseArraySuffixes(d.type);
          d.type = rebase(inner, d.type, arrayed);
        }
        else
          d.type = inner;
        d.type = parseArraySuffixes(d.type);
        return;
      }
      // Structured binding: auto [a, b] = ..., also auto& [a, b] and const auto& [a, b]
      if (at(P::LBracket) && autoUnderRefs(d.type))
      {
        advance();
        std::vector<Expr*> names;
        while (atIdent())
        {
          NameExpr* n = node<NameExpr>(pos);
          NamePart part;
          part.symbol = tok().id;
          advance();
          n->name.parts = makeSpanOf(std::vector<NamePart>{part});
          names.push_back(n);
          if (!accept(P::Comma))
            break;
        }
        expect(P::RBracket, "expected ']'");
        d.structuredBindings = makeSpanOf(names);
        return;
      }
      if (atMacroName() && (atIdent(1) || at(P::ColonColon, 1)) && !at(P::ColonColon, 1))
        advance();
      if (atIdent() || at(P::ColonColon) || at(P::Tilde) || at(Kw::operator_))
      {
        if (!parseQualifiedName(d.name, true))
        {
          error("expected declarator name");
          return;
        }
      }
      skipAttributes();
      if (at(P::LBracket) && !at(P::LBracket, 1))
      {
        d.type = parseArraySuffixes(d.type);
        // C's attributes after the bounds: `long mc_fpstate[64] __attribute__((__aligned__(16)))`
        // (FreeBSD's machine/ucontext.h), whose alignment the member takes (pushDecl).
        if (cMode)
          skipAttributes();
      }
      if (at(P::LParen) && !d.name.empty())
      {
        // Function, unless this is a variable with parenthesised initializer. Decide by looking
        // at the contents: a parameter list starts with a type or is empty.
        if (looksLikeParameterList(scope))
        {
          if (scope == Scope::Namespace && !cMode)
            d.objectArgs = bareNameArguments();
          d.isFunction = true;
          parseFunctionSuffix(d);
        }
      }
      if (at(P::Colon) && !at(P::ColonColon) && scope == Scope::Class && !d.isFunction)
      {
        advance();
        d.bitfield = parseConditionalExpression();
      }
    }

    Type* parseArraySuffix(Type* element)
    {
      ArrayType* a = node<ArrayType>(pos);
      advance();
      a->element = element;
      a->size = at(P::RBracket) ? nullptr : parseExpression();
      expect(P::RBracket, "expected ']'");
      return a;
    }

    // Every `[N]` after a declarator: `int a[2][3]` is an array of 2 arrays of 3 ints, so the
    // last suffix is the innermost type.
    Type* parseArraySuffixes(Type* element)
    {
      std::vector<ArrayType*> suffixes;
      while (at(P::LBracket))
        suffixes.push_back(static_cast<ArrayType*>(parseArraySuffix(nullptr)));
      Type* result = element;
      for (size_t i = suffixes.size(); i-- > 0;)
      {
        suffixes[i]->element = result;
        result = suffixes[i];
      }
      return result;
    }

    // At the `(` after a declarator's name, `(a, m);`: only names, then the declaration's end.
    // Without knowing what the names are, a function with unnamed parameters of those types, or an
    // object constructed from those variables; the names as expressions, for the resolver to
    // decide (design.md rule 2), or none.
    Span<Expr*> bareNameArguments()
    {
      // Each a name, qualified or not (`HeavyMode::Disabled`), then `,` or `)`.
      std::vector<std::pair<int, std::vector<NamePart>>> names; // (first token, parts)
      int i = 1;
      for (;;)
      {
        int first = i;
        std::vector<NamePart> parts;
        while (atIdent(i))
        {
          NamePart part;
          part.symbol = tok(i).id;
          parts.push_back(part);
          if (!at(P::ColonColon, i + 1))
            break;
          i += 2;
        }
        if (parts.empty() || !atIdent(i))
          return {};
        names.push_back({first, std::move(parts)});
        if (at(P::Comma, i + 1))
        {
          i += 2;
          continue;
        }
        if (!at(P::RParen, i + 1) || !at(P::Semi, i + 2))
          return {};
        break;
      }
      size_t mark = scratch.size();
      for (auto& [first, parts] : names)
      {
        NameExpr* n = node<NameExpr>(pos + first);
        n->name.parts = makeSpanOf(parts);
        scratch.push_back(n);
      }
      return makeSpan<Expr*>(mark);
    }

    bool looksLikeParameterList(Scope scope)
    {
      if (at(TokenKind::Number, 1) || at(TokenKind::String, 1) || at(TokenKind::Char, 1) || at(P::Minus, 1) ||
          at(Kw::true_, 1) || at(Kw::false_, 1) || at(Kw::nullptr_, 1) || at(Kw::this_, 1))
        return false;
      if (scope != Scope::Local)
        return !laterArgumentIsLiteral() && !firstParameterIsExpression(); // At namespace/class scope `name(...)` is a function.
      // Local: `T x(args);` is a variable, `T f(int);` a function (rare). Guess: parameter list if
      // empty or starts with a builtin type keyword / const / `void`.
      if (at(P::RParen, 1))
        return true;
      const Token& t = tok(1);
      if (t.getKind() == TokenKind::Keyword && (isBuiltinTypeKeyword(Kw(t.id)) || t.is(Kw::const_)))
        return !at(P::LParen, 2) && !at(P::LBrace, 2) && !at(P::ColonColon, 2) && !at(P::Dot, 2);
      return false;
    }

    // At the `(` after a declarator's name: whether an argument after the first starts with a
    // literal or a brace list, `Box other(HeavyMode::Disabled, 4);`, `Adder added(suite, {"a"});`,
    // which no parameter does. Template argument
    // lists are skipped (`Array<int, 4> a`); a `<` that is a comparison only hides a literal,
    // leaving the list to the other checks.
    bool laterArgumentIsLiteral()
    {
      int depth = 0;
      for (int i = 1;; ++i)
      {
        if (at(TokenKind::End, i))
          return false;
        if (at(P::LParen, i) || at(P::LBracket, i) || at(P::LBrace, i) || at(P::Less, i))
          ++depth;
        else if (at(P::RParen, i) || at(P::RBracket, i) || at(P::RBrace, i) || at(P::Greater, i))
        {
          if (depth == 0)
            return false;
          --depth;
        }
        else if (at(P::Shr, i))
          depth = depth >= 2 ? depth - 2 : 0;
        else if (at(P::Semi, i))
          return false;
        else if (depth == 0 && at(P::Comma, i) &&
                 (at(TokenKind::Number, i + 1) || at(TokenKind::String, i + 1) || at(TokenKind::Char, i + 1) ||
                  at(Kw::true_, i + 1) || at(Kw::false_, i + 1) || at(Kw::nullptr_, i + 1) || at(P::LBrace, i + 1)))
          return true;
      }
    }

    // At the `(` after a declarator's name: whether what starts inside is an expression, so that
    // `constexpr ID EMPTY(ID(-1));` declares a variable. A parameter is a type followed by a
    // declarator, a default argument, `,` or `)`. `Type(args)` and `Type{args}` are expressions
    // (design.md, kpp v0 rule 2: no most vexing parse, so `Widget w(Size(s));` is a variable),
    // except a parenthesised declarator followed by a parameter list or an array bound
    // (`int (*f)(int)`, `T (&a)[3]`, rule 4).
    bool firstParameterIsExpression()
    {
      uint32_t save = pos;
      size_t mark = scratch.size();
      bool savedRecovery = inRecovery;
      size_t savedDiagnostics = out.diagnostics.size();
      uint32_t nodes = out.nodeCount;
      advance(); // '('
      bool expression = at(P::Star) || at(P::Amp) || at(P::LParen) || at(P::Bang) || at(P::Tilde) || at(P::Plus) ||
                        at(P::PlusPlus) || at(P::MinusMinus);
      if (!expression && (atIdent() || at(P::ColonColon) || (at(TokenKind::Keyword) && isBuiltinTypeKeyword(Kw(tok().id)))))
      {
        Type* t = parseType();
        if (!t || inRecovery || at(P::LBrace))
          expression = true;
        else if (at(P::LParen))
        {
          int i = 1;
          if (atIdent(1) && at(P::ColonColon, 2) && at(P::Star, 3))
            i = 4; // `R (C::*name)(...)`
          else
            while (at(P::Star, i) || at(P::Amp, i) || at(P::AndAnd, i) || at(Kw::const_, i))
              ++i;
          if (atIdent(i))
            ++i;
          bool declarator = i > 1 && at(P::RParen, i) && (at(P::LParen, i + 1) || at(P::LBracket, i + 1));
          expression = !declarator;
        }
        else
          expression = !(atIdent() || at(P::Star) || at(P::Amp) || at(P::AndAnd) || at(P::Comma) || at(P::RParen) ||
                         at(P::Assign) || at(P::LBracket) || at(P::Ellipsis) || at(Kw::const_) || at(Kw::volatile_));
      }
      restore(save);
      scratch.resize(mark);
      inRecovery = savedRecovery;
      out.diagnostics.resize(savedDiagnostics);
      out.nodeCount = nodes;
      return expression;
    }

    // Parses `(params) cv ref noexcept -> ret override final = 0/default/delete requires ...`.
    void parseFunctionSuffix(Declarator& d)
    {
      expect(P::LParen, "expected '('");
      size_t mark = scratch.size();
      if (at(Kw::void_) && at(P::RParen, 1))
        advance(); // C's `(void)`: no parameters
      else if (!at(P::RParen))
      {
        for (;;)
        {
          if (accept(P::Ellipsis))
          {
            d.flags |= DF_Variadic;
            break;
          }
          Decl* p = parseParameter();
          if (!p)
            break;
          scratch.push_back(p);
          if (!accept(P::Comma))
            break;
        }
      }
      d.params = makeSpan<Decl*>(mark);
      if (!expect(P::RParen, "expected ')' after parameters"))
        return;
      for (;;)
      {
        skipAttributes();
        if (accept(Kw::const_)) d.flags |= DF_Const;
        else if (accept(Kw::volatile_)) d.flags |= DF_Volatile;
        else if (accept(P::Amp) || accept(P::AndAnd)) d.flags |= DF_RefQual;
        else if (at(Kw::noexcept_))
        {
          advance();
          d.flags |= DF_Noexcept;
          if (accept(P::LParen))
          {
            // `noexcept(false)`, `noexcept(isNothrowMoveConstructible<T>)`: noexcept only while the
            // constant is true (FunctionDecl::noexceptCondition), never whatever it says.
            d.noexceptCondition = parseExpression();
            expect(P::RParen, "expected ')' after the noexcept condition");
          }
        }
        else if (at(Kw::throw_) && at(P::LParen, 1))
        {
          advance();
          skipBalanced();
        }
        else if (atIdent(0) && spelling() == "override") { advance(); d.flags |= DF_Override; }
        else if (atIdent(0) && spelling() == "final") { advance(); d.flags |= DF_Final; }
        else if (at(P::Arrow))
        {
          advance();
          d.trailingReturn = parseType();
          if (!d.trailingReturn)
            error("expected trailing return type");
        }
        else if (at(Kw::requires_))
        {
          advance();
          d.requiresClause = parseRequiresClause();
        }
        else if (at(P::Assign))
        {
          if (at(TokenKind::Number, 1)) { advance(); advance(); d.flags |= DF_Pure; }
          else if (at(Kw::default_, 1)) { advance(); advance(); d.flags |= DF_Default; }
          else if (at(Kw::delete_, 1)) { advance(); advance(); d.flags |= DF_Delete; }
          else return;
        }
        else if (atMacroName() && !at(P::LBrace, 1) && !at(P::Semi, 1) && !at(P::Colon, 1))
        {
          // e.g. `void f() KPP_NORETURN;`
          advance();
          if (at(P::LParen))
            skipBalanced();
        }
        else if (atMacroName() && (at(P::LBrace, 1) || at(P::Semi, 1) || at(P::Colon, 1)))
        {
          advance();
        }
        else
          return;
      }
    }

    Decl* parseParameter()
    {
      uint32_t start = pos;
      skipAttributes();
      uint32_t flags = 0;
      parseDeclSpecifiers(flags);
      ParamDecl* p = decl<ParamDecl>(start);
      p->type = parseType();
      if (!p->type)
      {
        error("expected parameter type");
        return nullptr;
      }
      if (accept(P::Ellipsis))
        p->isPack = true;
      Declarator d;
      d.type = p->type;
      parseDeclarator(d, Scope::Local);
      p->type = d.type;
      if (!d.name.empty())
        p->name = d.name.last().symbol;
      if (d.isFunction)
      {
        functionTypeOfDeclarator(d, start);
        p->type = d.type;
      }
      if (accept(P::Assign))
      {
        if (atIdent(kwCaller) && (at(P::Comma, 1) || at(P::RParen, 1)))
        {
          p->defaultValue = node<CallerExpr>(pos); // the compiler fills in the call site
          advance();
        }
        else
          p->defaultValue = parseAssignment();
      }
      else if (at(P::LBrace))
        p->defaultValue = parseBraceInit();
      return p;
    }

    // A function declarator that declares no function (a parameter `int f(int)`, a typedef's
    // name): its type becomes the function type, and it is a declarator of that type.
    void functionTypeOfDeclarator(Declarator& d, uint32_t start)
    {
      FunctionType* f = node<FunctionType>(start);
      f->result = d.type;
      f->params = d.params;
      f->isVariadic = (d.flags & DF_Variadic) != 0;
      d.type = f;
      d.flags &= ~DF_Variadic;
      d.isFunction = false;
    }

    void finishFunction(uint32_t start, Type* returnType, Declarator& d, uint32_t flags, TemplateHeader templ, Scope scope)
    {
      FunctionDecl* f = decl<FunctionDecl>(start);
      f->returnType = returnType;
      f->name = d.name;
      f->params = d.params;
      f->objectArgs = d.objectArgs;
      for (Decl* p : d.params)
        p->parent = f; // Parameters belong to the function (its template parameters are in scope for them).
      f->flags |= flags | d.flags;
      f->trailingReturn = d.trailingReturn;
      f->requiresClause = d.requiresClause ? d.requiresClause : templ.requiresClause;
      adoptRequiresParams(f->requiresClause, f);
      f->condition = pendingCondition;
      pendingCondition = nullptr;
      f->explicitCondition = pendingExplicitCondition;
      pendingExplicitCondition = nullptr;
      f->noexceptCondition = d.noexceptCondition;
      f->attributes = pendingAttributes;
      pendingAttributes = 0;
      f->templateParams = templ.params;
      f->isTemplate = templ.present;
      // `void f(const auto& x)`, `int get(lua_State* L, auto PrototypeT::*... members)`: an
      // abbreviated function template, its `auto`s invented parameters after the written ones.
      std::vector<Decl*> invented(templ.params.begin(), templ.params.end());
      if (inventAutoParams(f->params, invented))
      {
        f->templateParams = makeSpanOf(invented);
        f->isTemplate = true;
      }
      const NamePart& last = d.name.last();
      if (last.special == 1) f->flags |= DF_Destructor;
      else if (last.special == 2) f->flags |= DF_Operator;
      else if (last.special == 3) f->flags |= DF_Conversion;
      else if (!returnType) f->flags |= DF_Constructor;
      (void)scope;
      bool functionTry = accept(Kw::try_);
      // Constructor initializers.
      if (at(P::Colon))
      {
        advance();
        std::vector<FunctionDecl::CtorInit> inits;
        for (;;)
        {
          FunctionDecl::CtorInit init{};
          if (!parseQualifiedName(init.member, true, false))
          {
            error("expected member initializer");
            break;
          }
          size_t mark = scratch.size();
          if (accept(P::LParen))
          {
            parseExpressionList(P::RParen);
            init.braced = false;
            expect(P::RParen, "expected ')'");
          }
          else if (accept(P::LBrace))
          {
            parseExpressionList(P::RBrace);
            init.braced = true;
            expect(P::RBrace, "expected '}'");
          }
          else
          {
            error("expected '(' or '{' in member initializer");
            break;
          }
          init.args = makeSpan<Expr*>(mark);
          init.isPack = accept(P::Ellipsis);
          inits.push_back(init);
          if (!accept(P::Comma))
            break;
        }
        f->ctorInits = makeSpanOf(inits);
      }
      if (functionTry || at(Kw::try_))
      {
        // function-try-block
        accept(Kw::try_);
        f->body = parseBlock();
        while (at(Kw::catch_))
        {
          advance();
          if (at(P::LParen))
            skipBalanced();
          parseBlock();
        }
      }
      else if (at(P::LBrace))
      {
        Decl* savedParent = currentParent;
        currentParent = f;
        f->body = parseBlock();
        currentParent = savedParent;
        ++out.functionBodies;
      }
      else if (!expect(P::Semi, "expected ';' or function body"))
        return;
      pushDecl(f);
    }

    void parseInitializer(VariableDecl* v)
    {
      if (accept(P::Assign))
      {
        v->init = at(P::LBrace) ? parseBraceInit() : parseAssignment();
      }
      else if (at(P::LBrace))
      {
        v->init = parseBraceInit();
        v->bracedInit = true;
      }
      else if (at(P::LParen))
      {
        // Direct initialization `T x(args)`.
        uint32_t start = pos;
        advance();
        FunctionalCastExpr* c = node<FunctionalCastExpr>(start);
        c->type = v->type;
        c->braced = false;
        size_t mark = scratch.size();
        parseExpressionList(P::RParen);
        c->args = makeSpan<Expr*>(mark);
        expect(P::RParen, "expected ')'");
        v->init = c;
      }
    }

    // --- namespaces, using, templates ---

    void parseNamespace()
    {
      uint32_t start = pos;
      advance();
      NamespaceDecl* ns = decl<NamespaceDecl>(start);
      if (at(Kw::inline_))
      {
        advance();
        ns->isInline = true;
      }
      skipAttributes();
      if (atIdent())
      {
        if (at(P::Assign, 1))
        {
          // namespace alias
          UsingAliasDecl* alias = decl<UsingAliasDecl>(start);
          alias->name = tok().id;
          advance();
          advance();
          NamedType* n = node<NamedType>(pos);
          if (!parseQualifiedName(n->name, true, false))
            error("expected namespace name");
          alias->type = n;
          expect(P::Semi, "expected ';'");
          pushDecl(alias);
          return;
        }
        parseQualifiedName(ns->nested, false, false);
        ns->name = ns->nested.parts[0].symbol;
      }
      if (!expect(P::LBrace, "expected '{' after namespace"))
        return;
      Decl* savedParent = currentParent;
      Access savedAccess = currentAccess;
      currentParent = ns;
      currentAccess = Access::Default;
      size_t mark = scratch.size();
      while (!at(P::RBrace) && !at(TokenKind::End))
        parseDeclaration(Scope::Namespace);
      ns->decls = makeSpan<Decl*>(mark);
      currentParent = savedParent;
      currentAccess = savedAccess;
      expect(P::RBrace, "expected '}' to close namespace");
      pushDecl(ns);
    }

    void parseUsing(TemplateHeader templ)
    {
      uint32_t start = pos;
      advance();
      if (accept(Kw::namespace_))
      {
        UsingDirectiveDecl* u = decl<UsingDirectiveDecl>(start);
        if (!parseQualifiedName(u->name, true, false))
          error("expected namespace name");
        expect(P::Semi, "expected ';'");
        pushDecl(u);
        return;
      }
      bool isEnum = accept(Kw::enum_);
      if (atIdent() && at(P::Assign, 1))
      {
        UsingAliasDecl* u = decl<UsingAliasDecl>(start);
        u->name = tok().id;
        u->templateParams = templ.params;
        u->isTemplate = templ.present;
        advance();
        advance();
        skipAttributes();
        u->type = parseType();
        if (!u->type)
          error("expected type in alias");
        else
          u->type = parseAliasSuffixes(u->type);
        expect(P::Semi, "expected ';'");
        pushDecl(u);
        return;
      }
      for (;;)
      {
        UsingNameDecl* u = decl<UsingNameDecl>(start);
        u->isEnum = isEnum;
        accept(Kw::typename_);
        if (!parseQualifiedName(u->name, true))
        {
          error("expected name after using");
          return;
        }
        accept(P::Ellipsis);
        pushDecl(u);
        if (!accept(P::Comma))
          break;
      }
      expect(P::Semi, "expected ';'");
    }

    // The abstract declarator an alias may put after its type: `T[N]`, `R(Args)`, `R(*)(Args)`
    // (calling conventions are dropped from the tokens before parsing: one convention on x64).
    Type* parseAliasSuffixes(Type* type)
    {
      type = parseTypeSuffixes(type); // pointers, references, (*)(Args), C::*
      if (at(P::LBracket))
        return parseArraySuffixes(type);
      if (at(P::LParen) && !at(P::Star, 1))
      {
        // `using Unary = double(double);`: a function type.
        uint32_t start = pos;
        Declarator d;
        parseFunctionSuffix(d);
        FunctionType* f = node<FunctionType>(start);
        f->result = type;
        f->params = d.params;
        f->isConst = (d.flags & DF_Const) != 0;
        f->isNoexcept = (d.flags & DF_Noexcept) != 0;
        f->isVariadic = (d.flags & DF_Variadic) != 0;
        return f;
      }
      return type;
    }

    void parseTemplate(Scope scope, uint32_t flags = 0)
    {
      uint32_t start = pos;
      advance();
      TemplateHeader header;
      header.present = true;
      if (at(P::Less))
      {
        advance();
        size_t mark = scratch.size();
        while (!at(P::Greater) && !at(P::Shr) && !at(TokenKind::End))
        {
          Decl* p = parseTemplateParam();
          if (!p)
            break;
          scratch.push_back(p);
          if (!accept(P::Comma))
            break;
        }
        header.params = makeSpan<Decl*>(mark);
        if (!acceptTemplateClose())
        {
          error("expected '>' after template parameters");
          return;
        }
      }
      else
      {
        // Explicit instantiation: `template class X<int>;` / `template void f<int>(int);`
        header.present = false;
      }
      // The type parameters are type names while the declaration (and its body) is parsed.
      struct TypeNamesGuard
      {
        std::vector<uint32_t>& names;
        size_t mark;
        ~TypeNamesGuard() { names.resize(mark); }
      } typeNamesGuard{templateTypeNames, templateTypeNames.size()};
      for (Decl* p : header.params)
        if (p && p->kind == NodeKind::TemplateParamDecl)
          if (auto* tp = static_cast<TemplateParamDecl*>(p); tp->isType && tp->name != UINT32_MAX)
            templateTypeNames.push_back(tp->name);
      if (at(Kw::requires_))
      {
        advance();
        header.requiresClause = parseRequiresClause();
      }
      if (at(Kw::template_))
      {
        // Nested template header (member template of a class template).
        size_t mark = scratch.size();
        parseTemplate(scope, flags);
        // Attach the outer parameters to the produced declaration.
        if (Decl* d = lastPushedDecl(mark))
        {
          if (d->templateParams.empty())
            d->templateParams = header.params;
          else if (!header.params.empty())
          {
            // `template<class S> template<class F> S Frame<S>::merged(F)`: the class's parameters,
            // then the member's, as an out-of-line member of a class template lists them.
            std::vector<Decl*> all(header.params.begin(), header.params.end());
            all.insert(all.end(), d->templateParams.begin(), d->templateParams.end());
            d->templateParams = makeSpanOf(all);
          }
        }
        return;
      }
      uint32_t declFlags = flags;
      parseDeclSpecifiers(declFlags);
      (void)start;
      // An explicit instantiation (`template Widget* countsAsButtons<ItemID, double>(...);`,
      // `template class X<int>;`) declares nothing: the whole program is compiled together, so what
      // is used is instantiated. Kept, it was a function of its own without a body that calls chose
      // over the template, and only declared. A class's is recorded (ParsedFile::explicitInstantiations):
      // its static data members are built at startup, as C++ instantiates them all.
      struct DropExplicitInstantiation
      {
        std::vector<Node*>& scratch;
        size_t mark;
        bool drop;
        ~DropExplicitInstantiation() { if (drop && scratch.size() > mark) scratch.resize(mark); }
      } dropInstantiation{scratch, scratch.size(), !header.present && header.params.empty()};
      if (at(Kw::class_) || at(Kw::struct_) || at(Kw::union_))
      {
        uint32_t classToken = pos;
        if (parseClassOrElaborated(scope, declFlags, header))
        {
          if (dropInstantiation.drop && !((flags | declFlags) & DF_Extern)) // (`extern template class X<int>;` instantiates nothing)
            if (Decl* d = lastPushedDecl(dropInstantiation.mark); d && d->kind == NodeKind::ClassDecl)
              if (auto* c = static_cast<ClassDecl*>(d); !c->isDefinition && !c->name.empty() && c->name.last().hasTemplateArgs)
              {
                NamedType* named = node<NamedType>(classToken);
                named->name = c->name;
                out.explicitInstantiations.push_back({named, currentParent});
              }
          return;
        }
      }
      if (at(Kw::using_))
      {
        parseUsing(header);
        return;
      }
      if (at(Kw::concept_) || at(Kw::enum_) || true)
      {
        // Everything else: function or variable template, specialisation, friend.
        TemplateHeader h = header;
        if (h.params.empty() && h.present)
          declFlags |= DF_Specialization;
        parseDeclarationInnerWithFlags(scope, h, declFlags);
      }
    }

    void parseDeclarationInnerWithFlags(Scope scope, TemplateHeader templ, uint32_t flags)
    {
      // Re-enter the generic path with pre-parsed flags by temporarily stashing them: the
      // generic parser re-parses specifiers (none left), so just call it with templ.
      size_t mark = scratch.size();
      parseDeclarationInner(scope, templ);
      for (size_t i = mark; i < scratch.size(); ++i)
        static_cast<Decl*>(scratch[i])->flags |= flags;
    }

    Decl* parseTemplateParam()
    {
      uint32_t start = pos;
      TemplateParamDecl* p = decl<TemplateParamDecl>(start);
      if (at(Kw::typename_) || at(Kw::class_))
      {
        advance();
        p->isType = true;
        if (accept(P::Ellipsis))
          p->isPack = true;
        if (atIdent())
        {
          p->name = tok().id;
          advance();
        }
        if (accept(P::Assign))
          p->defaultArg = parseType();
        return p;
      }
      if (at(Kw::template_))
      {
        // template<template<...> class T>
        advance();
        if (at(P::Less))
        {
          advance();
          size_t mark = scratch.size();
          while (!at(P::Greater) && !at(TokenKind::End))
          {
            Decl* inner = parseTemplateParam();
            if (!inner)
              break;
            scratch.push_back(inner);
            if (!accept(P::Comma))
              break;
          }
          p->innerParams = makeSpan<Decl*>(mark);
          acceptTemplateClose();
        }
        if (!accept(Kw::class_))
          accept(Kw::typename_);
        if (accept(P::Ellipsis))
          p->isPack = true;
        if (atIdent())
        {
          p->name = tok().id;
          advance();
        }
        if (accept(P::Assign))
          p->defaultArg = parseType();
        return p;
      }
      // Non-type parameter (possibly a concept-constrained type parameter `Concept T`).
      p->isType = false;
      p->valueType = parseType();
      if (!p->valueType)
      {
        error("expected template parameter");
        return nullptr;
      }
      if (accept(P::Ellipsis))
        p->isPack = true;
      if (at(P::LParen))
      {
        // `float (*func)(float, float)`: a declarator, a constant parameter of the type it builds.
        Declarator d;
        d.type = p->valueType;
        parseDeclarator(d, Scope::Local);
        p->valueType = d.type;
        if (!d.name.empty())
          p->name = d.name.last().symbol;
      }
      else if (atIdent())
      {
        p->name = tok().id;
        advance();
        // Constrained type parameter: `Concept<X> T` parsed as valueType=Concept, name=T.
        if (p->valueType->kind == NodeKind::NamedType)
          p->isType = true; // Heuristic: could also be `int N`; builtin types are not NamedType.
      }
      if (accept(P::Assign))
      {
        bool dummy = false;
        p->defaultArg = parseTemplateArgument(dummy);
      }
      return p;
    }

    // --- classes and enums ---

    // Returns true if a declaration was produced; false if this is an elaborated type specifier
    // that the caller should parse as a normal declaration (`struct X* p;`).
    // `union enum Property : u8 { { {int, Integer}, {double, Double}, {String, Text} } members... };`
    // (design.md 8.5): a class holding a tag and one alternative at a time. Desugared here into a
    // class with a nested kpp enum `Tag` of the alternatives' names (unscoped, so `Property::Integer`
    // names the tag value), a `tag` member and an anonymous union of one storage member per
    // alternative; the type checker and code generator add the tag-aware operations (p[Integer],
    // p == Integer, switch (p), construction, copying, destruction).
    bool parseTypedUnion(uint32_t flags, TemplateHeader templ)
    {
      uint32_t start = pos;
      advance(); // union
      advance(); // enum
      skipAttributes();
      ClassDecl* c = decl<ClassDecl>(start);
      c->keyword = Kw::struct_;
      c->flags |= flags;
      c->templateParams = templ.params;
      c->isTemplate = templ.present;
      c->isTypedUnion = true;
      if (!(atIdent() || at(P::ColonColon)) || !parseQualifiedName(c->name, true, false))
      {
        error("expected the name of the union enum");
        return true;
      }
      Type* underlying = nullptr;
      if (accept(P::Colon))
        underlying = parseType();
      if (at(P::Semi))
      {
        advance();
        pushDecl(c);
        return true;
      }
      if (!expect(P::LBrace, "expected '{' after union enum"))
        return true;
      c->isDefinition = true;
      Decl* savedParent = currentParent;
      Access savedAccess = currentAccess;
      uint32_t savedClassName = currentClassName;
      currentParent = c;
      currentAccess = Access::Public;
      currentClassName = c->name.last().symbol;
      size_t mark = scratch.size();
      EnumDecl* tag = decl<EnumDecl>(start);
      tag->isKpp = true;
      tag->isDefinition = true;
      tag->underlying = underlying;
      NamePart tagPart;
      tagPart.symbol = kwTag;
      tag->name.parts = makeSpanOf(std::vector<NamePart>{tagPart});
      ClassDecl* storage = decl<ClassDecl>(start); // the anonymous union of the alternatives
      storage->keyword = Kw::union_;
      storage->isDefinition = true;
      std::vector<Decl*> enumerators, alternatives;
      if (expect(P::LBrace, "expected '{' before the alternatives of the union enum"))
      {
        while (!at(P::RBrace) && !at(TokenKind::End))
        {
          if (!expect(P::LBrace, "expected '{' before an alternative"))
            break;
          Type* type = parseType();
          if (!type)
          {
            error("expected the type of the alternative");
            break;
          }
          type = parseTypeSuffixes(type);
          expect(P::Comma, "expected ',' between the type and the name of the alternative");
          if (!atIdent())
          {
            error("expected the name of the alternative");
            break;
          }
          uint32_t name = tok().id;
          uint32_t nameToken = pos;
          advance();
          expect(P::RBrace, "expected '}' after the alternative");
          currentParent = tag;
          EnumeratorDecl* en = decl<EnumeratorDecl>(nameToken);
          en->name = name;
          currentParent = storage;
          VariableDecl* field = decl<VariableDecl>(nameToken);
          field->type = type;
          NamePart fieldPart;
          fieldPart.symbol = symbols.intern("__alt_" + std::string(symbols.name(name)));
          field->name.parts = makeSpanOf(std::vector<NamePart>{fieldPart});
          currentParent = c;
          enumerators.push_back(en);
          alternatives.push_back(field);
          if (!accept(P::Comma))
            break;
        }
        expect(P::RBrace, "expected '}' after the alternatives of the union enum");
      }
      tag->enumerators = makeSpanOf(enumerators);
      {
        currentParent = tag;
        size_t tagMark = scratch.size();
        addEnumBuiltins(tag, start, tagMark);
        tag->members = makeSpan<Decl*>(tagMark);
        currentParent = c;
      }
      storage->members = makeSpanOf(alternatives);
      c->tagEnum = tag;
      c->alternatives = storage->members;
      pushDecl(tag);
      VariableDecl* tagField = decl<VariableDecl>(start);
      NamedType* tagType = node<NamedType>(start);
      tagType->name.parts = makeSpanOf(std::vector<NamePart>{tagPart});
      tagField->type = tagType;
      NamePart tagFieldPart;
      tagFieldPart.symbol = kwTagField;
      tagField->name.parts = makeSpanOf(std::vector<NamePart>{tagFieldPart});
      c->tagField = tagField;
      pushDecl(tagField);
      pushDecl(storage);
      {
        // `Tag tag() const { return __tag; }`: the alternative held (design.md 8.5).
        FunctionDecl* f = decl<FunctionDecl>(start);
        NamedType* result = node<NamedType>(start);
        result->name.parts = makeSpanOf(std::vector<NamePart>{tagPart});
        f->returnType = result;
        NamePart methodPart;
        methodPart.symbol = kwTagMethod;
        f->name.parts = makeSpanOf(std::vector<NamePart>{methodPart});
        f->flags |= DF_Const;
        NameExpr* field = node<NameExpr>(start);
        field->name.parts = makeSpanOf(std::vector<NamePart>{tagFieldPart});
        ReturnStmt* ret = node<ReturnStmt>(start);
        ret->value = field;
        BlockStmt* body = node<BlockStmt>(start);
        body->stmts = makeSpanOf(std::vector<Stmt*>{ret});
        f->body = body;
        pushDecl(f);
      }
      while (!at(P::RBrace) && !at(TokenKind::End))
      {
        if (accept(P::Semi))
          continue;
        parseDeclaration(Scope::Class);
        if (inRecovery)
          break;
      }
      c->members = makeSpan<Decl*>(mark);
      checkWhenClash(c);
      currentParent = savedParent;
      currentAccess = savedAccess;
      currentClassName = savedClassName;
      expect(P::RBrace, "expected '}' to close the union enum");
      skipAttributes();
      pushDecl(c);
      accept(P::Semi);
      return true;
    }

    bool testsEnabled() const
    {
      auto it = config ? config->find("tests") : ConfigValues::const_iterator();
      return config && it != config->end() && it->second.number != 0;
    }

    // `name(args)`, a call the parser writes itself.
    CallExpr* syntheticCall(uint32_t token, const char* name, std::vector<Expr*> args)
    {
      CallExpr* c = node<CallExpr>(token);
      NameExpr* callee = node<NameExpr>(token);
      NamePart part;
      part.symbol = symbols.intern(name);
      callee->name.parts = makeSpanOf(std::vector<NamePart>{part});
      c->callee = callee;
      c->args = makeSpanOf(args);
      return c;
    }

    // `test "name" { ... }` (design.md 8.6), at namespace scope or in a class. Without --tests
    // (config::tests) it is skipped. With it, the body is a function `void __kpp_test_<file>_<n>()`
    // (static in a class) and a variable registers it with the test runner (compiler/src/tests.cpp):
    // `int __kpp_test_registered_<file>_<n> = __kppRegisterTest(&__kpp_test_<file>_<n>, "name");`,
    // whose caller location is the test's.
    void parseTest(Scope scope, uint32_t start)
    {
      advance();
      uint32_t nameToken = pos;
      noSplice();
      advance();
      if (!testsEnabled())
      {
        skipBalanced();
        return;
      }
      if (scope == Scope::Class && currentParent && currentParent->isTemplate)
        error("a test in a class template (a test belongs to one class)");
      uint64_t h = 1469598103934665603ull;
      for (unsigned char c : out.path)
        h = (h ^ c) * 1099511628211ull;
      char suffix[40];
      snprintf(suffix, sizeof suffix, "%016llx_%u", (unsigned long long)h, testCount++);
      std::string name = std::string("__kpp_test_") + suffix;

      FunctionDecl* f = decl<FunctionDecl>(start);
      BuiltinType* voidType = node<BuiltinType>(start);
      voidType->keyword = Kw::void_;
      f->returnType = voidType;
      NamePart part;
      part.symbol = symbols.intern(name);
      f->name.parts = makeSpanOf(std::vector<NamePart>{part});
      if (scope == Scope::Class)
        f->flags |= DF_Static;
      Decl* savedParent = currentParent;
      currentParent = f;
      bool savedInTest = inTest;
      inTest = true;
      f->body = parseBlock();
      inTest = savedInTest;
      currentParent = savedParent;
      ++out.functionBodies;
      pushDecl(f);

      VariableDecl* v = decl<VariableDecl>(start);
      BuiltinType* intType = node<BuiltinType>(start);
      intType->keyword = Kw::int_;
      v->type = intType;
      NamePart varPart;
      varPart.symbol = symbols.intern(std::string("__kpp_test_registered_") + suffix);
      v->name.parts = makeSpanOf(std::vector<NamePart>{varPart});
      if (scope == Scope::Class)
        v->flags |= DF_Static;
      NameExpr* function = node<NameExpr>(start);
      function->name.parts = f->name.parts;
      UnaryExpr* address = node<UnaryExpr>(start);
      address->operand = function;
      address->op = uint32_t(P::Amp);
      address->isKeyword = false;
      address->postfix = false;
      LiteralExpr* text = node<LiteralExpr>(nameToken);
      text->literalKind = TokenKind::String;
      text->keyword = Kw::Count;
      v->init = syntheticCall(start, "__kppRegisterTest", {address, text});
      pushDecl(v);
    }

    // The operator== a class declares implicitly when it defaults operator<=> and declares no
    // operator== of its own (C++ [class.compare.default]/5): `bool operator==(const X&) const =
    // default`, with the <=>'s parameters, constness and access, so that a qualified call names it
    // (`super::operator==(other)`) and `a == b` finds it as the member it is. The class's members
    // are scratch[mark..].
    void implicitEquality(size_t mark)
    {
      FunctionDecl* spaceship = nullptr;
      for (size_t i = mark; i < scratch.size(); ++i)
      {
        if (scratch[i]->kind != NodeKind::FunctionDecl)
          continue;
        auto* f = static_cast<FunctionDecl*>(scratch[i]);
        if (f->name.parts.size() != 1 || f->name.last().special != 2)
          continue;
        if (P(f->name.last().opToken) == P::EqEq)
          return;
        if (P(f->name.last().opToken) == P::Spaceship && (f->flags & DF_Default) && f->templateParams.empty() && !spaceship)
          spaceship = f;
      }
      if (!spaceship)
        return;
      FunctionDecl* eq = decl<FunctionDecl>(spaceship->token);
      eq->flags |= DF_Default | (spaceship->flags & (DF_Const | DF_Constexpr | DF_Friend));
      eq->access = spaceship->access;
      eq->endToken = spaceship->endToken;
      NamePart part;
      part.special = 2;
      part.opToken = uint32_t(P::EqEq);
      eq->name.parts = makeSpanOf(std::vector<NamePart>{part});
      BuiltinType* boolType = node<BuiltinType>(spaceship->token);
      boolType->keyword = Kw::bool_;
      eq->returnType = boolType;
      std::vector<Decl*> params;
      for (Decl* p : spaceship->params)
      {
        auto* from = static_cast<ParamDecl*>(p);
        ParamDecl* copy = decl<ParamDecl>(from->token);
        copy->type = from->type;
        copy->name = from->name;
        copy->parent = eq;
        params.push_back(copy);
      }
      eq->params = makeSpanOf(params);
      pushDecl(eq);
    }

    bool parseClassOrElaborated(Scope scope, uint32_t flags, TemplateHeader templ)
    {
      if (at(Kw::union_) && at(Kw::enum_, 1))
        return parseTypedUnion(flags, templ);
      uint32_t start = pos;
      uint32_t save = pos;
      // A const read before the keyword: the declarators' after a definition; the type's, when
      // this is no definition and the caller parses the type again.
      bool constDeclarators = std::exchange(pendingConst, false);
      auto notADefinition = [&]()
      {
        restore(save);
        pendingConst = constDeclarators;
        return false;
      };
      Kw keyword = Kw(tok().id);
      advance();
      skipAttributes();
      skipMacroSpecifier();
      ClassDecl* c = decl<ClassDecl>(start);
      c->keyword = keyword;
      c->flags |= flags;
      c->alignment = pendingAlignment;
      pendingAlignment = nullptr;
      c->attributes |= pendingAttributes & ATTR_Packed; // `struct [[packed]] Tile`, `[[packed]] struct Tile`
      c->templateParams = templ.params;
      c->isTemplate = templ.present;
      if (templ.present && templ.params.empty())
        c->flags |= DF_Specialization;
      if (atIdent() || at(P::ColonColon))
      {
        if (!parseQualifiedName(c->name, true, false))
          return notADefinition();
      }
      skipAttributes();
      if (templ.present && !c->name.empty() && c->name.last().hasTemplateArgs)
        c->flags |= DF_Specialization; // `template<typename T> struct Describe<T*>`: a partial specialisation
      if (!c->templateParams.empty() && !(c->flags & DF_Specialization) && c->name.parts.size() == 1)
      {
        // A class template declared in a class template: its instances are made with the enclosing
        // template's arguments, then its own (ClassDecl::instanceParams).
        Span<Decl*> enclosing;
        for (Decl* p = c->parent; p && (p->kind == NodeKind::ClassDecl || p->kind == NodeKind::EnumDecl) && enclosing.empty(); p = p->parent)
          enclosing = p->kind == NodeKind::ClassDecl && !static_cast<ClassDecl*>(p)->instanceParams.empty() ? static_cast<ClassDecl*>(p)->instanceParams
                                                                                                             : p->templateParams;
        if (!enclosing.empty())
        {
          std::vector<Decl*> all(enclosing.begin(), enclosing.end());
          all.insert(all.end(), c->templateParams.begin(), c->templateParams.end());
          c->instanceParams = makeSpanOf(all);
        }
      }
      if (atIdent() && spelling() == "final")
      {
        advance();
        c->isFinal = true;
      }
      if (atIdent(kwClassConst))
      {
        advance();
        c->isClassConst = true;
      }
      if (at(P::Semi))
      {
        advance();
        pushDecl(c);
        return true;
      }
      if (!at(P::LBrace) && !at(P::Colon))
        return notADefinition(); // an elaborated type specifier used in a declaration
      if (accept(P::Colon))
      {
        std::vector<ClassDecl::Base> bases;
        for (;;)
        {
          ClassDecl::Base base{};
          base.access = Access::Default;
          for (;;)
          {
            if (keyword == Kw::struct_ && !cMode && (at(Kw::private_) || at(Kw::protected_)))
              out.structRule.push_back({pos, "a struct's bases are all public (a struct is a public value type, design.md \"struct and class\"): make it a class"});
            if (accept(Kw::virtual_)) base.isVirtual = true;
            else if (accept(Kw::public_)) base.access = Access::Public;
            else if (accept(Kw::private_)) base.access = Access::Private;
            else if (accept(Kw::protected_)) base.access = Access::Protected;
            else break;
          }
          base.type = parseType();
          if (!base.type)
          {
            error("expected base class");
            return true;
          }
          base.isPack = accept(P::Ellipsis);
          bases.push_back(base);
          if (!accept(P::Comma))
            break;
        }
        c->bases = makeSpanOf(bases);
      }
      if (!expect(P::LBrace, "expected '{' in class"))
        return true;
      c->isDefinition = true;
      Decl* savedParent = currentParent;
      Access savedAccess = currentAccess;
      uint32_t savedClassName = currentClassName;
      currentParent = c;
      currentAccess = keyword == Kw::class_ ? Access::Private : Access::Public;
      currentClassName = c->name.empty() ? UINT32_MAX : c->name.last().symbol;
      size_t mark = scratch.size();
      size_t savedMembersMark = classMembersMark;
      classMembersMark = mark;
      size_t errorsBefore = allErrors.size();
      while (!at(P::RBrace) && !at(TokenKind::End))
        parseDeclaration(Scope::Class);
      classMembersMark = savedMembersMark;
      // An imported C struct or union whose body had an error lost what the parser skipped (a member
      // in a construct it does not read): its layout would silently be another than C's, so a use
      // that needs it is an error (ClassDecl::importError). A pointer to it is still a pointer.
      if (cMode && allErrors.size() > errorsBefore)
      {
        const Diagnostic& first = allErrors[errorsBefore];
        c->importError = first.message;
        if (first.token < toks.size())
          c->importErrorAt = symbols.intern(std::string(spelling(toks[first.token])));
      }
      implicitEquality(mark);
      c->members = makeSpan<Decl*>(mark);
      checkWhenClash(c);
      // A friend function defined in its class (a hidden friend) takes no object: a static member,
      // which only argument-dependent lookup finds (the resolver's hiddenFriends). A defaulted
      // friend comparison (`friend bool operator==(const C&, const C&) = default;`) compares as
      // the defaulted member does: it is that member (`bool operator==(const C&) const = default;`).
      for (Decl* m : c->members)
      {
        if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Friend))
          continue;
        auto* f = static_cast<FunctionDecl*>(m);
        if (f->name.parts.size() != 1)
          continue;
        if (f->body)
          f->flags |= DF_Static;
        else if ((f->flags & DF_Default) && f->name.last().special == 2 && f->params.size() == 2)
        {
          f->flags = (f->flags & ~DF_Friend) | DF_Const;
          f->params = Span<Decl*>{f->params.data + 1, 1};
        }
      }
      if (keyword == Kw::struct_ && !cMode)
        for (Decl* m : c->members)
          if (m->kind == NodeKind::FunctionDecl && (m->flags & (DF_Virtual | DF_Override | DF_Final | DF_Pure)) && !(m->flags & DF_Friend))
            out.structRule.push_back({m->token, "a struct has no virtual functions (a struct is a public value type, design.md \"struct and class\"): make it a class"});
      currentParent = savedParent;
      currentAccess = savedAccess;
      currentClassName = savedClassName;
      expect(P::RBrace, "expected '}' to close class");
      skipAttributes();
      pushDecl(c);
      // Declarators after the body: `struct { ... } x, *y;`
      if (!at(P::Semi))
      {
        parseDeclSpecifiers(flags);
        bool synthetic = false;
        if (c->name.empty())
        {
          // `struct { ... } x;`: the class needs a name for x's type to refer to.
          NamePart part;
          part.symbol = symbols.intern("__anon" + std::to_string(start) + "_" + std::to_string(fileTag()));
          c->name.parts = makeSpanOf(std::vector<NamePart>{part});
          synthetic = true;
        }
        NamedType* selfType = node<NamedType>(start);
        selfType->name = c->name;
        Type* declaredType = selfType;
        if (constDeclarators)
        {
          ConstType* constType = node<ConstType>(start);
          constType->inner = selfType;
          declaredType = constType;
        }
        for (;;)
        {
          Declarator d;
          d.type = declaredType;
          parseDeclarator(d, scope);
          if (inRecovery)
            return true;
          if ((flags & DF_Typedef) && synthetic && d.type == selfType && d.name.parts.size() == 1 && !namesCBool(d.name))
          {
            // `typedef struct { ... } Name;`: the class itself takes the name, the first one; any
            // later plain name is a typedef of it (windns.h's `} DNS_KEY_DATA, *PDNS_KEY_DATA,
            // DNS_DNSKEY_DATA, *PDNS_DNSKEY_DATA;`, which renamed the class to its last name).
            c->name = d.name;
            selfType->name = d.name;
            synthetic = false;
            if (cMode)
              cTypedefNames.insert(d.name.parts[0].symbol); // a type name, as a typedef's
            if (!accept(P::Comma))
              break;
            continue;
          }
          VariableDecl* v = decl<VariableDecl>(start);
          v->type = d.type;
          v->name = d.name;
          v->flags |= flags | d.flags;
          parseInitializer(v);
          pushDecl(v);
          if (!accept(P::Comma))
            break;
        }
      }
      expect(P::Semi, "expected ';' after class");
      return true;
    }

    bool parseEnumOrElaborated(Scope scope, uint32_t flags, TemplateHeader templ)
    {
      uint32_t start = pos;
      uint32_t save = pos;
      // `const enum class Type { Entity, Tile } type;`: the const is the declarators' after a
      // definition; the type's, when this is no definition and the caller parses the type again.
      bool constDeclarators = std::exchange(pendingConst, false);
      auto restore = [&](uint32_t to)
      {
        this->restore(to);
        pendingConst = constDeclarators;
      };
      advance();
      EnumDecl* e = decl<EnumDecl>(start);
      e->flags |= flags;
      e->templateParams = templ.params;
      if (accept(Kw::class_) || accept(Kw::struct_))
        e->isClass = true;
      skipAttributes();
      skipMacroSpecifier();
      if (atIdent() || at(P::ColonColon))
      {
        if (!parseQualifiedName(e->name, true, false))
        {
          restore(save);
          return false;
        }
      }
      if (accept(P::Colon))
      {
        e->underlying = parseType();
        if (!e->underlying)
        {
          error("expected underlying type");
          return true;
        }
      }
      if (at(P::Semi))
      {
        advance();
        pushDecl(e);
        return true;
      }
      if (!at(P::LBrace))
      {
        restore(save);
        return false;
      }
      advance();
      e->isDefinition = true;
      Decl* savedParent = currentParent;
      currentParent = e;
      std::vector<Decl*> enumerators;
      size_t mark = scratch.size();
      while (!at(P::RBrace) && !at(TokenKind::End))
      {
        skipAttributes();
        if (at(P::LBrace))
        {
          // kpp enum: the values in their own block, then members.
          e->isKpp = true;
          advance();
          while (!at(P::RBrace) && !at(TokenKind::End))
          {
            skipAttributes();
            if (!atIdent())
            {
              error("expected an enumerator");
              break;
            }
            EnumeratorDecl* en = decl<EnumeratorDecl>(pos);
            en->name = tok().id;
            advance();
            if (accept(P::Assign))
              en->value = parseAssignmentNoBrace();
            enumerators.push_back(en);
            if (!accept(P::Comma))
              break;
          }
          expect(P::RBrace, "expected '}' after the enumerators");
          continue;
        }
        if (atIdent() && (at(P::Comma, 1) || at(P::RBrace, 1) || at(P::Assign, 1) || at(P::LBracket, 1)))
        {
          EnumeratorDecl* en = decl<EnumeratorDecl>(pos);
          en->name = tok().id;
          advance();
          skipAttributes();
          if (accept(P::Assign))
            en->value = parseAssignmentNoBrace();
          enumerators.push_back(en);
          if (!accept(P::Comma))
            break;
          continue;
        }
        if (atMacroName() && at(P::LParen, 1))
        {
          // Enumerators generated by a macro.
          MacroDecl* m = decl<MacroDecl>(pos);
          m->name = tok().id;
          advance();
          skipBalanced();
          m->endToken = pos;
          enumerators.push_back(m);
          accept(P::Comma);
          continue;
        }
        if (accept(P::Semi))
          continue;
        // kpp: member declarations inside an enum.
        parseDeclaration(Scope::Enum);
        if (inRecovery)
          break;
      }
      if (e->isKpp)
        addEnumBuiltins(e, start, mark);
      e->members = makeSpan<Decl*>(mark);
      e->enumerators = makeSpanOf(enumerators);
      currentParent = savedParent;
      expect(P::RBrace, "expected '}' to close enum");
      pushDecl(e);
      if (!at(P::Semi))
      {
        bool synthetic = false;
        if (e->name.empty())
        {
          // `enum { ... } x;`: the enum needs a name for x's type to refer to (as for classes).
          NamePart part;
          part.symbol = symbols.intern("__anon" + std::to_string(start) + "_" + std::to_string(fileTag()));
          e->name.parts = makeSpanOf(std::vector<NamePart>{part});
          synthetic = true;
        }
        NamedType* selfType = node<NamedType>(start);
        selfType->name = e->name;
        Type* declaratorType = selfType;
        if (constDeclarators)
        {
          ConstType* c = node<ConstType>(start);
          c->inner = selfType;
          declaratorType = c;
        }
        for (;;)
        {
          Declarator d;
          d.type = declaratorType;
          parseDeclarator(d, scope);
          if (inRecovery)
            return true;
          if ((flags & DF_Typedef) && synthetic && d.type == selfType && d.name.parts.size() == 1 && !namesCBool(d.name))
          {
            // `typedef enum { ... } Name;` (C headers: curl's CURLcode): the enum itself takes the name.
            e->name = d.name;
            selfType->name = d.name;
            if (!accept(P::Comma))
              break;
            continue;
          }
          VariableDecl* v = decl<VariableDecl>(start);
          v->type = d.type;
          v->name = d.name;
          v->flags |= flags | d.flags;
          parseInitializer(v);
          pushDecl(v);
          if (!accept(P::Comma))
            break;
        }
      }
      expect(P::Semi, "expected ';' after enum");
      return true;
    }

    // --- statements ---

    Stmt* parseBlock()
    {
      uint32_t start = pos;
      BlockStmt* b = node<BlockStmt>(start);
      if (!expect(P::LBrace, "expected '{'"))
        return b;
      size_t mark = scratch.size();
      while (!at(P::RBrace) && !at(TokenKind::End))
      {
        Stmt* s = parseStatement();
        if (s)
          scratch.push_back(s);
      }
      b->stmts = makeSpan<Stmt*>(mark);
      expect(P::RBrace, "expected '}'");
      if (inRecovery)
        inRecovery = false; // The block end is a reliable recovery point.
      return b;
    }

    Stmt* parseStatement()
    {
      DepthGuard guard(*this);
      if (depth > 400)
      {
        error("nesting too deep");
        recover();
        return nullptr;
      }
      uint32_t start = pos;
      size_t mark = scratch.size();
      Stmt* s = parseStatementInner();
      if (inRecovery)
      {
        scratch.resize(mark);
        if (pos == start)
          advance();
        recover();
        ErrorStmt* e = node<ErrorStmt>(start);
        e->endToken = pos;
        return e;
      }
      return s;
    }

    Stmt* parseStatementInner()
    {
      pendingAttributes = 0;
      pendingAlignment = nullptr;
      skipAttributes();
      uint32_t start = pos;
      if (at(TokenKind::End))
        return nullptr;
      if (at(P::LBrace))
        return parseBlock();
      if (accept(P::Semi))
        return node<EmptyStmt>(start);
      if (at(TokenKind::Keyword))
      {
        switch (Kw(tok().id))
        {
          case Kw::if_:
          {
            advance();
            IfStmt* s = node<IfStmt>(start);
            if (accept(Kw::constexpr_))
              s->isConstexpr = true;
            if (at(Kw::consteval_) || (at(P::Bang) && at(Kw::consteval_, 1)))
            {
              // `if consteval { ... } else { ... }`, `if !consteval { ... }` (C++23): no condition.
              bool negated = accept(P::Bang);
              advance();
              s->ifConsteval = negated ? 2 : 1;
              s->cond = nullptr;
              if (!at(P::LBrace))
                error("expected '{' after if consteval");
              s->then = parseStatement();
              if (accept(Kw::else_))
                s->else_ = parseStatement();
              return s;
            }
            if (!expect(P::LParen, "expected '(' after if"))
              return s;
            parseCondition(s->init, s->cond);
            expect(P::RParen, "expected ')'");
            s->then = parseStatement();
            if (accept(Kw::else_))
              s->else_ = parseStatement();
            return s;
          }
          case Kw::while_:
          {
            advance();
            WhileStmt* s = node<WhileStmt>(start);
            expect(P::LParen, "expected '('");
            Stmt* dummy = nullptr;
            parseCondition(dummy, s->cond);
            expect(P::RParen, "expected ')'");
            s->body = parseStatement();
            return s;
          }
          case Kw::do_:
          {
            advance();
            DoStmt* s = node<DoStmt>(start);
            s->body = parseStatement();
            if (!accept(Kw::while_))
              error("expected 'while' after do body");
            expect(P::LParen, "expected '('");
            s->cond = parseExpression();
            expect(P::RParen, "expected ')'");
            expect(P::Semi, "expected ';'");
            return s;
          }
          case Kw::for_:
            return parseFor();
          case Kw::switch_:
          {
            advance();
            SwitchStmt* s = node<SwitchStmt>(start);
            expect(P::LParen, "expected '('");
            parseCondition(s->init, s->cond);
            expect(P::RParen, "expected ')'");
            s->body = parseStatement();
            return s;
          }
          case Kw::case_:
          {
            advance();
            CaseStmt* s = node<CaseStmt>(start);
            s->value = parseTernaryNoColon();
            if (accept(P::Ellipsis))
              s->rangeEnd = parseTernaryNoColon();
            expect(P::Colon, "expected ':' after case");
            return s;
          }
          case Kw::default_:
            advance();
            expect(P::Colon, "expected ':' after default");
            return node<DefaultStmt>(start);
          case Kw::return_:
          case Kw::co_return_:
          {
            advance();
            ReturnStmt* s = node<ReturnStmt>(start);
            if (!at(P::Semi))
              s->value = at(P::LBrace) ? parseBraceInit() : parseExpression();
            expect(P::Semi, "expected ';' after return");
            return s;
          }
          case Kw::break_:
            advance();
            expect(P::Semi, "expected ';'");
            return node<BreakStmt>(start);
          case Kw::continue_:
            advance();
            expect(P::Semi, "expected ';'");
            return node<ContinueStmt>(start);
          case Kw::goto_:
          {
            advance();
            GotoStmt* s = node<GotoStmt>(start);
            s->label = atIdent() ? tok().id : UINT32_MAX;
            advance();
            expect(P::Semi, "expected ';'");
            return s;
          }
          case Kw::try_:
          {
            advance();
            TryStmt* s = node<TryStmt>(start);
            s->body = parseBlock();
            std::vector<TryStmt::Handler> handlers;
            while (at(Kw::catch_))
            {
              advance();
              TryStmt::Handler h{};
              expect(P::LParen, "expected '(' after catch");
              if (!accept(P::Ellipsis))
                h.param = parseParameter();
              expect(P::RParen, "expected ')'");
              h.body = parseBlock();
              handlers.push_back(h);
            }
            s->handlers = makeSpanOf(handlers);
            return s;
          }
          case Kw::asm_:
            advance();
            if (at(P::LParen))
              skipBalanced();
            accept(P::Semi);
            return node<AsmStmt>(start);
          case Kw::namespace_:
          case Kw::using_:
          case Kw::static_assert_:
          case Kw::template_:
          case Kw::typedef_:
          case Kw::class_:
          case Kw::struct_:
          case Kw::union_:
          case Kw::enum_:
          case Kw::static_:
          case Kw::constexpr_:
          case Kw::const_:
          case Kw::volatile_:
          case Kw::extern_:
          case Kw::thread_local_:
          case Kw::register_:
          case Kw::auto_:
          case Kw::typename_:
          case Kw::inline_:
          case Kw::constinit_:
          case Kw::consteval_:
          case Kw::when_:
            return parseDeclStmt();
          default:
            if (isBuiltinTypeKeyword(Kw(tok().id)) && !at(P::LParen, 1) && !at(P::LBrace, 1) && !at(P::ColonColon, 1))
              return parseDeclStmt();
            break;
        }
      }
      if (atIdent() && (spelling() == "__asm" || spelling() == "__asm__"))
      {
        advance();
        if (at(P::LParen) || at(P::LBrace))
          skipBalanced();
        else
          while (!at(P::Semi) && !at(P::RBrace) && !at(TokenKind::End))
            advance();
        accept(P::Semi);
        return node<AsmStmt>(start);
      }
      if (atIdent(kwFallthrough) && at(P::Semi, 1))
      {
        advance();
        advance();
        return node<FallthroughStmt>(start);
      }
      if (inTest && (atIdent(kwCheck) || atIdent(kwRequire)) && at(P::LParen, 1))
      {
        // check(cond); and require(cond); in a test (design.md 8.6): a comparison is evaluated by
        // the runner's __kppEqual (NotEqual, Less...), which keeps the operands for the message.
        AssertStmt* s = node<AssertStmt>(start);
        s->testKind = atIdent(kwCheck) ? 1 : 2;
        advance();
        advance();
        s->startToken = pos;
        s->cond = parseExpression();
        s->endToken = pos;
        expect(P::RParen, "expected ')' after the checked expression");
        expect(P::Semi, "expected ';' after check or require");
        if (s->cond->kind == NodeKind::BinaryExpr && !static_cast<BinaryExpr*>(s->cond)->isKeyword)
        {
          auto* b = static_cast<BinaryExpr*>(s->cond);
          const char* compare = P(b->op) == P::EqEq ? "__kppEqual" : P(b->op) == P::NotEq ? "__kppNotEqual" : P(b->op) == P::Less ? "__kppLess"
                              : P(b->op) == P::LessEq ? "__kppLessEqual" : P(b->op) == P::Greater ? "__kppGreater"
                              : P(b->op) == P::GreaterEq ? "__kppGreaterEqual" : nullptr;
          if (compare)
            s->cond = syntheticCall(b->token, compare, {b->lhs, b->rhs});
        }
        return s;
      }
      if ((atIdent(kwAssert) || atIdent(kwReleaseAssert)) && at(P::LParen, 1))
      {
        AssertStmt* s = node<AssertStmt>(start);
        s->release = atIdent(kwReleaseAssert);
        advance();
        advance();
        s->startToken = pos;
        s->cond = parseExpression();
        s->endToken = pos;
        expect(P::RParen, "expected ')' after the asserted expression");
        expect(P::Semi, "expected ';' after assert");
        return s;
      }
      // Label.
      if (atIdent() && at(P::Colon, 1) && !at(P::ColonColon, 1))
      {
        LabelStmt* s = node<LabelStmt>(start);
        s->label = tok().id;
        advance();
        advance();
        return s;
      }
      if (atMacroName() && at(P::LParen, 1))
      {
        uint32_t save = pos;
        size_t mark = scratch.size();
        size_t savedDiagnostics = out.diagnostics.size();
        uint32_t nodes = out.nodeCount;
        ExprStmt* s = node<ExprStmt>(start);
        s->expr = parseExpression();
        if (!inRecovery && accept(P::Semi))
          return s;
        if (!inRecovery && (at(P::RBrace) || atIdent() || at(TokenKind::Keyword)))
          return s; // Macro statement without ';', e.g. `WHEN_X(...)` on its own line.
        // Arguments that are not expressions (statements, types...): swallow the macro.
        inRecovery = false;
        out.diagnostics.resize(savedDiagnostics);
        scratch.resize(mark);
        out.nodeCount = nodes;
        restore(save);
        MacroStmt* m = node<MacroStmt>(start);
        m->name = tok().id;
        advance();
        skipBalanced();
        accept(P::Semi);
        return m;
      }
      if (looksLikeDeclaration())
        return parseDeclStmt();
      ExprStmt* s = node<ExprStmt>(start);
      s->expr = parseExpression();
      if (!at(P::Semi) && !inRecovery && s->expr->kind == NodeKind::CallExpr &&
          static_cast<CallExpr*>(s->expr)->callee->kind == NodeKind::NameExpr &&
          (at(P::LBrace) || at(P::RBrace) || atIdent() || at(TokenKind::Keyword)))
        return s; // Function-like macro expanding to a statement, used without ';'.
      expect(P::Semi, "expected ';' after expression");
      return s;
    }

    // Syntactic guess whether a statement starting with a name is a declaration.
    // At `ahead` tokens on, a declarator in parentheses: `(*name)(` or `(&name)(` (a pointer or
    // reference to a function), `(*name[N])(` or `(*name[N])[` (an array of pointers to functions
    // or to arrays). A call's first argument can start the same way (`f(*p);`, `f(&x);`,
    // `memcpy(&image[i * 4], &color, 3);`, `exchange(*items[0], *items[1]);`); no array of
    // references exists, so `(&name[` is always one.
    bool parenthesisedDeclaratorAhead(int ahead) const
    {
      if (!at(P::LParen, ahead) || !(at(P::Star, ahead + 1) || at(P::Amp, ahead + 1)) || !atIdent(ahead + 2))
        return false;
      if (at(P::RParen, ahead + 3))
        return at(P::LParen, ahead + 4);
      if (!at(P::Star, ahead + 1) || !at(P::LBracket, ahead + 3))
        return false;
      int i = ahead + 3;
      for (int depth = 0;; ++i)
      {
        if (at(TokenKind::End, i))
          return false;
        if (at(P::LBracket, i))
          ++depth;
        else if (at(P::RBracket, i) && --depth == 0)
          break;
      }
      return at(P::RParen, i + 1) && (at(P::LParen, i + 2) || at(P::LBracket, i + 2));
    }

    bool looksLikeDeclaration()
    {
      if (at(TokenKind::Keyword))
      {
        switch (Kw(tok().id))
        {
          case Kw::const_: case Kw::volatile_: case Kw::static_: case Kw::constexpr_: case Kw::auto_:
          case Kw::struct_: case Kw::class_: case Kw::union_: case Kw::enum_: case Kw::typename_:
          case Kw::extern_: case Kw::thread_local_: case Kw::register_: case Kw::typedef_: case Kw::using_:
          case Kw::inline_: case Kw::constinit_: case Kw::consteval_: case Kw::static_assert_:
            return true;
          case Kw::decltype_:
            break; // speculative path below
          default:
            if (isBuiltinTypeKeyword(Kw(tok().id)) && at(P::LParen, 1) && atIdent(2) && at(P::ColonColon, 3) && at(P::Star, 4))
              return true; // `int (Calc::*op)(int)`: a pointer to a member
            if (isBuiltinTypeKeyword(Kw(tok().id)) && parenthesisedDeclaratorAhead(1))
              return true; // `void (*callback)();`, `int (*op)(int) = twice;`: a function pointer declarator
            return isBuiltinTypeKeyword(Kw(tok().id)) && !at(P::LParen, 1) && !at(P::LBrace, 1) && !at(P::ColonColon, 1);
        }
      }
      if (!atIdent() && !at(P::ColonColon) && !at(Kw::decltype_))
        return false;
      uint32_t save = pos;
      size_t mark = scratch.size();
      bool savedRecovery = inRecovery;
      size_t savedDiagnostics = out.diagnostics.size();
      uint32_t nodes = out.nodeCount;
      bool result = false;
      // Macro statement `MACRO(...);` is an expression (a call) unless followed by a declaration.
      if (skipMacroSpecifier())
        result = atIdent() || at(TokenKind::Keyword);
      if (!result)
      {
        Type* t = parseType();
        if (t)
        {
          if (atIdent())
            result = at(P::Semi, 1) || at(P::Assign, 1) || at(P::LParen, 1) || at(P::LBrace, 1) || at(P::Comma, 1) ||
                     at(P::LBracket, 1);
          if (atIdent() && !result)
          {
            // `T x` followed by anything else (e.g. a macro suffix) is still most likely a declaration;
            // not after an owner's `^` (`mask ^ out << flags;` is exclusive or: design.md 20).
            result = !toks[pos - 1].is(P::Caret) && !at(P::ColonColon, 1) && !at(P::Less, 1) && !at(P::Dot, 1) && !at(P::Arrow, 1) &&
                     !at(P::LParen, 1);
            if (at(P::LParen, 1))
              result = true;
          }
          else if (at(P::Star) || at(P::Amp) || at(P::AndAnd))
          {
            // `T* x` / `T& x`: after the suffixes an identifier must follow.
            while (at(P::Star) || at(P::Amp) || at(P::AndAnd) || at(Kw::const_))
              advance();
            result = atIdent() && (at(P::Semi, 1) || at(P::Assign, 1) || at(P::LParen, 1) || at(P::LBrace, 1) ||
                                   at(P::Comma, 1) || at(P::LBracket, 1) || at(P::RParen, 1));
          }
          else if (parenthesisedDeclaratorAhead(0))
            result = true; // function pointer declarator `R (*f)(...)`; `f(*p);` and `f(&x);` are calls
          else if (at(P::LParen) && atIdent(1) && at(P::ColonColon, 2) && at(P::Star, 3))
            result = true; // pointer to member declarator `R (C::*name)(...)`
          else if (at(P::LBracket) && autoUnderRefs(t))
            result = true;
        }
      }
      restore(save);
      scratch.resize(mark);
      inRecovery = savedRecovery;
      out.diagnostics.resize(savedDiagnostics);
      out.nodeCount = nodes;
      return result;
    }

    Stmt* parseDeclStmt()
    {
      uint32_t start = pos;
      DeclStmt* s = node<DeclStmt>(start);
      size_t mark = scratch.size();
      parseDeclarationInner(Scope::Local, {});
      s->decls = makeSpan<Decl*>(mark);
      return s;
    }

    // Condition of if/while/switch: either an expression or a declaration `T x = e`, optionally
    // preceded by an init-statement `stmt;`.
    void parseCondition(Stmt*& init, Node*& cond, bool allowInit = true)
    {
      uint32_t save = pos;
      if (looksLikeDeclaration())
      {
        uint32_t start = pos;
        size_t mark = scratch.size();
        bool savedMode = conditionMode;
        conditionMode = true;
        parseDeclarationInner(Scope::Local, {});
        conditionMode = savedMode;
        bool declared = !inRecovery;
        if (inRecovery)
        {
          inRecovery = false;
          out.diagnostics.pop_back();
        }
        if (declared)
        {
          DeclStmt* d = node<DeclStmt>(start);
          d->decls = makeSpan<Decl*>(mark);
          // parseDeclarationInner consumed a ';' only if it was an init-statement.
          bool initStatement = toks[pos - 1].is(P::Semi) && allowInit;
          // A declaration in a condition is a variable with `= e` or `{e}`: `flags & MASK` and
          // `a && b` with variables on both sides are expressions that read like reference
          // declarations, and so are `p && f(p)` and `a & mask(a)` (a function declarator or a
          // parenthesized initializer, which a condition cannot have).
          bool uninitialised = false;
          if (!initStatement)
            for (Decl* x : d->decls)
            {
              if (x->kind == NodeKind::FunctionDecl)
                uninitialised = true;
              if (x->kind != NodeKind::VariableDecl || (x->flags & DF_Typedef))
                continue;
              auto* v = static_cast<VariableDecl*>(x);
              if (!v->init)
                uninitialised = true;
              else if (v->init->kind == NodeKind::FunctionalCastExpr && !static_cast<FunctionalCastExpr*>(v->init)->braced &&
                       static_cast<FunctionalCastExpr*>(v->init)->type == v->type)
                uninitialised = true; // `T x(args)`: direct initialization
            }
          if (!uninitialised)
          {
            if (initStatement)
            {
              init = d;
              parseCondition(init, cond, false);
              if (!cond)
                cond = d;
              return;
            }
            cond = d;
            return;
          }
        }
        // Not actually a declaration: fall back to an expression.
        scratch.resize(mark);
        restore(save);
      }
      Expr* e = parseExpression();
      if (allowInit && accept(P::Semi))
      {
        ExprStmt* s = node<ExprStmt>(e ? e->token : pos);
        s->expr = e;
        init = s;
        Stmt* dummy = nullptr;
        parseCondition(dummy, cond, false);
        return;
      }
      cond = e;
    }

    // `for constexpr (typename T : types<A, B>) body` / `for constexpr (int i : {1, 2, 4}) body`
    // (design.md 8.2): the loop variable is a template parameter of the body, which is instantiated
    // once per element of the list.
    Stmt* parseForConstexpr(uint32_t start)
    {
      ForConstexprStmt* s = node<ForConstexprStmt>(start);
      if (!expect(P::LParen, "expected '(' after for constexpr"))
        return s;
      TemplateParamDecl* v = decl<TemplateParamDecl>(pos);
      if (accept(Kw::typename_) || accept(Kw::class_))
        v->isType = true;
      else
      {
        v->isType = false;
        v->valueType = parseType();
        if (!v->valueType)
          error("expected the type of the for constexpr variable");
      }
      if (atIdent())
      {
        v->name = tok().id;
        advance();
      }
      else
        error("expected the name of the for constexpr variable");
      s->var = v;
      expect(P::Colon, "expected ':' in for constexpr");
      if (at(P::LBrace))
        s->list = parseBraceInit();
      else if (atIdent(kwTypes) && at(P::Less, 1))
        s->list = parseType(); // types<A, B, C>
      else
        s->list = parseExpression();
      expect(P::RParen, "expected ')' after for constexpr");
      if (!v->isType && v->name != UINT32_MAX)
        descriptorNames.push_back(v->name);
      s->body = parseStatement();
      if (s->body && (s->body->kind == NodeKind::CaseStmt || s->body->kind == NodeKind::DefaultStmt))
      {
        // `for constexpr (typename T : kinds) case T::kind: return parse<T>();` inside a switch: the
        // label and the statement it labels are the body (the parser keeps labels as statements of
        // their own).
        std::vector<Stmt*> stmts{s->body};
        if (Stmt* labelled = parseStatement())
          stmts.push_back(labelled);
        BlockStmt* block = node<BlockStmt>(s->body->token);
        block->stmts = makeSpanOf(stmts);
        s->body = block;
      }
      if (!v->isType && v->name != UINT32_MAX)
        descriptorNames.pop_back();
      return s;
    }

    Stmt* parseFor()
    {
      uint32_t start = pos;
      advance();
      if (accept(Kw::constexpr_))
        return parseForConstexpr(start);
      if (!expect(P::LParen, "expected '(' after for"))
        return nullptr;
      // Range-for: `for (decl : range)`. Detect by scanning for ':' before ';' at depth 0.
      {
        int level = 0;
        int ternary = 0;
        bool isRange = false;
        for (uint32_t i = pos; i < toks.size(); ++i)
        {
          const Token& t = toks[i];
          if (t.is(P::LParen) || t.is(P::LBracket) || t.is(P::LBrace)) ++level;
          else if (t.is(P::RParen) || t.is(P::RBracket) || t.is(P::RBrace))
          {
            if (level == 0) break;
            --level;
          }
          else if (level == 0 && t.is(P::Semi)) break;
          else if (level == 0 && t.is(P::Question)) ++ternary;
          else if (level == 0 && t.is(P::Colon))
          {
            if (ternary > 0) --ternary;
            else { isRange = true; break; }
          }
          else if (t.getKind() == TokenKind::End) break;
        }
        if (isRange)
        {
          RangeForStmt* s = node<RangeForStmt>(start);
          uint32_t declStart = pos;
          uint32_t flags = 0;
          parseDeclSpecifiers(flags);
          VariableDecl* v = decl<VariableDecl>(declStart);
          v->type = parseType();
          if (!v->type)
          {
            error("expected range variable type");
            return s;
          }
          Declarator d;
          d.type = v->type;
          parseDeclarator(d, Scope::Local);
          v->type = d.type;
          v->name = d.name;
          v->structuredBindings = d.structuredBindings;
          v->flags |= flags;
          s->var = v;
          expect(P::Colon, "expected ':' in range for");
          s->range = at(P::LBrace) ? parseBraceInit() : parseExpression();
          expect(P::RParen, "expected ')'");
          s->body = parseStatement();
          return s;
        }
      }
      ForStmt* s = node<ForStmt>(start);
      if (accept(P::Semi))
        s->init = nullptr;
      else if (looksLikeDeclaration())
        s->init = parseDeclStmt();
      else
      {
        ExprStmt* e = node<ExprStmt>(pos);
        e->expr = parseExpression();
        expect(P::Semi, "expected ';' in for");
        s->init = e;
      }
      if (!at(P::Semi))
      {
        Stmt* dummy = nullptr;
        parseCondition(dummy, s->cond, false);
      }
      expect(P::Semi, "expected ';' in for");
      if (!at(P::RParen))
        s->step = parseExpression();
      expect(P::RParen, "expected ')' after for");
      s->body = parseStatement();
      return s;
    }

    // --- expressions ---

    // Precedence climbing. Levels (low to high):
    // comma < assignment/ternary < || < && < | < ^ < & < ==,!= < <,>,<=,>= < <=> < <<,>> < +,- < *,/,% < .*,->* < unary < postfix
    Expr* parseExpression()
    {
      DepthGuard guard(*this);
      if (depth > 400)
      {
        error("nesting too deep");
        return node<ErrorExpr>(pos);
      }
      Expr* e = parseAssignment();
      while (at(P::Comma))
      {
        uint32_t opTok = pos;
        advance();
        if (at(P::Ellipsis))
        {
          advance();
          FoldExpr* f = node<FoldExpr>(opTok);
          f->lhs = e;
          f->rhs = nullptr;
          f->op = uint32_t(P::Comma);
          e = f;
          continue;
        }
        BinaryExpr* b = node<BinaryExpr>(opTok);
        b->lhs = e;
        b->op = uint32_t(P::Comma);
        b->isKeyword = false;
        b->rhs = parseAssignment();
        e = b;
      }
      return e;
    }

    // Pushes comma-separated assignment expressions (or brace-inits) onto scratch until `close`.
    void parseExpressionList(P close)
    {
      GreaterAllowed greater(noGreater);
      if (at(close))
        return;
      for (;;)
      {
        Expr* e = at(P::LBrace) ? parseBraceInit() : parseAssignment();
        if (accept(P::Ellipsis))
        {
          UnaryExpr* u = node<UnaryExpr>(pos - 1);
          u->operand = e;
          u->op = uint32_t(P::Ellipsis);
          u->isKeyword = false;
          u->postfix = true;
          e = u;
        }
        scratch.push_back(e);
        if (!accept(P::Comma))
          break;
        if (at(close))
          break;
      }
    }

    Expr* parseBraceInit()
    {
      uint32_t start = pos;
      BraceInitExpr* b = node<BraceInitExpr>(start);
      expect(P::LBrace, "expected '{'");
      size_t mark = scratch.size();
      std::vector<uint32_t> designators;
      bool designated = false;
      while (!at(P::RBrace) && !at(TokenKind::End))
      {
        // Designated initializer `.x = 1` (or `.x{...}`): the member's name is kept per argument.
        uint32_t designator = UINT32_MAX;
        if (at(P::Dot) && atIdent(1))
        {
          advance();
          designator = tok().id;
          designated = true;
          advance();
          if (!accept(P::Assign))
          {
            if (at(P::LBrace))
            {
              scratch.push_back(parseBraceInit());
              designators.push_back(designator);
              if (!accept(P::Comma))
                break;
              continue;
            }
          }
        }
        Expr* e = at(P::LBrace) ? parseBraceInit() : parseAssignment();
        if (accept(P::Ellipsis))
        {
          UnaryExpr* u = node<UnaryExpr>(pos - 1);
          u->operand = e;
          u->op = uint32_t(P::Ellipsis);
          u->isKeyword = false;
          u->postfix = true;
          e = u;
        }
        scratch.push_back(e);
        designators.push_back(designator);
        if (!accept(P::Comma))
          break;
        if (inRecovery)
          break;
      }
      b->args = makeSpan<Expr*>(mark);
      if (designated)
        b->designators = makeSpanOf(designators);
      expect(P::RBrace, "expected '}' to close initializer");
      return b;
    }

    static bool isAssignmentOp(P p)
    {
      switch (p)
      {
        case P::Assign: case P::PlusAssign: case P::MinusAssign: case P::StarAssign: case P::SlashAssign:
        case P::PercentAssign: case P::AmpAssign: case P::PipeAssign: case P::CaretAssign: case P::ShlAssign:
        case P::ShrAssign:
          return true;
        default:
          return false;
      }
    }

    Expr* parseAssignment() { return parseAssignmentImpl(true, true); }
    Expr* parseAssignmentNoBrace() { return parseAssignmentImpl(true, false); }
    // A conditional-expression, what a bit field's width is: `bool flag : 1 = false;` is a width
    // and a default member initializer, not an assignment.
    Expr* parseConditionalExpression()
    {
      Expr* lhs = parseLogicalOr();
      if (!at(P::Question))
        return lhs;
      uint32_t opTok = pos;
      advance();
      ConditionalExpr* c = node<ConditionalExpr>(opTok);
      c->cond = lhs;
      c->then = parseExpression();
      expect(P::Colon, "expected ':' in conditional");
      c->else_ = parseConditionalExpression();
      return c;
    }
    Expr* parseTernaryNoColon() { return parseAssignmentImpl(false, true); }

    Expr* parseAssignmentImpl(bool allowTernary, bool allowBrace)
    {
      uint32_t start = pos;
      if (at(Kw::throw_))
      {
        advance();
        ThrowExpr* t = node<ThrowExpr>(start);
        t->operand = at(P::Semi) || at(P::RParen) || at(P::Comma) || at(P::Colon) ? nullptr : parseAssignment();
        return t;
      }
      if (at(Kw::co_yield_))
        advance();
      Expr* lhs = parseLogicalOr();
      if (at(TokenKind::Punctuator))
      {
        P p = P(tok().id);
        if (isAssignmentOp(p))
        {
          uint32_t opTok = pos;
          advance();
          BinaryExpr* b = node<BinaryExpr>(opTok);
          b->lhs = lhs;
          b->op = uint32_t(p);
          b->isKeyword = false;
          b->rhs = (at(P::LBrace) && allowBrace) ? parseBraceInit() : parseAssignmentImpl(true, allowBrace);
          return b;
        }
        if (p == P::Question && allowTernary)
        {
          uint32_t opTok = pos;
          advance();
          ConditionalExpr* c = node<ConditionalExpr>(opTok);
          c->cond = lhs;
          c->then = parseExpression();
          expect(P::Colon, "expected ':' in conditional");
          c->else_ = parseAssignmentImpl(true, allowBrace);
          return c;
        }
        if (p == P::Elvis)
        {
          uint32_t opTok = pos;
          advance();
          ConditionalExpr* c = node<ConditionalExpr>(opTok);
          c->cond = lhs;
          c->then = nullptr;
          c->else_ = parseAssignmentImpl(true, allowBrace);
          return c;
        }
      }
      return lhs;
    }

    // Expression inside template arguments: no top-level '>' so relational operators are off.
    bool noGreater = false;
    // Inside parentheses, brackets or an argument list `>` and `>>` are operators again, whatever
    // encloses them: `b < ((i >> 8) & 3)` read as a trial template argument list.
    struct GreaterAllowed
    {
      bool& flag;
      bool saved;
      explicit GreaterAllowed(bool& flag) : flag(flag), saved(flag) { flag = false; }
      ~GreaterAllowed() { flag = saved; }
    };
    Expr* parseExpressionNoGreater(bool& sawLogical)
    {
      bool saved = noGreater;
      noGreater = true;
      uint32_t start = pos;
      Expr* e = parseAssignmentImpl(true, true);
      // Detect top-level && / || for the comparison heuristic.
      for (uint32_t i = start; i < pos; ++i)
        if (toks[i].is(P::AndAnd) || toks[i].is(P::OrOr))
          sawLogical = true;
      noGreater = saved;
      return e;
    }

    int binaryPrecedence(const Token& t, bool& isKeyword) const
    {
      isKeyword = false;
      if (t.getKind() == TokenKind::Keyword)
      {
        isKeyword = true;
        switch (Kw(t.id))
        {
          case Kw::or_: return 1;
          case Kw::and_: return 2;
          case Kw::bitor_: return 3;
          case Kw::xor_: return 4;
          case Kw::bitand_: return 5;
          case Kw::not_eq_: return 6;
          default: return -1;
        }
      }
      if (t.getKind() != TokenKind::Punctuator)
        return -1;
      switch (P(t.id))
      {
        case P::OrOr: return 1;
        case P::AndAnd: return 2;
        case P::Pipe: return 3;
        case P::Caret: return 4;
        case P::Amp: return 5;
        case P::EqEq: case P::NotEq: return 6;
        case P::Less: case P::LessEq: case P::GreaterEq: return 7;
        case P::Greater: return noGreater ? -1 : 7;
        case P::Spaceship: return 8;
        case P::Shl: return 9;
        case P::Shr: return noGreater ? -1 : 9;
        case P::Plus: case P::Minus: return 10;
        case P::Star: case P::Slash: case P::Percent: return 11;
        case P::DotStar: case P::ArrowStar: return 12;
        default: return -1;
      }
    }

    Expr* parseLogicalOr() { return parseBinary(1); }

    Expr* parseRequiresClause()
    {
      bool saved = inRequires;
      inRequires = true;
      Expr* e = parseLogicalOr();
      inRequires = saved;
      return e;
    }

    // The parameters of the requires-expressions in a requires clause (`requires requires (const
    // T& value) { value.save(); }`) belong to the declaration it constrains, which is parsed
    // after the clause: their types name its template parameters.
    static void adoptRequiresParams(Expr* e, Decl* owner, int depth = 0)
    {
      if (!e || depth > 32)
        return;
      switch (e->kind)
      {
        case NodeKind::ParenExpr: adoptRequiresParams(static_cast<ParenExpr*>(e)->inner, owner, depth + 1); break;
        case NodeKind::UnaryExpr: adoptRequiresParams(static_cast<UnaryExpr*>(e)->operand, owner, depth + 1); break;
        case NodeKind::BinaryExpr:
          adoptRequiresParams(static_cast<BinaryExpr*>(e)->lhs, owner, depth + 1);
          adoptRequiresParams(static_cast<BinaryExpr*>(e)->rhs, owner, depth + 1);
          break;
        case NodeKind::RequiresExpr:
          for (Decl* p : static_cast<RequiresExpr*>(e)->params)
            p->parent = owner;
          break;
        default: break;
      }
    }

    Expr* parseBinary(int minPrec)
    {
      Expr* lhs = parseUnary();
      for (;;)
      {
        bool isKeyword;
        int prec = binaryPrecedence(tok(), isKeyword);
        if (prec < minPrec)
          return lhs;
        if (noGreater && (at(P::GreaterEq) || at(P::ShrAssign)))
          return lhs;
        uint32_t opTok = pos;
        uint32_t op = tok().id;
        advance();
        // Fold expression `pack op ...`
        if (at(P::Ellipsis))
        {
          advance();
          FoldExpr* f = node<FoldExpr>(opTok);
          f->lhs = lhs;
          f->op = op;
          f->rhs = nullptr;
          if (!at(P::RParen))
          {
            // ( pack op ... op init )
            bool k;
            if (binaryPrecedence(tok(), k) >= 0)
            {
              advance();
              f->rhs = parseBinary(prec + 1);
            }
          }
          lhs = f;
          continue;
        }
        Expr* rhs = parseBinary(prec + 1);
        // No chained comparison without parentheses (design.md 22): `a < b > c` (which C++ reads as
        // `(a < b) > c`, a template-id to a reader), `a < b < c`, `a == b != c`. kpp's own sources
        // only: the converter's C++ input keeps C++'s reading.
        if (kppSource && !isKeyword && lhs->kind == NodeKind::BinaryExpr && !static_cast<BinaryExpr*>(lhs)->isKeyword)
        {
          auto relational = [](uint32_t o) { return o == uint32_t(P::Less) || o == uint32_t(P::Greater) || o == uint32_t(P::LessEq) || o == uint32_t(P::GreaterEq); };
          auto equality = [](uint32_t o) { return o == uint32_t(P::EqEq) || o == uint32_t(P::NotEq); };
          uint32_t inner = static_cast<BinaryExpr*>(lhs)->op;
          if ((relational(op) && relational(inner)) || (equality(op) && equality(inner)))
            out.diagnostics.push_back({opTok, "a comparison of a comparison needs parentheses: `(a < b) > c`, or `a < b && b > c` (design.md 22)"});
        }
        BinaryExpr* b = node<BinaryExpr>(opTok);
        b->lhs = lhs;
        b->rhs = rhs;
        b->op = op;
        b->isKeyword = isKeyword;
        // `(HANDLE) -1`: a cast when HANDLE is a type, which only the resolver knows (ParsedFile::castCandidates).
        Expr* paren = lhs; // also under casts: `(long long)(HANDLE) -2`
        while (paren->kind == NodeKind::CastExpr && static_cast<CastExpr*>(paren)->operand)
          paren = static_cast<CastExpr*>(paren)->operand;
        if (!isKeyword && !cMode && paren->kind == NodeKind::ParenExpr && static_cast<ParenExpr*>(paren)->inner &&
            static_cast<ParenExpr*>(paren)->inner->kind == NodeKind::NameExpr &&
            (op == uint32_t(P::Minus) || op == uint32_t(P::Plus) || op == uint32_t(P::Star) || op == uint32_t(P::Amp)))
          out.castCandidates.push_back({b, currentParent});
        lhs = b;
      }
    }

    Expr* parseUnary()
    {
      DepthGuard guard(*this);
      if (depth > 400)
      {
        error("nesting too deep");
        return node<ErrorExpr>(pos);
      }
      uint32_t start = pos;
      if (at(TokenKind::Punctuator))
      {
        P p = P(tok().id);
        switch (p)
        {
          case P::PlusPlus: case P::MinusMinus: case P::Plus: case P::Minus: case P::Bang: case P::Tilde:
          case P::Star: case P::Amp: case P::AndAnd:
          {
            // `~Foo()` could be a destructor name in `obj.~Foo()` but that goes through member parsing.
            advance();
            UnaryExpr* u = node<UnaryExpr>(start);
            u->op = uint32_t(p);
            u->isKeyword = false;
            u->postfix = false;
            u->operand = parseUnary();
            return u;
          }
          case P::Ellipsis:
          {
            // Fold expression `( ... op pack )`.
            advance();
            bool k;
            if (binaryPrecedence(tok(), k) >= 0)
            {
              FoldExpr* f = node<FoldExpr>(start);
              f->op = tok().id;
              advance();
              f->lhs = nullptr;
              f->rhs = parseBinary(1);
              return f;
            }
            error("unexpected '...'");
            return node<ErrorExpr>(start);
          }
          case P::ColonColon:
            if (at(Kw::new_, 1) || at(Kw::delete_, 1))
            {
              advance();
              Expr* e = parseUnary();
              if (e->kind == NodeKind::NewExpr) static_cast<NewExpr*>(e)->global = true;
              else if (e->kind == NodeKind::DeleteExpr) static_cast<DeleteExpr*>(e)->global = true;
              return e;
            }
            break;
          default:
            break;
        }
      }
      if (at(TokenKind::Keyword))
      {
        switch (Kw(tok().id))
        {
          case Kw::not_: case Kw::compl_:
          {
            advance();
            UnaryExpr* u = node<UnaryExpr>(start);
            u->op = uint32_t(Kw(toks[start].id));
            u->isKeyword = true;
            u->postfix = false;
            u->operand = parseUnary();
            return u;
          }
          case Kw::sizeof_: case Kw::alignof_:
          {
            SizeofExpr* s = node<SizeofExpr>(start);
            s->sizeofKind = Kw(tok().id);
            advance();
            if (accept(P::Ellipsis))
              s->isPack = true;
            if (at(P::LParen))
            {
              uint32_t save = pos;
              size_t mark = scratch.size();
              advance();
              Type* t = parseType();
              if (t && at(P::LBracket))
                t = parseArraySuffixes(t); // sizeof(int[4])
              if (t && at(P::RParen))
              {
                advance();
                s->type = t;
                return s;
              }
              restore(save);
              scratch.resize(mark);
            }
            s->operand = parseUnary();
            return s;
          }
          case Kw::noexcept_:
            if (at(P::LParen, 1))
            {
              // The noexcept operator: an error until the checker decides whether its operand can
              // throw (it was `true` whatever the operand, a silent wrong answer for a call of a
              // function not declared noexcept).
              error("the noexcept operator is not supported yet");
              advance();
              skipBalanced();
              return node<ErrorExpr>(start);
            }
            break;
          case Kw::new_:
            return parseNew(start);
          case Kw::delete_:
          {
            advance();
            DeleteExpr* d = node<DeleteExpr>(start);
            d->global = false;
            d->isArray = false;
            if (at(P::LBracket) && at(P::RBracket, 1))
            {
              advance();
              advance();
              d->isArray = true;
            }
            d->operand = parseUnary();
            return d;
          }
          case Kw::co_await_:
            advance();
            return parseUnary();
          case Kw::static_cast_: case Kw::dynamic_cast_: case Kw::reinterpret_cast_: case Kw::const_cast_:
          {
            NamedCastExpr* c = node<NamedCastExpr>(start);
            c->castKind = Kw(tok().id);
            advance();
            expect(P::Less, "expected '<' after cast");
            c->type = parseType();
            if (!c->type)
              error("expected type in cast");
            if (!acceptTemplateClose())
              error("expected '>' after cast type");
            expect(P::LParen, "expected '(' in cast");
            c->operand = parseExpression();
            expect(P::RParen, "expected ')' in cast");
            return parsePostfix(c);
          }
          case Kw::requires_:
          {
            // requires(params) { expr; typename T::x; { expr } -> Constraint; requires expr; }
            advance();
            RequiresExpr* r = node<RequiresExpr>(start);
            // Its parameters and requirements are ordinary declarations and expressions, also
            // inside a requires clause (`requires requires (T t) { (int)t; }`).
            bool savedRequires = inRequires;
            inRequires = false;
            struct RestoreRequires { bool& flag; bool saved; ~RestoreRequires() { flag = saved; } } restoreRequires{inRequires, savedRequires};
            if (accept(P::LParen))
            {
              size_t mark = scratch.size();
              if (!at(P::RParen))
                for (;;)
                {
                  Decl* p = parseParameter();
                  if (!p)
                    break;
                  scratch.push_back(p);
                  if (!accept(P::Comma))
                    break;
                }
              r->params = makeSpan<Decl*>(mark);
              expect(P::RParen, "expected ')' after the requires parameters");
            }
            if (expect(P::LBrace, "expected '{' after requires"))
            {
              std::vector<Expr*> requirements;
              std::vector<Type*> typeRequirements;
              while (!at(P::RBrace) && !at(TokenKind::End))
              {
                if (accept(Kw::typename_))
                {
                  if (Type* t = parseType())
                    typeRequirements.push_back(t);
                }
                else if (accept(P::LBrace))
                {
                  if (Expr* e = parseExpression())
                    requirements.push_back(e);
                  expect(P::RBrace, "expected '}' after the compound requirement");
                  while (!at(P::Semi) && !at(P::RBrace) && !at(TokenKind::End))
                    advance(); // `-> Constraint<...>`: the result's constraint is not checked
                }
                else
                {
                  accept(Kw::requires_); // a nested requirement: its expression must hold, which the typing of a constant does not check
                  if (Expr* e = parseExpression())
                    requirements.push_back(e);
                }
                if (!accept(P::Semi))
                {
                  while (!at(P::Semi) && !at(P::RBrace) && !at(TokenKind::End))
                    advance();
                  accept(P::Semi);
                }
              }
              expect(P::RBrace, "expected '}' after the requirements");
              r->requirements = makeSpanOf(requirements);
              r->typeRequirements = makeSpanOf(typeRequirements);
            }
            return r;
          }
          default:
            break;
        }
      }
      // C-style cast `(Type)expr`; never in a requires clause, whose `(isConvertible<U*, T*>)` may be
      // followed by the declaration's `implicit` or name (C++ allows no cast there).
      if (at(P::LParen) && !inRequires)
      {
        uint32_t save = pos;
        size_t mark = scratch.size();
        uint32_t nodes = out.nodeCount;
        advance();
        Type* t = parseType();
        if (t && at(P::RParen))
        {
          advance();
          if (looksLikeCastOperand(t) || parenthesisedTypeAhead())
          {
            CastExpr* c = node<CastExpr>(start);
            c->type = t;
            c->operand = parseUnary();
            return c;
          }
        }
        restore(save);
        scratch.resize(mark);
        out.nodeCount = nodes;
      }
      return parsePostfix(parsePrimary());
    }

    // At `(` after `(T)`: a parenthesised type that only a type can be (`(long long)`, `(char*)`,
    // `(const Item&)`), which no call's argument list is, so `(T)` casts what follows:
    // `(HANDLE)(long long)-1` is `(HANDLE)((long long)-1)`.
    bool parenthesisedTypeAhead()
    {
      if (!at(P::LParen))
        return false;
      uint32_t save = pos;
      size_t mark = scratch.size();
      uint32_t nodes = out.nodeCount;
      advance();
      Type* t = parseType();
      bool type = t && at(P::RParen) && (t->kind != NodeKind::NamedType || isTemplateTypeName(t));
      restore(save);
      scratch.resize(mark);
      out.nodeCount = nodes;
      return type;
    }

    bool looksLikeCastOperand(Type* t) const
    {
      // After `(T)`: a cast if T is obviously a type (builtin, pointer, const...) or the next
      // token cannot continue an expression that `(T)` started.
      bool obviousType = t->kind != NodeKind::NamedType || isTemplateTypeName(t);
      if (!obviousType && cMode)
      {
        const QualifiedName& name = static_cast<NamedType*>(t)->name;
        obviousType = name.parts.size() == 1 && !name.parts[0].hasTemplateArgs && cTypedefNames.count(name.parts[0].symbol);
      }
      if (!obviousType)
      {
        std::string_view s = spelling(toks[t->token]);
        // (and kpp's own names of them, design.md 13: `(usize)-1` as `(size_t)-1`)
        obviousType = s == "size_t" || s == "ptrdiff_t" || s == "intptr_t" || s == "uintptr_t" || s.starts_with("__int") ||
                      ((s.starts_with("int") || s.starts_with("uint")) && s.ends_with("_t")) || isKppNumberName(s);
      }
      if (obviousType)
        return atIdent() || at(TokenKind::Number) || at(TokenKind::String) || at(TokenKind::Char) || at(P::LParen) ||
               at(P::Minus) || at(P::Plus) || at(P::Bang) || at(P::Tilde) || at(P::Star) || at(P::Amp) ||
               at(P::PlusPlus) || at(P::MinusMinus) || at(TokenKind::Keyword) || at(P::ColonColon) || at(P::LBracket);
      return atIdent() || at(TokenKind::Number) || at(TokenKind::String) || at(TokenKind::Char) || at(P::Bang) ||
             at(P::Tilde) || at(Kw::this_) || at(Kw::sizeof_) || at(Kw::new_) || at(Kw::true_) || at(Kw::false_) ||
             at(Kw::nullptr_) || at(P::ColonColon) || at(Kw::static_cast_) || at(Kw::reinterpret_cast_) ||
             at(Kw::const_cast_) || at(Kw::dynamic_cast_) || startsTypeOperand();
    }

    // A builtin type, `decltype(...)` or `typename X::Y` after `(T)` (a functional cast or brace
    // initialisation: `(S64)char(7)`, `(S64)decltype(a)(10)`), or `alignof`, `noexcept`, `typeid`:
    // no parenthesised expression can be followed by one, so `(T)` casts what it starts.
    bool startsTypeOperand() const
    {
      if (!at(TokenKind::Keyword))
        return false;
      switch (Kw(tok().id))
      {
        case Kw::bool_: case Kw::char_: case Kw::char8_t_: case Kw::char16_t_: case Kw::char32_t_: case Kw::wchar_t_:
        case Kw::short_: case Kw::int_: case Kw::long_: case Kw::signed_: case Kw::unsigned_: case Kw::float_:
        case Kw::double_: case Kw::decltype_: case Kw::typename_: case Kw::alignof_: case Kw::noexcept_: case Kw::typeid_:
          return true;
        default:
          return false;
      }
    }

    // `offsetof(T, member)` (C's macro, from <cstddef>): read as what it expands to, the address of
    // the member in a T at address 0 as a size_t, `size_t(&((T*)nullptr)->member)`. The member may
    // be a path, `a.b[2]`. A union member of an unnamed type, `offsetof(struct cD, u)`, works too.
    Expr* parseOffsetof(uint32_t start)
    {
      advance(); // offsetof
      expect(P::LParen, "expected '('");
      Type* t = parseType();
      if (!t)
      {
        error("expected a type in offsetof");
        return node<ErrorExpr>(start);
      }
      expect(P::Comma, "expected ',' in offsetof");
      PointerType* pointer = node<PointerType>(t->token);
      pointer->pointee = t;
      LiteralExpr* null = node<LiteralExpr>(start);
      null->literalKind = TokenKind::Keyword;
      null->keyword = Kw::nullptr_;
      CastExpr* object = node<CastExpr>(start);
      object->type = pointer;
      object->operand = null;
      Expr* path = object;
      P op = P::Arrow;
      for (;;)
      {
        if (!atIdent())
        {
          error("expected a member name in offsetof");
          return node<ErrorExpr>(start);
        }
        MemberExpr* m = node<MemberExpr>(pos);
        m->base = path;
        m->op = op;
        NamePart part;
        part.symbol = tok().id;
        std::vector<NamePart> parts{part};
        m->member.parts = makeSpanOf(parts);
        advance();
        path = m;
        while (at(P::LBracket))
        {
          IndexExpr* index = node<IndexExpr>(pos);
          advance();
          index->base = path;
          size_t mark = scratch.size();
          scratch.push_back(parseExpression());
          index->args = makeSpan<Expr*>(mark);
          expect(P::RBracket, "expected ']'");
          path = index;
        }
        if (!accept(P::Dot))
          break;
        op = P::Dot;
      }
      expect(P::RParen, "expected ')' after offsetof");
      UnaryExpr* address = node<UnaryExpr>(start);
      address->op = uint32_t(P::Amp);
      address->isKeyword = false;
      address->postfix = false;
      address->operand = path;
      BuiltinType* size = node<BuiltinType>(start);
      size->keyword = Kw::int_;
      size->longCount = 2;
      size->isUnsigned = true;
      CastExpr* result = node<CastExpr>(start);
      result->type = size;
      result->operand = address;
      return result;
    }

    // `va_arg(list, T)` in a function defined with `...` (C's macro): a call of va_arg whose second
    // argument is `sizeof(T)`, which carries the type (an argument cannot be a type otherwise);
    // the resolver types the call as T, the code generator reads the next argument.
    Expr* parseVaArg(uint32_t start)
    {
      CallExpr* c = node<CallExpr>(start);
      NameExpr* callee = node<NameExpr>(start);
      parseQualifiedName(callee->name, false);
      c->callee = callee;
      expect(P::LParen, "expected '('");
      size_t mark = scratch.size();
      scratch.push_back(parseAssignment());
      expect(P::Comma, "expected ',' in va_arg");
      SizeofExpr* type = node<SizeofExpr>(pos);
      type->sizeofKind = Kw::sizeof_;
      type->type = parseType();
      if (!type->type)
        error("expected a type in va_arg");
      scratch.push_back(type);
      c->args = makeSpan<Expr*>(mark);
      expect(P::RParen, "expected ')' after va_arg");
      return c;
    }

    Expr* parseNew(uint32_t start)
    {
      advance();
      NewExpr* n = node<NewExpr>(start);
      size_t mark = scratch.size();
      if (at(P::LParen))
      {
        // Placement or parenthesised type.
        uint32_t save = pos;
        uint32_t nodes = out.nodeCount;
        advance();
        Type* t = parseType();
        if (t && at(P::RParen) && !at(P::LParen, 1) && !at(P::LBrace, 1) && !atIdent(1))
        {
          // new (T) is rare; treat as placement unless clearly a type.
        }
        restore(save);
        out.nodeCount = nodes;
        scratch.resize(mark);
        advance();
        parseExpressionList(P::RParen);
        n->placement = makeSpan<Expr*>(mark);
        expect(P::RParen, "expected ')' after placement arguments");
      }
      if (at(P::LParen))
      {
        advance();
        n->type = parseType();
        expect(P::RParen, "expected ')' after new type");
      }
      else
        n->type = parseType();
      if (!n->type)
      {
        error("expected type after new");
        return n;
      }
      n->type = parseArraySuffixes(n->type);
      mark = scratch.size();
      if (accept(P::LParen))
      {
        n->parenthesized = true;
        parseExpressionList(P::RParen);
        n->args = makeSpan<Expr*>(mark);
        expect(P::RParen, "expected ')' after new arguments");
      }
      else if (at(P::LBrace))
      {
        scratch.push_back(parseBraceInit());
        n->args = makeSpan<Expr*>(mark);
        n->braced = true;
      }
      return n;
    }

    Expr* parsePrimary()
    {
      uint32_t start = pos;
      if (atDescriptorType() && (at(P::LParen, 3) || at(P::LBrace, 3)))
        return parseTypeAsExpr(start); // `a.type()`: a value of the descriptor's type
      if (atIdent(kwNameof) && at(P::LParen, 1))
      {
        NameofExpr* n = node<NameofExpr>(start);
        advance();
        advance();
        n->operand = parseExpression();
        expect(P::RParen, "expected ')' after nameof");
        return n;
      }
      switch (tok().getKind())
      {
        case TokenKind::Number:
        case TokenKind::Char:
        {
          LiteralExpr* l = node<LiteralExpr>(start);
          l->literalKind = tok().getKind();
          l->keyword = Kw::Count;
          advance();
          return l;
        }
        case TokenKind::String:
        {
          LiteralExpr* l = node<LiteralExpr>(start);
          l->literalKind = TokenKind::String;
          l->keyword = Kw::Count;
          while (at(TokenKind::String) || (atMacroName() && !at(P::LParen, 1) && !at(P::ColonColon, 1)))
          {
            // In kpp only literals concatenate: `"a" NAME "b"` with NAME a string constant is C's
            // macro pasting (valid in the C++ the converter reads), whose pieces the literal would lose.
            if (!at(TokenKind::String) && kppSource)
              error("a name next to a string literal (only string literals concatenate)");
            advance();
          }
          return l;
        }
        case TokenKind::Keyword:
          switch (Kw(tok().id))
          {
            case Kw::true_: case Kw::false_: case Kw::nullptr_:
            {
              LiteralExpr* l = node<LiteralExpr>(start);
              l->literalKind = TokenKind::Keyword;
              l->keyword = Kw(tok().id);
              advance();
              return l;
            }
            case Kw::this_:
              advance();
              return node<ThisExpr>(start);
            case Kw::typeid_:
            {
              advance();
              TypeidExpr* t = node<TypeidExpr>(start);
              expect(P::LParen, "expected '('");
              uint32_t save = pos;
              Type* ty = parseType();
              if (ty && at(P::RParen))
                t->operand = ty;
              else
              {
                restore(save);
                t->operand = parseExpression();
              }
              expect(P::RParen, "expected ')'");
              return t;
            }
            case Kw::typename_:
            case Kw::decltype_:
            case Kw::struct_:
            case Kw::class_:
            case Kw::union_:
            case Kw::enum_:
              return parseTypeAsExpr(start);
            case Kw::operator_:
              return parseNameExpr(start); // `operator==(l, r)`: the operator function by name
            default:
              if (isBuiltinTypeKeyword(Kw(tok().id)))
                return parseTypeAsExpr(start);
              error("unexpected keyword in expression");
              return node<ErrorExpr>(start);
          }
        case TokenKind::Identifier:
          if (at(P::LParen, 1) && !cMode && spelling() == "offsetof")
            return parseOffsetof(start);
          if (at(P::LParen, 1) && !cMode && spelling() == "va_arg")
            return parseVaArg(start);
          return parseNameExpr(start);
        case TokenKind::Punctuator:
          if (at(P::LParen))
          {
            advance();
            ParenExpr* p = node<ParenExpr>(start);
            GreaterAllowed greater(noGreater);
            p->inner = parseExpression();
            expect(P::RParen, "expected ')'");
            return p;
          }
          if (at(P::LBracket))
            return parseLambda(start);
          if (at(P::LBrace))
            return parseBraceInit();
          if (at(P::ColonColon))
            return parseNameExpr(start);
          break;
        default:
          break;
      }
      error("expected expression");
      return node<ErrorExpr>(start);
    }

    // `int(x)`, `typename T::type{...}`, `decltype(x)::f()`, `Foo<T>::bar` where the name is a type-ish keyword.
    Expr* parseTypeAsExpr(uint32_t start)
    {
      Type* t = parseType();
      if (!t)
      {
        error("expected type");
        return node<ErrorExpr>(start);
      }
      if (at(P::LParen) || at(P::LBrace))
      {
        FunctionalCastExpr* c = node<FunctionalCastExpr>(start);
        c->type = t;
        c->braced = at(P::LBrace);
        size_t mark = scratch.size();
        if (c->braced)
          scratch.push_back(parseBraceInit());
        else
        {
          advance();
          parseExpressionList(P::RParen);
          expect(P::RParen, "expected ')'");
        }
        c->args = makeSpan<Expr*>(mark);
        return c;
      }
      if (at(P::ColonColon))
      {
        // decltype(x)::member
        advance();
        NameExpr* n = node<NameExpr>(start);
        parseQualifiedName(n->name, false);
        if (t->kind == NodeKind::DecltypeType)
          n->name.typeQualifier = t;
        return n;
      }
      // A type where an expression was expected (e.g. in a macro argument). Represent as a name.
      NameExpr* n = node<NameExpr>(start);
      if (t->kind == NodeKind::NamedType)
        n->name = static_cast<NamedType*>(t)->name;
      return n;
    }

    Expr* parseNameExpr(uint32_t start)
    {
      NameExpr* n = node<NameExpr>(start);
      if (!parseQualifiedName(n->name, false))
      {
        error("expected name");
        return node<ErrorExpr>(start);
      }
      if (at(TokenKind::String) && n->name.parts.size() == 1 && isAllCaps(spelling(toks[start])))
      {
        if (kppSource)
          error("a name next to a string literal (only string literals concatenate)"); // `NAME "b"`, as above
        LiteralExpr* l = node<LiteralExpr>(start);
        l->literalKind = TokenKind::String;
        l->keyword = Kw::Count;
        while (at(TokenKind::String) || (atMacroName() && !at(P::LParen, 1) && !at(P::ColonColon, 1)))
          advance();
        return l;
      }
      // `Name{...}` is a braced functional cast / aggregate construction (not in a requires
      // clause, where the brace after the constraint opens the function's body).
      if (at(P::LBrace) && !noGreater && !inRequires)
      {
        // Distinguish from a block starting after an expression statement: only at expression
        // positions where '{' cannot start a statement. The caller contexts (arguments,
        // initializers, return) are the common ones; `Foo{}` as a statement start is rare.
        FunctionalCastExpr* c = node<FunctionalCastExpr>(start);
        NamedType* t = node<NamedType>(start);
        t->name = n->name;
        c->type = t;
        c->braced = true;
        size_t mark = scratch.size();
        scratch.push_back(parseBraceInit());
        c->args = makeSpan<Expr*>(mark);
        return c;
      }
      return n;
    }

    // `auto` (also under const, references, pointers, a member pointer's member type, a pack) in
    // a parameter type.
    static bool mentionsAuto(const Type* t)
    {
      for (;;)
      {
        if (!t)
          return false;
        switch (t->kind)
        {
          case NodeKind::AutoType: return true;
          case NodeKind::ConstType: t = static_cast<const ConstType*>(t)->inner; break;
          case NodeKind::PointerType: t = static_cast<const PointerType*>(t)->pointee; break;
          case NodeKind::LRefType: t = static_cast<const LRefType*>(t)->referenced; break;
          case NodeKind::RRefType: t = static_cast<const RRefType*>(t)->referenced; break;
          case NodeKind::PackType: t = static_cast<const PackType*>(t)->pattern; break;
          case NodeKind::MemberPointerType: t = static_cast<const MemberPointerType*>(t)->pointee; break;
          default: return false;
        }
      }
    }

    // The same type with its `auto` replaced (in place) by the given type.
    static Type* replaceAuto(Type* t, Type* with)
    {
      if (!t)
        return t;
      switch (t->kind)
      {
        case NodeKind::AutoType: return with;
        case NodeKind::ConstType: { auto* c = static_cast<ConstType*>(t); c->inner = replaceAuto(c->inner, with); return t; }
        case NodeKind::PointerType: { auto* p = static_cast<PointerType*>(t); p->pointee = replaceAuto(p->pointee, with); return t; }
        case NodeKind::LRefType: { auto* r = static_cast<LRefType*>(t); r->referenced = replaceAuto(r->referenced, with); return t; }
        case NodeKind::RRefType: { auto* r = static_cast<RRefType*>(t); r->referenced = replaceAuto(r->referenced, with); return t; }
        case NodeKind::PackType: { auto* p = static_cast<PackType*>(t); p->pattern = replaceAuto(p->pattern, with); return t; }
        case NodeKind::MemberPointerType: { auto* m = static_cast<MemberPointerType*>(t); m->pointee = replaceAuto(m->pointee, with); return t; }
        default: return t;
      }
    }

    // t, the declarator operators of a parenthesised declarator (`&`, `*`, `const`) around base,
    // with base replaced by newBase (in place).
    static Type* rebase(Type* t, Type* base, Type* newBase)
    {
      if (!t || t == base)
        return newBase;
      switch (t->kind)
      {
        case NodeKind::PointerType: { auto* p = static_cast<PointerType*>(t); p->pointee = rebase(p->pointee, base, newBase); return t; }
        case NodeKind::LRefType: { auto* r = static_cast<LRefType*>(t); r->referenced = rebase(r->referenced, base, newBase); return t; }
        case NodeKind::RRefType: { auto* r = static_cast<RRefType*>(t); r->referenced = rebase(r->referenced, base, newBase); return t; }
        case NodeKind::ConstType: { auto* c = static_cast<ConstType*>(t); c->inner = rebase(c->inner, base, newBase); return t; }
        default: return t;
      }
    }

    // `auto` in a parameter's type (`const auto& x`, `auto C::*... members`): each such
    // parameter gets an invented type parameter, appended to templateParams, named in place of
    // the `auto` (a generic lambda's parameters, C++20's abbreviated function templates).
    // Returns whether any was invented.
    bool inventAutoParams(Span<Decl*> params, std::vector<Decl*>& templateParams)
    {
      bool any = false;
      for (Decl* pd : params)
      {
        auto* p = static_cast<ParamDecl*>(pd);
        if (!mentionsAuto(p->type))
          continue;
        TemplateParamDecl* tp = decl<TemplateParamDecl>(p->type->token);
        tp->isType = true;
        tp->isPack = p->isPack;
        tp->name = symbols.intern("__auto_" + std::to_string(autoCount++));
        NamedType* named = node<NamedType>(p->type->token);
        NamePart part;
        part.symbol = tp->name;
        named->name.parts = makeSpanOf(std::vector<NamePart>{part});
        p->type = replaceAuto(p->type, named);
        templateParams.push_back(tp);
        any = true;
      }
      return any;
    }

    Expr* parseLambda(uint32_t start)
    {
      LambdaExpr* l = node<LambdaExpr>(start);
      expect(P::LBracket, "expected '['");
      std::vector<LambdaCapture> captures;
      while (!at(P::RBracket) && !at(TokenKind::End))
      {
        LambdaCapture cap{};
        if (at(P::Amp) && (at(P::Comma, 1) || at(P::RBracket, 1)))
        {
          advance();
          l->defaultCapture = 2;
        }
        else if (at(P::Assign) && (at(P::Comma, 1) || at(P::RBracket, 1)))
        {
          advance();
          l->defaultCapture = 1;
        }
        else if (accept(Kw::this_))
        {
          cap.isThis = true;
          captures.push_back(cap);
        }
        else if (at(P::Star) && at(Kw::this_, 1))
        {
          advance();
          advance();
          cap.isThis = true;
          captures.push_back(cap);
        }
        else
        {
          cap.byRef = accept(P::Amp);
          if (!atIdent())
          {
            error("expected a capture");
            break;
          }
          uint32_t nameToken = pos;
          cap.symbol = tok().id;
          advance();
          accept(P::Ellipsis);
          bool braced = false;
          if (accept(P::Assign))
            cap.init = parseAssignment();
          else if (at(P::LBrace))
          {
            cap.init = parseBraceInit();
            braced = true;
          }
          else if (accept(P::LParen))
          {
            cap.init = parseExpression();
            expect(P::RParen, "expected ')'");
          }
          if (cap.init)
          {
            // An init-capture declares a variable of the closure: `auto x = expr;` in effect.
            auto* v = decl<VariableDecl>(nameToken);
            v->type = node<AutoType>(nameToken);
            NamePart part;
            part.symbol = cap.symbol;
            v->name.parts = makeSpanOf(std::vector<NamePart>{part});
            v->init = cap.init;
            v->bracedInit = braced;
            cap.decl = v;
          }
          captures.push_back(cap);
        }
        if (!accept(P::Comma))
          break;
      }
      expect(P::RBracket, "expected ']' after captures");
      l->captures = makeSpanOf(captures);
      // The closure type: a synthetic class with a unique name, never indexed as a declaration.
      // Named by its file's tag and the lambda's number in the file (`__lambda_6c1f0e2a_0`): the same
      // in every run, wherever the sources are and whatever files come before it (ParsedFile::lambdaTag).
      ClassDecl* closure = decl<ClassDecl>(start);
      closure->keyword = Kw::struct_;
      NamePart part;
      char tag[16];
      snprintf(tag, sizeof tag, out.lambdaTag ? "%08x" : "%u", out.lambdaTag ? out.lambdaTag : out.index);
      part.symbol = symbols.intern("__lambda_" + std::string(tag) + "_" + std::to_string(lambdaCount++));
      closure->name.parts = makeSpanOf(std::vector<NamePart>{part});
      closure->isClosure = true;
      closure->lambda = l;
      l->closure = closure;
      if (at(P::Less))
      {
        advance();
        size_t mark = scratch.size();
        while (!at(P::Greater) && !at(TokenKind::End))
        {
          Decl* p = parseTemplateParam();
          if (!p)
            break;
          scratch.push_back(p);
          if (!accept(P::Comma))
            break;
        }
        l->templateParams = makeSpan<Decl*>(mark);
        acceptTemplateClose();
      }
      if (at(P::LParen))
      {
        Declarator d;
        parseFunctionSuffix(d);
        l->params = d.params;
        l->returnType = d.trailingReturn;
      }
      // `auto` in a parameter makes the lambda generic: each such parameter gets an invented type
      // parameter (as C++ does) and the call operator is deduced at every call. A lambda with
      // template parameters keeps them on its closure, which its parameters belong to, so that
      // the parameters' types can see them.
      {
        std::vector<Decl*> templateParams(l->templateParams.begin(), l->templateParams.end());
        inventAutoParams(l->params, templateParams);
        if (!templateParams.empty())
        {
          if (templateParams.size() != l->templateParams.size())
            l->templateParams = makeSpanOf(templateParams);
          closure->templateParams = l->templateParams;
          for (Decl* pd : l->params)
            pd->parent = closure;
        }
      }
      for (;;)
      {
        if (accept(Kw::mutable_) || accept(Kw::constexpr_) || accept(Kw::consteval_))
          continue;
        if (at(Kw::noexcept_))
        {
          advance();
          if (at(P::LParen))
            skipBalanced();
          continue;
        }
        if (at(P::Arrow))
        {
          advance();
          l->returnType = parseType();
          continue;
        }
        if (at(Kw::requires_))
        {
          advance();
          parseRequiresClause();
          continue;
        }
        skipAttributes();
        break;
      }
      if (!at(P::LBrace))
      {
        error("expected lambda body");
        return l;
      }
      l->body = parseBlock();
      return l;
    }

    Expr* parsePostfix(Expr* e)
    {
      for (;;)
      {
        uint32_t start = pos;
        if (at(P::LParen))
        {
          advance();
          CallExpr* c = node<CallExpr>(start);
          c->callee = e;
          size_t mark = scratch.size();
          parseExpressionList(P::RParen);
          c->args = makeSpan<Expr*>(mark);
          expect(P::RParen, "expected ')' after arguments");
          e = c;
        }
        else if (at(P::LBracket) && !at(P::LBracket, 1))
        {
          advance();
          IndexExpr* i = node<IndexExpr>(start);
          i->base = e;
          size_t mark = scratch.size();
          parseExpressionList(P::RBracket);
          i->args = makeSpan<Expr*>(mark);
          expect(P::RBracket, "expected ']'");
          e = i;
        }
        else if (at(P::Dot) || at(P::Arrow) || at(P::SafeArrow))
        {
          P op = P(tok().id);
          advance();
          MemberExpr* m = node<MemberExpr>(start);
          m->base = e;
          m->op = op;
          if (accept(P::LBracket))
          {
            // `object.[m]`: the member a reflection descriptor names (design.md 8.3).
            m->dynamicMember = parseExpression();
            expect(P::RBracket, "expected ']' after '.['");
            e = m;
            continue;
          }
          accept(Kw::template_);
          if (!parseQualifiedName(m->member, false))
          {
            error("expected member name");
            return m;
          }
          e = m;
        }
        else if (at(P::PlusPlus) || at(P::MinusMinus))
        {
          UnaryExpr* u = node<UnaryExpr>(start);
          u->operand = e;
          u->op = tok().id;
          u->isKeyword = false;
          u->postfix = true;
          advance();
          e = u;
        }
        else if (at(P::LBrace) && e->kind == NodeKind::NameExpr && false)
        {
        }
        else
          return e;
      }
    }
  };

  template<> constexpr NodeKind Parser::nodeKindOf<NamedType>() { return NodeKind::NamedType; }
  template<> constexpr NodeKind Parser::nodeKindOf<PointerType>() { return NodeKind::PointerType; }
  template<> constexpr NodeKind Parser::nodeKindOf<LRefType>() { return NodeKind::LRefType; }
  template<> constexpr NodeKind Parser::nodeKindOf<RRefType>() { return NodeKind::RRefType; }
  template<> constexpr NodeKind Parser::nodeKindOf<ConstType>() { return NodeKind::ConstType; }
  template<> constexpr NodeKind Parser::nodeKindOf<ArrayType>() { return NodeKind::ArrayType; }
  template<> constexpr NodeKind Parser::nodeKindOf<FunctionType>() { return NodeKind::FunctionType; }
  template<> constexpr NodeKind Parser::nodeKindOf<DecltypeType>() { return NodeKind::DecltypeType; }
  template<> constexpr NodeKind Parser::nodeKindOf<AutoType>() { return NodeKind::AutoType; }
  template<> constexpr NodeKind Parser::nodeKindOf<BuiltinType>() { return NodeKind::BuiltinType; }
  template<> constexpr NodeKind Parser::nodeKindOf<MemberPointerType>() { return NodeKind::MemberPointerType; }
  template<> constexpr NodeKind Parser::nodeKindOf<PackType>() { return NodeKind::PackType; }
  template<> constexpr NodeKind Parser::nodeKindOf<NamespaceDecl>() { return NodeKind::NamespaceDecl; }
  template<> constexpr NodeKind Parser::nodeKindOf<ClassDecl>() { return NodeKind::ClassDecl; }
  template<> constexpr NodeKind Parser::nodeKindOf<EnumDecl>() { return NodeKind::EnumDecl; }
  template<> constexpr NodeKind Parser::nodeKindOf<EnumeratorDecl>() { return NodeKind::EnumeratorDecl; }
  template<> constexpr NodeKind Parser::nodeKindOf<FunctionDecl>() { return NodeKind::FunctionDecl; }
  template<> constexpr NodeKind Parser::nodeKindOf<ParamDecl>() { return NodeKind::ParamDecl; }
  template<> constexpr NodeKind Parser::nodeKindOf<VariableDecl>() { return NodeKind::VariableDecl; }
  template<> constexpr NodeKind Parser::nodeKindOf<UsingAliasDecl>() { return NodeKind::UsingAliasDecl; }
  template<> constexpr NodeKind Parser::nodeKindOf<UsingDirectiveDecl>() { return NodeKind::UsingDirectiveDecl; }
  template<> constexpr NodeKind Parser::nodeKindOf<UsingNameDecl>() { return NodeKind::UsingNameDecl; }
  template<> constexpr NodeKind Parser::nodeKindOf<TemplateParamDecl>() { return NodeKind::TemplateParamDecl; }
  template<> constexpr NodeKind Parser::nodeKindOf<StaticAssertDecl>() { return NodeKind::StaticAssertDecl; }
  template<> constexpr NodeKind Parser::nodeKindOf<MacroDecl>() { return NodeKind::MacroDecl; }
  template<> constexpr NodeKind Parser::nodeKindOf<ErrorDecl>() { return NodeKind::ErrorDecl; }
  template<> constexpr NodeKind Parser::nodeKindOf<EmptyDecl>() { return NodeKind::EmptyDecl; }
  template<> constexpr NodeKind Parser::nodeKindOf<DeductionGuideDecl>() { return NodeKind::DeductionGuideDecl; }
  template<> constexpr NodeKind Parser::nodeKindOf<AsmDecl>() { return NodeKind::AsmDecl; }
  template<> constexpr NodeKind Parser::nodeKindOf<FriendDecl>() { return NodeKind::FriendDecl; }
  template<> constexpr NodeKind Parser::nodeKindOf<ImportDecl>() { return NodeKind::ImportDecl; }
  template<> constexpr NodeKind Parser::nodeKindOf<AssertStmt>() { return NodeKind::AssertStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<CallerExpr>() { return NodeKind::CallerExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<NameofExpr>() { return NodeKind::NameofExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<BlockStmt>() { return NodeKind::BlockStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<ExprStmt>() { return NodeKind::ExprStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<DeclStmt>() { return NodeKind::DeclStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<IfStmt>() { return NodeKind::IfStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<ForStmt>() { return NodeKind::ForStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<RangeForStmt>() { return NodeKind::RangeForStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<ForConstexprStmt>() { return NodeKind::ForConstexprStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<WhileStmt>() { return NodeKind::WhileStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<DoStmt>() { return NodeKind::DoStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<SwitchStmt>() { return NodeKind::SwitchStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<CaseStmt>() { return NodeKind::CaseStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<DefaultStmt>() { return NodeKind::DefaultStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<ReturnStmt>() { return NodeKind::ReturnStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<BreakStmt>() { return NodeKind::BreakStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<ContinueStmt>() { return NodeKind::ContinueStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<GotoStmt>() { return NodeKind::GotoStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<LabelStmt>() { return NodeKind::LabelStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<TryStmt>() { return NodeKind::TryStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<EmptyStmt>() { return NodeKind::EmptyStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<ErrorStmt>() { return NodeKind::ErrorStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<FallthroughStmt>() { return NodeKind::FallthroughStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<AsmStmt>() { return NodeKind::AsmStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<MacroStmt>() { return NodeKind::MacroStmt; }
  template<> constexpr NodeKind Parser::nodeKindOf<NameExpr>() { return NodeKind::NameExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<LiteralExpr>() { return NodeKind::LiteralExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<ThisExpr>() { return NodeKind::ThisExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<CallExpr>() { return NodeKind::CallExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<IndexExpr>() { return NodeKind::IndexExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<MemberExpr>() { return NodeKind::MemberExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<UnaryExpr>() { return NodeKind::UnaryExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<BinaryExpr>() { return NodeKind::BinaryExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<ConditionalExpr>() { return NodeKind::ConditionalExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<CastExpr>() { return NodeKind::CastExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<NamedCastExpr>() { return NodeKind::NamedCastExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<SizeofExpr>() { return NodeKind::SizeofExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<NewExpr>() { return NodeKind::NewExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<DeleteExpr>() { return NodeKind::DeleteExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<LambdaExpr>() { return NodeKind::LambdaExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<BraceInitExpr>() { return NodeKind::BraceInitExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<ThrowExpr>() { return NodeKind::ThrowExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<FunctionalCastExpr>() { return NodeKind::FunctionalCastExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<TypeidExpr>() { return NodeKind::TypeidExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<FoldExpr>() { return NodeKind::FoldExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<RequiresExpr>() { return NodeKind::RequiresExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<ErrorExpr>() { return NodeKind::ErrorExpr; }
  template<> constexpr NodeKind Parser::nodeKindOf<ParenExpr>() { return NodeKind::ParenExpr; }
}

const char* nodeKindName(NodeKind kind)
{
  switch (kind)
  {
#define X(name) case NodeKind::name: return #name;
    KPP_NODE_KINDS(X)
#undef X
  }
  return "?";
}

namespace
{
  // Removes preprocessor directives from the token stream, keeping one branch of every
  // conditional: the first branch, except for `#if 0` where the else-branch is kept. This is a
  // stand-in for real configuration selection, good enough to parse a C++ code base as a corpus.
  void filterDirectives(std::vector<Token>& tokens, std::string_view text)
  {
    bool any = false;
    for (const Token& t : tokens)
      if (t.getKind() == TokenKind::Directive)
      {
        any = true;
        break;
      }
    if (!any)
      return;
    auto directiveName = [&](const Token& t, bool& isIfZero)
    {
      std::string_view s = text.substr(t.offset, t.length);
      size_t i = 1;
      while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
        ++i;
      size_t start = i;
      while (i < s.size() && ((s[i] >= 'a' && s[i] <= 'z') || s[i] == '_'))
        ++i;
      std::string_view name = s.substr(start, i - start);
      isIfZero = false;
      if (name == "if")
      {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
          ++i;
        isIfZero = i < s.size() && s[i] == '0' && (i + 1 == s.size() || !(s[i + 1] >= '0' && s[i + 1] <= '9'));
      }
      return name;
    };
    std::vector<Token> result;
    result.reserve(tokens.size());
    // Skips tokens until the matching #else/#elif/#endif (stopAtElse) or #endif of the current level.
    auto skipBranch = [&](size_t& i, bool stopAtElse) -> std::string_view
    {
      int level = 0;
      for (; i < tokens.size(); ++i)
      {
        const Token& t = tokens[i];
        if (t.getKind() != TokenKind::Directive)
          continue;
        bool zero;
        std::string_view name = directiveName(t, zero);
        if (name == "if" || name == "ifdef" || name == "ifndef")
          ++level;
        else if (name == "endif")
        {
          if (level == 0)
            return name;
          --level;
        }
        else if (level == 0 && stopAtElse && (name == "else" || name == "elif"))
          return name;
      }
      return {};
    };
    for (size_t i = 0; i < tokens.size(); ++i)
    {
      const Token& t = tokens[i];
      if (t.getKind() != TokenKind::Directive)
      {
        result.push_back(t);
        continue;
      }
      bool zero;
      std::string_view name = directiveName(t, zero);
      if (zero)
      {
        // #if 0: skip to the else branch and take it; a following #elif is treated as taken.
        ++i;
        skipBranch(i, true);
      }
      else if (name == "else" || name == "elif")
      {
        // The first branch was taken; skip the rest up to #endif.
        ++i;
        skipBranch(i, false);
      }
    }
    tokens.swap(result);
  }
}

void parseFile(std::vector<Token>& tokens, std::string_view text, SymbolTable& symbols, ParsedFile& out, bool cMode,
               const ConfigValues* config)
{
  filterDirectives(tokens, text);
  {
    // Calling conventions and pointer-size modifiers mean nothing here (one convention on x64),
    // and they appear in positions the declarator grammar does not expect, as in
    // `int (__cdecl* f)(void)` in C headers or `BOOL(__stdcall*)(void)` in converted sources.
    auto isNoise = [&](const Token& t)
    {
      if (TokenKind(t.kind) != TokenKind::Identifier)
        return false;
      std::string_view s(text.data() + t.offset, t.length);
      return s == "__cdecl" || s == "__stdcall" || s == "__fastcall" || s == "__vectorcall" || s == "__thiscall" ||
             s == "__clrcall" || s == "__ptr64" || s == "__ptr32" || s == "__sptr" || s == "__uptr" || s == "__w64" ||
             s == "__unaligned";
    };
    tokens.erase(std::remove_if(tokens.begin(), tokens.end(), isNoise), tokens.end());
  }
  if (cMode)
  {
    // C headers in the GNU dialect (MinGW-w64's, windows-gnu): `__extension__` before a declaration,
    // `__restrict__` (MSVC's `__restrict`), and `__attribute__((...))` where MSVC's put a calling convention, between a declaration's type
    // and its name and inside a declarator (`typedef int (__attribute__((__cdecl__)) *f)(int)`).
    // Both go, except the attributes that say something about a type (vector_size, aligned, packed,
    // mode), which stay where the declaration grammar takes them. So do what says nothing about a
    // declaration's type or layout wherever it stands: C99's `restrict` (`pthread_mutex_t * restrict
    // __mutex`), C11's `_Noreturn`, and clang's nullability qualifiers (FreeBSD's headers:
    // `int (* _Nullable _close)(void *)` in stdio.h's FILE, `const sigset_t * _Nonnull restrict`).
    auto spelled = [&](const Token& t) { return std::string_view(text.data() + t.offset, t.length); };
    auto identifier = [&](const Token& t, std::string_view s) { return TokenKind(t.kind) == TokenKind::Identifier && spelled(t) == s; };
    auto meaningless = [&](const Token& t)
    {
      if (TokenKind(t.kind) != TokenKind::Identifier)
        return false;
      std::string_view s = spelled(t);
      return s == "__extension__" || s == "__restrict__" || s == "restrict" || s == "_Noreturn" || s == "_Nonnull" || s == "_Nullable" ||
             s == "_Nullable_result" || s == "_Null_unspecified";
    };
    std::vector<Token> kept;
    kept.reserve(tokens.size());
    for (size_t i = 0; i < tokens.size(); ++i)
    {
      if (meaningless(tokens[i]))
        continue;
      if (identifier(tokens[i], "__attribute__") && i + 2 < tokens.size() && spelled(tokens[i + 1]) == "(" && spelled(tokens[i + 2]) == "(")
      {
        size_t end = i + 1;
        bool typeAttribute = false;
        for (int depth = 0; end < tokens.size(); ++end)
        {
          std::string_view s = spelled(tokens[end]);
          if (s == "(")
            ++depth;
          else if (s == ")" && --depth == 0)
            break;
          else if (TokenKind(tokens[end].kind) == TokenKind::Identifier)
          {
            std::string_view a = s;
            if (a.size() > 4 && a.starts_with("__") && a.ends_with("__"))
              a = a.substr(2, a.size() - 4);
            if (a == "vector_size" || a == "aligned" || a == "align" || a == "packed" || a == "mode")
              typeAttribute = true;
          }
        }
        if (!typeAttribute && end < tokens.size())
        {
          i = end; // past its closing ')'
          continue;
        }
      }
      kept.push_back(tokens[i]);
    }
    tokens = std::move(kept);
  }
  if (!cMode && out.path.ends_with(".kpp"))
    dropConfigDeclarations(tokens, text); // read before parsing (config.cpp)
  out.tokens = &tokens;
  out.text = text;
  out.arena.reserve(tokens.size() * 40 + 4096); // Measured: about 30 bytes of AST per token.
  Parser parser(tokens, text, symbols, out);
  parser.externC = cMode;
  parser.cMode = cMode;
  parser.kppSource = !cMode && out.path.ends_with(".kpp"); // (the driver sets the path; the converter parses C++ without one)
  parser.config = config;
  parser.parseTranslationUnit();
}
