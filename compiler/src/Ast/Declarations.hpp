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
constexpr uint16_t ATTR_ForeignVtable = 2048; // [[foreign_vtable]] on a class: an outside ABI (COM) fixes its vtable: no destructor in it, no delete, inherited (design.md 23b)
constexpr uint16_t ATTR_NoReturn = 4096; // [[noreturn]] on a function: it never returns (it throws, aborts or exits); a call of it ends a path (Walker::checkFunctionEnd)
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
  // How much deeper computing cachedType nested (the resolver's Nesting, packed: packNesting), stored
  // before cachedType (in the padding after isTemplate: no larger a Decl).
  std::atomic<uint32_t> typeNeeded{0};
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
  // Which element of a `for constexpr` this declaration's copy of the body is for (copyElementBodies; in the padding:
  // no larger a Decl): it tells apart the names of what each element has its own of. 0: none.
  uint32_t elementMark = 0;
  Expr* when = nullptr;
};

struct LambdaExpr;
struct NamespaceDecl : Decl { uint32_t name = UINT32_MAX; Span<Decl*> decls; bool isInline = false; QualifiedName nested; };
struct TemplateParamDecl : Decl { uint32_t name = UINT32_MAX; bool isType = true; bool isPack = false; Type* valueType = nullptr; Node* defaultArg = nullptr; Span<Decl*> innerParams; };
struct ClassDecl : Decl
{
  struct Base { Type* type; Access access; bool isVirtual; bool isPack; };

  Kw keyword = Kw::struct_;
  // A class declared in a function body, an anonymous one or a specialization: its number in its file in the parser's
  // order, which its name in the code carries with the file's tag (numberInFile in Ast.hpp, `f::Local@6c1f0e2a_3`), not
  // its place: a compile server keeps the code of what an edit above it only moved. UINT32_MAX: no number (in the
  // padding after keyword: no larger a ClassDecl).
  uint32_t number = UINT32_MAX;
  QualifiedName name; // Empty for anonymous.
  Span<Base> bases;
  Span<Decl*> members;
  bool isDefinition = false;
  bool isFinal = false;
  bool isClassConst = false;
  bool isClosure = false;         // Synthetic class standing for a lambda's closure type.
  uint8_t pack = 0;               // C: the #pragma pack(n) in force at its definition, 0 for none.
  Expr* alignment = nullptr;      // `struct alignas(16) X`: the alignment written.
  // `template<class T> requires Small<T> struct S<T>`: a partial specialization matches only where it
  // holds ([temp.spec.partial.match] 2), and it takes part in the constraints' subsumption.
  Expr* requiresClause = nullptr;
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
  uint32_t number = UINT32_MAX; // as ClassDecl::number: one declared in a function body, or an anonymous one
  Type* underlying = nullptr;
  Span<Decl*> enumerators;
  Span<Decl*> members; // kpp: methods inside enums.
  bool isKpp = false;  // kpp enum (values in their own brace block): count, values, str(), exhaustive switch.
};
// inheritedDefault: defaultValue is another declaration's of the function, which walks it (mergeDefaultArguments).
struct ParamDecl : Decl { Type* type; uint32_t name = UINT32_MAX; Expr* defaultValue = nullptr; bool isPack = false; bool inheritedDefault = false; };
struct FunctionDecl : Decl
{
  struct CtorInit { QualifiedName member; Span<Expr*> args; bool braced; bool isPack; };

  Type* returnType = nullptr; // Null for constructors, destructors, conversion operators.
  QualifiedName name;
  Span<Decl*> params; // ParamDecl*
  Type* trailingReturn = nullptr;
  Stmt* body = nullptr;
  // Test code (design.md 8.7): 1 a declaration, a test's own body or what the parser wrote around
  // it, 2 a test function's body, which starts with a __KppTestFrame.
  uint8_t testCode = 0;
  Span<CtorInit> ctorInits;
  Expr* requiresClause = nullptr;
  Expr* condition = nullptr; // conditional(expr): calls compile to nothing while expr is false.
  Expr* explicitCondition = nullptr; // explicit(expr): explicit only while expr (a template parameter, usually) is true.
  // The token of the `explicit` or `implicit` written on it (UINT32_MAX: neither), where a message's fix points
  // (design.md 23e). `implicit(cond)` is kept as explicitCondition `!(cond)`, with DF_Implicit and DF_Explicit.
  uint32_t conversionMark = UINT32_MAX;
  Expr* noexceptCondition = nullptr; // noexcept(expr): noexcept only while expr is true (`noexcept(false)`, `noexcept(isNothrowMoveConstructible<T>)`).
  // `Pair<int, Mode> five(a, m);` at namespace scope: the names as expressions. The resolver makes
  // the declaration an object constructed from them when they are variables (design.md rule 2).
  Span<Expr*> objectArgs;
  // An instantiation's own copy (ATTR_InstanceCopy): a hash of its arguments as printed, as a lambda
  // type's instance mark is, which the names of its local classes carry (TypeTable.cpp), so that two
  // instances' InnerSetupHelper are two names in every run.
  uint64_t instanceMark = 0;
  // A function at file or class level with a body: the tag its lambdas' closures and the classes and enumerations numbered in it
  // are named by (the parser's beginNumbering: its file's tag and its header's tokens), each numbered in it, so that what another
  // function of the file gains or loses renumbers none of them; 0 for any other function.
  uint32_t numberingTag = 0;
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
  // A static local: its number in the function at file or class level it is written in, which its storage's name
  // carries (Emitter::staticLocalName), not its place. UINT32_MAX: none.
  uint32_t number = UINT32_MAX;
  Expr* bitfield = nullptr;
  Span<Expr*> structuredBindings; // NameExpr* for `auto [a, b] = ...`
  Expr* alignment = nullptr; // alignas(N) on the variable or data member
  // A variable template's (`template<class T> requires Small<T> constexpr int v<T*> = 1;`): a partial
  // specialization matches only where it holds, and a use of the primary must satisfy it.
  Expr* requiresClause = nullptr;
};
// A class's, an enumeration's or a static local's number (ClassDecl::number, EnumDecl::number,
// VariableDecl::number); UINT32_MAX for any other declaration. A class's or an enumeration's numbered in the body of a
// function at file or class level has NumberInFunction set: its number is in that function (FunctionDecl::numberingTag).
constexpr uint32_t NumberInFunction = 0x80000000u;
inline uint32_t numberOf(const Decl* d)
{
  switch (d->kind)
  {
    case NodeKind::ClassDecl: return static_cast<const ClassDecl*>(d)->number;
    case NodeKind::EnumDecl: return static_cast<const EnumDecl*>(d)->number;
    case NodeKind::VariableDecl: return static_cast<const VariableDecl*>(d)->number;
    default: return UINT32_MAX;
  }
}
inline void setNumber(Decl* d, uint32_t number)
{
  if (d->kind == NodeKind::ClassDecl)
    static_cast<ClassDecl*>(d)->number = number;
  else if (d->kind == NodeKind::EnumDecl)
    static_cast<EnumDecl*>(d)->number = number;
  else if (d->kind == NodeKind::VariableDecl)
    static_cast<VariableDecl*>(d)->number = number;
}
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
struct DeductionGuideDecl : Decl { QualifiedName name; Span<Decl*> params; Type* target = nullptr; Expr* requiresClause = nullptr; };
struct AsmDecl : Decl {};
struct FriendDecl : Decl { Type* type; Decl* decl; };
struct ImportDecl : Decl { uint32_t pathToken; }; // extern "C" import "header.h"; (the string token)
