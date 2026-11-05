#pragma once
// Part of codegen: CodegenShared.hpp includes it inside namespace codegen.

// IR text several emitters append to (the shared declarations, a module's), one whole
// declaration or definition per append, in whatever order the emitters reach them. Under a lock of
// its own (only appended to while functions are emitted, read once they all are).
struct SharedText
{
  SharedText& operator+=(const std::string& s)
  {
    std::lock_guard lock(*mutex);
    entries.push_back(s);
    return *this;
  }
  // The text the module is assembled from, the same in every run: the entries sorted (LLVM takes
  // declarations and definitions in any order), each freed once copied.
  void finish()
  {
    std::sort(entries.begin(), entries.end());
    size_t total = text.size();
    for (const std::string& e : entries)
      total += e.size();
    text.reserve(total);
    for (std::string& e : entries)
    {
      text += e;
      std::string().swap(e);
    }
    entries.clear();
  }
  // The text from `needle` to the end of the entry holding it (before finish()), or empty.
  std::string fromFirst(const std::string& needle) const
  {
    for (const std::string& e : entries)
      if (size_t at = e.find(needle); at != std::string::npos)
        return e.substr(at);
    return {};
  }

  std::unique_ptr<std::mutex> mutex = std::make_unique<std::mutex>();
  std::vector<std::string> entries;
  std::string text; // the entries, once finish() put them in order
};
