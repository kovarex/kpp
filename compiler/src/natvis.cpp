// The .natvis of a program (natvis.hpp): fixed rules for the library's containers, each checked
// against the members it reads, and one rule per HashTable instance, whose slots are cast to its
// entry type by name. Type names are kpp's (TypeTable::toString, qualified), which the PDB's are
// (docs/debugging.md, "Values in Visual Studio": agreed with the code generator).
#include "natvis.hpp"
#include "codegen.hpp"
#include "resolver.hpp"
#include "TypeTable.hpp"
#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>

namespace
{
  std::string escape(const std::string& text)
  {
    std::string out;
    for (char c : text)
      if (c == '<')
        out += "&lt;";
      else if (c == '>')
        out += "&gt;";
      else if (c == '&')
        out += "&amp;";
      else if (c == '"')
        out += "&quot;";
      else
        out += c;
    return out;
  }

  std::string nameOf(const QualifiedName& name, const SymbolTable& symbols)
  {
    return name.parts.empty() || name.parts[name.parts.size() - 1].symbol == UINT32_MAX ? std::string()
                                                                                          : std::string(symbols.name(name.parts[name.parts.size() - 1].symbol));
  }

  // A class's data members by name: its own, and an anonymous struct's or union's in it (C++ reaches
  // those by name, and LLVM's CodeView puts them in the class).
  void dataMembers(const ClassDecl* c, const SymbolTable& symbols, std::set<std::string>& out)
  {
    for (const Decl* m : c->members)
      if (m->kind == NodeKind::VariableDecl && !(m->flags & DF_Static))
        out.insert(nameOf(static_cast<const VariableDecl*>(m)->name, symbols));
      else if (m->kind == NodeKind::ClassDecl && static_cast<const ClassDecl*>(m)->name.parts.empty())
        dataMembers(static_cast<const ClassDecl*>(m), symbols, out);
  }

  // A rule for the library's classes: the classes it names and the members it reads of each, its
  // XML, and the members it reads of other classes (by name: a specialization of those is no
  // concern, the rule naming only the classes it reads through).
  struct Rule
  {
    std::vector<std::string> classes;
    std::vector<std::string> members;
    std::string xml;
    std::vector<std::pair<std::string, std::vector<std::string>>> reads = {};
  };

  // A declaration's text, its tokens joined by one space: what a rule that copies the library's
  // computation compares with.
  std::string tokenText(const Decl* d, const std::vector<Token>& tokens, std::string_view text)
  {
    std::string out;
    for (uint32_t i = d->token; i < d->endToken && i < tokens.size(); ++i)
    {
      if (!out.empty())
        out += ' ';
      out += text.substr(tokens[i].offset, tokens[i].length);
    }
    return out;
  }

  // The library's literal: largeMark's `0x80`, for the rule to compare with.
  bool literalOf(const ClassDecl* c, const std::string& member, const SymbolTable& symbols, const std::vector<Token>& tokens, std::string_view text,
                 std::string& value)
  {
    for (const Decl* m : c->members)
      if (m->kind == NodeKind::VariableDecl && nameOf(static_cast<const VariableDecl*>(m)->name, symbols) == member)
      {
        const Expr* init = static_cast<const VariableDecl*>(m)->init;
        if (!init || init->kind != NodeKind::LiteralExpr || init->token >= tokens.size())
          return false;
        const Token& t = tokens[init->token];
        if (t.getKind() != TokenKind::Number)
          return false;
        value = std::string(text.substr(t.offset, t.length));
        return true;
      }
    return false;
  }

  // A type's debugValue() (docs/debugging.md, "debugValue"; design.md 26): a const member function of
  // that name and no parameters, whose body the debugger's natvis expressions can say, translated per
  // class instance. Its body is `return EXPRESSION;`, after `if (CONDITION) return EXPRESSION;`s. An
  // expression reads the object's data members (`this.x`, chains of members and pointers), folds what
  // reads none to a constant under the instance's arguments (`This::ValuesPerOne`, an enumerator), and
  // keeps arithmetic, comparisons, `!`, `?:` and casts to a number type, each as natvis writes it. A
  // call of a const member function without arguments, the object's own (`this.toDouble()`, kovarex
  // 2026-11-12: "Why not use what we already have?") or a member's (`this.weight.debugValue()`), is its
  // body inlined on that object, held to the same rules; a constexpr local is its constant. Anything
  // else (another call, an operator function, a variable) is the build's error, with what it was: a
  // view kpp cannot write is never left raw in silence.
  struct ObjectValue;
  struct LambdaValue;

  // An inlined call's parameter bound to its argument: an expression (an object's path: `(*this)`, `speed`), an
  // object the code constructs, or a lambda.
  struct Bound
  {
    const Decl* param = nullptr;
    std::string expression;
    std::shared_ptr<ObjectValue> object;
    std::shared_ptr<LambdaValue> lambda;
  };

  // A value the code constructs (`ID(index)` in fromIndex): each data member's expression; a member it does not set
  // is unknown to the view.
  struct ObjectValue
  {
    const Ty* type = nullptr;
    std::vector<std::pair<std::string, std::string>> members;
  };

  // A lambda passed as an argument (getPrototype's to applyOrAbort): the lambda and where it was written, what its
  // `this` and its captures are there.
  struct LambdaValue
  {
    LambdaExpr* lambda = nullptr;
    ParsedFile* file = nullptr;
    Decl* context = nullptr;
    TemplateArgs args;
    std::string prefix;
    const Ty* self = nullptr;
    std::vector<Bound> bound;
    std::shared_ptr<ObjectValue> object;
  };

  struct DebugValue
  {
    GlobalScope& global;
    const SymbolTable& symbols;
    ParsedFile* file;
    Decl* context;
    const TemplateArgs& args;
    std::string prefix = {}; // the member the object is, for an inlined call: `speed.`
    int depth = 0;
    const Ty* self = nullptr; // the object's class (the instance)
    std::string why = {};      // the first thing that could not be said
    Node* at = nullptr;        // where
    const ParsedFile* atFile = nullptr;
    // An inlined call's parameters, bound to its arguments.
    std::vector<Bound> bound = {};
    // The object an inlined call runs on when the code constructed it (getRaw<T>()'s ID): its members' values.
    std::shared_ptr<ObjectValue> object = {};
    // The asserts the body and the functions it calls make: each a condition and what the rule shows where it
    // holds, `assert failed: TEXT` where an assert's condition does not hold (kovarex, 2026-11-13: "The assert is
    // correct here"), `reaches abort()` where a function ends in a [[noreturn]] call. Shared by every inlined call
    // of one rule.
    std::shared_ptr<std::vector<std::pair<std::string, std::string>>> asserts = {};
    // The [[noreturn]] call the cases of the body last translated end in, when they do (apply's abort()).
    Stmt* noReturn = nullptr;
    // What holds where the code being translated runs, each part of a condition written as canonicalAtom
    // does (`X == 2`, `!(X == 0)`): a switch on X takes the case they pick (SignalIDBase's getName inside
    // typeToStr's case for a type: the one prototype list, not a choice of eight).
    std::vector<std::string> known = {};
    // Where the code being translated runs: the conditions of the cases on its way (getPointer only where an ID's
    // getName has passed `index == 0`), which an assert's failure holds only together with. Empty: always.
    std::string reached = {};
  };

  // Whether s is enclosed by one pair of parentheses, the first closing at the end.
  bool enclosed(const std::string& s)
  {
    if (s.size() < 2 || s.front() != '(' || s.back() != ')')
      return false;
    int depth = 0;
    bool quoted = false;
    for (size_t i = 0; i < s.size(); ++i)
    {
      char c = s[i];
      if (quoted)
      {
        if (c == '\\')
          ++i;
        else if (c == '"')
          quoted = false;
        continue;
      }
      if (c == '"')
        quoted = true;
      else if (c == '(')
        ++depth;
      else if (c == ')' && --depth == 0)
        return i == s.size() - 1;
    }
    return false;
  }

  // s without the parentheses that enclose all of it.
  std::string bare(std::string s)
  {
    while (enclosed(s))
      s = s.substr(1, s.size() - 2);
    return s;
  }

  // s is one operand without parentheses: a name, a number, a member or an element of one (`a.b->c[i]`), a
  // string, or enclosed.
  bool primary(const std::string& s)
  {
    if (s.empty())
      return false;
    if (enclosed(s) || (s.size() >= 2 && s.front() == '"' && s.back() == '"' && s.find('"', 1) == s.size() - 1))
      return true;
    if (s[0] == '-' || s[0] == '.')
      return false;
    for (size_t i = 0; i < s.size(); ++i)
    {
      char c = s[i];
      if (c == '-' && i + 1 < s.size() && s[i + 1] == '>')
      {
        ++i;
        continue;
      }
      if (!(std::isalnum((unsigned char)c) || c == '_' || c == ':' || c == '.' || c == '<' || c == '>' || c == '[' || c == ']'))
        return false;
    }
    return true;
  }

  // s as an operand: in parentheses only where it needs them (natvis reads `a.b`, not `((a)).b`).
  std::string wrap(const std::string& s)
  {
    std::string b = bare(s);
    return primary(b) ? b : "(" + b + ")";
  }

  // The places of token in s outside parentheses, brackets and strings.
  std::vector<size_t> topLevel(const std::string& s, const std::string& token)
  {
    std::vector<size_t> at;
    int depth = 0;
    bool quoted = false;
    for (size_t i = 0; i < s.size(); ++i)
    {
      char c = s[i];
      if (quoted)
      {
        if (c == '\\')
          ++i;
        else if (c == '"')
          quoted = false;
        continue;
      }
      if (c == '"')
        quoted = true;
      else if (c == '(' || c == '[')
        ++depth;
      else if (c == ')' || c == ']')
        --depth;
      else if (depth == 0 && s.compare(i, token.size(), token) == 0)
        at.push_back(i);
    }
    return at;
  }

  // A condition's parts that all hold, `a && (b && c)` as a, b, c (one part where an `||` or a `?:` decides).
  void conditionAtoms(const std::string& condition, std::vector<std::string>& out)
  {
    std::string s = bare(condition);
    if (s.empty())
      return;
    std::vector<size_t> ands = topLevel(s, " && ");
    if (ands.empty() || !topLevel(s, " || ").empty() || !topLevel(s, " ? ").empty())
    {
      out.push_back(s);
      return;
    }
    size_t from = 0;
    for (size_t at : ands)
    {
      conditionAtoms(s.substr(from, at - from), out);
      from = at + 4;
    }
    conditionAtoms(s.substr(from), out);
  }

  // One part of a condition, written the same way wherever it comes from: `X == 1` with X bare, `!(...)` of one.
  std::string canonicalAtom(const std::string& atom)
  {
    std::string s = bare(atom);
    if (s.size() > 3 && s[0] == '!' && enclosed(s.substr(1)))
      return "!(" + canonicalAtom(s.substr(1)) + ")";
    std::vector<size_t> eq = topLevel(s, " == ");
    if (eq.size() == 1 && topLevel(s, " && ").empty() && topLevel(s, " || ").empty() && topLevel(s, " ? ").empty())
      return bare(s.substr(0, eq[0])) + " == " + bare(s.substr(eq[0] + 4));
    return s;
  }

  // `X == N` with N an integer: X and N.
  bool equalsNumber(const std::string& atom, std::string& subject, std::string& number)
  {
    std::vector<size_t> eq = topLevel(atom, " == ");
    if (eq.size() != 1 || !topLevel(atom, " && ").empty() || !topLevel(atom, " || ").empty() || !topLevel(atom, " ? ").empty())
      return false;
    number = bare(atom.substr(eq[0] + 4));
    if (number.empty() || !std::all_of(number.begin() + (number[0] == '-' ? 1 : 0), number.end(), [](char c) { return std::isdigit((unsigned char)c); }))
      return false;
    subject = bare(atom.substr(0, eq[0]));
    return true;
  }

  // A condition with its parts once each, leaving out `!(X == M)` beside `X == N` (a switch's earlier cases
  // beside its own: `type == 6` says it is no other type), its parentheses only where they are needed.
  std::string simplifyCondition(const std::string& condition)
  {
    std::vector<std::string> atoms, kept;
    conditionAtoms(condition, atoms);
    for (const std::string& atom : atoms)
    {
      std::string a = canonicalAtom(atom);
      std::vector<size_t> eq = a.size() > 3 && a[0] == '!' ? topLevel(bare(a.substr(1)), " == ") : std::vector<size_t>();
      if (eq.size() == 1)
      {
        std::string inner = bare(a.substr(1));
        if (bare(inner.substr(0, eq[0])) == bare(inner.substr(eq[0] + 4)))
          return "false"; // (`!(X == X)`: it never holds)
      }
    }
    std::vector<std::pair<std::string, std::string>> equal;
    for (std::string& atom : atoms)
    {
      atom = canonicalAtom(atom);
      std::string subject, number;
      if (equalsNumber(atom, subject, number))
        equal.push_back({subject, number});
    }
    for (const std::string& atom : atoms)
    {
      if (std::find(kept.begin(), kept.end(), atom) != kept.end())
        continue;
      std::string subject, number;
      if (atom.size() > 3 && atom[0] == '!' && equalsNumber(bare(atom.substr(1)), subject, number) &&
          std::any_of(equal.begin(), equal.end(), [&](const auto& e) { return e.first == subject && e.second != number; }))
        continue;
      kept.push_back(atom);
    }
    std::string out;
    for (const std::string& atom : kept)
      out += (out.empty() ? "" : " && ") + (topLevel(atom, " || ").empty() && topLevel(atom, " ? ").empty() ? atom : "(" + atom + ")");
    return out;
  }

  // Facts that hold where a case is taken: its condition's parts, and that each earlier case's does not
  // (`X == 2` and `!(X == 0)`, `!(X == 1)`): a switch on X inside takes its one case (design.md 26).
  std::vector<std::string> caseFacts(const std::vector<std::string>& known, const std::vector<std::string>& whens, const std::vector<bool>& conditional, size_t i)
  {
    std::vector<std::string> facts = known;
    for (size_t k = 0; k < i; ++k)
      if (conditional[k])
      {
        std::string s = bare(whens[k]);
        std::vector<size_t> ors = topLevel(s, " || ");
        if (!topLevel(s, " && ").empty() || !topLevel(s, " ? ").empty())
          continue;
        size_t from = 0;
        ors.push_back(s.size());
        for (size_t at : ors)
        {
          facts.push_back(canonicalAtom("!(" + s.substr(from, at - from) + ")"));
          from = at + 4;
        }
      }
    if (i < whens.size() && conditional[i])
    {
      std::vector<std::string> atoms;
      conditionAtoms(whens[i], atoms);
      for (const std::string& atom : atoms)
        facts.push_back(canonicalAtom(atom));
    }
    return facts;
  }

  // Both conditions, either of them empty for none.
  std::string reachedAnd(const std::string& a, const std::string& b)
  {
    if (a.empty() || a == b)
      return b;
    if (b.empty() || a.find("(" + b + ")") != std::string::npos)
      return a; // (a condition already on the way: an ID's `index == 0` in its debugValue and its getName)
    return "(" + a + ") && (" + b + ")";
  }

  const Bound* boundOf(const DebugValue& v, const Decl* d)
  {
    for (const Bound& b : v.bound)
      if (b.param == d)
        return &b;
    return nullptr;
  }

  // Where each of a function's cases is taken, its conditions given: its own and none of the earlier ones.
  std::vector<std::string> caseReaches(const std::string& reached, const std::vector<std::string>& whens, const std::vector<bool>& conditional)
  {
    std::vector<std::string> out;
    std::string notBefore;
    for (size_t i = 0; i < whens.size(); ++i)
    {
      out.push_back(reachedAnd(reached, reachedAnd(notBefore, conditional[i] ? whens[i] : std::string())));
      if (conditional[i])
        notBefore = reachedAnd(notBefore, "!(" + whens[i] + ")");
    }
    return out;
  }

  bool fail(DebugValue& v, Node* at, std::string why)
  {
    if (v.why.empty())
    {
      v.why = std::move(why);
      v.at = at;
      v.atFile = v.file;
    }
    return false;
  }

  // A number type as natvis's C casts name it; empty for another.
  std::string natvisNumberType(const Ty* t)
  {
    if (!t)
      return {};
    switch (t->kind)
    {
    case TyKind::Bool: return "bool";
    case TyKind::Float: return t->bits == 32 ? "float" : t->bits == 64 ? "double" : "";
    case TyKind::Char: case TyKind::Int:
      switch (t->bits)
      {
      case 8: return t->isUnsigned ? "unsigned char" : "char";
      case 16: return t->isUnsigned ? "unsigned short" : "short";
      case 32: return t->isUnsigned ? "unsigned int" : "int";
      case 64: return t->isUnsigned ? "unsigned __int64" : "__int64";
      default: return {};
      }
    default: return {};
    }
  }

  // A cast's type as natvis writes it: a number type's C name, an enumeration's or a class pointer's debugger
  // name (Direction::Enum, Entity*). reference: a reference to a class, cast as a pointer and read through.
  std::string natvisCastType(GlobalScope& global, const SymbolTable& symbols, const Ty* t, bool& reference)
  {
    reference = false;
    if (!t)
      return {};
    TypeTable& types = global.types();
    if (t->isReference())
    {
      const Ty* object = types.unqualified(types.stripRef(t));
      if (object && object->kind == TyKind::Class)
      {
        reference = true;
        return types.toString(object, &symbols, true) + "*";
      }
      return {};
    }
    t = types.unqualified(t);
    if (std::string number = natvisNumberType(t); !number.empty())
      return number;
    if (t->kind == TyKind::Enum)
      return types.toString(t, &symbols, true);
    if (t->kind == TyKind::Pointer && t->inner)
    {
      const Ty* object = types.unqualified(t->inner);
      if (object->kind == TyKind::Class || object->kind == TyKind::Char || natvisNumberType(object) != "")
        return (object->kind == TyKind::Class ? types.toString(object, &symbols, true) : natvisNumberType(object).empty() ? std::string("char") : natvisNumberType(object)) + "*";
    }
    return {};
  }

  bool isDebugValue(const Decl* d, const SymbolTable& symbols)
  {
    if (!d || d->kind != NodeKind::FunctionDecl)
      return false;
    const QualifiedName& name = static_cast<const FunctionDecl*>(d)->name;
    return !name.parts.empty() && name.last().symbol != UINT32_MAX && symbols.name(name.last().symbol) == "debugValue";
  }

  // Whether e reads the object (a data member, `this`): what does not is folded to a constant.
  bool readsObject(Node* n)
  {
    if (!n)
      return false;
    switch (n->kind)
    {
    case NodeKind::ThisExpr: return true;
    case NodeKind::NameExpr:
    {
      Decl* d = static_cast<NameExpr*>(n)->resolved;
      if (d && d->kind == NodeKind::VariableDecl && (d->flags & DF_Static) && !(d->flags & DF_Constexpr) && d->parent &&
          d->parent->kind == NodeKind::ClassDecl)
        return true; // (a static data member: a global, read where the debugger stops)
      if (d && d->kind == NodeKind::ParamDecl)
        return true; // (an inlined call's parameter: its argument)
      return d && d->kind == NodeKind::VariableDecl && !(d->flags & DF_Static) && d->parent && d->parent->kind == NodeKind::ClassDecl;
    }
    case NodeKind::IndexExpr:
    {
      auto* x = static_cast<IndexExpr*>(n);
      if (readsObject(x->base))
        return true;
      for (Expr* a : x->args)
        if (readsObject(a))
          return true;
      return false;
    }
    case NodeKind::MemberExpr: return readsObject(static_cast<MemberExpr*>(n)->base);
    case NodeKind::ParenExpr: return readsObject(static_cast<ParenExpr*>(n)->inner);
    case NodeKind::UnaryExpr: return readsObject(static_cast<UnaryExpr*>(n)->operand);
    case NodeKind::BinaryExpr: return readsObject(static_cast<BinaryExpr*>(n)->lhs) || readsObject(static_cast<BinaryExpr*>(n)->rhs);
    case NodeKind::ConditionalExpr:
    {
      auto* c = static_cast<ConditionalExpr*>(n);
      return readsObject(c->cond) || readsObject(c->then) || readsObject(c->else_);
    }
    case NodeKind::CastExpr: return readsObject(static_cast<CastExpr*>(n)->operand);
    case NodeKind::NamedCastExpr: return readsObject(static_cast<NamedCastExpr*>(n)->operand);
    case NodeKind::FunctionalCastExpr:
      for (Expr* a : static_cast<FunctionalCastExpr*>(n)->args)
        if (readsObject(a))
          return true;
      return false;
    case NodeKind::BraceInitExpr:
      for (Expr* a : static_cast<BraceInitExpr*>(n)->args)
        if (readsObject(a))
          return true;
      return false;
    case NodeKind::CallExpr:
      for (Expr* a : static_cast<CallExpr*>(n)->args)
        if (readsObject(a))
          return true;
      return readsObject(static_cast<CallExpr*>(n)->callee);
    default: return false;
    }
  }

  // A constant as a natvis literal: an integer as itself, a floating one with its digits (a dot
  // or an exponent, so that natvis computes in floating point).
  std::string natvisConstant(const ConstValue& c)
  {
    if (c.kind == ConstValue::Kind::Float)
    {
      char buffer[64];
      snprintf(buffer, sizeof(buffer), "%.17g", c.f);
      std::string text = buffer;
      if (text.find_first_of(".einn") == std::string::npos)
        text += ".0";
      return text;
    }
    return std::to_string(c.i);
  }

  // The object itself, as a member's base: `this.x` (C++'s `this->x`, the parser's implicit `*this`).
  bool isThisObject(Expr* e)
  {
    while (e && e->kind == NodeKind::ParenExpr)
      e = static_cast<ParenExpr*>(e)->inner;
    if (!e)
      return false;
    if (e->kind == NodeKind::ThisExpr)
      return true;
    return e->kind == NodeKind::UnaryExpr && static_cast<UnaryExpr*>(e)->implicitThis;
  }

  // A case of a debugValue's rule: its condition (an expression, or a switch's text already made; neither
  // for the last) and the value it returns.
  struct DebugCase
  {
    Expr* first = nullptr;
    Expr* second = nullptr;
    std::string text = {};
    bool conditional() const { return this->first || !this->text.empty(); }
  };

  bool natvisExpression(DebugValue& v, Expr* e, std::string& out);
  bool natvisObject(DebugValue& v, Expr* e, std::shared_ptr<ObjectValue>& out);
  bool lambdaCall(DebugValue& v, CallExpr* call, const LambdaValue& lambda, std::string& out);
  std::string objectPath(const DebugValue& v);
  std::string noValue(DebugValue& v, const Ty* result);
  bool noReturnEnd(DebugValue& inner, Node* at, const std::vector<std::string>& whens, std::vector<bool>& conditional);
  std::string asResult(DebugValue& v, const Ty* result, Expr* value, const std::string& one);
  const Ty* calledClass(DebugValue& v, NameExpr* n, FunctionDecl* f);
  bool natvisCondition(DebugValue& v, Expr* e, std::string& out);

  // The body of a debugValue as natvis cases (DebugCase), the last one unconditional: `if (C) return E;`s,
  // a switch whose cases return (`case A: return E;`, `default:` last), a final `return E;`. False, with v's
  // why, for a body of another shape.
  bool debugCasesOf(DebugValue& v, Stmt* body, Node* at, std::vector<DebugCase>& cases, bool* endsNowhere);

  // The text of the tokens [start, end) as written.
  std::string sourceText(const DebugValue& v, uint32_t start, uint32_t end)
  {
    if (!v.file || !v.file->tokens || start >= end || end > v.file->tokens->size())
      return {};
    const std::vector<Token>& tokens = *v.file->tokens;
    const Token& last = tokens[end - 1];
    return std::string(v.file->text.substr(tokens[start].offset, last.offset + last.length - tokens[start].offset));
  }

  // A statement's text as written, up to its `;`.
  std::string statementText(const DebugValue& v, const Stmt* s)
  {
    if (!v.file || !v.file->tokens)
      return {};
    const std::vector<Token>& tokens = *v.file->tokens;
    int depth = 0;
    for (uint32_t i = s->token; i < tokens.size(); ++i)
    {
      const Token& t = tokens[i];
      depth += t.is(P::LParen) || t.is(P::LBrace) || t.is(P::LBracket);
      depth -= t.is(P::RParen) || t.is(P::RBrace) || t.is(P::RBracket);
      if (depth <= 0 && t.is(P::Semi))
        return sourceText(v, s->token, i);
    }
    return {};
  }

  bool debugCases(DebugValue& v, FunctionDecl* f, std::vector<DebugCase>& cases, bool* endsNowhere = nullptr)
  {
    if (!f || !f->body || f->body->kind != NodeKind::BlockStmt)
      return fail(v, f, "it has no body here");
    return debugCasesOf(v, f->body, f, cases, endsNowhere);
  }

  bool debugCasesOf(DebugValue& v, Stmt* body, Node* at, std::vector<DebugCase>& cases, bool* endsNowhere)
  {
    if (!body || body->kind != NodeKind::BlockStmt)
      return fail(v, at, "it has no body here");
    Node* f = at;
    bool noReturnEnd = false;
    // (the statements, an `if constexpr`'s taken branch spliced in where it stands)
    std::vector<Stmt*> todo(static_cast<BlockStmt*>(body)->stmts.begin(), static_cast<BlockStmt*>(body)->stmts.end());
    std::reverse(todo.begin(), todo.end());
    while (!todo.empty())
    {
      Stmt* s = todo.back();
      todo.pop_back();
      if ((!cases.empty() && !cases.back().conditional()) || noReturnEnd)
        return fail(v, s, "a statement after its last return");
      if (s->kind == NodeKind::IfStmt && static_cast<IfStmt*>(s)->isConstexpr && static_cast<IfStmt*>(s)->cond &&
          static_cast<IfStmt*>(s)->cond->kind != NodeKind::DeclStmt)
      {
        // `if constexpr`: the branch this instance takes (ID's getPrototype: RemovedPrototype or the list)
        auto* i = static_cast<IfStmt*>(s);
        ConstHooks hooks;
        std::optional<ConstValue> value = v.global.evaluate(static_cast<Expr*>(i->cond), v.file, v.context, v.args, hooks);
        GlobalScope::clearConstantReason();
        if (!value || value->kind != ConstValue::Kind::Int)
          return fail(v, s, "an `if constexpr` whose condition is no constant here");
        Stmt* taken = value->i ? i->then : i->else_;
        if (taken && taken->kind == NodeKind::BlockStmt)
          for (size_t k = static_cast<BlockStmt*>(taken)->stmts.size(); k-- > 0;)
            todo.push_back(static_cast<BlockStmt*>(taken)->stmts[k]);
        else if (taken)
          todo.push_back(taken);
        continue;
      }
      // A parameter given a new value (`index -= this._capacity;`, CompactDeque's wrap), maybe under an `if` without
      // an else: its value from here on, `(CONDITION ? NEW : OLD)`.
      {
        Stmt* assigning = s;
        Expr* when = nullptr;
        if (s->kind == NodeKind::IfStmt && !static_cast<IfStmt*>(s)->init && !static_cast<IfStmt*>(s)->isConstexpr && !static_cast<IfStmt*>(s)->ifConsteval &&
            !static_cast<IfStmt*>(s)->else_ && static_cast<IfStmt*>(s)->cond && static_cast<IfStmt*>(s)->cond->kind != NodeKind::DeclStmt)
        {
          assigning = static_cast<IfStmt*>(s)->then;
          while (assigning && assigning->kind == NodeKind::BlockStmt && static_cast<BlockStmt*>(assigning)->stmts.size() == 1)
            assigning = static_cast<BlockStmt*>(assigning)->stmts[0];
          when = static_cast<Expr*>(static_cast<IfStmt*>(s)->cond);
        }
        Expr* e = assigning && assigning->kind == NodeKind::ExprStmt ? static_cast<ExprStmt*>(assigning)->expr : nullptr;
        Expr* target = nullptr;
        Expr* operand = nullptr;
        P op = P(0);
        if (e && e->kind == NodeKind::BinaryExpr && !static_cast<BinaryExpr*>(e)->callee)
        {
          op = P(static_cast<BinaryExpr*>(e)->op);
          target = static_cast<BinaryExpr*>(e)->lhs;
          operand = static_cast<BinaryExpr*>(e)->rhs;
        }
        else if (e && e->kind == NodeKind::UnaryExpr && !static_cast<UnaryExpr*>(e)->callee)
        {
          op = P(static_cast<UnaryExpr*>(e)->op);
          target = static_cast<UnaryExpr*>(e)->operand;
        }
        while (target && target->kind == NodeKind::ParenExpr)
          target = static_cast<ParenExpr*>(target)->inner;
        Bound* parameter = nullptr;
        if (target && target->kind == NodeKind::NameExpr && static_cast<NameExpr*>(target)->resolved &&
            static_cast<NameExpr*>(target)->resolved->kind == NodeKind::ParamDecl)
          for (Bound& b : v.bound)
            if (b.param == static_cast<NameExpr*>(target)->resolved && !b.object && !b.lambda)
              parameter = &b;
        bool arithmetic = op == P::Assign || op == P::PlusAssign || op == P::MinusAssign || op == P::StarAssign || op == P::PlusPlus || op == P::MinusMinus;
        if (parameter && arithmetic)
        {
          std::string old = wrap(parameter->expression), value, condition;
          if (operand && !natvisExpression(v, operand, value))
            return false;
          std::string made = op == P::Assign ? wrap(value)
                             : op == P::PlusAssign ? old + " + " + wrap(value)
                             : op == P::MinusAssign ? old + " - " + wrap(value)
                             : op == P::StarAssign ? old + " * " + wrap(value)
                             : op == P::PlusPlus ? old + " + 1"
                                                 : old + " - 1";
          if (when && !natvisCondition(v, when, condition))
            return false;
          parameter->expression = when ? "(" + condition + " ? " + made + " : " + old + ")" : made;
          continue;
        }
      }
      if (s->kind == NodeKind::AssertStmt && !static_cast<AssertStmt*>(s)->testKind)
      {
        // An assert: the rule shows `assert failed: TEXT` where it does not hold, before its cases (an assert
        // after an `if` would hold only on its way: not said yet).
        auto* a = static_cast<AssertStmt*>(s);
        if (!a->release && !v.global.isDebug())
          continue; // (config::debug false: the assert is nothing, its expression discarded code, design.md 7)
        if (!cases.empty())
          return fail(v, s, "an assert after an `if` or a switch");
        if (!v.asserts)
          return fail(v, s, "an assert here");
        std::string condition;
        if (!natvisCondition(v, a->cond, condition))
          return false;
        v.asserts->push_back({reachedAnd(v.reached, "!(" + condition + ")"), "assert failed: " + sourceText(v, a->startToken, a->endToken)});
        continue;
      }
      // A call of a [[noreturn]] function at the end (Comparison::str()'s logEnumAndAbortOrThrow after its
      // switch): no value there; the rule's cases before it are all it shows.
      if (s->kind == NodeKind::ExprStmt && static_cast<ExprStmt*>(s)->expr && static_cast<ExprStmt*>(s)->expr->kind == NodeKind::CallExpr)
      {
        auto* call = static_cast<CallExpr*>(static_cast<ExprStmt*>(s)->expr);
        Decl* callee = call->callee && call->callee->kind == NodeKind::NameExpr ? static_cast<NameExpr*>(call->callee)->resolved
                       : call->callee && call->callee->kind == NodeKind::MemberExpr ? static_cast<MemberExpr*>(call->callee)->resolved
                                                                                    : nullptr;
        if (callee && callee->kind == NodeKind::FunctionDecl && (callee->attributes & ATTR_NoReturn))
        {
          noReturnEnd = true;
          v.noReturn = s;
          continue;
        }
      }
      auto returned = [&](Stmt* r) -> Expr*
      {
        while (r && r->kind == NodeKind::BlockStmt && static_cast<BlockStmt*>(r)->stmts.size() == 1)
          r = static_cast<BlockStmt*>(r)->stmts[0];
        return r && r->kind == NodeKind::ReturnStmt ? static_cast<ReturnStmt*>(r)->value : nullptr;
      };
      if (s->kind == NodeKind::ReturnStmt && static_cast<ReturnStmt*>(s)->value)
        cases.push_back({nullptr, static_cast<ReturnStmt*>(s)->value});
      else if (s->kind == NodeKind::SwitchStmt)
      {
        // switch (X) { case A: case B: return E; ... default: return D; }: a case `(X == A || X == B)` per
        // return, the default's last (a switch without one goes on to the statements after it).
        auto* sw = static_cast<SwitchStmt*>(s);
        std::string subject;
        if (sw->init || !sw->cond || sw->cond->kind == NodeKind::DeclStmt || !sw->body || sw->body->kind != NodeKind::BlockStmt)
          return fail(v, s, "a switch other than `switch (VALUE) { case A: return EXPRESSION; ... }`");
        if (!natvisExpression(v, static_cast<Expr*>(sw->cond), subject))
          return false;
        std::vector<std::string> labels;
        bool isDefault = false;
        Expr* fallback = nullptr;
        size_t switchFirst = cases.size();
        std::vector<std::vector<std::string>> switchLabels; // (each case's)
        for (Stmt* inner : static_cast<BlockStmt*>(sw->body)->stmts)
        {
          if (inner->kind == NodeKind::CaseStmt)
          {
            auto* c = static_cast<CaseStmt*>(inner);
            if (c->rangeEnd)
              return fail(v, inner, "a case range");
            std::string label;
            if (!natvisExpression(v, c->value, label))
              return false;
            labels.push_back(label);
            for (Expr* more : c->moreValues)
            {
              if (!natvisExpression(v, more, label))
                return false;
              labels.push_back(label);
            }
            continue;
          }
          if (inner->kind == NodeKind::DefaultStmt)
          {
            isDefault = true;
            continue;
          }
          Expr* value = returned(inner);
          if (!value || (labels.empty() && !isDefault))
            return fail(v, inner, "a switch's statement other than `return EXPRESSION;` after its case labels");
          if (isDefault)
            fallback = value; // (last, whatever the labels beside it)
          else
          {
            std::string condition;
            for (const std::string& label : labels)
              condition += (condition.empty() ? "" : " || ") + subject + " == " + label;
            cases.push_back({nullptr, value, labels.size() > 1 ? "(" + condition + ")" : condition});
            switchLabels.push_back(labels);
          }
          labels.clear();
          isDefault = false;
        }
        if (!labels.empty() || isDefault)
          return fail(v, s, "a case label without its `return EXPRESSION;`");
        if (fallback)
          cases.push_back({nullptr, fallback});
        if (!v.known.empty())
        {
          // What is known picks the case (`X == 2`: that case alone) or leaves cases out (`!(X == 0)`).
          auto isKnown = [&](const std::string& atom)
          {
            std::string fact = canonicalAtom(atom), subject, number, knownSubject, knownNumber;
            if (std::find(v.known.begin(), v.known.end(), fact) != v.known.end())
              return true;
            // (`!(X == 3)` where `X == 4` is known)
            if (fact.size() > 3 && fact[0] == '!' && equalsNumber(bare(fact.substr(1)), subject, number))
              for (const std::string& known : v.known)
                if (equalsNumber(known, knownSubject, knownNumber) && knownSubject == subject && knownNumber != number)
                  return true;
            return false;
          };
          std::vector<DebugCase> kept(cases.begin(), cases.begin() + switchFirst);
          bool decided = false;
          for (size_t i = switchFirst; i < cases.size() && !decided; ++i)
          {
            if (!cases[i].conditional())
            {
              kept.push_back(cases[i]);
              decided = true;
              break;
            }
            std::string labelsLeft;
            for (const std::string& label : switchLabels[i - switchFirst])
            {
              std::string equal = subject + " == " + label;
              if (isKnown(equal))
              {
                kept.push_back({nullptr, cases[i].second});
                decided = true;
                break;
              }
              if (!isKnown("!(" + equal + ")"))
                labelsLeft += (labelsLeft.empty() ? "" : " || ") + equal;
            }
            if (!decided && !labelsLeft.empty())
              kept.push_back({nullptr, cases[i].second, topLevel(labelsLeft, " || ").empty() ? labelsLeft : "(" + labelsLeft + ")"});
          }
          cases = std::move(kept);
          if (decided)
          {
            todo.clear(); // (the statements after it are not reached)
            continue;
          }
        }
      }
      else if (s->kind == NodeKind::DeclStmt && !static_cast<DeclStmt*>(s)->decls.empty() &&
               std::all_of(static_cast<DeclStmt*>(s)->decls.begin(), static_cast<DeclStmt*>(s)->decls.end(),
                           [](Decl* d) { return d->kind == NodeKind::VariableDecl && (d->flags & DF_Constexpr); }))
        continue; // (a constexpr local: its uses fold to its value)
      else if (s->kind == NodeKind::IfStmt)
      {
        auto* i = static_cast<IfStmt*>(s);
        Expr* then = returned(i->then);
        if (i->init || i->isConstexpr || i->ifConsteval || !i->cond || i->cond->kind == NodeKind::DeclStmt || !then)
          return fail(v, s, "an `if` other than `if (CONDITION) return EXPRESSION;`");
        cases.push_back({static_cast<Expr*>(i->cond), then});
        if (i->else_)
        {
          Expr* otherwise = returned(i->else_);
          if (!otherwise)
            return fail(v, i->else_, "an `else` other than `else return EXPRESSION;`");
          cases.push_back({nullptr, otherwise});
        }
      }
      else
        return fail(v, s, "a statement other than `if (CONDITION) return EXPRESSION;` and `return EXPRESSION;`");
    }
    if (cases.empty() && noReturnEnd && endsNowhere)
    {
      *endsNowhere = true; // (every case ruled out by what is known: the [[noreturn]] end alone)
      return true;
    }
    if (cases.empty() || (cases.back().conditional() && !(noReturnEnd && endsNowhere)))
      return fail(v, f, "no `return EXPRESSION;` at its end");
    if (endsNowhere)
      *endsNowhere = cases.back().conditional();
    return true;
  }

  bool natvisText(DebugValue& v, Expr* e, std::string& out);
  bool natvisCondition(DebugValue& v, Expr* e, std::string& out);
  bool literalText(const ParsedFile* f, const LiteralExpr* e, std::string& out);
  bool isLibraryClass(const Ty* t, const SymbolTable& symbols, const char* name);

  // A call to inline: its function's instance (the object's class's template arguments, a function template's
  // from the call), its parameters bound to the call's arguments (an object argument to the object's path,
  // `(*this)` for the viewed object), and the object it runs on (none for a static or free function).
  struct InlinedCall
  {
    TemplateArgs args;
    FunctionDecl* def = nullptr;
    FunctionDecl* instance = nullptr;
    const Ty* self = nullptr;
    std::string prefix;
    std::vector<Bound> bound;
    std::shared_ptr<ObjectValue> object;
  };

  // A value the code makes rather than reads: a class constructed (`Handle(index)`, `ItemID()`), or a call returning one.
  bool isMadeObject(Expr* e)
  {
    while (e && e->kind == NodeKind::ParenExpr)
      e = static_cast<ParenExpr*>(e)->inner;
    return e && (e->kind == NodeKind::FunctionalCastExpr || e->kind == NodeKind::BraceInitExpr || e->kind == NodeKind::CallExpr);
  }

  // An argument bound to its parameter: a lambda, an object the code makes, the viewed object, or an expression.
  bool bindArgument(DebugValue& v, Expr* argument, const Decl* param, std::vector<Bound>& out)
  {
    TypeTable& types = v.global.types();
    Expr* a = argument;
    while (a && a->kind == NodeKind::ParenExpr)
      a = static_cast<ParenExpr*>(a)->inner;
    Bound b;
    b.param = param;
    const Ty* type = a && a->ty ? types.unqualified(types.stripRef(a->ty)) : nullptr;
    if (a && a->kind == NodeKind::LambdaExpr)
    {
      auto l = std::make_shared<LambdaValue>();
      l->lambda = static_cast<LambdaExpr*>(a);
      l->file = v.file;
      l->context = v.context;
      l->args = v.args;
      l->prefix = v.prefix;
      l->self = v.self;
      l->bound = v.bound;
      l->object = v.object;
      b.lambda = l;
    }
    else if (a && a->kind == NodeKind::NameExpr && static_cast<NameExpr*>(a)->resolved && static_cast<NameExpr*>(a)->resolved->kind == NodeKind::ParamDecl &&
             boundOf(v, static_cast<NameExpr*>(a)->resolved) &&
             (boundOf(v, static_cast<NameExpr*>(a)->resolved)->object || boundOf(v, static_cast<NameExpr*>(a)->resolved)->lambda))
    {
      const Bound* passed = boundOf(v, static_cast<NameExpr*>(a)->resolved); // (applyOrAbort passing its op on to apply)
      b.object = passed->object;
      b.lambda = passed->lambda;
    }
    else if (type && type->kind == TyKind::Class && isThisObject(a))
    {
      if (v.object)
        b.object = v.object;
      else
        b.expression = objectPath(v); // (`getPointer(this)`: the viewed object)
    }
    else if (type && type->kind == TyKind::Class && isMadeObject(a))
    {
      if (!natvisObject(v, a, b.object))
        return false;
    }
    else if (!natvisExpression(v, argument, b.expression))
      return false;
    out.push_back(std::move(b));
    return true;
  }

  // A member of an object the code constructed: its value, as the construction set it.
  bool memberOf(DebugValue& v, Node* at, const std::string& name, std::string& out, const ObjectValue* object = nullptr)
  {
    for (const auto& [member, value] : (object ? object : v.object.get())->members)
      if (member == name)
      {
        out = wrap(value);
        return true;
      }
    return fail(v, at, "the member " + name + " of a value the code makes, which its construction does not set");
  }


  // A function's name in a message: an operator's `operator[]` (its symbol is none).
  std::string functionName(const SymbolTable& symbols, const FunctionDecl* f)
  {
    if (!f || f->name.parts.empty())
      return "a function";
    const auto& last = f->name.last();
    if (last.special == 2)
      return "operator" + std::string(P(last.opToken) == P::LBracket ? "[]" : punctuatorSpelling(P(last.opToken)));
    if (last.special != 0)
      return "a special member function";
    return std::string(symbols.name(last.symbol));
  }

  // A function among a class's members: its name's or operator's, the one the test takes (none, or more than one: null).
  FunctionDecl* memberFunction(GlobalScope& global, const SymbolTable& symbols, const Ty* cls, const char* name, P op, const std::function<bool(FunctionDecl*)>& takes)
  {
    if (!cls || cls->kind != TyKind::Class || !cls->decl)
      return nullptr;
    Decl* defined = global.definitionOf(cls->decl);
    if (!defined || defined->kind != NodeKind::ClassDecl)
      return nullptr;
    FunctionDecl* found = nullptr;
    for (Decl* m : static_cast<ClassDecl*>(defined)->members)
    {
      if (m->kind != NodeKind::FunctionDecl || static_cast<FunctionDecl*>(m)->name.parts.empty())
        continue;
      auto* f = static_cast<FunctionDecl*>(m);
      const auto& last = f->name.last();
      bool named = name ? last.special == 0 && symbols.name(last.symbol) == name : last.special == 2 && P(last.opToken) == op;
      if (!named || !takes(f))
        continue;
      if (found)
        return nullptr;
      found = f;
    }
    return found;
  }

  // The address of the object at v: `this`, a member's `&speed`, a pointer member's `target`.
  bool objectAddress(DebugValue& v, Node* at, std::string& out)
  {
    if (v.object)
      return fail(v, at, "the address of a value the code makes");
    if (v.prefix.empty())
      out = "this";
    else if (v.prefix.size() >= 2 && v.prefix.compare(v.prefix.size() - 2, 2, "->") == 0)
      out = v.prefix.substr(0, v.prefix.size() - 2);
    else
      out = "&" + wrap(v.prefix.substr(0, v.prefix.size() - 1));
    return true;
  }

  // The viewed object's path at v: `(*this)`, a member's `speed`, a pointer's `(*target)`.
  std::string objectPath(const DebugValue& v)
  {
    if (v.prefix.empty())
      return "(*this)";
    if (v.prefix.size() >= 2 && v.prefix.compare(v.prefix.size() - 2, 2, "->") == 0)
      return "(*" + v.prefix.substr(0, v.prefix.size() - 2) + ")";
    return v.prefix.substr(0, v.prefix.size() - 1);
  }

  bool prepareCall(DebugValue& v, Node* at, FunctionDecl* called, CallExpr* call, Expr* objectExpr, bool arrow, const Ty* staticClass, InlinedCall& c,
                   const std::vector<Bound>* preset)
  {
    TypeTable& types = v.global.types();
    bool isStatic = (called->flags & DF_Static) || !v.global.owningClass(called);
    if (called->flags & (DF_Virtual | DF_Override))
      return fail(v, at, "a call of a virtual function");
    if (!isStatic && !(called->flags & DF_Const))
      return fail(v, at, "a call of a member function that is not const");
    size_t given = call ? call->args.size() : preset ? preset->size() : 0;
    if (given != called->params.size())
      return fail(v, at, "a call that leaves a parameter its default value");
    if (v.depth > 8)
      return fail(v, at, "calls nested deeper than 8");
    const Ty* object = nullptr;
    std::string base;
    if (isStatic)
      object = staticClass;
    else if (!objectExpr || isThisObject(objectExpr))
    {
      if (isDebugValue(called, v.symbols))
        return fail(v, at, "a call of its own debugValue");
      object = v.self;
      c.object = v.object;
    }
    else if (const Bound* b = objectExpr->kind == NodeKind::NameExpr && static_cast<NameExpr*>(objectExpr)->resolved
                                ? boundOf(v, static_cast<NameExpr*>(objectExpr)->resolved) : nullptr; b && b->object && !arrow)
    {
      c.object = b->object; // (a parameter bound to an object the code made)
      object = b->object->type;
    }
    else if (!arrow && isMadeObject(objectExpr) && objectExpr->ty && types.unqualified(types.stripRef(objectExpr->ty))->kind == TyKind::Class)
    {
      if (!natvisObject(v, objectExpr, c.object)) // (getRaw<T>().getPrototype(): the ID getRaw makes)
        return false;
      object = c.object->type;
    }
    else
    {
      if (!natvisExpression(v, objectExpr, base))
        return false;
      if (base == "(*this)" && !arrow)
        base.clear(); // (a parameter bound to the viewed object: `id.getIndex()` in getPointer(this) reads `index`)
      else
        base = wrap(base) + (arrow ? "->" : ".");
      object = objectExpr->ty ? types.unqualified(types.stripRef(objectExpr->ty)) : nullptr;
      if (object && arrow && object->kind == TyKind::Pointer)
        object = object->inner ? types.unqualified(object->inner) : nullptr;
    }
    if (!isStatic && (!object || object->kind != TyKind::Class || !object->decl))
      return fail(v, at, "a call on something other than the object or a member that is a class");
    c.args = object && object->kind == TyKind::Class ? v.global.classArgsOf(object) : TemplateArgs();
    if (!called->templateParams.empty())
    {
      if (!call || call->templateArgCount != called->templateParams.size())
        return fail(v, at, "a call of a function template whose arguments are not known here");
      for (size_t i = 0; i < called->templateParams.size(); ++i)
      {
        if (!call->templateArgs[i] || !call->templateArgs[i]->isKnown() || types.isDependent(call->templateArgs[i]))
          return fail(v, at, "a call of a function template whose arguments are not known here");
        c.args.pairs.push_back({called->templateParams[i], call->templateArgs[i]});
      }
    }
    Decl* defined = c.args.empty() ? v.global.definitionOf(called) : v.global.definitionOf(called, c.args);
    c.def = defined && defined->kind == NodeKind::FunctionDecl ? static_cast<FunctionDecl*>(defined) : called;
    c.instance = c.args.empty() ? c.def : v.global.instanceBody(c.def, c.args, object);
    if (!c.instance)
      return fail(v, at, "a call of a function whose body is not here");
    c.self = isStatic ? nullptr : object;
    c.prefix = isStatic || c.object ? std::string() : v.prefix + base;
    for (size_t i = 0; call && i < given; ++i)
      if (!bindArgument(v, call->args[i], i < c.instance->params.size() ? c.instance->params[i] : nullptr, c.bound))
        return false;
    for (size_t i = 0; !call && preset && i < given && i < c.instance->params.size(); ++i)
    {
      c.bound.push_back((*preset)[i]);
      c.bound.back().param = c.instance->params[i];
    }
    return true;
  }

  // A const member function's body inlined on an object (the call's object, `this.toDouble()`, or a member,
  // `this.speed.debugValue()`; a conversion to bool on a class value): one `return EXPRESSION;` after
  // constexpr locals, as an expression, or as text (natvisText) for a function returning a String.
  bool inlinedMember(DebugValue& v, Node* at, FunctionDecl* called, Expr* objectExpr, bool arrow, bool text, std::string& out,
                     CallExpr* call = nullptr, const Ty* staticClass = nullptr, const std::vector<Bound>* preset = nullptr)
  {
    TypeTable& types = v.global.types();
    InlinedCall c;
    if (!prepareCall(v, at, called, call, objectExpr, arrow, staticClass, c, preset))
      return false;
    const TemplateArgs& args = c.args;
    FunctionDecl* def = c.def;
    FunctionDecl* instance = c.instance;
    DebugValue inner{v.global, v.symbols, def->file, instance, args, c.prefix, v.depth + 1, c.self};
    inner.bound = std::move(c.bound);
    inner.object = c.object;
    inner.asserts = v.asserts;
    inner.known = v.known;
    inner.reached = v.reached;
    std::vector<DebugCase> cases;
    bool endsNowhere = false;
    bool done = debugCases(inner, instance, cases, &endsNowhere);
    if (done && text && cases.size() != 1)
      done = fail(inner, instance, "its `if`s or switch, which a text built of it cannot hold");
    std::string value;
    if (done && text && natvisText(inner, cases[0].second, value))
    {
      out += value;
      return true;
    }
    if (done && !text)
    {
      // Its cases as one expression, `(C1 ? V1 : C2 ? V2 : V3)`; a body ending in a [[noreturn]] call, a
      // char pointer's `"?"` for the values it has no case for (Comparison::str()'s switch), any other value's
      // `reaches abort()` (noReturnEnd).
      std::string rest;
      bool characters = false;
      if (endsNowhere)
      {
        const Ty* returned = v.global.typeOfDeclIn(called, args);
        const Ty* pointee = returned && returned->kind == TyKind::Function && returned->inner && returned->inner->kind == TyKind::Pointer && returned->inner->inner
                              ? types.unqualified(returned->inner->inner)
                              : nullptr;
        characters = pointee && pointee->kind == TyKind::Char;
        if (characters)
          rest = "\"?\"";
      }
      std::vector<std::string> whens(cases.size());
      std::vector<bool> conditional(cases.size());
      for (size_t i = 0; done && i < cases.size(); ++i)
      {
        whens[i] = cases[i].text;
        conditional[i] = cases[i].conditional();
        if (cases[i].first && !natvisCondition(inner, cases[i].first, whens[i]))
          done = false;
      }
      if (done && endsNowhere && !characters)
        done = noReturnEnd(inner, instance, whens, conditional);
      std::vector<std::string> reaches = done ? caseReaches(inner.reached, whens, conditional) : std::vector<std::string>();
      std::string callReached = inner.reached;
      std::vector<std::string> callKnown = inner.known;
      const Ty* result = v.global.typeOfDeclIn(called, args);
      result = result && result->kind == TyKind::Function ? result->inner : nullptr;
      if (done && cases.empty() && !characters)
        rest = noValue(v, result);
      for (size_t i = cases.size(); done && i-- > 0;)
      {
        std::string one;
        inner.reached = reaches[i];
        inner.known = caseFacts(callKnown, whens, conditional, i);
        if (!natvisExpression(inner, cases[i].second, one))
        {
          done = false;
          break;
        }
        one = asResult(v, result, cases[i].second, one);
        rest = conditional[i] ? (rest.empty() ? one : "(" + whens[i] + " ? " + one + " : " + rest + ")") : one;
      }
      inner.reached = callReached;
      inner.known = callKnown;
      if (done)
      {
        out = wrap(rest);
        return true;
      }
    }
    if (v.why.empty())
    {
      v.why = "the function it calls, " + functionName(v.symbols, called) + ": " + inner.why;
      v.at = inner.at;
      v.atFile = inner.atFile;
    }
    return false;
  }

  // A returned pointer as the function's result type (`const Prototype*` from a lambda returning a Part's): the
  // cases of one conditional expression need one type.
  std::string asResult(DebugValue& v, const Ty* result, Expr* value, const std::string& one)
  {
    TypeTable& types = v.global.types();
    const Ty* r = result ? types.unqualified(types.stripRef(result)) : nullptr;
    const Ty* t = value && value->ty ? types.unqualified(types.stripRef(value->ty)) : nullptr;
    if (!r || !t || r == t || r->kind != TyKind::Pointer || t->kind != TyKind::Pointer || !r->inner || !t->inner ||
        types.unqualified(r->inner) == types.unqualified(t->inner))
      return one; // (the same type, `const` or not)
    bool reference = false;
    std::string type = natvisCastType(v.global, v.symbols, r, reference);
    return type.empty() ? one : "(" + type + ")" + wrap(one);
  }

  // A body whose cases end in a [[noreturn]] call (apply's switch, then abort()): where none of them is taken the
  // rule shows `reaches abort()`, like an assert's failure; the last case is then taken without its condition.
  bool noReturnEnd(DebugValue& inner, Node* at, const std::vector<std::string>& whens, std::vector<bool>& conditional)
  {
    if (!inner.asserts || !inner.noReturn)
      return fail(inner, at, "an end without a return here");
    std::string none = inner.reached;
    for (size_t i = 0; i < whens.size(); ++i)
      if (conditional[i])
        none = reachedAnd(none, "!(" + whens[i] + ")");
    inner.asserts->push_back({none, "reaches " + statementText(inner, inner.noReturn)});
    if (!conditional.empty())
      conditional.back() = false;
    return true;
  }

  // The value of a call whose every case the known facts rule out: it reaches its [[noreturn]] end, which the
  // rule shows before (`reaches abort()`, the same condition); a number of the result's type, never shown.
  std::string noValue(DebugValue& v, const Ty* result)
  {
    TypeTable& types = v.global.types();
    const Ty* r = result ? types.unqualified(types.stripRef(result)) : nullptr;
    bool reference = false;
    std::string type = r && r->kind == TyKind::Pointer ? natvisCastType(v.global, v.symbols, r, reference) : std::string();
    return type.empty() ? "0" : "(" + type + ")0";
  }

  bool inlinedCall(DebugValue& v, CallExpr* c, std::string& out)
  {
    auto* m = static_cast<MemberExpr*>(c->callee);
    return inlinedMember(v, c, static_cast<FunctionDecl*>(m->resolved), m->base, m->op == P::Arrow, false, out, c);
  }

  // A lambda's call: its body (a generic lambda's instance for the call's arguments) inlined where the lambda was
  // written, its parameters bound to the call's arguments.
  bool lambdaCall(DebugValue& v, CallExpr* call, const LambdaValue& lambda, std::string& out)
  {
    TypeTable& types = v.global.types();
    LambdaExpr* body = lambda.lambda;
    TemplateArgs args = lambda.args;
    if (v.depth > 8)
      return fail(v, call, "calls nested deeper than 8");
    if (!body->templateParams.empty())
    {
      if (call->templateArgCount != body->templateParams.size())
        return fail(v, call, "a call of a generic lambda whose arguments are not known here");
      TemplateArgs own;
      for (size_t i = 0; i < body->templateParams.size(); ++i)
        own.pairs.push_back({body->templateParams[i], call->templateArgs[i]});
      const Ty* closure = call->callee && call->callee->ty ? types.unqualified(types.stripRef(call->callee->ty)) : nullptr;
      LambdaExpr* instance = nullptr;
      TemplateArgs full;
      if (!closure || !v.global.lambdaInstance(closure, own, &full, &instance) || !instance)
        return fail(v, call, "a generic lambda whose body cannot be typed for this call");
      body = instance;
      args = full;
    }
    if (call->args.size() != body->params.size())
      return fail(v, call, "a lambda's call that leaves a parameter its default value");
    DebugValue inner{v.global, v.symbols, lambda.file, lambda.context, args, lambda.prefix, v.depth + 1, lambda.self};
    inner.bound = lambda.bound;
    inner.object = lambda.object;
    inner.asserts = v.asserts;
    inner.known = v.known;
    inner.reached = v.reached;
    for (size_t i = 0; i < call->args.size(); ++i)
      if (!bindArgument(v, call->args[i], body->params[i], inner.bound))
        return false;
    std::vector<DebugCase> cases;
    bool endsNowhere = false;
    bool done = debugCasesOf(inner, body->body, call, cases, &endsNowhere);
    std::string rest;
    std::vector<std::string> whens(cases.size());
    std::vector<bool> conditional(cases.size());
    for (size_t i = 0; done && i < cases.size(); ++i)
    {
      whens[i] = cases[i].text;
      conditional[i] = cases[i].conditional();
      if (cases[i].first && !natvisCondition(inner, cases[i].first, whens[i]))
        done = false;
    }
    if (done && endsNowhere)
      done = noReturnEnd(inner, call, whens, conditional);
    std::vector<std::string> reaches = done ? caseReaches(inner.reached, whens, conditional) : std::vector<std::string>();
    std::vector<std::string> callKnown = inner.known;
    if (done && cases.empty())
      rest = noValue(v, call->ty);
    for (size_t i = cases.size(); done && i-- > 0;)
    {
      std::string one;
      inner.reached = reaches[i];
      inner.known = caseFacts(callKnown, whens, conditional, i);
      if (!natvisExpression(inner, cases[i].second, one))
      {
        done = false;
        break;
      }
      one = asResult(v, call->ty, cases[i].second, one);
      rest = conditional[i] ? (rest.empty() ? one : "(" + whens[i] + " ? " + one + " : " + rest + ")") : one;
    }
    if (done)
    {
      out = wrap(rest);
      return true;
    }
    if (v.why.empty())
    {
      v.why = "the lambda it calls: " + inner.why;
      v.at = inner.at;
      v.atFile = inner.atFile;
    }
    return false;
  }

  // An object the code makes: a construction (`Handle(index)`, `ItemID()`: the constructor's member initializers and
  // the members' default initializers, its parameters bound to the arguments), or a call returning one (fromIndex,
  // getRaw<T>()), whose one return makes it.
  bool natvisObject(DebugValue& v, Expr* e, std::shared_ptr<ObjectValue>& out)
  {
    TypeTable& types = v.global.types();
    while (e && e->kind == NodeKind::ParenExpr)
      e = static_cast<ParenExpr*>(e)->inner;
    if (!e || !e->ty)
      return fail(v, e, "a value the code makes, of no known type");
    const Ty* cls = types.unqualified(types.stripRef(e->ty));
    if (cls->kind != TyKind::Class || !cls->decl)
      return fail(v, e, "a value the code makes that is no class");
    if (v.depth > 8)
      return fail(v, e, "calls nested deeper than 8");
    Span<Expr*> constructorArgs;
    bool construction = false;
    if (e->kind == NodeKind::FunctionalCastExpr)
    {
      constructorArgs = static_cast<FunctionalCastExpr*>(e)->args;
      construction = true;
    }
    else if (e->kind == NodeKind::BraceInitExpr)
    {
      constructorArgs = static_cast<BraceInitExpr*>(e)->args;
      construction = true;
    }
    else if (e->kind == NodeKind::CallExpr)
    {
      auto* c = static_cast<CallExpr*>(e);
      Decl* callee = c->callee && c->callee->kind == NodeKind::NameExpr ? static_cast<NameExpr*>(c->callee)->resolved
                     : c->callee && c->callee->kind == NodeKind::MemberExpr ? static_cast<MemberExpr*>(c->callee)->resolved
                                                                            : nullptr;
      if (!callee || callee->kind == NodeKind::ClassDecl || callee->kind == NodeKind::UsingAliasDecl || callee->kind == NodeKind::TemplateParamDecl ||
          (callee->kind == NodeKind::VariableDecl && (callee->flags & DF_Typedef)))
      {
        constructorArgs = c->args; // (`Handle(index)`, `ItemID()` read as a call of the class, its alias or a template parameter)
        construction = true;
      }
      else if (callee->kind == NodeKind::FunctionDecl)
      {
        // a function returning the object: its one return makes it
        auto* f = static_cast<FunctionDecl*>(callee);
        InlinedCall call;
        bool isStatic = (f->flags & DF_Static) || !v.global.owningClass(f);
        Expr* objectExpr = c->callee->kind == NodeKind::MemberExpr ? static_cast<MemberExpr*>(c->callee)->base : nullptr;
        bool arrow = c->callee->kind == NodeKind::MemberExpr && static_cast<MemberExpr*>(c->callee)->op == P::Arrow;
        const Ty* staticClass = isStatic && c->callee->kind == NodeKind::NameExpr ? calledClass(v, static_cast<NameExpr*>(c->callee), f) : nullptr;
        if (!prepareCall(v, c, f, c, objectExpr, arrow, staticClass, call, nullptr))
          return false;
        DebugValue inner{v.global, v.symbols, call.def->file, call.instance, call.args, call.prefix, v.depth + 1, call.self};
        inner.bound = std::move(call.bound);
        inner.object = call.object;
        inner.asserts = v.asserts;
        inner.known = v.known;
        inner.reached = v.reached;
        std::vector<DebugCase> cases;
        if (!debugCases(inner, call.instance, cases) || cases.size() != 1 || cases[0].conditional() || !natvisObject(inner, cases[0].second, out))
        {
          if (v.why.empty())
          {
            v.why = "the function making the value, " + functionName(v.symbols, f) + ": " +
                    (inner.why.empty() ? std::string("a body other than one `return EXPRESSION;`") : inner.why);
            v.at = inner.at ? inner.at : c;
            v.atFile = inner.atFile;
          }
          return false;
        }
        return true;
      }
      else
        return fail(v, e, "a value the code makes through something other than a function");
    }
    else if (isThisObject(e) && v.object)
    {
      out = v.object;
      return true;
    }
    else if (e->kind == NodeKind::NameExpr && static_cast<NameExpr*>(e)->resolved)
    {
      const Bound* b = boundOf(v, static_cast<NameExpr*>(e)->resolved);
      if (b && b->object)
      {
        out = b->object;
        return true;
      }
    }
    if (!construction)
      return fail(v, e, "a value the code makes in a way the debugger cannot follow");
    // A construction: the constructor the arguments choose, its member initializers; the rest by the members'
    // default initializers.
    auto made = std::make_shared<ObjectValue>();
    made->type = cls;
    TemplateArgs classArgs = v.global.classArgsOf(cls);
    std::vector<const Ty*> argTypes;
    for (Expr* a : constructorArgs)
      argTypes.push_back(a ? a->ty : nullptr);
    FunctionDecl* constructor = v.global.chooseConstructor(cls, argTypes);
    std::vector<std::string> set;
    if (constructor)
    {
      Decl* defined = classArgs.empty() ? v.global.definitionOf(constructor) : v.global.definitionOf(constructor, classArgs);
      auto* def = defined && defined->kind == NodeKind::FunctionDecl ? static_cast<FunctionDecl*>(defined) : constructor;
      FunctionDecl* instance = classArgs.empty() ? def : v.global.instanceBody(def, classArgs, cls);
      if (!instance)
        return fail(v, e, "a constructor whose body is not here");
      if (instance->body && (instance->body->kind != NodeKind::BlockStmt || !static_cast<BlockStmt*>(instance->body)->stmts.empty()))
        return fail(v, e, "a constructor with statements, whose value the debugger cannot follow");
      if (constructorArgs.size() != instance->params.size())
        return fail(v, e, "a construction that leaves a parameter its default value");
      DebugValue inner{v.global, v.symbols, def->file, instance, classArgs, "", v.depth + 1, cls};
      inner.asserts = v.asserts;
      inner.known = v.known;
      inner.reached = v.reached;
      for (size_t i = 0; i < constructorArgs.size(); ++i)
        if (!bindArgument(v, constructorArgs[i], instance->params[i], inner.bound))
          return false;
      for (const FunctionDecl::CtorInit& init : instance->ctorInits)
      {
        if (init.member.empty() || init.args.size() != 1)
          return fail(v, e, "a member initializer other than one value");
        std::string member(v.symbols.name(init.member.last().symbol)), value;
        if (!natvisExpression(inner, init.args[0], value))
        {
          if (v.why.empty())
          {
            v.why = "the constructor: " + inner.why;
            v.at = inner.at;
            v.atFile = inner.atFile;
          }
          return false;
        }
        made->members.push_back({member, value});
        set.push_back(member);
      }
    }
    else if (!constructorArgs.empty())
      return fail(v, e, "a construction whose constructor the debugger cannot find");
    // the members the constructor leaves: their default initializers, constants
    Decl* defined = v.global.definitionOf(cls->decl);
    if (defined && defined->kind == NodeKind::ClassDecl)
      for (Decl* m : static_cast<ClassDecl*>(defined)->members)
        if (m->kind == NodeKind::VariableDecl && !(m->flags & (DF_Static | DF_Typedef)) && static_cast<VariableDecl*>(m)->init)
        {
          std::string member = nameOf(static_cast<VariableDecl*>(m)->name, v.symbols);
          if (std::find(set.begin(), set.end(), member) != set.end())
            continue;
          ConstHooks hooks;
          std::optional<ConstValue> value = v.global.evaluate(static_cast<VariableDecl*>(m)->init, defined->file, defined, classArgs, hooks);
          GlobalScope::clearConstantReason();
          if (value && (value->kind == ConstValue::Kind::Int || value->kind == ConstValue::Kind::Float))
            made->members.push_back({member, natvisConstant(*value)});
        }
    out = made;
    return true;
  }

  // The class a static function's call names (`PrototypeList<PrototypeType>::getPointer(this)`: the qualifier's
  // instance here), the function's own class when it is no template, null for a free function.
  const Ty* calledClass(DebugValue& v, NameExpr* n, FunctionDecl* f)
  {
    Decl* owner = v.global.owningClass(f);
    if (!owner || owner->kind != NodeKind::ClassDecl)
      return nullptr;
    TypeTable& types = v.global.types();
    const Ty* cls = nullptr;
    if (n && n->name.parts.size() > 1)
    {
      NamedType qualifier{};
      qualifier.kind = NodeKind::NamedType;
      qualifier.token = n->token;
      qualifier.name = n->name;
      qualifier.name.parts.count -= 1;
      cls = v.global.typeOfAstIn(&qualifier, v.context, v.args);
    }
    else if (owner->templateParams.empty())
      cls = types.classType(owner, {});
    else
      cls = v.self;
    cls = cls ? types.unqualified(types.stripRef(cls)) : nullptr;
    return cls && cls->kind == TyKind::Class && !types.isDependent(cls) ? cls : nullptr;
  }

  // A class's const conversion whose result (its reference stripped) `wanted` accepts, null when it has none:
  // SizeLimitedString's `operator const std::String&() const` in a text view.
  FunctionDecl* conversionOf(GlobalScope& global, const Ty* t, const std::function<bool(const Ty*)>& wanted)
  {
    TypeTable& types = global.types();
    t = t ? types.unqualified(types.stripRef(t)) : nullptr;
    if (!t || t->kind != TyKind::Class || !t->decl)
      return nullptr;
    Decl* defined = global.definitionOf(t->decl);
    if (!defined || defined->kind != NodeKind::ClassDecl)
      return nullptr;
    TemplateArgs args = global.classArgsOf(t);
    for (Decl* m : static_cast<ClassDecl*>(defined)->members)
      if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Conversion) && (m->flags & DF_Const))
        if (const Ty* ft = global.typeOfDeclIn(m, args); ft && ft->kind == TyKind::Function && ft->inner && wanted(types.unqualified(types.stripRef(ft->inner))))
          return static_cast<FunctionDecl*>(m);
    return nullptr;
  }

  // A class's const conversion to bool (`constexpr operator bool() const { return this.version != 0; }`),
  // null when it has none.
  FunctionDecl* boolConversionOf(GlobalScope& global, const Ty* t)
  {
    TypeTable& types = global.types();
    t = t ? types.unqualified(types.stripRef(t)) : nullptr;
    if (!t || t->kind != TyKind::Class || !t->decl)
      return nullptr;
    Decl* defined = global.definitionOf(t->decl);
    if (!defined || defined->kind != NodeKind::ClassDecl)
      return nullptr;
    TemplateArgs args = global.classArgsOf(t);
    for (Decl* m : static_cast<ClassDecl*>(defined)->members)
      if (m->kind == NodeKind::FunctionDecl && (m->flags & DF_Conversion) && (m->flags & DF_Const))
        if (const Ty* ft = global.typeOfDeclIn(m, args); ft && ft->kind == TyKind::Function && ft->inner && ft->inner->kind == TyKind::Bool)
          return static_cast<FunctionDecl*>(m);
    return nullptr;
  }

  // A condition: a class value through its conversion to bool, inlined; anything else as an expression.
  bool natvisCondition(DebugValue& v, Expr* e, std::string& out)
  {
    if (FunctionDecl* conversion = e ? boolConversionOf(v.global, e->ty) : nullptr)
    {
      Expr* object = e;
      while (object && object->kind == NodeKind::ParenExpr)
        object = static_cast<ParenExpr*>(object)->inner;
      return inlinedMember(v, e, conversion, object, false, false, out);
    }
    return natvisExpression(v, e, out);
  }

  // `a == b` and `a != b` of a class whose comparison is defaulted (`= default`, or == rewritten from a
  // defaulted <=>), its data members compared one by one: each side the object (`this`), a member, or a
  // constant (`Force::player.id`, folded). Its members numbers, enumerations or such classes.
  bool memberwiseEquality(DebugValue& v, BinaryExpr* b, std::string& out)
  {
    TypeTable& types = v.global.types();
    struct Side
    {
      bool constant = false;
      ConstValue value;
      std::string path;
    };
    Side sides[2];
    Expr* operands[2] = {b->lhs, b->rhs};
    for (int i = 0; i < 2; ++i)
    {
      Expr* e = operands[i];
      while (e && e->kind == NodeKind::ParenExpr)
        e = static_cast<ParenExpr*>(e)->inner;
      if (!readsObject(e))
      {
        ConstHooks hooks;
        std::optional<ConstValue> value = v.global.evaluate(e, v.file, v.context, v.args, hooks);
        GlobalScope::clearConstantReason();
        if (!value || !value->isObject())
          return fail(v, e, "a comparison with a class value that is no constant");
        sides[i].constant = true;
        sides[i].value = *value;
      }
      else if (isThisObject(e))
        sides[i].path = v.prefix;
      else
      {
        if (!natvisExpression(v, e, sides[i].path))
          return false;
        sides[i].path += ".";
      }
    }
    std::vector<std::string> compared;
    std::function<bool(const Ty*, const Side&, const Side&, int)> members = [&](const Ty* cls, const Side& left, const Side& right, int depth) -> bool
    {
      Decl* defined = cls && cls->kind == TyKind::Class && cls->decl ? v.global.definitionOf(cls->decl) : nullptr;
      if (!defined || defined->kind != NodeKind::ClassDecl || depth > 4)
        return fail(v, b, "a comparison of something other than a class");
      auto* def = static_cast<ClassDecl*>(defined);
      if (!def->bases.empty())
        return fail(v, b, "a comparison of a class with bases");
      TemplateArgs args = v.global.classArgsOf(cls);
      size_t part = 0;
      for (Decl* m : def->members)
      {
        if (m->kind != NodeKind::VariableDecl || (m->flags & (DF_Static | DF_Typedef | DF_Friend)))
        {
          if (m->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(m)->name.parts.empty())
            return fail(v, b, "a comparison of a class with an anonymous struct or union");
          continue;
        }
        const Ty* type = v.global.typeOfDeclIn(m, args);
        type = type ? types.unqualified(type) : nullptr;
        std::string name(v.symbols.name(static_cast<VariableDecl*>(m)->name.last().symbol));
        auto at = [&](const Side& side, Side& member) -> bool
        {
          if (!side.constant)
          {
            member.path = side.path + name;
            return true;
          }
          if (!side.value.parts || part >= side.value.parts->size())
            return fail(v, b, "a constant class value without its members");
          member.constant = true;
          member.value = (*side.value.parts)[part];
          return true;
        };
        Side l, r;
        if (!at(left, l) || !at(right, r))
          return false;
        ++part;
        if (type && type->kind == TyKind::Class)
        {
          if (!l.constant)
            l.path += ".";
          if (!r.constant)
            r.path += ".";
          if (!members(type, l, r, depth + 1))
            return false;
          continue;
        }
        if (!type || !(type->isArithmetic() || type->kind == TyKind::Enum))
          return fail(v, b, "a comparison of a class with a member other than a number, an enumeration or such a class");
        auto text = [&](const Side& side) { return side.constant ? natvisConstant(side.value) : side.path; };
        compared.push_back(text(l) + " == " + text(r));
      }
      return true;
    };
    const Ty* cls = b->lhs->ty ? types.unqualified(types.stripRef(b->lhs->ty)) : nullptr;
    if (!members(cls, sides[0], sides[1], 0))
      return false;
    std::string all;
    for (const std::string& c : compared)
      all += (all.empty() ? "" : " && ") + c;
    if (all.empty())
      all = "true";
    out = P(b->op) == P::NotEq ? "!(" + all + ")" : "(" + all + ")";
    return true;
  }

  // A cast of operand to type (natvisCastType): `(T)(x)`; to a reference, `(*(T*)&x)`, the object itself
  // `(*(T*)this)` (`static_cast<const Entity&>(this).position`).
  bool natvisCast(DebugValue& v, Expr* operand, const std::string& type, bool reference, std::string& out)
  {
    std::string inner;
    if (reference && isThisObject(operand))
      inner = v.prefix.empty() ? "this"
              : v.prefix.back() == '.' ? "&" + v.prefix.substr(0, v.prefix.size() - 1) // (a member: its address)
                                       : v.prefix.substr(0, v.prefix.size() - 2); // (`p->`: the pointer)
    else if (!natvisExpression(v, operand, inner))
      return false;
    else if (reference)
      inner = "&(" + inner + ")";
    out = reference ? "(*(" + type + ")" + inner + ")" : "(" + type + ")" + wrap(inner);
    return true;
  }

  bool natvisExpression(DebugValue& v, Expr* e, std::string& out)
  {
    if (!e)
      return fail(v, e, "an expression the parser left out");
    if (e->kind == NodeKind::LiteralExpr && static_cast<LiteralExpr*>(e)->literalKind == TokenKind::String)
    {
      // ("a" in `this.branch ? " (branch)" : ""`: a C++ literal again, which natvis's expressions read)
      std::string text;
      if (!literalText(static_cast<LiteralExpr*>(e)->file ? static_cast<LiteralExpr*>(e)->file : v.file, static_cast<LiteralExpr*>(e), text))
        return fail(v, e, "a string literal with a prefix or an unusual escape");
      out = "\"";
      for (char c : text)
      {
        char hex[8];
        snprintf(hex, sizeof(hex), "\\x%02X", (unsigned char)c);
        out += c == '"' ? std::string("\\\"") : c == '\\' ? std::string("\\\\") : (unsigned char)c < 32 || (unsigned char)c > 126 ? std::string(hex) : std::string(1, c);
      }
      out += "\"";
      return true;
    }
    if (!readsObject(e))
    {
      ConstHooks hooks;
      std::optional<ConstValue> value = v.global.evaluate(e, v.file, v.context, v.args, hooks);
      GlobalScope::clearConstantReason();
      if (!value || (value->kind != ConstValue::Kind::Int && value->kind != ConstValue::Kind::Float))
        return fail(v, e, "a value that reads no member and is no number constant");
      out = natvisConstant(*value);
      if (!out.empty() && out[0] == '-')
        out = "(" + out + ")";
      return true;
    }
    switch (e->kind)
    {
    case NodeKind::ParenExpr:
    {
      std::string inner;
      if (!natvisExpression(v, static_cast<ParenExpr*>(e)->inner, inner))
        return false;
      out = wrap(inner);
      return true;
    }
    case NodeKind::NameExpr: // a data member, `this.` implicit
    {
      auto* n = static_cast<NameExpr*>(e);
      Decl* d = n->resolved;
      if (d && d->kind == NodeKind::ParamDecl)
      {
        const Bound* b = boundOf(v, d);
        if (b && !b->object && !b->lambda)
        {
          out = wrap(b->expression);
          return true;
        }
        return fail(v, e, b ? "an object or a lambda used as a value" : "a parameter of the function the debugger cannot call");
      }
      if (d && d->kind == NodeKind::VariableDecl && (d->flags & DF_Static))
      {
        // A static data member, a global (kovarex, 2026-11-13: a prototype's name through
        // `PrototypeList<T>::indexToPrototype[index]`): its name in the debug information, its class's
        // instance as the qualifier names it in this instance (`PrototypeList<ItemPrototype>::indexToPrototype`).
        Decl* owner = v.global.owningClass(d);
        if (!owner || owner->kind != NodeKind::ClassDecl)
          return fail(v, e, "a variable other than a data member or a class's static data member");
        TypeTable& types = v.global.types();
        const Ty* cls = nullptr;
        if (n->name.parts.size() > 1)
        {
          NamedType qualifier{};
          qualifier.kind = NodeKind::NamedType;
          qualifier.token = n->token;
          qualifier.name = n->name;
          qualifier.name.parts.count -= 1;
          cls = v.global.typeOfAstIn(&qualifier, v.context, v.args);
        }
        else if (owner->templateParams.empty())
          cls = types.classType(owner, {});
        else if (v.self)
          cls = v.self; // (a member of the class whose view this is, named alone)
        cls = cls ? types.unqualified(types.stripRef(cls)) : nullptr;
        if (!cls || cls->kind != TyKind::Class || types.isDependent(cls))
          return fail(v, e, "a static data member whose class the view cannot name");
        out = types.toString(cls, &v.symbols, true) + "::" + std::string(v.symbols.name(n->name.last().symbol));
        return true;
      }
      if (v.object)
        return memberOf(v, e, std::string(v.symbols.name(n->name.last().symbol)), out);
      out = v.prefix + std::string(v.symbols.name(n->name.last().symbol));
      return true;
    }
    case NodeKind::IndexExpr:
    {
      // An element: of an array or through a pointer, or of the library's Vector (its items), as natvis
      // writes them (`all.items[index]`).
      auto* x = static_cast<IndexExpr*>(e);
      if (x->args.size() != 1 || !x->base || !x->base->ty)
        return fail(v, e, "an index other than one value");
      TypeTable& types = v.global.types();
      const Ty* base = types.unqualified(types.stripRef(x->base->ty));
      bool vector = isLibraryClass(base, v.symbols, "Vector");
      if (!vector && base->kind == TyKind::Class)
      {
        // A class's own operator[] (CompactDeque's, its index wrapped): its const one, inlined on the object.
        FunctionDecl* index = memberFunction(v.global, v.symbols, base, nullptr, P::LBracket,
                                             [&](FunctionDecl* f) { return (f->flags & DF_Const) && f->params.size() == x->args.size(); });
        if (!index)
          return fail(v, e, "an index of a class without one const operator[]");
        std::vector<Bound> arguments;
        if (!bindArgument(v, x->args[0], nullptr, arguments))
          return false;
        return inlinedMember(v, e, index, x->base, false, false, out, nullptr, nullptr, &arguments);
      }
      if (!vector && base->kind != TyKind::Pointer && base->kind != TyKind::Array)
        return fail(v, e, "an index of a class other than the library's Vector");
      std::string object, at;
      if (!natvisExpression(v, x->base, object) || !natvisExpression(v, x->args[0], at))
        return false;
      out = wrap(object) + (vector ? ".items[" : "[") + at + "]";
      return true;
    }
    case NodeKind::MemberExpr:
    {
      auto* m = static_cast<MemberExpr*>(e);
      Decl* d = m->resolved;
      if (m->dynamicMember || !d || d->kind != NodeKind::VariableDecl || (d->flags & DF_Static))
        return fail(v, e, "a member that is no data member");
      std::string name(v.symbols.name(m->member.last().symbol));
      if (isThisObject(m->base))
      {
        if (v.object)
          return memberOf(v, e, name, out);
        out = v.prefix + name;
        return true;
      }
      if (m->op != P::Arrow && m->base && m->base->ty && v.global.types().unqualified(v.global.types().stripRef(m->base->ty))->kind == TyKind::Class)
      {
        // a member of an object the code made: a parameter bound to one (getPointer's `handle.index`), or a construction
        const Bound* b = m->base->kind == NodeKind::NameExpr && static_cast<NameExpr*>(m->base)->resolved ? boundOf(v, static_cast<NameExpr*>(m->base)->resolved) : nullptr;
        if ((b && b->object) || isMadeObject(m->base))
        {
          std::shared_ptr<ObjectValue> object = b && b->object ? b->object : nullptr;
          if (!object && !natvisObject(v, m->base, object))
            return false;
          return memberOf(v, e, name, out, object.get());
        }
      }
      std::string base;
      if (!natvisExpression(v, m->base, base))
        return false;
      if (base == "(*this)" && m->op != P::Arrow)
        out = v.prefix + name; // (`handle.index` with handle the viewed object: `index`)
      else
        out = wrap(base) + (m->op == P::Arrow ? "->" : ".") + name;
      return true;
    }
    case NodeKind::ThisExpr:
    {
      // `this` as a pointer (`&this`, folded by the parser: an iterator's `Iterator(&this, 0)`): the object's address
      const Ty* t = e->ty ? v.global.types().unqualified(v.global.types().stripRef(e->ty)) : nullptr;
      if (!t || t->kind != TyKind::Pointer)
        return fail(v, e, "the object itself, which has no value to show");
      return objectAddress(v, e, out);
    }
    case NodeKind::UnaryExpr:
    {
      auto* u = static_cast<UnaryExpr*>(e);
      if (u->implicitThis)
        return fail(v, e, "the object itself, which has no value to show");
      if (u->callee && u->calleeMember && !u->postfix && !u->isKeyword && P(u->op) == P::Star)
        return inlinedMember(v, e, u->callee, u->operand, false, false, out); // (an iterator's `*`, a Hook's)
      if (u->callee || u->call == OperatorCall::Function || u->postfix || u->isKeyword)
        return fail(v, e, "an operator that calls a function or changes a value");
      P op = P(u->op);
      if (op == P::Amp)
      {
        // an address: the object's (`&this`: `this`), or a member's
        if (isThisObject(u->operand))
          return objectAddress(v, e, out);
        std::string operand;
        if (!natvisExpression(v, u->operand, operand))
          return false;
        out = "&" + wrap(operand);
        return true;
      }
      if (op != P::Minus && op != P::Plus && op != P::Bang && op != P::Tilde && op != P::Star)
        return fail(v, e, "the operator " + std::string(punctuatorSpelling(op)));
      std::string operand;
      if (!natvisExpression(v, u->operand, operand))
        return false;
      out = std::string(punctuatorSpelling(op)) + operand;
      return true;
    }
    case NodeKind::BinaryExpr:
    {
      auto* b = static_cast<BinaryExpr*>(e);
      if ((P(b->op) == P::EqEq || P(b->op) == P::NotEq) && b->lhs && b->lhs->ty && b->rhs && b->rhs->ty)
      {
        TypeTable& types = v.global.types();
        const Ty* left = types.unqualified(types.stripRef(b->lhs->ty));
        const Ty* right = types.unqualified(types.stripRef(b->rhs->ty));
        bool defaulted = b->call == OperatorCall::Rewritten || (b->callee && (b->callee->flags & DF_Default));
        if (left == right && left->kind == TyKind::Class && defaulted)
          return memberwiseEquality(v, b, out);
      }
      if (b->callee && b->calleeMember && !b->isKeyword && (P(b->op) == P::EqEq || P(b->op) == P::NotEq) &&
          b->callee->name.last().special == 2 && P(b->callee->name.last().opToken) == P::EqEq)
      {
        // a class's own operator== (an iterator's), inlined on the left with the right as its argument; `!=` its negation
        std::vector<Bound> arguments;
        if (!bindArgument(v, b->rhs, nullptr, arguments))
          return false;
        std::string equal;
        if (!inlinedMember(v, e, b->callee, b->lhs, false, false, equal, nullptr, nullptr, &arguments))
          return false;
        out = P(b->op) == P::NotEq ? "!" + wrap(equal) : equal;
        return true;
      }
      if (b->callee || b->call == OperatorCall::Function || b->call == OperatorCall::Rewritten || b->isKeyword)
        return fail(v, e, "an operator function");
      P op = P(b->op);
      static const P allowed[] = {P::Plus, P::Minus, P::Star, P::Slash, P::Percent, P::Shl, P::Shr, P::Amp, P::Pipe, P::Caret,
                                  P::Less, P::Greater, P::LessEq, P::GreaterEq, P::EqEq, P::NotEq, P::AndAnd, P::OrOr};
      if (std::find(std::begin(allowed), std::end(allowed), op) == std::end(allowed))
        return fail(v, e, "the operator " + std::string(punctuatorSpelling(op)));
      std::string lhs, rhs;
      if (!natvisExpression(v, b->lhs, lhs) || !natvisExpression(v, b->rhs, rhs))
        return false;
      out = lhs + " " + std::string(punctuatorSpelling(op)) + " " + rhs;
      return true;
    }
    case NodeKind::ConditionalExpr:
    {
      auto* c = static_cast<ConditionalExpr*>(e);
      if (!c->then)
        return fail(v, e, "the operator ?:");
      std::string cond, then, otherwise;
      if (!natvisCondition(v, c->cond, cond) || !natvisExpression(v, c->then, then) || !natvisExpression(v, c->else_, otherwise))
        return false;
      out = "(" + cond + " ? " + then + " : " + otherwise + ")";
      return true;
    }
    case NodeKind::CastExpr: case NodeKind::NamedCastExpr: case NodeKind::FunctionalCastExpr:
    {
      Expr* operand = e->kind == NodeKind::CastExpr ? static_cast<CastExpr*>(e)->operand
                      : e->kind == NodeKind::NamedCastExpr ? static_cast<NamedCastExpr*>(e)->operand
                      : static_cast<FunctionalCastExpr*>(e)->args.size() == 1 ? static_cast<FunctionalCastExpr*>(e)->args[0] : nullptr;
      if (e->kind == NodeKind::NamedCastExpr && static_cast<NamedCastExpr*>(e)->castKind != Kw::static_cast_)
        return fail(v, e, "a cast other than static_cast, a C cast or a type's name");
      bool reference = false;
      std::string type = natvisCastType(v.global, v.symbols, e->ty, reference);
      if (!operand || type.empty())
        return fail(v, e, "a cast other than to a number, an enumeration, or a pointer or reference to a class");
      return natvisCast(v, operand, type, reference, out);
    }
    case NodeKind::CallExpr:
    {
      auto* c = static_cast<CallExpr*>(e);
      // A const member function's call, inlined: the object's own (`this.toDouble()`) or a member's, its
      // arguments bound to its parameters.
      if (c->callee && c->callee->kind == NodeKind::MemberExpr && static_cast<MemberExpr*>(c->callee)->resolved &&
          static_cast<MemberExpr*>(c->callee)->resolved->kind == NodeKind::FunctionDecl)
        return inlinedCall(v, c, out);
      // A lambda's call through its parameter (apply's `op(ItemID())`): its body, its captures where it was written.
      if (c->callee && c->callee->kind == NodeKind::NameExpr && static_cast<NameExpr*>(c->callee)->resolved &&
          static_cast<NameExpr*>(c->callee)->resolved->kind == NodeKind::ParamDecl)
        if (const Bound* b = boundOf(v, static_cast<NameExpr*>(c->callee)->resolved); b && b->lambda)
          return lambdaCall(v, c, *b->lambda, out);
      // A static or free function's call, inlined (ID's `PrototypeList<PrototypeType>::getPointer(this)`).
      if (c->callee && c->callee->kind == NodeKind::NameExpr && static_cast<NameExpr*>(c->callee)->resolved &&
          static_cast<NameExpr*>(c->callee)->resolved->kind == NodeKind::FunctionDecl)
      {
        auto* n = static_cast<NameExpr*>(c->callee);
        auto* f = static_cast<FunctionDecl*>(n->resolved);
        if ((f->flags & DF_Static) || !v.global.owningClass(f))
          return inlinedMember(v, c, f, nullptr, false, false, out, c, calledClass(v, n, f));
        return inlinedMember(v, c, f, nullptr, false, false, out, c); // (the object's own, `this.` implicit)
      }
      // A number type's name called with one value is its conversion: `f64(this.value)` (f64 the
      // library's alias of double; a type's name is no declaration the call resolved, as a function's is).
      Decl* callee = c->callee && c->callee->kind == NodeKind::NameExpr ? static_cast<NameExpr*>(c->callee)->resolved : nullptr;
      bool typeName = c->callee && c->callee->kind == NodeKind::NameExpr &&
                      (!callee || callee->kind == NodeKind::UsingAliasDecl || callee->kind == NodeKind::EnumDecl ||
                       (callee->kind == NodeKind::VariableDecl && (callee->flags & DF_Typedef)));
      bool reference = false;
      std::string type = natvisCastType(v.global, v.symbols, e->ty, reference);
      if (!typeName || c->args.size() != 1 || type.empty() || reference)
        return fail(v, e, "a call other than of a const member function (debugValue's body is the debugger's expression: data members, constants, operators and const member functions with such bodies)");
      return natvisCast(v, c->args[0], type, false, out);
    }
    case NodeKind::BraceInitExpr:
    {
      // A number type's conversion as the parser writes it in places (`u32(x)` among a call's arguments).
      auto* b = static_cast<BraceInitExpr*>(e);
      bool reference = false;
      std::string type = b->args.size() == 1 ? natvisCastType(v.global, v.symbols, e->ty, reference) : std::string();
      if (type.empty() || reference)
        return fail(v, e, "a braced value other than a number's conversion");
      return natvisCast(v, b->args[0], type, false, out);
    }
    default: return fail(v, e, std::string("this kind of expression (") + nodeKindName(e->kind) + ")");
    }
  }

  // A Span's construction from a pointer and a length (`Span(this.data, this.size)`): their two
  // expressions. False when e is no such construction.
  bool spanParts(Expr* e, Expr*& pointer, Expr*& length)
  {
    while (e && e->kind == NodeKind::ParenExpr)
      e = static_cast<ParenExpr*>(e)->inner;
    Span<Expr*> args;
    if (e && e->kind == NodeKind::CallExpr)
      args = static_cast<CallExpr*>(e)->args;
    else if (e && e->kind == NodeKind::FunctionalCastExpr)
      args = static_cast<FunctionalCastExpr*>(e)->args;
    else if (e && e->kind == NodeKind::BraceInitExpr)
      args = static_cast<BraceInitExpr*>(e)->args;
    else
      return false;
    if (args.size() != 2)
      return false;
    pointer = args[0];
    length = args[1];
    return true;
  }

  // Whether t is the library's class of that name (`String`, `Span`): its declaration in the library's files.
  bool isLibraryClass(const Ty* t, const SymbolTable& symbols, const char* name)
  {
    return t && t->kind == TyKind::Class && t->decl && t->decl->kind == NodeKind::ClassDecl && t->decl->file && t->decl->file->isLibrary &&
           nameOf(static_cast<const ClassDecl*>(t->decl)->name, symbols) == name;
  }

  // A string literal's characters, its escapes read (\" \\ \n \t \r \0), or false for one with a prefix
  // or another escape: a natvis DisplayString holds them as text.
  bool literalText(const ParsedFile* f, const LiteralExpr* e, std::string& out)
  {
    if (e->literalKind != TokenKind::String || !f || !f->tokens || e->token >= f->tokens->size())
      return false;
    const Token& t = (*f->tokens)[e->token];
    std::string_view text = f->text.substr(t.offset, t.length);
    if (text.size() < 2 || text.front() != '"' || text.back() != '"')
      return false;
    for (size_t i = 1; i + 1 < text.size(); ++i)
    {
      char c = text[i];
      if (c != '\\')
      {
        out += c;
        continue;
      }
      if (++i + 1 > text.size() - 1)
        return false;
      switch (text[i])
      {
      case '"': out += '"'; break;
      case '\\': out += '\\'; break;
      case 'n': out += '\n'; break;
      case 't': out += '\t'; break;
      case 'r': out += '\r'; break;
      case '0': break;
      case 'x':
      {
        // \xHH...: its byte (a UTF-8 sign's, Comparison's "\xE2\x89\xA5")
        size_t end = i + 1;
        unsigned value = 0;
        while (end + 1 < text.size() && isxdigit((unsigned char)text[end]))
          value = value * 16 + (isdigit((unsigned char)text[end]) ? text[end] - '0' : (tolower((unsigned char)text[end]) - 'a' + 10)), ++end;
        if (end == i + 1 || value > 255)
          return false;
        out += char(value);
        i = end - 1;
        break;
      }
      default: return false;
      }
    }
    return true;
  }

  // A text view's DisplayStrings (DebugValue, a debugValue returning std::String): its variants, each a
  // condition (empty: always) and a text, in order, the first that holds shown, the last unconditional.
  // The `+` of string literals (their text, natvis's braces doubled), std::toString(VALUE, ...) and other
  // values (each `{VALUE}` by its own view; a floating one `,g`, characters `,s8b`); a choice between texts
  // (`this.branch ? " (branch)" : ""`) and a call of a const member function returning text (MapVersion's
  // str(), Comparison's switch of signs) split the variants, natvis's expressions having no string literals.
  using TextVariants = std::vector<std::pair<std::string, std::string>>;

  std::string bothConditions(const std::string& a, const std::string& b)
  {
    if (a.empty())
      return b;
    if (b.empty())
      return a;
    return a + " && " + b;
  }

  bool isTextType(const Ty* t, const SymbolTable& symbols, TypeTable& types)
  {
    t = t ? types.unqualified(types.stripRef(t)) : nullptr;
    if (!t)
      return false;
    if (isLibraryClass(t, symbols, "String") || isLibraryClass(t, symbols, "StringView"))
      return true;
    return (t->kind == TyKind::Pointer || t->kind == TyKind::Array) && t->inner && types.unqualified(t->inner)->kind == TyKind::Char;
  }

  // Every variant of out followed by every variant of piece, in order (the first that holds of both).
  bool textProduct(DebugValue& v, Node* at, TextVariants& out, const TextVariants& piece)
  {
    if (piece.size() == 1)
    {
      for (auto& [condition, text] : out)
        condition = bothConditions(condition, piece[0].first), text += piece[0].second;
      return true;
    }
    TextVariants made;
    for (const auto& [condition, text] : out)
      for (const auto& [more, added] : piece)
        made.push_back({bothConditions(condition, more), text + added});
    if (made.size() > 32)
      return fail(v, at, "a text of more than 32 variants (its choices multiply)");
    out = std::move(made);
    return true;
  }

  bool natvisText(DebugValue& v, Expr* e, TextVariants& out);

  // A const member function returning text, inlined: each of its cases' texts under its condition (a
  // body ending in a [[noreturn]] call, `?` for the values it has no case for).
  bool inlinedText(DebugValue& v, Node* at, FunctionDecl* called, Expr* objectExpr, bool arrow, TextVariants& out, CallExpr* call = nullptr,
                   const Ty* staticClass = nullptr)
  {
    InlinedCall c;
    if (!prepareCall(v, at, called, call, objectExpr, arrow, staticClass, c, nullptr))
      return false;
    const TemplateArgs& args = c.args;
    FunctionDecl* def = c.def;
    FunctionDecl* instance = c.instance;
    DebugValue inner{v.global, v.symbols, def->file, instance, args, c.prefix, v.depth + 1, c.self};
    inner.bound = std::move(c.bound);
    inner.object = c.object;
    inner.asserts = v.asserts;
    inner.known = v.known;
    inner.reached = v.reached;
    std::vector<DebugCase> cases;
    bool endsNowhere = false;
    TextVariants piece;
    bool done = debugCases(inner, instance, cases, &endsNowhere);
    std::string callReached = inner.reached, notBefore;
    std::vector<std::string> callKnown = inner.known, earlier;
    for (size_t i = 0; done && i < cases.size(); ++i)
    {
      std::string when = cases[i].text;
      TextVariants one{{"", ""}};
      if (cases[i].first && !natvisCondition(inner, cases[i].first, when))
      {
        done = false;
        break;
      }
      inner.reached = reachedAnd(callReached, reachedAnd(notBefore, cases[i].conditional() ? when : std::string()));
      inner.known = callKnown;
      for (const std::string& before : earlier)
        inner.known.push_back(canonicalAtom("!(" + before + ")"));
      if (cases[i].conditional())
      {
        std::vector<std::string> atoms;
        conditionAtoms(when, atoms);
        for (const std::string& atom : atoms)
          inner.known.push_back(canonicalAtom(atom));
      }
      if (!natvisText(inner, cases[i].second, one))
      {
        done = false;
        break;
      }
      if (cases[i].conditional())
      {
        notBefore = reachedAnd(notBefore, "!(" + when + ")");
        earlier.push_back(when);
      }
      for (const auto& [condition, text] : one)
        piece.push_back({bothConditions(when, condition), text});
    }
    inner.reached = callReached;
    if (done)
    {
      if (endsNowhere)
        piece.push_back({"", "?"});
      return textProduct(v, at, out, piece);
    }
    if (v.why.empty())
    {
      v.why = "the function it calls, " + functionName(v.symbols, called) + ": " + inner.why;
      v.at = inner.at;
      v.atFile = inner.atFile;
    }
    return false;
  }

  bool natvisText(DebugValue& v, Expr* e, TextVariants& out)
  {
    while (e && e->kind == NodeKind::ParenExpr)
      e = static_cast<ParenExpr*>(e)->inner;
    if (!e)
      return fail(v, e, "an expression the parser left out");
    if (out.size() > 1 && !(e->kind == NodeKind::LiteralExpr && static_cast<LiteralExpr*>(e)->literalKind == TokenKind::String))
    {
      // After a choice (typeToStr's case per type): the piece once per variant, knowing that variant's
      // condition and that no earlier one's holds, so a switch on the same value takes its one case
      // (getName's prototype list for that type).
      TextVariants all;
      std::string reached = v.reached, notBefore;
      std::vector<std::string> known = v.known;
      for (size_t j = 0; j < out.size(); ++j)
      {
        TextVariants one{out[j]};
        std::vector<std::string> whens, atoms;
        std::vector<bool> conditional;
        for (size_t k = 0; k <= j; ++k)
        {
          whens.push_back(out[k].first);
          conditional.push_back(!out[k].first.empty());
        }
        v.known = caseFacts(known, whens, conditional, j);
        v.reached = reachedAnd(reached, reachedAnd(notBefore, out[j].first));
        bool done = natvisText(v, e, one);
        v.known = known;
        v.reached = reached;
        if (!done)
          return false;
        if (!out[j].first.empty())
          notBefore = reachedAnd(notBefore, "!(" + out[j].first + ")");
        all.insert(all.end(), one.begin(), one.end());
      }
      if (all.size() > 32)
        return fail(v, e, "a text of more than 32 variants (its choices multiply)");
      out = std::move(all);
      return true;
    }
    TypeTable& types = v.global.types();
    auto append = [&](const std::string& text)
    {
      for (auto& variant : out)
        variant.second += text;
      return true;
    };
    if (e->kind == NodeKind::LiteralExpr && static_cast<LiteralExpr*>(e)->literalKind == TokenKind::String)
    {
      std::string text, doubled;
      if (!literalText(static_cast<LiteralExpr*>(e)->file ? static_cast<LiteralExpr*>(e)->file : v.file, static_cast<LiteralExpr*>(e), text))
        return fail(v, e, "a string literal with a prefix or an escape other than \\\" \\\\ \\n \\t \\r \\x");
      for (char c : text)
        if (c == '{' || c == '}')
          doubled += std::string(2, c);
        else
          doubled += c;
      return append(doubled);
    }
    const Ty* type = e->ty ? types.unqualified(types.stripRef(e->ty)) : nullptr;
    bool string = isLibraryClass(type, v.symbols, "String");
    bool view = isLibraryClass(type, v.symbols, "StringView");
    auto isToString = [&](Expr* callee)
    {
      Decl* d = callee && callee->kind == NodeKind::NameExpr ? static_cast<NameExpr*>(callee)->resolved : nullptr;
      return d && d->kind == NodeKind::FunctionDecl && d->file && d->file->isLibrary && v.symbols.name(static_cast<FunctionDecl*>(d)->name.last().symbol) == "toString";
    };
    if (e->kind == NodeKind::BraceInitExpr && isLibraryClass(type, v.symbols, "Text"))
    {
      // std::toString(a, ".", b)'s pieces, a std::Text (design.md 14): each a piece of the text
      for (Expr* piece : static_cast<BraceInitExpr*>(e)->args)
        if (!natvisText(v, piece, out))
          return false;
      return true;
    }
    if (e->kind == NodeKind::BinaryExpr && P(static_cast<BinaryExpr*>(e)->op) == P::Plus && string)
      return natvisText(v, static_cast<BinaryExpr*>(e)->lhs, out) && natvisText(v, static_cast<BinaryExpr*>(e)->rhs, out);
    if (e->kind == NodeKind::CallExpr && isToString(static_cast<CallExpr*>(e)->callee))
    {
      // std::toString(x) or std::toString(a, ".", b): x as the debugger shows it, each piece of the text
      for (Expr* piece : static_cast<CallExpr*>(e)->args)
        if (!natvisText(v, piece, out))
          return false;
      return true;
    }
    if ((string || view) && ((e->kind == NodeKind::CallExpr && static_cast<CallExpr*>(e)->args.size() == 1 &&
                               !(static_cast<CallExpr*>(e)->callee && static_cast<CallExpr*>(e)->callee->kind == NodeKind::MemberExpr)) ||
                              (e->kind == NodeKind::FunctionalCastExpr && static_cast<FunctionalCastExpr*>(e)->args.size() == 1)))
      return natvisText(v, e->kind == NodeKind::CallExpr ? static_cast<CallExpr*>(e)->args[0] : static_cast<FunctionalCastExpr*>(e)->args[0], out); // (std::String("..."), std::StringView(x))
    if (e->kind == NodeKind::ConditionalExpr && static_cast<ConditionalExpr*>(e)->then && isTextType(e->ty, v.symbols, types))
    {
      // a choice between texts: the variants of each, under the condition and after it
      auto* c = static_cast<ConditionalExpr*>(e);
      std::string when;
      TextVariants then{{"", ""}}, otherwise{{"", ""}};
      if (!natvisCondition(v, c->cond, when) || !natvisText(v, c->then, then) || !natvisText(v, c->else_, otherwise))
        return false;
      TextVariants piece;
      for (const auto& [condition, text] : then)
        piece.push_back({bothConditions(when, condition), text});
      piece.insert(piece.end(), otherwise.begin(), otherwise.end());
      return textProduct(v, e, out, piece);
    }
    if (e->kind == NodeKind::CallExpr && isTextType(e->ty, v.symbols, types) &&
        static_cast<CallExpr*>(e)->callee && static_cast<CallExpr*>(e)->callee->kind == NodeKind::MemberExpr &&
        static_cast<MemberExpr*>(static_cast<CallExpr*>(e)->callee)->resolved &&
        static_cast<MemberExpr*>(static_cast<CallExpr*>(e)->callee)->resolved->kind == NodeKind::FunctionDecl)
    {
      auto* m = static_cast<MemberExpr*>(static_cast<CallExpr*>(e)->callee);
      if (isDebugValue(m->resolved, v.symbols) && !isThisObject(m->base))
      {
        // Another object's debugValue (a member's: CircuitCondition's `this.first.debugValue()`): that object, shown
        // by its own rule, as natvis shows a member, not its text inlined here, whose cases would multiply with
        // the others' (a condition's two signals and its comparison: 17 x 6 x 17).
        std::string object;
        if (!natvisExpression(v, m->base, object))
          return false;
        return append("{" + (m->op == P::Arrow ? "*(" + object + ")" : object) + "}");
      }
      // a const member function returning text, its text inlined (MapVersion's str(), Comparison's)
      return inlinedText(v, e, static_cast<FunctionDecl*>(m->resolved), m->base, m->op == P::Arrow, out, static_cast<CallExpr*>(e));
    }
    if (e->kind == NodeKind::CallExpr && isTextType(e->ty, v.symbols, types) && static_cast<CallExpr*>(e)->callee &&
        static_cast<CallExpr*>(e)->callee->kind == NodeKind::NameExpr && static_cast<NameExpr*>(static_cast<CallExpr*>(e)->callee)->resolved &&
        static_cast<NameExpr*>(static_cast<CallExpr*>(e)->callee)->resolved->kind == NodeKind::FunctionDecl)
    {
      // a static or free function returning text, or the object's own (`this.` implicit), its text inlined
      auto* n = static_cast<NameExpr*>(static_cast<CallExpr*>(e)->callee);
      auto* f = static_cast<FunctionDecl*>(n->resolved);
      bool isStatic = (f->flags & DF_Static) || !v.global.owningClass(f);
      return inlinedText(v, e, f, nullptr, false, out, static_cast<CallExpr*>(e), isStatic ? calledClass(v, n, f) : nullptr);
    }
    if (type && type->kind == TyKind::Class && !string && !view)
      if (FunctionDecl* conversion = conversionOf(v.global, type, [&](const Ty* t) { return isLibraryClass(t, v.symbols, "String"); }))
        return inlinedText(v, e, conversion, e, false, out); // (a class that becomes text through its conversion: StationID's SizeLimitedString name)
    std::string expression;
    if (!natvisExpression(v, e, expression))
      return false;
    if (type && type->kind == TyKind::Int && type->bits == 8)
      expression = "(int)(" + expression + ")"; // (a u8 or i8 as its number, not a character)
    bool characters = type && (type->kind == TyKind::Pointer || type->kind == TyKind::Array) && type->inner &&
                      types.unqualified(type->inner)->kind == TyKind::Char; // (a char array or pointer: its text)
    return append("{" + expression +
                  (type && type->kind == TyKind::Float ? ",g"
                   : characters ? ",s8b" // (kpp's text is UTF-8)
                   : string || view ? ",sb"
                   : "") +
                  "}");
  }

  // One text, for a place that cannot hold variants (an inlined function's text in an expression).
  bool natvisText(DebugValue& v, Expr* e, std::string& out)
  {
    TextVariants variants{{"", ""}};
    if (!natvisText(v, e, variants))
      return false;
    if (variants.size() != 1)
      return fail(v, e, "a choice between texts where only one text can stand");
    out += variants[0].second;
    return true;
  }

  // FILE:LINE:COLUMN of a node's token, as the compiler's diagnostics give a place.
  std::string placeOf(const ParsedFile* f, const Node* n)
  {
    if (!f || !n || !f->tokens || n->token >= f->tokens->size())
      return f ? f->path : std::string();
    size_t offset = (*f->tokens)[n->token].offset, line = 1, column = 1;
    for (size_t i = 0; i < offset && i < f->text.size(); ++i)
      if (f->text[i] == '\n')
        ++line, column = 1;
      else
        ++column;
    return f->path + ":" + std::to_string(line) + ":" + std::to_string(column);
  }

  // The natvis rule of one class instance with a debugValue() (DebugValue above): the class shown as
  // its debugValue's result: a number, an enumeration or a pointer as its value; a member that is a
  // class as that class's view and expansion; a Span (std::Span(pointer, length)) as its length and
  // elements. Each `if (CONDITION) return` is a case of the rule. False without a debugValue, and
  // false with message (FILE:LINE:COLUMN: error: ...) for one the rule cannot say.
  // A rule's DisplayStrings without the ones an earlier one with the same condition hides (a text's choice
  // made again inside a case that already decided it: `index == 0` in an ID's getName).
  std::string reachedDisplays(const std::string& displays)
  {
    std::string out;
    std::set<std::string> seen;
    size_t at = 0;
    while (at < displays.size())
    {
      size_t end = displays.find('\n', at);
      end = end == std::string::npos ? displays.size() : end + 1;
      std::string line = displays.substr(at, end - at);
      at = end;
      size_t c = line.find(" Condition=\"");
      if (c != std::string::npos && line.find("<DisplayString") != std::string::npos)
      {
        size_t from = c + 12, to = line.find('"', from);
        std::string condition = line.substr(from, to - from);
        std::string view;
        if (size_t i = line.find(" IncludeView=\""); i != std::string::npos)
          view = line.substr(i, line.find('"', i + 14) - i);
        if (!seen.insert(view + "|" + condition).second)
          continue;
      }
      out += line;
    }
    return out;
  }

  bool debugValueRule(GlobalScope& global, const SymbolTable& symbols, const DebugClass& d, std::string& out, std::string& message)
  {
    TypeTable& types = global.types();
    if (!d.type || !d.type->decl || d.type->decl->kind != NodeKind::ClassDecl)
      return false;
    Decl* defined = global.definitionOf(d.type->decl);
    if (!defined || defined->kind != NodeKind::ClassDecl || !defined->file || defined->file->isImported)
      return false;
    auto* cls = static_cast<ClassDecl*>(defined);
    if (!cls->flagBits.empty() && std::none_of(cls->members.begin(), cls->members.end(), [&](Decl* m) { return isDebugValue(m, symbols); }))
    {
      // A set of flags (design.md 27): the names of the flags set, `A | B | C` (kovarex, 2026-11-12), `{}` when none, the bits
      // no flag names as their number; a row per flag's name, true or false, and its bits. Natvis has no loop and no texts to
      // join, so the value line is a chain of views of the one object, one a flag in the order of their bits (a second name
      // left out): kpp<i> shows its flag's name when it is set, ` | ` and the next view when a later bit is set too
      // (`NAME | {*this,view(kpp3)}`: Visual Studio drops a text's leading spaces, so the separator goes after a name), else
      // the next view. The last shows the bits no flag names. The named views stand first: the default display, last,
      // starts the chain.
      std::vector<std::pair<unsigned, std::string>> named;
      unsigned long long declared = 0;
      std::string items;
      for (const ClassDecl::Flag& flag : cls->flagBits)
      {
        char mask[24];
        snprintf(mask, sizeof mask, "0x%llx", 1ull << flag.bit);
        std::string name(symbols.name(flag.name));
        if (!flag.alias)
        {
          named.push_back({flag.bit, name});
          declared |= 1ull << flag.bit;
        }
        items += "      <Item Name=\"" + escape(name) + "\">" + escape("(__kpp_bits & " + std::string(mask) + ") != 0") + "</Item>\n";
      }
      std::stable_sort(named.begin(), named.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
      auto hex = [](unsigned long long v)
      {
        char text[24];
        snprintf(text, sizeof text, "0x%llx", v);
        return std::string(text);
      };
      auto view = [](size_t i) { return "kpp" + std::to_string(i); };
      auto shown = [&](const std::string& in, const std::string& condition, const std::string& text)
      {
        return "    <DisplayString" + (in.empty() ? std::string() : " IncludeView=\"" + in + "\"") +
               (condition.empty() ? std::string() : " Condition=\"" + escape(condition) + "\"") + ">" + escape(text) + "</DisplayString>\n";
      };
      std::string unnamed = "(unsigned long long)(__kpp_bits & " + hex(~declared) + ")";
      std::string displays;
      unsigned long long later = ~0ull;
      for (size_t i = 0; i < named.size(); ++i)
      {
        later &= ~(1ull << named[i].first);
        std::string set = "(__kpp_bits & " + hex(1ull << named[i].first) + ") != 0";
        displays += shown(view(i), set + " && (__kpp_bits & " + hex(later) + ") != 0", named[i].second + " | {*this,view(" + view(i + 1) + ")}");
        displays += shown(view(i), set, named[i].second);
        displays += shown(view(i), "", "{*this,view(" + view(i + 1) + ")}");
      }
      displays += shown(view(named.size()), "", "{" + unnamed + "}");
      displays += shown("", "__kpp_bits == 0", "{{}}");
      displays += shown("", "", "{*this,view(" + view(0) + ")}");
      out = "  <Type Name=\"" + escape(d.name) + "\">\n" + displays + "    <Expand>\n" + items + "      <Item Name=\"[bits]\">__kpp_bits,x</Item>\n    </Expand>\n  </Type>\n";
      return true;
    }
    for (Decl* member : cls->members)
    {
      if (!isDebugValue(member, symbols) || static_cast<FunctionDecl*>(member)->name.parts.size() != 1)
        continue;
      auto* f = static_cast<FunctionDecl*>(member);
      auto error = [&](const ParsedFile* file, const Node* at, const std::string& what)
      {
        message = placeOf(file, at) + ": error: " + what + " (docs/debugging.md, \"debugValue\")";
        return false;
      };
      if (!f->params.empty() || !(f->flags & DF_Const) || (f->flags & DF_Static) || !f->templateParams.empty())
        return error(f->file, f, "debugValue, which the debugger shows " + d.name + " as, is a const member function without parameters");
      TemplateArgs args = global.classArgsOf(d.type);
      Decl* body = args.empty() ? global.definitionOf(f) : global.definitionOf(f, args);
      auto* def = body && body->kind == NodeKind::FunctionDecl ? static_cast<FunctionDecl*>(body) : f;
      FunctionDecl* instance = args.empty() || !def->body ? def : global.instanceBody(def, args, d.type);
      DebugValue v{global, symbols, def->file, instance, args, "", 0, d.type};
      v.asserts = std::make_shared<std::vector<std::pair<std::string, std::string>>>();
      std::vector<DebugCase> cases;
      auto cannot = [&]()
      {
        return error(v.atFile ? v.atFile : def->file, v.at ? v.at : def, "debugValue of " + d.name + " cannot be shown by the debugger: " + v.why);
      };
      if (!debugCases(v, instance, cases))
        return cannot();
      // What it returns: its result type.
      const Ty* shown = global.typeOfDeclIn(f, args);
      shown = shown && shown->kind == TyKind::Function && shown->inner ? types.unqualified(types.stripRef(shown->inner)) : nullptr;
      bool number = shown && (shown->isArithmetic() || shown->kind == TyKind::Enum || shown->kind == TyKind::Pointer);
      bool span = isLibraryClass(shown, symbols, "Span"); // (the library's Span)
      bool text = isLibraryClass(shown, symbols, "String"); // (a text view: its pieces)
      bool view = isLibraryClass(shown, symbols, "StringView"); // (characters: a pointer, or a pointer and a length)
      std::string displays, items;
      bool failed = false;
      std::string notBefore;
      for (const DebugCase& one : cases)
      {
        Expr* value = one.second;
        std::string when = one.text;
        if (one.first && !natvisCondition(v, one.first, when))
        {
          failed = true;
          break;
        }
        v.reached = reachedAnd(notBefore, one.conditional() ? when : std::string());
        if (one.conditional())
          notBefore = reachedAnd(notBefore, "!(" + when + ")");
        std::string attribute = one.conditional() ? " Condition=\"" + escape(when) + "\"" : std::string();
        Expr* pointer = nullptr;
        Expr* length = nullptr;
        if (span)
        {
          std::string data, size;
          if (!spanParts(value, pointer, length))
          {
            fail(v, value, "a Span other than one made of a pointer and a length (`Span(this.data, this.size)`)");
            failed = true;
            break;
          }
          if (!natvisExpression(v, pointer, data) || !natvisExpression(v, length, size))
          {
            failed = true;
            break;
          }
          if (length->ty && types.unqualified(types.stripRef(length->ty))->kind == TyKind::Pointer)
            size = "(" + size + ") - (" + data + ")"; // (Span(first, last))
          displays += "    <DisplayString" + attribute + ">{{length={" + escape(size) + "}}}</DisplayString>\n";
          items += "      <ArrayItems" + attribute + ">\n"
                   "        <Size>" + escape(size) + "</Size>\n"
                   "        <ValuePointer>" + escape(data) + "</ValuePointer>\n"
                   "      </ArrayItems>\n";
          continue;
        }
        if (view)
        {
          // std::StringView(chars) or std::StringView(pointer, length): the characters as text.
          Expr* made = value;
          while (made && made->kind == NodeKind::ParenExpr)
            made = static_cast<ParenExpr*>(made)->inner;
          Span<Expr*> args;
          if (made && made->kind == NodeKind::CallExpr)
            args = static_cast<CallExpr*>(made)->args;
          else if (made && made->kind == NodeKind::FunctionalCastExpr)
            args = static_cast<FunctionalCastExpr*>(made)->args;
          else if (made && made->kind == NodeKind::BraceInitExpr)
            args = static_cast<BraceInitExpr*>(made)->args;
          std::string chars, length;
          if (args.size() < 1 || args.size() > 2)
          {
            fail(v, value, "a StringView other than one made of characters, or of a pointer and a length (`std::StringView(this.text)`)");
            failed = true;
            break;
          }
          Expr* only = args.size() == 1 ? args[0] : nullptr;
          while (only && only->kind == NodeKind::ParenExpr)
            only = static_cast<ParenExpr*>(only)->inner;
          if (only && only->kind == NodeKind::LiteralExpr && static_cast<LiteralExpr*>(only)->literalKind == TokenKind::String)
          {
            // (`std::StringView("(shared)")`: the literal's text, as a text view's: natvis's expressions have no string
            // literals, Visual Studio shows `{"(shared)",s8b}` as an error)
            std::string text, doubled;
            auto* literal = static_cast<LiteralExpr*>(only);
            if (!literalText(literal->file ? literal->file : v.file, literal, text))
            {
              fail(v, only, "a string literal with a prefix or an escape other than \\\" \\\\ \\n \\t \\r \\x");
              failed = true;
              break;
            }
            for (char c : text)
              doubled += c == '{' || c == '}' ? std::string(2, c) : std::string(1, c);
            displays += "    <DisplayString" + attribute + ">" + escape(doubled) + "</DisplayString>\n";
            continue;
          }
          if (!natvisExpression(v, args[0], chars) || (args.size() == 2 && !natvisExpression(v, args[1], length)))
          {
            failed = true;
            break;
          }
          displays += "    <DisplayString" + attribute + ">{" + escape(chars) + (args.size() == 2 ? ",[" + escape(length) + "]s8b" : ",s8b") + "}</DisplayString>\n";
          continue;
        }
        if (text)
        {
          TextVariants variants{{"", ""}};
          if (!natvisText(v, value, variants))
          {
            failed = true;
            break;
          }
          for (const auto& [condition, display] : variants)
          {
            std::string all = simplifyCondition(bothConditions(when, condition));
            displays += "    <DisplayString" + (all.empty() ? std::string() : " Condition=\"" + escape(all) + "\"") + ">" + escape(display) + "</DisplayString>\n";
          }
          continue;
        }
        std::string expression;
        if (!natvisExpression(v, value, expression))
        {
          failed = true;
          break;
        }
        bool path = expression.find_first_of(" ()") == std::string::npos; // (a member, or a chain of them)
        if (!number && !(shown && shown->kind == TyKind::Class && path))
        {
          fail(v, value, "a value other than a number, a pointer, an enumeration, a data member that is a class or a Span: " +
                           (shown ? types.toString(shown, &symbols) : std::string("this")));
          failed = true;
          break;
        }
        displays += "    <DisplayString" + attribute + ">{" + escape(expression) + (shown->kind == TyKind::Float ? ",g" : "") + "}</DisplayString>\n";
        if (!number || (shown->kind == TyKind::Pointer && shown->inner && types.unqualified(shown->inner)->kind == TyKind::Class))
          items += "      <ExpandedItem" + attribute + ">" + escape(expression) + "</ExpandedItem>\n"; // (a class, or the object a class pointer points to)
      }
      if (failed)
        return cannot();
      // The asserts first: where one does not hold, the value is none (an ID's index outside its list).
      std::string assertsShown;
      for (const auto& [condition, text] : *v.asserts)
      {
        std::string doubled;
        for (char ch : text)
          doubled += ch == '{' || ch == '}' ? std::string(2, ch) : std::string(1, ch);
        std::string when = simplifyCondition(condition);
        if (when != "false")
          assertsShown += "    <DisplayString Condition=\"" + escape(when) + "\">" + escape(doubled) + "</DisplayString>\n";
      }
      out = "  <Type Name=\"" + escape(d.name) + "\">\n" + reachedDisplays(assertsShown + displays);
      if (!items.empty())
        out += "    <Expand>\n" + items + "    </Expand>\n";
      out += "  </Type>\n";
      return true;
    }
    return false;
  }
}

namespace
{
  // What a const member function without parameters returns on the object at v: an expression (a pointer:
  // Inventory's `data.get()`), or the object it makes (an iterator: CompactDeque's `iterator(&this, 0)`).
  bool returnedValue(DebugValue& v, Node* at, FunctionDecl* f, std::string& expression, std::shared_ptr<ObjectValue>& object)
  {
    TypeTable& types = v.global.types();
    InlinedCall c;
    if (!prepareCall(v, at, f, nullptr, nullptr, false, nullptr, c, nullptr))
      return false;
    DebugValue inner{v.global, v.symbols, c.def->file, c.instance, c.args, c.prefix, v.depth + 1, c.self};
    inner.bound = c.bound;
    inner.object = c.object;
    inner.asserts = v.asserts;
    inner.known = v.known;
    inner.reached = v.reached;
    std::vector<DebugCase> cases;
    bool done = debugCases(inner, c.instance, cases);
    if (done && (cases.size() != 1 || cases[0].conditional()))
      done = fail(inner, c.instance, "a body other than one `return EXPRESSION;`");
    if (done)
    {
      const Ty* r = cases[0].second->ty ? types.unqualified(types.stripRef(cases[0].second->ty)) : nullptr;
      if (r && r->kind == TyKind::Class)
        done = natvisObject(inner, cases[0].second, object);
      else if (natvisExpression(inner, cases[0].second, expression))
        expression = wrap(expression);
      else
        done = false;
    }
    if (!done && v.why.empty())
    {
      v.why = functionName(v.symbols, f) + ": " + inner.why;
      v.at = inner.at ? inner.at : at;
      v.atFile = inner.atFile;
    }
    return done;
  }

  // The member an expression names as the target of an assignment in the object's own code (`this.index`,
  // `index`), or empty.
  std::string assignedMember(const SymbolTable& symbols, Expr* e)
  {
    while (e && e->kind == NodeKind::ParenExpr)
      e = static_cast<ParenExpr*>(e)->inner;
    if (e && e->kind == NodeKind::MemberExpr && isThisObject(static_cast<MemberExpr*>(e)->base) && static_cast<MemberExpr*>(e)->op != P::Arrow)
      return std::string(symbols.name(static_cast<MemberExpr*>(e)->member.last().symbol));
    if (e && e->kind == NodeKind::NameExpr && static_cast<NameExpr*>(e)->resolved && static_cast<NameExpr*>(e)->resolved->kind == NodeKind::VariableDecl &&
        !(static_cast<NameExpr*>(e)->resolved->flags & DF_Static))
      return std::string(symbols.name(static_cast<NameExpr*>(e)->name.last().symbol));
    return {};
  }

  // An iterator's operator++ as natvis's steps: each statement an assignment to a member of the iterator
  // (`++this.index`, `this.current = this.current->next`), `return this;` at the end.
  bool iteratorStep(DebugValue& v, Node* at, FunctionDecl* f, const Ty* iterator, std::vector<std::string>& members, std::vector<std::string>* execs)
  {
    TemplateArgs args = v.global.classArgsOf(iterator);
    Decl* defined = args.empty() ? v.global.definitionOf(f) : v.global.definitionOf(f, args);
    auto* def = defined && defined->kind == NodeKind::FunctionDecl ? static_cast<FunctionDecl*>(defined) : f;
    FunctionDecl* instance = args.empty() ? def : v.global.instanceBody(def, args, iterator);
    if (!instance || !instance->body || instance->body->kind != NodeKind::BlockStmt)
      return fail(v, at, "an iterator's operator++ whose body is not here");
    DebugValue inner{v.global, v.symbols, def->file, instance, args, "", v.depth + 1, iterator};
    inner.object = v.object;
    inner.asserts = v.asserts;
    inner.known = v.known;
    for (Stmt* s : static_cast<BlockStmt*>(instance->body)->stmts)
    {
      if (s->kind == NodeKind::ReturnStmt && static_cast<ReturnStmt*>(s)->value && isThisObject(static_cast<ReturnStmt*>(s)->value))
        continue;
      Expr* e = s->kind == NodeKind::ExprStmt ? static_cast<ExprStmt*>(s)->expr : nullptr;
      std::string member, value;
      if (e && e->kind == NodeKind::UnaryExpr && !static_cast<UnaryExpr*>(e)->callee &&
          (P(static_cast<UnaryExpr*>(e)->op) == P::PlusPlus || P(static_cast<UnaryExpr*>(e)->op) == P::MinusMinus))
      {
        member = assignedMember(v.symbols, static_cast<UnaryExpr*>(e)->operand);
        if (!member.empty() && execs)
        {
          if (!memberOf(inner, e, member, value))
            return false;
          value += P(static_cast<UnaryExpr*>(e)->op) == P::PlusPlus ? " + 1" : " - 1";
        }
      }
      else if (e && e->kind == NodeKind::BinaryExpr && !static_cast<BinaryExpr*>(e)->callee &&
               (P(static_cast<BinaryExpr*>(e)->op) == P::Assign || P(static_cast<BinaryExpr*>(e)->op) == P::PlusAssign ||
                P(static_cast<BinaryExpr*>(e)->op) == P::MinusAssign))
      {
        auto* b = static_cast<BinaryExpr*>(e);
        member = assignedMember(v.symbols, b->lhs);
        if (!member.empty() && execs)
        {
          std::string current;
          if (!natvisExpression(inner, b->rhs, value))
            return false;
          if (P(b->op) != P::Assign)
          {
            if (!memberOf(inner, e, member, current))
              return false;
            value = current + (P(b->op) == P::PlusAssign ? " + " : " - ") + wrap(value);
          }
        }
      }
      if (member.empty())
        return fail(inner, s, "an iterator's operator++ with a statement other than an assignment to its member and `return this;`");
      if (std::find(members.begin(), members.end(), member) == members.end())
        members.push_back(member);
      if (execs)
        execs->push_back("__kpp_" + member + " = " + value);
    }
    return true;
  }

  // A class's data members as rows, its bases first (`[Base]`, the base's own view), an anonymous union's or
  // struct's members in their place: what Visual Studio shows of a class without a rule, before a container's
  // elements (kovarex, 2026-11-13: "Yes", Inventory's handPosition beside its stacks).
  void memberRows(GlobalScope& global, const SymbolTable& symbols, ClassDecl* cls, const TemplateArgs& args, bool bases, std::string& out)
  {
    TypeTable& types = global.types();
    for (const ClassDecl::Base& b : cls->bases)
    {
      const Ty* t = bases && b.type ? global.typeOfAstIn(b.type, cls, args) : nullptr;
      t = t ? types.unqualified(t) : nullptr;
      if (!t || t->kind != TyKind::Class)
        continue;
      std::string name = types.toString(t, &symbols, true);
      out += "      <Item Name=\"[" + escape(name) + "]\">*(" + escape(name) + "*)this</Item>\n";
    }
    for (Decl* m : cls->members)
    {
      if (m->kind == NodeKind::VariableDecl && !(m->flags & (DF_Static | DF_Typedef)))
      {
        std::string name = nameOf(static_cast<VariableDecl*>(m)->name, symbols);
        if (!name.empty())
          out += "      <Item Name=\"" + escape(name) + "\">" + escape(name) + "</Item>\n";
      }
      else if (m->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(m)->name.parts.empty())
        memberRows(global, symbols, static_cast<ClassDecl*>(m), args, false, out);
    }
  }

  // A container's elements (kovarex, 2026-11-13: "lets try C"): its own begin() and end(), const and without
  // parameters, and its iterator's ==, * and ++, as natvis's loop (CustomListItems): the iterator's members
  // that ++ changes are the loop's variables (`__kpp_index`), the others their begin() values; a pointer
  // iterator is one variable stepped by one. An assert on the way stops the loop where it fails. False with
  // why empty for a class without them.
  bool elementsRule(GlobalScope& global, const SymbolTable& symbols, const DebugClass& d, std::string& out, std::string& why)
  {
    TypeTable& types = global.types();
    if (!d.type || !d.type->decl || d.type->decl->kind != NodeKind::ClassDecl)
      return false;
    Decl* defined = global.definitionOf(d.type->decl);
    if (!defined || defined->kind != NodeKind::ClassDecl || !defined->file || defined->file->isImported || defined->file->isLibrary)
      return false;
    auto* cls = static_cast<ClassDecl*>(defined);
    if (!cls->flagBits.empty())
      return false;
    auto constWithout = [](FunctionDecl* f) { return (f->flags & DF_Const) && f->params.empty() && f->templateParams.empty(); };
    FunctionDecl* begin = memberFunction(global, symbols, d.type, "begin", P(0), constWithout);
    FunctionDecl* end = memberFunction(global, symbols, d.type, "end", P(0), constWithout);
    if (!begin || !end)
      return false;
    DebugValue v{global, symbols, cls->file, cls, global.classArgsOf(d.type), "", 0, d.type};
    v.asserts = std::make_shared<std::vector<std::pair<std::string, std::string>>>();
    auto failed = [&]
    {
      why = d.name + ": " + v.why;
      return false;
    };
    std::string beginValue, endValue, variables, condition, item;
    std::vector<std::string> execs;
    std::shared_ptr<ObjectValue> beginObject, endObject;
    if (!returnedValue(v, cls, begin, beginValue, beginObject) || !returnedValue(v, cls, end, endValue, endObject))
      return failed();
    if (!beginObject != !endObject || (beginObject && beginObject->type != endObject->type))
    {
      fail(v, begin, "begin() and end() of different types");
      return failed();
    }
    if (!beginObject)
    {
      const Ty* returned = global.typeOfDeclIn(begin, v.args);
      returned = returned && returned->kind == TyKind::Function && returned->inner ? types.unqualified(types.stripRef(returned->inner)) : nullptr;
      if (!returned || returned->kind != TyKind::Pointer)
      {
        fail(v, begin, "a begin() returning neither a pointer nor an iterator object");
        return failed();
      }
      variables = "        <Variable Name=\"__kpp_it\" InitialValue=\"" + escape(beginValue) + "\"/>\n";
      condition = "__kpp_it == " + endValue;
      item = "*__kpp_it";
      execs.push_back("__kpp_it = __kpp_it + 1");
    }
    else
    {
      const Ty* iterator = beginObject->type;
      FunctionDecl* step = memberFunction(global, symbols, iterator, nullptr, P::PlusPlus, [](FunctionDecl* f) { return f->params.empty(); });
      FunctionDecl* equal = memberFunction(global, symbols, iterator, nullptr, P::EqEq, [&](FunctionDecl* f)
      {
        const Ty* t = f->params.size() == 1 ? global.typeOfDeclIn(f->params[0], global.classArgsOf(iterator)) : nullptr;
        return (f->flags & DF_Const) && t && types.unqualified(types.stripRef(t)) == iterator;
      });
      FunctionDecl* star = memberFunction(global, symbols, iterator, nullptr, P::Star, [](FunctionDecl* f) { return (f->flags & DF_Const) && f->params.empty(); });
      if (!step || !equal || !star)
      {
        fail(v, begin, std::string("an iterator without ") + (!step ? "one operator++()" : !equal ? "one const operator== of its own type" : "one const operator*()"));
        return failed();
      }
      std::vector<std::string> changed;
      if (!iteratorStep(v, step, step, iterator, changed, nullptr))
        return failed();
      // the loop's object: a variable for each member ++ changes, begin()'s value for the others
      auto loop = std::make_shared<ObjectValue>();
      loop->type = iterator;
      for (const std::string& member : changed)
      {
        auto initial = std::find_if(beginObject->members.begin(), beginObject->members.end(), [&](const auto& m) { return m.first == member; });
        if (initial == beginObject->members.end())
        {
          fail(v, step, "an iterator member begin() does not set: " + member);
          return failed();
        }
        variables += "        <Variable Name=\"__kpp_" + member + "\" InitialValue=\"" + escape(initial->second) + "\"/>\n";
        loop->members.push_back({member, "__kpp_" + member});
      }
      for (const auto& [member, value] : beginObject->members)
        if (std::find(changed.begin(), changed.end(), member) == changed.end())
          loop->members.push_back({member, value});
      DebugValue at{global, symbols, cls->file, cls, global.classArgsOf(iterator), "", 0, iterator};
      at.object = loop;
      at.asserts = v.asserts;
      std::vector<Bound> other(1);
      other[0].object = endObject;
      if (!inlinedMember(at, equal, equal, nullptr, false, false, condition, nullptr, nullptr, &other) ||
          !inlinedMember(at, star, star, nullptr, false, false, item, nullptr, nullptr, nullptr) || !iteratorStep(at, step, step, iterator, changed, &execs))
      {
        v.why = at.why;
        return failed();
      }
    }
    std::string breaks = "          <Break Condition=\"" + escape(simplifyCondition(condition)) + "\"/>\n";
    for (const auto& [failing, text] : *v.asserts)
    {
      std::string when = simplifyCondition(failing);
      if (when != "false")
        breaks += "          <Break Condition=\"" + escape(when) + "\"/> <!-- " + escape(text) + " -->\n";
    }
    out.clear();
    memberRows(global, symbols, cls, global.classArgsOf(d.type), true, out);
    out += "      <CustomListItems MaxItemsPerView=\"5000\">\n" + variables + "        <Loop>\n" + breaks + "          <Item>" + escape(item) + "</Item>\n";
    for (const std::string& exec : execs)
      out += "          <Exec>" + escape(exec) + "</Exec>\n";
    out += "        </Loop>\n      </CustomListItems>\n";
    return true;
  }
}

std::string makeNatvis(const std::vector<ParsedFile*>& files, GlobalScope& global, const SymbolTable& symbols, const std::vector<DebugClass>& classes,
                       std::vector<std::string>& problems)
{
  // The library's class definitions at namespace scope, by name.
  // A specialization (`Vector<bool>`) is named with /spec<token> in the PDB (B's names): a wildcard
  // rule misses it, so one of a class with a rule is a problem.
  std::map<std::string, std::pair<const ClassDecl*, const ParsedFile*>> library;
  std::set<std::string> specialized;
  std::function<void(Span<Decl*>, const ParsedFile*)> collect = [&](Span<Decl*> decls, const ParsedFile* file)
  {
    for (const Decl* d : decls)
      if (d->kind == NodeKind::ClassDecl && !static_cast<const ClassDecl*>(d)->name.parts.empty() &&
          static_cast<const ClassDecl*>(d)->name.parts[static_cast<const ClassDecl*>(d)->name.parts.size() - 1].hasTemplateArgs)
        specialized.insert(nameOf(static_cast<const ClassDecl*>(d)->name, symbols));
      else if (d->kind == NodeKind::ClassDecl && static_cast<const ClassDecl*>(d)->isDefinition)
        library.emplace(nameOf(static_cast<const ClassDecl*>(d)->name, symbols), std::make_pair(static_cast<const ClassDecl*>(d), file));
      else if (d->kind == NodeKind::NamespaceDecl)
        collect(static_cast<const NamespaceDecl*>(d)->decls, file);
  };
  bool scoped = false; // the standard library is the scoped library std
  for (const ParsedFile* f : files)
    if (f->isLibrary)
    {
      collect(f->decls, f);
      scoped = scoped || f->libraryScope != UINT32_MAX;
    }

  std::set<std::string> broken; // rules left out for what they compute: by their first class
  std::string largeMark;
  if (auto s = library.find("String"); s == library.end() || !s->second.second->tokens ||
                                       !literalOf(s->second.first, "largeMark", symbols, *s->second.second->tokens, s->second.second->text, largeMark))
  {
    problems.push_back("String's largeMark is no number literal: the String rule is left out");
    broken.insert("String");
  }

  // `mark` is a String's last byte: a small string's length, or largeMark for one on the heap.
  const std::string mark = "(unsigned char)smallChars[sizeof(smallChars) - 1]";
  // (written out, no <Intrinsic>: a natvis file given to an evaluation by name, as Visual Studio's kpp evaluator
  // does, does not apply a type that has one; docs/debugging.md, "this in the watch window")
  const std::string large = "(" + mark + " == " + largeMark + ")";
  const std::string block = "(sizeof(**map) &gt; 16 ? 16 : sizeof(**map) &gt; 8 ? 32 : sizeof(**map) &gt; 4 ? 64 : "
                            "sizeof(**map) &gt; 2 ? 128 : sizeof(**map) &gt; 1 ? 256 : 512)";
  std::vector<Rule> rules = {
    {{"String"}, {"smallChars", "heapChars", "heapLength", "heapCapacity"},
     "  <Type Name=\"String\">\n"
     "    <DisplayString Condition=\"" + large + "\">{heapChars,[heapLength]s8}</DisplayString>\n"
     "    <DisplayString>{smallChars,[" + mark + "]s8}</DisplayString>\n"
     "    <StringView Condition=\"" + large + "\">heapChars,[heapLength]s8</StringView>\n"
     "    <StringView>smallChars,[" + mark + "]s8</StringView>\n"
     "    <Expand>\n"
     "      <Item Name=\"[length]\" Condition=\"" + large + "\">heapLength</Item>\n"
     "      <Item Name=\"[length]\" Condition=\"!" + large + "\">(int)" + mark + "</Item>\n"
     "      <Item Name=\"[capacity]\" Condition=\"" + large + "\">heapCapacity</Item>\n"
     "    </Expand>\n"
     "  </Type>\n"},
    {{"StringView"}, {"chars", "byteCount"},
     "  <Type Name=\"StringView\">\n"
     "    <DisplayString>{chars,[byteCount]s8}</DisplayString>\n"
     "    <StringView>chars,[byteCount]s8</StringView>\n"
     "    <Expand>\n"
     "      <Item Name=\"[length]\">byteCount</Item>\n"
     "    </Expand>\n"
     "  </Type>\n"},
    {{"Vector"}, {"items", "length", "allocated"},
     "  <Type Name=\"Vector&lt;*&gt;\">\n"
     "    <DisplayString>{{ length={length} }}</DisplayString>\n"
     "    <Expand>\n"
     "      <Item Name=\"[capacity]\" ExcludeView=\"simple\">allocated</Item>\n"
     "      <ArrayItems>\n"
     "        <Size>length</Size>\n"
     "        <ValuePointer>items</ValuePointer>\n"
     "      </ArrayItems>\n"
     "    </Expand>\n"
     "  </Type>\n"},
    {{"Span"}, {"items", "length"},
     "  <Type Name=\"Span&lt;*&gt;\">\n"
     "    <DisplayString>{{ length={length} }}</DisplayString>\n"
     "    <Expand>\n"
     "      <ArrayItems>\n"
     "        <Size>length</Size>\n"
     "        <ValuePointer>items</ValuePointer>\n"
     "      </ArrayItems>\n"
     "    </Expand>\n"
     "  </Type>\n"},
    // SmartPointer: `->` and `*` in a watch expression reach the object, as for std::unique_ptr
    // (`entity->controlBehavior->readCharge`, controlBehavior a T^).
    {{"UniquePtr"}, {"held"},
     "  <Type Name=\"UniquePtr&lt;*&gt;\">\n"
     "    <SmartPointer Usage=\"Minimal\">held</SmartPointer>\n"
     "    <DisplayString Condition=\"held == 0\">empty</DisplayString>\n"
     "    <DisplayString>{*held}</DisplayString>\n"
     "    <Expand>\n"
     "      <ExpandedItem Condition=\"held != 0\">*held</ExpandedItem>\n"
     "    </Expand>\n"
     "  </Type>\n"},
    // A SharedControl's strong count is the owners'; its weak one the weak references' plus one
    // while an owner lives (sharedReleaseWeak).
    {{"SharedPtr"}, {"pointer", "control"},
     "  <Type Name=\"SharedPtr&lt;*&gt;\">\n"
     "    <SmartPointer Usage=\"Minimal\">pointer</SmartPointer>\n"
     "    <DisplayString Condition=\"pointer == 0\">empty</DisplayString>\n"
     "    <DisplayString Condition=\"control == 0\">{*pointer}</DisplayString>\n"
     "    <DisplayString>{*pointer} [{control-&gt;strong.value} strong]</DisplayString>\n"
     "    <Expand>\n"
     "      <Item Name=\"[use count]\" Condition=\"control != 0\">control-&gt;strong.value</Item>\n"
     "      <Item Name=\"[weak count]\" Condition=\"control != 0\">control-&gt;weak.value - (control-&gt;strong.value != 0)</Item>\n"
     "      <ExpandedItem Condition=\"pointer != 0\">*pointer</ExpandedItem>\n"
     "    </Expand>\n"
     "  </Type>\n",
     {{"SharedControl", {"strong", "weak"}}, {"Atomic", {"value"}}}},
    {{"WeakPtr"}, {"pointer", "control"},
     "  <Type Name=\"WeakPtr&lt;*&gt;\">\n"
     "    <DisplayString Condition=\"control == 0\">empty</DisplayString>\n"
     "    <DisplayString Condition=\"control-&gt;strong.value == 0\">expired</DisplayString>\n"
     "    <DisplayString>{*pointer} [{control-&gt;strong.value} strong]</DisplayString>\n"
     "    <Expand>\n"
     "      <Item Name=\"[use count]\" Condition=\"control != 0\">control-&gt;strong.value</Item>\n"
     "      <ExpandedItem Condition=\"control != 0 &amp;&amp; control-&gt;strong.value != 0\">*pointer</ExpandedItem>\n"
     "    </Expand>\n"
     "  </Type>\n",
     {{"SharedControl", {"strong", "weak"}}, {"Atomic", {"value"}}}},
    {{"Map", "Set", "MultiMap", "MultiSet"}, {"tree"},
     "  <Type Name=\"Map&lt;*&gt;\">\n"
     "    <DisplayString>{tree}</DisplayString>\n"
     "    <Expand>\n"
     "      <ExpandedItem>tree</ExpandedItem>\n"
     "    </Expand>\n"
     "  </Type>\n"
     "  <Type Name=\"Set&lt;*&gt;\">\n"
     "    <DisplayString>{tree}</DisplayString>\n"
     "    <Expand>\n"
     "      <ExpandedItem>tree</ExpandedItem>\n"
     "    </Expand>\n"
     "  </Type>\n"
     "  <Type Name=\"MultiMap&lt;*&gt;\">\n"
     "    <DisplayString>{tree}</DisplayString>\n"
     "    <Expand>\n"
     "      <ExpandedItem>tree</ExpandedItem>\n"
     "    </Expand>\n"
     "  </Type>\n"
     "  <Type Name=\"MultiSet&lt;*&gt;\">\n"
     "    <DisplayString>{tree}</DisplayString>\n"
     "    <Expand>\n"
     "      <ExpandedItem>tree</ExpandedItem>\n"
     "    </Expand>\n"
     "  </Type>\n"},
    {{"OptionalTrivialValue", "OptionalCopiedValue", "OptionalOwnedValue"}, {"storage", "present"},
     "  <Type Name=\"Optional&lt;*&gt;\">\n"
     "    <DisplayString Condition=\"!present\">empty</DisplayString>\n"
     "    <DisplayString>{storage.value}</DisplayString>\n"
     "    <Expand>\n"
     "      <ExpandedItem Condition=\"present\">storage.value</ExpandedItem>\n"
     "    </Expand>\n"
     "  </Type>\n"},
    {{"Pair"}, {"first", "second"},
     "  <Type Name=\"Pair&lt;*&gt;\">\n"
     "    <DisplayString>({first}, {second})</DisplayString>\n"
     "  </Type>\n"},
    {{"HashMap", "HashSet"}, {"table"},
     "  <Type Name=\"HashMap&lt;*&gt;\">\n"
     "    <DisplayString>{table}</DisplayString>\n"
     "    <Expand>\n"
     "      <ExpandedItem>table</ExpandedItem>\n"
     "    </Expand>\n"
     "  </Type>\n"
     "  <Type Name=\"HashSet&lt;*&gt;\">\n"
     "    <DisplayString>{table}</DisplayString>\n"
     "    <Expand>\n"
     "      <ExpandedItem>table</ExpandedItem>\n"
     "    </Expand>\n"
     "  </Type>\n"},
    {{"HashTable"}, {"tags", "slots", "capacity", "count"}, ""},
    // The rules of these are per instance (below): a node's cast names its element type.
    {{"List"}, {"sentinel", "length"}, "", {{"ListLinks", {"next"}}, {"ListNode", {"value"}}}},
    {{"ForwardList"}, {"beforeHead"}, "", {{"ForwardLinks", {"next"}}, {"ForwardNode", {"value"}}}},
    {{"RbTree"}, {"header", "count"}, "", {{"RbNodeBase", {"parent", "left", "right"}}, {"RbNode", {"entry"}}}},
    // Element i is at position start + i of the map's space: block (start + i) / blockSize() at
    // that offset. `block` is Deque::blockSize() (checked below), sizeof(T) taken as sizeof(**map).
    {{"Deque"}, {"map", "start", "length"},
     "  <Type Name=\"Deque&lt;*&gt;\">\n"
     "    <DisplayString>{{ length={length} }}</DisplayString>\n"
     "    <Expand>\n"
     "      <IndexListItems>\n"
     "        <Size>length</Size>\n"
     "        <ValueNode>map[(start + $i) / " + block + "][(start + $i) % " + block + "]</ValueNode>\n"
     "      </IndexListItems>\n"
     "    </Expand>\n"
     "  </Type>\n"},
  };
  // The Deque rule's `block` is this blockSize: elements per block, about 512 bytes' worth, a power
  // of two, at least 16.
  if (auto d = library.find("Deque"); d != library.end())
  {
    const ParsedFile* file = d->second.second;
    std::string found;
    for (const Decl* m : d->second.first->members)
      if (m->kind == NodeKind::FunctionDecl && nameOf(static_cast<const FunctionDecl*>(m)->name, symbols) == "blockSize" && file->tokens)
        found = tokenText(m, *file->tokens, file->text);
    const std::string expected = "static i32 blockSize ( ) { i32 size = 16 ; while ( ( usize ) size * 2 * sizeof ( T ) <= 512 ) size *= 2 ; return size ; }";
    if (found != expected)
    {
      problems.push_back("Deque's blockSize is not the one its rule computes (" + found + "): the Deque rule is left out");
      broken.insert("Deque");
    }
  }

  std::string out = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                    "<!-- Written by kpp for a build with debug information (docs/debugging.md, \"Values in Visual Studio\"). -->\n"
                    "<AutoVisualizer xmlns=\"http://schemas.microsoft.com/vstudio/debugger/natvis/2010\">\n";
  std::set<std::string> ruleOut; // classes whose rule is left out
  for (const Rule& r : rules)
  {
    bool ok = !broken.count(r.classes[0]);
    // The classes it names, with its members, then those it reads through.
    std::vector<std::pair<std::string, std::vector<std::string>>> checked;
    for (const std::string& cls : r.classes)
    {
      if (specialized.count(cls))
        problems.push_back("the library specializes " + cls + ", whose rule names only its primary template: add a rule for the specialization");
      checked.emplace_back(cls, r.members);
    }
    checked.insert(checked.end(), r.reads.begin(), r.reads.end());
    for (const auto& [cls, read] : checked)
    {
      auto found = library.find(cls);
      if (found == library.end())
      {
        problems.push_back("the library has no class " + cls + ": the rule of " + r.classes[0] + " is left out");
        ok = false;
        continue;
      }
      std::set<std::string> members;
      dataMembers(found->second.first, symbols, members);
      for (const std::string& m : read)
        if (!members.count(m))
        {
          problems.push_back(cls + " has no data member " + m + ": the rule of " + r.classes[0] + " is left out");
          ok = false;
        }
    }
    if (ok)
    {
      // (the classes are std::String to the debugger when the library is scoped, design.md 16; a rule per instance below has its name from the debug information)
      std::string xml = r.xml;
      const std::string opening = "<Type Name=\"";
      for (size_t at = xml.find(opening); scoped && at != std::string::npos; at = xml.find(opening, at + 1))
        xml.insert(at + opening.size(), "std::");
      out += xml;
    }
    else
      ruleOut.insert(r.classes.begin(), r.classes.end());
  }

  // Rules per instance of a library class, whose casts name its element's type: of the classes the
  // debug information defines, by the names it gives them (CodegenResult::debugClasses, sorted by
  // name), so that a rule and the PDB agree by construction. (natvis matches a type by its
  // unqualified name.)
  TypeTable& types = global.types();
  std::unordered_map<const Ty*, const std::string*> named;
  for (const DebugClass& c : classes)
    named.emplace(c.type, &c.name);
  // A type's name in the PDB: a class's as the debug information gives it, a pointer's its pointee's
  // with `*`, the rest kpp's.
  std::function<std::string(const Ty*)> typeName = [&](const Ty* t)
  {
    if (auto n = named.find(t); n != named.end())
      return *n->second;
    if (t->kind == TyKind::Pointer && t->inner && !t->isConst && !t->isVolatile)
      return typeName(t->inner) + "*";
    return types.toString(t, &symbols, true);
  };
  auto instancesOf = [&](const std::string& cls, size_t arity)
  {
    std::vector<const DebugClass*> found;
    auto lib = library.find(cls);
    if (lib == library.end() || ruleOut.count(cls))
      return found;
    for (const DebugClass& c : classes)
      if (c.type->decl == lib->second.first && c.type->args.size() == arity)
        found.push_back(&c);
    return found;
  };
  // The name of the library's node class `cls` of element `element` (ListNode<T>): empty when the
  // debug information has none, so no node of it either.
  auto nodeName = [&](const std::string& cls, const Ty* element)
  {
    auto lib = library.find(cls);
    if (lib != library.end())
      for (const DebugClass& c : classes)
        if (c.type->decl == lib->second.first && c.type->args.size() == 1 && c.type->args[0] == element)
          return c.name;
    return std::string();
  };

  // A HashTable's entries: in its slots (E) or behind them (E*, stable), where its tag is 0x80 or
  // above (HashIterator::skipFree).
  for (const DebugClass* c : instancesOf("HashTable", 6))
  {
    const Ty* t = c->type;
    std::string entry = typeName(t->args[0]);
    bool stable = t->args[5]->kind == TyKind::Value && t->args[5]->value != 0;
    std::string item = stable ? "*((" + entry + "**)slots)[i]" : "((" + entry + "*)slots)[i]";
    out += "  <Type Name=\"" + escape(c->name) + "\">\n"
           "    <DisplayString>{{ count={count} }}</DisplayString>\n"
           "    <Expand>\n"
           "      <Item Name=\"[capacity]\" ExcludeView=\"simple\">capacity</Item>\n"
           "      <CustomListItems MaxItemsPerView=\"5000\">\n"
           "        <Variable Name=\"i\" InitialValue=\"0\"/>\n"
           "        <Loop Condition=\"i &lt; capacity\">\n"
           "          <If Condition=\"tags[i] &gt;= 0x80\">\n"
           "            <Item>" + escape(item) + "</Item>\n"
           "          </If>\n"
           "          <Exec>i++</Exec>\n"
           "        </Loop>\n"
           "      </CustomListItems>\n"
           "    </Expand>\n"
           "  </Type>\n";
  }
  // A UniquePtr with its default deleter is named `Item^` (Emitter::debuggerClassName), which the
  // rule for UniquePtr<*> does not match: the same rule by each such name.
  for (const DebugClass* c : instancesOf("UniquePtr", 2))
    if (!c->name.empty() && c->name.back() == '^')
      out += "  <Type Name=\"" + escape(c->name) + "\">\n"
             "    <SmartPointer Usage=\"Minimal\">held</SmartPointer>\n"
             "    <DisplayString Condition=\"held == 0\">empty</DisplayString>\n"
             "    <DisplayString>{*held}</DisplayString>\n"
             "    <Expand>\n"
             "      <ExpandedItem Condition=\"held != 0\">*held</ExpandedItem>\n"
             "    </Expand>\n"
             "  </Type>\n";
  // A List's nodes: a ring through its sentinel, `length` of them after it.
  for (const DebugClass* c : instancesOf("List", 1))
  {
    std::string node = escape(nodeName("ListNode", c->type->args[0]));
    out += "  <Type Name=\"" + escape(c->name) + "\">\n"
           "    <DisplayString>{{ length={length} }}</DisplayString>\n";
    if (!node.empty())
      out += "    <Expand>\n"
             "      <LinkedListItems>\n"
             "        <Size>length</Size>\n"
             "        <HeadPointer>(" + node + "*)sentinel.next</HeadPointer>\n"
             "        <NextPointer>(" + node + "*)next</NextPointer>\n"
             "        <ValueNode>value</ValueNode>\n"
             "      </LinkedListItems>\n"
             "    </Expand>\n";
    out += "  </Type>\n";
  }
  // A ForwardList's nodes: from beforeHead.next to a null next.
  for (const DebugClass* c : instancesOf("ForwardList", 1))
  {
    std::string node = escape(nodeName("ForwardNode", c->type->args[0]));
    out += "  <Type Name=\"" + escape(c->name) + "\">\n"
           "    <DisplayString Condition=\"beforeHead.next == 0\">empty</DisplayString>\n";
    if (!node.empty())
      out += "    <DisplayString>{{ front={((" + node + "*)beforeHead.next)-&gt;value} }}</DisplayString>\n"
             "    <Expand>\n"
             "      <LinkedListItems>\n"
             "        <HeadPointer>(" + node + "*)beforeHead.next</HeadPointer>\n"
             "        <NextPointer>(" + node + "*)next</NextPointer>\n"
             "        <ValueNode>value</ValueNode>\n"
             "      </LinkedListItems>\n"
             "    </Expand>\n";
    out += "  </Type>\n";
  }
  // An RbTree's (Map's, Set's, MultiMap's, MultiSet's) entries in order: its root is header.parent,
  // a missing child null.
  for (const DebugClass* c : instancesOf("RbTree", 4))
  {
    std::string node = escape(nodeName("RbNode", c->type->args[0]));
    out += "  <Type Name=\"" + escape(c->name) + "\">\n"
           "    <DisplayString>{{ count={count} }}</DisplayString>\n";
    if (!node.empty())
      out += "    <Expand>\n"
             "      <TreeItems>\n"
             "        <Size>count</Size>\n"
             "        <HeadPointer>(" + node + "*)header.parent</HeadPointer>\n"
             "        <LeftPointer>(" + node + "*)left</LeftPointer>\n"
             "        <RightPointer>(" + node + "*)right</RightPointer>\n"
             "        <ValueNode>entry</ValueNode>\n"
             "      </TreeItems>\n"
             "    </Expand>\n";
    out += "  </Type>\n";
  }
  // A typed union (`union enum`, design.md 8.5) as its active alternative: its tag (ClassDecl::tagField,
  // valued in the alternatives' order) and each alternative's storage (ClassDecl::alternatives), by
  // the names the parser gave them, which the PDB's are. One rule per class, and per instance of one
  // in a template.
  {
    std::set<const ClassDecl*> unions;
    std::function<void(Span<Decl*>)> find = [&](Span<Decl*> decls)
    {
      for (const Decl* d : decls)
        if (d->kind == NodeKind::NamespaceDecl)
          find(static_cast<const NamespaceDecl*>(d)->decls);
        else if (d->kind == NodeKind::ClassDecl)
        {
          auto* c = static_cast<const ClassDecl*>(d);
          if (c->isTypedUnion && c->isDefinition && c->tagField)
            unions.insert(c);
          find(c->members);
        }
    };
    for (const ParsedFile* f : files)
      find(f->decls);
    for (const DebugClass& d : classes)
    {
      if (!d.type->decl || d.type->decl->kind != NodeKind::ClassDecl || !unions.count(static_cast<const ClassDecl*>(d.type->decl)))
        continue;
      auto* c = static_cast<const ClassDecl*>(d.type->decl);
      std::string tag = nameOf(static_cast<const VariableDecl*>(c->tagField)->name, symbols);
      std::string shown, items;
      for (size_t i = 0; i < c->alternatives.size(); ++i)
      {
        std::string storage = nameOf(static_cast<const VariableDecl*>(c->alternatives[i])->name, symbols);
        std::string alternative = storage.rfind("__alt_", 0) == 0 ? storage.substr(6) : storage;
        std::string when = "(int)" + tag + " == " + std::to_string(i);
        shown += "    <DisplayString Condition=\"" + when + "\">" + escape(alternative) + " {" + storage + "}</DisplayString>\n";
        items += "      <Item Name=\"" + escape(alternative) + "\" Condition=\"" + when + "\">" + storage + "</Item>\n";
      }
      out += "  <Type Name=\"" + escape(d.name) + "\">\n" + shown +
             "    <Expand>\n"
             "      <Item Name=\"[tag]\">" + tag + "</Item>\n" + items +
             "    </Expand>\n"
             "  </Type>\n";
    }
  }
  // A type's debugValue() (DebugValue above): its rule, one per instance (debugValueRule); a container's
  // elements (elementsRule) in its Expand.
  for (const DebugClass& d : classes)
  {
    std::string rule, error, elements, why;
    bool shown = debugValueRule(global, symbols, d, rule, error);
    if (elementsRule(global, symbols, d, elements, why))
    {
      if (!shown)
        rule = "  <Type Name=\"" + escape(d.name) + "\">\n    <Expand>\n" + elements + "    </Expand>\n  </Type>\n";
      else if (size_t at = rule.rfind("    </Expand>\n"); at != std::string::npos)
        rule.insert(at, elements);
      else
        rule.insert(rule.rfind("  </Type>\n"), "    <Expand>\n" + elements + "    </Expand>\n");
      shown = true;
    }
    else if (!why.empty() && getenv("KPP_NATVIS_ELEMENTS"))
      fprintf(stderr, "natvis elements: %s\n", why.c_str());
    if (shown)
      out += rule;
  }
  out += "</AutoVisualizer>\n";
  return out;
}

std::vector<std::string> debugValueErrors(GlobalScope& global, const SymbolTable& symbols, const std::vector<DebugClass>& classes)
{
  std::vector<std::string> errors;
  for (const DebugClass& d : classes)
  {
    std::string rule, error;
    if (!debugValueRule(global, symbols, d, rule, error) && !error.empty() && std::find(errors.begin(), errors.end(), error) == errors.end())
      errors.push_back(error);
  }
  return errors;
}
