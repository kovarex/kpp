// struct Emitter's members: parameter packs: the declarations, part of the struct's body, included
// in it in Codegen/Emitter.hpp; the definitions in ParameterPacks.cpp (codegen.cpp split, stage 2,
// 2026-11-05, moved without a change).
    // --- parameter packs ---

    // The pack parameter an expression refers to (the first one found), or null.
    Decl* packIn(Node* n, int depth = 0);
    // Every pack parameter an expression refers to, in order, each once: `a * 10 + b` of `A... a`
    // and `B... b` expands both together, element i of each.
    void packsIn(Node* n, std::vector<Decl*>& out, int depth = 0);

    // The template parameter packs a pack parameter's declared type names, in order: `Args&&... args`
    // names Args; `auto PrototypeT::*... members` names PrototypeT and the pack invented for the
    // `auto`. An expansion binds each of them to its i-th element together.
    std::vector<Decl*> templatePacksOf(Decl* param);
    // The function parameter packs an expression names (an init-capture pack's pattern's).
    std::vector<Decl*> parameterPacksNamedIn(Node* n, int depth = 0);

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

    // Retypes the body being emitted for args (a pack bound to one element, or the instance's own
    // arguments again): the function's copy (currentFunction), or a generic lambda's private copy
    // (retypedLambda) when its call operator is emitted.
    void retypeForElement(const TemplateArgs& args, bool packElement = false)
    {
      if (retypedLambda)
        global.retypeLambdaFor(retypedLambdaTy, retypedLambda, args, packElement);
      else
        global.retypeFor(currentFunction, args, currentClassTy, packElement);
    }

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
          forEachTypePackElement(pack, body, pattern);
          return;
        }
      if (!param)
        unsupported(at, "a pack expansion that does not name a parameter pack");
      // Every parameter pack the pattern names expands with it, element i of each together
      // (`(a * 10 + b) + ...`): of one length, as C++ requires ([temp.variadic] 7).
      std::vector<Decl*> params;
      packsIn(pattern, params);
      std::vector<Decl*> packs;
      for (Decl* p : params)
        for (Decl* tp : templatePacksOf(p))
          if (std::find(packs.begin(), packs.end(), tp) == packs.end())
            packs.push_back(tp);
      if (packs.empty() || !currentArgs)
        unsupported(at, "a pack expansion outside an instantiation");
      size_t count = packLocals[param].size();
      for (Decl* p : params)
        if (packLocals[p].size() != count)
          unsupported(at, "a pack expansion over parameter packs of different lengths");
      boundPacks(packs, count, at); // (each as long as the parameter pack)
      // A template parameter pack the pattern names beside them (`args * Is`, the index-sequence idiom):
      // element i of it too, its arguments in effect for the pattern's value. Until 2026-11-09 "a template
      // parameter whose value is not known here".
      bool namedBeside = addNamedPacks(pattern, packs, count);
      TemplateArgs original = *currentArgs;
      // Inside another expansion's pattern (`sumAll(args, args...)...`): its element's bindings and
      // typing again when this one is done, for the rest of that pattern.
      const TemplateArgs* outer = elementInEffect;
      std::vector<std::pair<Decl*, Local>> outerLocals;
      for (Decl* p : params)
        if (auto l = locals.find(p); l != locals.end())
          outerLocals.push_back({p, l->second});
      for (size_t i = 0; i < count; ++i)
      {
        // The pattern's mentions of the packs name element i; the rest of the body keeps them whole,
        // and so does an expansion inside the pattern (elementPattern).
        TemplateArgs elementArgs = original;
        elementArgs.elementPacks = packs;
        elementArgs.elementIndex = uint32_t(i);
        elementArgs.elementPattern = pattern;
        retypeForElement(elementArgs, true);
        elementInEffect = &elementArgs;
        for (Decl* p : params)
          locals[p] = packLocals[p][i];
        if (namedBeside)
        {
          ArgsScope scope(*this, &elementArgs);
          body(i);
        }
        else
          body(i);
        elementInEffect = outer;
      }
      for (Decl* p : params)
        locals.erase(p);
      for (auto& [p, l] : outerLocals)
        locals[p] = l;
      if (outer)
        retypeForElement(*outer, true);
      else
        retypeForElement(original);
    }

    // Runs body with element k of the packs args name bound, as forEachPackElement's pass k: the
    // function retyped for that element, a parameter pack's mentions naming its element k. For a
    // pack expansion whose pattern is no expression: `Leaf<A>(a)...` initializing the base of
    // element k. Arguments that name no pack run as they are.
    template<typename F>
    void withPackElement(Span<Expr*> args, size_t k, Node* at, F&& body)
    {
      std::vector<Decl*> params;
      for (Expr* a : args)
        packsIn(a, params);
      Decl* typePack = nullptr;
      for (Expr* a : args)
        if (!typePack && params.empty() && currentArgs)
          typePack = typePackIn(a);
      if (params.empty() && !typePack)
      {
        body();
        return;
      }
      if (!currentArgs)
        unsupported(at, "a pack expansion outside an instantiation");
      TemplateArgs original = *currentArgs;
      TemplateArgs elementArgs = original;
      elementArgs.elementIndex = uint32_t(k);
      if (typePack)
      {
        // `Leaf<A>(A())...`: the type pack's element k, its arguments in effect (forEachTypePackElement).
        elementArgs.elementPacks = {typePack};
        retypeForElement(elementArgs, true);
        {
          ArgsScope scope(*this, &elementArgs);
          body();
        }
        retypeForElement(original);
        return;
      }
      std::vector<Decl*> packs;
      for (Decl* p : params)
        for (Decl* tp : templatePacksOf(p))
          if (std::find(packs.begin(), packs.end(), tp) == packs.end())
            packs.push_back(tp);
      if (packs.empty())
        unsupported(at, "a pack expansion outside an instantiation");
      for (Decl* p : params)
        if (k >= packLocals[p].size())
          unsupported(at, "a pack expansion over parameter packs of different lengths");
      boundPacks(packs, packLocals[params[0]].size(), at); // (each as long as the parameter pack)
      elementArgs.elementPacks = packs;
      retypeForElement(elementArgs, true);
      for (Decl* p : params)
        locals[p] = packLocals[p][k];
      body();
      for (Decl* p : params)
        locals.erase(p);
      retypeForElement(original);
    }

    // A template type pack that a pattern names in a type (`(func(Args()), ...)`,
    // `(... || isSame<T, Args>)`, `sizeof(Args)`) and the instantiation binds to a pack, or null.
    Decl* typePackIn(Node* n, int depth = 0);
    // Every such pack the pattern names, in order, each once; never one an expansion inside it expands.
    void typePacksIn(Node* n, std::vector<Decl*>& out, int depth = 0);
    // Adds to packs those the pattern names beside them that are bound to `count` elements (one of another
    // length is no part of this expansion: C++ requires one length, [temp.variadic] 7). Whether it added any.
    bool addNamedPacks(Node* pattern, std::vector<Decl*>& packs, size_t count);

    // Runs body once per element of the type pack `pack` (typePackIn), the function re-typed with
    // the pack bound to that element (a private copy: retypedPerElement), and the element's
    // arguments in effect; with a pattern, so are the other packs it names (`Is * Js`).
    template<class F>
    void forEachTypePackElement(Decl* pack, F&& body, Node* pattern = nullptr)
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
        if (pattern)
          addNamedPacks(pattern, elementArgs.elementPacks, bound->args.size());
        retypeForElement(elementArgs, true);
        ArgsScope scope(*this, &elementArgs);
        body(i);
      }
      retypeForElement(original);
    }

    bool isPackExpansion(Expr* e);

