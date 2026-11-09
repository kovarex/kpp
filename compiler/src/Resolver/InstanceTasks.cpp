// GlobalScope::Impl, class instances' member bodies checked as tasks (its declarations in InstanceTasks.hpp, part of GlobalScopeImpl.hpp): a translation unit
// of its own since 2026-11-05 (resolver.cpp split, stage 2), moved without a change.
#include "GlobalScopeImpl.hpp"

auto GlobalScope::Impl::queueMemberBody(const MemberTask& t) -> void
{
  {
    std::lock_guard lock(memberTaskMutex);
    memberTasks.push_back(t);
  }
  memberTaskCv.notify_one();
}

auto GlobalScope::Impl::runMemberTask(const MemberTask& t) -> void
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
    sub.rootFile = t.rootFile;
    sub.rootToken = t.rootToken;
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
