#pragma once
// Part of kppc --lsp: lsp.cpp includes it where it was.

struct Reader
{
  bool get(char& c)
  {
    if (at == size)
    {
      at = size = 0;
      if (!ReadFile(input, buffer, DWORD(sizeof buffer), &size, nullptr) || size == 0)
        return false;
    }
    c = buffer[at++];
    return true;
  }
  // One message's body; false at the end of the input.
  bool message(std::string& body)
  {
    size_t length = SIZE_MAX;
    for (;;)
    {
      std::string line;
      char c;
      while (get(c) && c != '\n')
        line += c;
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      if (line.empty())
      {
        if (length != SIZE_MAX)
          break;
        if (at == size && size == 0)
          return false;
        continue;
      }
      if (_strnicmp(line.c_str(), "Content-Length:", 15) == 0)
        length = size_t(strtoull(line.c_str() + 15, nullptr, 10));
    }
    body.resize(length);
    for (size_t k = 0; k < length; ++k)
      if (!get(body[k]))
        return false;
    return true;
  }

  char buffer[1 << 16];
  DWORD size = 0, at = 0;
};
