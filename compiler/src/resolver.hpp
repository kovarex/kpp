#pragma once
// Order-independent name resolution and type checking: phase 1 indexes every file's declarations
// into one shared scope tree (in parallel), phase 2 resolves type names and identifiers in all
// declarations and function bodies against that tree and computes the type of every expression
// (in parallel; the tree is read-only, canonical types are interned in a shared table).
#include "ast.hpp"
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
// constructors.
inline const Ty* braceListType(const Expr* e, Decl* initializerList, TypeTable& types)
{
  if (!e || e->kind != NodeKind::BraceInitExpr || !initializerList)
    return nullptr;
  auto* b = static_cast<const BraceInitExpr*>(e);
  if (b->args.empty() || !b->args[0] || !b->args[0]->ty || !b->args[0]->ty->isKnown())
    return nullptr;
  const Ty* first = b->args[0]->ty;
  if (first->kind == TyKind::Pack)
  {
    if (first->args.empty() || !first->args[0] || !first->args[0]->isKnown())
      return nullptr;
    first = first->args[0];
  }
  return types.braceList(initializerList, types.decay(first));
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
  // fileSuffix, with its type (to stderr, while resolving).
  void traceLine(const std::string& fileSuffix, uint32_t line);

  // Phase 1, thread-safe: index all top-level declarations of one file.
  void indexFile(ParsedFile& file);
  // After all files are indexed: attach out-of-line class definitions (`struct A::B {}`).
  void finishIndex();
  // Phase 2, thread-safe and read-only on the scope tree: resolve and type one file.
  void resolveFile(ParsedFile& file, const std::vector<Token>& tokens, std::string_view text, ResolveStats& stats);

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
  // Retypes the initializer of a static data member of a class template for one instantiation
  // (`static constexpr T ValuesPerOne = T(1) << Bits` with T and Bits bound), in place like retypeFor.
  void retypeInitializerFor(VariableDecl* v, const TemplateArgs& args);
  // The declaration a variable template takes its value from for these arguments (the
  // primary's parameters bound, defaults included): the most specialised matching partial or
  // full specialisation, its own parameters bound in specArgs, or the primary (specArgs = args).
  // A specialisation matches when its written arguments deduce to these and stay well-formed with
  // them (`force_memcpy_save<T, VoidT<T::force_memcpy_save>>` needs T to have the member). Null
  // when no match is more specialised than all the others (ambiguous, an error in C++).
  VariableDecl* variableTemplateFor(VariableDecl* primary, const TemplateArgs& args, TemplateArgs& specArgs);
  Decl* owningClass(Decl* d);                    // The class a member (possibly defined out of line) belongs to.
  // The in-class declaration of an out-of-line member definition (`static` and `virtual` are
  // written there and copied onto the definition); null when there is none.
  FunctionDecl* declarationOf(FunctionDecl* def);
  std::string qualifiedName(Decl* d);            // `ns::Class::name`
  bool isExternC(FunctionDecl* f);
  bool isTemplateContext(Decl* d);               // Inside a template (any enclosing declaration has parameters).
  // Instantiation queries, single-threaded (code generation):
  const Ty* typeOfDeclIn(Decl* d, const TemplateArgs& args);
  const Ty* typeOfAstIn(Type* t, Decl* context, const TemplateArgs& args);
  // Re-types the body of f for one instantiation (expression types and resolved callees are
  // rewritten in place). objectType: the instantiated class for members, or null.
  void retypeFor(FunctionDecl* f, const TemplateArgs& args, const Ty* objectType);
  // Re-types the body of a generic lambda (one with template parameters, written or invented for
  // its `auto` parameters) for one set of its own arguments, in the environment the lambda was
  // written in. Returns its call operator's type (parameters and result) or null; fullArgs gets
  // the enclosing instantiation's arguments plus the lambda's own, for declarations in the body.
  const Ty* retypeLambdaFor(const Ty* lambdaTy, const TemplateArgs& own, TemplateArgs* fullArgs);
  TemplateArgs classArgsOf(const Ty* classType); // Parameters of the class template -> arguments of the type.
  // Overload resolution by conversion ranking among candidates (members: objectType is the
  // object they are called on, classArgs its instantiation). Null if none is viable.
  FunctionDecl* chooseOverload(const std::vector<FunctionDecl*>& candidates, const std::vector<const Ty*>& argTypes,
                               const TemplateArgs& classArgs, const Ty* objectType, bool* ambiguous = nullptr);
  // The free function `operator<punctuator>(lhs[, rhs])` visible from context (the namespaces of
  // the operand classes and enums and context's own), chosen by overload ranking; null if none.
  FunctionDecl* findFreeOperator(uint32_t punctuator, const Ty* lhs, const Ty* rhs, Decl* context);
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
  // The value of a builtin boolean type trait (`isArray<T>`, `isSame<A, B>`...) written as a
  // name with template arguments, under the instantiation args; nullopt if it is not one or
  // its arguments are still dependent.
  std::optional<bool> typeTraitValue(NameExpr* e, Decl* context, const TemplateArgs& args);


private:
  struct Impl;
  Impl* impl;
};
