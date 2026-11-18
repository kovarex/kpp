#pragma once
// Part of kpp --lsp: lsp.cpp includes it where it was.

// The messages read, in order; the end of the input is a message with ended set.
struct Inbox
{
  void put(Json m)
  {
    std::lock_guard lock(mutex);
    messages.push_back(std::move(m));
    ready.notify_one();
  }
  void end()
  {
    std::lock_guard lock(mutex);
    ended = true;
    ready.notify_one();
  }
  bool take(Json& m)
  {
    std::unique_lock lock(mutex);
    ready.wait(lock, [&] { return !messages.empty() || ended; });
    if (messages.empty())
      return false;
    m = std::move(messages.front());
    messages.pop_front();
    return true;
  }
  bool empty()
  {
    std::lock_guard lock(mutex);
    return messages.empty();
  }
  // The queued messages pick chooses, taken out in order; the scan stops at the first one stop chooses
  // (the others stay queued in their order).
  template<typename Pick, typename Stop>
  std::vector<Json> takeQueued(Pick pick, Stop stop)
  {
    std::lock_guard lock(mutex);
    std::vector<Json> taken;
    for (auto it = messages.begin(); it != messages.end() && !stop(*it);)
      if (pick(*it))
      {
        taken.push_back(std::move(*it));
        it = messages.erase(it);
      }
      else
        ++it;
    return taken;
  }
  // Whether a queued message pick chooses is waiting.
  template<typename Pick>
  bool anyQueued(Pick pick)
  {
    std::lock_guard lock(mutex);
    return std::any_of(messages.begin(), messages.end(), pick);
  }
  // Waits until the whole input has been read (--read-input-first).
  void waitForEnd()
  {
    std::unique_lock lock(mutex);
    ready.wait(lock, [&] { return ended; });
  }

  std::mutex mutex;
  std::condition_variable ready;
  std::deque<Json> messages;
  bool ended = false;
};
