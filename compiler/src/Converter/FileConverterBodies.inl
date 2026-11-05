// FileConverterBodies.inl: FileConverter's members: function bodies: statements and expressions, edits applied.
// Part of convert.cpp, included in struct FileConverter's body in Converter/FileConverter.hpp (a file of its own since 2026-11-05, moved without a change).

    static bool terminates(Stmt* s)
    {
      if (!s)
        return false;
      switch (s->kind)
      {
        case NodeKind::ReturnStmt: case NodeKind::BreakStmt: case NodeKind::ContinueStmt: case NodeKind::GotoStmt:
        case NodeKind::FallthroughStmt:
          return true;
        case NodeKind::ExprStmt:
        {
          Expr* e = static_cast<ExprStmt*>(s)->expr;
          return e && e->kind == NodeKind::ThrowExpr;
        }
        case NodeKind::BlockStmt:
        {
          auto* b = static_cast<BlockStmt*>(s);
          return !b->stmts.empty() && terminates(b->stmts[b->stmts.size() - 1]);
        }
        case NodeKind::IfStmt:
        {
          auto* i = static_cast<IfStmt*>(s);
          return i->else_ && terminates(i->then) && terminates(i->else_);
        }
        default: return false;
      }
    }

    void walkSwitchBody(Stmt* body, Decl* owner)
    {
      if (!body || body->kind != NodeKind::BlockStmt)
      {
        walkStmt(body, owner);
        return;
      }
      auto* b = static_cast<BlockStmt*>(body);
      Stmt* last = nullptr; // last statement since the previous label
      for (Stmt* s : b->stmts)
      {
        if (s->kind == NodeKind::CaseStmt || s->kind == NodeKind::DefaultStmt)
        {
          if (last && !terminates(last))
          {
            // Indent like the label.
            uint32_t at = toks[s->token].offset;
            uint32_t lineStart = at;
            while (lineStart > 0 && text[lineStart - 1] != '\n')
              --lineStart;
            insert(at, "fallthrough;\n" + std::string(text.substr(lineStart, at - lineStart)));
            counts.add("fallthrough added");
          }
          last = nullptr;
          if (s->kind == NodeKind::CaseStmt)
            walkExpr(static_cast<CaseStmt*>(s)->value, owner);
          continue;
        }
        walkStmt(s, owner);
        last = s;
      }
    }

    void walkStmt(Stmt* s, Decl* owner)
    {
      if (!s)
        return;
      switch (s->kind)
      {
        case NodeKind::BlockStmt:
          for (Stmt* inner : static_cast<BlockStmt*>(s)->stmts)
            walkStmt(inner, owner);
          break;
        case NodeKind::ExprStmt: walkExpr(static_cast<ExprStmt*>(s)->expr, owner); break;
        case NodeKind::DeclStmt:
          for (Decl* d : static_cast<DeclStmt*>(s)->decls)
            if (d->kind == NodeKind::VariableDecl)
              walkExpr(static_cast<VariableDecl*>(d)->init, owner);
            else if (d->kind == NodeKind::UsingAliasDecl)
              recordVariantDeclaration(d, false); // a block's `using V = std::variant<...>;`
            else if (d->kind == NodeKind::ClassDecl)
              structAsClass(static_cast<ClassDecl*>(d)); // a local struct (MainLoop's `struct : ProgressObserver { ... }`)
          break;
        case NodeKind::IfStmt:
        {
          auto* i = static_cast<IfStmt*>(s);
          walkStmt(i->init, owner);
          walkNode(i->cond, owner);
          walkStmt(i->then, owner);
          walkStmt(i->else_, owner);
          break;
        }
        case NodeKind::AssertStmt: walkExpr(static_cast<AssertStmt*>(s)->cond, owner); break;
        case NodeKind::ForStmt:
        {
          auto* f = static_cast<ForStmt*>(s);
          walkStmt(f->init, owner);
          walkNode(f->cond, owner);
          walkExpr(f->step, owner);
          walkStmt(f->body, owner);
          break;
        }
        case NodeKind::RangeForStmt:
        {
          auto* f = static_cast<RangeForStmt*>(s);
          rangeForRanges[f->var] = f->range;
          walkExpr(f->range, owner);
          walkStmt(f->body, owner);
          break;
        }
        case NodeKind::WhileStmt:
          walkNode(static_cast<WhileStmt*>(s)->cond, owner);
          walkStmt(static_cast<WhileStmt*>(s)->body, owner);
          break;
        case NodeKind::DoStmt:
          walkStmt(static_cast<DoStmt*>(s)->body, owner);
          walkExpr(static_cast<DoStmt*>(s)->cond, owner);
          break;
        case NodeKind::SwitchStmt:
        {
          auto* sw = static_cast<SwitchStmt*>(s);
          walkStmt(sw->init, owner);
          walkNode(sw->cond, owner);
          walkSwitchBody(sw->body, owner);
          break;
        }
        case NodeKind::CaseStmt: walkExpr(static_cast<CaseStmt*>(s)->value, owner); break;
        case NodeKind::ReturnStmt: walkExpr(static_cast<ReturnStmt*>(s)->value, owner); break;
        case NodeKind::TryStmt:
        {
          auto* t = static_cast<TryStmt*>(s);
          walkStmt(t->body, owner);
          for (const TryStmt::Handler& h : t->handlers)
            walkStmt(h.body, owner);
          break;
        }
        default: break;
      }
    }

    void walkNode(Node* n, Decl* owner)
    {
      if (!n)
        return;
      if (n->kind == NodeKind::DeclStmt || n->kind == NodeKind::VariableDecl)
      {
        if (n->kind == NodeKind::VariableDecl)
          walkExpr(static_cast<VariableDecl*>(n)->init, owner);
        else
          walkStmt(static_cast<Stmt*>(n), owner);
        return;
      }
      walkExpr(static_cast<Expr*>(n), owner);
    }

    // The first token of an expression: a postfix node's own token is its operator (`.`, `(`, `[`).
    static uint32_t firstTokenOf(Expr* e)
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
        else
          return e->token;
      }
    }
    void walkExprs(Span<Expr*> es, Decl* owner)
    {
      for (Expr* e : es)
        walkExpr(e, owner);
    }

    void walkExpr(Expr* e, Decl* owner)
    {
      if (!e)
        return;
      switch (e->kind)
      {
        case NodeKind::NameExpr:
        {
          auto* n = static_cast<NameExpr*>(e);
          Decl* r = n->resolved;
          if (!r && n->name.parts.size() == 1 && !n->name.global && n->name.parts[0].symbol != UINT32_MAX &&
              symbols.name(n->name.parts[0].symbol) == "_" && nameUnderscoreUse(n->token))
            break;
          if (owner && n->name.parts.size() == 1 && !n->name.global && n->name.parts[0].special == 0)
          {
            bool member = false;
            if (r)
              member = r->parent && r->parent->kind == NodeKind::ClassDecl &&
                       (r->kind == NodeKind::FunctionDecl || (r->kind == NodeKind::VariableDecl && !(r->flags & DF_Typedef) && isDataMember(r))) &&
                       !(r->flags & DF_Static);
            else
              member = memberNames(owner).count(n->name.parts[0].symbol) != 0; // unresolved, but spelled like a member
            // (A fixture's test body, now parsed as its helper's Run(): the fixture handling writes
            // `this->` there, by text.)
            if (member && !fixtureBodyAt(n->token))
            {
              insert(toks[n->token].offset, "this->");
              counts.add("this-> added");
            }
          }
          break;
        }
        case NodeKind::CallExpr:
        {
          auto* call = static_cast<CallExpr*>(e);
          if (variants && call->callee->kind == NodeKind::NameExpr && !call->args.empty())
          {
            // std::get<T>(x), std::visit(f, x)...: which variant x is declared as, for the rewrite in run().
            const QualifiedName& n = static_cast<NameExpr*>(call->callee)->name;
            if (n.parts.size() == 2 && n.parts[0].symbol != UINT32_MAX && n.parts[1].symbol != UINT32_MAX && symbols.name(n.parts[0].symbol) == "std")
            {
              std::string_view f = symbols.name(n.parts[1].symbol);
              Expr* variantArg = f == "visit" && call->args.size() >= 2 ? call->args[1]
                               : (f == "get" || f == "holds_alternative" || f == "get_if") ? call->args[0]
                                                                                            : nullptr;
              if (const VariantInfo* info = variantOfExpr(variantArg))
                callVariant[call->callee->token] = info;
            }
          }
          for (Expr* arg : call->args)
            if (isGuidVariable(arg))
            {
              // `object->SetPrivateData(WKPDID_D3DDebugObjectName, ...)`, `FC->CanConvert(pixelFormat,
              // convertGUID, ...)`: by address, as the class and interface ids are below.
              insert(toks[arg->token].offset, "&");
              counts.add("GUIDs passed by address");
            }
          // `it.operator*()` and `it.operator->()` on an iterator: a std::vector's is a class in the
          // game's library and a pointer in kpp's, which has no member call for them (Agui's
          // visible_iterator), so they are written `(*it)` and `toAddress(it)`, which C++ gives the
          // same meaning for an iterator. Only where the declarations say a std iterator: any other
          // keeps the member call (PrototypeIDWithQualityIterator's `this->idIterator.operator->()`
          // calls PrototypeIDIterator's non-const one, which toAddress's const parameter would
          // not). The opening goes in at the base's first token before the
          // base's own edits there (`this->`); a postfix node's token is its operator, the `.`.
          if (call->args.empty() && call->callee->kind == NodeKind::MemberExpr && static_cast<MemberExpr*>(call->callee)->member.parts.size() == 1)
          {
            auto* m = static_cast<MemberExpr*>(call->callee);
            const NamePart& name = m->member.parts[0];
            bool star = name.opToken == uint32_t(P::Star), arrow = name.opToken == uint32_t(P::Arrow);
            uint32_t dot = m->token;
            if (m->op == P::Dot && name.special == 2 && (star || arrow) && dot + 4 < toks.size() &&
                toks[dot + 3].is(P::LParen) && toks[dot + 4].is(P::RParen) && isStdIteratorType(typeOfExpr(m->base, 0).type, owner))
            {
              insert(toks[firstTokenOf(m->base)].offset, star ? "(*" : "toAddress(");
              replace(toks[dot].offset, toks[dot + 4].offset + 1 - toks[dot].offset, ")");
              counts.add(star ? "explicit operator*() calls written *x" : "explicit operator->() calls written toAddress(x)");
            }
          }
          // (A printf-style call's 64-bit arguments: widenFormatArguments, by the format; its
          // 64-bit arguments read as 32 bits, by the declarations:)
          stdToString(call);
          if (!rewritePrintfCall(call))
            narrowFormatArguments(call);
          if (variants && call->callee->kind == NodeKind::NameExpr)
            if (auto v = variants->byDecl.find(static_cast<NameExpr*>(call->callee)->resolved); v != variants->byDecl.end())
              variantConstruction(variants->all[v->second], call->callee->token, call->args);
          walkExpr(call->callee, owner);
          walkExprs(call->args, owner);
          break;
        }
        case NodeKind::IndexExpr:
          walkExpr(static_cast<IndexExpr*>(e)->base, owner);
          walkExprs(static_cast<IndexExpr*>(e)->args, owner);
          break;
        case NodeKind::MemberExpr: walkExpr(static_cast<MemberExpr*>(e)->base, owner); break;
        case NodeKind::UnaryExpr: walkExpr(static_cast<UnaryExpr*>(e)->operand, owner); break;
        case NodeKind::BinaryExpr:
        {
          auto* binary = static_cast<BinaryExpr*>(e);
          if (P(binary->op) == P::Shl)
            sizeAsSizeT(binary->rhs);
          // C++'s `text += x` on a std::string: kpp's String appends with `<<` (design.md 14), the right
          // side in parentheses where `<<` would bind otherwise (`text += ok ? "a" : "b"`), and an
          // integer that C++ appends as one char written char(x) (PropertyTreeJson's UTF-8 encoder:
          // `buffer += (unsigned char)(0xc0 | (c >> 6))`).
          if (!in.c && !binary->isKeyword && P(binary->op) == P::PlusAssign && binary->rhs &&
              isStdStringExpr(binary->lhs))
          {
            // (the operator's token: the first `+=` outside brackets after the left side's first token,
            // past an inner one in `(key += '_') += name`)
            uint32_t op = firstTokenOf(binary->lhs);
            for (int depth = 0; op < toks.size() && !(depth == 0 && toks[op].is(P::PlusAssign)); ++op)
              if (toks[op].is(P::LParen) || toks[op].is(P::LBracket) || toks[op].is(P::LBrace))
                ++depth;
              else if (toks[op].is(P::RParen) || toks[op].is(P::RBracket) || toks[op].is(P::RBrace))
                --depth;
            uint32_t rhsFirst = op + 1;
            if (op < toks.size() && toks[op].is(P::PlusAssign))
            {
              // the end of the right side: the `;`, `)` or `,` that ends the expression
              uint32_t end = rhsFirst;
              for (int depth = 0; end < toks.size(); ++end)
              {
                if (toks[end].is(P::LParen) || toks[end].is(P::LBracket) || toks[end].is(P::LBrace))
                  ++depth;
                else if (toks[end].is(P::RParen) || toks[end].is(P::RBracket) || toks[end].is(P::RBrace))
                {
                  if (depth == 0)
                    break;
                  --depth;
                }
                else if (depth == 0 && (toks[end].is(P::Semi) || toks[end].is(P::Comma)))
                  break;
              }
              Expr* rhs = binary->rhs;
              while (rhs->kind == NodeKind::ParenExpr)
                rhs = static_cast<ParenExpr*>(rhs)->inner;
              Type* rhsType = plainTypeOf(rhs);
              bool asChar = isNonCharInteger(rhsType);
              // C text that is no literal or array: a char pointer, or a conditional of literals (`ok ?
              // "yes" : "no"`, whose type the parse may not know), is no piece of a text (design.md 14):
              // StringView(...)
              auto isCharPointer = [&](Type* t)
              {
                while (t && t->kind == NodeKind::ConstType)
                  t = static_cast<ConstType*>(t)->inner;
                if (!t || t->kind != NodeKind::PointerType)
                  return false;
                Type* pointee = static_cast<PointerType*>(t)->pointee;
                while (pointee && pointee->kind == NodeKind::ConstType)
                  pointee = static_cast<ConstType*>(pointee)->inner;
                return pointee && pointee->kind == NodeKind::BuiltinType && static_cast<BuiltinType*>(pointee)->keyword == Kw::char_ &&
                       !static_cast<BuiltinType*>(pointee)->isUnsigned && !static_cast<BuiltinType*>(pointee)->isSigned;
              };
              auto isStringLiteral = [&](Expr* x)
              {
                while (x && x->kind == NodeKind::ParenExpr)
                  x = static_cast<ParenExpr*>(x)->inner;
                return x && x->kind == NodeKind::LiteralExpr && static_cast<LiteralExpr*>(x)->literalKind == TokenKind::String;
              };
              bool cText = !asChar && !isStringLiteral(rhs) &&
                           (isCharPointer(rhsType) ||
                            // (a std::string's c_str(), whose type the parse does not know without the library)
                            (rhs->kind == NodeKind::CallExpr && static_cast<CallExpr*>(rhs)->args.empty() &&
                             static_cast<CallExpr*>(rhs)->callee->kind == NodeKind::MemberExpr &&
                             static_cast<MemberExpr*>(static_cast<CallExpr*>(rhs)->callee)->member.parts.size() == 1 &&
                             static_cast<MemberExpr*>(static_cast<CallExpr*>(rhs)->callee)->member.parts[0].symbol != UINT32_MAX &&
                             (symbols.name(static_cast<MemberExpr*>(static_cast<CallExpr*>(rhs)->callee)->member.parts[0].symbol) == "c_str" ||
                              symbols.name(static_cast<MemberExpr*>(static_cast<CallExpr*>(rhs)->callee)->member.parts[0].symbol) == "what")) ||
                            (rhs->kind == NodeKind::ConditionalExpr && static_cast<ConditionalExpr*>(rhs)->then &&
                             (isStringLiteral(static_cast<ConditionalExpr*>(rhs)->then) || isStringLiteral(static_cast<ConditionalExpr*>(rhs)->else_))));
              bool loose = binary->rhs->kind == NodeKind::ConditionalExpr ||
                           (binary->rhs->kind == NodeKind::BinaryExpr && !static_cast<BinaryExpr*>(binary->rhs)->isKeyword &&
                            P(static_cast<BinaryExpr*>(binary->rhs)->op) != P::Plus && P(static_cast<BinaryExpr*>(binary->rhs)->op) != P::Minus &&
                            P(static_cast<BinaryExpr*>(binary->rhs)->op) != P::Star && P(static_cast<BinaryExpr*>(binary->rhs)->op) != P::Slash &&
                            P(static_cast<BinaryExpr*>(binary->rhs)->op) != P::Percent);
              replace(toks[op].offset, toks[op].length, "<<");
              if (asChar || cText || loose)
              {
                insert(toks[rhsFirst].offset, asChar ? "char(" : cText ? "StringView(" : "(");
                insert(toks[end].offset, ")");
              }
              counts.add(asChar  ? "std::string += of an integer written << char(x)"
                         : cText ? "std::string += of C text written << StringView(x)"
                                 : "std::string += written <<");
            }
          }
          walkExpr(binary->lhs, owner);
          walkExpr(binary->rhs, owner);
          break;
        }
        case NodeKind::ConditionalExpr:
          walkExpr(static_cast<ConditionalExpr*>(e)->cond, owner);
          walkExpr(static_cast<ConditionalExpr*>(e)->then, owner);
          walkExpr(static_cast<ConditionalExpr*>(e)->else_, owner);
          break;
        case NodeKind::CastExpr: walkExpr(static_cast<CastExpr*>(e)->operand, owner); break;
        case NodeKind::NamedCastExpr: walkExpr(static_cast<NamedCastExpr*>(e)->operand, owner); break;
        case NodeKind::SizeofExpr: walkExpr(static_cast<SizeofExpr*>(e)->operand, owner); break;
        case NodeKind::NameofExpr: walkExpr(static_cast<NameofExpr*>(e)->operand, owner); break;
        case NodeKind::NoexceptExpr: walkExpr(static_cast<NoexceptExpr*>(e)->operand, owner); break;
        case NodeKind::NewExpr:
          walkExprs(static_cast<NewExpr*>(e)->placement, owner);
          walkExprs(static_cast<NewExpr*>(e)->args, owner);
          break;
        case NodeKind::DeleteExpr: walkExpr(static_cast<DeleteExpr*>(e)->operand, owner); break;
        case NodeKind::LambdaExpr:
        {
          auto* l = static_cast<LambdaExpr*>(e);
          for (const LambdaCapture& c : l->captures)
            walkExpr(c.init, owner);
          walkStmt(l->body, owner);
          break;
        }
        case NodeKind::BraceInitExpr: walkExprs(static_cast<BraceInitExpr*>(e)->args, owner); break;
        case NodeKind::ThrowExpr: walkExpr(static_cast<ThrowExpr*>(e)->operand, owner); break;
        case NodeKind::FunctionalCastExpr:
        {
          auto* cast = static_cast<FunctionalCastExpr*>(e);
          if (const VariantInfo* info = variants ? variantOfType(cast->type) : nullptr)
            variantConstruction(*info, cast->type->token, cast->args);
          walkExprs(cast->args, owner);
          break;
        }
        case NodeKind::FoldExpr:
          walkExpr(static_cast<FoldExpr*>(e)->lhs, owner);
          walkExpr(static_cast<FoldExpr*>(e)->rhs, owner);
          break;
        case NodeKind::ParenExpr: walkExpr(static_cast<ParenExpr*>(e)->inner, owner); break;
        default: break;
      }
    }

    // --- putting it together ---

    // Definitions of overridden functions the parser did not see as functions, found by text:
    // `... name(` at the start of a line with a `{` before the next `;`, through the body's end.
    void removeUnparsedDefinitions()
    {
      if (!overrides)
        return;
      for (const auto& [qualified, count] : overrides->functionCounts)
      {
        if (count != 1 || replacedFunctions.count(qualified))
          continue;
        std::string name = qualified.substr(qualified.rfind(':') == std::string::npos ? 0 : qualified.rfind(':') + 1);
        for (size_t p = text.find(name); p != std::string::npos; p = text.find(name, p + 1))
        {
          if (p > 0 && (isalnum((unsigned char)text[p - 1]) || text[p - 1] == '_' || text[p - 1] == ':' || text[p - 1] == '.'))
            continue;
          size_t q = p + name.size();
          while (q < text.size() && text[q] == ' ')
            ++q;
          if (q >= text.size() || text[q] != '(')
            continue;
          size_t lineStart = text.rfind('\n', p);
          lineStart = lineStart == std::string::npos ? 0 : lineStart + 1;
          std::string_view head = text.substr(lineStart, p - lineStart);
          if (head.find_first_not_of(" ") == std::string_view::npos || head.find(';') != std::string_view::npos || head.find("//") != std::string_view::npos)
            continue; // not a declaration line
          size_t brace = text.find('{', q), semi = text.find(';', q);
          if (brace == std::string::npos || (semi != std::string::npos && semi < brace))
            continue; // a prototype
          int depth = 0;
          size_t end = brace;
          for (; end < text.size(); ++end)
          {
            if (text[end] == '{')
              ++depth;
            else if (text[end] == '}' && --depth == 0)
              break;
          }
          if (end >= text.size())
            break;
          ++end;
          while (end < text.size() && (text[end] == ' ' || text[end] == '\r'))
            ++end;
          if (end < text.size() && text[end] == '\n')
            ++end;
          remove(uint32_t(lineStart), uint32_t(end - lineStart));
          counts.add("replaced by the override file (by text)");
          replacedFunctions.insert(qualified);
          break;
        }
      }
      // Variables likewise (a SUITE's `const char* address = "...";`): `... name =` (or `{`, `;`)
      // after a type on the line, through the `;` that ends it.
      for (const std::string& qualified : overrides->variables)
      {
        if (replacedVariables.count(qualified))
          continue;
        std::string name = qualified.substr(qualified.rfind(':') == std::string::npos ? 0 : qualified.rfind(':') + 1);
        for (size_t p = text.find(name); p != std::string::npos; p = text.find(name, p + 1))
        {
          if (p > 0 && (isalnum((unsigned char)text[p - 1]) || text[p - 1] == '_' || text[p - 1] == ':' || text[p - 1] == '.' || text[p - 1] == '>'))
            continue;
          size_t q = p + name.size();
          while (q < text.size() && text[q] == ' ')
            ++q;
          if (q >= text.size() || !(text[q] == '{' || text[q] == ';' || (text[q] == '=' && (q + 1 >= text.size() || text[q + 1] != '='))))
            continue;
          size_t lineStart = text.rfind('\n', p);
          lineStart = lineStart == std::string::npos ? 0 : lineStart + 1;
          std::string_view head = text.substr(lineStart, p - lineStart);
          if (head.find_first_not_of(" ") == std::string_view::npos || head.find_first_of(";(=,") != std::string_view::npos || head.find("//") != std::string_view::npos ||
              head.find("return") != std::string_view::npos)
            continue; // not a declaration line
          int depth = 0;
          size_t end = q;
          for (; end < text.size(); ++end)
          {
            if (text[end] == '{' || text[end] == '(')
              ++depth;
            else if (text[end] == '}' || text[end] == ')')
              --depth;
            else if (text[end] == ';' && depth == 0)
              break;
          }
          if (end >= text.size())
            break;
          ++end;
          while (end < text.size() && (text[end] == ' ' || text[end] == '\r'))
            ++end;
          if (end < text.size() && text[end] == '\n')
            ++end;
          remove(uint32_t(lineStart), uint32_t(end - lineStart));
          counts.add("replaced by the override file (by text)");
          replacedVariables.insert(qualified);
          break;
        }
      }
    }

    // text[begin, end) with the edits inside it applied; edits must be sorted as run() sorts them.
    // An insertion at end belongs to what follows, unless end is the end of the file.
    // The text [begin, end) with the edits inside it; an insertion at end goes with the next text,
    // or with this one when it is a macro's argument (keepEnd: what is inserted after its last token).
    std::string applyEdits(const std::vector<Edit>& sorted, uint32_t begin, uint32_t end, bool keepEnd = false) const
    {
      std::string out;
      uint32_t pos = begin;
      for (const Edit& e : sorted)
      {
        if (e.offset < begin || e.offset + e.length > end || (!keepEnd && e.length == 0 && e.offset == end && end < text.size()))
          continue;
        if (e.offset < pos)
        {
          // Inside an earlier removal or replacement: an insertion is dropped, a longer removal extends.
          if (e.length && e.offset + e.length > pos)
          {
            pos = e.offset + e.length;
            out += e.text;
          }
          continue;
        }
        out.append(text.substr(pos, e.offset - pos));
        out += e.text;
        pos = e.offset + e.length;
      }
      out.append(text.substr(std::min<size_t>(pos, end), end - std::min<size_t>(pos, end)));
      return out;
    }

    // What the configuration took out of the parsed text (blankForParse) gets its replacement:
    // `__try` is try, `__except (filter)` catch (...) (the filter is lost: a todo), the converter's
    // empty macros go.
    void convertBlanked()
    {
      if (!preprocessed)
        return;
      for (const Preprocessed::Blank& b : preprocessed->blanked)
      {
        replace(b.offset, b.length, b.replacement);
        if (b.replacement == "try")
          counts.add("SEH __try to try");
        else if (b.replacement == "catch (...)")
        {
          todo(b.offset, "SEH filter dropped: " + std::string(text.substr(b.offset, b.length)));
          counts.add("SEH __except to catch (...)");
        }
        else
          counts.add("macro use rewritten");
      }
    }

    // --- C's rules (ConvertInput::c, kppc --c-sources) ---
    // C converts a void* to any object pointer implicitly (`unsigned char* p = malloc(n);`,
    // `b->buffer = ret;`), kpp only with a cast: the converter writes the cast. The types come from
    // the declarations the converted files and their headers make; a C library function that
    // returns void* is known by name, as the converter does not read the C library's headers.

