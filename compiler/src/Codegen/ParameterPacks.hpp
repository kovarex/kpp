// struct Emitter's members: parameter packs: the declarations, part of the struct's body, included
// in it in Codegen/Emitter.hpp; the definitions in ParameterPacks.cpp (codegen.cpp split, stage 2,
// 2026-11-05, moved without a change).
    // --- parameter packs ---

    // The pack parameter an expression refers to (the first one found), or null.
    Decl* packIn(Node* n, int depth = 0);

    // The template parameter packs a pack parameter's declared type names, in order: `Args&&... args`
    // names Args; `auto PrototypeT::*... members` names PrototypeT and the pack invented for the
    // `auto`. An expansion binds each of them to its i-th element together.
    std::vector<Decl*> templatePacksOf(Decl* param);

    static void collectPacks(const Ty* t, std::vector<Decl*>& packs, int depth);

    // The element types each pack named by param's type is bound to in the instantiation being
    // emitted, all of `count` elements; unsupported when one is not bound to such a pack.
    std::vector<const Ty*> boundPacks(const std::vector<Decl*>& packs, size_t count, Node* at);

    // Runs body(i) for each element of the pack that `pattern` expands over, with the pack
    // parameter bound to that element and the function body re-typed for it.
    // Runs body on an expression, or, for a pack expansion `args...` of exactly one element, on its
    // pattern with that element bound (a scalar member initialised with `forward<Args>(args)...`).
    template<typename F>
    void withSingleElement(Expr* e, Node* at, F&& body)
    {
      if (!isPackExpansion(e))
      {
        body(e);
        return;
      }
      Expr* pattern = static_cast<UnaryExpr*>(e)->operand;
      if (packElementCount(pattern, at) != 1)
        unsupported(at, "a pack of " + std::to_string(packElementCount(pattern, at)) + " elements where one value is expected");
      forEachPackElement(pattern, at, [&](size_t) { body(pattern); });
    }

    // How many values arguments stand for once their pack expansions are expanded.
    size_t expandedCount(Span<Expr*> args, Node* at);

    // Runs body on each value arguments stand for: an expression as it is, a pack expansion's
    // pattern once per element, the element bound.
    template<typename F>
    void forEachExpanded(Span<Expr*> args, Node* at, F&& body)
    {
      for (Expr* a : args)
        if (isPackExpansion(a))
        {
          Expr* pattern = static_cast<UnaryExpr*>(a)->operand;
          forEachPackElement(pattern, at, [&](size_t) { body(pattern); });
        }
        else
          body(a);
    }

    size_t packElementCount(Expr* pattern, Node* at);

    // What the instantiation binds a template parameter pack to (typePackIn found it bound).
    const Ty* boundPack(Decl* pack);

    template<typename F>
    void forEachPackElement(Expr* pattern, Node* at, F&& body)
    {
      Decl* param = packIn(pattern);
      if (!param && currentArgs)
        if (Decl* pack = typePackIn(pattern))
        {
          // `values[I]...` over a template parameter pack (`int... I` of the index-sequence idiom):
          // the pattern once per element, with I bound to that element.
          forEachTypePackElement(pack, body);
          return;
        }
      if (!param)
        unsupported(at, "a pack expansion that does not name a parameter pack");
      std::vector<Decl*> packs = templatePacksOf(param);
      if (packs.empty() || !currentArgs)
        unsupported(at, "a pack expansion outside an instantiation");
      std::vector<Local> elements = packLocals[param];
      boundPacks(packs, elements.size(), at); // (each as long as the parameter pack)
      TemplateArgs original = *currentArgs;
      for (size_t i = 0; i < elements.size(); ++i)
      {
        // The pattern's mentions of the packs name element i; the rest of the body keeps them whole.
        TemplateArgs elementArgs = original;
        elementArgs.elementPacks = packs;
        elementArgs.elementIndex = uint32_t(i);
        global.retypeFor(currentFunction, elementArgs, currentClassTy, true);
        locals[param] = elements[i];
        body(i);
      }
      locals.erase(param);
      global.retypeFor(currentFunction, original, currentClassTy);
    }

    // A template type pack that a pattern names in a type (`(func(Args()), ...)`,
    // `(... || isSame<T, Args>)`, `sizeof(Args)`) and the instantiation binds to a pack, or null.
    Decl* typePackIn(Node* n, int depth = 0);

    // Runs body once per element of the type pack `pack` (typePackIn), the function re-typed with
    // the pack bound to that element (a private copy: retypedPerElement), and the element's
    // arguments in effect.
    template<class F>
    void forEachTypePackElement(Decl* pack, F&& body)
    {
      TemplateArgs original = *currentArgs;
      const Ty* bound = nullptr;
      for (auto& [d, t] : original.pairs)
        if (d == pack)
          bound = t;
      for (size_t i = 0; bound && i < bound->args.size(); ++i)
      {
        // The pattern's mentions of the pack name element i; the parameters, the class instance
        // and its members keep the whole pack (`lists` an `Array<Entry, sizeof...(Ts)>&`).
        TemplateArgs elementArgs = original;
        elementArgs.elementPacks = {pack};
        elementArgs.elementIndex = uint32_t(i);
        global.retypeFor(currentFunction, elementArgs, currentClassTy, true);
        ArgsScope scope(*this, &elementArgs);
        body(i);
      }
      global.retypeFor(currentFunction, original, currentClassTy);
    }

    bool isPackExpansion(Expr* e);

