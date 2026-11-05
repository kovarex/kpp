// IfEval.inl: the #if evaluator.
// Part of convert.cpp, included where it stood in its anonymous namespace (a file of its own since 2026-11-05, moved without a change).

  // Evaluates an #if expression as C's preprocessor does, in long long: defined(X), X (its body,
  // evaluated in turn), __has_include, numbers, the unary ! (or `not`), - + ~, then * / %, + -,
  // << >>, the comparisons, & ^ |, && and || (`and`, `or`), with C's precedence and parentheses
  // (luaconf.h's `#if INT_MAX-20 < 32760`, lopcodes.h's `#if SIZE_Bx < LUAI_BITSINT-1`). A name the
  // table does not have is 0; those neither in undefinedOnPurpose nor in `known` (the names some
  // converted file defines, in a branch this build drops) are collected in `undefined`.
  struct IfEval
  {
    std::string s;
    const MacroTable& table;
    std::set<std::string>* undefined = nullptr;
    const std::set<std::string>* known = nullptr;
    int depth = 0;
    size_t p = 0;
    bool unexpected(const std::string& name) const { return !undefinedOnPurpose.count(name) && !(known && known->count(name)); }
    void skipSpace()
    {
      while (p < s.size() && isspace((unsigned char)s[p]))
        ++p;
    }
    long long symbol(const std::string& name)
    {
      auto it = table.find(name);
      if (it == table.end())
      {
        if (undefined && unexpected(name))
          undefined->insert(name);
        return 0;
      }
      if (it->second.empty())
        return 1;
      if (depth > 8)
        return 0;
      IfEval body{it->second, table, undefined, known, depth + 1};
      return body.orExpr();
    }
    bool isDefined(const std::string& name)
    {
      if (table.count(name))
        return true;
      if (undefined && unexpected(name))
        undefined->insert(name);
      return false;
    }
    std::string word()
    {
      size_t start = p;
      while (p < s.size() && isIdentChar(s[p]))
        ++p;
      return s.substr(start, p - start);
    }
    long long primary()
    {
      skipSpace();
      if (p < s.size() && s[p] == '!')
      {
        ++p;
        return !primary();
      }
      if (p < s.size() && (s[p] == '-' || s[p] == '+' || s[p] == '~'))
      {
        char op = s[p++];
        long long v = primary();
        return op == '-' ? -v : op == '~' ? ~v : v;
      }
      if (p < s.size() && s[p] == '(')
      {
        ++p;
        long long v = orExpr();
        skipSpace();
        if (p < s.size() && s[p] == ')')
          ++p;
        return v;
      }
      if (p < s.size() && isdigit((unsigned char)s[p]))
      {
        long long v = strtoll(s.c_str() + p, nullptr, 0);
        while (p < s.size() && isIdentChar(s[p]))
          ++p;
        return v;
      }
      std::string name = word();
      if (name == "not")
        return !primary();
      if (name == "defined")
      {
        skipSpace();
        bool paren = p < s.size() && s[p] == '(';
        if (paren)
          ++p;
        skipSpace();
        std::string sym = word();
        skipSpace();
        if (paren && p < s.size() && s[p] == ')')
          ++p;
        return isDefined(sym);
      }
      if (name == "__has_include")
      {
        size_t open = s.find_first_of("<\"", p);
        size_t close = open == std::string::npos ? open : s.find_first_of(">\"", open + 1);
        size_t paren = close == std::string::npos ? close : s.find(')', close);
        if (paren == std::string::npos)
          return 0;
        std::string header = s.substr(open + 1, close - open - 1);
        p = paren + 1;
        return availableHeaders.count(header) != 0;
      }
      if (name.empty())
      {
        ++p;
        return 0;
      }
      return symbol(name);
    }
    // Whether the operator op comes next (and not a longer one it starts: `<` is not `<<` or `<=`,
    // `&` not `&&`); taken when it does.
    bool next(const char* op)
    {
      skipSpace();
      size_t n = strlen(op);
      if (s.compare(p, n, op) != 0)
        return false;
      char after = p + n < s.size() ? s[p + n] : 0;
      if ((n == 1 && (op[0] == '<' || op[0] == '>') && (after == op[0] || after == '=')) || (n == 1 && (op[0] == '&' || op[0] == '|') && after == op[0]) ||
          (n == 1 && op[0] == '!' && after == '=') || (n == 2 && (op[0] == '<' || op[0] == '>') && op[1] == op[0] && after == '='))
        return false;
      p += n;
      return true;
    }
    long long mulExpr()
    {
      long long l = primary();
      for (;;)
      {
        if (next("*"))
          l *= primary();
        else if (next("/"))
        {
          long long r = primary();
          l = r == 0 ? 0 : l / r;
        }
        else if (next("%"))
        {
          long long r = primary();
          l = r == 0 ? 0 : l % r;
        }
        else
          return l;
      }
    }
    long long addExpr()
    {
      long long l = mulExpr();
      for (;;)
      {
        if (next("+"))
          l += mulExpr();
        else if (next("-"))
          l -= mulExpr();
        else
          return l;
      }
    }
    long long shiftExpr()
    {
      long long l = addExpr();
      for (;;)
      {
        if (next("<<"))
          l <<= addExpr();
        else if (next(">>"))
          l >>= addExpr();
        else
          return l;
      }
    }
    long long relExpr()
    {
      long long l = shiftExpr();
      for (;;)
      {
        if (next("<="))
          l = l <= shiftExpr();
        else if (next(">="))
          l = l >= shiftExpr();
        else if (next("<"))
          l = l < shiftExpr();
        else if (next(">"))
          l = l > shiftExpr();
        else
          return l;
      }
    }
    long long cmpExpr()
    {
      long long l = relExpr();
      for (;;)
      {
        if (next("=="))
          l = l == relExpr();
        else if (next("!="))
          l = l != relExpr();
        else
          return l;
      }
    }
    long long bitAndExpr()
    {
      long long l = cmpExpr();
      while (next("&"))
        l &= cmpExpr();
      return l;
    }
    long long bitXorExpr()
    {
      long long l = bitAndExpr();
      while (next("^"))
        l ^= bitAndExpr();
      return l;
    }
    long long bitExpr()
    {
      long long l = bitXorExpr();
      while (next("|"))
        l |= bitXorExpr();
      return l;
    }
    long long andExpr()
    {
      long long l = bitExpr();
      for (;;)
      {
        skipSpace();
        if (s.compare(p, 2, "&&") == 0)
          p += 2;
        else if (s.compare(p, 3, "and") == 0 && (p + 3 >= s.size() || !isIdentChar(s[p + 3])))
          p += 3;
        else
          return l;
        long long r = bitExpr();
        l = l && r;
      }
    }
    long long orExpr()
    {
      long long l = andExpr();
      for (;;)
      {
        skipSpace();
        if (s.compare(p, 2, "||") == 0)
          p += 2;
        else if (s.compare(p, 2, "or") == 0 && (p + 2 >= s.size() || !isIdentChar(s[p + 2])))
          p += 2;
        else
          return l;
        long long r = andExpr();
        l = l || r;
      }
    }
  };

