#pragma once
// Restore<T>: sets a variable back when the scope ends, the one-off guards the compiler wrote out for
// each variable (`struct FileRestore { ParsedFile*& f; ParsedFile* saved; ~FileRestore() { f = saved;
// } } fileRestore{currentFile, currentFile};`) made one (design.md 24).
//
//   Restore restoreFile(currentFile);            // currentFile as it is now, back at the end
//   Restore restoreRequires(inRequires, saved);  // inRequires is `saved` at the end
#include <utility>

template<typename T>
class Restore
{
public:
  explicit Restore(T& slot) : slot(slot), saved(slot) {}
  Restore(T& slot, T saved) : slot(slot), saved(std::move(saved)) {}
  ~Restore() { this->slot = std::move(this->saved); }
  Restore(const Restore&) = delete;
  Restore& operator=(const Restore&) = delete;

private:
  T& slot;
  T saved;
};
