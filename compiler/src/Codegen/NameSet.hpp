#pragma once
// Part of codegen: CodegenShared.hpp includes it inside namespace codegen.

// A set of IR names several emitters mark, in shards with a lock each (marking is frequent).
struct NameSet
{
  struct Shard
  {
    SpinMutex mutex;
    std::unordered_set<std::string> names;
  };
  // By the hash's top bits: a shard's set buckets by its low bits (a power-of-two bucket count),
  // and picked by those too, its names all fell into one bucket in 64 (11.7% of codegen's time
  // at -j 1 was spent walking those chains).
  Shard& shardOf(const std::string& name) { return shards[(std::hash<std::string>()(name) >> 58) % std::size(shards)]; }
  // Marks name; true for whoever marked it first.
  bool claim(const std::string& name)
  {
    Shard& s = shardOf(name);
    std::lock_guard lock(s.mutex);
    return s.names.insert(name).second;
  }
  bool contains(const std::string& name)
  {
    Shard& s = shardOf(name);
    std::lock_guard lock(s.mutex);
    return s.names.count(name) != 0;
  }
  // Unmarks name: its next claim is a first again.
  void release(const std::string& name)
  {
    Shard& s = shardOf(name);
    std::lock_guard lock(s.mutex);
    s.names.erase(name);
  }

  Shard shards[64];
};
