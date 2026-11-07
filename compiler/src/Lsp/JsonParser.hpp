#pragma once
// Part of kpp --lsp: lsp.cpp includes it where it was.

struct JsonParser
{
  void space()
  {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
      ++i;
  }
  bool literal(std::string_view word)
  {
    if (s.substr(i, word.size()) != word)
      return ok = false;
    i += word.size();
    return true;
  }
  static void appendUtf8(std::string& out, uint32_t c)
  {
    if (c < 0x80)
      out += char(c);
    else if (c < 0x800)
      out += char(0xC0 | (c >> 6)), out += char(0x80 | (c & 0x3F));
    else if (c < 0x10000)
      out += char(0xE0 | (c >> 12)), out += char(0x80 | ((c >> 6) & 0x3F)), out += char(0x80 | (c & 0x3F));
    else
      out += char(0xF0 | (c >> 18)), out += char(0x80 | ((c >> 12) & 0x3F)), out += char(0x80 | ((c >> 6) & 0x3F)), out += char(0x80 | (c & 0x3F));
  }
  uint32_t hex4()
  {
    if (i + 4 > s.size())
      return ok = false, 0;
    uint32_t v = 0;
    for (int k = 0; k < 4; ++k)
    {
      char c = s[i++];
      v = v * 16 + uint32_t(c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : (ok = false, 0));
    }
    return v;
  }
  std::string string()
  {
    std::string out;
    ++i; // "
    while (ok && i < s.size() && s[i] != '"')
    {
      char c = s[i++];
      if (c != '\\')
      {
        out += c;
        continue;
      }
      if (i >= s.size())
        break;
      char e = s[i++];
      switch (e)
      {
      case 'n': out += '\n'; break;
      case 't': out += '\t'; break;
      case 'r': out += '\r'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'u':
      {
        uint32_t c1 = hex4();
        if (c1 >= 0xD800 && c1 < 0xDC00 && s.substr(i, 2) == "\\u")
        {
          i += 2;
          uint32_t c2 = hex4();
          c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
        }
        appendUtf8(out, c1);
        break;
      }
      default: out += e;
      }
    }
    if (i >= s.size())
      ok = false;
    ++i; // "
    return out;
  }
  Json value(int depth = 0)
  {
    space();
    if (i >= s.size() || depth > 200)
      return ok = false, Json();
    char c = s[i];
    if (c == '{')
    {
      ++i;
      Json o = Json::object();
      space();
      if (i < s.size() && s[i] == '}')
        return ++i, o;
      while (ok)
      {
        space();
        if (i >= s.size() || s[i] != '"')
          return ok = false, o;
        std::string key = string();
        space();
        if (i >= s.size() || s[i] != ':')
          return ok = false, o;
        ++i;
        o.members.emplace_back(std::move(key), value(depth + 1));
        space();
        if (i < s.size() && s[i] == ',')
          ++i;
        else if (i < s.size() && s[i] == '}')
          return ++i, o;
        else
          ok = false;
      }
      return o;
    }
    if (c == '[')
    {
      ++i;
      Json a = Json::array();
      space();
      if (i < s.size() && s[i] == ']')
        return ++i, a;
      while (ok)
      {
        a.items.push_back(value(depth + 1));
        space();
        if (i < s.size() && s[i] == ',')
          ++i;
        else if (i < s.size() && s[i] == ']')
          return ++i, a;
        else
          ok = false;
      }
      return a;
    }
    if (c == '"')
      return Json(string());
    if (c == 't')
      return literal("true"), Json(true);
    if (c == 'f')
      return literal("false"), Json(false);
    if (c == 'n')
      return literal("null"), Json();
    size_t start = i;
    while (i < s.size() && (isdigit((unsigned char)s[i]) || strchr("+-.eE", s[i])))
      ++i;
    if (start == i)
      return ok = false, Json();
    return Json(strtod(std::string(s.substr(start, i - start)).c_str(), nullptr));
  }

  std::string_view s;
  size_t i = 0;
  bool ok = true;
};
