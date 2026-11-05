#include "SymbolTable.hpp"
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct SymbolTable::Impl
{
  static constexpr uint32_t shardCount = 256;
  struct Shard
  {
    std::shared_mutex mutex;
    std::unordered_map<std::string_view, uint32_t> map;
  };
  // id -> name, in blocks that never move, so that name() reads without a lock (it is asked for
  // all the time on every thread; under namesMutex it serialised the parallel front end). An id
  // is handed out only after its entry is written (through the shard's mutex), so a reader
  // holding an id sees it.
  static constexpr uint32_t blockBits = 12, blockSize = 1u << blockBits, maxBlocks = 1u << 12;

  ~Impl()
  {
    for (auto& b : blocks)
      delete[] b.load();
  }

  static uint64_t hash(std::string_view s)
  {
    uint64_t h = 14695981039346656037ull;
    for (unsigned char c : s)
      h = (h ^ c) * 1099511628211ull;
    return h;
  }

  std::string_view store(std::string_view s)
  {
    if (size_t(chunkEnd - chunkPos) < s.size())
    {
      size_t size = std::max<size_t>(1 << 16, s.size());
      chunks.push_back(std::make_unique<char[]>(size));
      chunkPos = chunks.back().get();
      chunkEnd = chunkPos + size;
    }
    memcpy(chunkPos, s.data(), s.size());
    std::string_view result(chunkPos, s.size());
    chunkPos += s.size();
    return result;
  }

  Shard shards[shardCount];
  // Names are stored in stable chunks so string_views in the maps stay valid.
  std::mutex namesMutex;
  std::vector<std::string*> names; // id -> name (pointer stable)
  std::vector<std::unique_ptr<char[]>> chunks;
  char* chunkPos = nullptr;
  char* chunkEnd = nullptr;
  std::atomic<std::string_view*> blocks[maxBlocks] = {};
  std::atomic<uint32_t> count{0};
};

SymbolTable::SymbolTable() : impl(new Impl) {}
SymbolTable::~SymbolTable() { delete impl; }

uint32_t SymbolTable::intern(std::string_view name)
{
  uint64_t h = Impl::hash(name);
  Impl::Shard& shard = impl->shards[h & (Impl::shardCount - 1)];
  {
    std::shared_lock lock(shard.mutex);
    auto it = shard.map.find(name);
    if (it != shard.map.end())
      return it->second;
  }
  std::unique_lock lock(shard.mutex);
  auto it = shard.map.find(name);
  if (it != shard.map.end())
    return it->second;
  uint32_t id;
  std::string_view stored;
  {
    std::lock_guard namesLock(impl->namesMutex);
    stored = impl->store(name);
    id = impl->count.load(std::memory_order_relaxed);
    if (id >= Impl::blockSize * Impl::maxBlocks)
    {
      fprintf(stderr, "too many distinct identifiers (%u)\n", id);
      abort();
    }
    std::string_view* block = impl->blocks[id >> Impl::blockBits].load(std::memory_order_relaxed);
    if (!block)
    {
      block = new std::string_view[Impl::blockSize];
      impl->blocks[id >> Impl::blockBits].store(block, std::memory_order_release);
    }
    block[id & (Impl::blockSize - 1)] = stored;
    impl->count.store(id + 1, std::memory_order_release);
  }
  shard.map.emplace(stored, id);
  return id;
}

std::string_view SymbolTable::name(uint32_t id) const
{
  return impl->blocks[id >> Impl::blockBits].load(std::memory_order_acquire)[id & (Impl::blockSize - 1)];
}

size_t SymbolTable::size() const
{
  return impl->count.load(std::memory_order_acquire);
}
