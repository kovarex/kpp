// NativeTests.inl: the game's UnitTest++ tests written as kpp's own (kppc --convert-native-tests,
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
    compare("CHECK_ARRAY_EQUAL", "actual", "==", "expected", false);
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
      continue; // (left as it is: kppc reports the unknown name)
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
