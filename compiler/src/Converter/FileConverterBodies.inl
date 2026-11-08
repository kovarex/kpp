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
      // a label with nothing after it so far: in C++ it shares the next label's statements, in kpp it would run none
      Stmt* bareLabel = nullptr;
      // such labels and the label after each, given `fallthrough;` once the statements they share show its indentation
      std::vector<std::pair<Stmt*, Stmt*>> shared;
      for (Stmt* s : b->stmts)
      {
        // A label, or a macro's use that writes one (stb_image's `STBI__CASE(1,2) { ... } break;`, Agui's
        // `HANDLE_CASE(Type::X, member);`, Lua's vmcase): the case before it ends there.
        bool label = s->kind == NodeKind::CaseStmt || s->kind == NodeKind::DefaultStmt;
        bool macroLabel = !label && writesLabel(s);
        if (label || macroLabel)
        {
          if (bareLabel && !last)
            shared.push_back({bareLabel, s});
          bareLabel = label ? s : nullptr;
          removeTrailingBreak(last);
          moveFallthroughOutOfBlock(last);
          if (last && !terminates(last))
          {
            // Indented as the case's last statement (its line's), before the label (which keeps its own).
            uint32_t at = toks[s->token].offset;
            uint32_t lineStart = at;
            while (lineStart > 0 && text[lineStart - 1] != '\n')
              --lineStart;
            std::string labelIndent(text.substr(lineStart, at - lineStart)), indent;
            uint32_t lastLine = toks[last->token].offset;
            while (lastLine > 0 && text[lastLine - 1] != '\n')
              --lastLine;
            for (uint32_t k = lastLine; k < text.size() && (text[k] == ' ' || text[k] == '\t'); ++k)
              indent += text[k];
            std::string deeper = indent.size() > labelIndent.size() && indent.compare(0, labelIndent.size(), labelIndent) == 0
                                     ? indent.substr(labelIndent.size())
                                     : std::string();
            // (before a macro's use at its line's start: an insertion at the use is inside the use's replacement)
            uint32_t where = macroLabel ? lineStart : at;
            if (macroLabel && labelIndent.find_first_not_of(" \t") != std::string::npos)
              todo(at, "a case runs on into the one a macro writes after it on its line: write fallthrough; before the macro's use");
            else
            {
              insert(where, macroLabel ? labelIndent + deeper + "fallthrough;\n" : deeper + "fallthrough;\n" + labelIndent);
              counts.add("fallthrough added");
              // (after a macro's use, `vmcase(4, ...)`, whose expansion the parse does not see: withdrawn when its
              // expansion ends with the case's break, which dropTrailingBreaksInExpansion drops)
              fallthroughAfterUse[toks[last->token].offset] = where;
            }
          }
          last = nullptr;
          if (s->kind == NodeKind::CaseStmt)
            walkExpr(static_cast<CaseStmt*>(s)->value, owner);
          if (label)
            continue;
          // (the use itself: what it writes after its label is its case's, its arguments rewritten as any
          // statement's; it counts as no statement, so `STBI__CASE(1,2) break;` keeps a break that may be a loop's)
          walkStmt(s, owner);
          last = s;
          continue;
        }
        walkStmt(s, owner);
        if (s->kind != NodeKind::EmptyStmt) // (`default: { ...; break; };`: the block is its last statement)
        {
          last = s;
          bareLabel = nullptr;
          fallthroughAfterLabels(shared, s);
        }
      }
      fallthroughAfterLabels(shared, nullptr);
      removeTrailingBreak(last); // (the last group's)
    }

    // Whether the statement is a macro's use whose expansion starts with a label (`case`, `default`).
    bool writesLabel(Stmt* s)
    {
      if (!s || s->token + 1 >= toks.size() || toks[s->token].getKind() != TokenKind::Identifier || !toks[s->token + 1].is(P::LParen))
        return false;
      const LocalMacro* m = localMacroAt(std::string(spelling(s->token)), toks[s->token].offset);
      if (!m)
        return false;
      size_t start = m->body.find_first_not_of(" \t\r\n\\");
      size_t end = start;
      while (end != std::string::npos && end < m->body.size() && isIdentChar(m->body[end]))
        ++end;
      std::string first = start == std::string::npos ? std::string() : m->body.substr(start, end - start);
      return first == "case" || first == "default";
    }

    // No implicit fallthrough (design.md): every group of statements after a label ends with a break, so the
    // `break;` C++ ends one with goes, also as the last statement of a block that ends it, and also when it is the
    // group's only statement: `case A: break;` becomes `case A:`, which runs nothing (labels side by side share no
    // statements, kovarex 2026-11-08).
    void removeTrailingBreak(Stmt* last)
    {
      Stmt* end = last;
      while (end && end->kind == NodeKind::BlockStmt && !static_cast<BlockStmt*>(end)->stmts.empty())
        end = static_cast<BlockStmt*>(end)->stmts[static_cast<BlockStmt*>(end)->stmts.size() - 1];
      if (!end || end->kind != NodeKind::BreakStmt)
        return;
      uint32_t t = end->token;
      if (t + 1 >= toks.size() || !toks[t + 1].is(P::Semi) || text.substr(toks[t].offset, toks[t].length) != "break")
        return; // (not written as `break;` in this file)
      removeStatementText(toks[t].offset, endOffset(t + 1));
      counts.add("trailing break removed");
    }

    // A label directly followed by another (C++'s `case A: case B: build();`, comments between too) shares that
    // label's statements in C++; in kpp a label runs only what follows it (kovarex 2026-11-08), so it gets
    // `fallthrough;`: after its `:` when the next label is on its line (`case A: fallthrough; case B:`), else on a
    // line of its own after the label's, indented as the statements they share (`statement`, the first, if it
    // starts its line; else the label's indentation and two spaces).
    void fallthroughAfterLabels(std::vector<std::pair<Stmt*, Stmt*>>& labels, Stmt* statement)
    {
      auto lineStartOf = [&](uint32_t at)
      {
        while (at > 0 && text[at - 1] != '\n')
          --at;
        return at;
      };
      auto blank = [&](uint32_t from, uint32_t to) { return text.substr(from, to - from).find_first_not_of(" \t") == std::string_view::npos; };
      std::string statementIndent;
      if (statement)
      {
        uint32_t at = toks[statement->token].offset, lineStart = lineStartOf(at);
        if (blank(lineStart, at))
          statementIndent = text.substr(lineStart, at - lineStart);
      }
      for (auto [label, next] : labels)
      {
        uint32_t t = label->token + 1;
        for (int depth = 0, questions = 0; t < toks.size(); ++t)
          if (toks[t].is(P::LParen) || toks[t].is(P::LBracket) || toks[t].is(P::LBrace))
            ++depth;
          else if (toks[t].is(P::RParen) || toks[t].is(P::RBracket) || toks[t].is(P::RBrace))
            --depth;
          else if (depth == 0 && toks[t].is(P::Question))
            ++questions;
          else if (depth == 0 && toks[t].is(P::Colon) && questions-- == 0)
            break;
        if (t >= toks.size())
          continue;
        uint32_t colonEnd = endOffset(t);
        size_t lineEnd = text.find('\n', colonEnd);
        // (on the label's line also before a preprocessor line, `#if`/`#endif` around the next label, which the
        // conversion may drop with what is inserted at its start: trio's `case QUALIFIER_SIZE_T_UPPER:`)
        size_t nextLine = lineEnd == std::string_view::npos ? lineEnd : text.find_first_not_of(" \t", lineEnd + 1);
        if (lineEnd == std::string_view::npos || lineEnd >= toks[next->token].offset ||
            (nextLine != std::string_view::npos && text[nextLine] == '#'))
          insert(colonEnd, " fallthrough;");
        else
        {
          uint32_t labelLine = lineStartOf(toks[label->token].offset);
          std::string indent = statementIndent;
          if (indent.empty())
          {
            uint32_t k = labelLine;
            while (k < text.size() && (text[k] == ' ' || text[k] == '\t'))
              ++k;
            indent = std::string(text.substr(labelLine, k - labelLine)) + "  ";
          }
          insert(uint32_t(lineEnd + 1), indent + "fallthrough;\n");
        }
        counts.add("fallthrough added after a label sharing the next one's statements");
      }
      labels.clear();
    }

    // A case's `[[fallthrough]];` as the last statement of the block that ends the case (RepathData's `case
    // State::StopRenamed: { ...; [[fallthrough]]; }`): kpp's `fallthrough;` is the case's own last statement, so
    // it goes after the block's closing brace, on a line of its own indented as the brace.
    void moveFallthroughOutOfBlock(Stmt* last)
    {
      if (!last || last->kind != NodeKind::BlockStmt || !static_cast<BlockStmt*>(last)->endToken)
        return;
      uint32_t close = static_cast<BlockStmt*>(last)->endToken;
      Stmt* end = last;
      while (end && end->kind == NodeKind::BlockStmt && !static_cast<BlockStmt*>(end)->stmts.empty())
        end = static_cast<BlockStmt*>(end)->stmts[static_cast<BlockStmt*>(end)->stmts.size() - 1];
      if (!end || end->kind != NodeKind::FallthroughStmt || close >= toks.size() || !toks[close].is(P::RBrace))
        return;
      uint32_t t = end->token;
      uint32_t endToken = toks[t].is(P::LBracket) ? t + 5 : t + 1; // `[[fallthrough]];` or `fallthrough;`
      if (endToken >= toks.size() || !toks[endToken].is(P::Semi))
        return;
      removeStatementText(toks[t].offset, endOffset(endToken)); // (cancels walkStmt's rewrite of it, inside)
      uint32_t lineStart = toks[close].offset;
      while (lineStart > 0 && (text[lineStart - 1] == ' ' || text[lineStart - 1] == '\t'))
        --lineStart;
      insert(endOffset(close), "\n" + std::string(text.substr(lineStart, toks[close].offset - lineStart)) + "fallthrough;");
      counts.add("fallthrough moved out of the block ending its case");
    }

    // No implicit fallthrough in a macro's expansion (the parse sees the use, not the cases it writes: Lua's
    // `vmcase(l, b) case l: {b} break;`, an X-macro's `case Type::X: ...; break;`, DrawQueue's switch inside a
    // do/while): a `break;` goes when the text proves it ends a case: a label before it at its brace level with
    // only statements between (it follows a `;`, a `}`, the label's `:` or the `{` of a block right after it,
    // never `)`, `else` or `do`, whose statement it would be), and after it at that level a label, the closing
    // brace of a switch's body, or the end of the expansion where the use is followed by a label or its switch's
    // closing brace (`labelAfterUse`). Anything else keeps it: an error under the flag, never a break dropped from
    // a loop. A label right after another (`case A: case B:`) gets `fallthrough;`, as walkSwitchBody's do.
    // A case's block right after its label ends with the case's break too (InputAction's `case X: { ...; break; }`),
    // and the expansion's last `break` may take its `;` from the use (Agui's `HANDLE_CASE(...);`, whose body ends
    // `break`): `semicolonAfterUse` says the use is followed by one, `takesSemicolon` that the use's `;` goes too.
    // `endsCase`: set when the expansion's last statement was such a break, or is a jump (return, throw, continue,
    // goto, a break kept): the use ends its case, and no `fallthrough;` follows it.
    std::string dropTrailingBreaksInExpansion(const std::string& e, bool labelAfterUse, bool semicolonAfterUse = false, bool* endsCase = nullptr,
                                              bool* takesSemicolon = nullptr)
    {
      struct Tok { uint32_t begin, end; char punct; std::string_view word; };
      std::vector<Tok> ts;
      for (uint32_t i = 0; i < e.size();)
      {
        char c = e[i];
        if (isspace((unsigned char)c) || c == '\\')
          ++i;
        else if (c == '/' && i + 1 < e.size() && e[i + 1] == '/')
          while (i < e.size() && e[i] != '\n')
            ++i;
        else if (c == '/' && i + 1 < e.size() && e[i + 1] == '*')
        {
          size_t close = e.find("*/", i + 2);
          i = close == std::string::npos ? uint32_t(e.size()) : uint32_t(close + 2);
        }
        else if (c == '"' || c == '\'')
        {
          uint32_t b = i++;
          while (i < e.size() && e[i] != c)
            i += e[i] == '\\' ? 2 : 1;
          i = std::min<uint32_t>(i + 1, uint32_t(e.size()));
          ts.push_back({b, i, 'L', {}});
        }
        else if (isIdentChar(c))
        {
          uint32_t b = i;
          while (i < e.size() && (isIdentChar(e[i]) || (isdigit((unsigned char)e[b]) && e[i] == '.')))
            ++i;
          ts.push_back({b, i, 'w', std::string_view(e).substr(b, i - b)});
        }
        else if (c == ':' && i + 1 < e.size() && e[i + 1] == ':')
        {
          ts.push_back({i, i + 2, 'q', {}});
          i += 2;
        }
        else
        {
          ts.push_back({i, i + 1, c, {}});
          ++i;
        }
      }
      struct Frame
      {
        bool switchBody = false, caseBlock = false, label = false;
        char prev = 0; // the last token's kind at this level: a punctuator, 'w' a word, ':' a label's end
        // a `break` that ends its case if a label or the switch's end follows: its text [begin, end), and whether it
        // is the expansion's last token without its `;`
        uint32_t candidateBegin = UINT32_MAX, candidateEnd = 0;
        bool bare = false;
        // the last statement at this level is a return, throw, continue, goto or break (or a block standing as a
        // statement whose last one is: `case X: { ...; return nullptr; }`), and whether this level is such a block
        bool jumps = false, statementBlock = false;
        uint32_t labelEnd = UINT32_MAX; // the end of the last label's `:` at this level
      };
      std::vector<Frame> frames(1);
      std::vector<std::pair<uint32_t, uint32_t>> drop;
      std::vector<uint32_t> fallthroughs; // after a label's `:` that the next label follows (`case A: case B:`)
      int parens = 0;
      bool switchHead = false;
      bool droppedBare = false;
      auto statementStart = [](char prev) { return prev == 0 || prev == ';' || prev == '}' || prev == ':' || prev == '{'; };
      auto decide = [&](Frame& f, bool ends)
      {
        if (f.candidateBegin != UINT32_MAX && ends)
        {
          drop.push_back({f.candidateBegin, f.candidateEnd});
          droppedBare = droppedBare || f.bare;
        }
        f.candidateBegin = UINT32_MAX;
        f.bare = false;
      };
      for (size_t k = 0; k < ts.size(); ++k)
      {
        const Tok& t = ts[k];
        Frame& f = frames.back();
        if (parens == 0 && statementStart(f.prev) && t.punct != '{' && t.punct != '}') // (a statement's first token)
          f.jumps = t.word == "return" || t.word == "throw" || t.word == "continue" || t.word == "goto" || t.word == "break";
        if (t.punct == '(' || t.punct == '[')
          ++parens;
        else if (t.punct == ')' || t.punct == ']')
          --parens;
        else if (t.punct == '{')
        {
          decide(f, false);
          bool afterLabel = f.prev == ':' && parens == 0;
          bool standsAlone = statementStart(f.prev) && parens == 0;
          f.jumps = false;
          f.prev = '{';
          frames.push_back({});
          frames.back().switchBody = switchHead && parens == 0;
          frames.back().caseBlock = frames.back().label = afterLabel && !frames.back().switchBody;
          frames.back().statementBlock = standsAlone && !frames.back().switchBody;
          switchHead = false;
          continue;
        }
        else if (t.punct == '}')
        {
          Frame closed = f;
          if (closed.caseBlock && frames.size() > 1)
          {
            // (the block's break ends the case when what follows the block does)
            frames.pop_back();
            frames.back().candidateBegin = closed.candidateBegin;
            frames.back().candidateEnd = closed.candidateEnd;
            frames.back().prev = '}';
            frames.back().jumps = closed.jumps;
            continue;
          }
          decide(f, f.switchBody);
          if (frames.size() > 1)
            frames.pop_back();
          frames.back().prev = '}';
          frames.back().jumps = closed.statementBlock && closed.jumps;
          continue;
        }
        if (parens > 0 || (parens == 0 && (t.punct == ')' || t.punct == ']')))
        {
          if (t.punct == ')' || t.punct == ']')
            f.prev = ')';
          continue;
        }
        if (t.word == "switch")
          switchHead = true;
        if (t.word == "case" || (t.word == "default" && k + 1 < ts.size() && ts[k + 1].punct == ':')) // (not `= default;`)
        {
          // the label: up to its `:` (a `?`'s `:` is the conditional's)
          size_t j = k + 1;
          for (int questions = 0, depth = 0; j < ts.size(); ++j)
            if (ts[j].punct == '(' || ts[j].punct == '[')
              ++depth;
            else if (ts[j].punct == ')' || ts[j].punct == ']')
              --depth;
            else if (depth == 0 && ts[j].punct == '?')
              ++questions;
            else if (depth == 0 && ts[j].punct == ':' && questions-- == 0)
              break;
          if (j >= ts.size())
            return e; // (not a label this reads: the expansion as it is)
          if (f.prev == ':' && k > 0 && ts[k - 1].end == f.labelEnd)
            fallthroughs.push_back(f.labelEnd);
          decide(f, true);
          f.label = true;
          f.labelEnd = ts[j].end;
          f.prev = ':';
          f.jumps = false;
          k = j;
          continue;
        }
        bool semicolon = k + 1 < ts.size() && ts[k + 1].punct == ';';
        bool bare = k + 1 == ts.size() && frames.size() == 1 && semicolonAfterUse;
        // (also a group's only statement, `case A: break;` and `case A: { break; }`: kpp's `case A:` runs nothing)
        bool afterLabel = (f.prev == ':' && k > 0 && ts[k - 1].end == f.labelEnd) || (f.prev == '{' && f.caseBlock);
        if (t.word == "break" && (semicolon || bare) && f.label && (f.prev == ';' || f.prev == '}' || afterLabel))
        {
          decide(f, false);
          f.candidateBegin = t.begin;
          f.candidateEnd = semicolon ? ts[k + 1].end : t.end;
          f.bare = bare;
          f.prev = ';';
          f.jumps = true;
          k += semicolon ? 1 : 0;
          continue;
        }
        decide(f, false);
        f.prev = t.punct ? t.punct : 'w';
      }
      size_t dropped = drop.size();
      decide(frames.back(), frames.size() == 1 && labelAfterUse);
      if (endsCase)
        *endsCase = drop.size() > dropped || (frames.size() == 1 && frames.back().jumps);
      if (takesSemicolon)
        *takesSemicolon = droppedBare;
      if (drop.empty() && fallthroughs.empty())
        return e;
      // (both in the text's order: a removal [from, to), or an insertion at `from` where to is UINT32_MAX)
      std::vector<std::pair<uint32_t, uint32_t>> changes = drop;
      for (uint32_t at : fallthroughs)
        changes.push_back({at, UINT32_MAX});
      std::sort(changes.begin(), changes.end());
      std::string out;
      uint32_t at = 0;
      for (auto [from, to] : changes)
      {
        if (to == UINT32_MAX)
        {
          out.append(e, at, from - at);
          out += " fallthrough;";
          at = from;
          counts.add("fallthrough added after a label sharing the next one's statements in a macro's expansion");
          continue;
        }
        while (from > at && (e[from - 1] == ' ' || e[from - 1] == '\t'))
          --from;
        out.append(e, at, from - at);
        at = to;
        counts.add("trailing break removed in a macro's expansion");
      }
      out.append(e, at, std::string::npos);
      return out;
    }

    // A macro's use whose expansion ends its case: the `fallthrough;` walkSwitchBody inserted after it goes.
    void withdrawFallthrough(uint32_t useBegin)
    {
      auto inserted = fallthroughAfterUse.find(useBegin);
      if (inserted == fallthroughAfterUse.end())
        return;
      for (Edit& edit : edits)
        if (edit.offset == inserted->second && edit.length == 0 && edit.text.find("fallthrough;") != std::string::npos)
        {
          edit.text.clear();
          counts.add("fallthrough after a macro's use withdrawn: its expansion ends the case");
        }
    }

    // Whether a label or the closing brace of a switch's body follows the text at `at` (after a `;` and comments):
    // a macro use's next statement, for dropTrailingBreaksInExpansion.
    bool labelAfterUse(uint32_t at)
    {
      for (;;)
      {
        while (at < text.size() && (isspace((unsigned char)text[at]) || text[at] == ';' || text[at] == '\\'))
          ++at;
        if (text.compare(at, 2, "//") == 0 || (text[at] == '#' && (at == 0 || text.find_last_not_of(" \t", at - 1) == std::string::npos ||
                                                                   text[text.find_last_not_of(" \t", at - 1)] == '\n')))
          at = uint32_t(std::min(text.find('\n', at), text.size())); // (a comment, or a line `#undef CALL_DESTRUCTOR`)
        else if (text.compare(at, 2, "/*") == 0)
        {
          size_t close = text.find("*/", at + 2);
          at = close == std::string::npos ? uint32_t(text.size()) : uint32_t(close + 2);
        }
        else
          break;
      }
      if (at >= text.size())
        return false;
      uint32_t wordEnd = at;
      while (wordEnd < text.size() && isIdentChar(text[wordEnd]))
        ++wordEnd;
      std::string word(text.substr(at, wordEnd - at));
      if (word == "case" || word == "default")
        return true;
      if (!word.empty())
      {
        // the next use of a macro whose expansion starts with a label (lvm.c's vmcase after vmcase)
        const LocalMacro* next = localMacroAt(word, at);
        if (!next)
          return false;
        size_t start = next->body.find_first_not_of(" \t\r\n\\");
        size_t end = start;
        while (end != std::string::npos && end < next->body.size() && isIdentChar(next->body[end]))
          ++end;
        std::string first = start == std::string::npos ? std::string() : next->body.substr(start, end - start);
        return first == "case" || first == "default";
      }
      if (text[at] != '}')
        return false;
      // the switch's own closing brace: its `{` follows `switch (...)`
      auto close = std::lower_bound(toks.begin(), toks.end(), at, [](const Token& t, uint32_t o) { return t.offset < o; });
      if (close == toks.end() || close->offset != at || !close->is(P::RBrace))
        return false;
      int depth = 0;
      for (auto k = close; k != toks.begin();)
      {
        --k;
        if (k->is(P::RBrace))
          ++depth;
        else if (k->is(P::LBrace) && depth-- == 0)
        {
          if (k == toks.begin() || !(k - 1)->is(P::RParen))
            return false;
          int parens = 0;
          for (auto p = k - 1;; --p)
          {
            if (p->is(P::RParen))
              ++parens;
            else if (p->is(P::LParen) && --parens == 0)
            {
              if (p == toks.begin())
                return false;
              // `switch (...)`, or a macro's that writes it (lvm.c's `vmdispatch(o) switch(o)`)
              std::string head(spelling(uint32_t(p - 1 - toks.begin())));
              if (head == "switch")
                return true;
              const LocalMacro* m = localMacroAt(head, (p - 1)->offset);
              size_t start = m ? m->body.find_first_not_of(" \t\r\n\\") : std::string::npos;
              return start != std::string::npos && m->body.compare(start, 6, "switch") == 0 &&
                     (start + 6 == m->body.size() || !isIdentChar(m->body[start + 6]));
            }
            if (p == toks.begin())
              return false;
          }
        }
      }
      return false;
    }

    // Removes the statement text [from, to): with its line when it stands alone on it, before a comment keeping
    // the comment where the statement was, else with the space before it.
    void removeStatementText(uint32_t from, uint32_t to)
    {
      uint32_t lineStart = from;
      while (lineStart > 0 && (text[lineStart - 1] == ' ' || text[lineStart - 1] == '\t'))
        --lineStart;
      uint32_t lineEnd = to;
      while (lineEnd < text.size() && (text[lineEnd] == ' ' || text[lineEnd] == '\t' || text[lineEnd] == '\r'))
        ++lineEnd;
      bool aloneBefore = lineStart == 0 || text[lineStart - 1] == '\n';
      if (aloneBefore && (lineEnd == text.size() || text[lineEnd] == '\n'))
        remove(lineStart, lineEnd + (lineEnd < text.size() ? 1 : 0) - lineStart); // (the line it stands on alone)
      else if (aloneBefore)
        remove(from, lineEnd - from); // (`break; // why`: the comment stays, indented as the break was)
      else
        remove(lineStart, to - lineStart); // (`case A: x(); break;`, `{ x(); break; }`)
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
        // C++'s `[[fallthrough]];`, the statement kpp writes `fallthrough;`
        case NodeKind::FallthroughStmt:
          if (s->token + 5 < toks.size() && toks[s->token].is(P::LBracket))
          {
            replace(toks[s->token].offset, endOffset(s->token + 5) - toks[s->token].offset, "fallthrough;");
            counts.add("[[fallthrough]] written as fallthrough");
          }
          break;
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

    // --- C's rules (ConvertInput::c, kpp --c-sources) ---
    // C converts a void* to any object pointer implicitly (`unsigned char* p = malloc(n);`,
    // `b->buffer = ret;`), kpp only with a cast: the converter writes the cast. The types come from
    // the declarations the converted files and their headers make; a C library function that
    // returns void* is known by name, as the converter does not read the C library's headers.

