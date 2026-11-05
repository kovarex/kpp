// struct Emitter's members: sizes, alignment, class layouts, vtables and fields. Part of
// codegen.cpp, included in the struct's body in Codegen/Emitter.hpp (a file of its own since
// 2026-11-05, moved without a change).
    uint64_t sizeOf(const Ty* t, Node* at = nullptr)
    {
      switch (t->kind)
      {
        case TyKind::Bool: return 1;
        case TyKind::Char: case TyKind::Int: case TyKind::Float: return t->bits / 8;
        case TyKind::Nullptr: case TyKind::Pointer: case TyKind::LRef: case TyKind::RRef: case TyKind::Function:
        case TyKind::MemberPointer:
          return 8;
        case TyKind::Lambda:
        {
          // The closure struct's own size: its fields where LLVM puts them (a reference capture is
          // a pointer; fieldTypes holds what it points to), rounded to the largest alignment; an
          // empty closure is one byte.
          if (!t->decl)
            return 8;
          const ClosureInfo& info = closureFor(t, at);
          uint64_t size = 0, align = 1;
          for (size_t i = 0; i < info.fieldTypes.size(); ++i)
          {
            uint64_t s = info.byRef[i] ? 8 : sizeOf(info.fieldTypes[i], at), a = info.byRef[i] ? 8 : llvmAlignOf(info.fieldTypes[i], at);
            size = (size + a - 1) / a * a + s;
            align = std::max(align, a);
          }
          return std::max<uint64_t>((size + align - 1) / align * align, 1);
        }
        case TyKind::Array: return t->count * sizeOf(t->inner, at);
        case TyKind::Vector: return t->count * sizeOf(t->inner, at);
        case TyKind::Enum: return sizeOf(enumUnderlying(t->decl), at);
        case TyKind::Class: return classSizeAndAlign(t, at).first;
        default:
          unsupported(at, "sizeof " + types.toString(t, &symbols));
      }
    }

    // A class's size and alignment from its fields, remembered once its layout is complete.
    std::unordered_map<const Ty*, std::pair<uint64_t, uint64_t>> sizeCache; // this emitter's view of the remembered sizes
    std::pair<uint64_t, uint64_t> classSizeAndAlign(const Ty* t, Node* at)
    {
      noteLayout(t);
      if (auto mine = sizeCache.find(t); mine != sizeCache.end())
        return mine->second;
      if (std::pair<uint64_t, uint64_t> known; sh.finalSizes.find(t, known))
        return sizeCache.emplace(t, known).first->second;
      auto lock = sharedLock(); // the size is remembered in the shared layout
      StructInfo& info = structFor(t, at);
      if (info.align)
      {
        sizeCache.emplace(t, std::pair{info.size, info.align});
        if (mayPublish())
          sh.finalSizes.publish(t, {info.size, info.align});
        return {info.size, info.align};
      }
      if (info.sizing)
        unsupported(at, "the class " + types.toString(t, &symbols) + ", which contains itself (a member or base of its own type)");
      struct Sizing
      {
        bool& flag;
        Sizing(bool& flag) : flag(flag) { flag = true; }
        ~Sizing() { flag = false; }
      } sizing(info.sizing);
      uint64_t size = 0, align = 1;
      for (size_t i = 0; i < info.fieldTypes.size(); ++i)
      {
        if (i < info.zeroSize.size() && info.zeroSize[i])
          continue; // an empty base: no storage of its own
        const Ty* f = info.fieldTypes[i];
        uint64_t a = info.packed ? 1 : alignOf(f, at);
        size = (size + a - 1) / a * a + sizeOf(f, at);
        align = std::max(align, a);
      }
      align = std::max(align, declaredAlignment(t, at)); // `struct alignas(16) X`
      size = std::max<uint64_t>((size + align - 1) / align * align, 1);
      if (info.complete)
        info.size = size, info.align = align;
      return {size, align};
    }

    uint64_t alignOf(const Ty* t, Node* at = nullptr)
    {
      switch (t->kind)
      {
        case TyKind::Array: return alignOf(t->inner, at);
        case TyKind::Vector: return t->count * sizeOf(t->inner, at); // SSE vectors are 16-byte aligned
        case TyKind::Class: return classSizeAndAlign(t, at).second;
        case TyKind::Lambda:
        {
          if (!t->decl)
            return 8;
          const ClosureInfo& info = closureFor(t, at);
          uint64_t align = 1;
          for (size_t i = 0; i < info.fieldTypes.size(); ++i)
            align = std::max(align, info.byRef[i] ? uint64_t(8) : alignOf(info.fieldTypes[i], at));
          return align;
        }
        default: return std::max<uint64_t>(1, sizeOf(t, at));
      }
    }

    // The alignment a class declares with alignas(N), or 1; N under the instantiation's arguments
    // (`template<size_t Align> struct alignas(Align) AlignedBytes`).
    uint64_t declaredAlignment(const Ty* t, Node* at)
    {
      ClassDecl* c = classDef(t->decl, at);
      if (!c->alignment)
        return 1;
      TemplateArgs args = global.classArgsOf(canonicalClass(t, at));
      return alignmentValue(c->alignment, c, args, types.toString(t, &symbols), at);
    }

    // An alignas(N) written in owner (a class, a function), N under args: a power of two.
    uint64_t alignmentValue(Expr* alignment, Decl* owner, const TemplateArgs& args, const std::string& what, Node* at)
    {
      ArgsScope scope(*this, &args);
      ConstContextScope context(*this, owner);
      ParsedFile* saved = currentFile;
      currentFile = owner && owner->file ? owner->file : currentFile;
      std::optional<int64_t> v = evalConst(alignment);
      currentFile = saved;
      if (!v || *v <= 0 || (*v & (*v - 1)))
        unsupported(at, "the alignment of " + what + ", which is not a constant power of two");
      return uint64_t(*v);
    }

    // The alignment LLVM gives a value of type t from the type alone (its ABI alignment): what a
    // class's alignas adds on top of its fields' it only gets from padding fields and `align`.
    uint64_t llvmAlignOf(const Ty* t, Node* at)
    {
      switch (t->kind)
      {
        case TyKind::Array: return llvmAlignOf(t->inner, at);
        case TyKind::Class: return structFor(t, at).llvmAlign;
        default: return alignOf(t, at);
      }
    }

    // `, align N` for an object of type t (and an alignas(N) of the variable itself, or 0) whose
    // alignment is more than LLVM gives its type: an alloca's, a global's.
    std::string alignSuffix(const Ty* t, uint64_t declared, Node* at)
    {
      t = types.unqualified(types.stripRef(t));
      if (!declared && !overAlignedType(t, at))
        return "";
      uint64_t want = std::max(declared, alignOf(t, at));
      return want > llvmAlignOf(t, at) ? ", align " + std::to_string(want) : std::string();
    }

    // Whether an object of type t needs more alignment than LLVM gives its type (a class with
    // alignas, or holding a member that has it).
    bool overAlignedType(const Ty* t, Node* at)
    {
      t = types.unqualified(t);
      while (t->kind == TyKind::Array)
        t = types.unqualified(t->inner);
      return t->kind == TyKind::Class && structFor(t, at).overAligned;
    }

    // The alignment of a class (info, laid out but for its LLVM text) and of each field: LLVM aligns
    // a field to its type's ABI alignment only, so where alignas asks for more (on the class, on a
    // member, or a member's class has it) the bytes C++ puts before a field become a `[n x i8]` field
    // of their own, and the class gets the size and alignment sizeof and alignof give. Returns the
    // padding to write at the end of the struct (after its byte of its own, when empty).
    uint64_t alignPadding(StructInfo& info, ClassDecl* c, const Ty* key, const TemplateArgs& args, Node* at)
    {
      uint64_t natural = 1;
      for (size_t i = 0; i < info.fieldTypes.size(); ++i)
        if (!info.zeroSize[i] && !info.packed)
          natural = std::max(natural, llvmAlignOf(info.fieldTypes[i], at));
      info.llvmAlign = natural;
      if (info.packed)
        return 0;
      std::vector<uint64_t> declaredAt(info.fieldTypes.size(), 0); // a member's own alignas
      bool needed = false;
      for (Decl* m : c->members)
        if (m->kind == NodeKind::VariableDecl && static_cast<VariableDecl*>(m)->alignment && info.fieldIndex.count(m))
        {
          uint32_t i = info.fieldIndex[m];
          declaredAt[i] = std::max(declaredAt[i], alignmentValue(static_cast<VariableDecl*>(m)->alignment, c, args,
                                                                 "the member " + sym(static_cast<VariableDecl*>(m)->name.last().symbol), at));
          needed = needed || declaredAt[i] > llvmAlignOf(info.fieldTypes[i], at);
        }
      for (size_t i = 0; i < info.fieldTypes.size() && !needed; ++i)
        needed = !info.zeroSize[i] && overAlignedType(info.fieldTypes[i], at);
      uint64_t declaredClass = c->alignment ? declaredAlignment(key, at) : 1;
      if (!needed && declaredClass <= natural)
        return 0;
      std::vector<const Ty*> fields;
      std::vector<bool> zero;
      std::vector<uint32_t> newIndex(info.fieldTypes.size());
      uint64_t offset = 0, align = 1;
      for (size_t i = 0; i < info.fieldTypes.size(); ++i)
      {
        bool none = info.zeroSize[i];
        uint64_t want = none ? 1 : std::max(alignOf(info.fieldTypes[i], at), declaredAt[i]);
        uint64_t llvmWant = none ? 1 : llvmAlignOf(info.fieldTypes[i], at);
        uint64_t place = (offset + want - 1) / want * want;
        if (place != (offset + llvmWant - 1) / llvmWant * llvmWant)
        {
          fields.push_back(types.array(types.intType(8), uint32_t(place - offset)));
          zero.push_back(false);
        }
        newIndex[i] = uint32_t(fields.size());
        fields.push_back(info.fieldTypes[i]);
        zero.push_back(none);
        offset = place + (none ? 0 : sizeOf(info.fieldTypes[i], at));
        align = std::max(align, want);
      }
      if (info.empty)
        offset = std::max<uint64_t>(offset, 1); // its byte of its own
      align = std::max(align, declaredClass);
      uint64_t size = std::max<uint64_t>((offset + align - 1) / align * align, 1);
      info.fieldTypes = std::move(fields);
      info.zeroSize = std::move(zero);
      for (auto& [d, index] : info.fieldIndex)
        index = newIndex[index];
      for (auto& [d, index] : info.bases)
        index = newIndex[index];
      for (auto& [d, index] : info.anonymous)
        index = newIndex[index];
      info.size = size;
      info.align = align;
      info.overAligned = align > natural;
      uint64_t llvmSize = (offset + natural - 1) / natural * natural;
      return size > llvmSize ? size - offset : 0;
    }

    // A local's or a global's own alignas(N), or 0: N in the context it is written in (the function
    // being emitted for a local, the variable itself for a global), under the current arguments.
    uint64_t variableAlignment(VariableDecl* v, Decl* context, Node* at)
    {
      if (!v || !v->alignment)
        return 0;
      return alignmentValue(v->alignment, context, currentArgs ? *currentArgs : TemplateArgs{}, sym(v->name.last().symbol), at);
    }

    // A class type whose class is declared but has no definition in the program.
    bool declaredOnly(const Ty* t)
    {
      t = classKey(t);
      if (!t || !t->decl)
        return false;
      Decl* def = global.definitionOf(t->decl);
      return !def || def->kind != NodeKind::ClassDecl;
    }

    ClassDecl* classDef(Decl* decl, Node* at)
    {
      Decl* def = global.definitionOf(decl);
      if (!def || def->kind != NodeKind::ClassDecl)
        fail(at, qualifiedName(decl) + " is only declared (no definition in the program), and this use needs its layout");
      auto* c = static_cast<ClassDecl*>(def);
      // An imported C struct the C parser could not read in full: its layout without what it skipped
      // would silently be another than C's.
      if (c->importError)
        fail(at, qualifiedName(decl) + ": the C import could not read its definition (" + std::string(c->importError) +
                     (c->importErrorAt != UINT32_MAX ? " at `" + sym(c->importErrorAt) + "`" : std::string()) + "), and this use needs its layout");
      return c;
    }

    // The instantiation type with the definition as its decl (forward declarations map here).
    const Ty* canonicalClass(const Ty* t, Node* at)
    {
      t = classKey(t);
      ClassDecl* def = classDef(t->decl, at);
      std::vector<const Ty*> args;
      bool changed = def != t->decl;
      for (const Ty* a : t->args)
      {
        args.push_back(definitionsIn(a));
        changed = changed || args.back() != a;
      }
      return changed ? types.classType(def, Span<const Ty*>{args.data(), uint32_t(args.size())}) : t;
    }

    // Type t with every class in it named by its definition, a forward declaration's decl and the
    // definition's being one class: the key of a class's layout (and IR name) is then the same however
    // its arguments were reached (`ReverseIterator<const agui::Widget**>` with Widget declared ahead in
    // one file and defined in another was two layouts, the second named `.2` in whatever order the
    // threads met them, so the IR differed from run to run).
    std::unordered_map<const Ty*, const Ty*> definitionsCache;
    const Ty* definitionsIn(const Ty* t, int depth = 0)
    {
      if (!t || depth > 32)
        return t;
      if (auto it = definitionsCache.find(t); it != definitionsCache.end())
        return it->second;
      const Ty* u = types.unqualified(t);
      const Ty* result = u;
      switch (u->kind)
      {
        case TyKind::Pointer: result = types.pointer(definitionsIn(u->inner, depth + 1)); break;
        case TyKind::LRef: result = types.lref(definitionsIn(u->inner, depth + 1)); break;
        case TyKind::RRef: result = types.rref(definitionsIn(u->inner, depth + 1)); break;
        case TyKind::Array: result = u->decl ? u : types.array(definitionsIn(u->inner, depth + 1), u->count); break;
        case TyKind::Class:
          if (!u->isBraceList())
          {
            Decl* def = u->decl ? global.definitionOf(u->decl) : nullptr;
            std::vector<const Ty*> args;
            bool changed = def && def != u->decl && def->kind == NodeKind::ClassDecl;
            for (const Ty* a : u->args)
            {
              args.push_back(definitionsIn(a, depth + 1));
              changed = changed || args.back() != a;
            }
            if (changed)
              result = types.classType(def && def->kind == NodeKind::ClassDecl ? def : u->decl, Span<const Ty*>{args.data(), uint32_t(args.size())});
          }
          break;
        default: break;
      }
      if (result != u && (t->isConst || t->isVolatile))
        result = types.qualified(result, t->isConst, t->isVolatile);
      else if (result == u)
        result = t;
      definitionsCache.emplace(t, result);
      return result;
    }

    // The layouts this emitter has used: complete (or failed) ones, which do not change any more,
    // so they are read without the shared lock.
    std::unordered_map<const Ty*, StructInfo*> layoutCache;

    StructInfo& structFor(const Ty* classTy, Node* at)
    {
      if (auto mine = layoutCache.find(classTy); mine != layoutCache.end())
        return checkedLayout(*mine->second, classTy, at);
      if (StructInfo* known; sh.finalLayouts.find(classTy, known))
        return checkedLayout(*layoutCache.emplace(classTy, known).first->second, classTy, at);
      auto lock = sharedLock();
      StructInfo* info = nullptr;
      if (auto known = structsByType.find(classTy); known != structsByType.end())
        info = known->second;
      else
      {
        const Ty* key = canonicalClass(classTy, at);
        auto it = structs.find(key);
        if (it != structs.end())
        {
          structsByType.emplace(classTy, &it->second);
          info = &it->second;
        }
        else
        {
          size_t errorsBefore = errors.size();
          try
          {
            info = &buildStruct(key, at);
          }
          catch (Unsupported&)
          {
            // Every use reports what made it fail, at the use: the first use like the others
            // (its own report came from inside the layout, wherever that was), so that the lines
            // do not depend on which use comes first.
            structs[key].failed = true;
            structs[key].failedWhy = lastFailure;
            structs[key].failedShown = lastFailureShown;
            ++sh.failuresRemembered;
            layoutCache.emplace(classTy, &structs[key]);
            if (mayPublish())
              sh.finalLayouts.publish(classTy, &structs[key]);
            errors.resize(errorsBefore);
            fail(at, structs[key].failedWhy, structs[key].failedShown);
          }
        }
      }
      if (info->complete || info->failed)
      {
        layoutCache.emplace(classTy, info);
        if (mayPublish())
          sh.finalLayouts.publish(classTy, info);
      }
      return checkedLayout(*info, classTy, at);
    }

    // A failed layout gives every use the error its first use got (the same lines whichever use
    // comes first, so that they do not depend on the order functions are emitted in).
    StructInfo& checkedLayout(StructInfo& info, const Ty* classTy, Node* at)
    {
      noteLayout(classTy);
      if (info.failed)
        fail(at, info.failedWhy.empty() ? "codegen does not support the class " + types.toString(classKey(classTy), &symbols) + ", whose layout failed yet" : info.failedWhy, info.failedShown);
      return info;
    }

    // The placeholder class of a type name declared nowhere (a draft) that a value of type t is or
    // holds by value (through arrays), or null.
    static const Ty* draftHeld(const Ty* t)
    {
      for (; t; t = t->kind == TyKind::Array ? t->inner : nullptr)
        if (t->kind == TyKind::Class && t->decl && (t->decl->attributes & ATTR_Draft))
          return t;
      return nullptr;
    }

    // Lays out a class: bases first, then its data members; every member and base must be known
    // (a guess would be a wrong layout that compiles).
    StructInfo& buildStruct(const Ty* key, Node* at)
    {
      ClassDecl* c = static_cast<ClassDecl*>(key->decl);
      auto layoutError = [&](const std::string& why) { unsupported(at, "the layout of " + types.toString(key, &symbols) + ": " + why); };
      // A member as written (`Tick lastCheck`), for the error naming what has no known type.
      auto memberName = [&](Decl* m)
      {
        std::string name = m->kind == NodeKind::VariableDecl && !static_cast<VariableDecl*>(m)->name.empty() ? sym(static_cast<VariableDecl*>(m)->name.last().symbol) : std::string("?");
        if (!m->file || !m->file->tokens || m->endToken <= m->token || m->endToken > m->file->tokens->size())
          return name;
        const std::vector<Token>& toks = *m->file->tokens;
        size_t from = toks[m->token].offset, to = toks[m->endToken - 1].offset + toks[m->endToken - 1].length;
        std::string written;
        for (char ch : m->file->text.substr(from, std::min<size_t>(to - from, 160)))
          if (ch != '\n' && ch != '\r' && !(ch == ' ' && !written.empty() && written.back() == ' '))
            written += ch == '\t' ? ' ' : ch;
        return name + " (`" + written + "`)";
      };
      // A type name declared nowhere is the resolver's placeholder class (a draft, so that checking
      // goes on): an error wherever a layout needs it, never an empty class that compiles.
      // (Not shown again when the checker reported the name where it is written.)
      if (c->attributes & ATTR_Draft)
        fail(at, "the type '" + global.draftName(c) + "' is not declared", !global.undeclaredReported(global.draftName(c)));
      auto undeclared = [&](const std::string& what, const Ty* draft)
      {
        fail(at, "the layout of " + types.toString(key, &symbols) + ": " + what + " names '" + global.draftName(draft->decl) + "', which is not declared",
             !global.undeclaredReported(global.draftName(draft->decl)));
      };
      // `Imm<strlen("x")>`: an argument the checker could not work out (a constant it could not
      // evaluate) is no argument to lay out with; taken for 0, `char data[N]` was an empty array
      // that writes ran past.
      for (size_t i = 0; i < key->args.size(); ++i)
        if (!key->args[i] || !key->args[i]->isKnown())
        {
          Span<Decl*> params = key->decl->templateParams;
          auto* p = i < params.size() ? static_cast<TemplateParamDecl*>(params[i]) : nullptr;
          std::string which = p && p->name != UINT32_MAX ? "the argument for " + sym(p->name) : "argument " + std::to_string(i + 1);
          fail(at, "the layout of " + types.toString(key, &symbols) + ": " + which +
                       (p && p->isType ? " is a type the compiler could not resolve" : " is a constant the compiler could not evaluate"));
        }
      TemplateArgs args = global.classArgsOf(key);
      if (c->keyword == Kw::union_)
      {
        // A union is one block of storage; every member lives at its start.
        StructInfo& info = structs[key];
        info.name = irTypeName("%struct.", key);
        info.isUnion = true;
        uint64_t size = 1, align = 1;
        for (Decl* m : c->members)
        {
          const Ty* ft = nullptr;
          if (m->kind == NodeKind::VariableDecl && !(m->flags & (DF_Static | DF_Typedef | DF_Friend)))
          {
            if (!whenMemberHolds(m, c, args, key, at))
              continue; // (a `when` member this instance does not have)
            ft = global.typeOfDeclIn(m, args);
          }
          else if (m->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(m)->name.empty() && static_cast<ClassDecl*>(m)->isDefinition)
            ft = types.classType(m, key->args); // an anonymous struct takes the enclosing instantiation's arguments
          else
            continue;
          info.fieldIndex[m] = 0;
          info.fieldDecls.push_back(m);
          if (!ft || !ft->isKnown() || types.isDependent(ft))
            layoutError("the member " + memberName(m) + " has an unknown type");
          if (const Ty* draft = draftHeld(ft))
            undeclared("the member " + memberName(m), draft);
          for (const Ty* a = types.unqualified(ft); a->kind == TyKind::Array; a = types.unqualified(a->inner))
            if (a->count == 0 && !(c->file && c->file->isImported))
              fail(at, "the layout of " + types.toString(key, &symbols) + ": the member " + memberName(m) + " is an array without a length in a union (no flexible array member there)");
          if (m->kind == NodeKind::ClassDecl)
            info.anonymous.push_back({m, 0});
          size = std::max(size, sizeOf(ft, at));
          align = std::max(align, alignOf(ft, at));
          if (m->kind == NodeKind::VariableDecl && static_cast<VariableDecl*>(m)->alignment)
            align = std::max(align, alignmentValue(static_cast<VariableDecl*>(m)->alignment, c, args, "the member " + memberName(m), at));
        }
        // A [[packed]] union (ImmutableString's, in a packed class: MSVC's #pragma pack(1)) is as
        // large as its largest member, aligned to 1: no padding up to a member's alignment.
        if (c->attributes & ATTR_Packed)
          align = 1;
        // The storage: integers of the union's alignment, up to i128 (LLVM's most aligned integer);
        // a member aligned to more (OptionalOwningStorage of an alignas(64) class) makes the union
        // over-aligned, its objects then aligned with `align`.
        uint64_t unit = std::min<uint64_t>(align, 16), total = (size + align - 1) / align * align;
        info.llvmAlign = unit;
        info.fieldTypes.push_back(types.array(types.intType(uint8_t(unit * 8)), uint32_t(total / unit)));
        if (align > unit)
        {
          info.size = total;
          info.align = align;
          info.overAligned = true;
        }
        decls += info.name + " = type { " + llType(info.fieldTypes[0], c) + " }\n";
        ++result.structs;
        info.complete = true;
        return structs[key];
      }
      StructInfo& info = structs[key];
      info.name = irTypeName("%struct.", key);
      // Bases first (resolved before deciding on the vtable pointer).
      std::vector<const Ty*> baseKeys;
      for (const ClassDecl::Base& b : c->bases)
      {
        if (b.isPack)
          layoutError("a pack of base classes (`: Ts...`)");
        const Ty* bt = global.typeOfAstIn(b.type, c, args);
        if (!bt || bt->kind != TyKind::Class || types.isDependent(bt))
          layoutError("a base class whose type is unknown (" + (bt ? types.toString(bt, &symbols) : std::string("?")) + ")");
        if (const Ty* draft = draftHeld(bt))
          undeclared("a base class", draft);
        const Ty* baseKey = canonicalClass(bt, at);
        if (structFor(baseKey, at).flexible)
          fail(at, "the layout of " + types.toString(key, &symbols) + ": the base " + types.toString(baseKey, &symbols) +
                       " has a flexible array member, whose elements this class's members would overlap");
        baseKeys.push_back(baseKey);
      }
      // The primary base is the first polymorphic one: at offset 0, its vtable pointer the class's
      // own. A polymorphic class without a polymorphic base has a vtable pointer of its own there.
      // The polymorphic bases are laid out first, then the others (MSVC's layout, design.md section
      // 9); info.bases keeps declaration order, the order bases are built in.
      int primary = -1;
      for (size_t i = 0; i < baseKeys.size() && primary < 0; ++i)
        if (structs[baseKeys[i]].polymorphic)
          primary = int(i);
      bool ownVptr = false;
      if (primary < 0)
        for (Decl* m : c->members)
          if (m->kind == NodeKind::FunctionDecl && (m->flags & (DF_Virtual | DF_Override)) && !(m->flags & (DF_Static | DF_Constructor)) &&
              m->templateParams.empty())
            ownVptr = true;
      info.polymorphic = primary >= 0 || ownVptr;
      info.primaryBase = primary >= 0 ? baseKeys[primary] : nullptr;
      if (ownVptr)
        info.fieldTypes.push_back(types.pointer(types.voidType()));
      std::vector<uint32_t> baseIndex(baseKeys.size());
      for (bool polymorphicPass : {true, false})
        for (size_t i = 0; i < baseKeys.size(); ++i)
          if (structs[baseKeys[i]].polymorphic == polymorphicPass)
          {
            baseIndex[i] = uint32_t(info.fieldTypes.size());
            info.fieldTypes.push_back(baseKeys[i]);
          }
      for (size_t i = 0; i < baseKeys.size(); ++i)
        info.bases.push_back({baseKeys[i]->decl, baseIndex[i]});
      int bitUnitIndex = -1;    // the storage field the current run of bit fields shares
      uint32_t bitUnitBits = 0; // its size in bits
      uint32_t bitUsed = 0;
      for (Decl* m : c->members)
      {
        if (m->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(m)->name.empty() && static_cast<ClassDecl*>(m)->isDefinition)
        {
          // Anonymous struct/union member: a field whose own members are reachable by name, laid
          // out under the enclosing instantiation's arguments (ImmutableStringTemplate<N>'s union
          // holding a LongStr<N>).
          const Ty* ft = types.classType(m, key->args);
          structFor(ft, at);
          info.anonymous.push_back({m, uint32_t(info.fieldTypes.size())});
          info.fieldIndex[m] = uint32_t(info.fieldTypes.size());
          info.fieldDecls.push_back(m);
          info.fieldTypes.push_back(ft);
          bitUnitIndex = -1;
          continue;
        }
        if (m->kind != NodeKind::VariableDecl || (m->flags & (DF_Static | DF_Typedef | DF_Friend)))
          continue;
        // A `when` member on the template's parameters is in this instance's layout only where its
        // condition holds (design.md 19); two of one name that both hold are a clash.
        if (!whenMemberHolds(m, c, args, key, at))
          continue;
        if (!static_cast<VariableDecl*>(m)->name.empty())
          for (Decl* f : info.fieldDecls)
            if ((m->when || f->when) && f->kind == NodeKind::VariableDecl && !static_cast<VariableDecl*>(f)->name.empty() &&
                static_cast<VariableDecl*>(f)->name.last().symbol == static_cast<VariableDecl*>(m)->name.last().symbol)
              fail(at, "the layout of " + types.toString(key, &symbols) + ": two `when` declarations of the member " + memberName(m) +
                           " exist at once, a clash (design.md 19)");
        const Ty* ft = global.typeOfDeclIn(m, args);
        if (!ft || !ft->isKnown() || types.isDependent(ft))
          layoutError("the member " + memberName(m) + " has an unknown type");
        if (const Ty* draft = draftHeld(ft))
          undeclared("the member " + memberName(m), draft);
        // `char data[N]` with N no constant the checker could evaluate has the length 0; only a
        // struct's last data member may be such an array, written without a length (`char data[];`,
        // a flexible array member), after another data member (C's rules; not in a union, above).
        for (const Ty* a = types.unqualified(ft); a && a->kind == TyKind::Array; a = types.unqualified(a->inner))
          if (a->count == 0 && !(c->file && c->file->isImported))
          {
            auto* declared = static_cast<VariableDecl*>(m)->type;
            bool unbounded = a == types.unqualified(ft) && declared && declared->kind == NodeKind::ArrayType && !static_cast<ArrayType*>(declared)->size;
            if (!unbounded)
              fail(at, "the layout of " + types.toString(key, &symbols) + ": the member " + memberName(m) +
                           " is an array whose length is no constant the compiler could evaluate");
            bool last = true;
            for (auto after = std::find(c->members.begin(), c->members.end(), m) + 1; after != c->members.end(); ++after)
              last = last && !((*after)->kind == NodeKind::VariableDecl && !((*after)->flags & (DF_Static | DF_Typedef | DF_Friend))) &&
                     !((*after)->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(*after)->name.empty() && static_cast<ClassDecl*>(*after)->isDefinition);
            if (!last || info.fieldDecls.empty() || static_cast<VariableDecl*>(m)->bitfield)
              fail(at, "the layout of " + types.toString(key, &symbols) + ": the member " + memberName(m) + " is a flexible array member " +
                          (!last ? "before another data member" : info.fieldDecls.empty() ? "with no data member before it" : "with a bit width") +
                          " (a flexible array member is the last data member of a struct with others)");
            info.flexible = true;
          }
        // A lambda held by value (FunctionImpl<F>'s `F f`, TransformView's): its closure, laid out
        // from the lambda's AST whoever asks first; one that cannot be laid out fails this class.
        for (const Ty* held = types.unqualified(ft); held; held = held->kind == TyKind::Array ? types.unqualified(held->inner) : nullptr)
          if (isLambda(held))
            closureFor(held, at);
        if (Expr* widthExpr = static_cast<VariableDecl*>(m)->bitfield)
        {
          // `unsigned level : 3;`: consecutive bit fields share a storage unit while their types
          // have one size and their widths fit (MSVC: `uint8_t x : 5; bool on : 1;` share a byte,
          // `int a : 3; unsigned b : 4;` four bytes); each remembers its shift and width.
          // The width under the class's arguments, in its context: not the arguments of whichever
          // function asked for the layout first.
          ParsedFile* savedFile = currentFile;
          currentFile = c->file ? c->file : currentFile;
          std::optional<int64_t> w;
          {
            ArgsScope widthArgs(*this, &args);
            ConstContextScope widthContext(*this, c);
            w = evalConst(widthExpr);
          }
          currentFile = savedFile;
          if (!w || *w <= 0)
            unsupported(m, "a bit field whose width is not a constant");
          const Ty* unit = types.unqualified(ft);
          if (unit->kind == TyKind::Enum)
            unit = enumUnderlying(unit->decl);
          if (unit->kind == TyKind::Bool)
            unit = types.intType(8, true);
          uint32_t unitBits = uint32_t(sizeOf(unit, m) * 8), width = uint32_t(*w);
          if (bitUnitIndex < 0 || bitUnitBits != unitBits || bitUsed + width > unitBits)
          {
            bitUnitIndex = int(info.fieldTypes.size());
            bitUnitBits = unitBits;
            bitUsed = 0;
            info.fieldTypes.push_back(unit);
            info.fieldDecls.push_back(m); // the first field of the unit stands for the unit in memberwise work
          }
          info.fieldIndex[m] = uint32_t(bitUnitIndex);
          info.bitfields[m] = {bitUsed, width};
          bitfieldOf[m] = {bitUsed, width, unit};
          bitUsed += width;
          continue;
        }
        bitUnitIndex = -1;
        info.fieldIndex[m] = uint32_t(info.fieldTypes.size());
        info.fieldDecls.push_back(m);
        info.fieldTypes.push_back(ft);
      }
      // Empty bases take no storage, unless the first member is of the same class (two objects of
      // one type at one address: C++ gives the base a byte then).
      info.zeroSize.assign(info.fieldTypes.size(), false);
      const Ty* firstMember = info.fieldDecls.empty() ? nullptr : classKey(info.fieldTypes[fieldIndexOf(info, info.fieldDecls[0])]);
      bool allBasesEmpty = true;
      for (auto& [baseDef, index] : info.bases)
      {
        bool emptyBase = structs[info.fieldTypes[index]].empty;
        allBasesEmpty = allBasesEmpty && emptyBase;
        info.zeroSize[index] = emptyBase && classKey(info.fieldTypes[index]) != firstMember;
      }
      info.empty = !info.polymorphic && info.fieldDecls.empty() && allBasesEmpty;
      info.packed = (c->attributes & ATTR_Packed) != 0;
      uint64_t tailPadding = alignPadding(info, c, key, args, at);
      info.tailPadding = tailPadding;
      std::string line = info.name + " = type " + (info.packed ? "<{ " : "{ ");
      for (size_t i = 0; i < info.fieldTypes.size(); ++i)
        line += (i ? ", " : "") + (info.zeroSize[i] ? std::string("[0 x i8]") : llType(info.fieldTypes[i], c));
      for (Decl* m : info.fieldDecls)
      {
        const Ty* held = types.unqualified(info.fieldTypes[fieldIndexOf(info, m)]);
        while (held->kind == TyKind::Array)
          held = types.unqualified(held->inner);
        if (isClass(held) && structFor(held, c).flexible) // (laid out already, for its field above)
          fail(at, "the layout of " + types.toString(key, &symbols) + ": the member " + memberName(m) + " holds " + types.toString(held, &symbols) +
                       ", which has a flexible array member: only a pointer may refer to such an object inside another");
      }
      if (info.empty)
        line += info.fieldTypes.empty() ? "i8" : ", i8"; // one byte of its own as an object
      if (tailPadding)
        line += ", [" + std::to_string(tailPadding) + " x i8]";
      line += info.packed ? " }>\n" : " }\n";
      decls += line;
      ++result.structs;
      info.complete = true;
      if (structs[key].polymorphic)
      {
        // The base fields are final now (alignPadding may have moved them).
        std::vector<uint32_t> finalIndex;
        for (auto& [baseDef, index] : structs[key].bases)
          finalIndex.push_back(index);
        structs[key].vtables = collectVtables(key, c, args, baseKeys, finalIndex, primary);
        emitVtables(key, at);
      }
      return structs[key];
    }

    // The vtables of class key (design.md section 9): the primary one extends the primary base's
    // (or starts empty, the class's own); then every vtable of the other polymorphic bases, and those
    // of the primary base after its first, as the bases have them, at the bases' places. The class's
    // virtual functions take the slots they override in all of them; one that overrides no slot of
    // the primary vtable also gets a slot there, so that a call through the class needs no thunk.
    std::vector<Vtable> collectVtables(const Ty* key, ClassDecl* c, const TemplateArgs& args, const std::vector<const Ty*>& baseKeys,
                                       const std::vector<uint32_t>& baseIndex, int primary)
    {
      std::vector<Vtable> tables(1);
      for (size_t i = 0; i < baseKeys.size(); ++i)
      {
        std::vector<Vtable> inherited = structs[baseKeys[i]].vtables;
        for (size_t j = 0; j < inherited.size(); ++j)
        {
          Vtable& t = inherited[j];
          for (VtableSlot& s : t.slots)
            s.ownerPath.insert(s.ownerPath.begin(), baseIndex[i]);
          if (int(i) == primary && j == 0)
          {
            tables[0].slots = std::move(t.slots);
            continue;
          }
          t.path.insert(t.path.begin(), baseIndex[i]);
          t.name.clear();
          t.vptrValue.clear();
          tables.push_back(std::move(t));
        }
      }
      auto addToPrimary = [&](const VtableSlot& slot)
      {
        for (const VtableSlot& s : tables[0].slots)
          if (s.key == slot.key)
            return;
        tables[0].slots.push_back(slot);
      };
      bool declaresDestructor = false;
      {
        ArgsScope scope(*this, &args);
        for (Decl* m : c->members)
        {
          if (m->kind != NodeKind::FunctionDecl || (m->flags & (DF_Static | DF_Constructor)) || !m->templateParams.empty())
            continue;
          auto* f = static_cast<FunctionDecl*>(m);
          declaresDestructor = declaresDestructor || (f->flags & DF_Destructor);
          VtableSlot own{methodKey(f), f, key, {}};
          bool overrides = false;
          for (Vtable& t : tables)
            for (VtableSlot& s : t.slots)
              if (s.key == own.key)
              {
                s = own;
                overrides = true;
              }
          if (overrides || (f->flags & (DF_Virtual | DF_Override)))
            addToPrimary(own);
        }
      }
      if (!declaresDestructor)
      {
        // A class that declares no destructor has an implicit one, virtual when a base's is: it
        // destroys this class's members too, so it takes the destructor slots.
        VtableSlot implicit{"~", nullptr, key, {}};
        bool virtualDestructor = false;
        for (Vtable& t : tables)
          for (VtableSlot& s : t.slots)
            if (s.key == "~")
            {
              s = implicit;
              virtualDestructor = true;
            }
        if (virtualDestructor)
          addToPrimary(implicit);
      }
      return tables;
    }

    // The address an object's subobject at path (base fields down from class cls) has in an object
    // placed at address 0, which is its offset: a constant expression, "null" for the object itself.
    std::string subobjectAtNull(const Ty* cls, const std::vector<uint32_t>& path, Node* at)
    {
      std::string p = "null";
      const Ty* t = cls;
      for (uint32_t index : path)
      {
        StructInfo& info = structFor(t, at);
        p = "getelementptr (" + info.name + ", ptr " + p + ", i32 0, i32 " + std::to_string(index) + ")";
        t = info.fieldTypes[index];
      }
      return p;
    }

    // Whether two subobjects at these paths start at the same address: a field 0 is at its
    // object's start, so trailing zeros go (the primary base's subobject, and its primary base's).
    static bool samePlace(std::vector<uint32_t> a, std::vector<uint32_t> b)
    {
      while (!a.empty() && a.back() == 0)
        a.pop_back();
      while (!b.empty() && b.back() == 0)
        b.pop_back();
      return a == b;
    }

    // The definitions of class key's vtables, with a thunk for each slot whose function belongs to a
    // class at another place than the subobject whose vtable pointer leads to the slot. A vtable
    // pointer points at the first function's slot, the functions following in declaration order
    // (MSVC's layout and COM's: a C object's table of functions is called as a kpp class's, and the
    // reverse). Slot -1 is the class's descriptor (typeid, dynamic_cast), slot -2 the offset of the
    // subobject holding the pointer within the whole object (delete and dynamic_cast find the whole
    // object with it).
    // The name a class's vtables, thunks and typeinfo are called by: its printed name, and for a class
    // whose printed name another class shares, the number its IR type got (irTypeName's ".N"). A
    // local class of a function template has one name for all instances
    // (`LambdaIDConnector::make::InnerResolver@73` for every Callback): their vtables were one
    // linkonce_odr vtable, which the linker kept for all, so every InnerResolver ran one callback.
    std::string classIrName(const Ty* key, const StructInfo& info)
    {
      std::string name = irString(key);
      const size_t hashed = std::string("%struct.").size() + 16; // "%struct." and 16 hex digits
      if (info.name.size() > hashed && info.name[hashed] == '.')
        name += "#" + info.name.substr(hashed + 1);
      return name;
    }

    void emitVtables(const Ty* key, Node* at)
    {
      StructInfo& info = structs[key];
      std::string ti = descriptorFor(key, at);
      std::vector<Vtable> tables = info.vtables;
      std::string text, thunks;
      // The vtable asks for its functions, not whichever function happened to need the layout first
      // (the requester the diagnostics name, the same in every run).
      struct NameRestore { std::string& name; std::string saved; ~NameRestore() { name = std::move(saved); } } restore{currentFunctionName, currentFunctionName};
      currentFunctionName = "the vtable of " + types.toString(key, &symbols);
      NameRestore contextRestore{currentContext, currentContext};
      NameRestore requesterContextRestore{currentRequestedByContext, currentRequestedByContext};
      currentContext = currentFunctionName;
      currentRequestedByContext.clear();
      for (size_t t = 0; t < tables.size(); ++t)
      {
        Vtable& table = tables[t];
        table.name = "@\"vtable." + classIrName(key, info) + (t ? "." + std::to_string(t) : std::string()) + "\"";
        std::string entries = "ptr " + subobjectAtNull(key, table.path, at) + ", ptr " + ti;
        for (size_t s = 0; s < table.slots.size(); ++s)
        {
          const VtableSlot& slot = table.slots[s];
          if (slot.function && (slot.function->flags & DF_Pure))
          {
            entries += ", ptr null";
            continue;
          }
          std::string target = slot.function ? useFunction(slot.function, global.classArgsOf(slot.owner), slot.owner)
                                             : implicitDestructor(slot.owner, at); // a class declaring none still destroys its own members
          if (!samePlace(slot.ownerPath, table.path))
          {
            std::string thunk = "@\"thunk." + classIrName(key, info) + "." + std::to_string(t) + "." + std::to_string(s) + "\"";
            thunks += adjustingThunk(thunk, target, slot, subobjectAtNull(key, table.path, at), subobjectAtNull(key, slot.ownerPath, at), at);
            target = thunk;
          }
          entries += ", ptr " + target;
        }
        std::string tableType = "[" + std::to_string(table.slots.size() + 2) + " x ptr]";
        text += "$" + table.name.substr(1) + " = comdat any\n" + table.name + " = linkonce_odr constant " + tableType + " [" + entries + "], comdat\n";
        table.vptrValue = "getelementptr inbounds (" + tableType + ", ptr " + table.name + ", i64 0, i64 2)";
      }
      StructInfo& done = structs[key];
      done.vtables = std::move(tables);
      done.vtableName = done.vtables[0].name;
      done.vptrValue = done.vtables[0].vptrValue;
      done.vtableText = text + thunks;
    }

    // A function that moves `this` from the subobject at `from` to the one at `to` (offsets as
    // subobjectAtNull gives them) and calls target with the arguments it was given.
    std::string adjustingThunk(const std::string& name, const std::string& target, const VtableSlot& slot, const std::string& from, const std::string& to, Node* at)
    {
      Signature s;
      if (slot.function)
      {
        TemplateArgs args = global.classArgsOf(slot.owner);
        ArgsScope scope(*this, &args);
        s = signatureOf(slot.function);
      }
      else
      {
        s.returnType = "void";
        s.paramTypes = {"ptr"};
        s.hasThis = true;
      }
      if (s.variadic)
        unsupported(at, "the variadic virtual function " + qualifiedName(slot.function) + " reached through another base of " +
                            types.toString(slot.owner, &symbols) + " (its thunk cannot pass the arguments on)");
      std::string params, args;
      for (size_t i = 1; i < s.paramTypes.size(); ++i)
      {
        params += ", " + s.paramTypes[i] + " %p" + std::to_string(i);
        args += ", " + s.paramTypes[i] + " %p" + std::to_string(i);
      }
      std::string body = "  %to = ptrtoint ptr " + to + " to i64\n  %from = ptrtoint ptr " + from + " to i64\n  %d = sub i64 %to, %from\n"
                         "  %this1 = getelementptr i8, ptr %this, i64 %d\n";
      if (s.returnType == "void")
        body += "  tail call void " + target + "(ptr %this1" + args + ")\n  ret void\n";
      else
        body += "  %r = tail call " + s.returnType + " " + target + "(ptr %this1" + args + ")\n  ret " + s.returnType + " %r\n";
      return "$" + name.substr(1) + " = comdat any\ndefine linkonce_odr " + s.returnType + " " + name + "(ptr %this" + params + ") comdat {\nentry:\n" + body + "}\n";
    }

    // What a pointer to a member function of class cls holds when calling f with the object is not
    // enough: a virtual f calls the object's override (through the vtable pointer of owner, the class
    // declaring f), and an f of a base at another place in cls gets `this` moved to that base (path,
    // from cls down to owner). One function per class and member, in every module.
    std::string memberFunctionThunk(const Ty* cls, const Ty* owner, const std::vector<std::pair<std::string, uint32_t>>& path, FunctionDecl* f, int slot, Node* at)
    {
      Signature s;
      std::string key;
      {
        TemplateArgs args = global.classArgsOf(owner);
        ArgsScope scope(*this, &args);
        s = signatureOf(f);
        key = methodKey(f);
      }
      if (s.variadic)
        unsupported(at, "a pointer to the variadic member function " + qualifiedName(f) + " (its thunk cannot pass the arguments on)");
      std::string target = slot < 0 ? useFunction(f, global.classArgsOf(owner), owner) : std::string();
      return sharedThunk("@\"pmf." + irString(cls) + "." + irString(owner) + "::" + key + "\"", s, pathAtNull(path), target, slot);
    }

    // The offset of the subobject a path of base fields leads to, as a pointer constant (see
    // subobjectAtNull).
    static std::string pathAtNull(const std::vector<std::pair<std::string, uint32_t>>& path)
    {
      std::string offset = "null";
      for (auto& [structName, index] : path)
        offset = "getelementptr (" + structName + ", ptr " + offset + ", i32 0, i32 " + std::to_string(index) + ")";
      return offset;
    }

    // The descriptor of a class (what slot -1 of its vtables points at, what dynamic_cast searches):
    // { name, number of direct bases, [{ base's descriptor, base's offset }] }, for any class (a
    // polymorphic class's bases need not be polymorphic). Built once, used per module (useDescriptor).
    std::string descriptorFor(const Ty* cls, Node* at)
    {
      auto lock = sharedLock();
      StructInfo& info = structFor(cls, at);
      if (!info.typeInfoName.empty())
        return info.typeInfoName;
      const Ty* key = canonicalClass(cls, at);
      std::string name = "@\"typeinfo." + classIrName(key, info) + "\"";
      info.typeInfoName = name;
      bool savedShared = emittingShared;
      emittingShared = true; // the descriptor goes into every module that uses it
      std::string nameConstant = cStringConstant(types.toString(key, &symbols));
      emittingShared = savedShared;
      std::vector<std::pair<Decl*, uint32_t>> bases = info.bases;
      std::vector<const Ty*> fieldTypes = info.fieldTypes;
      std::string structName = info.name, list;
      for (auto& [baseDef, index] : bases)
        list += (list.empty() ? "" : ", ") + std::string("{ ptr, i64 } { ptr ") + descriptorFor(fieldTypes[index], at) +
                ", i64 ptrtoint (ptr getelementptr (" + structName + ", ptr null, i32 0, i32 " + std::to_string(index) + ") to i64) }";
      std::string arrayType = "[" + std::to_string(bases.size()) + " x { ptr, i64 }]";
      structFor(cls, at).descriptorText = "$" + name.substr(1) + " = comdat any\n" + name + " = linkonce_odr constant { ptr, i64, " + arrayType + " } { ptr " +
                                          nameConstant + ", i64 " + std::to_string(bases.size()) + ", " + arrayType + " " +
                                          (bases.empty() ? std::string("zeroinitializer") : "[" + list + "]") + " }, comdat\n";
      return name;
    }

    // A class's descriptor and its bases' in the module being emitted (each once).
    void useDescriptor(const StructInfo& info)
    {
      auto lock = sharedLock();
      if (info.descriptorText.empty() || !vtablesIn[currentModule].insert(info.typeInfoName).second)
        return;
      moduleDecls[currentModule] += info.descriptorText;
      for (auto& [baseDef, index] : std::vector<std::pair<Decl*, uint32_t>>(info.bases))
        useDescriptor(structFor(info.fieldTypes[index], nullptr));
    }

    // Identity of a method for override matching: name, parameter types and constness.
    std::string methodKey(FunctionDecl* f)
    {
      if (f->flags & DF_Destructor)
        return "~";
      std::string full = qualifiedName(f);
      size_t pos = full.rfind("::");
      std::string k = pos == std::string::npos ? full : full.substr(pos + 2);
      const Ty* ft = declType(f);
      k += "(";
      for (size_t i = 0; ft && ft->kind == TyKind::Function && i < ft->args.size(); ++i)
        k += (i ? ", " : "") + types.toString(ft->args[i], &symbols);
      k += ")";
      if (f->flags & DF_Const)
        k += " const";
      return k;
    }

    // Slot of f in the primary vtable of classTy, or -1 if f is not virtual there. A call goes
    // through the vtable pointer of the class that declares f (every virtual function of a class has
    // a slot in its primary vtable), with `this` moved to that class's subobject.
    int slotOf(const Ty* classTy, FunctionDecl* f, Node* at)
    {
      StructInfo& info = structFor(classTy, at);
      if (!info.polymorphic)
        return -1;
      TemplateArgs args = global.classArgsOf(canonicalClass(classTy, at));
      ArgsScope scope(*this, &args);
      return slotKeyed(structFor(classTy, at), methodKey(f));
    }
    static int slotKeyed(const StructInfo& info, const std::string& key)
    {
      if (info.vtables.empty())
        return -1;
      for (size_t i = 0; i < info.vtables[0].slots.size(); ++i)
        if (info.vtables[0].slots[i].key == key)
          return int(i);
      return -1;
    }

    // Loads a vtable slot of the object at addr: a function's (from 0), or the class descriptor (-1).
    std::string loadSlot(const std::string& addr, int slot)
    {
      std::string vptr = newTemp();
      fn += "  " + vptr + " = load ptr, ptr " + addr + "\n";
      std::string entry = newTemp();
      fn += "  " + entry + " = getelementptr inbounds ptr, ptr " + vptr + ", i64 " + std::to_string(slot) + "\n";
      std::string fp = newTemp();
      fn += "  " + fp + " = load ptr, ptr " + entry + "\n";
      return fp;
    }

    std::string gep(const std::string& structName, const std::string& addr, uint32_t index)
    {
      std::string r = newTemp();
      fn += "  " + r + " = getelementptr inbounds " + structName + ", ptr " + addr + ", i32 0, i32 " + std::to_string(index) + "\n";
      return r;
    }

    // The type a data member has in the object's class as laid out (under the class's arguments:
    // Holder<int&>'s `A value` is an int&), found in the class, its bases or its anonymous members;
    // null when it is none of them.
    const Ty* fieldTypeIn(const Ty* classTy, Decl* member, Node* at, int depth = 0)
    {
      if (!isClass(classTy) || depth > 16)
        return nullptr;
      StructInfo& info = structFor(classTy, at);
      if (auto it = info.fieldIndex.find(member); it != info.fieldIndex.end())
        return info.isUnion ? nullptr : info.fieldTypes[it->second];
      for (auto& [base, index] : std::vector<std::pair<Decl*, uint32_t>>(info.bases))
        if (const Ty* t = fieldTypeIn(structFor(classTy, at).fieldTypes[index], member, at, depth + 1))
          return t;
      for (auto& [anon, index] : std::vector<std::pair<Decl*, uint32_t>>(info.anonymous))
        if (const Ty* t = fieldTypeIn(structFor(classTy, at).fieldTypes[index], member, at, depth + 1))
          return t;
      return nullptr;
    }

    bool fieldAddress(const Ty* classTy, Decl* member, const std::string& objectAddr, std::string& out, Node* at)
    {
      StructInfo& info = structFor(classTy, at);
      auto it = info.fieldIndex.find(member);
      if (it != info.fieldIndex.end())
      {
        out = info.isUnion ? objectAddr : gep(info.name, objectAddr, it->second);
        return true;
      }
      // Fallback: the resolver may have a different Decl* for the same field (e.g. an out-of-line
      // definition or a using-declaration). Match by name when the pointer lookup fails.
      if (member->kind == NodeKind::VariableDecl)
      {
        auto* vd = static_cast<VariableDecl*>(member);
        if (!vd->name.empty())
        {
          uint32_t nameSymbol = vd->name.last().symbol;
          for (auto& [d, idx] : info.fieldIndex)
          {
            if (d->kind == NodeKind::VariableDecl)
            {
              auto* fd = static_cast<VariableDecl*>(d);
              if (!fd->name.empty() && fd->name.last().symbol == nameSymbol)
              {
                out = info.isUnion ? objectAddr : gep(info.name, objectAddr, idx);
                return true;
              }
            }
          }
        }
      }
      std::vector<std::pair<Decl*, uint32_t>> anonymous = info.anonymous;
      std::vector<std::pair<Decl*, uint32_t>> bases = info.bases;
      std::vector<const Ty*> fieldTypes = info.fieldTypes;
      bool isUnion = info.isUnion;
      std::string name = info.name;
      Span<const Ty*> classArgs = canonicalClass(classTy, at)->args; // an anonymous member's: its class's arguments (buildStruct)
      for (auto& [anon, index] : anonymous)
      {
        std::string addr = isUnion ? objectAddr : gep(name, objectAddr, index);
        if (fieldAddress(types.classType(anon, classArgs), member, addr, out, at))
          return true;
      }
      for (auto& [baseDef, index] : bases)
      {
        std::string baseAddr = gep(name, objectAddr, index);
        if (fieldAddress(fieldTypes[index], member, baseAddr, out, at))
          return true;
      }
      return false;
    }

    // Look up a field by symbol name in a class's own members and its bases (used when the
    // resolver left MemberExpr::resolved null).  Searches the AST directly, without building
    // struct layouts, so it's safe for types that structFor would choke on.
    Decl* findFieldByName(const Ty* classTy, uint32_t nameSymbol, Node* at, int depth = 0)
    {
      if (depth > 16 || !classTy || classTy->kind != TyKind::Class || !classTy->decl)
        return nullptr;
      Decl* def = global.definitionOf(classTy->decl);
      if (!def || def->kind != NodeKind::ClassDecl)
        return nullptr;
      auto* c = static_cast<ClassDecl*>(def);
      TemplateArgs args = global.classArgsOf(classTy);
      Decl* absent = nullptr; // a `when` member of the name this instance does not have
      for (Decl* m : c->members)
      {
        if (m->kind == NodeKind::VariableDecl && !(m->flags & (DF_Static | DF_Typedef | DF_Friend)))
        {
          auto* vd = static_cast<VariableDecl*>(m);
          if (!vd->name.empty() && vd->name.last().symbol == nameSymbol)
          {
            // (several `when` members may declare one name: the one this instance has)
            if (!whenMemberHolds(m, c, args, classTy, at))
            {
              absent = m;
              continue;
            }
            return m;
          }
        }
        // Search anonymous struct/union members.
        if (m->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(m)->name.empty() && static_cast<ClassDecl*>(m)->isDefinition)
          if (Decl* found = findFieldByName(types.classType(m, classTy->args), nameSymbol, at, depth + 1))
            return found;
      }
      // Search base classes.
      for (const ClassDecl::Base& b : c->bases)
      {
        const Ty* bt = global.typeOfAstIn(b.type, c, args);
        if (bt && bt->kind == TyKind::Class)
          if (Decl* found = findFieldByName(bt, nameSymbol, at, depth + 1))
            return found;
      }
      if (absent)
        fail(at, "'" + sym(nameSymbol) + "' does not exist in " + types.toString(classTy, &symbols) +
                     ": its `when` condition does not hold there (design.md 19)");
      return nullptr;
    }

    // Whether data member m of class c exists in the instance key (args its arguments): a `when`
    // member on a class template's parameters (design.md 19) is decided per instance, its condition
    // folded under the instance's arguments as a bit field's width is. The parser decided every
    // other `when` member (one it found false is no member of c).
    bool whenMemberHolds(Decl* m, ClassDecl* c, const TemplateArgs& args, const Ty* key, Node* at)
    {
      if (!m->when || m->whenKnown >= 0)
        return m->whenKnown != 0;
      ParsedFile* savedFile = currentFile;
      currentFile = c->file ? c->file : currentFile;
      std::optional<bool> holds;
      {
        ArgsScope whenArgs(*this, &args);
        ConstContextScope whenContext(*this, c);
        holds = evalCondition(m->when);
      }
      currentFile = savedFile;
      if (!holds)
        fail(at, "the `when` condition of " + types.toString(key, &symbols) + "'s member " +
                     (static_cast<VariableDecl*>(m)->name.empty() ? std::string("?") : sym(static_cast<VariableDecl*>(m)->name.last().symbol)) +
                     " is no constant the compiler could evaluate (design.md 19)");
      return *holds;
    }

