// The driver: the back end's threads in a build that streams (step 4's P2); the declarations, in
// namespace driver (Driver.hpp includes this), the definitions in BackEndPool.cpp (main.cpp split,
// stage 2, 2026-11-05, moved without a change).
  // Step 4's P2 (docs/codegen-speed.md): the back end's threads in a build that streams. They compile the
  // program pieces codegen hands over at level 0's barrier while it emits the instances, then the rest
  // of the build's modules. At normal priority, as codegen's threads: below it (until 2026-11-21), a
  // piece's thread preempted while it held a spin lock left codegen's threads spinning on it until
  // Windows boosted the starved thread, about 5 s each time (LLVM's allocator, rpmalloc, the process's
  // malloc in a kpp with LLVM, spins without sleeping): 4 of 20 clean builds of the game took 15.5 to 21.6 s
  // instead of 13 to 14 (docs/codegen-speed.md, "Step 4's P2 at normal priority").
  // The largest task queued first (its weight: a module's text size), those of one
  // weight in the order they came: a large piece taken last ends the build alone (the game's 17 MB
  // program piece of ValueToWidget, taken 93rd of 128 at the barrier, compiled 5.4 s past codegen's end).
  // While codegen runs, only widthWhileCodegenRuns of the tasks run at once: codegen's threads are the
  // build's critical path then (the instances' modules are compiled only once it ends, and until then
  // these threads have the program pieces alone, which they finished 2 s before codegen did), and as
  // many of these as there are hardware threads took half the machine from them. With half the
  // threads the game's clean build is 0.8 s shorter and the program pieces still end while codegen
  // runs (docs/codegen-speed.md, "The back end's threads while codegen runs").
  class BackEndPool
  {
  public:
    BackEndPool(unsigned threads, unsigned widthWhileCodegenRuns)
      : width(std::clamp(widthWhileCodegenRuns, 1u, std::max(1u, threads)))
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
    // Codegen ended (for duringCodegen's count): every thread takes tasks from here on.
    void codegenEnded()
    {
      duringCodegenRuns = false;
      {
        std::lock_guard lock(mutex);
        width = unsigned(workers.size());
      }
      ready.notify_all();
    }
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
    size_t duringCodegen() const { return endedDuringCodegen; }

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
      for (;;)
      {
        Task task;
        {
          std::unique_lock lock(mutex);
          ready.wait(lock, [&] { return closing || (!queue.empty() && running < width); });
          if (queue.empty())
            return;
          std::pop_heap(queue.begin(), queue.end(), lighter);
          task = std::move(queue.back());
          queue.pop_back();
          ++running;
        }
        task.run();
        bool ended = duringCodegenRuns;
        task.run = nullptr; // (its text freed now)
        {
          std::lock_guard lock(mutex);
          --pending;
          --running;
          streamedPending -= task.streamed;
          endedDuringCodegen += ended;
        }
        ready.notify_one(); // (a thread the width kept waiting)
        done.notify_all();
      }
    }
    std::vector<std::thread> workers;
    std::mutex mutex;
    std::condition_variable ready, done;
    std::vector<Task> queue; // (a heap: lighter)
    size_t pending = 0, streamedPending = 0, endedDuringCodegen = 0, nextOrder = 0;
    unsigned running = 0; // tasks running now
    unsigned width;       // how many may: widthWhileCodegenRuns, then every thread (codegenEnded)
    bool closing = false;
    std::atomic<bool> duringCodegenRuns{true};
  };
