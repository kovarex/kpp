// FileConverterTokens.inl: FileConverter's members: macro expansion, printf and the token pass.
// Part of convert.cpp, included in struct FileConverter's body in Converter/FileConverter.hpp (a file of its own since 2026-11-05, moved without a change).

    // A macro argument as `#x` makes it a string literal, by C's rule: the white space between its
    // tokens, line breaks too (an argument may span lines: a release_assert of a long condition),
    // becomes one space, none at its ends, and the `"` and `\` of its string and character
    // literals are escaped.
    static std::string stringized(const std::string& argument)
    {
      std::string out = "\"";
      bool space = false;
      for (size_t i = 0; i < argument.size();)
      {
        char c = argument[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
          space = out.size() > 1;
          ++i;
          continue;
        }
        if (space)
          out += ' ';
        space = false;
        if (c == '"' || (c == '\'' && !isDigitSeparator(argument, i)))
        {
          size_t j = i + 1;
          if (size_t raw = rawStringEnd(argument, i); raw != std::string_view::npos)
            j = raw - 1;
          while (j < argument.size() && argument[j] != c && argument[j] != '\n')
            j += argument[j] == '\\' && j + 1 < argument.size() ? 2 : 1;
          size_t end = std::min(j + 1, argument.size());
          for (size_t k = i; k < end; ++k)
          {
            if (argument[k] == '"' || argument[k] == '\\')
              out += '\\';
            out += argument[k];
          }
          i = end;
          continue;
        }
        out += c;
        ++i;
      }
      return out + "\"";
    }

    // Whether a macro is variadic: its last parameter is `...`, which __VA_ARGS__ stands for.
    static bool variadic(const LocalMacro& m) { return !m.params.empty() && m.params.back() == "..."; }
    // Whether a use with count arguments fits the macro: one each, or for a variadic macro any number
    // for the `...` (none included). Fewer are empty, as the game's compiler (MSVC) has it:
    // UpgradableEntity's FOR_EACH_SUPPORTED_TYPE(DEFINE_ENUM_ENTRY,) passes DEFINE_ENUM_ENTRY(type, _)
    // one argument once its empty __VA_ARGS__ takes the comma along.
    static bool argumentsFit(const LocalMacro& m, size_t count)
    {
      return variadic(m) || count <= m.params.size();
    }

    // The body of a local macro with its parameters replaced by the argument texts; `#x` quotes,
    // `##` pastes. __VA_ARGS__ is the arguments of a variadic macro's `...`, joined by commas; when
    // there are none, a comma right before it goes too, as the game's compiler (MSVC) has it.
    // An argument passed on from one macro to another: its rewritten text and its spelling, between
    // \x01, \x02 and \x03, until the inner macro takes one of them (untagged).
    static std::string taggedArgument(const std::string& rewritten, const std::string& spelling)
    {
      return "\x01" + untagged(rewritten, true) + "\x02" + untagged(spelling, false) + "\x03";
    }
    static std::string withoutProtection(std::string text)
    {
      text.erase(std::remove_if(text.begin(), text.end(), [](char c) { return c == '\x04' || c == '\x05'; }), text.end());
      return text;
    }
    static std::string untagged(const std::string& text, bool rewritten)
    {
      if (text.find('\x01') == std::string::npos)
        return text;
      std::string out;
      for (size_t i = 0; i < text.size();)
      {
        size_t open = text.find('\x01', i);
        if (open == std::string::npos)
        {
          out.append(text, i, std::string::npos);
          break;
        }
        size_t middle = text.find('\x02', open), close = text.find('\x03', middle);
        out.append(text, i, open - i);
        out += rewritten ? text.substr(open + 1, middle - open - 1) : text.substr(middle + 1, close - middle - 1);
        i = close + 1;
      }
      return out;
    }
    // The macro's body with the arguments put in. spellings, when given, are the arguments as written,
    // which # and ## take (C++ stringizes and pastes the tokens as they are), a place right after
    // `.`, `->` or `::` (a member's name: Agui's Listener puts its argument in `this->NAME` and
    // `other.NAME`); elsewhere args, the arguments with the conversion's rewrites. An argument passed on
    // to another macro takes both, tagged (taggedArgument), for that macro to choose by its own use
    // of it: UpgradableEntity's dispatch puts its call after `->`, UnitTest++'s TEST passes its name
    // on to be pasted and stringized, CHECK passes its value on to be evaluated.
    std::string expandMacroOnce(const LocalMacro& m, const std::vector<std::string>& args, const std::vector<std::string>* spellings = nullptr,
                                uint32_t useOffset = 0)
    {
      // For each parenthesis open in the body, whether it is a macro's argument list.
      std::vector<bool> macroCalls;
      std::string out;
      bool withoutId = args.size() == 4 && m.body == replacedMacroBodies.at("CHECK_INTERNAL") &&
                       args[3].find_first_not_of(" \t") != std::string::npos &&
                       args[3].substr(args[3].find_first_not_of(" \t"), args[3].find_last_not_of(" \t") + 1 - args[3].find_first_not_of(" \t")) == "nullptr";
      const std::string& b = withoutId ? checkInternalWithoutId : m.body;
      if (m.body == replacedMacroBodies.at("CHECK_INTERNAL"))
        counts.add(withoutId ? "checks written as a call of UnitTest::checkInternal" : "checks with an id written as a call of UnitTest::checkInternal");
      auto variadicOf = [&](const std::vector<std::string>& list)
      {
        std::string joined;
        for (size_t k = m.params.size() - 1; k < list.size(); ++k)
          joined += (k + 1 == m.params.size() ? "" : ", ") + list[k];
        return joined;
      };
      std::string variadicArgs = variadic(m) ? variadicOf(args) : std::string();
      bool afterPaste = false;
      // The text of argument k put in at body position at (just after the parameter's name): its
      // spelling after # or next to ##, or after `.`, `->` or `::`; both, tagged, inside another
      // macro's arguments; else the rewritten argument.
      auto argumentAt = [&](size_t k, size_t at, bool stringize, bool pasteBefore) -> std::string
      {
        if (!spellings)
          return k < args.size() ? args[k] : std::string();
        size_t next = at;
        while (next < b.size() && b[next] == ' ')
          ++next;
        bool pasted = pasteBefore || b.compare(next, 2, "##") == 0;
        size_t last = out.find_last_not_of(" \t");
        bool member = last != std::string::npos &&
                      (out[last] == '.' || (last > 0 && out[last] == '>' && out[last - 1] == '-') || (last > 0 && out[last] == ':' && out[last - 1] == ':'));
        bool forwarded = std::find(macroCalls.begin(), macroCalls.end(), true) != macroCalls.end();
        std::string argument = k < args.size() ? args[k] : std::string();
        std::string spelling = k < spellings->size() ? (*spellings)[k] : std::string();
        if (stringize || pasted || member)
          return spelling;
        if (forwarded && argument != spelling)
          return taggedArgument(argument, spelling);
        // (Renamed already where a rewrite changed it: expandMacro's renames pass it by. One the
        // rewrites left alone stays plain text, a macro's name passed to an X-macro list among them.)
        return argument == spelling ? argument : "\x04" + argument + "\x05";
      };
      for (size_t i = 0; i < b.size();)
      {
        char c = b[i];
        if (c == '#' && i + 1 < b.size() && b[i + 1] == '#')
        {
          while (!out.empty() && out.back() == ' ')
            out.pop_back();
          i += 2;
          while (i < b.size() && b[i] == ' ')
            ++i;
          afterPaste = true;
          continue;
        }
        bool pasteBefore = afterPaste;
        afterPaste = false;
        if (spellings && c == '(')
        {
          size_t last = out.find_last_not_of(" \t");
          size_t start = last;
          while (start != std::string::npos && start > 0 && isIdentChar(out[start - 1]))
            --start;
          bool call = last != std::string::npos && isIdentChar(out[last]) && localMacroAt(out.substr(start, last + 1 - start), useOffset);
          macroCalls.push_back(call);
        }
        else if (spellings && c == ')' && !macroCalls.empty())
          macroCalls.pop_back();
        if (variadic(m) && b.compare(i, 11, "__VA_ARGS__") == 0 && (i == 0 || !isIdentChar(b[i - 1])) &&
            (i + 11 == b.size() || !isIdentChar(b[i + 11])))
        {
          std::string joined;
          for (size_t k = m.params.size() - 1; k < args.size(); ++k)
            joined += (k + 1 == m.params.size() ? "" : ", ") + argumentAt(k, i + 11, false, pasteBefore);
          if (joined.empty())
          {
            size_t end = out.find_last_not_of(" \t");
            if (end != std::string::npos && out[end] == ',')
              out.erase(end);
          }
          out += joined;
          i += 11;
          continue;
        }
        bool stringize = c == '#';
        size_t idStart = stringize ? i + 1 : i;
        if (idStart < b.size() && (isalpha((unsigned char)b[idStart]) || b[idStart] == '_') && (idStart == 0 || stringize || !isIdentChar(b[idStart - 1])))
        {
          size_t j = idStart;
          while (j < b.size() && isIdentChar(b[j]))
            ++j;
          std::string word = b.substr(idStart, j - idStart);
          size_t which = m.params.size();
          for (size_t k = 0; k < m.params.size(); ++k)
            if (m.params[k] == word)
              which = k;
          if (which < m.params.size())
          {
            std::string argument = argumentAt(which, j, stringize, pasteBefore);
            out += stringize ? stringized(withoutProtection(untagged(argument, false))) : argument;
          }
          else if (stringize && word == "__VA_ARGS__" && variadic(m))
            out += stringized(spellings ? variadicOf(*spellings) : variadicArgs);
          else
            out.append(b, i, j - i);
          i = j;
          continue;
        }
        if (size_t end = commentEnd(b, i); end != std::string::npos)
        {
          // A comment the expansion keeps, as written: an apostrophe in it (DrawQueue's "I'm not
          // sure") is no character literal that would hide the parameters after it.
          out.append(b, i, end - i);
          i = end;
          continue;
        }
        if (c == '"' || (c == '\'' && !isDigitSeparator(b, i)))
        {
          size_t j = i + 1;
          if (size_t raw = rawStringEnd(b, i); raw != std::string_view::npos)
            j = raw - 1;
          while (j < b.size() && b[j] != c)
          {
            if (b[j] == '\\')
              ++j;
            ++j;
          }
          size_t end = std::min(j + 1, b.size());
          out.append(b, i, end - i);
          i = end;
          continue;
        }
        out += c;
        ++i;
      }
      return out;
    }

    // The end of the comment (`/* */` or `//` to the line's end) that starts at i in macro text;
    // npos when none starts there.
    static size_t commentEnd(const std::string& s, size_t i)
    {
      if (s[i] != '/' || i + 1 >= s.size())
        return std::string::npos;
      if (s[i + 1] == '*')
      {
        size_t close = s.find("*/", i + 2);
        return close == std::string::npos ? s.size() : close + 2;
      }
      if (s[i + 1] == '/')
        return std::min(s.find('\n', i), s.size());
      return std::string::npos;
    }

    // The `)` that closes the `(` at open in macro text, skipping string and character literals and
    // comments; npos when there is none.
    static size_t closingParen(const std::string& s, size_t open)
    {
      int depth = 0;
      for (size_t k = open; k < s.size(); ++k)
      {
        if (size_t raw = rawStringEnd(s, k); raw != std::string_view::npos)
          k = raw - 1;
        else if (size_t end = commentEnd(s, k); end != std::string::npos)
          k = end - 1;
        else if (s[k] == '"' || (s[k] == '\'' && !isDigitSeparator(s, k)))
        {
          char quote = s[k];
          for (++k; k < s.size() && s[k] != quote; ++k)
            if (s[k] == '\\')
              ++k;
        }
        else if (s[k] == '(')
          ++depth;
        else if (s[k] == ')' && --depth == 0)
          return k;
      }
      return std::string::npos;
    }

    // Calls visit(k, depth) for each character of s outside strings and comments, depth counting the
    // brackets of every kind open before it (a closing one is at its own depth); stops when visit
    // returns false.
    template<typename Visit>
    static void eachCodeChar(const std::string& s, Visit visit)
    {
      int depth = 0;
      for (size_t k = 0; k < s.size(); ++k)
      {
        if (size_t raw = rawStringEnd(s, k); raw != std::string_view::npos)
          k = raw - 1;
        else if (size_t end = commentEnd(s, k); end != std::string::npos)
          k = end - 1;
        else if (s[k] == '"' || (s[k] == '\'' && !isDigitSeparator(s, k)))
        {
          char quote = s[k];
          for (++k; k < s.size() && s[k] != quote; ++k)
            if (s[k] == '\\')
              ++k;
        }
        else
        {
          bool closing = s[k] == ')' || s[k] == ']' || s[k] == '}';
          depth -= closing;
          if (!visit(k, depth))
            return;
          depth += s[k] == '(' || s[k] == '[' || s[k] == '{';
        }
      }
    }
    static std::string trimmed(const std::string& s)
    {
      size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
      return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
    }
    // A statement macro's expansion in C's do/while form, `do { BODY } while (false)` (or `(0)`),
    // which exists only to make `MACRO(x);` one statement wherever a statement stands: BODY, else
    // nothing.
    static std::optional<std::string> doWhileBody(const std::string& expansion)
    {
      std::string s = trimmed(expansion);
      if (s.compare(0, 2, "do") != 0 || s.size() < 3 || isIdentChar(s[2]))
        return std::nullopt;
      size_t open = s.find_first_not_of(" \t", 2);
      if (open == std::string::npos || s[open] != '{')
        return std::nullopt;
      size_t close = std::string::npos;
      eachCodeChar(s, [&](size_t k, int depth)
      {
        if (k > open && s[k] == '}' && depth == 0)
        {
          close = k;
          return false;
        }
        return true;
      });
      if (close == std::string::npos)
        return std::nullopt;
      std::string after;
      for (char c : s.substr(close + 1))
        if (!isspace((unsigned char)c))
          after += c;
      if (after != "while(false)" && after != "while(0)")
        return std::nullopt;
      // (a `break` or `continue` of its own leaves the do/while: without the wrapper it would leave
      // the loop around the use, so such a macro keeps it. One inside a block of a switch or a loop
      // of the body is that statement's; one anywhere else keeps the wrapper.)
      std::string body = trimmed(s.substr(open + 1, close - open - 1));
      auto wordAt = [&](size_t k, const char* word)
      {
        size_t n = strlen(word);
        return body.compare(k, n, word) == 0 && (k == 0 || !isIdentChar(body[k - 1])) && (k + n == body.size() || !isIdentChar(body[k + n]));
      };
      auto blockEnd = [&](size_t from) // the `}` closing the block that opens at body[from] == '{'
      {
        size_t end = std::string::npos;
        std::string rest = body.substr(from);
        eachCodeChar(rest, [&](size_t k, int depth)
        {
          if (k > 0 && rest[k] == '}' && depth == 0)
          {
            end = from + k;
            return false;
          }
          return true;
        });
        return end;
      };
      bool leaves = false;
      size_t skipTo = 0;
      eachCodeChar(body, [&](size_t k, int)
      {
        if (k < skipTo)
          return true;
        for (const char* statement : {"switch", "for", "while", "do"})
          if (wordAt(k, statement))
          {
            size_t at = k + strlen(statement);
            if (strcmp(statement, "do") != 0)
            {
              size_t paren = body.find_first_not_of(" \t", at);
              size_t closeParen = paren != std::string::npos && body[paren] == '(' ? closingParen(body, paren) : std::string::npos;
              at = closeParen == std::string::npos ? std::string::npos : closeParen + 1;
            }
            size_t brace = at == std::string::npos ? at : body.find_first_not_of(" \t", at);
            if (brace != std::string::npos && body[brace] == '{')
              if (size_t end = blockEnd(brace); end != std::string::npos)
                skipTo = end + 1;
            return true;
          }
        if (wordAt(k, "break") || wordAt(k, "continue"))
          leaves = true;
        return !leaves;
      });
      if (leaves)
        return std::nullopt;
      return body;
    }
    // Whether body is one statement (`if (c) { ... }`, `f(x);`), which stands where the do/while did
    // without braces: no `;` before its end and no `else`, at its top level.
    static bool oneStatement(const std::string& body)
    {
      if (body.empty() || (body.back() != ';' && body.back() != '}'))
        return false;
      bool one = true;
      std::string head = body.substr(0, 2);
      eachCodeChar(body, [&](size_t k, int depth)
      {
        if (depth != 0)
          return true;
        if ((body[k] == ';' && k + 1 != body.size()) || (body[k] == '}' && k + 1 != body.size()) ||
            (body.compare(k, 4, "else") == 0 && (k == 0 || !isIdentChar(body[k - 1])) && (k + 4 == body.size() || !isIdentChar(body[k + 4]))))
          one = false;
        // (a block of its own at the top, `{ ... }`, is a statement only with an if, a switch or a loop
        // before it)
        if (body[k] == '{' && head != "if" && body.compare(0, 3, "for") != 0 && body.compare(0, 5, "while") != 0 && body.compare(0, 6, "switch") != 0)
          one = false;
        return one;
      });
      return one;
    }

    // useOffset is where the macro is used, for __FILE__ and __LINE__ in known macros' replacements.
    std::string expandMacro(const LocalMacro& m, const std::vector<std::string>& args, uint32_t useOffset, const std::vector<std::string>* spellings = nullptr)
    {
      // The preprocessor's file and line in the body are the use's (`__FILE__` in a header's
      // release_assert-like macro).
      std::string text = this->renameOutsideArguments(withFileAndLine(untagged(rescanMacros(expandMacroOnce(m, args, spellings, useOffset), useOffset), true), useOffset));
      return atDeclarationScope(useOffset) ? bracedObjects(std::move(text)) : text;
    }
    // Whether the text at offset stands at namespace or class scope, outside every function body:
    // where kpp reads `T f(...)` as a function (in a body it is an object, and a macro's
    // `Serialiser::TagWriter _(output, "entity", ...)` stays as the printf rewrite reads it). Each
    // `{` is a scope's when its statement names namespace, class, struct, union or enum (or is
    // `extern "C"`), an initializer's after `=`, `(`, `,` or `{` (its enclosing one's), a body's
    // otherwise; inside a body everything is.
    std::vector<uint8_t> tokenInBody;
    bool atDeclarationScope(uint32_t offset)
    {
      if (tokenInBody.empty() && !toks.empty())
      {
        tokenInBody.assign(toks.size(), 0);
        std::vector<uint8_t> stack;
        uint32_t statementStart = 0;
        for (uint32_t k = 0; k < toks.size(); ++k)
        {
          tokenInBody[k] = !stack.empty() && stack.back();
          if (toks[k].is(P::LBrace))
          {
            bool parentBody = !stack.empty() && stack.back();
            bool scope = false;
            for (uint32_t j = statementStart; j < k && !scope; ++j)
              if (toks[j].getKind() == TokenKind::Keyword && !(j > 0 && (toks[j - 1].is(P::Less) || toks[j - 1].is(P::Comma)))) // (not a template parameter's `class T`)
              {
                std::string_view w = spelling(j);
                scope = w == "namespace" || w == "class" || w == "struct" || w == "union" || w == "enum" || w == "extern";
              }
              else if (toks[j].getKind() == TokenKind::Identifier)
              {
                // (a macro that opens a namespace: UnitTest++'s `SUITE(Name) { ... }`, whose TESTs
                // stand at namespace scope)
                auto macro = localMacros.find(std::string(spelling(j)));
                if (macro != localMacros.end())
                  for (const LocalMacro& m : macro->second)
                    scope = scope || m.body.find("namespace") != std::string::npos;
              }
            bool initializer = k > 0 && (toks[k - 1].is(P::Assign) || toks[k - 1].is(P::LParen) || toks[k - 1].is(P::Comma) || toks[k - 1].is(P::LBrace));
            stack.push_back(parentBody || (!scope && !initializer));
            statementStart = k + 1;
          }
          else if (toks[k].is(P::RBrace))
          {
            if (!stack.empty())
              stack.pop_back();
            statementStart = k + 1;
          }
          else if (toks[k].is(P::Semi) || toks[k].getKind() == TokenKind::Directive)
            statementStart = k + 1;
        }
      }
      auto at = std::lower_bound(toks.begin(), toks.end(), offset, [](const Token& t, uint32_t o) { return t.offset < o; });
      return at == toks.end() || !tokenInBody[size_t(at - toks.begin())];
    }
    // An object a macro's expansion declares by parentheses, `UnitTest::ListAdder adderX (List,
    // &testX);` (TEST's, at namespace scope, where kpp reads `T f(...)` as a function): braces, when
    // its class is the program's and braces construct it as parentheses do (braceableClasses). A
    // statement's start only (after `;`, `{`, `}` or the text's start), its `)` ending the statement.
    std::string bracedObjects(std::string text) const
    {
      if (in.c || !braceableClasses || braceableClasses->empty())
        return text;
      for (size_t i = 0; i < text.size(); ++i)
      {
        if (!(isalpha((unsigned char)text[i]) || text[i] == '_') || (i > 0 && (isIdentChar(text[i - 1]) || text[i - 1] == ':')))
          continue;
        size_t before = text.find_last_not_of(" \t\r\n", i == 0 ? std::string::npos : i - 1);
        if (i > 0 && before != std::string::npos && !strchr(";{}", text[before]))
          continue;
        size_t typeEnd = i;
        while (typeEnd < text.size() && (isIdentChar(text[typeEnd]) || (text[typeEnd] == ':' && typeEnd + 1 < text.size() && text[typeEnd + 1] == ':')))
          typeEnd += text[typeEnd] == ':' ? 2 : 1;
        std::string type = text.substr(i, typeEnd - i);
        if (!braceableClasses->count(type))
          continue;
        size_t name = text.find_first_not_of(" \t", typeEnd);
        if (name == std::string::npos || name == typeEnd || !(isalpha((unsigned char)text[name]) || text[name] == '_'))
          continue;
        size_t nameEnd = name;
        while (nameEnd < text.size() && isIdentChar(text[nameEnd]))
          ++nameEnd;
        size_t open = text.find_first_not_of(" \t", nameEnd);
        if (open == std::string::npos || text[open] != '(')
          continue;
        size_t close = open;
        for (int depth = 0; close < text.size(); ++close)
          if (text[close] == '(')
            ++depth;
          else if (text[close] == ')' && --depth == 0)
            break;
        if (close >= text.size())
          continue;
        size_t after = text.find_first_not_of(" \t", close + 1);
        if (after != std::string::npos && text[after] != ';')
          continue;
        text[open] = '{';
        text[close] = '}';
        text.erase(nameEnd, open - nameEnd); // (`adderX {...}`: no space before the brace)
        i = close - (open - nameEnd);
      }
      return text;
    }
    // C's `assert(c)` where an expression stands (llimits.h's `#define check_exp(c,e)
    // (lua_assert(c), (e))`): kpp's assert is a statement, so the assert goes in a lambda called on
    // the spot, which asserts as the statement does.
    static bool expressionPosition(const std::string& before)
    {
      size_t last = before.find_last_not_of(" \t\n");
      return last != std::string::npos && strchr("(,?=&|!+-*/%<>^~[", before[last]);
    }
    std::string rescanMacros(std::string result, uint32_t useOffset)
    {
      // Re-scan for nested local macro calls (X-macro pattern: outer expands to inner(args) calls)
      // and for the known function macros (a local macro wrapping SAVING_TAG, for example), which
      // the token-level rewrite does not see inside the expansion's text. Each pass goes one level
      // deeper, until nothing is left to expand: Lua's nest about eight deep (setsvalue2s through
      // checkliveness, righttt and ttypenv to rttype).
      for (int pass = 0; pass < 16; ++pass)
      {
        bool changed = false;
        std::string next;
        for (size_t i = 0; i < result.size();)
        {
          if (isalpha((unsigned char)result[i]) || result[i] == '_')
          {
            size_t j = i;
            while (j < result.size() && isIdentChar(result[j]))
              ++j;
            std::string name = result.substr(i, j - i);
            if (name == "assert" && j < result.size() && result[j] == '(' && expressionPosition(next))
            {
              size_t close = closingParen(result, j);
              if (close != std::string::npos)
              {
                next += "([&] { assert(" + result.substr(j + 1, close - j - 1) + "); }())";
                changed = true;
                i = close + 1;
                counts.add("asserts in expressions put in lambdas");
                continue;
              }
            }
            if (in.c && cWrittenOutMacros && cWrittenOutMacros->count("*") && !cOwnConstant(name))
              if (const std::string* own = cOwnObjectAt(name, useOffset))
              {
                next += *own;
                changed = true;
                i = j;
                continue;
              }
            if (in.c && cStaticRenames && cStaticRenames->count(name) && !(i > 0 && (result[i - 1] == '.' || result[i - 1] == '>')))
            {
              next += cStaticRenames->at(name);
              i = j;
              continue;
            }
            if (in.c && !objectMacros.empty())
              if (auto object = objectMacros.find(name); object != objectMacros.end() && macroHoldsAt(object->first, useOffset) && !cOwnConstant(name))
              {
                // C: an object macro inside an expansion too, after `.` as well (`tree[n].Freq` in
                // trees.c's smaller(tree, n, m, depth))
                next += object->second;
                changed = true;
                i = j;
                continue;
              }
            if (cMacros && !(i > 0 && (result[i - 1] == '.' || result[i - 1] == '>')))
            {
              // C's macros that are nothing, one keyword or their argument, inside an expansion as
              // in the text (FLUSH_BLOCK's `Tracev((stderr,"[FLUSH]"));`)
              size_t open = j;
              while (open < result.size() && (result[open] == ' ' || result[open] == '\t'))
                ++open;
              bool called = open < result.size() && result[open] == '(';
              size_t close = called ? closingParen(result, open) : std::string::npos;
              if (cMacros->empty.count(name) && !called)
              {
                changed = true;
                i = j;
                continue;
              }
              if (auto keyword = cMacros->keyword.find(name); keyword != cMacros->keyword.end())
              {
                next += keyword->second;
                changed = true;
                i = j;
                continue;
              }
              if (cMacros->emptyCall.count(name) && close != std::string::npos)
              {
                changed = true;
                i = close + 1;
                continue;
              }
              if (cMacros->identity.count(name) && close != std::string::npos)
              {
                next += result.substr(open + 1, close - open - 1);
                changed = true;
                i = close + 1;
                continue;
              }
            }
            const LocalMacro* inner = localMacroAt(name, useOffset); // the definition in force where the outer macro is used
            if (inner && j < result.size() && result[j] == '(')
            {
              // Parse arguments from the text.
              size_t k = j + 1;
              int depth = 0;
              std::vector<std::string> innerArgs;
              size_t argStart = k;
              bool closed = false;
              for (; k < result.size(); ++k)
              {
                if (result[k] == '(' || result[k] == '[' || result[k] == '{')
                  ++depth;
                else if (result[k] == ')' || result[k] == ']' || result[k] == '}')
                {
                  if (depth == 0) { closed = result[k] == ')'; break; }
                  --depth;
                }
                else if (result[k] == ',' && depth == 0)
                {
                  innerArgs.push_back(result.substr(argStart, k - argStart));
                  argStart = k + 1;
                }
                else if (size_t raw = rawStringEnd(result, k); raw != std::string_view::npos)
                  k = raw - 1;
                else if (result[k] == '"')
                {
                  ++k;
                  while (k < result.size() && result[k] != '"') { if (result[k] == '\\') ++k; ++k; }
                }
              }
              if (closed && (k > argStart || !innerArgs.empty()))
                innerArgs.push_back(result.substr(argStart, k - argStart));
              // Trim whitespace from args.
              for (auto& a : innerArgs)
              {
                while (!a.empty() && isspace((unsigned char)a.front())) a.erase(a.begin());
                while (!a.empty() && isspace((unsigned char)a.back())) a.pop_back();
              }
              if (closed && argumentsFit(*inner, innerArgs.size()))
              {
                bool tagged = false;
                for (const std::string& a : innerArgs)
                  tagged = tagged || a.find('\x01') != std::string::npos;
                if (tagged)
                {
                  std::vector<std::string> rewritten, spelled;
                  for (const std::string& a : innerArgs)
                  {
                    rewritten.push_back(untagged(a, true));
                    spelled.push_back(untagged(a, false));
                  }
                  next += expandMacroOnce(*inner, rewritten, &spelled, useOffset);
                }
                else
                  next += expandMacroOnce(*inner, innerArgs);
                changed = true;
                i = k + 1;
                continue;
              }
            }
            auto kf = knownFunctionMacros.find(name);
            size_t close = kf != knownFunctionMacros.end() && j < result.size() && result[j] == '(' ? closingParen(result, j) : std::string::npos;
            if (close != std::string::npos)
            {
              // As in convertTokens: $1 stands for the argument text.
              size_t dollar = kf->second.find("$1");
              if (dollar == std::string::npos)
                next += withFileAndLine(kf->second, useOffset);
              else
                next += withFileAndLine(kf->second.substr(0, dollar), useOffset) + result.substr(j + 1, close - j - 1) +
                        withFileAndLine(kf->second.substr(dollar + 2), useOffset);
              changed = true;
              i = close + 1;
              continue;
            }
            auto am = argumentMacros.find(name);
            close = am != argumentMacros.end() && j < result.size() && result[j] == '(' ? closingParen(result, j) : std::string::npos;
            if (close != std::string::npos)
            {
              // The arguments are text here already: $N and #N both come from it.
              std::vector<std::string> args;
              int depth = 0;
              size_t argStart = j + 1;
              for (size_t k = j + 1; k <= close; ++k)
              {
                char c = result[k];
                if (size_t raw = rawStringEnd(result, k); raw != std::string_view::npos)
                  k = raw - 1;
                else if (c == '"' || (c == '\'' && !isDigitSeparator(result, k)))
                  for (++k; k < close && result[k] != c; ++k)
                    k += result[k] == '\\';
                else if (c == '(' || c == '[' || c == '{')
                  ++depth;
                else if ((c == ')' || c == ']' || c == '}') && depth > 0)
                  --depth;
                else if ((c == ',' && depth == 0) || k == close)
                {
                  std::string a = result.substr(argStart, k - argStart);
                  while (!a.empty() && isspace((unsigned char)a.front()))
                    a.erase(a.begin());
                  while (!a.empty() && isspace((unsigned char)a.back()))
                    a.pop_back();
                  args.push_back(a);
                  argStart = k + 1;
                }
              }
              if (args.size() == arityOf(am->second))
              {
                next += expandArguments(am->second, args, args);
                changed = true;
                i = close + 1;
                continue;
              }
            }
            // A string, name or spliced macro (lstring.h's `luaS_newliteral(L, MEMERRMSG)` gives
            // `"" MEMERRMSG`), which convertTokens writes out at the uses it sees, but not in here.
            // Another name for a function is one before a call too: luaconf.h's lua_number2str is
            // `sprintf(...)`, which override_printf.h's `#define sprintf trio_sprintf` makes trio's
            // in Lua's C++ build (the C runtime's printed 0/0 as `-nan(ind)`, trio `nan`).
            if (auto om = objectMacros.find(name); om != objectMacros.end() &&
                (!(j < result.size() && result[j] == '(') || isAliasMacro(om->first, om->second)) &&
                !(i > 0 && (result[i - 1] == '.' || (result[i - 1] == '>' && i > 1 && result[i - 2] == '-') ||
                            (result[i - 1] == ':' && i > 1 && result[i - 2] == ':'))) &&
                macroHoldsAt(om->first, useOffset) && !cOwnConstant(om->first))
            {
              next += om->second;
              changed = true;
              i = j;
              continue;
            }
            // The object-like names the converter rewrites (SDL_PRIs64 in a LOG_GRAPHICS_DEBUG
            // argument, FAR, ...), which the token-level rewrite does not see inside the expansion.
            if (auto km = knownMacros.find(name); km != knownMacros.end() && !(j < result.size() && result[j] == '('))
            {
              next += km->second;
              changed = true;
              i = j;
              continue;
            }
            next.append(result, i, j - i);
            i = j;
            continue;
          }
          if (result[i] == '/' && i + 1 < result.size() && (result[i + 1] == '*' || result[i + 1] == '/'))
          {
            // A comment goes as it is: no macro is used inside it, and its words are no literals
            // (DrawQueue's `/* but I'm not sure ... */` took the rest of its expansion for a character
            // literal, and the PUSH_TO_SPRITE_LIST after it stayed unexpanded).
            size_t k = result[i + 1] == '*' ? result.find("*/", i + 2) : result.find('\n', i + 2);
            k = k == std::string::npos ? result.size() : k + (result[i + 1] == '*' ? 2 : 0);
            next.append(result, i, k - i);
            i = k;
            continue;
          }
          if (result[i] == '"' || (result[i] == '\'' && !isDigitSeparator(result, i)))
          {
            // A string or character literal goes as it is: no macro is used inside it.
            size_t k = i + 1;
            if (size_t raw = rawStringEnd(result, i); raw != std::string_view::npos)
              k = raw - 1;
            while (k < result.size() && result[k] != result[i])
              k += result[k] == '\\' ? 2 : 1;
            k = std::min(k + 1, result.size());
            next.append(result, i, k - i);
            i = k;
            continue;
          }
          next += result[i++];
        }
        if (!changed)
          break;
        result = std::move(next);
      }
      return result;
    }

    // The names of a scoped library in text that never was tokens, qualified as convertTokens
    // qualifies them in code (design.md 16): a function-like macro's body through an alias macro,
    // Lua's `lua_number2str(s,n)` writing `sprintf` that override_printf.h made `trio_sprintf`, is
    // `trio::trio_sprintf`. Literals, members (after `.` or `->`) and qualified names stay.
    std::string qualifyScopedNames(const std::string& s)
    {
      if (!scopedLibraryNames || scopedLibraryNames->empty())
        return s;
      std::string out;
      for (size_t i = 0; i < s.size();)
      {
        char c = s[i];
        if (c == '"' || (c == '\'' && !isDigitSeparator(s, i)))
        {
          size_t j = i + 1;
          while (j < s.size() && s[j] != c)
            j += s[j] == '\\' ? 2 : 1;
          j = std::min(j + 1, s.size());
          out.append(s, i, j - i);
          i = j;
          continue;
        }
        if (!(isalpha((unsigned char)c) || c == '_') || (i > 0 && isIdentChar(s[i - 1])))
        {
          out += c;
          ++i;
          continue;
        }
        size_t j = i;
        while (j < s.size() && isIdentChar(s[j]))
          ++j;
        std::string word = s.substr(i, j - i);
        size_t p = out.size();
        while (p > 0 && out[p - 1] == ' ')
          --p;
        bool reached = p > 0 && (out[p - 1] == '.' || (out[p - 1] == '>' && p > 1 && out[p - 2] == '-') || (out[p - 1] == ':' && p > 1 && out[p - 2] == ':'));
        auto library = reached ? scopedLibraryNames->end() : scopedLibraryNames->find(word);
        if (library != scopedLibraryNames->end())
        {
          out += library->second + "::";
          counts.add("names of a scoped library qualified");
        }
        out += word;
        i = j;
      }
      return out;
    }
    // The game's classes whose names the library uses, renamed in text that never was tokens; in C,
    // and this file's renamed statics and private names (cStaticRenames) too: trio's math.c names
    // its ieee_double_shape_type in its EXTRACT_WORDS macros, whose expansions are such text.
    std::string renameGameClasses(const std::string& s)
    {
      std::string renamed = renameGameClassNames(qualifyScopedNames(s), &counts);
      if (!in.c || !cStaticRenames || cStaticRenames->empty())
        return renamed;
      std::string out;
      for (size_t i = 0; i < renamed.size();)
      {
        char c = renamed[i];
        if (c == '"' || (c == '\'' && !isDigitSeparator(renamed, i)))
        {
          size_t j = i + 1;
          while (j < renamed.size() && renamed[j] != c)
            j += renamed[j] == '\\' ? 2 : 1;
          j = std::min(j + 1, renamed.size());
          out.append(renamed, i, j - i);
          i = j;
          continue;
        }
        if (!(isalpha((unsigned char)c) || c == '_'))
        {
          out += c;
          ++i;
          continue;
        }
        size_t j = i;
        while (j < renamed.size() && isIdentChar(renamed[j]))
          ++j;
        std::string word = renamed.substr(i, j - i);
        size_t p = out.size();
        while (p > 0 && out[p - 1] == ' ')
          --p;
        bool member = p > 0 && (out[p - 1] == '.' || (out[p - 1] == '>' && p > 1 && out[p - 2] == '-'));
        auto r = member ? cStaticRenames->end() : cStaticRenames->find(word);
        out += r != cStaticRenames->end() ? r->second : word;
        i = j;
      }
      return out;
    }
    // A macro's expansion with the game's classes renamed in the text that came from its body, not in
    // the arguments put in with their rewrites (between \x04 and \x05, which go): their names had the
    // renames of code, in their context (a std::vector there is the library's Vector already, no
    // game class Vector).
    std::string renameOutsideArguments(const std::string& s)
    {
      if (s.find('\x04') == std::string::npos)
        return this->renameGameClasses(s);
      std::string out, pending;
      int depth = 0;
      for (char c : s)
        if (c == '\x04' || c == '\x05')
        {
          if (depth == 0 && c == '\x04')
          {
            out += this->renameGameClasses(pending);
            pending.clear();
          }
          depth += c == '\x04' ? 1 : -1;
        }
        else if (depth > 0)
          out += c;
        else
          pending += c;
      return out + this->renameGameClasses(pending);
    }

    bool macroHoldsAt(const std::string& name, uint32_t offset) const
    {
      auto range = ownMacroRanges.find(name);
      return range == ownMacroRanges.end() || (range->second.first <= offset && offset < range->second.second);
    }

    // The name at token i is a member reached through `.`, or a method called through `->`
    // (`immediateContext->Map(...)`, a COM method): not the game's class of that name, so none of the
    // renames for those apply. (After `->` a name not called is a trailing return type, `-> Vector`.)
    // Whether toks[i] is the name a declaration declares rather than one it uses: after a class
    // key, `using`, `namespace` or `goto`, or after a type (a name, `>`, a pointer or reference)
    // and before what follows a declarator (`Set set(...)`: set, not Set).
    bool declaredNameAt(uint32_t i) const
    {
      if (i == 0)
        return false;
      const Token& before = toks[i - 1];
      if (before.is(Kw::class_) || before.is(Kw::struct_) || before.is(Kw::union_) || before.is(Kw::enum_) || before.is(Kw::using_) ||
          before.is(Kw::namespace_) || before.is(Kw::goto_))
        return true;
      bool afterType = before.getKind() == TokenKind::Identifier || before.is(P::Greater) || before.is(P::Shr) || before.is(P::Star) ||
                       before.is(P::Amp) || before.is(P::AndAnd) ||
                       (before.getKind() == TokenKind::Keyword &&
                        (before.is(Kw::void_) || before.is(Kw::bool_) || before.is(Kw::char_) || before.is(Kw::char8_t_) || before.is(Kw::char16_t_) ||
                         before.is(Kw::char32_t_) || before.is(Kw::wchar_t_) || before.is(Kw::short_) || before.is(Kw::int_) || before.is(Kw::long_) ||
                         before.is(Kw::float_) || before.is(Kw::double_) || before.is(Kw::signed_) || before.is(Kw::unsigned_) || before.is(Kw::auto_)));
      return afterType && i + 1 < toks.size() &&
             (toks[i + 1].is(P::LParen) || toks[i + 1].is(P::Semi) || toks[i + 1].is(P::Assign) || toks[i + 1].is(P::LBrace) ||
              toks[i + 1].is(P::LBracket) || toks[i + 1].is(P::Comma) || toks[i + 1].is(P::Colon) || toks[i + 1].is(P::RParen));
    }
    bool memberAccessAt(uint32_t i) const
    {
      return i > 0 && (toks[i - 1].is(P::Dot) || (toks[i - 1].is(P::Arrow) && i + 1 < toks.size() && toks[i + 1].is(P::LParen)));
    }

    // A printf-style format's conversions, in the order of the arguments they read: the conversion
    // character ('*' for a width or precision given as an argument) and whether it reads a 64-bit
    // integer (the z, j, t, ll and I64 sizes of d i u x X o).
    struct FormatConversion
    {
      char conversion;
      bool wide;
    };
    static std::vector<FormatConversion> formatConversions(const std::string& format)
    {
      std::vector<FormatConversion> out;
      for (size_t k = 0; k < format.size(); ++k)
      {
        if (format[k] != '%')
          continue;
        if (++k < format.size() && format[k] == '%')
          continue;
        while (k < format.size() && strchr("-+ #0'", format[k]))
          ++k;
        for (int part = 0; part < 2; ++part) // the width, then the precision after a `.`
        {
          if (part == 1 && !(k < format.size() && format[k] == '.'))
            break;
          if (part == 1)
            ++k;
          if (k < format.size() && format[k] == '*')
          {
            out.push_back({'*', false});
            ++k;
          }
          while (k < format.size() && isdigit((unsigned char)format[k]))
            ++k;
        }
        bool isWide = false;
        if (format.compare(k, 3, "I64") == 0)
          isWide = true, k += 3;
        else if (format.compare(k, 3, "I32") == 0)
          k += 3;
        else if (format.compare(k, 2, "ll") == 0)
          isWide = true, k += 2;
        else if (k < format.size() && strchr("zjt", format[k]))
          isWide = true, ++k;
        else
          while (k < format.size() && strchr("hlLqI", format[k]))
            ++k;
        if (k >= format.size())
          break;
        out.push_back({format[k], isWide && strchr("diuxXo", format[k]) != nullptr});
      }
      return out;
    }
    // The format at token i: adjacent literals and the string macros between them (`"%s "
    // UTF8_MULTIPLY " %zu"`), into format; returns its last token.
    uint32_t formatAt(uint32_t i, std::string& format)
    {
      uint32_t last = i;
      for (; last < toks.size(); ++last)
        if (toks[last].getKind() == TokenKind::String)
          format += spelling(last);
        else if (auto macro = toks[last].getKind() == TokenKind::Identifier ? objectMacros.find(std::string(spelling(last))) : objectMacros.end();
                 macro != objectMacros.end() && isStringLiterals(macro->second))
          format += macro->second;
        else
          break;
      return last - 1;
    }
    // The token ranges [first, end) of the arguments from token at to the call's closing
    // parenthesis, by depth; the last range's end is that parenthesis.
    std::vector<std::pair<uint32_t, uint32_t>> argumentRanges(uint32_t at)
    {
      std::vector<std::pair<uint32_t, uint32_t>> out;
      while (at < toks.size())
      {
        uint32_t end = at;
        for (int depth = 0; end < toks.size(); ++end)
          if (toks[end].is(P::LParen) || toks[end].is(P::LBracket) || toks[end].is(P::LBrace))
            ++depth;
          else if (toks[end].is(P::RParen) || toks[end].is(P::RBracket) || toks[end].is(P::RBrace))
          {
            if (depth-- == 0)
              break;
          }
          else if (toks[end].is(P::Comma) && depth == 0)
            break;
        if (end == at || end >= toks.size())
          break;
        out.push_back({at, end});
        if (!toks[end].is(P::Comma))
          break;
        at = end + 1;
      }
      return out;
    }
    // Writes the argument [first, end) as `type(argument)`. The argument's last token ends with the
    // parenthesis in place of itself: an insert after it would fall where a macro's rewrite of its
    // own closing parenthesis takes it (`LOG_AND_ABORT("%zu", directionCount)`); a one-token
    // argument is replaced whole, as an insert at its start would be taken by the replacement. A
    // second rewrite of that token would leave the parentheses unbalanced, which the build reports.
    void castArgument(uint32_t first, uint32_t end, const std::string& type)
    {
      uint32_t last = end - 1;
      if (last == first)
        replace(toks[first].offset, toks[first].length, type + "(" + std::string(spelling(first)) + ")");
      else
      {
        insert(toks[first].offset, type + "(");
        replace(toks[last].offset, toks[last].length, std::string(spelling(last)) + ")");
      }
    }
    // A printf-style format at token i (a string literal, or adjacent ones, that is an argument and
    // has a `%`): each argument a 64-bit conversion reads (%zu, %llu, %I64u and their d/x kin) is
    // written size_t(...), the size_t C++ passes there, which a container size's int (kpp's
    // library) is not: the vararg would carry 32 bits where printf reads 64. By the format, not by
    // types, so that it holds where the converter has none (templates' instances, fixtures'
    // members). Returns the last token of the literal.
    uint32_t widenFormatArguments(uint32_t i)
    {
      std::string format;
      uint32_t last = formatAt(i, format);
      if (format.find('%') == std::string::npos || last + 1 >= toks.size() || !toks[last + 1].is(P::Comma))
        return last;
      std::vector<FormatConversion> conversions = formatConversions(format);
      std::vector<std::pair<uint32_t, uint32_t>> arguments = argumentRanges(last + 2);
      for (size_t n = 0; n < conversions.size() && n < arguments.size(); ++n)
      {
        auto [at, end] = arguments[n];
        bool cast = toks[at].getKind() == TokenKind::Identifier && spelling(at) == "size_t" && toks[at + 1].is(P::LParen);
        if (conversions[n].wide && !cast)
        {
          { size_t before = edits.size(); castArgument(at, end, "size_t"); for (size_t e = before; e < edits.size(); ++e) widenedFormats[i].push_back(e); }
          counts.add("64-bit printf arguments written size_t");
        }
      }
      return last;
    }
    // Whether the declarations say an expression is a 64-bit integer: a variable, member, parameter
    // or function result of such a type (size_t, int64_t, uint64_t, long long, and aliases of them:
    // UnitNumber), arithmetic with one, a cast to one, a literal suffixed so.
    bool isWideIntegerType(Type* type, int depth = 0)
    {
      while (type && (type->kind == NodeKind::ConstType || type->kind == NodeKind::LRefType || type->kind == NodeKind::RRefType))
        type = type->kind == NodeKind::ConstType ? static_cast<ConstType*>(type)->inner
             : type->kind == NodeKind::LRefType  ? static_cast<LRefType*>(type)->referenced
                                                 : static_cast<RRefType*>(type)->referenced;
      if (!type || depth > 8)
        return false;
      if (type->kind == NodeKind::BuiltinType)
        return static_cast<BuiltinType*>(type)->longCount >= 2;
      if (type->kind != NodeKind::NamedType)
        return false;
      auto* named = static_cast<NamedType*>(type);
      const QualifiedName& name = named->name;
      if (!name.parts.empty() && name.last().symbol != UINT32_MAX)
      {
        static const std::set<std::string_view> wide = {"size_t", "int64_t", "uint64_t", "ptrdiff_t", "intptr_t", "uintptr_t", "ssize_t", "time_t"};
        if (wide.count(symbols.name(name.last().symbol)))
          return true;
      }
      Decl* d = named->resolved;
      // `typename container_type::size_type` (FlatSet's, over a std::vector) and the like, which the
      // C++ parse does not resolve: a std container's size_type is size_t, its difference_type
      // ptrdiff_t, whatever width the library's sizes have.
      if (!d && name.parts.size() >= 2 && name.last().symbol != UINT32_MAX)
      {
        std::string_view last = symbols.name(name.last().symbol);
        if (last == "size_type" || last == "difference_type")
          return true;
      }
      if (d && d->kind == NodeKind::UsingAliasDecl)
        return isWideIntegerType(static_cast<UsingAliasDecl*>(d)->type, depth + 1);
      if (d && d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef))
        return isWideIntegerType(static_cast<VariableDecl*>(d)->type, depth + 1);
      return false;
    }
    bool isWideInteger(Expr* e, int depth = 0)
    {
      while (e && e->kind == NodeKind::ParenExpr)
        e = static_cast<ParenExpr*>(e)->inner;
      if (!e || depth > 16)
        return false;
      switch (e->kind)
      {
        case NodeKind::LiteralExpr:
        {
          std::string_view text = spelling(e->token);
          return static_cast<LiteralExpr*>(e)->literalKind == TokenKind::Number &&
                 (text.find("ll") != std::string_view::npos || text.find("LL") != std::string_view::npos || text.find("i64") != std::string_view::npos);
        }
        case NodeKind::NameExpr:
        case NodeKind::MemberExpr:
          return isWideIntegerType(typeOfExpr(e, 0).type);
        case NodeKind::CallExpr:
        {
          // A std container's size, a size_t in C++ (TestLuaEvents' `%u` of getChunks().size()):
          // written uint32_t(...), as C++ reads it, whichever width the library's sizes have.
          if (isStdSizeCall(e))
            return true;
          Expr* callee = static_cast<CallExpr*>(e)->callee;
          Decl* f = callee->kind == NodeKind::NameExpr ? static_cast<NameExpr*>(callee)->resolved
                  : callee->kind == NodeKind::MemberExpr ? static_cast<MemberExpr*>(callee)->resolved
                                                         : nullptr;
          return f && f->kind == NodeKind::FunctionDecl && isWideIntegerType(static_cast<FunctionDecl*>(f)->returnType);
        }
        case NodeKind::BinaryExpr:
        {
          auto* binary = static_cast<BinaryExpr*>(e);
          if (binary->isKeyword)
            return false;
          switch (P(binary->op))
          {
            case P::Plus: case P::Minus: case P::Star: case P::Slash: case P::Percent: case P::Amp: case P::Pipe: case P::Caret:
              return isWideInteger(binary->lhs, depth + 1) || isWideInteger(binary->rhs, depth + 1);
            case P::Shl: case P::Shr: case P::Assign: case P::PlusAssign: case P::MinusAssign:
              return isWideInteger(binary->lhs, depth + 1);
            default:
              return false;
          }
        }
        case NodeKind::UnaryExpr:
        {
          auto* unary = static_cast<UnaryExpr*>(e);
          return !unary->isKeyword && (P(unary->op) == P::Minus || P(unary->op) == P::Tilde || P(unary->op) == P::Plus ||
                                       P(unary->op) == P::PlusPlus || P(unary->op) == P::MinusMinus) && isWideInteger(unary->operand, depth + 1);
        }
        case NodeKind::ConditionalExpr:
        {
          auto* c = static_cast<ConditionalExpr*>(e);
          return (c->then && isWideInteger(c->then, depth + 1)) || isWideInteger(c->else_, depth + 1);
        }
        case NodeKind::CastExpr: return isWideIntegerType(static_cast<CastExpr*>(e)->type);
        case NodeKind::NamedCastExpr: return isWideIntegerType(static_cast<NamedCastExpr*>(e)->type);
        case NodeKind::FunctionalCastExpr: return isWideIntegerType(static_cast<FunctionalCastExpr*>(e)->type);
        default: return false;
      }
    }
    // design.md 14's printf rewrite: a call of ssprintf, or a printf statement, with a literal format
    // becomes toString(...) or print(...) of pieces, the text C printed: the format's text between
    // its conversions as literals, each conversion its argument as the piece that writes it so
    // (`%u` uint32_t(x), the 32 bits printf reads; `%s` StringView(p), or the String a c_str() came
    // from; `%5.2f` alignRight(fixed(x, 2), 5); `%08x` hex(x, 8); `%p` address(p)). A conversion
    // outside that leaves the call as it is (counted): `*` widths, precision on integers and
    // strings, the ' ' and '#' flags but for `%#0Nx`, %E %G %a %n, wide strings. A `*` width is
    // the argument before the value, which the piece takes as its width (LuaHelper's `%*c`).
    struct PrintfConversion
    {
      std::string flags, length;
      int width = -1, precision = -1;
      bool starWidth = false; // the width is an argument (`%*c`), read before the value
      char conversion = 0;
    };
    // The format's raw text (its literals' contents, escapes as written) cut at its conversions:
    // segments[k] before conversions[k], the last after the last. False when it holds a conversion
    // the rewrite does not write (unsupported names it).
    static bool splitPrintfFormat(const std::string& raw, bool rawLiteral, std::vector<std::string>& segments, std::vector<PrintfConversion>& conversions,
                                  std::string& unsupported)
    {
      std::string segment;
      for (size_t k = 0; k < raw.size(); ++k)
      {
        if (raw[k] == '\\' && k + 1 < raw.size() && !rawLiteral)
        {
          segment += raw.substr(k, 2);
          ++k;
          continue;
        }
        if (raw[k] != '%')
        {
          segment += raw[k];
          continue;
        }
        if (k + 1 < raw.size() && raw[k + 1] == '%')
        {
          segment += '%';
          ++k;
          continue;
        }
        PrintfConversion c;
        size_t p = k + 1;
        while (p < raw.size() && strchr("-+ #0'", raw[p]))
          c.flags += raw[p++];
        if (p < raw.size() && raw[p] == '*')
        {
          c.starWidth = true;
          ++p;
        }
        if (p < raw.size() && isdigit((unsigned char)raw[p]))
        {
          c.width = 0;
          while (p < raw.size() && isdigit((unsigned char)raw[p]))
            c.width = c.width * 10 + (raw[p++] - '0');
        }
        if (p < raw.size() && raw[p] == '.')
        {
          ++p;
          if (p < raw.size() && raw[p] == '*')
          {
            unsupported = "`*` precision";
            return false;
          }
          c.precision = 0;
          while (p < raw.size() && isdigit((unsigned char)raw[p]))
            c.precision = c.precision * 10 + (raw[p++] - '0');
        }
        for (const char* length : {"I64", "I32", "hh", "ll", "h", "l", "z", "j", "t", "L", "q"})
          if (raw.compare(p, strlen(length), length) == 0)
          {
            c.length = length;
            p += strlen(length);
            break;
          }
        if (p >= raw.size())
        {
          unsupported = "a format ending in a conversion's middle";
          return false;
        }
        c.conversion = raw[p];
        k = p;
        if (!strchr("diuxXocsfegp", c.conversion) || c.flags.find(' ') != std::string::npos || c.flags.find('\'') != std::string::npos ||
            (c.length == "L") || (c.length == "l" && (c.conversion == 's' || c.conversion == 'c')))
        {
          unsupported = std::string("%") + c.flags + (c.length) + c.conversion;
          return false;
        }
        if (c.precision >= 0 && strchr("diuxXocsp", c.conversion))
        {
          unsupported = "precision on %" + std::string(1, c.conversion);
          return false;
        }
        if (c.flags.find('#') != std::string::npos && !((c.conversion == 'x' || c.conversion == 'X') && c.flags.find('0') != std::string::npos && c.width > 2))
        {
          unsupported = "the # flag";
          return false;
        }
        segments.push_back(segment);
        segment.clear();
        conversions.push_back(c);
      }
      segments.push_back(segment);
      return true;
    }
    // An integer type's exact name, when the declarations give it: int32_t for int, uint32_t for
    // unsigned or uint32_t, int64_t for long long, char for char ("" otherwise or when unknown).
    std::string exactIntegerName(Type* type, int depth = 0)
    {
      while (type && (type->kind == NodeKind::ConstType || type->kind == NodeKind::LRefType || type->kind == NodeKind::RRefType))
        type = type->kind == NodeKind::ConstType ? static_cast<ConstType*>(type)->inner
             : type->kind == NodeKind::LRefType  ? static_cast<LRefType*>(type)->referenced
                                                 : static_cast<RRefType*>(type)->referenced;
      if (!type || depth > 8)
        return {};
      if (type->kind == NodeKind::BuiltinType)
      {
        auto* b = static_cast<BuiltinType*>(type);
        bool isUnsigned = b->isUnsigned || b->keyword == Kw::unsigned_;
        if (b->keyword == Kw::char_)
          return b->isUnsigned || b->isSigned ? std::string() : "char";
        if (b->keyword == Kw::short_)
          return isUnsigned ? "uint16_t" : "int16_t";
        if (b->keyword == Kw::int_ || b->keyword == Kw::unsigned_ || b->keyword == Kw::signed_ || b->keyword == Kw::long_)
          return std::string(isUnsigned ? "uint" : "int") + (b->longCount >= 2 ? "64_t" : "32_t");
        return {};
      }
      if (type->kind != NodeKind::NamedType)
        return {};
      auto* named = static_cast<NamedType*>(type);
      if (!named->name.parts.empty() && named->name.last().symbol != UINT32_MAX)
      {
        static const std::map<std::string_view, std::string> names = {
          {"int8_t", "int8_t"},   {"uint8_t", "uint8_t"},   {"int16_t", "int16_t"}, {"uint16_t", "uint16_t"}, {"int32_t", "int32_t"},
          {"uint32_t", "uint32_t"}, {"int64_t", "int64_t"}, {"uint64_t", "uint64_t"}, {"size_t", "size_t"}};
        if (auto n = names.find(symbols.name(named->name.last().symbol)); n != names.end())
          return n->second;
      }
      Decl* d = named->resolved;
      if (d && d->kind == NodeKind::UsingAliasDecl)
        return exactIntegerName(static_cast<UsingAliasDecl*>(d)->type, depth + 1);
      if (d && d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef))
        return exactIntegerName(static_cast<VariableDecl*>(d)->type, depth + 1);
      return {};
    }
    // The opening and closing text around an argument that make it the piece a conversion writes.
    bool printfPiece(const PrintfConversion& c, Expr* arg, uint32_t argEnd, bool trio, std::string& open, std::string& close, uint32_t& cStrDot,
                     const std::string& widthText = std::string())
    {
      // (a `*` width: its argument's text; with the `0` flag, which pads after a sign, none)
      if (c.starWidth && (widthText.empty() || c.flags.find('0') != std::string::npos))
        return false;
      Expr* plain = arg;
      while (plain && plain->kind == NodeKind::ParenExpr)
        plain = static_cast<ParenExpr*>(plain)->inner;
      std::string exact = arg ? exactIntegerName(plainTypeOf(arg)) : std::string();
      // (`int32_t(value)`, which the parse reads as a call of the type's name)
      if (exact.empty() && plain && plain->kind == NodeKind::CallExpr && static_cast<CallExpr*>(plain)->args.size() == 1 &&
          static_cast<CallExpr*>(plain)->callee->kind == NodeKind::NameExpr)
      {
        auto* typeName = static_cast<NameExpr*>(static_cast<CallExpr*>(plain)->callee);
        if (!typeName->resolved || typeName->resolved->kind != NodeKind::FunctionDecl)
        {
          NamedType named{};
          named.kind = NodeKind::NamedType;
          named.name = typeName->name;
          named.resolved = typeName->resolved;
          exact = exactIntegerName(&named);
        }
      }
      bool literalInteger = plain && plain->kind == NodeKind::LiteralExpr && static_cast<LiteralExpr*>(plain)->literalKind == TokenKind::Number &&
                            isdigit((unsigned char)spelling(plain->token)[0]) && spelling(plain->token).find('.') == std::string_view::npos;
      auto integerType = [&](bool isSigned)
      {
        std::string bits = c.length == "ll" || c.length == "I64" || c.length == "j" || c.length == "q" || c.length == "z" || c.length == "t" ? "64"
                         : c.length == "h"                                                                                                 ? "16"
                         : c.length == "hh"                                                                                                ? "8"
                                                                                                                                           : "32";
        if (c.length == "z" && !isSigned)
          return std::string("size_t");
        return std::string(isSigned ? "int" : "uint") + bits + "_t";
      };
      std::string valueOpen, valueClose;
      switch (c.conversion)
      {
        case 'd':
        case 'i':
        case 'u':
        {
          std::string type = integerType(c.conversion != 'u');
          if (exact != type && !(literalInteger && (type == "int32_t" || type == "uint32_t")))
            valueOpen = type + "(", valueClose = ")";
          break;
        }
        case 'x':
        case 'X':
        case 'o':
        {
          std::string type = integerType(false);
          std::string name = c.conversion == 'x' ? "hex(" : c.conversion == 'X' ? "hexUpper(" : "octal(";
          valueOpen = name + (exact == type || literalInteger ? "" : type + "(");
          valueClose = std::string(exact == type || literalInteger ? "" : ")");
          if (c.flags.find('0') != std::string::npos && c.width > 0 && c.flags.find('-') == std::string::npos)
          {
            bool prefix = c.flags.find('#') != std::string::npos;
            valueOpen = (prefix ? std::string(c.conversion == 'X' ? "\"0X\", " : "\"0x\", ") : std::string()) + valueOpen;
            valueClose += ", " + std::to_string(prefix ? c.width - 2 : c.width) + ")";
            open = valueOpen, close = valueClose;
            return true;
          }
          valueClose += ")";
          break;
        }
        case 'c':
          if (exact != "char")
            valueOpen = "char(", valueClose = ")";
          break;
        case 's':
          if (plain && plain->kind == NodeKind::LiteralExpr && static_cast<LiteralExpr*>(plain)->literalKind == TokenKind::String)
            break;
          if (plain && plain->kind == NodeKind::CallExpr && static_cast<CallExpr*>(plain)->args.empty() &&
              static_cast<CallExpr*>(plain)->callee->kind == NodeKind::MemberExpr)
          {
            auto* m = static_cast<MemberExpr*>(static_cast<CallExpr*>(plain)->callee);
            if (m->op == P::Dot && !m->member.parts.empty() && m->member.parts[0].symbol != UINT32_MAX &&
                symbols.name(m->member.parts[0].symbol) == "c_str" && isStdStringType(plainTypeOf(m->base)))
            {
              // (the String itself: the argument's own `.c_str()`, its last tokens, goes)
              uint32_t dot = argEnd >= 4 ? argEnd - 4 : 0;
              if (argEnd >= 4 && toks[dot].is(P::Dot) && spelling(dot + 1) == "c_str" && toks[dot + 2].is(P::LParen) && toks[dot + 3].is(P::RParen))
              {
                cStrDot = dot;
                break;
              }
            }
          }
          {
            // (a char pointer, or the converter knows no type: `(const char*)` as printf read it,
            // glGetString's `const GLubyte*`)
            Type* type = arg ? plainTypeOf(arg) : nullptr;
            while (type && type->kind == NodeKind::ConstType)
              type = static_cast<ConstType*>(type)->inner;
            bool charPointer = false;
            if (type && type->kind == NodeKind::PointerType)
            {
              Type* pointee = static_cast<PointerType*>(type)->pointee;
              while (pointee && pointee->kind == NodeKind::ConstType)
                pointee = static_cast<ConstType*>(pointee)->inner;
              charPointer = pointee && pointee->kind == NodeKind::BuiltinType && static_cast<BuiltinType*>(pointee)->keyword == Kw::char_ &&
                            !static_cast<BuiltinType*>(pointee)->isUnsigned && !static_cast<BuiltinType*>(pointee)->isSigned;
            }
            // (`x.c_str()` and `e.what()` are C text whatever x's class: a wide one fails to compile)
            bool cText = plain && plain->kind == NodeKind::CallExpr && static_cast<CallExpr*>(plain)->callee->kind == NodeKind::MemberExpr &&
                         !static_cast<MemberExpr*>(static_cast<CallExpr*>(plain)->callee)->member.parts.empty() &&
                         static_cast<MemberExpr*>(static_cast<CallExpr*>(plain)->callee)->member.parts[0].symbol != UINT32_MAX &&
                         (symbols.name(static_cast<MemberExpr*>(static_cast<CallExpr*>(plain)->callee)->member.parts[0].symbol) == "c_str" ||
                          symbols.name(static_cast<MemberExpr*>(static_cast<CallExpr*>(plain)->callee)->member.parts[0].symbol) == "what");
            bool text = charPointer || cText || (type && isStdStringType(type)) || (plain && plain->kind == NodeKind::ConditionalExpr);
            if (arg && !text)
            {
              valueOpen = "StringView((const char*)(", valueClose = "))";
              counts.add("printf %s of a pointer of another or unknown type written StringView((const char*)(x))");
            }
            else
              valueOpen = "StringView(", valueClose = ")";
          }
          break;
        case 'f':
        case 'e':
        case 'g':
        {
          int precision = c.precision < 0 ? 6 : c.precision;
          if (c.conversion == 'g' && precision == 0)
            precision = 1;
          // (ssprintf's text is trio's, CommonUtil's StringUtil.cpp: the game's TrioPieces.kpp)
          if (trio)
            valueOpen = std::string(c.conversion == 'f' ? "trioFixed(" : c.conversion == 'e' ? "trioScientific(" : "trioSignificant(");
          else
            valueOpen = std::string(c.conversion == 'f' ? "fixed(" : c.conversion == 'e' ? "scientific(" : "significant(");
          valueClose = ", " + std::to_string(precision) + ")";
          break;
        }
        case 'p':
          valueOpen = "address(", valueClose = ")";
          break;
        default: return false;
      }
      if (c.flags.find('+') != std::string::npos)
      {
        if (!strchr("difeg", c.conversion))
          return false;
        valueOpen = "withSign(" + valueOpen, valueClose += ")";
      }
      // (a `*` width's argument as the width: negative, it aligns left, as printf's does)
      std::string width = c.starWidth ? widthText : c.width > 0 ? std::to_string(c.width) : std::string();
      if (!width.empty())
      {
        if (c.flags.find('-') != std::string::npos)
          valueOpen = "alignLeft(" + valueOpen, valueClose += ", " + width + ")";
        else if (c.flags.find('0') != std::string::npos && strchr("diufeg", c.conversion))
          valueOpen = "zeroPadded(" + valueOpen, valueClose += ", " + width + ")";
        else
          valueOpen = "alignRight(" + valueOpen, valueClose += ", " + width + ")";
      }
      open = valueOpen, close = valueClose;
      return true;
    }
    // The printf rewrite in a macro's expansion (a local macro's text, written at its use), where
    // there are no expressions: the calls of ssprintf and of the game's format functions found by
    // name, each value written with its type (`int32_t(x)` for %d, `StringView(p)` for %s).
    std::string rewritePrintfText(const std::string& t, int depth = 0)
    {
      if (depth > 8 || (t.find("ssprintf") == std::string::npos && t.find("Logging::") == std::string::npos && t.find("printf") == std::string::npos &&
                                t.find("addDebugProperty") == std::string::npos && t.find("makeProperty") == std::string::npos &&
                                t.find("AbortException::make") == std::string::npos && t.find("TagWriter") == std::string::npos))
        return t;
      // a literal's end from its opening quote (raw ones too)
      auto literalEnd = [&](size_t q) -> size_t
      {
        if (t[q] == 'R' && q + 1 < t.size() && t[q + 1] == '"')
        {
          size_t open = t.find('(', q + 2);
          if (open == std::string::npos)
            return std::string::npos;
          std::string delimiter = t.substr(q + 2, open - q - 2);
          size_t close = t.find(")" + delimiter + "\"", open);
          return close == std::string::npos ? close : close + delimiter.size() + 2;
        }
        size_t e = q + 1;
        while (e < t.size() && t[e] != t[q])
          e += t[e] == '\\' ? 2 : 1;
        return e < t.size() ? e + 1 : std::string::npos;
      };
      std::string out;
      for (size_t i = 0; i < t.size();)
      {
        char c = t[i];
        if (c == '"' || c == '\'')
        {
          size_t e = literalEnd(i);
          if (e == std::string::npos)
            return t;
          out += t.substr(i, e - i);
          i = e;
          continue;
        }
        if (!(isalpha((unsigned char)c) || c == '_') || (i > 0 && isIdentChar(t[i - 1])))
        {
          out += c;
          ++i;
          continue;
        }
        size_t j = i;
        while (j < t.size() && isIdentChar(t[j]))
          ++j;
        std::string word = t.substr(i, j - i);
        size_t open = j;
        while (open < t.size() && t[open] == ' ')
          ++open;
        auto preceded = [&](const std::string& qualifier)
        { return out.size() >= qualifier.size() && out.compare(out.size() - qualifier.size(), qualifier.size(), qualifier) == 0; };
        size_t formatIndex = SIZE_MAX;
        bool member = out.size() >= 1 && (out.back() == '.' || (out.size() >= 2 && out.compare(out.size() - 2, 2, "->") == 0));
        // (printf and fprintf print through the C runtime, not trio; zstd's DISPLAY macro's
        // `fprintf(stderr, ...)`)
        bool printLike = (word == "printf" || word == "fprintf") && !member && !preceded("::");
        if (in.c && !printLike)
          formatIndex = SIZE_MAX;
        else if (printLike)
          formatIndex = word == "fprintf" ? 1 : 0;
        else if (word == "ssprintf" && !member)
          formatIndex = 0;
        else if ((word == "log" || word == "logVerbose" || word == "logAndAbortOrThrow") && preceded("Logging::"))
          formatIndex = 3;
        else if (word == "addDebugProperty" && member)
          formatIndex = 1;
        else if (word == "makeProperty" && preceded("PropertyItem::"))
          formatIndex = 1;
        else if (word == "make" && preceded("AbortException::"))
          formatIndex = 0;
        else if (word == "_" && preceded("TagWriter "))
          formatIndex = 2; // (SAVING_TAG's `Serialiser::TagWriter _(output, "tag", format, ...)`)
        if (formatIndex == SIZE_MAX || open >= t.size() || t[open] != '(')
        {
          out += word;
          i = j;
          continue;
        }
        // the arguments, at depth 0 of the call
        std::vector<std::pair<size_t, size_t>> args;
        size_t k = open + 1, start = k;
        int nest = 0;
        for (; k < t.size(); ++k)
        {
          char d = t[k];
          if (d == '"' || d == '\'' || (d == 'R' && k + 1 < t.size() && t[k + 1] == '"' && !(k > 0 && isIdentChar(t[k - 1]))))
          {
            size_t e = literalEnd(k);
            if (e == std::string::npos)
              return t;
            k = e - 1;
          }
          else if (d == '(' || d == '[' || d == '{')
            ++nest;
          else if (d == ')' || d == ']' || d == '}')
          {
            if (nest-- == 0)
              break;
          }
          else if (d == ',' && nest == 0)
          {
            args.push_back({start, k});
            start = k + 1;
          }
        }
        if (k >= t.size())
          return t;
        if (k > start || !args.empty())
          args.push_back({start, k});
        auto trimmed = [&](std::pair<size_t, size_t> a)
        {
          size_t b = a.first, e = a.second;
          while (b < e && isspace((unsigned char)t[b]))
            ++b;
          while (e > b && isspace((unsigned char)t[e - 1]))
            --e;
          return std::pair<size_t, size_t>{b, e};
        };
        // the call's arguments, each rewritten inside first
        std::vector<std::string> texts;
        for (auto a : args)
        {
          auto [b, e] = trimmed(a);
          texts.push_back(rewritePrintfText(t.substr(b, e - b), depth + 1));
        }
        std::string callee = word == "ssprintf" ? "toString" : printLike ? "print" : word;
        std::string rewritten;
        bool done = false;
        // (print returns no count: only a statement's printf, after `;`, `{`, `}` or `)` and before `;`)
        if (printLike)
        {
          size_t before = out.find_last_not_of(" \t\r\n"), after = t.find_first_not_of(" \t", k + 1);
          bool statement = (before == std::string::npos || strchr(";{}):", out[before])) && after != std::string::npos && t[after] == ';';
          if (!statement)
          {
            counts.add("printf calls in macro expansions left as they are (no statement)");
            formatIndex = SIZE_MAX;
          }
        }
        if (formatIndex < texts.size())
        {
          // the format: literals only (raw ones of one delimiter)
          const std::string& f = texts[formatIndex];
          std::string raw, rawDelimiter;
          int rawLiterals = 0;
          bool literals = !f.empty();
          for (size_t q = 0; q < f.size() && literals;)
          {
            if (isspace((unsigned char)f[q]))
            {
              ++q;
              continue;
            }
            if (f[q] == 'R' && q + 1 < f.size() && f[q + 1] == '"')
            {
              size_t o = f.find('(', q + 2);
              std::string delimiter = o == std::string::npos ? std::string() : f.substr(q + 2, o - q - 2);
              size_t close = o == std::string::npos ? o : f.find(")" + delimiter + "\"", o);
              if (close == std::string::npos || (rawLiterals == 0 && !raw.empty()) || (rawLiterals > 0 && delimiter != rawDelimiter))
              {
                literals = false;
                break;
              }
              rawDelimiter = delimiter;
              raw += f.substr(o + 1, close - o - 1);
              q = close + delimiter.size() + 2;
              ++rawLiterals;
              continue;
            }
            if (f[q] != '"' || rawLiterals > 0)
            {
              literals = false;
              break;
            }
            size_t e = q + 1;
            while (e < f.size() && f[e] != '"')
              e += f[e] == '\\' ? 2 : 1;
            raw += f.substr(q + 1, e - q - 1);
            q = e + 1;
          }
          std::vector<std::string> segments;
          std::vector<PrintfConversion> conversions;
          std::string unsupported;
          size_t following = texts.size() - formatIndex - 1;
          size_t starsHere = 0;
          auto literal = [&](const std::string& x)
          { return rawLiterals > 0 ? "R\"" + rawDelimiter + "(" + x + ")" + rawDelimiter + "\"" : "\"" + x + "\""; };
          if (literals && splitPrintfFormat(raw, rawLiterals > 0, segments, conversions, unsupported) &&
              conversions.size() + (starsHere = size_t(std::count_if(conversions.begin(), conversions.end(), [](const PrintfConversion& c) { return c.starWidth; }))) == following &&
              !(word != "ssprintf" && !printLike && conversions.empty() && following == 0 && raw.find('%') == std::string::npos))
          {
            std::vector<std::string> pieces;
            bool ok = true;
            for (size_t n = 0, a = formatIndex + 1; n < conversions.size() && ok; ++n)
            {
              std::string o, cl;
              uint32_t noDot = UINT32_MAX;
              std::string widthText = conversions[n].starWidth ? texts[a++] : std::string();
              const std::string& value = texts[a++];
              PrintfConversion conversion = conversions[n];
              if (conversion.conversion == 's' && !value.empty() && (value[0] == '"' || (value[0] == 'R' && value.size() > 1 && value[1] == '"')))
              {
                // (a literal: itself, aligned as asked)
                ok = printfPiece(conversion, nullptr, 0, !printLike, o, cl, noDot, widthText);
                if (ok && o.compare(0, 11, "StringView(") == 0)
                  o.erase(0, 11), cl.erase(0, 1);
                else if (ok && o.find("StringView(") != std::string::npos)
                {
                  size_t at = o.find("StringView(");
                  o.erase(at, 11);
                  cl.erase(0, 1);
                }
              }
              else
                ok = printfPiece(conversion, nullptr, 0, !printLike, o, cl, noDot, widthText);
              if (ok)
              {
                if (!segments[n].empty())
                  pieces.push_back(literal(segments[n]));
                pieces.push_back(o + value + cl);
              }
            }
            if (ok)
            {
              if (!segments.back().empty() || pieces.empty())
                pieces.push_back(literal(segments.back()));
              for (size_t a = 0; a < formatIndex; ++a)
                rewritten += texts[a] + ", ";
              for (size_t n = 0; n < pieces.size(); ++n)
                rewritten += (n ? ", " : "") + pieces[n];
              done = true;
              counts.add("printf calls in macro expansions written with pieces");
            }
          }
          else if (!literals && word != "ssprintf" && !printLike)
          {
            // (a run-time format, as rewritePrintfCall writes it)
            for (size_t a = 0; a < formatIndex; ++a)
              rewritten += texts[a] + ", ";
            if (following == 0)
              rewritten += "StringView(" + f + ")";
            else
            {
              rewritten += "ssprintf(" + f;
              for (size_t a = formatIndex + 1; a < texts.size(); ++a)
                rewritten += ", " + texts[a];
              rewritten += ")";
            }
            done = true;
            counts.add("format function calls of a run-time format in macro expansions written ssprintf(format, ...)");
          }
        }
        if (!done)
        {
          callee = word;
          rewritten.clear();
          for (size_t a = 0; a < texts.size(); ++a)
            rewritten += (a ? ", " : "") + texts[a];
          if (printLike)
            counts.add("printf calls in macro expansions left as they are");
          else if (word != "ssprintf")
            counts.add("format function calls in macro expansions left as they are");
        }
        out += callee + t.substr(j, open - j) + "(" + rewritten + ")";
        i = k + 1;
      }
      return out;
    }
    std::vector<Edit> backEdits; // inserts that go after every other edit at their offset
    std::map<uint32_t, std::vector<size_t>> widenedFormats; // a format's token: the edits widenFormatArguments made for it
    std::set<CallExpr*> printfRewritten;
    // The game's own format functions (`..., const char* format, ...)` formatting through
    // CommonUtil's vssprintf): each takes a Text instead (convertFormatFunction), its calls the
    // pieces. Recognized alike in every file: C-variadic, the last named parameter `format`, a name of
    // these.
    static bool isFormatFunctionName(std::string_view name)
    {
      static const std::set<std::string_view> names = {"log", "logVerbose", "logAndAbortOrThrow", "addDebugProperty", "makeProperty",
                                                       "make", "printf", "TagWriter"};
      return names.count(name) > 0;
    }
    bool isFormatFunction(Decl* d) const
    {
      if (!d || d->kind != NodeKind::FunctionDecl || !(d->flags & DF_Variadic))
        return false;
      auto* f = static_cast<FunctionDecl*>(d);
      if (f->name.empty() || f->name.last().symbol == UINT32_MAX || f->params.empty() || !isFormatFunctionName(symbols.name(f->name.last().symbol)))
        return false;
      Decl* last = f->params[f->params.size() - 1];
      return last->kind == NodeKind::ParamDecl && static_cast<ParamDecl*>(last)->name != UINT32_MAX &&
             symbols.name(static_cast<ParamDecl*>(last)->name) == "format";
    }
    // A format function's declaration or definition (isFormatFunction): `const char* format, ...` is
    // `Text text`, and its body's `va_list args; va_start(args, format); X = vssprintf(format, args);
    // va_end(args);` is `X = toString(text);`, where it was (TagWriter's after its early return: the
    // text is written only where the C++ wrote it). Another use of the format or its va_list is an
    // error, not a silent fallback.
    void convertFormatFunction(FunctionDecl* f)
    {
      uint32_t open = f->token;
      while (open < toks.size() && !toks[open].is(P::LParen))
        ++open;
      uint32_t ellipsis = UINT32_MAX, lastComma = UINT32_MAX, beforeFormat = open;
      for (uint32_t k = open, depth = 0; k < toks.size(); ++k)
      {
        if (toks[k].is(P::LParen))
          ++depth;
        else if (toks[k].is(P::RParen) && --depth == 0)
          break;
        else if (depth == 1 && toks[k].is(P::Comma))
        {
          if (lastComma != UINT32_MAX)
            beforeFormat = lastComma;
          lastComma = k;
        }
        else if (depth == 1 && toks[k].is(P::Ellipsis))
        {
          ellipsis = k;
          break;
        }
      }
      if (ellipsis == UINT32_MAX || lastComma == UINT32_MAX)
      {
        todo(toks[f->token].offset, "a format function whose `format, ...` the converter cannot find");
        return;
      }
      replace(toks[beforeFormat + 1].offset, endOffset(ellipsis) - toks[beforeFormat + 1].offset, "Text text");
      counts.add("format functions taking a Text");
      if (!f->body || f->endToken <= f->body->token || f->endToken > toks.size())
        return;
      uint32_t first = f->body->token, end = f->endToken;
      // the statements: va_list NAME; va_start(NAME, format); va_end(NAME);
      std::string list;
      auto removeStatement = [&](uint32_t from, uint32_t semicolon)
      {
        uint32_t start = toks[from].offset, stop = endOffset(semicolon);
        size_t lineStart = text.rfind('\n', start == 0 ? 0 : start - 1);
        lineStart = lineStart == std::string_view::npos ? 0 : lineStart + 1;
        if (text.substr(lineStart, start - lineStart).find_first_not_of(" \t") == std::string_view::npos)
        {
          size_t lineEnd = text.find('\n', stop);
          if (lineEnd != std::string_view::npos && text.substr(stop, lineEnd - stop).find_first_not_of(" \t\r") == std::string_view::npos)
            start = uint32_t(lineStart), stop = uint32_t(lineEnd + 1);
        }
        remove(start, stop - start);
      };
      bool replaced = false;
      std::set<uint32_t> handled;
      for (uint32_t k = first; k + 1 < end; ++k)
      {
        std::string_view word = spelling(k);
        if (word == "va_list" && k + 2 < end && toks[k + 2].is(P::Semi))
        {
          list = std::string(spelling(k + 1));
          removeStatement(k, k + 2);
          for (uint32_t h = k; h <= k + 2; ++h)
            handled.insert(h);
        }
        else if ((word == "va_start" || word == "va_end") && toks[k + 1].is(P::LParen))
        {
          uint32_t semicolon = k + 1;
          while (semicolon < end && !toks[semicolon].is(P::Semi))
            ++semicolon;
          removeStatement(k, semicolon);
          for (uint32_t h = k; h <= semicolon; ++h)
            handled.insert(h);
        }
        else if (word == "vssprintf" && k + 5 < end && toks[k + 1].is(P::LParen) && spelling(k + 2) == "format" && toks[k + 3].is(P::Comma) &&
                 spelling(k + 4) == list && toks[k + 5].is(P::RParen))
        {
          replace(toks[k].offset, endOffset(k + 5) - toks[k].offset,
                  std::string(shadowedAt("toString", toks[k].offset) ? "::toString" : "toString") + "(text)");
          replaced = true;
          for (uint32_t h = k; h <= k + 5; ++h)
            handled.insert(h);
        }
      }
      for (uint32_t k = first; k < end; ++k)
        if (!handled.count(k) && toks[k].getKind() == TokenKind::Identifier && (spelling(k) == "format" || (!list.empty() && spelling(k) == list)))
        {
          todo(toks[k].offset, "a format function's body that uses its format otherwise than through vssprintf");
          return;
        }
      if (!replaced)
        todo(toks[f->token].offset, "a format function's body without its vssprintf");
    }
    // design.md "struct and class" (option 3, kovarex 2026-11-01): a struct is a public value type. A
    // struct of the C++ with a private or protected member, a base that is not public, a virtual
    // function or a polymorphic base is a class: `class`, then `public:` first so that its members
    // keep the C++'s access, and `public` before each base written without an access (a class's bases
    // start private). Its friends stay.
    static bool declaresVirtualFunction(const ClassDecl* c)
    {
      for (Decl* m : c->members)
        if (m && m->kind == NodeKind::FunctionDecl && (m->flags & (DF_Virtual | DF_Override | DF_Final | DF_Pure)))
          return true;
      return false;
    }
    bool polymorphicClass(const ClassDecl* c, int depth)
    {
      if (!c || depth > 16)
        return false;
      if (declaresVirtualFunction(c))
        return true;
      for (const ClassDecl::Base& b : c->bases)
        if (b.type && b.type->kind == NodeKind::NamedType)
          if (Decl* d = static_cast<NamedType*>(b.type)->resolved)
          {
            Decl* def = global.definitionOf(d);
            if (def && def->kind == NodeKind::ClassDecl && polymorphicClass(static_cast<ClassDecl*>(def), depth + 1))
              return true;
          }
      return false;
    }
    void structAsClass(ClassDecl* c)
    {
      if (in.c || c->keyword != Kw::struct_ || !c->isDefinition || c->isClosure || (c->flags & DF_Friend))
        return;
      bool privateMember = false, privateBase = false, polymorphicBase = false;
      for (Decl* m : c->members)
        privateMember = privateMember || (m && !(m->flags & DF_Friend) && (m->access == Access::Private || m->access == Access::Protected));
      for (const ClassDecl::Base& b : c->bases)
      {
        privateBase = privateBase || b.access == Access::Private || b.access == Access::Protected;
        if (b.type && b.type->kind == NodeKind::NamedType)
          if (Decl* d = static_cast<NamedType*>(b.type)->resolved)
            if (Decl* def = global.definitionOf(d); def && def->kind == NodeKind::ClassDecl)
              polymorphicBase = polymorphicBase || polymorphicClass(static_cast<ClassDecl*>(def), 1);
      }
      bool isVirtual = declaresVirtualFunction(c);
      if (!privateMember && !privateBase && !isVirtual && !polymorphicBase)
        return;
      // The keyword, after any `template<...>`.
      uint32_t k = c->token;
      while (k < toks.size() && !toks[k].is(Kw::struct_) && !toks[k].is(P::LBrace))
        ++k;
      if (k >= toks.size() || !toks[k].is(Kw::struct_))
        return;
      // The bases written without an access, and the body's `{`.
      uint32_t brace = k;
      bool inBases = false;
      for (int depth = 0; ++brace < toks.size();)
      {
        const Token& t = toks[brace];
        if (t.is(P::LParen) || t.is(P::LBracket) || t.is(P::Less))
          ++depth;
        else if (t.is(P::RParen) || t.is(P::RBracket) || t.is(P::Greater))
          --depth;
        else if (t.is(P::Shr))
          depth -= 2;
        else if (depth <= 0 && t.is(P::LBrace))
          break;
        else if (depth <= 0 && (t.is(P::Colon) || (inBases && t.is(P::Comma))))
        {
          inBases = true;
          uint32_t first = brace + 1;
          while (first < toks.size() && toks[first].is(Kw::virtual_))
            ++first;
          if (first < toks.size() && !toks[first].is(Kw::public_) && !toks[first].is(Kw::private_) && !toks[first].is(Kw::protected_))
          {
            insert(toks[first].offset, "public ");
            counts.add("struct bases written public (structs written as classes)");
          }
        }
      }
      if (brace >= toks.size())
        return;
      replace(toks[k].offset, toks[k].length, "class");
      if (!(brace + 2 < toks.size() && toks[brace + 1].is(Kw::public_) && toks[brace + 2].is(P::Colon)))
      {
        size_t lineStart = text.rfind('\n', toks[k].offset);
        lineStart = lineStart == std::string_view::npos ? 0 : lineStart + 1;
        size_t indentEnd = text.find_first_not_of(" \t", lineStart);
        std::string indent(text.substr(lineStart, indentEnd - lineStart));
        insert(endOffset(brace), "\n" + indent + "public:");
      }
      counts.add(std::string("structs written as classes (design.md \"struct and class\"): ") +
                 (isVirtual ? "a virtual function" : privateMember ? "a private or protected member" : privateBase ? "a base that is not public" : "a polymorphic base"));
    }

    // How std::to_string writes a value of a type, by the overload C++ calls for it: 'i' an integer's
    // digits, as kpp's toString writes them; 'f' a floating value; '?' a bool, a plain char, an
    // enumeration (each promoted to int, where kpp's toString writes true, the character, the
    // enumerator's name) or a type the declarations do not resolve.
    char toStringKind(Type* type, int depth = 0)
    {
      while (type && (type->kind == NodeKind::ConstType || type->kind == NodeKind::LRefType || type->kind == NodeKind::RRefType))
        type = type->kind == NodeKind::ConstType ? static_cast<ConstType*>(type)->inner
             : type->kind == NodeKind::LRefType  ? static_cast<LRefType*>(type)->referenced
                                                 : static_cast<RRefType*>(type)->referenced;
      if (!type || depth > 8)
        return '?';
      if (type->kind == NodeKind::BuiltinType)
      {
        auto* builtin = static_cast<BuiltinType*>(type);
        if (builtin->keyword == Kw::float_ || builtin->keyword == Kw::double_)
          return 'f';
        if (builtin->keyword == Kw::char_ && !builtin->isSigned && !builtin->isUnsigned)
          return '?';
        return builtin->keyword == Kw::int_ || builtin->keyword == Kw::char_ || builtin->keyword == Kw::short_ || builtin->keyword == Kw::long_ ||
                   builtin->keyword == Kw::signed_ || builtin->keyword == Kw::unsigned_
                 ? 'i'
                 : '?';
      }
      if (type->kind != NodeKind::NamedType)
        return '?';
      Decl* d = static_cast<NamedType*>(type)->resolved;
      if (d && d->kind == NodeKind::UsingAliasDecl)
        return toStringKind(static_cast<UsingAliasDecl*>(d)->type, depth + 1);
      if (d && d->kind == NodeKind::VariableDecl && (d->flags & DF_Typedef))
        return toStringKind(static_cast<VariableDecl*>(d)->type, depth + 1);
      return '?';
    }
    // std::to_string(x) by x's type (toStringKind): an integer's is toString(x) (the std:: rename); a
    // floating value's is C's %f, six decimals (`0.100000`), where kpp's toString is the shortest
    // text (docs/stdlib/text.md), so toString(fixed(x, 6)), the fixed inside the call's own
    // parentheses; the rest's is toStringAsStd(x) (overrides/added/CommonUtil/ToStringAsStd.kpp),
    // which picks among those by the type the compiler knows.
    void stdToString(CallExpr* call)
    {
      if (in.c || call->args.size() != 1 || call->callee->kind != NodeKind::NameExpr)
        return;
      const QualifiedName& name = static_cast<NameExpr*>(call->callee)->name;
      if (name.parts.size() != 2 || name.parts[0].symbol == UINT32_MAX || name.parts[1].symbol == UINT32_MAX ||
          symbols.name(name.parts[0].symbol) != "std" || symbols.name(name.parts[1].symbol) != "to_string")
        return;
      char kind = toStringKind(plainTypeOf(call->args[0]));
      if (kind == 'i')
      {
        counts.add("std::to_string of an integer written toString(x)");
        return;
      }
      uint32_t first = firstTokenOf(call->callee);
      uint32_t open = first;
      while (open < toks.size() && !toks[open].is(P::LParen))
        ++open;
      std::vector<std::pair<uint32_t, uint32_t>> ranges = open < toks.size() ? argumentRanges(open + 1) : std::vector<std::pair<uint32_t, uint32_t>>();
      if (ranges.size() != 1 || !toks[ranges[0].second].is(P::RParen))
      {
        todo(toks[first].offset, "std::to_string whose argument the converter does not find");
        return;
      }
      if (kind == 'f')
      {
        std::string fixed = shadowedAt("fixed", toks[open].offset) ? "::fixed(" : "fixed(";
        replace(toks[open].offset, toks[open].length, "(" + fixed);
        replace(toks[ranges[0].second].offset, toks[ranges[0].second].length, ", 6))");
        counts.add("std::to_string of a floating value written toString(fixed(x, 6))");
        return;
      }
      // (the std:: rename's edit, `toString`, gets the name)
      for (Edit& e : edits)
        if (e.offset == toks[first].offset && e.length && e.text.size() >= 8 && e.text.compare(e.text.size() - 8, 8, "toString") == 0)
        {
          e.text += "AsStd";
          counts.add("std::to_string of a bool, char, enumeration or unknown type written toStringAsStd(x)");
          return;
        }
      todo(toks[first].offset, "std::to_string without the std:: rename's edit");
    }
    bool rewritePrintfCall(CallExpr* call)
    {
      if (call->args.empty())
        return false;
      // the callee: ssprintf, a printf statement, a Log macro or SAVING_TAG, or a format function
      Decl* resolved = call->callee->kind == NodeKind::NameExpr     ? static_cast<NameExpr*>(call->callee)->resolved
                     : call->callee->kind == NodeKind::MemberExpr ? static_cast<MemberExpr*>(call->callee)->resolved
                                                                  : nullptr;
      std::string_view name;
      if (call->callee->kind == NodeKind::NameExpr && static_cast<NameExpr*>(call->callee)->name.parts.size() == 1 &&
          static_cast<NameExpr*>(call->callee)->name.parts[0].symbol != UINT32_MAX)
        name = symbols.name(static_cast<NameExpr*>(call->callee)->name.parts[0].symbol);
      static const std::map<std::string_view, size_t> formatMacros = {
        {"LOG_INFO", 0},    {"LOG_NOTICE", 0}, {"LOG_WARNING", 0}, {"LOG_ERROR", 0}, {"LOG_DATA", 0},     {"LOG_GRAPHICS", 0}, {"LOG_FUTURE", 0},
        {"LOG_VERBOSE", 0}, {"LOG_AND_ABORT", 0}, {"LOG_AND_ABORT_NEVER_THROW", 0}, {"LOG", 1}, {"SAVING_TAG", 2}};
      std::string to;
      size_t formatIndex = 0;
      bool trio = true; // (formatted through CommonUtil's vssprintf: trio's floating text)
      // (C: printf and fprintf, the converted C libraries' too; their own variadic interfaces,
      // trio's, gzprintf, stb's writef, keep C's `...`, and a write into a buffer, snprintf and its
      // relatives, has no piece)
      if (in.c && name != "printf" && name != "fprintf")
        return false;
      if (name == "ssprintf")
        to = "toString";
      else if (name == "printf" && !resolved)
        to = "print", trio = false;
      else if (name == "fprintf" && !resolved)
        to = "print", trio = false, formatIndex = 1; // print(stream, pieces...), as C++23's std::print(FILE*, ...)
      else if (auto macro = !resolved && !name.empty() ? formatMacros.find(name) : formatMacros.end(); macro != formatMacros.end())
        formatIndex = macro->second;
      else if (isFormatFunction(resolved))
        formatIndex = static_cast<FunctionDecl*>(resolved)->params.size() - 1;
      else
        return false;
      if (formatIndex >= call->args.size())
        return false;
      // (a format function's call with a format of no conversion and nothing after it: as it is,
      // the overload of a string takes it)
      Expr* format = call->args[formatIndex];
      bool stringMacroFirst = format->kind == NodeKind::NameExpr && format->token < toks.size() &&
                              toks[format->token].getKind() == TokenKind::Identifier && objectMacros.count(std::string(spelling(format->token))) &&
                              isStringLiterals(objectMacros.at(std::string(spelling(format->token))));
      // A format function's (or macro's) format the pieces do not write: one chosen at run time
      // (TagWriter's `this->statistics ? "true" : "false"`), or literals the rewrite cannot read (a C
      // header's string macro among them, SDL_HINT_AUDIO_DRIVER's): its text as C++ wrote it,
      // ssprintf's (trio's) at run time, one piece; with nothing after it, the text itself.
      auto runtimeFormat = [&]() -> bool
      {
        if (!to.empty())
          return false;
        // (a std::string there is no format: `LOG_AND_ABORT(ssprintf(...))` took the overload of a
        // string in C++, as it does here)
        if (isStdStringExpr(format))
          return false;
        uint32_t formatFirst = cFirstToken(format);
        std::vector<std::pair<uint32_t, uint32_t>> all = argumentRanges(formatFirst);
        if (all.size() != call->args.size() - formatIndex)
          return false;
        uint32_t paren = all.back().second;
        if (paren >= toks.size() || !toks[paren].is(P::RParen))
          return false;
        bool alone = all.size() == 1;
        if (alone && format->kind == NodeKind::LiteralExpr)
          return false;
        // (before the format, not at it: a rewrite of the format itself, ssprintf's of a run-time
        // format inside, replaces it and would cancel an insertion there)
        if (formatFirst == 0)
          return false;
        uint32_t after = endOffset(formatFirst - 1);
        Edit* ending = nullptr; // (a rewrite of the token before, a Log macro's name and `(`: the opening goes into its text)
        for (Edit& e : edits)
          if (e.length > 0 && e.offset < after && e.offset + e.length == after)
            ending = &e;
        if (ending)
          ending->text += alone ? "StringView(" : "ssprintf(";
        else if (after < toks[formatFirst].offset)
          replace(after, toks[formatFirst].offset - after, std::string(" ") + (alone ? "StringView(" : "ssprintf("));
        else
          insert(after, alone ? "StringView(" : "ssprintf(");
        uint32_t at = endOffset(paren - 1);
        Edit* parenRewrite = nullptr;
        for (Edit& e : edits)
          if (e.offset == toks[paren].offset && e.length > 0)
            parenRewrite = &e;
        if (parenRewrite && at == toks[paren].offset)
          parenRewrite->text = ")" + parenRewrite->text;
        else
          backEdits.push_back({at, 0, ")"});
        printfRewritten.insert(call);
        counts.add(alone ? "format function calls of a run-time format alone written StringView(format)"
                         : "format function calls of a run-time format written ssprintf(format, ...)");
        return true;
      };
      if (!stringMacroFirst && (format->kind != NodeKind::LiteralExpr || static_cast<LiteralExpr*>(format)->literalKind != TokenKind::String))
        return runtimeFormat();
      // the raw text of the format's literals (and of string macros among them)
      std::string raw, rawDelimiter;
      int rawLiterals = 0;
      uint32_t last = format->token;
      for (; last < toks.size(); ++last)
      {
        std::string piece;
        const std::string* blanked = nullptr; // (a C header's string macro the parse read as its text: SDL_HINT_AUDIO_DRIVER)
        std::string blankedText;
        if (preprocessed && toks[last].getKind() == TokenKind::Identifier)
          for (const Preprocessed::Blank& b : preprocessed->blanked)
            if (b.offset == toks[last].offset && b.length >= toks[last].length)
            {
              size_t x = b.replacement.find_first_not_of(" \t"), y = b.replacement.find_last_not_of(" \t");
              if (x != std::string::npos && isStringLiterals(b.replacement.substr(x, y - x + 1)))
                blankedText = b.replacement.substr(x, y - x + 1), blanked = &blankedText;
            }
        if (toks[last].getKind() == TokenKind::String)
          piece = std::string(spelling(last));
        else if (blanked)
          piece = *blanked;
        else if (auto macro = toks[last].getKind() == TokenKind::Identifier ? objectMacros.find(std::string(spelling(last))) : objectMacros.end();
                 macro != objectMacros.end() && isStringLiterals(macro->second))
          piece = macro->second;
        else
          break;
        for (size_t q = 0; q < piece.size();)
        {
          if (piece[q] == ' ' || piece[q] == '\t' || piece[q] == '\n' || piece[q] == '\r')
          {
            ++q;
            continue;
          }
          if (piece[q] == 'R' && q + 1 < piece.size() && piece[q + 1] == '"')
          {
            // a raw literal (`R"(...)"`, the tests' Lua): its pieces are raw literals of its delimiter
            size_t open = piece.find('(', q + 2);
            if (open == std::string::npos || (rawLiterals == 0 && !raw.empty()) || (rawLiterals > 0 && piece.substr(q + 2, open - q - 2) != rawDelimiter))
            {
              if (runtimeFormat())
                return true;
              counts.add("printf calls left as they are (raw and plain literals together)");
              return false;
            }
            rawDelimiter = piece.substr(q + 2, open - q - 2);
            size_t closeAt = piece.find(")" + rawDelimiter + "\"", open + 1);
            if (closeAt == std::string::npos)
              return false;
            raw += piece.substr(open + 1, closeAt - open - 1);
            q = closeAt + rawDelimiter.size() + 2;
            ++rawLiterals;
            continue;
          }
          if (piece[q] != '"' || rawLiterals > 0)
          {
            if (runtimeFormat())
              return true;
            counts.add(rawLiterals > 0 ? "printf calls left as they are (raw and plain literals together)" : "printf calls left as they are (a prefixed literal)");
            return false;
          }
          size_t e = q + 1;
          while (e < piece.size() && piece[e] != '"')
            e += piece[e] == '\\' ? 2 : 1;
          raw += piece.substr(q + 1, e - q - 1);
          q = e + 1;
        }
      }
      --last;
      std::vector<std::string> segments;
      std::vector<PrintfConversion> conversions;
      std::string unsupported;
      if (!splitPrintfFormat(raw, rawLiterals > 0, segments, conversions, unsupported))
      {
        if (runtimeFormat())
          return true;
        counts.add("printf calls left as they are (" + unsupported + ")");
        return false;
      }
      std::vector<std::pair<uint32_t, uint32_t>> ranges =
        last + 1 < toks.size() && toks[last + 1].is(P::Comma) ? argumentRanges(last + 2) : std::vector<std::pair<uint32_t, uint32_t>>();
      if (to.empty() && conversions.empty() && ranges.empty() && raw.find('%') == std::string::npos && call->args.size() == formatIndex + 1)
        return false;
      // (a `*` width reads an argument before its value: the value's range, and the width's text,
      // one name or number, which the piece takes)
      size_t stars = 0;
      for (const PrintfConversion& c : conversions)
        stars += c.starWidth;
      std::vector<std::pair<uint32_t, uint32_t>> all = ranges;
      std::vector<size_t> valueArgument(conversions.size());
      std::vector<std::string> widthTexts(conversions.size());
      bool widthsKnown = all.size() == conversions.size() + stars;
      if (stars && widthsKnown)
      {
        ranges.clear();
        for (size_t k = 0, r = 0; k < conversions.size(); ++k)
        {
          if (conversions[k].starWidth)
          {
            auto [b, e] = all[r++];
            if (e == b + 1 && (toks[b].getKind() == TokenKind::Identifier || toks[b].getKind() == TokenKind::Number))
              widthTexts[k] = std::string(spelling(b));
            else if (e == b + 2 && toks[b].is(P::Minus) && toks[b + 1].getKind() == TokenKind::Number)
              widthTexts[k] = "-" + std::string(spelling(b + 1));
            else
              widthsKnown = false;
          }
          valueArgument[k] = r;
          ranges.push_back(all[r++]);
        }
      }
      else
        for (size_t k = 0; k < conversions.size(); ++k)
          valueArgument[k] = k;
      if (all.size() != call->args.size() - 1 - formatIndex || !widthsKnown || conversions.size() != ranges.size())
      {
        if (runtimeFormat())
          return true;
        counts.add("printf calls left as they are (arguments not one per conversion, or a macro's)");
        return false;
      }
      if (to == "print")
      {
        // (a statement: print returns no count)
        uint32_t first = firstTokenOf(call->callee), close = ranges.empty() ? last + 1 : ranges.back().second;
        if (first == 0 || !(toks[first - 1].is(P::Semi) || toks[first - 1].is(P::LBrace) || toks[first - 1].is(P::RBrace) || toks[first - 1].is(P::RParen) ||
                            toks[first - 1].is(Kw::else_) || toks[first - 1].is(P::Colon)) ||
            !(close < toks.size() && toks[close].is(P::RParen) && close + 1 < toks.size() && toks[close + 1].is(P::Semi)))
          return false;
      }
      std::vector<std::string> opens(conversions.size()), closes(conversions.size());
      std::vector<uint32_t> cStrDots(conversions.size(), UINT32_MAX);
      for (size_t k = 0; k < conversions.size(); ++k)
        if (!printfPiece(conversions[k], call->args[valueArgument[k] + 1 + formatIndex], ranges[k].second, trio, opens[k], closes[k], cStrDots[k],
                         widthTexts[k]))
        {
          if (runtimeFormat())
            return true;
          counts.add("printf calls left as they are (a + flag on %" + std::string(1, conversions[k].conversion) + ")");
          return false;
        }
      // widenFormatArguments' casts for this format go: the pieces say the width
      if (auto widened = widenedFormats.find(format->token); widened != widenedFormats.end())
        for (size_t index : widened->second)
        {
          Edit& e = edits[index];
          e.text = e.length == 0 ? std::string() : std::string(text.substr(e.offset, e.length));
        }
      auto literal = [&](const std::string& s) { return rawLiterals > 0 ? "R\"" + rawDelimiter + "(" + s + ")" + rawDelimiter + "\"" : "\"" + s + "\""; };
      // (the library's names, `::toString` where the program's own hides it: NoiseProgram's
      // toString(), Color's hex(), DebugTools' parameter toString)
      uint32_t callAt = toks[firstTokenOf(call->callee)].offset;
      auto qualified = [&](std::string text)
      {
        static const char* names[] = {"toString", "print", "hex", "hexUpper", "octal", "fixed", "significant", "scientific", "trioFixed",
                                      "trioSignificant", "trioScientific", "withSign",
                                      "alignLeft", "alignRight", "zeroPadded", "address", "StringView"};
        for (const char* name : names)
          for (size_t at = text.find(name); at != std::string::npos; at = text.find(name, at + 1))
          {
            size_t end = at + strlen(name);
            if ((at > 0 && (isIdentChar(text[at - 1]) || text[at - 1] == ':')) || end >= text.size() || text[end] != '(')
              continue;
            if (shadowedAt(name, callAt))
            {
              text.insert(at, "::");
              at += 2;
            }
          }
        return text;
      };
      if (!to.empty())
      {
        to = qualified(to + "(");
        to.pop_back();
      }
      for (std::string& open : opens)
        open = qualified(open);
      // the callee (also written `::printf`: InteractiveTestSelect has a printf of its own); a
      // format function's and a macro's stay
      if (!to.empty())
      {
        uint32_t calleeFirst = firstTokenOf(call->callee), calleeLast = calleeFirst;
        while (calleeLast + 1 < toks.size() && !toks[calleeLast + 1].is(P::LParen))
          ++calleeLast;
        replace(toks[calleeFirst].offset, endOffset(calleeLast) - toks[calleeFirst].offset, to);
      }
      // The format and the comma and spaces after it become its first segment and the first piece's
      // opening; each later comma the segment before its piece and that piece's opening. (Before
      // the argument, not at it: a rewrite of the argument itself, a macro written out or a
      // variant's access, replaces it and would cancel an insertion there.)
      uint32_t formatStart = toks[format->token].offset, formatEnd = endOffset(last);
      if (conversions.empty())
        replace(formatStart, formatEnd - formatStart, literal(segments[0]));
      else
        replace(formatStart, toks[ranges[0].first].offset - formatStart, (segments[0].empty() ? std::string() : literal(segments[0]) + ", ") + opens[0]);
      for (size_t k = 0; k < conversions.size(); ++k)
      {
        if (k > 0)
        {
          uint32_t comma = toks[ranges[k - 1].second].offset;
          replace(comma, toks[ranges[k].first].offset - comma,
                  closes[k - 1] + ", " + (segments[k].empty() ? std::string() : literal(segments[k]) + ", ") + opens[k]);
        }
        // (the last piece's closing after its argument; the others' are in the next comma's text)
        std::string after = closes[k] + (!segments[k + 1].empty() ? ", " + literal(segments[k + 1]) : std::string());
        if (k + 1 == conversions.size() && !after.empty())
        {
          // (before the call's `)`, which a macro's rewrite may replace: `LOG_ERROR(...)`'s is the
          // expansion's, which would cancel an insertion there; then into its text)
          uint32_t at = endOffset(ranges[k].second - 1), paren = toks[ranges[k].second].offset;
          Edit* parenRewrite = nullptr;
          for (Edit& e : edits)
            if (e.offset == paren && e.length > 0)
              parenRewrite = &e;
          if (parenRewrite && at == paren)
            parenRewrite->text = after + parenRewrite->text;
          else
            backEdits.push_back({at, 0, after});
        }
        if (cStrDots[k] != UINT32_MAX)
          remove(toks[cStrDots[k]].offset, endOffset(cStrDots[k] + 3) - toks[cStrDots[k]].offset);
      }
      printfRewritten.insert(call);
      counts.add(name == "ssprintf" ? "ssprintf calls written toString(pieces)"
                 : to == "print"     ? "printf statements written print(pieces)"
                 : !resolved         ? "Log and SAVING_TAG macro calls written with pieces"
                                     : "calls of the game's format functions written with pieces");
      return true;
    }
    // Factorio's own format mismatches, made explicit as C++ prints them on x64: an argument the
    // declarations say is 64-bit, read by a 32-bit conversion (`%u` of a UnitNumber or a size_t
    // index, `#%ull`), is written uint32_t(...) (int32_t for %d and %i), the 32 bits printf reads
    // of it; arguments after the last the format reads are dropped (C++ evaluates and ignores
    // them; the one the game has, a toUint32(), has no effect).
    void narrowFormatArguments(CallExpr* call)
    {
      // A C-variadic function's call whose last named parameter is the format (ssprintf, printf,
      // snprintf), as kppc checks them: no other call's literal is a format.
      Expr* callee = call->callee;
      Decl* d = callee->kind == NodeKind::NameExpr ? static_cast<NameExpr*>(callee)->resolved
              : callee->kind == NodeKind::MemberExpr ? static_cast<MemberExpr*>(callee)->resolved
                                                     : nullptr;
      // The C library's, which the C++ parse does not declare: by name, with the format's place.
      static const std::map<std::string_view, size_t> cFunctions = {{"printf", 0}, {"sprintf", 1}, {"snprintf", 2}, {"fprintf", 1}};
      size_t formatIndex;
      std::string_view calleeName;
      if (d && d->kind == NodeKind::FunctionDecl && (d->flags & DF_Variadic) && !static_cast<FunctionDecl*>(d)->params.empty())
      {
        formatIndex = static_cast<FunctionDecl*>(d)->params.size() - 1;
        calleeName = symbols.name(static_cast<FunctionDecl*>(d)->name.last().symbol);
      }
      else if (auto c = !d && callee->kind == NodeKind::NameExpr && static_cast<NameExpr*>(callee)->name.parts.size() == 1
                          ? cFunctions.find(symbols.name(static_cast<NameExpr*>(callee)->name.parts[0].symbol))
                          : cFunctions.end();
               c != cFunctions.end())
      {
        formatIndex = c->second;
        calleeName = c->first;
      }
      else
        return;
      for (size_t a = formatIndex; a < call->args.size() && a == formatIndex; ++a)
      {
        Expr* arg = call->args[a];
        if (!arg || arg->kind != NodeKind::LiteralExpr || static_cast<LiteralExpr*>(arg)->literalKind != TokenKind::String)
          continue;
        std::string format;
        uint32_t last = formatAt(arg->token, format);
        if (format.find('%') == std::string::npos && !(last + 1 < toks.size() && toks[last + 1].is(P::Comma)))
          return;
        std::vector<FormatConversion> conversions = formatConversions(format);
        std::vector<std::pair<uint32_t, uint32_t>> ranges =
          last + 1 < toks.size() && toks[last + 1].is(P::Comma) ? argumentRanges(last + 2) : std::vector<std::pair<uint32_t, uint32_t>>();
        size_t following = call->args.size() - a - 1;
        if (ranges.size() != following)
          return; // the tokens are not the call's arguments (a macro's): leave them
        for (size_t n = 0; n < conversions.size() && n < following; ++n)
          if (!conversions[n].wide && strchr("diuxXoc", conversions[n].conversion) && isWideInteger(call->args[a + 1 + n]))
          {
            castArgument(ranges[n].first, ranges[n].second, strchr("di", conversions[n].conversion) ? "int32_t" : "uint32_t");
            counts.add("64-bit printf arguments read as 32 bits written so");
          }
        if (following > 0 && format.find('%') == std::string::npos && callee->kind == NodeKind::NameExpr && calleeName == "ssprintf")
        {
          // A format that reads nothing: `ssprintf("text", x)` is the text (ssprintf without
          // arguments is deleted: CommonUtil's StringUtil.hpp).
          replace(toks[firstTokenOf(callee)].offset, toks[arg->token].offset - toks[firstTokenOf(callee)].offset, "String(");
          remove(toks[last + 1].offset, toks[ranges.back().second].offset - toks[last + 1].offset);
          counts.add("printf arguments the format does not read dropped");
        }
        // (Not in C: a C library's variadic function may read its own format, stb_image_write's
        // `stbiw__writef(s, "111 221 2222 11", 0, 0, format, ...)`, whose arguments are its data;
        // kppc's format check reports it instead: language/variadic_with_own_format.)
        else if (following > conversions.size() && !in.c)
        {
          uint32_t from = conversions.empty() ? last + 1 : ranges[conversions.size() - 1].second;
          remove(toks[from].offset, toks[ranges.back().second].offset - toks[from].offset);
          counts.add("printf arguments the format does not read dropped");
        }
        return;
      }
    }

    void convertTokens()
    {
      for (uint32_t i = 0; i + 2 < toks.size(); ++i)
      {
        const Token& t = toks[i];
        if (in.c && t.getKind() == TokenKind::Keyword && spelling(i) != keywordSpelling(Kw(t.id)))
        {
          // A C macro of one keyword (zutil.h's `local`, static): the keyword.
          replace(t.offset, t.length, std::string(keywordSpelling(Kw(t.id))));
          counts.add("C macros of one keyword written as it");
          continue;
        }
        if (in.c && preprocessed && t.getKind() == TokenKind::Identifier && spelling(i).size() >= 4 &&
            spelling(i).substr(spelling(i).size() - 4) == "NULL" && !memberAccessAt(i))
          if (auto macro = preprocessed->defined.find(std::string(spelling(i))); macro != preprocessed->defined.end() && macro->second == "0")
          {
            // C's null pointer by another name (zlib.h's `#define Z_NULL 0`): kpp imports the macro
            // as an int constant, which is no null pointer.
            replace(t.offset, t.length, "nullptr");
            counts.add("C null pointer macros written as nullptr");
            continue;
          }
        if (in.c && t.getKind() == TokenKind::Identifier && !memberAccessAt(i))
        {
          // Windows' TCHAR names, macros of its headers for the narrow or the wide function: the
          // narrow one, as the C libraries are built without UNICODE (minizip's iowin32.c).
          static const std::map<std::string_view, std::string_view> tcharNames = {
            {"CreateFile", "CreateFileA"}, {"DeleteFile", "DeleteFileA"}, {"MoveFile", "MoveFileA"}, {"GetFileAttributes", "GetFileAttributesA"},
            {"GetFileAttributesEx", "GetFileAttributesExA"}, {"FindFirstFile", "FindFirstFileA"}, {"FindNextFile", "FindNextFileA"},
            {"GetModuleFileName", "GetModuleFileNameA"}, {"GetModuleHandle", "GetModuleHandleA"}, {"LoadLibrary", "LoadLibraryA"},
            {"GetFullPathName", "GetFullPathNameA"}, {"CreateDirectory", "CreateDirectoryA"}, {"RemoveDirectory", "RemoveDirectoryA"},
            {"GetTempPath", "GetTempPathA"}, {"FormatMessage", "FormatMessageA"}, {"OutputDebugString", "OutputDebugStringA"},
            {"GetEnvironmentVariable", "GetEnvironmentVariableA"}, {"_tcslen", "strlen"}, {"_tcsncpy", "strncpy"}, {"_tcscpy", "strcpy"},
            {"_tcscat", "strcat"}, {"_tcschr", "strchr"}, {"_tcsrchr", "strrchr"}, {"_tcscmp", "strcmp"}, {"_tcsicmp", "_stricmp"},
            {"_sntprintf", "_snprintf"}, {"_stprintf", "sprintf"}, {"_tfopen", "fopen"}, {"_topen", "_open"}, {"_tstat", "_stat"}};
          if (auto narrow = tcharNames.find(spelling(i)); narrow != tcharNames.end())
          {
            replace(t.offset, t.length, std::string(narrow->second));
            counts.add("Windows' TCHAR names written as the narrow ones (C)");
            continue;
          }
        }
        // (A private name, private-names.txt's, is a type too: also in a class body, stb_truetype's
        // `struct stbrp_rect { stbrp_coord x,y; ... }`.)
        if (in.c && cStaticRenames && t.getKind() == TokenKind::Identifier && !memberAccessAt(i) &&
            (!inClassBody(t.offset) || (cPrivateNames && cPrivateNames->count(std::string(spelling(i))))))
          if (auto renamed = cStaticRenames->find(std::string(spelling(i))); renamed != cStaticRenames->end())
          {
            replace(t.offset, t.length, renamed->second);
            counts.add("C statics another file declares too renamed with their file's name");
            continue;
          }
        // A name of a scoped library the program uses (--convert-library), qualified by the library:
        // `zstd::ZSTD_compress(...)`, `zstd::ZSTD_inBuffer in = {}` (design.md 16).
        // (a global one, C++'s `::stbi_write_png_compression_level` in MemoryBitmap.cpp, is the
        // library's too: `stb_image::stbi_write_png_compression_level`)
        bool globalQualifier = i > 0 && toks[i - 1].is(P::ColonColon) &&
                               (i < 2 || !(toks[i - 2].getKind() == TokenKind::Identifier || toks[i - 2].is(P::Greater)));
        if (scopedLibraryNames && !scopedLibraryNames->empty() && t.getKind() == TokenKind::Identifier &&
            !(i > 0 && (toks[i - 1].is(P::Dot) || toks[i - 1].is(P::Arrow) || (toks[i - 1].is(P::ColonColon) && !globalQualifier))))
          if (auto library = scopedLibraryNames->find(std::string(spelling(i)));
              library != scopedLibraryNames->end() && !shadowedAt(library->first, t.offset)) // (a local `compress`, BitmapUtil's)
          {
            if (globalQualifier)
              replace(toks[i - 1].offset, t.offset + t.length - toks[i - 1].offset, library->second + "::" + library->first);
            else
              replace(t.offset, t.length, library->second + "::" + library->first);
            counts.add("names of a scoped library qualified");
            continue;
          }
        if (in.c && t.getKind() == TokenKind::Identifier && spelling(i) == "config" && !memberAccessAt(i) && !inClassBody(t.offset))
        {
          // kpp's build configuration is the namespace `config` (config::debug): zlib's
          // `typedef struct config_s {...} config;` and opus's locals named so are renamed. A member
          // keeps its name (it is reached through `.` or `->`).
          replace(t.offset, t.length, "config_");
          counts.add("C names `config` renamed (kpp's configuration namespace)");
          continue;
        }
        if (in.c && t.getKind() == TokenKind::Identifier && lookupKeyword(spelling(i)) != UINT32_MAX)
        {
          // A C name that is a C++ keyword (trio's `class`, curl's `new`): kpp has the keyword.
          replace(t.offset, t.length, std::string(spelling(i)) + "_");
          counts.add("C names that are C++ keywords renamed");
          continue;
        }
        // (Not in C: its 64-bit arguments have their C types, and a scanf's are pointers, trio's
        // `trio_sscanf(buffer, "%lld", &value)`.)
        if (!in.c && t.getKind() == TokenKind::String && i > 0 && (toks[i - 1].is(P::LParen) || toks[i - 1].is(P::Comma)) &&
            !(i > 0 && toks[i - 1].getKind() == TokenKind::String))
          widenFormatArguments(i);
        if (t.getKind() == TokenKind::Identifier && !fixtureBodies.empty())
          if (const FixtureBody* body = fixtureBodyAt(i))
          {
            std::string name(spelling(i));
            bool qualified = i > 0 && (toks[i - 1].is(P::Dot) || toks[i - 1].is(P::Arrow) || toks[i - 1].is(P::ColonColon));
            // (A name before a name is a declaration's type.)
            if (!qualified && !toks[i + 1].is(P::ColonColon) && toks[i + 1].getKind() != TokenKind::Identifier && body->members.count(name) &&
                !isFixtureLocal(*body, name, i))
            {
              insert(t.offset, "this->");
              counts.add("this-> added in fixture test bodies");
            }
          }
        // C's parenthesized declarator name, lua.h's `LUA_API int (lua_gettop) (lua_State *L);`
        // (written so that a function-like macro of the same name does not expand): the name alone,
        // in a declaration outside any function body. `(f)(x)` in an expression is a call and stays.
        if (t.is(P::LParen) && i > 0 && i + 3 < toks.size() && toks[i + 1].getKind() == TokenKind::Identifier && toks[i + 2].is(P::RParen) &&
            toks[i + 3].is(P::LParen) && !functionNameAt(t.offset))
        {
          static const std::set<std::string_view> typeWords = {"void", "char", "short", "int", "long", "float", "double", "bool", "signed",
                                                               "unsigned", "const", "volatile", "auto"};
          const Token& before = toks[i - 1];
          if (before.getKind() == TokenKind::Identifier || (before.getKind() == TokenKind::Keyword && typeWords.count(spelling(i - 1))) ||
              before.is(P::Star) || before.is(P::Amp) || before.is(P::AndAnd) || before.is(P::Greater))
          {
            remove(t.offset, t.length);
            remove(toks[i + 2].offset, toks[i + 2].length);
            counts.add("parenthesized declarator names unwrapped");
            i += 2;
            continue;
          }
        }
        if (t.getKind() == TokenKind::Identifier && spelling(i) == "__uuidof" && i + 3 < toks.size() && toks[i + 1].is(P::LParen) &&
            toks[i + 2].getKind() == TokenKind::Identifier && toks[i + 3].is(P::RParen) && i > 2 && (toks[i - 1].is(P::EqEq) || toks[i - 1].is(P::NotEq)) &&
            toks[i - 2].getKind() == TokenKind::Identifier &&
            !(toks[i - 3].is(P::Dot) || toks[i - 3].is(P::Arrow) || toks[i - 3].is(P::ColonColon)))
        {
          // `iid == __uuidof(IUnknown)` with iid a REFIID (a pointer, above): C++ compares the ids'
          // values (guiddef.h's operator==, a memcmp), as C's IsEqualIID does.
          insert(toks[i - 2].offset, "(memcmp(");
          replace(toks[i - 1].offset, endOffset(i + 3) - toks[i - 1].offset,
                  ", &IID_" + std::string(spelling(i + 2)) + ", sizeof(IID)) " + std::string(spelling(i - 1)) + " 0)");
          counts.add("interface ids compared by value");
          i += 3;
          continue;
        }
        if (t.getKind() == TokenKind::Identifier && spelling(i) == "__uuidof" && i + 3 < toks.size() && toks[i + 1].is(P::LParen) &&
            toks[i + 2].getKind() == TokenKind::Identifier && toks[i + 3].is(P::RParen))
        {
          // MSVC's `__uuidof(IFoo)`: the interface's IID, which the SDK declares as IID_IFoo (in C,
          // a REFIID is a pointer to it; kpp_directx.h declares the generated interfaces' IIDs).
          replace(t.offset, endOffset(i + 3) - t.offset, "&IID_" + std::string(spelling(i + 2)));
          counts.add("__uuidof to IID");
          i += 3;
          continue;
        }
        if (t.getKind() == TokenKind::Identifier && i > 1 && (toks[i - 1].is(P::Comma) || (toks[i - 1].is(P::LParen) && (toks[i - 2].getKind() == TokenKind::Identifier || toks[i - 2].is(P::Greater)))) &&
            (toks[i + 1].is(P::Comma) || toks[i + 1].is(P::RParen)) && (spelling(i).rfind("CLSID_", 0) == 0 || spelling(i).rfind("IID_", 0) == 0))
        {
          // A class or interface id passed by name (`CoCreateInstance(CLSID_WbemLocator, ...,
          // IID_IWbemLocator, ...)`): C++ binds it to a REFCLSID or REFIID reference, which C's (and
          // so the import's, and the converted functions') is a pointer to it.
          insert(t.offset, "&");
          counts.add("class and interface ids passed by address");
          continue;
        }
        if (t.getKind() == TokenKind::Identifier && !fileAliasRenames.empty() &&
            !(i > 0 && (toks[i - 1].is(P::Dot) || toks[i - 1].is(P::Arrow) || toks[i - 1].is(P::ColonColon))))
          if (auto rename = fileAliasRenames.find(std::string(spelling(i))); rename != fileAliasRenames.end())
          {
            replace(t.offset, t.length, rename->second);
            counts.add("file-local aliases renamed");
            continue;
          }
        if (t.is(P::LParen) && i + 4 < toks.size() && toks[i + 1].getKind() == TokenKind::Identifier && isAllCapsName(spelling(i + 1)) &&
            toks[i + 2].is(P::RParen) && (toks[i + 3].is(P::Amp) || toks[i + 3].is(P::Star)) && toks[i + 4].getKind() == TokenKind::Identifier &&
            !(i > 0 && (toks[i - 1].getKind() == TokenKind::Identifier || toks[i - 1].is(P::RParen) || toks[i - 1].is(P::RBracket) ||
                        toks[i - 1].is(P::Greater) || callsWithParenthesis(i - 1))))
        {
          // `(LPWSTR)&msg`, a C type's cast of an address or a dereference: kpp reads `(NAME) & x`
          // without knowing that NAME is a type, as a binary `&`, so the operand is parenthesized,
          // `(LPWSTR)(&msg)`, which is a cast. A name in capitals is taken for a C type (the
          // Windows typedefs), as C++ code does not write `(CONSTANT) & x`.
          uint32_t end = i + 4;
          for (;;)
          {
            if (end + 2 < toks.size() && (toks[end + 1].is(P::Dot) || toks[end + 1].is(P::Arrow) || toks[end + 1].is(P::ColonColon)) &&
                toks[end + 2].getKind() == TokenKind::Identifier)
              end += 2;
            else if (end + 1 < toks.size() && (toks[end + 1].is(P::LBracket) || toks[end + 1].is(P::LParen)))
            {
              int depth = 0;
              uint32_t k = end + 1;
              for (; k < toks.size(); ++k)
              {
                if (toks[k].is(P::LBracket) || toks[k].is(P::LParen))
                  ++depth;
                else if ((toks[k].is(P::RBracket) || toks[k].is(P::RParen)) && --depth == 0)
                  break;
              }
              if (k == toks.size())
                break;
              end = k;
            }
            else
              break;
          }
          insert(toks[i + 3].offset, "(");
          insert(endOffset(end), ")");
          counts.add("casts of an address parenthesized");
        }
        if (t.is(P::LParen) && i + 4 < toks.size() && toks[i + 1].getKind() == TokenKind::Identifier && isAllCapsName(spelling(i + 1)) &&
            toks[i + 2].is(P::RParen) && toks[i + 3].is(P::Minus) && toks[i + 4].getKind() == TokenKind::Number &&
            !(i > 0 && (toks[i - 1].getKind() == TokenKind::Identifier || toks[i - 1].is(P::RParen) || toks[i - 1].is(P::RBracket) ||
                        toks[i - 1].is(P::Greater) || callsWithParenthesis(i - 1))))
        {
          // `(HANDLE) -1`, a C type's cast of a negative number (StackWalker's invalid snapshot
          // handle), which kpp reads as a subtraction from NAME, as above: `(HANDLE)(-1)`.
          insert(toks[i + 3].offset, "(");
          insert(endOffset(i + 4), ")");
          counts.add("casts of a negative number parenthesized");
        }
        // `X::value_type` (reference, pointer, difference_type, iterator_category) of an iterator type
        // X is IteratorTraits<X>'s, as std::iterator_traits gives them: CreatableEntityTypeIterator's
        // `using value_type = ListIterator::value_type;` with ListIterator a std::vector's const_iterator,
        // a pointer in kpp.
        static const std::set<std::string_view> iteratorMemberTypes = {"value_type", "reference", "pointer", "difference_type", "iterator_category"};
        if (t.getKind() == TokenKind::Identifier && i + 2 < toks.size() && toks[i + 1].is(P::ColonColon) && iteratorMemberTypes.count(spelling(i + 2)) &&
            !toks[i + 3].is(P::ColonColon) && !(i > 0 && (toks[i - 1].is(P::ColonColon) || toks[i - 1].is(P::Dot) || toks[i - 1].is(P::Arrow))) &&
            namesIteratorType(std::string(spelling(i))))
        {
          insert(t.offset, "IteratorTraits<");
          insert(endOffset(i), ">");
          counts.add("iterator member types through IteratorTraits");
          i += 2;
          continue;
        }
        // The same of a container's iterator named through the container, `WidgetArray::iterator::reference`
        // (agui::Widget::visible_iterator, WidgetArray a Vector): IteratorTraits<WidgetArray::iterator>.
        if (t.getKind() == TokenKind::Identifier && i >= 2 && i + 2 < toks.size() && toks[i - 1].is(P::ColonColon) &&
            toks[i - 2].getKind() == TokenKind::Identifier && toks[i + 1].is(P::ColonColon) && iteratorMemberTypes.count(spelling(i + 2)) &&
            !toks[i + 3].is(P::ColonColon) && spelling(i).size() >= 8 && spelling(i).substr(spelling(i).size() - 8) == "iterator")
        {
          uint32_t start = i - 2;
          while (start >= 2 && toks[start - 1].is(P::ColonColon) && toks[start - 2].getKind() == TokenKind::Identifier)
            start -= 2;
          if (!(start > 0 && (toks[start - 1].is(P::ColonColon) || toks[start - 1].is(P::Greater) || toks[start - 1].is(P::Dot) || toks[start - 1].is(P::Arrow))))
          {
            insert(toks[start].offset, "IteratorTraits<");
            insert(endOffset(i), ">");
            counts.add("iterator member types through IteratorTraits");
            i += 2;
            continue;
          }
        }
        // A string macro or a macro that is a qualified name is its text wherever it is used: macros
        // know no scopes (the constant the conversion makes of UTF8_BULLET is inside StringUtil's
        // namespace), and only literals concatenate (`"\n" UTF8_BULLET " "`).
        if (in.c && cWrittenOutMacros && cWrittenOutMacros->count("*") && t.getKind() == TokenKind::Identifier && !(i > 0 && toks[i - 1].is(P::ColonColon)) &&
            !cNamedConstant(std::string(spelling(i))))
          if (const std::string* own = cOwnObjectAt(std::string(spelling(i)), t.offset))
          {
            // C: this file's own macro in force here (one of several definitions of the name);
            // marked, as a macro's use is (--convert-marks: zlib's z_off64_t, `__int64` on Windows
            // and `long long` on Linux, is a group the platforms' merge chooses between)
            std::string written = rescanMacros(cExpandedObject(*own, t.offset), t.offset);
            if (convertMarks && !isStringLiterals(*own))
            {
              std::string id = in.path.filename().string() + ":@" + std::to_string(t.offset);
              written = std::string(t.offset > 0 && text[t.offset - 1] == '/' ? " " : "") + "/*kpp:if " + id + "*/" + written + "/*kpp:endif " + id + "*/";
            }
            replace(t.offset, t.length, written);
            counts.add("C macro constants written out at their uses");
            continue;
          }
        // (In C a macro is one after `.` too: zlib's `tree[n].Freq` is `tree[n].fc.freq`.)
        if (t.getKind() == TokenKind::Identifier && !objectMacros.empty() &&
            !(i > 0 && (((toks[i - 1].is(P::Dot) || toks[i - 1].is(P::Arrow)) && !in.c) || toks[i - 1].is(P::ColonColon))))
          if (auto macro = objectMacros.find(std::string(spelling(i))); macro != objectMacros.end() && macroHoldsAt(macro->first, t.offset) &&
              !cNamedConstant(macro->first))
          {
            std::string written = in.c ? rescanMacros(cExpandedObject(macro->second, t.offset), t.offset) : scopedQualified(macro->second);
            // Marked (--convert-marks) as a macro's use is, with the literals it joins: a string
            // macro may be defined otherwise on another platform or configuration (InputState's
            // `"left-" WIN_KEY_NAME`, "windows" or "super"), and the merge chooses between the whole
            // literals, `(config::platform == Platform::Windows ? "left-" "windows" : "left-" "super")`.
            // Another name (FileOperations' ftello64, `_ftelli64` on Windows and `ftello` on Linux)
            // is marked alone: a use whose names differ is merged as the statement around it.
            if (convertMarks && !isStringLiterals(macro->second) &&
                (in.c || isQualifiedName(macro->second) || isAliasMacro(macro->first, macro->second)))
            {
              std::string id = in.path.filename().string() + ":@" + std::to_string(t.offset);
              written = std::string(t.offset > 0 && text[t.offset - 1] == '/' ? " " : "") + "/*kpp:if " + id + "*/" + written + "/*kpp:endif " + id + "*/";
            }
            if (convertMarks && !in.c && isStringLiterals(macro->second))
            {
              auto stringMacro = [&](uint32_t k)
              {
                if (toks[k].getKind() != TokenKind::Identifier || (k > 0 && toks[k - 1].is(P::ColonColon)))
                  return false;
                auto m = objectMacros.find(std::string(spelling(k)));
                return m != objectMacros.end() && macroHoldsAt(m->first, toks[k].offset) && !cNamedConstant(m->first) && isStringLiterals(m->second);
              };
              auto joined = [&](uint32_t k) { return toks[k].getKind() == TokenKind::String || stringMacro(k); };
              uint32_t first = i, last = i;
              while (first > 0 && joined(first - 1))
                --first;
              while (last + 1 < toks.size() && joined(last + 1))
                ++last;
              bool firstMacro = true, lastMacro = true;
              for (uint32_t k = first; k < i; ++k)
                firstMacro = firstMacro && !stringMacro(k);
              for (uint32_t k = i + 1; k <= last; ++k)
                lastMacro = lastMacro && !stringMacro(k);
              std::string id = in.path.filename().string() + ":@" + std::to_string(toks[first].offset);
              // (a mark right after `/` would make `//`, a line comment)
              std::string open = "/*kpp:if " + id + "*/", close = "/*kpp:endif " + id + "*/";
              if (firstMacro)
              {
                if (toks[first].offset > 0 && text[toks[first].offset - 1] == '/')
                  open = " " + open;
                if (first == i)
                  written = open + written;
                else
                  insert(toks[first].offset, open);
              }
              if (lastMacro)
              {
                if (last == i)
                  written += close;
                else
                  insert(endOffset(last), close);
              }
            }
            replace(t.offset, t.length, written);
            counts.add(isStringLiterals(macro->second)                                       ? "string macros written out"
                       : isQualifiedName(macro->second) || isAliasMacro(macro->first, macro->second) ? "name macros written out"
                                                                                                      : "spliced macros written out");
            continue;
          }
        if (t.getKind() == TokenKind::Identifier && headerAliases && !headerAliases->empty() &&
            !(i > 0 && (toks[i - 1].is(P::Dot) || toks[i - 1].is(P::Arrow) || toks[i - 1].is(P::ColonColon))))
          // (a known macro's spelling stands for every platform's: SDL_stdinc.h's alloca is
          // `__builtin_alloca` on Linux and `_alloca` on Windows, `__builtin_alloca` on both)
          if (auto alias = headerAliases->find(std::string(spelling(i))); alias != headerAliases->end() && !knownMacros.count(alias->first))
          {
            replace(t.offset, t.length, scopedQualified(alias->second));
            counts.add("C header alias macros expanded");
            continue;
          }
        if (t.getKind() == TokenKind::Identifier && toks[i + 1].is(P::LParen) && !localMacros.empty())
        {
          const LocalMacro* lm = localMacroAt(std::string(spelling(i)), t.offset);
          if (lm)
          {
            // The arguments: token ranges between top-level commas, up to the matching ')'.
            std::vector<std::pair<uint32_t, uint32_t>> args;
            uint32_t j = i + 2, argStart = j;
            int depth = 0;
            bool closed = false;
            for (; j < toks.size(); ++j)
            {
              const Token& tj = toks[j];
              if (tj.is(P::LParen) || tj.is(P::LBracket) || tj.is(P::LBrace))
                ++depth;
              else if (tj.is(P::RParen) || tj.is(P::RBracket) || tj.is(P::RBrace))
              {
                if (depth == 0)
                {
                  closed = tj.is(P::RParen);
                  break;
                }
                --depth;
              }
              else if (tj.is(P::Comma) && depth == 0)
              {
                args.push_back({argStart, j});
                argStart = j + 1;
              }
              else if (tj.getKind() == TokenKind::End)
                break; // (an argument may hold statements: lvm.c's `vmcase(OP_MOVE, setobjs2s(L, ra, RB(i));)`)
            }
            if (closed)
            {
              if (j > argStart || !args.empty())
                args.push_back({argStart, j});
              if (argumentsFit(*lm, args.size()))
              {
                std::vector<std::string> argText;
                for (auto [a0, a1] : args)
                  argText.push_back(a1 > a0 ? std::string(text.substr(toks[a0].offset, endOffset(a1 - 1) - toks[a0].offset)) : std::string());
                if (uint32_t close = leftOutTests && !leftOutTests->empty() && toks[j + 1].is(P::LBrace) ? closingBrace(j + 1) : 0)
                {
                  // A test's name is its macro's last argument (TEST(Name), TEST_FIXTURE(Fixture, Name),
                  // TEST_PARAMETRISED_FIXTURE(Fixture, Parameter, Name)), its suite the innermost SUITE
                  // block around it.
                  std::string name(spelling(i));
                  if (name == "SUITE" && argText.size() == 1)
                  {
                    suiteBlocks.push_back({argText[0], j + 1, close});
                    suitesConverted->insert(argText[0]);
                  }
                  else if ((name == "TEST" || name.rfind("TEST_", 0) == 0) && !argText.empty())
                  {
                    std::string suite;
                    for (const SuiteBlock& s : suiteBlocks)
                      if (s.open < i && i < s.close)
                        suite = s.name;
                    if (auto left = leftOutTests->find(suite + "/" + argText.back()); left != leftOutTests->end())
                    {
                      replace(t.offset, endOffset(close) - t.offset, "// " + left->first + " is left out (overrides/left-out-tests.txt): " + left->second);
                      leftOutTestsFound->insert(left->first);
                      counts.add("tests left out");
                      i = close;
                      continue;
                    }
                  }
                }
                // (The fixture class a TEST_FIXTURE's expansion names, for its body's this->.)
                if (j + 1 < toks.size() && toks[j + 1].is(P::LBrace))
                  noteFixtureBody(expandMacro(*lm, argText, t.offset), j + 1);
                LocalMacroUse use{t.offset, endOffset(j), {}, lm};
                for (auto [a0, a1] : args)
                  use.args.push_back(a1 > a0 ? std::pair(toks[a0].offset, endOffset(a1 - 1)) : std::pair(toks[a0].offset, toks[a0].offset));
                localMacroUses.push_back(std::move(use));
                counts.add(lm->from == 0 && lm->to == UINT32_MAX && headerMacros && headerMacros->count(std::string(spelling(i))) ? "C header macro expanded" : "local macro expanded");
                ++i; // on to the arguments: run() expands the use
                continue;
              }
            }
          }
        }
        if (t.getKind() == TokenKind::Number)
        {
          // A std::chrono literal (`10ms`, `1.5s`): the library's duration of that count, in double
          // for a fractional one.
          static const std::tuple<const char*, const char*, const char*> suffixes[] = {
            {"ns", "Nanoseconds", "Duration<double, Nano>"}, {"us", "Microseconds", "Duration<double, Micro>"},
            {"ms", "Milliseconds", "Duration<double, Milli>"}, {"min", "Minutes", "Duration<double, Ratio<60>>"},
            {"s", "Seconds", "Duration<double>"}, {"h", "Hours", "Duration<double, Ratio<3600>>"}};
          std::string_view number = spelling(i);
          size_t digits = 0;
          while (digits < number.size() && (isdigit((unsigned char)number[digits]) || number[digits] == '.' || number[digits] == '\''))
            ++digits;
          std::string_view suffix = number.substr(digits);
          bool replaced = false;
          for (const auto& [name, integral, floating] : suffixes)
            if (digits > 0 && suffix == name)
            {
              std::string count;
              for (char c : number.substr(0, digits))
                if (c != '\'')
                  count += c;
              bool fractional = count.find('.') != std::string::npos;
              replace(t.offset, t.length, std::string(fractional ? floating : integral) + "(" + count + ")");
              counts.add("chrono literal");
              replaced = true;
              break;
            }
          if (replaced)
            continue;
        }
        if (t.is(Kw::using_) && toks[i + 1].is(Kw::namespace_) && spelling(i + 2) == "std")
        {
          // `using namespace std::chrono_literals;` (or std::chrono, std::literals): the literals
          // are rewritten where they are written and the library's names are global.
          uint32_t j = i + 2;
          std::string path;
          for (; j < toks.size() && !toks[j].is(P::Semi); ++j)
            path += std::string(spelling(j));
          if (j < toks.size() && (path == "std::chrono_literals" || path == "std::chrono" || path == "std::literals" ||
                                  path == "std::literals::chrono_literals" || path == "std::chrono::literals"))
          {
            remove(t.offset, lineEndAfter(endOffset(j)) - t.offset);
            counts.add("using namespace for chrono removed");
            i = j;
            continue;
          }
        }
        if (t.is(Kw::using_) && i + 4 < toks.size() && spelling(i + 1) == "std" && toks[i + 2].is(P::ColonColon) &&
            toks[i + 3].getKind() == TokenKind::Identifier && toks[i + 4].is(P::Semi))
        {
          // `using std::ostream;`: the library's names are global, so the declaration goes and the
          // file's later uses of the name are the library's (OutStream).
          std::string name(spelling(i + 3));
          if (auto known = stdNames.find(name); known != stdNames.end())
          {
            stdUsingNames[name] = known->second;
            remove(t.offset, lineEndAfter(endOffset(i + 4)) - t.offset);
            counts.add("using-declarations of std names removed");
            i += 4;
            continue;
          }
          todo(t.offset, "using std::" + name + ": a std name the converter has no kpp name for");
        }
        if (t.getKind() == TokenKind::Identifier && !stdUsingNames.empty() &&
            !(i > 0 && (toks[i - 1].is(P::Dot) || toks[i - 1].is(P::Arrow) || toks[i - 1].is(P::ColonColon))))
          if (auto used = stdUsingNames.find(std::string(spelling(i))); used != stdUsingNames.end())
          {
            replace(t.offset, t.length, used->second);
            counts.add("std names of using-declarations renamed");
            continue;
          }
        if (t.is(Kw::using_) && toks[i + 1].is(Kw::namespace_) && !directiveRanges.empty())
        {
          auto removed = std::find_if(directiveRanges.begin(), directiveRanges.end(), [&](const DirectiveRange& r) { return r.begin == t.offset && r.removed; });
          if (removed != directiveRanges.end())
          {
            remove(t.offset, removed->lineEnd - t.offset);
            counts.add("file-scope using-directives replaced by qualified names");
            while (i + 1 < toks.size() && !toks[i].is(P::Semi))
              ++i;
            continue;
          }
        }
        if (t.getKind() == TokenKind::Identifier && !directiveRanges.empty() && !(i > 0 && (toks[i - 1].is(P::ColonColon) || toks[i - 1].is(P::Arrow))) &&
            !memberAccessAt(i))
        {
          // A name a using-directive finds: qualified where the directive goes, and where a global
          // class, alias or variable of the name would be found beside it, the library's or the
          // program's (Lua's `Token` beside MathExpressions' in its tests: C++ never had both in one
          // translation unit).
          std::string name(spelling(i));
          if (const DirectiveRange* via = directiveFinding(name, t.offset);
              via && (via->removed || (libraryTypes && libraryTypes->count(name)) || globalNamed(name)) && !declaredCloser(name, t.offset) &&
              !declaredNameAt(i))
          {
            insert(t.offset, via->key);
            counts.add(via->removed ? "names of a removed using-directive qualified" : "names clashing with a global one qualified by their namespace");
            continue;
          }
        }
        if (t.getKind() == TokenKind::Identifier &&
            (spelling(i) == "__FILE__" || spelling(i) == "__LINE__" || ((spelling(i) == "__FUNCTION__" || spelling(i) == "__func__") && functionNameAt(t.offset))))
        {
          // The preprocessor's file name and line: the literal values, since kpp has no preprocessor.
          replace(t.offset, endOffset(i) - t.offset, withFileAndLine(std::string(spelling(i)), t.offset));
          counts.add("__FILE__, __LINE__, __FUNCTION__ replaced");
          continue;
        }
        if (t.getKind() == TokenKind::Identifier && spelling(i) == "Vector" && !(i >= 2 && toks[i - 1].is(P::ColonColon) && spelling(i - 2) == "std") && !memberAccessAt(i))
        {
          // The game's own 2D `Vector` class is Vector2D in kpp, where Vector is the library's std::vector.
          replace(t.offset, endOffset(i) - t.offset, "Vector2D");
          counts.add("Vector renamed to Vector2D");
          continue;
        }
        if (t.getKind() == TokenKind::Identifier && spelling(i) == "Map" && !(i >= 2 && toks[i - 1].is(P::ColonColon) && spelling(i - 2) == "std") && !memberAccessAt(i))
        {
          // The game's `class Map` is GameMap in kpp, where Map<K,V> is the library's std::map.
          replace(t.offset, endOffset(i) - t.offset, "GameMap");
          counts.add("Map renamed to GameMap");
          continue;
        }
        if (t.getKind() == TokenKind::Identifier && spelling(i) == "Optional" && !(i >= 2 && toks[i - 1].is(P::ColonColon) && spelling(i - 2) == "std") && !memberAccessAt(i))
        {
          // The game's sentinel-based `Optional<T, empty>` is GameOptional in kpp, where Optional<T> is the library's std::optional.
          replace(t.offset, endOffset(i) - t.offset, "GameOptional");
          counts.add("Optional renamed to GameOptional");
          continue;
        }
        if (t.getKind() == TokenKind::Identifier && spelling(i) == "Pair" && !(i >= 2 && toks[i - 1].is(P::ColonColon) && spelling(i - 2) == "std") && !memberAccessAt(i))
        {
          // The game's `Pair<K,V>` is GamePair in kpp, where Pair<A,B> is the library's std::pair.
          replace(t.offset, endOffset(i) - t.offset, "GamePair");
          counts.add("Pair renamed to GamePair");
          continue;
        }
        if (t.getKind() == TokenKind::Identifier && spelling(i) == "RuntimeError" && !(i >= 2 && toks[i - 1].is(P::ColonColon) && spelling(i - 2) == "std") && !memberAccessAt(i))
        {
          // The game's `class RuntimeError : public std::runtime_error` is GameRuntimeError in kpp,
          // where RuntimeError is the library's std::runtime_error.
          replace(t.offset, endOffset(i) - t.offset, "GameRuntimeError");
          counts.add("RuntimeError renamed to GameRuntimeError");
          continue;
        }
        if (t.getKind() == TokenKind::Identifier && (spelling(i) == "Clock" || spelling(i) == "TimePoint"))
        {
          // `using Clock = std::chrono::steady_clock;` and `using TimePoint = Clock::time_point;` at
          // global scope (three Net headers), where the library declares Clock and TimePoint: those
          // are GameClock and GameTimePoint, at their declarations and wherever the unqualified (or
          // `::`-qualified) name finds them. A class's own Clock (MainLoop::Clock) keeps its name.
          bool qualified = i >= 2 && toks[i - 1].is(P::ColonColon) &&
                           (toks[i - 2].getKind() == TokenKind::Identifier || toks[i - 2].is(P::Greater) || toks[i - 2].is(P::Shr) ||
                            toks[i - 2].is(P::RParen));
          bool global = i >= 1 && toks[i - 1].is(P::ColonColon) && !qualified;
          std::string name(spelling(i));
          if (!qualified && (global || !shadowedAt(name, t.offset)))
          {
            replace(t.offset, endOffset(i) - t.offset, "Game" + name);
            counts.add(name + " renamed to Game" + name);
            continue;
          }
        }
        if (t.getKind() == TokenKind::Identifier && i >= 2 && toks[i - 1].is(P::Tilde) && (toks[i - 2].is(P::Dot) || toks[i - 2].is(P::Arrow)) &&
            i + 1 < toks.size() && (toks[i + 1].is(P::LParen) || toks[i + 1].is(P::Less)))
        {
          // `value.~vector()`: the destructor of a std class, by the class's kpp name.
          auto it = stdNames.find(std::string(spelling(i)));
          if (it != stdNames.end() && it->second.find("::") == std::string::npos && isupper((unsigned char)it->second[0]))
          {
            replace(t.offset, endOffset(i) - t.offset, it->second);
            counts.add("std renamed");
          }
        }
        if (t.getKind() == TokenKind::Identifier && spelling(i) == "std" && i + 5 < toks.size() && toks[i + 1].is(P::ColonColon) &&
            spelling(i + 2) == "pair" && toks[i + 3].is(P::Less) && toks[i + 4].is(Kw::const_))
        {
          // `std::pair<const K, V>`, a map's entry as C++ spells it (AsteroidCollectorNavmesh's
          // `for (const std::pair<const ChunkPosition, std::unique_ptr<...>>& entry : records)`): a
          // kpp Map's entries are Pair<K, V> (docs/stdlib/map.md), so the key's const goes. A const
          // that belongs to a pointer's or a reference's target (`std::pair<const T*, float>`) stays.
          uint32_t k = i + 5;
          for (int depth = 0; k < toks.size(); ++k)
          {
            if (toks[k].is(P::Less) || toks[k].is(P::LParen))
              ++depth;
            else if (toks[k].is(P::Greater) || toks[k].is(P::RParen))
              --depth;
            else if (toks[k].is(P::Shr))
              depth -= 2;
            if (depth < 0 || (depth == 0 && (toks[k].is(P::Comma) || toks[k].is(P::Semi))))
              break;
          }
          if (k < toks.size() && toks[k].is(P::Comma) && !(toks[k - 1].is(P::Star) || toks[k - 1].is(P::Amp) || toks[k - 1].is(P::AndAnd)))
          {
            remove(toks[i + 4].offset, toks[i + 5].offset - toks[i + 4].offset);
            counts.add("const keys of map entries dropped");
          }
        }
        if (t.getKind() == TokenKind::Identifier && spelling(i) == "std" && toks[i + 1].is(P::ColonColon) &&
            toks[i + 2].getKind() == TokenKind::Identifier)
        {
          std::string name(spelling(i + 2));
          if (variants && !variants->all.empty() && (name == "get" || name == "holds_alternative" || name == "get_if" || name == "visit"))
            recordVariantUse(name, i);
          if (name == "source_location" && i + 6 < toks.size() && toks[i + 3].is(P::ColonColon) && spelling(i + 4) == "current" &&
              toks[i + 5].is(P::LParen) && toks[i + 6].is(P::RParen))
          {
            // `std::source_location loc = std::source_location::current()` as a default argument:
            // kpp's caller location, which the compiler fills at each call (design.md section 7).
            replace(t.offset, endOffset(i + 6) - t.offset, "caller");
            counts.add("source_location::current() to caller");
            i += 6;
            continue;
          }
          if ((name == "views" || name == "ranges") && i + 4 < toks.size() && toks[i + 3].is(P::ColonColon) && toks[i + 4].getKind() == TokenKind::Identifier)
          {
            // std::views and std::ranges::subrange are the library's views (Views.kpp, docs/stdlib/views.md);
            // std::ranges::find and find_if over a whole range its algorithms' range forms (algorithms.md).
            static const std::map<std::string, std::string> viewNames = {
              {"views::iota", "iotaView"}, {"views::transform", "transformView"}, {"views::reverse", "reverseView"},
              {"ranges::subrange", "Subrange"}, {"ranges::find", "::find"}, {"ranges::find_if", "::findIf"}};
            auto v = viewNames.find(name + "::" + std::string(spelling(i + 4)));
            if (v != viewNames.end())
            {
              bool qualify = v->second[0] != ':' && shadowedAt(v->second, t.offset);
              replace(t.offset, endOffset(i + 4) - t.offset, qualify ? "::" + v->second : v->second);
              counts.add(name == "views" || v->second == "Subrange" ? "std::views renamed" : "std::ranges algorithms renamed");
              i += 4;
              continue;
            }
            counts.add("std::" + name + "::" + std::string(spelling(i + 4)) + " (no mapping)");
          }
          if (name == "locale" && i + 13 < toks.size() && toks[i + 3].is(P::ColonColon) && spelling(i + 4) == "global" && toks[i + 5].is(P::LParen) &&
              spelling(i + 6) == "std" && spelling(i + 8) == "locale" && spelling(i + 10) == "classic" && toks[i + 11].is(P::LParen) &&
              toks[i + 12].is(P::RParen) && toks[i + 13].is(P::RParen) && i + 14 < toks.size() && toks[i + 14].is(P::Semi))
          {
            // `std::locale::global(std::locale::classic());` (Main.cpp): the library's text conversion is
            // the classic ("C") locale's always, so making it the global one changes nothing.
            replace(t.offset, endOffset(i + 14) - t.offset, "/* std::locale::global(std::locale::classic()): kpp converts text as the classic locale */");
            counts.add("std::locale::global(classic) removed");
            i += 14;
            continue;
          }
          if ((name == "in_place_type_t" || name == "in_place_type") && i > 0 && toks[i - 1].is(P::LParen) && i + 3 < toks.size() && toks[i + 3].is(P::Less))
          {
            // `f(std::in_place_type_t<T>(), a, b)`, a std::variant built in place as its alternative
            // T (DrawCommandBatch's batches.emplace_back): the alternative's own constructor,
            // `f(T(a, b))`, which sets the typed union's alternative from the value.
            uint32_t j = i + 4;
            int depth = 0;
            for (; j < toks.size(); ++j)
            {
              if (toks[j].is(P::Less))
                ++depth;
              else if (toks[j].is(P::Greater) && depth-- == 0)
                break;
              else if (toks[j].is(P::Semi) || toks[j].getKind() == TokenKind::End)
                break;
            }
            uint32_t after = j + 1;
            if (j < toks.size() && toks[j].is(P::Greater) && name == "in_place_type_t" && after + 1 < toks.size() && toks[after].is(P::LParen) && toks[after + 1].is(P::RParen))
              after += 2;
            uint32_t close = closingParenToken(i - 1);
            if (j < toks.size() && toks[j].is(P::Greater) && close != UINT32_MAX && after <= close && (toks[after].is(P::Comma) || after == close))
            {
              remove(t.offset, endOffset(i + 3) - t.offset); // `std::in_place_type_t<`
              if (after == close)
                replace(toks[j].offset, toks[close].offset - toks[j].offset, "()");
              else
              {
                uint32_t next = after + 1;
                replace(toks[j].offset, toks[next].offset - toks[j].offset, "(");
                insert(toks[close].offset, ")");
              }
              counts.add("std::in_place_type to the alternative's constructor");
              i = j;
              continue;
            }
          }
          if ((name == "memory_order" && i + 4 < toks.size() && toks[i + 3].is(P::ColonColon) && toks[i + 4].getKind() == TokenKind::Identifier) ||
              name.rfind("memory_order_", 0) == 0)
          {
            // A memory order passed to an atomic operation (`value.load(std::memory_order::relaxed)`,
            // TimeStatistics' AtomicNanoseconds): the library's Atomic takes none, every operation is
            // sequentially consistent (docs/stdlib/atomic.md), which is at least as strong. The
            // argument goes with its comma; elsewhere a memory order stays and fails to compile.
            uint32_t last = name == "memory_order" ? i + 4 : i + 2;
            bool before = i > 0 && (toks[i - 1].is(P::Comma) || toks[i - 1].is(P::LParen));
            bool after = last + 1 < toks.size() && (toks[last + 1].is(P::Comma) || toks[last + 1].is(P::RParen));
            if (before && after)
            {
              if (toks[i - 1].is(P::Comma))
                remove(toks[i - 1].offset, endOffset(last) - toks[i - 1].offset);
              else if (toks[last + 1].is(P::Comma))
                remove(t.offset, toks[last + 2].offset - t.offset);
              else
                remove(t.offset, endOffset(last) - t.offset);
              counts.add("memory order arguments dropped");
              i = last;
              continue;
            }
          }
          if (name == "thread" && i + 4 < toks.size() && toks[i + 3].is(P::ColonColon) && spelling(i + 4) == "hardware_concurrency")
          {
            // A static member whose library name differs: std::thread::hardware_concurrency().
            replace(t.offset, endOffset(i + 4) - t.offset, shadowedAt("Thread", t.offset) ? "::Thread::hardwareConcurrency" : "Thread::hardwareConcurrency");
            counts.add("std static member renamed");
            i += 4;
            continue;
          }
          if (name == "chrono" && i + 4 < toks.size() && toks[i + 3].is(P::ColonColon) && toks[i + 4].getKind() == TokenKind::Identifier)
          {
            // std::chrono's names are the library's time types (Time.kpp, docs/stdlib/time.md).
            static const std::map<std::string_view, std::string> chronoNames = {
              {"nanoseconds", "Nanoseconds"}, {"microseconds", "Microseconds"}, {"milliseconds", "Milliseconds"},
              {"seconds", "Seconds"}, {"minutes", "Minutes"}, {"hours", "Hours"}, {"days", "Days"},
              {"duration", "Duration"}, {"time_point", "TimePoint"}, {"steady_clock", "SteadyClock"},
              {"system_clock", "SystemClock"}, {"high_resolution_clock", "HighResolutionClock"},
              {"duration_cast", "durationCast"}, {"time_point_cast", "timePointCast"}, {"floor", "floor"}, {"ceil", "ceil"}};
            auto c = chronoNames.find(spelling(i + 4));
            if (c != chronoNames.end())
            {
              std::string chronoTo = c->second;
              if (shadowedAt(chronoTo, t.offset))
                chronoTo = "::" + chronoTo;
              replace(t.offset, endOffset(i + 4) - t.offset, chronoTo);
              counts.add("std::chrono renamed");
              i += 4;
              continue;
            }
            counts.add("std::chrono::" + std::string(spelling(i + 4)) + " (no mapping)");
          }
          auto it = stdNames.find(name);
          std::string to = it == stdNames.end() ? name : it->second;
          static const std::map<std::string, size_t> containerArguments = {
            {"vector", 1}, {"deque", 1}, {"list", 1}, {"forward_list", 1}, {"set", 2}, {"multiset", 2}, {"map", 3}, {"multimap", 3},
            {"unordered_set", 3}, {"unordered_multiset", 3}, {"unordered_map", 4}, {"unordered_multimap", 4}};
          auto ca = containerArguments.find(name);
          if (ca != containerArguments.end() && i + 3 < toks.size() && toks[i + 3].is(P::Less))
          {
            // `std::vector<T, Allocate>`: the library's containers have no allocator parameter, so the
            // argument goes (CommonUtil's eraseIf overloads); a template parameter used only there
            // goes from its template header too (dropAllocatorParameters).
            std::vector<std::pair<uint32_t, uint32_t>> args; // token ranges of the arguments
            uint32_t j = i + 4, argStart = j;
            uint32_t close = UINT32_MAX; // offset of the `>` that closes the container's arguments
            int angle = 0, paren = 0;
            for (; j < toks.size(); ++j)
            {
              if (toks[j].is(P::LParen))
                ++paren;
              else if (toks[j].is(P::RParen))
              {
                if (--paren < 0)
                  break;
              }
              else if (paren > 0)
                continue;
              else if (toks[j].is(P::Less))
                ++angle;
              else if (toks[j].is(P::Greater) || toks[j].is(P::Shr))
              {
                // `>>` one level in closes the last argument's own arguments and then the container's.
                if (angle == 0 || (angle == 1 && toks[j].is(P::Shr)))
                {
                  close = toks[j].offset + angle;
                  break;
                }
                angle -= toks[j].is(P::Shr) ? 2 : 1;
              }
              else if (toks[j].is(P::Comma) && angle == 0)
              {
                args.push_back({argStart, j});
                argStart = j + 1;
              }
              else if (toks[j].is(P::Semi) || toks[j].is(P::LBrace) || toks[j].getKind() == TokenKind::End)
                break;
            }
            if (close != UINT32_MAX)
            {
              args.push_back({argStart, j});
              if (args.size() > ca->second)
              {
                // From the end of the last argument kept to the closing `>` (the first of a `>>`).
                uint32_t from = endOffset(args[ca->second - 1].second - 1);
                remove(from, close - from);
                for (size_t a = ca->second; a < args.size(); ++a)
                  for (uint32_t k = args[a].first; k < args[a].second; ++k)
                    if (toks[k].getKind() == TokenKind::Identifier)
                      droppedAllocators[toks[k].offset] = std::string(spelling(k));
                counts.add("allocator arguments dropped");
              }
            }
          }
          if ((name == "unique_ptr" || name == "make_unique" || name == "make_unique_for_overwrite") && i + 3 < toks.size() && toks[i + 3].is(P::Less))
          {
            // `unique_ptr<T[]>`, `make_unique<T[]>(n)` and `make_unique_for_overwrite<T[]>(n)`: the
            // array form is its own class.
            uint32_t j = i + 4;
            int depth = 0;
            for (; j < toks.size(); ++j)
            {
              if (toks[j].is(P::Less))
                ++depth;
              else if (toks[j].is(P::Greater) || toks[j].is(P::Shr))
              {
                if (depth == 0)
                  break;
                depth -= toks[j].is(P::Shr) ? 2 : 1;
                if (depth < 0)
                  break;
              }
              else if (toks[j].is(P::Semi) || toks[j].getKind() == TokenKind::End)
                break;
            }
            if (j < toks.size() && j >= i + 6 && toks[j - 1].is(P::RBracket) && toks[j - 2].is(P::LBracket))
            {
              to = name == "unique_ptr" ? "UniqueArray" : name == "make_unique" ? "makeUniqueArray" : "makeUniqueArrayForOverwrite";
              remove(toks[j - 2].offset, endOffset(j - 1) - toks[j - 2].offset);
              counts.add("unique_ptr<T[]> to UniqueArray");
            }
          }
          auto variable = stdNames.find(name + "_v");
          auto alias = stdNames.find(name + "_t");
          if (alias != stdNames.end() && i + 3 < toks.size() && toks[i + 3].is(P::Less))
          {
            // `std::make_unsigned<T>::type`: the trait's type, which is the library's MakeUnsigned<T>
            // (what std::make_unsigned_t<T> converts to).
            uint32_t j = i + 4;
            for (int depth = 0; j < toks.size(); ++j)
            {
              if (toks[j].is(P::Less))
                ++depth;
              else if (toks[j].is(P::Greater) || toks[j].is(P::Shr))
              {
                if (depth == 0)
                  break;
                depth -= toks[j].is(P::Shr) ? 2 : 1;
                if (depth < 0)
                  break;
              }
              else if (toks[j].is(P::Semi) || toks[j].getKind() == TokenKind::End)
                break;
            }
            if (j + 2 < toks.size() && toks[j + 1].is(P::ColonColon) && spelling(j + 2) == "type")
            {
              to = alias->second;
              it = alias;
              remove(toks[j + 1].offset, endOffset(j + 2) - toks[j + 1].offset);
              counts.add("trait class to its type");
            }
          }
          if (name.compare(0, 3, "is_") == 0 && variable != stdNames.end() && i + 3 < toks.size() && toks[i + 3].is(P::Less))
          {
            // `std::is_member_function_pointer<T>()` and `std::is_const<T>::value`: the trait's
            // value, which is the compiler's isMemberFunctionPointer<T>; kpp has no class form.
            uint32_t j = i + 4;
            int depth = 0;
            for (; j < toks.size(); ++j)
            {
              if (toks[j].is(P::Less))
                ++depth;
              else if (toks[j].is(P::Greater) || toks[j].is(P::Shr))
              {
                if (depth == 0)
                  break;
                depth -= toks[j].is(P::Shr) ? 2 : 1;
                if (depth < 0)
                  break;
              }
              else if (toks[j].is(P::Semi) || toks[j].getKind() == TokenKind::End)
                break;
            }
            bool call = j + 2 < toks.size() && ((toks[j + 1].is(P::LParen) && toks[j + 2].is(P::RParen)) ||
                                                (toks[j + 1].is(P::LBrace) && toks[j + 2].is(P::RBrace)));
            bool value = j + 2 < toks.size() && toks[j + 1].is(P::ColonColon) && spelling(j + 2) == "value";
            if (call || value)
            {
              to = variable->second;
              it = variable;
              remove(toks[j + 1].offset, endOffset(j + 2) - toks[j + 1].offset);
              counts.add("trait class to its value");
            }
          }
          counts.add(it == stdNames.end() ? "std::" + name + " (no mapping)" : "std renamed");
          if (to.find("::") == std::string::npos && shadowedAt(to, t.offset))
          {
            // `std::string` inside a class or namespace with its own String, `std::next(source)` in
            // a class whose base has a `next` member (Loader through UpdatableEntity's list hook).
            to = "::" + to;
            counts.add("std name qualified as global");
          }
          replace(t.offset, endOffset(i + 2) - t.offset, to);
          if (it != stdNames.end() && i + 4 < toks.size() && toks[i + 3].is(P::ColonColon) && spelling(i + 4) == name &&
              isIdentChar(to.back()))
          {
            // The class's constructors by its name: `using std::string::string;` inherits them
            // (TestFlatMap's MoveOnlyString), `using String::String;` in kpp.
            replace(toks[i + 4].offset, toks[i + 4].length, to.substr(to.rfind(':') == std::string::npos ? 0 : to.rfind(':') + 1));
            counts.add("std constructor name renamed");
            i += 4;
            continue;
          }
          i += 2;
          continue;
        }
        if (t.is(Kw::typename_) && toks[i + 1].getKind() == TokenKind::Identifier && toks[i + 2].is(P::ColonColon))
        {
          remove(t.offset, toks[i + 1].offset - t.offset); // `typename T::x` -> `T::x`
          counts.add("typename dropped");
          continue;
        }
        if (t.is(Kw::template_) && i > 0 && (toks[i - 1].is(P::Dot) || toks[i - 1].is(P::Arrow) || toks[i - 1].is(P::ColonColon)))
        {
          remove(t.offset, toks[i + 1].offset - t.offset); // `x.template f<T>()` -> `x.f<T>()`
          counts.add("template keyword dropped");
          continue;
        }
        if (t.getKind() == TokenKind::Identifier && toks[i + 1].is(P::LParen))
        {
          // Members of std types whose library spelling differs and that no game class declares:
          // std::type_info's hash_code is TypeInfo's hashCode. (Optional has std's value_or: the
          // game's OptionalFundamental declares one of its own.)
          static const std::map<std::string_view, std::string> memberRenames = {{"hash_code", "hashCode"}};
          auto mr = memberRenames.find(spelling(i));
          if (mr != memberRenames.end() && i > 0 && (toks[i - 1].is(P::Dot) || toks[i - 1].is(P::Arrow)))
          {
            replace(t.offset, t.length, mr->second);
            counts.add("std member renamed");
            continue;
          }
        }
        if (t.getKind() == TokenKind::Identifier && i > 0 && (toks[i - 1].is(P::Dot) || toks[i - 1].is(P::Arrow)))
        {
          // The C headers' macros that name a member path (in_addr's `#define s_addr S_un.S_addr`,
          // in6_addr's s6_addr, whose `_S6_un._S6_u8` are macros too): the import has the structs' own
          // members, so a use gets the path.
          static const std::map<std::string_view, std::string> memberMacros = {{"s_addr", "S_un.S_addr"}, {"s6_addr", "u.Byte"}};
          if (auto mm = memberMacros.find(spelling(i)); mm != memberMacros.end())
          {
            replace(t.offset, t.length, mm->second);
            counts.add("C member macros expanded");
            continue;
          }
        }
        if (t.getKind() == TokenKind::Identifier)
        {
          std::string name(spelling(i));
          auto km = knownMacros.find(name);
          if (km != knownMacros.end())
          {
            if (km->second.empty() && toks[i + 1].is(P::LParen) && (name == "FORMAT_ATTR" || name == "TAG_FORMAT_ATTR"))
            {
              // `FORMAT_ATTR(4, 5)` after a parameter list: the attribute macro and its arguments go.
              int depth = 0;
              uint32_t j = i + 1;
              for (; j < toks.size(); ++j)
              {
                if (toks[j].is(P::LParen))
                  ++depth;
                else if (toks[j].is(P::RParen) && --depth == 0)
                  break;
              }
              if (j < toks.size())
              {
                remove(t.offset, endOffset(j) - t.offset);
                counts.add("macro use rewritten");
                i = j;
                continue;
              }
            }
            replace(t.offset, t.length, km->second);
            counts.add("macro use rewritten");
            continue;
          }
          if (name == "__try")
          {
            // Structured exception handling: kpp's exceptions are SEH underneath, so `try` catches
            // what `__try` did; the filter expression is lost (a todo).
            replace(t.offset, t.length, "try");
            counts.add("SEH __try to try");
            continue;
          }
          if (name == "__except" && toks[i + 1].is(P::LParen))
          {
            int depth = 0;
            uint32_t j = i + 1;
            for (; j < toks.size(); ++j)
            {
              if (toks[j].is(P::LParen))
                ++depth;
              else if (toks[j].is(P::RParen) && --depth == 0)
                break;
            }
            if (j < toks.size())
            {
              todo(t.offset, "SEH __except filter dropped: " + std::string(text.substr(toks[i + 1].offset, endOffset(j) - toks[i + 1].offset)));
              replace(t.offset, endOffset(j) - t.offset, "catch (...)");
              counts.add("SEH __except to catch (...)");
              i = j;
              continue;
            }
          }
          // Util/Enums.hpp: ENUM(Name, Type, A, B, ...) is a class holding an `enum class Enum` with
          // conversions, validation, str() and parsing from text; ENUM_IMPL is that class's body and
          // ENUM_IMPL_NOSTRING the body without the text part. Expanded as the macros expand.
          if ((name == "ENUM" || name == "ENUM_IMPL" || name == "ENUM_IMPL_NOSTRING") && toks[i + 1].is(P::LParen))
          {
            std::vector<std::string> args;
            uint32_t j = i + 2;
            int depth = 0;
            bool simple = true;
            uint32_t argStart = j;
            for (; j < toks.size(); ++j)
            {
              if (toks[j].is(P::LParen))
                ++depth;
              else if (toks[j].is(P::RParen) && depth-- == 0)
                break;
              else if (toks[j].is(P::Comma) && depth == 0)
              {
                simple = simple && j == argStart + 1;
                args.push_back(std::string(spelling(argStart)));
                argStart = j + 1;
              }
            }
            if (j < toks.size() && j > argStart)
            {
              simple = simple && j == argStart + 1;
              args.push_back(std::string(spelling(argStart)));
            }
            // Name, Type and the enumerators, each a single name (types such as `uint8_t` are one).
            if (j < toks.size() && simple && args.size() >= 3)
            {
              std::vector<std::string> values(args.begin() + 2, args.end());
              std::string body = enumClassBody(args[0], args[1], values, name != "ENUM_IMPL_NOSTRING");
              replace(t.offset, endOffset(j) - t.offset, name == "ENUM" ? "class " + args[0] + "\n{\n" + body + "}" : body);
              counts.add(name + " expanded");
              i = j;
              continue;
            }
            todo(t.offset, name + " with arguments that are not single names: left as written");
          }
          // BITFIELD(Name, member1, member2, ...) -> class Name : public BitFieldBase { enum class Enum { members }; using enum Enum; ... }
          if (name == "BITFIELD" && toks[i + 1].is(P::LParen))
          {
            int depth = 0;
            uint32_t j = i + 1;
            for (; j < toks.size(); ++j)
            {
              if (toks[j].is(P::LParen))
                ++depth;
              else if (toks[j].is(P::RParen) && --depth == 0)
                break;
            }
            if (j < toks.size())
            {
              // Collect all arguments: first is the Name, rest are enum members.
              std::vector<std::string> args;
              uint32_t argStart = i + 2;
              int d = 0;
              for (uint32_t k = argStart; k <= j; ++k)
              {
                if (toks[k].is(P::LParen) || toks[k].is(P::LBracket) || toks[k].is(P::LBrace))
                  ++d;
                else if (toks[k].is(P::RParen) || toks[k].is(P::RBracket) || toks[k].is(P::RBrace))
                {
                  if (d > 0) { --d; continue; }
                  // closing RParen; after a trailing comma (`Unhandled, )`) the last argument is
                  // empty: k is argStart, and endOffset(k - 1) would come before it.
                  if (k == argStart)
                    break;
                  std::string a(text.substr(toks[argStart].offset, endOffset(k - 1) - toks[argStart].offset));
                  while (!a.empty() && isspace((unsigned char)a.front())) a.erase(a.begin());
                  while (!a.empty() && isspace((unsigned char)a.back())) a.pop_back();
                  if (!a.empty())
                    args.push_back(a);
                  break;
                }
                else if (toks[k].is(P::Comma) && d == 0)
                {
                  std::string a(text.substr(toks[argStart].offset, toks[k].offset - toks[argStart].offset));
                  while (!a.empty() && isspace((unsigned char)a.front())) a.erase(a.begin());
                  while (!a.empty() && isspace((unsigned char)a.back())) a.pop_back();
                  args.push_back(a);
                  argStart = k + 1;
                }
              }
              if (args.size() >= 2)
              {
                std::string bfName = args[0];
                std::string members;
                for (size_t m = 1; m < args.size(); ++m)
                {
                  if (m > 1) members += ", ";
                  members += args[m];
                }
                std::string expanded =
                  "class " + bfName + " : public BitFieldBase\n"
                  "{\n"
                  "public:\n"
                  "  enum class Enum\n"
                  "  {\n"
                  "    " + members + "\n"
                  "  };\n"
                  "  constexpr static size_t value_count = " + std::to_string(args.size() - 1) + ";\n"
                  "  using UnderlyingType = SmallBitset<value_count>::UnderlyingType;\n"
                  "\n"
                  "  constexpr " + bfName + "() = default;\n"
                  "  constexpr " + bfName + "(InitializerList<Enum> values) : bits(" + bfName + "::toBits(values)) {}\n"
                  "  constexpr " + bfName + "(Enum value) : bits(static_cast<uint64_t>(1ULL << static_cast<size_t>(value))) {}\n"
                  "\n"
                  "private:\n"
                  "  constexpr " + bfName + "(SmallBitset<value_count> bits) : bits(bits) {}\n"
                  "\n"
                  "public:\n"
                  "  " + bfName + "& add(Enum value) { this->bits.set(static_cast<size_t>(value)); return *this; }\n"
                  "  " + bfName + "& remove(Enum value) { this->bits.reset(static_cast<size_t>(value)); return *this; }\n"
                  "  " + bfName + "& flip(Enum value) { this->bits.flip(static_cast<size_t>(value)); return *this; }\n"
                  "  " + bfName + "& set(Enum value, bool boolValue) { boolValue ? this->add(value) : this->remove(value); return *this; }\n"
                  "\n"
                  "  " + bfName + "& set() { this->bits.set(); return *this; }\n"
                  "  " + bfName + "& reset() { this->bits.reset(); return *this; }\n"
                  "  " + bfName + "& flip() { this->bits.flip(); return *this; }\n"
                  "\n"
                  "  constexpr bool none() const { return this->bits.none(); }\n"
                  "  constexpr bool any() const { return this->bits.any(); }\n"
                  "  constexpr bool all() const { return this->bits.all(); }\n"
                  "  template<Enum value>\n"
                  "  constexpr bool contains() const { return this->bits.test<static_cast<size_t>(value)>(); }\n"
                  "  constexpr bool checkedContains(Enum value) const { return this->bits.test(static_cast<size_t>(value)); }\n"
                  "\n"
                  "  constexpr bool operator==(const " + bfName + " other) const { return this->bits == other.bits; }\n"
                  "  constexpr bool operator!=(const " + bfName + " other) const { return this->bits != other.bits; }\n"
                  "  constexpr " + bfName + " operator|(Enum value) const { return (*this) | " + bfName + "(value); }\n"
                  "  constexpr " + bfName + " operator|(const " + bfName + " other) const { return " + bfName + "(this->bits | other.bits); }\n"
                  "  constexpr " + bfName + " operator&(Enum value) const { return (*this) & " + bfName + "(value); }\n"
                  "  constexpr " + bfName + " operator&(const " + bfName + " other) const { return " + bfName + "(this->bits & other.bits); }\n"
                  "  " + bfName + "& operator&=(const " + bfName + "& other) { this->bits &= other.bits; return *this; }\n"
                  "  " + bfName + "& operator|=(const " + bfName + "& other) { this->bits |= other.bits; return *this; }\n"
                  "\n"
                  "  constexpr SmallBitset<value_count>::UnderlyingType getBits() const { return this->bits.getBits(); }\n"
                  "  static " + bfName + " fromBits(uint64_t bits) { " + bfName + " value; value.bits = SmallBitset<value_count>(bits); return value; }\n"
                  "\n"
                  "  using enum Enum;\n"
                  "private:\n"
                  "  constexpr static uint64_t toBits(InitializerList<Enum> values)\n"
                  "  {\n"
                  "    uint64_t acc = 0;\n"
                  "    for (auto value : values)\n"
                  "      acc |= static_cast<uint64_t>(1ULL << static_cast<size_t>(value));\n"
                  "    return acc;\n"
                  "  }\n"
                  "\n"
                  "  SmallBitset<value_count> bits;\n"
                  "};";
                // The expansion ends the class with its own `;`, so a `;` after the use goes with it.
                uint32_t end = j;
                if (end + 1 < toks.size() && toks[end + 1].is(P::Semi))
                  end = j + 1;
                replace(t.offset, endOffset(end) - t.offset, expanded);
                counts.add("BITFIELD expanded");
                i = end;
                continue;
              }
            }
          }
          // MAKE_OPTIONAL(BaseClass, EmptyExpr) -> what the macro defines: the OptionalEmptyValue<BaseClass>
          // specialisation returning EmptyExpr, and `using OptionalBaseClass = GameOptional<BaseClass>;`.
          if (name == "MAKE_OPTIONAL" && toks[i + 1].is(P::LParen))
          {
            int depth = 0;
            uint32_t j = i + 1;
            for (; j < toks.size(); ++j)
            {
              if (toks[j].is(P::LParen))
                ++depth;
              else if (toks[j].is(P::RParen) && --depth == 0)
                break;
            }
            if (j < toks.size())
            {
              // First argument is BaseClass name.
              uint32_t firstArgStart = i + 2;
              uint32_t firstArgEnd = firstArgStart;
              for (; firstArgEnd < j; ++firstArgEnd)
                if (toks[firstArgEnd].is(P::Comma))
                  break;
              if (firstArgEnd < j)
              {
                std::string baseClass(text.substr(toks[firstArgStart].offset, toks[firstArgEnd].offset - toks[firstArgStart].offset));
                while (!baseClass.empty() && isspace((unsigned char)baseClass.back())) baseClass.pop_back();
                while (!baseClass.empty() && isspace((unsigned char)baseClass.front())) baseClass.erase(baseClass.begin());
                std::string empty(text.substr(toks[firstArgEnd + 1].offset, toks[j].offset - toks[firstArgEnd + 1].offset));
                while (!empty.empty() && isspace((unsigned char)empty.back())) empty.pop_back();
                while (!empty.empty() && isspace((unsigned char)empty.front())) empty.erase(empty.begin());
                std::string expanded = "template<>\n"
                                       "struct OptionalEmptyValue<" + baseClass + ">\n"
                                       "{\n"
                                       "  constexpr static " + baseClass + " value() { return " + empty + "; }\n"
                                       "};\n"
                                       "using Optional" + baseClass + " = GameOptional<" + baseClass + ">;";
                uint32_t end = j;
                if (end + 1 < toks.size() && toks[end + 1].is(P::Semi))
                  end = j + 1;
                replace(t.offset, endOffset(end) - t.offset, expanded);
                counts.add("MAKE_OPTIONAL expanded");
                i = end;
                continue;
              }
            }
          }
          auto am = argumentMacros.find(name);
          if (am != argumentMacros.end() && toks[i + 1].is(P::LParen))
          {
            // Recorded with its arguments' token ranges; the tokens inside stay in the stream for
            // the other rewrites, and run() replaces the whole use.
            ArgumentMacroUse use{t.offset, 0, {}, &am->second};
            uint32_t j = i + 2, argStart = j;
            int depth = 0;
            for (; j < toks.size(); ++j)
            {
              const Token& tj = toks[j];
              if (tj.is(P::LParen) || tj.is(P::LBracket) || tj.is(P::LBrace))
                ++depth;
              else if ((tj.is(P::RParen) || tj.is(P::RBracket) || tj.is(P::RBrace)) && depth-- == 0)
                break;
              else if (tj.is(P::Comma) && depth == 0)
              {
                use.args.push_back({toks[argStart].offset, endOffset(j - 1)});
                argStart = j + 1;
              }
            }
            if (j < toks.size() && toks[j].is(P::RParen) && j > argStart)
            {
              use.args.push_back({toks[argStart].offset, endOffset(j - 1)});
              use.end = endOffset(j);
            }
            if (use.end && use.args.size() == arityOf(am->second))
            {
              argumentMacroUses.push_back(std::move(use));
              counts.add("argument macro expanded");
            }
            else
              todo(t.offset, name + " with arguments it does not take: left as written");
            continue;
          }
          auto kf = knownFunctionMacros.find(name);
          if (kf != knownFunctionMacros.end() && toks[i + 1].is(P::LParen))
          {
            int depth = 0;
            uint32_t j = i + 1;
            for (; j < toks.size(); ++j)
            {
              if (toks[j].is(P::LParen))
                ++depth;
              else if (toks[j].is(P::RParen) && --depth == 0)
                break;
            }
            if (j < toks.size())
            {
              // The text before $1 replaces `MACRO(`, the text after it the closing `)`; the
              // argument's own tokens stay in the stream for the other rewrites.
              size_t dollar = kf->second.find("$1");
              std::string before = withFileAndLine(dollar == std::string::npos ? kf->second : kf->second.substr(0, dollar), t.offset);
              std::string after = withFileAndLine(dollar == std::string::npos ? std::string() : kf->second.substr(dollar + 2), t.offset);
              if (dollar == std::string::npos && j > i + 2)
              {
                // The arguments go with the macro (LOG_DEBUG is `((void)0)` here); the use stays
                // readable as a comment.
                std::string use(text.substr(t.offset, endOffset(j) - t.offset));
                for (size_t p = use.find("*/"); p != std::string::npos; p = use.find("*/", p))
                  use.replace(p, 2, "* /");
                replace(t.offset, endOffset(j) - t.offset, before + " /* " + use + " */");
              }
              else if (dollar == std::string::npos)
                replace(t.offset, endOffset(j) - t.offset, before);
              else
              {
                replace(t.offset, toks[i + 2].offset - t.offset, before);
                replace(toks[j].offset, toks[j].length, after);
              }
              counts.add("macro use rewritten");
            }
          }
        }
      }
    }

    // --- declaration-level rewrites ---

