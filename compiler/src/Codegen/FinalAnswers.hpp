#pragma once
// Part of codegen: CodegenShared.hpp includes it inside namespace codegen.

// Answers that do not change once given (a complete layout, a class's size, whether it needs
// destroying, a member's bit-field), published for every emitter in shards with a lock each. Each
// emitter keeps its own copy of the answers it used; one it has not used yet is looked up here, not
// under the shared lock: 32 emitters, each taking the shared lock for every layout and answer it
// used the first time (1.3 million times for the game), waited 103 s of their 172 s on it.
template<typename K, typename V, typename Hash = std::hash<K>>
struct FinalAnswers
{
  struct Shard
  {
    SpinMutex mutex;
    std::unordered_map<K, V, Hash> answers;
  };
  Shard& shardOf(const K& k) { return shards[(Hash()(k) >> 58) % std::size(shards)]; } // (by the top bits, as NameSet's)
  bool find(const K& k, V& v)
  {
    Shard& s = shardOf(k);
    std::lock_guard lock(s.mutex);
    auto it = s.answers.find(k);
    if (it == s.answers.end())
      return false;
    v = it->second;
    return true;
  }
  void publish(const K& k, const V& v)
  {
    Shard& s = shardOf(k);
    std::lock_guard lock(s.mutex);
    s.answers.emplace(k, v);
  }
  void clear()
  {
    for (Shard& s : shards)
    {
      std::lock_guard lock(s.mutex);
      s.answers.clear();
    }
  }

  Shard shards[64];
};
