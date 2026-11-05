#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>

// Interns identifier spellings to dense ids, safe to call from many threads at once.
class SymbolTable
{
public:
  SymbolTable();
  ~SymbolTable();
  uint32_t intern(std::string_view name);
  std::string_view name(uint32_t id) const;
  size_t size() const;

private:
  struct Impl;
  Impl* impl;
};
