#pragma once
// Order-independent name resolution and type checking: phase 1 indexes every file's declarations
// into one shared scope tree (in parallel), phase 2 resolves type names and identifiers in all
// declarations and function bodies against that tree and computes the type of every expression
// (in parallel; the tree is read-only, canonical types are interned in a shared table).
#include "ast.hpp"
#include "consteval.hpp"
#include "types.hpp"
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
  std::vector<std::pair<Decl*, const Ty*>> pairs;
  // While one element of a pack expansion is emitted: the packs its pattern expands and the
  // element's index. The pattern's own mentions of those packs name that element; everything else
  // (the function's parameters, the class instance, its members) keeps the whole packs the pairs
  // bind (`(OrderList::update<Ts>(lists[OrderList::getIndex<Ts>()]), ...)` with lists an
  // `Array<Entry, sizeof...(Ts)>&`).
  std::vector<Decl*> elementPacks;
  uint32_t elementIndex = 0;
  bool empty() const { return pairs.empty(); }
};

// Whether an expression names an object that outlives the expression (an lvalue): a variable or
// parameter (a named rvalue reference included), a field, an element of an array or pointer, a
// dereference. Calls and operators that yield references carry the reference in their type.
inline bool isLvalueExpression(const Expr* e)
{
  if (!e)
    return false;
  switch (e->kind)
  {
    case NodeKind::NameExpr:
    {
      Decl* d = static_cast<const NameExpr*>(e)->resolved;
      return d && (d->kind == NodeKind::VariableDecl || d->kind == NodeKind::ParamDecl);
    }
    case NodeKind::MemberExpr:
    {
      Decl* d = static_cast<const MemberExpr*>(e)->resolved;
      return d && d->kind == NodeKind::VariableDecl;
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
      return !u->isKeyword && !u->postfix && P(u->op) == P::Star && t && t->kind == TyKind::Pointer;
    }
    case NodeKind::ParenExpr:
      return isLvalueExpression(static_cast<const ParenExpr*>(e)->inner);
    case NodeKind::ConditionalExpr:
    {
      // `c ? shift.second : shift.first`: two lvalues of one type make an lvalue (C++).
      auto* c = static_cast<const ConditionalExpr*>(e);
      if (!c->then || !c->else_ || !c->then->ty || !c->else_->ty || !isLvalueExpression(c->then) || !isLvalueExpression(c->else_))
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

struct ResolveStats
{
  // Type names (in declarations, casts, template arguments...).
  uint64_t typesResolved = 0, typesDependent = 0, typesUnresolved = 0, typesBuiltinByName = 0;
  // Identifiers in expressions.
  uint64_t namesLocal = 0, namesMember = 0, namesGlobal = 0, namesDependent = 0, namesUnresolved = 0;
  uint64_t qualifiedUnresolvedStd = 0; // std:: and other names that are not part of the corpus.
  uint64_t unresolvedMacroLike = 0;    // ALL_CAPS identifiers: almost always macros.
  std::unordered_map<uint32_t, uint32_t> unresolvedSymbols; // symbol -> count
  struct Where { uint32_t file; uint32_t token; };
  std::unordered_map<uint32_t, Where> unresolvedFirst; // symbol -> first occurrence
  uint32_t fileIndex = 0; // Set by the driver before resolving, so first occurrences can be reported.

  // Member accesses `a.b` / `a->b` / `this->b`, bound through the type of the object.
  uint64_t membersBound = 0, membersDependent = 0, membersUnknownBase = 0, membersNotFound = 0;
  std::unordered_map<uint32_t, uint32_t> missingMembers; // symbol -> count (known class, no such member)
  std::unordered_map<uint32_t, Where> missingMembersFirst;
  std::unordered_map<uint32_t, uint32_t> unknownBaseMembers; // member name -> count, when the object type is unknown

  // Expressions.
  uint64_t exprsTyped = 0, exprsDependent = 0, exprsUnknown = 0;
  // Why an expression's type is unknown (first cause wins).
  uint64_t unknownName = 0, unknownBase = 0, unknownMember = 0, unknownOverload = 0, unknownDeduction = 0,
           unknownAutoReturn = 0, unknownUnsupported = 0, unknownOperator = 0, unknownAuto = 0;
  uint64_t callsResolved = 0, callsAmbiguousArity = 0;
  // Expressions typed while checking template bodies for an instantiation (also included above).
  uint64_t instExprsTyped = 0, instExprsDependent = 0, instExprsUnknown = 0;

  // --why: where expression types became unknown, keyed by cause << 56 | node kind << 32 | the
  // name or operator symbol of the expression whose typing failed.
  bool collectWhy = false;
  struct WhySite { const ParsedFile* file; uint32_t token; uint32_t count; };
  std::unordered_map<uint64_t, WhySite> why;

  void add(const ResolveStats& o);
};

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
  // Resolves `count` files with resolveOne(i) on `threads` workers, which take the class instances'
  // member bodies as tasks when they run out of files (queued by the files' walks instead of
  // checked by the asking walk; a task may queue more), until nothing is left.
  void resolveAll(size_t count, unsigned threads, const std::function<void(size_t)>& resolveOne);
  // After the checker (and the code generator, which asks it for instance bodies) is done: the
  // static_asserts of the class template instances it made are checked, and the errors the checker
  // found are returned, `path:line: error: ...` one each, sorted. A static_assert whose condition
  // is false, or no constant the checker can evaluate, is one. instancesChecked: how many of the
  // instances the code generator of this same build checked already (checkInstanceStaticAsserts'
  // result), 0 for all.
  std::vector<std::string> checkerErrors(size_t instancesChecked = 0);
  // Access checking, warnings only, after enableAccessWarnings() before the resolve (kppc
  // --access-warnings; design.md, "struct and class"): the uses of private and protected members
  // outside their class, its derived classes (protected) and its friends, and the structs with a
  // private or protected member, a base that is not public or a virtual function. accessReport()
  // after the build: the warnings (`path:line: warning: ...`, each once, by file and line), with a
  // note for each use a friend declaration allows and for each friend declaration, and in summary
  // their counts.
  void enableAccessWarnings();
  // A constructor codegen calls in the code of `in` (the function or the global's initializer being
  // emitted), at node at: checked as a use of the member it is (the checker does not choose
  // constructors, codegen does). Nothing without enableAccessWarnings().
  void checkConstructorAccess(FunctionDecl* ctor, Decl* in, Node* at);
  // The struct rule as errors (kppc --struct-rule; design.md "struct and class"): a struct with a
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
  void setLP64(bool lp64);
  // A conversion's resolve (kppc --convert): both branches of an `if constexpr` in a function that is no
  // template are walked, as C++ checks them (the converter rewrites both); a build walks the taken one.
  void setConverting(bool converting);
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
    uint32_t symbol;
    const void* classScope;
    bool memberOnly = false; // a member function (no friend): only lookups in its class, or a class derived from it, reach it
    static inline const void* const anyClass = reinterpret_cast<const void*>(uintptr_t(1));
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
  // The places in a file each instance walk's own statistics name ("first at"), to move with its tokens.
  void forEachInstanceStatsPlace(uint32_t fileIndex, const ParsedFile* file, const std::function<void(uint32_t& token)>& visit);
  // The instances' own copies of a file's nodes (bodies, initializers, generic lambdas' operators),
  // with the declaration each was copied from: they index the file's tokens too.
  void forEachInstanceCopy(const ParsedFile& file, const std::function<void(Node* copy, const Decl* source)>& visit);
  // The same for every file at once, with the file each copy's tokens index (`kppc --lsp`'s rename).
  void forEachInstanceCopy(const std::function<void(Node* copy, const Decl* source, const ParsedFile& file)>& visit);

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
  // How an argument of type t (e: the expression, for a string literal) packed into a Text appends
  // itself (TextPieceKind): by its type alone, wherever the call is.
  TextPieceKind textPieceOf(const Expr* e, const Ty* t);
  Decl* typeInfoDecl();                         // The library's TypeInfo class (what typeid yields), or null.
  Decl* enumFromStrDecl();                       // The library's enumFromStr<E>(const char*) template (E::fromStr lowers to it), or null.
  Decl* strongOrderingDecl();                    // The library's strong_ordering class (what `a <=> b` yields), or null.
  bool isExplicit(FunctionDecl* f, const TemplateArgs& args); // `explicit`, or `explicit(cond)` with cond true under args.
  bool isNoexcept(FunctionDecl* f, const TemplateArgs& args); // `noexcept`, or `noexcept(cond)` with cond true under args.
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
  // The arguments for the out-of-class definition of a class template's static data member:
  // classArgs, and the definition's own declarations of the class's parameters bound the same.
  TemplateArgs definitionArgs(Decl* def, const TemplateArgs& classArgs);
  // The in-class declaration of an out-of-line member definition (`static` and `virtual` are
  // written there and copied onto the definition); null when there is none.
  FunctionDecl* declarationOf(FunctionDecl* def);
  std::string qualifiedName(Decl* d);            // `ns::Class::name`
  // The name a type name declared nowhere was written with, for the placeholder class the resolver
  // gives it (a draft, ATTR_Draft) so that checking goes on; "?" for any other declaration.
  std::string draftName(const Decl* d);
  bool isExternC(FunctionDecl* f);
  bool isTemplateContext(Decl* d);               // Inside a template (any enclosing declaration has parameters).
  // Instantiation queries, single-threaded (code generation):
  const Ty* typeOfDeclIn(Decl* d, const TemplateArgs& args);
  const Ty* typeOfAstIn(Type* t, Decl* context, const TemplateArgs& args);
  // The value of the constant expression e (read from file, written in context: a function, the
  // variable whose initializer it is, a class) under args: the checker's compile-time evaluator,
  // with the code generator's arithmetic mode, enumeration storage and layouts (hooks). Null when
  // e is no constant.
  std::optional<ConstValue> evaluate(Expr* e, ParsedFile* file, Decl* context, const TemplateArgs& args, const ConstHooks& hooks);
  // The value init gives an object of type t when it initializes one (a global's or static
  // member's initializer: `Flags f = {A, B};`, `Array<int, 3> a = {1, 2, 3}`, `Box b = Box(1, 2);`):
  // a brace list's elements, a constructor's arguments, a conversion; context and hooks as for
  // evaluate. Null when it is no constant (the object is then initialized at run time).
  // build.kpp at compile time (design.md 12): a Project default-constructed, its configuration
  // the enumerator of that name, build(project) run, then describe(project): that String's
  // characters. Nullopt with error ("Build.kpp:12: why") when that is no constant (a call it
  // cannot run, memory left allocated, more than a million steps).
  std::optional<std::string> runBuild(std::string_view configuration, std::string& error);
  // Why this thread's last evaluation that failed is no constant ("a call to puts, ..."), empty
  // when the interpreter cannot evaluate it yet; cleared by clearConstantReason.
  static std::string constantReason();
  static void clearConstantReason();
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
  TemplateArgs classArgsOf(const Ty* classType); // Parameters of the class template -> arguments of the type.
  // Overload resolution by conversion ranking among candidates (members: objectType is the
  // object they are called on, classArgs its instantiation). Null if none is viable. nullConstants:
  // per argument, whether it is the literal 0 (a null pointer constant), as the checker's calls rank it.
  FunctionDecl* chooseOverload(const std::vector<FunctionDecl*>& candidates, const std::vector<const Ty*>& argTypes,
                               const TemplateArgs& classArgs, const Ty* objectType, bool* ambiguous = nullptr,
                               const std::vector<bool>* nullConstants = nullptr);
  // The free function `operator<punctuator>(lhs[, rhs])` visible from context (the namespaces of
  // the operand classes and enums and context's own), chosen by overload ranking; null if none.
  FunctionDecl* findFreeOperator(uint32_t punctuator, const Ty* lhs, const Ty* rhs, Decl* context);
  // Whether a value of type from (an lvalue as a reference to its type) converts implicitly to to.
  bool converts(const Ty* from, const Ty* to);
  // The same for a free function by name (`begin(range)` and `end(range)` of a range-for over a
  // class without those members).
  FunctionDecl* findFreeFunction(std::string_view name, const std::vector<const Ty*>& argTypes, Decl* context);
  // The class a friend function defined in a class (a hidden friend) was found through for these
  // argument types (an instance of a class template: its arguments are the function's); null for
  // any other function, or when two instances of its class are among the arguments.
  const Ty* hiddenFriendClass(FunctionDecl* f, const std::vector<const Ty*>& argTypes);
  // What a class operand converts to for a built-in operator when no operator function takes it
  // (`area < smallBlueprintAreaSize` with a Const<float>): the result type of its one non-explicit
  // conversion operator to a non-class type, its own or inherited; null when it has none or
  // several. The resolver types such operators by this.
  const Ty* scalarConversionOf(const Ty* classType);
  // The class instances the explicit instantiations of a file name (`template struct
  // Registry<int>;`, ParsedFile::explicitInstantiations), in order, each with the token it is
  // written at; one naming no instance of a class template is left out.
  std::vector<std::pair<const Ty*, uint32_t>> explicitInstantiationsOf(ParsedFile& file);
  // The user's free operator for `lhs op rhs` with an enumeration operand (`Flags operator|(Flags,
  // Flags)`), which wins over the built-in one; null when the built-in applies. The resolver types
  // the expression by the same rule.
  FunctionDecl* enumOperator(uint32_t punctuator, const Ty* lhs, const Ty* rhs, Decl* context);
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
  Impl* impl;
  // Types `nodes` (f itself, or an instantiation's copy of f's nodes) for one instantiation of f;
  // returns what the body's returns deduced (an `auto` function's return type), or null.
  // retypingElements: a type pack bound to one element (retypeFor), whose static_asserts are not
  // checked again; keepBranches: retypeFor's packElement.
  const Ty* typeFunctionFor(FunctionDecl* f, FunctionDecl* nodes, const TemplateArgs& args, const Ty* objectType, bool retypingElements = false,
                            bool keepBranches = false);
  // The free function named symbol for these argument types, looked up from the namespaces of the
  // argument classes and enums and from context's (findFreeOperator, findFreeFunction).
  FunctionDecl* findFree(uint32_t symbol, const std::vector<const Ty*>& argTypes, Decl* context);
};
