// FileConverterC.inl: FileConverter's members: C's rules (--c-sources).
// Part of convert.cpp, included in struct FileConverter's body in Converter/FileConverter.hpp (a file of its own since 2026-11-05, moved without a change).

    // The type a typedef names, followed through typedefs; const and volatile kept.
    Type* cUnaliased(Type* t, int depth = 0) const
    {
      while (t && depth++ < 16)
      {
        if (t->kind != NodeKind::NamedType)
          return t;
        Decl* d = static_cast<NamedType*>(t)->resolved;
        if (d && d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef))
          t = static_cast<VariableDecl*>(d)->type;
        else if (d && d->kind == NodeKind::UsingAliasDecl)
          t = static_cast<UsingAliasDecl*>(d)->type;
        else
          return t;
      }
      return t;
    }
    static Type* cWithoutQualifiers(Type* t)
    {
      while (t && t->kind == NodeKind::ConstType)
        t = static_cast<ConstType*>(t)->inner;
      return t;
    }
    // Whether t is a pointer to void (const or not), or to an object type other than void.
    bool cIsVoidPointer(Type* t) const
    {
      t = cWithoutQualifiers(cUnaliased(t));
      if (!t || t->kind != NodeKind::PointerType)
        return false;
      Type* pointee = cWithoutQualifiers(cUnaliased(static_cast<PointerType*>(t)->pointee));
      return pointee && pointee->kind == NodeKind::BuiltinType && static_cast<BuiltinType*>(pointee)->keyword == Kw::void_;
    }
    bool cIsObjectPointer(Type* t) const
    {
      Type* bare = cWithoutQualifiers(cUnaliased(t));
      if (!bare || bare->kind != NodeKind::PointerType || cIsVoidPointer(t))
        return false;
      Type* pointee = cWithoutQualifiers(cUnaliased(static_cast<PointerType*>(bare)->pointee));
      return pointee && pointee->kind != NodeKind::FunctionType;
    }
    // A type as kpp source: what a cast to it writes. Empty for a type this does not spell (a
    // function pointer).
    std::string cSpelling(Type* t) const
    {
      if (!t)
        return "";
      switch (t->kind)
      {
        case NodeKind::BuiltinType:
        {
          auto* b = static_cast<BuiltinType*>(t);
          std::string s = b->isUnsigned ? "unsigned " : b->isSigned ? "signed " : "";
          for (int i = 0; i < b->longCount; ++i)
            s += "long ";
          if (b->keyword != Kw::int_ || s.empty())
            s += std::string(keywordSpelling(b->keyword));
          else
            s.pop_back();
          return s;
        }
        case NodeKind::NamedType:
        {
          const QualifiedName& name = static_cast<NamedType*>(t)->name;
          std::string s = name.global ? "::" : "";
          for (size_t i = 0; i < name.parts.size(); ++i)
          {
            if (name.parts[i].hasTemplateArgs || name.parts[i].symbol == UINT32_MAX)
              return "";
            std::string part(symbols.name(name.parts[i].symbol));
            s += (i ? "::" : "") + part + (lookupKeyword(part) != UINT32_MAX ? "_" : ""); // (renamed as the C names are)
          }
          // (a scoped library's type, qualified as the code's uses are: minizip's `(const zlib::Bytef*)buf`)
          if (name.parts.size() == 1 && !name.global && scopedLibraryNames)
            if (auto library = scopedLibraryNames->find(s); library != scopedLibraryNames->end())
              s = library->second + "::" + s;
          return s;
        }
        case NodeKind::PointerType:
        {
          std::string pointee = cSpelling(static_cast<PointerType*>(t)->pointee);
          return pointee.empty() ? "" : pointee + "*";
        }
        case NodeKind::ConstType:
        {
          auto* c = static_cast<ConstType*>(t);
          std::string inner = cSpelling(c->inner);
          if (inner.empty())
            return "";
          return std::string(c->isConst ? "const " : "") + (c->isVolatile ? "volatile " : "") + inner;
        }
        default: return "";
      }
    }
    // The C library's functions that return void*.
    static bool cReturnsVoidPointer(std::string_view name)
    {
      static const std::set<std::string_view> names = {"malloc", "calloc", "realloc", "aligned_alloc", "_aligned_malloc",
                                                       "_aligned_realloc", "_recalloc", "memchr", "memcpy", "memmove",
                                                       "memset", "bsearch", "alloca", "_alloca", "_malloca"};
      return names.count(name) != 0;
    }
    // The declared type of what d names: a variable's, a parameter's, a data member's.
    static Type* cDeclType(Decl* d)
    {
      if (!d)
        return nullptr;
      if (d->kind == NodeKind::VariableDecl && !(d->flags & DF_Typedef))
        return static_cast<VariableDecl*>(d)->type;
      if (d->kind == NodeKind::ParamDecl)
        return static_cast<ParamDecl*>(d)->type;
      return nullptr;
    }
    // The member named `member` of the struct or union t (a pointer to it with ->), or null.
    Decl* cMemberOf(Type* t, uint32_t member, bool arrow) const
    {
      t = cWithoutQualifiers(cUnaliased(t));
      if (arrow)
      {
        if (!t || (t->kind != NodeKind::PointerType && t->kind != NodeKind::ArrayType))
          return nullptr;
        t = cWithoutQualifiers(cUnaliased(t->kind == NodeKind::PointerType ? static_cast<PointerType*>(t)->pointee : static_cast<ArrayType*>(t)->element));
      }
      if (!t || t->kind != NodeKind::NamedType)
        return nullptr;
      Decl* d = static_cast<NamedType*>(t)->resolved;
      if (!d || d->kind != NodeKind::ClassDecl)
        return nullptr;
      std::function<Decl*(ClassDecl*)> find = [&](ClassDecl* c) -> Decl*
      {
        for (Decl* m : c->members)
        {
          if (m->kind == NodeKind::VariableDecl && !static_cast<VariableDecl*>(m)->name.empty() && static_cast<VariableDecl*>(m)->name.last().symbol == member)
            return m;
          if (m->kind == NodeKind::ClassDecl && static_cast<ClassDecl*>(m)->name.empty()) // an anonymous struct or union's members
            if (Decl* inner = find(static_cast<ClassDecl*>(m)))
              return inner;
        }
        return nullptr;
      };
      return find(static_cast<ClassDecl*>(d));
    }
    // The type of a C expression, as far as the conversions below need it: null when unknown.
    Type* cTypeOf(Expr* e)
    {
      static BuiltinType voidType = []
      {
        BuiltinType v;
        v.kind = NodeKind::BuiltinType;
        v.token = 0;
        v.keyword = Kw::void_;
        return v;
      }();
      static PointerType voidPointer = []
      {
        PointerType p;
        p.kind = NodeKind::PointerType;
        p.token = 0;
        p.pointee = &voidType;
        return p;
      }();
      if (!e)
        return nullptr;
      switch (e->kind)
      {
        case NodeKind::ParenExpr: return cTypeOf(static_cast<ParenExpr*>(e)->inner);
        case NodeKind::NameExpr: return cDeclType(static_cast<NameExpr*>(e)->resolved);
        case NodeKind::MemberExpr:
        {
          auto* m = static_cast<MemberExpr*>(e);
          if (Type* t = cDeclType(m->resolved))
            return t;
          if (m->member.parts.size() != 1)
            return nullptr;
          return cDeclType(cMemberOf(cTypeOf(m->base), m->member.parts[0].symbol, m->op == P::Arrow));
        }
        case NodeKind::CallExpr:
        {
          Expr* callee = static_cast<CallExpr*>(e)->callee;
          while (callee->kind == NodeKind::ParenExpr)
            callee = static_cast<ParenExpr*>(callee)->inner;
          if (callee->kind != NodeKind::NameExpr || !static_cast<NameExpr*>(callee)->resolved ||
              static_cast<NameExpr*>(callee)->resolved->kind != NodeKind::FunctionDecl)
          {
            // through a function pointer (minizip's `bufio->filefunc.zopendisk_file(...)`)
            Type* f = cWithoutQualifiers(cUnaliased(cTypeOf(callee)));
            if (f && f->kind == NodeKind::PointerType)
              f = cWithoutQualifiers(cUnaliased(static_cast<PointerType*>(f)->pointee));
            if (f && f->kind == NodeKind::FunctionType)
              return static_cast<FunctionType*>(f)->result;
          }
          if (callee->kind != NodeKind::NameExpr)
            return nullptr;
          auto* name = static_cast<NameExpr*>(callee);
          if (name->resolved && name->resolved->kind == NodeKind::FunctionDecl)
            return static_cast<FunctionDecl*>(name->resolved)->returnType;
          if (!name->resolved && name->name.parts.size() == 1 && name->name.parts[0].symbol != UINT32_MAX)
          {
            // (a header's macro that names the function: ogg's `#define _ogg_malloc malloc`)
            std::string called(symbols.name(name->name.parts[0].symbol));
            if (headerAliases)
              if (auto alias = headerAliases->find(called); alias != headerAliases->end())
                called = alias->second;
            if (cReturnsVoidPointer(called))
              return &voidPointer;
          }
          return nullptr;
        }
        case NodeKind::CastExpr: return static_cast<CastExpr*>(e)->type;
        case NodeKind::IndexExpr:
        {
          Type* t = cWithoutQualifiers(cUnaliased(cTypeOf(static_cast<IndexExpr*>(e)->base)));
          return !t ? nullptr : t->kind == NodeKind::PointerType ? static_cast<PointerType*>(t)->pointee : t->kind == NodeKind::ArrayType ? static_cast<ArrayType*>(t)->element : nullptr;
        }
        case NodeKind::UnaryExpr:
        {
          auto* u = static_cast<UnaryExpr*>(e);
          if (!u->isKeyword && u->op == uint32_t(P::Star) && !u->postfix)
          {
            Type* t = cWithoutQualifiers(cUnaliased(cTypeOf(u->operand)));
            return t && t->kind == NodeKind::PointerType ? static_cast<PointerType*>(t)->pointee : nullptr;
          }
          return nullptr;
        }
        case NodeKind::BinaryExpr:
        {
          auto* b = static_cast<BinaryExpr*>(e);
          if (!b->isKeyword && (b->op == uint32_t(P::Assign) || b->op == uint32_t(P::PlusAssign) || b->op == uint32_t(P::MinusAssign)))
            return cTypeOf(b->lhs);
          if (!b->isKeyword && (b->op == uint32_t(P::Plus) || b->op == uint32_t(P::Minus)))
          {
            // pointer arithmetic keeps the pointer's type (a pointer minus a pointer is no pointer)
            Type* l = cTypeOf(b->lhs);
            Type* bare = cWithoutQualifiers(cUnaliased(l));
            if (bare && bare->kind == NodeKind::PointerType)
            {
              Type* r = cWithoutQualifiers(cUnaliased(cTypeOf(b->rhs)));
              return r && r->kind == NodeKind::PointerType ? nullptr : l;
            }
          }
          return nullptr;
        }
        case NodeKind::ConditionalExpr:
        {
          auto* c = static_cast<ConditionalExpr*>(e);
          return cTypeOf(c->then ? c->then : c->cond);
        }
        default: return nullptr;
      }
    }
    // Where an expression ends, from its first token: at the `;`, `,` or closing bracket that ends
    // the initializer, assignment, argument or return value it is.
    uint32_t cExpressionEnd(uint32_t first) const
    {
      int depth = 0;
      for (uint32_t i = first; i < toks.size(); ++i)
      {
        const Token& t = toks[i];
        if (t.is(P::LParen) || t.is(P::LBracket) || t.is(P::LBrace))
          ++depth;
        else if (t.is(P::RParen) || t.is(P::RBracket) || t.is(P::RBrace))
        {
          if (depth == 0)
            return toks[i].offset;
          --depth;
        }
        else if (depth == 0 && (t.is(P::Semi) || t.is(P::Comma)))
          return toks[i].offset;
      }
      return uint32_t(text.size());
    }
    static uint32_t cFirstToken(Expr* e)
    {
      for (;;)
      {
        if (e->kind == NodeKind::MemberExpr)
          e = static_cast<MemberExpr*>(e)->base;
        else if (e->kind == NodeKind::CallExpr)
          e = static_cast<CallExpr*>(e)->callee;
        else if (e->kind == NodeKind::IndexExpr)
          e = static_cast<IndexExpr*>(e)->base;
        else if (e->kind == NodeKind::UnaryExpr && static_cast<UnaryExpr*>(e)->postfix)
          e = static_cast<UnaryExpr*>(e)->operand;
        else if (e->kind == NodeKind::BinaryExpr)
          e = static_cast<BinaryExpr*>(e)->lhs;
        else if (e->kind == NodeKind::ConditionalExpr)
          e = static_cast<ConditionalExpr*>(e)->cond;
        else
          return e->token;
      }
    }
    // An insertion before the token at offset, which a rewrite of that token (a header's name macro
    // written out: `_ogg_malloc` as `malloc`) would cancel: then it goes into the rewrite's text.
    void cInsertBefore(uint32_t offset, const std::string& s)
    {
      for (Edit& e : edits)
        if (e.offset == offset && e.length > 0)
        {
          e.text = s + e.text;
          return;
        }
      insert(offset, s);
    }
    // C: a top-level declaration of a name its file keeps to itself (cPrivate): `private` before the
    // whole declaration and its `static` gone, also a `static` a keyword macro writes (zstd's
    // FORCE_INLINE_TEMPLATE, `static inline`), which drops the word from that rewrite.
    std::string cDeclName(Decl* d) const
    {
      const QualifiedName* name = d->kind == NodeKind::FunctionDecl ? &static_cast<FunctionDecl*>(d)->name
                                : d->kind == NodeKind::VariableDecl ? &static_cast<VariableDecl*>(d)->name
                                : d->kind == NodeKind::ClassDecl    ? &static_cast<ClassDecl*>(d)->name
                                : d->kind == NodeKind::EnumDecl     ? &static_cast<EnumDecl*>(d)->name
                                                                    : nullptr;
      if (!name || name->empty() || name->last().symbol == UINT32_MAX)
        return {};
      return std::string(symbols.name(name->last().symbol));
    }
    void cMakePrivate(Decl* d)
    {
      std::string name = cDeclName(d);
      bool anonymousEnum = d->kind == NodeKind::EnumDecl && name.empty();
      if (!cPrivate || (name.empty() && !anonymousEnum) || !(anonymousEnum ? cPrivate->count("") : cPrivate->count(name)))
        return;
      writePrivate(d);
    }
    // C++: an anonymous namespace at a source file's global scope (Atlas.cpp's helpers, AppManager.cpp's
    // ConfirmationBoxConnectToServer): its braces go and its declarations are the file's private ones
    // (design.md 15), when it holds no namespace (private is for a file's top-level declarations).
    bool cUnwrapAnonymousNamespace(NamespaceDecl* n)
    {
      for (Decl* inner : n->decls)
        if (inner->kind == NodeKind::NamespaceDecl || inner->kind == NodeKind::UsingDirectiveDecl)
          return false;
      uint32_t open = n->token;
      while (open < toks.size() && !toks[open].is(P::LBrace))
        ++open;
      uint32_t close = n->endToken;
      while (close > open && !toks[close - 1].is(P::RBrace))
        --close;
      if (open >= toks.size() || close <= open + 1 || !toks[n->token].is(Kw::namespace_))
        return false;
      uint32_t end = endOffset(open);
      remove(toks[n->token].offset, end - toks[n->token].offset);
      remove(toks[close - 1].offset, toks[close - 1].length);
      counts.add("C++ anonymous namespaces unwrapped, their declarations private");
      return true;
    }
    // `private` before a declaration (design.md 15) and its `static` gone, also a `static` a keyword
    // macro writes (zstd's FORCE_INLINE_TEMPLATE, `static inline`), which drops the word from that
    // rewrite. Not a function's prototype or a type's forward declaration: the conversion removes
    // those, kpp needs none.
    void writePrivate(Decl* d)
    {
      if ((d->kind == NodeKind::FunctionDecl && !static_cast<FunctionDecl*>(d)->body) ||
          (d->kind == NodeKind::ClassDecl && !static_cast<ClassDecl*>(d)->isDefinition) ||
          (d->kind == NodeKind::EnumDecl && !static_cast<EnumDecl*>(d)->isDefinition))
        return;
      uint32_t start = declStart(d);
      while (start < text.size() && (text[start] == ' ' || text[start] == '\t'))
        ++start;
      if (!cPrivateAt.insert(start).second)
        return;
      uint32_t k = d->token;
      while (k > 0 && toks[k - 1].offset >= start)
        --k;
      for (; k < toks.size() && !toks[k].is(P::LParen) && !toks[k].is(P::Assign) && !toks[k].is(P::LBrace) && !toks[k].is(P::Semi); ++k)
        if (toks[k].is(Kw::static_))
        {
          bool rewritten = false;
          for (Edit& e : edits)
            if (e.offset == toks[k].offset && e.length > 0)
            {
              // (a keyword macro's words: `static inline` -> `inline`)
              std::string words;
              for (size_t a = 0; a < e.text.size();)
              {
                size_t b = e.text.find(' ', a);
                if (b == std::string::npos)
                  b = e.text.size();
                if (e.text.compare(a, b - a, "static") != 0)
                  words += (words.empty() ? "" : " ") + e.text.substr(a, b - a);
                a = b + 1;
              }
              e.text = words;
              rewritten = true;
              break;
            }
          if (!rewritten)
          {
            uint32_t end = endOffset(k);
            while (end < text.size() && (text[end] == ' ' || text[end] == '\t'))
              ++end;
            remove(toks[k].offset, end - toks[k].offset);
          }
          break;
        }
      cInsertBefore(start, "private ");
      counts.add(in.c ? "C names kept to their file written private" : "C++ names kept to their file written private");
    }
    // A rewrite of the token at offset, which another rewrite of it (a keyword macro written out:
    // stb_image_write's `STBIWDEF` as `extern`) would cancel: then it takes that one's place.
    void cReplace(uint32_t offset, uint32_t length, const std::string& s)
    {
      for (Edit& e : edits)
        if (e.offset == offset && e.length == length)
        {
          e.text = s;
          return;
        }
      replace(offset, length, s);
    }
    // C: this file's own object-like macros, each with the text where it holds (from its #define to
    // its #undef or next #define: divsufsort.c defines STACK_SIZE twice), from the configured text
    // (the definitions of the branches the configuration keeps).
    struct COwnObject
    {
      std::string name, body;
      uint32_t from, to;
    };
    std::vector<COwnObject> cOwnObjects;
    bool cOwnObjectsRead = false;
    const std::string* cOwnObjectAt(const std::string& name, uint32_t offset)
    {
      if (!cOwnObjectsRead)
      {
        cOwnObjectsRead = true;
        std::string_view t = preprocessed ? std::string_view(preprocessed->configured) : text;
        std::map<std::string, size_t> open;
        for (size_t at = 0; at < t.size();)
        {
          size_t end = t.find('\n', at);
          if (end == std::string_view::npos)
            end = t.size();
          size_t lineStart = at;
          std::string line(t.substr(at, end - at));
          while (!line.empty() && (line.back() == '\\' || (line.back() == '\r' && line.size() > 1 && line[line.size() - 2] == '\\')) && end < t.size())
          {
            line.erase(line.find_last_of('\\'));
            size_t next = t.find('\n', end + 1);
            if (next == std::string_view::npos)
              next = t.size();
            line += " " + std::string(t.substr(end + 1, next - end - 1));
            end = next;
          }
          at = end + 1;
          size_t p = line.find_first_not_of(" \t");
          if (p == std::string::npos || line[p] != '#')
            continue;
          p = line.find_first_not_of(" \t", p + 1);
          if (p == std::string::npos)
            continue;
          bool define = line.compare(p, 6, "define") == 0, undef = line.compare(p, 5, "undef") == 0;
          if (!define && !undef)
            continue;
          p = line.find_first_not_of(" \t", p + (define ? 6 : 5));
          if (p == std::string::npos)
            continue;
          size_t n = p;
          while (n < line.size() && isIdentChar(line[n]))
            ++n;
          std::string name = line.substr(p, n - p);
          if (auto o = open.find(name); o != open.end())
          {
            cOwnObjects[o->second].to = uint32_t(lineStart);
            open.erase(o);
          }
          if (!define || (n < line.size() && line[n] == '('))
            continue;
          std::string body = line.substr(n);
          for (size_t c = body.find("/*"); c != std::string::npos; c = body.find("/*", c))
          {
            size_t e = body.find("*/", c + 2);
            body.replace(c, (e == std::string::npos ? body.size() : e + 2) - c, " ");
          }
          if (size_t c = body.find("//"); c != std::string::npos)
            body.erase(c);
          size_t a = body.find_first_not_of(" \t\r"), b = body.find_last_not_of(" \t\r");
          body = a == std::string::npos ? std::string() : body.substr(a, b - a + 1);
          if (body.empty())
            continue;
          open[name] = cOwnObjects.size();
          cOwnObjects.push_back({name, body, uint32_t(end), UINT32_MAX});
        }
      }
      const std::string* found = nullptr;
      for (const COwnObject& o : cOwnObjects)
        if (o.name == name && o.from <= offset && offset < o.to)
          found = &o.body;
      return found;
    }
    // C: a written-out macro's body with the object macros it names written out too, as the
    // preprocessor rescans (inftrees.h's `#define ENOUGH (ENOUGH_LENS+ENOUGH_DISTS)`).
    std::string cExpandedObject(const std::string& body, uint32_t useOffset, int depth = 0)
    {
      if (depth > 16)
        return body;
      std::string out;
      for (size_t i = 0; i < body.size();)
      {
        if (body[i] == '"' || body[i] == '\'')
        {
          size_t k = i + 1;
          while (k < body.size() && body[k] != body[i])
            k += body[k] == '\\' ? 2 : 1;
          out += body.substr(i, k + 1 - i);
          i = k + 1;
          continue;
        }
        if (isalpha((unsigned char)body[i]) || body[i] == '_')
        {
          size_t j = i;
          while (j < body.size() && isIdentChar(body[j]))
            ++j;
          std::string name = body.substr(i, j - i);
          auto macro = objectMacros.find(name);
          if (cOwnConstant(name))
            out += name;
          else if (const std::string* own = cOwnObjectAt(name, useOffset); own && *own != body && cWrittenOutMacros && cWrittenOutMacros->count("*"))
            out += cExpandedObject(*own, useOffset, depth + 1);
          else if (macro != objectMacros.end() && macro->second != body && macroHoldsAt(macro->first, useOffset))
            out += cExpandedObject(macro->second, useOffset, depth + 1);
          else if (cStaticRenames && cStaticRenames->count(name))
            out += cStaticRenames->at(name);
          else
            out += name;
          i = j;
          continue;
        }
        out += body[i++];
      }
      return out;
    }
    // C: an object-like macro a .c file defines once, at file scope, whose value is a number the
    // same at every use: a `private const auto` of its file (design.md 15) instead of its text
    // written out at its uses. Its body, with the header macros it names written out (as the
    // preprocessor rescans), is a literal, a negative one or parenthesized (so `x / SIZE` means
    // what it did), with no string (sizeof would differ) and no name but this file's other such
    // constants and type words (a variable it named would be the use's local in C, a global here).
    std::map<std::string, std::string> cOwnConstants; // name -> its value's text ("" once found not to be one)
    std::set<std::string> cScopeEnumerators; // an interface header's: its unscoped enumerations' enumerators
    std::vector<std::pair<uint32_t, uint32_t>> cTopLevelRanges;
    bool cTopLevelRangesRead = false;
    const std::string* cOwnConstant(const std::string& name, int depth = 0)
    {
      bool scope = isScopeHeader();
      if (!in.c || (in.path.extension() != ".c" && !scope) || !cWrittenOutMacros || !cWrittenOutMacros->count("*") || depth > 16)
        return nullptr;
      if (scope && cStandardName(name))
        return nullptr;
      if (knownFunctionMacros.count(name))
        return nullptr; // (trio's FORMAT_STRING, a SAL annotation's name too, whose #define is not read as one)
      if (auto known = cOwnConstants.find(name); known != cOwnConstants.end())
        return known->second.empty() ? nullptr : &known->second;
      cOwnConstants[name] = std::string(); // (also while its body is checked: a cycle is no constant)
      cOwnObjectAt(std::string(), 0);
      const COwnObject* own = nullptr;
      for (const COwnObject& o : cOwnObjects)
        if (o.name == name)
        {
          if (own)
            return nullptr; // defined twice
          own = &o;
        }
      if (!own || own->to != UINT32_MAX)
        return nullptr;
      if (!cTopLevelRangesRead && in.parsed)
      {
        cTopLevelRangesRead = true;
        for (Decl* d : in.parsed->decls)
          if (d->endToken > d->token && d->endToken <= toks.size())
            cTopLevelRanges.push_back({toks[d->token].offset, endOffset(d->endToken - 1)});
      }
      for (const auto& [from, to] : cTopLevelRanges)
        if (from <= own->from && own->from < to)
          return nullptr; // in a function or a struct
      // the body, header macros written out
      std::string value;
      std::string body = own->body;
      int expansions = 0;
      static const std::set<std::string> typeWords = {"sizeof", "unsigned", "signed", "int", "long", "short", "char", "float", "double", "void"};
      // An interface header's (scope): also a choice (zstd.h's `((int)(sizeof(size_t) == 4 ?
      // ZSTD_WINDOWLOG_MAX_32 : ZSTD_WINDOWLOG_MAX_64))`) and another name of one of its enumerators
      // (`#define ZSTD_STRATEGY_MIN ZSTD_fast`), a constant of the enumeration's type.
      bool enumerator = false;
      if (scope && cScopeEnumerators.empty() && in.parsed)
        for (Decl* d : in.parsed->decls)
          if (d->kind == NodeKind::EnumDecl && !static_cast<EnumDecl*>(d)->isClass)
            for (Decl* e : static_cast<EnumDecl*>(d)->enumerators)
              cScopeEnumerators.insert(std::string(symbols.name(static_cast<EnumeratorDecl*>(e)->name)));
      for (size_t i = 0; i < body.size();)
      {
        char c = body[i];
        if (c == '"')
          return nullptr;
        if (isdigit((unsigned char)c) || (c == '.' && i + 1 < body.size() && isdigit((unsigned char)body[i + 1])))
        {
          size_t j = i;
          while (j < body.size() && (isIdentChar(body[j]) || body[j] == '.' ||
                                     ((body[j] == '+' || body[j] == '-') && (body[j - 1] == 'e' || body[j - 1] == 'E') && !(body[i] == '0' && j > i + 1 && (body[i + 1] == 'x' || body[i + 1] == 'X')))))
            ++j;
          value += body.substr(i, j - i);
          i = j;
          continue;
        }
        if (isalpha((unsigned char)c) || c == '_')
        {
          size_t j = i;
          while (j < body.size() && isIdentChar(body[j]))
            ++j;
          std::string word = body.substr(i, j - i);
          i = j;
          if (typeWords.count(word) || (scope && word == "size_t"))
            value += word;
          else if (cOwnConstant(word, depth + 1) || (cScopeConstants && cScopeConstants->count(word) && !scope))
          {
            value += word;
            enumerator = enumerator || scope; // (made of the interface's constants: `ZSTD_CHAINLOG_MIN ZSTD_HASHLOG_MIN`)
          }
          else if (scope && cScopeEnumerators.count(word))
          {
            value += word;
            enumerator = true;
          }
          else if (auto macro = objectMacros.find(word); macro != objectMacros.end() && macroHoldsAt(macro->first, own->from) && ++expansions < 64)
          {
            // (a header's macro: its text in its place, checked as this one's; as the
            // preprocessor substitutes it, without parentheses of its own)
            body = body.substr(0, i - word.size()) + macro->second + body.substr(i);
            i -= word.size();
          }
          else
            return nullptr;
          continue;
        }
        if (!strchr(" 	()+-*/%<>|&~!^", c) && !(scope && strchr("?:=", c)))
          return nullptr;
        value += c;
        ++i;
      }
      // a literal, a negative one, or parenthesized as a whole
      size_t a = value.find_first_not_of(" 	"), b = value.find_last_not_of(" 	");
      if (a == std::string::npos)
        return nullptr;
      value = value.substr(a, b - a + 1);
      bool atom = value.find_first_of(" 	()+*/%<>|&~!^") == std::string::npos &&
                  value.find('-', 1) == std::string::npos;
      bool wrapped = value.front() == '(' && value.back() == ')';
      if (wrapped)
        for (size_t k = 0, open = 0; k < value.size(); ++k)
        {
          open += value[k] == '(';
          open -= value[k] == ')';
          if (open == 0 && k + 1 < value.size())
            wrapped = false;
        }
      if ((!atom && !wrapped) || (value.find_first_of("0123456789") == std::string::npos && !enumerator))
        return nullptr;
      cOwnConstants[name] = value;
      return &cOwnConstants[name];
    }
    // C's `U32 const x`, `ZSTD_window_t const window`, `struct S const* s` (zstd writes its consts
    // after the type): `const U32 x`, the const first, as kpp's parse reads it after a type's name
    // only so (language/east_const_after_type_name, pending).
    void cWestConst()
    {
      for (uint32_t i = 0; i + 2 < toks.size(); ++i)
      {
        if (toks[i].getKind() != TokenKind::Identifier || !toks[i + 1].is(Kw::const_) || memberAccessAt(i) ||
            (i > 0 && toks[i - 1].is(P::ColonColon)))
          continue;
        // (`x const` is no expression in C: a name before const is a type's, or an empty macro's,
        // triostr.c's `TRIO_PUBLIC_STRING TRIO_CONST char *`, which is no east const)
        if (cMacros && cMacros->empty.count(std::string(spelling(i))))
          continue;
        uint32_t first = i;
        if (i > 0 && (toks[i - 1].is(Kw::struct_) || toks[i - 1].is(Kw::union_) || toks[i - 1].is(Kw::enum_)))
          first = i - 1;
        cInsertBefore(toks[first].offset, "const ");
        remove(endOffset(i), endOffset(i + 1) - endOffset(i));
        counts.add("C's consts after a type's name written before it");
      }
    }
    // C's array parameters are pointers (`U32 rep[ZSTD_REP_NUM]` is `U32* rep`): written so, in
    // function declarations and in function types (zstd's ZSTD_blockCompressor), where kpp keeps the
    // array (language/array_parameter_is_pointer, pending).
    void cArrayParameter(Decl* param)
    {
      if (param->kind != NodeKind::ParamDecl)
        return;
      auto* p = static_cast<ParamDecl*>(param);
      cArrayParametersIn(p->type);
      if (!p->type || p->type->kind != NodeKind::ArrayType || p->name == UINT32_MAX)
        return;
      for (uint32_t k = p->token; k + 1 < toks.size() && !toks[k].is(P::Comma) && !toks[k].is(P::RParen); ++k)
        if (toks[k].getKind() == TokenKind::Identifier && toks[k].id == p->name && toks[k + 1].is(P::LBracket))
        {
          uint32_t close = k + 1;
          int depth = 0;
          for (; close < toks.size(); ++close)
            if (toks[close].is(P::LBracket))
              ++depth;
            else if (toks[close].is(P::RBracket) && --depth == 0)
              break;
          if (close >= toks.size() || toks[close + 1].is(P::LBracket)) // (an array of arrays: left alone)
            return;
          cInsertBefore(toks[k].offset, "*");
          remove(endOffset(k), endOffset(close) - endOffset(k));
          counts.add("C array parameters written as pointers");
          return;
        }
    }
    void cArrayParametersIn(Type* t)
    {
      for (int depth = 0; t && depth < 16; ++depth)
      {
        if (t->kind == NodeKind::PointerType)
          t = static_cast<PointerType*>(t)->pointee;
        else if (t->kind == NodeKind::ConstType)
          t = static_cast<ConstType*>(t)->inner;
        else if (t->kind == NodeKind::FunctionType)
        {
          for (Decl* param : static_cast<FunctionType*>(t)->params)
            cArrayParameter(param);
          return;
        }
        else
          return;
      }
    }
    void cArrayParameters(Span<Decl*> decls)
    {
      for (Decl* d : decls)
      {
        if (d->kind == NodeKind::FunctionDecl)
          for (Decl* param : static_cast<FunctionDecl*>(d)->params)
            cArrayParameter(param);
        else if (d->kind == NodeKind::VariableDecl)
          cArrayParametersIn(static_cast<VariableDecl*>(d)->type);
        else if (d->kind == NodeKind::ClassDecl)
          cArrayParameters(static_cast<ClassDecl*>(d)->members);
        else if (d->kind == NodeKind::NamespaceDecl)
          cArrayParameters(static_cast<NamespaceDecl*>(d)->decls);
      }
    }
    // C's typedef of a function type, `typedef void stbi_write_func(void *context, void *data, int size);`
    // (stb_image_write's): `using stbi_write_func = void(void *context, void *data, int size);`, the
    // form kpp resolves (language/typedef_of_function_type, pending).
    void cFunctionTypedefs()
    {
      for (uint32_t i = 0; i + 4 < toks.size(); ++i)
      {
        if (!toks[i].is(Kw::typedef_))
          continue;
        uint32_t name = i + 1;
        while (name + 1 < toks.size() && !toks[name].is(P::Semi) && !toks[name].is(P::LBrace) && !toks[name].is(P::LParen) &&
               !(toks[name].getKind() == TokenKind::Identifier && toks[name + 1].is(P::LParen)))
          ++name;
        if (name == i + 1 || name + 1 >= toks.size() || toks[name].getKind() != TokenKind::Identifier || !toks[name + 1].is(P::LParen))
          continue;
        uint32_t close = name + 1;
        for (int depth = 0; close < toks.size(); ++close)
          if (toks[close].is(P::LParen))
            ++depth;
          else if (toks[close].is(P::RParen) && --depth == 0)
            break;
        if (close + 1 >= toks.size() || !toks[close + 1].is(P::Semi))
          continue;
        // (the name as written out: a rename of it moves to the using)
        std::string spelled(text.substr(toks[name].offset, toks[name].length));
        for (const Edit& e : edits)
          if (e.offset == toks[name].offset && e.length == toks[name].length)
            spelled = e.text;
        cReplace(toks[i].offset, toks[i].length, "using " + spelled + " =");
        cReplace(toks[name].offset, toks[name].length, "");
        counts.add("C typedefs of function types written as using");
      }
    }
    // C's `(U64)(size_t)(oend-op)`, a cast of a cast of a parenthesized expression that the parse
    // may not have read as casts (size_t is no declared name for it): kpp reads `(T)(x)` before `(`
    // as a cast of the whole postfix expression, so the inner cast gets parentheses of its own:
    // `(U64)((size_t)(oend-op))`.
    void cCastChains()
    {
      for (uint32_t i = 0; i + 8 < toks.size(); ++i)
      {
        if (!toks[i].is(P::LParen) || toks[i + 1].getKind() != TokenKind::Identifier || !toks[i + 2].is(P::RParen) || !toks[i + 3].is(P::LParen) ||
            toks[i + 4].getKind() != TokenKind::Identifier || !toks[i + 5].is(P::RParen) || !toks[i + 6].is(P::LParen))
          continue;
        // (`f(a)(b)(c)` is calls: the first group must follow no name or `)`)
        if (i > 0 && (toks[i - 1].getKind() == TokenKind::Identifier || toks[i - 1].is(P::RParen) || toks[i - 1].is(P::RBracket)))
          continue;
        int depth = 0;
        for (uint32_t k = i + 6; k < toks.size(); ++k)
          if (toks[k].is(P::LParen))
            ++depth;
          else if (toks[k].is(P::RParen) && --depth == 0)
          {
            if (k + 1 < toks.size() && (toks[k + 1].is(P::Dot) || toks[k + 1].is(P::Arrow) || toks[k + 1].is(P::LBracket) || toks[k + 1].is(P::LParen)))
              break; // (a postfix after it: the chain means something else)
            cInsertBefore(toks[i + 3].offset, "(");
            insert(endOffset(k), ")");
            counts.add("C casts of casts parenthesized");
            break;
          }
      }
    }
    // Whether offset is directly inside a struct's or union's body (a member's declaration).
    bool inClassBody(uint32_t offset) const
    {
      const ScopeRange* innermost = nullptr;
      for (const ScopeRange& r : scopeRanges)
        if (r.begin <= offset && offset < r.end && (!innermost || r.begin >= innermost->begin))
          innermost = &r;
      return innermost && innermost->classBody;
    }
    // value converted to target as C converts it on assignment: a void* to an object pointer
    // gets the cast kpp needs.
    void cConvert(Expr* value, Type* target)
    {
      if (!value || !target || !cIsObjectPointer(target) || !cIsVoidPointer(cTypeOf(value)))
        return;
      cCast(value, cSpelling(target));
    }
    // The typed pointer parameters of the C library's functions that C code passes a void* (the
    // converter does not read the C library's headers): by function, by argument index.
    static const std::map<int, std::string>* cLibraryPointerParameters(std::string_view function)
    {
      static const std::map<std::string_view, std::map<int, std::string>> table = {
        {"_wopen", {{0, "const wchar_t*"}}},   // zlib's gz_open(const void* path, ...)
        {"wcstombs", {{1, "const wchar_t*"}}}, // and its length
        {"wcslen", {{0, "const wchar_t*"}}},   // minizip's iowin32.c (its filename is a void*)
        {"wcsncpy", {{0, "wchar_t*"}, {1, "const wchar_t*"}}},
        {"wcscpy", {{0, "wchar_t*"}, {1, "const wchar_t*"}}},
        {"wcscat", {{0, "wchar_t*"}, {1, "const wchar_t*"}}},
        {"strlen", {{0, "const char*"}}},
        {"strncpy", {{0, "char*"}, {1, "const char*"}}},
        {"strcpy", {{0, "char*"}, {1, "const char*"}}},
        {"strcat", {{0, "char*"}, {1, "const char*"}}},
        {"_tcslen", {{0, "const char*"}}}, // (the TCHAR names, narrow: tcharNames)
        {"_tcsncpy", {{0, "char*"}, {1, "const char*"}}},
        {"_tcscpy", {{0, "char*"}, {1, "const char*"}}},
        {"_tcscat", {{0, "char*"}, {1, "const char*"}}},
      };
      auto it = table.find(function);
      return it == table.end() ? nullptr : &it->second;
    }
    // value, a void*, cast to the object pointer type spelled.
    void cCast(Expr* value, const std::string& spelled)
    {
      if (spelled.empty())
        return;
      Expr* bare = value;
      while (bare->kind == NodeKind::ParenExpr)
        bare = static_cast<ParenExpr*>(bare)->inner;
      uint32_t first = cFirstToken(value);
      bool postfix = bare->kind == NodeKind::NameExpr || bare->kind == NodeKind::MemberExpr || bare->kind == NodeKind::CallExpr ||
                     bare->kind == NodeKind::IndexExpr || value->kind == NodeKind::ParenExpr;
      cInsertBefore(toks[first].offset, "(" + spelled + ")" + (postfix ? "" : "("));
      if (!postfix)
        insert(cExpressionEnd(first), ")");
      counts.add("void* converted to object pointers with a cast (C)");
    }
    void cWalkExprs(Span<Expr*> es)
    {
      for (Expr* e : es)
        cWalkExpr(e);
    }
    void cWalkExpr(Expr* e)
    {
      if (!e)
        return;
      switch (e->kind)
      {
        case NodeKind::ParenExpr: cWalkExpr(static_cast<ParenExpr*>(e)->inner); break;
        case NodeKind::CallExpr:
        {
          auto* call = static_cast<CallExpr*>(e);
          cWalkExpr(call->callee);
          cWalkExprs(call->args);
          Expr* callee = call->callee;
          while (callee->kind == NodeKind::ParenExpr)
            callee = static_cast<ParenExpr*>(callee)->inner;
          if (callee->kind == NodeKind::NameExpr && !static_cast<NameExpr*>(callee)->resolved &&
              static_cast<NameExpr*>(callee)->name.parts.size() == 1 && static_cast<NameExpr*>(callee)->name.parts[0].symbol != UINT32_MAX)
            if (const std::map<int, std::string>* parameters = cLibraryPointerParameters(symbols.name(static_cast<NameExpr*>(callee)->name.parts[0].symbol)))
              for (const auto& [index, spelled] : *parameters)
                if (size_t(index) < call->args.size() && cIsVoidPointer(cTypeOf(call->args[index])))
                  cCast(call->args[index], spelled);
          if (callee->kind == NodeKind::NameExpr && static_cast<NameExpr*>(callee)->resolved &&
              static_cast<NameExpr*>(callee)->resolved->kind == NodeKind::FunctionDecl)
          {
            auto* f = static_cast<FunctionDecl*>(static_cast<NameExpr*>(callee)->resolved);
            for (size_t a = 0; a < call->args.size() && a < f->params.size(); ++a)
              if (f->params[a]->kind == NodeKind::ParamDecl)
                cConvert(call->args[a], static_cast<ParamDecl*>(f->params[a])->type);
          }
          break;
        }
        case NodeKind::IndexExpr:
          cWalkExpr(static_cast<IndexExpr*>(e)->base);
          cWalkExprs(static_cast<IndexExpr*>(e)->args);
          break;
        case NodeKind::MemberExpr: cWalkExpr(static_cast<MemberExpr*>(e)->base); break;
        case NodeKind::UnaryExpr: cWalkExpr(static_cast<UnaryExpr*>(e)->operand); break;
        case NodeKind::BinaryExpr:
        {
          auto* b = static_cast<BinaryExpr*>(e);
          cWalkExpr(b->lhs);
          cWalkExpr(b->rhs);
          if (!b->isKeyword && b->op == uint32_t(P::Assign))
            cConvert(b->rhs, cTypeOf(b->lhs));
          break;
        }
        case NodeKind::ConditionalExpr:
        {
          auto* c = static_cast<ConditionalExpr*>(e);
          cWalkExpr(c->cond);
          cWalkExpr(c->then);
          cWalkExpr(c->else_);
          break;
        }
        case NodeKind::CastExpr:
        {
          auto* cast = static_cast<CastExpr*>(e);
          cWalkExpr(cast->operand);
          break;
        }
        case NodeKind::SizeofExpr: cWalkExpr(static_cast<SizeofExpr*>(e)->operand); break;
        case NodeKind::BraceInitExpr: cWalkExprs(static_cast<BraceInitExpr*>(e)->args); break;
        default: break;
      }
    }
    void cWalkVariable(Decl* d)
    {
      if (d->kind != NodeKind::VariableDecl)
        return;
      auto* v = static_cast<VariableDecl*>(d);
      cWalkExpr(v->init);
      if (!v->bracedInit)
        cConvert(v->init, v->type);
    }
    void cWalkStmt(Stmt* s, FunctionDecl* function)
    {
      if (!s)
        return;
      auto walkNode = [&](Node* n)
      {
        if (!n)
          return;
        if (n->kind == NodeKind::DeclStmt)
          cWalkStmt(static_cast<Stmt*>(n), function);
        else if (n->kind == NodeKind::VariableDecl)
          cWalkVariable(static_cast<Decl*>(n));
        else
          cWalkExpr(static_cast<Expr*>(n));
      };
      switch (s->kind)
      {
        case NodeKind::BlockStmt:
          for (Stmt* inner : static_cast<BlockStmt*>(s)->stmts)
            cWalkStmt(inner, function);
          break;
        case NodeKind::ExprStmt: cWalkExpr(static_cast<ExprStmt*>(s)->expr); break;
        case NodeKind::DeclStmt:
          for (Decl* d : static_cast<DeclStmt*>(s)->decls)
            cWalkVariable(d);
          break;
        case NodeKind::IfStmt:
        {
          auto* i = static_cast<IfStmt*>(s);
          cWalkStmt(i->init, function);
          walkNode(i->cond);
          cWalkStmt(i->then, function);
          cWalkStmt(i->else_, function);
          break;
        }
        case NodeKind::ForStmt:
        {
          auto* f = static_cast<ForStmt*>(s);
          cWalkStmt(f->init, function);
          walkNode(f->cond);
          cWalkExpr(f->step);
          cWalkStmt(f->body, function);
          break;
        }
        case NodeKind::WhileStmt:
          walkNode(static_cast<WhileStmt*>(s)->cond);
          cWalkStmt(static_cast<WhileStmt*>(s)->body, function);
          break;
        case NodeKind::DoStmt:
          cWalkStmt(static_cast<DoStmt*>(s)->body, function);
          cWalkExpr(static_cast<DoStmt*>(s)->cond);
          break;
        case NodeKind::SwitchStmt:
        {
          auto* sw = static_cast<SwitchStmt*>(s);
          cWalkStmt(sw->init, function);
          walkNode(sw->cond);
          cWalkStmt(sw->body, function);
          break;
        }
        case NodeKind::ReturnStmt:
        {
          Expr* value = static_cast<ReturnStmt*>(s)->value;
          cWalkExpr(value);
          if (function)
            cConvert(value, function->returnType);
          break;
        }
        default: break;
      }
    }
    void convertC(Span<Decl*> decls)
    {
      if (cInterfaceText)
      {
        // The header imports itself: its interface is the C import's, one entity for the program
        // (the game imports fontstash.h; its FONSparams, FONSflags and prototypes were declared twice).
        importC(in.path.filename().string());
        counts.add("C headers given alone that stay C imports: importing themselves");
      }
      for (Decl* d : decls)
      {
        if (cInterfaceText && !(d->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(d)->body) && d->token < toks.size())
        {
          const Token& name = toks[d->token];
          if (name.offset + name.length <= cInterfaceText->size() &&
              cInterfaceText->compare(name.offset, name.length, text.substr(name.offset, name.length)) == 0)
          {
            // The whole declaration, to its `;`: `typedef struct { ... } stbtt__buf;`'s name and the
            // `;` after an enum's braces too.
            uint32_t start = declStart(d), k = d->token;
            while (k > 0 && toks[k - 1].offset >= start)
              --k;
            int depth = 0;
            for (; k < toks.size(); ++k)
            {
              if (toks[k].is(P::LBrace))
                ++depth;
              else if (toks[k].is(P::RBrace))
                --depth;
              else if (toks[k].is(P::Semi) && depth == 0)
                break;
            }
            uint32_t end = k < toks.size() ? endOffset(k) : endOffset(toks.size() - 1);
            remove(start, lineEndAfter(end) - start);
            counts.add("C declarations the header's import gives removed");
            continue;
          }
        }
        cMakePrivate(d);
        if (d->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(d)->body)
        {
          cWalkStmt(static_cast<FunctionDecl*>(d)->body, static_cast<FunctionDecl*>(d));
          // A function C code calls by its name (vorbis calls ogg's): C linkage, as every function
          // a C file defines without static has. (Its own flags may say C already: the resolver
          // gives it the linkage of the header's extern "C" declaration, which kpp imports.)
          bool written = d->token >= 2 && toks[d->token - 1].getKind() == TokenKind::String && toks[d->token - 2].is(Kw::extern_);
          // A function that takes or returns a struct by value, which no interface header of the
          // library declares (cover.c's COVER_computeEpochs): the converted files are its only
          // callers, and it keeps kpp's linkage (kpp's C-callable functions pass no struct values).
          auto byValue = [&](Type* t)
          {
            t = cWithoutQualifiers(cUnaliased(t));
            return t && t->kind == NodeKind::NamedType && static_cast<NamedType*>(t)->resolved &&
                   static_cast<NamedType*>(t)->resolved->kind == NodeKind::ClassDecl;
          };
          auto* f = static_cast<FunctionDecl*>(d);
          bool structValue = byValue(f->returnType);
          for (Decl* param : f->params)
            structValue = structValue || (param->kind == NodeKind::ParamDecl && byValue(static_cast<ParamDecl*>(param)->type));
          if (structValue && !f->name.empty() && f->name.last().symbol != UINT32_MAX &&
              !(cInterfaceNames && cInterfaceNames->count(std::string(symbols.name(f->name.last().symbol)))))
          {
            counts.add("C functions passing structs by value the interface does not declare kept kpp's linkage");
            written = true;
          }
          // A function whose declaration names one of the library's private names (private-names.txt):
          // its C view, which the game imports, has the program's type of that name, another one
          // (stb_truetype's stbtt_PackFontRangesGatherRects takes its fallback's stbrp_rect, the
          // imported fontstash.h stb_rect_pack's), which C allows only across translation units: it
          // keeps kpp's linkage.
          if (cPrivate && cPrivate->count(cDeclName(d)) && !written)
          {
            counts.add("C functions kept to their file kept kpp's linkage");
            written = true;
          }
          if (cPrivateNames && !cPrivateNames->empty() && !written)
          {
            uint32_t k = d->token;
            for (uint32_t start = declStart(d); k > 0 && toks[k - 1].offset >= start;)
              --k;
            for (; k < toks.size() && !toks[k].is(P::LBrace); ++k)
              if (toks[k].getKind() == TokenKind::Identifier && cPrivateNames->count(std::string(spelling(k))))
              {
                counts.add("C functions taking the library's private types kept kpp's linkage");
                written = true;
                break;
              }
          }
          // A C++ overload of an interface function (fontstash.h, which the game compiles as C++, defines
          // a fonsAddFontSdf of three parameters besides the interface's of four): C++'s linkage there,
          // kpp's here, as C has one function of a name.
          if (!f->name.empty() && f->name.last().symbol != UINT32_MAX && cInterfaceNames)
            if (auto declared = cInterfaceNames->find(std::string(symbols.name(f->name.last().symbol)));
                declared != cInterfaceNames->end() && !declared->second.count(f->params.size()) && !written)
            {
              counts.add("C++ overloads of the interface's functions kept kpp's linkage");
              written = true;
            }
          if (!(d->flags & DF_Static) && !written && toks[d->token].is(Kw::extern_))
          {
            cReplace(toks[d->token].offset, toks[d->token].length, "extern \"C\""); // (`extern int f(...) {...}`, minizip's)
            counts.add("functions given C linkage (C)");
          }
          else if (!(d->flags & DF_Static) && !written)
          {
            cInsertBefore(toks[d->token].offset, "extern \"C\" ");
            counts.add("functions given C linkage (C)");
          }
        }
        else if (d->kind == NodeKind::VariableDecl)
        {
          cWalkVariable(d);
          cVariableLinkage(static_cast<VariableDecl*>(d));
        }
        else if (d->kind == NodeKind::NamespaceDecl)
          convertC(static_cast<NamespaceDecl*>(d)->decls);
      }
    }
    // A variable a C file defines without static (stb_image_write's stbi_write_png_compression_level,
    // which the game sets through the C header): C linkage, as functions get. A definition without
    // an initializer (C's tentative one, zero) gets `= {}`, since `extern "C" int x;` declares only.
    // (Declarations, `extern int x;`, keep theirs: they name the definition either way.)
    void cVariableLinkage(VariableDecl* v)
    {
      if (v->flags & (DF_Static | DF_Extern | DF_ExternC | DF_Typedef) || v->name.empty() || v->structuredBindings.size() ||
          (v->token >= 2 && toks[v->token - 1].getKind() == TokenKind::String && toks[v->token - 2].is(Kw::extern_)))
        return;
      uint32_t end = v->token;
      int depth = 0;
      bool several = false;
      for (; end < toks.size(); ++end)
      {
        if (toks[end].is(P::LParen) || toks[end].is(P::LBrace) || toks[end].is(P::LBracket))
          ++depth;
        else if (toks[end].is(P::RParen) || toks[end].is(P::RBrace) || toks[end].is(P::RBracket))
          --depth;
        else if (depth == 0 && toks[end].is(P::Comma))
          several = true;
        else if (depth == 0 && toks[end].is(P::Semi))
          break;
      }
      if (end >= toks.size())
        return;
      if (several)
      {
        // (C code naming one of them fails to link: kpp's names differ)
        counts.add("C variables of several declarators kept kpp's linkage");
        return;
      }
      cInsertBefore(toks[v->token].offset, "extern \"C\" ");
      if (!v->init && !v->bracedInit)
        insert(toks[end].offset, " = {}");
      counts.add("variables given C linkage (C)");
    }

