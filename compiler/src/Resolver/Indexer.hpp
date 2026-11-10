// GlobalScope::Impl, phase 1, indexing: part of the class's body, included in it in GlobalScopeImpl.hpp,
// its definitions in Indexer.cpp (resolver.cpp split, 2026-11-05, moved without a change).
  // ---------------- phase 1: indexing ----------------

  static uint32_t simpleName(const QualifiedName& name);

  // Symbol under which a (possibly operator) function is indexed, or UINT32_MAX.
  // The symbol a function is known by: its own name, the last part of `Class<T>::name`.
  uint32_t functionSymbol(const QualifiedName& name) const;

  // The out-of-line member function definitions indexing deferred (deferredFunctions), attached to
  // their classes (outOfLineMembers; out-of-line in a namespace: indexed there). finishIndex runs it,
  // and a compile server's build that adds such definitions (step 4).
  std::unordered_map<const FunctionDecl*, Scope*> outOfLineScope; // the class each out-of-line definition was attached to
  void attachOutOfLine(std::vector<FunctionDecl*>& attached);

  // The name a declaration a false `when` dropped declares in its scope (design.md 19), or none.
  uint32_t absentName(const Decl* d);

  void indexDecls(Span<Decl*> decls, Scope* scope);

  void indexDecl(Decl* d, Scope* scope);

  // Classes and enums declared in a function body get their scopes as nested ones do, but their
  // names stay out of the enclosing scope: the function's locals carry them (declareLocal). The
  // scope's parent is the function's, its class for a member function (finishIndex moves an
  // out-of-line member's ones under its class).
  // The aliases and typedefs of a function body in scope at token `at` (`using nl = Limits<T1>;`
  // before `nl::min()`): those declared before it in the blocks around it, in declaration order,
  // so that an inner one comes later and hides an outer one of its name.
  // What a block-scope `using Graphics::Architecture;` brings in: the declaration it names, unless
  // that is a function (whose overloads the using-declaration would take together).
  Decl* localUsingTarget(UsingNameDecl* u, Scope* scope, const LocalStack* locals);

  // `using Enum::True;` in a class or namespace: the enumerator it names, which the name means as
  // that enumerator's value (a constant, never an object); null for any other using-declaration.
  Decl* usingEnumerator(Decl* d, int depth);

  // Those visible at token `at` in the body of the lambda whose closure is context (the code
  // generator types a lambda's body from its closure): a body's declarations belong to the function
  // around the lambda, whose statements do not reach into it.
  void collectLambdaBodyAliases(Decl* context, uint32_t at, LocalStack& out);

  // `using enum UndoActionDescription::Type;` in a block: the enumeration's enumerators, as locals
  // of the block (C++20's using-enum-declaration).
  void pushLocalUsingEnum(UsingNameDecl* u, Scope* scope, LocalStack& locals);

  void collectVisibleLocalAliases(Stmt* s, uint32_t at, LocalStack& out, int depth = 0);

  void indexLocalTypes(Stmt* s, Scope* scope, FunctionDecl* f, std::unordered_map<uint32_t, std::vector<ClassDecl*>>* forward = nullptr);
  // f->elementBodies: a copy of a `for constexpr` body for each of its elements, if it declares what each element has its
  // own of; its local classes and enums given scopes of their own under scope, and the arguments around the loop (around)
  // with the variable bound to their element, as the classes of an instance's body have that instance's.
  void copyElementBodies(ForConstexprStmt* f, const std::vector<const Ty*>& elements, const Subst* around, Scope* scope, FunctionDecl* function);

  // Calls body with each lambda in expression n, not looking into the lambdas themselves (local
  // types in a lambda's body are indexed with the body: GraphicsInterfaceDX11::logDeviceInfo's
  // createDriverDescription declares an unnamed union).
  template<typename F>
  static void forEachLambda(Node* n, F&& body, int depth = 0)
  {
    if (!n || depth > 64)
      return;
    auto each = [&](Span<Expr*> list)
    {
      for (Expr* e : list)
        forEachLambda(e, body, depth + 1);
    };
    switch (n->kind)
    {
      case NodeKind::LambdaExpr: body(static_cast<LambdaExpr*>(n)); break;
      case NodeKind::CallExpr: forEachLambda(static_cast<CallExpr*>(n)->callee, body, depth + 1); each(static_cast<CallExpr*>(n)->args); break;
      case NodeKind::MemberExpr: forEachLambda(static_cast<MemberExpr*>(n)->base, body, depth + 1); break;
      case NodeKind::IndexExpr: forEachLambda(static_cast<IndexExpr*>(n)->base, body, depth + 1); each(static_cast<IndexExpr*>(n)->args); break;
      case NodeKind::UnaryExpr: forEachLambda(static_cast<UnaryExpr*>(n)->operand, body, depth + 1); break;
      case NodeKind::BinaryExpr:
        forEachLambda(static_cast<BinaryExpr*>(n)->lhs, body, depth + 1);
        forEachLambda(static_cast<BinaryExpr*>(n)->rhs, body, depth + 1);
        break;
      case NodeKind::ConditionalExpr:
        forEachLambda(static_cast<ConditionalExpr*>(n)->cond, body, depth + 1);
        forEachLambda(static_cast<ConditionalExpr*>(n)->then, body, depth + 1);
        forEachLambda(static_cast<ConditionalExpr*>(n)->else_, body, depth + 1);
        break;
      case NodeKind::CastExpr: forEachLambda(static_cast<CastExpr*>(n)->operand, body, depth + 1); break;
      case NodeKind::NamedCastExpr: forEachLambda(static_cast<NamedCastExpr*>(n)->operand, body, depth + 1); break;
      case NodeKind::FunctionalCastExpr: each(static_cast<FunctionalCastExpr*>(n)->args); break;
      case NodeKind::BraceInitExpr: each(static_cast<BraceInitExpr*>(n)->args); break;
      case NodeKind::ParenExpr: forEachLambda(static_cast<ParenExpr*>(n)->inner, body, depth + 1); break;
      case NodeKind::ThrowExpr: forEachLambda(static_cast<ThrowExpr*>(n)->operand, body, depth + 1); break;
      default: break;
    }
  }

  // The scope of a class or enum defined in a function body, its members indexed in it.
  Scope* indexLocalType(Decl* d, Scope* scope);

