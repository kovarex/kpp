// NativeTests.inl: the game's UnitTest++ tests written as kpp's own (kpp --convert-native-tests,
// design.md 8.7, docs/test-suites.md). UnitTest++'s macros get bodies of kpp's syntax
// (nativeTestMacroBodies), which the converter's macro expansion puts the arguments in as for any
// macro, so that a check inside another macro (the game's own CHECK_BUILDABILITY) is written alike.

// The marks the bodies put around a check's operands, resolved once the arguments are in:
// \x06 operand \x07 is the operand, in parentheses when an operator in it binds less tightly than a
// comparison (`require((flags & 3) == 1)`, `require(a == b)`); \x0e value \x0f is CHECK_FAILED's
// stream (`"no " << name`), its `<<` pieces as fail's arguments (`fail("no ", name)`).
static const char nativeOperandOpen = '\x06', nativeOperandClose = '\x07', nativeStreamOpen = '\x0e', nativeStreamClose = '\x0f';

static std::string withTestMarksResolved(std::string text)
{
  const char marks[] = {nativeOperandOpen, nativeStreamOpen, 0};
  if (text.find_first_of(marks) == std::string::npos)
    return text;
  // The characters at the inner text's top level, outside brackets and literals.
  auto scan = [](const std::string& inner, auto&& onTop)
  {
    int depth = 0;
    for (size_t k = 0; k < inner.size(); ++k)
    {
      char c = inner[k];
      if (c == '"' || c == '\'')
      {
        for (++k; k < inner.size() && inner[k] != c; ++k)
          k += inner[k] == '\\';
        continue;
      }
      if (c == '(' || c == '[' || c == '{')
        ++depth;
      else if (c == ')' || c == ']' || c == '}')
        --depth;
      else if (depth == 0)
        onTop(k);
    }
  };
  auto trimmed = [](const std::string& s)
  {
    size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e + 1 - b);
  };
  for (size_t open = text.find_first_of(marks); open != std::string::npos; open = text.find_first_of(marks, open))
  {
    bool operand = text[open] == nativeOperandOpen;
    // (the innermost: an operand's own marks are resolved by its own expansion, but a mark may
    // still hold one put in by an argument passed on)
    size_t close = text.find(operand ? nativeOperandClose : nativeStreamClose, open);
    if (close == std::string::npos)
      break;
    size_t nested = text.find_first_of(marks, open + 1);
    if (nested != std::string::npos && nested < close)
    {
      open = nested;
      continue;
    }
    std::string inner = trimmed(text.substr(open + 1, close - open - 1));
    std::string out;
    if (operand)
    {
      bool loose = false;
      scan(inner, [&](size_t k)
      {
        auto at = [&](const char* s) { return inner.compare(k, strlen(s), s) == 0; };
        auto spaced = [&] { return k > 0 && inner[k - 1] == ' ' && k + 1 < inner.size() && inner[k + 1] == ' '; };
        if (at("?") || at("==") || at("!=") || at("<=") || at(">=") || at("&&") || at("||") ||
            ((at("=") || at("<") || at(">") || at("&") || at("|") || at("^")) && spaced()))
          loose = true;
      });
      out = loose ? "(" + inner + ")" : inner;
      // (a char pointer that is visibly one: CHECK_EQUAL compared its text, and `==` of two char
      // pointers in a check is an error; the others the compiler points out)
      for (const char* call : {".c_str()", "->c_str()", ".what()", "->what()", ".get<const char*>()", "->get<const char*>()"})
        if (!loose && inner.size() > strlen(call) && inner.compare(inner.size() - strlen(call), strlen(call), call) == 0)
          out = "StringView(" + inner + ")";
    }
    else
    {
      size_t from = 0;
      std::vector<std::string> pieces;
      scan(inner, [&](size_t k)
      {
        if (k >= from && inner.compare(k, 2, "<<") == 0)
        {
          pieces.push_back(inner.substr(from, k - from));
          from = k + 2;
        }
      });
      pieces.push_back(inner.substr(from));
      for (const std::string& piece : pieces)
        out += (out.empty() ? "" : ", ") + trimmed(piece);
    }
    text.replace(open, close + 1 - open, out);
    open = 0; // (from the start again: an outer mark may now be innermost)
  }
  return text;
}

// UnitTest++'s macros (libraries/UnitTest++/TestMacros.h, CheckMacros.h, tests/GeneratedTests.hpp)
// as kpp's syntax, by their parameters' names. Every check is `require`, as the game's fork ended a
// test at its first failed check. A location parameter (*_SOURCELOCATION) goes: a check in a test
// function names its callers itself. DEPENDENCY and SUITE_MODPACKS are marks the suite's header takes
// (withSuiteHeadersResolved), calls of names no program has, so that the expansion puts the
// arguments in.
static std::string nativeOperand(const char* name) { return std::string(1, nativeOperandOpen) + name + nativeOperandClose; }

static const std::map<std::string, std::string>& nativeTestMacroBodies()
{
  static const std::map<std::string, std::string> bodies = []
  {
    std::map<std::string, std::string> b;
    b["SUITE"] = "suite Name";
    b["DEPENDENCY"] = "kpp_depends_mark(Name)";
    b["SUITE_MODPACKS"] = "kpp_tags_mark(__VA_ARGS__)";
    b["TEST"] = "test Name";
    b["TEST_FIXTURE"] = "test Name : Fixture";
    b["TEST_PARAMETRISED_FIXTURE"] = "test Name : Fixture(Parameter)";
    b["TEST_GENERATOR"] = "generate Name";
    std::string stream = std::string(1, nativeStreamOpen) + "value" + nativeStreamClose;
    b["CHECK_FAILED"] = "fail(" + stream + ")";
    b["CHECK_FAILED_SOURCELOCATION"] = "fail(" + stream + ")";
    b["CHECK_FAILED_SIGNED"] = "fail(" + stream + ", id)";
    b["CHECK"] = "require(value)";
    b["CHECK_SOURCELOCATION"] = "require(value)";
    b["CHECK_SIGNED"] = "require(value, id)";
    b["CHECK_SIGNED_SOURCELOCATION"] = "require(value, id)";
    auto compare = [&](const char* name, const char* left, const char* op, const char* right, bool id)
    { b[name] = "require(" + nativeOperand(left) + " " + op + " " + nativeOperand(right) + (id ? ", id)" : ")"); };
    compare("CHECK_EQUAL", "actual", "==", "expected", false);
    compare("CHECK_EQUAL_SOURCELOCATION", "actual", "==", "expected", false);
    compare("CHECK_EQUAL_SIGNED", "actual", "==", "expected", true);
    compare("CHECK_EQUAL_SIGNED_SOURCELOCATION", "actual", "==", "expected", true);
    compare("CHECK_UNEQUAL", "actual", "!=", "unexpected", false);
    compare("CHECK_UNEQUAL_SIGNED", "actual", "!=", "unexpected", true);
    compare("CHECK_BIGGER", "value", ">", "biggerThan", false);
    compare("CHECK_BIGGER_SIGNED", "value", ">", "biggerThan", true);
    compare("CHECK_BIGGEREQUAL", "value", ">=", "biggerThan", false);
    compare("CHECK_BIGGEREQUAL_SIGNED", "value", ">=", "biggerThan", true);
    compare("CHECK_SMALLER", "value", "<", "smallerThan", false);
    compare("CHECK_SMALLER_SIGNED", "value", "<", "smallerThan", true);
    compare("CHECK_SMALLEREQUAL", "value", "<=", "smallerThan", false);
    compare("CHECK_SMALLEREQUAL_SIGNED", "value", "<=", "smallerThan", true);
    b["CHECK_ARRAY_EQUAL"] = "require(UnitTest::isArrayEqual(actual, expected))";
    b["CHECK_NULL"] = "require(" + nativeOperand("actual") + " == nullptr)";
    b["CHECK_NULL_SIGNED"] = "require(" + nativeOperand("actual") + " == nullptr, id)";
    b["CHECK_NOT_NULL"] = "require(" + nativeOperand("actual") + " != nullptr)";
    b["CHECK_NOT_NULL_SIGNED"] = "require(" + nativeOperand("actual") + " != nullptr, id)";
    b["CHECK_BETWEEN"] = "require(UnitTest::isBetween(value, lowerBound, upperBound))";
    b["CHECK_BETWEENEQUAL"] = "require(UnitTest::isBetweenOrEqual(value, lowerBound, upperBound))";
    b["CHECK_CLOSE"] = "require(UnitTest::isClose(actual, expected, tolerance))";
    b["CHECK_NOT_CLOSE"] = "require(!UnitTest::isClose(actual, expected, tolerance))";
    b["CHECK_CLOSE_SIGNED"] = "require(UnitTest::isClose(actual, expected, tolerance), id)";
    b["CHECK_CLOSE_PERCENTAGE"] = "require(UnitTest::isClosePercentage(actual, expected, tolerancePercentage))";
    b["CHECK_CLOSE_PERCENTAGE_SIGNED"] = "require(UnitTest::isClosePercentage(actual, expected, tolerancePercentage), id)";
    b["CHECK_ARRAY_CLOSE"] = "require(UnitTest::isArrayClose(actual, expected, tolerance))";
    b["CHECK_CONTAINS"] = "require(UnitTest::contains(actual, substring))";
    b["CHECK_CONTAINS_SOURCELOCATION"] = "require(UnitTest::contains(actual, substring))";
    b["CHECK_CONTAINS_SIGNED"] = "require(UnitTest::contains(actual, substring), id)";
    b["CHECK_DOES_NOT_CONTAIN"] = "require(!UnitTest::contains(actual, substring))";
    b["CHECK_DOES_NOT_CONTAIN_SIGNED"] = "require(!UnitTest::contains(actual, substring), id)";
    b["CHECK_STARTS_WITH"] = "require(UnitTest::startsWith(actual, prefix))";
    b["CHECK_STARTS_WITH_SOURCELOCATION"] = "require(UnitTest::startsWith(actual, prefix))";
    b["CHECK_DOES_NOT_START_WITH"] = "require(!UnitTest::startsWith(actual, prefix))";
    b["CHECK_DOES_NOT_START_WITH_SOURCELOCATION"] = "require(!UnitTest::startsWith(actual, prefix))";
    b["CHECK_THROW"] = "requireThrows<ExpectedExceptionType>(expression)";
    b["CHECK_THROW_EQUAL"] = "require(StringView(requireThrows<ExpectedExceptionType>(expression).what()) == expectedMessage)";
    b["CHECK_THROW_CONTAINS"] = "require(UnitTest::contains(requireThrows<ExpectedExceptionType>(expression).what(), expectedMessage))";
    return b;
  }();
  return bodies;
}

// The suites' headers, once a file is written: each DEPENDENCY's and SUITE_MODPACKS's mark (with the
// `;` after it) goes, and the suite it stands in names them, `suite Name depends A, B tags vanilla`
// (the modpacks' strings as names).
static std::string withSuiteHeadersResolved(std::string text, Counts& counts)
{
  if (text.find("kpp_depends_mark(") == std::string::npos && text.find("kpp_tags_mark(") == std::string::npos)
    return text;
  struct Header
  {
    std::vector<std::string> depends, tags;
  };
  std::map<size_t, Header> headers; // by the offset right after the suite's name
  for (size_t at = text.find("kpp_"); at != std::string::npos; at = text.find("kpp_", at))
  {
    bool depends = text.compare(at, 17, "kpp_depends_mark(") == 0, tags = text.compare(at, 14, "kpp_tags_mark(") == 0;
    if (!depends && !tags)
    {
      at += 4;
      continue;
    }
    size_t valueStart = at + (depends ? 17 : 14), end = text.find(')', valueStart);
    if (end == std::string::npos)
      break;
    std::string value = text.substr(valueStart, end - valueStart);
    // (the suite: the last `suite NAME` before the mark)
    size_t suite = text.rfind("suite ", at);
    while (suite != std::string::npos && suite > 0 && (isIdentChar(text[suite - 1])))
      suite = suite == 0 ? std::string::npos : text.rfind("suite ", suite - 1);
    if (suite == std::string::npos)
    {
      at = end;
      continue; // (left as it is: kpp reports the unknown name)
    }
    size_t nameEnd = suite + 6;
    while (nameEnd < text.size() && isIdentChar(text[nameEnd]))
      ++nameEnd;
    Header& h = headers[nameEnd];
    for (size_t from = 0; from <= value.size();)
    {
      size_t comma = value.find(',', from);
      if (comma == std::string::npos)
        comma = value.size();
      std::string item = value.substr(from, comma - from);
      item.erase(std::remove_if(item.begin(), item.end(), [](char c) { return c == '"' || isspace((unsigned char)c); }), item.end());
      if (!item.empty())
        (depends ? h.depends : h.tags).push_back(item);
      from = comma + 1;
    }
    // (the mark, its `;`, and its line when nothing else stands on it)
    size_t removeEnd = end + 1;
    size_t semicolon = text.find_first_not_of(" \t", removeEnd);
    if (semicolon != std::string::npos && text[semicolon] == ';')
      removeEnd = semicolon + 1;
    size_t lineStart = text.rfind('\n', at);
    lineStart = lineStart == std::string::npos ? 0 : lineStart + 1;
    size_t lineEnd = text.find('\n', removeEnd);
    bool alone = text.find_first_not_of(" \t", lineStart) == at &&
                 (lineEnd == std::string::npos ? text.find_first_not_of(" \t\r", removeEnd) == std::string::npos
                                               : text.find_first_not_of(" \t\r", removeEnd) == lineEnd);
    if (alone)
      text.erase(lineStart, (lineEnd == std::string::npos ? text.size() : lineEnd + 1) - lineStart);
    else
      text.erase(at, removeEnd - at);
    counts.add(depends ? "suite dependencies written in the suite's header" : "suite modpacks written as the suite's tags");
    at = alone ? lineStart : at;
  }
  // (from the last header: an insertion moves the offsets after it)
  for (auto it = headers.rbegin(); it != headers.rend(); ++it)
  {
    std::string header;
    if (!it->second.depends.empty())
    {
      header += " depends ";
      for (size_t k = 0; k < it->second.depends.size(); ++k)
        header += (k ? ", " : "") + it->second.depends[k];
    }
    if (!it->second.tags.empty())
    {
      header += " tags ";
      for (size_t k = 0; k < it->second.tags.size(); ++k)
        header += (k ? ", " : "") + it->second.tags[k];
    }
    text.insert(it->first, header);
  }
  return text;
}

// The text with comments and the insides of string and character literals blanked, offsets kept:
// what the passes below read, so that Lua in a raw string or a word in a comment is not code.
static std::string maskedForTestScan(const std::string& text)
{
  std::string masked = text;
  auto blank = [&](size_t from, size_t to)
  {
    for (size_t k = from; k < to && k < masked.size(); ++k)
      if (masked[k] != '\n')
        masked[k] = ' ';
  };
  for (size_t k = 0; k < text.size();)
  {
    char c = text[k];
    if (c == '/' && k + 1 < text.size() && (text[k + 1] == '/' || text[k + 1] == '*'))
    {
      size_t end = text[k + 1] == '/' ? text.find('\n', k) : text.find("*/", k + 2);
      end = end == std::string::npos ? text.size() : end + (text[k + 1] == '*' ? 2 : 0);
      blank(k, end);
      k = end;
    }
    else if (c == '"' && k > 0 && text[k - 1] == 'R')
    {
      size_t open = text.find('(', k);
      std::string closing = ")" + text.substr(k + 1, open - k - 1) + "\"";
      size_t end = text.find(closing, open);
      end = end == std::string::npos ? text.size() : end + closing.size();
      blank(k + 1, end - 1);
      k = end;
    }
    else if (c == '"' || (c == '\'' && (k == 0 || !isIdentChar(text[k - 1]))))
    {
      size_t end = k + 1;
      while (end < text.size() && text[end] != c && text[end] != '\n')
        end += text[end] == '\\' ? 2 : 1;
      blank(k + 1, end);
      k = end + 1;
    }
    else
      ++k;
  }
  return masked;
}

// Whether the word at offset at of the masked text stands alone and is not a member's or a qualified
// name (`x.check`, `x->check`, `N::check`).
static bool isBareWord(const std::string& masked, size_t at, size_t length)
{
  if ((at > 0 && isIdentChar(masked[at - 1])) || (at + length < masked.size() && isIdentChar(masked[at + length])))
    return false;
  size_t before = at;
  while (before > 0 && isspace((unsigned char)masked[before - 1]))
    --before;
  return before == 0 || !(masked[before - 1] == '.' || masked[before - 1] == ':' || (masked[before - 1] == '>' && before > 1 && masked[before - 2] == '-'));
}

// `check`, `require` and `fail` are statements in a test (design.md 8.7), so a test's own local of
// that name cannot be called. The game's tests have only `check`, lambdas: a bare `check(` that
// starts a statement. A file with one has its bare `check`s renamed, member functions (`void
// check(`, called through this-> or an object) left alone.
static std::string withLocalChecksRenamed(std::string text, Counts& counts)
{
  static const std::string word = "check", renamed = "verify";
  std::string masked = maskedForTestScan(text);
  std::vector<size_t> bare;
  bool called = false;
  for (size_t at = masked.find(word); at != std::string::npos; at = masked.find(word, at + word.size()))
  {
    if (!isBareWord(masked, at, word.size()))
      continue;
    size_t before = at;
    while (before > 0 && isspace((unsigned char)masked[before - 1]))
      --before;
    if (before >= 4 && masked.compare(before - 4, 4, "void") == 0)
      continue;
    bare.push_back(at);
    size_t next = masked.find_first_not_of(" \t", at + word.size());
    if (next != std::string::npos && masked[next] == '(' && (before == 0 || strchr(";{})", masked[before - 1])))
      called = true;
  }
  if (!called)
    return text;
  for (auto at = bare.rbegin(); at != bare.rend(); ++at)
    text.replace(*at, word.size(), renamed);
  counts.add("tests' local check functions renamed");
  return text;
}

// A declaration at namespace scope, as withTestDeclarationsMarked reads them.
struct TestScanDeclaration
{
  size_t start = 0;
  bool hasCheck = false, isClass = false, hasBody = false;
  std::vector<std::string> name; // the qualified name's parts
};

static size_t closingBrace(const std::string& masked, size_t open, size_t to)
{
  int depth = 0;
  for (size_t k = open; k < to; ++k)
    if (masked[k] == '{')
      ++depth;
    else if (masked[k] == '}' && --depth == 0)
      return k;
  return to;
}

// The names of a function's head (up to its `(`): `void A::B<T>::f` is {A, B, f}.
static std::vector<std::string> testScanFunctionName(const std::string& head)
{
  // (the first `(` outside template arguments: `Function<bool()> A::f()`)
  size_t paren = std::string::npos;
  int angles = 0;
  for (size_t k = 0; k < head.size() && paren == std::string::npos; ++k)
    if (head[k] == '<')
      ++angles;
    else if (head[k] == '>')
      --angles;
    else if (head[k] == '(' && angles <= 0)
      paren = k;
  if (paren == std::string::npos)
    return {};
  size_t end = paren;
  while (end > 0 && isspace((unsigned char)head[end - 1]))
    --end;
  std::vector<std::string> parts;
  while (end > 0)
  {
    size_t begin = end;
    while (begin > 0 && (isIdentChar(head[begin - 1]) || head[begin - 1] == '~'))
      --begin;
    if (begin == end)
      break;
    parts.insert(parts.begin(), head.substr(begin, end - begin));
    end = begin;
    if (end >= 2 && head[end - 1] == ':' && head[end - 2] == ':')
      end -= 2;
    else
      break;
    if (end > 0 && head[end - 1] == '>')
    {
      int angles = 0;
      do
      {
        --end;
        angles += head[end] == '>' ? 1 : head[end] == '<' ? -1 : 0;
      } while (end > 0 && angles > 0);
    }
  }
  return parts;
}

static bool hasTestCheck(const std::string& masked, size_t from, size_t to)
{
  for (const char* word : {"require(", "fail(", "requireThrows<"})
  {
    size_t length = strlen(word) - 1;
    for (size_t at = masked.find(word, from); at != std::string::npos && at < to; at = masked.find(word, at + 1))
      if (isBareWord(masked, at, length))
        return true;
  }
  return false;
}

// Whether a declaration's head has an initializer: an `=` outside brackets (a parameter's default
// value is inside), and no operator's.
static bool testScanHasInitializer(const std::string& head)
{
  if (head.find("operator") != std::string::npos)
    return false;
  int depth = 0;
  for (size_t k = 0; k < head.size(); ++k)
  {
    char c = head[k];
    if (c == '(' || c == '[' || c == '<')
      ++depth;
    else if (c == ')' || c == ']' || c == '>')
      --depth;
    else if (c == '=' && depth == 0)
      return true;
  }
  return false;
}

// The class a declaration's head names (`template<class T> class X : public Y`), or "".
static std::string testScanClassName(const std::string& head)
{
  if (head.find('(') != std::string::npos || testScanHasInitializer(head))
    return "";
  std::string name;
  for (const char* keyword : {"class", "struct", "union"})
    for (size_t word = head.find(keyword); word != std::string::npos; word = head.find(keyword, word + 1))
    {
      size_t after = word + strlen(keyword);
      if (!isBareWord(head, word, strlen(keyword)) || (word > 0 && head[word - 1] == '<') || after >= head.size() || !isspace((unsigned char)head[after]))
        continue;
      size_t begin = head.find_first_not_of(" \t\r\n", after), end = begin;
      while (end != std::string::npos && end < head.size() && isIdentChar(head[end]))
        ++end;
      if (begin != std::string::npos && end > begin)
        name = head.substr(begin, end - begin);
    }
  return name;
}

// Reads the declarations of the namespace scope [from, to): into namespaces, `when` blocks and
// `extern "C"`, past suites and tests, whose declarations are test ones already.
static void scanTestDeclarations(const std::string& masked, size_t from, size_t to, std::vector<TestScanDeclaration>& out)
{
  for (size_t at = from; at < to;)
  {
    if (isspace((unsigned char)masked[at]) || masked[at] == ';')
    {
      ++at;
      continue;
    }
    size_t firstEnd = at;
    while (firstEnd < to && isIdentChar(masked[firstEnd]))
      ++firstEnd;
    std::string first = masked.substr(at, firstEnd - at);
    TestScanDeclaration declaration;
    declaration.start = at;
    size_t end = to;
    int parens = 0;
    bool scope = false;
    for (size_t k = at; k < to; ++k)
    {
      char c = masked[k];
      if (c == '(' || c == '[')
        ++parens;
      else if (c == ')' || c == ']')
        --parens;
      else if (parens == 0 && c == ';')
      {
        end = k + 1;
        break;
      }
      else if (parens == 0 && c == '{')
      {
        size_t close = closingBrace(masked, k, to);
        if (first == "namespace" || first == "extern" || first == "when" || first == "else")
        {
          scanTestDeclarations(masked, k + 1, close, out);
          scope = true;
          end = close + 1;
          break;
        }
        if (first == "suite" || first == "test" || first == "generate")
        {
          scope = true;
          end = close + 1;
          break;
        }
        if (first == "enum")
        {
          k = close;
          continue;
        }
        std::string head = masked.substr(at, k - at);
        std::string className = testScanClassName(head);
        if (!className.empty())
        {
          declaration.isClass = true;
          declaration.name = {className};
        }
        size_t next = masked.find_first_not_of(" \t\r\n", close + 1);
        if (declaration.isClass || testScanHasInitializer(head) || (next != std::string::npos && next < to && (masked[next] == ',' || masked[next] == '{')))
        {
          // (a class's, an initializer's or a member initializer's braces: the declaration goes on)
          k = close;
          continue;
        }
        declaration.hasBody = true;
        declaration.name = testScanFunctionName(head);
        end = close + 1;
        break;
      }
    }
    if (!scope && first != "using" && first != "friend")
    {
      if (declaration.name.empty())
        declaration.name = testScanFunctionName(masked.substr(at, end - at));
      declaration.hasCheck = hasTestCheck(masked, at, end);
      out.push_back(std::move(declaration));
    }
    at = end;
  }
}

// The file's functions and classes outside suites that use a check are `test` ones (design.md 8.7):
// each such definition, and in the file the declarations of what it defines (`test class` for an
// out-of-class member's class, `test` on a function declared before its definition), as a test
// function's first declaration decides. The tests' .hpp and .cpp are one text here.
static std::string withTestDeclarationsMarked(std::string text, Counts& counts)
{
  std::string masked = maskedForTestScan(text);
  std::vector<TestScanDeclaration> declarations;
  scanTestDeclarations(masked, 0, masked.size(), declarations);
  std::set<std::string> functions, classes;
  for (const TestScanDeclaration& d : declarations)
    if (d.hasCheck && !d.isClass && !d.name.empty())
    {
      functions.insert(d.name.back());
      classes.insert(d.name.begin(), d.name.end() - 1);
    }
  for (const TestScanDeclaration& d : declarations)
    if (d.isClass && d.hasCheck)
      classes.insert(d.name.back());
  std::vector<size_t> marks;
  for (const TestScanDeclaration& d : declarations)
  {
    bool mark = d.hasCheck;
    if (d.isClass)
      mark = mark || classes.count(d.name.back()) > 0;
    else if (!d.hasBody && !d.name.empty())
      mark = functions.count(d.name.back()) > 0;
    else if (d.name.size() > 1)
      mark = mark || std::any_of(d.name.begin(), d.name.end() - 1, [&](const std::string& part) { return classes.count(part) > 0; });
    if (mark)
      marks.push_back(d.start);
  }
  std::sort(marks.begin(), marks.end());
  for (auto at = marks.rbegin(); at != marks.rend(); ++at)
  {
    text.insert(*at, "test ");
    counts.add("test functions and classes marked");
  }
  return text;
}

// Suites and tests keep their names: a game class's rename (Vector is Vector2D, renameGameClassNames)
// reached them as it reaches every argument, but suite names are a context of their own (design.md
// 8.7), and `suite Vector` beside the library's Vector hides nothing.
static std::string withTestNamesRestored(std::string text)
{
  static const std::map<std::string, std::string> original = {
    {"Vector2D", "Vector"}, {"GameMap", "Map"}, {"GameOptional", "Optional"}, {"GamePair", "Pair"}, {"GameRuntimeError", "RuntimeError"}};
  std::string masked = maskedForTestScan(text);
  std::vector<std::pair<size_t, size_t>> names; // (offset, length) of each name to restore
  for (size_t line = 0; line < masked.size();)
  {
    size_t end = masked.find('\n', line);
    end = end == std::string::npos ? masked.size() : end;
    size_t at = masked.find_first_not_of(" \t", line);
    bool suite = at != std::string::npos && at < end && masked.compare(at, 6, "suite ") == 0;
    bool test = at != std::string::npos && at < end && masked.compare(at, 5, "test ") == 0;
    if (suite || test)
    {
      // (the name after the keyword; a suite's dependencies too; a test's only when nothing but its
      // fixture follows, which tells `test Name` from `test Type function()`)
      for (size_t word = at + (suite ? 6 : 5); word < end;)
      {
        size_t wordEnd = word;
        while (wordEnd < end && isIdentChar(masked[wordEnd]))
          ++wordEnd;
        if (wordEnd == word)
        {
          ++word;
          continue;
        }
        std::string name = masked.substr(word, wordEnd - word);
        if (name == "tags")
          break;
        size_t after = masked.find_first_not_of(" \t\r", wordEnd);
        bool alone = after == std::string::npos || after >= end || masked[after] == ':';
        if (original.count(name) && (suite || alone))
          names.emplace_back(word, wordEnd - word);
        if (test)
          break;
        word = wordEnd;
      }
    }
    line = end + 1;
  }
  for (auto it = names.rbegin(); it != names.rend(); ++it)
    text.replace(it->first, it->second, original.at(text.substr(it->first, it->second)));
  return text;
}
