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
  };

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
      return d && d->kind == NodeKind::VariableDecl && !(d->flags & DF_Static) && d->parent && d->parent->kind == NodeKind::ClassDecl;
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

  // The body of a debugValue as natvis cases (DebugCase), the last one unconditional: `if (C) return E;`s,
  // a switch whose cases return (`case A: return E;`, `default:` last), a final `return E;`. False, with v's
  // why, for a body of another shape.
  bool debugCases(DebugValue& v, FunctionDecl* f, std::vector<DebugCase>& cases, bool* endsNowhere = nullptr)
  {
    if (!f || !f->body || f->body->kind != NodeKind::BlockStmt)
      return fail(v, f, "it has no body here");
    bool noReturnEnd = false;
    for (Stmt* s : static_cast<BlockStmt*>(f->body)->stmts)
    {
      if ((!cases.empty() && !cases.back().conditional()) || noReturnEnd)
        return fail(v, s, "a statement after its last return");
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
          }
          labels.clear();
          isDefault = false;
        }
        if (!labels.empty() || isDefault)
          return fail(v, s, "a case label without its `return EXPRESSION;`");
        if (fallback)
          cases.push_back({nullptr, fallback});
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
    if (cases.empty() || (cases.back().conditional() && !(noReturnEnd && endsNowhere)))
      return fail(v, f, "no `return EXPRESSION;` at its end");
    if (endsNowhere)
      *endsNowhere = cases.back().conditional();
    return true;
  }

  bool natvisText(DebugValue& v, Expr* e, std::string& out);
  bool natvisCondition(DebugValue& v, Expr* e, std::string& out);
  bool literalText(const ParsedFile* f, const LiteralExpr* e, std::string& out);

  // A const member function's body inlined on an object (the call's object, `this.toDouble()`, or a member,
  // `this.speed.debugValue()`; a conversion to bool on a class value): one `return EXPRESSION;` after
  // constexpr locals, as an expression, or as text (natvisText) for a function returning a String.
  bool inlinedMember(DebugValue& v, Node* at, FunctionDecl* called, Expr* objectExpr, bool arrow, bool text, std::string& out)
  {
    if (!(called->flags & DF_Const) || (called->flags & (DF_Static | DF_Virtual | DF_Override)) || !called->templateParams.empty() || !called->params.empty())
      return fail(v, at, "a call of a function other than a const, non-virtual member function without parameters");
    std::string base;
    const Ty* object = nullptr;
    TypeTable& types = v.global.types();
    if (!objectExpr || isThisObject(objectExpr))
    {
      if (isDebugValue(called, v.symbols))
        return fail(v, at, "a call of its own debugValue");
      object = v.self;
    }
    else
    {
      if (!natvisExpression(v, objectExpr, base))
        return false;
      base += arrow ? "->" : ".";
      object = objectExpr->ty;
      if (object)
        object = types.unqualified(types.stripRef(object));
      if (object && arrow && object->kind == TyKind::Pointer)
        object = object->inner ? types.unqualified(object->inner) : nullptr;
    }
    Node* c = at;
    if (!object || object->kind != TyKind::Class || !object->decl)
      return fail(v, c, "a call on something other than the object or a member that is a class");
    if (v.depth > 8)
      return fail(v, c, "calls nested deeper than 8");
    TemplateArgs args = v.global.classArgsOf(object);
    Decl* declared = called;
    Decl* defined = args.empty() ? v.global.definitionOf(declared) : v.global.definitionOf(declared, args);
    auto* def = defined && defined->kind == NodeKind::FunctionDecl ? static_cast<FunctionDecl*>(defined) : static_cast<FunctionDecl*>(declared);
    FunctionDecl* instance = args.empty() ? def : v.global.instanceBody(def, args, object);
    DebugValue inner{v.global, v.symbols, def->file, instance, args, v.prefix + base, v.depth + 1, object};
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
      // char pointer's `"?"` for the values it has no case for (Comparison::str()'s switch).
      std::string rest;
      if (endsNowhere)
      {
        const Ty* returned = v.global.typeOfDeclIn(called, args);
        const Ty* pointee = returned && returned->kind == TyKind::Function && returned->inner && returned->inner->kind == TyKind::Pointer && returned->inner->inner
                              ? types.unqualified(returned->inner->inner)
                              : nullptr;
        if (!pointee || pointee->kind != TyKind::Char)
          done = fail(inner, instance, "an end without a return, which only a function returning characters may have (\"?\" then)");
        rest = "\"?\"";
      }
      for (size_t i = cases.size(); done && i-- > 0;)
      {
        std::string one, when = cases[i].text;
        if (!natvisExpression(inner, cases[i].second, one) || (cases[i].first && !natvisCondition(inner, cases[i].first, when)))
        {
          done = false;
          break;
        }
        rest = cases[i].conditional() ? (rest.empty() ? one : "(" + when + " ? " + one + " : " + rest + ")") : one;
      }
      if (done)
      {
        out = "(" + rest + ")";
        return true;
      }
    }
    if (v.why.empty())
    {
      v.why = "the function it calls, " + std::string(v.symbols.name(called->name.last().symbol)) + ": " + inner.why;
      v.at = inner.at;
      v.atFile = inner.atFile;
    }
    return false;
  }

  bool inlinedCall(DebugValue& v, CallExpr* c, std::string& out)
  {
    auto* m = static_cast<MemberExpr*>(c->callee);
    return inlinedMember(v, c, static_cast<FunctionDecl*>(m->resolved), m->base, m->op == P::Arrow, false, out);
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
    out = reference ? "(*(" + type + ")" + inner + ")" : "(" + type + ")(" + inner + ")";
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
      out = "(" + inner + ")";
      return true;
    }
    case NodeKind::NameExpr: // a data member, `this.` implicit
    {
      auto* n = static_cast<NameExpr*>(e);
      out = v.prefix + std::string(v.symbols.name(n->name.last().symbol));
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
        out = v.prefix + name;
        return true;
      }
      std::string base;
      if (!natvisExpression(v, m->base, base))
        return false;
      out = base + (m->op == P::Arrow ? "->" : ".") + name;
      return true;
    }
    case NodeKind::UnaryExpr:
    {
      auto* u = static_cast<UnaryExpr*>(e);
      if (u->implicitThis)
        return fail(v, e, "the object itself, which has no value to show");
      if (u->callee || u->call == OperatorCall::Function || u->postfix || u->isKeyword)
        return fail(v, e, "an operator that calls a function or changes a value");
      P op = P(u->op);
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
      // A const member function's call, inlined: the object's own (`this.toDouble()`) or a member's.
      if (c->callee && c->callee->kind == NodeKind::MemberExpr && c->args.empty() && static_cast<MemberExpr*>(c->callee)->resolved &&
          static_cast<MemberExpr*>(c->callee)->resolved->kind == NodeKind::FunctionDecl)
        return inlinedCall(v, c, out);
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
  bool inlinedText(DebugValue& v, Node* at, FunctionDecl* called, Expr* objectExpr, bool arrow, TextVariants& out)
  {
    if (!(called->flags & DF_Const) || (called->flags & (DF_Static | DF_Virtual | DF_Override)) || !called->templateParams.empty() || !called->params.empty())
      return fail(v, at, "a call of a function other than a const, non-virtual member function without parameters");
    std::string base;
    const Ty* object = nullptr;
    TypeTable& types = v.global.types();
    if (!objectExpr || isThisObject(objectExpr))
    {
      if (isDebugValue(called, v.symbols))
        return fail(v, at, "a call of its own debugValue");
      object = v.self;
    }
    else
    {
      if (!natvisExpression(v, objectExpr, base))
        return false;
      base += arrow ? "->" : ".";
      object = objectExpr->ty ? types.unqualified(types.stripRef(objectExpr->ty)) : nullptr;
      if (object && arrow && object->kind == TyKind::Pointer)
        object = object->inner ? types.unqualified(object->inner) : nullptr;
    }
    if (!object || object->kind != TyKind::Class || !object->decl)
      return fail(v, at, "a call on something other than the object or a member that is a class");
    if (v.depth > 8)
      return fail(v, at, "calls nested deeper than 8");
    TemplateArgs args = v.global.classArgsOf(object);
    Decl* defined = args.empty() ? v.global.definitionOf(called) : v.global.definitionOf(called, args);
    auto* def = defined && defined->kind == NodeKind::FunctionDecl ? static_cast<FunctionDecl*>(defined) : called;
    FunctionDecl* instance = args.empty() ? def : v.global.instanceBody(def, args, object);
    DebugValue inner{v.global, v.symbols, def->file, instance, args, v.prefix + base, v.depth + 1, object};
    std::vector<DebugCase> cases;
    bool endsNowhere = false;
    TextVariants piece;
    bool done = debugCases(inner, instance, cases, &endsNowhere);
    for (size_t i = 0; done && i < cases.size(); ++i)
    {
      std::string when = cases[i].text;
      TextVariants one{{"", ""}};
      if ((cases[i].first && !natvisCondition(inner, cases[i].first, when)) || !natvisText(inner, cases[i].second, one))
      {
        done = false;
        break;
      }
      for (const auto& [condition, text] : one)
        piece.push_back({bothConditions(when, condition), text});
    }
    if (done)
    {
      if (endsNowhere)
        piece.push_back({"", "?"});
      return textProduct(v, at, out, piece);
    }
    if (v.why.empty())
    {
      v.why = "the function it calls, " + std::string(v.symbols.name(called->name.last().symbol)) + ": " + inner.why;
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
    if (e->kind == NodeKind::CallExpr && static_cast<CallExpr*>(e)->args.empty() && isTextType(e->ty, v.symbols, types) &&
        static_cast<CallExpr*>(e)->callee && static_cast<CallExpr*>(e)->callee->kind == NodeKind::MemberExpr &&
        static_cast<MemberExpr*>(static_cast<CallExpr*>(e)->callee)->resolved &&
        static_cast<MemberExpr*>(static_cast<CallExpr*>(e)->callee)->resolved->kind == NodeKind::FunctionDecl)
    {
      // a const member function returning text, its text inlined (MapVersion's str(), Comparison's)
      auto* m = static_cast<MemberExpr*>(static_cast<CallExpr*>(e)->callee);
      return inlinedText(v, e, static_cast<FunctionDecl*>(m->resolved), m->base, m->op == P::Arrow, out);
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
      // A set of flags (design.md 27): the flag's name when one is set, {} when none, else its bits; a row per
      // flag's name, true or false, and its bits.
      std::string displays = "    <DisplayString Condition=\"__kpp_bits == 0\">{{}}</DisplayString>\n", items;
      for (const ClassDecl::Flag& flag : cls->flagBits)
      {
        char mask[24];
        snprintf(mask, sizeof mask, "0x%llx", 1ull << flag.bit);
        std::string name(symbols.name(flag.name));
        if (!flag.alias)
          displays += "    <DisplayString Condition=\"__kpp_bits == " + std::string(mask) + "\">" + escape(name) + "</DisplayString>\n";
        items += "      <Item Name=\"" + escape(name) + "\">" + escape("(__kpp_bits & " + std::string(mask) + ") != 0") + "</Item>\n";
      }
      out = "  <Type Name=\"" + escape(d.name) + "\">\n" + displays + "    <DisplayString>{{bits={__kpp_bits,x}}}</DisplayString>\n    <Expand>\n" + items +
            "      <Item Name=\"[bits]\">__kpp_bits,x</Item>\n    </Expand>\n  </Type>\n";
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
      for (const DebugCase& one : cases)
      {
        Expr* value = one.second;
        std::string when = one.text;
        if (one.first && !natvisCondition(v, one.first, when))
        {
          failed = true;
          break;
        }
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
            std::string all = bothConditions(when, condition);
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
      out = "  <Type Name=\"" + escape(d.name) + "\">\n" + displays;
      if (!items.empty())
        out += "    <Expand>\n" + items + "    </Expand>\n";
      out += "  </Type>\n";
      return true;
    }
    return false;
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
  // A type's debugValue() (DebugValue above): its rule, one per instance (debugValueRule).
  for (const DebugClass& d : classes)
  {
    std::string rule, error;
    if (debugValueRule(global, symbols, d, rule, error))
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
