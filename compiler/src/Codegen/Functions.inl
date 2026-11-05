// struct Emitter's members: functions' names and signatures. Part of codegen.cpp, included in the
// struct's body in Codegen/Emitter.hpp (a file of its own since 2026-11-05, moved without a
// change).
    // --- functions ---

    // IR name of a function (for an instantiation: with the class and template arguments),
    // computed once per function, class and arguments.
    struct NameKey
    {
      FunctionDecl* f;
      const Ty* classTy;
      std::vector<std::pair<Decl*, const Ty*>> args;
      bool operator==(const NameKey&) const = default;
    };
    struct NameKeyHash
    {
      size_t operator()(const NameKey& k) const
      {
        size_t h = std::hash<const void*>()(k.f) * 31 + std::hash<const void*>()(k.classTy);
        for (auto& [d, t] : k.args)
          h = (h * 1000003) ^ (std::hash<const void*>()(d) * 31 + std::hash<const void*>()(t));
        return h;
      }
    };
    std::unordered_map<NameKey, std::string, NameKeyHash> functionNames;

    const std::string& functionName(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy)
    {
      NameKey key{f, classTy, args.pairs};
      auto it = functionNames.find(key);
      if (it == functionNames.end())
        it = functionNames.emplace(std::move(key), computeFunctionName(f, args, classTy)).first;
      return it->second;
    }

    // q, the qualified name of a member d, with the arguments written for the full specialisation
    // owning d (`template<> struct Limits<int>`) before the member's own name, so that the members
    // of two specialisations of one template differ (`Flag<true>::value`, `Flag<false>::value`).
    // Unchanged for a member of any other class.
    std::string withSpecialisationArgs(std::string q, Decl* d)
    {
      Decl* owner = global.owningClass(d);
      if (!owner || owner->kind != NodeKind::ClassDecl || !(owner->flags & DF_Specialization))
        return q;
      const NamePart& last = static_cast<ClassDecl*>(owner)->name.last();
      std::string spelled;
      for (Node* a : last.templateArgs)
      {
        const Ty* t = nullptr;
        switch (a->kind)
        {
          case NodeKind::NamedType: case NodeKind::PointerType: case NodeKind::LRefType: case NodeKind::RRefType:
          case NodeKind::ConstType: case NodeKind::ArrayType: case NodeKind::FunctionType: case NodeKind::BuiltinType:
          case NodeKind::DecltypeType: case NodeKind::AutoType: case NodeKind::MemberPointerType:
            t = global.typeOfAst(static_cast<Type*>(a), owner);
            break;
          default: break;
        }
        std::string one;
        if (t && t->isKnown())
          one = irString(t);
        else if (owner->file && owner->file->tokens && a->token < owner->file->tokens->size())
        {
          const Token& tok = (*owner->file->tokens)[a->token];
          one = std::string(owner->file->text.substr(tok.offset, tok.length));
        }
        spelled += (spelled.empty() ? "" : ", ") + one;
      }
      size_t pos = q.rfind("::");
      if (pos != std::string::npos)
        q.insert(pos, "<" + spelled + ">");
      return q;
    }

    std::string computeFunctionName(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy)
    {
      if (global.isExternC(f))
        return "@" + sym(f->name.last().symbol);
      if (f->name.parts.size() == 1 && (sym(f->name.parts[0].symbol) == "main" || sym(f->name.parts[0].symbol) == "wmain") && !global.owningClass(f))
        return "@kpp." + sym(f->name.parts[0].symbol); // called by the generated @main (@wmain), which reports uncaught exceptions
      ArgsScope scope(*this, &args);
      std::string q;
      if (classTy && !classTy->args.empty())
      {
        // Class path with the instantiation's arguments, then the member's own name.
        std::string full = qualifiedName(f);
        std::string cls = qualifiedName(classTy->decl);
        q = irString(classTy);
        size_t pos = full.rfind("::");
        q += pos == std::string::npos ? "::" + full : full.substr(pos);
      }
      else
      {
        q = qualifiedName(f);
        if (f->name.parts.size() > 1 && !global.owningClass(f))
        {
          // `int M::twice(int) {}` outside its namespace: the name the declaration in M has.
          std::string prefix;
          for (size_t i = 0; i + 1 < f->name.parts.size(); ++i)
            if (f->name.parts[i].symbol != UINT32_MAX)
              prefix += sym(f->name.parts[i].symbol) + "::";
          size_t pos = q.rfind("::");
          std::string head = pos == std::string::npos ? "" : q.substr(0, pos + 2), tail = pos == std::string::npos ? q : q.substr(pos + 2);
          if (!prefix.empty() && head.find(prefix) == std::string::npos)
            q = head + prefix + tail;
        }
        q = withSpecialisationArgs(q, f);
        if (Decl* owner = global.owningClass(f); owner && owner->kind == NodeKind::ClassDecl && declaredInFunction(owner))
        {
          // A local class's member: the class as types print it (its function and place), with
          // the arguments of the function template instance it is in, then the member's own name;
          // two local classes named Visitor are two classes, and so is one in two instances.
          std::string instance;
          std::vector<std::string> printed;
          for (auto& [d, ty] : args.pairs)
            if (d && d->kind == NodeKind::TemplateParamDecl)
              printed.push_back(sym(static_cast<TemplateParamDecl*>(d)->name) + "=" + (ty ? irString(ty) : std::string("?")));
          std::sort(printed.begin(), printed.end());
          for (const std::string& p : printed)
            instance += (instance.empty() ? "<" : ", ") + p;
          if (!instance.empty())
            instance += ">";
          size_t pos = q.rfind("::");
          q = irString(classTy ? classTy : types.classType(owner, {})) + instance + (pos == std::string::npos ? "::" + q : q.substr(pos));
        }
      }
      if (!f->templateParams.empty())
      {
        q += "<";
        for (size_t i = 0; i < f->templateParams.size(); ++i)
        {
          const Ty* t = nullptr;
          for (auto& [d, ty] : args.pairs)
            if (d == f->templateParams[i])
              t = ty;
          q += (i ? ", " : "") + (t ? irString(t) : std::string("?"));
        }
        q += ">";
      }
      else if ((f->flags & DF_Specialization) && f->isTemplate)
      {
        // `template<> Vector<String> splitWords(StringView)`: the explicit arguments, or the return
        // type when the parameter appears only there, tell the specialisations of one template apart.
        std::string spelled;
        for (Node* a : f->name.last().templateArgs)
        {
          const Ty* t = uint8_t(a->kind) <= uint8_t(NodeKind::PackType) ? global.typeOfAst(static_cast<Type*>(a), f) : nullptr;
          spelled += (spelled.empty() ? "" : ", ") + (t && t->isKnown() ? irString(t) : std::string("?"));
        }
        if (spelled.empty())
          if (const Ty* ft = declType(f); ft && ft->kind == TyKind::Function && ft->inner)
            spelled = irString(ft->inner);
        q += "<" + spelled + ">";
      }
      const Ty* ft = declType(f);
      std::string params;
      for (size_t i = 0; ft && i < ft->args.size(); ++i)
        params += (i ? ", " : "") + irString(ft->args[i]);
      // A const method is a different function from its non-const overload.
      return "@\"" + q + "(" + params + ")" + ((f->flags & DF_Const) && !(f->flags & DF_Static) ? " const" : "") + "\"";
    }

    // The function as an error's `(in ...)` names it (currentContext): written as errors write types
    // (types.toString: `Item^`, `const Vector<int32>&`), its class with the instance's arguments, a
    // function template's arguments, its parameters and a const method's `const`, so that two
    // instances or overloads stay two texts (the checker's errors are a set). computeFunctionName's
    // structure, with the program's own spelling instead of the IR's.
    std::string displayFunctionName(FunctionDecl* f, const TemplateArgs& args, const Ty* classTy)
    {
      ArgsScope scope(*this, &args);
      auto spell = [&](const Ty* t) { return t ? types.toString(t, &symbols) : std::string("?"); };
      std::string q;
      Decl* owner = global.owningClass(f);
      if (global.isExternC(f))
        q = sym(f->name.last().symbol);
      else if (owner && owner->kind == NodeKind::ClassDecl)
      {
        // A member: its class as types print it (an instance's arguments, a local class's place),
        // then the member's own name.
        std::string full = qualifiedName(f);
        size_t pos = full.rfind("::");
        q = spell(classTy ? classTy : types.classType(owner, {})) + (pos == std::string::npos ? "::" + full : full.substr(pos));
      }
      else
      {
        q = qualifiedName(f);
        if (f->name.parts.size() > 1)
        {
          // `int M::twice(int) {}` outside its namespace: the name the declaration in M has.
          std::string prefix;
          for (size_t i = 0; i + 1 < f->name.parts.size(); ++i)
            if (f->name.parts[i].symbol != UINT32_MAX)
              prefix += sym(f->name.parts[i].symbol) + "::";
          size_t pos = q.rfind("::");
          std::string head = pos == std::string::npos ? "" : q.substr(0, pos + 2), tail = pos == std::string::npos ? q : q.substr(pos + 2);
          if (!prefix.empty() && head.find(prefix) == std::string::npos)
            q = head + prefix + tail;
        }
        q = withSpecialisationArgs(q, f);
      }
      if (!f->templateParams.empty())
      {
        q += "<";
        for (size_t i = 0; i < f->templateParams.size(); ++i)
        {
          const Ty* t = nullptr;
          for (auto& [d, ty] : args.pairs)
            if (d == f->templateParams[i])
              t = ty;
          q += (i ? ", " : "") + spell(t);
        }
        q += ">";
      }
      else if ((f->flags & DF_Specialization) && f->isTemplate)
      {
        // (an explicit specialisation's arguments, as computeFunctionName tells them apart)
        std::string spelled;
        for (Node* a : f->name.last().templateArgs)
        {
          const Ty* t = uint8_t(a->kind) <= uint8_t(NodeKind::PackType) ? global.typeOfAst(static_cast<Type*>(a), f) : nullptr;
          spelled += (spelled.empty() ? "" : ", ") + (t && t->isKnown() ? spell(t) : std::string("?"));
        }
        if (spelled.empty())
          if (const Ty* ft = declType(f); ft && ft->kind == TyKind::Function && ft->inner)
            spelled = spell(ft->inner);
        q += "<" + spelled + ">";
      }
      const Ty* ft = declType(f);
      std::string params;
      for (size_t i = 0; ft && i < ft->args.size(); ++i)
        params += (i ? ", " : "") + spell(ft->args[i]);
      return q + "(" + params + ")" + ((f->flags & DF_Const) && !(f->flags & DF_Static) ? " const" : "");
    }

    struct Signature
    {
      std::string returnType;
      std::vector<std::string> paramTypes;
      std::vector<const Ty*> paramTys; // null for this/sret
      bool hasThis = false, hasSret = false, variadic = false;
    };

    // A lambda's call operator's signature, as emitLambda writes its header and a caller declares it
    // (callClosure): the closure, the result's slot when it returns in memory, then the call type's
    // parameters; an unknown result is a pointer, as emitLambda emits its body.
    Signature lambdaSignatureOf(const Ty* callTy, Node* at)
    {
      Signature s;
      const Ty* ret = callTy->inner ? callTy->inner : types.voidType();
      if (!ret->isKnown())
        ret = types.pointer(types.voidType());
      s.hasThis = true;
      s.paramTypes.push_back("ptr");
      s.paramTys.push_back(nullptr);
      if (returnsInMemory(ret))
      {
        s.hasSret = true;
        s.paramTypes.push_back("ptr");
        s.paramTys.push_back(nullptr);
      }
      for (const Ty* pt : callTy->args)
      {
        s.paramTypes.push_back(isAggregate(pt) || pt->isReference() || !pt->isKnown() ? "ptr" : llType(pt, at));
        s.paramTys.push_back(pt);
      }
      s.returnType = s.hasSret ? "void" : ret->isReference() ? "ptr" : llType(ret, at);
      return s;
    }

    // `declare RET NAME(TYPES)`, as declareFromHeader reads it from the header of the definition.
    static std::string declarationText(const Signature& s, const std::string& name)
    {
      std::string text = "declare " + s.returnType + " " + name + "(";
      for (size_t i = 0; i < s.paramTypes.size(); ++i)
        text += (i ? ", " : "") + s.paramTypes[i];
      if (s.variadic)
        text += s.paramTypes.empty() ? "..." : ", ...";
      return text + ")";
    }

    // Whether the declarations of requests are recorded (Shared::requestDeclarations): a debug
    // build's modules, until level 0's barrier.
    bool recordingRequests() const { return !sh.barrierTaken && sh.moduleCount > 1 && sh.importableModules == 0; }

    void recordRequestDeclaration(const std::string& name, std::string declaration)
    {
      std::lock_guard lock(sh.definitionsMutex);
      sh.requestDeclarations.emplace(name, std::move(declaration));
    }

    // Runs f, a computation for a request's declaration, not part of what is being emitted: when it
    // fails (Unsupported), what fail() recorded is taken back (the error, the erroring mark of the
    // function being emitted, the last failure), and false is returned. The definition then fails
    // the same way where it is emitted, and reports it there.
    template<typename F>
    bool withoutFailing(F&& f)
    {
      size_t errorsBefore = errors.size(), erroringBefore = erroring.size();
      bool unattributedBefore = unattributedErrors, shownBefore = lastFailureShown;
      std::string failureBefore = lastFailure;
      try
      {
        f();
        return true;
      }
      catch (Unsupported&)
      {
        errors.resize(errorsBefore);
        if (erroring.size() != erroringBefore)
          erroring.erase(owner);
        unattributedErrors = unattributedBefore;
        lastFailure = std::move(failureBefore);
        lastFailureShown = shownBefore;
        return false;
      }
    }

