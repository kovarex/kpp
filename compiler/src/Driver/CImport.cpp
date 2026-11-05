// The driver: the C import: `extern "C" import` headers preprocessed by clang, their macros'
// constants, the library's functions left out; its declarations in CImport.hpp (Driver.hpp). A
// translation unit of its own since 2026-11-05 (main.cpp split, stage 2), moved without a change.
#include "Driver.hpp"

namespace driver
{
std::string readWholeFile(const std::filesystem::path& path)
{
  std::string text;
  readFile(path, text);
  return text;
}

std::vector<std::string> makeDependencies(const std::string& text)
{
  std::vector<std::string> words;
  std::string word;
  auto end = [&]()
  {
    if (!word.empty())
      words.push_back(std::move(word));
    word.clear();
  };
  for (size_t i = 0; i < text.size(); ++i)
  {
    char c = text[i];
    if (c == '\\' && i + 1 < text.size() && (text[i + 1] == '\n' || text[i + 1] == '\r'))
    {
      while (i + 1 < text.size() && (text[i + 1] == '\n' || text[i + 1] == '\r'))
        ++i;
      end();
    }
    else if (c == '\\' && i + 1 < text.size() && (text[i + 1] == ' ' || text[i + 1] == '#'))
      word += text[++i];
    else if (c == '$' && i + 1 < text.size() && text[i + 1] == '$')
      word += text[++i];
    else if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
      end();
    else
      word += c;
  }
  end();
  size_t first = 0;
  while (first < words.size() && words[first].back() != ':')
    ++first;
  return first < words.size() ? std::vector<std::string>(words.begin() + first + 1, words.end()) : std::vector<std::string>{};
}

std::string fileStamp(const std::string& path)
{
  std::error_code ec;
  uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec)
    return {};
  auto time = std::filesystem::last_write_time(path, ec);
  if (ec)
    return {};
  return std::to_string(size) + " " + std::to_string(time.time_since_epoch().count()) + " " + path;
}

bool writeHeaderStamps(const std::string& makeDeps, const std::filesystem::path& source, const std::filesystem::path& out)
{
  std::vector<std::string> headers = makeDependencies(makeDeps);
  if (headers.empty())
    return false;
  std::string stamps;
  for (const std::string& h : headers)
  {
    if (std::filesystem::path(h).filename() == source.filename())
      continue; // the file of #includes clang was given, not a header (and deleted once done)
    std::string stamp = fileStamp(h);
    if (stamp.empty())
      return false;
    stamps += stamp + "\n";
  }
  FILE* f = fopen(out.string().c_str(), "wb");
  if (!f)
    return false;
  fwrite(stamps.data(), 1, stamps.size(), f);
  fclose(f);
  return true;
}

bool headersUnchanged(const std::filesystem::path& stampsPath)
{
  std::string stamps = readWholeFile(stampsPath);
  if (stamps.empty())
    return false;
  std::vector<std::string> lines;
  for (size_t pos = 0; pos < stamps.size();)
  {
    size_t end = stamps.find('\n', pos);
    if (end == std::string::npos)
      end = stamps.size();
    if (end > pos)
      lines.push_back(stamps.substr(pos, end - pos));
    pos = end + 1;
  }
  // (on every thread: the game's import reads hundreds of headers, a compile server's every build)
  std::atomic<bool> same{true};
  parallelFor(lines.size(), std::max(1u, std::thread::hardware_concurrency()), [&](size_t i)
  {
    const std::string& line = lines[i];
    size_t a = line.find(' '), b = a == std::string::npos ? a : line.find(' ', a + 1);
    if (same && (b == std::string::npos || fileStamp(line.substr(b + 1)) != line))
      same = false;
  });
  return same;
}

std::map<std::string, std::string> expandMacroCalls(const std::string& defines, const std::string& wrapper, const std::filesystem::path& source,
                                                    const std::string& clang, const std::string& includeArgs)
{
  std::vector<std::string> names;
  for (size_t p = 0; p < defines.size();)
  {
    size_t end = defines.find('\n', p);
    if (end == std::string::npos)
      end = defines.size();
    std::string line = defines.substr(p, end - p);
    p = end + 1;
    if (line.rfind("#define ", 0) != 0 || line.compare(8, 2, "__") == 0)
      continue;
    size_t nameEnd = line.find_first_of(" (", 8);
    if (nameEnd == std::string::npos || line[nameEnd] == '(')
      continue;
    bool call = false;
    for (size_t i = nameEnd + 1; i < line.size() && !call; ++i)
      if (line[i] == '(')
      {
        size_t j = i;
        while (j > nameEnd + 1 && line[j - 1] == ' ')
          --j;
        call = j > nameEnd + 1 && (isalnum((unsigned char)line[j - 1]) || line[j - 1] == '_');
      }
    if (call)
      names.push_back(line.substr(8, nameEnd - 8));
  }
  std::map<std::string, std::string> out;
  if (names.empty())
    return out;
  std::string text = wrapper + "\n";
  for (size_t i = 0; i < names.size(); ++i)
    text += "__kpp_x_" + std::to_string(i) + "__ " + names[i] + "\n";
  std::filesystem::path expanded = source;
  expanded.replace_extension(".pp");
  FILE* w = fopen(source.string().c_str(), "wb");
  if (!w)
    return out;
  fwrite(text.data(), 1, text.size(), w);
  fclose(w);
  if (system(("\"\"" + clang + "\" -x c -w" + includeArgs + " \"" + source.string() + "\" -E -P -o \"" + expanded.string() + "\"\"").c_str()) != 0)
    return out; // the bodies stay as they are (not constants)
  std::string result = readWholeFile(expanded);
  for (size_t p = 0; p < result.size();) // the markers' lines (one with an empty expansion is the marker alone)
  {
    size_t end = result.find('\n', p);
    if (end == std::string::npos)
      end = result.size();
    std::string line = result.substr(p, end - p);
    p = end + 1;
    if (line.rfind("__kpp_x_", 0) != 0)
      continue;
    size_t close = line.find("__ ", 8);
    if (close == std::string::npos)
      continue;
    size_t index = size_t(strtoull(line.c_str() + 8, nullptr, 10));
    std::string body = line.substr(close + 3);
    while (!body.empty() && (body.back() == ' ' || body.back() == '\r'))
      body.pop_back();
    if (index < names.size() && !body.empty())
      out[names[index]] = body;
  }
  return out;
}

std::set<std::string> typedefNames(const std::string& text)
{
  std::set<std::string> names;
  auto identChar = [](char c) { return isalnum((unsigned char)c) || c == '_'; };
  bool inTypedef = false;
  int depth = 0;
  char before = 0; // the last character before the identifier, spaces skipped
  for (size_t i = 0; i < text.size();)
  {
    char c = text[i];
    if (isdigit((unsigned char)c))
    {
      // a number, `0x10` and `1.5e3f` whole (its letters are no identifier)
      while (i < text.size() && (identChar(text[i]) || text[i] == '.'))
        ++i;
      before = '0';
      continue;
    }
    if (identChar(c))
    {
      size_t j = i;
      while (j < text.size() && identChar(text[j]))
        ++j;
      std::string_view word(text.data() + i, j - i);
      size_t next = j;
      while (next < text.size() && (text[next] == ' ' || text[next] == '\t' || text[next] == '\r' || text[next] == '\n'))
        ++next;
      char after = next < text.size() ? text[next] : 0;
      if (!inTypedef && word == "typedef" && (i == 0 || !identChar(text[i - 1])))
      {
        inTypedef = true;
        depth = 0;
      }
      else if (inTypedef && ((depth == 0 && (after == ',' || after == ';' || after == '[')) || (depth == 1 && before == '*' && after == ')')))
        names.insert(std::string(word));
      before = word.back();
      i = j;
      continue;
    }
    if (inTypedef)
    {
      if (c == '(' || c == '{' || c == '[')
        ++depth;
      else if ((c == ')' || c == '}' || c == ']') && depth > 0)
        --depth;
      else if (c == ';' && depth == 0)
        inTypedef = false;
    }
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
      before = c;
    ++i;
  }
  return names;
}

std::string macroConstants(const std::string& defines, const std::map<std::string, std::string>& expansions, const std::set<std::string>& typedefs)
{
  std::string out = "\n// Object-like macros, as constants.\n";
  // Names of object-like macros with a body: a body may refer to them (INFINITY uses _HUGE_ENUF).
  std::set<std::string> candidates;
  {
    size_t p = 0;
    while (p < defines.size())
    {
      size_t end = defines.find('\n', p);
      if (end == std::string::npos)
        end = defines.size();
      std::string line = defines.substr(p, end - p);
      p = end + 1;
      if (line.rfind("#define ", 0) != 0)
        continue;
      size_t nameEnd = line.find_first_of(" (", 8);
      if (nameEnd == std::string::npos || line[nameEnd] == '(' || nameEnd + 1 >= line.size())
        continue;
      candidates.insert(line.substr(8, nameEnd - 8));
    }
  }
  static const std::set<std::string> typeWords = {"float", "double", "int", "long", "short", "char", "unsigned", "signed", "const",
                                                  "sizeof", "size_t", "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t",
                                                  "uint32_t", "uint64_t", "intptr_t", "uintptr_t", "wchar_t", "bool", "true", "false",
                                                  "DWORD", "WORD", "BYTE", "HANDLE", "LONG", "ULONG", "UINT", "INT", "LONG_PTR", "ULONG_PTR",
                                                  "LPVOID", "SIZE_T", "DWORD_PTR", "HRESULT", "BOOL", "LONGLONG", "ULONGLONG", "UINT_PTR", "INT_PTR",
                                                  "clock_t", "time_t", "ptrdiff_t", "errno_t", "wint_t", "intmax_t", "uintmax_t", "UINT8", "UINT16",
                                                  "UINT32", "UINT64", "INT8", "INT16", "INT32", "INT64", "ULONG64", "LONG64", "DWORD64", "USHORT",
                                                  "SHORT", "UCHAR", "CHAR", "WCHAR"};
  std::map<std::string, std::string> integerTypes; // constants with a plain integer body, by name
  std::map<std::string, std::string> floatTypes;   // constants with a plain floating literal body (__FLT_MAX__), by name
  std::vector<std::pair<std::string, std::string>> laterAliases; // (name, the macro it names) not typed when read
  // `3.40282347e+38F`, `2.2204460492503131e-16`: a decimal floating literal.
  auto floatLiteral = [](const std::string& body)
  {
    return !body.empty() && isdigit((unsigned char)body[0]) && body.find_first_not_of("0123456789.eE+-fFlL") == std::string::npos &&
           body.find_first_of(".eE") != std::string::npos;
  };
  // A floating literal among a body's numbers (`* 0.5`, `2e3 + 1`): a '.' or a decimal exponent in a
  // number, never an E of a name. `(WSABASEERR + 13)`, MinGW-w64's WSAEACCES, is an int (taken for a
  // double, `DERIVED / 2` divided as one).
  auto floatingNumber = [](const std::string& body)
  {
    for (size_t i = 0; i < body.size(); ++i)
    {
      bool inName = i > 0 && (isalnum((unsigned char)body[i - 1]) || body[i - 1] == '_');
      bool starts = isdigit((unsigned char)body[i]) || (body[i] == '.' && i + 1 < body.size() && isdigit((unsigned char)body[i + 1]));
      if (inName || !starts)
        continue;
      bool hex = body[i] == '0' && i + 1 < body.size() && (body[i + 1] == 'x' || body[i + 1] == 'X');
      size_t j = i;
      bool floating = false;
      while (j < body.size() && (isalnum((unsigned char)body[j]) || body[j] == '.' || body[j] == '_' ||
                                 (!hex && (body[j] == '+' || body[j] == '-') && (body[j - 1] == 'e' || body[j - 1] == 'E'))))
      {
        floating = floating || (!hex && (body[j] == '.' || body[j] == 'e' || body[j] == 'E'));
        ++j;
      }
      if (floating)
        return true;
      i = j - 1;
    }
    return false;
  };
  for (size_t p = 0; p < defines.size();)
  {
    size_t end = defines.find('\n', p);
    if (end == std::string::npos)
      end = defines.size();
    std::string line = defines.substr(p, end - p);
    p = end + 1;
    if (line.rfind("#define ", 0) != 0)
      continue;
    size_t nameEnd = line.find_first_of(" (", 8);
    if (nameEnd == std::string::npos || line[nameEnd] == '(' || nameEnd + 1 >= line.size())
      continue;
    std::string body = line.substr(nameEnd + 1);
    while (!body.empty() && (body.back() == ' ' || body.back() == '\r'))
      body.pop_back();
    if (auto e = expansions.find(line.substr(8, nameEnd - 8)); e != expansions.end())
      body = e->second;
    if (floatLiteral(body))
    {
      floatTypes[line.substr(8, nameEnd - 8)] = body.back() == 'f' || body.back() == 'F' ? "float" : "double";
      continue;
    }
    // A negative one in parentheses is plain too (corecrt_math.h's `_FINITE (-1)`, which FP_NORMAL names).
    if (body.size() > 3 && body.front() == '(' && body.back() == ')' && body[1] == '-')
      body = body.substr(2, body.size() - 3);
    if (body.empty() || !isdigit((unsigned char)body[0]) || body.find_first_not_of("0123456789xXabcdefABCDEFuUlLi") != std::string::npos ||
        (body.find('i') != std::string::npos && body.find("i64") == std::string::npos && body.find("i32") == std::string::npos) ||
        (body.find_first_of("xX") == std::string::npos && body.find_first_of("abcdefABCDEF") != std::string::npos))
      continue;
    bool wide = body.find_first_of("lL") != std::string::npos || body.find("i64") != std::string::npos;
    bool isUnsigned = body.find_first_of("uU") != std::string::npos;
    integerTypes[line.substr(8, nameEnd - 8)] = wide ? (isUnsigned ? "unsigned long long" : "long long") : (isUnsigned ? "unsigned int" : "int");
  }
  size_t pos = 0;
  while (pos < defines.size())
  {
    size_t end = defines.find('\n', pos);
    if (end == std::string::npos)
      end = defines.size();
    std::string line = defines.substr(pos, end - pos);
    pos = end + 1;
    if (line.rfind("#define ", 0) != 0)
      continue;
    size_t nameStart = 8;
    size_t nameEnd = line.find_first_of(" (", nameStart);
    if (nameEnd == std::string::npos || line[nameEnd] == '(')
      continue; // function-like or empty
    std::string name = line.substr(nameStart, nameEnd - nameStart);
    std::string body = line.substr(nameEnd + 1);
    while (!body.empty() && (body.back() == ' ' || body.back() == '\r'))
      body.pop_back();
    if (auto e = expansions.find(name); e != expansions.end())
      body = e->second;
    // __FILE__-style internals stay out, unless a plain number (__LONG_LONG_MAX__, __FLT_MAX__)
    bool internal = name.rfind("__", 0) == 0;
    if (name.empty() || body.empty() || (internal && body.find_first_not_of("0123456789xXabcdefABCDEFuUlL") != std::string::npos && !floatLiteral(body)))
      continue;
    bool hasNumber = false, ok = true, call = false, namesKnown = true;
    int quotes = 0;
    for (size_t i = 0; i < body.size(); ++i)
    {
      char c = body[i];
      bool prevIdent = i > 0 && (isalnum((unsigned char)body[i - 1]) || body[i - 1] == '_');
      if (c == '"')
        ++quotes;
      else if (isdigit((unsigned char)c))
        hasNumber = hasNumber || !prevIdent; // a digit inside a name (_SAL1_1_) is not a number
      else if (!(isalnum((unsigned char)c) || strchr("_ .+-*/()<>|&~!^,'\\xX", c)))
        ok = false;
      if ((isalpha((unsigned char)c) || c == '_') && !prevIdent && quotes % 2 == 0)
      {
        // An identifier: it must be a type word, a typedef of the headers or another constant macro for
        // the body to be a constant.
        size_t j = i;
        while (j < body.size() && (isalnum((unsigned char)body[j]) || body[j] == '_'))
          ++j;
        std::string word = body.substr(i, j - i);
        if (!typeWords.count(word) && !typedefs.count(word) && !candidates.count(word))
          namesKnown = false;
      }
      if (c == '(')
      {
        size_t j = i;
        while (j > 0 && body[j - 1] == ' ')
          --j;
        if (j > 0 && (isalnum((unsigned char)body[j - 1]) || body[j - 1] == '_'))
          call = true; // `name(`: a function-like macro or a call, not a constant; `)(` in casts is fine
      }
    }
    bool aliasOfInteger = integerTypes.count(body) != 0; // DBL_MANT_DIG = __DBL_MANT_DIG__
    bool aliasOfFloat = floatTypes.count(body) != 0;     // FLT_MAX = __FLT_MAX__
    if (!ok || call || (quotes != 0 && quotes != 2) || !namesKnown)
      continue;
    if (!hasNumber && quotes == 0 && body.find('(') == std::string::npos && !aliasOfInteger && !aliasOfFloat)
    {
      // An alias of a macro not typed yet, typed after the others (-dM's macros come in name order:
      // curl's `#define EWOULDBLOCK WSAEWOULDBLOCK` before MinGW-w64's `#define WSAEWOULDBLOCK
      // (WSABASEERR + 35)`).
      if (candidates.count(body))
        laterAliases.emplace_back(name, body);
      continue;
    }
    // A declared type where the body shows one: a cast, a string, a float or an integer literal.
    std::string type = "auto";
    if (body.rfind("((", 0) == 0 && body.find(')') != std::string::npos)
    {
      std::string cast = body.substr(2, body.find(')') - 2);
      bool word = !cast.empty();
      for (char ch : cast)
        if (!(isalnum((unsigned char)ch) || ch == '_' || ch == ' '))
          word = false;
      if (word && cast.find(' ') == std::string::npos)
        type = cast;
    }
    else if (quotes == 2)
      type = "char*";
    else if (hasNumber && body.find_first_of("\"") == std::string::npos && floatingNumber(body))
      type = body.back() == 'f' || body.back() == 'F' ? "float" : "double";
    else if (hasNumber && body.find_first_not_of("0123456789xXabcdefABCDEFuUlLi()- +") == std::string::npos &&
             (body.find('i') == std::string::npos || body.find("i64") != std::string::npos || body.find("i32") != std::string::npos))
    {
      bool wide = body.find_first_of("lL") != std::string::npos || body.find("i64") != std::string::npos;
      bool isUnsigned = body.find_first_of("uU") != std::string::npos;
      type = wide ? (isUnsigned ? "unsigned long long" : "long long") : (isUnsigned ? "unsigned int" : "int");
    }
    if (type == "auto" && aliasOfInteger)
      type = integerTypes[body];
    if (type == "auto" && aliasOfFloat)
      type = floatTypes[body];
    if (type == "auto" && hasNumber && quotes == 0)
    {
      // Names of other integer constants with numbers and operators (LLONG_MIN = (-__LONG_LONG_MAX__-1LL)):
      // an integer of the widest kind seen.
      bool integer = true, wide = false, isUnsigned = false;
      for (size_t i = 0; i < body.size() && integer;)
      {
        char c = body[i];
        if (isalpha((unsigned char)c) || c == '_')
        {
          size_t j = i;
          while (j < body.size() && (isalnum((unsigned char)body[j]) || body[j] == '_'))
            ++j;
          std::string word = body.substr(i, j - i);
          auto known = integerTypes.find(word);
          bool suffix = i > 0 && isdigit((unsigned char)body[i - 1]) && word.find_first_not_of("uUlLi0123456789") == std::string::npos;
          if (known != integerTypes.end())
          {
            wide = wide || known->second.find("long long") != std::string::npos;
            isUnsigned = isUnsigned || known->second.find("unsigned") != std::string::npos;
          }
          else if (suffix)
          {
            wide = wide || word.find_first_of("lL") != std::string::npos || word.find("i64") != std::string::npos;
            isUnsigned = isUnsigned || word.find_first_of("uU") != std::string::npos;
          }
          else
            integer = false;
          i = j;
          continue;
        }
        if (c == '.' || c == '"' || c == '\'')
          integer = false;
        ++i;
      }
      if (integer)
        type = wide ? (isUnsigned ? "unsigned long long" : "long long") : (isUnsigned ? "unsigned int" : "int");
    }
    if (type.find("int") != std::string::npos || type.find("long") != std::string::npos)
      integerTypes[name] = type;
    out += "const " + type + " " + name + " = " + body + ";\n";
  }
  // The aliases of macros typed after them, each once the macro it names is (an alias of such an
  // alias in a later round), declared after it.
  for (bool typed = true; typed;)
  {
    typed = false;
    for (auto it = laterAliases.begin(); it != laterAliases.end();)
    {
      auto integer = integerTypes.find(it->second);
      auto floating = floatTypes.find(it->second);
      if (integer == integerTypes.end() && floating == floatTypes.end())
      {
        ++it;
        continue;
      }
      std::string type = integer != integerTypes.end() ? integer->second : floating->second;
      out += "const " + type + " " + it->first + " = " + it->second + ";\n";
      (integer != integerTypes.end() ? integerTypes : floatTypes)[it->first] = type;
      it = laterAliases.erase(it);
      typed = true;
    }
  }
  return out;
}

LexResult lexInChunks(std::string_view text, SymbolTable& symbols, unsigned threads)
{
  const size_t piece = 256 * 1024;
  std::vector<size_t> starts{0};
  for (size_t at = piece; at < text.size();)
  {
    size_t nl = text.find('\n', at);
    while (nl != std::string_view::npos && nl > 0 && (text[nl - 1] == '\\' || (text[nl - 1] == '\r' && nl > 1 && text[nl - 2] == '\\')))
      nl = text.find('\n', nl + 1);
    if (nl == std::string_view::npos || nl + 1 >= text.size())
      break;
    starts.push_back(nl + 1);
    at = nl + 1 + piece;
  }
  if (starts.size() == 1)
    return lex(text, symbols);
  std::vector<LexResult> parts(starts.size());
  parallelFor(starts.size(), threads, [&](size_t k)
  {
    size_t from = starts[k], to = k + 1 < starts.size() ? starts[k + 1] : text.size();
    std::string copy(text.substr(from, to - from));
    parts[k] = lex(copy, symbols);
    for (Token& t : parts[k].tokens)
      t.offset += uint32_t(from);
  });
  LexResult whole;
  size_t count = 0;
  for (const LexResult& r : parts)
    count += r.tokens.size();
  whole.tokens.reserve(count);
  for (size_t k = 0; k < parts.size(); ++k)
  {
    std::vector<Token>& tokens = parts[k].tokens;
    if (k + 1 < parts.size() && !tokens.empty() && tokens.back().getKind() == TokenKind::End)
      tokens.pop_back(); // only the last piece ends the text
    whole.tokens.insert(whole.tokens.end(), tokens.begin(), tokens.end());
    whole.lineCount += parts[k].lineCount;
    whole.errorCount += parts[k].errorCount;
  }
  return whole;
}

std::string vaListForSysV(const std::string& text)
{
  const std::string from = "__builtin_va_list", to = "__kpp_va_list";
  std::string out = "typedef struct __kpp_va_list_tag { unsigned int gp_offset; unsigned int fp_offset; void* overflow_arg_area; "
                    "void* reg_save_area; } __kpp_va_list[1];\n";
  out.reserve(text.size() + out.size());
  auto word = [&](size_t i) { return i < text.size() && (isalnum((unsigned char)text[i]) || text[i] == '_'); };
  for (size_t i = 0; i < text.size();)
  {
    if (text.compare(i, from.size(), from) == 0 && !(i > 0 && word(i - 1)) && !word(i + from.size()))
    {
      out += to;
      i += from.size();
    }
    else
      out += text[i++];
  }
  return out;
}

std::string clangToolchain(const std::string& clang, const std::string& includeArgs)
{
  std::string out;
  FILE* pipe = _popen(("\"\"" + clang + "\" -x c -E -v" + includeArgs + " NUL -o NUL 2>&1\"").c_str(), "r");
  if (!pipe)
    return out;
  bool list = false;
  std::string line;
  char chunk[4096];
  while (fgets(chunk, sizeof chunk, pipe))
  {
    line += chunk;
    if (line.back() != '\n' && !feof(pipe))
      continue; // the cc1 line is longer than a chunk
    std::string s = std::move(line);
    line.clear();
    if (s.rfind("clang version", 0) == 0)
      out += s;
    else if (s.find("-cc1") != std::string::npos)
    {
      // The command line's other options name the working directory: only these two.
      for (const char* option : {"-triple ", "-fms-compatibility-version="})
        if (size_t at = s.find(option); at != std::string::npos)
          out += s.substr(at, s.find(' ', at + strlen(option)) - at) + "\n";
    }
    else if (s.rfind("#include <...>", 0) == 0)
      list = true;
    else if (s.rfind("End of search list", 0) == 0)
      list = false;
    else if (list)
      out += s;
  }
  _pclose(pipe);
  return out;
}

bool sameWrittenType(const Type* a, const Type* b)
{
  if (!a || !b || a->kind != b->kind)
    return false;
  switch (a->kind)
  {
    case NodeKind::BuiltinType:
    {
      auto* x = static_cast<const BuiltinType*>(a);
      auto* y = static_cast<const BuiltinType*>(b);
      return x->keyword == y->keyword && x->longCount == y->longCount && x->isUnsigned == y->isUnsigned && x->isSigned == y->isSigned;
    }
    case NodeKind::ConstType:
    {
      auto* x = static_cast<const ConstType*>(a);
      auto* y = static_cast<const ConstType*>(b);
      return x->isConst == y->isConst && x->isVolatile == y->isVolatile && sameWrittenType(x->inner, y->inner);
    }
    case NodeKind::PointerType:
      return sameWrittenType(static_cast<const PointerType*>(a)->pointee, static_cast<const PointerType*>(b)->pointee);
    case NodeKind::NamedType:
    {
      const QualifiedName& x = static_cast<const NamedType*>(a)->name;
      const QualifiedName& y = static_cast<const NamedType*>(b)->name;
      if (x.global != y.global || x.typeQualifier || y.typeQualifier || x.parts.size() != y.parts.size())
        return false;
      for (size_t i = 0; i < x.parts.size(); ++i)
        if (x.parts[i].symbol != y.parts[i].symbol || x.parts[i].special || y.parts[i].special || x.parts[i].hasTemplateArgs ||
            y.parts[i].hasTemplateArgs)
          return false;
      return true;
    }
    default:
      return false;
  }
}

size_t leaveOutLibraryFunctions(std::vector<std::unique_ptr<ParsedFile>>& parsed, ParsedFile& imported, const SymbolTable& symbols,
                                std::vector<std::string>& names)
{
  std::unordered_map<uint32_t, std::vector<const FunctionDecl*>> library;
  for (const std::unique_ptr<ParsedFile>& p : parsed)
    if (p->isLibrary && !p->isImported)
      for (Decl* d : p->decls)
        if (d->kind == NodeKind::FunctionDecl && d->templateParams.empty() && !d->when && !d->filePrivate && !(d->flags & DF_ExternC))
        {
          auto* f = static_cast<const FunctionDecl*>(d);
          if (f->name.parts.size() == 1 && !f->name.global && !f->name.last().special && !f->name.last().hasTemplateArgs)
            library[f->name.last().symbol].push_back(f);
        }
  auto declaredByLibrary = [&](const FunctionDecl* c)
  {
    if (c->name.parts.size() != 1 || c->name.last().special)
      return false;
    auto it = library.find(c->name.last().symbol);
    if (it == library.end())
      return false;
    for (const FunctionDecl* f : it->second)
    {
      if (f->params.size() != c->params.size() || (f->flags & DF_Variadic) != (c->flags & DF_Variadic))
        continue;
      bool same = true;
      for (size_t i = 0; i < f->params.size() && same; ++i)
        same = f->params[i]->kind == NodeKind::ParamDecl && c->params[i]->kind == NodeKind::ParamDecl &&
               sameWrittenType(static_cast<const ParamDecl*>(f->params[i])->type, static_cast<const ParamDecl*>(c->params[i])->type);
      if (same)
        return true;
    }
    return false;
  };
  size_t kept = 0, leftOut = 0;
  std::set<std::string> leftOutNames;
  for (Decl* d : imported.decls)
    if (d->kind == NodeKind::FunctionDecl && declaredByLibrary(static_cast<const FunctionDecl*>(d)))
    {
      ++leftOut;
      leftOutNames.insert(std::string(symbols.name(static_cast<const FunctionDecl*>(d)->name.last().symbol)));
    }
    else
      imported.decls.data[kept++] = d;
  imported.decls.count = uint32_t(kept);
  names.assign(leftOutNames.begin(), leftOutNames.end());
  return leftOut;
}

void importCHeaders(std::vector<SourceFile>& files, std::vector<LexResult>& lexed, std::vector<std::unique_ptr<ParsedFile>>& parsed,
                    SymbolTable& symbols, const Options& options)
{
  std::vector<std::string> headers;
  for (size_t i = 0; i < parsed.size(); ++i)
    for (Decl* d : parsed[i]->decls)
      if (d->kind == NodeKind::ImportDecl)
      {
        const Token& t = lexed[i].tokens[static_cast<ImportDecl*>(d)->pathToken];
        std::string path = files[i].text.substr(t.offset + 1, t.length - 2);
        if (std::find(headers.begin(), headers.end(), path) == headers.end())
          headers.push_back(path);
      }
  if (headers.empty())
    return;
  auto importStart = std::chrono::steady_clock::now();
  std::string wrapper = "#define WIN32_LEAN_AND_MEAN\n#define NOMINMAX\n#define _USE_MATH_DEFINES\n";
  for (const std::string& h : headers)
    // (intrin.h is MSVC's: another target's clang has the same SSE intrinsics in immintrin.h)
    wrapper += !options.target.windows() && h == "intrin.h" ? std::string("#include <immintrin.h>\n#include <x86intrin.h>\n") : "#include <" + h + ">\n";
  // MSVC's intrin.h declares these; clang's has them as builtins it declares itself. The code
  // generator emits each one's instruction (msvcBuiltin).
  if (std::find(headers.begin(), headers.end(), "intrin.h") != headers.end())
    wrapper += "unsigned short __popcnt16(unsigned short);\nunsigned int __popcnt(unsigned int);\nunsigned __int64 __popcnt64(unsigned __int64);\n"
               "unsigned char _rotl8(unsigned char, unsigned char);\nunsigned char _rotr8(unsigned char, unsigned char);\n"
               "unsigned short _rotl16(unsigned short, unsigned char);\nunsigned short _rotr16(unsigned short, unsigned char);\n";
  // malloc.h's alloca is `#define alloca _alloca` (only where __STDC__ is not defined, as it is
  // here), which the import does not carry: declared as the function, the code generator's
  // stack allocation like _alloca.
  if (std::find(headers.begin(), headers.end(), "malloc.h") != headers.end())
    wrapper += "#undef alloca\nvoid* alloca(size_t);\n";
  // _alloca: the UCRT's malloc.h declares it, MinGW-w64's makes it a macro of __builtin_alloca, also
  // where another header includes malloc.h (MinGW-w64's stdlib.h does): declared as the function
  // wherever a header made it a macro.
  wrapper += "#ifdef _alloca\n#undef _alloca\nvoid* _alloca(size_t);\n#endif\n";
  // HRESULT_FROM_WIN32: the Windows SDK's winerror.h has it an inline function, MinGW-w64's a
  // function-like macro, which the import does not carry (its inline function only outside
  // __CRT__NO_INLINE, which clang's unoptimized -E sets with every other inline function of its
  // CRT, math.h's on __builtin_fabsf among them): declared as the function wherever a header made it
  // a macro (its definition for the link is the program's: kpp-native's inline_c.c for the game).
  wrapper += "#ifdef HRESULT_FROM_WIN32\n#undef HRESULT_FROM_WIN32\nlong HRESULT_FROM_WIN32(long);\n#endif\n";
  // GCC's builtins that C code calls (the game's Linux branches, glibc's inline functions): clang
  // declares them itself, so no header does. Declared here for the program, each unless a header
  // made it a macro (a library's `#define __builtin_expect(x, y) (x)` for MSVC); the code generator
  // emits each one's instructions (gccBuiltin).
  for (const char* declaration :
       {"int __builtin_clz(unsigned int)", "int __builtin_clzl(unsigned long)", "int __builtin_clzll(unsigned long long)",
        "int __builtin_ctz(unsigned int)", "int __builtin_ctzl(unsigned long)", "int __builtin_ctzll(unsigned long long)",
        "int __builtin_popcount(unsigned int)", "int __builtin_popcountl(unsigned long)", "int __builtin_popcountll(unsigned long long)",
        "unsigned short __builtin_bswap16(unsigned short)", "unsigned int __builtin_bswap32(unsigned int)",
        "unsigned long long __builtin_bswap64(unsigned long long)", "long __builtin_expect(long, long)",
        "void __builtin_prefetch(const void*, ...)", "void* __builtin_alloca(__SIZE_TYPE__)"})
  {
    std::string text = declaration;
    size_t start = text.find("__builtin_"), end = text.find('(', start);
    wrapper += "#ifndef " + text.substr(start, end - start) + "\n" + text + ";\n#endif\n";
  }
  // The include directories as absolute paths, in the key and on clang's command line: a relative
  // `--cinclude native\include` names another directory in every worktree.
  std::error_code ec;
  // Another target's headers: clang preprocesses them for it, from its sysroot (in the key too).
  std::string includeArgs = targetFlags(options);
  if (!options.target.msvc() && !options.sysroot.empty())
    includeArgs += " \"--sysroot=" + options.sysroot + "\"";
  for (const std::string& dir : options.cIncludeDirs)
  {
    std::filesystem::path absolute = std::filesystem::absolute(dir, ec);
    includeArgs += " -I\"" + (ec ? dir : absolute.lexically_normal().string()) + "\"";
  }
  std::string clang = clangPath();
  std::string toolchain = clangToolchain(clang, includeArgs);
  if (toolchain.find("clang version") == std::string::npos)
  {
    fprintf(stderr, "importing C headers failed: %s -v gave no version\n", clang.c_str());
    codegenExit = 1;
    return;
  }
  uint64_t hash = 1469598103934665603ull;
  // the version of what the cache holds (macroConstants' rules, vaListForSysV), then what it is made from
  for (unsigned char c : "kpp import 9\n" + wrapper + includeArgs + "\n" + toolchain)
    hash = (hash ^ c) * 1099511628211ull;
  std::filesystem::path cacheDir = std::filesystem::temp_directory_path(ec) / "kpp-imports";
  std::filesystem::create_directories(cacheDir, ec);
  char hex[32];
  snprintf(hex, sizeof hex, "%016llx", (unsigned long long)hash);
  std::filesystem::path cached = cacheDir / (std::string(hex) + ".i");
  std::filesystem::path dependencies = cacheDir / (std::string(hex) + ".deps");
  std::string text;
  // A cached import is used only while every header clang read for it is as it was then (the
  // key names the headers imported, not what they hold: an edited kpp_directx.h is seen).
  bool cacheHit = std::filesystem::exists(cached, ec) && headersUnchanged(dependencies);
  lastImportStamps = dependencies.string();
  if (!cacheHit)
  {
    // Several compilers may build one import at once (the spec suite's): each works in files of its
    // own and renames the results into place whole.
    std::string own = std::string(hex) + "." + std::to_string(_getpid());
    std::filesystem::path wrapperPath = cacheDir / (own + ".c");
    std::filesystem::path preprocessed = cacheDir / (own + ".pp");
    std::filesystem::path defines = cacheDir / (own + ".dm");
    std::filesystem::path makeDeps = cacheDir / (own + ".d");
    FILE* w = fopen(wrapperPath.string().c_str(), "wb");
    if (!w)
    {
      fprintf(stderr, "cannot write %s\n", wrapperPath.string().c_str());
      return;
    }
    fwrite(wrapper.data(), 1, wrapper.size(), w);
    fclose(w);
    std::string base = "\"\"" + clang + "\" -x c -w" + includeArgs + " \"" + wrapperPath.string() + "\"";
    int rc = system((base + " -E -P -MD -MF \"" + makeDeps.string() + "\" -o \"" + preprocessed.string() + "\"\"").c_str());
    int rc2 = system((base + " -E -dM -o \"" + defines.string() + "\"\"").c_str());
    if (rc != 0 || rc2 != 0)
    {
      fprintf(stderr, "importing C headers failed (clang exit %d/%d)\n", rc, rc2);
      codegenExit = 1;
      return;
    }
    std::string definesText = readWholeFile(defines);
    std::map<std::string, std::string> expansions = expandMacroCalls(definesText, wrapper, cacheDir / (own + ".x.c"), clang, includeArgs);
    text = readWholeFile(preprocessed);
    text += macroConstants(definesText, expansions, typedefNames(text));
    if (!options.target.windows())
      text = vaListForSysV(text);
    // The import, then its stamps (a reader that finds the import with older stamps checks those:
    // they still match only when the headers, and so the import, are the same).
    std::filesystem::path ownImport = cacheDir / (own + ".i"), ownStamps = cacheDir / (own + ".deps");
    if (FILE* out = fopen(ownImport.string().c_str(), "wb"))
    {
      fwrite(text.data(), 1, text.size(), out);
      fclose(out);
      std::filesystem::rename(ownImport, cached, ec);
      if (!ec && writeHeaderStamps(readWholeFile(makeDeps), wrapperPath, ownStamps))
        std::filesystem::rename(ownStamps, dependencies, ec);
    }
    for (const std::filesystem::path& p : {wrapperPath, preprocessed, defines, makeDeps, ownImport, ownStamps, cacheDir / (own + ".x.c"), cacheDir / (own + ".x.pp")})
      std::filesystem::remove(p, ec);
  }
  else
    text = readWholeFile(cached);
  // The imported declarations become one more file, parsed in C mode.
  SourceFile source;
  source.path = "<imported C headers>";
  source.text = std::move(text);
  source.text.reserve(source.text.size() + 1);
  files.push_back(std::move(source));
  lexed.push_back(lexInChunks(files.back().text, symbols, options.threads));
  parsed.push_back(std::make_unique<ParsedFile>());
  parsed.back()->path = files.back().path.string();
  parsed.back()->index = uint32_t(parsed.size() - 1);
  parsed.back()->isImported = true;
  parseFile(lexed.back().tokens, files.back().text, symbols, *parsed.back(), Dialect::C, nullptr);
  for (size_t i = 0; i < parsed.size(); ++i)
  {
    // The vectors may have grown: re-point the parsed files at their tokens and text.
    parsed[i]->tokens = &lexed[i].tokens;
    parsed[i]->text = files[i].text;
  }
  std::vector<std::string> libraryNames;
  size_t leftOut = leaveOutLibraryFunctions(parsed, *parsed.back(), symbols, libraryNames);
  std::string leftOutText;
  if (leftOut)
  {
    leftOutText = ", " + std::to_string(leftOut) + " left out as the standard library's (";
    for (size_t i = 0; i < libraryNames.size(); ++i)
      leftOutText += (i ? ", " : "") + libraryNames[i];
    leftOutText += ")";
  }
  char line[1024];
  snprintf(line, sizeof line, "imports: %zu header(s) -> %zu declarations, %zu parse errors (ignored)%s, %.3f s%s\n", headers.size(),
           size_t(parsed.back()->declCount), parsed.back()->diagnostics.size(), leftOutText.c_str(),
           seconds(std::chrono::steady_clock::now() - importStart), cacheHit ? " (cached)" : "");
  lastImportsLine = line; // a compile server's incremental build prints it again (Build::importsLine)
  report("%s", line);
}
}
