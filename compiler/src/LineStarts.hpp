#pragma once
// Where each line of a text starts, for the line and column of an offset (a binary search): the one
// copy of what codegen's diagnostics, the resolver's locations and kpp --lsp each worked out on their
// own (design.md 24). Lines and columns counted from 1 (line, column), or the line's index from 0
// (index, kpp --lsp's positions); a column in bytes.
#include <algorithm>
#include <cstddef>
#include <string_view>
#include <vector>

class LineStarts
{
public:
  explicit LineStarts(std::string_view text)
  {
    this->starts.push_back(0);
    for (size_t i = 0; i < text.size(); ++i)
      if (text[i] == '\n')
        this->starts.push_back(i + 1);
  }

  // The index, from 0, of the line offset is on.
  size_t index(size_t offset) const { return size_t(std::upper_bound(this->starts.begin(), this->starts.end(), offset) - this->starts.begin()) - 1; }
  size_t line(size_t offset) const { return this->index(offset) + 1; }
  size_t column(size_t offset) const { return offset - this->starts[this->index(offset)] + 1; }
  // Where the line of index i starts.
  size_t operator[](size_t i) const { return this->starts[i]; }
  size_t size() const { return this->starts.size(); }

private:
  std::vector<size_t> starts;
};
