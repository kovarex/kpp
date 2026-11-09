// GlobalScope::Impl, phase 2, lookup: part of the class's body, included in it in GlobalScopeImpl.hpp,
// its definitions in Lookup.cpp (resolver.cpp split, 2026-11-05, moved without a change).
  // ---------------- phase 2: lookup ----------------

  // Declaration order across the program: the driver's file order (the library first), then the
  // position in the file.
  static bool declaredBefore(const Decl* a, const Decl* b);

  // Puts what the parallel index phase appended in declaration order: the entries of each name
  // (so that pick() and overload ties do not depend on which thread got there first), the
  // using-directives, `using enum`s and out-of-line members.
  void sortEntries(Scope* scope, std::unordered_set<Scope*>& seen);

  // Entries that open a scope are preferred (definition over forward declaration, primary
  // template over specialisation).
  static Entry pick(const std::vector<Entry>& entries);

  // The private names (design.md 15) of the file a lookup from context is made for: that of the
  // innermost private scope, class or enum around context, else the thread's (lookupFile: a
  // namespace's scope is every file's).
  Scope* privateScopeFor(Scope* context);

  // Another file than from's that declares symbol private (design.md 15), the first in file order,
  // for the error of naming it (null: none).
  const ParsedFile* filePrivateTo(uint32_t symbol, const ParsedFile* from);

  Scope* lexicalScopeOf(Decl* d);

  // Resolves a type to the scope it denotes (class/enum/namespace scope), following aliases.
  Scope* scopeOfType(Type* type, Scope* context, const LocalStack* locals, int depth);

  Scope* scopeOfEntry(const Entry& e, int depth);

  void noteRelations(Scope* s);

  void ensureBases(Scope* cls, int depth);

  void ensureUsing(Scope* ns, int depth);

  void ensureUsingEnums(Scope* cls, int depth);

  // Whether a member is one of each object (a non-static data member or member function): reached
  // through two base subobjects, it is two members.
  static bool isInstanceMember(const Decl* d);

  // Looks a symbol up in one scope: its own entries, template parameters, using-enums, bases.
  bool lookupInScope(Scope* s, uint32_t symbol, Lookup& out, int depth, bool withUsing = true, bool skipDependentBases = false);

  // The name in namespace s's inline namespaces (Scope::inlineScopes, and theirs), added to out: what
  // out found (found) and they find are one lookup, the functions of the name one overload set
  // ([namespace.qual] 2). Whether anything was found.
  bool withInlineNamespaces(Scope* s, uint32_t symbol, Lookup& out, bool found, int depth);

  // Whether a declaration can name a scope, as the part before `::` must: a namespace, a class,
  // an enumeration, an alias, a template parameter (C++ looks only at those there).
  static bool namesScope(const Decl* d);

  // qualifierOnly: the name is followed by `::` (`thisThread::get_id()` with a local variable
  // thisThread in scope): variables and functions of the name are passed over.
  // A lookup the name of a false `when` declaration stopped (design.md 19): nothing found, and why.
  static Lookup absentStop(const Lookup& l);

  Lookup lookupUnqualified(uint32_t symbol, Scope* context, const LocalStack* locals, int depth, bool qualifierOnly = false);
  // The type a NamedType names: an elaborated `struct stat s` passes over a variable or a function
  // of the name, as C++ does ([basic.lookup.elab]).
  Lookup lookupTypeName(const NamedType* t, Scope* context, const LocalStack* locals, int depth, const Subst* subst,
                        const std::vector<Decl*>* elementPacks = nullptr, uint32_t elementIndex = 0);

  // The nearest namespace enclosing both a and b (the global one at worst).
  static Scope* enclosingBoth(Scope* a, Scope* b);

  // The entity a namespace's using-declaration names (`using Colors::Red;`), else d itself.
  Decl* throughUsing(Decl* d, int depth);

  // One name found twice at one level (a namespace's own declaration and a using-directive's, or
  // two directives'): the same entity (also when one of them is a using-declaration of it, `using
  // Colors::Red;` beside `using namespace Colors;`), or functions (one overload set), or ambiguous.
  Lookup sameNameTogether(const Lookup& a0, const Lookup& b0, int depth);

  // A name in an alias's own definition, `using Option = Option<DataType>;` in a class deriving the
  // template: not yet the alias (C++ declares it after its type), but what the class's bases or
  // the enclosing scopes declare.
  Lookup lookupPastAlias(UsingAliasDecl* alias, uint32_t symbol, int depth);

  // The namespaces argument-dependent lookup searches for an argument of type t: a class's or an
  // enumeration's innermost enclosing namespace (not the global one, which ordinary lookup
  // searches anyway), and those of a class instance's type arguments.
  void associatedNamespaces(const Ty* t, std::vector<Scope*>& out, int depth = 0, std::vector<const Ty*>* visited = nullptr);

  // The friend functions defined in a class (hidden friends: static members to the compiler, the
  // parser's DF_Static) named symbol, which no lookup finds but argument-dependent lookup through an
  // argument of the class (or of one deriving from it, or of an instance with it as an argument).
  // Kept per class definition and symbol; a compile server's build that changed the index makes
  // them again (forgetMergedOverloads).
  std::mutex hiddenFriendsMutex;
  std::map<std::pair<const Decl*, uint32_t>, std::unique_ptr<std::vector<Entry>>> hiddenFriendSets;
  std::vector<ClassDecl*> hiddenFriendsDecided; // the classes whose hiddenFriendState is set (forgetMergedOverloads)
  const std::vector<Entry>* hiddenFriendsOf(ClassDecl* def, uint32_t symbol);

  // The hidden friends named symbol that argument-dependent lookup finds through an argument of
  // type t (its class, the class's bases, the classes of an instance's arguments), each set with
  // its class's template arguments.
  struct HiddenFriends
  {
    const std::vector<Entry>* entries;
    const Ty* cls;
  };
  void hiddenFriends(const Ty* t, uint32_t symbol, std::vector<HiddenFriends>& out, int depth = 0, std::vector<const Ty*>* visited = nullptr);

  // The hidden friends named symbol the arguments find (hiddenFriends) as one lookup, and in subst
  // their classes' template arguments; the classes of two found sets being instances of one template
  // (`Box<int> == Box<long>` with Box's friend operator==), whose parameters would take two
  // arguments, is said in conflict and nothing is found.
  Lookup hiddenFriendLookup(uint32_t symbol, const std::vector<const Ty*>& argTypes, Subst& subst, std::string* conflict = nullptr);

  // Two lookups' function overloads as one set (kept for the run: a lookup's overloads outlive it);
  // `a` alone when either found something other than functions.
  std::mutex mergedOverloadsMutex;
  std::map<std::pair<const void*, const void*>, std::unique_ptr<std::vector<Entry>>> mergedOverloads;
  // A compile server's build that changed the index (step 4): the merged sets are made again from
  // the entries as they are now; the old ones stay allocated (a lookup result may still point at one).
  std::vector<std::unique_ptr<std::vector<Entry>>> retiredOverloads;
  void forgetMergedOverloads();
  Lookup mergeOverloads(const Lookup& a, const Lookup& b);

  // A class and a function of one name in one scope: a C header's struct and function, which C keeps
  // apart (a struct's name is its tag: `struct stat` and stat(), statvfs), and a nested class and a
  // member function (or data member) of one class, which C++ allows. Where a name is a value (a
  // call's callee, an identifier, the operand of &, a member access), it is the function, or the
  // variable or enumerator, as C++ hides the class there ([basic.scope.hiding]/2); as a type it stays
  // the class (pick: no `struct` needed). At namespace scope only the imported headers': two of the
  // program's declarations of one name are the global clash, and an imported and a program's one
  // too, so kpp's own names never meet this there.
  static void preferValue(Lookup& l);

  // `using lib::size;` in namespace wrap beside wrap's own size(int) (Filesystem's `using
  // __fs::file_size;`): the functions the namespace's using-declarations of the name stand for and
  // its own, as one overload set (kept for the run); l itself when none of its entries is such a
  // using-declaration, or when one of them is no function.
  std::mutex usingOverloadsMutex;
  std::map<const void*, std::unique_ptr<std::vector<Entry>>> usingOverloads;
  Lookup withNamespaceUsings(const Lookup& l, int depth);

  // The using-directives among a body's locals (LocalStack::usingDirective items), innermost first.
  bool lookupInLocalUsings(uint32_t symbol, const LocalStack& locals, Lookup& out, int depth);

  // Whether two template arguments are the same: the same type, or constants of the same value
  // (`IntegralConstant<bool, isSigned<T>>` folds to a bool true, the specialisation's written
  // `true` is typed by its parameter `T V`: the same argument).
  static bool sameArgument(const Ty* a, const Ty* b);

  // Among a class template's entries, its primary template: the declaration that is no
  // specialisation, its definition when it has one. Null when the program declares none.
  static Decl* primaryOf(const std::vector<Entry>& entries);

  // The primary template's parameter that template argument i is for: its pack takes the rest.
  static Decl* parameterFor(Decl* primary, size_t i);

  static bool isPackParam(const Ty* t);

  // A partial specialisation's patterns (`<T, T, R...>`) as types, its own parameters standing for
  // themselves; a pack expansion is its pack parameter. False when one does not convert.
  bool specializationPatterns(ClassDecl* c, Decl* primary, std::vector<const Ty*>& out, int depth);

  // A specialisation written with fewer arguments than its primary has parameters
  // (`is_optional<Optional<T>>` of `template<typename T, typename Enable = void>`): the rest are
  // the primary's defaults, read with the arguments before them, as C++ completes it.
  // `enclosing`: a member template's enclosing class's arguments, which a default may name
  // (`template<typename U, typename W = T>` in Wrapper<T>).
  void appendDefaultPatterns(Decl* primary, std::vector<const Ty*>& out, int depth, const Subst* enclosing = nullptr);

  // Deduces partial specialisation c's parameters from the arguments: `<T, T, R...>` against
  // <B, B, C> makes T = B and R = {C}, against <C, A, B> it does not match. A pack expansion
  // among the patterns (the last one, as C++ requires) takes the rest of the arguments. An
  // argument that is itself a pack parameter (the patterns of another specialisation, when
  // ordering them) matches only a pack expansion. False when c does not match.
  bool matchPatterns(ClassDecl* c, Decl* primary, const std::vector<const Ty*>& args, std::vector<const Ty*>& deduced, int depth);

  // Whether partial specialisation a is at least as specialised as b: b's patterns match a's,
  // a's parameters standing for unique types (`<T, T, R...>` is, `<T, F, R...>` is not, either way).
  bool atLeastAsSpecialized(ClassDecl* a, ClassDecl* b, Decl* primary, int depth);

  bool argsDependent(const std::vector<const Ty*>& args);

  // Among the entries a class template's name resolved to, the specialisation for `args`: a full
  // one whose arguments are exactly `args` (its own class, without arguments), or else the most
  // specialised partial one (`template<typename T> struct Describe<T*>`) whose patterns deduce
  // from them (its class with the deduced arguments of its own parameters). A pack among `args`
  // stands for its elements. Null when there is none or an argument is still dependent.
  // tied: when no matching partial specialization is more specialized than every other (C++:
  // ambiguous, [temp.spec.partial.match] 1), those no other is more specialized than; the first
  // declared of them is still the result, for the checker to report (checkSpecializationTie).
  const Ty* matchSpecialization(const Lookup& l, const std::vector<const Ty*>& written, int depth, std::vector<ClassDecl*>* tied = nullptr,
                                const Subst* enclosing = nullptr);

  // A name as written, found from context: what lookupWritten finds, a using-declaration of an
  // enumerator taken for that enumerator (usingEnumerator). elementPacks, elementIndex: the name is in
  // the pattern of a pack expansion typed for one element (TypeContext::elementPacks), its own template
  // arguments and qualifiers naming that element of the packs (`IsRef<Ts>::v`, `Ts::value`).
  Lookup lookupQualified(const QualifiedName& name, Scope* context, const LocalStack* locals, int depth, const Subst* subst = nullptr,
                         const std::vector<Decl*>* elementPacks = nullptr, uint32_t elementIndex = 0);

  // A lookup in s, and in the global scope (`::helper`) first in the private names of the file the
  // lookup from context is made for (design.md 15), which hide the global ones there.
  bool lookupInScopeOrPrivate(Scope* s, Scope* context, uint32_t symbol, Lookup& out, int depth);

  // --std-scope's bridge, until every program writes std:: (the fallback of the move into std): a name the global
  // scope does not declare is looked up in std, and functions of a name both declare are one overload set, as
  // they were when the library's names were global. `out` is what the global scope found (found); whether there is
  // anything now.
  bool bridgedToStd(uint32_t symbol, Lookup& out, bool found, int depth);

  // The same bridge for a declaration: `template<> struct Hash<Key>` written bare specializes std's Hash, when the
  // global scope has no such template of its own.
  void moveBareSpecializationsToStd();

  Lookup lookupWritten(const QualifiedName& name, Scope* context, const LocalStack* locals, int depth, const Subst* subst = nullptr,
                       const std::vector<Decl*>* elementPacks = nullptr, uint32_t elementIndex = 0);

