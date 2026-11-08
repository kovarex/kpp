// The driver: the back end's threads in a build that streams (step 4's P2); the declarations, in
// namespace driver (Driver.hpp includes this), the definitions in BackEndPool.cpp (main.cpp split,
// stage 2, 2026-11-05, moved without a change).
  // Step 4's P2 (docs/codegen-speed.md): the back end's threads in a build that streams. They compile the
  // program pieces codegen hands over at level 0's barrier while it emits the instances, below normal
  // priority so that codegen keeps the cores it uses; then (raise) the rest of the build's modules at
  // normal priority. The largest task queued first (its weight: a module's text size), those of one
  // weight in the order they came: a large piece taken last ends the build alone (the game's 17 MB
  // program piece of ValueToWidget, taken 93rd of 128 at the barrier, compiled 5.4 s past codegen's end).
  class BackEndPool
  {
  public:
    explicit BackEndPool(unsigned threads)
    {
      for (unsigned t = 0; t < std::max(1u, threads); ++t)
        workers.emplace_back([this] { work(); });
    }
    // What is queued and not started is dropped (an early return: codegen failed).
    ~BackEndPool()
    {
      {
        std::lock_guard lock(mutex);
        queue.clear();
        closing = true;
      }
      ready.notify_all();
      for (std::thread& t : workers)
        t.join();
    }
    // streamed: a program piece handed over at the barrier (waitStreamed).
    void push(std::function<void()> task, bool streamed, size_t weight)
    {
      {
        std::lock_guard lock(mutex);
        queue.push_back({std::move(task), streamed, weight, nextOrder++});
        std::push_heap(queue.begin(), queue.end(), lighter);
        ++pending;
        streamedPending += streamed;
      }
      ready.notify_one();
    }
    // Codegen ended: normal priority for every task started from now on.
    void raise() { below = false; }
    void waitStreamed()
    {
      std::unique_lock lock(mutex);
      done.wait(lock, [&] { return streamedPending == 0; });
    }
    void wait()
    {
      std::unique_lock lock(mutex);
      done.wait(lock, [&] { return pending == 0; });
    }
    // How many tasks ended while codegen ran (after wait).
    size_t duringCodegen() const { return endedBelow; }

  private:
    struct Task
    {
      std::function<void()> run;
      bool streamed = false;
      size_t weight = 0;
      size_t order = 0;
    };
    // The heap's order: the heaviest on top, of one weight the first pushed.
    static bool lighter(const Task& a, const Task& b) { return a.weight != b.weight ? a.weight < b.weight : a.order > b.order; }
    void work()
    {
      bool lowered = false;
      for (;;)
      {
        Task task;
        {
          std::unique_lock lock(mutex);
          ready.wait(lock, [&] { return closing || !queue.empty(); });
          if (queue.empty())
            return;
          std::pop_heap(queue.begin(), queue.end(), lighter);
          task = std::move(queue.back());
          queue.pop_back();
        }
        if (lowered != below)
        {
          lowered = below;
          setThreadBelowNormal(lowered);
        }
        task.run();
        bool ended = below;
        task.run = nullptr; // (its text freed now)
        {
          std::lock_guard lock(mutex);
          --pending;
          streamedPending -= task.streamed;
          endedBelow += ended;
        }
        done.notify_all();
      }
    }
    std::vector<std::thread> workers;
    std::mutex mutex;
    std::condition_variable ready, done;
    std::vector<Task> queue; // (a heap: lighter)
    size_t pending = 0, streamedPending = 0, endedBelow = 0, nextOrder = 0;
    bool closing = false;
    std::atomic<bool> below{true};
  };
