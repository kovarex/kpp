#pragma once
// The syntax tree's declaration nodes (Ast.hpp includes it, after Node.hpp).
#include "Node.hpp"

// --- Declarations ---

enum DeclFlags : uint32_t
{
  DF_Static = 1 << 0, DF_Virtual = 1 << 1, DF_Inline = 1 << 2, DF_Constexpr = 1 << 3,
  DF_Explicit = 1 << 4, DF_Friend = 1 << 5, DF_Extern = 1 << 6, DF_Mutable = 1 << 7,
  DF_Const = 1 << 8, DF_Override = 1 << 9, DF_Final = 1 << 10, DF_Pure = 1 << 11,
  DF_Default = 1 << 12, DF_Delete = 1 << 13, DF_Noexcept = 1 << 14, DF_ThreadLocal = 1 << 15,
  DF_Consteval = 1 << 16, DF_Implicit = 1 << 17, DF_Variadic = 1 << 18, DF_RefQual = 1 << 19,
  DF_Volatile = 1 << 20, DF_Typedef = 1 << 21, DF_Export = 1 << 22, DF_BothConst = 1 << 23,
  DF_Accumulate = 1 << 24, DF_Register = 1 << 25, DF_Operator = 1 << 26, DF_Constructor = 1 << 27,
  DF_Destructor = 1 << 28, DF_Conversion = 1 << 29, DF_Specialization = 1 << 30,
  DF_ExternC = 1u << 31, // Declared inside extern "C": C linkage, unmangled name.
};

enum class Access : uint8_t { Default, Public, Protected, Private };

struct Scope;
struct Ty;
struct ParsedFile;
constexpr uint8_t ATTR_NoInline = 1, ATTR_ForceInline = 2, ATTR_VectorCall = 4, ATTR_Draft = 8; // Decl::attributes, from [[...]] / resolver
constexpr uint8_t ATTR_Specialized = 16; // a class template that has specialisations (the resolver's finishIndex)
constexpr uint8_t ATTR_InstanceCopy = 32; // an instantiation's own copy of a function (InstanceCopier::function)
constexpr uint8_t ATTR_Packed = 64; // [[packed]] on a class: its members without padding, alignment 1 (C's #pragma pack: ClassDecl::pack)
constexpr uint16_t ATTR_NoDiscard = 256;  // [[nodiscard]] on a function, class or enumeration: a call's result discarded is a warning (Walker::checkDiscarded)
constexpr uint16_t ATTR_NoUniqueAddress = 1024; // [[no_unique_address]] on a data member: an empty one takes no storage (Emitter::buildStruct)
constexpr uint16_t ATTR_Deprecated = 512; // [[deprecated]], [[deprecated("why")]]: a call of the function is a warning (ParsedFile::deprecations has why)
constexpr uint8_t ATTR_MemberSpecialization = 128; // `template<> R UpdateBuffer::f()` through an alias of a class template's instance (finishIndex)
constexpr uint8_t BUILTIN_EnumCount = 1, BUILTIN_EnumValues = 2, BUILTIN_EnumStr = 3, BUILTIN_EnumFromStr = 4; // Decl::builtin: generated enum members

struct Decl : Node
{
  std::atomic<struct Dependencies*> typeDependencies{nullptr}; // What computing cachedType read (the resolver's, owned by it).

  ParsedFile* file = nullptr; // The file this declaration was parsed from.
  uint32_t flags = 0;
  Access access = Access::Default;
  Span<Decl*> templateParams; // TemplateParamDecl*. Non-empty for templates.
  bool isTemplate = false;    // `template<...>` was present (possibly empty for specializations).
  Decl* parent = nullptr;     // Lexically enclosing declaration, set by the parser.
  Scope* ownScope = nullptr;  // Set by the resolver for namespaces, classes and enums.
  std::atomic<const Ty*> cachedType{nullptr}; // Declared type, filled lazily by the type checker (non-dependent only).
  uint16_t attributes = 0;    // ATTR_* bits (in the padding before endToken: no larger a Decl).
  uint32_t endToken = 0;      // One past the declaration's last token (set when the parser finishes it).
  uint8_t builtin = 0;        // BUILTIN_*: a member the compiler generates (no body/initializer in the AST).
  bool filePrivate = false;   // `private` at file scope (design.md 15): a name its own file alone sees.
  // `when (condition) declaration;` (design.md 19): the declaration exists only while the constant
  // condition holds. whenKnown is the walk's answer (as IfStmt::constexprKnown): -1 not evaluated,
  // 0 false (no storage, nothing checked or emitted), 1 true; each instance's copy has its own.
  int8_t whenKnown = -1;
  Expr* when = nullptr;
};

struct LambdaExpr;
struct NamespaceDecl : Decl { uint32_t name = UINT32_MAX; Span<Decl*> decls; bool isInline = false; QualifiedName nested; };
struct TemplateParamDecl : Decl { uint32_t name = UINT32_MAX; bool isType = true; bool isPack = false; Type* valueType = nullptr; Node* defaultArg = nullptr; Span<Decl*> innerParams; };
struct ClassDecl : Decl
{
  struct Base { Type* type; Access access; bool isVirtual; bool isPack; };

  Kw keyword = Kw::struct_;
  QualifiedName name; // Empty for anonymous.
  Span<Base> bases;
  Span<Decl*> members;
  bool isDefinition = false;
  bool isFinal = false;
  bool isClassConst = false;
  bool isClosure = false;         // Synthetic class standing for a lambda's closure type.
  uint8_t pack = 0;               // C: the #pragma pack(n) in force at its definition, 0 for none.
  Expr* alignment = nullptr;      // `struct alignas(16) X`: the alignment written.
  // An imported C struct or union whose body the import could not read in full (a member in a
  // construct the C parser does not read): the parser's message and the symbol of the token it
  // stopped at. A use that needs its layout is an error (codegen's classDef), never a layout without
  // the member (FreeBSD's mcontext_t without its mc_fpstate was 288 bytes, not 800).
  const char* importError = nullptr;
  uint32_t importErrorAt = UINT32_MAX;
  LambdaExpr* lambda = nullptr;   // The lambda, for closures.
  // A class template declared in a class template (`Heap<V, Tag>::IteratorBase<Value>`): the
  // parameters its instances are made with, the enclosing template's then its own (set by the
  // resolver when it indexes the class; empty for any other class).
  Span<Decl*> instanceParams;
  // `union enum` (design.md 8.5): a tag and one live alternative. The parser desugars it to the
  // members below; the compiler adds the tag-aware operations.
  bool isTypedUnion = false;
  Decl* tagEnum = nullptr;        // the nested kpp enum `Tag` naming the alternatives (unscoped: `Property::Integer`)
  Decl* tagField = nullptr;       // the tag data member (`__tag`; the generated `tag()` returns it)
  Span<Decl*> alternatives;       // the storage member of each alternative, in tag order (inside an anonymous union)
  // The members a false `when` dropped in this configuration (design.md 19): not members, but names
  // a lookup in the class stops at (Scope::absent); a data member's constructor initializers are
  // dropped with it (absentMemberInit).
  Span<Decl*> absentMembers;
  // Whether the class defines a friend function (a hidden friend, which argument-dependent lookup
  // finds): 0 not looked at yet, 1 none, 2 some. The resolver's (hiddenFriendsOf), decided once.
  std::atomic<int8_t> hiddenFriendState{0};
};
struct EnumeratorDecl : Decl { uint32_t name; Expr* value = nullptr; };
struct EnumDecl : Decl
{
  QualifiedName name;
  bool isClass = false;
  bool isDefinition = false;
  Type* underlying = nullptr;
  Span<Decl*> enumerators;
  Span<Decl*> members; // kpp: methods inside enums.
  bool isKpp = false;  // kpp enum (values in their own brace block): count, values, str(), exhaustive switch.
};
struct ParamDecl : Decl { Type* type; uint32_t name = UINT32_MAX; Expr* defaultValue = nullptr; bool isPack = false; };
struct FunctionDecl : Decl
{
  struct CtorInit { QualifiedName member; Span<Expr*> args; bool braced; bool isPack; };

  Type* returnType = nullptr; // Null for constructors, destructors, conversion operators.
  QualifiedName name;
  Span<Decl*> params; // ParamDecl*
  Type* trailingReturn = nullptr;
  Stmt* body = nullptr;
  Span<CtorInit> ctorInits;
  Expr* requiresClause = nullptr;
  Expr* condition = nullptr; // conditional(expr): calls compile to nothing while expr is false.
  Expr* explicitCondition = nullptr; // explicit(expr): explicit only while expr (a template parameter, usually) is true.
  Expr* noexceptCondition = nullptr; // noexcept(expr): noexcept only while expr is true (`noexcept(false)`, `noexcept(isNothrowMoveConstructible<T>)`).
  // `Pair<int, Mode> five(a, m);` at namespace scope: the names as expressions. The resolver makes
  // the declaration an object constructed from them when they are variables (design.md rule 2).
  Span<Expr*> objectArgs;
  // An instantiation's own copy (ATTR_InstanceCopy): a hash of its arguments as printed, as a lambda
  // type's instance mark is, which the names of its local classes carry (TypeTable.cpp), so that two
  // instances' InnerSetupHelper are two names in every run.
  uint64_t instanceMark = 0;
  // A member function's ref qualifier (DF_RefQual says it has one): RefQualifier::LValue `f() &`,
  // which an lvalue object calls, RValue `f() &&`, which an rvalue one does ([over.match.funcs] 4).
  // Two overloads of one parameter list differing in it are two functions.
  enum class RefQualifier : uint8_t { None, LValue, RValue };
  RefQualifier refQualifier = RefQualifier::None;
};
struct VariableDecl : Decl
{
  Type* type = nullptr; // Type of the first declarator; others share the base type with their own declarator.
  QualifiedName name;
  Expr* init = nullptr;
  bool bracedInit = false;
  // A hidden `auto&& a = get<i>(e)` that a tuple-like structured binding names (the resolver's
  // tupleBindings); the binding's name node in the object's list points at it (resolved).
  bool tupleBinding = false;
  Expr* bitfield = nullptr;
  Span<Expr*> structuredBindings; // NameExpr* for `auto [a, b] = ...`
  Expr* alignment = nullptr; // alignas(N) on the variable or data member
};
// A constructor's initializer of a data member of cls (its definition) that a false `when` dropped:
// it exists exactly when the member does (design.md 19), so it does not either.
inline bool absentMemberInit(const ClassDecl* cls, const FunctionDecl::CtorInit& init)
{
  if (!cls || cls->absentMembers.empty() || init.member.parts.size() != 1 || init.member.global)
    return false;
  uint32_t name = init.member.parts[0].symbol;
  bool absent = false;
  for (const Decl* a : cls->absentMembers)
    absent = absent || (a->kind == NodeKind::VariableDecl && !static_cast<const VariableDecl*>(a)->name.empty() &&
                        static_cast<const VariableDecl*>(a)->name.last().symbol == name);
  if (absent) // (unless a member that exists has the name: `when (!c)`'s of `when (c)`'s)
    for (const Decl* m : cls->members)
      if (m->kind == NodeKind::VariableDecl && !static_cast<const VariableDecl*>(m)->name.empty() &&
          static_cast<const VariableDecl*>(m)->name.last().symbol == name)
        return false;
  return absent;
}
struct UsingAliasDecl : Decl { uint32_t name; Type* type; };
struct UsingDirectiveDecl : Decl { QualifiedName name; };
// isPack: `using Ts::operator()...;`, the name of each element of the pack Ts.
struct UsingNameDecl : Decl { QualifiedName name; bool isEnum = false; bool isPack = false; };
struct StaticAssertDecl : Decl { Expr* cond; Expr* message = nullptr; };
struct MacroDecl : Decl { uint32_t name; uint32_t endToken; };
struct ErrorDecl : Decl { uint32_t endToken; };
struct EmptyDecl : Decl {};
// `template<typename... Ts> Overload(Ts...) -> Overload<Ts...>;`: class template argument deduction
// from arguments of these parameter types gives target (DF_Explicit: not for copy-initialisation).
struct DeductionGuideDecl : Decl { QualifiedName name; Span<Decl*> params; Type* target = nullptr; };
struct AsmDecl : Decl {};
struct FriendDecl : Decl { Type* type; Decl* decl; };
struct ImportDecl : Decl { uint32_t pathToken; }; // extern "C" import "header.h"; (the string token)
