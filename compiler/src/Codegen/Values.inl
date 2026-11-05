// struct Emitter's members: values and conversions. Part of codegen.cpp, included in the struct's
// body in Codegen/Emitter.hpp (a file of its own since 2026-11-05, moved without a change).
    // --- values ---

    // The class's `operator T()` converting to `to` (exactly, or any scalar one when none is exact),
    // its own or inherited (`operator bool` of a BoolConstant base); explicit ones for bool, where
    // conditions allow them, and in an explicit cast when they yield the type itself
    // (`double(b)`, `static_cast<double>(b)` with `explicit operator const double&()`: direct
    // initialisation's rule). foundIn: the class (or base) declaring it.
    // opArgs: the arguments to call it with (the class's, and for an operator template the one its
    // result is deduced to: `template<class T> operator T()` converting to double has T = double).
    FunctionDecl* conversionOperator(const Ty* from, const Ty* to, Node* at, const Ty** foundIn = nullptr, bool explicitCast = false,
                                     TemplateArgs* opArgs = nullptr)
    {
      return findConversion(from, at, foundIn, [&](FunctionDecl* f, const TemplateArgs& args, const Ty* ret) -> int
      {
        bool exact = ret == types.unqualified(to) || (isClass(ret) && isClass(to) && classKey(ret) == classKey(to));
        // A class derived from `to` serves too: its `to` part (SignalIDConnector's `operator
        // SignalID()` where an IDWithQuality, SignalID's base, is wanted), after the conversion to
        // `to` itself if there is one (C++ compares the derived-to-base conversion after it: a
        // RailBuilder made a RailSignalBase& takes `operator RailSignalBase&()`, not `operator
        // RailChainSignal&()`).
        std::vector<std::pair<std::string, uint32_t>> path;
        bool derived = !exact && isClass(ret) && isClass(to) && !types.isDependent(ret) && basePath(ret, to, path, at);
        if (global.isExplicit(f, args) && to->kind != TyKind::Bool && (!explicitCast || !(exact || derived)))
          return 0;
        // (A scalar is no way to a class: from it a converting constructor would be a second
        // user-defined conversion. Direction's `operator Enum()` is not how a Direction becomes a
        // RealOrientation; RealOrientation's constructor from a Direction is.)
        return exact ? 2 : derived ? 1 : isClass(ret) || isClass(to) ? 0 : 1;
      }, to, opArgs);
    }

    // A class value through its (non-explicit) conversion operator to a pointer, own or inherited,
    // or nothing when it has none: what `*` and `delete` of such an object work on.
    std::optional<Value> toPointer(const Value& object, Node* at)
    {
      const Ty* owner = nullptr;
      TemplateArgs opArgs;
      FunctionDecl* op = findConversion(object.ty, at, &owner, [&](FunctionDecl* f, const TemplateArgs& args, const Ty* ret) -> int
      {
        return !global.isExplicit(f, args) && ret->kind == TyKind::Pointer ? 2 : 0;
      }, nullptr, &opArgs);
      if (!op)
        return std::nullopt;
      return callConversion(op, object.ty, owner, object.text, at, &opArgs);
    }

    // A class's (non-explicit) conversion operator to an enum or an integer, own or inherited, or
    // null: what a switch on the object switches on.
    FunctionDecl* integralConversionOperator(const Ty* from, Node* at, const Ty** foundIn = nullptr)
    {
      return findConversion(from, at, foundIn, [&](FunctionDecl* f, const TemplateArgs& args, const Ty* ret) -> int
      {
        if (global.isExplicit(f, args))
          return 0;
        return ret->kind == TyKind::Enum || ret->kind == TyKind::Int || ret->kind == TyKind::Char || ret->kind == TyKind::Bool ? 2 : 0;
      });
    }

    // The conversion operator of `from` or its bases (the class first, then each base level) that
    // score rates best: 2 at once, else the first rated 1; 0 is not a candidate.
    // to: the type converted to, when known, which a conversion operator template deduces its
    // result from; opArgs: the arguments the one found is called with.
    template<typename Score>
    FunctionDecl* findConversion(const Ty* from, Node* at, const Ty** foundIn, Score&& score, const Ty* to = nullptr, TemplateArgs* opArgs = nullptr)
    {
      FunctionDecl* best = nullptr;
      const Ty* bestIn = nullptr;
      TemplateArgs bestArgs;
      int bestScore = 0;
      std::vector<const Ty*> classes{canonicalClass(from, at)};
      for (size_t i = 0; i < classes.size() && i < 64 && bestScore < 2; ++i)
      {
        const Ty* cls = classes[i];
        ClassDecl* c = classDef(cls->decl, at);
        TemplateArgs args = global.classArgsOf(cls);
        ArgsScope scope(*this, &args);
        for (Decl* m : c->members)
        {
          if (m->kind != NodeKind::FunctionDecl || !(m->flags & DF_Conversion))
            continue;
          auto* f = static_cast<FunctionDecl*>(m);
          const Ty* ft = declType(m);
          if (!ft || ft->kind != TyKind::Function || !ft->inner || !ft->inner->isKnown())
            continue;
          TemplateArgs fargs = args;
          const Ty* ret = types.unqualified(types.stripRef(ft->inner));
          if (!f->templateParams.empty())
          {
            // `template<class T> operator T()` (or `operator const T&()`): T is the type converted
            // to, and the operator a candidate only when its constraints hold for it.
            if (!to || ret->kind != TyKind::Param || std::find(f->templateParams.begin(), f->templateParams.end(), ret->decl) == f->templateParams.end())
              continue;
            Decl* param = ret->decl;
            ret = types.unqualified(types.stripRef(to));
            fargs.pairs.push_back({param, ret});
            if (!global.constraintsHold(f, fargs))
              continue;
          }
          int s = score(f, fargs, ret);
          if (s > bestScore)
          {
            best = f;
            bestIn = cls;
            bestArgs = fargs;
            bestScore = s;
            if (s >= 2)
              break;
          }
        }
        for (const ClassDecl::Base& b : c->bases)
          if (const Ty* bt = global.typeOfAstIn(b.type, c, args); bt && isClass(bt) && !types.isDependent(bt))
            classes.push_back(canonicalClass(bt, at));
      }
      if (foundIn)
        *foundIn = bestIn;
      if (opArgs)
        *opArgs = bestArgs;
      return best;
    }

    // Calls a conversion operator found in `owner` (the object's class or one of its bases) on
    // the object at addr.
    Value callConversion(FunctionDecl* op, const Ty* objectTy, const Ty* owner, const std::string& addr, Node* at, const TemplateArgs* opArgs = nullptr)
    {
      std::string self = addr;
      if (owner != canonicalClass(objectTy, at) && !baseAddress(objectTy, owner, addr, self, at))
        unsupported(at, "a conversion operator of the base " + types.toString(owner, &symbols) + ", which the layout of " +
                            types.toString(objectTy, &symbols) + " does not have");
      // A virtual one is the object's override (ScriptRenderBase's pure `virtual explicit operator
      // bool() const`, which was called directly: a link error, or the base's body for an impure one).
      std::string indirect;
      if (op->templateParams.empty() && isClass(owner) && structFor(owner, at).polymorphic)
        if (int slot = slotOf(owner, op, at); slot >= 0)
          indirect = loadSlot(self, slot);
      return callRvalue(callFunction(op, opArgs && !op->templateParams.empty() ? *opArgs : global.classArgsOf(owner), owner, self, "", {}, at, indirect), at);
    }

    // explicitCast: the conversion an explicit cast asks for, which may use explicit conversion
    // operators.
    // --zero-null-warnings: a literal 0 of the program's own sources made a null pointer or member
    // pointer here without a cast (design.md 4: kpp is to require nullptr; C's fix-it rewrites the
    // token each warning names, so the place is the literal's own, its column included under
    // --diagnostic-columns). Imported C keeps C's rule. Each token once, however many instances
    // convert it.
    void noteZeroNull(Node* at)
    {
      if (!zeroNullWarningsOn() || !currentFile || currentFile->isImported)
        return;
      Node* e = at;
      while (e && e->kind == NodeKind::ParenExpr)
        e = static_cast<ParenExpr*>(e)->inner;
      if (!e || e->kind != NodeKind::LiteralExpr || static_cast<LiteralExpr*>(e)->literalKind != TokenKind::Number)
        return; // (a constant that is 0, not the literal: the fix-it has no token to rewrite; the error will name it)
      ParsedFile* file = static_cast<LiteralExpr*>(e)->file ? static_cast<LiteralExpr*>(e)->file : currentFile;
      if (!file || file->isImported)
        return;
      noteZeroNullWarning(locationOf(e, file, true) + ": warning: the literal 0 is no pointer: write nullptr (design.md 4)");
    }

    Value convert(Value v, const Ty* to, Node* at, bool explicitCast = false)
    {
      const Ty* from = v.ty;
      to = types.stripRef(to);
      if (!to || !to->isKnown())
        unsupported(at, "a conversion to an unknown type");
      if (from == to || types.unqualified(from) == types.unqualified(to))
        return {v.text, to, v.isTemporary};
      // A value whose type the checker did not find is no pointer to compare with null or pass on.
      if (!from->isKnown())
        unsupported(at, "a conversion from a value of unknown type to " + types.toString(to, &symbols));
      auto kindOf = [&](const Ty* t) { return t->kind == TyKind::Enum ? TyKind::Int : t->kind; };
      TyKind fk = kindOf(from), tk = kindOf(to);
      auto bitsOf = [&](const Ty* t) -> uint8_t { return t->kind == TyKind::Enum ? uint8_t(enumUnderlying(t->decl)->bits) : t->bits; };
      auto unsignedOf = [&](const Ty* t) { return t->kind == TyKind::Enum ? enumUnderlying(t->decl)->isUnsigned : t->isUnsigned; };
      if (isLambda(from) && tk == TyKind::Pointer && to->inner && to->inner->kind == TyKind::Function)
        return {lambdaThunk(from, to->inner, at), to}; // a lambda without captures as a pointer to function
      if (fk == TyKind::Class || tk == TyKind::Class)
      {
        if (fk == TyKind::Class && tk == TyKind::Class)
        {
          std::string addr;
          if (baseAddress(from, to, v.text, addr, at))
            return {addr, to, v.isTemporary && addr == v.text};
        }
        if (fk == TyKind::Class)
        {
          const Ty* owner = nullptr;
          TemplateArgs opArgs;
          if (FunctionDecl* op = conversionOperator(from, to, at, &owner, explicitCast, &opArgs))
            return convert(callConversion(op, from, owner, v.text, at, &opArgs), to, at);
        }
        // (A class without a conversion to bool is no condition: never its address compared with null.)
        unsupported(at, "a conversion from " + types.toString(from, &symbols) + " to " + types.toString(to, &symbols));
      }
      // The result's name, taken when an instruction defines it: a conversion that emits nothing (the
      // same bits, a pointer to a pointer) takes no number. Which of two equivalent types a value
      // carries can differ from run to run (`const Chunk*` through forward<const Chunk*&> or through
      // ChunkSet's own argument), and a number taken for nothing renumbered the rest of the function
      // (Vector<ChunkSet<...>>::emplace_back, one run in four at -j 32).
      std::string r;
      auto result = [&]() -> const std::string& { if (r.empty()) r = newTemp(); return r; };
      // A pointer to a member function is the function (null when none), one to a data member the
      // member's offset, -1 when null (offset 0 is the first member's), as MSVC has them.
      // (The literal 0 is a null pointer constant too: `f(0)` for an `int Base::*` parameter.)
      if (tk == TyKind::MemberPointer && (fk == TyKind::Nullptr || (fk == TyKind::Int && v.text == "0")))
      {
        if (fk == TyKind::Int && !explicitCast)
          noteZeroNull(at);
        return {to->inner && to->inner->kind == TyKind::Function ? "null" : "-1", to};
      }
      if (fk == TyKind::MemberPointer && tk == TyKind::Bool)
      {
        bool function = from->inner && from->inner->kind == TyKind::Function;
        fn += "  " + result() + " = icmp ne " + (function ? "ptr " + v.text + ", null" : "i64 " + v.text + ", -1") + "\n";
        return {r, to};
      }
      // `int (Both::*)() const = &Named::getId`: a pointer to a member of a base, as one of a class
      // derived from it. A data member's offset grows by the base's place; a member function gets a
      // thunk moving `this` to the base, for a function known here (a pointer read at run time
      // carries nothing to move by).
      if (fk == TyKind::MemberPointer && tk == TyKind::MemberPointer && !from->args.empty() && !to->args.empty() && isClass(from->args[0]) &&
          isClass(to->args[0]) && classKey(from->args[0]) != classKey(to->args[0]))
      {
        const Ty* base = withCurrentArgs(from->args[0]);
        const Ty* derived = withCurrentArgs(to->args[0]);
        std::vector<std::pair<std::string, uint32_t>> path;
        if (basePath(derived, base, path, at))
        {
          bool moved = false;
          for (auto& [structName, index] : path)
            moved = moved || index != 0;
          if (!moved)
            return {v.text, to};
          if (from->inner && from->inner->kind == TyKind::Function)
          {
            if (v.text.empty() || v.text[0] != '@')
              unsupported(at, "a pointer to a member function of " + types.toString(base, &symbols) + " read at run time, as one of " +
                                  types.toString(derived, &symbols) + ", which has that base at an offset");
            std::string function = v.text[1] == '"' ? v.text.substr(2, v.text.size() - 3) : v.text.substr(1);
            return {sharedThunk("@\"pmf." + irString(derived) + "." + function + "\"", methodSignature(from->inner, at), pathAtNull(path), v.text, -1), to};
          }
          std::string offset = newTemp(), sum = newTemp(), isNull = newTemp();
          fn += "  " + offset + " = ptrtoint ptr " + pathAtNull(path) + " to i64\n";
          fn += "  " + sum + " = add i64 " + v.text + ", " + offset + "\n";
          fn += "  " + isNull + " = icmp eq i64 " + v.text + ", -1\n";
          fn += "  " + result() + " = select i1 " + isNull + ", i64 -1, i64 " + sum + "\n";
          return {r, to};
        }
      }
      std::string fromT = llType(from, at), toT = llType(to, at);
      if (fromT == toT && !(fk == TyKind::Pointer && tk == TyKind::Pointer))
        return {v.text, to, v.isTemporary}; // the same bits: bool to `enum class E : bool`, uint8 to an int8 enum
      // (two class pointers are both `ptr`, yet Derived* -> Base* may move to the base subobject)
      if ((fk == TyKind::Int || fk == TyKind::Char) && (tk == TyKind::Int || tk == TyKind::Char))
      {
        uint8_t fb = bitsOf(from), tb = bitsOf(to);
        if (fb == tb)
          return {v.text, to};
        fn += "  " + result() + " = " + (fb > tb ? "trunc" : unsignedOf(from) ? "zext" : "sext") + " " + fromT + " " + v.text + " to " + toT + "\n";
        return {r, to};
      }
      if (fk == TyKind::Bool && (tk == TyKind::Int || tk == TyKind::Char))
      {
        fn += "  " + result() + " = zext i1 " + v.text + " to " + toT + "\n";
        return {r, to};
      }
      if ((fk == TyKind::Int || fk == TyKind::Char) && tk == TyKind::Bool)
      {
        fn += "  " + result() + " = icmp ne " + fromT + " " + v.text + ", 0\n";
        return {r, to};
      }
      if ((fk == TyKind::Pointer || fk == TyKind::Nullptr) && tk == TyKind::Bool)
      {
        if (fk == TyKind::Nullptr && !explicitCast) // ([conv.bool]: only `bool(nullptr)`, a direct initialisation)
          fail(at, "nullptr does not convert to bool without a cast");
        fn += "  " + result() + " = icmp ne ptr " + v.text + ", null\n";
        return {r, to};
      }
      if (fk == TyKind::Float && tk == TyKind::Bool)
      {
        fn += "  " + result() + " = fcmp une " + fromT + " " + v.text + ", 0.0\n";
        return {r, to};
      }
      if (fk == TyKind::Float && (tk == TyKind::Int || tk == TyKind::Char))
      {
        fn += "  " + result() + " = " + (unsignedOf(to) ? "fptoui" : "fptosi") + " " + fromT + " " + v.text + " to " + toT + "\n";
        return {r, to};
      }
      if ((fk == TyKind::Int || fk == TyKind::Char || fk == TyKind::Bool) && tk == TyKind::Float)
      {
        bool u = fk == TyKind::Bool || unsignedOf(from);
        fn += "  " + result() + " = " + (u ? "uitofp" : "sitofp") + " " + fromT + " " + v.text + " to " + toT + "\n";
        return {r, to};
      }
      if (fk == TyKind::Float && tk == TyKind::Float)
      {
        fn += "  " + result() + " = " + (from->bits > to->bits ? "fptrunc" : "fpext") + " " + fromT + " " + v.text + " to " + toT + "\n";
        return {r, to};
      }
      if (tk == TyKind::Pointer && (fk == TyKind::Pointer || fk == TyKind::Nullptr || fk == TyKind::Function || fk == TyKind::Array))
      {
        if (fk == TyKind::Pointer && isClass(from->inner) && isClass(to->inner) && classKey(from->inner) != classKey(to->inner))
        {
          std::string addr;
          if (baseAddress(from->inner, to->inner, v.text, addr, at))
            return {keepNull(v.text, addr), to};
        }
        if (!explicitCast && !impliedPointerConversion(from, to, at))
          fail(at, "a pointer (" + types.toString(from, &symbols) + ") does not convert to " + types.toString(to, &symbols) + " without a cast");
        return {fk == TyKind::Nullptr ? "null" : v.text, to};
      }
      if (tk == TyKind::Pointer && fk == TyKind::Int)
      {
        // Without a cast only the literal 0 is a pointer, the null one (C++ [conv.ptr]/1); a constant
        // 0 makes the same null pointer. `int* p = n;` is an error, as `int y = &x;` is below.
        // Imported C converts any integer, as its compilers accept with a warning.
        if (!explicitCast && v.text != "0" && !(currentFile && currentFile->isImported))
          fail(at, "an integer (" + types.toString(from, &symbols) + ") does not convert to " + types.toString(to, &symbols) + " without a cast");
        if (!explicitCast && v.text == "0")
          noteZeroNull(at);
        // A signed integer narrower than a pointer is widened as a signed value first, as through
        // intptr_t: `(HANDLE)-1` is all ones (INVALID_HANDLE_VALUE); inttoptr alone zero-extends.
        std::string value = v.text, valueT = fromT;
        if (!unsignedOf(from) && bitsOf(from) < 64)
        {
          value = newTemp();
          fn += "  " + value + " = sext " + fromT + " " + v.text + " to i64\n";
          valueT = "i64";
        }
        fn += "  " + result() + " = inttoptr " + valueT + " " + value + " to ptr\n";
        return {r, to};
      }
      if ((fk == TyKind::Pointer || fk == TyKind::Nullptr) && tk == TyKind::Int)
      {
        // Only a cast turns a pointer into an integer (`(uintptr_t)p`, reinterpret_cast); `int y =
        // &x;` is an error in C++, and was a silently truncated address here. nullptr converts as
        // (void*)0 does ([expr.reinterpret.cast]/4): stb_image's `(size_t)(cond ? NULL : NULL)`.
        if (!explicitCast)
          fail(at, "a pointer (" + types.toString(from, &symbols) + ") does not convert to " + types.toString(to, &symbols) + " without a cast");
        fn += "  " + result() + " = ptrtoint ptr " + (fk == TyKind::Nullptr ? std::string("null") : v.text) + " to " + toT + "\n";
        return {r, to};
      }
      // SSE vector to scalar: extract lane 0.
      // __m128 -> float, __m128d -> double, __m128i -> int32/int64.
      if (fk == TyKind::Vector && (tk == TyKind::Float || tk == TyKind::Int || tk == TyKind::Char))
      {
        // If element types differ, bitcast the vector first.
        const Ty* elemTy = from->inner;
        if (elemTy && elemTy->kind != tk)
        {
          // e.g. <4 x i32> -> double: bitcast <4 x i32> to <2 x double> then extract.
          // or <4 x float> -> int32: bitcast <4 x float> to <4 x i32> then extract.
          uint32_t totalBits = from->count * (elemTy->bits ? elemTy->bits : 32);
          uint32_t newCount = totalBits / to->bits;
          const Ty* newVec = types.vectorType(to, newCount);
          std::string t1 = newTemp();
          fn += "  " + t1 + " = bitcast " + fromT + " " + v.text + " to " + llType(newVec, at) + "\n";
          fn += "  " + result() + " = extractelement " + llType(newVec, at) + " " + t1 + ", i32 0\n";
        }
        else
        {
          fn += "  " + result() + " = extractelement " + fromT + " " + v.text + ", i32 0\n";
        }
        return {r, to};
      }
      // Scalar to SSE vector: splat (broadcast to all lanes).
      if (tk == TyKind::Vector && (fk == TyKind::Float || fk == TyKind::Int || fk == TyKind::Char || fk == TyKind::Bool))
      {
        const Ty* elemTy = to->inner;
        // Convert the scalar to the element type first if needed.
        Value scalar = (from != elemTy && types.unqualified(from) != types.unqualified(elemTy))
                        ? convert(v, elemTy, at) : v;
        std::string t1 = newTemp(), t2 = newTemp();
        fn += "  " + t1 + " = insertelement " + toT + " undef, " + llType(elemTy, at) + " " + scalar.text + ", i32 0\n";
        // Shuffle to splat lane 0 to all lanes.
        std::string mask = "<";
        for (uint32_t i = 0; i < to->count; ++i)
          mask += (i ? ", " : "") + std::string("i32 0");
        mask += ">";
        fn += "  " + t2 + " = shufflevector " + toT + " " + t1 + ", " + toT + " undef, <" + std::to_string(to->count) + " x i32> " + mask + "\n";
        return {t2, to};
      }
      // Vector to vector bitcast (same total size, different element types).
      if (fk == TyKind::Vector && tk == TyKind::Vector)
      {
        fn += "  " + result() + " = bitcast " + fromT + " " + v.text + " to " + toT + "\n";
        return {r, to};
      }
      unsupported(at, "a conversion from " + types.toString(from, &symbols) + " to " + types.toString(to, &symbols));
    }

    // Address of the `base` subobject inside the `derived` object at addr (false if not a base).
    // An explicit cast: what convert does, plus the downcast to a derived class (pointer or
    // reference) that an implicit conversion never makes.
    Value explicitConvert(Value v, const Ty* to, Node* at)
    {
      const Ty* target = types.stripRef(to);
      const Ty* from = v.ty;
      bool pointers = from && target && from->kind == TyKind::Pointer && target->kind == TyKind::Pointer && isClass(from->inner) && isClass(target->inner);
      bool objects = isClass(from) && isClass(target);
      if (pointers || objects)
      {
        const Ty* fromCls = pointers ? from->inner : from;
        const Ty* toCls = pointers ? target->inner : target;
        std::vector<std::pair<std::string, uint32_t>> path;
        if (classKey(fromCls) != classKey(toCls) && basePath(toCls, fromCls, path, at))
        {
          std::string addr = derivedAddress(path, v.text);
          return {pointers ? keepNull(v.text, addr) : addr, pointers ? target : types.unqualified(toCls), false}; // `static_cast<VerticalFlow&>(layout)`
        }
      }
      return convert(v, to, at, true);
    }

    // A pointer moved to a base or derived subobject: null stays null (C++'s rule for pointer conversions).
    std::string keepNull(const std::string& original, const std::string& adjusted)
    {
      if (adjusted == original)
        return adjusted;
      std::string isNull = newTemp(), r = newTemp();
      fn += "  " + isNull + " = icmp eq ptr " + original + ", null\n";
      fn += "  " + r + " = select i1 " + isNull + ", ptr null, ptr " + adjusted + "\n";
      return r;
    }

    // Whether C++ converts from (a pointer, array, function or nullptr) to the pointer `to` without a
    // cast ([conv.ptr], [conv.qual], [conv.array], [conv.func]): to the same type with added
    // qualifiers, to `void*` from an object pointer, from a derived class to its base (the caller
    // moves the address); a function or an array to a pointer to it, or to its element. Imported C
    // converts any object pointer to and from `void*` (C11 6.3.2.3).
    bool impliedPointerConversion(const Ty* from, const Ty* to, Node* at)
    {
      if (from->kind == TyKind::Nullptr)
        return true;
      const Ty* target = to->inner;
      const Ty* source = from->kind == TyKind::Function ? from : from->inner;
      if (!target || !source || types.isDependent(target) || types.isDependent(source) || !target->isKnown() || !source->isKnown())
        return true; // (nothing known to judge by)
      if (from->kind == TyKind::Function)
        return types.unqualified(target) == source;
      bool keepsQualifiers = (target->isConst || !source->isConst) && (target->isVolatile || !source->isVolatile);
      const Ty* t = types.unqualified(target);
      const Ty* f = types.unqualified(source);
      if (t == f)
        return keepsQualifiers;
      if (from->kind == TyKind::Pointer && qualificationConverts(from, to))
        return true; // `DisplayPanel* const*` as a `const DisplayPanel* const*`
      bool imported = currentFile && currentFile->isImported;
      if (t->kind == TyKind::Void && f->kind != TyKind::Function)
        return keepsQualifiers || imported;
      if (imported && f->kind == TyKind::Void)
        return true;
      std::vector<std::pair<std::string, uint32_t>> path;
      return isClass(t) && isClass(f) && keepsQualifiers && basePath(f, t, path, at);
    }

    // C++'s qualification conversion of a pointer to pointers ([conv.qual]): every level of `to` keeps
    // the qualifiers of `from`'s, and a level that adds one has const at every level above it but
    // the outermost (`T**` is no `const T**`, `T* const*` is a `const T* const*`).
    bool qualificationConverts(const Ty* from, const Ty* to)
    {
      bool constAbove = true;
      for (const Ty *a = from->inner, *b = to->inner; a && b;)
      {
        if ((a->isConst && !b->isConst) || (a->isVolatile && !b->isVolatile))
          return false;
        if ((a->isConst != b->isConst || a->isVolatile != b->isVolatile) && !constAbove)
          return false;
        const Ty* ua = types.unqualified(a);
        const Ty* ub = types.unqualified(b);
        if (ua == ub)
          return true;
        if (ua->kind != TyKind::Pointer || ub->kind != TyKind::Pointer)
          return false;
        constAbove = constAbove && b->isConst;
        a = ua->inner;
        b = ub->inner;
      }
      return false;
    }

    // The chain of base-class fields from derived down to base: (struct name, field index) pairs.
    // A base reached along two paths (`struct Both : Left, Right`, both of them Bases) is two
    // subobjects, and the conversion to it is ambiguous: an error, as in C++ ([conv.ptr]/3).
    bool basePath(const Ty* derived, const Ty* base, std::vector<std::pair<std::string, uint32_t>>& path, Node* at)
    {
      // A class only declared (no definition in the program) has no bases and is no class's base
      // (a base must be defined): no path, and no layout asked for (a cast between pointers to it
      // reinterprets the pointer, as a C-style cast does).
      if (declaredOnly(derived) || declaredOnly(base))
        return classKey(derived) == classKey(base);
      if (canonicalClass(derived, at) == canonicalClass(base, at))
        return true;
      StructInfo& info = structFor(derived, at);
      std::string name = info.name;
      std::vector<std::pair<Decl*, uint32_t>> bases = info.bases;
      std::vector<const Ty*> fieldTypes = info.fieldTypes;
      std::vector<std::pair<std::string, uint32_t>> found;
      bool any = false;
      for (auto& [baseDef, index] : bases)
      {
        std::vector<std::pair<std::string, uint32_t>> sub{{name, index}};
        if (!basePath(fieldTypes[index], base, sub, at))
          continue;
        if (any)
          fail(at, "the conversion of " + types.toString(derived, &symbols) + " to its base " + types.toString(base, &symbols) +
                       " is ambiguous: it has that base more than once");
        any = true;
        found = std::move(sub);
      }
      path.insert(path.end(), found.begin(), found.end());
      return any;
    }

    // The derived object's address from its base subobject's, back along the path of base fields.
    std::string derivedAddress(const std::vector<std::pair<std::string, uint32_t>>& path, const std::string& baseAddr)
    {
      if (path.empty())
        return baseAddr;
      std::string offset = "ptr null";
      for (auto& [name, index] : path)
        offset = "ptr getelementptr (" + name + ", " + offset + ", i32 0, i32 " + std::to_string(index) + ")";
      std::string off = newTemp(), neg = newTemp(), r = newTemp();
      fn += "  " + off + " = ptrtoint " + offset + " to i64\n";
      fn += "  " + neg + " = sub i64 0, " + off + "\n";
      fn += "  " + r + " = getelementptr i8, ptr " + baseAddr + ", i64 " + neg + "\n";
      return r;
    }

    // The type of the object a conditional's operand x names when the conditional, of type t, can
    // be that object (C++'s lvalue conditional): x an lvalue or a reference of type t or, for a
    // class, of a class derived from it (whose t part it is). Null when x converts to t, so that
    // the conditional is a new value (`p ? p->name : Mod::CORE_MOD`, a String and a `const char*`).
    const Ty* conditionalObject(Expr* x, const Ty* t, Node* at)
    {
      if (!x || !x->ty || !(isLvalueExpression(x) || x->ty->isReference()))
        return nullptr;
      const Ty* xt = withCurrentArgs(types.unqualified(types.stripRef(x->ty)));
      const Ty* want = withCurrentArgs(types.unqualified(t));
      if (!xt || !want || types.isDependent(xt))
        return nullptr;
      std::vector<std::pair<std::string, uint32_t>> path;
      if (isClass(xt) && isClass(want))
        return basePath(xt, want, path, at) ? xt : nullptr;
      return xt == want ? xt : nullptr;
    }

    // The address of the object the operand x names (of type xt, conditionalObject's), as an
    // object of t: its t part for a class derived from t.
    std::string conditionalAddress(Expr* x, const Ty* xt, const Ty* t, Node* at)
    {
      std::string addr = lvalue(x);
      std::string part = addr;
      if (isClass(xt) && canonicalClass(xt, at) != canonicalClass(t, at) && !baseAddress(xt, t, addr, part, at))
        unsupported(at, "a conditional operand of a class derived from the result's, without the base");
      return part;
    }

    bool baseAddress(const Ty* derived, const Ty* base, const std::string& addr, std::string& out, Node* at)
    {
      std::vector<std::pair<std::string, uint32_t>> path;
      if (!basePath(derived, base, path, at))
        return false;
      out = addr;
      for (auto& [name, index] : path)
        out = gep(name, out, index);
      return true;
    }

    // An arithmetic operand's type after the integral promotions.
    const Ty* promoted(const Ty* t)
    {
      t = types.unqualified(types.stripRef(t));
      if (t->kind == TyKind::Enum)
        t = enumUnderlying(t->decl); // (then promoted as it: an `enum : uint8_t` is an int, `U200 << 1` 400)
      if (t->kind == TyKind::Bool || t->kind == TyKind::Char || (t->kind == TyKind::Int && t->bits < 32))
        return types.intType(32);
      return t;
    }

    const Ty* commonArithmetic(const Ty* a, const Ty* b)
    {
      return types.usualArithmetic(promoted(a), promoted(b));
    }

    Value load(const std::string& addr, const Ty* ty, Node* at)
    {
      ty = types.stripRef(ty);
      if (ty->kind == TyKind::Array)
      {
        std::string r = newTemp();
        fn += "  " + r + " = getelementptr inbounds " + llType(ty, at) + ", ptr " + addr + ", i64 0, i64 0\n";
        return {r, types.pointer(ty->inner)};
      }
      if (ty->kind == TyKind::Function)
        return {addr, types.pointer(ty)}; // a function (through a reference, `F&& f`): its address, nothing to load
      if (isAggregate(ty))
        return {addr, types.unqualified(ty)};
      std::string r = newTemp();
      fn += "  " + r + " = load " + llType(ty, at) + ", ptr " + addr + "\n";
      return {r, types.unqualified(ty)};
    }

    void store(Value v, const std::string& addr, const Ty* ty, Node* at)
    {
      ty = types.stripRef(ty);
      if (isClass(ty))
        unsupported(at, "storing a class value directly");
      if (ty->kind == TyKind::Array)
      {
        // A scalar stored into an array: zero-init the array, then store the scalar
        // converted to the element type in element 0 (C's `T a[N] = {value}` semantics).
        storeZero(ty, addr, at);
        auto isNumeric = [](TyKind k) { return k == TyKind::Int || k == TyKind::Bool || k == TyKind::Char || k == TyKind::Float; };
        if (v.ty && ty->inner && isNumeric(v.ty->kind) && isNumeric(ty->inner->kind))
        {
          Value elem = convert(v, ty->inner, at);
          std::string elemAddr = newTemp();
          fn += "  " + elemAddr + " = getelementptr inbounds " + llType(ty, at) + ", ptr " + addr + ", i64 0, i64 0\n";
          fn += "  store " + llType(ty->inner, at) + " " + elem.text + ", ptr " + elemAddr + "\n";
        }
        return;
      }
      Value c = convert(v, ty, at);
      fn += "  store " + llType(ty, at) + " " + c.text + ", ptr " + addr + "\n";
    }

    std::string thisPointer(Node* at)
    {
      auto it = locals.find(nullptr);
      if (it == locals.end())
        unsupported(at, "a member used outside a method");
      std::string r = newTemp();
      fn += "  " + r + " = load ptr, ptr " + it->second.addr + "\n";
      return r;
    }

    std::string operatorName(P op)
    {
      return op == P::LBracket ? "operator[]" : op == P::LParen ? "operator()" : "operator" + std::string(punctuatorSpelling(op));
    }

    // The address of the object a call's result names: a returned reference's referent, a class
    // result's object (its slot), and a scalar returned by value (a prvalue) in a temporary of its
    // own. Never the scalar itself: Targeter's `T* operator*() const` bound to Map's
    // `operator[](const K&)` passed the rail pointer as the key's address.
    std::string resultAddress(const Value& v, Node* at)
    {
      if (isClass(v.ty) || isLambda(v.ty) || (v.ty && v.ty->isReference()))
        return v.text;
      std::string slot = newObjectSlot(v.ty, at);
      fn += "  store " + llType(v.ty, at) + " " + v.text + ", ptr " + slot + "\n";
      return slot;
    }

    // A variable template's object for the arguments of its use n (`&hasBegin<Vector<int>>`, a
    // reference bound to it): a constant global per argument list, holding the value the use folds
    // to (its rvalue). Never one global named by the template alone: that would be every
    // instance's, its initializer evaluated without the arguments (`hasBegin<Vector<int>>` read
    // false). Only a const one: a mutable instance would need its own storage, built at startup.
    bool isConstValue(VariableDecl* v)
    {
      if (v->flags & DF_Constexpr)
        return true; // (a concept too: a constexpr bool without a written type)
      const Ty* t = declType(v);
      return t && !t->isReference() && t->isConst;
    }
    std::string variableTemplateObject(NameExpr* n, VariableDecl* v, Node* at)
    {
      if (!isConstValue(v))
        unsupported(at, "a variable template that is not a const value (" + qualifiedName(v) + ")");
      std::string spelled;
      for (const auto& pair : variableTemplateArgs(n, v, at).pairs)
        spelled += (spelled.empty() ? "" : ", ") + irString(pair.second);
      std::string name = "@\"" + qualifiedName(v) + "<" + spelled + ">\"";
      Value value = rvalue(n);
      if (declaredNames.claim(name))
      {
        decls += "$" + name.substr(1) + " = comdat any\n";
        decls += name + " = linkonce_odr constant " + llType(value.ty, at) + " " + value.text + ", comdat\n";
      }
      return name;
    }

    std::string lvalue(Expr* e)
    {
      if (++recursionDepth > 128)
      {
        recursionDepth = 0;
        fail(e, "expression nested too deeply");
      }
      struct DepthGuard { int& d; ~DepthGuard() { --d; } } dg{recursionDepth};
      switch (e->kind)
      {
        case NodeKind::NameExpr:
        {
          auto* n = static_cast<NameExpr*>(e);
          Decl* d = n->resolved;
          if (!d && n->name.parts.size() == 1 && n->name.parts[0].hasTemplateArgs && e->ty && e->ty->kind == TyKind::Bool)
            break; // a builtin trait (`isSame<int, int>`, left unresolved, typed bool) bound to a reference: its value goes to a temporary (below)
          if (!d)
            unsupported(e, "an unresolved name (" + sym(n->name.last().symbol) + ")", !(n->name.parts.size() == 1 && global.undeclaredReported(sym(n->name.last().symbol))));
          if (d->kind == NodeKind::VariableDecl)
            if (std::string bound; bindingAddress(static_cast<VariableDecl*>(d), e, bound))
              return bound;
          auto it = locals.find(d);
          if (it != locals.end())
          {
            if (it->second.isRef)
            {
              std::string r = newTemp();
              fn += "  " + r + " = load ptr, ptr " + it->second.addr + "\n";
              return r;
            }
            return it->second.addr;
          }
          if (d->kind == NodeKind::VariableDecl)
          {
            if (Decl* cls = global.owningClass(d); cls && !(d->flags & DF_Static))
            {
              if (!currentClassTy)
                unsupported(e, "a member used outside a method");
              return memberObject(currentClassTy, d, thisPointer(e), e); // `r = v` with r a reference member
            }
            if (!d->templateParams.empty())
              return variableTemplateObject(n, static_cast<VariableDecl*>(d), e);
            return globalAddress(static_cast<VariableDecl*>(d), e, qualifierClass(e, e));
          }
          if (d->kind == NodeKind::EnumeratorDecl)
            break; // an enumerator bound to a reference: its value goes to a temporary (below)
          if (d->kind == NodeKind::TemplateParamDecl && !static_cast<TemplateParamDecl*>(d)->isType)
            break; // a constant template parameter bound to a reference (`take(K)`): the same
          if (d->kind == NodeKind::FunctionDecl && !(global.owningClass(d) && !(d->flags & DF_Static)))
          {
            // A function bound to a reference (`call(add, 2, 3)` with `F&& f`): the function itself,
            // whose address the reference holds, as `&add` gives it.
            auto* f = static_cast<FunctionDecl*>(d);
            // A function template, or a static member of a class template (`sortWith(all,
            // depthOrder)` in Tech<AmmoTag>'s member): the instance, as its address names it.
            if (!f->templateParams.empty() || global.isTemplateContext(f))
              return rvalue(e).text;
            return useFunction(f, {}, nullptr);
          }
          unsupported(e, "taking the address of " + sym(n->name.last().symbol));
        }
        case NodeKind::MemberExpr:
        {
          auto* m = static_cast<MemberExpr*>(e);
          Decl* d = m->resolved;
          std::string object;
          const Ty* baseTy;
          if (m->op == P::Arrow)
          {
            Value p = arrowOperand(rvalue(m->base), e);
            if (p.ty->kind != TyKind::Pointer)
              unsupported(e, "-> on something that is not a pointer");
            object = p.text;
            baseTy = p.ty->inner;
          }
          else
          {
            Value o = rvalue(m->base);
            if (!isClass(o.ty))
              unsupported(e, "a member access on a non-class object");
            object = o.text;
            baseTy = o.ty;
          }
          baseTy = types.stripRef(baseTy);
          if (!baseTy || !isClass(baseTy))
            unsupported(e, "a member access on a non-class object");
          if ((!d || d->kind != NodeKind::VariableDecl) && !m->member.empty())
            d = findFieldByName(baseTy, m->member.last().symbol, e);
          if (!d || d->kind != NodeKind::VariableDecl)
            unsupported(e, "a member access that did not resolve to a field", !followsUndeclared(baseTy));
          if (d->flags & DF_Static)
            return globalAddress(static_cast<VariableDecl*>(d), e, baseTy);
          if (const Ty* qualifier = memberQualifier(m, e))
          {
            // `this->Hook<Robot, TagB>::id`: the field of the instance of its class that the
            // qualifier is or derives from (the first base holding a field of that declaration was
            // another instance of Hook). The field's own class, not the qualifier: `b.Left::id`
            // with id in a virtual base is the one shared subobject's.
            // The qualifier's subobject first: `b.Right::id` with Base under both Left and Right is
            // the id of Right's Base, not of the first Base among the bases.
            Decl* owner = global.owningClass(d);
            const Ty* instance = owner ? instantiationIn(qualifier, owner, e) : nullptr;
            if (instance && qualifier != canonicalClass(baseTy, e) && qualifier != instance)
            {
              std::string addr;
              if (!baseAddress(baseTy, qualifier, object, addr, e))
                unsupported(e, "a member of " + types.toString(qualifier, &symbols) + ", which is no base of " + types.toString(baseTy, &symbols));
              object = addr;
              baseTy = qualifier;
            }
            if (instance && instance != canonicalClass(baseTy, e))
            {
              std::string addr;
              if (!baseAddress(baseTy, instance, object, addr, e))
                unsupported(e, "a member of " + types.toString(instance, &symbols) + ", which is no base of " + types.toString(baseTy, &symbols));
              object = addr;
              baseTy = instance;
            }
          }
          return memberObject(baseTy, d, object, e);
        }
        case NodeKind::UnaryExpr:
        {
          auto* u = static_cast<UnaryExpr*>(e);
          if (!u->isKeyword && P(u->op) == P::Star)
          {
            Value p = rvalue(u->operand);
            if (isClass(p.ty))
            {
              // No operator* of its own: the class's conversion to a pointer (`&*context.undoFiller`).
              const Ty* foundIn = nullptr;
              if (!findMemberFunction(p.ty, operatorName(P::Star), {}, p.ty, e, &foundIn, true))
                if (std::optional<Value> pointer = toPointer(p, e))
                  return pointer->text;
              return resultAddress(operatorCall(p, operatorName(P::Star), {}, e, true), e);
            }
            if (p.ty->kind != TyKind::Pointer)
              unsupported(e, "* on something that is not a pointer");
            return p.text;
          }
          if (!u->isKeyword && !u->postfix && (P(u->op) == P::PlusPlus || P(u->op) == P::MinusMinus))
          {
            rvalue(e);
            return lvalue(u->operand);
          }
          break;
        }
        case NodeKind::IndexExpr:
        {
          auto* i = static_cast<IndexExpr*>(e);
          if (i->args.size() != 1)
            unsupported(e, "multi-argument indexing");
          Value base = withCheckedConst(rvalue(i->base), i->base);
          if (ClassDecl* c = typedUnionOf(base.ty, e))
            return alternativeLvalue(c, base, i, e, true); // p[Integer]: the alternative, if held
          if (isClass(base.ty))
            return resultAddress(operatorCall(base, operatorName(P::LBracket), i->args, e, true), e);
          Value index = convert(rvalue(i->args[0]), types.intType(64), e);
          if (base.ty->kind != TyKind::Pointer)
            unsupported(e, "indexing a non-pointer");
          std::string r = newTemp();
          fn += "  " + r + " = getelementptr inbounds " + llType(base.ty->inner, e) + ", ptr " + base.text + ", i64 " + index.text + "\n";
          return r;
        }
        case NodeKind::ParenExpr: return lvalue(static_cast<ParenExpr*>(e)->inner);
        case NodeKind::CastExpr:
        {
          // `(T&&)value` in forward, `(Base&)derived`: a cast to a reference names the operand
          // itself; a copy in a temporary would be a dangling reference once returned.
          auto* c = static_cast<CastExpr*>(e);
          if (e->ty && e->ty->isReference())
            return lvalue(c->operand);
          break;
        }
        case NodeKind::NamedCastExpr:
        {
          auto* c = static_cast<NamedCastExpr*>(e);
          // A reference to the operand's own type, or a reinterpret_cast or const_cast: the operand's
          // address. Another class (a base or a derived one, or one the operand converts to with
          // `operator TrainStop&()`) is the cast's value, as explicitConvert makes it.
          const Ty* from = c->operand && c->operand->ty ? types.unqualified(types.stripRef(c->operand->ty)) : nullptr;
          const Ty* to = e->ty ? types.unqualified(types.stripRef(e->ty)) : nullptr;
          if (e->ty && e->ty->isReference() && c->castKind != Kw::dynamic_cast_ &&
              (c->castKind == Kw::reinterpret_cast_ || c->castKind == Kw::const_cast_ || !from || !to || !isClass(from) || !isClass(to) ||
               classKey(from) == classKey(to)))
            return lvalue(c->operand);
          break;
        }
        case NodeKind::BinaryExpr:
        {
          auto* b = static_cast<BinaryExpr*>(e);
          if (!b->isKeyword && (P(b->op) == P::DotStar || P(b->op) == P::ArrowStar))
            return memberPointerAddress(b);
          if (!b->isKeyword && isAssignment(P(b->op)))
          {
            Value v = rvalue(e);
            if (isClass(v.ty))
              return v.text;
            if (assignedAt == b)
              return assignedPlace; // (its left operand evaluated once)
            if (fixedPlace(b->lhs))
              return lvalue(b->lhs); // (evaluating it again changes nothing)
            unsupported(e, "a scalar assignment used as an lvalue whose place was not recorded");
          }
          if (!b->isKeyword && P(b->op) == P::Comma)
          {
            rvalue(b->lhs);
            return lvalue(b->rhs);
          }
          break;
        }
        case NodeKind::CallExpr: return resultAddress(callValue(static_cast<CallExpr*>(e), ""), e);
        case NodeKind::ConditionalExpr:
        {
          auto* c = static_cast<ConditionalExpr*>(e);
          const Ty* t = e->ty ? types.stripRef(e->ty) : nullptr;
          const Ty* thenObject = c->then && t ? conditionalObject(c->then, t, e) : nullptr;
          const Ty* elseObject = thenObject ? conditionalObject(c->else_, t, e) : nullptr;
          if (thenObject && elseObject)
          {
            // `first ? a : b` with two lvalues is an lvalue: the address of the chosen one.
            Value cond = convert(rvalue(c->cond), types.boolType(), e);
            std::string tL = newLabel("cond.then"), eL = newLabel("cond.else"), jL = newLabel("cond.end");
            fn += "  br i1 " + cond.text + ", label %" + tL + ", label %" + eL + "\n";
            size_t temps = statementTemps.size();
            startBlock(tL);
            std::string a = conditionalAddress(c->then, thenObject, t, e);
            destroyTempsFrom(temps);
            std::string aEnd = currentBlock;
            emitBranch(jL);
            startBlock(eL);
            std::string b = conditionalAddress(c->else_, elseObject, t, e);
            destroyTempsFrom(temps);
            std::string bEnd = currentBlock;
            emitBranch(jL);
            startBlock(jL);
            std::string r = newTemp();
            fn += "  " + r + " = phi ptr [ " + a + ", %" + aEnd + " ], [ " + b + ", %" + bEnd + " ]\n";
            return r;
          }
          break;
        }
        default:
          break;
      }
      Value v = rvalue(e);
      if (isClass(v.ty) || isLambda(v.ty))
        return v.text; // an object in memory already (a closure too: `auto&& add = [&](int x) {...};`)
      std::string slot = newObjectSlot(v.ty, e);
      fn += "  store " + llType(v.ty, e) + " " + v.text + ", ptr " + slot + "\n";
      return slot;
    }

    // A global built at startup (a class with a constructor, or an initializer that is not a
    // constant): a function constructing it, run from llvm.global_ctors in definition order (see
    // ctorsText); destructors at exit do not run yet.
    // init: the global's initializer (for a static member of a class template, its instantiation's
    // copy), or null.
    // perThread: a thread_local global, built by each thread at its first use, not at startup: the
    // function checks a thread_local guard, and globalFor calls it before every use (C++'s
    // thread_local wrapper). Destroyed at no thread's exit yet.
    void emitGlobalConstructor(VariableDecl* v, const std::string& name, const Ty* t, Expr* init, Node* at, bool perThread = false,
                               VariableDecl* definition = nullptr)
    {
      FunctionState saved = saveState();
      std::string savedOverride = funcletOverride;
      FunctionDecl* savedFunction = currentFunction;
      ParsedFile* savedFile = currentFile;
      bool savedShared = emittingShared;
      funcletOverride.clear();
      resetFunctionState();
      currentFunction = nullptr;
      VariableDecl* def = definition ? definition : variableDefinition(v);
      currentFile = def->file;
      // Names in the initializer are typed from the declaration it belongs to (a member alias of
      // the class: `Limits<IDType::IndexType>::max()`), not from the root scope.
      struct InitializerRestore { Decl*& d; Decl* saved; ~InitializerRestore() { d = saved; } } initializerRestore{initializerOf, initializerOf};
      initializerOf = def;
      emittingShared = false;
      std::string fnName = std::string(perThread ? "@\"kpp.tlsinit." : "@\"kpp.init.") + name.substr(2, name.size() - 3) + "\""; // per instantiation for class template members
      std::string guard = name.substr(0, name.size() - 1) + ".tlsguard\"";
      {
        std::lock_guard lock(sh.definitionsMutex);
        if (sh.globalInits.emplace(fnName, Shared::GlobalInit{v, name, t, init, at, perThread, definition, startupAt}).second)
        {
          sh.globalInitsOf[v].push_back(fnName);
          if (def != v)
            sh.globalInitsOf[def].push_back(fnName);
        }
      }
      // In the module its name hashes to, whoever uses the global first (the same in every run),
      // and its errors are its own, not of the function that happened to use it first.
      struct ModuleRestore { unsigned& m; unsigned saved; ~ModuleRestore() { m = saved; } } moduleRestore{currentModule, currentModule};
      currentModule = moduleCount <= 1 ? 0 : sh.emissionModule(std::hash<std::string>()(fnName));
      // What it adds (its static InitializerList arrays) belongs to it, not to its first user.
      struct OwnerRestore { std::string& owner; std::string saved; ~OwnerRestore() { owner = saved; } } ownerRestore{owner, owner};
      owner = fnName;
      // Nor is what it inlines its first user's (a global's initializer that reads an edited name
      // is the server's full codegen).
      struct InlinedRestore
      {
        std::vector<const FunctionDecl*>& now;
        std::vector<const FunctionDecl*> saved;
        ~InlinedRestore() { now = std::move(saved); }
      } inlinedRestore{inlinedNow, std::move(inlinedNow)};
      inlinedNow.clear();
      struct NameRestore
      {
        std::string& name;
        std::string& requestedBy;
        std::string savedName, savedRequestedBy;
        ~NameRestore() { name = savedName, requestedBy = savedRequestedBy; }
      } nameRestore{currentFunctionName, currentRequestedBy, currentFunctionName, currentRequestedBy};
      currentFunctionName = fnName;
      currentRequestedBy.clear();
      // (as an error names it: the global, not its initializer's LLVM name)
      struct ContextNameRestore { std::string& name; std::string saved; ~ContextNameRestore() { name = std::move(saved); } }
        contextRestore{currentContext, currentContext}, requesterContextRestore{currentRequestedByContext, currentRequestedByContext};
      currentContext = "the initializer of " + (name.size() > 3 && name.compare(0, 2, "@\"") == 0 ? name.substr(2, name.size() - 3) : name);
      currentRequestedByContext.clear();
      try
      {
        if (perThread)
        {
          {
            auto lock = sharedLock();
            if (declaredNames.claim(guard))
              decls += "$" + guard.substr(1) + " = comdat any\n" + guard + " = linkonce_odr thread_local global i1 false, comdat\n";
          }
          fn += "  %tls.built = load i1, ptr " + guard + "\n  br i1 %tls.built, label %tls.done, label %tls.build\ntls.build:\n";
          fn += "  store i1 true, ptr " + guard + "\n";
        }
        pushScope();
        if (t->isReference() && init)
        {
          // The address of what the reference is bound to.
          std::string target = isClass(types.stripRef(init->ty)) ? rvalue(init).text : lvalue(init);
          fn += "  store ptr " + target + ", ptr " + name + "\n";
        }
        else if (init)
          initializeFrom(name, t, init, at);
        else if (isClass(t))
          defaultConstruct(name, t, at);
        else if (t->kind == TyKind::Array)
          constructArray(name, t, at); // `static Style styles[3];`
        destroyTemps();
        popScope();
        if (perThread)
          fn += "  br label %tls.done\ntls.done:\n";
        fn += "  ret void\n";
        addDefinition(fnName, functionText("define void " + fnName + "() {\nentry:\n"), true); // the global's, not its first user's
        auto lock = sharedLock();
        if (perThread)
          threadLocalInits[name] = fnName;
        else if (sh.startupFunctions.insert(fnName).second) // (once: a server's build emits it again)
          startups.push_back(startupAt ? Shared::Startup{startupAt->file, startupAt->token, def->token, fnName}
                                       : Shared::Startup{def->file ? def->file->index : 0, def->token, 0, fnName});
      }
      catch (Unsupported&)
      {
        restoreState(saved);
        funcletOverride = savedOverride;
        currentFunction = savedFunction;
        currentFile = savedFile;
        emittingShared = savedShared;
        throw;
      }
      restoreState(saved);
      funcletOverride = savedOverride;
      currentFunction = savedFunction;
      currentFile = savedFile;
      emittingShared = savedShared;
    }

    // The llvm.global_ctors array, all of it in module 0: the globals built at startup in
    // definition order within a file (design.md section 2: a global's initializer may read the ones
    // defined before it in its file), the files in the driver's order, the library first. Not by
    // module: which module a startup function lands in says nothing about the order.
    std::string ctorsText(unsigned k)
    {
      if (k != 0 || startups.empty())
        return "";
      std::vector<Shared::Startup> order = startups;
      std::sort(order.begin(), order.end(), [](const Shared::Startup& a, const Shared::Startup& b) {
        return std::tie(a.file, a.token, a.member, a.function) < std::tie(b.file, b.token, b.member, b.function);
      });
      std::string s = "@llvm.global_ctors = appending global [" + std::to_string(order.size()) + " x { i32, ptr, ptr }] [";
      for (size_t i = 0; i < order.size(); ++i)
        s += std::string(i ? ", " : "") + "{ i32, ptr, ptr } { i32 65535, ptr " + order[i].function + ", ptr null }";
      return s + "]\n";
    }

    // The declaration carrying a global's initializer: the out-of-line definition of a static data
    // member (`double Gui::scale = 1.0;`), or the declaration itself.
    VariableDecl* variableDefinition(VariableDecl* v)
    {
      Decl* d = global.definitionOf(v);
      return d && d->kind == NodeKind::VariableDecl ? static_cast<VariableDecl*>(d) : v;
    }

    // The definition of static member v in the class instance classTy: the explicit specialisation
    // written for that instance (`template<> const uint32_t ElectricNetworkStatistics::smoothingTicks
    // = 0;`), or the template's definition; v's own definition without an instance.
    VariableDecl* variableDefinition(VariableDecl* v, const Ty* classTy)
    {
      if (!classTy)
        return variableDefinition(v);
      Decl* d = global.definitionOf(v, global.classArgsOf(classTy));
      return d && d->kind == NodeKind::VariableDecl ? static_cast<VariableDecl*>(d) : v;
    }

    // The type the qualifier of a qualified name names, under the current arguments: `Counted<int>`
    // of `Counted<int>::instances`, the class `decltype(check<T>(nullptr))` names of
    // `decltype(check<T>(nullptr))::value`; null for an unqualified name.
    const Ty* qualifierOf(NameExpr* n)
    {
      TemplateArgs none;
      if (n->name.parts.size() == 1 && n->name.typeQualifier)
        return global.typeOfAstIn(n->name.typeQualifier, typingContext(), currentArgs ? *currentArgs : none);
      if (n->name.parts.size() <= 1)
        return nullptr;
      NamedType tmp{};
      tmp.kind = NodeKind::NamedType;
      tmp.token = n->token;
      tmp.name = n->name;
      tmp.name.parts.count -= 1;
      const Ty* q = global.typeOfAstIn(&tmp, typingContext(), currentArgs ? *currentArgs : none);
      if (!q || !q->isKnown() || types.isDependent(q))
        if (const Ty* bound = boundParameter(tmp.name))
          return bound;
      return q;
    }

    // A template parameter written as a qualifier (`T::loadExplicit(input)` in a generic lambda
    // `[&]<class T>(T) { ... }`): its argument in the instance being emitted, by name. The lambda's
    // own parameters are in no scope that a re-typing from the enclosing function looks in.
    const Ty* boundParameter(const QualifiedName& q)
    {
      if (!currentArgs || q.parts.size() != 1 || q.parts[0].hasTemplateArgs || q.parts[0].symbol == UINT32_MAX)
        return nullptr;
      for (const auto& [param, type] : currentArgs->pairs)
        if (param && param->kind == NodeKind::TemplateParamDecl && static_cast<TemplateParamDecl*>(param)->name == q.parts[0].symbol &&
            type && type->isKnown() && !types.isDependent(type))
          return type;
      return nullptr;
    }

    // The class a member's name is qualified with (`Hook<Robot, TagB>` of `r.Hook<Robot, TagB>::getId()`),
    // canonical; null for an unqualified member.
    // The implicit assignment of class target (objectTy itself or a base of it), member by member,
    // into the object of class objectTy at addr from arg (`this->Base::operator=(other)` and
    // `super::operator=(other)` of a class declaring none); the target object as an lvalue.
    Value implicitAssignment(const std::string& addr, const Ty* objectTy, const Ty* target, Expr* arg, Node* at)
    {
      std::string dst = addr;
      if (canonicalClass(target, at) != canonicalClass(objectTy, at) && !baseAddress(objectTy, target, addr, dst, at))
        unsupported(at, "operator= of " + types.toString(target, &symbols) + ", which is no base of " + types.toString(objectTy, &symbols));
      Value src = rvalue(arg);
      const Ty* srcTy = types.unqualified(types.stripRef(src.ty));
      std::string from = src.text;
      if (!isClass(srcTy))
        unsupported(at, "assigning a value that is no object to " + types.toString(target, &symbols));
      if (canonicalClass(srcTy, at) != canonicalClass(target, at) && !baseAddress(srcTy, target, src.text, from, at))
        unsupported(at, "assigning " + types.toString(srcTy, &symbols) + " to " + types.toString(target, &symbols));
      assignObject(dst, from, target, at, movable(arg));
      return {dst, types.lref(target)};
    }

    const Ty* memberQualifier(MemberExpr* m, Node* at)
    {
      if (m->member.parts.size() < 2)
        return nullptr;
      NamedType written{};
      written.kind = NodeKind::NamedType;
      written.token = m->token;
      written.name = m->member;
      written.name.parts.count -= 1;
      TemplateArgs none;
      const Ty* q = global.typeOfAstIn(&written, typingContext(), currentArgs ? *currentArgs : none);
      return q && isClass(types.stripRef(q)) ? canonicalClass(types.unqualified(types.stripRef(q)), at) : nullptr;
    }

    // `Counted<int>::instances`: the instantiation a qualified static member belongs to (null for
    // an unqualified name or a class that is not a template).
    const Ty* qualifierClass(Node* e, Node* at)
    {
      if (!e || e->kind != NodeKind::NameExpr)
        return nullptr;
      const Ty* q = qualifierOf(static_cast<NameExpr*>(e));
      q = q ? types.unqualified(types.stripRef(q)) : nullptr;
      return q && isClass(q) && !q->args.empty() ? canonicalClass(q, at) : nullptr;
    }

    // The instantiation a static member of a class template is used with: the qualifier written,
    // or the class of the method being emitted, or the base of either that is an instantiation of
    // the member's class (`MyLimits<int>::digits` with digits declared in the base
    // IntegerLimits<int, true, 32>).
    const Ty* staticMemberClass(VariableDecl* v, Node* e, Node* at, const Ty* classTy)
    {
      Decl* owner = global.owningClass(v);
      if (!owner || owner->templateParams.empty())
        return nullptr;
      if (classTy)
      {
        // `IsPointerLike<int*>::value` with value declared in IsPointerLike<T*>'s base Yes
        // (Constant<bool, true>): the base's instantiation.
        const Ty* c = canonicalClass(classTy, at);
        if (const Ty* found = instantiationIn(c, owner, at))
          return found;
        return c;
      }
      if (const Ty* q = qualifierType(e, at))
        if (const Ty* found = instantiationIn(q, owner, at))
          return found;
      if (currentClassTy)
        if (const Ty* found = instantiationIn(currentClassTy, owner, at))
          return found;
      if (const Ty* q = qualifierClass(e, at))
        return q;
      return nullptr; // no instantiation in sight (an alias used as the qualifier): one shared object, as for a plain class
    }

    // The class a qualified name's qualifier names (`MyLimits<int>` of `MyLimits<int>::digits`),
    // any class; null for an unqualified name.
    const Ty* qualifierType(Node* e, Node* at)
    {
      if (!e || e->kind != NodeKind::NameExpr)
        return nullptr;
      const Ty* q = qualifierOf(static_cast<NameExpr*>(e));
      q = q ? types.unqualified(types.stripRef(q)) : nullptr;
      return q && isClass(q) && !types.isDependent(q) ? canonicalClass(q, at) : nullptr;
    }

    // The instantiation of the class template owner that cls is or derives from; null if none.
    const Ty* instantiationIn(const Ty* cls, Decl* owner, Node* at, int depth = 0)
    {
      if (!cls || !isClass(cls) || depth > 32)
        return nullptr;
      const Ty* c = canonicalClass(cls, at);
      ClassDecl* def = classDef(c->decl, at);
      if (def == classDef(owner, at))
        return c;
      TemplateArgs args = global.classArgsOf(c);
      for (const ClassDecl::Base& b : def->bases)
        if (const Ty* bt = global.typeOfAstIn(b.type, def, args); bt && isClass(bt) && !types.isDependent(bt))
          if (const Ty* found = instantiationIn(bt, owner, at, depth + 1))
            return found;
      return nullptr;
    }

    // `using EmptyWidget::EmptyWidget;` in a class without constructors of its own: the base whose
    // constructors are inherited (its name, as a constructor initializer would spell it); null if none.
    std::optional<QualifiedName> inheritedConstructorBase(ClassDecl* c)
    {
      for (Decl* m : c->members)
      {
        if (m->kind != NodeKind::UsingNameDecl)
          continue;
        const QualifiedName& q = static_cast<UsingNameDecl*>(m)->name;
        if (q.parts.size() < 2 || q.parts[q.parts.size() - 1].symbol != q.parts[q.parts.size() - 2].symbol)
          continue;
        QualifiedName base = q;
        base.parts.count -= 1;
        return base;
      }
      return std::nullopt;
    }

    // The address of the object a global names: the global itself, or, for a reference global,
    // what it was bound to. classTy: the instantiation, for a static member of a class template.
    std::string globalAddress(VariableDecl* v, Node* at, const Ty* classTy = nullptr)
    {
      std::string g;
      if (isStaticLocal(v))
      {
        g = staticLocalElsewhere(v, at); // (its function's emission has it among its locals)
        classTy = nullptr;
      }
      else
      {
        classTy = staticMemberClass(v, at, at, classTy);
        g = globalFor(v, at, classTy);
      }
      const Ty* t = classTy ? global.typeOfDeclIn(v, global.classArgsOf(classTy)) : global.typeOfDecl(v);
      if (!t || !t->isReference())
        return g;
      std::string p = newTemp();
      fn += "  " + p + " = load ptr, ptr " + g + "\n";
      return p;
    }

