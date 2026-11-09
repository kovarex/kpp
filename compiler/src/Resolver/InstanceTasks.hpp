// GlobalScope::Impl, class instances' member bodies checked as tasks: part of the class's body, included in it in GlobalScopeImpl.hpp,
// its definitions in InstanceTasks.cpp (resolver.cpp split, 2026-11-05, moved without a change).
  // ---- member bodies of class instances, checked as tasks (docs/compile-speed.md, step 3) ----
  // While the files are resolved, instantiating a class queues the check of each member body
  // instead of checking it on the asking thread; every worker takes them after the files
  // (GlobalScope::checkQueuedMemberBodies). An instance's typing does not depend on when it is
  // checked (each instance is claimed once, its statistics and errors kept apart and sorted).
  // A template function's own (generic) walk, queued by its file's walk (Walker::walkDecl): the
  // walker's place there, and the file's statistics it adds to once the resolve is done.
  struct DeclWalk
  {
    FunctionDecl* f;
    Scope* scope;
    const Ty* thisType;
    std::vector<LocalStack::Item> locals;
    const ParsedFile* file;
    ResolveStats* into;
  };
  struct MemberTask
  {
    const Ty* classTy;
    FunctionDecl* f;
    const ParsedFile* requestFile;
    uint32_t requestToken;
    int depth;
    InstanceRecord* functionInstance = nullptr; // or a function instance's queued check (its record holds the task)
    std::shared_ptr<DeclWalk> declWalk;         // or a template function's own walk
    const ParsedFile* rootFile = nullptr;       // Walker::rootFile
    uint32_t rootToken = UINT32_MAX;
  };
  // The statistics of the queued walks, added to their files' once every worker is done (adding is
  // order-independent: the counts sum, a first occurrence is the earliest place).
  std::mutex deferredStatsMutex;
  std::vector<std::pair<ResolveStats*, std::unique_ptr<ResolveStats>>> deferredStats;
  std::atomic<bool> queueMemberBodies{false};
  std::mutex memberTaskMutex;
  std::condition_variable memberTaskCv;
  std::deque<MemberTask> memberTasks;
  int memberTasksRunning = 0;
  void queueMemberBody(const MemberTask& t);
  void runMemberTask(const MemberTask& t);
