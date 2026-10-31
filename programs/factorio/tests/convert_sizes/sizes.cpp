// The converter's test of sizes whose width C++ code observes: a std container's size(), length()
// and count() are size_t (8 bytes) in C++ and int (4) in Kpp's library, so where one is the right
// operand of a serialiser's <<, which writes sizeof(T) bytes, the converter writes size_t(...). Each
// line prints the bytes one such << wrote: what this file prints compiled as C++
// (convert_sizes.expected) is what its conversion must print. The game's own containers keep their
// own size types (a uint32_t size() stays 4 bytes); FlatSet's size_type is its std::vector's.
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

struct Output
{
  int bytes = 0;
  template<class T>
  Output& operator<<(const T&)
  {
    this->bytes += int(sizeof(T));
    return *this;
  }
};

// The game's CompactDeque, IntrusiveSet, BlueprintEntities: a size of their own, 4 bytes in both.
class SmallQueue
{
public:
  uint32_t size() const { return 3; }
};

// The game's FlatSet: its size_type is the std::vector's, size_t in C++.
template<class T>
class FlatSet
{
public:
  using container_type = std::vector<T>;
  using size_type = typename container_type::size_type;
  size_type size() const { return this->data.size(); }
  container_type data;
};

class Holder
{
public:
  using Mapping = std::map<std::string, std::set<int>>;
  void save(Output& output) const;
  void saveEach(Output& output) const;

  Mapping mapping;
  std::vector<std::vector<int>> rows;
  std::set<int> sets[2];
  SmallQueue queue;
  FlatSet<int> flat;
  std::string text;
};

void report(const char* what, Output& output)
{
  printf("%s %d\n", what, output.bytes);
  output.bytes = 0;
}

void Holder::save(Output& output) const
{
  output << this->mapping.size();
  report("member map size", output);
  for (const auto& item : this->mapping)
  {
    output << item.second.size();
    report("map element's set size", output);
    output << item.first.length();
    report("map element's string length", output);
  }
  for (auto& row : this->rows)
  {
    output << row.size();
    report("vector element's size", output);
  }
  output << this->rows[0].size();
  report("indexed vector's size", output);
  output << this->sets[1].size();
  report("array element's size", output);
  output << this->mapping.count("a");
  report("map count", output);
  output << this->text.length();
  report("string length", output);
  output << this->queue.size();
  report("own uint32_t size", output);
  output << this->flat.size();
  report("FlatSet size", output);
}

void Holder::saveEach(Output& output) const
{
  const auto& rowsAgain = this->rows;
  output << rowsAgain.size();
  report("auto reference's size", output);
  auto copy = this->mapping;
  output << copy.size();
  report("auto copy's size", output);
  // A printf-style argument is read by its format: `%zu` takes 8 bytes. A value above 32 bits
  // shows whether the whole size_t was passed.
  char text[64];
  snprintf(text, sizeof(text), "%zu %zu %s", this->rows.size(), (size_t(1) << 40) + this->text.length(), "end");
  printf("printf-style sizes %s\n", text);
}

int main()
{
  Holder holder;
  holder.mapping["a"] = {1, 2};
  holder.rows.push_back({1, 2, 3});
  holder.text = "text";
  Output output;
  holder.save(output);
  holder.saveEach(output);
  return 0;
}
