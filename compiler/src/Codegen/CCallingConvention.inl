// struct Emitter's members: C calling conventions. Part of codegen.cpp, included in the struct's
// body in Codegen/Emitter.hpp (a file of its own since 2026-11-05, moved without a change).
    // --- C calling conventions (docs/design.md, "Targets and hosts") ---
    // A kpp function passes a class as a pointer to a copy and returns one through a result slot,
    // on every target. A C function (extern "C", or an imported header's) passes a struct by value
    // as the target's C does: MS x64 a struct of 1, 2, 4 or 8 bytes as one integer and any other
    // as a pointer to a copy, a result of 1, 2, 4 or 8 bytes in RAX and any other through the slot;
    // SysV x86-64 (Linux, FreeBSD) one of up to 16 bytes in one or two registers by its eightbytes'
    // classes (INTEGER where an integer or a pointer lies in it, SSE where only floats do), when
    // enough of those registers are left, and any other on the stack (`byval`) or through the slot.
    static bool isCFunction(const FunctionDecl* f) { return (f->flags & DF_ExternC) || (f->file && f->file->isImported); }

    struct CPassing
    {
      bool inMemory = true;
      std::vector<std::string> parts; // in registers: each register's LLVM type, in order
      std::vector<uint64_t> offsets;  // and where it starts in the struct
      unsigned ints = 0, sses = 0;    // SysV: the registers it takes
    };

    // The scalars a C struct holds, with their offsets: the members of its members, the elements of
    // its arrays, the storage of its bit fields, every member of a union at its start.
    void cScalars(const Ty* t, uint64_t base, std::vector<std::pair<uint64_t, const Ty*>>& out, Node* at)
    {
      t = types.unqualified(t);
      if (t->kind == TyKind::Enum)
        return cScalars(enumUnderlying(t->decl), base, out, at);
      if (t->kind == TyKind::Array)
      {
        uint64_t size = sizeOf(t->inner, at);
        for (uint64_t i = 0; i < t->count; ++i)
          cScalars(t->inner, base + i * size, out, at);
        return;
      }
      if (!isClass(t))
      {
        out.push_back({base, t});
        return;
      }
      classSizeAndAlign(t, at); // laid out, the fields' sizes known
      std::vector<const Ty*> fields;
      std::vector<bool> zeroSize;
      bool packed, isUnion;
      {
        auto lock = sharedLock();
        StructInfo& info = structFor(t, at);
        if (info.polymorphic)
          unsupported(at, "a class with virtual functions (" + irString(t) + ") passed by value to or from a C function");
        fields = info.fieldTypes, zeroSize = info.zeroSize, packed = info.packed, isUnion = info.isUnion;
      }
      uint64_t offset = 0;
      for (size_t i = 0; i < fields.size(); ++i)
      {
        if (i < zeroSize.size() && zeroSize[i])
          continue; // an empty base: no storage
        uint64_t a = packed ? 1 : alignOf(fields[i], at);
        offset = isUnion ? 0 : (offset + a - 1) / a * a;
        cScalars(fields[i], base + offset, out, at);
        offset += sizeOf(fields[i], at);
      }
    }

    // How the target's C passes (or returns) a struct of type t by value.
    CPassing cPassing(const Ty* t, Node* at)
    {
      CPassing p;
      uint64_t size = sizeOf(t, at);
      if (sh.target.windows())
      {
        if (size == 1 || size == 2 || size == 4 || size == 8)
          p.inMemory = false, p.parts = {"i" + std::to_string(size * 8)}, p.offsets = {0};
        return p;
      }
      if (size == 0 || size > 16)
        return p;
      std::vector<std::pair<uint64_t, const Ty*>> scalars;
      cScalars(t, 0, scalars, at);
      enum { None, Integer, Sse };
      int classes[2] = {None, None};
      bool upperHalf[2] = {false, false}; // an SSE eightbyte with a float in its upper four bytes
      for (auto& [offset, s] : scalars)
      {
        uint64_t ssize = sizeOf(s, at);
        if (s->kind == TyKind::Vector || offset % std::max<uint64_t>(1, alignOf(s, at)) != 0 || (offset % 8) + ssize > 8)
          return p; // unaligned (packed), or a vector: in memory
        bool sse = s->kind == TyKind::Float;
        if (sse && ssize > 8)
          return p; // (x87's long double: in memory)
        int& c = classes[offset / 8];
        c = c == Integer || !sse ? Integer : Sse;
        if (sse && offset % 8 >= 4)
          upperHalf[offset / 8] = true;
      }
      for (unsigned i = 0; i * 8 < size; ++i)
      {
        uint64_t bytes = std::min<uint64_t>(8, size - i * 8);
        if (classes[i] == Sse)
        {
          bool isDouble = false;
          for (auto& [offset, s] : scalars)
            isDouble = isDouble || (offset == i * 8 && sizeOf(s, at) == 8);
          p.parts.push_back(isDouble ? "double" : upperHalf[i] ? "<2 x float>" : "float");
          ++p.sses;
        }
        else
        {
          p.parts.push_back("i" + std::to_string(bytes * 8)); // (padding alone: an integer too)
          ++p.ints;
        }
        p.offsets.push_back(i * 8);
      }
      p.inMemory = false;
      return p;
    }

    // SysV's registers left for a C call's arguments as they are passed.
    struct CRegisters
    {
      unsigned ints = 6, sses = 8;
    };

    // The LLVM type a C call's struct argument of type t is passed as, given the registers left
    // (taken when it goes in registers): one text per register ("i64", "double"), or one `byval`
    // text for a copy on the stack, or "ptr" (Windows' pointer to a copy).
    std::vector<std::string> cArgumentTypes(const Ty* t, CRegisters& left, CPassing& p, Node* at)
    {
      p = cPassing(t, at);
      if (!sh.target.windows() && !p.inMemory && (p.ints > left.ints || p.sses > left.sses))
        p.inMemory = true; // not enough registers left: all of it on the stack, as C does
      if (!p.inMemory)
      {
        left.ints -= p.ints, left.sses -= p.sses;
        return p.parts;
      }
      if (sh.target.windows())
        return {"ptr"};
      uint64_t align = std::max<uint64_t>(8, alignOf(t, at));
      return {"ptr byval([" + std::to_string(sizeOf(t, at)) + " x i8]) align " + std::to_string(align)};
    }

    // A scalar argument of a C call takes one of SysV's registers.
    void cScalarArgument(const Ty* t, CRegisters& left)
    {
      if (!t)
        return;
      const Ty* u = types.unqualified(t);
      if (u->kind == TyKind::Float)
        left.sses -= left.sses > 0;
      else
        left.ints -= left.ints > 0;
    }

    // A C call's arguments as the target's C passes them: each struct by value (its argument text a
    // pointer to the call's copy) loaded into its registers, or passed `byval` from the copy, or (on
    // Windows, other than 1, 2, 4 or 8 bytes) the pointer itself. sretSlot: the call passes a result
    // slot first (it takes a register).
    void lowerCArguments(std::vector<std::pair<const Ty*, std::string>>& args, bool sretSlot, Node* at)
    {
      CRegisters left;
      if (sretSlot)
        left.ints -= 1;
      for (auto& [t, text] : args)
      {
        if (!t || !isAggregate(t))
        {
          cScalarArgument(t, left);
          continue;
        }
        if (text.compare(0, 4, "ptr ") != 0)
        {
          if (sh.target.windows())
            continue; // a class through `...`, which argumentText passed as MSVC does already
          unsupported(at, "a struct argument of a C function that is no copy in memory (" + text + ")");
        }
        std::string addr = text.substr(4);
        CPassing p;
        std::vector<std::string> passed = cArgumentTypes(t, left, p, at);
        if (p.inMemory)
        {
          text = passed[0] + " " + addr;
          continue;
        }
        text.clear();
        for (size_t i = 0; i < p.parts.size(); ++i)
        {
          std::string from = addr;
          if (p.offsets[i])
          {
            from = newTemp();
            fn += "  " + from + " = getelementptr inbounds i8, ptr " + addr + ", i64 " + std::to_string(p.offsets[i]) + "\n";
          }
          std::string v = newTemp();
          fn += "  " + v + " = load " + p.parts[i] + ", ptr " + from + "\n";
          text += (i ? ", " : "") + p.parts[i] + " " + v;
        }
      }
    }

    // How a C function returns its result of class type ret (a struct by value): in registers (the
    // LLVM return type, its parts at their offsets), or through the caller's slot (inMemory).
    std::string cReturnType(const CPassing& p)
    {
      if (p.parts.size() == 1)
        return p.parts[0];
      std::string t = "{ ";
      for (size_t i = 0; i < p.parts.size(); ++i)
        t += (i ? ", " : "") + p.parts[i];
      return t + " }";
    }

    // The `declare` line's return type and parameters of C function f under its target's C calling
    // convention (signatureOf's, with its structs by value lowered).
    std::pair<std::string, std::string> cDeclaration(FunctionDecl* f, const Signature& s)
    {
      const Ty* ft = declType(f);
      const Ty* ret = ft->inner;
      std::string returnType = s.returnType, params;
      CRegisters left;
      if (s.hasSret)
      {
        CPassing r = cPassing(ret, f);
        if (!r.inMemory)
          returnType = cReturnType(r); // (no slot parameter)
        else
          params = "ptr", left.ints -= 1;
      }
      for (size_t i = s.hasSret ? 1 : 0; i < s.paramTypes.size(); ++i)
      {
        const Ty* pt = s.paramTys[i];
        std::vector<std::string> texts;
        if (pt && isAggregate(pt))
        {
          CPassing p;
          texts = cArgumentTypes(pt, left, p, f);
        }
        else
        {
          cScalarArgument(pt, left);
          texts = {s.paramTypes[i]};
        }
        for (const std::string& t : texts)
          params += (params.empty() ? "" : ", ") + t;
      }
      if (s.variadic)
        params += params.empty() ? "..." : ", ...";
      return {returnType, params};
    }

    // A function (in every module) that moves `this` by the offset `at` (a pointer constant) and calls
    // callee, or the function in slot `slot` of the vtable `this` then has, with its other arguments.
    std::string sharedThunk(const std::string& name, const Signature& s, const std::string& at, const std::string& callee, int slot)
    {
      auto lock = sharedLock();
      if (emittedShared[name])
        return name;
      emittedShared[name] = true;
      std::string params, args;
      for (size_t i = 1; i < s.paramTypes.size(); ++i)
      {
        params += ", " + s.paramTypes[i] + " %p" + std::to_string(i);
        args += ", " + s.paramTypes[i] + " %p" + std::to_string(i);
      }
      std::string body = "  %off = ptrtoint ptr " + at + " to i64\n  %self = getelementptr i8, ptr %this, i64 %off\n";
      std::string target = callee;
      if (slot >= 0)
      {
        body += "  %vt = load ptr, ptr %self\n  %sp = getelementptr inbounds ptr, ptr %vt, i64 " + std::to_string(slot) + "\n  %fp = load ptr, ptr %sp\n";
        target = "%fp";
      }
      if (s.returnType == "void")
        body += "  tail call void " + target + "(ptr %self" + args + ")\n  ret void\n";
      else
        body += "  %r = tail call " + s.returnType + " " + target + "(ptr %self" + args + ")\n  ret " + s.returnType + " %r\n";
      shared += sharedComdat(name) + "define linkonce_odr " + s.returnType + " " + name + "(ptr %this" + params + ") comdat {\nentry:\n" + body + "}\n";
      return name;
    }

    // The calling convention of a member function of type ft, as signatureOf gives it for a
    // declaration: `this`, the result's slot for a class result, then the parameters.
    Signature methodSignature(const Ty* ft, Node* at)
    {
      Signature s;
      const Ty* ret = ft->inner;
      if (returnsInMemory(ret))
      {
        s.hasSret = true;
        s.returnType = "void";
      }
      else
        s.returnType = !ret || !ret->isKnown() || ret->isReference() ? "ptr" : llType(ret, at);
      s.hasThis = true;
      s.paramTypes.push_back("ptr");
      s.paramTys.push_back(nullptr);
      if (s.hasSret)
      {
        s.paramTypes.push_back("ptr");
        s.paramTys.push_back(nullptr);
      }
      for (const Ty* pt : ft->args)
      {
        s.paramTypes.push_back(isAggregate(pt) || pt->isReference() || !pt->isKnown() ? "ptr" : llType(pt, at));
        s.paramTys.push_back(pt);
      }
      s.variadic = ft->variadic;
      return s;
    }

    // Signature under the arguments currently in effect.
    Signature signatureOf(FunctionDecl* f)
    {
      const Ty* ft = declType(f);
      if (!ft || ft->kind != TyKind::Function)
        unsupported(f, "a function whose type is unknown");
      Signature s;
      bool method = global.owningClass(f) && !(f->flags & DF_Static) && !global.isExternC(f);
      const Ty* ret = ft->inner;
      if (returnsInMemory(ret))
      {
        s.hasSret = true;
        s.returnType = "void";
      }
      else if (!ret || !ret->isKnown())
        s.returnType = "ptr"; // unknown return type: treat as opaque pointer
      else
        s.returnType = ret->isReference() ? "ptr" : llType(ret, f);
      if (method)
      {
        s.hasThis = true;
        s.paramTypes.push_back("ptr");
        s.paramTys.push_back(nullptr);
      }
      if (s.hasSret)
      {
        s.paramTypes.push_back("ptr");
        s.paramTys.push_back(nullptr);
      }
      for (size_t i = 0; i < ft->args.size(); ++i)
      {
        const Ty* pt = ft->args[i];
        if (pt->kind == TyKind::Pack)
        {
          // A deduced pack still in one piece (`Args&&...` of a member template): one parameter per element.
          for (const Ty* e : pt->args)
          {
            s.paramTypes.push_back(isAggregate(e) || e->isReference() || !e->isKnown() ? "ptr" : llType(e, f));
            s.paramTys.push_back(e);
          }
          continue;
        }
        s.paramTypes.push_back(isAggregate(pt) || pt->isReference() || !pt->isKnown() ? "ptr" : llType(pt, f));
        s.paramTys.push_back(pt);
      }
      s.variadic = ft->variadic;
      return s;
    }

    std::string signatureText(const Signature& s)
    {
      std::string t = s.returnType + " (";
      for (size_t i = 0; i < s.paramTypes.size(); ++i)
        t += (i ? ", " : "") + s.paramTypes[i];
      if (s.variadic)
        t += s.paramTypes.empty() ? "..." : ", ...";
      t += ")";
      return t;
    }

