// SourceEdits.inl: overrides/source-edits.txt, hand edits of the C++ the converter reads.
// Part of convert.cpp, included after its anonymous namespace.

// The list's edits: each an `@ FILE: why` line (FILE as Factorio's repository names it: the input
// whose path ends with it), then the old text's lines, each after `- `, and the new text's, each after
// `+ ` (a lone `-` or `+` is an empty line). Every occurrence of the old text in FILE becomes the new
// text, before the configuration and the front end read it, on every platform and configuration
// alike. An edit whose file is not an input of this conversion is skipped (a conversion of a part of
// the game: convert.cmd's CommonUtil, Agui and Util); one whose old text the file does not have is
// an error, so that an edit that went stale with Factorio's source is seen. Used where the game's
// code differs between Windows and Linux by more than the converter can merge, and the edit is what
// kpp decided: kpp's strings are UTF-8 on every platform, wide only at the Windows API's boundaries (kovarex, 2026-11-05;
// design.md 12, "One spelling on every platform").
std::string applyConvertSourceEdits(const std::filesystem::path& overridesDir, const std::vector<std::filesystem::path>& paths,
                                    const std::vector<std::string*>& texts)
{
  std::string list;
  std::filesystem::path listPath = overridesDir / "source-edits.txt";
  std::error_code ec;
  if (overridesDir.empty() || !std::filesystem::exists(listPath, ec) || !readFile(listPath, list))
    return std::string();
  struct SourceEdit
  {
    std::string file, why;
    std::string from, to;
    bool hasFrom = false, hasTo = false;
    int line = 0;
  };
  std::vector<SourceEdit> edits;
  std::string errors;
  auto error = [&](int line, const std::string& what)
  { errors += listPath.generic_string() + ":" + std::to_string(line) + ": " + what + "\n"; };
  int lineNumber = 0;
  for (size_t at = 0; at < list.size();)
  {
    size_t end = list.find('\n', at);
    if (end == std::string::npos)
      end = list.size();
    std::string line = list.substr(at, end - at);
    at = end + 1;
    ++lineNumber;
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty() || line[0] == '#')
      continue;
    if (line[0] == '@')
    {
      size_t colon = line.find(": ");
      if (line.size() < 3 || line[1] != ' ' || colon == std::string::npos)
      {
        error(lineNumber, "`" + line + "` is not `@ FILE: why`");
        continue;
      }
      edits.push_back({line.substr(2, colon - 2), line.substr(colon + 2)});
      edits.back().line = lineNumber;
      continue;
    }
    if ((line[0] != '-' && line[0] != '+') || (line.size() > 1 && line[1] != ' ') || edits.empty())
    {
      error(lineNumber, "`" + line + "` is not `- old line` or `+ new line` after an `@ FILE: why` line");
      continue;
    }
    SourceEdit& e = edits.back();
    std::string text = line.size() > 1 ? line.substr(2) : std::string();
    if (line[0] == '-')
    {
      if (e.hasTo)
      {
        error(lineNumber, "an old line after the new ones (an edit is its `-` lines, then its `+` lines)");
        continue;
      }
      e.from += (e.hasFrom ? "\n" : "") + text;
      e.hasFrom = true;
    }
    else
    {
      e.to += (e.hasTo ? "\n" : "") + text;
      e.hasTo = true;
    }
  }
  for (const SourceEdit& e : edits)
    if (!e.hasFrom)
      error(e.line, "the edit of " + e.file + " has no old text");
  if (!errors.empty())
    return errors;
  auto lower = [](std::string s)
  {
    for (char& c : s)
      c = char(std::tolower((unsigned char)c));
    return s;
  };
  // An edit's file is the input whose path ends with it (the conversions of convert.cmd's kpp\ give
  // no --convert-base).
  std::vector<std::string> inputPaths;
  for (const std::filesystem::path& path : paths)
    inputPaths.push_back(lower("/" + std::filesystem::absolute(path, ec).lexically_normal().generic_string()));
  for (const SourceEdit& e : edits)
  {
    std::string suffix = lower("/" + e.file);
    size_t input = 0;
    while (input < inputPaths.size() && !(inputPaths[input].size() >= suffix.size() &&
                                          inputPaths[input].compare(inputPaths[input].size() - suffix.size(), suffix.size(), suffix) == 0))
      ++input;
    if (input == inputPaths.size())
      continue;
    std::string& text = *texts[input];
    // In the file's own line endings.
    bool crlf = text.find("\r\n") != std::string::npos;
    auto lineEndings = [&](std::string s)
    {
      if (!crlf)
        return s;
      std::string out;
      for (char c : s)
      {
        if (c == '\n')
          out += '\r';
        out += c;
      }
      return out;
    };
    std::string from = lineEndings(e.from), to = lineEndings(e.to);
    size_t count = 0;
    for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
    {
      text.replace(at, from.size(), to);
      ++count;
    }
    if (count == 0)
      error(e.line, e.file + " does not have the edit's old text (" + e.why + "): its source changed, the edit is to be written again");
  }
  return errors;
}
