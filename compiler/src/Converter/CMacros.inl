// CMacros.inl: the C libraries' macros collected.
// Part of convert.cpp, included where it stood (a file of its own since 2026-11-05, moved without a change).

namespace
{
  // emptyDefines: the library's defines that are empty (defines.txt's `TRIO_PUBLIC=`), which hold
  // over its own definitions of them, so that another name for one is empty too (triostr.h's
  // `#define TRIO_PUBLIC_STRING TRIO_PUBLIC`).
  CMacros collectCMacros(const std::vector<const std::string*>& configuredTexts, const std::set<std::string>& emptyDefines)
  {
    std::map<std::string, std::set<std::string>> bodies; // name -> "(param) body" or " body", every definition
    for (const std::string* text : configuredTexts)
    {
      const std::string& t = *text;
      for (size_t at = 0; at < t.size();)
      {
        size_t end = t.find('\n', at);
        if (end == std::string::npos)
          end = t.size();
        std::string line = t.substr(at, end - at);
        // continuation lines
        while (!line.empty() && (line.back() == '\\' || (line.size() > 1 && line.back() == '\r' && line[line.size() - 2] == '\\')) && end < t.size())
        {
          line.erase(line.find_last_of('\\'));
          size_t next = t.find('\n', end + 1);
          if (next == std::string::npos)
            next = t.size();
          line += " " + t.substr(end + 1, next - end - 1);
          end = next;
        }
        at = end + 1;
        size_t p = line.find_first_not_of(" \t");
        if (p == std::string::npos || line[p] != '#')
          continue;
        p = line.find_first_not_of(" \t", p + 1);
        if (p == std::string::npos || line.compare(p, 6, "define") != 0)
          continue;
        p = line.find_first_not_of(" \t", p + 6);
        if (p == std::string::npos)
          continue;
        size_t n = p;
        while (n < line.size() && isIdentChar(line[n]))
          ++n;
        if (n == p)
          continue;
        std::string name = line.substr(p, n - p), params;
        if (n < line.size() && line[n] == '(')
        {
          size_t close = line.find(')', n);
          if (close == std::string::npos)
            continue;
          params = line.substr(n + 1, close - n - 1);
          n = close + 1;
        }
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
        size_t pa = params.find_first_not_of(" \t"), pb = params.find_last_not_of(" \t");
        params = pa == std::string::npos ? std::string() : params.substr(pa, pb - pa + 1);
        bodies[name].insert(n > p && line[p + name.size()] == '(' ? "(" + params + ") " + body : " " + body);
      }
    }
    for (const std::string& name : emptyDefines)
      bodies[name] = {" "};
    static const std::set<std::string> keywords = {"static", "const", "extern", "inline", "volatile", "register"};
    CMacros out;
    for (const auto& [name, defined] : bodies)
    {
      if (defined.size() != 1)
        continue;
      const std::string& d = *defined.begin();
      if (d[0] == ' ')
      {
        std::string body = d.substr(1);
        if (body.empty())
          out.empty.insert(name);
        else if (keywords.count(body))
          out.keyword[name] = body;
      }
      else
      {
        size_t close = d.find(") ");
        std::string param = d.substr(1, close - 1), body = d.substr(close + 2);
        bool identifier = !param.empty();
        for (char c : param)
          identifier = identifier && isIdentChar(c);
        if (identifier && body == param)
          out.identity.insert(name);
        else if (body.empty())
          out.emptyCall.insert(name);
        else
        {
          // (list, a1, ..., an) -> (a1,...,an)
          std::vector<std::string> params;
          std::string compact;
          for (size_t a = 0; a <= param.size();)
          {
            size_t comma = param.find(',', a);
            if (comma == std::string::npos)
              comma = param.size();
            std::string one = param.substr(a, comma - a);
            size_t x = one.find_first_not_of(" \t"), y = one.find_last_not_of(" \t");
            params.push_back(x == std::string::npos ? std::string() : one.substr(x, y - x + 1));
            a = comma + 1;
          }
          for (char ch : body)
            if (ch != ' ' && ch != '\t')
              compact += ch;
          std::string expected = "(";
          for (size_t k = 1; k < params.size(); ++k)
            expected += (k > 1 ? "," : "") + params[k];
          expected += ")";
          if (params.size() >= 2 && compact == expected)
            out.dropFirst.insert(name);
        }
      }
    }
    // body: one parameter, or an identity macro of one
    for (const auto& [name, defined] : bodies)
    {
      if (defined.size() != 1 || (*defined.begin())[0] != '(' || out.identity.count(name))
        continue;
      const std::string& d = *defined.begin();
      size_t close = d.find(") ");
      std::string params = d.substr(1, close - 1), body = d.substr(close + 2);
      std::string compact;
      for (char ch : body)
        if (ch != ' ' && ch != '\t')
          compact += ch;
      size_t open = compact.find('(');
      if (open != std::string::npos && compact.back() == ')' && out.identity.count(compact.substr(0, open)))
        compact = compact.substr(open + 1, compact.size() - open - 2);
      size_t index = 0;
      for (size_t a = 0; a <= params.size(); ++index)
      {
        size_t comma = params.find(',', a);
        if (comma == std::string::npos)
          comma = params.size();
        std::string one = params.substr(a, comma - a);
        size_t x = one.find_first_not_of(" \t"), y = one.find_last_not_of(" \t");
        if (x != std::string::npos && one.substr(x, y - x + 1) == compact)
        {
          out.keepArgument[name] = index;
          break;
        }
        a = comma + 1;
      }
    }
    // storage words, through other macros and past attributes (one word: a keyword macro; none:
    // an empty one)
    {
      std::function<bool(const std::string&, std::vector<std::string>&, int)> words = [&](const std::string& body, std::vector<std::string>& out,
                                                                                            int depth) -> bool
      {
        static const std::map<std::string, std::string> storageWords = {
          {"static", "static"}, {"extern", "extern"}, {"inline", "inline"}, {"__inline", "inline"}, {"__inline__", "inline"},
          {"__forceinline", "inline"}, {"const", "const"}, {"volatile", "volatile"}, {"register", "register"}};
        for (size_t k = 0; k < body.size();)
        {
          if (body[k] == ' ' || body[k] == '\t')
          {
            ++k;
            continue;
          }
          if (!isIdentChar(body[k]))
            return false;
          size_t e = k;
          while (e < body.size() && isIdentChar(body[e]))
            ++e;
          std::string word = body.substr(k, e - k);
          k = e;
          if (word == "__attribute__" || word == "__declspec")
          {
            // its parenthesized argument goes with it
            while (k < body.size() && body[k] == ' ')
              ++k;
            if (k >= body.size() || body[k] != '(')
              return false;
            int depth2 = 0;
            for (; k < body.size(); ++k)
            {
              if (body[k] == '(')
                ++depth2;
              else if (body[k] == ')' && --depth2 == 0)
              {
                ++k;
                break;
              }
            }
            continue;
          }
          if (auto w = storageWords.find(word); w != storageWords.end())
          {
            if (std::find(out.begin(), out.end(), w->second) == out.end())
              out.push_back(w->second);
            continue;
          }
          auto other = bodies.find(word);
          if (depth > 8 || other == bodies.end() || other->second.size() != 1 || (*other->second.begin())[0] != ' ' ||
              !words(other->second.begin()->substr(1), out, depth + 1))
            return false;
        }
        return true;
      };
      for (const auto& [name, defined] : bodies)
      {
        if (defined.size() != 1 || (*defined.begin())[0] != ' ')
          continue;
        std::vector<std::string> found;
        if (!words(defined.begin()->substr(1), found, 0))
          continue;
        if (found.empty())
          out.empty.insert(name);
        else if (found.size() == 1)
          out.keyword[name] = found[0];
        else
        {
          // static first, as C writes it
          std::stable_sort(found.begin(), found.end(), [](const std::string& a, const std::string& b) { return a == "static" && b != "static"; });
          std::string joined;
          for (const std::string& w : found)
            joined += (joined.empty() ? "" : " ") + w;
          out.storage[name] = joined;
        }
      }
    }
    // `*(...)`
    for (const auto& [name, defined] : bodies)
      if (defined.size() == 1 && (*defined.begin())[0] == ' ')
      {
        std::string body = defined.begin()->substr(1);
        if (body.size() > 3 && body[0] == '*' && body.find_first_not_of(" ", 1) != std::string::npos && body[body.find_first_not_of(" ", 1)] == '(' &&
            body.back() == ')')
          out.postfix[name] = body;
      }
    // a `*` among empty macros and calling conventions
    for (const auto& [name, defined] : bodies)
    {
      if (defined.size() != 1 || (*defined.begin())[0] != ' ')
        continue;
      std::string body = defined.begin()->substr(1), left;
      static const std::set<std::string> conventions = {"__stdcall", "__cdecl", "__fastcall", "__vectorcall", "WINAPI", "APIENTRY", "CALLBACK"};
      for (size_t k = 0; k < body.size();)
      {
        if (isIdentChar(body[k]))
        {
          size_t e = k;
          while (e < body.size() && isIdentChar(body[e]))
            ++e;
          std::string word = body.substr(k, e - k);
          if (!out.empty.count(word) && !conventions.count(word))
            left += word;
          k = e;
        }
        else if (body[k] == ' ' || body[k] == '\t')
          ++k;
        else
          left += body[k++];
      }
      if (left == "*")
        out.pointer.insert(name);
    }
    // an object-like name for one of them (`#define TRIO_VARGS2 TRIO_ARGS2`)
    for (const auto& [name, defined] : bodies)
      if (defined.size() == 1 && (*defined.begin())[0] == ' ' && out.dropFirst.count(defined.begin()->substr(1)))
        out.dropFirst.insert(name);
    return out;
  }
}

