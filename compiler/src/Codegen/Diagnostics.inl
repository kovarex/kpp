// struct Emitter's members: errors, failures and their places. Part of codegen.cpp, included in the
// struct's body in Codegen/Emitter.hpp (a file of its own since 2026-11-05, moved without a
// change).
    // --- diagnostics ---

    std::unordered_map<const ParsedFile*, std::vector<uint32_t>> lineStarts; // per file: the offset each line starts at

    // shown: false for a failure that follows from an error the checker reported (fail): the
    // checker's error is the build's, and the function fails as it would with this one.
    void error(Node* at, const std::string& message, bool shown = true)
    {
      if (shown)
        errors.push_back(locationOf(at, currentFile, true) + ": error: " + message); // every codegen error is labelled, the gaps too
      else
        ++hiddenErrors;
      // A program function's own emission (not an instance's, a lambda's operator on demand, a
      // global's constructor or a shared text it happens to emit first): the function emitted again
      // gives the error again, at its place then.
      if (!owner.empty() && owner == emittingName && emittingNow.args.empty() && !emittingNow.classTy && emittingNow.depth == 0 &&
          emittingNow.name.empty() && emittingNow.requestedBy.empty() && onDemandNow.name.empty() && !emittingShared)
        erroring.emplace(owner, emittingNow.f);
      else
        unattributedErrors = true;
    }

    // "file:line" of a node in a file ("file:line:column" with --diagnostic-columns when column),
    // "?" when unknown.
    std::string locationOf(Node* at, ParsedFile* file, bool column = false)
    {
      size_t line = lineOf(at, file);
      if (!line)
        return "?";
      if (!column)
        return sourceLocation(file->path, line);
      // A member access's at its member's name (its node's token is the `.` or `->`).
      const std::vector<Token>& toks = *file->tokens;
      size_t token = std::min<size_t>(at->token, toks.size() - 1);
      if (at->kind == NodeKind::MemberExpr && token + 1 < toks.size() && toks[token + 1].getKind() == TokenKind::Identifier)
        ++token;
      const std::vector<uint32_t>& starts = lineStarts[file];
      size_t tokenLine = size_t(std::upper_bound(starts.begin(), starts.end(), toks[token].offset) - starts.begin());
      return sourceLocation(file->path, tokenLine, 1 + toks[token].offset - starts[tokenLine - 1]);
    }

    // "file:line" of a node, written into the program itself (what --stub-missing's stub says): the
    // same in every build, whatever form its diagnostics take (--msvc-diagnostics, a compile server's
    // marked places, source.hpp).
    std::string programLocationOf(Node* at, ParsedFile* file)
    {
      size_t line = lineOf(at, file);
      // (the path as the program's text gives it: --file-prefix-map's, no path of this machine in a release)
      return line ? programPathOf(file) + ":" + std::to_string(line) : std::string("?");
    }

    // The line a node starts on in its file (from 1), 0 when unknown.
    size_t lineOf(Node* at, ParsedFile* file)
    {
      if (!file || !file->tokens || !at || file->tokens->empty())
        return 0;
      const std::vector<Token>& toks = *file->tokens;
      const Token& t = toks[std::min<size_t>(at->token, toks.size() - 1)];
      std::vector<uint32_t>& starts = lineStarts[file];
      if (starts.empty())
      {
        starts.push_back(0);
        for (uint32_t i = 0; i < file->text.size(); ++i)
          if (file->text[i] == '\n')
            starts.push_back(i + 1);
      }
      return size_t(std::upper_bound(starts.begin(), starts.end(), t.offset) - starts.begin()); // lines starting at or before the token
    }

    struct Unsupported
    {
    };
    std::string lastFailure; // the message of the last fail(), without its place (a failed layout or global repeats it at every use)
    bool lastFailureShown = true; // and whether it was shown (fail's shown)
    // An error that abandons what is being emitted (the function, layout or global it is in).
    // shown: false when the failure follows from a checker error: a name or a type the checker
    // reported as not declared (followsUndeclared), which the build fails with already.
    [[noreturn]] void fail(Node* at, const std::string& message, bool shown = true)
    {
      std::string where;
      if (!currentContext.empty())
        where = " (in " + currentContext + (currentRequestedByContext.empty() ? "" : ", requested by " + currentRequestedByContext) + ")";
      else if (!currentFunctionName.empty())
        where = " (in " + currentFunctionName + (currentRequestedBy.empty() ? "" : ", requested by " + currentRequestedBy) + ")";
      // A value of unknown type in a function the checker reported an error in (`auto v = p.size()`
      // on an undeclared type's p) follows from that error too; and so does any failure on an
      // expression that holds an error the checker reported (`undefinedName(1)`, a call given
      // `NOT_A_NAME`): the build fails with that error already, and this one only repeats it (an
      // editor showed both). A failure elsewhere in the same function is shown.
      if (shown && ((message.find("unknown type") != std::string::npos && global.checkerErrorIn(checkerChecked)) || holdsCheckerError(at)))
        shown = false;
      lastFailure = message;
      lastFailureShown = shown;
      error(at, message + where, shown);
      throw Unsupported{};
    }
    // Whether node at is an expression that holds an error the checker reported: one at a token from
    // its first to its last (its subtree's), in the file being emitted. Only an expression (a call,
    // an argument, a conversion): a failure on a declaration (a local array whose length the checker
    // found no constant) is the code generator's own consequence, and stays shown.
    bool holdsCheckerError(Node* at)
    {
      if (!at || at->kind < NodeKind::NameExpr || at->kind > NodeKind::ParenExpr || !currentFile || !global.checkerErrorIn(checkerChecked))
        return false;
      uint32_t first = at->token, last = at->token;
      forEachNode(at, [&](Node* n)
      {
        first = std::min(first, n->token);
        last = std::max(last, n->token);
      });
      return global.checkerErrorWithin(currentFile, first, last);
    }
    // A construct the code generator cannot translate yet.
    [[noreturn]] void unsupported(Node* at, const std::string& what, bool shown = true) { fail(at, "codegen does not support " + what + " yet", shown); }
    // Whether t (through references and pointers) is an undeclared type's placeholder class the
    // checker reported where it is written: what fails on it follows from that error.
    bool followsUndeclared(const Ty* t)
    {
      while (t && (t->isReference() || t->kind == TyKind::Pointer))
        t = t->inner;
      return t && t->decl && (t->decl->attributes & ATTR_Draft) && global.undeclaredReported(global.draftName(t->decl));
    }

