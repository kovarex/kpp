// bench.kpp over the C++ standard library, line for line: std::vector, std::string,
// std::unordered_map, std::map, std::sort. (Kpp's FastHashMap rows run std::unordered_map again.) Built with clang -O2 against whichever standard library the
// target uses (MSVC STL on Windows; libstdc++ or libc++ under WSL, see run.cmd).
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

static uint64_t randomState = 88172645463325252ull;
static uint64_t nextRandom()
{
  randomState ^= randomState << 13;
  randomState ^= randomState >> 7;
  randomState ^= randomState << 17;
  return randomState;
}

static int formatKey(char* buffer, const char* prefix, int value)
{
  int length = 0;
  while (prefix[length] != 0)
  {
    buffer[length] = prefix[length];
    ++length;
  }
  char digits[16];
  int count = 0;
  do
  {
    digits[count++] = char('0' + value % 10);
    value /= 10;
  } while (value != 0);
  while (count > 0)
    buffer[length++] = digits[--count];
  buffer[length] = 0;
  return length;
}

static int selected = -1; // when given on the command line, only the benchmark at this position runs
static int position = 0;

template<typename F>
static void run(const char* name, F body)
{
  int index = position++;
  if (selected >= 0 && index != selected)
    return;
  double best = 1e30;
  int64_t checksum = 0;
  for (int round = 0; round < 7; ++round)
  {
    randomState = 88172645463325252ull;
    auto start = std::chrono::steady_clock::now();
    checksum = body();
    double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (elapsed < best)
      best = elapsed;
  }
  printf("%-22s %9.2f ms  %lld\n", name, best, (long long)checksum);
}

static const int bigCount = 10000000;
static const int stringCount = 1000000;

static int64_t vectorPushInt()
{
  std::vector<int> values;
  for (int i = 0; i < bigCount; ++i)
    values.push_back(i);
  return values.size() + values[bigCount / 2];
}

static int64_t vectorPushReserved()
{
  std::vector<int> values;
  values.reserve(bigCount);
  for (int i = 0; i < bigCount; ++i)
    values.push_back(i);
  return values.size() + values[bigCount / 2];
}

static std::vector<int> resized;

static int64_t vectorResize()
{
  std::vector<int> values;
  values.push_back(1);
  values.resize(bigCount);
  int64_t result = values.size() + values[bigCount - 1];
  resized = std::move(values); // escapes, so the optimizer cannot drop the zeroing
  return result;
}

static std::vector<int> sharedInts;

static int64_t vectorIterate()
{
  int64_t sum = 0;
  for (int round = 0; round < 10; ++round)
    for (int value : sharedInts)
      sum += value;
  return sum;
}

static int64_t vectorCopy()
{
  int64_t sum = 0;
  for (int round = 0; round < 10; ++round)
  {
    std::vector<int> copy = sharedInts;
    sum += copy[round];
  }
  return sum;
}

static int64_t vectorInsertFront()
{
  std::vector<int> values;
  for (int i = 0; i < 50000; ++i)
    values.insert(values.begin(), i);
  return values[0] + values[49999];
}

static int64_t vectorPushString()
{
  char buffer[32];
  std::vector<std::string> values;
  for (int i = 0; i < stringCount; ++i)
  {
    int length = formatKey(buffer, "item", i);
    values.push_back(std::string(buffer, length));
  }
  return values.size() + values[stringCount / 2].size();
}

static int64_t stringSmall()
{
  char buffer[32];
  int64_t total = 0;
  for (int i = 0; i < bigCount; ++i)
  {
    int length = formatKey(buffer, "k", i);
    std::string text(buffer, length);
    total += text.size() + text[0];
  }
  return total;
}

static std::vector<std::string> sharedStrings;

static int64_t stringVectorCopy()
{
  int64_t total = 0;
  for (int round = 0; round < 5; ++round)
  {
    std::vector<std::string> copy = sharedStrings;
    total += copy[round].size();
  }
  return total;
}

static int64_t stringAppendChar()
{
  std::string text;
  for (int i = 0; i < bigCount; ++i)
    text.push_back(char('a' + i % 26));
  return text.size() + text[12345];
}

static int64_t stringAppendPieces()
{
  char buffer[32];
  std::string text;
  for (int i = 0; i < stringCount; ++i)
  {
    formatKey(buffer, "piece", i);
    text += buffer;
  }
  return text.size();
}

static int64_t stringConcat()
{
  std::string left("left-hand-side-of-it");
  std::string right("right");
  int64_t total = 0;
  for (int i = 0; i < stringCount; ++i)
  {
    std::string joined = left + right;
    total += joined.size();
  }
  return total;
}

static std::string haystack;

static int64_t stringFind()
{
  int64_t total = 0;
  for (int round = 0; round < 50; ++round)
    total += haystack.find("needle-in-the-haystack");
  return total;
}

static int64_t stringFindChar()
{
  int64_t total = 0;
  for (int round = 0; round < 200; ++round)
    total += haystack.find('#', 0);
  return total;
}

static int64_t stringCompare()
{
  int64_t equal = 0;
  for (int round = 0; round < 4; ++round)
    for (int i = 1; i < stringCount; ++i)
      if (sharedStrings[i] == sharedStrings[i - 1] || sharedStrings[i] < sharedStrings[i - 1])
        ++equal;
  return equal;
}

static int64_t sortInts()
{
  std::vector<int> values;
  values.reserve(stringCount);
  for (int i = 0; i < stringCount; ++i)
    values.push_back(int(nextRandom() % 1000000000));
  std::sort(values.begin(), values.end());
  return values[0] + values[stringCount / 2];
}

static int64_t sortIntsPredicate()
{
  std::vector<int> values;
  values.reserve(stringCount);
  for (int i = 0; i < stringCount; ++i)
    values.push_back(int(nextRandom() % 1000000000));
  std::sort(values.begin(), values.end(), [](int a, int b) { return a > b; });
  return values[0] + values[stringCount / 2];
}

static int64_t sortStrings()
{
  std::vector<std::string> values = sharedStrings;
  for (int i = 0; i < (int)values.size(); ++i)
    std::swap(values[i], values[int(nextRandom() % (uint64_t)values.size())]);
  std::sort(values.begin(), values.end());
  return values[0].size() + values[stringCount / 2].size();
}

template<typename M>
static int64_t hashIntInsert()
{
  M map;
  for (int i = 0; i < stringCount; ++i)
    map[int(nextRandom() % 4000000)] += i;
  return map.size();
}

template<typename M>
static int64_t hashIntLookup()
{
  M map;
  for (int i = 0; i < stringCount; ++i)
    map[i * 7] = i;
  int64_t found = 0;
  for (int round = 0; round < 4; ++round)
    for (int i = 0; i < stringCount; ++i)
    {
      auto value = map.find(int(nextRandom() % 7000000));
      if (value != map.end())
        found += value->second;
    }
  return found;
}

template<typename M>
static int64_t hashStringInsert()
{
  M map;
  for (int i = 0; i < stringCount; ++i)
    map[sharedStrings[i]] = i;
  return map.size();
}

template<typename M>
static int64_t hashStringLookup()
{
  M map;
  for (int i = 0; i < stringCount; i += 2)
    map[sharedStrings[i]] = i;
  int64_t found = 0;
  for (int round = 0; round < 2; ++round)
    for (int i = 0; i < stringCount; ++i)
    {
      auto value = map.find(sharedStrings[i]);
      if (value != map.end())
        found += value->second;
    }
  return found;
}

static const int mapCount = 100000;

static int64_t mapInsert()
{
  std::map<int, int> map;
  for (int i = 0; i < mapCount; ++i)
    map[int(nextRandom() % 1000000)] += i;
  return map.size();
}

static int64_t mapLookup()
{
  std::map<int, int> map;
  for (int i = 0; i < mapCount; ++i)
    map[i * 7] = i;
  int64_t found = 0;
  for (int i = 0; i < stringCount; ++i)
  {
    auto entry = map.find(int(nextRandom() % 700000));
    if (entry != map.end())
      found += entry->second;
  }
  return found;
}

static int64_t mapIterate()
{
  std::map<int, int> map;
  for (int i = 0; i < mapCount; ++i)
    map[i * 7] = i;
  int64_t sum = 0;
  for (int round = 0; round < 20; ++round)
    for (auto& entry : map)
      sum += entry.second;
  return sum;
}

int main(int argc, char** argv)
{
  if (argc > 1)
    selected = atoi(argv[1]);
  for (int i = 0; i < bigCount; ++i)
    sharedInts.push_back(i);
  char buffer[32];
  for (int i = 0; i < stringCount; ++i)
  {
    int length = formatKey(buffer, "name", int(int64_t(i) * 7919 % stringCount));
    sharedStrings.push_back(std::string(buffer, length));
  }
  for (int i = 0; i < 1000000; ++i)
    haystack.push_back(char('a' + i % 23));
  haystack.append("needle-in-the-haystack#");

  run("vector.push int", vectorPushInt);
  run("vector.push reserved", vectorPushReserved);
  run("vector.resize", vectorResize);
  run("vector.iterate", vectorIterate);
  run("vector.copy int", vectorCopy);
  run("vector.insert front", vectorInsertFront);
  run("vector.push String", vectorPushString);
  run("string.small", stringSmall);
  run("string.vector copy", stringVectorCopy);
  run("string.append char", stringAppendChar);
  run("string.append pieces", stringAppendPieces);
  run("string.concat", stringConcat);
  run("string.find", stringFind);
  run("string.find char", stringFindChar);
  run("string.compare", stringCompare);
  run("sort.int", sortInts);
  run("sort.int predicate", sortIntsPredicate);
  run("sort.String", sortStrings);
  run("hash.int insert", []() { return hashIntInsert<std::unordered_map<int, int>>(); });
  run("hash.int lookup", []() { return hashIntLookup<std::unordered_map<int, int>>(); });
  run("hash.String insert", []() { return hashStringInsert<std::unordered_map<std::string, int>>(); });
  run("hash.String lookup", []() { return hashStringLookup<std::unordered_map<std::string, int>>(); });
  run("fasthash.int insert", []() { return hashIntInsert<std::unordered_map<int, int>>(); });
  run("fasthash.int lookup", []() { return hashIntLookup<std::unordered_map<int, int>>(); });
  run("fasthash.String insert", []() { return hashStringInsert<std::unordered_map<std::string, int>>(); });
  run("fasthash.String lookup", []() { return hashStringLookup<std::unordered_map<std::string, int>>(); });
  run("map.insert", mapInsert);
  run("map.lookup", mapLookup);
  run("map.iterate", mapIterate);
  return 0;
}
