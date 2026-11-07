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

  std::mutex mutex;
  std::condition_variable ready;
  std::deque<Json> messages;
  bool ended = false;
};
