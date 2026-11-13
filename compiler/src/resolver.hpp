#pragma once
// Order-independent name resolution and type checking: phase 1 indexes every file's declarations
// into one shared scope tree (in parallel), phase 2 resolves type names and identifiers in all
// declarations and function bodies against that tree and computes the type of every expression
// (in parallel; the tree is read-only, canonical types are interned in a shared table).
#include "Ast/Ast.hpp"
#include "consteval.hpp"
#include "TypeTable.hpp"
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct Scope;

// Template parameter -> argument type, for queries about one instantiation.
struct TemplateArgs
{
  bool empty() const { return pairs.empty(); }

  std::vector<std::pair<Decl*, const Ty*>> pairs;
  // While one element of a pack expansion is emitted: the packs its pattern expands and the
  // element's index. The pattern's own mentions of those packs name that element; everything else
  // (the function's parameters, the class instance, its members) keeps the whole packs the pairs
  // bind (`(OrderList::update<Ts>(lists[OrderList::getIndex<Ts>()]), ...)` with lists an
  // `Array<Entry, sizeof...(Ts)>&`).
  std::vector<Decl*> elementPacks;
  uint32_t elementIndex = 0;
  // The pattern of that expansion (the instance's copy's node): it is typed for the element, and an
  // expansion inside it takes its packs whole (`sumAll(args, args...)...`: each element and all of
  // them, [temp.variadic] 5); null where the code generator names no pattern.
  const Node* elementPattern = nullptr;
};

// Whether an expression names an object that outlives the expression (an lvalue): a variable or
// parameter (a named rvalue reference included), a field, an element of an array or pointer, a
// dereference, and the built-in operators C++ makes lvalues of a scalar: a prefix increment or
// decrement, an assignment (`a = b`, `x += 1`), a comma whose right operand is one ([expr.pre.incr]
// 1, [expr.ass] 1, [expr.comma] 1; until 2026-11-05 values: `bump(++x)` with `bump(int&)` was
// refused). Calls and operators that yield references carry the reference in their type.
// The branch that goes on of `c ? throw x : y` (y; of `c ? y : throw x` too): the conditional is that
// branch, its type and value category ([expr.cond] 2). Null when neither or both throw.
inline Expr* branchBesideThrow(const ConditionalExpr* c)
{
  auto throws = [](const Expr* x)
  {
    while (x && x->kind == NodeKind::ParenExpr)
      x = static_cast<const ParenExpr*>(x)->inner;
    return x && x->kind == NodeKind::ThrowExpr;
  };
  return !c->then || throws(c->then) == throws(c->else_) ? nullptr : throws(c->then) ? c->else_ : c->then;
}

inline bool isLvalueExpression(const Expr* e);

// A non-static data member that is no reference, of an object that is an rvalue (`make().b`, `move(h).b`,
// `Holder{...}.a`): an xvalue ([expr.ref] 6.2), which a `T&&` binds and a by-value parameter moves from (until
// 2026-11-08 an lvalue: copied, and f(const T&) chosen over f(T&&); thread J's expression generator).
inline bool isXvalueMember(const Expr* e)
{
  if (!e || e->kind != NodeKind::MemberExpr)
    return false;
  auto* m = static_cast<const MemberExpr*>(e);
  Decl* d = m->resolved;
  if (!d || d->kind != NodeKind::VariableDecl || (d->flags & (DF_Static | DF_Typedef)) || !m->base || P(m->op) != P::Dot)
    return false;
  const Type* declared = static_cast<const VariableDecl*>(d)->type;
  if ((declared && (declared->kind == NodeKind::LRefType || declared->kind == NodeKind::RRefType)) || (e->ty && e->ty->isReference()))
    return false;
  const Expr* b = m->base;
  while (b && b->kind == NodeKind::ParenExpr)
    b = static_cast<const ParenExpr*>(b)->inner;
  if (!b || !b->ty || !b->ty->isKnown() || b->ty->kind == TyKind::LRef || b->ty->kind == TyKind::Pointer)
    return false;
  return !isLvalueExpression(b);
}

inline bool isLvalueExpression(const Expr* e)
{
  if (!e)
    return false;
  // A scalar operand, of a built-in operator (a class's operator function gives its result type).
  auto scalar = [](const Expr* x) {
    const Ty* t = x ? x->ty : nullptr;
    if (t && t->isReference())
      t = t->inner;
    return t && (t->isArithmetic() || t->kind == TyKind::Enum || t->kind == TyKind::Pointer || t->kind == TyKind::MemberPointer);
  };
  // A class's left operand: its implicit operator= (no function of its own) gives the object assigned.
  auto isClassOperand = [](const Expr* x) { return x && x->ty && (x->ty->isReference() ? x->ty->inner : x->ty)->kind == TyKind::Class; };
  switch (e->kind)
  {
    case NodeKind::BinaryExpr:
    {
      auto* b = static_cast<const BinaryExpr*>(e);
      if (b->isKeyword)
        return false;
      switch (P(b->op))
      {
        case P::Assign: case P::PlusAssign: case P::MinusAssign: case P::StarAssign: case P::SlashAssign: case P::PercentAssign:
        case P::AmpAssign: case P::PipeAssign: case P::CaretAssign: case P::ShlAssign: case P::ShrAssign:
          return (scalar(b->lhs) || (P(b->op) == P::Assign && b->call == OperatorCall::Builtin && isClassOperand(b->lhs))) && b->call != OperatorCall::Function;
        case P::Comma:
          return b->call != OperatorCall::Function && isLvalueExpression(b->rhs);
        default:
          return false;
      }
    }
    case NodeKind::NameExpr:
    {
      Decl* d = static_cast<const NameExpr*>(e)->resolved;
      return d && (d->kind == NodeKind::VariableDecl || d->kind == NodeKind::ParamDecl);
    }
    case NodeKind::MemberExpr:
    {
      Decl* d = static_cast<const MemberExpr*>(e)->resolved;
      return d && d->kind == NodeKind::VariableDecl && !isXvalueMember(e);
    }
    case NodeKind::IndexExpr:
    {
      const Ty* base = static_cast<const IndexExpr*>(e)->base->ty;
      if (base && base->isReference())
        base = base->inner;
      return base && (base->kind == TyKind::Pointer || base->kind == TyKind::Array);
    }
    case NodeKind::UnaryExpr:
    {
      auto* u = static_cast<const UnaryExpr*>(e);
      const Ty* t = u->operand ? u->operand->ty : nullptr;
      if (t && t->isReference())
        t = t->inner;
      if (!u->isKeyword && !u->postfix && (P(u->op) == P::PlusPlus || P(u->op) == P::MinusMinus))
        return scalar(u->operand) && u->call != OperatorCall::Function; // `++x`
      return !u->isKeyword && !u->postfix && P(u->op) == P::Star && t && t->kind == TyKind::Pointer;
    }
    case NodeKind::ParenExpr:
      return isLvalueExpression(static_cast<const ParenExpr*>(e)->inner);
    case NodeKind::HeldExpr:
      return isLvalueExpression(static_cast<const HeldExpr*>(e)->holder->inner);
    case NodeKind::ConditionalExpr:
    {
      // `c ? shift.second : shift.first`: two lvalues of one type make an lvalue (C++), a call
      // returning a reference one of them (`c ? map.unsafeRandom : map.getTriggersRandomGenerator()`).
      auto* c = static_cast<const ConditionalExpr*>(e);
      auto lvalue = [](const Expr* x) { return isLvalueExpression(x) || x->ty->kind == TyKind::LRef; };
      if (const Expr* other = branchBesideThrow(c))
        return other->ty && lvalue(other); // (`c ? throw x : a`)
      if (!c->then || !c->else_ || !c->then->ty || !c->else_->ty || !lvalue(c->then) || !lvalue(c->else_))
        return false;
      const Ty* a = c->then->ty->isReference() ? c->then->ty->inner : c->then->ty;
      const Ty* b = c->else_->ty->isReference() ? c->else_->ty->inner : c->else_->ty;
      return a == b;
    }
    default:
      return false;
  }
}

// A narrow string literal's type as C++ has it, an lvalue of const char[N] (adjacent literals
// joined, the terminating zero counted; the walk types the literal as the pointer it decays to):
// a `const T&` parameter deduces T = char[N] from it. Null for any other expression, a wide
// literal or one whose tokens are not at hand.
const Ty* stringLiteralArray(const Expr* e, const Ty* t, TypeTable& types);

// How one argument packed into a Text (design.md 14) appends itself: the library's function the
// Text calls for it (Text.kpp: appendMemberPiece<T>, appendFreePiece<T> or appendCTextPiece) and
// the type T it is instantiated for (null for C text); or error, why the argument can be no piece
// (a compile error naming its type). Neither for a type not known yet.
struct TextPieceKind
{
  FunctionDecl* thunk = nullptr;
  const Ty* type = nullptr;
  std::string error;
};

// The type an argument expression presents to overload resolution: an lvalue of a non-reference
// type is an lvalue reference to it, so that `T&&` parameters take only temporaries and `move(x)`
// (C++ rules); `t` is the expression's type, or one element of a pack it expands. A string
// literal presents its array (stringLiteralArray).
inline const Ty* argumentType(const Expr* e, const Ty* t, TypeTable& types)
{
  if (e && t && t->kind == TyKind::Pointer && (e->kind == NodeKind::LiteralExpr || e->kind == NodeKind::ParenExpr))
    if (const Ty* array = stringLiteralArray(e, t, types))
      return types.lref(array);
  if (t && t->isKnown() && t->kind != TyKind::Pack && !t->isReference() && isXvalueMember(e))
    return types.rref(t); // (an xvalue: `make().b` binds a T&& and no T&)
  if (!t || !t->isKnown() || t->kind == TyKind::Pack || !isLvalueExpression(e))
    return t;
  if (t->kind == TyKind::RRef)
    return types.lref(t->inner); // a named rvalue reference is an lvalue
  return t->isReference() ? t : types.lref(t);
}

// The type a brace list `{a, b, c}` presents to overload resolution: the brace list type
// (TypeTable::braceList) of the first element's type, the first of a pack's for `{args...}` (null
// when the elements are untyped or there is no such class); for `{}`, the empty brace list. Only for ranking and deduction: the
// expression itself stays untyped, since a brace list also initialises aggregates and calls
// constructors. Each element initialises one of the list's from what it is, so when every
// element is an lvalue the element type is an lvalue reference: a converting constructor taking
// `T&` takes them (`implicit BoxRef(Box& b)` for `{a, b}`).
inline const Ty* braceListType(const Expr* e, Decl* initializerList, TypeTable& types)
{
  if (e && e->kind == NodeKind::BraceInitExpr && static_cast<const BraceInitExpr*>(e)->text)
    return e->ty; // a call's arguments packed for its Text parameter: a Text (design.md 14)
  if (!e || e->kind != NodeKind::BraceInitExpr || !initializerList)
    return nullptr;
  auto* b = static_cast<const BraceInitExpr*>(e);
  if (b->args.empty())
    return types.emptyBraceList(initializerList);
  if (!b->args[0])
    return nullptr;
  // (A nested list, `{{a, b}}`, is a brace list itself.)
  const Ty* first = b->args[0]->kind == NodeKind::BraceInitExpr ? braceListType(b->args[0], initializerList, types) : b->args[0]->ty;
  if (!first || !first->isKnown())
    return nullptr;
  if (first->kind == TyKind::Pack)
  {
    if (first->args.empty() || !first->args[0] || !first->args[0]->isKnown())
      return nullptr;
    first = first->args[0];
  }
  bool lvalues = types.stripRef(first)->kind != TyKind::Array;
  for (const Expr* a : b->args)
    lvalues = lvalues && a && a->ty && a->ty->kind != TyKind::Pack && isLvalueExpression(a);
  // Each element as an argument presents it; none when one is not known or is a pack's expansion.
  std::vector<const Ty*> elements;
  for (const Expr* a : b->args)
  {
    const Ty* t = a && a->kind == NodeKind::BraceInitExpr ? braceListType(a, initializerList, types)
                  : a && a->ty && a->ty->isKnown() && a->ty->kind != TyKind::Pack ? argumentType(a, a->ty, types) : nullptr;
    if (!t)
    {
      elements.clear();
      break;
    }
    elements.push_back(t);
  }
  return types.braceList(initializerList, lvalues ? argumentType(b->args[0], first, types) : types.decay(first),
                         Span<const Ty*>{elements.data(), uint32_t(elements.size())});
}

#include "ResolveStats.hpp"

// The --why report: per cause of unknown expression types, the most common failing constructs
// with a count and the first place each occurs.
std::string whyReport(const ResolveStats& total, SymbolTable& symbols, int perCause);

class GlobalScope
{
public:
  explicit GlobalScope(SymbolTable& symbols);
  ~GlobalScope();
  // --why-at: print every expression typed on this line of the file whose path ends with
  // fileSuffix, with its type (to stderr, while resolving). Each call adds a line.
  void traceLine(const std::string& fileSuffix, uint32_t line);

  // Phase 1, thread-safe: index all top-level declarations of one file.
  void indexFile(ParsedFile& file);
  // After all files are indexed: attach out-of-line class definitions (`struct A::B {}`).
  void finishIndex();
  // After finishIndex: the errors for top-level names the program declares again although the
  // standard library declares them (design.md, "One global scope, so clashes are errors"), one
  // `path:line: error: ...` line each, sorted.
  std::vector<std::string> topLevelClashes();
  // Phase 2, thread-safe and read-only on the scope tree: resolve and type one file.
  void resolveFile(ParsedFile& file, const std::vector<Token>& tokens, std::string_view text, ResolveStats& stats);
  // After the checker (and the code generator, which asks it for instance bodies) is done: the
  // static_asserts of the class template instances it made are checked, and the errors the checker
  // found are returned, `path:line: error: ...` one each, sorted. A static_assert whose condition
  // is false, or no constant the checker can evaluate, is one. instancesChecked: how many of the
  // instances the code generator of this same build checked already (checkInstanceStaticAsserts'
  // result), 0 for all.
  std::vector<std::string> checkerErrors(size_t instancesChecked = 0);
  // The checker's warnings, `path:line: warning: ...` one each, sorted: a [[nodiscard]] result
  // discarded, a [[deprecated]] function called (no error: code is generated).
  std::vector<std::string> checkerWarnings();
  // Whether the checker has reported an error in the program (checkerErrors' own, before any
  // instance codegen emits is checked): kpp --check stops before codegen then, when codegen has no
  // session to follow the edit (main.cpp, reportResolve).
  bool hasCheckerErrors();
  // Access checking, warnings only, after enableAccessWarnings() before the resolve (kpp
  // --access-warnings; design.md, "struct and class"): the uses of private and protected members
  // outside their class, its derived classes (protected) and its friends, and the structs with a
  // private or protected member, a base that is not public or a virtual function. accessReport()
  // after the build: the warnings (`path:line: warning: ...`, each once, by file and line), with a
  // note for each use a friend declaration allows and for each friend declaration, and in summary
  // their counts.
  void enableAccessWarnings();
  // A constructor codegen calls in the code of `in` (the function or the global's initializer being
  // emitted), at node at: checked as a use of the member it is (the checker does not choose
  // constructors, codegen does). With access errors on, what it denies, the error codegen reports;
  // else empty (a warning of the report with enableAccessWarnings()).
  std::string checkConstructorAccess(FunctionDecl* ctor, Decl* in, Node* at);
  // A use of a private or protected member its code may not name is an error (design.md "struct and
  // class": the default, not for a conversion's C++). Before the resolve.
  void enableAccessErrors();
  // The struct rule as errors (kpp --struct-rule; design.md "struct and class"): a struct with a
  // private or protected section, a base that is not public or a virtual function, or deriving from
  // a polymorphic class. Before the resolve. The default once the converter writes such structs as
  // classes.
  void enableStructRule();
  std::vector<std::string> accessReport(const std::vector<ParsedFile*>& files, std::string& summary);
  // A static_assert in the body of a template instance's function is checked only if the program
  // uses that function, as C++ instantiates a member or function template only where it is used:
  // the code generator says which instance bodies (GlobalScope::instanceBody) it emitted, and
  // checkerErrors() has those functions' failed asserts. unusedFailedAsserts(): how many failed in
  // functions it never emitted (all of them, when no code is generated).
  void functionEmitted(const FunctionDecl* f);
  size_t unusedFailedAsserts();
  // Whether this build generated code (a compile server's kept program: the last build's code
  // generator emitted functions): when not, none counts as emitted, as in a build from scratch.
  void setCodeGenerated(bool generated);
  // The target's C is LP64 with a 32-bit wchar_t (every target but Windows; docs/design.md, "Targets
  // and hosts"): `long` is 64 bits and `wchar_t` (with L"..." and L'x') 32, printf's `%ld` takes 64
  // bits, and MSVC's lengths (`%I64d`) are no C library's there. Before anything is resolved.
  void setLP64(bool lp64, bool wcharUnsigned = false);
  // The target's C++ ABI is MSVC's (the windows target, not windows-gnu): a pointer to a data member
  // is 4 bytes there (constSizeOf, as the code generator lays it out), 8 in Itanium's.
  void setMsvcAbi(bool msvc);
  // A conversion's resolve (kpp --convert): both branches of an `if constexpr` in a function that is no
  // template are walked, as C++ checks them (the converter rewrites both); a build walks the taken one.
  void setConverting(bool converting);
  // config::debug of the build (kpp's `--config debug=...`, false in a release build): where it is
  // false, `assert` compiles to nothing (design.md 7), so the checker and the evaluator leave its
  // expression as discarded code (a `when (config::debug)` member it names is no error, design.md 19).
  void setDebug(bool debug);
  bool isDebug() const;
  // Whether a checker error at a place in the file at path was reported outside any unit (the
  // index's, a class instance's static_assert): walking the file again would not take it back.
  bool unitlessErrorIn(const std::string& path);
  // Whether the checker reported an error in f's text (its signature or body), or in f, an instance's
  // copy: a value of unknown type the code generator meets in f follows from it.
  bool checkerErrorIn(const FunctionDecl* f);
  // Whether the checker reported an error at a token of file from `from` to `to` (both included):
  // the code generator's failure on an expression holding it follows from that error.
  bool checkerErrorWithin(const ParsedFile* file, uint32_t from, uint32_t to);
  // Whether the checker reported "'name' is not declared" somewhere (an undeclared type the code
  // generator meets in a layout: its own error would only follow from that one).
  bool undeclaredReported(const std::string& name);
  // The first part of checkerErrors: the static_asserts of the class template instances named so
  // far. The code generator runs it once its functions are emitted (they name instances of their
  // own), before it checks the asserts on layouts. From the from-th instance made on (those before
  // it checked by an earlier call of the same build); returns how many there are now.
  size_t checkInstanceStaticAsserts(size_t from = 0);
  // The static_asserts whose condition needs a class's size or alignment, which only the code
  // generator's layout knows: each with the arguments of the instance it is in (none outside
  // templates) and that instance for messages. The checker leaves them to the code generator.
  struct LayoutStaticAssert
  {
    StaticAssertDecl* decl;
    TemplateArgs args;
    std::string instance;
  };
  std::vector<LayoutStaticAssert> layoutStaticAsserts();

  size_t scopeCount() const;
  size_t entryCount() const;
  size_t typeCount() const;
  // The types that mention no unknown one (TypeTable::sizeWithoutUnknown): the report's, deterministic.
  size_t typeCountWithoutUnknown() const;
  struct InstantiationStats
  {
    size_t classes, functions, bodies, requests, waits;
    double seconds;
  };
  InstantiationStats instantiationStats() const;
  // The statistics of the template instances' walks, each counted once whichever walk (a file's,
  // another instance's, a probe's) asked for it first: added to the files' to make the totals.
  ResolveStats instanceStats();
  // What the units of the front end's work recorded they read (step 2 of the persistent compiler,
  // docs/compile-speed.md): how many units of each kind, and their names and uses in all.
  struct DependencyStats
  {
    size_t walks = 0, declaredTypes = 0, instances = 0, classInstances = 0, scopes = 0;
    size_t names = 0, uses = 0;
  };
  DependencyStats dependencyStats();
  // Whether the resolve records them (before resolveAll; a compile server's builds do).
  static void recordDependencies(bool on);

  // Step 3b, the compile server's fast path (docs/compile-speed.md): a file whose function bodies
  // alone changed is walked again in the kept program. resolveDone: what is instantiated from now
  // on is codegen's. beginRewalk/endRewalk around the new walk: endRewalk is false when the walk
  // reused an instance codegen made (a resolve from scratch would differ; the instances it made
  // itself are counted, rewalkCounts). forgetWalks: the file's walks' recorded reads, before they
  // are walked again. instancesWalksUse: the instances the file's walks requested.
  void resolveDone();
  void beginRewalk();
  bool endRewalk();
  void forgetWalks(const ParsedFile& file);
  std::vector<const void*> instancesWalksUse(const ParsedFile& file);
  // Those of the instances (the old walks' of file) no walk or used instance asks for now, and what only they asked
  // for, taken out of the tables with their statistics and errors (a server's edit; a build from
  // scratch never makes them): what the report counted of them added to dropped, their templates to
  // templates. (One codegen still asks for, through a use the recorded dependencies do not hold, it
  // makes again from its template: the same code, made after the resolve.)
  void dropUnusedInstances(const std::vector<const void*>& instances, const ParsedFile& file, InstantiationStats& dropped,
                           std::vector<const Decl*>& templates, std::vector<const Ty*>& droppedClasses);
  // What the instances a rewalk made (beginRewalk to endRewalk) added to the report's counts.
  InstantiationStats rewalkCounts() const;

  // A changed function's name as lookups recorded it: its symbol, and for a member the class scope
  // (lookups in a class are recorded with the class; anyClass: a member whose class is unknown, read
  // by a lookup in any class; null: a namespace's function).
  struct ChangedName
  {
    static inline const void* const anyClass = reinterpret_cast<const void*>(uintptr_t(1));

    uint32_t symbol;
    const void* classScope;
    bool memberOnly = false; // a member function (no friend): only lookups in its class, or a class derived from it, reach it
  };
  // Step 4 (functions added, removed or changed in a kept file). unindexFunction: takes a function
  // out of the index (its scope's entry, or its class's out-of-line definitions). indexFunctions:
  // indexes functions grafted into a kept file (their parents set), out-of-line definitions
  // attached as finishIndex attaches them; false when one needs what only a whole index gives (a
  // definition matching no declaration, an object declaration to decide). Both forget the cached
  // overload sets that merged the names' entries.
  void unindexFunction(FunctionDecl* f);
  // A class's data members taken out of its scope (with their walks' recorded reads) and indexed in
  // it (their parents set): step 4's layout edits. membersReadOutsideUnits: whether the class's member
  // list was read outside any recorded unit (an edit of it is then a build from scratch).
  void unindexMember(VariableDecl* v);
  void indexMembers(const std::vector<VariableDecl*>& added);
  bool membersReadOutsideUnits(const Decl* cls);
  bool indexFunctions(const std::vector<FunctionDecl*>& added);
  // What read a name (by symbol) or a declaration's declared type: the files of the walks that did
  // (to walk again), the declarations whose declared types did (forgotten here, with what they
  // cached, and followed: what read those types is found too); false when an instance, a class
  // instance or a scope's relations read one (a resolve from scratch could make them otherwise).
  // walks: the declarations whose walks read them (their functions' code changes with them).
  // bodies: function definitions whose bodies changed, for the units the compile-time evaluator ran
  // them in (a constexpr function's): such walks are found as the names' readers are; it is false
  // when one ran outside any unit, or a declared type ran one (an array bound, a constant template
  // argument: what it typed may be a layout). classes: the classes whose member lists changed, for
  // what read them by position (USE_ClassMembers). why: the reason it is false. instances: the function
  // and member instances that read them, to be checked again (recheckInstancesOf); without it such
  // an instance makes it false. typedByWalk: the declarations among the reading walks whose type the
  // walk gives (decltype, a deduced auto), to be compared after the walk (forgetTypesFromWalks);
  // without it such a declaration makes it false.
  bool invalidateReaders(const std::vector<ChangedName>& changed, const std::vector<const Decl*>& decls, std::vector<const ParsedFile*>& files,
                         std::vector<const Decl*>* walks = nullptr, const std::vector<const FunctionDecl*>& bodies = {}, std::string* why = nullptr,
                         std::vector<const void*>* instances = nullptr, std::vector<const Decl*>* typedByWalk = nullptr,
                         const std::vector<const Decl*>& classes = {});
  // Their declared types as they are, then forgotten (with what computing them read): after their
  // walks, typeOfDecl gives them anew, and a build that changed one is built from scratch.
  std::vector<const Ty*> forgetTypesFromWalks(const std::vector<const Decl*>& decls);
  uint32_t functionSymbolOf(const FunctionDecl* f);
  ChangedName changedNameOf(const FunctionDecl* f);
  ChangedName changedNameOf(const VariableDecl* v);
  // The declarations whose walks read one of the names (nothing forgotten): the callers of an edited
  // function, whose code may hold its body (codegen inlines trivial functions).
  std::vector<const Decl*> walksReading(const std::vector<ChangedName>& changed);
  // A body a compile server's build grafted into a kept function (BodyGraft's graftBodies): its local classes and enumerations
  // get their scopes, as the file's index gave its old body's (Indexer's indexLocalTypes), an out-of-line member's seeing its
  // class (finishIndex's localTypesOfOutOfLine). Its old body's keep theirs: nothing reaches them now.
  void indexGraftedBody(FunctionDecl* f);
  std::vector<FunctionDecl*> outOfLineDefinitions(const Decl* cls); // the out-of-line member definitions attached to a class
  // Step 4, a template's body changed: its instances (function instances, class instances' members)
  // copied from the new body and checked again, their statistics replacing theirs; false when one
  // cannot be (only codegen made it, it deduces another return type). Between beginRewalk and
  // endRewalk, which tell whether the checks made instances anew.
  // readers: instances invalidateReaders found reading a change, checked again likewise, or, made by
  // codegen, taken out of the table with the codegen-made instances that used them (codegen, run in
  // full, makes them anew); dropped: how many were taken out. instancesOf: the templates of every
  // instance checked again or taken out (once each, in their files' order), what codegen emits their
  // instances again from: what it asked instanceBody for (and the definition whose body was copied,
  // where that is another declaration).
  // Of functions, those with an instance record that copied their body, though they are no template
  // (the evaluator runs a function from such a copy; an `auto` return typed before its file's walk
  // makes one): a body edit of one has its copy made anew (recheckInstancesOf), as a template's.
  std::vector<FunctionDecl*> withBodyCopies(const std::vector<FunctionDecl*>& functions);
  bool recheckInstancesOf(const std::vector<FunctionDecl*>& sources, const std::vector<const void*>& readers, std::string& why,
                          size_t* dropped = nullptr, std::vector<const Decl*>* instancesOf = nullptr);
  // The instance statistics as the resolve counts them: every instance walk's but codegen's.
  ResolveStats instanceStatsAtResolveEnd();

  // Queries for code generation (after resolveFile ran on every file).
  TypeTable& types();
  const Ty* typeOfDecl(Decl* d);                 // Declared (or deduced) canonical type.
  const Ty* typeOfAst(Type* t, Decl* context);   // A written type, resolved in the context of a declaration.
  Decl* definitionOf(Decl* d);                   // Class/enum definition, or the out-of-line definition of a member function or a static data member.
  // The same for a member of a class template instantiation (classArgs: the class's arguments):
  // the explicit specialisation written for it (`template<> R Button<RecipeID>::f()`), if any.
  Decl* definitionOf(Decl* d, const TemplateArgs& classArgs);
  Decl* initializerListDecl();                   // The library's InitializerList class, or null.
  const Ty* textType();                          // The library's Text class (design.md 14), or null.
  // Which of the library's compiler intrinsics f is (Text.kpp: 1 kppEnumeratorName, 2
  // kppEnumTypeName), 0 for none: code generation computes their calls.
  int intrinsicOf(const FunctionDecl* f);
  // Which of the library's move, forward and asConst f is (stdlib/core/Utility.kpp, UtilityExt.kpp), by
  // declaration: a call of one is typed as the cast it is and never instantiated, and code generation
  // emits it as the cast (design.md, "move, forward and asConst are casts"). None for any other function.
  enum class CallIntrinsic : uint8_t { None, Move, Forward, AsConst };
  CallIntrinsic callIntrinsicOf(const FunctionDecl* f);
  // How an argument of type t (e: the expression, for a string literal) packed into a Text appends
  // itself (TextPieceKind): by its type alone, wherever the call is.
  TextPieceKind textPieceOf(const Expr* e, const Ty* t);
  Decl* typeInfoDecl();                         // The library's TypeInfo class (what typeid yields), or null.
  Decl* enumFromStrDecl();                       // The library's enumFromStr<E>(const char*) template (E::fromStr lowers to it), or null.
  Decl* strongOrderingDecl();                    // The library's strong_ordering class (what `a <=> b` yields), or null.
  bool isExplicit(FunctionDecl* f, const TemplateArgs& args); // `explicit`, or `explicit(cond)` with cond true under args.
  bool isNoexcept(FunctionDecl* f, const TemplateArgs& args); // `noexcept`, or `noexcept(cond)` with cond true under args.
  // Whether moving a t (else copying one) cannot throw, its constructor alone (isNothrowMoveConstructible<t>
  // asks its destructor too, as the libraries do). False where it cannot be told.
  bool constructsWithoutThrowing(const Ty* t, bool move);
  // Whether t's destructor may throw ([except.spec] 8); unknown where what a closure among its parts
  // holds by value is not known here (closureValueParts).
  std::optional<bool> destructorMayThrow(const Ty* t);
  // A structured binding a lambda's body names (lambdaNames): its hidden object, its name, the type
  // of its first use.
  struct BindingUse
  {
    Decl* object;
    uint32_t name;
    const Ty* type;
  };
  // What a lambda's body names, from its AST alone: the locals of the functions around it (each once,
  // in the order first written: used) and whether it uses `this`. inner gathers what is declared
  // inside it (its parameters, its locals, nested lambdas' own), which is no capture; C++ declares a
  // name before its uses, so it is in inner before a use is reached. useTypes, when given, gets the
  // type of each used local's first name expression. keptBranches: an `if constexpr` its instance
  // decided names only what the kept branch names (not in a generic lambda, whose one closure serves
  // every call's instance of the body). bindingUses, when given, gets each structured binding the
  // body names, once, in the order first written. The code generator's capture list is made from it,
  // and so is closureValueParts.
  void lambdaNames(Node* n, std::unordered_set<Decl*>& inner, std::vector<Decl*>& used, bool& usesThis,
                   std::unordered_map<Decl*, const Ty*>* useTypes = nullptr, int depth = 0, bool keptBranches = false,
                   std::vector<BindingUse>* bindingUses = nullptr);
  // A capture a closure holds by value: its type, and the initializer of an init-capture (null for a
  // copy of a variable, made from it as an lvalue).
  struct ClosurePart
  {
    const Ty* type;
    Expr* init;
  };
  // What a closure of lambda type lambdaTy holds by value, as the code generator lays it out (a capture
  // by reference holds a pointer, `this` too): each capture by copy, a structured binding's own, a
  // pack's element by element. Null where one of them is not known here. The traits and the
  // destructor's exception specification of a closure are made from it.
  std::optional<std::vector<ClosurePart>> closureValueParts(const Ty* lambdaTy);
  // The direct base of t whose noexcept virtual destructor t's, which may throw, overrides; null: none.
  const Ty* destructorLoosens(const Ty* t);
  // t's destructor is virtual: written so, a base's, or a class's with a virtual function, its own or a base's
  // (kovarex's rule, design.md 23b), unless t is marked [[foreign_vtable]], its own or a base's.
  bool hasVirtualDestructor(const Ty* t);
  // The initializer of v (a data member's default initializer, a static member's or a variable
  // template's) for one instantiation: a copy typed under args once and kept, as instanceBody's
  // copies of function bodies are; v's own initializer when args is empty. Thread-safe.
  Expr* instanceInitializer(VariableDecl* v, const TemplateArgs& args);
  // The declaration a variable template takes its value from for these arguments (the
  // primary's parameters bound, defaults included): the most specialised matching partial or
  // full specialisation, its own parameters bound in specArgs, or the primary (specArgs = args).
  // A specialisation matches when its written arguments deduce to these and stay well-formed with
  // them (`force_memcpy_save<T, VoidT<T::force_memcpy_save>>` needs T to have the member). Null
  // when no match is more specialised than all the others (ambiguous, an error in C++).
  VariableDecl* variableTemplateFor(VariableDecl* primary, const TemplateArgs& args, TemplateArgs& specArgs);
  // Whether requires-expression r, written in owner (a variable template's initializer:
  // `hasBegin<T> = requires (T& t) { t.begin(); }`), holds for args: its requirements probed for
  // them, not the value its last typing left on the shared node. nullopt when it cannot be told.
  std::optional<bool> requirementsHold(RequiresExpr* r, Decl* owner, const TemplateArgs& args);
  Decl* owningClass(Decl* d);                    // The class a member (possibly defined out of line) belongs to.
  // A program's own global allocation and deallocation functions (design.md, "Allocation functions"):
  // whether it declares any (new and delete then call the global ones, not malloc and free); whether
  // f, one of the library's, is replaced by a program definition of the same parameters (not
  // emitted, never a candidate); and the global `operator delete` (array: `operator delete[]`) that
  // takes the size after the pointer (sized) or not, the program's where it replaced the library's.
  bool programAllocationFunctions();
  // A class object given as a template argument (TypeTable::objectValue), as the value it is.
  std::optional<ConstValue> argumentObject(const Ty* t);
  bool replacedAllocation(const FunctionDecl* f);
  FunctionDecl* globalDeallocation(bool array, bool sized);
  // The arguments for the out-of-class definition of a class template's static data member:
  // classArgs, and the definition's own declarations of the class's parameters bound the same.
  TemplateArgs definitionArgs(Decl* def, const TemplateArgs& classArgs);
  // The in-class declaration of an out-of-line member definition (`static` and `virtual` are
  // written there and copied onto the definition); null when there is none.
  FunctionDecl* declarationOf(FunctionDecl* def);
  void mergeDefaultArguments(FunctionDecl* f); // finishIndex: every declaration of f in its file gets all of its default arguments
  std::string qualifiedName(Decl* d);            // `ns::Class::name`
  // The name a type name declared nowhere was written with, for the placeholder class the resolver
  // gives it (a draft, ATTR_Draft) so that checking goes on; "?" for any other declaration.
  std::string draftName(const Decl* d);
  bool isExternC(FunctionDecl* f);
  // The named return variable of f (NRVO, kovarex 2026-11-04): the one automatic local every
  // `return` of f names (`return v;`, nested lambdas' and local classes' returns aside), of f's
  // return class (cv aside), built in the result's place with no copy, no move and no constructor
  // needed. Null when f has none: a return of something else or of another variable, v a
  // parameter, static, thread_local, extern, volatile, a reference, a structured binding, a catch's
  // parameter, a compile-time loop's variable, absent by its `when`, declared alignas, or of another
  // type. v is declared where codegen builds it at the result: a declaration statement (an init and
  // a condition are ones) or a range-for's variable. Typed by typeOfDecl alone: a template instance's
  // typed copy needs its arguments (the overload below). why: when none, the reason, for
  // --return-copy-warnings.
  VariableDecl* namedReturnVariable(const FunctionDecl* f, std::string* why = nullptr);
  // The same over any body given the type it returns: a lambda's call operator's, whose body
  // codegen emits from the LambdaExpr (no FunctionDecl). args: the template arguments the body's
  // declarations are typed under (codegen's declType: typeOfDeclIn), given for an instance's typed
  // copy, whose variables typeOfDecl alone does not give the instance's types.
  VariableDecl* namedReturnVariable(const Stmt* body, const Ty* result, std::string* why = nullptr, const TemplateArgs* args = nullptr);
  // --return-copy-warnings: each `return x;` of a local or parameter of class type that is copied
  // or moved, not built in the result's place, with why (the program's files; not templates yet).
  std::vector<std::string> returnCopyWarnings(const std::vector<ParsedFile*>& files);
  // --implicit-uses (source.hpp): a line for each constructor and conversion operator marked `implicit` in the program's
  // files (not imported C), in their order, "PATH:LINE<TAB>constructor|conversion<TAB>NAME<TAB>uses N<TAB>asked M",
  // "<TAB>conditional" after an `implicit(cond)`; each use's place on a line of its own after it, "<TAB>at PATH:LINE".
  std::vector<std::string> implicitUses(const std::vector<ParsedFile*>& files);
  bool isTemplateContext(Decl* d);               // Inside a template (any enclosing declaration has parameters).
  // Instantiation queries, single-threaded (code generation):
  const Ty* typeOfDeclIn(Decl* d, const TemplateArgs& args);
  // d's type under the arguments of the instantiation's copy whose body declares the class `in` (a
  // closure), as that copy's walk typed it, or null when `in` is in no copy: what a closure
  // capturing a parameter pack lays out, whichever function needs the closure first.
  const Ty* typeInInstanceOf(Decl* d, const Decl* in);
  const Ty* typeOfAstIn(Type* t, Decl* context, const TemplateArgs& args);
  // The value of the constant expression e (read from file, written in context: a function, the
  // variable whose initializer it is, a class) under args: the checker's compile-time evaluator,
  // with the code generator's arithmetic mode, enumeration storage and layouts (hooks). Null when
  // e is no constant.
  std::optional<ConstValue> evaluate(Expr* e, ParsedFile* file, Decl* context, const TemplateArgs& args, const ConstHooks& hooks);
  // Whether some base subobject anywhere in class cls's hierarchy is at a non-zero offset: a pointer
  // to its member functions then carries an adjustment of `this` (16 bytes, MSVC's multiple
  // inheritance model), else it is the function alone (8). Decided from the declarations as the
  // code generator lays classes out (its own vtable pointer, the polymorphic bases, then the others;
  // an empty base takes no bytes), so that sizeof agrees at compile time and at run time, and never
  // from a layout, which may hold such a pointer to the class itself.
  bool hasBaseAtOffset(const Ty* cls);
  // Whether an object of class cls starts with its subobject of class base, or is one, from the
  // declarations as hasBaseAtOffset (a layout may need the class asking): only the first base the
  // layout places counts, so a false may be a base that takes the start after an empty one.
  bool classStartsWith(const Ty* cls, const Ty* base);
  // Whether base is a base of cls, from the declarations.
  bool classDerivesFrom(const Ty* cls, const Ty* base);
  // The value init gives an object of type t when it initializes one (a global's or static
  // member's initializer: `Flags f = {A, B};`, `Array<int, 3> a = {1, 2, 3}`, `Box b = Box(1, 2);`):
  // a brace list's elements, a constructor's arguments, a conversion; no init (`constexpr P p;`):
  // t default-initialized. Context and hooks as for evaluate. Null when it is no constant (the
  // object is then initialized at run time).
  // build.kpp at compile time (design.md 12): a Project default-constructed, its configuration
  // the enumerator of that name, build(project) run, then describe(project): that String's
  // characters. Nullopt with error ("Build.kpp:12: why") when that is no constant (a call it
  // cannot run, memory left allocated, more than a million steps).
  std::optional<std::string> runBuild(std::string_view configuration, std::string& error);
  // Why this thread's last evaluation that failed is no constant ("a call to puts, ..."), empty
  // when the interpreter cannot evaluate it yet; cleared by clearConstantReason.
  static std::string constantReason();
  static void clearConstantReason();
  // What an object no call operator of which takes these arguments is called through
  // (Impl::surrogateFunction): a conversion's pointer or reference to a function, or null.
  const Ty* surrogateFunction(const Ty* object, const std::vector<const Ty*>& args);
  // The underlying type of an enumeration type, under an instance's arguments (`enum : T`): the
  // fixed one, or one int cannot hold off Windows; null when it is int, unknown while dependent.
  const Ty* underlyingOf(const Ty* enumeration);
  // The value of an enumerator of the enumeration enumType, an instance's (`Tm<3>::value`).
  std::optional<ConstValue> evaluateEnumerator(Decl* enumerator, const Ty* enumType, const ConstHooks& hooks);
  std::optional<ConstValue> evaluateInitializer(const Ty* t, Expr* init, ParsedFile* file, Decl* context, const TemplateArgs& args,
                                                const ConstHooks& hooks);
  // Re-types the body of f for one instantiation (expression types and resolved callees are
  // rewritten in place). objectType: the instantiated class for members, or null. packElement:
  // args bind a parameter pack to one of its elements (to emit a pattern of its expansion), and
  // the if constexpr branches the instance took with the whole pack stay taken.
  void retypeFor(FunctionDecl* f, const TemplateArgs& args, const Ty* objectType, bool packElement = false);
  // The instantiation of f for args (objectType: the instantiated class for members, or null): a
  // copy of the nodes f owns, typed for these arguments once and kept, so that the template's own
  // nodes are never written for an instantiation (docs/design.md, "Instantiations own their typed
  // bodies"). Thread-safe; the same copy for the same arguments.
  FunctionDecl* instanceBody(FunctionDecl* f, const TemplateArgs& args, const Ty* objectType);
  // A copy of that instantiation (of f itself when args is empty) no one else has, typed for args:
  // for an emitter that re-types it in place, once per element of a pack expansion or `for
  // constexpr` (retypeFor), while other threads read the instantiation's own copy. Its lambdas keep
  // the instantiation's closure types. Not kept.
  FunctionDecl* privateInstance(FunctionDecl* f, const TemplateArgs& args, const Ty* objectType);
  // The call operator of a generic lambda (one with template parameters, written or invented for
  // its `auto` parameters) for one set of its own arguments: its parameters and body copied and
  // typed in the environment the lambda was written in, once, and kept (body gets the copy; its
  // captures and closure are the lambda's). Returns the operator's type (parameters and result)
  // or null; fullArgs gets the enclosing instantiation's arguments plus the lambda's own, for
  // declarations in the body. Thread-safe.
  const Ty* lambdaInstance(const Ty* lambdaTy, const TemplateArgs& own, TemplateArgs* fullArgs, LambdaExpr** body);
  // A copy of such an instance's body (body: lambdaInstance's) no one else has, typed for args (its
  // fullArgs): for an emitter that retypes it in place, once per element of a pack expansion or
  // `for constexpr` in it (retypeLambdaFor), while other threads read the instance's own copy. Not
  // kept. Null when the lambda's environment is not known.
  LambdaExpr* privateLambdaInstance(const Ty* lambdaTy, LambdaExpr* body, const TemplateArgs& args);
  // Retypes such a copy for args, as retypeFor retypes a function's (packElement alike).
  void retypeLambdaFor(const Ty* lambdaTy, LambdaExpr* body, const TemplateArgs& args, bool packElement = false);
  TemplateArgs classArgsOf(const Ty* classType); // Parameters of the class template -> arguments of the type.
  // Overload resolution by conversion ranking among candidates (members: objectType is the
  // object they are called on, classArgs its instantiation). Null if none is viable. nullConstants:
  // per argument, whether it is the literal 0 (a null pointer constant), as the checker's calls rank it.
  FunctionDecl* chooseOverload(const std::vector<FunctionDecl*>& candidates, const std::vector<const Ty*>& argTypes,
                               const TemplateArgs& classArgs, const Ty* objectType, bool* ambiguous = nullptr,
                               const std::vector<bool>* nullConstants = nullptr);
  // A closure type's call operator as a member function ([expr.prim.lambda.closure] 4): a candidate
  // of chooseOverload beside a class's own (`using Ts::operator()...;` in a class derived from
  // closures), a declaration only, in no class's members; null for any other type.
  FunctionDecl* closureCallOperator(const Ty* lambdaTy);
  // The closure type whose call operator f is (closureCallOperator), or null: a call of it calls
  // the lambda, never a function of that name.
  const Ty* closureOfCallOperator(const FunctionDecl* f);
  // The constructor of class classType (known, not dependent) a construction with these arguments
  // runs (design.md 24: one rule for codegen, the interpreter and the checker): the class's own
  // constructors and those it inherits by `using Base::Base;` (a base's copy and move constructors
  // aside, and one with an own constructor's parameters hidden by it), chosen by overload
  // resolution; null if none is viable. nullConstants as for chooseOverload. inheritedFrom, when
  // given: the base class whose constructor won, else null.
  // implicitOnly: a copy-initialization (`T x = v;`, an argument, a return), which no explicit
  // constructor (under explicit by default: none not marked implicit) makes.
  FunctionDecl* chooseConstructor(const Ty* classType, const std::vector<const Ty*>& argTypes,
                                  const std::vector<bool>* nullConstants = nullptr, const Ty** inheritedFrom = nullptr, bool implicitOnly = false);
  // A kpp enum that declares constructors (kovarex, 2026-11-11: `Direction() { this = None; }`): its definition; null for
  // any other type, and for an enum declaring none. Its values are made by them as a class's objects are: one without an
  // initializer (a local, a member, an element, `E()`, `E{}`) by its default constructor; `E(x)` of one number or
  // enumeration value stays the conversion. chooseConstructor chooses among them.
  EnumDecl* enumWithConstructors(const Ty* t);
  // Which conversion operator of class `from` (own or a base's, the class first, then each base
  // level) a conversion runs (design.md 24: one rule for codegen, the interpreter and the checker), or
  // null. ToType: to `to` (implicit, or an explicit cast with explicitCast): the operator to `to`
  // itself first, else one to a class derived from `to`, else, for a scalar `to`, one to any scalar;
  // an explicit operator only for a bool or, in an explicit cast, to `to` or a derived class. A
  // template `operator T()` takes T = to when its constraints hold. ToPointer: a non-explicit one to a
  // pointer (`*object`, `delete object`). ToIntegral: a non-explicit one to an enumeration, integer,
  // char or bool (a switch). foundIn: the class declaring it; opArgs: what it is called with.
  // ToValue: as ToType, no conversion function giving an lvalue reference (an rvalue reference to a
  // scalar bound to what a class object converts to: `int&& r(object);` never takes `operator int&()`).
  // ToLvalue: as ToType, only those giving a non-const lvalue reference (a non-const `int&` bound to what
  // it converts to: `operator int&()`, never `operator int()` beside it, [over.match.ref] 1.1).
  // contextualBool: an explicit operator bool converts to bool (a condition, a direct-initialization);
  // false for a copy-initialization, which takes only implicit ones (C++ [class.conv.fct] 2).
  enum class ConversionKind : uint8_t { ToType, ToPointer, ToIntegral, ToValue, ToLvalue };
  FunctionDecl* chooseConversion(const Ty* from, const Ty* to, ConversionKind kind, bool explicitCast = false, const Ty** foundIn = nullptr,
                                 TemplateArgs* opArgs = nullptr, bool contextualBool = true);
  // The free function `operator<punctuator>(lhs[, rhs])` visible from context (the namespaces of
  // the operand classes and enums and context's own), chosen by overload ranking; null if none.
  FunctionDecl* findFreeOperator(uint32_t punctuator, const Ty* lhs, const Ty* rhs, Decl* context);
  // Whether a value of type from (an lvalue as a reference to its type) converts implicitly to to.
  bool converts(const Ty* from, const Ty* to);
  // The same for a free function by name (`begin(range)` and `end(range)` of a range-for over a
  // class without those members).
  FunctionDecl* findFreeFunction(std::string_view name, const std::vector<const Ty*>& argTypes, Decl* context);
  // What the compiler names in the standard library, std (design.md 16).
  FunctionDecl* libraryFunction(std::string_view name);
  bool isLibraryClass(const Ty* t, std::string_view name);
  // The class a friend function defined in a class (a hidden friend) was found through for these
  // argument types (an instance of a class template: its arguments are the function's); null for
  // any other function, or when two instances of its class are among the arguments.
  const Ty* hiddenFriendClass(FunctionDecl* f, const std::vector<const Ty*>& argTypes);
  // What a class operand converts to for a built-in operator when no operator function takes it
  // (`area < smallBlueprintAreaSize` with a Const<float>): the result type of its one non-explicit
  // conversion operator to a non-class type, its own or inherited; null when it has none or
  // several. The resolver types such operators by this.
  const Ty* scalarConversionOf(const Ty* classType);
  // The built-in operator `l op r` (r null: `op l`) takes when a class operand converts to several
  // scalar types (scalarConversionOf none), as C++ chooses it among the built-in candidates
  // ([over.built] 13-14, 24: every pair of promoted arithmetic types, of promoted integral ones for
  // % & | ^ << >>): each class operand converted to the candidate's parameter by chooseConversion,
  // the best candidate by the conversion sequences ([over.match.best], [over.ics.rank]). l, r: a
  // class object as T& for an lvalue, T&& for an rvalue (chooseConversion), a scalar as it is. lc,
  // rc: the parameter types of the candidate chosen, which each operand converts to. False when no
  // candidate takes them, when two are equally good (ambiguousOut), and where a class converts to
  // a pointer or an enumeration, whose candidates are not here.
  bool builtinOperands(uint32_t op, const Ty* l, const Ty* r, const Ty** lc, const Ty** rc, bool* ambiguousOut = nullptr);
  // The class instances the explicit instantiations of a file name (`template struct
  // Registry<int>;`, ParsedFile::explicitInstantiations), in order, each with the token it is
  // written at; one naming no instance of a class template is left out.
  std::vector<std::pair<const Ty*, uint32_t>> explicitInstantiationsOf(ParsedFile& file);
  // The user's free operator for `lhs op rhs` with an enumeration operand (`Flags operator|(Flags,
  // Flags)`), which wins over the built-in one; null when the built-in applies. The resolver types
  // the expression by the same rule.
  FunctionDecl* enumOperator(uint32_t punctuator, const Ty* lhs, const Ty* rhs, Decl* context);
  // Whether free operator f takes `lhs op rhs` strictly better than member operator member, which
  // C++ ranks together with the free ones ([over.match.oper]) and the resolver too: no worse for
  // either operand, better for one. freeArgs: f's class's arguments (a hidden friend's); memberArgs:
  // those of the class member was found in.
  bool freeOperatorBeatsMember(FunctionDecl* f, const TemplateArgs& freeArgs, FunctionDecl* member, const TemplateArgs& memberArgs, const Ty* lhs,
                               const Ty* rhs);
  // Whether the implicit copy or move assignment of class lhs beats member, the operator= of lhs's class codegen found for
  // rhs (memberArgs: the class's arguments), as the checker decides it: Impl::implicitAssignmentBeats.
  bool implicitAssignmentBeats(FunctionDecl* member, const TemplateArgs& memberArgs, const Ty* lhs, const Ty* rhs);
  // Adds to args what f's own template parameters deduce to from the argument types (for a
  // constructor template chosen by code generation; args holds the class's arguments).
  void deduceArgs(FunctionDecl* f, const std::vector<const Ty*>& argTypes, TemplateArgs& args);
  // Whether f's requires clause holds for args (true when it has none): for a template the code
  // generator picks itself (a conversion operator template for the type converted to).
  bool constraintsHold(FunctionDecl* f, const TemplateArgs& args);
  // The value of a builtin boolean type trait (`isArray<T>`, `isSame<A, B>`...) written as a
  // name with template arguments, under the instantiation args; nullopt if it is not one or
  // its arguments are still dependent.
  std::optional<bool> typeTraitValue(NameExpr* e, Decl* context, const TemplateArgs& args);
private:
  struct Impl;
  // Types `nodes` (f itself, or an instantiation's copy of f's nodes) for one instantiation of f;
  // returns what the body's returns deduced (an `auto` function's return type), or null.
  // retypingElements: a type pack bound to one element (retypeFor), whose static_asserts are not
  // checked again; keepBranches: retypeFor's packElement.
  const Ty* typeFunctionFor(FunctionDecl* f, FunctionDecl* nodes, const TemplateArgs& args, const Ty* objectType, bool retypingElements = false,
                            bool keepBranches = false);
  // The free function named symbol for these argument types, looked up from the namespaces of the
  // argument classes and enums and from context's (findFreeOperator, findFreeFunction).
  FunctionDecl* findFree(uint32_t symbol, const std::vector<const Ty*>& argTypes, Decl* context);
public:
  // Resolves `count` files with resolveOne(i) on `threads` workers, which take the class instances'
  // member bodies as tasks when they run out of files (queued by the files' walks instead of
  // checked by the asking walk; a task may queue more), until nothing is left.
  void resolveAll(size_t count, unsigned threads, const std::function<void(size_t)>& resolveOne);
  // The places in a file each instance walk's own statistics name ("first at"), to move with its tokens.
  void forEachInstanceStatsPlace(uint32_t fileIndex, const ParsedFile* file, const std::function<void(uint32_t& token)>& visit);
  // The instances' own copies of a file's nodes (bodies, initializers, generic lambdas' operators),
  // with the declaration each was copied from: they index the file's tokens too.
  void forEachInstanceCopy(const ParsedFile& file, const std::function<void(Node* copy, const Decl* source)>& visit);
  // The same for every file at once, with the file each copy's tokens index (`kpp --lsp`'s rename).
  void forEachInstanceCopy(const std::function<void(Node* copy, const Decl* source, const ParsedFile& file)>& visit);
  // The function instances' bodies, each with where the program's own code asked for it in the end
  // (through the instances that asked for one another: InstanceRecord::rootFile; null rootFile for
  // none): `kpp --lsp`'s references of a constructor an instance runs.
  void forEachInstanceRoot(const std::function<void(Node* copy, const ParsedFile& file, const ParsedFile* rootFile, uint32_t rootToken)>& visit);
private:
  Impl* impl;
};
