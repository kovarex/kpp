// GlobalScope::Impl, class instances' member bodies checked as tasks: part of resolver.cpp, included in the class's body there (a file of its
// own since 2026-11-05, moved without a change; docs/design.md, "Compilation model").
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
  void queueMemberBody(const MemberTask& t)
  {
    {
      std::lock_guard lock(memberTaskMutex);
      memberTasks.push_back(t);
    }
    memberTaskCv.notify_one();
  }
  void runMemberTask(const MemberTask& t)
  {
    if (const DeclWalk* dw = t.declWalk.get())
    {
      auto local = std::make_unique<ResolveStats>();
      local->fileIndex = dw->into->fileIndex;
      local->collectWhy = dw->into->collectWhy;
      Walker w(*this, *local, *dw->file->tokens, dw->file->text);
      w.file = dw->file;
      FileOfLookups walkedFile(dw->file);
      w.scope = dw->scope;
      w.thisType = dw->thisType;
      w.locals.items = dw->locals;
      w.walkingQueued = true;
      w.walkDecl(dw->f);
      std::lock_guard lock(deferredStatsMutex);
      deferredStats.push_back({dw->into, std::move(local)});
      return;
    }
    if (InstanceRecord* inst = t.functionInstance)
    {
      // A function instance's check, unless a walk that needed its result took it over first.
      if (inst->queued.exchange(0) != 1)
        return;
      const FunctionInstanceTask& ft = *inst->task;
      if (!ft.def->file || !ft.def->file->tokens)
      {
        inst->state.store(2, std::memory_order_release); // nothing to check: built, as an inline check would leave it
        inst->state.notify_all();
        return;
      }
      ResolveStats scratch;
      Walker w(*this, scratch, *ft.def->file->tokens, ft.def->file->text);
      w.file = ft.def->file;
      FileOfLookups walkedFile(ft.def->file);
      w.scope = ft.bodyScope;
      w.runFunctionInstance(inst, ft);
      return;
    }
    auto* c = static_cast<ClassDecl*>(t.classTy->decl);
    if (!c->file || !c->file->tokens)
      return;
    ResolveStats scratch;
    Walker w(*this, scratch, *c->file->tokens, c->file->text);
    w.file = c->file;
    FileOfLookups walkedFile(c->file);
    w.scope = c->ownScope;
    w.instantiationDepth = t.depth;
    Subst subst = substFor(t.classTy);
    auto start = std::chrono::steady_clock::now();
    w.subCheck(c, subst, c->ownScope, types.pointer(t.classTy), [&](Walker& sub)
    {
      sub.requestFile = t.requestFile;
      sub.requestToken = t.requestToken;
      if (t.f)
      {
        w.checkMemberBody(sub, t.f, subst);
        return;
      }
      // The whole instance (one task per class instance: per member, the tasks' own setup cost
      // more than they spread).
      for (Decl* m : membersRead(c))
        if (m->kind == NodeKind::FunctionDecl && static_cast<FunctionDecl*>(m)->body && m->templateParams.empty())
          w.checkMemberBody(sub, static_cast<FunctionDecl*>(m), subst);
      for (FunctionDecl* f : c->ownScope->outOfLineMembers)
        if (f->body && (!isMemberSpecialization(f) || memberSpecializationIsFor(f, t.classTy)))
          w.checkMemberBody(sub, f, subst);
    });
    instNanos += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
  }
