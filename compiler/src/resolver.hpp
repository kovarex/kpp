#pragma once
// Order-independent name resolution and type checking: phase 1 indexes every file's declarations
// into one shared scope tree (in parallel), phase 2 resolves type names and identifiers in all
// declarations and function bodies against that tree and computes the type of every expression
// (in parallel; the tree is read-only, canonical types are interned in a shared table).
#include "ast.hpp"
#include "consteval.hpp"
#include "types.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
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

// The type an argument expression presents to overload resolution: an lvalue of a non-reference
// type is an lvalue reference to it, so that `T&&` parameters take only temporaries and `move(x)`
// (C++ rules); `t` is the expression's type, or one element of a pack it expands.
inline const Ty* argumentType(const Expr* e, const Ty* t, TypeTable& types)
{
  if (!t || !t->isKnown() || t->kind == TyKind::Pack || !isLvalueExpression(e))
    return t;
  if (t->kind == TyKind::RRef)
    return types.lref(t->inner); // a named rvalue reference is an lvalue
  return t->isReference() ? t : types.lref(t);
}

// The type a brace list `{a, b, c}` presents to overload resolution: the brace list type
// (TypeTable::braceList) of the first element's type, the first of a pack's for `{args...}` (null
// when the elements are untyped or there is no such class). Only for ranking and deduction: the
// expression itself stays untyped, since a brace list also initialises aggregates and calls
// constructors. Each element initialises one of the list's from what it is, so when every
// element is an lvalue the element type is an lvalue reference: a converting constructor taking
// `T&` takes them (`implicit BoxRef(Box& b)` for `{a, b}`).
inline const Ty* braceListType(const Expr* e, Decl* initializerList, TypeTable& types)
{
  if (!e || e->kind != NodeKind::BraceInitExpr || !initializerList)
    return nullptr;
  auto* b = static_cast<const BraceInitExpr*>(e);
  if (b->args.empty() || !b->args[0])
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
  // After the checker (and the code generator, which asks it for instance bodies) is done: the
  // static_asserts of the class template instances it made are checked, and the errors the checker
  // found are returned, `path:line: error: ...` one each, sorted. A static_assert whose condition
  // is false, or no constant the checker can evaluate, is one.
  std::vector<std::string> checkerErrors();
  // A static_assert in the body of a template instance's function is checked only if the program
  // uses that function, as C++ instantiates a member or function template only where it is used:
  // the code generator says which instance bodies (GlobalScope::instanceBody) it emitted, and
  // checkerErrors() has those functions' failed asserts. unusedFailedAsserts(): how many failed in
  // functions it never emitted (all of them, when no code is generated).
  void functionEmitted(const FunctionDecl* f);
  size_t unusedFailedAsserts();
  // The first part of checkerErrors: the static_asserts of the class template instances named so
  // far. The code generator runs it once its functions are emitted (they name instances of their
  // own), before it checks the asserts on layouts.
  void checkInstanceStaticAsserts();
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

  // Queries for code generation (after resolveFile ran on every file).
  TypeTable& types();
  const Ty* typeOfDecl(Decl* d);                 // Declared (or deduced) canonical type.
  const Ty* typeOfAst(Type* t, Decl* context);   // A written type, resolved in the context of a declaration.
  Decl* definitionOf(Decl* d);                   // Class/enum definition, or the out-of-line definition of a member function or a static data member.
  // The same for a member of a class template instantiation (classArgs: the class's arguments):
  // the explicit specialisation written for it (`template<> R Button<RecipeID>::f()`), if any.
  Decl* definitionOf(Decl* d, const TemplateArgs& classArgs);
  Decl* initializerListDecl();                   // The library's InitializerList class, or null.
  Decl* typeInfoDecl();                          // The library's TypeInfo class (what typeid yields), or null.
  Decl* enumFromStrDecl();                       // The library's enumFromStr<E>(const char*) template (E::fromStr lowers to it), or null.
  Decl* strongOrderingDecl();                    // The library's strong_ordering class (what `a <=> b` yields), or null.
  bool isExplicit(FunctionDecl* f, const TemplateArgs& args); // `explicit`, or `explicit(cond)` with cond true under args.
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
  // object they are called on, classArgs its instantiation). Null if none is viable.
  FunctionDecl* chooseOverload(const std::vector<FunctionDecl*>& candidates, const std::vector<const Ty*>& argTypes,
                               const TemplateArgs& classArgs, const Ty* objectType, bool* ambiguous = nullptr);
  // The free function `operator<punctuator>(lhs[, rhs])` visible from context (the namespaces of
  // the operand classes and enums and context's own), chosen by overload ranking; null if none.
  FunctionDecl* findFreeOperator(uint32_t punctuator, const Ty* lhs, const Ty* rhs, Decl* context);
  // Whether a value of type from (an lvalue as a reference to its type) converts implicitly to to.
  bool converts(const Ty* from, const Ty* to);
  // The same for a free function by name (`begin(range)` and `end(range)` of a range-for over a
  // class without those members).
  FunctionDecl* findFreeFunction(std::string_view name, const std::vector<const Ty*>& argTypes, Decl* context);
  // What a class operand converts to for a built-in operator when no operator function takes it
  // (`area < smallBlueprintAreaSize` with a Const<float>): the result type of its one non-explicit
  // conversion operator to a non-class type, its own or inherited; null when it has none or
  // several. The resolver types such operators by this.
  const Ty* scalarConversionOf(const Ty* classType);
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
